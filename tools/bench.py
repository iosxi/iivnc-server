"""iivnc-server の速さを測る(受け手は復号せず、読み飛ばすだけ)。

    python tools/bench.py --port 5998 [--seconds 6] [--mode full|incremental]

full        全体を毎回求める(非差分の要求)。全画面の符号化の速さ。
incremental 差分の要求を出し続ける(先出し 1 つ)。画面が動いているときの更新の速さ。
各エンコーディング(Tight ロスレス / Tight+JPEG / ZRLE / Raw)ごとに、
1 秒あたりの更新数・1 回の大きさ・受信の速さを出す。
"""
import argparse, socket, struct, sys, time
sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
import rfbcheck  # noqa: E402


class Skipper(rfbcheck.Client):
    """矩形を読み飛ばす(Tight は展開後の大きさを計算して、圧縮の長さを辿る)"""

    def update(self):
        t = self.u8()
        while t != 0:
            if t == 3:
                self.read(3); ln = struct.unpack('>i', self.read(4))[0]; self.read(abs(ln))
            elif t == 2:
                pass
            else:
                raise RuntimeError(f'メッセージ {t}')
            t = self.u8()
        self.read(1)
        n = self.u16()
        for _ in range(n):
            x, y, w, h, e = struct.unpack('>HHHHi', self.read(12))
            if e == 0:
                self.read(w * h * 4)
            elif e == 1:
                self.read(4)
            elif e == 16:
                self.read(self.u32())
            elif e == 7:
                ctl = self.u8() >> 4
                if ctl == 8:
                    self.read(3)
                elif ctl == 9:
                    self.read(self.clen())
                else:
                    filt = self.u8() if ctl & 4 else 0
                    if filt == 1:
                        nc = self.u8() + 1
                        self.read(nc * 3)
                        size = ((w + 7) // 8) * h if nc == 2 else w * h
                    else:
                        size = w * h * 3
                    if size < 12:
                        self.read(size)
                    else:
                        self.read(self.clen())
            elif e == -239:
                self.read(w * h * 4 + (w + 7) // 8 * h)
            elif e in (-232, -258):
                pass
            elif e in (-223,):
                self.w, self.h = w, h
            elif e == -308:
                ns = self.u8(); self.read(3 + 16 * ns); self.w, self.h = w, h
            else:
                raise RuntimeError(f'エンコーディング {e}')
        return n


def bench(port, label, encs, seconds, mode, password=None):
    c = Skipper(port, password)
    c.set_encodings(encs)
    c.request(0)
    c.update()
    b0 = c.bytes
    n = 0
    t0 = time.perf_counter()
    if mode == 'incremental':
        c.request(1)
    while time.perf_counter() - t0 < seconds:
        if mode == 'full':
            c.request(0)
            c.update()
        else:
            c.request(1)        # 先出し
            c.update()
        n += 1
    dt = time.perf_counter() - t0
    c.s.close()
    by = c.bytes - b0
    print(f'  {label:26s} {c.w}x{c.h}  {n / dt:6.1f} 回/秒  1 回 {by / max(n, 1) / 1024:8.1f} KB  {by * 8 / dt / 1e6:8.1f} Mbps')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=5998)
    ap.add_argument('--seconds', type=float, default=6)
    ap.add_argument('--mode', default='full', choices=['full', 'incremental'])
    ap.add_argument('--password')
    a = ap.parse_args()
    print(f'[{a.mode}]')
    for label, encs in [
        ('Tight ロスレス', [7, 1, -239]),
        ('Tight+JPEG 95 4:4:4', [7, 1, -239, -32 + 8, -512 + 95, -768]),
        ('Tight+JPEG 80 4:2:0', [7, 1, -239, -32 + 6, -512 + 80, -767]),
        ('ZRLE', [16, 1, -239]),
        ('Raw', [0, 1, -239]),
    ]:
        bench(a.port, label, encs, a.seconds, a.mode, a.password)


if __name__ == '__main__':
    main()
