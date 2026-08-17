# poly_mul_ntt_openfhe_n8192_soc トラブルシューティング

`poly_mul_ntt_openfhe_soc` (N=4のトイ実装) を N=8192・実用規模モジュラス(60ビット)へ
スケールアップしたプロジェクト。単純なパラメータ変更ではなく、以下の点でアーキテクチャ
自体を作り直している。MicroBlaze SoC構成一般の落とし穴 (ap_clk/ap_rst明示接続、
Vitis Unified Python APIの文字コードエラー、`VITIS_EMBEDDED_INSTALL=true`、xsctの
`"MicroBlaze #*"`フィルタ、Windows MAX_PATH(260文字)制限など) は
`poly_mul_ntt_soc`/`poly_mul_fntt_soc`/`poly_mul_ntt_openfhe_soc` の各TROUBLESHOOTING.mdを
参照。ここでは N=8192化に伴う新規の設計判断のみ記録する。

---

## 1. (設計判断) N=4トイ実装からのアーキテクチャ変更点

| 項目 | N=4版 | N=8192版 | 理由 |
|------|-------|----------|------|
| モジュラスQ | 257 (8ビット) | 1152921504606830593 (60ビット) | 実用のOpenFHE規模に近づけるため |
| mod演算 | `%` 演算子 | Barrett還元 | 60ビットで`%`は汎用除算器を合成してしまう |
| 回転因子テーブル | `ARRAY_PARTITION complete`で配線化 (0 BRAM) | `BIND_STORAGE impl=BRAM` で明示的にBRAM化 | 8192エントリは配線化するとLUT/FFが破綻。添字が実行時変数なのでどのみち配線化できない |
| バタフライ | 2ステージ手展開 | 13ステージのループ (シフト/マスクで添字計算、割り算回避) | 手展開は N=8192 では現実的でない |
| トップ関数IF | AXI-Liteスカラー register x12 (a0..b3, r0..r3) | `m_axi` メモリポインタ1本 + 制御レジスタ | 8192係数をレジスタ単位ではやり取りできない |
| 通信プロトコル | 9バイト固定パケット | バルク転送 (131073B request / 65537B response) | 同上 |
| 共有BRAM | なし | `axi_bram_ctrl_poly` (MicroBlaze/HLSコア共有, 128KB) | UART充填とHLSコアの計算を仲介するバッファが必要 |
| MicroBlaze local_mem | 128KB | 32KB | バルクデータは共有BRAM側に移るため縮小 |

## 2. (設計判断) パラメータ・テストベクトルを使い捨てにせず共有ライブラリ化

N=4版は `/tmp/verify_openfhe_ntt_n4.py` という使い捨てスクリプトで回転因子や期待値を
検証していた (リポジトリに残っていない)。N=8192版では、ユーザーの要望
(他プロジェクトでも同じパラメータ・テストベクトルを使い回したい) に基づき、
`fpga/lib/ntt_params.py` (パラメータ生成), `fpga/lib/ntt_reference.py` (NTTベース
Pythonリファレンス実装), `fpga/lib/ntt_testvectors.py` (テストベクトル生成) を
チェックインされた共有ツールとして整備した。生成物 (`fpga/lib/generated/*.json`) も
リポジトリに含める。

決定的Miller-Rabin (底`[2,3,5,7,11,13,17,19,23,29,31,37]`, n<3.3e24で確定的に正しい)を
使い、sympy等の追加依存なしで完結させた。

## 3. (設計判断) Barrett還元のビット幅設計

Barrett還元の実装で見落としやすい点: 60ビットのu,vの積xは最大120ビットになるが、
Barrett定数μは61ビットあるため、`x * μ` は最大184ビットになる (120+61ビットではなく
120+64ビットの`ap_uint<192>`で確保する必要がある。狭く見積もると桁あふれする)。
実装 (`poly_mul_ntt.cpp`) では以下の型で桁数を確保した:

```
coef_t  = ap_uint<64>   (Q未満の値。60ビットQに対し64ビットコンテナ)
dcoef_t = ap_uint<128>  (u*v, 最大120ビット)
x*μ     = ap_uint<192>  (dcoef_t(128) * coef_t(64) = 最大192ビット)
```

`qhat*Q`の掛け算も、qhatをdcoef_t(128bit)ではなくcoef_t(64bit)に収めてから
`coef_t*coef_t`として計算する (128x128の掛け算になるのを避け、64x64に抑えることで
DSP使用量を削減する)。

## 4. (HLS合成で発見・解決済み) II=1強制がBarrett乗算器のタイミング崩壊を引き起こした

### 症状

`ntt_fwd_ct`/`ntt_inv_gs`のバタフライループに (N=4版の作法を踏襲して) 素朴に
`#pragma HLS PIPELINE II=1` を付けて最初にHLS合成したところ:

```
INFO: [HLS 200-790] **** Loop Constraint Status: All loop constraints were NOT satisfied.
INFO: [HLS 200-789] **** Estimated Fmax: 13.21 MHz
```

100MHz(10ns)目標に対し推定Fmaxが13.21MHzしかなく、致命的にタイミングが崩壊していた。

### 原因

Barrett還元は124x62ビット→184ビットの乗算 (`x*μ`) を含む重い演算。
`STAGE_FWD_BUTTERFLY_FWD`/`STAGE_INV_BUTTERFLY_INV` ループは `a[j]`/`a[j+t]` への
読み書きに真のメモリ依存があり (段によっては連続するk反復で同じアドレスに触れるため)
II=1を強制すると、その依存を満たすために乗算器を1〜数サイクルの組合せ回路的な
実装に押し込めてしまい (csynth.rpt の Bind Op Report で `mul_124ns_62ns_184_1_1`
のように**1サイクルレイテンシ**の乗算器が選ばれていたことで確認)、結果として
1段あたりの組合せ遅延が10nsを大幅に超えた。対照的に、IIを制約しなかった
`POINTWISE`/`SCALE_INV`ループでは同じ乗算がきちんと5サイクルパイプライン
(`mul_128ns_62ns_184_5_1`) で実装され、Slack 7.30と健全だった。

### 対策

バタフライループの `PIPELINE II=1` を単なる `PIPELINE` (II指定なし) に変更し、
HLSに達成可能なIIを選ばせるようにした。UART往復転送 (15〜20秒見込み) が
支配的ボトルネックなので、多少IIが大きくなっても計算時間への影響は無視できる、
という判断 (計算全体は後述の通りμs〜msオーダーで収まる)。

### 結果 (修正後の実測)

```
INFO: [HLS 200-789] **** Estimated Fmax: 136.99 MHz   (100MHz目標に対し margin あり)
```

- `STAGE_FWD_BUTTERFLY_FWD`/`STAGE_INV_BUTTERFLY_INV`: 達成II=23 (メモリ依存起因、
  タイミング上は健全)。全体レイテンシは 3,706,978 サイクル ≈ 37ms (@100MHz) —
  UART往復15〜20秒に対し完全に無視できる
- 回転因子テーブルはBRAMとして実装されていることを確認 (`NTT_ROOT_FWD`/`NTT_ROOT_INV`
  それぞれ `ROM_1P`, 30 BRAM18ブロック使用。N=4版の「配線化」とは逆の結果で意図通り)
- `%`による汎用除算器は出ていない (Barrett還元・条件付き減算のみで構成されている)

## 5. HLS合成結果のリソース使用量 (HLSコア単体, `csynth.rpt`)

| リソース | 使用量 | 割合 |
|---------|-------|------|
| BRAM (18Kb換算) | 144 | 53% (xc7a100t全270ブロック中) |
| DSP48E1 | 184 | 76% (xc7a100t全240個中) |
| FF | 10272 | 8% |
| LUT | 10317 | 16% |

**計画時点の見積りとの差分**: 事前見積りではDSP使用量を30-60個程度と見込んでいたが、
実際は184個 (76%) と大幅に上回った。Barrett還元が複数の異なるビット幅の乗算
(u*v, x*μ, qhat*Q) を含み、しかも forward NTT用・inverse NTT用・pointwise乗算用に
それぞれ個別の乗算器インスタンスが生成されるため (関数呼び出しがそれぞれ異なる
ループコンテキストにインライン展開される)、見積りより多くの乗算器が実体化された
と考えられる。それでも240個中184個で収まっており、致命的ではない。
BRAM使用量は逆に見積り(72%)より少なく53%で収まった。

ただしこれはHLSコア単体の数値であり、Block Design全体 (共有axi_bram_ctrl_poly
128KB≈29ブロック、MicroBlaze local_mem 32KB≈8ブロック等) を含めた最終的な
BRAM/DSP利用率は `/build` (Vivado合成) 後に別途確認する必要がある (下記6節)。

## 6. (未実施) Vivado合成・実機での検証待ちの項目

- Block Design全体でのBRAM36/DSP48実利用率 (HLSコア単体76%DSPに対し、
  MicroBlaze等を含めた全体でも240個の範囲に収まるか)
- 既存の10ns(100MHz)制約でBlock Design全体がタイミングクロージャするか
  (HLSコア単体は136.99MHz相当のマージンがあるが、AXI Interconnect/SmartConnect
  等を含めた実装後タイミングは別途確認が必要)
- 実機での実測UART転送時間
- `axi_bram_ctrl_poly`と`poly_mul_ntt_n8192_0/m_axi_gmem`を同一スレーブへ接続する際に
  Vivadoが自動挿入するSmartConnect/AXI Interconnectの実際の構成 (`create_project.tcl`
  内のコメント参照。`apply_bd_automation`の`-config`辞書キー名は実行してみないと
  確定しない部分がある)
