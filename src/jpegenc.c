/* ==================================================================
 * jpegenc.c - ベースライン JPEG の符号化(SSE2)
 *
 *  Tight の JPEG 矩形は、受け手が libjpeg などでそのまま読める
 *  完全な JPEG(JFIF)でなければならない。量子化表は IJG の品質の
 *  換算、ハフマン表は規格の付録 K の標準の表を使う。
 *
 *  速さのために、x64 なら必ずある SSE2 だけを使う(AVX2 は古い CPU に
 *  無いので使わない。Windows 10 の機械すべてで動くように)。
 *
 *   色の変換  8 画素ずつ。15 ビットの固定小数点(pmaddwd)。
 *   DCT       libjpeg の整数版(islow、jfdctint.c)と同じ計算を、
 *             8 列(8 行)同時に。縦→横の順に行い、結果は転置した
 *             並び(列が先)のまま量子化とジグザグの表で読み替える。
 *   量子化    除算を、libjpeg-turbo と同じ逆数・補正・倍率のかけ算に
 *             置き換える(結果は除算と一致する)。
 *   ハフマン  0 でない係数の位置を 64 ビットの地図にして飛び越す。
 *             書き出しは 4 バイトずつ(0xFF が無ければそのまま)。
 *
 *  以前の浮動小数点の版(1 画素・1 係数ずつ)より 1920×1080 で
 *  数倍速い。大きさ・画質は libjpeg(PIL)とほぼ同じ。
 * ================================================================== */

#include "jpegenc.h"
#include <string.h>
#include <stdlib.h>
#include <intrin.h>
#include <emmintrin.h>

/* ------------------------------------------------------------------ */
/*  表                                                                  */
/* ------------------------------------------------------------------ */

static const unsigned char k_zigzag[64] = {     /* 自然順の位置 → ジグザグの番号 の逆 */
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static const unsigned char k_lumQ[64] = {        /* 自然順 */
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99 };
static const unsigned char k_chrQ[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };

static const unsigned char k_dcLumBits[17] = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char k_dcLumVal[12]  = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char k_dcChrBits[17] = { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const unsigned char k_dcChrVal[12]  = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char k_acLumBits[17] = { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const unsigned char k_acLumVal[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa };
static const unsigned char k_acChrBits[17] = { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const unsigned char k_acChrVal[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa };

typedef struct { unsigned short code[256]; unsigned char size[256]; } HTab;

static HTab g_dcLum, g_dcChr, g_acLum, g_acChr;
static volatile long g_ready;

static void make_htab(HTab *t, const unsigned char *bits, const unsigned char *val)
{
    int i, j, k = 0;
    unsigned code = 0;
    memset(t, 0, sizeof(*t));
    for (i = 1; i <= 16; i++) {
        for (j = 0; j < bits[i]; j++) {
            t->code[val[k]] = (unsigned short)code;
            t->size[val[k]] = (unsigned char)i;
            k++;
            code++;
        }
        code <<= 1;
    }
}

static void init_tables(void)
{
    if (g_ready) return;
    make_htab(&g_dcLum, k_dcLumBits, k_dcLumVal);
    make_htab(&g_dcChr, k_dcChrBits, k_dcChrVal);
    make_htab(&g_acLum, k_acLumBits, k_acLumVal);
    make_htab(&g_acChr, k_acChrBits, k_acChrVal);
    _InterlockedExchange(&g_ready, 1);
}

/* 転置した並び(列が先、t[k * 8 + m] = 係数[m][k])でのジグザグの順 */
static unsigned char g_tzig[64];

static void init_tzig(void)
{
    int i;
    for (i = 0; i < 64; i++)
        g_tzig[i] = (unsigned char)((k_zigzag[i] & 7) * 8 + (k_zigzag[i] >> 3));
}

struct JpegEnc {
    int            quality;
    unsigned char  qLum[64], qChr[64];  /* 自然順(見出しに書く) */
    unsigned short qt[2][3][64];        /* [輝度 / 色差][逆数 / 補正 / 倍率]、転置した並び */
    short         *plane;               /* MCU 1 行分の Y・Cb・Cr と、間引いた Cb・Cr */
    size_t         planeCap;            /* short の数 */
};

JpegEnc *jpe_new(void)
{
    JpegEnc *e = (JpegEnc *)calloc(1, sizeof(JpegEnc));
    init_tables();
    if (!g_tzig[1]) init_tzig();
    return e;
}

void jpe_free(JpegEnc *e)
{
    if (!e) return;
    free(e->plane);
    free(e);
}

size_t jpe_bound(int w, int h)
{
    size_t blocks = (size_t)((w + 15) / 16) * (size_t)((h + 15) / 16) * 12;
    return blocks * 420 + 1024;
}

/* 除数 → 逆数・補正・倍率(libjpeg-turbo の compute_reciprocal と同じ)。
 * ((|x| + 補正) × 逆数 >> 16) × 倍率 >> 16 が (|x| + 除数/2) / 除数 と一致する。 */
static void reciprocal(unsigned divisor, unsigned short *recip, unsigned short *corr, unsigned short *scale)
{
    unsigned long b;
    unsigned fq, fr, c;
    int r;
    _BitScanReverse(&b, divisor);
    r = 16 + (int)b;
    fq = (1u << r) / divisor;
    fr = (1u << r) % divisor;
    c = divisor / 2;
    if (fr == 0) { fq >>= 1; r--; }
    else if (fr <= divisor / 2) c++;
    else fq++;
    *recip = (unsigned short)fq;
    *corr  = (unsigned short)c;
    *scale = (unsigned short)(1u << (32 - r));
}

static void set_quality(JpegEnc *e, int q)
{
    int i, scale;
    if (q < 1) q = 1;
    if (q > 100) q = 100;
    if (e->quality == q) return;
    e->quality = q;
    scale = q < 50 ? 5000 / q : 200 - q * 2;
    for (i = 0; i < 64; i++) {
        int a = (k_lumQ[i] * scale + 50) / 100, b = (k_chrQ[i] * scale + 50) / 100;
        int t = (i & 7) * 8 + (i >> 3);
        e->qLum[i] = (unsigned char)(a < 1 ? 1 : a > 255 ? 255 : a);
        e->qChr[i] = (unsigned char)(b < 1 ? 1 : b > 255 ? 255 : b);
        /* islow の DCT の結果は 8 倍なので、除数も 8 倍 */
        reciprocal(e->qLum[i] * 8u, &e->qt[0][0][t], &e->qt[0][1][t], &e->qt[0][2][t]);
        reciprocal(e->qChr[i] * 8u, &e->qt[1][0][t], &e->qt[1][1][t], &e->qt[1][2][t]);
    }
}

/* ------------------------------------------------------------------ */
/*  ビットの書き出し                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char     *p;
    unsigned long long buf;     /* 下位 cnt ビットが未出力 */
    int                cnt;
} JW;

/* size は 27 以下(符号 16 + 値 11) */
static __forceinline void jw_put(JW *w, unsigned code, int size)
{
    w->buf = (w->buf << size) | code;
    w->cnt += size;
    if (w->cnt >= 32) {
        unsigned v = (unsigned)(w->buf >> (w->cnt - 32));
        w->cnt -= 32;
        if (!((~v - 0x01010101u) & v & 0x80808080u)) {     /* 0xFF のバイトが無い */
            v = _byteswap_ulong(v);
            memcpy(w->p, &v, 4);
            w->p += 4;
        } else {
            int s;
            for (s = 24; s >= 0; s -= 8) {
                unsigned char c = (unsigned char)(v >> s);
                *w->p++ = c;
                if (c == 0xFF) *w->p++ = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  色の変換と間引き                                                    */
/* ------------------------------------------------------------------ */

/* (a, b) の組を pmaddwd の係数に */
#define PAIR(a, b) _mm_set1_epi32((int)(((unsigned)(unsigned short)(short)(b) << 16) | (unsigned short)(short)(a)))

/* BGRX 8 画素 → Y - 128、Cb、Cr(どれも -128..127)。係数は 2^15 倍、0.5 を足して丸める */
static __forceinline void color8(const unsigned char *s, short *py, short *pcb, short *pcr)
{
    const __m128i m = _mm_set1_epi32(0xFF), one = _mm_set1_epi16(1);
    __m128i p0 = _mm_loadu_si128((const __m128i *)s), p1 = _mm_loadu_si128((const __m128i *)(s + 16));
    __m128i b = _mm_packs_epi32(_mm_and_si128(p0, m), _mm_and_si128(p1, m));
    __m128i g = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(p0, 8), m), _mm_and_si128(_mm_srli_epi32(p1, 8), m));
    __m128i r = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(p0, 16), m), _mm_and_si128(_mm_srli_epi32(p1, 16), m));
    __m128i rgL = _mm_unpacklo_epi16(r, g), rgH = _mm_unpackhi_epi16(r, g);
    __m128i b1L = _mm_unpacklo_epi16(b, one), b1H = _mm_unpackhi_epi16(b, one);
#define CONV(krg, kb) _mm_packs_epi32( \
        _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(rgL, krg), _mm_madd_epi16(b1L, kb)), 15), \
        _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(rgH, krg), _mm_madd_epi16(b1H, kb)), 15))
    _mm_storeu_si128((__m128i *)py, _mm_sub_epi16(CONV(PAIR(9798, 19235), PAIR(3735, 16384)), _mm_set1_epi16(128)));
    _mm_storeu_si128((__m128i *)pcb, CONV(PAIR(-5529, -10855), PAIR(16384, 16384)));
    _mm_storeu_si128((__m128i *)pcr, CONV(PAIR(16384, -13720), PAIR(-2664, 16384)));
#undef CONV
}

/* 画素の rows 行(y0 から。下の端は最後の行を繰り返す)を幅 W の平面へ(右の端は最後の画素を繰り返す) */
static void convert_rows(const unsigned char *px, int stride, int w, int h, int y0, int rows, int W,
                         short *Y, short *Cb, short *Cr)
{
    unsigned char tmp[32];
    int x, y, i;
    for (y = 0; y < rows; y++) {
        int sy = y0 + y < h ? y0 + y : h - 1;
        const unsigned char *row = px + (size_t)sy * (size_t)stride;
        short *py = Y + (size_t)y * W, *pb = Cb + (size_t)y * W, *pr = Cr + (size_t)y * W;
        for (x = 0; x + 8 <= w; x += 8)
            color8(row + (size_t)x * 4, py + x, pb + x, pr + x);
        for (; x < W; x += 8) {
            for (i = 0; i < 8; i++) {
                int sx = x + i < w ? x + i : w - 1;
                memcpy(tmp + i * 4, row + (size_t)sx * 4, 4);
            }
            color8(tmp, py + x, pb + x, pr + x);
        }
    }
}

/* 2×2 の平均(libjpeg の h2v2_downsample と同じ丸め: 1, 2, 1, 2 を足す)。W2 は 8 の倍数 */
static void down_h2v2(const short *a, const short *b, short *out, int W2)
{
    const __m128i ones = _mm_set1_epi16(1), bias = _mm_set_epi32(2, 1, 2, 1);
    int x;
    for (x = 0; x < W2; x += 8) {
        __m128i s0 = _mm_add_epi32(_mm_madd_epi16(_mm_loadu_si128((const __m128i *)(a + 2 * x)), ones),
                                   _mm_madd_epi16(_mm_loadu_si128((const __m128i *)(b + 2 * x)), ones));
        __m128i s1 = _mm_add_epi32(_mm_madd_epi16(_mm_loadu_si128((const __m128i *)(a + 2 * x + 8)), ones),
                                   _mm_madd_epi16(_mm_loadu_si128((const __m128i *)(b + 2 * x + 8)), ones));
        s0 = _mm_srai_epi32(_mm_add_epi32(s0, bias), 2);
        s1 = _mm_srai_epi32(_mm_add_epi32(s1, bias), 2);
        _mm_storeu_si128((__m128i *)(out + x), _mm_packs_epi32(s0, s1));
    }
}

/* 横 2 つの平均(h2v1_downsample と同じ: 0, 1, 0, 1 を足す) */
static void down_h2v1(const short *a, short *out, int W2)
{
    const __m128i ones = _mm_set1_epi16(1), bias = _mm_set_epi32(1, 0, 1, 0);
    int x;
    for (x = 0; x < W2; x += 8) {
        __m128i s0 = _mm_madd_epi16(_mm_loadu_si128((const __m128i *)(a + 2 * x)), ones);
        __m128i s1 = _mm_madd_epi16(_mm_loadu_si128((const __m128i *)(a + 2 * x + 8)), ones);
        s0 = _mm_srai_epi32(_mm_add_epi32(s0, bias), 1);
        s1 = _mm_srai_epi32(_mm_add_epi32(s1, bias), 1);
        _mm_storeu_si128((__m128i *)(out + x), _mm_packs_epi32(s0, s1));
    }
}

/* ------------------------------------------------------------------ */
/*  DCT(libjpeg の jfdctint.c と同じ計算を 8 本同時に)                 */
/* ------------------------------------------------------------------ */

#define CONST_BITS 13
#define PASS1_BITS 2
#define F_0_298  2446
#define F_0_390  3196
#define F_0_541  4433
#define F_0_765  6270
#define F_0_899  7373
#define F_1_175  9633
#define F_1_501 12299
#define F_1_847 15137
#define F_1_961 16069
#define F_2_053 16819
#define F_2_562 20995
#define F_3_072 25172

static __forceinline void transpose8(__m128i *r)
{
    __m128i a0 = _mm_unpacklo_epi16(r[0], r[1]), a1 = _mm_unpackhi_epi16(r[0], r[1]);
    __m128i a2 = _mm_unpacklo_epi16(r[2], r[3]), a3 = _mm_unpackhi_epi16(r[2], r[3]);
    __m128i a4 = _mm_unpacklo_epi16(r[4], r[5]), a5 = _mm_unpackhi_epi16(r[4], r[5]);
    __m128i a6 = _mm_unpacklo_epi16(r[6], r[7]), a7 = _mm_unpackhi_epi16(r[6], r[7]);
    __m128i b0 = _mm_unpacklo_epi32(a0, a2), b1 = _mm_unpackhi_epi32(a0, a2);
    __m128i b2 = _mm_unpacklo_epi32(a1, a3), b3 = _mm_unpackhi_epi32(a1, a3);
    __m128i b4 = _mm_unpacklo_epi32(a4, a6), b5 = _mm_unpackhi_epi32(a4, a6);
    __m128i b6 = _mm_unpacklo_epi32(a5, a7), b7 = _mm_unpackhi_epi32(a5, a7);
    r[0] = _mm_unpacklo_epi64(b0, b4); r[1] = _mm_unpackhi_epi64(b0, b4);
    r[2] = _mm_unpacklo_epi64(b1, b5); r[3] = _mm_unpackhi_epi64(b1, b5);
    r[4] = _mm_unpacklo_epi64(b2, b6); r[5] = _mm_unpackhi_epi64(b2, b6);
    r[6] = _mm_unpacklo_epi64(b3, b7); r[7] = _mm_unpackhi_epi64(b3, b7);
}

/* 32 ビットの組(lo, hi)を 2^n で割って丸め、16 ビットに */
static __forceinline __m128i desc(__m128i lo, __m128i hi, int n)
{
    const __m128i rnd = _mm_set1_epi32(1 << (n - 1));
    return _mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(lo, rnd), n), _mm_srai_epi32(_mm_add_epi32(hi, rnd), n));
}

/* d[0..7] の各列(レーン)ごとに 1 次元の DCT。pass 1 は PASS1_BITS だけ大きく残し、pass 2 で戻す。
 * 掛け算は libjpeg の式を 2 項ずつ pmaddwd にまとめたもの(結果は同じ)。 */
static __forceinline void dct1d(__m128i *d, int pass)
{
    const int n = pass == 1 ? CONST_BITS - PASS1_BITS : CONST_BITS + PASS1_BITS;
    __m128i t0 = _mm_add_epi16(d[0], d[7]), t7 = _mm_sub_epi16(d[0], d[7]);
    __m128i t1 = _mm_add_epi16(d[1], d[6]), t6 = _mm_sub_epi16(d[1], d[6]);
    __m128i t2 = _mm_add_epi16(d[2], d[5]), t5 = _mm_sub_epi16(d[2], d[5]);
    __m128i t3 = _mm_add_epi16(d[3], d[4]), t4 = _mm_sub_epi16(d[3], d[4]);
    __m128i t10 = _mm_add_epi16(t0, t3), t13 = _mm_sub_epi16(t0, t3);
    __m128i t11 = _mm_add_epi16(t1, t2), t12 = _mm_sub_epi16(t1, t2);
    __m128i lo, hi, z3L, z3H, z4L, z4H, k;

    if (pass == 1) {
        d[0] = _mm_slli_epi16(_mm_add_epi16(t10, t11), PASS1_BITS);
        d[4] = _mm_slli_epi16(_mm_sub_epi16(t10, t11), PASS1_BITS);
    } else {
        const __m128i two = _mm_set1_epi16(1 << (PASS1_BITS - 1));
        d[0] = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(t10, t11), two), PASS1_BITS);
        d[4] = _mm_srai_epi16(_mm_add_epi16(_mm_sub_epi16(t10, t11), two), PASS1_BITS);
    }
    /* 偶数: z1 = (t12 + t13) * 0.541 */
    lo = _mm_unpacklo_epi16(t13, t12); hi = _mm_unpackhi_epi16(t13, t12);
    k = PAIR(F_0_541 + F_0_765, F_0_541);
    d[2] = desc(_mm_madd_epi16(lo, k), _mm_madd_epi16(hi, k), n);
    k = PAIR(F_0_541, F_0_541 - F_1_847);
    d[6] = desc(_mm_madd_epi16(lo, k), _mm_madd_epi16(hi, k), n);

    /* 奇数: z3 = (t4 + t6)、z4 = (t5 + t7)、z5 = (z3 + z4) * 1.175 */
    {
        __m128i z3 = _mm_add_epi16(t4, t6), z4 = _mm_add_epi16(t5, t7);
        lo = _mm_unpacklo_epi16(z3, z4); hi = _mm_unpackhi_epi16(z3, z4);
        k = PAIR(F_1_175 - F_1_961, F_1_175);
        z3L = _mm_madd_epi16(lo, k); z3H = _mm_madd_epi16(hi, k);
        k = PAIR(F_1_175, F_1_175 - F_0_390);
        z4L = _mm_madd_epi16(lo, k); z4H = _mm_madd_epi16(hi, k);
    }
    lo = _mm_unpacklo_epi16(t4, t7); hi = _mm_unpackhi_epi16(t4, t7);
    k = PAIR(F_0_298 - F_0_899, -F_0_899);
    d[7] = desc(_mm_add_epi32(_mm_madd_epi16(lo, k), z3L), _mm_add_epi32(_mm_madd_epi16(hi, k), z3H), n);
    k = PAIR(-F_0_899, F_1_501 - F_0_899);
    d[1] = desc(_mm_add_epi32(_mm_madd_epi16(lo, k), z4L), _mm_add_epi32(_mm_madd_epi16(hi, k), z4H), n);
    lo = _mm_unpacklo_epi16(t5, t6); hi = _mm_unpackhi_epi16(t5, t6);
    k = PAIR(F_2_053 - F_2_562, -F_2_562);
    d[5] = desc(_mm_add_epi32(_mm_madd_epi16(lo, k), z4L), _mm_add_epi32(_mm_madd_epi16(hi, k), z4H), n);
    k = PAIR(-F_2_562, F_3_072 - F_2_562);
    d[3] = desc(_mm_add_epi32(_mm_madd_epi16(lo, k), z3L), _mm_add_epi32(_mm_madd_epi16(hi, k), z3H), n);
}

static __forceinline int nbits(int v)
{
    unsigned long idx;
    if (v < 0) v = -v;
    if (!v) return 0;
    _BitScanReverse(&idx, (unsigned long)v);
    return (int)idx + 1;
}

/* 平面の 8×8(src から、1 行 stride 個)を DCT・量子化してハフマン符号に */
static void encode_block(JW *w, const short *src, int stride, const unsigned short (*qt)[64],
                         int *lastDc, const HTab *dc, const HTab *ac)
{
    __declspec(align(16)) short t[64], zz[64];
    const __m128i zero = _mm_setzero_si128(), lim = _mm_set1_epi16(1023), nlim = _mm_set1_epi16(-1023);
    __m128i d[8];
    unsigned long long nz = 0;
    unsigned long idx;
    int i, n, diff, dcv = 0, last;

    for (i = 0; i < 8; i++) d[i] = _mm_loadu_si128((const __m128i *)(src + (size_t)i * stride));
    dct1d(d, 1);            /* 縦 */
    transpose8(d);
    dct1d(d, 2);            /* 横。d[k] のレーン m = 係数[m][k] */

    for (i = 0; i < 8; i++) {
        __m128i x = d[i], s = _mm_srai_epi16(x, 15);
        __m128i a = _mm_sub_epi16(_mm_xor_si128(x, s), s);
        a = _mm_add_epi16(a, _mm_loadu_si128((const __m128i *)(qt[1] + i * 8)));
        a = _mm_mulhi_epu16(a, _mm_loadu_si128((const __m128i *)(qt[0] + i * 8)));
        a = _mm_mulhi_epu16(a, _mm_loadu_si128((const __m128i *)(qt[2] + i * 8)));
        a = _mm_sub_epi16(_mm_xor_si128(a, s), s);
        if (!i) dcv = (short)_mm_cvtsi128_si32(a);
        a = _mm_min_epi16(_mm_max_epi16(a, nlim), lim);     /* ベースラインの AC は 10 ビットまで */
        _mm_store_si128((__m128i *)(t + i * 8), a);
    }
    t[0] = (short)dcv;
    for (i = 0; i < 64; i++) zz[i] = t[g_tzig[i]];
    for (i = 0; i < 4; i++) {
        __m128i a = _mm_load_si128((const __m128i *)(zz + i * 16));
        __m128i b = _mm_load_si128((const __m128i *)(zz + i * 16 + 8));
        unsigned bits = (unsigned)_mm_movemask_epi8(_mm_packs_epi16(_mm_cmpeq_epi16(a, zero), _mm_cmpeq_epi16(b, zero)));
        nz |= (unsigned long long)bits << (i * 16);
    }
    nz = ~nz & ~1ull;

    diff = zz[0] - *lastDc;
    *lastDc = zz[0];
    n = nbits(diff);
    jw_put(w, ((unsigned)dc->code[n] << n) | ((unsigned)(diff < 0 ? diff - 1 : diff) & ((1u << n) - 1)), dc->size[n] + n);

    last = 0;
    while (nz) {
        int run, v, sym;
        _BitScanForward64(&idx, nz);
        nz &= nz - 1;
        run = (int)idx - last - 1;
        while (run > 15) { jw_put(w, ac->code[0xF0], ac->size[0xF0]); run -= 16; }
        v = zz[idx];
        n = nbits(v);
        sym = (run << 4) | n;
        jw_put(w, ((unsigned)ac->code[sym] << n) | ((unsigned)(v < 0 ? v - 1 : v) & ((1u << n) - 1)), ac->size[sym] + n);
        last = (int)idx;
    }
    if (last != 63) jw_put(w, ac->code[0], ac->size[0]);
}

/* ------------------------------------------------------------------ */
/*  見出し                                                              */
/* ------------------------------------------------------------------ */

static unsigned char *put16(unsigned char *p, int v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; return p + 2; }

static unsigned char *put_dht(unsigned char *p, int cls, const unsigned char *bits, const unsigned char *val)
{
    int i, n = 0;
    for (i = 1; i <= 16; i++) n += bits[i];
    *p++ = 0xFF; *p++ = 0xC4;
    p = put16(p, 2 + 1 + 16 + n);
    *p++ = (unsigned char)cls;
    memcpy(p, bits + 1, 16); p += 16;
    memcpy(p, val, (size_t)n); p += n;
    return p;
}

static unsigned char *put_headers(const JpegEnc *e, unsigned char *p, int w, int h, int subsamp)
{
    static const unsigned char app0[18] = { 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
    int i, hs = subsamp == JPE_444 ? 1 : 2, vs = subsamp == JPE_420 ? 2 : 1;

    *p++ = 0xFF; *p++ = 0xD8;
    memcpy(p, app0, sizeof(app0)); p += sizeof(app0);

    *p++ = 0xFF; *p++ = 0xDB;
    p = put16(p, 2 + 65 * 2);
    *p++ = 0;
    for (i = 0; i < 64; i++) *p++ = e->qLum[k_zigzag[i]];
    *p++ = 1;
    for (i = 0; i < 64; i++) *p++ = e->qChr[k_zigzag[i]];

    *p++ = 0xFF; *p++ = 0xC0;
    p = put16(p, 8 + 3 * 3);
    *p++ = 8;
    p = put16(p, h);
    p = put16(p, w);
    *p++ = 3;
    *p++ = 1; *p++ = (unsigned char)((hs << 4) | vs); *p++ = 0;
    *p++ = 2; *p++ = 0x11; *p++ = 1;
    *p++ = 3; *p++ = 0x11; *p++ = 1;

    p = put_dht(p, 0x00, k_dcLumBits, k_dcLumVal);
    p = put_dht(p, 0x10, k_acLumBits, k_acLumVal);
    p = put_dht(p, 0x01, k_dcChrBits, k_dcChrVal);
    p = put_dht(p, 0x11, k_acChrBits, k_acChrVal);

    *p++ = 0xFF; *p++ = 0xDA;
    p = put16(p, 6 + 2 * 3);
    *p++ = 3;
    *p++ = 1; *p++ = 0x00;
    *p++ = 2; *p++ = 0x11;
    *p++ = 3; *p++ = 0x11;
    *p++ = 0; *p++ = 63; *p++ = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/*  本体                                                                */
/* ------------------------------------------------------------------ */

size_t jpe_encode(JpegEnc *e, const unsigned char *px, int stride, int w, int h,
                  int quality, int subsamp, unsigned char *out, size_t cap)
{
    int    mw = subsamp == JPE_444 ? 8 : 16, mh = subsamp == JPE_420 ? 16 : 8;
    int    dcY = 0, dcCb = 0, dcCr = 0, mx, my, bx, by, r, W, cw;
    const size_t worstMcu = 6 * 420 + 64;      /* 1 ブロック最大 約 206 バイト、0xFF の詰め物で倍 */
    size_t need;
    short *Y, *Cb, *Cr, *Cb2, *Cr2, *pcb, *pcr;
    JW     jw;

    if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || cap < 1024) return 0;
    W = (w + mw - 1) / mw * mw;
    need = (size_t)W * (size_t)mh * 3 + (size_t)W * 8;
    if (need > e->planeCap) {
        short *p = (short *)realloc(e->plane, need * sizeof(short));
        if (!p) return 0;
        e->plane = p;
        e->planeCap = need;
    }
    Y = e->plane;
    Cb = Y + (size_t)W * mh;
    Cr = Cb + (size_t)W * mh;
    Cb2 = Cr + (size_t)W * mh;
    Cr2 = Cb2 + (size_t)(W / 2) * 8;
    cw = subsamp == JPE_444 ? W : W / 2;
    pcb = subsamp == JPE_444 ? Cb : Cb2;
    pcr = subsamp == JPE_444 ? Cr : Cr2;

    set_quality(e, quality);
    jw.p = put_headers(e, out, w, h, subsamp);
    jw.buf = 0;
    jw.cnt = 0;

    for (my = 0; my < h; my += mh) {
        convert_rows(px, stride, w, h, my, mh, W, Y, Cb, Cr);
        if (subsamp == JPE_420) {
            for (r = 0; r < 8; r++) {
                down_h2v2(Cb + (size_t)(2 * r) * W, Cb + (size_t)(2 * r + 1) * W, Cb2 + (size_t)r * cw, cw);
                down_h2v2(Cr + (size_t)(2 * r) * W, Cr + (size_t)(2 * r + 1) * W, Cr2 + (size_t)r * cw, cw);
            }
        } else if (subsamp == JPE_422) {
            for (r = 0; r < 8; r++) {
                down_h2v1(Cb + (size_t)r * W, Cb2 + (size_t)r * cw, cw);
                down_h2v1(Cr + (size_t)r * W, Cr2 + (size_t)r * cw, cw);
            }
        }
        for (mx = 0; mx < W; mx += mw) {
            int cx = subsamp == JPE_444 ? mx : mx / 2;
            if ((size_t)(jw.p - out) + worstMcu > cap) return 0;
            for (by = 0; by < mh; by += 8)
                for (bx = 0; bx < mw; bx += 8)
                    encode_block(&jw, Y + (size_t)by * W + mx + bx, W, e->qt[0], &dcY, &g_dcLum, &g_acLum);
            encode_block(&jw, pcb + cx, cw, e->qt[1], &dcCb, &g_dcChr, &g_acChr);
            encode_block(&jw, pcr + cx, cw, e->qt[1], &dcCr, &g_dcChr, &g_acChr);
        }
    }
    {
        int pad = (8 - (jw.cnt & 7)) & 7;      /* 端数のビットを 1 で埋める */
        if (pad) jw_put(&jw, (1u << pad) - 1, pad);
        while (jw.cnt >= 8) {
            unsigned char c = (unsigned char)(jw.buf >> (jw.cnt - 8));
            *jw.p++ = c;
            if (c == 0xFF) *jw.p++ = 0;
            jw.cnt -= 8;
        }
    }
    *jw.p++ = 0xFF;
    *jw.p++ = 0xD9;
    return (size_t)(jw.p - out);
}
