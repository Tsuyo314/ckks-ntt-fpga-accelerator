# poly_mul_ntt_crt_n8192_soc — NTT計算の詳細

`high_level/src/poly_mul_ntt.cpp` が実際に何を計算しているかを説明する。ベースの
CT順変換/GS逆変換・負巡回畳み込みの構成、パラメータ探索方法などは
`poly_mul_ntt_openfhe_n8192_soc/NTT_ALGORITHM.md` と共通なので、本ドキュメントでは
**CRT/RNS分解の導入とShoup乗算への変更に伴う差分**に絞って説明する。

---

## 1. 何を計算しているか

次数≤8191の多項式 A(x), B(x) (係数は mod Q_total) を、**環 Z_{Q_total}[x]/(x^8192+1) 上で
乗算**する。ただし `poly_mul_ntt_openfhe_n8192_soc` と異なり、単一の大きい素数Qではなく

```
Q_total = q0 * q1 * ... * q(L-1)   (各qiはNTT向き素数, qi = ki*2N+1)
```

という **L個の相異なるNTT向き素数の積** をモジュラスとして扱う (CRT/RNS分解)。

初期実装では **L=2** (デフォルト)。各 qi は既存のn8192版と同じ60ビット級で、
`fpga/lib/ntt_params.py --n 8192 --bits 60 --count 2` で生成する。

## 2. 全体の処理分担: どこまでがFPGA、どこからがPython

現時点の設計 (プロジェクト作成時にユーザーと合意した方針) では:

- **FPGA (本IP)**: ホストから毎回渡されるモジュラス`q`・回転因子テーブル・Shoup係数
  テーブルだけを使い、**単一モジュラスの負巡回NTT積を1回計算する**
  (`poly_mul_ntt_openfhe_n8192_soc`とほぼ同じ処理を、パラメータを実行時に受け取って
  実行するだけ。IP自身はチャネルという概念を持たない。詳細は3節参照)
- **Pythonホスト側 (`host/host_crt.py`)**:
  - 入力多項式 A, B (係数 < Q_total) を、チャネルごとに `ai = A mod qi`, `bi = B mod qi`
    へ分解する (**RNS分解**)
  - FPGAにチャネルごとのリクエストをL回 (時分割で) 送り、L個の残差多項式を受け取る
  - L個の残差から最終的な係数 (mod Q_total) を復元する (**CRT復元**、
    `fpga/lib/ntt_reference.py` の `crt_reconstruct` を使用)

つまり「RNS分解」と「CRT復元」自体はPython側の責務であり、FPGA (本IP) の役割は
「L個の独立したNTTベース負巡回積を、チャネルごとに切り替えながら計算する」ことに限定される。
将来的にCRT復元をハードウェア側に持たせる場合は、Q_total (現状120ビット級)
規模の広域整数演算が新たに必要になる。

## 3. チャネル選択とテーブル構成 (2フェーズ・完全実行時パラメータ化)

**[この節は実装過程で当初案から大きく変わった。詳細な経緯・実測値はTROUBLESHOOTING.md
5節参照]**

当初は `poly_mul_ntt_crt(coef_t *mem, int ch)` というトップ関数が、チャネル数`NTT_L`分の
`Q_TAB[NTT_L]`/`RFWD[NTT_L][NTT_N]`等を合成時ROMとして保持し、`ch`で実行時に行選択する
設計だった。しかしこれは「論理的な時分割」であって物理的にはLチャネル分のBRAMを同時に
確保してしまい、HLSコア単体でBRAM使用量が332/270ブロック(122%)となりxc7a100tに
収まらなかった。

最終的に採用した設計は、**モジュラス・回転因子テーブルを合成時ROMから完全に排除し、
呼び出しごとにホストが`mem`経由で実行時ロードする**方式。トップ関数は:

```c
void poly_mul_ntt_crt(coef_t *mem, coef_t q, coef_t mu, coef_t ninv, coef_t ninvsh, ap_uint<1> phase);
```

`ch`という概念自体がIPから消え、`q`(モジュラス)・`mu`(Barrett定数)・`ninv`/`ninvsh`
(N^-1とそのShoup定数) はすべてs_axiliteスカラーレジスタとしてホストが毎回送る。IPは
「今どのチャネルを計算しているか」を一切知らない、完全にチャネル非依存の汎用単一
モジュラスNTT積コアになった。

### テーブルの扱い: `nfwd`/`ninv`の境界を利用した1チャネル分のみの保持

回転因子テーブルは「`nfwd`(順変換)は`RFWD`/`RFWD_SH`しか使わない」「`ninv`(逆変換)は
`RINV`/`RINV_SH`しか使わない」という自然な境界があるため、両方を同時に持つ必要がない。
これを利用し、オンチップには`TAB`/`TABSH`という**1チャネル分だけの実行時書き換え可能
BRAM**を用意し、フェーズに応じて中身を差し替えて使い回す:

```c
static coef_t A[NTT_N], B[NTT_N];        // BIND_STORAGE RAM_2P
static coef_t TAB[NTT_N], TABSH[NTT_N];  // BIND_STORAGE RAM_1P (書込→読出が排他的なため)
```

### 2フェーズ実行

1回のポリ乗算リクエストを **2回の`Start()`呼び出し**に分割する:

- **phase=0** (順変換+点ごとの乗算): `mem`のレイアウト `[0,N)=A, [N,2N)=B,
  [2N,3N)=TAB(=RFWD), [3N,4N)=TABSH(=RFWD_SH)` を読み込み、`ntt_fwd(A)`,`ntt_fwd(B)`,
  点ごとの乗算(Barrett, `q`/`mu`使用) までを実行する。結果はA配列に**オンチップの
  まま留め、`mem`への書き戻しはしない**(`A`が`static`なので`Start()`をまたいでも
  BRAM内容が保持される性質を利用)。応答はACKのみ
- **phase=1** (逆変換): ホストが`mem`の同じ`[2N,4N)`領域をRINV/RINV_SHで**上書きして
  再送**し、それを`TAB`/`TABSH`へ再ロードしてから`ntt_inv(A)`(`ninv`/`ninvsh`使用)を
  実行し、結果を`mem[0,N)`へ書き戻す

MicroBlaze (`software/src/main.c`) はリクエストごとに`q`/`mu`/`ninv`/`ninvsh`と
テーブルデータをホストから受信して`Set_q()`等のレジスタに設定し、`phase`を切り替えながら
`Start()`を2回呼ぶ (時分割処理は「同じ論理回路をLチャネル分呼び直す」という発想は
当初案と同じだが、テーブルの物理的な保持を1チャネル分に切り詰めた点が異なる)。

共有`axi_bram_ctrl_poly`は128KB→256KBに拡張されている
(`low_level/scripts/create_project.tcl`参照)。

## 4. Barrett還元のビット幅設計 (点ごとの乗算)

`poly_mul_ntt_openfhe_n8192_soc/TROUBLESHOOTING.md` の3節と同じ設計。ただし `NTT_Q`/
`NTT_MU` の決め打ち定数ではなく、トップ関数の引数 `q`/`mu` (ホストが呼び出しごとに送る
s_axiliteスカラー、3節参照) を実行時に読んで使う点が異なる。Barrett還元のシフト量
`2*NTT_W_BITS` は全チャネル共通の値としてカーネル内に`#define`されている
(`ntt_params.py` が生成時に全チャネルのモジュラスが同じビット長であることをassertする。
NTT_W_BITSはチャネル非依存の定数なので、q/muと違い実行時パラメータ化していない)。

## 5. Shoup乗算 (バタフライの回転因子乗算・N^-1スケーリング)

`poly_mul_ntt_openfhe_n8192_soc` は順変換・逆変換バタフライの回転因子乗算、および逆変換
末尾のN^-1スケーリングも含めて **すべてBarrett還元** で行っていた。本プロジェクトでは
この3箇所を **Shoup乗算** に変更した (点ごとの乗算=A[i]*B[i] のみBarrettのまま)。

### 原理

回転因子 `w` やN^-1は、**実行時ではなく合成前に値が確定している固定定数**である。
Shoupの手法は、この性質を使って「固定定数側だけ」事前に高位ワードを計算しておくことで、
Barrett還元より軽い還元を実現する:

事前計算 (Python, `ntt_params.py` の `shoup_precompute`):
```
wsh = floor(w * 2^64 / q)   (q < 2^63 が前提。60ビット素数なので満たす)
```

実行時 (HLS, `mul_mod_shoup`):
```
qhat = high64(u * wsh)        // 商の見積り (u*wsh の上位64ビット)
r    = low64(u*w) - low64(qhat*q)   // 64ビット幅の折り返し演算
if (r >= q) r -= q;            // 補正は最大1回 (Barrettは最大2回)
```

Barrettとの違いは、`u*w` と `qhat*q` がいずれも「下位64ビットだけ」あれば良い
(ap_uint<64>への代入で自動的に折り返される) 点。Barrettは `u*v` (両方とも実行時変数) の
**フル120ビット積**を保持してから還元する必要があったのに対し、Shoupは片方の乗数が
固定なので、事前計算した `wsh` の恩恵でより軽い演算列になる。

### なぜ点ごとの乗算 (A[i]*B[i]) はShoupにできないか

Shoupの前提は「乗数の片方が既知の固定定数」であること。点ごとの乗算は両方とも
NTT変換後の**実行時の値**であり、どちらも合成前には確定しないため、Shoupの事前計算
(`wsh`) を作れない。したがってBarrett還元のまま。

## 6. (検証済み) BRAM使用量と最終リソース使用量

当初の「L=2チャネル分のテーブルをROMとして同時保持」設計ではHLSコア単体で
BRAM 332/270ブロック(122%)となり、xc7a100tに収まらなかった。3節の2フェーズ・
実行時パラメータ化により、HLSコア単体では **132/270ブロック(48%)** まで削減できた。

ただしこれでもBlock Design全体では「共有`axi_bram_ctrl_poly`(256KB)」と「HLSコア内蔵の
A/B/TAB/TABSH」の両方に同じデータを同時保持する二重バッファリングが生じ、実装
(`impl_1`)時点で一度チップ全体289/270ブロック(107%)相当の超過が発覚した。
`m_axi`バースト長の縮小(256→16, gmem用FIFOを20→4ブロックに削減)と
MicroBlaze `local_mem`の縮小(32KB→16KB)を追加で行い、最終的にBlock RAM Tile
(RAMB36換算) 133/135 (98.5%) に収めてビットストリーム生成まで完走した
(かなり際どいマージン。経緯・詳細な実測値はTROUBLESHOOTING.md 5〜8節参照)。

## 7. 実際に使用したパラメータ (N=8192, L=2)

`fpga/lib/ntt_params.py --n 8192 --bits 60 --count 2` で生成
(`fpga/lib/generated/ntt_params_n8192_crt2_q60.json`)。

| 項目 | チャネル0 | チャネル1 |
|------|-----------|-----------|
| 素数 q (60ビット) | 1152921504606830593 | 1152921504606748673 |
| Barrett定数 μ | 1152921504606863359 | 1152921504606945279 |
| N^-1 mod q | 1152780767118475267 | 1152780767118393357 |

```
Q_total = q0 * q1 = 1329227995784783648948129721038553089  (約120ビット)
```

回転因子テーブル (`root_fwd`/`root_fwd_shoup`/`root_inv`/`root_inv_shoup`, 各8192
エントリ) を含め、これらの値はすべてホスト側 (`host/host_crt.py`) が
このJSONから読み、リクエストごとにFPGAへ送信する。詳細はTROUBLESHOOTING.md 7節参照。

## 関連ファイル

| ファイル | 内容 |
|---------|------|
| `fpga/lib/ntt_params.py` | モジュラス探索 (`--count`でL個)・回転因子・Shoup係数生成 (共有) |
| `fpga/lib/ntt_reference.py` | NTTベースPythonリファレンス実装・CRT復元 (共有オラクル) |
| `fpga/lib/ntt_testvectors.py` | RNS版テストベクトル生成 (共有) |
| `high_level/src/poly_mul_ntt.cpp` | 本ドキュメントで解説したNTT実装そのもの |
| `fpga/lib/generated/ntt_params_n8192_crt2_q60.json` | 生成済みパラメータ (Lチャネル分、ホストが実行時に読む。カーネル自体はコンパイル時に依存しない。3節参照) |
| `FLOW.md` | UART通信〜MicroBlaze〜HLS IPの全体フロー (2フェーズプロトコルへの更新は未反映、TROUBLESHOOTING.md参照) |
| `TROUBLESHOOTING.md` | 設計判断の経緯・実測リソース使用量・実機検証結果 |
| `poly_mul_ntt_openfhe_n8192_soc/NTT_ALGORITHM.md` | ベースとなる単一モジュラス版の詳細 |
