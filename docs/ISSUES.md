# Stutter プロジェクト問題点まとめ

調査日: 2026-07-10  
対象: リポジトリ `main`（HEAD: `a63b7f7` 付近）  
調査範囲: ソースコード・SPEC・README・CI・`.tmp` 検証ログ・ビルド成果物

---

## 1. プロジェクト概要（現状）

| 項目 | 状態 |
|------|------|
| 種別 | マルチFXグリッチシーケンサー（JUCE 8 / C++20） |
| フォーマット | VST3 / AU / Standalone（macOS 想定） |
| コア機能 | 8レーン×16ステップ、8種エフェクト、カーブ3系統、プリセット29個 |
| ビルド | ローカル Release ビルド済み、VST3 は pluginval strictness 8 で SUCCESS |
| 既知の修正済み | バッファ読み出しノイズ（アンカー方式）、Init プリセット曲線残留、DSP レビュー指摘の大半 |

**結論:** フェーズ1〜3は実装済みで、主要な音質バグ（キャプチャ基準点のずれによるノイズ等）は修正済み。一方で **UI と DSP が接続されていないパラメータ**、**スレッド安全性の穴**、**配布・検証の未完** が残っている。

---

## 2. Critical（機能が仕様どおり動かない）

### 2.1 `sequencerOn` が DSP に反映されない

- APVTS に `sequencerOn` があり、HeaderBar の SEQ トグルは正しくバインドされている。
- しかし `processBlock` / `updateTransportAndSequence` 内で **`sequencer.setEnabled(...)` を一度も呼んでいない**。
- `StepSequencer::sequencerEnabled` はデフォルト `true` のまま固定。
- **結果:** UI でシーケンサーを OFF にしても、ステップエフェクトは動き続ける。SPEC の「バイパスでカーブモジュレーターのみ」が実現できていない。

**修正方針:** ブロック先頭で `sequencer.setEnabled(apvts.getRawParameterValue(ID::sequencerOn)->load() > 0.5f)` を呼ぶ（または `processBlock` 内で同等のガード）。

### 2.2 `internalBpm` が二重管理され、実質未使用

- APVTS パラメータ `internalBpm`（40–240、デフォルト 120）が存在する。
- 輸送計算は **別メンバ** `std::atomic<double> internalBpm` を読む（`PluginProcessor.cpp`）。
- APVTS 側の値は **どこからも transport に同期されない**。`setInternalBpm` も UI から呼ばれていない。
- プリセットに `internalBpm` を保存しても、フリーラン時の BPM には効かない可能性が高い。
- BPM 表示は atomic 側のみ。ユーザーが INTERNAL BPM を編集する UI もない。

**修正方針:** 単一の情報源に統一する（推奨: APVTS を正とし、processBlock で raw 値を読む。または atomic を廃止）。フリーラン時の BPM 編集 UI（ノブ/テキスト）を追加。

### 2.3 `hostSync` に UI がない

- DSP は `hostSync` を正しく参照している（ホスト再生中かつ ON のとき PPQ 同期）。
- しかし HeaderBar 等に **トグルが存在しない**。ホストオートメーション／プリセットでのみ変更可能。
- ユーザーが FREE 表示時に「強制ホスト同期」や「強制フリーラン」を切り替えられない。

**修正方針:** HeaderBar の BPM 表示付近に Host Sync トグルを追加。

### 2.4 カーブの同期分周（`syncDiv`）に UI がない

- `CurveModulator` は `setSyncDivision` / `getSyncDivision` を持ち、プリセットは `syncDiv` を設定する（例: Sidechain Pump = 1/4）。
- CurveEditor は Enable + 形状プリセットのみ。**分周（1/1〜1/16）を変更する UI が無い**。
- ユーザーがロード後にポンプ速度を変えられない（プリセット再ロード以外）。

**修正方針:** カーブエディタのツールバーに ComboBox（1/1, 1/2, 1/4, 1/8, 1/16）を追加。

---

## 3. High（クラッシュ・音切れ・データ競合のリスク）

### 3.1 CurveModulator のテーブルベイクとオーディオスレッドの競合

- UI / プリセットロード時に `setPoints` → `bakeTable()` が `table[]` を書き換える。
- オーディオスレッドは `getValueAtPhase` で **同じ `table` をロックなしで読む**。
- コメント上は「オフスレッドでベイク」とあるが、**ダブルバッファや世代カウンタが無い**。
- 再生中にカーブをドラッグすると、理論上テーブルの途中状態を読む可能性（クリック・一瞬の変な値）。

**修正方針:** 2 面テーブル + atomic index、または `AbstractFifo` でポイント更新をオーディオ側でベイク、など。

### 3.2 ステップグリッドの UI / オーディオ間データレース

- `StepSequencer::steps` は `bool` 配列。UI スレッドが `setStep`、オーディオが `getStep` 相当で読む。
- SPEC は「tearing の実害は無視できる」と記載。C++ メモリモデル上は data race。
- 実害は小さいが、ASAN/TSAN や将来の最適化で問題化する余地がある。

**修正方針:** `std::atomic<bool>` 化、またはブロック頭でコピーしたスナップショットを使う。

### 3.3 Editor 破棄後の `onPresetLoaded` ダングリング

- Editor コンストラクタで `presetManager.onPresetLoaded = [this] { ... }` を設定。
- デストラクタで **コールバックをクリアしていない**。
- Editor を閉じた後にプリセット操作が走ると、破棄済み `this` を呼ぶ可能性がある（ホストがエディタを破棄してもプロセッサは生きる典型パターン）。

**修正方針:** `~StutterAudioProcessorEditor` で `onPresetLoaded = nullptr`（または weak トークン）。

### 3.4 `prepareToPlay` 宣言より大きい `processBlock` ブロック

- dry scratch は `prepareToPlay` サイズで確保。超過時は切り詰めて処理（アサートあり）。
- ホストによってはブロックサイズが後から増える。縮退は RT セーフだが **音の欠落／ドライウェット不一致** が起きうる。

**修正方針:** 余裕を持った最大サイズで確保する、または超過時に安全にスキップ＋ログ。

### 3.5 AU 検証の失敗（配置・登録依存）

- `.tmp/pluginval_au_log.txt`: ビルド成果物パス上の AU に対し `Num plugins found: 0` → **FAILURE**。
- VST3 は同条件で SUCCESS。
- AU は macOS が `~/Library/Audio/Plug-Ins/Components` 等に登録しないとスキャンできないことが多い。`COPY_PLUGIN_AFTER_BUILD` が OFF の CI/ローカルでは再現しやすい。
- README は `auval -v aufx Stt1 Manx` を推奨するが、**CI では auval を実行していない**。

**修正方針:** CI で `COPY_PLUGIN_AFTER_BUILD=ON` 相当のコピー後に `auval`、または pluginval 前に Components へコピー。

---

## 4. Medium（UX・仕様ギャップ・配布）

### 4.1 ユーザープリセットの削除 UI がない

- Save のみ。ディスク上の `~/Library/Audio/Presets/Maniax/Stutter/*.xml` を手動削除する必要がある。
- 上書きは可能だが、誤保存の整理が困難。

### 4.2 ホスト「プログラム」API が実質空

- `getNumPrograms() == 1`、名前は空。ファクトリープリセットは独自ブラウザのみ。
- 一部ホストのプリセットメニュー／A/B との連携が弱い。

### 4.3 コードサイニング・公証がテンプレート止まり

- CI は証明書 secret があれば codesign / notarytool する。
- **stapler staple が TODO のまま**（zip 内バンドルへのチケット埋め込み未実装）。
- 未設定時は ad-hoc 署名のまま配布物になる（Gatekeeper でブロックされやすい）。

### 4.4 ローカルビルドが arm64 のみ

- 手元の VST3 バイナリ: `Mach-O 64-bit bundle arm64`。
- CI は `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` で universal。
- README のビルド手順に universal 指定が無く、Intel Mac 向けローカル成果物と差が出る。

### 4.5 テストハーネスが CI に乗っていない

- `tools/render_test`（不連続性メトリクス・Init 残渣チェック）は `STUTTER_BUILD_TESTS=ON` 時のみ。
- ノイズ修正の回帰防止が **手動依存**。CI は pluginval（VST3）のみ。

### 4.6 プラットフォーム限定

- ユーザープリセットパスが `~/Library/Audio/Presets/...` 固定（macOS 専用）。
- Windows / Linux はビルド定義上も対象外。意図的なら README に明記を推奨。

### 4.7 ライセンスファイルが無い

- ルートに `LICENSE` が無い（JUCE 依存は GPL/Commercial 等の制約あり）。
- 公開リポジトリ（`github.com/maniax-jp/stutter`）としては法的・配布上のリスク。

### 4.8 Texture レーンのクロスフェードが線形

- Buffer レーンは等パワー（sin 曲線）。Texture は線形 `st.gain` ブレンド。
- Gate/Filter/Crush のオンオフでわずかなレベルディップの可能性（軽微）。

### 4.9 マルチパターン未実装

- SPEC は「v1 は 1 パターンで良い」と明記。現時点では問題ではなく、**将来拡張の未着手**。

### 4.10 プリセット埋め込み方式が SPEC と異なる

- SPEC: BinaryData 埋め込み。
- 実装: `FactoryPresets.cpp` で ValueTree をコード生成（動作上は問題なし）。
- ドキュメントと実装のズレ。

---

## 5. Low（コード品質・デッドコード・ドキュメント）

| ID | 内容 |
|----|------|
| L1 | `StepSequencer::freeRunPpq` は prepare で 0 にするだけで未使用（輸送は Processor 側） |
| L2 | `wasHostPlaying` は代入のみで後続ロジック無し |
| L3 | `applyGlobalModulators` 内の `syncDivisions[]` は `ignoreUnused` されたデッド配列 |
| L4 | サンプル単位の全レーン処理は CPU 負荷が高い（最適化余地。現状は品質優先で妥当な可能性） |
| L5 | `getTailLengthSeconds() == 0`。クロスフェード中の短いテールをホストに伝えない |
| L6 | 倍精度処理非対応（`SupportsDoublePrecision: no`）。一般的だが記載なし |
| L7 | `.tmp/` にビルドログ・WAV・検証結果が大量（gitignore 済み）。作業メモが正規 docs と分離 |
| L8 | Plugin programs 名空、pluginval 上 `Num programs: 0` 表示（JUCE/VST3 の都合の可能性） |
| L9 | Curve 形状の UI 再描画がポイント評価を再実装（DSP の bake と二重実装、ドリフトリスク） |
| L10 | ファクトリー 28 + Init = 29 は README と一致。設計書は 28 と表記（Init 別）で問題なし |

---

## 6. ビルド・CI・検証マトリクス

| チェック | 状態 | 備考 |
|----------|------|------|
| `cmake --build` Release | 成功（ローカル） | arm64 のみの可能性 |
| pluginval VST3 level 8 | SUCCESS（`.tmp/pluginval_final2.txt`） | |
| pluginval AU | FAILURE | スキャン 0 件（配置依存） |
| auval | CI 未実施 | README のみ |
| render_test 全レーン | 合格（`.tmp/render_after2`） | CI 未組み込み |
| Universal binary | CI で検証 | ローカル手順不足 |
| Notarization stapler | 未実装 | TODO コメントあり |
| ユニットテスト / ASAN | なし | |

---

## 7. すでに対応済みで再発防止に残すべきもの

以下は **修正済み**。問題点ではなく、回帰テストの根拠として記録。

1. **CaptureBuffer 相対読みのブロックずれ** → 絶対座標アンカー + `readInterpolatedAbsolute`
2. **ループ／スライス境界のクリック** → ワープクロスフェード、retrigger smoother
3. **Gate 矩形エッジのクリック** → 最小 1.5ms スルー
4. **TapeStop/Start の毎ステップ再トリガー** → `ContinueThroughRun`
5. **processBlock 内 `setSize`** → prepare 確保のみ
6. **Filter SVF 発振・係数毎サンプル更新** → クランプ + 制御レート更新
7. **Init / プリセット切替での曲線残留** → `fromValueTree` 全リセット、Filter 中立値 1.0
8. **Sine プリセットが三角波** → 16 点サンプリング

`render_test` を CI に入れると、これらの再発を機械的に防げる。

---

## 8. 優先度付きアクションリスト

### P0（すぐ直すべきバグ）

1. `sequencerOn` → `sequencer.setEnabled`
2. `internalBpm` の APVTS / atomic 統一 +（必要なら）BPM UI
3. Editor 破棄時の `onPresetLoaded` クリア

### P1（仕様どおりの操作性）

4. Host Sync トグル UI
5. カーブ syncDiv セレクタ UI
6. ユーザープリセット削除

### P2（堅牢性・配布）

7. Curve テーブルのダブルバッファ
8. CI に `auval` と `render_test`
9. stapler 実装、ライセンス方針の明示
10. README に universal ビルド手順

### P3（改善）

11. ステップ配列の atomic / スナップショット
12. ホスト Programs 連携
13. CPU 最適化（ブロック処理化）
14. マルチパターン

---

## 9. ファイル参照（調査の入口）

| 領域 | パス |
|------|------|
| 仕様 | `SPEC.md` |
| エントリ | `src/PluginProcessor.{h,cpp}` |
| シーケンサー | `src/dsp/StepSequencer.h` |
| キャプチャ | `src/dsp/CaptureBuffer.h` |
| カーブ | `src/dsp/CurveModulator.h` |
| UI | `src/ui/*` |
| プリセット | `src/PresetManager.*`, `src/FactoryPresets.*` |
| CI | `.github/workflows/build.yml` |
| 検証ハーネス | `tools/render_test/` |
| 過去修正メモ | `.tmp/NOISE_FIX.md`, `.tmp/DSP_FIXES.md` |

---

## 10. まとめ

本プロジェクトは、コア DSP・UI・プリセット・CI の骨格が揃った **完成度の高い v1 手前** の状態にある。音の致命的ノイズは修正済みで、VST3 の pluginval も通っている。

一方、**ヘッダーの SEQ トグルが音に効かない**、**BPM / Host Sync / カーブ分周の操作性が不完全**、**プリセットコールバックとカーブテーブルのスレッド安全性**、**AU 検証と公証の穴** が、製品化・実使用での主な阻害要因である。

P0〜P1 を潰せば「仕様どおりに操作できるプラグイン」に近づき、P2 で配布・回帰が安定する。
