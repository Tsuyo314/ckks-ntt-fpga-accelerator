#include <stdint.h>
#include "xil_printf.h"
#include "xparameters.h"
#include "xpoly_ntt_inv.h"

// poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ3 (逆変換) 専用ビットストリームの
// MicroBlazeファームウェア。単一フェーズプロトコル (同期バイト1つ + Start()/IsDone() 1回
// + 応答1つ)。poly_ntt_pw_n8192_soc/software/src/main.c と同じ構成。
//
// リクエスト (PC→FPGA):
//   'I' + ch(1B) + q(8B) + ninv(8B) + ninvsh(8B) + Ĉ係数x8192(8B LE)
//     + RINV係数x8192(8B LE) + RINV_SH係数x8192(8B LE)
//   = 1 + 1 + 24 + 3*65536 = 196634 バイト
// レスポンス (FPGA→PC):
//   'i' + ch(1B) + result係数x8192(8B LE) = 1 + 1 + 65536 = 65538 バイト
//
// 共有BRAM (192KB, 32bit幅) 上のレイアウト: ブロック b in {C_HAT=0, TAB=1, TABSH=2}、
// 係数iのオフセット = word[2*(b*NTT_N+i)] (下位32bit), word[2*(b*NTT_N+i)+1] (上位32bit)。
// 結果は常にブロックC_HAT(オフセット0)から読み出す (poly_ntt_inv.cppがmem[0,N)に
// 書き戻すため)。
//
// 対話ループ中は xil_printf を使わない (バイナリプロトコルに混ざって
// host側のパースが壊れるため)。

#define NTT_N 8192
#define SYNC_REQ 'I'   // リクエスト同期バイト
#define SYNC_RESP 'i'  // レスポンス同期バイト

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

// 共有BRAM上のブロックb(0=C_HAT,1=TAB,2=TABSH)の係数iへ64bit値を下位/上位32bit語に
// 分割して書き込む。
static inline void write_coef(volatile uint32_t *poly_mem, int b, int i, uint64_t v) {
    poly_mem[2 * (b * NTT_N + i)]     = (uint32_t)(v & 0xFFFFFFFFu);
    poly_mem[2 * (b * NTT_N + i) + 1] = (uint32_t)(v >> 32);
}

int main() {
    XPoly_ntt_inv accel;
    volatile uint32_t *poly_mem = (volatile uint32_t *)POLY_MEM_BASEADDR;

    if (XPoly_ntt_inv_Initialize(&accel, 0) != XST_SUCCESS) {
        xil_printf("ERROR: XPoly_ntt_inv_Initialize failed\r\n");
        while (1) {}
    }
    XPoly_ntt_inv_Set_mem(&accel, (u32)POLY_MEM_BASEADDR);

    xil_printf("READY poly_ntt_inv_n8192_soc (bulk mode, N=8192, phase=inv)\r\n");

    while (1) {
        while ((unsigned char)inbyte() != SYNC_REQ) {}  // シンクバイトが来るまで読み飛ばす(再同期)

        unsigned char ch = (unsigned char)inbyte();
        uint64_t q      = read_u64_le();
        uint64_t ninv   = read_u64_le();
        uint64_t ninvsh = read_u64_le();

        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 0, i, read_u64_le());  // C_HAT
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 1, i, read_u64_le());  // TAB = RINV
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 2, i, read_u64_le());  // TABSH = RINV_SH

        XPoly_ntt_inv_Set_q(&accel, q);
        XPoly_ntt_inv_Set_ninv(&accel, ninv);
        XPoly_ntt_inv_Set_ninvsh(&accel, ninvsh);
        XPoly_ntt_inv_Start(&accel);
        while (!XPoly_ntt_inv_IsDone(&accel)) {}

        outbyte(SYNC_RESP);
        outbyte((char)ch);
        for (int i = 0; i < NTT_N; i++) {
            write_u32_le(poly_mem[2 * i]);
            write_u32_le(poly_mem[2 * i + 1]);
        }
    }

    return 0;
}
