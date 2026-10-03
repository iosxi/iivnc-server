/* ==================================================================
 * jpegenc.c - ベースライン JPEG の符号化
 *
 *  Tight の JPEG 矩形は、受け手が libjpeg などでそのまま読める
 *  完全な JPEG(JFIF)でなければならない。量子化表は IJG の品質の
 *  換算、ハフマン表は規格の付録 K の標準の表を使う。
 *
 *  DCT は AAN の浮動小数点版(libjpeg の jfdctflt と同じ)。
 *  量子化は AAN の倍率をかけた除数の逆数をかけて丸める。
 * ================================================================== */

#include "jpegenc.h"
#include <string.h>
#include <stdlib.h>
#include <intrin.h>

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

struct JpegEnc {
    int           quality;
    unsigned char qLum[64], qChr[64];   /* 自然順 */
    float         dLum[64], dChr[64];   /* 除数の逆数(自然順) */
};

JpegEnc *jpe_new(void)
{
    JpegEnc *e = (JpegEnc *)calloc(1, sizeof(JpegEnc));
    init_tables();
    return e;
}

void jpe_free(JpegEnc *e) { free(e); }

size_t jpe_bound(int w, int h)
{
    size_t blocks = (size_t)((w + 15) / 16) * (size_t)((h + 15) / 16) * 12;
    return blocks * 420 + 1024;
}

static void set_quality(JpegEnc *e, int q)
{
    static const float aan[8] = { 1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
                                  1.0f, 0.785694958f, 0.541196100f, 0.275899379f };
    int i, scale;
    if (q < 1) q = 1;
    if (q > 100) q = 100;
    if (e->quality == q) return;
    e->quality = q;
    scale = q < 50 ? 5000 / q : 200 - q * 2;
    for (i = 0; i < 64; i++) {
        int a = (k_lumQ[i] * scale + 50) / 100, b = (k_chrQ[i] * scale + 50) / 100;
        e->qLum[i] = (unsigned char)(a < 1 ? 1 : a > 255 ? 255 : a);
        e->qChr[i] = (unsigned char)(b < 1 ? 1 : b > 255 ? 255 : b);
        e->dLum[i] = 1.0f / ((float)e->qLum[i] * aan[i >> 3] * aan[i & 7] * 8.0f);
        e->dChr[i] = 1.0f / ((float)e->qChr[i] * aan[i >> 3] * aan[i & 7] * 8.0f);
    }
}

/* ------------------------------------------------------------------ */
/*  ビットの書き出し                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char     *p;
    unsigned long long buf;
    int                cnt;
} JW;

static __forceinline void jw_put(JW *w, unsigned code, int size)
{
    w->buf = (w->buf << size) | code;
    w->cnt += size;
    while (w->cnt >= 8) {
        unsigned char c = (unsigned char)(w->buf >> (w->cnt - 8));
        *w->p++ = c;
        if (c == 0xFF) *w->p++ = 0;
        w->cnt -= 8;
    }
}

/* ------------------------------------------------------------------ */
/*  DCT と符号化                                                        */
/* ------------------------------------------------------------------ */

static void fdct(float *d)
{
    float t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13, z1, z2, z3, z4, z5, z11, z13;
    float *p;
    int i;

    for (p = d, i = 0; i < 8; i++, p += 8) {
        t0 = p[0] + p[7]; t7 = p[0] - p[7];
        t1 = p[1] + p[6]; t6 = p[1] - p[6];
        t2 = p[2] + p[5]; t5 = p[2] - p[5];
        t3 = p[3] + p[4]; t4 = p[3] - p[4];
        t10 = t0 + t3; t13 = t0 - t3; t11 = t1 + t2; t12 = t1 - t2;
        p[0] = t10 + t11; p[4] = t10 - t11;
        z1 = (t12 + t13) * 0.707106781f;
        p[2] = t13 + z1; p[6] = t13 - z1;
        t10 = t4 + t5; t11 = t5 + t6; t12 = t6 + t7;
        z5 = (t10 - t12) * 0.382683433f;
        z2 = 0.541196100f * t10 + z5;
        z4 = 1.306562965f * t12 + z5;
        z3 = t11 * 0.707106781f;
        z11 = t7 + z3; z13 = t7 - z3;
        p[5] = z13 + z2; p[3] = z13 - z2;
        p[1] = z11 + z4; p[7] = z11 - z4;
    }
    for (p = d, i = 0; i < 8; i++, p++) {
        t0 = p[0] + p[56]; t7 = p[0] - p[56];
        t1 = p[8] + p[48]; t6 = p[8] - p[48];
        t2 = p[16] + p[40]; t5 = p[16] - p[40];
        t3 = p[24] + p[32]; t4 = p[24] - p[32];
        t10 = t0 + t3; t13 = t0 - t3; t11 = t1 + t2; t12 = t1 - t2;
        p[0] = t10 + t11; p[32] = t10 - t11;
        z1 = (t12 + t13) * 0.707106781f;
        p[16] = t13 + z1; p[48] = t13 - z1;
        t10 = t4 + t5; t11 = t5 + t6; t12 = t6 + t7;
        z5 = (t10 - t12) * 0.382683433f;
        z2 = 0.541196100f * t10 + z5;
        z4 = 1.306562965f * t12 + z5;
        z3 = t11 * 0.707106781f;
        z11 = t7 + z3; z13 = t7 - z3;
        p[40] = z13 + z2; p[24] = z13 - z2;
        p[8] = z11 + z4; p[56] = z11 - z4;
    }
}

static __forceinline int nbits(int v)
{
    unsigned long idx;
    if (v < 0) v = -v;
    if (!v) return 0;
    _BitScanReverse(&idx, (unsigned long)v);
    return (int)idx + 1;
}

static void encode_block(JW *w, float *blk, const float *div, int *lastDc, const HTab *dc, const HTab *ac)
{
    int q[64], i, run, diff, n;

    fdct(blk);
    for (i = 0; i < 64; i++) {
        float v = blk[k_zigzag[i]] * div[k_zigzag[i]];
        q[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    /* ベースラインの AC は 10 ビットまで */
    for (i = 1; i < 64; i++) q[i] = q[i] > 1023 ? 1023 : q[i] < -1023 ? -1023 : q[i];
    diff = q[0] - *lastDc;
    *lastDc = q[0];
    n = nbits(diff);
    jw_put(w, dc->code[n], dc->size[n]);
    if (n) jw_put(w, (unsigned)(diff < 0 ? diff - 1 : diff) & ((1u << n) - 1), n);

    run = 0;
    for (i = 1; i < 64; i++) {
        int v = q[i];
        if (!v) { run++; continue; }
        while (run > 15) { jw_put(w, ac->code[0xF0], ac->size[0xF0]); run -= 16; }
        n = nbits(v);
        jw_put(w, ac->code[(run << 4) | n], ac->size[(run << 4) | n]);
        jw_put(w, (unsigned)(v < 0 ? v - 1 : v) & ((1u << n) - 1), n);
        run = 0;
    }
    if (run) jw_put(w, ac->code[0], ac->size[0]);
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

/* MCU の範囲(端は最後の画素を繰り返す)を Y / Cb / Cr の平面に取り出す */
static void load_mcu(const unsigned char *px, int stride, int w, int h, int x0, int y0, int mw, int mh,
                     float *Y, float *Cb, float *Cr)
{
    int x, y;
    for (y = 0; y < mh; y++) {
        int sy = y0 + y < h ? y0 + y : h - 1;
        const unsigned char *row = px + (size_t)sy * (size_t)stride;
        for (x = 0; x < mw; x++) {
            int sx = x0 + x < w ? x0 + x : w - 1;
            const unsigned char *s = row + sx * 4;
            float b = s[0], g = s[1], r = s[2];
            int i = y * mw + x;
            Y[i]  =  0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
            Cb[i] = -0.168735892f * r - 0.331264108f * g + 0.5f * b;
            Cr[i] =  0.5f * r - 0.418687589f * g - 0.081312411f * b;
        }
    }
}

size_t jpe_encode(JpegEnc *e, const unsigned char *px, int stride, int w, int h,
                  int quality, int subsamp, unsigned char *out, size_t cap)
{
    float Y[256], Cb[256], Cr[256], blk[64];
    int   mw = subsamp == JPE_444 ? 8 : 16, mh = subsamp == JPE_420 ? 16 : 8;
    int   dcY = 0, dcCb = 0, dcCr = 0, mx, my, bx, by, x, y;
    const size_t worstMcu = 6 * 420 + 64;      /* 1 ブロック最大 約 206 バイト、0xFF の詰め物で倍 */
    JW    jw;

    if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || cap < 1024) return 0;
    set_quality(e, quality);
    jw.p = put_headers(e, out, w, h, subsamp);
    jw.buf = 0;
    jw.cnt = 0;

    for (my = 0; my < h; my += mh) {
        for (mx = 0; mx < w; mx += mw) {
            if ((size_t)(jw.p - out) + worstMcu > cap) return 0;
            load_mcu(px, stride, w, h, mx, my, mw, mh, Y, Cb, Cr);
            for (by = 0; by < mh; by += 8)
                for (bx = 0; bx < mw; bx += 8) {
                    for (y = 0; y < 8; y++) memcpy(blk + y * 8, Y + (by + y) * mw + bx, 8 * sizeof(float));
                    encode_block(&jw, blk, e->dLum, &dcY, &g_dcLum, &g_acLum);
                }
            if (subsamp == JPE_444) {
                memcpy(blk, Cb, sizeof(blk));
                encode_block(&jw, blk, e->dChr, &dcCb, &g_dcChr, &g_acChr);
                memcpy(blk, Cr, sizeof(blk));
                encode_block(&jw, blk, e->dChr, &dcCr, &g_dcChr, &g_acChr);
            } else {
                float cr[64];
                for (y = 0; y < 8; y++)
                    for (x = 0; x < 8; x++) {
                        if (subsamp == JPE_420) {
                            int i = (y * 2) * 16 + x * 2;
                            blk[y * 8 + x] = (Cb[i] + Cb[i + 1] + Cb[i + 16] + Cb[i + 17]) * 0.25f;
                            cr[y * 8 + x]  = (Cr[i] + Cr[i + 1] + Cr[i + 16] + Cr[i + 17]) * 0.25f;
                        } else {
                            int i = y * 16 + x * 2;
                            blk[y * 8 + x] = (Cb[i] + Cb[i + 1]) * 0.5f;
                            cr[y * 8 + x]  = (Cr[i] + Cr[i + 1]) * 0.5f;
                        }
                    }
                encode_block(&jw, blk, e->dChr, &dcCb, &g_dcChr, &g_acChr);
                encode_block(&jw, cr, e->dChr, &dcCr, &g_dcChr, &g_acChr);
            }
        }
    }
    if (jw.cnt & 7) {                   /* 端数のビットを 1 で埋める(幅ちょうどの値で) */
        int pad = 8 - (jw.cnt & 7);
        jw_put(&jw, (1u << pad) - 1, pad);
    }
    *jw.p++ = 0xFF;
    *jw.p++ = 0xD9;
    return (size_t)(jw.p - out);
}
