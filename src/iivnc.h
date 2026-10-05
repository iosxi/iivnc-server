/* ==================================================================
 * iivnc.h - iivnc-server 全体で使う宣言
 *
 *  構成
 *    main.c     起動、タスクトレイ、多重起動の判定
 *    config.c   iivnc-server.ini の読み書き、ログ
 *    capture.c  画面の取り込み(DXGI の複製 / GDI / 検証用の絵)と変化の記録
 *    pool.c     符号化の作業スレッド
 *    encode.c   画素形式の変換、Raw / CopyRect / ZRLE / Tight の符号化
 *    server.c   待ち受け、RFB の手順、接続ごとの送受信
 *    input.c    キー・マウスの再現
 *    clip.c     クリップボードの受け渡し
 *    ui.c       設定画面
 *    theme.c    ライト/ダークの配色(kotemado と同じもの)
 *    zdeflate.c zinflate.c jpegenc.c vncdes.c  共通部品(依存ライブラリなし)
 * ================================================================== */
#ifndef IIVNC_H
#define IIVNC_H

#ifndef UNICODE
#error "UNICODE を定義してビルドする(build.bat は /DUNICODE を付けている)"
#endif

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "zlite.h"

#define APP_NAME     L"iivnc-server"
#define APP_VERSION  L"1.5.0"
#define APP_VERSION_A "1.5.0"

#define WM_APP_TRAY     (WM_APP + 1)
#define WM_APP_COMMAND  (WM_APP + 2)    /* 別のプロセスから(-exit など) */
#define WM_APP_SETCLIP  (WM_APP + 3)    /* lParam = 相手から来た文字(malloc した WCHAR*) */
#define WM_APP_CLIENTS  (WM_APP + 4)    /* 接続の数が変わった */
#define WM_APP_NOTIFY   (WM_APP + 5)    /* lParam = 知らせる文字(malloc した WCHAR*) */
#define WM_APP_RESTART  (WM_APP + 6)    /* 待ち受けをやり直す(設定が変わった) */
#define WM_APP_RELOAD   (WM_APP + 7)    /* 分身: 設定を読み直す */
#define WM_APP_DISCONNECT (WM_APP + 8)  /* 分身: 全員を切断 */
#define WM_APP_FXOFFER  (WM_APP + 9)    /* 検証用: wParam の相手へ、今クリップボードにあるファイルを渡す */
#define TRAY_CLASS      L"iivnc.Server.Tray"
#define AGENT_TITLE     L"iivnc-server agent"   /* 分身の窓の題(サービスのトレイが探す) */
#define CD_CLIP_TEXT    1                   /* WM_COPYDATA: サービスのトレイ → 分身。利用者がコピーした文字(UTF-16) */
#define CD_CLIP_FILES   2                   /* 同じく、コピーしたファイルの一覧(0 区切り、最後に 0 が 2 つ) */

#define CMD_EXIT 1

/* ------------------------------------------------------------------ */
/*  設定(config.c)                                                     */
/* ------------------------------------------------------------------ */

typedef struct Config {
    int   port;                 /* 待ち受けるポート(既定 5900) */
    WCHAR listen[64];           /* 空 = すべて、"127.0.0.1" など */
    char  password[9];          /* 操作できるパスワード(平文に戻したもの) */
    char  viewPassword[9];      /* 見るだけのパスワード(空なら無し) */
    int   viewOnly;             /* 1 = 誰にも操作させない */
    int   display;              /* 0 = すべての画面、n = \\.\DISPLAYn だけ */
    int   notify;               /* 1 = 接続・切断を通知で知らせる */
    int   showCursor;           /* 1 = Windows がカーソルを隠していても(マウスが無い PC など)相手に見せる */
    int   fxOffer;              /* 検証用(ini の fxoffer=1。保存しない): 相手がつながったら、今クリップボードにあるファイルを渡す */
    int   theme;                /* 0 = システム、1 = ライト、2 = ダーク */
    int   log;
    int   maxFps;               /* 0 = 制限しない */
    int   sasBefore;            /* サービス登録の前の SoftwareSASGeneration(-1 = 無かった、-2 = 控えていない) */
    int   selftest;             /* 検証用: 分身が起動 20 秒後に入力デスクトップを撮ってログに書く */
} Config;

extern Config g_cfg;
extern HINSTANCE g_inst;
extern WCHAR  g_iniPath[MAX_PATH];
extern WCHAR  g_exeDir[MAX_PATH];
extern HWND   g_mainWnd;
extern BOOL   g_dryRun;         /* -dryrun: 入力を再現せずログに書く */
extern int    g_testSrc;        /* -testsrc: 画面の代わりに検証用の絵を出す(1 = 動く、2 = 止まった絵) */
extern int    g_testFrames;     /* -testsrc で動かすフレーム数(0 = ずっと) */
extern int    g_testFps;
extern int    g_testResize;
extern int    g_testCursor;         /* -testcursor: 1 = hidden(形はあるが隠れている)、2 = none(形も無く隠れている) */
extern BOOL   g_forceGdi;       /* -gdi: DXGI を使わず GDI で取り込む(検証用) */     /* -testresize N: N フレーム目で 1280x720 に変える */        /* -testsrc の 1 秒あたりのフレーム数(0 = 求められるだけ) */

void config_init(void);
void config_load(void);
BOOL config_save(void);
void log_open(void);
void log_printf(const WCHAR *fmt, ...);
void log_printfA(const char *fmt, ...);
char  *utf16_to_utf8(const WCHAR *s, int *outLen);
WCHAR *utf8_to_utf16(const char *s, int len);

/* ------------------------------------------------------------------ */
/*  画素形式                                                            */
/* ------------------------------------------------------------------ */

typedef struct PixFmt {
    int bpp, depth, bigEndian, trueColor;
    int rmax, gmax, bmax, rshift, gshift, bshift;
} PixFmt;

extern const PixFmt k_nativePf;   /* 32bpp 深さ 24 リトルエンディアン R16 G8 B0(= BGRX) */

/* ------------------------------------------------------------------ */
/*  接続(server.c)                                                     */
/* ------------------------------------------------------------------ */

#define ENC_RAW       0
#define ENC_COPYRECT  1
#define ENC_RRE       2
#define ENC_HEXTILE   5
#define ENC_TIGHT     7
#define ENC_ZRLE      16
#define PSE_JPEG_Q0       (-32)     /* -32..-23: Tight の画質 0..9 */
#define PSE_DESKTOPSIZE   (-223)
#define PSE_LASTRECT      (-224)
#define PSE_POINTERPOS    (-232)
#define PSE_CURSOR        (-239)
#define PSE_XCURSOR       (-240)
#define PSE_COMPRESS0     (-256)    /* -256..-247: 圧縮の強さ 0..9 */
#define PSE_QEMUKEY       (-258)
#define PSE_EXTDESKTOP    (-308)
#define PSE_FENCE         (-312)
#define PSE_CONTUPDATES   (-313)
#define PSE_FINEQ0        (-512)    /* -512..-412: TurboVNC の JPEG 画質 0..100 */
#define PSE_SUBSAMP0      (-768)    /* -768..-763: TurboVNC の間引き */
#define PSE_EXTCLIP       ((int)0xC0A1E5CE)

typedef struct ZHist {          /* zlib ストリームの直前 32KB(送ったデータの元) */
    zbyte data[ZD_WINDOW];
    int   len;
    int   started;              /* 見出しを送った */
} ZHist;

typedef struct Client Client;
struct Client {
    Client *next;
    int     id;
    SOCKET  s;
    WCHAR   addr[80];
    HANDLE  thrRead, thrWrite, hWake;
    volatile LONG quit;
    CRITICAL_SECTION sendLock;  /* 1 つのメッセージを途中で混ぜない */
    CRITICAL_SECTION cs;        /* 下の「設定」を守る(読み手のスレッドが書く) */
    BOOL    viewOnly;
    BOOL    active;             /* 初期化まで済んだ */

    /* 設定(cs で守る) */
    PixFmt  pf;
    int     prefEnc;            /* ENC_TIGHT / ENC_ZRLE / ENC_RAW */
    BOOL    copyRect, richCursor, pointerPos, desktopSize, extDesktop, qemuKey, extClip, lastRect;
    int     jpegQuality;        /* -1 = JPEG を使わない */
    int     subsamp;            /* JPE_444 など */
    int     zlevel;
    BOOL    settingsChanged;    /* 色の表(colour map)を送り直すなど */

    /* 更新の状態(g_scr.lock で守る) */
    BYTE   *dirty;              /* ブロックごとの変化 */
    BOOL    dirtyAny;
    BOOL    copyPending;
    RECT    copyDst;            /* 画面の座標 */
    POINT   copySrc;
    BOOL    reqPending;
    RECT    reqRect;
    BOOL    hungry;             /* 要求があり、送るものを待っている */
    BOOL    needResize;
    int     cursorVer;          /* 送った形の版 */
    POINT   cursorSent;         /* 送った位置 */
    POINT   pointerFromClient;  /* この相手が最後に動かした位置 */
    BOOL    qemuAckPending, extClipCapsPending;

    /* ファイルのコピー＆貼り付け(cs で守る) */
    BOOL    fileXfer;           /* 相手が iivnc で、ファイルを受け渡せる */
    BOOL    fxHelloPending;     /* 「受け渡せる」を知らせる */
    struct FxMsg *fxHead, *fxTail;  /* 送るのを待っている FX_MSG(書き手のスレッドが送る) */

    /* クリップボード(cs で守る) */
    char   *clipOut;            /* 相手へ送る UTF-8(NULL = 無し) */
    int     clipOutLen;
    BOOL    clipNotifyPending;  /* 拡張: 「変わった」を知らせる */
    BOOL    clipProvidePending; /* 拡張: 要求されたので中身を送る */
    DWORD   clipPeerFlags;      /* 相手の拡張クリップボードの能力 */

    /* 書き手のスレッドだけが使う */
    BYTE   *sendfb;             /* 相手が持っているはずの絵 */
    int     sfW, sfH;
    ZHist   ztight[4];
    ZHist   zzrle;
    BOOL    colourMapSent;
    void   *encState;           /* encode.c の作業領域 */

    /* 統計 */
    volatile LONG64 bytesSent;
    volatile LONG   frames;
    LONG64  statTicks, statBytes, statMax, statEnc;
    int     statCount;
    DWORD   statSince;
};

extern volatile LONG g_clientCount;

BOOL server_start(void);
void server_stop(void);
void server_disconnect_all(void);
void server_disconnect(int id);
int  server_list(WCHAR *buf, int cap);      /* 「アドレス」を改行で並べる。戻り値は数 */
void server_clipboard_changed(const char *utf8, int len);
void server_files_changed(HDROP hd);            /* クリップボードにファイルがコピーされた(メインのスレッド) */
void server_files_changed_paths(const WCHAR *paths);    /* 同じく、パスの一覧で(サービスのトレイから) */
void clip_text_from_tray(const WCHAR *text);    /* 分身: サービスのトレイが読んだ利用者の文字 */
void server_fx_offer_current(int id);           /* 検証用: 今のクリップボードのファイルを id の相手へ */

/* filexfer.c: ファイルのコピー＆貼り付け(iivnc-client と同じファイル) */
#define FX_MSG          105                 /* RFB のメッセージ番号(両向き。iivnc どうしだけで使う) */
#define PSE_IIVNC_FILES ((int)0x69467831)   /* クライアントが「ファイルを受け渡せる」と知らせる疑似エンコーディング */
#define FX_MAX          (16 << 20)          /* 1 つのメッセージの中身の上限 */
enum { FX_HELLO = 1, FX_FILES, FX_READ, FX_DATA };
BOOL fx_make_offer(const int *conns, int nconn, HDROP hd, BYTE **out, int *outLen);
BOOL fx_make_offer_paths(const int *conns, int nconn, const WCHAR *paths, BYTE **out, int *outLen);
WCHAR *fx_hdrop_paths(HDROP hd);                                /* 0 区切りの一覧(HeapFree) */
HANDLE fx_host_user_token(void);                                /* なりすます利用者(無ければ NULL。呼んだ側が閉じる) */
void fx_request(int conn, const BYTE *p, int n);
void fx_deliver(int conn, const BYTE *p, int n);
void fx_conn_closed(int conn);
void fx_offer_received(int conn, const BYTE *p, int n);
BOOL fx_clipboard_is_ours(void);
void fx_stop(void);
BOOL fx_host_send(int conn, int sub, const BYTE *p, int n);    /* server.c: id の相手へ FX_MSG を送る */
BOOL client_send(Client *c, const void *data, int len);   /* sendLock の中で呼ぶ */
void client_wake(Client *c);

/* ------------------------------------------------------------------ */
/*  画面(capture.c)                                                    */
/* ------------------------------------------------------------------ */

#define BLOCK 32                /* 変化を記録する単位(画素) */

typedef struct Screen {
    SRWLOCK lock;
    int     w, h;               /* 取り込む範囲の大きさ */
    int     vx, vy;             /* その左上の、仮想画面での座標 */
    BYTE   *fb;                 /* BGRX、1 行 = w*4 バイト */
    int     bw, bh;             /* ブロックの数 */
    int     sizeVer;
    BOOL    valid;              /* 開いてから一度は取り込んだ(fb が今の画面) */
    /* カーソル */
    int     curVer;
    int     curW, curH, curHotX, curHotY;
    BYTE   *curPix;             /* BGRA */
    BYTE   *curMask;            /* 1 = 見える(1 行 = (curW+7)/8 バイト) */
    int     curX, curY;         /* 取り込む範囲での位置 */
    BOOL    curVisible;
    int     curPosVer;
    /* 接続 */
    Client *clients;
    HANDLE  hHungry;            /* 誰かが絵を待っている */
    WCHAR   method[32];         /* "DXGI" / "GDI" / "検証用" */
} Screen;

extern Screen g_scr;

void capture_init(void);
void capture_shutdown(void);
void capture_mark_all(Client *c);          /* g_scr.lock を持って呼ぶ */
void capture_kick(void);                   /* 待っている人がいるかもしれない */
void capture_prepare_client(void);         /* ServerInit の前に大きさを決める */
void capture_reset(void);                  /* 設定が変わったので開き直す */

/* ------------------------------------------------------------------ */
/*  作業スレッド(pool.c)                                               */
/* ------------------------------------------------------------------ */

typedef void (*PoolFn)(void *ctx, int index, int worker);

typedef struct PoolBatch {
    PoolFn         fn;
    void          *ctx;
    int            count;
    volatile LONG  next;        /* 次に取る番号 */
    volatile LONG  remaining;
    volatile LONG *done;        /* 番号ごとの終わった印(呼び手が用意する) */
    struct PoolBatch *link;
} PoolBatch;

void pool_init(void);
int  pool_workers(void);                    /* 作業領域の数(呼び手のスレッドの分を含む) */
void pool_run(PoolBatch *b, int callerWorker);   /* 呼び手も手伝い、全部終わるまで待つ */
void pool_start(PoolBatch *b);              /* 投げるだけ */
void pool_wait_item(PoolBatch *b, int index, int callerWorker);  /* その番号が終わるまで手伝う */
void pool_finish(PoolBatch *b, int callerWorker);   /* 全部終わるまで手伝う */
int  pool_caller_slot(void);                /* 呼び手のスレッド用の作業領域の番号を借りる */
void pool_release_slot(int slot);
void *pool_zwork(int worker);
void *pool_jpeg(int worker);

/* ------------------------------------------------------------------ */
/*  符号化(encode.c)                                                   */
/* ------------------------------------------------------------------ */

typedef struct Buf {
    BYTE  *p;
    size_t len, cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_put(Buf *b, const void *data, size_t n);
void buf_u8(Buf *b, int v);
void buf_u16(Buf *b, int v);
void buf_u32(Buf *b, unsigned v);
void buf_free(Buf *b);

/* 1 回の更新で送る矩形(画面の座標)を符号化して送る。
   戻り値は送れたか。rects は CopyRect の後に送るもの。 */
BOOL encode_update(Client *c, const RECT *rects, int nrects, BOOL haveCopy, RECT copyDst, POINT copySrc,
                   Buf *pseudo, int npseudo, LONG64 *encTicks);
void encode_free(Client *c);
BOOL pf_is_native(const PixFmt *pf);
void pf_put_pixels(const PixFmt *pf, const BYTE *bgrx, int n, BYTE *out);   /* 相手の形式へ */
int  pf_bytes(const PixFmt *pf);

/* ------------------------------------------------------------------ */
/*  入力(input.c)                                                      */
/* ------------------------------------------------------------------ */

void input_pointer(Client *c, int mask, int x, int y);
void input_key(Client *c, BOOL down, unsigned keysym);
void input_qemu_key(Client *c, BOOL down, unsigned keysym, unsigned keycode);
void input_release_all(Client *c);

/* ------------------------------------------------------------------ */
/*  クリップボード(clip.c)                                             */
/* ------------------------------------------------------------------ */

void clip_init(HWND hwnd);
void clip_on_update(HWND hwnd);
void clip_set_from_remote(HWND hwnd, WCHAR *text);  /* text は free する */
void clip_get_current(char **utf8, int *len);       /* 今の内容(malloc)。無ければ NULL */

/* ------------------------------------------------------------------ */
/*  画面まわり(ui.c / theme.c)                                         */
/* ------------------------------------------------------------------ */

void ui_show_settings(HWND owner);
BOOL ui_dialog_message(MSG *msg);
BOOL ui_settings_open(void);
void ui_refresh_status(void);
void ui_theme_changed(void);

void     theme_init(void);
BOOL     theme_refresh(void);
BOOL     theme_is_dark(void);
COLORREF theme_back(void);
COLORREF theme_footer(void);
COLORREF theme_ctrl_back(void);
COLORREF theme_text(void);
COLORREF theme_dim_text(void);
COLORREF theme_line(void);
HBRUSH   theme_back_brush(void);
HBRUSH   theme_footer_brush(void);
HBRUSH   theme_ctrl_brush(void);
void     theme_allow_dark(HWND hwnd);
void     theme_apply_dialog(HWND dlg);
LRESULT  theme_ctlcolor(UINT msg, HDC dc, HWND ctl, BOOL dimText);
BOOL     theme_custom_draw_button(NMCUSTOMDRAW *cd, LRESULT *result);

/* ------------------------------------------------------------------ */
/*  サービス(svc.c)                                                    */
/* ------------------------------------------------------------------ */

enum { RUN_NORMAL, RUN_AGENT };
extern int  g_runMode;
extern BOOL g_uiService;        /* 設定画面がサービスの設定(管理者)として開いている */

typedef struct SvcStatus {      /* 分身 → トレイ・設定画面(共有メモリ) */
    LONG  version, seq;
    LONG  listening, port, clients, notifySeq;
    WCHAR method[32];
    WCHAR listen[64];
    WCHAR error[160];
    WCHAR notify[256];
    WCHAR clientList[2048];
} SvcStatus;

int  svc_service_main(void);                /* -service */
int  svc_tray_main(int cmd);                /* -tray */
int  svc_install_cmd(void);                 /* -install-service(管理者) */
BOOL svc_uninstall(HWND owner);             /* 管理者で */
BOOL svc_installed(void);                   /* この ini で登録されているか */
BOOL svc_path_risky(WCHAR *who, int cap);
BOOL svc_run_elevated(const WCHAR *args, HWND owner, BOOL wait, DWORD *exitCode);
BOOL svc_read_status(SvcStatus *st);
void svc_signal_disconnect(void);
void svc_signal_reload(void);
void svc_firewall(BOOL add);
#define FW_RULE L"iivnc-server (VNC)"       /* サービスのときに足す規則の名前 */

/* fwrules.c: Windows ファイアウォールの、この exe の規則(iivnc-client と同じファイル) */
typedef struct { int count, allow, block; long allowProfiles, blockProfiles; } FwInfo;
BOOL fw_query(const WCHAR *keep, FwInfo *fi);                   /* keep: 数えない規則の名前(NULL 可) */
int  fw_remove(const WCHAR *keep);                              /* 管理者で。消した数、-1 = 失敗 */
int  fw_remove_elevated(HWND owner, const WCHAR *keep, const WCHAR *args);
void fw_describe(const FwInfo *fi, WCHAR *s, int cap);
BOOL agent_init(void);
void agent_com_security(void);              /* 分身: ログインしている人のエクスプローラーからの COM を受け付ける */
void agent_status_update(void);
void agent_notify(const WCHAR *s);
void agent_request_sas(void);
BOOL agent_follow_input_desktop(void);      /* 呼んだスレッドを入力デスクトップへ。移ったら TRUE */
BOOL agent_input_desktop_changed(void);

/* main.c */
void app_notify(const WCHAR *fmt, ...);     /* 通知(バルーン)を出す。どのスレッドからでもよい */
void app_update_tray(void);
void app_listen_addresses(WCHAR *buf, int cap);   /* このPC の IPv4 アドレスの一覧 */
const WCHAR *app_listen_error(void);
BOOL app_listening(void);

#endif
