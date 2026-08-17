# poly_ntt_pw_n8192_soc トラブルシューティング

`poly_mul_ntt_crt_n8192_soc` (CRT/RNS分解 + Shoup乗算バタフライ版 負巡回NTT, N=8192,
Lチャネル) を「順変換」「pointwise乗算」「逆変換」の3つの独立したビットストリームに
物理分割したうちの、フェーズ2 (pointwise乗算) 単体プロジェクト。3分割全体の設計判断・
測定結果比較表は `poly_ntt_fwd_n8192_soc/TROUBLESHOOTING.md` 1節を参照
(以下は本プロジェクト固有の内容のみ)。

---

## 1. (設計判断) NTTバタフライ・回転因子テーブルを一切持たない構成

pointwise乗算は、NTTドメインで既に順変換済みの2つの多項式 Â, B̂ を要素ごとに
Barrett還元で乗算するだけであり、インデックス並べ替え (バタフライの `j`/`j+t`
アドレッシング) が不要なため、`add_mod`/`sub_mod`/`mul_mod_shoup`もNTT用の
`TAB`/`TABSH`もN語ぶんの`static`オンチップ配列も一切不要である。単純な
パイプラインループ (`mem[i] = mul_mod_barrett(mem[i], mem[N+i], q, mu)`) のみで
構成した結果、3プロジェクト中もっとも小さいフットプリントになった
(下記2節参照)。

## 2. 実測結果 (3プロジェクト中もっとも劇的な効果)

| 指標 | 元の結合設計 | 本プロジェクト |
|---|---|---|
| Block RAM Tile (RAMB36) | 133/135 (98.5%) | **37/135 (27.4%)** |
| DSP48E1 | 110/240 (46%) | 64/240 (27%) |
| WNS | -0.030ns (未達) | **+0.162ns (達成)** |

HLSコア単体のcsynth.rptでも BRAM_18K 4/270 (1%) と、順変換版(132/270)・逆変換版
(100/270)に比べて圧倒的に小さい。「フェーズを分離するほど余裕が生まれる」という
3分割の狙いを最も明確に裏付けた結果。

## 3. csim検証

`high_level/scripts/run_csim.tcl` (`tb_poly_ntt_pw.cpp`) で検証。期待値はHLSの
Barrett還元実装とは独立した経路 (`unsigned __int128`によるネイティブなmod演算) で
計算しており、同じチャネル0の実q/mu (`fpga/lib/generated/
ntt_params_n8192_crt2_q60.json`) を使用。合成入力は
`a[i]=(i*2654435761+1) mod q`, `b[i]=(i*40503+7) mod q`。結果: `TEST PASSED: 8192
elements OK`。

## 4. Vivado実装結果

`/build poly_ntt_pw_n8192_soc` 相当のバッチ実行で、DRCエラー0件・ビットストリーム
生成成功。共有`axi_bram_ctrl_poly`は128KB (32bit幅x32768語、mem[0,N)=Â,
[N,2N)=B̂の2ブロック構成、順変換版の4ブロック=256KBの半分)。`local_mem`は他プロジェクト
と同じ16KB据え置き。
