# Stutter

マルチFXグリッチシーケンサープラグイン(VST3 / AU / Standalone、macOS)。
iZotope Stutter Edit 2 / Cableguys ShaperBox 3 / Xfer LFO Tool / Illformed Glitch 2 / Sugar Bytes Effectrix 2 にインスパイアされた「いいとこ取り」設計。

> **macOS 専用**です。Windows / Linux はビルド定義上も対象外で、サポートしていません。

![UI](docs/images/overview.png)

## 機能

- **オートメーションで呼び出す 128 の Scene** — ブロック配置・全パラメータ・カーブを丸ごと
  記憶したスナップショット。画面で組み上げ、DAW のオートメーションレーンから呼び出す
- **12レーン × 可変長ブロックのシーケンサー** — Beats 1〜8 × Divisions 2〜8 と Swing。
  ブロックの「長さ」が音を決める(TapeStop が1小節かけて停止しきる、など)
  - Buffer系: Stutter / TapeStop / TapeStart / Reverse / Repitch / Stretcher / Shuffler
  - Texture系: Gate / Filter / Crush / Distortion
  - Send系: Delay(フィードバックを直列チェーンから隔離)
  - 切替は等パワークロスフェードでクリックレス
- **ルーティング可能なモジュレーションマトリクス** — 任意のカーブを任意のレーン
  パラメータへ。加えて v1 由来の Volume / Filter / Pan の3系統
- **オートメーション2本だけの制御面** — `Scene`(どれを呼び出すか)と `Active`(どこで
  効かせるか)。グリッチは大半の小節で無効であるべきなので、有効区間を決めるのは
  タイムラインの仕事。無効区間では原音がそのまま通る(無音にはならない)
- **ファクトリーコンテンツ** — v2 Scene バンク4種(14シーン)+ v1 由来のプリセット28個。
  ユーザープリセットは `~/Library/Audio/Presets/Maniax/Stutter/`
- カスタムダークUI(1200×800、比率固定リサイズ、発光プレイヘッド、ドラッグ描画グリッド)

## インストール

[リリースページ](https://github.com/maniax-jp/stutter/releases/latest)から
`Stutter-<version>-macOS.zip` をダウンロードし、展開して配置します。

| 形式 | 配置先 |
|---|---|
| VST3 | `~/Library/Audio/Plug-Ins/VST3/` |
| Audio Unit | `~/Library/Audio/Plug-Ins/Components/` |
| Standalone | 任意の場所(`/Applications` など) |

DAW を起動し直すとプラグインリストに現れます。Logic は初回のみ検証が走ります。

### ⚠️ 配布ビルドは署名・公証されていません

署名証明書が未設定のため ad-hoc 署名のみのビルドです。macOS が
「開発元を確認できないため開けません」と表示してブロックします。

**プラグイン(VST3 / AU)** — DAW のスキャンで弾かれるので、隔離属性を外します:

```sh
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Stutter.vst3
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/Stutter.component
```

**Standalone アプリ** — システム設定 → プライバシーとセキュリティ →「このまま開く」。

## ドキュメント

| 文書 | 対象 |
|---|---|
| [MANUAL.md](docs/MANUAL.md) | 使いかた(音楽制作者向け) |
| [SPEC.md](docs/SPEC.md) | 設計の意図・State スキーマ・ビルド・検証・既知の制約 |

## License

Stutter is licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)**. See [LICENSE](LICENSE) for the full text.

This project uses [JUCE 8](https://juce.com/), which is available under the AGPLv3 for open-source projects (or under a commercial JUCE license for closed-source use). Distributing Stutter under AGPL-3.0 satisfies JUCE's open-source licensing terms. If you hold a commercial JUCE license, you may relicense your own build/fork accordingly — this repository's default license is AGPL-3.0 unless you choose otherwise for your own distribution.
