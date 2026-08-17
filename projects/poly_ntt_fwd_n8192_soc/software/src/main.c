#include <stdint.h>
#include "xil_printf.h"
#include "xparameters.h"
#include "xpoly_ntt_fwd.h"

// poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ1 (順変換) 専用ビットストリームの
// MicroBlazeファームウェア。単一フェーズプロトコル (同期バイト1つ + Start()/IsDone() 1回
// + 応答1つ)。poly_ntt_pw_n8192_soc/software/src/main.c と同じ構成。
//
// リクエスト (PC→FPGA):
//   'F' + ch(1B) + q(8B) + A係数x8192(8B LE) + B係数x8192(8B LE)
//     + RFWD係数x8192(8B LE) + RFWD_SH係数x8192(8B LE)
//   = 1 + 1 + 8 + 4*65536 = 262154 バイト
// レスポンス (FPGA→PC):
//   'f' + ch(1B) + Â係数x8192(8B LE) + B̂係数x8192(8B LE) = 1 + 1 + 131072 = 131074 バイト
//
// 共有BRAM (256KB, 32bit幅) 上のレイアウト: ブロック b in {A=0, B=1, TAB=2, TABSH=3}、
// 係数iのオフセット = word[2*(b*NTT_N+i)] (下位32bit), word[2*(b*NTT_N+i)+1] (上位32bit)。
// 順変換結果は poly_ntt_fwd.cpp がブロックA/Bにそのまま上書きするので、応答はブロック
// A(オフセット0),B(オフセットN)から読み出す。
//
// 対話ループ中は xil_printf を使わない (バイナリプロトコルに混ざって
// host側のパースが壊れるため)。

#define NTT_N 8192
#define SYNC_REQ 'F'   // リクエスト同期バイト
#define SYNC_RESP 'f'  // レスポンス同期バイト

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
    XPoly_ntt_fwd accel;
    volatile uint32_t *poly_mem = (volatile uint32_t *)POLY_MEM_BASEADDR;

    if (XPoly_ntt_fwd_Initialize(&accel, 0) != XST_SUCCESS) {
        xil_printf("ERROR: XPoly_ntt_fwd_Initialize failed\r\n");
        while (1) {}
    }
    XPoly_ntt_fwd_Set_mem(&accel, (u32)POLY_MEM_BASEADDR);

    xil_printf("READY poly_ntt_fwd_n8192_soc (bulk mode, N=8192, phase=fwd)\r\n");

    while (1) {
        while ((unsigned char)inbyte() != SYNC_REQ) {}  // シンクバイトが来るまで読み飛ばす(再同期)

        unsigned char ch = (unsigned char)inbyte();
        uint64_t q = read_u64_le();

        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 0, i, read_u64_le());  // A
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 1, i, read_u64_le());  // B
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 2, i, read_u64_le());  // TAB = RFWD
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 3, i, read_u64_le());  // TABSH = RFWD_SH

        XPoly_ntt_fwd_Set_q(&accel, q);
        XPoly_ntt_fwd_Start(&accel);
        while (!XPoly_ntt_fwd_IsDone(&accel)) {}

        outbyte(SYNC_RESP);
        outbyte((char)ch);
        for (int i = 0; i < 2 * NTT_N; i++) {
            write_u32_le(poly_mem[2 * i]);
            write_u32_le(poly_mem[2 * i + 1]);
        }
    }

    return 0;
}
