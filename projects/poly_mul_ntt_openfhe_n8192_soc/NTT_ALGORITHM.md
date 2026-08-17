# poly_mul_ntt_openfhe_n8192_soc — NTT計算の詳細

`high_level/src/poly_mul_ntt.cpp` が実際に何を計算しているかを説明する。
アルゴリズムの数学的な構成 (CT順変換/GS逆変換 + 負巡回) は
`poly_mul_ntt_openfhe_soc/NTT_ALGORITHM.md` (N=4のトイ版) と全く同じで、
本ドキュメントでは **N=8192へのスケールアップに伴うパラメータとハードウェア実装の違い**
に絞って説明する。全体のUART通信フローは `FLOW.md` を、設計判断の経緯は
`TROUBLESHOOTING.md` を参照。

---

## 1. 何を計算しているか

次数≤8191の多項式 A(x), B(x) (係数は mod Q) を、**環 Z_Q[x]/(x^8192+1) 上で乗算**する。
N=4版と数学的に同じ「負巡回 (negacyclic) 畳み込み」を、N=8192・実用規模のモジュラスQで
行う。

---

## 2. パラメータ

`fpga/lib/ntt_params.py --n 8192 --bits 60` で生成 (再現・再検証可能。値を
決め打ちで信用せず、実装時に実際にスクリプトを走らせて確認したもの):

| 記号 | 値 |
|------|-----|
| `N` | 8192 |
| `cycloOrder` (=2N) | 16384 |
| `Q` | 1152921504606830593 (60ビット素数, `Q = k*2N+1`, `k = 70368744177663`) |
| 原始根 `g` | 10 |
| `ψ` (psi, 原始16384乗根) | 330791804103690911 |
| `ψ⁻¹` | 443664889705400686 |
| `N⁻¹` | 1152780767118475267 |
| Barrett定数 `μ` (`= floor(2^120/Q)`) | 1152921504606863359 |

検証済み: `ψ^16384 ≡ 1 (mod Q)`, `ψ^8192 ≡ -1 (mod Q)`, `ψ·ψ⁻¹ ≡ 1`, `N·N⁻¹ ≡ 1`。
生成物は `fpga/lib/generated/ntt_params_n8192_q60.json` (JSON, 全回転因子含む) と
`high_level/src/ntt_params_n8192.h` (HLS用Cヘッダ、生成JSONと同じ内容をコピー配置)。

N=4版の `P=257` (8ビットのフェルマー素数) と違い、Qは実際のOpenFHE
(CKKS/BFV/BGV, N=8192) で使われる規模に近い60ビットのNTT向き素数である。

---

## 3. 回転因子テーブルの構成

規約はN=4版と同一 (`Table[bitrev(i)] = ψ^i`, OpenFHEの
`ChineseRemainderTransformFTTNat::PreCompute` と同じ)。N=8192なので13ビット幅の
ビット反転を使い、各8192エントリの `NTT_ROOT_FWD`/`NTT_ROOT_INV` テーブルを
`fpga/lib/ntt_params.py` の `build_twiddle_tables()` で機械的に生成する
(N=4版のように手計算・手記述はしない)。

**ハードウェア表現の違い**: N=4版は `ARRAY_PARTITION complete` によりテーブルを
配線 (即値) に変換し、意図的にBRAM/ROMを使わせなかった
(`poly_mul_ntt_openfhe_soc/TROUBLESHOOTING.md` 項目2参照、この手法は非スケーラブルと
明記されていた)。N=8192では8192エントリを配線化するとLUT/FFが破綻するため、
逆に `#pragma HLS BIND_STORAGE ... impl=BRAM` で明示的にBRAM格納にする。
バタフライループの添字 (`m+i` や `h+i`) は実行時変数なので、HLSはどのみち
配線化できず、この変更は必然でもある。

---

## 4. 順変換 (Forward NTT) — Cooley-Tukey バタフライ (ループ実装)

N=4版の `ntt4_fwd_ct()` は2ステージを手展開していたが、N=8192では13ステージを
手展開できないため、`ntt_fwd_ct()` はステージ×バタフライの2重ループになっている
(`poly_mul_ntt.cpp`)。バタフライの中身 (「先に回転因子を掛けてから加減算」という
CTの特徴) は同一:

```
hi = mul_mod(a[j+t], w)   (先に乗算)
lo = a[j]
a[j]   = add_mod(lo, hi)
a[j+t] = sub_mod(lo, hi)
```

**割り算を避けるための添字計算**: 段ごとのバタフライ間距離 `t` は常に2のべき乗
(`t = 2^(LOG2N-1-stage)`) なので、素朴に `i = k / t, j_local = k % t` と書くと
HLSが除算器を合成してしまう。実際にはシフトとマスクだけで計算する
(`i = k >> tlog`, `j_local = k & mask`, `tlog = log2(t)`)。この添字変換が
N=4の手展開バタフライと数学的に一致することは、Pythonで両方の実装を突き合わせて
検証済み (N=4,8,16,32の全パターン、およびN=8192の乱数テストベクトルで確認)。

---

## 5. 点ごとの乗算 (Pointwise Multiplication)

```
C[i] = A[i] * B[i] mod Q   (i = 0..8191)
```

N=4版と同じ `mul_mod` を再利用する。中身がBarrett還元に変わった点のみ後述6節参照。

---

## 6. 逆変換 (Inverse NTT) — Gentleman-Sande バタフライ (ループ実装)

`ntt_inv_gs()`。GSバタフライの特徴 (「先に加減算してから回転因子を掛ける」) も
N=4版と同一。最後に全要素へ `N⁻¹` を掛けるスケーリングも同様。

### mod演算の実装変更: `%` → Barrett還元

N=4版の `add_mod`/`sub_mod`/`mul_mod` は `P=257` が8ビット定数だったからこそ
`%` 演算子が安価に合成できた。Q=60ビットで同じ書き方をすると、HLSは
**汎用除算器** を合成してしまい、LUT/タイミングの両面で破綻する。そのため:

- `add_mod`/`sub_mod`: 演算結果は常に `[0, 2Q)` に収まるため、条件付き1回の減算
  (`if (t >= Q) t -= Q;`) で十分。除算・剰余は使わない。
- `mul_mod`: 2つの60ビット値の積 (最大120ビット) を、事前計算したBarrett定数
  `μ = floor(2^120/Q)` を使って還元する。
  ```c
  qhat = (x * μ) >> 120;      // 商の見積り (最大1小さい場合がある)
  r    = x - qhat * Q;
  if (r >= Q) r -= Q;          // Barrett還元特有の補正 (最大2回)
  if (r >= Q) r -= Q;
  ```

---

## 7. 検証方法 (N=4版とは異なりHLS内で手計算・手検算はしない)

N=4版は具体的な数値例を全ステージ手計算して`NTT_ALGORITHM.md`に記載していたが、
N=8192・60ビットではそれは非現実的。代わりに:

1. `fpga/lib/ntt_reference.py` が同じCT-forward/GS-inverseアルゴリズムを
   Pythonで実装しており (このHLSコードと1対1に対応する構造)、`negacyclic_conv_direct`
   (O(N²)直接畳み込み) との突き合わせを小さいN (4,8,16) で自己検証済み。
2. N=8192自体でも、生成した1テストベクトルに対しO(N²)直接畳み込み
   (Pythonで約8秒) を1回限り実行し、NTTベース参照実装の結果と完全一致することを
   確認済み。
3. `fpga/lib/ntt_testvectors.py` で生成したシード付きテストベクトル
   (`fpga/lib/generated/testvectors_n8192_q60.json`) を実機テストのオラクルとして使う
   (`host/host_bulk.py`)。

---

## 関連ファイル

| ファイル | 内容 |
|---------|------|
| `fpga/lib/ntt_params.py` | モジュラス探索・原始根・回転因子テーブル生成 (共有) |
| `fpga/lib/ntt_reference.py` | NTTベースPythonリファレンス実装 (共有オラクル) |
| `fpga/lib/ntt_testvectors.py` | テストベクトル生成 (共有) |
| `high_level/src/poly_mul_ntt.cpp` | 本ドキュメントで解説したNTT実装そのもの |
| `high_level/src/ntt_params_n8192.h` | 生成済みパラメータ (Q, μ, 回転因子テーブル等) |
| `FLOW.md` | UART通信〜MicroBlaze〜HLS IP のシステム全体の処理フロー |
| `TROUBLESHOOTING.md` | 設計判断の経緯 |
