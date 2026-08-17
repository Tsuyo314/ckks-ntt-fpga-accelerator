#include <stdint.h>
#include "xil_printf.h"
#include "xparameters.h"
#include "xpoly_mul_ntt_crt.h"

// CRT/RNS分解 + Shoup乗算バタフライ版 負巡回NTT (N=8192, Lチャネル, 各チャネル60ビット
// 素数qi) で多項式同士を乗算するアクセラレータを、PCからバルク転送で叩くための
// 同期バイト方式のバイナリプロトコル。
//
// [2フェーズ構成] HLSコアのBRAM予算の都合上 (poly_mul_ntt.cpp / TROUBLESHOOTING.md 5節
// 参照)、1回のポリ乗算を2回のStart()呼び出しに分割している:
//   フェーズ0 (順変換+点ごとの乗算): ホストがモジュラス関連スカラー(q,mu,ninv,ninvsh)+
//     A+B+RFWD+RFWD_SHを送り、コアはntt_fwd(A),ntt_fwd(B),点ごとの乗算(Barrett)までを
//     行う。結果はコア内部(BRAM, static)に保持されたまま (memへの書き戻しなし)。
//     応答はACKのみ。
//   フェーズ1 (逆変換): ホストが共有メモリの回転因子テーブル領域をRINV/RINV_SHで
//     上書きして送り、コアはそれを再ロードしてntt_inv(A)を実行し、結果をmemへ書き戻す。
//
// リクエスト/レスポンスのバイト数:
//   フェーズ0リクエスト (PC→FPGA):
//     'C' + ch(1B) + q(8B) + mu(8B) + ninv(8B) + ninvsh(8B)
//     + A係数x8192(8B LE) + B係数x8192(8B LE) + RFWD x8192(8B LE) + RFWD_SH x8192(8B LE)
//     = 1 + 1 + 32 + 4*65536 = 262178 バイト
//   フェーズ0レスポンス (FPGA→PC): 'A' + ch(1B) = 2 バイト (ACKのみ、結果はコア内部保持)
//   フェーズ1リクエスト (PC→FPGA):
//     'D' + ch(1B) + RINV x8192(8B LE) + RINV_SH x8192(8B LE) = 1 + 1 + 2*65536 = 131074 バイト
//   フェーズ1レスポンス (FPGA→PC): 'R' + ch(1B) + 結果係数x8192(8B LE) = 1 + 1 + 65536 = 65538 バイト
//
// 共有BRAM (256KB, 32bit幅) 上のレイアウト: ブロック b in {A=0, B=1, TAB=2, TABSH=3}、
// 係数iのオフセット = word[2*(b*NTT_N+i)] (下位32bit), word[2*(b*NTT_N+i)+1] (上位32bit)。
// フェーズ0ではTAB/TABSH領域にRFWD/RFWD_SHを、フェーズ1では同じ領域にRINV/RINV_SHを書く
// (ホスト側が使い回す。host/host_crt.py参照)。結果は常にブロックA(オフセット0)から
// 読み出す (poly_mul_ntt.cppのWRループがmem[0,N)に書き戻すため)。
//
// 対話ループ中は xil_printf を使わない (バイナリプロトコルに混ざって
// host側のパースが壊れるため。poly_mul_ntt_openfhe_soc/TROUBLESHOOTING.md 参照)。

#define NTT_N 8192
#define SYNC_REQ0 'C'  // フェーズ0リクエスト同期バイト
#define SYNC_ACK0 'A'  // フェーズ0レスポンス(ACKのみ)
#define SYNC_REQ1 'D'  // フェーズ1リクエスト同期バイト
#define SYNC_RESP 'R'  // フェーズ1レスポンス(結果あり)

// axi_bram_ctrl_poly の S_AXI ベースアドレス。Vitisプラットフォーム生成後の
// xparameters.h に実際のマクロ名 (BD上のセル名 axi_bram_ctrl_poly に由来) が
// 出力されるので、ビルド後に xparameters.h を確認しこの #define を実値に
// 合わせること (アドレスを決め打ちしない。poly_mul_ntt_openfhe_n8192_soc/main.c で
// 実際に XPAR_AXI_BRAM_CTRL_POLY_S_AXI_BASEADDR ではなく XPAR_AXI_BRAM_CTRL_POLY_BASEADDR
// だったことがあるので要確認)。
#ifndef POLY_MEM_BASEADDR
#define POLY_MEM_BASEADDR XPAR_AXI_BRAM_CTRL_POLY_BASEADDR
#endif

static inline uint32_t read_u32_le(void) {
    uint32_t v = 0;
    v |= (uint32_t)(unsigned char)inbyte();
    v |= (uint32_t)(unsigned char)inbyte() << 8;
    v |= (uint32_t)(unsigned char)inbyte() << 16;
    v |= (uint32_t)(unsigned char)inbyte() << 24;
    return v;
}

static inline uint64_t read_u64_le(void) {
    uint64_t lo = read_u32_le();
    uint64_t hi = read_u32_le();
    return lo | (hi << 32);
}

static inline void write_u32_le(uint32_t v) {
    outbyte((char)(v & 0xFF));
    outbyte((char)((v >> 8) & 0xFF));
    outbyte((char)((v >> 16) & 0xFF));
    outbyte((char)((v >> 24) & 0xFF));
}

// 共有BRAM上のブロックb(0=A,1=B,2=TAB,3=TABSH)の係数iへ64bit値を下位/上位32bit語に
// 分割して書き込む。
static inline void write_coef(volatile uint32_t *poly_mem, int b, int i, uint64_t v) {
    poly_mem[2 * (b * NTT_N + i)]     = (uint32_t)(v & 0xFFFFFFFFu);
    poly_mem[2 * (b * NTT_N + i) + 1] = (uint32_t)(v >> 32);
}

int main() {
    XPoly_mul_ntt_crt accel;
    volatile uint32_t *poly_mem = (volatile uint32_t *)POLY_MEM_BASEADDR;

    if (XPoly_mul_ntt_crt_Initialize(&accel, 0) != XST_SUCCESS) {
        xil_printf("ERROR: XPoly_mul_ntt_crt_Initialize failed\r\n");
        while (1) {}
    }
    XPoly_mul_ntt_crt_Set_mem(&accel, (u32)POLY_MEM_BASEADDR);

    xil_printf("READY poly_mul_ntt_crt_n8192_soc (bulk mode, N=8192, CRT/RNS, 2-phase)\r\n");

    while (1) {
        // --- フェーズ0: 順変換+点ごとの乗算 ---
        while ((unsigned char)inbyte() != SYNC_REQ0) {}  // シンクバイトが来るまで読み飛ばす(再同期)

        unsigned char ch = (unsigned char)inbyte();
        uint64_t q      = read_u64_le();
        uint64_t mu     = read_u64_le();
        uint64_t ninv   = read_u64_le();
        uint64_t ninvsh = read_u64_le();

        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 0, i, read_u64_le());  // A
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 1, i, read_u64_le());  // B
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 2, i, read_u64_le());  // TAB = RFWD
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 3, i, read_u64_le());  // TABSH = RFWD_SH

        XPoly_mul_ntt_crt_Set_q(&accel, q);
        XPoly_mul_ntt_crt_Set_mu(&accel, mu);
        XPoly_mul_ntt_crt_Set_ninv(&accel, ninv);
        XPoly_mul_ntt_crt_Set_ninvsh(&accel, ninvsh);
        XPoly_mul_ntt_crt_Set_phase(&accel, 0);
        XPoly_mul_ntt_crt_Start(&accel);
        while (!XPoly_mul_ntt_crt_IsDone(&accel)) {}

        outbyte(SYNC_ACK0);
        outbyte((char)ch);

        // --- フェーズ1: 逆変換 ---
        while ((unsigned char)inbyte() != SYNC_REQ1) {}  // シンクバイトが来るまで読み飛ばす(再同期)
        (void)inbyte();  // ch (フェーズ0と同じ値をホストが送る想定。応答で使うのでフェーズ0側のchを使う)

        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 2, i, read_u64_le());  // TAB = RINV
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 3, i, read_u64_le());  // TABSH = RINV_SH

        // q/ninv/ninvsh はフェーズ0で設定済みのCTRLレジスタ値がそのまま有効 (Start()を
        // またいでも制御レジスタの値は保持される)。phaseだけ切り替えれば良い。
        XPoly_mul_ntt_crt_Set_phase(&accel, 1);
        XPoly_mul_ntt_crt_Start(&accel);
        while (!XPoly_mul_ntt_crt_IsDone(&accel)) {}

        outbyte(SYNC_RESP);
        outbyte((char)ch);
        for (int i = 0; i < NTT_N; i++) {
            write_u32_le(poly_mem[2 * i]);
            write_u32_le(poly_mem[2 * i + 1]);
        }
    }

    return 0;
}
