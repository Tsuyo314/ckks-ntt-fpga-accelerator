# poly_mul_ntt_openfhe_n8192_soc — 処理の流れ

PCで多項式係数8192個x2本を入力すると、USB-UART経由でNexys A7上のMicroBlazeに送られ、
係数列は共有AXI BRAM (`axi_bram_ctrl_poly`) に書き込まれる。MicroBlazeがHLSアクセラレータ
(OpenFHE方式の負巡回NTT, N=8192) に計算させ、結果を同じ共有BRAM経由でPCに送り返す。

`poly_mul_ntt_openfhe_soc` (N=4) との最大の違いは、8192係数をAXI-Liteスカラーレジスタ
1個ずつでは扱えないため、**共有AXI BRAMバッファ経由のバルク転送方式**にした点。

---

## 全体構成

```
┌────────────────────┐          USB-UART           ┌────────────────────────────────────────────────────┐
│  PC (Windows)       │   115200bps, 8N1            │  Nexys A7-100T (xc7a100tcsg324-1)                   │
│                     │ <──────────────────────────>│                                                      │
│ host_bulk.py        │                              │  ┌────────────┐  AXI4  ┌─────────────┐             │
│  - 共有テストベクトル │                              │  │ MicroBlaze │<──────>│ AXI Uartlite│             │
│    読み込み・送受信   │                              │  │ (main.c)   │ Periph └─────────────┘             │
└────────────────────┘                              │  │            │                                      │
                                                     │  │            │  AXI4                                │
                                                     │  │            │<───────┐                             │
                                                     │  └────────────┘        │                             │
                                                     │        AXI4-Lite       ▼                             │
                                                     │  ┌──────────────┐  ┌──────────────────┐              │
                                                     │  │poly_mul_ntt_n8192│ │axi_bram_ctrl_poly│             │
                                                     │  │  (HLS IP)     │  │  (共有128KB BRAM) │             │
                                                     │  │ s_axi_control │  │  32bit x 32768語  │             │
                                                     │  │ m_axi_gmem ───┼─>│                   │             │
                                                     │  └──────────────┘  └──────────────────┘              │
                                                     └────────────────────────────────────────────────────────┘
```

- **PC側**: `host/host_bulk.py` — 共有テストベクトル(`fpga/lib/generated/testvectors_n8192_q60.json`)
  を読み込み、バルク送受信して結果を検証
- **MicroBlaze側**: `software/src/main.c` — UARTから係数列を受け取り共有BRAMへ書き込み、
  アクセラレータを駆動して結果を共有BRAMから読み出しUARTへ返す
- **アクセラレータ**: `high_level/src/poly_mul_ntt.cpp` — OpenFHE方式 (CT順変換/GS逆変換) の
  負巡回NTT (N=8192, mod Q (60ビット)) で多項式乗算を行うHLS IP。`m_axi_gmem` で
  共有BRAMを直接読み書きする
- Block Design (MicroBlaze + AXI Uartlite + 共有axi_bram_ctrl + 自作HLS IP) は
  `low_level/scripts/create_project.tcl` で構築

---

## 1リクエストあたりの処理の流れ

```
[PC]                                [MicroBlaze: main.c]                     [HLS IP: poly_mul_ntt_n8192]
  |                                        |                                            |
  | 1. テストベクトル読み込み (A,B各8192個)   |                                            |
  |                                        |                                            |
  | 2. リクエスト送信 (131073バイト)          |                                            |
  |    'C' + A(8Bx8192) + B(8Bx8192) ────> | 3. inbyte()で'C'が来るまで読み飛ばす (再同期) |
  |                                        | 4. 係数列をUARTから読みながら直接           |
  |                                        |    共有BRAM (axi_bram_ctrl_poly) へ書き込み |
  |                                        |    (32bit語2つに分割: 下位/上位)             |
  |                                        |                                            |
  |                                        | 5. XPoly_mul_ntt_n8192_Set_mem() で         |
  |                                        |    共有BRAMのベースアドレスを設定 (初回のみ)  |
  |                                        | 6. XPoly_mul_ntt_n8192_Start()  ──────────> | 7. m_axi_gmem 経由で共有BRAMから
  |                                        | 8. XPoly_mul_ntt_n8192_IsDone() をポーリング |    A,B係数をバースト読み込み
  |                                        |    <───────────────────────────────────────| 9. NTT計算 (正変換x2→点乗算→逆変換)
  |                                        |                                            |    → 結果を共有BRAMへバースト書き戻し
  |                                        |                                            | 10. 計算完了 (ap_done)
  |                                        |                                            |
  |                                        | 11. 共有BRAMから結果を読み出しながら         |
  |                                        |     UARTへバルク送信                        |
  |    'R' + 結果(8Bx8192) <────────────── |     (無限ループの先頭に戻り 3. へ)           |
  |                                        |                                            |
  | 12. 応答を受信・デコード                 |                                            |
  | 13. 共有テストベクトルの期待値            |                                            |
  |     (NTTベースリファレンス実装が計算済み)  |                                            |
  |     と突き合わせて [OK]/[MISMATCH] 表示  |                                            |
```

**所要時間の見積り**: 115200bpsでの転送は約11.5KB/s。リクエスト131KB+レスポンス65KBで
往復あたり約15〜20秒。NTT計算自体はHLSコアの内部で1〜数ms程度 (BRAMポート数の制約で
バタフライがII=2程度になったとしても数ms) で、UART転送が支配的なボトルネックになる
(N=4版は計算・通信ともμs〜ms オーダーだったのに対し、8192係数のバルク転送そのものが
支配項になる点が最大の違い)。

## 通信プロトコル

| 方向 | バイト列 | 意味 |
|------|---------|------|
| PC→FPGA | `'C'` + A係数x8192(各8バイトLE) + B係数x8192(各8バイトLE) = 131073バイト | 各係数はQ未満の値、リトルエンディアン64bit |
| FPGA→PC | `'R'` + 結果係数x8192(各8バイトLE) = 65537バイト | 同上 |

先頭の `'C'`/`'R'` はシンクバイト (N=4版から継承)。ズレが生じても次のシンクバイトまで
読み飛ばせば復帰できる。

## 関連ファイル

| ファイル | 役割 |
|---------|------|
| `high_level/src/poly_mul_ntt.cpp` | NTTアクセラレータのHLS実装 (本体) |
| `low_level/scripts/create_project.tcl` | Block Design構築 (共有BRAM含む) 〜ビットストリーム生成 |
| `software/src/main.c` | MicroBlaze側のバルクプロトコル処理 |
| `software/scripts/create_platform.py` / `create_app.py` | Vitisプラットフォーム・アプリ作成 |
| `host/host_bulk.py` | PC側のバルク送受信・検証スクリプト |
| `fpga/lib/ntt_params.py` / `ntt_reference.py` / `ntt_testvectors.py` | 共有パラメータ/リファレンス/テストベクトル生成ツール |
| `NTT_ALGORITHM.md` | NTT計算の数学的詳細 |
| `TROUBLESHOOTING.md` | 設計判断・落とし穴の記録 |
