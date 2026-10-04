"""Windows がカーソルを隠しているとき(マウスが無い PC など)に、相手へカーソルが届くか確かめる。

    python tools/cursorcheck.py

サーバーを -testsrc static と -testcursor(隠れたカーソル)で動かし、rfbcheck の受け手で
カーソルの形(-239)を受け取って、見える画素の数を数える。showcursor=1 なら隠れていても見え、
showcursor=0 なら見えない(前の版と同じ)はず。-testcursor none は形も無い場合(標準の矢印になる)。
利用者の画面もカーソルも使わない。
"""
import os, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rfbcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'iivnc-server.exe')
TEST = os.path.join(ROOT, 'build', 'test')
INI = os.path.join(TEST, 'cursor.ini')
PORT = 5996


def run(show, cursor):
    open(INI, 'w', encoding='utf-8', newline='\n').write(
        f'port={PORT}\nlisten=127.0.0.1\nnotify=0\nlog=1\nshowcursor={show}\n')
    subprocess.Popen([EXE, '-ini', INI, '-testsrc', 'static'] + (['-testcursor', cursor] if cursor else []))
    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', PORT), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    try:
        c = rfbcheck.Client(PORT, None)
        c.set_encodings([0, -239])
        c.request(0)
        c.update()
        c.s.close()
        return c.cursor, getattr(c, 'cursor_bits', None)
    finally:
        subprocess.run([EXE, '-ini', INI, '-exit'])


def main():
    os.makedirs(TEST, exist_ok=True)
    ok = True
    cases = [(1, None, True), (1, 'hidden', True), (1, 'none', True),
             (0, None, True), (0, 'hidden', False), (0, 'none', False)]
    for show, cursor, want in cases:
        shape, bits = run(show, cursor)
        seen = bool(bits)
        good = seen == want
        ok &= good
        print(f'  {"OK" if good else "NG"}  showcursor={show} カーソル {cursor or "見える":7s} → '
              f'形 {shape[:2] if shape else None}、見える画素 {bits}(期待: {"見える" if want else "見えない"})')
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
