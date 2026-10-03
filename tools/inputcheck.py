"""入力の再現を、-testsrc(入力はログに書くだけ)のログで確かめる。

    python tools/inputcheck.py

マウス(移動・ボタン・ホイール)、キーシムのキー、QEMU のスキャンコードを送り、
サーバーのログの [dryrun] 行(SendInput に渡すはずだった中身)を期待値と比べる。
利用者のマウス・キーボードは動かさない。
"""
import os, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rfbcheck  # noqa: E402
import test as t  # noqa: E402

LOG = os.path.join(t.TEST, 'test.log')

# 期待値。dryrun の画面は 1920x1080、座標は ((2d+1)*65536)/(2w)
def absx(x): return ((2 * x + 1) * 65536) // (2 * 1920)
def absy(y): return ((2 * y + 1) * 65536) // (2 * 1080)


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    t.start(['-testsrc', 'static'])
    try:
        c = rfbcheck.Client(t.PORT, None)
        c.set_encodings([7, -258])              # QEMU のキーも使う
        c.request(0)
        c.update()
        ptr = lambda m, x, y: c.s.sendall(struct.pack('>BBHH', 5, m, x, y))
        key = lambda d, k: c.s.sendall(struct.pack('>BBxxI', 4, d, k))
        qkey = lambda d, k, code: c.s.sendall(struct.pack('>BBHII', 255, 0, d, k, code))
        ptr(0, 100, 200)
        ptr(1, 100, 200)        # 左を押す
        ptr(0, 100, 200)        # 離す
        ptr(8, 100, 200)        # ホイール上(押した時だけ 1 段)
        ptr(0, 100, 200)
        ptr(128, 1919, 1079)    # 戻る(X1)+ 右下の角へ
        ptr(0, 1919, 1079)
        key(1, ord('a')); key(0, ord('a'))
        key(1, 0xffe1); key(1, ord('A')); key(0, ord('A')); key(0, 0xffe1)   # Shift + A
        key(1, 0xffc2); key(0, 0xffc2)                                        # F5
        key(1, 0x01006f22); key(0, 0x01006f22)                                # 漢(配列に無い → Unicode)
        key(1, 0xff2a); key(0, 0xff2a)                                        # 半角/全角
        qkey(1, ord('a'), 0x1E); qkey(0, ord('a'), 0x1E)                      # QEMU: A
        qkey(1, 0xffe4, 0x9D); qkey(0, 0xffe4, 0x9D)                          # QEMU: 右 Ctrl(E0 1D)
        qkey(1, 0xff13, 0xC6); qkey(0, 0xff13, 0xC6)                          # QEMU: Pause
        time.sleep(0.5)
        c.s.close()
    finally:
        t.stop()
    lines = [l.split('[dryrun] ', 1)[1].strip() for l in open(LOG, encoding='utf-8') if '[dryrun]' in l]
    expect = [
        f'mouse flags=C001 dx={absx(100)} dy={absy(200)} data=0',
        'mouse flags=0002 dx=0 dy=0 data=0',
        'mouse flags=0004 dx=0 dy=0 data=0',
        'mouse flags=0800 dx=0 dy=0 data=120',
        f'mouse flags=C001 dx={absx(1919)} dy={absy(1079)} data=0',
        'mouse flags=0080 dx=0 dy=0 data=1',
        'mouse flags=0100 dx=0 dy=0 data=1',
        'key vk=41 scan=1E flags=0', 'key vk=41 scan=1E flags=2',
        'key vk=A0 scan=2A flags=0', 'key vk=41 scan=1E flags=0', 'key vk=41 scan=1E flags=2', 'key vk=A0 scan=2A flags=2',
        'key vk=74 scan=3F flags=0', 'key vk=74 scan=3F flags=2',
        'key vk=00 scan=6F22 flags=4', 'key vk=00 scan=6F22 flags=6',
        None, None,                 # 半角/全角(配列で変わるので表示だけ)
        'key vk=00 scan=1E flags=8', 'key vk=00 scan=1E flags=A',
        'key vk=00 scan=1D flags=9', 'key vk=00 scan=1D flags=B',
        'key vk=13 scan=00 flags=0', 'key vk=13 scan=00 flags=2',     # Pause は仮想キーで送る
    ]
    ok = len(lines) == len(expect)
    for i in range(max(len(lines), len(expect))):
        got = lines[i] if i < len(lines) else '(無し)'
        exp = expect[i] if i < len(expect) else '(無し)'
        good = exp is None or got == exp
        ok &= good
        print(f'  {"OK" if good else "NG"}  {got:44s}' + ('' if good else f'  期待: {exp}'))
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
