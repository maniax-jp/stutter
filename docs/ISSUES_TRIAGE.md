# docs/ISSUES.md トリアージ(オーケストレーター判断)

## 対応する(妥当な指摘)

### パッケージA: DSP・状態・スレッド安全性
- **2.1** sequencerOn が DSP に未配線 → processBlock で sequencer.setEnabled を呼ぶ
- **2.2(DSP側)** internalBpm の二重管理 → APVTS を唯一の情報源に統一(atomicメンバ廃止、rawParameterValueを読む)。プリセット保存値が効くように
- **3.1** CurveModulator のテーブルベイク競合 → 2面テーブル+atomicインデックス(publish時にflip)。UIスレッドからのsetPoints/applyPresetは裏面にベイクしてflip
- **3.2** ステップ配列の data race → std::atomic<bool> 化(relaxedで十分)
- **3.3** Editor破棄後の onPresetLoaded ダングリング → デストラクタで nullptr クリア
- **3.4** prepareToPlay宣言超過ブロック → チャンク分割処理(確保なしで全サンプル処理、音欠落なし)
- **4.8** Textureレーンのクロスフェードを等パワー化(Bufferレーンと同じsin曲線)
- **L1/L2/L3** デッドコード削除(freeRunPpq, wasHostPlaying, syncDivisions[])

### パッケージB: UI(Aの後、internalBpm統一に依存)
- **2.3** Host Sync トグルを HeaderBar のBPM表示付近に追加(既存トグルスタイル)
- **2.4** カーブエディタのツールバーに syncDiv ComboBox(1/1, 1/2, 1/4, 1/8, 1/16)。変更時 setSyncDivision + ダーティフラグ
- **2.2(UI側)** FREE(内部クロック)時に BPM を編集できるUI(BPM表示をドラッグ/ダブルクリック編集、APVTSのinternalBpmにバインド)
- **4.1** ユーザープリセットの削除(ブラウザのUserセクションに削除手段。確認ダイアログ付き)

### パッケージC: CI・ドキュメント・ライセンス
- **3.5/6章** CI に auval を追加: ビルド後 AU を ~/Library/Audio/Plug-Ins/Components へコピーし killall -9 AudioComponentRegistrar した上で `auval -v aufx Stt1 Manx`(失敗時はログ出力してジョブ失敗)
- **4.5** CI に render_test を追加: `-DSTUTTER_BUILD_TESTS=ON` でビルドし実行、非ゼロ終了でジョブ失敗(回帰ゲート)
- **4.4** README にユニバーサルビルド手順(`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`)を追記
- **4.6** README に「macOS専用(Windows/Linux未対応)」を明記
- **4.10** SPEC のプリセット記述を実装(コード生成方式)に合わせて修正
- **4.7** LICENSE 追加: JUCE 8 のOSSライセンス(AGPLv3)に従い AGPL-3.0 を配置し、READMEにライセンスセクション追記。※ユーザーが商用JUCEライセンスを持つ場合は変更可能な旨を最終報告で明示

## 対応しない(理由)
- **4.2/L8** ホストPrograms連携: ホスト毎の互換検討が必要な将来課題。独自ブラウザで機能は充足
- **4.3** stapler実装: 実証明書secretsが無い環境では検証不能。実装だけ進めるとテスト不能なコードになるためTODO維持が妥当
- **4.9** マルチパターン: SPECでv1対象外と明記済み
- **L4** CPU最適化: 計測に基づかない最適化はしない(現状pluginval/実機で問題報告なし)
- **L5** tailLength: テールは最大5msのクロスフェードのみで実害なし
- **L6** 倍精度: 業界的に一般的な省略。必要になれば対応
- **L7** .tmp整理: gitignore済みの作業領域であり問題なし
- **L9** カーブ描画の二重実装: 統合リファクタはリスク>益。現状表示ズレの実害なし

## 検証ゲート(全パッケージ完了後)
- render_test 全PASS、pluginval strictness 8 PASS
- sequencerOn OFF でステップエフェクトが無効になり、カーブのみ効くことをヘッドレスで確認
- internalBpm をAPVTSで変えるとフリーラン速度が変わることを確認
