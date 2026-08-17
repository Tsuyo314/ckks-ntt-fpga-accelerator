# poly_mul_ntt_crt_n8192_soc — 処理の流れ

`poly_mul_ntt_openfhe_n8192_soc/FLOW.md` と全体構成 (PC ⇔ MicroBlaze ⇔ 共有AXI BRAM ⇔
HLSアクセラレータ) は同じ。差分は **チャネル番号 (1バイト) がプロトコルに追加**されている
点と、**1回の「多項式乗算」につきPC↔FPGAのやり取りがLチャネル分 (時分割) 発生する**点。

---

## 全体構成

```
┌────────────────────┐          USB-UART           ┌────────────────────────────────────────────────────┐
│  PC (Windows)       │   115200bps, 8N1            │  Nexys A7-100T (xc7a100tcsg324-1)                   │
│                     │ <──────────────────────────>│                                                      │
│ host_crt.py         │                              │  ┌────────────┐  AXI4  ┌─────────────┐             │
│  - RNS分解 (A mod qi) │                              │  │ MicroBlaze │<──────>│ AXI Uartlite│             │
│  - Lチャネル分送受信   │                              │  │ (main.c)   │ Periph └─────────────┘             │
│  - CRT復元 (最終結果)  │                              │  │            │                                      │
└────────────────────┘                              │  │            │  AXI4                                │
                                                     │  │            │<───────┐                             │
                                                     │  └────────────┘        │                             │
                                                     │        AXI4-Lite       ▼                             │
                                                     │  ┌──────────────┐  ┌──────────────────┐              │
                                                     │  │poly_mul_ntt_crt│ │axi_bram_ctrl_poly│             │
                                                     │  │  (HLS IP)     │ │  (共有128KB BRAM) │             │
                                                     │  │ s_axi_CTRL    │ │  32bit x 32768語  │             │
                                                     │  │ (mem, ch)     │ │                   │             │
                                                     │  │ m_axi_gmem ───┼─>│                   │             │
                                                     │  └──────────────┘  └──────────────────┘              │
                                                     └────────────────────────────────────────────────────────┘
```

- **PC側**: `host/host_crt.py` — チャネルごとのモジュラスで入力を分解し、Lチャネル分の
  リクエストを送受信、最後にCRT復元。共有テストベクトルは
  `fpga/lib/generated/testvectors_n8192_crt2_q60.json`
- **MicroBlaze側**: `software/src/main.c` — UARTからチャネル番号+係数列を受け取り
  共有BRAMへ書き込み、`Set_ch()`でチャネルを選んでからアクセラレータを駆動、結果を
  チャネル番号付きでUARTへ返す
- **アクセラレータ**: `high_level/src/poly_mul_ntt.cpp` — CRT/RNS対応 + Shoup乗算バタフライ
  版の負巡回NTT (N=8192, チャネルごとに異なるモジュラスqi) で多項式乗算を行うHLS IP

---

## 1リクエスト(1チャネル)あたりの処理の流れ

```
[PC]                                [MicroBlaze: main.c]                     [HLS IP: poly_mul_ntt_crt]
  |                                        |                                            |
  | 1. A mod qi, B mod qi を計算 (RNS分解)   |                                            |
  |                                        |                                            |
  | 2. リクエスト送信 (131074バイト)          |                                            |
  |    'C'+ch(1B)+ai(8Bx8192)+bi(8Bx8192) ─>| 3. inbyte()で'C'が来るまで読み飛ばす (再同期) |
  |                                        | 4. チャネル番号(1B)を読み取り保持            |
  |                                        | 5. 係数列をUARTから読みながら直接           |
  |                                        |    共有BRAM (axi_bram_ctrl_poly) へ書き込み |
  |                                        |                                            |
  |                                        | 6. Set_mem() (初回のみ) / Set_ch(ch)        |
  |                                        | 7. Start() ──────────────────────────────> | 8. m_axi_gmem 経由で共有BRAMから
  |                                        | 9. IsDone() をポーリング                     |    ai,bi係数をバースト読み込み
  |                                        |    <───────────────────────────────────────| 10. チャネルchのQ/テーブルでNTT計算
  |                                        |                                            |     (正変換x2→点乗算(Barrett)→逆変換
  |                                        |                                            |      (Shoup)) → 結果を共有BRAMへ書戻し
  |                                        |                                            | 11. 計算完了 (ap_done)
  |                                        |                                            |
  |                                        | 12. 共有BRAMから結果を読み出しながら         |
  |                                        |     'R'+ch(1B)+結果をUARTへバルク送信       |
  |    'R'+ch(1B)+結果(8Bx8192) <───────── |     (無限ループの先頭に戻り 3. へ)           |
  |                                        |                                            |
  | 13. 応答のchが送信時のchと一致するか確認 |                                            |
  | 14. 全チャネル(L回)繰り返す              |                                            |
  | 15. Lチャネル分の残差からCRT復元         |                                            |
  | 16. 期待値と突き合わせて[OK]/[MISMATCH]  |                                            |
```

**所要時間の見積り**: 1チャネルあたりの往復は `poly_mul_ntt_openfhe_n8192_soc` と同様
約15〜20秒 (115200bpsのUART転送が支配的)。**Lチャネル分 (時分割) を直列に繰り返す**ため、
1回の多項式乗算全体ではおよそ L倍 (L=2なら約30〜40秒) かかる見込み。

## 通信プロトコル

| 方向 | バイト列 | 意味 |
|------|---------|------|
| PC→FPGA | `'C'` + ch(1バイト) + A係数x8192(各8バイトLE) + B係数x8192(各8バイトLE) = 131074バイト | チャネルchのモジュラスqi未満の値 |
| FPGA→PC | `'R'` + ch(1バイト) + 結果係数x8192(各8バイトLE) = 65538バイト | 同上 |

先頭の `'C'`/`'R'` はシンクバイト (既存プロジェクトから継承)。チャネルバイトはシンクバイトの
直後、係数列の前に置く。ズレが生じても次のシンクバイトまで読み飛ばせば復帰できる。

## 関連ファイル

| ファイル | 役割 |
|---------|------|
| `high_level/src/poly_mul_ntt.cpp` | NTTアクセラレータのHLS実装 (本体、チャネル引数ch対応) |
| `low_level/scripts/create_project.tcl` | Block Design構築 (共有BRAM含む) 〜ビットストリーム生成 |
| `software/src/main.c` | MicroBlaze側のバルクプロトコル処理 (チャネルバイト対応) |
| `software/scripts/create_platform.py` / `create_app.py` | Vitisプラットフォーム・アプリ作成 |
| `host/host_crt.py` | PC側のRNS分解・Lチャネル送受信・CRT復元・検証 |
| `fpga/lib/ntt_params.py` / `ntt_reference.py` / `ntt_testvectors.py` | 共有パラメータ/リファレンス/テストベクトル生成ツール (CRT/RNS対応) |
| `NTT_ALGORITHM.md` | NTT計算・CRT/RNS分解・Shoup乗算の数学的詳細 |
| `TROUBLESHOOTING.md` | 設計判断・落とし穴の記録 |
