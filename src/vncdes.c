/* ==================================================================
 * vncdes.c - VNC 認証の DES
 *
 *  iivnc-server と iivnc-client で同じファイルを使う(中身をそろえる)。
 *
 *  VNC 認証は 16 バイトの挑戦を、パスワード(8 バイトまで、足りない分は 0)を
 *  鍵にした DES(ECB)で 8 バイトずつ暗号化して返す。VNC の d3des は鍵の
 *  各バイトのビットを下位から読むので、標準の DES で言えば鍵の各バイトを
 *  ビット反転したものになる。
 *
 *  ini に書くパスワードは、ほかの VNC と同じく固定の鍵
 *  {23,82,107,6,35,78,88,7} で暗号化して 16 進で置く(隠すだけで、守りではない)。
 *
 *  速さは要らないので、表の順にビットを並べ替える素直な書き方にしている。
 * ================================================================== */

#include "vncdes.h"
#include <string.h>

typedef unsigned long long u64;

static const unsigned char k_ip[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const unsigned char k_fp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
static const unsigned char k_e[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
static const unsigned char k_p[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
static const unsigned char k_pc1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
static const unsigned char k_pc2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const unsigned char k_shift[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const unsigned char k_s[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
      13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
      10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 } };

/* in の上位から inBits ビットを、表(1 始まり)の順に並べ替える */
static u64 permute(u64 in, int inBits, const unsigned char *tab, int n)
{
    u64 out = 0;
    int i;
    for (i = 0; i < n; i++) out = (out << 1) | ((in >> (inBits - tab[i])) & 1);
    return out;
}

static u64 load64(const unsigned char *p)
{
    u64 v = 0;
    int i;
    for (i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void store64(unsigned char *p, u64 v)
{
    int i;
    for (i = 7; i >= 0; i--) { p[i] = (unsigned char)v; v >>= 8; }
}

static void des_block(const unsigned char key[8], const unsigned char in[8], unsigned char out[8], int decrypt)
{
    u64 sub[16], cd, b, l, r;
    unsigned c, d;
    int i, j;

    cd = permute(load64(key), 64, k_pc1, 56);
    c = (unsigned)(cd >> 28) & 0xFFFFFFF;
    d = (unsigned)cd & 0xFFFFFFF;
    for (i = 0; i < 16; i++) {
        for (j = 0; j < k_shift[i]; j++) {
            c = ((c << 1) | (c >> 27)) & 0xFFFFFFF;
            d = ((d << 1) | (d >> 27)) & 0xFFFFFFF;
        }
        sub[i] = permute(((u64)c << 28) | d, 56, k_pc2, 48);
    }

    b = permute(load64(in), 64, k_ip, 64);
    l = b >> 32;
    r = b & 0xFFFFFFFF;
    for (i = 0; i < 16; i++) {
        u64 x = permute(r, 32, k_e, 48) ^ sub[decrypt ? 15 - i : i], f = 0, t;
        for (j = 0; j < 8; j++) {
            unsigned six = (unsigned)(x >> (42 - 6 * j)) & 63;
            unsigned row = ((six >> 4) & 2) | (six & 1), col = (six >> 1) & 15;
            f = (f << 4) | k_s[j][row * 16 + col];
        }
        f = permute(f, 32, k_p, 32);
        t = r;
        r = l ^ f;
        l = t;
    }
    store64(out, permute((r << 32) | l, 64, k_fp, 64));
}

static void vnc_key(const unsigned char *src, unsigned char key[8])
{
    int i, k;
    for (i = 0; i < 8; i++) {
        unsigned char v = src[i], r = 0;
        for (k = 0; k < 8; k++) if (v & (1 << k)) r |= (unsigned char)(0x80 >> k);
        key[i] = r;
    }
}

void vncdes_response(const char *password, const unsigned char challenge[16], unsigned char response[16])
{
    unsigned char pw[8] = { 0 }, key[8];
    size_t n = strlen(password);
    memcpy(pw, password, n > 8 ? 8 : n);
    vnc_key(pw, key);
    des_block(key, challenge, response, 0);
    des_block(key, challenge + 8, response + 8, 0);
}

static const unsigned char k_fixed[8] = { 23, 82, 107, 6, 35, 78, 88, 7 };

void vncdes_obfuscate(const char *password, unsigned char out[8])
{
    unsigned char pw[8] = { 0 }, key[8];
    size_t n = strlen(password);
    memcpy(pw, password, n > 8 ? 8 : n);
    vnc_key(k_fixed, key);
    des_block(key, pw, out, 0);
}

void vncdes_reveal(const unsigned char in[8], char password[9])
{
    unsigned char key[8];
    vnc_key(k_fixed, key);
    des_block(key, in, (unsigned char *)password, 1);
    password[8] = 0;
}

void vncdes_test_block(const unsigned char key[8], const unsigned char in[8], unsigned char out[8])
{
    des_block(key, in, out, 0);
}
