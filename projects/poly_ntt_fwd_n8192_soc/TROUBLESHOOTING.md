# poly_ntt_fwd_n8192_soc トラブルシューティング

`poly_mul_ntt_crt_n8192_soc` (CRT/RNS分解 + Shoup乗算バタフライ版 負巡回NTT, N=8192,
Lチャネル) を「順変換」「pointwise乗算」「逆変換」の3つの独立したビットストリームに
物理分割したうちの、フェーズ1 (順変換) 単体プロジェクト。MicroBlaze SoC構成一般の
落とし穴 (ap_clk/ap_rst明示接続、Windows MAX_PATH制限など) や Barrett/Shoup還元自体の
設計判断は `poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md` を参照。ここでは3分割・
このプロジェクト固有の内容のみ記録する。

---

## 1. (設計判断) 3分割の狙いとフェーズ1固有の測定結果

`poly_mul_ntt_crt_n8192_soc` は「順変換+pointwise乗算」(phase=0) と「逆変換」(phase=1)
の2フェーズを1つのビットストリームに同居させており、チップ全体でBlock RAM
133/135 RAMB36 (98.5%)、タイミングもWNS=-0.030ns (36858エンドポイント中4件未達) という
ほぼ余裕のない状態だった。本プロジェクト以降の3プロジェクト
(poly_ntt_fwd_n8192_soc / poly_ntt_pw_n8192_soc / poly_ntt_inv_n8192_soc) は、
この3つの処理を物理的に別々のビットストリームに分割し、各ビットストリームがその
フェーズに必要なロジック・BRAMだけを持つようにしたもの。

**実測結果 (Vivado実装後、`design_1_wrapper_utilization_placed.rpt`/
`design_1_wrapper_timing_summary_routed.rpt` より):**

| プロジェクト | Block RAM Tile (RAMB36) | DSP48E1 | WNS |
|---|---|---|---|
| poly_mul_ntt_crt_n8192_soc (元の結合設計) | 133/135 (98.5%) | 110/240 (46%) | -0.030ns (未達) |
| **poly_ntt_fwd_n8192_soc (本プロジェクト)** | **133/135 (98.52%)** | 26/240 (11%) | **+0.466ns (達成)** |
| poly_ntt_pw_n8192_soc | 37/135 (27.4%) | 64/240 (27%) | +0.162ns (達成) |
| poly_ntt_inv_n8192_soc | 117/135 (86.7%) | 36/240 (15%) | +0.784ns (達成) |

**注意 (想定と異なった点)**: 本プロジェクトはBlock RAM使用率が元の結合設計と
ほぼ同じ (98.5%→98.52%、実質横ばい) だった。理由は、順変換に必要な
`A[N]`/`B[N]`/`TAB[N]`/`TABSH[N]` の4配列(256KB)は元の結合設計でも本プロジェクトでも
同一サイズであり、BRAM使用量を支配していたのはこの配列サイズ (+それと同サイズの
共有`axi_bram_ctrl_poly`との「二重バッファリング」) であって、pointwise乗算・逆変換
ロジック自体はBRAMをほとんど消費していなかったため。したがって「フェーズを外す」
だけではBRAM使用量は下がらない。実際に下がったのはタイミング (WNS -0.030ns →
+0.466ns) で、これは同居していたpointwise/逆変換の制御ロジック・配線輻輳が
取り除かれたことによるもの。BRAM自体を減らしたいのであれば、A/B/TAB/TABSHの
サイズそのもの (N=8192, 60ビット幅) を見直す必要がある。

## 2. Lチャネル数について — 今回の3分割では増えていない

この設計 (元の結合設計・3分割後のいずれも) では、モジュラス・回転因子テーブルは
合成時ROMではなくチャネルごとにホストが実行時に`mem`経由でストリーミングするため、
チャネル数Lはそもそもハードウェアリソースを消費しない (L=2でもL=8でも同じ
ビットストリームで処理できる。時間だけがLに比例して増える)。3分割で実際に得られた
のは「Lを増やせる余地」ではなく、各ビットストリームのBRAM/タイミング的な安全
マージンである。実際の検証は既存のL=2テストベクトル
(`fpga/lib/generated/testvectors_n8192_crt2_q60.json`) のまま行っており、L=4やL=8を
試すには`fpga/lib/ntt_params.py --count L`で新規パラメータ・テストベクトルを
生成する必要がある (未実施)。

## 3. csim検証

`high_level/scripts/run_csim.tcl` (`tb_poly_ntt_fwd.cpp`) で検証。ゴールデンデータ
(`tb_data_fwd.h`) は `fpga/lib/ntt_reference.py` の `ntt_forward_ct()` (HLS実装とは
独立したPython実装) で、実チャネル0パラメータ (`fpga/lib/generated/
ntt_params_n8192_crt2_q60.json`) と合成入力 `a[i]=(i*2654435761+1) mod q`,
`b[i]=(i*40503+7) mod q` から事前計算したもの。結果: `TEST PASSED: 16384 elements OK
(A_HAT + B_HAT)`。

## 4. HLSコア単体の合成結果

`csynth.rpt`: BRAM_18K 132/270 (48%, 元の結合コアの132/270と同じ。上記1節参照)、
DSP 26/240 (10%)、Fmax推定136.99MHz。

## 5. Vivado実装結果

`/build poly_ntt_fwd_n8192_soc` 相当のバッチ実行で、DRCエラー0件・ビットストリーム
生成成功。詳細は1節の表を参照。`local_mem`は16KB、共有`axi_bram_ctrl_poly`は256KB
(32bit幅x65536語、mem[0,N)=A, [N,2N)=B, [2N,3N)=TAB(=RFWD), [3N,4N)=TABSH(=RFWD_SH))。
