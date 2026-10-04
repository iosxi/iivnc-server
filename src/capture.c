/* ==================================================================
 * capture.c - 画面の取り込みと変化の記録
 *
 *  取り込みは 3 通り。どれも process_frame() に「移動した矩形」と
 *  「変わったかもしれない矩形」を渡し、同じ道を通る。
 *
 *   DXGI   Desktop Duplication。GPU が変わった矩形と移動した矩形を
 *          教えてくれるので、画面全体を見回らなくてよい。変わった矩形
 *          だけを GPU から CPU 側の写しへ取り寄せる。
 *   GDI    DXGI が使えないとき(リモート デスクトップの中、回転した画面
 *          など)。全体を BitBlt して前の絵と比べる。
 *   検証用 -testsrc。合成した絵を動かす(スクロールは移動した矩形として
 *          渡すので CopyRect の道も通る)。利用者の画面を写さない。
 *
 *  変わったかもしれない矩形は、32×32 のブロックごとに前の絵と比べ、
 *  本当に変わったブロックだけを接続ごとの dirty に記録する。DXGI の
 *  変化の矩形はウィンドウ全体のように粗いことが多いので、これで送る量が
 *  大きく減る(カーソルの点滅なら 1 ブロック)。
 *
 *  取り込みは「誰かが絵を待っているとき」だけ行う(引き寄せ式)。
 *  送っている間に起きた変化は DXGI の側で積み重なり、次に取り込んだとき
 *  まとめて受け取れる。待っている人がいなければ CPU も GPU も使わない。
 *
 *  fb を書き換えるのはこのスレッドだけ。比べて写す間は鍵を持たず、
 *  dirty に印を付けるときだけ排他の鍵を持つ。書き手のスレッドが写す
 *  途中で書き換わっても、そのブロックには後で印が付くので送り直される。
 * ================================================================== */

#include "iivnc.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>

Screen g_scr;

enum { MODE_NONE, MODE_DXGI, MODE_GDI, MODE_TEST };

typedef struct MoveRect { POINT src; RECT dst; } MoveRect;   /* 出力の座標 */

typedef struct Dup {
    IDXGIOutputDuplication *dup;
    ID3D11Device           *dev;
    ID3D11DeviceContext    *ctx;
    ID3D11Texture2D        *staging;
    RECT                    desk;       /* 仮想画面の座標 */
    int                     ox, oy;     /* fb の中の位置 */
    int                     w, h;
} Dup;

#define MAX_DUP 8

static int            g_mode;
static Dup            g_dups[MAX_DUP];
static int            g_ndup;
static int            g_curOwner = -1;
static HANDLE         g_thread;
static volatile LONG  g_quit;
static BYTE          *g_meta;
static UINT           g_metaCap;
static BYTE          *g_shape;
static UINT           g_shapeCap;
static BYTE          *g_changed;        /* ブロックごとの「今回変わった」 */
static int           *g_changedList;
static int            g_nchanged;
static DWORD          g_lastGrab;
static volatile LONG  g_reset;

/* GDI */
static HDC     g_memDC;
static HBITMAP g_dib, g_oldBmp;
static BYTE   *g_dibBits;
static HCURSOR g_lastCursor;

/* 検証用 */
static BYTE   *g_test;
static HDC     g_testDC;
static HBITMAP g_testBmp, g_testOld;
static int     g_testFrame;
static BOOL    g_testFirst;

/* ------------------------------------------------------------------ */
/*  fb の大きさ                                                         */
/* ------------------------------------------------------------------ */

static void alloc_client_dirty(Client *c)
{
    free(c->dirty);
    c->dirty = (BYTE *)calloc((size_t)g_scr.bw * g_scr.bh, 1);
}

void capture_mark_all(Client *c)
{
    if (!c->dirty) return;
    memset(c->dirty, 1, (size_t)g_scr.bw * g_scr.bh);
    c->dirtyAny = TRUE;
}

/* 取り込む範囲を決める。大きさが変わったら全員に知らせる */
static void set_fb(int vx, int vy, int w, int h)
{
    Client *c;
    AcquireSRWLockExclusive(&g_scr.lock);
    if (w != g_scr.w || h != g_scr.h || !g_scr.fb) {
        free(g_scr.fb);
        g_scr.fb = (BYTE *)calloc((size_t)w * h, 4);
        g_scr.w = w;
        g_scr.h = h;
        g_scr.bw = (w + BLOCK - 1) / BLOCK;
        g_scr.bh = (h + BLOCK - 1) / BLOCK;
        g_scr.sizeVer++;
        free(g_changed);
        free(g_changedList);
        g_changed = (BYTE *)calloc((size_t)g_scr.bw * g_scr.bh, 1);
        g_changedList = (int *)malloc(sizeof(int) * (size_t)g_scr.bw * g_scr.bh);
        for (c = g_scr.clients; c; c = c->next) {
            alloc_client_dirty(c);
            capture_mark_all(c);
            c->copyPending = FALSE;
            c->needResize  = TRUE;
            SetRect(&c->reqRect, 0, 0, w, h);
        }
        log_printf(L"取り込む範囲: (%d,%d) %dx%d", vx, vy, w, h);
    }
    g_scr.vx = vx;
    g_scr.vy = vy;
    ReleaseSRWLockExclusive(&g_scr.lock);
}

typedef struct { int want, n; RECT r, all; BOOL found; } MonEnum;

static BOOL CALLBACK mon_proc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    MonEnum *me = (MonEnum *)lp;
    MONITORINFOEXW mi;
    (void)dc; (void)rc;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (MONITORINFO *)&mi)) return TRUE;
    if (me->want) {
        const WCHAR *p = wcsstr(mi.szDevice, L"DISPLAY");
        if (p && _wtoi(p + 7) == me->want) { me->r = mi.rcMonitor; me->found = TRUE; }
    }
    return TRUE;
}

/* 選ばれた画面の範囲(仮想画面の座標) */
static void selected_rect(RECT *r)
{
    MonEnum me;
    ZeroMemory(&me, sizeof(me));
    me.want = g_cfg.display;
    if (me.want) EnumDisplayMonitors(NULL, NULL, mon_proc, (LPARAM)&me);
    if (me.found) { *r = me.r; return; }
    r->left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r->top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r->right  = r->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r->bottom = r->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/* 接続の初期化の前に呼ぶ(ServerInit で大きさを知らせるため) */
static void query_size(void)
{
    RECT r;
    if (g_mode != MODE_NONE) return;                            /* 開いているものに従う */
    if (g_testSrc) { set_fb(0, 0, 1920, 1080); return; }
    selected_rect(&r);
    set_fb(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

/* ------------------------------------------------------------------ */
/*  変化の記録                                                          */
/* ------------------------------------------------------------------ */

static BOOL dirty_intersects(const Client *c, const RECT *r)
{
    int bx0 = r->left / BLOCK, by0 = r->top / BLOCK, bx1 = (r->right - 1) / BLOCK, by1 = (r->bottom - 1) / BLOCK, x, y;
    for (y = by0; y <= by1; y++)
        for (x = bx0; x <= bx1; x++)
            if (c->dirty[y * g_scr.bw + x]) return TRUE;
    return FALSE;
}

static void mark_rect(Client *c, const RECT *r)
{
    int bx0 = r->left / BLOCK, by0 = r->top / BLOCK, bx1 = (r->right - 1) / BLOCK, by1 = (r->bottom - 1) / BLOCK, x, y;
    for (y = by0; y <= by1; y++)
        for (x = bx0; x <= bx1; x++)
            c->dirty[y * g_scr.bw + x] = 1;
    c->dirtyAny = TRUE;
}

/* 移動した矩形を fb に当て、接続ごとに CopyRect にするか変化にするか決める */
static void apply_move(int ox, int oy, const MoveRect *m)
{
    RECT  dst = m->dst, src;
    POINT s = m->src;
    int   w, h, y, stride = g_scr.w * 4;
    BYTE *fb = g_scr.fb;
    Client *c;

    OffsetRect(&dst, ox, oy);
    s.x += ox; s.y += oy;
    w = dst.right - dst.left;
    h = dst.bottom - dst.top;
    if (w <= 0 || h <= 0 || dst.left < 0 || dst.top < 0 || dst.right > g_scr.w || dst.bottom > g_scr.h ||
        s.x < 0 || s.y < 0 || s.x + w > g_scr.w || s.y + h > g_scr.h) return;
    SetRect(&src, s.x, s.y, s.x + w, s.y + h);

    if (dst.top > s.y) {
        for (y = h - 1; y >= 0; y--)
            memmove(fb + (size_t)(dst.top + y) * stride + dst.left * 4, fb + (size_t)(s.y + y) * stride + s.x * 4, (size_t)w * 4);
    } else {
        for (y = 0; y < h; y++)
            memmove(fb + (size_t)(dst.top + y) * stride + dst.left * 4, fb + (size_t)(s.y + y) * stride + s.x * 4, (size_t)w * 4);
    }

    AcquireSRWLockExclusive(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        if (!c->active || !c->dirty) continue;
        if (c->copyRect && !c->copyPending && !dirty_intersects(c, &src)) {
            c->copyPending = TRUE;
            c->copyDst = dst;
            c->copySrc = s;
            c->dirtyAny = TRUE;
        } else {
            mark_rect(c, &dst);
        }
    }
    ReleaseSRWLockExclusive(&g_scr.lock);
}

/* r(fb の座標)の範囲を src と比べ、違うブロックだけ fb へ写して印を付ける。
   src[0] は fb の (sx0, sy0) に当たる。 */
static void compare_rect(const BYTE *src, int pitch, int sx0, int sy0, const RECT *r)
{
    int bx, by, stride = g_scr.w * 4;
    int bx0 = r->left / BLOCK, by0 = r->top / BLOCK, bx1 = (r->right - 1) / BLOCK, by1 = (r->bottom - 1) / BLOCK;

    for (by = by0; by <= by1; by++) {
        int y0 = max(by * BLOCK, (int)r->top), y1 = min(by * BLOCK + BLOCK, (int)r->bottom);
        for (bx = bx0; bx <= bx1; bx++) {
            int x0 = max(bx * BLOCK, (int)r->left), x1 = min(bx * BLOCK + BLOCK, (int)r->right), y;
            size_t n = (size_t)(x1 - x0) * 4;
            int idx = by * g_scr.bw + bx;
            for (y = y0; y < y1; y++) {
                BYTE       *d = g_scr.fb + (size_t)y * stride + x0 * 4;
                const BYTE *s = src + (size_t)(y - sy0) * pitch + (x0 - sx0) * 4;
                if (memcmp(d, s, n)) break;
            }
            if (y == y1) continue;
            for (; y < y1; y++)
                memcpy(g_scr.fb + (size_t)y * stride + x0 * 4, src + (size_t)(y - sy0) * pitch + (x0 - sx0) * 4, n);
            if (!g_changed[idx]) {
                g_changed[idx] = 1;
                g_changedList[g_nchanged++] = idx;
            }
        }
    }
}

static void commit_changes(void)
{
    Client *c;
    int i;
    if (!g_nchanged) return;
    AcquireSRWLockExclusive(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        if (!c->active || !c->dirty) continue;
        for (i = 0; i < g_nchanged; i++) c->dirty[g_changedList[i]] = 1;
        c->dirtyAny = TRUE;
    }
    ReleaseSRWLockExclusive(&g_scr.lock);
    for (i = 0; i < g_nchanged; i++) g_changed[g_changedList[i]] = 0;
    g_nchanged = 0;
}

/* 1 つの出力の 1 フレーム分。src は出力の左上(fb の ox, oy)に当たる */
static void process_frame(const BYTE *src, int pitch, int ox, int oy, int ow, int oh,
                          const MoveRect *moves, int nmoves, const RECT *dirty, int ndirty)
{
    int i;
    for (i = 0; i < nmoves; i++) apply_move(ox, oy, &moves[i]);
    for (i = 0; i < ndirty; i++) {
        RECT r = dirty[i];
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > ow) r.right = ow;
        if (r.bottom > oh) r.bottom = oh;
        if (r.right <= r.left || r.bottom <= r.top) continue;
        OffsetRect(&r, ox, oy);
        if (r.right > g_scr.w) r.right = g_scr.w;
        if (r.bottom > g_scr.h) r.bottom = g_scr.h;
        if (r.right <= r.left || r.bottom <= r.top) continue;
        compare_rect(src, pitch, ox, oy, &r);
    }
    commit_changes();
    g_scr.valid = TRUE;
}

/* ------------------------------------------------------------------ */
/*  カーソル                                                            */
/* ------------------------------------------------------------------ */

/* 形を決める。pix は BGRA、mask は 1 = 見える */
static void set_cursor_shape(int w, int h, int hx, int hy, const BYTE *pix, const BYTE *mask)
{
    int mb = (w + 7) / 8;
    BYTE *p = (BYTE *)malloc((size_t)w * h * 4 + 4), *m = (BYTE *)malloc((size_t)mb * h + 1);
    if (!p || !m) { free(p); free(m); return; }
    memcpy(p, pix, (size_t)w * h * 4);
    memcpy(m, mask, (size_t)mb * h);
    AcquireSRWLockExclusive(&g_scr.lock);
    free(g_scr.curPix);
    free(g_scr.curMask);
    g_scr.curPix  = p;
    g_scr.curMask = m;
    g_scr.curW = w; g_scr.curH = h;
    g_scr.curHotX = hx; g_scr.curHotY = hy;
    g_scr.curVer++;
    ReleaseSRWLockExclusive(&g_scr.lock);
}

static void set_cursor_pos(int x, int y, BOOL visible)
{
    BOOL changed = FALSE;
    AcquireSRWLockExclusive(&g_scr.lock);
    if (x != g_scr.curX || y != g_scr.curY || visible != g_scr.curVisible) {
        g_scr.curX = x;
        g_scr.curY = y;
        if (visible != g_scr.curVisible) { g_scr.curVer++; changed = TRUE; }   /* 見え方が変わった = 形を送り直す */
        g_scr.curVisible = visible;
        g_scr.curPosVer++;
    }
    ReleaseSRWLockExclusive(&g_scr.lock);
    if (changed) log_printf(L"カーソル: Windows が%s", visible ? L"表示している" : L"隠している");
}

static void cursor_from_hcursor(HCURSOR hc);

/* 形がまだ一つも無ければ、標準の矢印にする */
static void cursor_ensure_shape(void)
{
    BOOL none;
    AcquireSRWLockShared(&g_scr.lock);
    none = g_scr.curPix == NULL;
    ReleaseSRWLockShared(&g_scr.lock);
    if (none) {
        cursor_from_hcursor(LoadCursorW(NULL, IDC_ARROW));
        log_printf(L"カーソル: 形が無いので標準の矢印を使う");
    }
}

/* Windows 10 はマウスがつながっていないとカーソルを隠したままにする(見えるのは
   マウスを挿したときだけ)。隠れている間も Windows が持っている形(矢印・I ビームなど)を
   追い、相手には見せる(showcursor=1)。隠れているので DXGI は形を渡してこない */
static void cursor_follow_hidden(void)
{
    CURSORINFO ci;
    if (!g_cfg.showCursor) return;
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && ci.hCursor && ci.hCursor != g_lastCursor) {
        g_lastCursor = ci.hCursor;
        cursor_from_hcursor(ci.hCursor);
    }
    cursor_ensure_shape();
}

/* マウスの有無を記録に書く(マウスが無い PC でカーソルが見えないときの手がかり) */
static void log_mouse_devices(void)
{
    static BOOL done;
    RAWINPUTDEVICELIST *l;
    UINT n = 0, i, mice = 0;
    CURSORINFO ci;
    if (done) return;
    done = TRUE;
    GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    l = (RAWINPUTDEVICELIST *)calloc(n ? n : 1, sizeof(*l));
    if (l && (int)GetRawInputDeviceList(l, &n, sizeof(*l)) >= 0) {
        for (i = 0; i < n; i++) {
            WCHAR name[256];
            UINT  len = ARRAYSIZE(name);
            if (l[i].dwType != RIM_TYPEMOUSE) continue;
            mice++;
            name[0] = 0;
            GetRawInputDeviceInfoW(l[i].hDevice, RIDI_DEVICENAME, name, &len);
            log_printf(L"マウスの装置: %s", name);
        }
    }
    free(l);
    ci.cbSize = sizeof(ci);
    GetCursorInfo(&ci);
    log_printf(L"マウス: SM_MOUSEPRESENT %d、装置 %u 個。カーソルの flags %lu、形 %p", GetSystemMetrics(SM_MOUSEPRESENT), mice,
               (unsigned long)ci.flags, (void *)ci.hCursor);
}

/* DXGI の形を BGRA と見える印に直す */
static void cursor_from_dxgi(const DXGI_OUTDUPL_POINTER_SHAPE_INFO *si, const BYTE *buf)
{
    int w = (int)si->Width, h = (int)si->Height, x, y, mb;
    BYTE *pix, *mask;

    if (si->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return;
    mb = (w + 7) / 8;
    pix  = (BYTE *)calloc((size_t)w * h, 4);
    mask = (BYTE *)calloc((size_t)mb * h, 1);
    if (!pix || !mask) { free(pix); free(mask); return; }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            BYTE *d = pix + ((size_t)y * w + x) * 4;
            BOOL  vis = FALSE;
            if (si->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                int a = (buf[y * si->Pitch + x / 8] >> (7 - (x & 7))) & 1;
                int o = (buf[(y + h) * si->Pitch + x / 8] >> (7 - (x & 7))) & 1;
                if (!a) {               /* 置き換え: 黒か白 */
                    BYTE v = o ? 255 : 0;
                    d[0] = d[1] = d[2] = v;
                    vis = TRUE;
                } else if (o) {         /* 反転: 黒で近似する */
                    vis = TRUE;
                }
            } else {
                const BYTE *s = buf + y * si->Pitch + x * 4;
                if (si->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    vis = s[3] >= 128;
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                } else {                /* MASKED_COLOR: A=0 は置き換え、A=FF は XOR */
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                    vis = s[3] == 0 || (s[0] | s[1] | s[2]);
                }
            }
            d[3] = 255;
            if (vis) mask[y * mb + x / 8] |= (BYTE)(0x80 >> (x & 7));
        }
    }
    set_cursor_shape(w, h, (int)si->HotSpot.x, (int)si->HotSpot.y, pix, mask);
    free(pix);
    free(mask);
}

/* GDI のカーソル(HCURSOR)から作る */
static void cursor_from_hcursor(HCURSOR hc)
{
    ICONINFO   ii;
    BITMAP     bm;
    BITMAPINFO bi;
    HDC        dc;
    int        w, h, x, y, mb;
    BYTE      *col = NULL, *msk = NULL, *pix = NULL, *mask = NULL;
    BOOL       hasAlpha = FALSE;

    if (!GetIconInfo(hc, &ii)) return;
    GetObjectW(ii.hbmMask, sizeof(bm), &bm);
    w = bm.bmWidth;
    h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) goto done;
    dc = GetDC(NULL);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -(ii.hbmColor ? h : h * 2);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    msk = (BYTE *)calloc((size_t)w * h * 2, 4);
    GetDIBits(dc, ii.hbmMask, 0, (UINT)(ii.hbmColor ? h : h * 2), msk, &bi, DIB_RGB_COLORS);
    if (ii.hbmColor) {
        bi.bmiHeader.biHeight = -h;
        col = (BYTE *)calloc((size_t)w * h, 4);
        GetDIBits(dc, ii.hbmColor, 0, (UINT)h, col, &bi, DIB_RGB_COLORS);
        for (y = 0; y < w * h; y++) if (col[y * 4 + 3]) { hasAlpha = TRUE; break; }
    }
    ReleaseDC(NULL, dc);

    mb = (w + 7) / 8;
    pix  = (BYTE *)calloc((size_t)w * h, 4);
    mask = (BYTE *)calloc((size_t)mb * h, 1);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            BYTE *d = pix + ((size_t)y * w + x) * 4;
            BOOL  vis;
            int   a = msk[((size_t)y * w + x) * 4] != 0;
            if (col) {
                const BYTE *s = col + ((size_t)y * w + x) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                vis = hasAlpha ? s[3] >= 128 : (!a || (s[0] | s[1] | s[2]));
            } else {
                int o = msk[((size_t)(y + h) * w + x) * 4] != 0;
                d[0] = d[1] = d[2] = (BYTE)(!a && o ? 255 : 0);
                vis = !a || o;
            }
            d[3] = 255;
            if (vis) mask[y * mb + x / 8] |= (BYTE)(0x80 >> (x & 7));
        }
    set_cursor_shape(w, h, (int)ii.xHotspot, (int)ii.yHotspot, pix, mask);
done:
    free(col); free(msk); free(pix); free(mask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

/* ------------------------------------------------------------------ */
/*  DXGI                                                                */
/* ------------------------------------------------------------------ */

static void dxgi_close(void)
{
    int i;
    for (i = 0; i < g_ndup; i++) {
        Dup *d = &g_dups[i];
        if (d->staging) ID3D11Texture2D_Release(d->staging);
        if (d->dup) IDXGIOutputDuplication_Release(d->dup);
        if (d->ctx) ID3D11DeviceContext_Release(d->ctx);
        if (d->dev) ID3D11Device_Release(d->dev);
    }
    ZeroMemory(g_dups, sizeof(g_dups));
    g_ndup = 0;
}

/* DXGI の複製を始めた直後の 1 枚目は中身が無いことがある(LastPresentTime=0、
   変化の情報も無い。2026-10-04 実測)。画面が止まっていれば 2 枚目も来ない。
   そこで最初の絵とカーソルは GDI で撮って入れ、以後は DXGI の変化だけを使う。 */
static void gdi_seed(void)
{
    BITMAPINFO bi;
    HDC        sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    HBITMAP    bmp, old;
    BYTE      *bits = NULL;
    CURSORINFO ci;
    int        i, n;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_scr.w;
    bi.bmiHeader.biHeight = -g_scr.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bmp) {
        RECT full;
        old = (HBITMAP)SelectObject(mdc, bmp);
        BitBlt(mdc, 0, 0, g_scr.w, g_scr.h, sdc, g_scr.vx, g_scr.vy, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        n = g_scr.w * g_scr.h;
        for (i = 0; i < n; i++) bits[i * 4 + 3] = 255;
        SetRect(&full, 0, 0, g_scr.w, g_scr.h);
        process_frame(bits, g_scr.w * 4, 0, 0, g_scr.w, g_scr.h, NULL, 0, &full, 1);
        SelectObject(mdc, old);
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
        BOOL vis = (ci.flags & CURSOR_SHOWING) != 0;
        if (vis && ci.hCursor) cursor_from_hcursor(ci.hCursor);
        set_cursor_pos(ci.ptScreenPos.x - g_scr.vx, ci.ptScreenPos.y - g_scr.vy, vis);
        if (!vis) cursor_follow_hidden();
    }
}

static BOOL dxgi_open(void)
{
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad;
    UINT a, o;
    HRESULT hr, lastErr = S_OK;
    RECT bound = { 0 };
    int i;
    BOOL rotated = FALSE;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) return FALSE;
    for (a = 0; IDXGIFactory1_EnumAdapters1(fac, a, &ad) == S_OK; a++) {
        ID3D11Device        *dev = NULL;
        ID3D11DeviceContext *ctx = NULL;
        IDXGIOutput         *out;
        for (o = 0; g_ndup < MAX_DUP && IDXGIAdapter1_EnumOutputs(ad, o, &out) == S_OK; o++) {
            DXGI_OUTPUT_DESC        od;
            IDXGIOutput1           *o1 = NULL;
            IDXGIOutput5           *o5 = NULL;
            IDXGIOutputDuplication *dup = NULL;
            const WCHAR            *p;
            int                     num;

            IDXGIOutput_GetDesc(out, &od);
            p = wcsstr(od.DeviceName, L"DISPLAY");
            num = p ? _wtoi(p + 7) : 0;
            if (!od.AttachedToDesktop || (g_cfg.display && num != g_cfg.display)) { IDXGIOutput_Release(out); continue; }
            if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY && od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                rotated = TRUE;
                IDXGIOutput_Release(out);
                continue;
            }
            if (!dev) {
                D3D_FEATURE_LEVEL fl;
                hr = D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                       D3D11_SDK_VERSION, &dev, &fl, &ctx);
                if (FAILED(hr)) { lastErr = hr; IDXGIOutput_Release(out); break; }
            }
            hr = E_FAIL;
            if (SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput5, (void **)&o5))) {
                DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
                hr = IDXGIOutput5_DuplicateOutput1(o5, (IUnknown *)dev, 0, 1, &fmt, &dup);
                IDXGIOutput5_Release(o5);
            }
            if (FAILED(hr) && SUCCEEDED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void **)&o1))) {
                hr = IDXGIOutput1_DuplicateOutput(o1, (IUnknown *)dev, &dup);
                IDXGIOutput1_Release(o1);
            }
            IDXGIOutput_Release(out);
            if (FAILED(hr)) { lastErr = hr; continue; }
            {
                DXGI_OUTDUPL_DESC dd;
                IDXGIOutputDuplication_GetDesc(dup, &dd);
                if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                    log_printf(L"DXGI: 画面 %d の形式 %d には対応していない", num, (int)dd.ModeDesc.Format);
                    IDXGIOutputDuplication_Release(dup);
                    lastErr = DXGI_ERROR_UNSUPPORTED;
                    continue;
                }
            }
            {
                Dup *d = &g_dups[g_ndup++];
                ZeroMemory(d, sizeof(*d));
                d->dup = dup;
                d->dev = dev; ID3D11Device_AddRef(dev);
                d->ctx = ctx; ID3D11DeviceContext_AddRef(ctx);
                d->desk = od.DesktopCoordinates;
                d->w = d->desk.right - d->desk.left;
                d->h = d->desk.bottom - d->desk.top;
            }
        }
        if (ctx) ID3D11DeviceContext_Release(ctx);
        if (dev) ID3D11Device_Release(dev);
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(fac);

    if (!g_ndup || rotated) {
        if (rotated) log_printf(L"DXGI: 回転した画面があるので GDI で取り込む");
        else log_printf(L"DXGI: 複製できない (0x%08lX)", (unsigned long)lastErr);
        dxgi_close();
        /* 安全なデスクトップ(UAC の確認、ロック画面)の間は E_ACCESSDENIED。後でやり直す */
        return FALSE;
    }
    bound = g_dups[0].desk;
    for (i = 1; i < g_ndup; i++) UnionRect(&bound, &bound, &g_dups[i].desk);
    set_fb(bound.left, bound.top, bound.right - bound.left, bound.bottom - bound.top);
    for (i = 0; i < g_ndup; i++) {
        g_dups[i].ox = g_dups[i].desk.left - bound.left;
        g_dups[i].oy = g_dups[i].desk.top - bound.top;
    }
    log_mouse_devices();
    gdi_seed();
    log_printf(L"DXGI で取り込む(出力 %d 個)。最初の絵は GDI で撮った", g_ndup);
    return TRUE;
}

static BOOL ensure_staging(Dup *d)
{
    D3D11_TEXTURE2D_DESC td;
    if (d->staging) return TRUE;
    ZeroMemory(&td, sizeof(td));
    td.Width = (UINT)d->w;
    td.Height = (UINT)d->h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(d->dev, &td, NULL, &d->staging));
}

static BOOL reserve(BYTE **p, UINT *cap, UINT need)
{
    if (need <= *cap) return TRUE;
    free(*p);
    *p = (BYTE *)malloc(need);
    *cap = *p ? need : 0;
    return *p != NULL;
}

/* 戻り値: 1 = 何か変わった、0 = 変わらない、-1 = やり直しが要る */
static int dxgi_grab(DWORD timeout)
{
    int i, result = 0;
    for (i = 0; i < g_ndup; i++) {
        Dup *d = &g_dups[i];
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *res = NULL;
        HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(d->dup, g_ndup == 1 ? timeout : (i == 0 ? 8 : 0), &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {
            log_printf(L"DXGI: AcquireNextFrame 0x%08lX", (unsigned long)hr);
            return -1;
        }

        if (fi.LastMouseUpdateTime.QuadPart) {
            if (fi.PointerPosition.Visible) {
                g_curOwner = i;
                set_cursor_pos(d->ox + fi.PointerPosition.Position.x, d->oy + fi.PointerPosition.Position.y, TRUE);
            } else if (g_curOwner == i || g_curOwner < 0) {
                set_cursor_pos(g_scr.curX, g_scr.curY, FALSE);
            }
            result = 1;
        }
        if (fi.PointerShapeBufferSize && reserve(&g_shape, &g_shapeCap, fi.PointerShapeBufferSize)) {
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
            UINT need = 0;
            if (SUCCEEDED(IDXGIOutputDuplication_GetFramePointerShape(d->dup, g_shapeCap, g_shape, &need, &si))) {
                cursor_from_dxgi(&si, g_shape);
                g_lastCursor = NULL;            /* 隠れたら、また GetCursorInfo の形から追い直す */
                result = 1;
            }
        }
        if (!g_scr.curVisible) cursor_follow_hidden();

        if (fi.LastPresentTime.QuadPart && res && ensure_staging(d)) {
            ID3D11Texture2D *tex = NULL;
            MoveRect *moves = NULL;
            RECT     *dirty = NULL;
            int       nmoves = 0, ndirty = 0, k;
            BOOL      ok = TRUE;

            IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex);
            if (fi.TotalMetadataBufferSize && reserve(&g_meta, &g_metaCap, fi.TotalMetadataBufferSize * 2)) {
                UINT used = 0, used2 = 0;
                DXGI_OUTDUPL_MOVE_RECT *mr = (DXGI_OUTDUPL_MOVE_RECT *)g_meta;
                hr = IDXGIOutputDuplication_GetFrameMoveRects(d->dup, g_metaCap, mr, &used);
                if (SUCCEEDED(hr)) {
                    nmoves = (int)(used / sizeof(DXGI_OUTDUPL_MOVE_RECT));
                    /* MoveRect と DXGI_OUTDUPL_MOVE_RECT は同じ並び(POINT, RECT) */
                    moves = (MoveRect *)mr;
                    dirty = (RECT *)(g_meta + ((used + 15) & ~15u));
                    hr = IDXGIOutputDuplication_GetFrameDirtyRects(d->dup, g_metaCap - ((used + 15) & ~15u), dirty, &used2);
                    ndirty = SUCCEEDED(hr) ? (int)(used2 / sizeof(RECT)) : 0;
                }
                if (FAILED(hr)) ok = FALSE;
                for (k = 0; ok && tex && k < ndirty; k++) {
                    D3D11_BOX box;
                    RECT r = dirty[k];
                    if (r.left < 0) r.left = 0;
                    if (r.top < 0) r.top = 0;
                    if (r.right > d->w) r.right = d->w;
                    if (r.bottom > d->h) r.bottom = d->h;
                    if (r.right <= r.left || r.bottom <= r.top) continue;
                    box.left = (UINT)r.left; box.top = (UINT)r.top; box.front = 0;
                    box.right = (UINT)r.right; box.bottom = (UINT)r.bottom; box.back = 1;
                    ID3D11DeviceContext_CopySubresourceRegion(d->ctx, (ID3D11Resource *)d->staging, 0, (UINT)r.left, (UINT)r.top, 0,
                                                              (ID3D11Resource *)tex, 0, &box);
                }
            }
            if (ok && tex && (nmoves || ndirty)) {
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)d->staging, 0, D3D11_MAP_READ, 0, &map))) {
                    int before = g_nchanged;
                    (void)before;
                    process_frame((const BYTE *)map.pData, (int)map.RowPitch, d->ox, d->oy, d->w, d->h,
                                  moves, nmoves, dirty, ndirty);
                    ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)d->staging, 0);
                    result = 1;
                }
            }
            if (tex) ID3D11Texture2D_Release(tex);
        }
        if (res) IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(d->dup);
    }
    return result;
}

/* ------------------------------------------------------------------ */
/*  GDI                                                                 */
/* ------------------------------------------------------------------ */

static void gdi_close(void)
{
    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_dib);
        DeleteDC(g_memDC);
    }
    g_memDC = NULL;
    g_dib = NULL;
    g_dibBits = NULL;
    g_lastCursor = NULL;
}

static BOOL gdi_open(void)
{
    BITMAPINFO bi;
    HDC dc;
    RECT r;
    selected_rect(&r);
    set_fb(r.left, r.top, r.right - r.left, r.bottom - r.top);
    dc = GetDC(NULL);
    g_memDC = CreateCompatibleDC(dc);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_scr.w;
    bi.bmiHeader.biHeight = -g_scr.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&g_dibBits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!g_dib) { DeleteDC(g_memDC); g_memDC = NULL; return FALSE; }
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);
    log_mouse_devices();
    log_printf(L"GDI で取り込む");
    return TRUE;
}

static int gdi_grab(void)
{
    HDC dc = GetDC(NULL);
    RECT full;
    CURSORINFO ci;
    int i, n;
    BOOL ok = BitBlt(g_memDC, 0, 0, g_scr.w, g_scr.h, dc, g_scr.vx, g_scr.vy, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, dc);
    if (!ok) return -1;
    GdiFlush();
    /* GDI の第 4 バイトは 0。DXGI と同じく 255 にそろえる(比べるときの違いを無くす) */
    n = g_scr.w * g_scr.h;
    for (i = 0; i < n; i++) g_dibBits[i * 4 + 3] = 255;
    SetRect(&full, 0, 0, g_scr.w, g_scr.h);
    process_frame(g_dibBits, g_scr.w * 4, 0, 0, g_scr.w, g_scr.h, NULL, 0, &full, 1);

    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
        BOOL vis = (ci.flags & CURSOR_SHOWING) != 0;
        if (vis && ci.hCursor != g_lastCursor) {
            g_lastCursor = ci.hCursor;
            cursor_from_hcursor(ci.hCursor);
        }
        set_cursor_pos(ci.ptScreenPos.x - g_scr.vx, ci.ptScreenPos.y - g_scr.vy, vis);
        if (!vis) cursor_follow_hidden();
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  検証用の絵                                                          */
/* ------------------------------------------------------------------ */

#define TW 1920
#define TH 1080
/* スクロールする文字の欄 */
#define SX 40
#define SY 120
#define SW 900
#define SH 600

static void test_text_line(int y, int n)
{
    RECT r;
    WCHAR s[160];
    SetRect(&r, SX, y, SX + SW, y + 20);
    FillRect(g_testDC, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    wsprintfW(s, L"%05d  iivnc の検証用の行です。The quick brown fox jumps over the lazy dog. 漢字かなカナ 0123456789", n);
    SetBkMode(g_testDC, TRANSPARENT);
    SetTextColor(g_testDC, RGB((n * 37) & 127, 20, 60));
    TextOutW(g_testDC, SX + 6, y + 2, s, lstrlenW(s));
}

static void test_photo(int frame)
{
    int x, y;
    unsigned seed = 12345u + (unsigned)frame * 7919u;
    for (y = 0; y < 300; y++) {
        BYTE *row = g_test + ((size_t)(140 + y) * TW + 1000) * 4;
        for (x = 0; x < 400; x++) {
            int r, g, b, n;
            seed = seed * 1103515245u + 12345u;
            n = (int)((seed >> 16) & 31) - 16;
            r = (x * 255 / 400 + frame * 3) & 255;
            g = (y * 255 / 300) & 255;
            b = (128 + (int)(64 * ((x + y + frame * 5) % 97) / 97));
            r = max(0, min(255, r + n)); g = max(0, min(255, g + n)); b = max(0, min(255, b + n));
            row[x * 4 + 0] = (BYTE)b; row[x * 4 + 1] = (BYTE)g; row[x * 4 + 2] = (BYTE)r; row[x * 4 + 3] = 255;
        }
    }
}

/* 全面: 動くグラデーションに縞と雑音(写真や動画のように色が多い) */
static void test_video(int frame)
{
    int x, y;
    unsigned seed = 777u + (unsigned)frame * 2654435761u;
    for (y = 0; y < TH; y++) {
        BYTE *row = g_test + (size_t)y * TW * 4;
        for (x = 0; x < TW; x++) {
            int r, g, b, n;
            seed = seed * 1103515245u + 12345u;
            n = (int)((seed >> 16) & 15) - 8;
            r = ((x + frame * 4) * 255 / TW) & 255;
            g = ((y + frame * 2) * 255 / TH) & 255;
            b = (((x / 40 + y / 40 + frame / 3) & 1) ? 200 : 60);
            r = max(0, min(255, r + n)); g = max(0, min(255, g + n)); b = max(0, min(255, b + n));
            row[x * 4 + 0] = (BYTE)b; row[x * 4 + 1] = (BYTE)g; row[x * 4 + 2] = (BYTE)r; row[x * 4 + 3] = 255;
        }
    }
}

static void test_fix_alpha(const RECT *r)
{
    int x, y;
    for (y = r->top; y < r->bottom; y++)
        for (x = r->left; x < r->right; x++) g_test[((size_t)y * TW + x) * 4 + 3] = 255;
}

static BOOL test_open(void)
{
    BITMAPINFO bi;
    HDC dc;
    HFONT f;
    RECT r;
    int i;
    BYTE *cur, *mask;

    set_fb(0, 0, TW, TH);
    g_testFirst = TRUE;
    if (g_test) return TRUE;
    dc = GetDC(NULL);
    g_testDC = CreateCompatibleDC(dc);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = TW;
    bi.bmiHeader.biHeight = -TH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_testBmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&g_test, NULL, 0);
    ReleaseDC(NULL, dc);
    g_testOld = (HBITMAP)SelectObject(g_testDC, g_testBmp);
    f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    SelectObject(g_testDC, f);

    /* 背景、窓、帯 */
    SetRect(&r, 0, 0, TW, TH);
    FillRect(g_testDC, &r, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
    for (i = 0; i < 16; i++) {
        HBRUSH b = CreateSolidBrush(RGB(i * 16, 255 - i * 16, (i * 53) & 255));
        SetRect(&r, i * (TW / 16), TH - 60, (i + 1) * (TW / 16), TH);
        FillRect(g_testDC, &r, b);
        DeleteObject(b);
    }
    SetRect(&r, SX - 4, SY - 30, SX + SW + 4, SY + SH + 4);
    FillRect(g_testDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    for (i = 0; i < SH / 20; i++) test_text_line(SY + i * 20, i);
    test_photo(0);
    SetRect(&r, 0, 0, TW, TH);
    test_fix_alpha(&r);
    g_testFrame = 0;

    /* 矢印のカーソル(12x19) */
    cur  = (BYTE *)calloc(12 * 19, 4);
    mask = (BYTE *)calloc(2 * 19, 1);
    for (i = 0; i < 19; i++) {
        int x, w = i < 12 ? i + 1 : (i < 16 ? 4 : 3);
        for (x = 0; x < w && x < 12; x++) {
            BYTE v = (x == 0 || x == w - 1 || i == 18) ? 0 : 255;
            cur[(i * 12 + x) * 4 + 0] = cur[(i * 12 + x) * 4 + 1] = cur[(i * 12 + x) * 4 + 2] = v;
            cur[(i * 12 + x) * 4 + 3] = 255;
            mask[i * 2 + x / 8] |= (BYTE)(0x80 >> (x & 7));
        }
    }
    if (g_testCursor != 2) set_cursor_shape(12, 19, 0, 0, cur, mask);
    set_cursor_pos(960, 540, !g_testCursor);
    if (g_testCursor && g_cfg.showCursor) cursor_ensure_shape();   /* 検証用: 隠れたカーソル(none は形も無し) */
    free(cur);
    free(mask);
    return TRUE;
}

static void test_close(void)
{
    if (!g_testDC) return;
    SelectObject(g_testDC, g_testOld);
    DeleteObject(g_testBmp);
    DeleteDC(g_testDC);
    g_testDC = NULL;
    g_test = NULL;
}

static int test_grab(void)
{
    MoveRect mv;
    RECT     dirty[4], r;
    int      nd = 0, nm = 0, y;

    if (g_testFirst) {                  /* 開いた直後は全体 */
        g_testFirst = FALSE;
        SetRect(&dirty[0], 0, 0, TW, TH);
        process_frame(g_test, TW * 4, 0, 0, TW, TH, NULL, 0, dirty, 1);
        return 1;
    }
    if (g_testSrc == 2 || (g_testFrames && g_testFrame >= g_testFrames)) {
        Sleep(50);
        return 0;
    }
    if (g_testFps > 0) {                /* 決めた速さより速くは作らない */
        static LARGE_INTEGER last, freq;
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        for (;;) {
            LONGLONG wait;
            QueryPerformanceCounter(&now);
            wait = (last.QuadPart + freq.QuadPart / g_testFps - now.QuadPart) * 1000 / freq.QuadPart;
            if (!last.QuadPart || wait <= 0) break;
            Sleep((DWORD)wait);
        }
        last = now;
    }
    g_testFrame++;
    if (g_testResize && g_testFrame == g_testResize) {
        /* 画面の大きさが変わった(1280x720 にする)。接続には DesktopSize で知らせる */
        set_fb(0, 0, 1280, 720);
        SetRect(&dirty[0], 0, 0, 1280, 720);
        process_frame(g_test, TW * 4, 0, 0, 1280, 720, NULL, 0, dirty, 1);
        return 1;
    }
    if (g_testSrc == 3) {               /* 動画のように全面が変わる */
        test_video(g_testFrame);
        SetRect(&dirty[0], 0, 0, TW, TH);
        process_frame(g_test, TW * 4, 0, 0, g_scr.w, g_scr.h, NULL, 0, dirty, 1);
        return 1;
    }
    /* 文字の欄を 20px 上へ送り、下に 1 行足す(移動した矩形として渡す) */
    for (y = 0; y < SH - 20; y++)
        memmove(g_test + ((size_t)(SY + y) * TW + SX) * 4, g_test + ((size_t)(SY + y + 20) * TW + SX) * 4, (size_t)SW * 4);
    mv.src.x = SX; mv.src.y = SY + 20;
    SetRect(&mv.dst, SX, SY, SX + SW, SY + SH - 20);
    nm = 1;
    test_text_line(SY + SH - 20, SH / 20 + g_testFrame - 1);
    SetRect(&dirty[nd++], SX, SY + SH - 20, SX + SW, SY + SH);
    test_fix_alpha(&dirty[0]);
    /* 動く四角 */
    {
        int bx = 1000 + (g_testFrame * 7) % 800, by = 600;
        HBRUSH b = CreateSolidBrush(RGB((g_testFrame * 5) & 255, 80, 200));
        SetRect(&r, 1000, 600, 1900, 700);
        FillRect(g_testDC, &r, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
        SetRect(&r, bx, by, bx + 60, by + 60);
        FillRect(g_testDC, &r, b);
        DeleteObject(b);
        SetRect(&dirty[nd], 1000, 600, 1900, 700);
        test_fix_alpha(&dirty[nd]);
        nd++;
    }
    /* 写真のような欄 */
    test_photo(g_testFrame);
    SetRect(&dirty[nd++], 1000, 140, 1400, 440);
    process_frame(g_test, TW * 4, 0, 0, g_scr.w, g_scr.h, &mv, nm, dirty, nd);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  取り込みのスレッド                                                  */
/* ------------------------------------------------------------------ */

static void close_capture(void)
{
    g_scr.valid = FALSE;
    dxgi_close();
    gdi_close();
    g_mode = MODE_NONE;
}

static BOOL open_capture(void)
{
    /* サービスの分身: 今の入力デスクトップ(ログイン画面・ロック画面・UAC は Winlogon)へ移ってから開く */
    if (g_runMode == RUN_AGENT && !g_testSrc) agent_follow_input_desktop();
    if (g_testSrc) {
        g_mode = MODE_TEST;
        lstrcpyW(g_scr.method, L"検証用");
        return test_open();
    }
    if (!g_forceGdi && dxgi_open()) {
        g_mode = MODE_DXGI;
        lstrcpyW(g_scr.method, L"DXGI");
        return TRUE;
    }
    if (gdi_open()) {
        g_mode = MODE_GDI;
        lstrcpyW(g_scr.method, L"GDI");
        return TRUE;
    }
    return FALSE;
}

static void wake_hungry(void)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next)
        if (c->hungry) SetEvent(c->hWake);
    ReleaseSRWLockShared(&g_scr.lock);
}

static DWORD WINAPI capture_thread(void *arg)
{
    DWORD idleSince = GetTickCount();
    int   gdiFrames = 0;
    (void)arg;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!g_quit) {
        Client *c;
        BOOL anyClient = FALSE, anyHungry = FALSE;
        int  r;

        AcquireSRWLockShared(&g_scr.lock);
        for (c = g_scr.clients; c; c = c->next) {
            if (!c->active) continue;
            anyClient = TRUE;
            if (c->hungry) anyHungry = TRUE;
        }
        ReleaseSRWLockShared(&g_scr.lock);

        if (anyClient) idleSince = GetTickCount();
        if (!anyHungry) {
            DWORD to = anyClient ? 1000 : g_mode != MODE_NONE ? 3000 : INFINITE;
            if (WaitForSingleObject(g_scr.hHungry, to) == WAIT_TIMEOUT && !anyClient &&
                g_mode != MODE_NONE && GetTickCount() - idleSince >= 3000) {
                close_capture();
                log_printf(L"接続が無いので取り込みを止めた");
            }
            continue;
        }

        if (InterlockedExchange(&g_reset, 0) && g_mode != MODE_NONE) close_capture();
        /* サービスの分身: 入力デスクトップが替わったら開き直す(GDI は替わっても失敗しない) */
        if (g_runMode == RUN_AGENT && g_mode != MODE_NONE && !g_testSrc) {
            static DWORD lastCheck;
            if (GetTickCount() - lastCheck >= 250) {
                lastCheck = GetTickCount();
                if (agent_input_desktop_changed()) {
                    log_printf(L"[agent] 入力デスクトップが替わった");
                    close_capture();
                }
            }
        }
        if (g_mode == MODE_NONE) {
            if (!open_capture()) {
                Sleep(500);
                continue;
            }
        }

        if (g_cfg.maxFps > 0) {
            DWORD gap = 1000 / (DWORD)g_cfg.maxFps, el = GetTickCount() - g_lastGrab;
            if (el < gap) Sleep(gap - el);
        }
        g_lastGrab = GetTickCount();

        switch (g_mode) {
        case MODE_DXGI: r = dxgi_grab(100); break;
        case MODE_GDI: {
            static DWORD lastGdi;
            DWORD el = GetTickCount() - lastGdi;
            if (el < 33) Sleep(33 - el);
            lastGdi = GetTickCount();
            r = gdi_grab();
            /* GDI のままでいる必要が無くなったか、ときどき確かめる */
            if (!g_forceGdi && ++gdiFrames % 150 == 0) {
                close_capture();
                continue;
            }
            break;
        }
        case MODE_TEST: r = test_grab(); break;
        default: r = 0;
        }
        if (r < 0) {
            close_capture();
            Sleep(100);
            continue;
        }
        if (r > 0) wake_hungry();
    }
    close_capture();
    test_close();
    return 0;
}

void capture_reset(void)
{
    InterlockedExchange(&g_reset, 1);
    SetEvent(g_scr.hHungry);
}

void capture_kick(void)
{
    SetEvent(g_scr.hHungry);
}

void capture_init(void)
{
    InitializeSRWLock(&g_scr.lock);
    g_scr.hHungry = CreateEventW(NULL, FALSE, FALSE, NULL);
    lstrcpyW(g_scr.method, L"-");
    query_size();
    g_thread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
}

void capture_shutdown(void)
{
    if (!g_thread) return;
    InterlockedExchange(&g_quit, 1);
    SetEvent(g_scr.hHungry);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
}

/* server.c が接続の初期化の前に呼ぶ */
void capture_prepare_client(void)
{
    query_size();
}
