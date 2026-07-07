# Stutter — Multi-FX Glitch Plugin (VST3 / AU, macOS)

## コンセプト
iZotope Stutter Edit 2 / Cableguys ShaperBox 3 / Xfer LFO Tool / Illformed Glitch 2 / Sugar Bytes Effectrix 2 の「いいとこ取り」:
- **Effectrix/Glitch 系**: ステップシーケンサーグリッドで複数エフェクトをパターン化
- **Stutter Edit 系**: スタッター(バッファリピート)+ピッチ/レート変化、テープストップ
- **LFO Tool / ShaperBox 系**: 描画可能なカーブエディタでボリューム/フィルター/パンをモジュレーション

## 技術スタック
- JUCE 8(CMake FetchContent、タグ `8.0.8` を優先、失敗時は最新の 8.x)
- C++20、CMake、フォーマット: VST3 + AU + Standalone
- 会社名 `Maniax`、プラグイン名 `Stutter`、プラグインコード等は適切に設定
- パラメータは `AudioProcessorValueTreeState`(APVTS)。ステップグリッド/カーブは APVTS の state ValueTree 内に保存(getStateInformation/setStateInformation で完全に永続化)

## DSP アーキテクチャ

### 全体構成
- ホストのトランスポート(BPM / PPQ / 再生状態)に同期。ホストが再生していない時は内部クロック(フリーラン)にフォールバック
- 常時書き込みの循環キャプチャバッファ(最低2秒、ステレオ)を持ち、各エフェクトはそこからスライスを取得
- シグナルチェーン: 入力 → キャプチャ → **ステップシーケンサーエフェクト群**(排他的またはブレンド)→ **グローバルモジュレーター**(Volume / Filter / Pan カーブ)→ Dry/Wet ミックス → 出力ゲイン

### ステップシーケンサー
- 8 レーン × 16 ステップ(1小節 = 16分音符単位)。将来のパターン拡張を考慮した設計だが v1 は 1 パターンで良い
- 各レーン = 1 エフェクト。ステップON でそのステップ区間中エフェクトが有効
- 上のレーンが優先(排他)だが、Gate/Filter/Crush 系は重ねがけ可能(カテゴリで区別: Buffer系=排他、Texture系=加算)
- ステップ切り替え時のクリックノイズ防止に短いクロスフェード(~5ms)必須

### エフェクトレーン(8種)
1. **Stutter** — キャプチャバッファのスライスをレート(1/4, 1/8, 1/16, 1/32, 1/64、三連/付点)でループ再生。パラメータ: レート、ループ長減衰(だんだん短く=Stutter Edit 的グラデーション)、ピッチスライド量
2. **TapeStop** — 再生速度を 1.0→0.0 へカーブ(指数/線形可変)で減速。ステップ区間長に合わせて減速時間をスケール。パラメータ: カーブ、減速時間
3. **TapeStart** — 逆:0→1.0 へ加速(スピンアップ)
4. **Reverse** — 直近キャプチャを逆再生。パラメータ: スライス長
5. **Repitch** — バリスピードでピッチダウン/アップしながらループ。パラメータ: 半音(-24..+24)、スライド
6. **Gate** — トランスゲート。パラメータ: レート、デューティ、シェイプ(矩形〜サイン)
7. **Filter** — SVF(LP/BP/HP)+ LFO スイープ。パラメータ: タイプ、カットオフ、レゾナンス、LFOレート/深さ
8. **Crush** — ビットクラッシュ + ダウンサンプル。パラメータ: ビット深度(16→1)、レート除算

### グローバルモジュレーター(LFO Tool / ShaperBox 風)
- 3 系統: **Volume**、**Filter**(カットオフ)、**Pan**
- それぞれ描画可能なカーブ(ブレークポイント + 各セグメントの曲率ハンドル)。プリセット形状(Saw down/up, Sine, Square, Sidechain duck, Steps)をワンクリック適用
- 周期: 1/1〜1/16(テンポ同期)。ON/OFF 個別
- カーブデータは補間テーブル化してオーディオレートで滑らかに適用(ジッパーノイズ禁止)

### グローバル
- Dry/Wet、Output Gain、シーケンサー ON/OFF(バイパスでカーブモジュレーターのみも可)

## UI(こだわりポイント — 最重要)
- サイズ: 900×620、リサイズ可能(比率固定)
- **ダークテーマ**: 背景 #14161c 系、アクセントはレーン毎の鮮やかなカラー(シアン/マゼンタ/オレンジ/グリーン等)。全体はフラット+微グロー
- レイアウト:
  - 上部バー: ロゴ、Dry/Wet、Output、シーケンサーON/OFF、(内部クロックBPM表示)
  - 中央: **ステップグリッド**(8レーン×16ステップ)。再生位置のプレイヘッドがリアルタイムで光って走る。ステップはドラッグで連続ON/OFF描画可能。レーン名クリックで下部にそのレーンのパラメータパネル表示
  - 下部: タブ切替 — 選択レーンのパラメータ / Volume・Filter・Pan カーブエディタ(ブレークポイントをドラッグ、ダブルクリックで追加/削除、右ドラッグで曲率)
- カスタム LookAndFeel: 独自ロータリースライダー(アーク+グロー)、独自トグル
- 60fps 級の滑らかな再描画(ただし Timer は 30〜60Hz、無駄な repaint 禁止)。オーディオスレッド→UI は atomic / FIFO で(ロック禁止)

## リアルタイム安全性(必須)
- processBlock 内でメモリ確保・ロック・I/O 禁止。バッファは prepareToPlay で確保
- パラメータはスムージング(SmoothedValue)
- どのエフェクト切替でもクリック/ポップが出ないこと

## プリセットシステム(必須)
- ファクトリープリセットを **24個以上** 内蔵。カテゴリ: Stutter系 / Tape系 / Gate・Sidechain系 / Glitch(複合)系 / Filter・Texture系
- 各プリセットは全状態(ステップグリッド、レーンパラメータ、カーブ、Mix)を含む
- 実装: プリセットはJSON(またはValueTree XML)としてBinaryDataに埋め込み。ユーザープリセットは `~/Library/Audio/Presets/Maniax/Stutter/` に保存/読込
- UI: ヘッダーバーにプリセットブラウザ(前後ボタン+名前クリックでリスト表示、Save ボタン)
- 良いプリセット例: "Classic Stutter Build"(小節後半にレート漸増スタッター)、"Tape Drop"(4拍目にテープストップ)、"Sidechain Pump"(Volumeカーブでダッキング)、"Glitch Storm"(複合)、"Trance Gate 16th" など、音楽的に実用的なもの

## ビルド・検証
- `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build` が通ること
- AU は `auval -v aufx <code> <manu>` がパスすること(Standalone/VST3 と併せて確認)

## リポジトリ構成(目安)
```
CMakeLists.txt
src/
  PluginProcessor.{h,cpp}
  PluginEditor.{h,cpp}
  dsp/  (Engine, CaptureBuffer, 各エフェクト, CurveModulator, StepSequencer)
  ui/   (LookAndFeel, StepGrid, CurveEditor, LaneParamPanel, HeaderBar)
```

## State構造(フェーズ1で確定・実装済み)

`getStateInformation` / `setStateInformation` は APVTS の `copyState()` で得られる
ValueTree(`Parameters` ルート、タグ名は APVTS 依存)に、構造的データ(ステップグリッド・
カーブ)を子ノードとして追加した上で XML シリアライズして保存する。読み込み時は
子ノードを取り除いてから `apvts.replaceState()` に渡し、子ノードは別途
`StepSequencer::fromValueTree` / `CurveModulator::fromValueTree` に渡す。

### 1. APVTS パラメータ(自動化可能な値はすべてここ)

パラメータIDの命名は `src/dsp/ParameterIDs.h`(`stutter::ID` 名前空間)に集約。

- グローバル: `dryWet`, `outputGain`(dB), `sequencerOn`(bool), `hostSync`(bool),
  `internalBpm`(内部フリーランクロックのBPM、ホスト停止時に使用)
- レーン毎: `lane{N}_{paramName}`(N=0..7)。レーン番号とエフェクトの対応は固定:
  - 0 = Stutter (`rate` [choice], `decay` [0..1], `pitchSlide` [-24..24 st])
  - 1 = TapeStop (`curve` [0..1], `time` [0..1])
  - 2 = TapeStart (`curve` [0..1], `time` [0..1])
  - 3 = Reverse (`sliceLen` [choice])
  - 4 = Repitch (`semitones` [-24..24], `slide` [0..1])
  - 5 = Gate (`rate` [choice], `duty` [0.01..0.99], `shape` [0..1])
  - 6 = Filter (`type` [choice: LP/BP/HP], `cutoff` [20..20000 Hz], `resonance` [0..0.99],
    `lfoRate` [choice], `lfoDepth` [0..1])
  - 7 = Crush (`bitDepth` [1..16], `rateDiv` [0..1])

UI(フェーズ2)はこれらを `AudioProcessorValueTreeState::getParameter(id)` /
`SliderAttachment` 等で直接バインドできる。レーンのカテゴリ(Buffer=排他 / Texture=加算)は
`LaneEffect::getCategory()` で取得可能(0-4=Buffer、5-7=Texture)。

### 2. ステップグリッド(構造データ、`Sequencer` ノード)

APVTS の state ツリーの直下の子として追加される非パラメータ ValueTree:

```
Sequencer
  Lane index=0
    Step index=0  on=true/false
    Step index=1  on=...
    ...(16個, index=0..15)
  Lane index=1
    ...
  ...(8個, index=0..7)
```

- 読み書き API: `StepSequencer::getStep(lane, step)` / `setStep(lane, step, bool)`
  (UIスレッドから直接呼んでよい。内部はただの `bool` 配列で、オーディオスレッドは
  `processBlock` 内でのみ参照 — ロック不要、書き込みはブール1個の単純代入なので
  audio-thread から見て tearing の実害は無視できる想定)
- 永続化: `StepSequencer::toValueTree()` / `fromValueTree()`
- 将来のマルチパターン拡張は `Sequencer` の下に `Pattern index=N` を挟む形で
  後方互換に拡張できるよう、意図的にフラットな `Lane > Step` 構造にしてある

### 3. カーブモジュレーター(構造データ、`Curves` ノード)

```
Curves
  Curve name="Volume" enabled=true/false syncDiv=<int>
    Point position=0.0 value=0.5 curvature=0.0
    Point position=... value=... curvature=...
    ...(可変長、最低2点)
  Curve name="Filter" ...
  Curve name="Pan" ...
```

- `position`: 0..1(サイクル内の位置)、`value`: 0..1(モジュレーション量。Volumeは
  0.5=unity gain・0..1が0..2倍、Panは0.5=中央・0/1が左右振り切り、Filterは
  0..1を200Hz〜20kHzの指数マッピング)
- `curvature`: -1..1(そのポイントから次のポイントへのセグメントの曲率。0=線形、
  正=イーズイン、負=イーズアウト)
- `syncDiv`: テンポ同期分周インデックス(1/1〜1/16 相当。`CurveModulator` 内部の
  `cyclesPerPpqQuarter` 相当テーブルに対応させる。フェーズ2のUIはこの整数を
  分数表示に変換して見せればよい)
- 読み書き API: `CurveModulator::setPoints/getPoints`(ブレークポイント配列)、
  `applyPreset(name)`("SawDown"/"SawUp"/"Sine"/"Square"/"SidechainDuck"/"Steps")、
  `setEnabled/isEnabled`, `setSyncDivision/getSyncDivision`
- 内部でブレークポイントを 1024 点のテーブルに事前計算(`bakeTable()`)し、
  オーディオスレッドは `getValueAtPhase(phase)` で読むだけ(ロックフリー・
  補間読み出しのみ)。**UIは編集の都度 `setPoints`/`applyPreset` を呼べば
  自動的に再ベイクされる**(この再ベイクはオーディオスレッドでは呼ばないこと)
- 永続化: `CurveModulator::toValueTree()` / `fromValueTree()`。
  `StutterAudioProcessor::getStateInformation` 側で3系統(Volume/Filter/Pan)を
  `name` プロパティで区別して1つの `Curves` ノードにまとめている
- アクセス: `StutterAudioProcessor::getCurve(stutter::ModTarget::Volume|Filter|Pan)`

### 4. プリセットとの関係(フェーズ2以降で実装)

上記の「パラメータ + Sequencer + Curves」を1つの ValueTree にまとめたものが
そのままプリセット1個分のデータになる(`getStateInformation` が返すXMLと同一形式)。
フェーズ2のプリセットブラウザは、この完全な状態スナップショットをプリセット名と
紐づけて保存/読込すればよい(BinaryData埋め込みのファクトリープリセット、
`~/Library/Audio/Presets/Maniax/Stutter/` のユーザープリセットとも同一シリアライズ経路を再利用可能)。
