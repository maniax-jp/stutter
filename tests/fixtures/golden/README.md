# v1.1.2 ゴールデンベースライン

v2 開発における **DSP 回帰の検出基準**。実装着手前(commit `58cf80b` = v1.1.2)の
`render_test` 出力を固定したもの。

## なぜ必要か

WP1(エフェクト契約の刷新)と WP3(BlockSequencer)は、8つのエフェクトを
`stepLengthSamples` ベースの契約から `BlockContext` ベースへ移行する。この過程には
微妙な単位変換が含まれる。特に `src/dsp/effects/StutterEffect.h:59` の

```cpp
baseLoopLenSamples = stepLengthSamples * fraction * 4.0;
```

の `* 4.0` は「16分音符前提を打ち消すためだけに存在する係数」であり、移行中に善意で
「修正」されて全プリセットの音を変えてしまう典型例。こうした変化は音を聴かない限り
気づけず、気づいた時には原因の特定が困難になる。

**WP1 と WP3 はどちらも v1 とビット単位で一致しなければならない。** WP1 はパラメータの
出所を変えるだけで DSP を変えないし、WP3 も `beats=4, divisions=4, swing=0` で
1ステップ1ブロックなら v1 の固定16ステップグリッドを完全に再現するはずだから。
一致しなければそれは進捗ではなくバグ。

## 中身

| ファイル | 用途 |
|---|---|
| `v1-lane-renders.sha256` | 8レーン各々の WAV の SHA-256。ビット一致の判定に使う |
| `v1-lane-metrics.txt` | 不連続性メトリクスの数値。一致しなかった時に**どう**違うかを読むため |
| `verify-against-v1.sh` | 現在のビルドを回して上記と突き合わせる |

WAV 実体(18MB)はコミットしていない。判定はビット一致なのでチェックサムで十分であり、
実体が必要なら v1.1.2 のコミットから再生成できる。

## 使い方

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTUTTER_BUILD_TESTS=ON
cmake --build build --target render_test -j8
tests/fixtures/golden/verify-against-v1.sh
```

終了コード 0 = 全8レーンが v1.1.2 とビット一致。1 = 差異あり(どのレーンが違うかと
現在のメトリクスを表示する)。

## 決定性について

v1 の信号経路に RNG は存在せず、テスト信号も純粋な `sin()`。レンダリング条件
(48kHz / block 512 / 120 BPM / 4小節 / hostSync OFF)もすべて固定。実際に3回連続で
実行して 8/8 レーンがバイト単位で同一であることを確認済み。

**v2 でこの決定性は自明でなくなる。** Shuffler / Stretcher は乱数を使うため、
`BlockContext::seed` から導出したローカル RNG のみを使い、グローバル RNG は決して
使ってはならない(使うとレンダリング結果がブロックサイズ依存になる)。これは
WP11 の `DeterminismTests.cpp` で「同一 seed ならブロックサイズを変えてもビット一致」
として明示的にテストする。

## ベースラインを意図的に更新する場合

WP4 以降では DSP の変更が正当なものになりうる(新エフェクト追加、Send カテゴリの
導入など)。その場合のみ、変更が意図通りであることを音で確認した上で:

```sh
cd $(mktemp -d) && /path/to/render_test out
shasum -a 256 out/*.wav | sed 's|out/||' > \
    /path/to/repo/tests/fixtures/golden/v1-lane-renders.sha256
```

更新時は**何を変えたから音が変わったのか**をコミットメッセージに必ず書くこと。
理由の書かれていないベースライン更新は、回帰を隠蔽したのと区別がつかない。
