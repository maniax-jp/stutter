# Stutter v2 — Multi-FX Glitch Sequencer (VST3 / AU / Standalone, macOS)

MIDI ノートで **Scene** を切り替えて演奏するマルチFXグリッチシーケンサー。
可変長ブロックのパターン、任意パラメータへルーティング可能なモジュレーション、
12レーンのエフェクトを持つ。

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
ドラッグ中に毎回約10MBのバンクを再構築することになる。ノブのドラッグのようなスカラー編集は
`LiveParamOverlay`(lock-free)を通り、バンク再構築を起こさない。

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

`GestureEngine` はシーケンサーの**前**で MIDI を消費する。チャンク内で届いたノートが
そのチャンクのレンダリング前に Scene を切り替えられるようにするため。

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
- **カテゴリ**: Buffer(排他、レーン順で解決)/ Texture(直列に積む、`chainPosition` で
  並べ替え可能)/ Send(Texture の後段、リターンバスへ)
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

### GestureEngine

- **Play Mode**: Auto(常時オン、v1 相当)/ MIDI(ノートを押している間だけ wet)
- **ゲートはシーケンサーのミュートではなく Dry/Wet ミックス上のランプ**。これにより
  遷移が構造的にクリックレスになる。実測で最大ステップ 0.004167(48kHz の5msランプが
  意味する 1/240 と一致)
- **ゲートを閉じると wet は silence ではなく dry に collapse する**。ノートを離したとき
  素材が残るべきで、トラックに穴を開けるべきではない。副次的に、Auto モードでは
  ゲートが 1.0 に留まりミックス式が v1 と同一の計算に退化する
- **クオンタイズは早入力を許す**: 境界の半グリッド以内に早く到達したノートは、1グリッド
  待つのではなくその境界で発火する。演奏者がビートを先取りできる(SE2 の意味論)
- **Release モード5種**: OnGrid / FullGesture / Latch / Instant / Stick
- **Scene Lock**: ノートを Scene 選択には使わずゲートにのみ使う。演奏中に別の Scene を
  編集できる
- 複数ノート保持時は**最後の1つを離したときだけ**ジェスチャが終わる

### これらがどこに保存されるか

| 設定 | 格納先 | 理由 |
|---|---|---|
| Play Mode / Scene Lock / Quantize | state ツリーのルートプロパティ | セッション全体の設定であり、Scene ごとに変わるものではない |
| Release モード | **Scene ごと** | 鍵盤ごとに離しかたを変えられることに意味がある(SE2 と同じ) |

**いずれも APVTS パラメータにしていない。** これらは連続値ではなくモード切替であり、
Play Mode をフレーズ途中でオートメーションされると「ノートが鳴るかどうか」自体が
変わる。誰も要求していない挙動をホストが作り出せてしまうため、意図的に自動化対象から
外している。

そのため APVTS が運んでくれず、`getStateInformation` が明示的に書く必要がある。
`loadInitState` でも明示的に Auto へ戻す — MIDI モードはノートが来るまで無音なので、
前のパッチから引き継ぐと「プラグインが壊れている」ようにしか見えない。

**ノート → Scene のマッピングは現状 identity 固定。** `setNoteMapping` は実装済みだが
プロセッサは `setIdentityMapping()` しか呼んでおらず、任意の鍵盤に任意の Scene を
割り当てる UI はまだない。

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
| Scene ストリップ | 鍵盤表示。どのノートに Scene があるか、どれが鳴っているか、どれを編集中か |
| 演奏バー | Play Mode / Quantize / Release / Scene Lock |
| ブロックグリッド | 12レーン × 可変 division。可変長ブロックの描画・編集、発光プレイヘッド |
| 下部タブ | LANE(選択レーンのパラメータ)/ VOLUME / FILTER / PAN(カーブ)/ MOD(ルーティング表) |

演奏バーがヘッダではなく鍵盤の直下にあるのは、これらが「ノートを弾いたときに何が
起きるか」を決める設定であり、判断している最中にユーザーが見ているのが鍵盤だから。

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
| Playable Set | C/D/E/F/G に強度順で配置、MIDI 演奏用 |

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

- **`stutter_tests`**(Catch2、35ケース / 1310アサーション、`ctest` で0.27秒) — これがゲート。
  24タグで絞り込める(`[modulation]` だけなら0.038秒)
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

### CI

`ctest` → `render_test` → ゴールデン比較 → universal build 検証(lipo)→
`pluginval --strictness-level 8` → `auval`。

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
    GestureEngine.h           MIDI → Scene、ゲート
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
- **ノート → Scene マッピングは identity 固定**。`setNoteMapping` は実装済みだが UI がない
- **レート表示の4倍ずれ**は v1.1.2 まで存在した。ラベル側を実態に合わせて修正済みで、
  プリセットの音は不変。`stutter::legacyRateIndexLabels()` が唯一の定義
- **オーバーサンプリングなし**。Crush / Distortion / Repitch は高設定でエイリアスする。
  グリッチ表現としては許容範囲という判断
- **ホストの Programs API は未対応**。プリセットは独自ブラウザのみ
