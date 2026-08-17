# poly_ntt_inv_n8192_soc トラブルシューティング

`poly_mul_ntt_crt_n8192_soc` (CRT/RNS分解 + Shoup乗算バタフライ版 負巡回NTT, N=8192,
Lチャネル) を「順変換」「pointwise乗算」「逆変換」の3つの独立したビットストリームに
物理分割したうちの、フェーズ3 (逆変換) 単体プロジェクト。3分割全体の設計判断・
測定結果比較表は `poly_ntt_fwd_n8192_soc/TROUBLESHOOTING.md` 1節を参照
(以下は本プロジェクト固有の内容のみ)。

---

## 1. (解決済み) assign_bd_address のrangeは2のべき乗でなければならない

`low_level/scripts/create_project.tcl` の初版では、共有`axi_bram_ctrl_poly`の
実メモリサイズが192KB (Bブロックが不要な3ブロック構成: mem[0,N)=Ĉ/result,
[N,2N)=TAB(=RINV), [2N,3N)=TABSH(=RINV_SH)) であることに合わせて
`assign_bd_address -offset $off -range 192K $seg` としていたが、実行すると
以下のエラーで失敗した:

```
ERROR: [BD 41-1075] Cannot assign slave segment '/axi_bram_ctrl_poly/S_AXI/Mem0'
into address space '/microblaze_0/Data' at address '0xC000_0000 [ 192K ]'.
The proposed range '192K' must be a power of 2.
ERROR: [Common 17-39] 'assign_bd_address' failed due to earlier errors.
```

192K (=3×64K) は2のべき乗ではないため、AXIアドレスデコーダのアドレス空間として
割り当てられない。**対策**: アドレス空間だけ次のべき乗である256Kを割り当て、
実メモリ (`blk_mem_gen_poly`の`Write_Depth_A`) は192KB (49152語) のまま変更しない。
未使用の64KB分のアドレス空間は単に使われないだけで実害はない。

```tcl
assign_bd_address -offset $off -range 256K $seg
```

この問題は共有BRAMのサイズが2のべき乗ちょうど (順変換版256KB, pointwise版128KB)
にならない構成 (本プロジェクトの3ブロック=192KB) で初めて顕在化した。将来
同様に非2のべき乗サイズの共有BRAMを使う場合は同じ対策が必要。

## 2. 実測結果

| 指標 | 元の結合設計 | 本プロジェクト |
|---|---|---|
| Block RAM Tile (RAMB36) | 133/135 (98.5%) | **117/135 (86.7%)** |
| DSP48E1 | 110/240 (46%) | 36/240 (15%) |
| WNS | -0.030ns (未達) | **+0.784ns (達成)** |

HLSコア単体のcsynth.rptでは BRAM_18K 100/270 (37%, 元の結合コアの132/270より
Bブロックが不要な分減少)。3プロジェクト中もっともタイミング余裕が大きい結果になった。

## 3. csim検証

`high_level/scripts/run_csim.tcl` (`tb_poly_ntt_inv.cpp`) で検証。逆変換単体は
入力Ĉに「意味のある」NTTドメイン値を用意しないと検証にならないため、
`fpga/lib/ntt_reference.py` の `ntt_forward_ct(a)` (HLS実装とは独立したPython実装)
で合成入力 `a[i]=(i*2654435761+1) mod q` を順変換した結果を入力Ĉとし、
`ntt_inverse_gs(ntt_forward_ct(a)) == a` というround-trip性質で期待値 (=元のa) と
突き合わせる方式にした (`tb_data_inv.h`、実チャネル0パラメータ q/root_inv/
root_inv_shoup/n_inv/n_inv_shoupを埋め込み)。結果: `TEST PASSED: 8192 elements OK`。

## 4. Vivado実装結果

`/build poly_ntt_inv_n8192_soc` 相当のバッチ実行で、1節の修正後にDRCエラー0件・
ビットストリーム生成成功。`local_mem`は他プロジェクトと同じ16KB据え置き。
