/* ==================================================================
 * input.c - 相手から来たキー・マウスの再現
 *
 *  マウス: 絶対座標で SendInput する。座標の換算は画素の中心を指す
 *  ((2d+1)·65536)/(2w)(input-mouser で実測。d·65535/(w-1) は 3840 幅で
 *  1px ずれる点が出た)。ボタン 4/5 は縦のホイール、6/7 は横、8 は「戻る」。
 *
 *  キー:
 *   - QEMU の拡張キー(相手がスキャン コードも送ってくる)ならスキャン
 *     コードで再現する。物理的なキーの位置がそのまま伝わるので、配列の
 *     違いや IME のキーで迷わない。
 *   - キーシムだけのときは、特殊キーは表で仮想キーへ、文字は
 *     VkKeyScanEx でこちらの配列のキーを探す。必要な Shift / Ctrl / Alt が
 *     今の状態と違えば、そのキーの間だけ押し直す(日本語配列の相手が送る
 *     「@」を英語配列で打つ、など)。見つからない文字は Unicode で打つ。
 *
 *  -dryrun のときは再現せずログに書く。
 *
 *  サービスの分身(SYSTEM)として動いているときは、送る前に入力デスクトップ
 *  (ログイン画面・ロック画面・UAC の確認画面は Winlogon)へスレッドを移す。
 *  Ctrl+Alt+Del は SendInput では起きないので、サービスに SendSAS を頼む。
 *  普通の権限で動いていると、管理者のウィンドウへは届かない(UIPI)。
 * ================================================================== */

#include "iivnc.h"

#define INJECT_MAGIC 0x11A5C0DE

typedef struct Pressed {
    unsigned keysym;
    WORD     vk, scan;
    DWORD    flags;                 /* KEYEVENTF_SCANCODE / EXTENDEDKEY */
} Pressed;

#define MAX_PRESSED 64

typedef struct InputState {
    int     lastMask;
    int     lastX, lastY;
    Pressed keys[MAX_PRESSED];
    int     nkeys;
    BOOL    shift, ctrl, alt;       /* 相手が押している修飾キー */
} InputState;

static CRITICAL_SECTION g_cs;
static volatile LONG    g_csInit;
static InputState       g_st[64];   /* 接続 ID の下位 6 ビットで引く */

static InputState *state_of(Client *c)
{
    if (InterlockedCompareExchange(&g_csInit, 1, 0) == 0) InitializeCriticalSection(&g_cs);
    return &g_st[c->id & 63];
}

static void send_inputs(INPUT *in, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (in[i].type == INPUT_MOUSE) in[i].mi.dwExtraInfo = INJECT_MAGIC;
        else in[i].ki.dwExtraInfo = INJECT_MAGIC;
    }
    if (g_runMode == RUN_AGENT) agent_follow_input_desktop();
    if (g_dryRun) {
        for (i = 0; i < n; i++) {
            if (in[i].type == INPUT_MOUSE)
                log_printf(L"[dryrun] mouse flags=%04lX dx=%ld dy=%ld data=%ld", in[i].mi.dwFlags, in[i].mi.dx, in[i].mi.dy, (long)in[i].mi.mouseData);
            else
                log_printf(L"[dryrun] key vk=%02X scan=%02X flags=%lX", in[i].ki.wVk, in[i].ki.wScan, in[i].ki.dwFlags);
        }
        return;
    }
    SendInput((UINT)n, in, sizeof(INPUT));
}

/* ------------------------------------------------------------------ */
/*  マウス                                                              */
/* ------------------------------------------------------------------ */

void input_pointer(Client *c, int mask, int x, int y)
{
    InputState *s = state_of(c);
    INPUT in[12];
    int   n = 0, changed;
    static const struct { int bit; DWORD down, up, data; } k_btn[] = {
        { 1, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0 },
        { 2, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0 },
        { 4, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0 },
        { 128, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1 },
    };
    int i;

    EnterCriticalSection(&g_cs);
    ZeroMemory(in, sizeof(in));
    if (x != s->lastX || y != s->lastY || !s->lastX) {
        int vl = GetSystemMetrics(SM_XVIRTUALSCREEN), vt = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int px = g_scr.vx + x, py = g_scr.vy + y;
        if (g_dryRun) { vl = 0; vt = 0; vw = 1920; vh = 1080; }
        in[n].type = INPUT_MOUSE;
        in[n].mi.dx = (LONG)(((LONGLONG)(2 * (px - vl) + 1) * 65536) / (2 * (LONGLONG)vw));
        in[n].mi.dy = (LONG)(((LONGLONG)(2 * (py - vt) + 1) * 65536) / (2 * (LONGLONG)vh));
        in[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        n++;
        s->lastX = x;
        s->lastY = y;
    }
    changed = mask ^ s->lastMask;
    for (i = 0; i < 4; i++) {
        if (!(changed & k_btn[i].bit)) continue;
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = (mask & k_btn[i].bit) ? k_btn[i].down : k_btn[i].up;
        in[n].mi.mouseData = k_btn[i].data;
        n++;
    }
    /* ホイールは押したときだけ 1 段 */
    if ((changed & mask) & 8)  { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;  in[n].mi.mouseData = (DWORD)WHEEL_DELTA; n++; }
    if ((changed & mask) & 16) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;  in[n].mi.mouseData = (DWORD)-WHEEL_DELTA; n++; }
    if ((changed & mask) & 32) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL; in[n].mi.mouseData = (DWORD)-WHEEL_DELTA; n++; }
    if ((changed & mask) & 64) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL; in[n].mi.mouseData = (DWORD)WHEEL_DELTA; n++; }
    s->lastMask = mask;
    if (n) send_inputs(in, n);
    LeaveCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------ */
/*  キー                                                                */
/* ------------------------------------------------------------------ */

typedef struct { unsigned keysym; WORD vk; BOOL ext; } KeyMap;

static const KeyMap k_keys[] = {
    { 0xff08, VK_BACK, 0 }, { 0xff09, VK_TAB, 0 }, { 0xfe20, VK_TAB, 0 }, { 0xff0b, VK_CLEAR, 0 },
    { 0xff0d, VK_RETURN, 0 }, { 0xff13, VK_PAUSE, 0 }, { 0xff14, VK_SCROLL, 0 }, { 0xff15, VK_SNAPSHOT, 1 },
    { 0xff1b, VK_ESCAPE, 0 }, { 0xffff, VK_DELETE, 1 },
    { 0xff50, VK_HOME, 1 }, { 0xff51, VK_LEFT, 1 }, { 0xff52, VK_UP, 1 }, { 0xff53, VK_RIGHT, 1 },
    { 0xff54, VK_DOWN, 1 }, { 0xff55, VK_PRIOR, 1 }, { 0xff56, VK_NEXT, 1 }, { 0xff57, VK_END, 1 },
    { 0xff61, VK_SNAPSHOT, 1 }, { 0xff63, VK_INSERT, 1 }, { 0xff67, VK_APPS, 1 }, { 0xff6b, VK_CANCEL, 1 },
    { 0xff7f, VK_NUMLOCK, 1 },
    { 0xff8d, VK_RETURN, 1 }, { 0xff95, VK_HOME, 0 }, { 0xff96, VK_LEFT, 0 }, { 0xff97, VK_UP, 0 },
    { 0xff98, VK_RIGHT, 0 }, { 0xff99, VK_DOWN, 0 }, { 0xff9a, VK_PRIOR, 0 }, { 0xff9b, VK_NEXT, 0 },
    { 0xff9c, VK_END, 0 }, { 0xff9d, VK_CLEAR, 0 }, { 0xff9e, VK_INSERT, 0 }, { 0xff9f, VK_DELETE, 0 },
    { 0xffaa, VK_MULTIPLY, 0 }, { 0xffab, VK_ADD, 0 }, { 0xffac, VK_SEPARATOR, 0 }, { 0xffad, VK_SUBTRACT, 0 },
    { 0xffae, VK_DECIMAL, 0 }, { 0xffaf, VK_DIVIDE, 1 },
    { 0xffb0, VK_NUMPAD0, 0 }, { 0xffb1, VK_NUMPAD1, 0 }, { 0xffb2, VK_NUMPAD2, 0 }, { 0xffb3, VK_NUMPAD3, 0 },
    { 0xffb4, VK_NUMPAD4, 0 }, { 0xffb5, VK_NUMPAD5, 0 }, { 0xffb6, VK_NUMPAD6, 0 }, { 0xffb7, VK_NUMPAD7, 0 },
    { 0xffb8, VK_NUMPAD8, 0 }, { 0xffb9, VK_NUMPAD9, 0 },
    { 0xffe1, VK_LSHIFT, 0 }, { 0xffe2, VK_RSHIFT, 0 }, { 0xffe3, VK_LCONTROL, 0 }, { 0xffe4, VK_RCONTROL, 1 },
    { 0xffe5, VK_CAPITAL, 0 }, { 0xffe7, VK_LMENU, 0 }, { 0xffe8, VK_RMENU, 1 }, { 0xffe9, VK_LMENU, 0 },
    { 0xffea, VK_RMENU, 1 }, { 0xffeb, VK_LWIN, 1 }, { 0xffec, VK_RWIN, 1 }, { 0xfe03, VK_RMENU, 1 },
    { 0xff21, VK_KANJI, 0 }, { 0xff22, VK_NONCONVERT, 0 }, { 0xff23, VK_CONVERT, 0 }, { 0xff27, VK_KANA, 0 },
    { 0xff2a, VK_OEM_AUTO, 0 }, { 0xff30, VK_OEM_ATTN, 0 }, { 0xff25, VK_KANA, 0 }, { 0xff26, VK_KANA, 0 },
    { 0x1008ff12, VK_VOLUME_MUTE, 1 }, { 0x1008ff11, VK_VOLUME_DOWN, 1 }, { 0x1008ff13, VK_VOLUME_UP, 1 },
    { 0x1008ff14, VK_MEDIA_PLAY_PAUSE, 1 }, { 0x1008ff15, VK_MEDIA_STOP, 1 }, { 0x1008ff16, VK_MEDIA_PREV_TRACK, 1 },
    { 0x1008ff17, VK_MEDIA_NEXT_TRACK, 1 }, { 0x1008ff26, VK_BROWSER_BACK, 1 }, { 0x1008ff27, VK_BROWSER_FORWARD, 1 },
    { 0x1008ff29, VK_BROWSER_REFRESH, 1 }, { 0x1008ff1b, VK_BROWSER_SEARCH, 1 }, { 0x1008ff18, VK_BROWSER_HOME, 1 },
    { 0x1008ff19, VK_LAUNCH_MAIL, 1 },
};

/* 日本語配列のときは、IME のキーをスキャン コードで打つ(仮想キーより確か) */
static const struct { unsigned keysym; WORD scan; } k_jpScan[] = {
    { 0xff2a, 0x29 }, { 0xff27, 0x70 }, { 0xff23, 0x79 }, { 0xff22, 0x7B }, { 0xff30, 0x3A },
};

static HKL target_layout(void)
{
    HWND fg = GetForegroundWindow();
    DWORD tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    return GetKeyboardLayout(tid);
}

static void key_event(INPUT *in, WORD vk, WORD scan, DWORD flags)
{
    ZeroMemory(in, sizeof(*in));
    in->type = INPUT_KEYBOARD;
    in->ki.wVk = vk;
    in->ki.wScan = scan;
    in->ki.dwFlags = flags;
}

static Pressed *find_pressed(InputState *s, unsigned keysym)
{
    int i;
    for (i = 0; i < s->nkeys; i++) if (s->keys[i].keysym == keysym) return &s->keys[i];
    return NULL;
}

static void remember(InputState *s, unsigned keysym, WORD vk, WORD scan, DWORD flags)
{
    Pressed *p = find_pressed(s, keysym);
    if (!p && s->nkeys < MAX_PRESSED) p = &s->keys[s->nkeys++];
    if (!p) return;
    p->keysym = keysym; p->vk = vk; p->scan = scan; p->flags = flags;
}

static void forget(InputState *s, Pressed *p)
{
    *p = s->keys[--s->nkeys];
}

static void track_mods(InputState *s, unsigned keysym, BOOL down)
{
    if (keysym == 0xffe1 || keysym == 0xffe2) s->shift = down;
    else if (keysym == 0xffe3 || keysym == 0xffe4) s->ctrl = down;
    else if (keysym == 0xffe9 || keysym == 0xffea || keysym == 0xffe7 || keysym == 0xffe8) s->alt = down;
}

void input_key(Client *c, BOOL down, unsigned keysym)
{
    InputState *s = state_of(c);
    INPUT in[12];
    int   n = 0, i;
    HKL   hkl;
    BOOL  jp;

    if (g_dryRun) log_printf(L"[dryrun-keysym] %s keysym=%X", down ? L"down" : L"up", keysym);
    EnterCriticalSection(&g_cs);
    track_mods(s, keysym, down);
    hkl = target_layout();
    jp = PRIMARYLANGID(LOWORD((DWORD_PTR)hkl)) == LANG_JAPANESE;

    /* Ctrl+Alt+Del: サービスの分身なら SendSAS を頼む */
    if (down && (keysym == 0xffff || keysym == 0xff9f) && s->ctrl && s->alt && g_runMode == RUN_AGENT) {
        agent_request_sas();
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (!down) {
        Pressed *p = find_pressed(s, keysym);
        if (p) {
            key_event(&in[n++], p->vk, p->scan, p->flags | KEYEVENTF_KEYUP);
            forget(s, p);
            send_inputs(in, n);
        }
        LeaveCriticalSection(&g_cs);
        return;
    }

    /* 日本語配列の IME のキー */
    if (jp) {
        for (i = 0; i < (int)ARRAYSIZE(k_jpScan); i++) {
            if (k_jpScan[i].keysym != keysym) continue;
            key_event(&in[n++], 0, k_jpScan[i].scan, KEYEVENTF_SCANCODE);
            remember(s, keysym, 0, k_jpScan[i].scan, KEYEVENTF_SCANCODE);
            send_inputs(in, n);
            LeaveCriticalSection(&g_cs);
            return;
        }
    }
    /* F1..F24 */
    if (keysym >= 0xffbe && keysym <= 0xffd5) {
        WORD vk = (WORD)(VK_F1 + (keysym - 0xffbe)), scan = (WORD)MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl);
        key_event(&in[n++], vk, scan, 0);
        remember(s, keysym, vk, scan, 0);
        send_inputs(in, n);
        LeaveCriticalSection(&g_cs);
        return;
    }
    /* 特殊キー */
    for (i = 0; i < (int)ARRAYSIZE(k_keys); i++) {
        DWORD fl;
        WORD  scan;
        if (k_keys[i].keysym != keysym) continue;
        fl = k_keys[i].ext ? KEYEVENTF_EXTENDEDKEY : 0;
        scan = (WORD)MapVirtualKeyExW(k_keys[i].vk, MAPVK_VK_TO_VSC, hkl);
        key_event(&in[n++], k_keys[i].vk, scan, fl);
        remember(s, keysym, k_keys[i].vk, scan, fl);
        send_inputs(in, n);
        LeaveCriticalSection(&g_cs);
        return;
    }
    /* 文字 */
    {
        unsigned uc = 0;
        if ((keysym >= 0x20 && keysym <= 0x7e) || (keysym >= 0xa0 && keysym <= 0xff)) uc = keysym;
        else if ((keysym & 0xff000000) == 0x01000000) uc = keysym & 0xffffff;
        if (uc && uc < 0x10000) {
            SHORT r = VkKeyScanExW((WCHAR)uc, hkl);
            if (r != -1 && (LOBYTE(r) != 0xFF)) {
                WORD vk = LOBYTE(r);
                int  need = HIBYTE(r);
                BOOL ns = (need & 1) != 0, nc = (need & 2) != 0, na = (need & 4) != 0;
                WORD scan = (WORD)MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl);
                /* 文字に要る修飾キーをそろえる(相手が押している Ctrl / Alt はそのまま) */
                BOOL fixShift = ns != s->shift;
                BOOL addCtrl = nc && !s->ctrl, addAlt = na && !s->alt;
                if (fixShift) key_event(&in[n++], VK_SHIFT, 0x2A, s->shift ? KEYEVENTF_KEYUP : 0);
                if (addCtrl) key_event(&in[n++], VK_CONTROL, 0x1D, 0);
                if (addAlt) key_event(&in[n++], VK_MENU, 0x38, 0);
                key_event(&in[n++], vk, scan, 0);
                if (fixShift || addCtrl || addAlt) {
                    /* 修飾キーを足したときは、離すのもここで済ませる */
                    key_event(&in[n++], vk, scan, KEYEVENTF_KEYUP);
                    if (addAlt) key_event(&in[n++], VK_MENU, 0x38, KEYEVENTF_KEYUP);
                    if (addCtrl) key_event(&in[n++], VK_CONTROL, 0x1D, KEYEVENTF_KEYUP);
                    if (fixShift) key_event(&in[n++], VK_SHIFT, 0x2A, s->shift ? 0 : KEYEVENTF_KEYUP);
                } else {
                    remember(s, keysym, vk, scan, 0);
                }
                send_inputs(in, n);
            } else {
                key_event(&in[n++], 0, (WORD)uc, KEYEVENTF_UNICODE);
                key_event(&in[n++], 0, (WORD)uc, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
                send_inputs(in, n);
            }
        } else if (uc) {
            /* BMP の外: サロゲートの組で */
            WCHAR hi = (WCHAR)(0xD800 + ((uc - 0x10000) >> 10)), lo = (WCHAR)(0xDC00 + ((uc - 0x10000) & 0x3FF));
            key_event(&in[n++], 0, hi, KEYEVENTF_UNICODE);
            key_event(&in[n++], 0, lo, KEYEVENTF_UNICODE);
            key_event(&in[n++], 0, hi, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
            key_event(&in[n++], 0, lo, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
            send_inputs(in, n);
        } else {
            log_printf(L"知らないキーシム 0x%X", keysym);
        }
    }
    LeaveCriticalSection(&g_cs);
}

/* 仮想キーで送るほうが確かなキー(Pause、メディア・ブラウザのキー) */
static BOOL by_vk(unsigned keysym)
{
    return keysym == 0xff13 || (keysym & 0xffff0000) == 0x10080000;
}

void input_qemu_key(Client *c, BOOL down, unsigned keysym, unsigned keycode)
{
    InputState *s = state_of(c);
    INPUT in[2];
    WORD  scan;
    DWORD fl;

    if (g_dryRun) log_printf(L"[dryrun-keysym] %s keysym=%X qnum=%X", down ? L"down" : L"up", keysym, keycode);
    if (!keycode || by_vk(keysym) || keycode == 0xC6) {   /* 0xC6 = Pause */
        if (keycode == 0xC6 && !keysym) keysym = 0xff13;
        input_key(c, down, keysym);
        return;
    }
    EnterCriticalSection(&g_cs);
    track_mods(s, keysym, down);
    if (down && (keycode == 0xD3 || keycode == 0x53) && s->ctrl && s->alt && g_runMode == RUN_AGENT) {
        agent_request_sas();
        LeaveCriticalSection(&g_cs);
        return;
    }
    scan = (WORD)(keycode & 0x7F);
    fl = KEYEVENTF_SCANCODE | ((keycode & 0x80) ? KEYEVENTF_EXTENDEDKEY : 0);
    key_event(&in[0], 0, scan, fl | (down ? 0 : KEYEVENTF_KEYUP));
    if (down) remember(s, 0x80000000u | keycode, 0, scan, fl);
    else {
        Pressed *p = find_pressed(s, 0x80000000u | keycode);
        if (p) forget(s, p);
    }
    send_inputs(in, 1);
    LeaveCriticalSection(&g_cs);
}

void input_release_all(Client *c)
{
    InputState *s = state_of(c);
    INPUT in[MAX_PRESSED + 8];
    int   n = 0, i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < s->nkeys; i++) key_event(&in[n++], s->keys[i].vk, s->keys[i].scan, s->keys[i].flags | KEYEVENTF_KEYUP);
    if (s->lastMask & 1) { ZeroMemory(&in[n], sizeof(INPUT)); in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_LEFTUP; }
    if (s->lastMask & 2) { ZeroMemory(&in[n], sizeof(INPUT)); in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_MIDDLEUP; }
    if (s->lastMask & 4) { ZeroMemory(&in[n], sizeof(INPUT)); in[n].type = INPUT_MOUSE; in[n++].mi.dwFlags = MOUSEEVENTF_RIGHTUP; }
    if (n) {
        send_inputs(in, n);
        log_printf(L"押されたままのキー・ボタン %d 個を離した", n);
    }
    ZeroMemory(s, sizeof(*s));
    LeaveCriticalSection(&g_cs);
}
