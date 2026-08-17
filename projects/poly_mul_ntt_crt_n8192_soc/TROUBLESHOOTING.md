# poly_mul_ntt_crt_n8192_soc トラブルシューティング

`poly_mul_ntt_openfhe_n8192_soc` (単一モジュラス, Barrett還元のみ) をベースに、
CRT/RNS分解 (複数のNTT向き素数の積をモジュラスとして扱う) とShoup乗算 (バタフライの
回転因子乗算・N^-1スケーリング) を追加したプロジェクト。MicroBlaze SoC構成一般の落とし穴
(ap_clk/ap_rst明示接続、Vitis Unified Python APIの文字コードエラー、
`VITIS_EMBEDDED_INSTALL=true`、xsctの`"MicroBlaze #*"`フィルタ、Windows MAX_PATH(260文字)
制限など) や、Barrett還元のビット幅設計・II制約の落とし穴は
`poly_mul_ntt_openfhe_n8192_soc/TROUBLESHOOTING.md` を参照。ここではCRT/RNS化・Shoup化に
伴う新規の設計判断・未検証項目のみ記録する。

---

## 1. (設計判断) 処理分担: RNS分解・CRT復元はPython側、FPGA側は単一チャネルNTT積のみ

ユーザーとの計画合意時点で、以下の理由から「とりあえずPython側」とした:

- CRT復元 (Lチャネルの残差から Q_total = prod(qi) 未満の整数を復元する) は
  数百ビット級の広域整数演算が必要で、ハードウェア実装は別途相応の設計・検証コストがかかる
- 本プロジェクトの主眼はまず「Shoup乗算バタフライ」と「チャネル切り替え (時分割) の
  ハードウェア構成」を動かすことなので、スコープを分離した
- FPGA側は `poly_mul_ntt_openfhe_n8192_soc` とほぼ同じロジックにチャネル引数を足すだけで
  済むため、既存の動作実績 (タイミングクロージャ済み、リソース使用量も実測済み) を
  最大限再利用できる

将来的にCRT復元をハードウェア側に持たせる場合は新規の設計・検証が必要になる
(NTT_ALGORITHM.md 2節参照)。

## 2. (設計判断) チャネルは同一物理ハードウェアの時分割、レジスタ1本 (`ch`) で切り替え

Lチャネル分の回転因子テーブルを保持するため、`RFWD`/`RINV`/Shoup companion テーブルは
すべて `[NTT_L][NTT_N]` の2次元配列にし、`ch` (AXI-Liteスカラーレジスタ) で実行時に
行を選択する構成にした。素数ごとに別々のビットストリームを用意して都度JTAG経由で
再書き込みする案は、UART経由の対話プロトコルと相性が悪い (再プログラムのたびにJTAG接続・
Vivado起動が必要) ため採用しなかった。

## 3. (設計判断) Barrett還元のシフト量は全チャネル共通の `NTT_W_BITS` 1本に固定

Barrett還元 (点ごとの乗算用) のシフト量は `2 * (モジュラスのビット長)` である必要があり、
理論上はチャネルごとに異なりうる。しかし `ntt_params.py --count L` は同じ `--bits` 目標値
から降順にL個の素数を探すため、実際にはほぼ確実に全チャネルが同じビット長になる
(N=8192, bits=60, count=2 では実際に両チャネルとも60ビットだった)。

これを利用して、`write_c_header_multi()` は全チャネルのビット長が一致することを
Python側でassertし、`NTT_W_BITS`という単一の`#define`だけを出力する設計にした。
チャネルごとに実行時可変のシフト量 (バレルシフタが必要) にする案もあったが、
現実には発生しない状況のために追加のハードウェアコストを払う理由がないと判断した。
異なるビット長のモジュラスを混在させたくなった場合は、`ntt_params.py`の
`write_c_header_multi`を`W_BITS_TAB[NTT_L]`方式に変更する必要がある。

## 4. (Python単体で検証済み) Shoup乗算・CRT/RNS往復の正しさ

HLS合成前に、以下をPython (ビット精度シミュレーション) で検証済み:

- `fpga/lib/ntt_params.py` の `shoup_precompute` が生成する `wsh` を使い、
  `qhat = (u*wsh)>>64; r = (u*w - qhat*q) mod 2^64; if r>=q: r-=q` という
  HLS実装と同じ手順で `u*w mod q` を正しく計算できることを、ランダム値で検証
- `fpga/lib/ntt_reference.py` の `negacyclic_poly_mul_rns` / `crt_reconstruct` が、
  RNS分解→NTT畳み込み(各チャネル)→CRT復元の往復で、直接畳み込み
  (`negacyclic_conv_direct`, mod Q_total) と一致することを、小さいN・複数チャネル数の
  組み合わせで自己検証 (`ntt_reference.py`の`_self_test_rns()`)
- **N=8192・実際に生成したパラメータ (`ntt_params_n8192_crt2_q60.json`) を使い、
  `poly_mul_ntt.cpp`のC++ロジックと完全に同じ演算 (64ビット折り返し含む) をPythonで
  再現したシミュレーションが、テストベクトル (`testvectors_n8192_crt2_q60.json`) の
  期待値と全係数一致することを確認済み** (HLS合成前のロジック検証。実際のHLS csim/cosimは
  別途 `/vitis` で実施が必要)

## 5. (解決済み) BRAM使用量がHLSコア単体で122%となり、xc7a100tに収まらなかった問題

`/vitis poly_mul_ntt_crt_n8192_soc` で最初にHLS合成した実測値 (`poly_mul_ntt_crt_csynth.rpt`)。
DSP48E1・タイミングは問題なかったが、**BRAMがHLSコア単体だけで定員オーバー**であることが
確認された。

```
BRAM_18K: 332 / 270 available = 122%   (DSPは145/240=60%, Fmax推定136.99MHzで問題なし)
```

内訳:

| 項目 | BRAM_18K |
|------|----------|
| RFWD (nfwd, L=2 x N=8192 entries, 60bit幅 ← HLSが自動的にQのビット長まで詰めた) | 60 |
| RFWD_SH (nfwd, L=2 x N=8192 entries, 64bit幅) | 64 |
| RINV + RINV_SH (ninv, 同様の内訳) | 124 |
| A/B 作業用バッファ (8192語x64bit x2, RAM_2P) | 64 |
| gmem (m_axiバースト用FIFO) | 20 |
| **合計** | **332** |

`poly_mul_ntt_openfhe_n8192_soc` (単一モジュラス, Barrettのみ) はHLSコア単体でBRAM 53%
(144/270) だった。本プロジェクトはチャネル数L=2とShoup companionテーブル追加の掛け算で
回転因子テーブル本体のストレージが実測で約3.1倍に増え、結果としてHLSコア単体で
定員を超えた。DSPは60%と余裕があり (むしろ単一モジュラス版のBarrett実装76%より
少ない)、ボトルネックはBRAMに限られていた。

### 根本原因

`RFWD`/`RFWD_SH`/`RINV`/`RINV_SH` はいずれも `static const coef_t X[NTT_L][NTT_N]` と
宣言しており、**Lチャネル分のテーブルを合成時に全部ROMへ焼き込んで同時にBRAM上に
配置していた**。これは「チャネル切り替えは同じ論理回路が使うテーブル行を実行時に
選ぶだけ (`ch`でインデックス)」という設計だったため、物理的なBRAM容量は
Lチャネル分を同時に確保する必要があった。当初の意図 (「とりあえず時分割で処理」) から
すると、これは「論理的な時分割」であって「物理リソースの時分割」にはなっていなかった、
という見方ができる。

### 採用した対策: 2フェーズ構成への再設計

検討した4候補 (1.物理的な時分割へ変更 2.NTT_N縮小 3.L=1に戻す 4.60ビット化) のうち、
1番の方向性をさらに推し進め、**モジュラス・回転因子テーブルを合成時ROM (`Q_TAB`/`RFWD`等)
から完全に排除し、呼び出しごとにホストが`mem`経由で実行時ロードする**設計に変更した。
具体的には:

- トップ関数を `poly_mul_ntt_crt(coef_t *mem, int ch)` から
  `poly_mul_ntt_crt(coef_t *mem, coef_t q, coef_t mu, coef_t ninv, coef_t ninvsh, ap_uint<1> phase)`
  に変更。`q/mu/ninv/ninvsh`はs_axiliteスカラーレジスタとしてホストが毎回送る
  (`ch`という概念自体をIPから排除し、IPは完全にチャネル非依存の汎用単一モジュラス
  NTT積コアになった)
- `nfwd`/`ninv`が使う回転因子テーブルは、`RFWD`/`RINV`を`[NTT_L][NTT_N]`ROMとして
  持つのではなく、**`nfwd`は`RFWD`/`RFWD_SH`しか使わない、`ninv`は`RINV`/`RINV_SH`しか
  使わない**という自然な境界を利用し、`TAB`/`TABSH`という1チャネル分だけの実行時
  書き換え可能BRAM (`static coef_t TAB[NTT_N]`, `BIND_STORAGE type=RAM_1P`) を
  使い回す構成にした
- 1回のポリ乗算を **2回の`Start()`呼び出しに分割**:
  - **phase=0** (順変換+点ごとの乗算): `mem`からA,B,TAB(=RFWD),TABSH(=RFWD_SH)を
    ロードし、`ntt_fwd(A)`,`ntt_fwd(B)`,点ごとの乗算(Barrett)まで実行。結果はA配列に
    オンチップ(`static`のBRAM, `Start()`をまたいで内容保持)のまま留め、`mem`への
    書き戻しはしない。応答はACKのみ
  - **phase=1** (逆変換): ホストが`mem`の同じテーブル領域をRINV/RINV_SHで上書きして
    送り、それを`TAB`/`TABSH`に再ロードしてから`ntt_inv(A)`を実行し、結果を`mem`に
    書き戻す
- 共有`axi_bram_ctrl_poly`は128KB→256KBに拡張 (`mem`レイアウト:
  `[0,N)=A, [N,2N)=B, [2N,3N)=TAB, [3N,4N)=TABSH`)

再合成の結果、HLSコア単体のBRAMは **332→132ブロック (48%)** まで削減できた
(TAB/TABSHをRAM_1Pにしても容量そのものは変わらなかった。BRAM primitiveは元々
デュアルポート構造なのでポート数を減らしても物理ブロック数は減らないため。
削減効果があったのは「Lチャネル分→1チャネル分」への変更そのもの)。

### 想定外だった第二の壁: 「二重バッファリング」問題

2フェーズ化してHLSコア単体は48%まで下がったが、実際にVivadoで論理合成・実装まで進めると
`place_design`のDRCで **`RAMB36E1: 144個要求 / 135個しか利用可能でない`** というエラーで
失敗した (チップ全体で289/270ブロック=107%相当)。

原因は、HLSコア単体のcsynth.rpt (48%) には**共有`axi_bram_ctrl_poly` (256KBに拡張済み)
分のBRAMが含まれていない**ため。1回のリクエストで A・B・TAB・TABSH の4ブロック
(256KB全部) を一度に`mem`へ書き込んでから`Start()`する設計だったので、同じデータを
「共有BRAM上」と「HLSコア内蔵BRAM上」の両方に同時に確保する必要があり、チップ全体で
見ると二重に確保していたことになる。テーブルサイズを1チャネル分に減らしても、
「共有バッファとコア内蔵BRAMの二重確保」という別の形で無駄が残っていた。

### 追加で実施した削減策

1. **`m_axi`バースト長を256→16に縮小**: gmem用AXIバーストFIFOのBRAM使用量が
   20→4ブロックに削減 (burst=8まで試したが4ブロックで底打ちしたため16で確定。
   UART転送(115200bps)が支配的ボトルネックの設計なので、バースト長縮小による
   実効性能への影響は無視できる)
2. **MicroBlaze `local_mem`を32KB→16KB に縮小**: バルクデータは共有BRAM側にあるため
   MicroBlaze自身のローカルメモリはコード/スタック用途のみで足りる

最終的にVivado実装が成功し、チップ全体のBlock RAM使用率は **133/135 RAMB36
(98.5%)** に収まった (`design_1_wrapper_utilization_placed.rpt`実測値。BRAM18換算で
266/270ブロック相当、空きはわずか2 RAMB36=4 BRAM18ブロックのみという際どいマージン)。

## 6. (検証済み) HLS合成・Vivado合成・実機での検証結果

- **HLS合成結果 (csynth.rpt)**: BRAM 132/270 (48%), DSP 110/240 (45%)。`%`による
  汎用除算器は生成されていない (Barrett還元・Shoup乗算のみで構成)。バタフライループの
  IIはHLSが選んだ値でタイミングクロージャ (Fmax推定136.99MHz、100MHz目標に対し
  マージンあり)
- **Block Design全体でのBRAM36/DSP48実利用率**: 実装(`impl_1`)完了、ビットストリーム
  生成成功、DRCエラー0件。Block RAM Tile (RAMB36換算) 133/135 (98.5%)、DSP 110/240
  (46%)、Slice LUT 10066/63400 (16%)、Slice Register 12521/126800 (10%)。
  最終ルーティング後のタイミングは **WNS=-0.030ns, TNS=-0.120ns (36858エンドポイント中
  4件が未達)** で、`report_timing_summary`上は「タイミング制約未達」の判定。ただし
  UART(115200bps)がボトルネックの設計であり、内部ロジックの30ピコ秒の未達は実用上
  無視できるレベルと判断し、実機検証に進んだ (9節で実際に4/4ベクトルOKを確認済み)
- **実機での実測UART転送時間**: `host/host_crt.py`で1チャネルあたり約39.8秒、
  L=2で1ベクトルあたり約79.6秒 (FLOW.mdの見積り30〜40秒/チャネルとおおむね一致)
- **`axi_bram_ctrl_poly`と`poly_mul_ntt_crt_0/m_axi_gmem`を同一スレーブへ接続する際の
  実際の構成**: Vivadoは新規のSmartConnectを追加挿入せず、**既存の
  `microblaze_0_axi_periph` (種別`xilinx.com:ip:axi_interconnect:2.1`、クラシック
  AXI Interconnect) を拡張し、MicroBlazeのペリフェラルバスとHLSコアの`m_axi_gmem`の
  両方を`axi_bram_ctrl_poly`の単一スレーブポートへ調停する**構成になっていた
  (`get_bd_cells`でVLNVを確認して判明。プロトコル変換・幅変換IPは追加されていない)

## 7. 使用したパラメータ (N=8192, L=2)

`fpga/lib/ntt_params.py --n 8192 --bits 60 --count 2` で生成
(`fpga/lib/generated/ntt_params_n8192_crt2_q60.json`)。

| 項目 | チャネル0 | チャネル1 |
|------|-----------|-----------|
| 素数 q (60ビット, `q=k*2N+1`) | 1152921504606830593 (k=70368744177663) | 1152921504606748673 (k=70368744177658) |
| 原始根 g | 10 | 3 |
| 原始2N乗根 ψ | 330791804103690911 | 609248293264176271 |
| Barrett定数 μ (=⌊2^120/q⌋) | 1152921504606863359 | 1152921504606945279 |
| N^-1 mod q | 1152780767118475267 | 1152780767118393357 |
| N^-1のShoup定数 | 18444492273895866368 | 18444492273895866368 |

```
Q_total = q0 * q1 = 1329227995784783648948129721038553089  (約120ビット)
```

回転因子テーブル (`root_fwd`/`root_fwd_shoup`/`root_inv`/`root_inv_shoup`, 各8192エントリ)
は現在ホスト側 (`host/host_crt.py`) がこのJSONから読み、リクエストごとにFPGAへ送信する
(6節までの通り、HLSカーネル自体はこれらの値をコンパイル時に一切保持しない)。

## 8. 最終リソース使用量サマリ (xc7a100t, `xc7a100tcsg324-1`)

| リソース | HLSコア単体 (`poly_mul_ntt_crt`) | Block Design全体 (実装後実測) |
|---------|-----------------------------------|-------------------------------|
| Block RAM (BRAM18換算) | 132/270 (48%) | 266/270相当 (RAMB36換算 133/135 = 98.5%) |
| DSP48E1 | 110/240 (45%) | 110/240 (46%) |
| Slice LUT | 9316/63400 (14%, csynth推定) | 10066/63400 (16%) |
| FF (Slice Register) | 9553/63400相当 (7%, csynth推定) | 12521/126800 (10%) |
| タイミング (100MHz/10ns制約) | Fmax推定136.99MHz | WNS=-0.030ns (36858中4エンドポイント未達、実用上無視できるレベル) |

参考: `poly_mul_ntt_openfhe_n8192_soc` (単一モジュラス版) はHLSコア単体でBRAM 53%
(144/270), DSP 76% (184/240) だった。RNS/CRT化 (L=2, Shoup乗算追加) によりBRAMは
ほぼ同水準、DSPはむしろ減少 (Shoup乗算がBarrettより軽い演算のため) という結果になった。

## 9. 実機検証結果

`/program` → `download_and_run.tcl` → `host/host_crt.py COM<N> --count 4` で
`testvectors_n8192_crt2_q60.json`収録の全4ベクトルを実行:

```
[0] OK (total=79.57s, ch0=39.78s, ch1=39.79s)
[1] OK (total=79.58s, ch0=39.79s, ch1=39.79s)
[2] OK (total=79.59s, ch0=39.79s, ch1=39.80s)
[3] OK (total=79.58s, ch0=39.79s, ch1=39.79s)
Summary: 4/4 OK
```

CRT復元後の係数がすべて期待値と一致し、5節のタイミング未達 (WNS=-0.030ns) が実用上
問題にならないことも実機で確認できた。
