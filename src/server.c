/* ==================================================================
 * server.c - 待ち受けと RFB の手順
 *
 *  接続ごとにスレッドを 2 本使う。
 *    読み手  初期化の手順と、相手からのメッセージ(入力、要求、設定)
 *    書き手  要求があり、送るもの(変化、カーソル、クリップボード)が
 *            あるときだけ符号化して送る。送るものが無ければ「待っている」
 *            印を立てて取り込みのスレッドを起こす(capture.c)。
 *
 *  RFB 3.3 / 3.7 / 3.8。認証は VNC 認証(パスワード)。パスワードが無い
 *  ときは 127.0.0.1 でしか待ち受けない(main.c が止める)。
 *  同じ IP から 60 秒に 5 回失敗したら、その IP を 60 秒締め出す。
 *
 *  対応するエンコーディング: Tight(JPEG あり)、ZRLE、Raw、CopyRect。
 *  疑似エンコーディング: カーソル、カーソル位置、画面の大きさ(2 種)、
 *  画質・圧縮の強さ、QEMU のキー(スキャン コード)、拡張クリップボード。
 * ================================================================== */

#include "iivnc.h"
#include "vncdes.h"
#include "jpegenc.h"
#include <bcrypt.h>

volatile LONG g_clientCount;

static SOCKET         g_listen[2] = { INVALID_SOCKET, INVALID_SOCKET };
static int            g_nlisten;
static HANDLE         g_acceptThread;
static volatile LONG  g_stopping;
static volatile LONG  g_nextId;
static CRITICAL_SECTION g_failCs;
static BOOL           g_inited;

typedef struct { char ip[64]; int fails; DWORD first, blockedUntil; } FailRec;
static FailRec g_fails[32];

void capture_prepare_client(void);

/* ------------------------------------------------------------------ */
/*  送受信                                                              */
/* ------------------------------------------------------------------ */

static BOOL rd(Client *c, void *buf, int n)
{
    char *p = (char *)buf;
    while (n > 0) {
        int r = recv(c->s, p, n, 0);
        if (r <= 0 || c->quit) return FALSE;
        p += r;
        n -= r;
    }
    return TRUE;
}

static BOOL skip(Client *c, unsigned n)
{
    char tmp[4096];
    while (n) {
        int k = n > sizeof(tmp) ? (int)sizeof(tmp) : (int)n;
        if (!rd(c, tmp, k)) return FALSE;
        n -= (unsigned)k;
    }
    return TRUE;
}

static unsigned be16(const BYTE *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned be32(const BYTE *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

BOOL client_send(Client *c, const void *data, int len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        int r;
        if (c->quit) return FALSE;
        r = send(c->s, p, len, 0);
        if (r <= 0) {
            InterlockedExchange(&c->quit, 1);
            return FALSE;
        }
        p += r;
        len -= r;
        InterlockedAdd64(&c->bytesSent, r);
    }
    return TRUE;
}

static BOOL send_locked(Client *c, const void *data, int len)
{
    BOOL ok;
    EnterCriticalSection(&c->sendLock);
    ok = client_send(c, data, len);
    LeaveCriticalSection(&c->sendLock);
    return ok;
}

void client_wake(Client *c) { SetEvent(c->hWake); }

static void kill_client(Client *c)
{
    InterlockedExchange(&c->quit, 1);
    shutdown(c->s, SD_BOTH);
    SetEvent(c->hWake);
}

/* ------------------------------------------------------------------ */
/*  締め出し                                                            */
/* ------------------------------------------------------------------ */

static FailRec *fail_rec(const char *ip, BOOL create)
{
    int i, oldest = 0;
    for (i = 0; i < 32; i++) if (!strcmp(g_fails[i].ip, ip)) return &g_fails[i];
    if (!create) return NULL;
    for (i = 1; i < 32; i++) if (g_fails[i].first < g_fails[oldest].first) oldest = i;
    ZeroMemory(&g_fails[oldest], sizeof(FailRec));
    lstrcpynA(g_fails[oldest].ip, ip, sizeof(g_fails[oldest].ip));
    return &g_fails[oldest];
}

static BOOL is_blocked(const char *ip)
{
    FailRec *f;
    BOOL b = FALSE;
    EnterCriticalSection(&g_failCs);
    f = fail_rec(ip, FALSE);
    if (f && f->blockedUntil && (LONG)(f->blockedUntil - GetTickCount()) > 0) b = TRUE;
    LeaveCriticalSection(&g_failCs);
    return b;
}

static void record_fail(const char *ip, BOOL failed)
{
    FailRec *f;
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_failCs);
    f = fail_rec(ip, failed);
    if (f && !failed) ZeroMemory(f, sizeof(*f));
    else if (f) {
        if (now - f->first > 60000) { f->first = now; f->fails = 0; }
        if (!f->fails) f->first = now;
        if (++f->fails >= 5) {
            f->blockedUntil = now + 60000;
            log_printfA("%s: 認証の失敗が続いたので 60 秒締め出す", ip);
        }
    }
    LeaveCriticalSection(&g_failCs);
}

/* ------------------------------------------------------------------ */
/*  初期化の手順                                                        */
/* ------------------------------------------------------------------ */

static void send_reason(Client *c, const char *reason)
{
    BYTE n[4];
    unsigned len = (unsigned)strlen(reason);
    n[0] = (BYTE)(len >> 24); n[1] = (BYTE)(len >> 16); n[2] = (BYTE)(len >> 8); n[3] = (BYTE)len;
    client_send(c, n, 4);
    client_send(c, reason, (int)len);
}

static void put_pf(BYTE *p, const PixFmt *pf)
{
    p[0] = (BYTE)pf->bpp; p[1] = (BYTE)pf->depth; p[2] = (BYTE)pf->bigEndian; p[3] = (BYTE)pf->trueColor;
    p[4] = (BYTE)(pf->rmax >> 8); p[5] = (BYTE)pf->rmax;
    p[6] = (BYTE)(pf->gmax >> 8); p[7] = (BYTE)pf->gmax;
    p[8] = (BYTE)(pf->bmax >> 8); p[9] = (BYTE)pf->bmax;
    p[10] = (BYTE)pf->rshift; p[11] = (BYTE)pf->gshift; p[12] = (BYTE)pf->bshift;
    p[13] = p[14] = p[15] = 0;
}

static BOOL handshake(Client *c, const char *ip)
{
    BYTE  v[12], b[16], resp[16], ch[16];
    int   minor, sec;
    BOOL  hasPw = g_cfg.password[0] || g_cfg.viewPassword[0], ok = FALSE;

    if (!client_send(c, "RFB 003.008\n", 12) || !rd(c, v, 12)) return FALSE;
    if (memcmp(v, "RFB ", 4) || v[7] != '.') return FALSE;
    minor = atoi((const char *)v + 8);
    if (atoi((const char *)v + 4) != 3) return FALSE;
    minor = minor >= 8 ? 8 : minor == 7 ? 7 : 3;

    if (is_blocked(ip)) {
        if (minor == 3) { BYTE z[4] = { 0 }; client_send(c, z, 4); }
        else { BYTE z = 0; client_send(c, &z, 1); }
        send_reason(c, "Too many authentication failures");
        return FALSE;
    }

    sec = hasPw ? 2 : 1;
    if (minor == 3) {
        BYTE t[4] = { 0, 0, 0, (BYTE)sec };
        if (!client_send(c, t, 4)) return FALSE;
    } else {
        BYTE t[2] = { 1, (BYTE)sec }, chosen;
        if (!client_send(c, t, 2) || !rd(c, &chosen, 1)) return FALSE;
        if (chosen != sec) return FALSE;
    }

    if (sec == 2) {
        char pwA[9] = { 0 }, pwV[9] = { 0 };
        BYTE expA[16], expV[16];
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, ch, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return FALSE;
        if (!client_send(c, ch, 16) || !rd(c, resp, 16)) return FALSE;
        lstrcpynA(pwA, g_cfg.password, 9);
        lstrcpynA(pwV, g_cfg.viewPassword, 9);
        if (pwA[0]) { vncdes_response(pwA, ch, expA); if (!memcmp(expA, resp, 16)) ok = TRUE; }
        if (!ok && pwV[0]) { vncdes_response(pwV, ch, expV); if (!memcmp(expV, resp, 16)) { ok = TRUE; c->viewOnly = TRUE; } }
        SecureZeroMemory(pwA, sizeof(pwA));
        SecureZeroMemory(pwV, sizeof(pwV));
        record_fail(ip, !ok);
        if (!ok) {
            BYTE r[4] = { 0, 0, 0, 1 };
            client_send(c, r, 4);
            if (minor == 8) send_reason(c, "Authentication failed");
            log_printf(L"%s: パスワードが違う", c->addr);
            return FALSE;
        }
        { BYTE r[4] = { 0 }; if (!client_send(c, r, 4)) return FALSE; }
    } else if (minor == 8) {
        BYTE r[4] = { 0 };
        if (!client_send(c, r, 4)) return FALSE;
    }
    if (g_cfg.viewOnly) c->viewOnly = TRUE;

    /* ClientInit: 共有しないなら、ほかの接続を切る */
    if (!rd(c, b, 1)) return FALSE;
    if (!b[0]) {
        Client *o;
        AcquireSRWLockShared(&g_scr.lock);
        for (o = g_scr.clients; o; o = o->next) if (o != c) kill_client(o);
        ReleaseSRWLockShared(&g_scr.lock);
    }

    /* ServerInit */
    capture_prepare_client();
    {
        WCHAR  host[MAX_COMPUTERNAME_LENGTH + 1], name[128];
        DWORD  hn = ARRAYSIZE(host);
        char  *u;
        int    ulen;
        BYTE   si[24];
        if (!GetComputerNameW(host, &hn)) lstrcpyW(host, L"?");
        wsprintfW(name, L"iivnc - %s", host);
        u = utf16_to_utf8(name, &ulen);
        AcquireSRWLockExclusive(&g_scr.lock);
        si[0] = (BYTE)(g_scr.w >> 8); si[1] = (BYTE)g_scr.w;
        si[2] = (BYTE)(g_scr.h >> 8); si[3] = (BYTE)g_scr.h;
        put_pf(si + 4, &k_nativePf);
        si[20] = (BYTE)(ulen >> 24); si[21] = (BYTE)(ulen >> 16); si[22] = (BYTE)(ulen >> 8); si[23] = (BYTE)ulen;
        /* ここで一覧に入れ、取り込みの変化を受け取り始める */
        c->dirty = (BYTE *)calloc((size_t)g_scr.bw * g_scr.bh, 1);
        capture_mark_all(c);
        c->cursorVer = -1;
        c->cursorSent.x = c->cursorSent.y = -1;
        c->next = g_scr.clients;
        g_scr.clients = c;
        c->active = TRUE;
        ReleaseSRWLockExclusive(&g_scr.lock);
        ok = client_send(c, si, 24) && client_send(c, u, ulen);
        free(u);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/*  相手からのメッセージ                                                */
/* ------------------------------------------------------------------ */

static void set_pixel_format(Client *c, const BYTE *p)
{
    PixFmt pf;
    pf.bpp = p[0]; pf.depth = p[1]; pf.bigEndian = p[2] != 0; pf.trueColor = p[3] != 0;
    pf.rmax = (int)be16(p + 4); pf.gmax = (int)be16(p + 6); pf.bmax = (int)be16(p + 8);
    pf.rshift = p[10]; pf.gshift = p[11]; pf.bshift = p[12];
    if (pf.bpp != 8 && pf.bpp != 16 && pf.bpp != 32) { kill_client(c); return; }
    if (!pf.trueColor) {
        /* 色の表を使う形式: BGR233 の表を送り、その番号で表す */
        pf.trueColor = 1;
        pf.rmax = 7; pf.gmax = 7; pf.bmax = 3;
        pf.rshift = 0; pf.gshift = 3; pf.bshift = 6;
        EnterCriticalSection(&c->cs);
        c->settingsChanged = TRUE;
        c->colourMapSent = FALSE;
        LeaveCriticalSection(&c->cs);
    }
    if (pf.rmax > 255 || pf.gmax > 255 || pf.bmax > 255) { kill_client(c); return; }
    EnterCriticalSection(&c->cs);
    c->pf = pf;
    if (pf.bpp == 8 && !p[3]) c->colourMapSent = FALSE; else c->colourMapSent = TRUE;
    LeaveCriticalSection(&c->cs);
    AcquireSRWLockExclusive(&g_scr.lock);
    capture_mark_all(c);
    c->copyPending = FALSE;
    ReleaseSRWLockExclusive(&g_scr.lock);
    log_printf(L"%s: 画素形式 %dbpp 深さ %d", c->addr, pf.bpp, pf.depth);
}

static const int k_jpegQ[10] = { 15, 29, 41, 42, 62, 77, 79, 86, 92, 100 };

static void set_encodings(Client *c, const BYTE *list, int n)
{
    int i, pref = -1, quality = -1, fine = -1, subs = -1, comp = -1;
    BOOL cr = 0, rc = 0, pp = 0, ds = 0, eds = 0, qk = 0, ec = 0, lr = 0, hasZ = 0, hasT = 0;
    for (i = 0; i < n; i++) {
        int e = (int)be32(list + i * 4);
        if (e == ENC_TIGHT || e == ENC_ZRLE || e == ENC_RAW) {
            if (pref < 0) pref = e;
            if (e == ENC_TIGHT) hasT = TRUE;
            if (e == ENC_ZRLE) hasZ = TRUE;
        } else if (e == ENC_COPYRECT) cr = TRUE;
        else if (e == PSE_CURSOR) rc = TRUE;
        else if (e == PSE_POINTERPOS) pp = TRUE;
        else if (e == PSE_DESKTOPSIZE) ds = TRUE;
        else if (e == PSE_EXTDESKTOP) eds = TRUE;
        else if (e == PSE_QEMUKEY) qk = TRUE;
        else if (e == PSE_EXTCLIP) ec = TRUE;
        else if (e == PSE_LASTRECT) lr = TRUE;
        else if (e >= PSE_JPEG_Q0 && e <= PSE_JPEG_Q0 + 9) quality = e - PSE_JPEG_Q0;
        else if (e >= PSE_COMPRESS0 && e <= PSE_COMPRESS0 + 9) comp = e - PSE_COMPRESS0;
        else if (e >= PSE_FINEQ0 && e <= PSE_FINEQ0 + 100) fine = e - PSE_FINEQ0;
        else if (e >= PSE_SUBSAMP0 && e <= PSE_SUBSAMP0 + 5) subs = e - PSE_SUBSAMP0;
    }
    EnterCriticalSection(&c->cs);
    c->prefEnc = pref < 0 ? ENC_RAW : pref;
    if (c->prefEnc == ENC_TIGHT && !hasT) c->prefEnc = ENC_RAW;
    c->copyRect = cr; c->richCursor = rc; c->pointerPos = pp; c->desktopSize = ds; c->extDesktop = eds;
    c->lastRect = lr;
    if (qk && !c->qemuKey) c->qemuAckPending = TRUE;
    c->qemuKey = qk;
    if (ec && !c->extClip) c->extClipCapsPending = TRUE;
    c->extClip = ec;
    /* JPEG: 画質の指定があるときだけ使う */
    c->jpegQuality = fine >= 0 ? (fine < 1 ? 1 : fine) : quality >= 0 ? k_jpegQ[quality] : -1;
    if (subs >= 0) c->subsamp = subs == 0 ? JPE_444 : subs == 2 ? JPE_422 : JPE_420;   /* 1X / 2X / それ以上 */
    else c->subsamp = quality < 0 ? JPE_444 : quality >= 7 ? JPE_444 : quality >= 4 ? JPE_422 : JPE_420;
    c->zlevel = comp >= 0 ? comp : 1;
    if (c->zlevel < 1 && c->prefEnc == ENC_ZRLE) c->zlevel = 1;
    (void)hasZ;
    LeaveCriticalSection(&c->cs);
    log_printf(L"%s: エンコーディング %d、JPEG 画質 %d、圧縮 %d、CopyRect %d、カーソル %d、QEMU キー %d、拡張クリップボード %d",
               c->addr, c->prefEnc, c->jpegQuality, c->zlevel, cr, rc, qk, ec);
    SetEvent(c->hWake);
}

static void fb_request(Client *c, BOOL incremental, int x, int y, int w, int h)
{
    RECT r;
    AcquireSRWLockExclusive(&g_scr.lock);
    SetRect(&r, x, y, x + w, y + h);
    if (r.right > g_scr.w) r.right = g_scr.w;
    if (r.bottom > g_scr.h) r.bottom = g_scr.h;
    if (r.right > r.left && r.bottom > r.top) {
        if (!incremental && c->dirty) {
            int bx, by;
            for (by = r.top / BLOCK; by <= (r.bottom - 1) / BLOCK; by++)
                for (bx = r.left / BLOCK; bx <= (r.right - 1) / BLOCK; bx++) c->dirty[by * g_scr.bw + bx] = 1;
            c->dirtyAny = TRUE;
        }
        if (c->reqPending) UnionRect(&c->reqRect, &c->reqRect, &r);
        else c->reqRect = r;
        c->reqPending = TRUE;
    } else if (!c->reqPending) {
        /* 範囲が空(大きさが変わった直後など)でも、知らせることがあれば送れるように */
        SetRect(&c->reqRect, 0, 0, g_scr.w, g_scr.h);
        c->reqPending = TRUE;
    }
    ReleaseSRWLockExclusive(&g_scr.lock);
    SetEvent(c->hWake);
}

/* 拡張クリップボード */
#define CLIP_TEXT     1u
#define CLIP_CAPS     (1u << 24)
#define CLIP_REQUEST  (1u << 25)
#define CLIP_PEEK     (1u << 26)
#define CLIP_NOTIFY   (1u << 27)
#define CLIP_PROVIDE  (1u << 28)
#define CLIP_MAX      (16u << 20)

static BOOL ext_clip_in(Client *c, const BYTE *p, unsigned n)
{
    unsigned flags;
    if (n < 4) return TRUE;
    flags = be32(p);
    if (flags & CLIP_CAPS) {
        EnterCriticalSection(&c->cs);
        c->clipPeerFlags = flags;
        LeaveCriticalSection(&c->cs);
    } else if (flags & CLIP_REQUEST) {
        if (flags & CLIP_TEXT) {
            EnterCriticalSection(&c->cs);
            c->clipProvidePending = TRUE;
            LeaveCriticalSection(&c->cs);
            SetEvent(c->hWake);
        }
    } else if (flags & CLIP_PEEK) {
        EnterCriticalSection(&c->cs);
        c->clipNotifyPending = TRUE;
        LeaveCriticalSection(&c->cs);
        SetEvent(c->hWake);
    } else if (flags & CLIP_NOTIFY) {
        if (flags & CLIP_TEXT) {
            /* すぐに中身を求める(書き手に頼む) */
            BYTE m[12] = { 3, 0, 0, 0 };
            unsigned len = (unsigned)-4, f = CLIP_REQUEST | CLIP_TEXT;
            m[4] = (BYTE)(len >> 24); m[5] = (BYTE)(len >> 16); m[6] = (BYTE)(len >> 8); m[7] = (BYTE)len;
            m[8] = (BYTE)(f >> 24); m[9] = (BYTE)(f >> 16); m[10] = (BYTE)(f >> 8); m[11] = (BYTE)f;
            send_locked(c, m, 12);
        }
    } else if (flags & CLIP_PROVIDE) {
        BYTE  *out = NULL;
        size_t olen = zi_inflate_all(p + 4, n - 4, &out, CLIP_MAX + 8);
        if (olen != (size_t)-1 && olen >= 4 && (flags & CLIP_TEXT)) {
            unsigned tlen = be32(out);
            if (tlen <= olen - 4) {
                WCHAR *w;
                while (tlen && !out[4 + tlen - 1]) tlen--;          /* 終わりの NUL */
                w = utf8_to_utf16((const char *)out + 4, (int)tlen);
                if (w) PostMessageW(g_mainWnd, WM_APP_SETCLIP, 0, (LPARAM)w);
            }
        }
        free(out);
    }
    return TRUE;
}

static BOOL handle_cut_text(Client *c)
{
    BYTE h[7];
    int  len;
    BYTE *p;
    if (!rd(c, h, 7)) return FALSE;
    len = (int)be32(h + 3);
    if (len < 0) {
        unsigned n = (unsigned)-len;
        if (n > CLIP_MAX + 64) return FALSE;
        p = (BYTE *)malloc(n + 1);
        if (!p || !rd(c, p, (int)n)) { free(p); return FALSE; }
        if (c->extClip) ext_clip_in(c, p, n);
        free(p);
        return TRUE;
    }
    if ((unsigned)len > CLIP_MAX) return skip(c, (unsigned)len);
    p = (BYTE *)malloc((size_t)len + 1);
    if (!p || !rd(c, p, len)) { free(p); return FALSE; }
    if (!c->viewOnly) {
        /* Latin-1。改行は LF なので CRLF にする */
        WCHAR *w = (WCHAR *)malloc(((size_t)len * 2 + 1) * sizeof(WCHAR));
        int i, k = 0;
        if (w) {
            for (i = 0; i < len; i++) {
                if (p[i] == '\n' && (i == 0 || p[i - 1] != '\r')) w[k++] = L'\r';
                w[k++] = p[i];
            }
            w[k] = 0;
            PostMessageW(g_mainWnd, WM_APP_SETCLIP, 0, (LPARAM)w);
        }
    }
    free(p);
    return TRUE;
}

static BOOL message_loop(Client *c)
{
    BYTE t, b[32];
    for (;;) {
        if (!rd(c, &t, 1)) return FALSE;
        switch (t) {
        case 0:                         /* SetPixelFormat */
            if (!rd(c, b, 19)) return FALSE;
            set_pixel_format(c, b + 3);
            break;
        case 2: {                       /* SetEncodings */
            int n;
            BYTE *list;
            if (!rd(c, b, 3)) return FALSE;
            n = (int)be16(b + 1);
            list = (BYTE *)malloc((size_t)n * 4 + 4);
            if (!list || !rd(c, list, n * 4)) { free(list); return FALSE; }
            set_encodings(c, list, n);
            free(list);
            break;
        }
        case 3:                         /* FramebufferUpdateRequest */
            if (!rd(c, b, 9)) return FALSE;
            fb_request(c, b[0] != 0, (int)be16(b + 1), (int)be16(b + 3), (int)be16(b + 5), (int)be16(b + 7));
            break;
        case 4:                         /* KeyEvent */
            if (!rd(c, b, 7)) return FALSE;
            if (!c->viewOnly) input_key(c, b[0] != 0, be32(b + 3));
            break;
        case 5: {                       /* PointerEvent */
            int x, y;
            if (!rd(c, b, 5)) return FALSE;
            x = (int)be16(b + 1);
            y = (int)be16(b + 3);
            if (!c->viewOnly) {
                AcquireSRWLockExclusive(&g_scr.lock);
                c->pointerFromClient.x = x;
                c->pointerFromClient.y = y;
                ReleaseSRWLockExclusive(&g_scr.lock);
                input_pointer(c, b[0], x, y);
            }
            break;
        }
        case 6:                         /* ClientCutText */
            if (!handle_cut_text(c)) return FALSE;
            break;
        case 150:                       /* EnableContinuousUpdates(知らせていないので無視) */
            if (!rd(c, b, 9)) return FALSE;
            break;
        case 248: {                     /* ClientFence(同上) */
            if (!rd(c, b, 8)) return FALSE;
            if (!skip(c, b[7])) return FALSE;
            break;
        }
        case 251: {                     /* SetDesktopSize: 変えられないと答える */
            if (!rd(c, b, 7)) return FALSE;
            if (!skip(c, (unsigned)b[5] * 16)) return FALSE;
            break;
        }
        case 255:                       /* QEMU */
            if (!rd(c, b, 1)) return FALSE;
            if (b[0] == 0) {
                if (!rd(c, b + 1, 10)) return FALSE;
                if (!c->viewOnly) input_qemu_key(c, be16(b + 1) != 0, be32(b + 3), be32(b + 7));
            } else if (b[0] == 1) {
                if (!rd(c, b + 1, 2)) return FALSE;
            } else {
                return FALSE;
            }
            break;
        default:
            log_printf(L"%s: 知らないメッセージ %d", c->addr, t);
            return FALSE;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  書き手                                                              */
/* ------------------------------------------------------------------ */

static void put32be(BYTE *p, unsigned v) { p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v; }

static void send_clipboard(Client *c)
{
    char    *text = NULL;
    int      len = 0;
    BOOL     notify, provide, ext, caps;
    unsigned peer;

    EnterCriticalSection(&c->cs);
    caps    = c->extClipCapsPending;
    notify  = c->clipNotifyPending;
    provide = c->clipProvidePending;
    ext     = c->extClip;
    peer    = c->clipPeerFlags;
    c->extClipCapsPending = c->clipNotifyPending = c->clipProvidePending = FALSE;
    if (c->clipOut && !ext) { text = c->clipOut; len = c->clipOutLen; c->clipOut = NULL; }
    LeaveCriticalSection(&c->cs);

    if (caps) {
        BYTE m[16] = { 3, 0, 0, 0 };
        put32be(m + 4, (unsigned)-8);
        put32be(m + 8, CLIP_CAPS | CLIP_TEXT | CLIP_REQUEST | CLIP_PEEK | CLIP_NOTIFY | CLIP_PROVIDE);
        put32be(m + 12, CLIP_MAX);
        send_locked(c, m, 16);
    }
    if (text) {
        /* 昔からの形: Latin-1、改行は LF */
        WCHAR *w = utf8_to_utf16(text, len);
        if (w) {
            int   i, k = 0, n = lstrlenW(w);
            BYTE *m = (BYTE *)malloc((size_t)n + 8);
            if (m) {
                for (i = 0; i < n; i++) {
                    if (w[i] == L'\r') continue;
                    m[8 + k++] = w[i] < 256 ? (BYTE)w[i] : '?';
                }
                m[0] = 3; m[1] = m[2] = m[3] = 0;
                put32be(m + 4, (unsigned)k);
                send_locked(c, m, k + 8);
                free(m);
            }
            free(w);
        }
        free(text);
    }
    if (notify && ext && (peer & CLIP_NOTIFY || !peer)) {
        BYTE m[12] = { 3, 0, 0, 0 };
        put32be(m + 4, (unsigned)-4);
        put32be(m + 8, CLIP_NOTIFY | CLIP_TEXT);
        send_locked(c, m, 12);
    }
    if (provide && ext) {
        char  *cur = NULL;
        int    clen = 0;
        clip_get_current(&cur, &clen);
        if (cur) {
            /* 中身: U32 長さ + UTF-8(CRLF、終わりに NUL)を新しい zlib ストリームで */
            size_t  rawLen = (size_t)clen + 1 + 4;
            BYTE   *raw = (BYTE *)malloc(rawLen), *z;
            ZDWork *zw = zd_work_new();
            if (raw && zw) {
                put32be(raw, (unsigned)(clen + 1));
                memcpy(raw + 4, cur, (size_t)clen);
                raw[4 + clen] = 0;
                z = (BYTE *)malloc(zd_bound(rawLen) + 16);
                if (z) {
                    size_t zl = zd_compress(zw, raw, 0, rawLen, z + 12, 1, 1);
                    z[0] = 3; z[1] = z[2] = z[3] = 0;
                    put32be(z + 4, (unsigned)-(int)(zl + 4));
                    put32be(z + 8, CLIP_PROVIDE | CLIP_TEXT);
                    send_locked(c, z, (int)zl + 12);
                    free(z);
                }
            }
            free(raw);
            zd_work_free(zw);
            free(cur);
        }
    }
}

/* 色の表(BGR233) */
static void send_colour_map(Client *c)
{
    BYTE *m = (BYTE *)malloc(6 + 256 * 6);
    int   i;
    if (!m) return;
    m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 1; m[5] = 0;
    for (i = 0; i < 256; i++) {
        unsigned r = (unsigned)(i & 7) * 65535 / 7, g = (unsigned)((i >> 3) & 7) * 65535 / 7, b = (unsigned)((i >> 6) & 3) * 65535 / 3;
        BYTE *p = m + 6 + i * 6;
        p[0] = (BYTE)(r >> 8); p[1] = (BYTE)r; p[2] = (BYTE)(g >> 8); p[3] = (BYTE)g; p[4] = (BYTE)(b >> 8); p[5] = (BYTE)b;
    }
    send_locked(c, m, 6 + 256 * 6);
    free(m);
}

static void pseudo_header(Buf *b, int x, int y, int w, int h, int enc)
{
    buf_u16(b, x); buf_u16(b, y); buf_u16(b, w); buf_u16(b, h); buf_u32(b, (unsigned)enc);
}

/* dirty のブロックを矩形にまとめる(要求の範囲で切る)。
   要求の範囲に収まるブロックの印は消す。g_scr.lock を持って呼ぶ */
static int collect_rects(Client *c, const RECT *req, RECT **out, int *cap)
{
    int bx0 = req->left / BLOCK, by0 = req->top / BLOCK, bx1 = (req->right - 1) / BLOCK, by1 = (req->bottom - 1) / BLOCK;
    int n = 0, by, bx, nopen = 0, i;
    typedef struct { int x0, x1, y0; BOOL used; } Open;
    Open open[512];

    for (by = by0; by <= by1 + 1; by++) {
        int  runs[512][2], nr = 0;
        BOOL usedNow[512];
        if (by <= by1) {
            for (bx = bx0; bx <= bx1; bx++) {
                if (!c->dirty[by * g_scr.bw + bx]) continue;
                if (nr && runs[nr - 1][1] == bx) runs[nr - 1][1] = bx + 1;
                else if (nr < 512) { runs[nr][0] = bx; runs[nr][1] = bx + 1; nr++; }
            }
        }
        for (i = 0; i < nopen; i++) open[i].used = FALSE;
        for (i = 0; i < nr; i++) {
            int k;
            usedNow[i] = FALSE;
            for (k = 0; k < nopen; k++)
                if (!open[k].used && open[k].x0 == runs[i][0] && open[k].x1 == runs[i][1]) { open[k].used = TRUE; usedNow[i] = TRUE; break; }
        }
        /* 続かなかった矩形を閉じる */
        for (i = 0; i < nopen;) {
            if (open[i].used) { i++; continue; }
            if (n >= *cap) {
                int nc = *cap ? *cap * 2 : 256;
                RECT *nr2 = (RECT *)realloc(*out, sizeof(RECT) * (size_t)nc);
                if (!nr2) return n;
                *out = nr2;
                *cap = nc;
            }
            SetRect(&(*out)[n], open[i].x0 * BLOCK, open[i].y0 * BLOCK, min(open[i].x1 * BLOCK, g_scr.w), min(by * BLOCK, g_scr.h));
            IntersectRect(&(*out)[n], &(*out)[n], req);
            if (!IsRectEmpty(&(*out)[n])) n++;
            open[i] = open[--nopen];
        }
        for (i = 0; i < nr; i++) {
            if (usedNow[i] || nopen >= 512) continue;
            open[nopen].x0 = runs[i][0];
            open[nopen].x1 = runs[i][1];
            open[nopen].y0 = by;
            open[nopen].used = TRUE;
            nopen++;
        }
    }
    /* 印を消す(ブロックが要求の範囲に収まるものだけ) */
    for (by = by0; by <= by1; by++)
        for (bx = bx0; bx <= bx1; bx++) {
            RECT b;
            SetRect(&b, bx * BLOCK, by * BLOCK, min(bx * BLOCK + BLOCK, g_scr.w), min(by * BLOCK + BLOCK, g_scr.h));
            if (b.left >= req->left && b.top >= req->top && b.right <= req->right && b.bottom <= req->bottom)
                c->dirty[by * g_scr.bw + bx] = 0;
        }
    {
        BOOL any = FALSE;
        int  k, nb = g_scr.bw * g_scr.bh;
        for (k = 0; k < nb; k++) if (c->dirty[k]) { any = TRUE; break; }
        c->dirtyAny = any;
    }
    return n;
}

/* 5 秒ごとに、更新の回数・1 回の符号化と送信にかかった時間・量をログに書く */
static void stat_add(Client *c, LONG64 ticks, LONG64 enc, LONG64 bytes)
{
    static LARGE_INTEGER freq;
    DWORD now = GetTickCount();
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    c->statTicks += ticks;
    c->statEnc += enc;
    c->statBytes += bytes;
    c->statCount++;
    if (ticks > c->statMax) c->statMax = ticks;
    if (!c->statSince) c->statSince = now;
    if (now - c->statSince >= 5000) {
        double sec = (now - c->statSince) / 1000.0;
        log_printf(L"%s: %.1f 回/秒、1 回 符号化 %.2fms(送信まで %.2fms 最大 %.2fms)、平均 %I64d バイト、%.1f Mbps", c->addr,
                   c->statCount / sec, c->statEnc * 1000.0 / (double)freq.QuadPart / c->statCount,
                   c->statTicks * 1000.0 / (double)freq.QuadPart / c->statCount,
                   c->statMax * 1000.0 / (double)freq.QuadPart, c->statBytes / c->statCount, c->statBytes * 8 / sec / 1e6);
        c->statTicks = c->statBytes = c->statMax = c->statEnc = 0;
        c->statCount = 0;
        c->statSince = now;
    }
}

static DWORD WINAPI writer_thread(void *arg)
{
    Client *c = (Client *)arg;
    RECT   *rects = NULL;
    int     rcap = 0;
    Buf     pseudo = { 0 };

    while (!c->quit) {
        RECT  req, copyDst = { 0 };
        POINT copySrc = { 0 };
        BOOL  haveCopy = FALSE, resize = FALSE, sendCursor = FALSE, sendPos = FALSE, qemuAck = FALSE, colourMap;
        int   n = 0, npseudo = 0, w, h, k;
        BYTE *curPix = NULL, *curMask = NULL;
        int   cw = 0, chh = 0, chx = 0, chy = 0, cx = 0, cy = 0;
        BOOL  cvis = FALSE;
        BOOL  wantRich, wantPos, wantDs, wantEds;

        WaitForSingleObject(c->hWake, 1000);
        if (c->quit) break;

        send_clipboard(c);

        EnterCriticalSection(&c->cs);
        wantRich = c->richCursor;
        wantPos  = c->pointerPos;
        wantDs   = c->desktopSize;
        wantEds  = c->extDesktop;
        colourMap = !c->colourMapSent;
        c->colourMapSent = TRUE;
        if (c->qemuAckPending) { qemuAck = TRUE; c->qemuAckPending = FALSE; }
        LeaveCriticalSection(&c->cs);
        if (colourMap) send_colour_map(c);

        AcquireSRWLockShared(&g_scr.lock);
        if (!c->reqPending) {
            c->hungry = FALSE;
            ReleaseSRWLockShared(&g_scr.lock);
            if (qemuAck) { EnterCriticalSection(&c->cs); c->qemuAckPending = TRUE; LeaveCriticalSection(&c->cs); }
            continue;
        }
        if (!g_scr.valid) {
            /* まだ一度も取り込んでいない(fb が今の画面ではない) */
            c->hungry = TRUE;
            ReleaseSRWLockShared(&g_scr.lock);
            if (qemuAck) { EnterCriticalSection(&c->cs); c->qemuAckPending = TRUE; LeaveCriticalSection(&c->cs); }
            SetEvent(g_scr.hHungry);
            continue;
        }
        w = g_scr.w;
        h = g_scr.h;
        /* 要求は画面の大きさが変わる前のものかもしれない。今の画面に切り詰める */
        {
            RECT all;
            SetRect(&all, 0, 0, w, h);
            if (!IntersectRect(&req, &c->reqRect, &all)) req = all;
        }
        if (c->needResize) {
            resize = TRUE;
            c->needResize = FALSE;
        }
        if (wantRich && c->cursorVer != g_scr.curVer && g_scr.curPix) {
            sendCursor = TRUE;
            c->cursorVer = g_scr.curVer;
            cvis = g_scr.curVisible || g_cfg.showCursor;      /* 隠れていても見せる(マウスが無い PC 向け) */
            cw = g_scr.curW; chh = g_scr.curH; chx = g_scr.curHotX; chy = g_scr.curHotY;
            curPix = (BYTE *)malloc((size_t)cw * chh * 4);
            curMask = (BYTE *)malloc((size_t)((cw + 7) / 8) * chh);
            if (curPix && curMask) {
                memcpy(curPix, g_scr.curPix, (size_t)cw * chh * 4);
                memcpy(curMask, g_scr.curMask, (size_t)((cw + 7) / 8) * chh);
            } else sendCursor = FALSE;
        }
        if (wantPos && g_scr.curVisible && (g_scr.curX != c->cursorSent.x || g_scr.curY != c->cursorSent.y)) {
            cx = g_scr.curX; cy = g_scr.curY;
            c->cursorSent.x = cx; c->cursorSent.y = cy;
            /* この相手が自分で動かした位置なら知らせなくてよい */
            if (cx != c->pointerFromClient.x || cy != c->pointerFromClient.y) sendPos = TRUE;
        }
        if (!resize) {
            if (c->copyPending) {
                haveCopy = c->copyRect;
                copyDst = c->copyDst;
                copySrc = c->copySrc;
                c->copyPending = FALSE;
            }
            if (c->dirtyAny) n = collect_rects(c, &req, &rects, &rcap);
        }
        if (!n && !haveCopy && !resize && !sendCursor && !sendPos && !qemuAck) {
            c->hungry = TRUE;
            ReleaseSRWLockShared(&g_scr.lock);
            SetEvent(g_scr.hHungry);
            continue;
        }
        /* 相手が持っている絵(sendfb)を合わせる */
        if (c->sfW != w || c->sfH != h || !c->sendfb) {
            free(c->sendfb);
            c->sendfb = (BYTE *)calloc((size_t)w * h, 4);
            c->sfW = w;
            c->sfH = h;
            if (!c->sendfb) { ReleaseSRWLockShared(&g_scr.lock); break; }
        }
        if (haveCopy) {
            int cwid = copyDst.right - copyDst.left, chei = copyDst.bottom - copyDst.top, y;
            BYTE *fb = c->sendfb;
            if (copyDst.top > copySrc.y)
                for (y = chei - 1; y >= 0; y--)
                    memmove(fb + ((size_t)(copyDst.top + y) * w + copyDst.left) * 4, fb + ((size_t)(copySrc.y + y) * w + copySrc.x) * 4, (size_t)cwid * 4);
            else
                for (y = 0; y < chei; y++)
                    memmove(fb + ((size_t)(copyDst.top + y) * w + copyDst.left) * 4, fb + ((size_t)(copySrc.y + y) * w + copySrc.x) * 4, (size_t)cwid * 4);
        }
        for (k = 0; k < n; k++) {
            int y, rw = (rects[k].right - rects[k].left) * 4;
            for (y = rects[k].top; y < rects[k].bottom; y++)
                memcpy(c->sendfb + ((size_t)y * w + rects[k].left) * 4, g_scr.fb + ((size_t)y * w + rects[k].left) * 4, (size_t)rw);
        }
        c->reqPending = resize;         /* 大きさを知らせた後は、続けて絵を送る */
        c->hungry = FALSE;
        ReleaseSRWLockShared(&g_scr.lock);

        /* 疑似矩形 */
        pseudo.len = 0;
        if (resize) {
            if (wantEds) {
                pseudo_header(&pseudo, 0, 0, w, h, PSE_EXTDESKTOP);
                buf_u8(&pseudo, 1); buf_u8(&pseudo, 0); buf_u8(&pseudo, 0); buf_u8(&pseudo, 0);
                buf_u32(&pseudo, 0);
                buf_u16(&pseudo, 0); buf_u16(&pseudo, 0); buf_u16(&pseudo, w); buf_u16(&pseudo, h);
                buf_u32(&pseudo, 0);
                npseudo++;
            } else if (wantDs) {
                pseudo_header(&pseudo, 0, 0, w, h, PSE_DESKTOPSIZE);
                npseudo++;
            } else {
                log_printf(L"%s: 画面の大きさが変わったが、相手が対応していないので切る", c->addr);
                free(curPix); free(curMask);
                break;
            }
        }
        if (qemuAck) {
            pseudo_header(&pseudo, 0, 0, 0, 0, PSE_QEMUKEY);
            npseudo++;
        }
        if (sendCursor) {
            PixFmt pf;
            int    mb = (cw + 7) / 8;
            EnterCriticalSection(&c->cs);
            pf = c->pf;
            LeaveCriticalSection(&c->cs);
            if (!cvis) {
                /* 見えないカーソル: 1×1 の透明 */
                BYTE px[4] = { 0 };
                pseudo_header(&pseudo, 0, 0, 1, 1, PSE_CURSOR);
                buf_reserve(&pseudo, 8);
                pf_put_pixels(&pf, px, 1, pseudo.p + pseudo.len);
                pseudo.len += (size_t)pf.bpp / 8;
                buf_u8(&pseudo, 0);
            } else {
                pseudo_header(&pseudo, chx, chy, cw, chh, PSE_CURSOR);
                buf_reserve(&pseudo, (size_t)cw * chh * 4);
                pf_put_pixels(&pf, curPix, cw * chh, pseudo.p + pseudo.len);
                pseudo.len += (size_t)cw * chh * (pf.bpp / 8);
                buf_put(&pseudo, curMask, (size_t)mb * chh);
            }
            npseudo++;
        }
        free(curPix);
        free(curMask);
        if (sendPos) {
            pseudo_header(&pseudo, cx, cy, 0, 0, PSE_POINTERPOS);
            npseudo++;
        }

        {
            LARGE_INTEGER t0, t1;
            LONG64 before = c->bytesSent, enc = 0;
            QueryPerformanceCounter(&t0);
            if (!encode_update(c, rects, n, haveCopy, copyDst, copySrc, &pseudo, npseudo, &enc)) break;
            QueryPerformanceCounter(&t1);
            InterlockedIncrement(&c->frames);
            stat_add(c, t1.QuadPart - t0.QuadPart, enc, c->bytesSent - before);
        }
        if (resize) SetEvent(c->hWake);
    }
    InterlockedExchange(&c->quit, 1);
    shutdown(c->s, SD_BOTH);
    free(rects);
    buf_free(&pseudo);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  接続 1 本                                                           */
/* ------------------------------------------------------------------ */

static DWORD WINAPI client_thread(void *arg)
{
    Client *c = (Client *)arg;
    char    ip[64];
    WideCharToMultiByte(CP_UTF8, 0, c->addr, -1, ip, sizeof(ip), NULL, NULL);
    {
        char *colon = strrchr(ip, ':');
        if (colon && strchr(ip, ']')) { char *br = strchr(ip, ']'); *br = 0; memmove(ip, ip + 1, strlen(ip)); }
        else if (colon && strchr(ip, '.') && colon == strchr(ip, ':')) *colon = 0;
    }

    if (handshake(c, ip)) {
        InterlockedIncrement(&g_clientCount);
        PostMessageW(g_mainWnd, WM_APP_CLIENTS, 0, 0);
        log_printf(L"%s: 接続した%s", c->addr, c->viewOnly ? L"(見るだけ)" : L"");
        if (g_cfg.notify) app_notify(L"%s から接続されました%s", c->addr, c->viewOnly ? L"(見るだけ)" : L"");
        c->thrWrite = CreateThread(NULL, 0, writer_thread, c, 0, NULL);
        message_loop(c);
        kill_client(c);
        if (c->thrWrite) { WaitForSingleObject(c->thrWrite, INFINITE); CloseHandle(c->thrWrite); }
        input_release_all(c);
        InterlockedDecrement(&g_clientCount);
        log_printf(L"%s: 切れた(送ったバイト %I64d、更新 %ld 回)", c->addr, c->bytesSent, c->frames);
        if (g_cfg.notify && !g_stopping) app_notify(L"%s との接続が切れました", c->addr);
    }

    /* 一覧から外す */
    AcquireSRWLockExclusive(&g_scr.lock);
    {
        Client **pp;
        for (pp = &g_scr.clients; *pp; pp = &(*pp)->next)
            if (*pp == c) { *pp = c->next; break; }
    }
    ReleaseSRWLockExclusive(&g_scr.lock);
    PostMessageW(g_mainWnd, WM_APP_CLIENTS, 0, 0);

    closesocket(c->s);
    CloseHandle(c->hWake);
    DeleteCriticalSection(&c->sendLock);
    DeleteCriticalSection(&c->cs);
    encode_free(c);
    free(c->dirty);
    free(c->sendfb);
    free(c->clipOut);
    if (c->thrRead) CloseHandle(c->thrRead);
    free(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  待ち受け                                                            */
/* ------------------------------------------------------------------ */

static DWORD WINAPI accept_thread(void *arg)
{
    (void)arg;
    while (!g_stopping) {
        fd_set fs;
        struct timeval tv = { 0, 500000 };
        int i;
        FD_ZERO(&fs);
        for (i = 0; i < g_nlisten; i++) FD_SET(g_listen[i], &fs);
        if (select(0, &fs, NULL, NULL, &tv) <= 0) continue;
        for (i = 0; i < g_nlisten; i++) {
            struct sockaddr_storage sa;
            int    slen = sizeof(sa), one = 1;
            SOCKET s;
            Client *c;
            DWORD  alen;
            if (!FD_ISSET(g_listen[i], &fs)) continue;
            s = accept(g_listen[i], (struct sockaddr *)&sa, &slen);
            if (s == INVALID_SOCKET) continue;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            c = (Client *)calloc(1, sizeof(Client));
            if (!c) { closesocket(s); continue; }
            c->s = s;
            c->id = InterlockedIncrement(&g_nextId);
            alen = ARRAYSIZE(c->addr);
            if (WSAAddressToStringW((struct sockaddr *)&sa, (DWORD)slen, NULL, c->addr, &alen)) lstrcpyW(c->addr, L"?");
            /* ::ffff:1.2.3.4 は 1.2.3.4 と書く */
            if (!wcsncmp(c->addr, L"[::ffff:", 8)) {
                WCHAR *e = wcschr(c->addr, L']');
                if (e) { WCHAR t[80]; *e = 0; wsprintfW(t, L"%s%s", c->addr + 8, e + 1); lstrcpyW(c->addr, t); }
            }
            c->hWake = CreateEventW(NULL, FALSE, FALSE, NULL);
            InitializeCriticalSection(&c->sendLock);
            InitializeCriticalSection(&c->cs);
            c->pf = k_nativePf;
            c->prefEnc = ENC_RAW;
            c->jpegQuality = -1;
            c->zlevel = 1;
            c->colourMapSent = TRUE;
            c->thrRead = CreateThread(NULL, 0, client_thread, c, 0, NULL);
            if (!c->thrRead) {
                closesocket(s);
                CloseHandle(c->hWake);
                free(c);
            }
        }
    }
    return 0;
}

static SOCKET open_listen(int family, const struct sockaddr *sa, int salen)
{
    SOCKET s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    int    zero = 0, one = 1;
    if (s == INVALID_SOCKET) return s;
    if (family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof(zero));
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
    if (bind(s, sa, salen) || listen(s, 8)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

BOOL server_start(void)
{
    if (!g_inited) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        InitializeCriticalSection(&g_failCs);
        g_inited = TRUE;
    }
    g_stopping = 0;
    g_nlisten = 0;
    if (!g_cfg.listen[0]) {
        struct sockaddr_in6 a6;
        struct sockaddr_in  a4;
        ZeroMemory(&a6, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons((u_short)g_cfg.port);
        g_listen[0] = open_listen(AF_INET6, (struct sockaddr *)&a6, sizeof(a6));
        if (g_listen[0] != INVALID_SOCKET) g_nlisten = 1;
        else {
            ZeroMemory(&a4, sizeof(a4));
            a4.sin_family = AF_INET;
            a4.sin_port = htons((u_short)g_cfg.port);
            g_listen[0] = open_listen(AF_INET, (struct sockaddr *)&a4, sizeof(a4));
            if (g_listen[0] != INVALID_SOCKET) g_nlisten = 1;
        }
    } else {
        ADDRINFOW hints, *res = NULL, *ai;
        WCHAR port[16];
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
        wsprintfW(port, L"%d", g_cfg.port);
        if (!GetAddrInfoW(g_cfg.listen, port, &hints, &res)) {
            for (ai = res; ai && g_nlisten < 2; ai = ai->ai_next) {
                SOCKET s = open_listen(ai->ai_family, ai->ai_addr, (int)ai->ai_addrlen);
                if (s != INVALID_SOCKET) g_listen[g_nlisten++] = s;
            }
            FreeAddrInfoW(res);
        }
    }
    if (!g_nlisten) {
        log_printf(L"ポート %d で待ち受けられない (%d)", g_cfg.port, WSAGetLastError());
        return FALSE;
    }
    g_acceptThread = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
    log_printf(L"ポート %d で待ち受ける(%s)", g_cfg.port, g_cfg.listen[0] ? g_cfg.listen : L"すべて");
    return TRUE;
}

void server_disconnect_all(void)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) kill_client(c);
    ReleaseSRWLockShared(&g_scr.lock);
}

void server_disconnect(int id)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) if (c->id == id) kill_client(c);
    ReleaseSRWLockShared(&g_scr.lock);
}

void server_stop(void)
{
    int i;
    DWORD t0;
    InterlockedExchange(&g_stopping, 1);
    for (i = 0; i < g_nlisten; i++) closesocket(g_listen[i]);
    if (g_acceptThread) {
        WaitForSingleObject(g_acceptThread, 2000);
        CloseHandle(g_acceptThread);
        g_acceptThread = NULL;
    }
    g_nlisten = 0;
    server_disconnect_all();
    t0 = GetTickCount();
    for (;;) {
        BOOL any;
        AcquireSRWLockShared(&g_scr.lock);
        any = g_scr.clients != NULL;
        ReleaseSRWLockShared(&g_scr.lock);
        if (!any || GetTickCount() - t0 > 3000) break;
        Sleep(20);
    }
}

int server_list(WCHAR *buf, int cap)
{
    Client *c;
    int n = 0, used = 0;
    buf[0] = 0;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        int k;
        if (!c->active) continue;
        k = _snwprintf(buf + used, (size_t)(cap - used), L"%s%s%s", n ? L"\n" : L"", c->addr, c->viewOnly ? L"(見るだけ)" : L"");
        if (k < 0) break;
        used += k;
        n++;
    }
    ReleaseSRWLockShared(&g_scr.lock);
    return n;
}

void server_clipboard_changed(const char *utf8, int len)
{
    Client *c;
    AcquireSRWLockShared(&g_scr.lock);
    for (c = g_scr.clients; c; c = c->next) {
        if (!c->active) continue;
        EnterCriticalSection(&c->cs);
        if (c->extClip) {
            c->clipNotifyPending = TRUE;
        } else {
            free(c->clipOut);
            c->clipOut = (char *)malloc((size_t)len + 1);
            if (c->clipOut) { memcpy(c->clipOut, utf8, (size_t)len); c->clipOut[len] = 0; c->clipOutLen = len; }
        }
        LeaveCriticalSection(&c->cs);
        SetEvent(c->hWake);
    }
    ReleaseSRWLockShared(&g_scr.lock);
}
