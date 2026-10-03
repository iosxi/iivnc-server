"""iivnc-server の自動検証(画面は写さない・入力は再現しない)。

    python tools/test.py

サーバーを -testsrc(合成した絵。入力はログに書くだけ)と検証用の ini で起動し、
tools/rfbcheck.py の受け手で各エンコーディングの結果を Raw の絵と比べる。
利用者が普段使っている iivnc-server(既定の ini)とは干渉しない(多重起動の判定は ini ごと)。
"""
import os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rfbcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'iivnc-server.exe')
TEST = os.path.join(ROOT, 'build', 'test')
INI = os.path.join(TEST, 'test.ini')
PORT = 5999


def start(args, password=None):
    os.makedirs(TEST, exist_ok=True)
    with open(INI, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'port={PORT}\nlisten=127.0.0.1\nnotify=0\nlog=1\n')
        if password:
            f.write(f'password={vnc_hide(password)}\n')
    subprocess.Popen([EXE, '-ini', INI] + args)
    for _ in range(50):
        try:
            import socket
            socket.create_connection(('127.0.0.1', PORT), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit('サーバーが待ち受けない')


def stop():
    subprocess.run([EXE, '-ini', INI, '-exit'])
    time.sleep(0.5)


def vnc_hide(pw):
    key = bytes(int(f'{b:08b}'[::-1], 2) for b in bytes([23, 82, 107, 6, 35, 78, 88, 7]))
    return rfbcheck.des_encrypt(key, pw.encode()[:8].ljust(8, b'\0')).hex()


def main():
    ok = True
    print('[認証]')
    start(['-testsrc', 'static'], password='s3cret')
    try:
        try:
            rfbcheck.Client(PORT, 'wrongpw!')
            print('  違うパスワードで通ってしまった NG'); ok = False
        except RuntimeError as e:
            print(f'  違うパスワード → {e}  OK')
        c = rfbcheck.Client(PORT, 's3cret')
        print(f'  正しいパスワード → {c.w}x{c.h} "{c.name}"  OK')
        c.s.close()
    finally:
        stop()

    print('[止まった絵]')
    start(['-testsrc', 'static'])
    try:
        Q = lambda q: -32 + q
        for label, encs, kw in [
            ('Raw', [0], {}),
            ('ZRLE', [16], {}),
            ('ZRLE 16bpp(565)', [16], {'pf16': True}),
            ('Raw 16bpp(565)', [0], {'pf16': True}),
            ('Tight(ロスレス)', [7], {}),
            ('Tight 圧縮 6', [7, -256 + 6], {}),
            ('Tight + JPEG 画質 9', [7, Q(9)], {'min_psnr': 40}),
            ('Tight + JPEG 画質 5', [7, Q(5)], {'min_psnr': 27}),
            ('Tight + JPEG 画質 0', [7, Q(0)], {'min_psnr': 22}),
        ]:
            ok &= rfbcheck.check_static(PORT, None, label, encs, **kw)
    finally:
        stop()

    print('[動く絵(スクロールは CopyRect)を 300 フレーム追って、止まったところで比べる]')
    for label, encs, kw in [
        ('Tight + CopyRect + カーソル', [7, 1, -239, -232, -258], {}),
        ('ZRLE + CopyRect', [16, 1, -239], {}),
        ('Tight(CopyRect なし)', [7], {}),
        ('Raw + CopyRect', [0, 1], {}),
        ('Tight + JPEG 8 + CopyRect', [7, 1, -32 + 8], {'min_psnr': 35}),
    ]:
        start(['-testsrc', '-testframes', '300'])
        try:
            ok &= rfbcheck.check_animated(PORT, None, label, encs, **kw)
        finally:
            stop()
    print('[途中で画面の大きさが変わる(100 フレーム目で 1920x1080 → 1280x720)]')
    for label, encs in [
        ('Tight + DesktopSize', [7, 1, -223]),
        ('ZRLE + ExtendedDesktopSize', [16, 1, -308, -223]),
    ]:
        start(['-testsrc', '-testframes', '200', '-testresize', '100'])
        try:
            ok &= rfbcheck.check_animated(PORT, None, label, encs)
        finally:
            stop()

    print('[3 人が同時につなぐ]')
    start(['-testsrc', '-testframes', '200'])
    try:
        import threading
        res = []
        th = [threading.Thread(target=lambda e=e, m=m: res.append(rfbcheck.check_animated(PORT, None, f'同時 {e}', e, min_psnr=m)))
              for e, m in (([7, 1], None), ([16, 1], None), ([7, 1, -32 + 8], 35))]
        for x in th: x.start()
        for x in th: x.join()
        ok &= len(res) == 3 and all(res)
    finally:
        stop()

    print('[でたらめなデータを送っても落ちない]')
    start(['-testsrc', 'static'])
    try:
        import random, socket
        rnd = random.Random(5)
        for k in range(200):
            c = rfbcheck.Client(PORT, None)
            junk = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 400)))
            try:
                c.s.sendall(junk)
                c.s.settimeout(0.2)
                try:
                    c.s.recv(65536)
                except OSError:
                    pass
            except OSError:
                pass
            c.s.close()
        c = rfbcheck.Client(PORT, None)          # まだ受け付けるか
        c.set_encodings([7]); c.request(0); c.update(); c.s.close()
        print('  200 回のでたらめな接続のあとも動いている  OK')
    except Exception as e:
        print(f'  NG: {e}'); ok = False
    finally:
        stop()
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
