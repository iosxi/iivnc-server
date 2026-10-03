/* ==================================================================
 * encode.c - 画素形式の変換と符号化(Raw / CopyRect / ZRLE / Tight)
 *
 *  1 回の更新は次のように進む。
 *
 *   1. 変わった矩形を「単位」に切る(Tight は 128×128、ZRLE と Raw は 256×256)。
 *      単位 1 つが RFB の矩形 1 つになる。
 *   2. 一段目(並列): 単位ごとに色を数えて種類を決め、zlib に通す前の
 *      バイト列を作る。Tight の塗りつぶしや Raw はここで出来上がる。
 *   3. Tight で JPEG にする単位は、横に続くものをつなげて 1 枚にする
 *      (JPEG の見出しは 600 バイトほどあるので、細かいと割高)。
 *   4. 二段目(並列): zlib(各ストリームの直前 32KB を辞書にする)か JPEG。
 *      同じストリームの前の単位の結果を待たずに圧縮できる(zlite.h)。
 *   5. 送り手は単位を番号順に、出来たものから送る。
 *
 *  Tight のストリームは、フルカラー = 0、2 色 = 1、パレット = 2。
 *  JPEG は画質が指定されたとき(相手が画質の疑似エンコーディングを
 *  送ってきたとき)だけ使い、色が多い単位に使う。
 * ================================================================== */

#include "iivnc.h"
#include "jpegenc.h"

#define TIGHT_TILE  128
#define BIG_TILE    256
#define JPEG_MAXW   1024

enum { U_DONE, U_FILL, U_MONO, U_PALETTE, U_FULL, U_JPEG, U_ZRLE, U_SKIP };

const PixFmt k_nativePf = { 32, 24, 0, 1, 255, 255, 255, 16, 8, 0 };

typedef struct Unit {
    int    x, y, w, h;
    int    kind;
    int    stream;              /* zlib のストリーム(-1 = 使わない) */
    int    prevSame;            /* 同じストリームの前の単位(-1 = 無し) */
    int    header;              /* ストリームの最初(zlib の見出しを付ける) */
    int    ncolors;
    DWORD  pal[256];
    BYTE  *rawBase;             /* zlib に通す前のバイト列 */
    size_t rawCap, rawLen;
    Buf    out;
    LONG64 tEnd;                /* 二段目が終わった時刻(QPC) */
} Unit;

typedef struct EncState {
    Unit          *units;
    int            nunits, cap;
    volatile LONG *done;
    int            doneCap;
    /* 1 回の更新の間だけ使う */
    Client        *c;
    PixFmt         pf;
    int            enc, jpegQ, subsamp, zlevel;
    BOOL           tpixel;      /* Tight の 3 バイトの画素(R,G,B)を使える */
    int            cpixel;      /* ZRLE の画素のバイト数 */
    BYTE          *scratch[64]; /* 作業領域ごとの圧縮の出力 */
    size_t         scratchCap[64];
    BYTE          *dictBuf[64]; /* 作業領域ごとの「辞書 + 元のデータ」 */
    size_t         dictCap[64];
} EncState;

/* ------------------------------------------------------------------ */
/*  Buf                                                                 */
/* ------------------------------------------------------------------ */

void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra > b->cap) {
        size_t cap = (b->len + extra) * 2 + 256;
        BYTE *p = (BYTE *)realloc(b->p, cap);
        if (!p) return;
        b->p = p;
        b->cap = cap;
    }
}
void buf_put(Buf *b, const void *data, size_t n) { buf_reserve(b, n); memcpy(b->p + b->len, data, n); b->len += n; }
void buf_u8(Buf *b, int v)  { buf_reserve(b, 1); b->p[b->len++] = (BYTE)v; }
void buf_u16(Buf *b, int v) { buf_reserve(b, 2); b->p[b->len++] = (BYTE)(v >> 8); b->p[b->len++] = (BYTE)v; }
void buf_u32(Buf *b, unsigned v)
{
    buf_reserve(b, 4);
    b->p[b->len++] = (BYTE)(v >> 24); b->p[b->len++] = (BYTE)(v >> 16);
    b->p[b->len++] = (BYTE)(v >> 8);  b->p[b->len++] = (BYTE)v;
}
void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

static void rect_header(Buf *b, int x, int y, int w, int h, int enc)
{
    buf_u16(b, x); buf_u16(b, y); buf_u16(b, w); buf_u16(b, h);
    buf_u32(b, (unsigned)enc);
}

/* ------------------------------------------------------------------ */
/*  画素形式                                                            */
/* ------------------------------------------------------------------ */

BOOL pf_is_native(const PixFmt *pf)
{
    return pf->bpp == 32 && pf->trueColor && !pf->bigEndian && pf->rmax == 255 && pf->gmax == 255 &&
           pf->bmax == 255 && pf->rshift == 16 && pf->gshift == 8 && pf->bshift == 0;
}

int pf_bytes(const PixFmt *pf) { return pf->bpp / 8; }

static __forceinline unsigned pix_value(const PixFmt *pf, const BYTE *s)
{
    unsigned r = s[2], g = s[1], b = s[0];
    return (((r * (unsigned)pf->rmax + 127) / 255) << pf->rshift) |
           (((g * (unsigned)pf->gmax + 127) / 255) << pf->gshift) |
           (((b * (unsigned)pf->bmax + 127) / 255) << pf->bshift);
}

static __forceinline void put_value(const PixFmt *pf, unsigned v, BYTE *o)
{
    switch (pf->bpp) {
    case 8:  o[0] = (BYTE)v; break;
    case 16:
        if (pf->bigEndian) { o[0] = (BYTE)(v >> 8); o[1] = (BYTE)v; }
        else { o[0] = (BYTE)v; o[1] = (BYTE)(v >> 8); }
        break;
    default:
        if (pf->bigEndian) { o[0] = (BYTE)(v >> 24); o[1] = (BYTE)(v >> 16); o[2] = (BYTE)(v >> 8); o[3] = (BYTE)v; }
        else { o[0] = (BYTE)v; o[1] = (BYTE)(v >> 8); o[2] = (BYTE)(v >> 16); o[3] = (BYTE)(v >> 24); }
    }
}

void pf_put_pixels(const PixFmt *pf, const BYTE *bgrx, int n, BYTE *out)
{
    int i, bpp = pf->bpp / 8;
    if (pf_is_native(pf)) {
        for (i = 0; i < n; i++) {
            out[i * 4 + 0] = bgrx[i * 4 + 0]; out[i * 4 + 1] = bgrx[i * 4 + 1];
            out[i * 4 + 2] = bgrx[i * 4 + 2]; out[i * 4 + 3] = 0;
        }
        return;
    }
    for (i = 0; i < n; i++) put_value(pf, pix_value(pf, bgrx + i * 4), out + i * bpp);
}

/* ZRLE の CPIXEL: 32bpp で 3 バイトに収まるなら 3 バイト */
static int cpixel_size(const PixFmt *pf)
{
    if (pf->trueColor && pf->bpp == 32 && pf->depth <= 24) {
        unsigned all = ((unsigned)pf->rmax << pf->rshift) | ((unsigned)pf->gmax << pf->gshift) | ((unsigned)pf->bmax << pf->bshift);
        if (all < (1u << 24) || !(all & 0xFF)) return 3;
    }
    return pf->bpp / 8;
}

static __forceinline void put_cpixel(const EncState *st, const BYTE *s, BYTE *o)
{
    const PixFmt *pf = &st->pf;
    unsigned v;
    if (st->cpixel != 3) { put_value(pf, pix_value(pf, s), o); return; }
    if (pf_is_native(pf)) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; return; }
    v = pix_value(pf, s);
    {
        unsigned all = ((unsigned)pf->rmax << pf->rshift) | ((unsigned)pf->gmax << pf->gshift) | ((unsigned)pf->bmax << pf->bshift);
        if (all >= (1u << 24)) v >>= 8;         /* 上位 3 バイトに入っている */
    }
    if (pf->bigEndian) { o[0] = (BYTE)(v >> 16); o[1] = (BYTE)(v >> 8); o[2] = (BYTE)v; }
    else { o[0] = (BYTE)v; o[1] = (BYTE)(v >> 8); o[2] = (BYTE)(v >> 16); }
}

/* ------------------------------------------------------------------ */
/*  色を数える                                                          */
/* ------------------------------------------------------------------ */

#define HSZ 1024

typedef struct {
    DWORD key[HSZ];
    short idx[HSZ];             /* -1 = 空き */
} ColorHash;

/* 色の数を数える(max を超えたら max+1 を返す)。pal に並べる。 */
static int count_colors(const BYTE *fb, int stride, int x, int y, int w, int h, int max, DWORD *pal, ColorHash *ch)
{
    int   n = 0, i, j;
    DWORD last = 0xFFFFFFFF;
    memset(ch->idx, -1, sizeof(ch->idx));
    for (j = 0; j < h; j++) {
        const DWORD *row = (const DWORD *)(fb + (size_t)(y + j) * stride) + x;
        for (i = 0; i < w; i++) {
            DWORD c = row[i] & 0xFFFFFF;
            unsigned hh;
            if (c == last) continue;
            last = c;
            hh = (c * 2654435761u) >> 22;
            for (;;) {
                if (ch->idx[hh] < 0) {
                    if (n >= max) return max + 1;
                    ch->idx[hh] = (short)n;
                    ch->key[hh] = c;
                    pal[n++] = c;
                    break;
                }
                if (ch->key[hh] == c) break;
                hh = (hh + 1) & (HSZ - 1);
            }
        }
    }
    return n;
}

static __forceinline int color_index(const ColorHash *ch, DWORD c)
{
    unsigned hh = (c * 2654435761u) >> 22;
    while (ch->key[hh] != c || ch->idx[hh] < 0) hh = (hh + 1) & (HSZ - 1);
    return ch->idx[hh];
}

/* ------------------------------------------------------------------ */
/*  単位                                                                */
/* ------------------------------------------------------------------ */

static BYTE *unit_raw(Unit *u, size_t need)
{
    if (need + 16 > u->rawCap) {
        free(u->rawBase);
        u->rawCap = need + 16;
        u->rawBase = (BYTE *)malloc(u->rawCap);
        if (!u->rawBase) u->rawCap = 0;
    }
    return u->rawBase;
}

static void tpixel(const DWORD c, BYTE *o)
{
    o[0] = (BYTE)(c >> 16); o[1] = (BYTE)(c >> 8); o[2] = (BYTE)c;
}

static void compact_len(Buf *b, size_t len)
{
    buf_u8(b, (int)((len & 0x7F) | (len >= 128 ? 0x80 : 0)));
    if (len >= 128) {
        buf_u8(b, (int)(((len >> 7) & 0x7F) | (len >= 16384 ? 0x80 : 0)));
        if (len >= 16384) buf_u8(b, (int)((len >> 14) & 0xFF));
    }
}

/* --- Tight の一段目 --- */
static void tight_analyze(EncState *st, Unit *u, const BYTE *fb, int stride)
{
    ColorHash ch;
    int max = st->jpegQ >= 0 ? 24 : 256, n, i, j;
    int npix = u->w * u->h;

    u->out.len = 0;
    u->stream = -1;
    if (max > npix / 2) max = npix / 2 < 2 ? 2 : npix / 2;
    n = count_colors(fb, stride, u->x, u->y, u->w, u->h, max, u->pal, &ch);
    u->ncolors = n;

    if (n == 1) {
        BYTE t[3];
        u->kind = U_FILL;
        rect_header(&u->out, u->x, u->y, u->w, u->h, ENC_TIGHT);
        buf_u8(&u->out, 0x80);
        tpixel(u->pal[0], t);
        buf_put(&u->out, t, 3);
        return;
    }
    if (n <= max) {
        BYTE *d;
        if (n == 2) {
            int rowBytes = (u->w + 7) / 8;
            u->kind = U_MONO;
            u->stream = 1;
            d = unit_raw(u, (size_t)rowBytes * u->h);
            if (!d) { u->kind = U_SKIP; return; }
            memset(d, 0, (size_t)rowBytes * u->h);
            for (j = 0; j < u->h; j++) {
                const DWORD *row = (const DWORD *)(fb + (size_t)(u->y + j) * stride) + u->x;
                for (i = 0; i < u->w; i++)
                    if ((row[i] & 0xFFFFFF) != u->pal[0]) d[j * rowBytes + i / 8] |= (BYTE)(0x80 >> (i & 7));
            }
            u->rawLen = (size_t)rowBytes * u->h;
        } else {
            u->kind = U_PALETTE;
            u->stream = 2;
            d = unit_raw(u, (size_t)npix);
            if (!d) { u->kind = U_SKIP; return; }
            for (j = 0; j < u->h; j++) {
                const DWORD *row = (const DWORD *)(fb + (size_t)(u->y + j) * stride) + u->x;
                DWORD last = 0xFFFFFFFF;
                int   li = 0;
                for (i = 0; i < u->w; i++) {
                    DWORD c = row[i] & 0xFFFFFF;
                    if (c != last) { last = c; li = color_index(&ch, c); }
                    d[j * u->w + i] = (BYTE)li;
                }
            }
            u->rawLen = (size_t)npix;
        }
        return;
    }
    if (st->jpegQ >= 0) {
        u->kind = U_JPEG;
        return;
    }
    {
        BYTE *d = unit_raw(u, (size_t)npix * 3);
        u->kind = U_FULL;
        u->stream = 0;
        if (!d) { u->kind = U_SKIP; return; }
        for (j = 0; j < u->h; j++) {
            const BYTE *s = fb + (size_t)(u->y + j) * stride + (size_t)u->x * 4;
            for (i = 0; i < u->w; i++, s += 4, d += 3) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
        }
        u->rawLen = (size_t)npix * 3;
    }
}

/* --- ZRLE の一段目: 64×64 のタイルを並べたバイト列 --- */
static void zrle_tile(EncState *st, const BYTE *fb, int stride, int x, int y, int w, int h, Buf *o)
{
    ColorHash ch;
    DWORD pal[128];
    int   cp = st->cpixel, npix = w * h, i, j, n, runs = 0, singles = 0;
    size_t est, rleBytes, palRle, packed;
    int   usePal = 0, useRle = 0;
    DWORD prev = 0xFFFFFFFF;
    int   runLen = 0;

    n = count_colors(fb, stride, x, y, w, h, 127, pal, &ch);
    if (n == 1) {
        BYTE t[4];
        buf_u8(o, 1);
        put_cpixel(st, fb + (size_t)y * stride + (size_t)x * 4, t);
        buf_put(o, t, (size_t)cp);
        return;
    }
    /* 連の数(行をまたいで続く) */
    for (j = 0; j < h; j++) {
        const DWORD *row = (const DWORD *)(fb + (size_t)(y + j) * stride) + x;
        for (i = 0; i < w; i++) {
            DWORD c = row[i] & 0xFFFFFF;
            if (c == prev) { runLen++; continue; }
            if (runLen == 1) singles++; else if (runLen > 1) runs++;
            prev = c;
            runLen = 1;
        }
    }
    if (runLen == 1) singles++; else if (runLen > 1) runs++;

    est = (size_t)npix * cp;
    rleBytes = (size_t)(cp + 1) * (runs + singles);
    if (rleBytes < est) { useRle = 1; est = rleBytes; }
    if (n <= 127) {
        palRle = (size_t)cp * n + 2 * (size_t)runs + singles;
        if (palRle < est) { useRle = 1; usePal = 1; est = palRle; }
        if (n <= 16) {
            int bits = n <= 2 ? 1 : n <= 4 ? 2 : 4;
            packed = (size_t)cp * n + (size_t)((w * bits + 7) / 8) * h;
            if (packed < est) { useRle = 0; usePal = 1; }
        }
    }

    if (!usePal && !useRle) {                       /* 生 */
        buf_u8(o, 0);
        buf_reserve(o, (size_t)npix * cp);
        for (j = 0; j < h; j++) {
            const BYTE *s = fb + (size_t)(y + j) * stride + (size_t)x * 4;
            for (i = 0; i < w; i++, s += 4) { put_cpixel(st, s, o->p + o->len); o->len += (size_t)cp; }
        }
        return;
    }
    if (usePal) {
        BYTE t[4];
        buf_u8(o, useRle ? 128 + n : n);
        for (i = 0; i < n; i++) {
            BYTE px[4];
            px[0] = (BYTE)pal[i]; px[1] = (BYTE)(pal[i] >> 8); px[2] = (BYTE)(pal[i] >> 16); px[3] = 255;
            put_cpixel(st, px, t);
            buf_put(o, t, (size_t)cp);
        }
    } else {
        buf_u8(o, 128);
    }
    if (usePal && !useRle) {                        /* 詰めたパレット */
        int bits = n <= 2 ? 1 : n <= 4 ? 2 : 4, rb = (w * bits + 7) / 8;
        buf_reserve(o, (size_t)rb * h);
        for (j = 0; j < h; j++) {
            const DWORD *row = (const DWORD *)(fb + (size_t)(y + j) * stride) + x;
            BYTE *d = o->p + o->len;
            int   acc = 0, nb = 0;
            memset(d, 0, (size_t)rb);
            for (i = 0; i < w; i++) {
                acc = (acc << bits) | color_index(&ch, row[i] & 0xFFFFFF);
                nb += bits;
                if (nb == 8) { *d++ = (BYTE)acc; acc = 0; nb = 0; }
            }
            if (nb) *d = (BYTE)(acc << (8 - nb));
            o->len += (size_t)rb;
        }
        return;
    }
    /* RLE(パレットあり / なし)。連は行をまたいで続く */
    {
        const BYTE *runPx = NULL;
        DWORD cur = 0;
        int   len = 0;
        for (j = 0; j <= h; j++) {
            const DWORD *row = (const DWORD *)(fb + (size_t)(y + (j < h ? j : 0)) * stride) + x;
            for (i = 0; i < w; i++) {
                DWORD c = j < h ? (row[i] & 0xFFFFFF) : 0xFFFFFFFF;
                if (len && c == cur) { len++; continue; }
                if (len) {
                    int rem = len - 1;
                    if (usePal) {
                        int idx = color_index(&ch, cur);
                        if (len == 1) { buf_u8(o, idx); goto next; }
                        buf_u8(o, idx | 128);
                    } else {
                        BYTE t[4];
                        put_cpixel(st, runPx, t);
                        buf_put(o, t, (size_t)cp);
                    }
                    while (rem >= 255) { buf_u8(o, 255); rem -= 255; }
                    buf_u8(o, rem);
                }
            next:
                if (j == h) break;
                cur = c;
                runPx = (const BYTE *)&row[i];
                len = 1;
            }
        }
    }
}

static void zrle_analyze(EncState *st, Unit *u, const BYTE *fb, int stride)
{
    /* 最悪でもタイルごとに「生」(1 + 画素数 × CPIXEL)なので、この大きさで足りる。
       tmp は単位の領域をそのまま使う(buf_reserve で広げることは無い) */
    size_t worst = (size_t)u->w * u->h * 4 + 1024;
    Buf    tmp;
    int    tx, ty;

    u->out.len = 0;
    u->kind = U_ZRLE;
    u->stream = 0;
    tmp.p = unit_raw(u, worst);
    if (!tmp.p) { u->kind = U_SKIP; return; }
    tmp.len = 0;
    tmp.cap = worst;
    for (ty = 0; ty < u->h; ty += 64)
        for (tx = 0; tx < u->w; tx += 64)
            zrle_tile(st, fb, stride, u->x + tx, u->y + ty, min(64, u->w - tx), min(64, u->h - ty), &tmp);
    u->rawLen = tmp.len;
}

static void raw_analyze(EncState *st, Unit *u, const BYTE *fb, int stride)
{
    int bpp = st->pf.bpp / 8, j;
    u->kind = U_DONE;
    u->stream = -1;
    u->out.len = 0;
    rect_header(&u->out, u->x, u->y, u->w, u->h, ENC_RAW);
    buf_reserve(&u->out, (size_t)u->w * u->h * bpp);
    for (j = 0; j < u->h; j++) {
        pf_put_pixels(&st->pf, fb + (size_t)(u->y + j) * stride + (size_t)u->x * 4, u->w, u->out.p + u->out.len);
        u->out.len += (size_t)u->w * bpp;
    }
}

static void phase1(void *ctx, int index, int slot)
{
    EncState *st = (EncState *)ctx;
    Unit *u = &st->units[index];
    const BYTE *fb = st->c->sendfb;
    int stride = st->c->sfW * 4;
    (void)slot;
    if (st->enc == ENC_TIGHT) tight_analyze(st, u, fb, stride);
    else if (st->enc == ENC_ZRLE) zrle_analyze(st, u, fb, stride);
    else raw_analyze(st, u, fb, stride);
}

/* ------------------------------------------------------------------ */
/*  二段目                                                              */
/* ------------------------------------------------------------------ */

static ZHist *stream_hist(EncState *st, int s)
{
    return st->enc == ENC_ZRLE ? &st->c->zzrle : &st->c->ztight[s];
}

/* 作業領域 slot の dictBuf に「直前 32KB(同じストリームの前の単位と、前回までの
   続き)+ この単位のデータ」を並べる。戻り値は辞書の長さ。*base は並べた先頭 */
static size_t build_dict(EncState *st, int index, int slot, BYTE **base)
{
    Unit  *u = &st->units[index];
    size_t got = 0, need = ZD_WINDOW + u->rawLen + 16;
    BYTE  *end;
    int    p;
    if (st->dictCap[slot] < need) {
        free(st->dictBuf[slot]);
        st->dictCap[slot] = need + 65536;
        st->dictBuf[slot] = (BYTE *)malloc(st->dictCap[slot]);
        if (!st->dictBuf[slot]) { st->dictCap[slot] = 0; *base = NULL; return 0; }
    }
    end = st->dictBuf[slot] + ZD_WINDOW;
    for (p = u->prevSame; p >= 0 && got < ZD_WINDOW; p = st->units[p].prevSame) {
        Unit  *q = &st->units[p];
        size_t take = min(q->rawLen, ZD_WINDOW - got);
        memcpy(end - got - take, q->rawBase + q->rawLen - take, take);
        got += take;
    }
    if (got < ZD_WINDOW) {
        ZHist *h = stream_hist(st, u->stream);
        size_t take = min((size_t)h->len, ZD_WINDOW - got);
        memcpy(end - got - take, h->data + h->len - take, take);
        got += take;
    }
    memcpy(end, u->rawBase, u->rawLen);
    *base = end - got;
    return got;
}

static void phase2_body(EncState *st, Unit *u, int index, int slot);

static void phase2(void *ctx, int index, int slot)
{
    EncState *st = (EncState *)ctx;
    Unit *u = &st->units[index];
    LARGE_INTEGER t;
    phase2_body(st, u, index, slot);
    QueryPerformanceCounter(&t);
    u->tEnd = t.QuadPart;
}

static void phase2_body(EncState *st, Unit *u, int index, int slot)
{

    if (u->kind == U_JPEG) {
        /* ふつうは画素あたり 3 バイトに収まる。収まらなければ最悪の大きさで作り直す */
        size_t cap = (size_t)u->w * u->h * 3 + 65536, n = 0;
        int stride = st->c->sfW * 4, tries;
        for (tries = 0; tries < 2 && !n; tries++) {
            if (tries) cap = jpe_bound(u->w, u->h);
            if (st->scratchCap[slot] < cap) {
                free(st->scratch[slot]);
                st->scratch[slot] = (BYTE *)malloc(cap);
                st->scratchCap[slot] = st->scratch[slot] ? cap : 0;
            }
            if (!st->scratch[slot]) break;
            n = jpe_encode((JpegEnc *)pool_jpeg(slot), st->c->sendfb + (size_t)u->y * stride + (size_t)u->x * 4,
                           stride, u->w, u->h, st->jpegQ, st->subsamp, st->scratch[slot], cap);
        }
        u->out.len = 0;
        rect_header(&u->out, u->x, u->y, u->w, u->h, ENC_TIGHT);
        buf_u8(&u->out, 0x90);
        compact_len(&u->out, n);
        buf_put(&u->out, st->scratch[slot], n);
        return;
    }
    if (u->kind == U_MONO || u->kind == U_PALETTE || u->kind == U_FULL || u->kind == U_ZRLE) {
        const BYTE *raw = u->rawBase;
        size_t clen = 0, need;
        BOOL   useZ = u->kind == U_ZRLE || u->rawLen >= 12;

        if (useZ) {
            BYTE  *base;
            size_t dict = build_dict(st, index, slot, &base);
            need = zd_bound(u->rawLen);
            if (st->scratchCap[slot] < need) {
                free(st->scratch[slot]);
                st->scratch[slot] = (BYTE *)malloc(need);
                st->scratchCap[slot] = st->scratch[slot] ? need : 0;
            }
            if (!st->scratch[slot] || !base) return;
            clen = zd_compress((ZDWork *)pool_zwork(slot), base, dict, u->rawLen, st->scratch[slot],
                               st->zlevel, u->header);
        }
        u->out.len = 0;
        if (u->kind == U_ZRLE) {
            rect_header(&u->out, u->x, u->y, u->w, u->h, ENC_ZRLE);
            buf_u32(&u->out, (unsigned)clen);
            buf_put(&u->out, st->scratch[slot], clen);
            return;
        }
        rect_header(&u->out, u->x, u->y, u->w, u->h, ENC_TIGHT);
        if (u->kind == U_FULL) {
            buf_u8(&u->out, u->stream << 4);
        } else {
            int i;
            buf_u8(&u->out, (u->stream << 4) | 0x40);
            buf_u8(&u->out, 1);                     /* パレットの濾過 */
            buf_u8(&u->out, u->ncolors - 1);
            for (i = 0; i < u->ncolors; i++) {
                BYTE t[3];
                tpixel(u->pal[i], t);
                buf_put(&u->out, t, 3);
            }
        }
        if (useZ) {
            compact_len(&u->out, clen);
            buf_put(&u->out, st->scratch[slot], clen);
        } else {
            buf_put(&u->out, raw, u->rawLen);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  更新 1 回                                                           */
/* ------------------------------------------------------------------ */

static void hist_append(ZHist *h, const BYTE *p, size_t n)
{
    if (n >= ZD_WINDOW) {
        memcpy(h->data, p + n - ZD_WINDOW, ZD_WINDOW);
        h->len = ZD_WINDOW;
        return;
    }
    if (h->len + n > ZD_WINDOW) {
        size_t keep = ZD_WINDOW - n;
        memmove(h->data, h->data + h->len - keep, keep);
        h->len = (int)keep;
    }
    memcpy(h->data + h->len, p, n);
    h->len += (int)n;
}

static Unit *add_unit(EncState *st, int x, int y, int w, int h)
{
    Unit *u;
    if (st->nunits >= st->cap) {
        int ncap = st->cap ? st->cap * 2 : 256;
        Unit *nu = (Unit *)realloc(st->units, sizeof(Unit) * (size_t)ncap);
        if (!nu) return NULL;
        ZeroMemory(nu + st->cap, sizeof(Unit) * (size_t)(ncap - st->cap));
        st->units = nu;
        st->cap = ncap;
    }
    u = &st->units[st->nunits++];
    u->x = x; u->y = y; u->w = w; u->h = h;
    u->rawLen = 0;
    u->prevSame = -1;
    u->header = 0;
    return u;
}

void encode_free(Client *c)
{
    EncState *st = (EncState *)c->encState;
    int i;
    if (!st) return;
    for (i = 0; i < st->cap; i++) {
        free(st->units[i].rawBase);
        buf_free(&st->units[i].out);
    }
    for (i = 0; i < 64; i++) { free(st->scratch[i]); free(st->dictBuf[i]); }
    free(st->units);
    free((void *)st->done);
    free(st);
    c->encState = NULL;
}

BOOL encode_update(Client *c, const RECT *rects, int nrects, BOOL haveCopy, RECT copyDst, POINT copySrc,
                   Buf *pseudo, int npseudo, LONG64 *encTicks)
{
    LARGE_INTEGER tStart;
    EncState *st = (EncState *)c->encState;
    PoolBatch b;
    Buf       head = { 0 };
    int       i, tile, nsend, slot, last[4];
    BOOL      ok = TRUE;

    QueryPerformanceCounter(&tStart);
    *encTicks = 0;
    if (!st) {
        st = (EncState *)calloc(1, sizeof(EncState));
        if (!st) return FALSE;
        c->encState = st;
    }
    st->c = c;
    EnterCriticalSection(&c->cs);
    st->pf      = c->pf;
    st->enc     = c->prefEnc;
    st->jpegQ   = c->jpegQuality;
    st->subsamp = c->subsamp;
    st->zlevel  = c->zlevel;
    LeaveCriticalSection(&c->cs);
    st->tpixel = st->pf.trueColor && st->pf.bpp == 32 && st->pf.depth == 24 &&
                 st->pf.rmax == 255 && st->pf.gmax == 255 && st->pf.bmax == 255;
    if (st->enc == ENC_TIGHT && !st->tpixel) st->enc = ENC_RAW;       /* server.c で ZRLE へ回すので念のため */
    st->cpixel = cpixel_size(&st->pf);

    /* 単位に切る */
    st->nunits = 0;
    tile = st->enc == ENC_TIGHT ? TIGHT_TILE : BIG_TILE;
    for (i = 0; i < nrects; i++) {
        int x, y;
        for (y = rects[i].top; y < rects[i].bottom; y += tile)
            for (x = rects[i].left; x < rects[i].right; x += tile)
                if (!add_unit(st, x, y, min(tile, (int)rects[i].right - x), min(tile, (int)rects[i].bottom - y))) return FALSE;
    }
    if (st->doneCap < st->nunits) {
        free((void *)st->done);
        st->doneCap = st->nunits + 64;
        st->done = (volatile LONG *)calloc((size_t)st->doneCap, sizeof(LONG));
        if (!st->done) { st->doneCap = 0; return FALSE; }
    }

    slot = pool_caller_slot();

    /* 一段目 */
    b.fn = phase1; b.ctx = st; b.count = st->nunits; b.done = st->done;
    pool_run(&b, slot);

    /* JPEG を横につなぐ */
    if (st->enc == ENC_TIGHT) {
        int prev = -1;
        for (i = 0; i < st->nunits; i++) {
            Unit *u = &st->units[i];
            if (u->kind != U_JPEG) { prev = -1; continue; }
            if (prev >= 0) {
                Unit *p = &st->units[prev];
                if (p->y == u->y && p->h == u->h && p->x + p->w == u->x && p->w + u->w <= JPEG_MAXW) {
                    p->w += u->w;
                    u->kind = U_SKIP;
                    continue;
                }
            }
            prev = i;
        }
    }

    /* ストリームの並び */
    for (i = 0; i < 4; i++) last[i] = -1;
    for (i = 0; i < st->nunits; i++) {
        Unit *u = &st->units[i];
        BOOL usesZ = u->kind == U_ZRLE || ((u->kind == U_MONO || u->kind == U_PALETTE || u->kind == U_FULL) && u->rawLen >= 12);
        if (!usesZ) continue;
        u->prevSame = last[u->stream];
        if (u->prevSame < 0 && !stream_hist(st, u->stream)->started) u->header = 1;
        last[u->stream] = i;
    }

    /* 二段目を始め、出来たものから送る */
    b.fn = phase2; b.ctx = st; b.count = st->nunits; b.done = st->done;
    pool_start(&b);

    nsend = npseudo + (haveCopy ? 1 : 0);
    for (i = 0; i < st->nunits; i++) if (st->units[i].kind != U_SKIP) nsend++;
    buf_u8(&head, 0);
    buf_u8(&head, 0);
    buf_u16(&head, min(nsend, 65535));
    if (pseudo && pseudo->len) buf_put(&head, pseudo->p, pseudo->len);
    if (haveCopy) {
        rect_header(&head, copyDst.left, copyDst.top, copyDst.right - copyDst.left, copyDst.bottom - copyDst.top, ENC_COPYRECT);
        buf_u16(&head, copySrc.x);
        buf_u16(&head, copySrc.y);
    }

    EnterCriticalSection(&c->sendLock);
    ok = client_send(c, head.p, (int)head.len);
    for (i = 0; i < st->nunits; i++) {
        Unit *u = &st->units[i];
        pool_wait_item(&b, i, slot);
        if (u->kind == U_SKIP || !ok) continue;
        ok = client_send(c, u->out.p, (int)u->out.len);
    }
    LeaveCriticalSection(&c->sendLock);
    pool_finish(&b, slot);
    pool_release_slot(slot);
    buf_free(&head);

    /* 符号化だけにかかった時間(最後の単位が出来た時刻まで) */
    for (i = 0; i < st->nunits; i++)
        if (st->units[i].tEnd - tStart.QuadPart > *encTicks) *encTicks = st->units[i].tEnd - tStart.QuadPart;

    /* ストリームの直前 32KB を覚える */
    for (i = 0; i < st->nunits; i++) {
        Unit *u = &st->units[i];
        BOOL usesZ = u->kind == U_ZRLE || ((u->kind == U_MONO || u->kind == U_PALETTE || u->kind == U_FULL) && u->rawLen >= 12);
        if (!usesZ) continue;
        hist_append(stream_hist(st, u->stream), u->rawBase, u->rawLen);
        stream_hist(st, u->stream)->started = 1;
    }
    /* 大きな更新の後は、64 個目より後の単位の領域を手放す(画面全体の更新で
       数十 MB になるので、持ち続けない) */
    for (i = 64; i < st->cap; i++) {
        Unit *u = &st->units[i];
        if (!u->rawBase && !u->out.p) break;
        free(u->rawBase);
        u->rawBase = NULL;
        u->rawCap = 0;
        buf_free(&u->out);
    }
    return ok;
}
