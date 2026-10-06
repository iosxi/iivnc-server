"""実画面の取り込みを比べるための、決まった動きを描き続ける窓(tools/compare.py と組む)。

    python tools/srcwin.py --scene office|video --seconds 12 --out build/test/src.json

窓は最前面・非アクティブ(利用者の窓からフォーカスを取らない)。左上の 16 個の 16×16 の升に
フレーム番号(16 ビット)を白黒で描き、DwmFlush の後の時刻(perf_counter = QPC。プロセスをまたいで同じ時計)を
フレームごとに記録する。

office  1280×720。文字の欄が 1 フレームに 4 画素ずつ流れ、四角が動く(ふだんの操作のような絵)
video   1280×720。写真のような絵が斜めに流れ、全面が毎フレーム変わる(動画のような絵)
idle    1280×720。左上の縞だけが変わる(1 回の更新に最低限かかる手間を見る)
"""
import argparse, ctypes, json, os, time
from ctypes import wintypes as W
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

WIDTH, HEIGHT = 1280, 720
MARK = 16           # 升の大きさ
BITS = 16

u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32
u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))    # 座標を物理画素で扱う

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, W.HWND, W.UINT, W.WPARAM, W.LPARAM)
u32.DefWindowProcW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u32.DefWindowProcW.restype = ctypes.c_ssize_t
g32.SetDIBitsToDevice.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int, W.DWORD, W.DWORD, ctypes.c_int, ctypes.c_int,
                                  W.UINT, W.UINT, ctypes.c_void_p, ctypes.c_void_p, W.UINT]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [('style', W.UINT), ('lpfnWndProc', WNDPROC), ('cbClsExtra', ctypes.c_int),
                ('cbWndExtra', ctypes.c_int), ('hInstance', W.HINSTANCE), ('hIcon', W.HICON),
                ('hCursor', W.HANDLE), ('hbrBackground', W.HBRUSH), ('lpszMenuName', W.LPCWSTR),
                ('lpszClassName', W.LPCWSTR)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG), ('biPlanes', W.WORD),
                ('biBitCount', W.WORD), ('biCompression', W.DWORD), ('biSizeImage', W.DWORD),
                ('biXPelsPerMeter', W.LONG), ('biYPelsPerMeter', W.LONG), ('biClrUsed', W.DWORD),
                ('biClrImportant', W.DWORD)]


def office_frames():
    font = ImageFont.truetype('C:/Windows/Fonts/meiryo.ttc', 18)
    tall = Image.new('RGB', (WIDTH - 360, 4000), (255, 255, 255))
    d = ImageDraw.Draw(tall)
    rng = np.random.default_rng(1)
    words = 'iivnc server client Tight JPEG ZRLE 画面 取り込み 符号化 復号 速さ 遅れ 更新 窓 文字'.split()
    for i, y in enumerate(range(0, 4000, 26)):
        line = ' '.join(rng.choice(words, 10))
        d.text((10, y), f'{i:04d}  {line}', fill=(30, 30, 30), font=font)
    tall = np.asarray(tall)
    base = np.full((HEIGHT, WIDTH, 3), 240, np.uint8)
    base[:, WIDTH - 340:] = (220, 228, 240)

    def frame(n):
        f = base.copy()
        y = (n * 4) % (4000 - HEIGHT)
        f[:, :WIDTH - 360] = tall[y:y + HEIGHT]
        bx = WIDTH - 330 + (n * 3) % 220
        by = 100 + int(200 + 180 * np.sin(n / 20))
        f[by:by + 100, bx:bx + 100] = (40, 120, 220)
        return f
    return frame


def video_frames():
    rng = np.random.default_rng(2)
    big = Image.fromarray(rng.integers(0, 255, (HEIGHT * 2 // 8, WIDTH * 2 // 8, 3), np.uint8)) \
        .resize((WIDTH * 2, HEIGHT * 2), Image.BICUBIC).filter(ImageFilter.GaussianBlur(3))
    d = ImageDraw.Draw(big)
    for _ in range(60):
        x, y = rng.integers(0, WIDTH * 2), rng.integers(0, HEIGHT * 2)
        r = int(rng.integers(20, 120))
        d.ellipse((x - r, y - r, x + r, y + r), fill=tuple(int(v) for v in rng.integers(0, 255, 3)))
    big = np.asarray(big.filter(ImageFilter.GaussianBlur(1)))

    def frame(n):
        x = (n * 7) % WIDTH
        y = (n * 3) % HEIGHT
        return big[y:y + HEIGHT, x:x + WIDTH].copy()
    return frame


def idle_frames():
    base = np.full((HEIGHT, WIDTH, 3), 200, np.uint8)
    return lambda n: base.copy()


def put_mark(f, n):
    for i in range(BITS):
        v = 255 if (n >> (BITS - 1 - i)) & 1 else 0
        f[0:MARK, i * MARK:(i + 1) * MARK] = v
    f[MARK:MARK + 4, 0:MARK * BITS] = 128       # 縞の下の帯(読み取りの目印ではない。境目をはっきりさせるだけ)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', default='office', choices=['office', 'video', 'idle'])
    ap.add_argument('--seconds', type=float, default=12)
    ap.add_argument('--x', type=int, default=200)
    ap.add_argument('--y', type=int, default=200)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    frame = {'office': office_frames, 'video': video_frames, 'idle': idle_frames}[a.scene]()

    hinst = ctypes.windll.kernel32.GetModuleHandleW(None)
    proc = WNDPROC(lambda h, m, w, l: u32.DefWindowProcW(h, m, w, l))
    wc = WNDCLASSW(0x20, proc, 0, 0, hinst, None, None, None, None, 'iivncSrcWin')   # CS_OWNDC
    u32.RegisterClassW(ctypes.byref(wc))
    ex = 0x8 | 0x80 | 0x08000000            # TOPMOST | TOOLWINDOW | NOACTIVATE
    u32.CreateWindowExW.restype = W.HWND
    hwnd = u32.CreateWindowExW(ex, 'iivncSrcWin', 'iivnc src', 0x80000000,     # WS_POPUP
                               a.x, a.y, WIDTH, HEIGHT, None, None, hinst, None)
    u32.ShowWindow(hwnd, 4)                 # SW_SHOWNOACTIVATE
    u32.GetDC.restype = W.HDC
    hdc = u32.GetDC(hwnd)
    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), WIDTH, -HEIGHT, 1, 32, 0, 0, 0, 0, 0, 0)
    pt = W.POINT(0, 0)
    u32.ClientToScreen(hwnd, ctypes.byref(pt))
    buf = np.zeros((HEIGHT, WIDTH, 4), np.uint8)
    msg = W.MSG()
    times = []
    t0 = time.perf_counter()
    n = 0
    while time.perf_counter() - t0 < a.seconds:
        while u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            u32.TranslateMessage(ctypes.byref(msg)); u32.DispatchMessageW(ctypes.byref(msg))
        f = frame(n)
        put_mark(f, n & 0xFFFF)
        buf[..., 0] = f[..., 2]; buf[..., 1] = f[..., 1]; buf[..., 2] = f[..., 0]
        g32.SetDIBitsToDevice(hdc, 0, 0, WIDTH, HEIGHT, 0, 0, 0, HEIGHT,
                              buf.ctypes.data_as(ctypes.c_void_p), ctypes.byref(bi), 0)
        g32.GdiFlush()
        ctypes.windll.dwmapi.DwmFlush()     # 合成を 1 回待つ(= 60Hz に合わせる)
        times.append(time.perf_counter())
        n += 1
    u32.DestroyWindow(hwnd)
    dur = times[-1] - times[0]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8') as fo:
        json.dump({'scene': a.scene, 'x': pt.x, 'y': pt.y, 'w': WIDTH, 'h': HEIGHT, 'mark': MARK, 'bits': BITS,
                   'times': times, 'fps': (len(times) - 1) / dur}, fo)
    print(f'描いた: {n} フレーム, {(len(times) - 1) / dur:.1f} fps, 窓の左上 ({pt.x},{pt.y})')


if __name__ == '__main__':
    main()
