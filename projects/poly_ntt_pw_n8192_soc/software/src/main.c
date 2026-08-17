#include <stdint.h>
#include "xil_printf.h"
#include "xparameters.h"
#include "xpoly_ntt_pw.h"

// poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ2 (pointwise乗算) 専用ビット
// ストリームのMicroBlazeファームウェア。poly_mul_ntt_crt_n8192_socの2フェーズ
// ('C'/'A'+'D'/'R') プロトコルと違い、このビットストリームは常に1つのフェーズしか
// 行わないので「同期バイト1つ + Start()/IsDone() 1回 + 応答1つ」の単純なループにする。
//
// リクエスト (PC→FPGA):
//   'P' + ch(1B) + q(8B) + mu(8B) + Â係数x8192(8B LE) + B̂係数x8192(8B LE)
//   = 1 + 1 + 16 + 2*65536 = 131090 バイト
// レスポンス (FPGA→PC):
//   'p' + ch(1B) + Ĉ係数x8192(8B LE) = 1 + 1 + 65536 = 65538 バイト
//
// 共有BRAM (128KB, 32bit幅) 上のレイアウト: ブロック b in {A_HAT=0, B_HAT=1}、
// 係数iのオフセット = word[2*(b*NTT_N+i)] (下位32bit), word[2*(b*NTT_N+i)+1] (上位32bit)。
// 結果は常にブロックA_HAT(オフセット0)から読み出す (poly_ntt_pw.cppがmem[0,N)に
// 書き戻すため)。
//
// 対話ループ中は xil_printf を使わない (バイナリプロトコルに混ざって
// host側のパースが壊れるため。poly_mul_ntt_openfhe_soc/TROUBLESHOOTING.md参照)。

#define NTT_N 8192
#define SYNC_REQ 'P'  // リクエスト同期バイト
#define SYNC_RESP 'p' // レスポンス同期バイト

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

// 共有BRAM上のブロックb(0=A_HAT,1=B_HAT)の係数iへ64bit値を下位/上位32bit語に
// 分割して書き込む。
static inline void write_coef(volatile uint32_t *poly_mem, int b, int i, uint64_t v) {
    poly_mem[2 * (b * NTT_N + i)]     = (uint32_t)(v & 0xFFFFFFFFu);
    poly_mem[2 * (b * NTT_N + i) + 1] = (uint32_t)(v >> 32);
}

int main() {
    XPoly_ntt_pw accel;
    volatile uint32_t *poly_mem = (volatile uint32_t *)POLY_MEM_BASEADDR;

    if (XPoly_ntt_pw_Initialize(&accel, 0) != XST_SUCCESS) {
        xil_printf("ERROR: XPoly_ntt_pw_Initialize failed\r\n");
        while (1) {}
    }
    XPoly_ntt_pw_Set_mem(&accel, (u32)POLY_MEM_BASEADDR);

    xil_printf("READY poly_ntt_pw_n8192_soc (bulk mode, N=8192, phase=pointwise)\r\n");

    while (1) {
        while ((unsigned char)inbyte() != SYNC_REQ) {}  // シンクバイトが来るまで読み飛ばす(再同期)

        unsigned char ch = (unsigned char)inbyte();
        uint64_t q  = read_u64_le();
        uint64_t mu = read_u64_le();

        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 0, i, read_u64_le());  // A_HAT
        for (int i = 0; i < NTT_N; i++) write_coef(poly_mem, 1, i, read_u64_le());  // B_HAT

        XPoly_ntt_pw_Set_q(&accel, q);
        XPoly_ntt_pw_Set_mu(&accel, mu);
        XPoly_ntt_pw_Start(&accel);
        while (!XPoly_ntt_pw_IsDone(&accel)) {}

        outbyte(SYNC_RESP);
        outbyte((char)ch);
        for (int i = 0; i < NTT_N; i++) {
            write_u32_le(poly_mem[2 * i]);
            write_u32_le(poly_mem[2 * i + 1]);
        }
    }

    return 0;
}
