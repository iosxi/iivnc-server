"""サービスとして動いている iivnc-server を確かめる(登録は済ませておく)。

    python tools/svccheck.py --port 5999 --password s3cret [--lock]

1. 取り込んだ絵を、同時に GDI で撮った画面と比べる。
2. Ctrl+Alt+Del を送り、安全なデスクトップ(ロック・切り替え・タスク マネージャーの画面)が
   取り込めること、Esc で元へ戻せることを確かめる。実際の画面がしばらくその表示になる。
3. --lock: 画面をロックし、ロック画面が取り込めること、キーで資格情報の画面が出ることを確かめる。
   ロックは解除しない(パスワードが要る)ので、ご本人に解除してもらう。
"""
import argparse, ctypes, os, struct, sys, time
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rfbcheck  # noqa: E402

user32 = ctypes.windll.user32
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))


def input_desktop():
    h = user32.OpenInputDesktop(0, False, 1)
    if not h:
        return '(開けない = Winlogon など)'
    buf = ctypes.create_unicode_buffer(64)
    n = ctypes.c_ulong()
    user32.GetUserObjectInformationW(h, 2, buf, 128, ctypes.byref(n))
    user32.CloseDesktop(h)
    return buf.value


def pump(c, n=6):
    c.s.settimeout(1.5)
    for _ in range(n):
        c.request(1)
        try:
            c.update()
        except Exception:
            break
    c.s.settimeout(10)
    return c.fb.copy()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=5999)
    ap.add_argument('--password', default='s3cret')
    ap.add_argument('--lock', action='store_true')
    a = ap.parse_args()
    from PIL import ImageGrab
    # 画面は動いていることがあるので、取り込みの前後に GDI で撮り、近い方と比べる
    g0 = np.asarray(ImageGrab.grab(all_screens=True).convert('RGB'))
    ref = rfbcheck.reference(a.port, a.password, (0, -239))
    g1 = np.asarray(ImageGrab.grab(all_screens=True).convert('RGB'))
    m = max(np.mean(np.all(ref == g0, axis=2)), np.mean(np.all(ref == g1, axis=2)))
    print(f'1. 取り込みと GDI の画面の一致: {m * 100:.2f}%')

    c = rfbcheck.Client(a.port, a.password)
    c.set_encodings([7, -239])
    c.request(0)
    c.update()
    key = lambda d, k: c.s.sendall(struct.pack('>BBxxI', 4, d, k))
    before = pump(c, 1)
    for k in (0xffe3, 0xffe9, 0xffff):
        key(1, k)
    for k in (0xffff, 0xffe9, 0xffe3):
        key(0, k)
    time.sleep(2.5)
    during = pump(c)
    desk = input_desktop()
    print(f'2. Ctrl+Alt+Del の後: 入力デスクトップ {desk}、変わった画素 {np.mean(np.any(before != during, axis=2)) * 100:.1f}%、'
          f'平均の色 {before.reshape(-1, 3).mean(0).round(1)} → {during.reshape(-1, 3).mean(0).round(1)}')
    key(1, 0xff1b)
    key(0, 0xff1b)
    time.sleep(2.0)
    after = pump(c)
    print(f'   Esc の後: 入力デスクトップ {input_desktop()}、元の画面と同じ画素 {np.mean(np.all(before == after, axis=2)) * 100:.1f}%')

    if a.lock:
        user32.LockWorkStation()
        time.sleep(4)
        locked = pump(c)
        print(f'3. ロックの後: 入力デスクトップ {input_desktop()}、平均の色 {locked.reshape(-1, 3).mean(0).round(1)}、'
              f'変わった画素 {np.mean(np.any(after != locked, axis=2)) * 100:.1f}%')
        key(1, 0x20)
        key(0, 0x20)
        time.sleep(2.5)
        cred = pump(c)
        print(f'   スペースの後: 変わった画素 {np.mean(np.any(locked != cred, axis=2)) * 100:.1f}%(資格情報の画面が出れば変わる)')
    c.s.close()


if __name__ == '__main__':
    main()
