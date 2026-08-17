#include <stdint.h>
#include "xil_printf.h"
#include "xparameters.h"
#include "xpoly_mul_ntt_n8192.h"

// OpenFHE の CT-forward/GS-inverse 負巡回 NTT (N=8192, mod Q (60ビット),
// 環 Z_Q[x]/(x^8192+1)) で多項式同士を乗算するアクセラレータを、PCから
// バルク転送で叩くための同期バイト方式のバイナリプロトコル。
//
// poly_mul_ntt_openfhe_soc (N=4) の9バイト固定パケット・スカラーレジスタ方式は
// 8192係数には使えないため、係数列をまるごと共有AXI BRAM (axi_bram_ctrl_poly)
// へ流し込み、HLSコア (m_axi_gmem) がそこを読み書きする方式に変更した。
//
// リクエスト (PC→FPGA): 'C' + A係数x8192(8バイトLE) + B係数x8192(8バイトLE)
//   = 1 + 2*8192*8 = 131073 バイト
// レスポンス (FPGA→PC): 'R' + 結果係数x8192(8バイトLE)
//   = 1 + 8192*8 = 65537 バイト
//
// 各64ビット係数は、共有BRAMが32ビット幅であるため下位/上位32ビット語
// (リトルエンディアン) の2語に分割して格納する。係数iのオフセット:
//   A: word[2*i], word[2*i+1]
//   B: word[2*(N+i)], word[2*(N+i)+1]
// HLSコア (poly_mul_ntt_n8192.cpp) は mem[0..N-1]=A, mem[N..2N-1]=B を読み、
// 結果を mem[0..N-1] に上書きするため、レスポンスもA領域から読み出す。
//
// 対話ループ中は xil_printf を使わない (バイナリプロトコルに混ざって
// host側のパースが壊れるため。poly_mul_ntt_openfhe_soc/TROUBLESHOOTING.md 参照)。

#define NTT_N 8192
#define SYNC_REQ  'C'
#define SYNC_RESP 'R'

// axi_bram_ctrl_poly の S_AXI ベースアドレス。Vitisプラットフォーム生成後の
// xparameters.h に実際のマクロ名 (BD上のセル名 axi_bram_ctrl_poly に由来) が
// 出力されるので、ビルド後に xparameters.h を確認しこの #define を実値に
// 合わせること (アドレスを決め打ちしない)。
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

static inline void write_u32_le(uint32_t v) {
    outbyte((char)(v & 0xFF));
    outbyte((char)((v >> 8) & 0xFF));
    outbyte((char)((v >> 16) & 0xFF));
    outbyte((char)((v >> 24) & 0xFF));
}

int main() {
    XPoly_mul_ntt_n8192 accel;
    volatile uint32_t *poly_mem = (volatile uint32_t *)POLY_MEM_BASEADDR;

    if (XPoly_mul_ntt_n8192_Initialize(&accel, 0) != XST_SUCCESS) {
        xil_printf("ERROR: XPoly_mul_ntt_n8192_Initialize failed\r\n");
        while (1) {}
    }
    XPoly_mul_ntt_n8192_Set_mem(&accel, (u32)POLY_MEM_BASEADDR);

    xil_printf("READY poly_mul_ntt_openfhe_n8192_soc (bulk mode, N=8192)\r\n");

    while (1) {
        // シンクバイトが来るまで読み飛ばす (再同期)
        while ((unsigned char)inbyte() != SYNC_REQ) {}

        for (int i = 0; i < NTT_N; i++) {
            uint32_t lo = read_u32_le();
            uint32_t hi = read_u32_le();
            poly_mem[2 * i]     = lo;
            poly_mem[2 * i + 1] = hi;
        }
        for (int i = 0; i < NTT_N; i++) {
            uint32_t lo = read_u32_le();
            uint32_t hi = read_u32_le();
            poly_mem[2 * (NTT_N + i)]     = lo;
            poly_mem[2 * (NTT_N + i) + 1] = hi;
        }

        XPoly_mul_ntt_n8192_Start(&accel);
        while (!XPoly_mul_ntt_n8192_IsDone(&accel)) {}

        outbyte(SYNC_RESP);
        for (int i = 0; i < NTT_N; i++) {
            write_u32_le(poly_mem[2 * i]);
            write_u32_le(poly_mem[2 * i + 1]);
        }
    }

    return 0;
}
