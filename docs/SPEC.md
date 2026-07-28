# Stutter v2 — Multi-FX Glitch Sequencer (VST3 / AU / Standalone, macOS)

**Scene** をホストのオートメーションから呼び出すマルチFXグリッチシーケンサー。
可変長ブロックのパターン、任意パラメータへルーティング可能なモジュレーション、
12レーンのエフェクトを持つ。

**画面で組み、オートメーションで呼び出す**という責務分離が設計の軸。エフェクトの中身
(レーン、ブロック、カーブ、パラメータ)はエディタが所有し、タイムラインが決めるのは
「どの Scene を、どの区間で有効にするか」だけ。オートメーション対象は `sceneSelect` と
`active` の2本のみで、これは意図的な制限である。

本書は設計の意図と全体像を扱う。個々のクラスの詳細は各ヘッダの doc comment が一次情報
であり、そちらのほうが常に新しい。

## コンセプト

参考にした5製品からの「いいとこ取り」:

| 製品 | 借りた考え方 |
|---|---|
| iZotope Stutter Edit 2 | Gesture(= Scene): 1ノートが全状態のスナップショットを呼び出す。TVM 相当の全パラメータ変調。Release モード。早入力を許すクオンタイズ |
| Illformed Glitch 2 | 可変長ブロックのシーケンサー。per-scene の Seed による再現性。Scene Lock。Send/Feedback/Return のディレイ |
| Sugar Bytes Effectrix 2 | エフェクトごとのレーンを並べ替え可能な直列チェーンとして扱う |
| Cableguys ShaperBox 3 | ブレークポイントの Hard/Medium/Soft ウェイト(明示的なアンチクリック) |
| Xfer LFO Tool | 描画カーブによるモジュレーション、テンポ同期の速度倍率 |

## 技術スタック

- JUCE 8.0.8(CMake FetchContent)、C++20
- フォーマット: VST3 / AU / Standalone、**macOS 専用**
- 会社名 `Maniax`、プラグインコード `Stt1`、メーカーコード `Manx`
- テストは Catch2 v3(同じく FetchContent)

---

## アーキテクチャの中核: 状態を2つに分ける

v2 の設計判断で最も重要なのは、**編集用の状態と再生用の状態を別物として持つ**こと。

```
  UI ──編集──▶ SceneDocument (juce::ValueTree, undo 付き)
                    │
                    │ publish() = ベイク
                    ▼
              SceneStore (SceneSnapshot のバンク)
                    │
                    │ atomic ポインタ経由で読むだけ
                    ▼
             オーディオスレッド
```

**なぜ分けるか**: オーディオスレッドがロックせずに読めるようにするため。エディタは
スナップショットに触れず、オーディオスレッドはツリーに触れない。`SceneSnapshot` は
固定長配列だけの POD で、`std::vector` も `juce::String` も持たない — atomic ポインタ
越しに読む構造体にヒープ所有メンバがあってはならないため。

**publish は明示的**(マウスアップ時など)。プロパティ変更のたびに自動で走らせると、
ドラッグ中に毎回約10MBのバンクを再構築することになる。

ただし**ノブのドラッグは現状この対策の外にある**。当初は `LiveParamOverlay`(lock-free)
を通す設計だったが、実装ではノブ編集は `LaneParamWriteback`(パラメータのリスナー)が
値を記録し、タイマーがシーンへ書き戻して `publish()` している。`LiveParamOverlay` は
宣言されているだけで使われていない。実測でこれが問題になってはいないが、SPEC の元の
意図とは異なる。

リスナーが APVTS ではなく**パラメータ**に付いているのは gesture を受け取るため。
詳細は「なぜ制御面が2本だけなのか」を参照。

**退役キュー**: 差し替えられたバンクは即座に解放せず、時間ベースのキューに入れて
メッセージスレッドのタイマーで回収する。参照カウントにするとオーディオスレッドの読み取り
経路に atomic インクリメントが乗るが、オーディオスレッドはそもそもスナップショットの
ポインタを processBlock を跨いで保持しないため、猶予期間で十分。

---

## DSP

### シグナルチェーン

```
入力 → CaptureBuffer(2.5秒リング) → BlockSequencer(12レーン)
     → グローバルカーブ(Volume/Filter/Pan) → ジェスチャゲート × Dry/Wet → 出力ゲイン
```

`SceneSelector` はシーケンサーの**前**でオートメーションを読む。そのチャンクの
レンダリング前に Scene が確定するようにするため(1チャンク遅れないように)。

### BlockSequencer

v1 の固定16セルグリッドを可変長ブロックに置き換えたもの。

**なぜブロックか**: セルは「このレーンはこのステップで ON」としか言えない。TapeStop の
減速のような方向性を持つエンベロープは「どれだけ保持されているか」を知る必要があり、
v1 では16分ごとに再スタートして**停止に到達できなかった**。ブロックは開始と長さを持つ
ので「8division かけて停止する」が表現できる。

- **パターン幾何**: Beats 1〜8 × Divisions 2〜8。divisions を3や6にすると三連や
  ポリリズムが別のレートテーブルなしで得られる
- **Swing**: 奇数境界のみを変位させる。偶数境界(パターン終端を含む)は固定される
  ので、パターン長は変わらない。division ごとに独立してリスケールする実装だと
  パターンが短くなり、それはグルーヴではなくテンポ変更になる
- **カテゴリ**: Buffer(排他、レーン順で解決)/ Texture(直列に積む、`chainPosition`
  でソートされるが設定 UI が無いため実効順序はレーン順)/ Send(Texture の後段、
  リターンバスへ)
- **クロスフェード**: レーン切替時に5msの等パワー(sin テーパ)。ブロック長の25%で
  クランプする
- **RetriggerPolicy**: `RetriggerEachBlock` / `ContinueThroughRun`(隣接ブロックを
  1つの連続した run として扱う)/ `RetriggerEachDivision`(長尺ブロックでも
  division ごとに再ラッチ。Shuffler/Stretcher 用)

サンプル単位ループは v1 から意図的に維持している。ブロック端のサンプル精度と Swing を
安価に実現しているのがこのループ構造であり、バッファをブロック境界で分割する方式に
すると両方が複雑になる。整数除算前の epsilon ガードも同様に必須で、Swing により境界が
表現不能な位置に落ちる頻度が上がるため **v1 より重要**になっている。

### エフェクトレーン(12種)

| # | 名前 | カテゴリ | 主なパラメータ |
|---|---|---|---|
| 0 | Stutter | Buffer | rate, decay, pitchSlide |
| 1 | TapeStop | Buffer | curve, time |
| 2 | TapeStart | Buffer | curve, time |
| 3 | Reverse | Buffer | sliceLen |
| 4 | Repitch | Buffer | semitones, slide |
| 5 | Gate | Texture | rate, duty, shape |
| 6 | Filter | Texture | type, cutoff, resonance, lfoRate, lfoDepth |
| 7 | Crush | Texture | bitDepth, rateDiv |
| 8 | Stretcher | Buffer | speed, grain, jitter, pitch |
| 9 | Shuffler | Buffer | slice, range, shuffle/repeat/reverse 確率 |
| 10 | Delay | **Send** | time, feedback, send, return, slew |
| 11 | Distortion | Texture | mode(Razor/Shape/Fold/Rectify), drive, tone, mix |

**Delay が Send カテゴリの存在理由**。ディレイの出力を、そのディレイに入力を供給する
直列チェーンに戻すのは再帰的でゲインが発散する。send/return として走らせることで
フィードバック経路が閉じて有界になる。Send/Feedback/Return の3分割は Glitch 2 由来で、
「入力量」と「出力量」を分けられる — リターンを切ってもラインは鳴り続ける、という
slap-back のレシピが単一の mix ノブでは表現できない。

**Shuffler と Stretcher は乱数を使うが、必ず `BlockContext::seed` からローカル生成器を
作る。グローバル RNG は禁止**。グローバルだと出力がホストのバッファ分割の仕方に依存し、
同じプロジェクトがバッファサイズ違いで別の音になり、オフラインバウンスが聴いた音と
一致しなくなる。テストがこれを直接検証している(同一 seed でチャンクサイズを変えても
ビット一致すること)。

### ModulationEngine

v1 はカーブ3本が Volume/Filter/Pan に固定配線だった。v2 は**任意のカーブが任意の
パラメータを駆動できる**。

**優先順位規則(明示)**: `final = clamp(base + curve_offset * depth)`。Scene(または
オートメーション)が base を決め、カーブがそこからオフセットする。逆ではない。カーブが
駆動しているノブをオートメーションすると変調範囲全体が動く、という SE2 / ShaperBox と
同じ挙動になる。

**CPU 対策は後付けではなく設計に組み込み**。16カーブ × 12レーン × 12パラメータを毎
サンプル評価すると 48kHz で毎秒数百万回のテーブル参照になる。対策は2つ:

1. ベイク済みルートリストのみ走査(2本ルーティングなら2回、16回ではない)
2. 16サンプルごとの制御レート評価 + 線形補間

実測でフル装備 Scene が**リアルタイムの0.1%**(予算30%)。補間も効いており、フルスイープ
での最大サンプル間ステップは 0.000533。

### latched / continuous

各パラメータは `ParamDescriptor::latchAtBlockStart` でどちらかを宣言する。

- **latched**: `onBlockStart` でトリガ時点の値をラッチ。構造を決めるもの(Stutter の
  rate — ループ途中で長さが変わると読み取りアンカーが破綻する、Reverse の sliceLen、
  Filter の type)。**変調は効く**が、毎サンプルではなくトリガごとにサンプリングされる
- **continuous**: `SampleContext::modulatedParams` から毎サンプル読む。スイープさせたい
  もの(Filter の cutoff、Gate の duty)

取り違えると音で分かる: latched なものを continuous にするとジッパーノイズかクリック、
continuous なものを latched にするとカーブを掛けても動かない。

### SceneSelector

オートメーションから来る2つの値だけを持つ。**どの Scene が鳴るか**(`sceneSelect`)と、
**それが聞こえるか**(`active`)。

- **Scene は 1 始まり、0 は「未指定」。** ユーザーはブラウザで見た番号をオートメーション
  レーンに手で打ち込むので、画面の番号とパラメータの値は同一でなければならない。人が
  スロットを数えるときは 1 から始めるし、そうすると 0 が「未指定」用に空く — 書かれて
  いないレーンは 0 を送ってくるので、それを「切り替えない」と解釈すれば、触っていない
  レーンがエディタでの選択を奪わない。代償は配列1要素の無駄だけで、これが最も安い
- 配列サイズは 128 のまま。スロット0は確保されるが使われない
  (`firstSceneIndex` / `lastSceneIndex` / `noSceneIndex` を参照)
- **`processChunk` でポーリングする。** `parameterChanged` リスナーは使わない。リスナーは
  ホストが好きなスレッドで呼ぶため、そこから `activeScene` に触るとレンダリング途中で
  Scene が変わりうる。ポーリングなら「そのチャンクの描画前に Scene が確定する」保証が
  維持され、`getRawParameterValue` はロックフリーで済む
- **値が変わったときだけミラーを立てる。** ホストは毎ブロック同じ値を再送するので、
  無条件に立てるとミラーが回り続ける
- **ゲートはシーケンサーのミュートではなく Dry/Wet ミックス上のランプ**。これにより
  遷移が構造的にクリックレスになる。実測で最大ステップ 0.004167(48kHz の5msランプが
  意味する 1/240 と一致)。`active` は Bool なのでブロック境界でステップ状に切り替わり、
  ランプがなければ全てのエッジがクリックになる
- **ゲートを閉じると wet は silence ではなく dry に collapse する**。無効区間では素材が
  そのまま通るべきで、トラックに穴を開けるべきではない。グリッチが大半の小節で無効で
  あることを前提とした設計なので、この性質は必須

### なぜ制御面が2本だけなのか

レーンパラメータ(cutoff 等)はオートメーション対象**ではない**。APVTS 上のレーン値は
アクティブ Scene のミラーであって音の権威ではなく(`ParamIndex.h` 参照)、音声スレッドは
`SceneStore` のスナップショットだけを読む。ホストがミラーを書いても音は変わらない。

その代わり、**ホストのオートメーション書き込みが Scene 文書を汚染しないようにしている**。
`LaneParamWriteback` は `beginChangeGesture`/`endChangeGesture` に挟まれた変更だけを
「ユーザー編集」として扱う。JUCE の Slider/ComboBox アタッチメントは必ずドラッグを
gesture で囲み、オートメーション再生は決して囲まないため、この一点で両者を区別できる。
これがないと、オートメーションを再生するだけで Scene が書き換わり、プロジェクト保存時に
恒久化してしまう。

### 保存先

| 設定 | 格納先 |
|---|---|
| `sceneSelect` / `active` | APVTS パラメータ(= ホストが保存し、オートメーションできる) |
| `activeScene` | state ツリーのルートプロパティ(復元時の着地点) |
| Release モード | Scene ごと(退役。enum は互換のため残置) |

`setStateInformation` には順序上の罠がある。`apvts.replaceState()` は `sceneSelect` を
既定値に戻すため、その**後**に復元済みの Scene を書き戻さないと、次のブロックの
ポーリングが既定値で上書きしてしまい、旧セッションが常に既定 Scene で開く。

**スキーマバージョンは上げていない。** 今回消えたのはルートプロパティ3つ
(`playMode` / `sceneLock` / `triggerQuantize`)だけで、他の構造は不変。バージョンを
上げると既存の v2 プロジェクトが全て Init に落ちる。読み捨てで安全に開ける。

---

## State スキーマ(v2)

`getStateInformation` / `setStateInformation` は APVTS の state ツリーに構造データを
子として足して XML 化する。ルートに `version` プロパティを書き、
**v1 の state はロードせず Init にフォールバックする**(v1 には version プロパティが
無いので `getProperty(version, 1)` が 1 を返す)。

### パラメータの3層区分

| 層 | 例 | 格納先 | オートメーション |
|---|---|---|---|
| 真のグローバル | `dryWet`, `outputGain`, `hostSync`, `internalBpm`, `sequencerOn` | APVTS(権威) | 可 |
| Scene ミラー | 全 `lane{N}_{param}` | APVTS(ミラー)。権威は Scene | 可 |
| Scene 構造 | ブロック、カーブ、seed、幾何 | ValueTree のみ | 不可 |

128 Scene × 約80パラメータ = 10,240 個はどのホストも許容しないため、**アクティブ
Scene 1個分だけをミラーする**。ホストがミラー層をオートメーションすると、その時点で
アクティブな Scene に適用される(SE2 / Glitch 2 と同じ挙動)。

**ミラーのフィードバック遮断**: オーディオスレッドは Scene 変更時にフラグを立てるだけ。
プロセッサのタイマーが `suppressParamWriteback` を立ててから値を流し込み、APVTS
リスナーはそのフラグが立っていれば即 return する。これが無いとミラー自身の書き込みが
ユーザー編集として読み戻され、ミラー元の Scene を書き換えるループになる。

### ツリー構造

```
<StutterState version="2" activeScene sceneLock playMode triggerQuantize>
  <PARAMETERS>                      APVTS: グローバル + アクティブSceneのミラー
  <Scenes>
    <Scene index name note seed beats divisions swing loopPolicy releaseMode>
      <LaneParams>
        <Lane index mute solo enabled mix gain pan filterType ... chainPosition>
          <P i v/>
      <Blocks>
        <B lane start len tier flags prob seed/>
      <Curves>
        <Curve target speed depth tier bipolar enabled>
          <Pt p v c w/>
      <Weights><W lane v/></Weights>
  <Curves>                          v1 由来の Volume/Filter/Pan 3系統(下記参照)
```

- ブロックの `start`/`len` は **division 単位の整数**(PPQ ではない)。Swing と
  beats/divisions は評価時に適用されるので、グリッド解像度を変えてもブロック配置は
  壊れず re-time される
- カーブの `target` は整数 `ParamIndex`。これがモジュレーションルートをデータとして
  保存可能にしている
- ブロックは `startDiv` 昇順・非重複であることが `SceneSchema` によって保証される。
  `BlockSequencer` は前進専用カーソルでこれに依存しているので、**この不変条件は
  `sceneFromTree` でのみ確立される**

**プロパティ読み出しは必ず文字列も受け付けること。** `juce::ValueTree` の XML
ラウンドトリップはプロパティを属性として書き、読み戻すと `var` は**文字列型**になる。
`isInt()` だけを見る実装は、保存されたシーンの整数(lane / start / len / beats /
divisions / tier / seed)を全てデフォルトへ戻してしまう。症状はパースエラーではなく
「もっともらしい別のシーン」— 全ブロックが lane 0 に潰れ、重なった分が重複排除で
消える — なので気付きにくい。`SceneSchema` の `getPropInt` / `getPropFloat` /
`getPropBool` はこのために文字列を受ける。

### 2つのカーブ系統が併存している

v1 由来の3系統(Volume / Filter / Pan)は `CurveModulator` として残っており、グローバル
出力段に掛かる。v2 のモジュレーションマトリクスはそれとは別に、任意のレーンパラメータへ
ルーティングできる。前者は「出力にかかる固定3系統」、後者は「どこにでも挿せる16本」と
役割が違うため併存させている。

### ParamDescriptor が単一の情報源

v1 はパラメータ定義が3箇所(`ParameterIDs.h` の ID、`ParameterLayout.cpp` のレンジ、
`LaneParamPanel` のノブ生成)に分散し、手で同期していた。v2 は各エフェクトが
`getParamDescriptors()` で宣言し、**APVTS レイアウト・UI・モジュレーションのターゲット
メニュー・Scene スキーマがすべてそこから生成される**。パラメータを使う側が定義する。

---

## UI

**1200 × 800**、比率固定でリサイズ可能。ダークテーマ(背景 #0d0e12 系)。

| 領域 | 内容 |
|---|---|
| ヘッダ | ロゴ、プリセットブラウザ、Dry/Wet、Output、SEQ/SYNC トグル、BPM |
| Scene ストリップ | Scene スロット一覧。どこに中身があるか、どれが鳴っているか、どれを編集中か |
| 演奏バー | ACTIVE / Grid(Beats×Divisions)/ Swing |
| ブロックグリッド | 12レーン × 可変 division。可変長ブロックの描画・編集、発光プレイヘッド |
| 下部タブ | LANE(選択レーンのパラメータ)/ VOLUME / FILTER / PAN(カーブ)/ MOD(ルーティング表) |

ストリップの **playing / selected は別物**。playing は `sceneSelect` に追従するので
オートメーション中に勝手に動き、selected はクリックしたときだけ動く。分けてあるのは
「タイムラインが別の Scene を鳴らしている間に、手元で1つを編集する」ができるようにする
ため。再生中はどれが聞こえているかのほうが重要なので、playing が視覚的に優先される。

ACTIVE は**コントロールであると同時に読み取り値**でもある。自前のトグル状態ではなく
パラメータを描画し、30Hz のタイマーで再描画する — ButtonAttachment の更新は
`AsyncUpdater` 経由なので、速いオートメーションのエッジは中間状態が消えうる。

### BlockGrid のマウス操作(Glitch 2 由来)

修飾キーを使わず、すべて単一ジェスチャで完結する:

| 操作 | 動作 |
|---|---|
| 空白部ドラッグ | ブロック作成(長さはドラッグ追従) |
| ブロック本体ドラッグ | 移動 |
| ブロック端ドラッグ | リサイズ |
| 右クリック / 右ドラッグ | 1個消去 / 複数消去 |
| 右ダブルクリック | レーン全消去 |
| レーンヘッダクリック | レーン選択 |

ドラッグ1回 = undo 1回(マウスダウンでトランザクションを開き、中間の変更が合流する)。

**Scene ブラウザでは再生中の Scene が編集中の Scene より視覚的に優先される。**
演奏中はどれを聴いているかのほうが、どれを開いているかより重要になるため。

---

## リアルタイム安全性

- `processBlock` 内でメモリ確保・ロック・I/O を行わない。バッファは `prepareToPlay` で確保
- `SceneStore::get()` は wait-free(acquire ロード1回)
- パラメータはスムージング、エフェクト切替は等パワークロスフェード
- どの遷移でもクリック/ポップが出ないこと(テストが隣接サンプル差分で検証)

---

## プリセット

**v2 Scene バンク**(`src/FactoryScenes.cpp`)— 4バンク14シーン。各バンクは v1 では
表現できなかった能力を1つずつ実演する構成:

| バンク | 実演する能力 |
|---|---|
| Held Envelopes | 1小節を跨ぐブロックで TapeStop が実際に停止に到達する |
| Swing & Odd Grids | Beats×Divisions と Swing(4×3、5×4) |
| Routed Modulation | カーブがレーンパラメータを直接駆動する |
| Playable Set | 強度順に並べた5 Scene。Scene オートメーションを昇順に動かすと激しくなる |

**v1 プリセット**(`src/FactoryPresets.cpp`、29個)も残っており、`PresetManager` 経由で
ロードできる。ただし内容はロード時に v2 へ変換される: v1 の 16 ステップは
beats=4 × divisions=4 と厳密に一致するため、`buildScenesTreeFromSteps` が丸めなしで
ブロックへ再エンコードする(隣接する ON は1ブロックに統合)。**変換先は Scene 0 では
なく Scene 60**。オーディオ側とエディタ側は独立に Scene を選ぶため、Scene 0 に書くと
「音は正しいがグリッドは空」になり、ロード失敗にしか見えない。

ユーザープリセットは `~/Library/Audio/Presets/Maniax/Stutter/*.xml`。

**プリセットは必ず「音が変わること」をテストすること。** 以下は実際に全部同時に
起きていた:

- `buildFullStateTree` が `version` を書かず、全 29 プリセットがバージョンガードで
  Init に差し替えられていた(ブラウザには名前が出るのに音は無変化)
- `FactoryScenes` がプラグイン本体から未参照で、4バンクがユーザーから到達不能だった
- バンクをロードしても Scene 0 に留まり、バンクは C4 以降にマップされているため無音
- v1 の `stepsOn` が読まれない `<Sequencer>` ノードに入ったままで、28個中20個が無音

構造(パースできる・ベイクできる)のテストは全て通っていた。**「選ぶと音が変わる」を
誰も検証していなかった**ことが、これら全てを同時に見逃した単一の原因。テストハーネス
自体にも3つの盲点があった: 1インスタンスで連続ロードすると前のバンクの Scene が
生き残って無音プリセットが「音あり」と測定される / 定常サイン波は Buffer 系レーンが
スライスを再生すると完全一致する / 1秒のレンダリングは16division中7までしか進まない。

---

## テストと検証

### 2つのターゲット

- **`stutter_tests`**(Catch2、55ケース / 2333アサーション、`ctest` で約1秒) — これがゲート。
  27タグで絞り込める(`[modulation]` だけなら0.04秒)
- **`render_test`**(311行) — 各レーンを WAV に書き出して人間が聴けるようにする。
  不連続性メトリクスも出力し、ゴールデンゲートがそれを比較する

音を出して人間が聴くのはテストフレームワークの仕事ではないので分けている。

### ゴールデンベースライン

`tests/fixtures/golden/` に v1.1.2 のレンダリング結果を SHA-256 で固定してある。
`verify-against-v1.sh` が現在のビルドと突き合わせる。

**8レーン中7レーンは今も v1 とビット一致**。Filter のみ、音声パス切替時に更新した
(差分は実測で最大1LSB / 24bit ≒ -138 dBFS の浮動小数点丸め差)。

ベースラインを更新する場合は**何を変えたから音が変わったのかをコミットメッセージに
必ず書くこと**。理由の無い更新は回帰の隠蔽と区別がつかない。詳細は
`tests/fixtures/golden/README.md`。

### ローカル検証

```sh
tools/fetch-pluginval.sh   # 初回のみ。CI と同じ pluginval を tools/bin へ
tools/validate.sh          # VST3 を strictness 8 で検証
```

**タグを打つ前に必ず実行すること。** v2.0.0 の初回リリースは、ローカルのテストが全て
通った状態で pluginval に落とされた。ctest とゴールデンゲートはこのクラスの不具合
(スレッド安全性、パラメータのファズ、バス構成)を見ない。

**合格しても所要時間を見ること。** pluginval のタイムアウトは1テストあたり30秒で、
開発機は CI ランナーより速いため「遅いが通る」状態が起こりうる。実際 v2.0.0 の回帰は
ローカルで45秒かけて**合格**し、CI では同じテストがタイムアウトした(修正後は16秒)。
`validate.sh` は30秒を超えると警告する。

**auval が `didn't find the component` を返す場合は、まず macOS を再起動すること。**
Core Audio の AU 登録は壊れることがあり、その状態では `auval -a` が Apple 製しか
返さない。実際に一度そうなっており、市販プラグイン(AmpliTube 5 など)も同じく
不可視だった。`AudioComponentRegistrar` の kill、`launchctl kickstart`、AU キャッシュ
削除のいずれも効かず、**再起動で解消した**(復旧後は 390 個中 332 個が第三者製)。

`validate.sh` はこの状態を「第三者製 AU が1つも見えない」ことで判定し、SKIP として
報告する。1つでも見えていれば auval の失敗は本物として扱う。

かつて auval は `MusicDeviceMIDIEvent を実装しているが type が aufx` という警告を
出していた。MIDI 入力を宣言していたことが原因で、`NEEDS_MIDI_INPUT FALSE` にした結果
シンボルごと消えた(`nm` で確認済み)。

### CI

`ctest` → `render_test` → ゴールデン比較 → ユニバーサルビルド検証(lipo)→
`pluginval --strictness-level 8`(VST3)→ `auval -v aufx Stt1 Manx`(AU)。

**どれか一つでも落ちればリリースは publish されない。** v2.0.0 の初回ビルドは
pluginval の Parameter thread safety で止まっており、このゲートは実際に機能している。

署名・公証(`MACOS_CERT_P12` / `MACOS_CERT_PASSWORD`)は secret が設定されていれば
実行され、無ければスキップされる。未設定のまま出荷したビルドは ad-hoc 署名になり、
Gatekeeper に拒否される。

---

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

JUCE 8.0.8 は CMake FetchContent で自動取得される。ビルド後、VST3 / AU は
`~/Library/Audio/Plug-Ins/` へ自動コピーされる。

### ユニバーサルビルド(arm64 + x86_64)

ローカルビルドの既定はホストアーキテクチャのみ(Apple Silicon なら arm64)。
CI が出荷するのはユニバーサルバイナリなので、その構成を再現するには明示する:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build -j8
```

**通常のローカルビルドではこの構成が検証されない。** リリース前には一度これで
ビルドし、`lipo -archs` が両アーキテクチャを報告することを確認すること。

---

## リポジトリ構成

```
CMakeLists.txt
src/
  PluginProcessor.{h,cpp}     チェーン、状態、ミラー
  PluginEditor.{h,cpp}
  FactoryScenes.{h,cpp}       v2 Scene バンク
  FactoryPresets.{h,cpp}      v1 プリセット
  PresetManager.{h,cpp}
  dsp/
    BlockSequencer.h          可変長ブロックのシーケンサー
    SceneSelector.h           オートメーション → Scene、ゲート
    ModulationEngine.h        ルーティング可能マトリクス
    LaneEffectV2.h            エフェクト契約(BlockContext / SampleContext)
    ParamDescriptor.h         パラメータ宣言(単一の情報源)
    ParamIndex.h              フラットなパラメータアドレス空間
    TimingMode.h              音価とレート表記
    CaptureBuffer.h           リングバッファ(絶対サンプル位置指定)
    CurveModulator.h          v1 由来の Volume/Filter/Pan
    effects/                  12レーン分
  state/
    SceneSnapshot.h           オーディオスレッドが読む POD
    SceneStore.{h,cpp}        publish / retire
    SceneDocument.h           UI が編集する ValueTree(undo 付き)
    SceneSchema.{h,cpp}       ツリー ⇔ スナップショット、カーブベイク
    LiveParamOverlay.h        ノブドラッグの高速経路
  ui/
    BlockGrid.{h,cpp}  SceneBrowser.{h,cpp}  ModRoutePanel.{h,cpp}
    CurveEditor.{h,cpp}  LaneParamPanel.{h,cpp}  HeaderBar.{h,cpp}
    BottomTabs.{h,cpp}  StutterLookAndFeel.{h,cpp}
tests/          Catch2 スイート + ゴールデンフィクスチャ
tools/render_test/  WAV レンダリング用ハーネス
```

---

## 既知の制約

- **v1 の state は読まない**。バージョンガードで Init にフォールバックする(意図的な
  判断)。v1 の**プリセット**は上記のとおり v2 のブロックへ変換してロードされる
- **`loopPolicy` は Forward のみ実装**。Palindrome / OneShot は保存・復元されるが
  `BlockSequencer` は PPQ から位置を導くだけで常に前進し、`reverseDirection` は false 固定。
  設定する UI が無いためユーザーが未実装パスに到達することはない
- **Texture チェーンの並べ替え UI がない**。`chainPosition` はシーケンサーが毎ブロック
  ソートに使っているが、全レーンが 0 のままで設定手段が無いため、実効順序は常にレーン
  順(グリッドの上から下)になる。既定の挙動としては筋が通っており壊れてはいないが、
  SPEC が Effectrix / ShaperBox 由来として挙げたドラッグ並べ替えは未実現
- **`LiveParamOverlay` は未使用**。ノブ編集は APVTS 経由でシーンへ書き戻される
  (`writeLaneParamToScene`)。SPEC が想定した「バンク再構築を避ける高速パス」は
  現状使っていない
- **レート表示の4倍ずれ**は v1.1.2 まで存在した。ラベル側を実態に合わせて修正済みで、
  プリセットの音は不変。`stutter::legacyRateIndexLabels()` が唯一の定義
- **オーバーサンプリングなし**。Crush / Distortion / Repitch は高設定でエイリアスする。
  グリッチ表現としては許容範囲という判断
- **ホストの Programs API は未対応**。プリセットは独自ブラウザのみ
