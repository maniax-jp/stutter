# Stutter

マルチFXグリッチシーケンサープラグイン(VST3 / AU / Standalone、macOS)。
iZotope Stutter Edit 2 / Cableguys ShaperBox 3 / Xfer LFO Tool / Illformed Glitch 2 / Sugar Bytes Effectrix 2 にインスパイアされた「いいとこ取り」設計。

![UI](docs/screenshot.png)

## 機能

- **8レーン × 16ステップのエフェクトシーケンサー**(ホストテンポ同期、停止時は内部クロック)
  - Stutter(レート/ループ長減衰/ピッチスライド)、Tape Stop、Tape Start、Reverse、Repitch、Gate、Filter(SVF+LFO)、Crush
  - Buffer系レーンは排他、Texture系レーンは重ねがけ可能。切替は等パワークロスフェードでクリックレス
- **描画可能なカーブモジュレーター 3系統**(Volume / Filter / Pan)— ブレークポイント+曲率、プリセット形状6種、テンポ同期
- **ファクトリープリセット 29個**(Init + Stutter系6 / Tape系5 / Gate&Sidechain系6 / Glitch系6 / Filter&Texture系5)+ ユーザープリセット(`~/Library/Audio/Presets/Maniax/Stutter/`)
- カスタムダークUI(900×620、比率固定リサイズ、発光プレイヘッド、ドラッグ描画グリッド)

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

JUCE 8.0.8 は CMake FetchContent で自動取得。ビルド後、VST3/AU は `~/Library/Audio/Plug-Ins/` に自動コピーされる。

## 検証

- `pluginval --strictness-level 8` パス(VST3)
- AU: `auval -v aufx Stt1 Manx`

## ドキュメント

仕様・State構造は [SPEC.md](SPEC.md) を参照。
