# トラブルシューティング総集編

このリポジトリの5プロジェクトを開発・移植・実機検証する過程で発生した問題と対処を
テーマ別にまとめたもの。各プロジェクト固有の詳細・数値根拠は
`projects/*/TROUBLESHOOTING.md` に残してあり、本ファイルはそれらを横断して
「何がうまくいかなかったか」「なぜか」「どう直したか」を一望できるようにした要約版。

対象プロジェクトと役割は [README.md](README.md) を参照。

---

## 1. HLS設計での落とし穴

### 1.1 `PIPELINE II=1` の強制がBarrett乗算器のタイミングを崩壊させた

**症状**: バタフライループに素朴に `#pragma HLS PIPELINE II=1` を付けて合成したところ、
推定Fmaxが **13.21 MHz** しか出ず、100MHz目標に対して致命的に崩壊した。

**原因**: Barrett還元は124×62ビット→184ビットという重い乗算(`x*μ`)を含む。バタフライ
ループには真のメモリ依存(`a[j]`/`a[j+t]`への読み書き)があり、`II=1`を満たすために
乗算器が1サイクルの組合せ回路に押し込まれ、1段あたりの遅延が10nsを大幅に超えていた。

**対策**: `PIPELINE II=1` を単なる `PIPELINE`(II指定なし)に変え、HLSに達成可能なIIを
選ばせた。UART往復(15〜20秒)が支配的ボトルネックなので、IIが多少大きくなっても
計算時間(全体でも数十ms)への影響は無視できると判断。結果、推定Fmaxは**136.99MHz**まで
回復した。
(`poly_mul_ntt_openfhe_n8192_soc/TROUBLESHOOTING.md` #4)

### 1.2 リソース見積もりが大きく外れた(DSP超過・BRAM過少)

計画段階の見積もりと実測が食い違った例:

| プロジェクト | 見積もり | 実測 | 差の原因 |
|---|---|---|---|
| poly_mul_ntt_openfhe_n8192_soc | DSP 30〜60個 | **184個(76%)** | Barrett還元が複数ビット幅の乗算(u×v, x×μ, qhat×Q)を含み、forward/inverse/pointwiseそれぞれに個別の乗算器インスタンスがインライン生成されたため |
| poly_mul_ntt_openfhe_n8192_soc | BRAM 72% | **53%** | (逆に見積もりより少なく済んだケース) |

いずれも致命的ではなかったが、「HLSは呼び出しコンテキストごとに乗算器を複製する」
「ビット幅の組み合わせが増えるほどDSPが線形以上に増える」ことは事前に見積もりづらい。

### 1.3 チャネル分のテーブルを合成時ROMへ同時に焼き込み、BRAMが122%に達した

**症状**: CRT/RNS版(L=2チャネル)を素朴に「`[NTT_L][NTT_N]`のテーブルをROMとして持ち、
`ch`レジスタで実行時に行選択する」設計で合成したところ、HLSコア単体でBRAMが
**332/270ブロック(122%)** となり、L=2の時点で既に物理的に収まらなかった。

**原因**: 「チャネル切り替え」を論理的な時分割(同じ回路が使うROM行を選ぶだけ)として
実装しており、物理的なBRAM容量はLチャネル分を同時に確保する必要があった。

**対策**: モジュラス・回転因子テーブルを合成時ROMから完全に排除し、呼び出しごとに
ホストがUART経由で1チャネル分だけを実行時ロードする設計に再設計。これによりHLSコア
単体のBRAMは332→132ブロック(48%)まで削減できた。副作用として、**チャネル数Lは
ハードウェア資源を消費しなくなった代わりに、チャネルが増えるほど処理時間が線形に
増える**(1チャネルあたり通信を含め約80秒)という設計になった。
(`poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md` #5、詳細は
[poly_mul_ntt_crt_n8192_soc/README.md「チャネル数(L, 素数の個数)について」](projects/poly_mul_ntt_crt_n8192_soc/README.md#チャネル数l-素数の個数について))

### 1.4 「二重バッファリング」問題 — HLSコア単体のBRAM%だけでは全体の余裕がわからない

1.3の対策でHLSコア単体のBRAMは48%まで下がったが、実際にVivadoで論理合成・実装まで
進めると`place_design`のDRCで **`RAMB36E1: 144個要求 / 135個しか利用可能でない`**
というエラーで失敗した(チップ全体で289/270ブロック=107%相当)。

**原因**: HLSコア単体のcsynth.rpt(48%)には、共有`axi_bram_ctrl_poly`(256KBに拡張済み)
分のBRAMが含まれていない。同じデータを「共有BRAM上」と「HLSコア内蔵BRAM上」の
両方に同時に確保する構成だったため、チップ全体で見ると二重に確保していた。

**対策**: `m_axi`バースト長を256→16に縮小(FIFOのBRAM使用量20→4ブロック)、
MicroBlaze `local_mem`を32KB→16KBに縮小、の2点で削減し、最終的にチップ全体の
Block RAM使用率は**133/135 RAMB36(98.5%)**、空きはわずか2ブロックというギリギリの
マージンで実装に成功した。

**教訓**: HLSコア単体の資源使用率(csynth.rpt)は、Block Design全体で共有するバッファや
FIFOの分を含んでいない。「HLSコア単体で余裕がある」ように見えても、Vivado実装まで
進めて初めて発覚するオーバーフローがあり得る。
(`poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md` #5)

### 1.5 「フェーズを分離しただけ」ではBRAMは減らない場合がある

CRT版を3ビットストリーム(順変換/pointwise乗算/逆変換)に分割した際、pointwise版は
BRAM 98.5%→27.4%と劇的に改善したが、**順変換版はBRAM 98.5%→98.52%とほぼ横ばい**だった。

**原因**: 順変換に必要な`A[N]`/`B[N]`/`TAB[N]`/`TABSH[N]`の4配列(256KB)は結合設計でも
分割後でも同一サイズであり、BRAM使用量を支配していたのはこの配列サイズ(+それと
同サイズの共有BRAMとの二重バッファリング)であって、pointwise・逆変換ロジック自体は
BRAMをほとんど消費していなかった。分割で実際に改善したのは**タイミング**
(WNS -0.030ns → +0.466ns、同居していた制御ロジック・配線輻輳が取り除かれたため)で
あって、BRAM自体を減らすには配列サイズそのものを見直す必要がある。
(`poly_ntt_fwd_n8192_soc/TROUBLESHOOTING.md` #1)

### 1.6 `assign_bd_address`のrangeは2のべき乗でなければならない

共有BRAMの実メモリサイズが192KB(2のべき乗でない3ブロック構成)のプロジェクト
(逆変換版)で、`assign_bd_address -offset $off -range 192K $seg` が
`The proposed range '192K' must be a power of 2.` で失敗した。

**対策**: アドレス空間だけ次のべき乗(256K)を割り当て、実メモリは192KBのまま変更しない
(未使用の64KB分のアドレス空間は単に使われないだけで実害なし)。
(`poly_ntt_inv_n8192_soc/TROUBLESHOOTING.md` #1)

### 1.7 タイミング未達(WNS負)でも実用上問題にならないと判断したケース

CRT版(結合設計)は最終ルーティング後 **WNS=-0.030ns、TNS=-0.120ns**
(36858エンドポイント中4件未達)で、Vivado上は「制約未達」の判定だった。しかし
UART(115200bps)がボトルネックの設計であり、内部ロジックのわずか30ピコ秒の未達は
実用上無視できると判断して実機検証に進み、実際に4/4テストベクトルで正しい結果が
得られることを確認した。「タイミングクロージャしていない=即座に動かない」ではなく、
実際の律速要因がどこにあるかを踏まえて判断する必要がある(ただし一般には推奨される
判断ではなく、本件はUART律速という特殊事情によるもの)。
(`poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md` #6, #9)

---

## 2. Vitis/Windowsツールチェーン特有の落とし穴

以下は全プロジェクト共通で繰り返し踏んだ問題(MicroBlaze SoC構成を使うプロジェクト全般)。

### 2.1 Vitis Unified Python API (`vitis.bat -s`) の文字コードエラー

`create_platform.py`/`create_app.py`は`import vitis`するPythonスクリプトだが、
**通常の`python`コマンドでは実行できない**(`vitis`モジュールはVitis同梱のPython環境
専用)。正しくは`vitis.bat -s <script.py>`で実行する必要があり、さらに**同じ`cmd.exe`
セッション内で先に`chcp 65001`を実行しておかないと文字コードエラーになる**:

```bash
cmd.exe /c "chcp 65001 && C:\AMDDesignTools\2025.2\Vitis\bin\vitis.bat -s software\scripts\create_platform.py"
```

これは今回リポジトリを整備し実機で通しテストする過程で改めて踏んだ問題で、
公開前のREADMEドラフトでは誤って`python software\scripts\create_platform.py`と
書いてしまっていた(4節参照)。

### 2.2 `VITIS_EMBEDDED_INSTALL=true` が必要

`create_platform.py`/`create_app.py`の先頭で`import vitis`する前に
`os.environ['VITIS_EMBEDDED_INSTALL'] = 'true'`を設定する必要がある(未設定だと
組み込み向けAPIが有効化されない)。

### 2.3 xsctの`targets -set -filter`は`"MicroBlaze #*"`を使う

`download_and_run.tcl`でMicroBlazeターゲットを選択する際、`"MicroBlaze*"`だと
Debug Module自体にもマッチしてしまうため、`{name =~ "MicroBlaze #*"}`という
フィルタが必要。

### 2.4 Windows MAX_PATH(260バイト)制限

Vitisが生成するBSP/ドメイン/アプリのビルド成果物パスは非常に深くなりやすく、
このリポジトリのように長いディレクトリ名(`ntt-fhe-fpga-accelerator\projects\...`)
の下でビルドすると260文字制限に抵触しやすい。`create_project.tcl`の`ip_repo`や
`create_platform.py`/`create_app.py`の`PROJ_ROOT`は、`subst W: <対象プロジェクトへの
絶対パス>`で短いドライブレターにマッピングしてから実行することを前提にしている
(各スクリプト内のコメント・`PROJ_ROOT = "W:"`参照)。

### 2.5 Vitis Serverのワークスペースロック残留(今回の実機検証で複数回遭遇)

`create_app.py`を実行すると、直前の`create_platform.py`/`create_app.py`の実行が
正常終了していても以下のエラーで失敗することが繰り返し発生した:

```
Exception: Vitis Create Client failed, Cannot set workspace, status = StatusCode.FAILED_PRECONDITION,
           details = 'The workspace 'W:\software\workspace' is already in use.'
```

**原因**: Vitis Server(`com.xilinx.rigel.app.RigelApp`、`java.exe`プロセス)が
バックグラウンドに残り、`software/workspace/_ide/.wsdata/.lock`を握ったままになる。

**対策**: 該当する`java.exe`(`RigelApp`)プロセスを`taskkill /F`で終了し、
`.lock`ファイルを削除してから再実行する。PowerShellの`Get-CimInstance Win32_Process`で
コマンドラインを確認すれば`RigelApp`のPIDを特定できる。fwd/pw/inv 3プロジェクトを
それぞれゼロからビルドする過程で、ほぼ毎回このロックに遭遇した。

---

## 3. このリポジトリの公開準備・実機検証で新たに発覚した問題

以下は元の私物ワークスペースには存在せず、今回このリポジトリを整備して実機で
通しテストする過程で見つけた問題(前節までとは異なり、リポジトリそのものの不備)。

### 3.1 「クイックスタート」にMicroBlazeファームウェア書き込み手順が抜けていた

当初のREADMEドラフトは「ビットストリームさえ書き込めばそのまま動く」という想定で
書いていたが、実際にビットストリームだけを書き込んで`host_bulk.py`を実行したところ
**応答が一切返らなかった**(`OSError: unexpected response: got 0 bytes`)。原因は
MicroBlaze側にファームウェア(ELF)がまだ載っていなかったため。ビットストリームの
書き込みと、`xsct`経由でのELFダウンロード・実行は別工程であり、両方必須という
当たり前の点をクイックスタート手順から書き漏らしていた。ビルド済みELFを
`software/elf/`に同梱し、全プロジェクトのREADMEに手順を追記して修正した。

### 3.2 3段パイプライン(fwd/pw/inv)は実は一度もソフトウェア込みで実機テストされていなかった

元の私物ワークスペースを確認したところ、`poly_ntt_fwd_n8192_soc`/`poly_ntt_pw_n8192_soc`/
`poly_ntt_inv_n8192_soc`の3プロジェクトは、Vivado側(ビットストリーム生成・タイミング・
DRC)は完了していたが、**MicroBlaze側のソフトウェア(Vitisプラットフォーム/アプリ)が
一度もビルドされていなかった**(`software/workspace/`が存在しない)。つまり、
以前の私の報告(「実機で動作確認済み」)はVivado合成レベルの検証止まりで不正確だった。
今回改めてゼロからプラットフォーム・アプリをビルドし、実機に書き込んで
fwd→pw→invを通しで実行し、最終結果が期待値と一致することを確認した
(2.5節のロック問題に複数回遭遇しつつも、最終的に3/3成功)。

### 3.3 `.gitignore`の`software/workspace/`パターンが深い階層でマッチしなかった

`.gitignore`に`software/workspace/`(スラッシュを含み、先頭に`**/`なし)と書いていたが、
このパターンはリポジトリ**直下**からの相対パスとしてのみ解釈され、
`projects/<name>/software/workspace/`のような深い階層にはマッチしなかった
(gitignoreの仕様上、途中にスラッシュを含むパターンは`.gitignore`の場所を基準に
アンカーされる)。3.2節の検証で生成された約67MBのVitisビルド成果物が誤って
コミット対象にステージされ、`git status`で発覚した。`**/software/workspace/`に
修正して解消した。

### 3.4 `create_platform.py`のXSAパスが、リポジトリから除外済みの`.runs/`を指していた

`create_platform.py`は元々`<PROJ_ROOT>/low_level/<proj>.runs/impl_1/design_1_wrapper.xsa`
を参照する設計だったが、このリポジトリでは`*.runs/`ディレクトリを`.gitignore`で
除外している(Vivadoの生成物で巨大なため)。そのため、クローンした状態のまま
`create_platform.py`を実行すると`.xsa`が見つからずに失敗する。ビットストリームと
同様に`.xsa`も`low_level/bitstream/`配下に同梱し、`create_platform.py`が参照する
パスをそちらに向けて修正した。

---

## 4. 見積もりと実測が食い違った項目まとめ

| 項目 | 見積もり/当初設計 | 実測/最終結果 | 該当プロジェクト |
|---|---|---|---|
| DSP使用量 | 30〜60個 | 184個(76%) | poly_mul_ntt_openfhe_n8192_soc |
| CRT版チャネルROM化のBRAM | (見積もりなし、L=2で素朴に実装) | 332/270(122%、収まらず) | poly_mul_ntt_crt_n8192_soc |
| チャネル分離後の全体BRAM | HLSコア単体48%(余裕ありに見えた) | チップ全体98.5%(二重バッファリングで発覚) | poly_mul_ntt_crt_n8192_soc |
| 3分割による順変換版のBRAM削減効果 | 分割すれば減るはず | ほぼ横ばい(98.5%→98.52%) | poly_ntt_fwd_n8192_soc |
| クイックスタートの必要工程 | ビットストリーム書き込みのみでOKのはず | MicroBlazeファームウェア書き込みも必須と判明 | 全5プロジェクト |
| fwd/pw/invの実機検証状況 | 「検証済み」と認識していた | ソフトウェア未ビルド・実は未検証だった | poly_ntt_fwd/pw/inv_n8192_soc |

## 5. 各プロジェクトの詳細トラブルシューティング

- [projects/poly_mul_ntt_openfhe_n8192_soc/TROUBLESHOOTING.md](projects/poly_mul_ntt_openfhe_n8192_soc/TROUBLESHOOTING.md)
- [projects/poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md](projects/poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md)
- [projects/poly_ntt_fwd_n8192_soc/TROUBLESHOOTING.md](projects/poly_ntt_fwd_n8192_soc/TROUBLESHOOTING.md)
- [projects/poly_ntt_pw_n8192_soc/TROUBLESHOOTING.md](projects/poly_ntt_pw_n8192_soc/TROUBLESHOOTING.md)
- [projects/poly_ntt_inv_n8192_soc/TROUBLESHOOTING.md](projects/poly_ntt_inv_n8192_soc/TROUBLESHOOTING.md)
