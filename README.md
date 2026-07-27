# Stutter

マルチFXグリッチシーケンサープラグイン(VST3 / AU / Standalone、macOS)。
iZotope Stutter Edit 2 / Cableguys ShaperBox 3 / Xfer LFO Tool / Illformed Glitch 2 / Sugar Bytes Effectrix 2 にインスパイアされた「いいとこ取り」設計。

> **macOS 専用**です。Windows / Linux はビルド定義上も対象外で、サポートしていません。

![UI](docs/images/overview.png)

## 機能

- **MIDI ノートで切り替える 128 の Scene** — ブロック配置・全パラメータ・カーブを丸ごと
  記憶したスナップショット。エフェクトを楽器のように演奏できる
- **12レーン × 可変長ブロックのシーケンサー** — Beats 1〜8 × Divisions 2〜8 と Swing。
  ブロックの「長さ」が音を決める(TapeStop が1小節かけて停止しきる、など)
  - Buffer系: Stutter / TapeStop / TapeStart / Reverse / Repitch / Stretcher / Shuffler
  - Texture系: Gate / Filter / Crush / Distortion
  - Send系: Delay(フィードバックを直列チェーンから隔離)
  - 切替は等パワークロスフェードでクリックレス
- **ルーティング可能なモジュレーションマトリクス** — 任意のカーブを任意のレーン
  パラメータへ。加えて v1 由来の Volume / Filter / Pan の3系統
- **MIDI 演奏レイヤー** — Play Mode(Auto / MIDI)、トリガークオンタイズ(早入力を許す)、
  Release モード5種(Scene ごと)、Scene Lock
- **ファクトリーコンテンツ** — v2 Scene バンク4種(14シーン)+ v1 由来のプリセット28個。
  ユーザープリセットは `~/Library/Audio/Presets/Maniax/Stutter/`
- カスタムダークUI(1200×800、比率固定リサイズ、発光プレイヘッド、ドラッグ描画グリッド)

> **v1.1.2 との互換性はありません。** v1 で保存したプロジェクトの状態は読み込まず、
> Init にフォールバックします(バージョンガードによる意図的な判断)。v1 のファクトリー
> プリセット28個は v2 のブロックへ変換して同梱しています。

## 使いかた

エンドユーザー向けの操作説明は **[MANUAL.md](docs/MANUAL.md)** を参照。

## ビルド

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

JUCE 8.0.8 は CMake FetchContent で自動取得。ビルド後、VST3/AU は `~/Library/Audio/Plug-Ins/` に自動コピーされる。

### ユニバーサルビルド(arm64 + x86_64)

デフォルトのローカルビルドはホストアーキテクチャ(Apple Silicon Mac では arm64 のみ)になる。
Intel Mac でも動くユニバーサルバイナリを作る場合は `CMAKE_OSX_ARCHITECTURES` を指定する(CI もこの設定でビルドしている):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build -j8
```

## 検証

CI(macos-14)が以下を順に実行し、どれか一つでも落ちればリリースは publish されない:

- `ctest` — Catch2 スイート(54ケース / 2186アサーション)
- `render_test` + **ゴールデンベースライン照合** — 8レーンのレンダリング結果を v1.1.2 と
  SHA-256 で比較。DSP の意図しない変化を検出する
- ユニバーサルバイナリ検証(`lipo`)
- `pluginval --strictness-level 8`(VST3)
- `auval -v aufx Stt1 Manx`(AU)

## ドキュメント

| 文書 | 対象 |
|---|---|
| [MANUAL.md](docs/MANUAL.md) | 使いかた(音楽制作者向け) |
| [SPEC.md](docs/SPEC.md) | 設計の意図・State スキーマ・既知の制約 |

個々のクラスの詳細は各ヘッダの doc comment が一次情報。

## License

Stutter is licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)**. See [LICENSE](LICENSE) for the full text.

This project uses [JUCE 8](https://juce.com/), which is available under the AGPLv3 for open-source projects (or under a commercial JUCE license for closed-source use). Distributing Stutter under AGPL-3.0 satisfies JUCE's open-source licensing terms. If you hold a commercial JUCE license, you may relicense your own build/fork accordingly — this repository's default license is AGPL-3.0 unless you choose otherwise for your own distribution.
