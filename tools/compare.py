"""実画面を取り込む VNC サーバーどうしを、同じ受け手・同じ動く絵で比べる(tools/srcwin.py と組む)。

    python tools/compare.py --port 5911 --pid <サーバーの PID> --scene office|video|idle --quality hq|std|lossless

srcwin.py の窓(最前面・非アクティブ、物理座標 (200,200) から 1280×720)を出し、差分の要求を出し続ける
(更新の頭を受けた時点で次を要求する。iivnc-client と同じ先出し)。左上の縞(フレーム番号)に重なる矩形だけ
復号して番号を読み、描いた時刻との差を遅れとする。ほかの矩形は zlib の流れを保つために展開だけして捨てる。
CopyRect は求めない(縞の外から写されると番号が読めないため)。

出すもの: 更新/秒、見えたフレーム/秒(60 が上限)、取りこぼし、遅れ(中央値・95%)、帯域、サーバーの CPU(1 コア = 100%)。
"""
import argparse, ctypes, io, json, os, socket, struct, subprocess, sys, time, zlib
from ctypes import wintypes as W
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rfbcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QUALITY = {
    'hq': [7, -239, -256 + 1, -32 + 9, -512 + 95, -768],       # iivnc の「高画質」(JPEG 95・4:4:4)
    'std': [7, -239, -256 + 1, -32 + 6, -512 + 80, -767],      # 「標準」(JPEG 80・4:2:0)
    'lossless': [7, -239, -256 + 1],
}


def cpu_seconds(pid):
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    c, e, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
    k.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    k.CloseHandle(h)
    f = lambda t: (t.dwHighDateTime << 32 | t.dwLowDateTime) / 1e7
    return f(kt) + f(ut)


class Receiver(rfbcheck.Client):
    def setup(self, mx, my, mw, mh):
        # 32 ビット・深さ 24(Tight は 1 画素 3 バイト RGB で来る)
        self.s.sendall(struct.pack('>B3x', 0) + struct.pack('>BBBBHHHBBB3x', 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
        self.bpp = 4
        self.mx, self.my, self.mw, self.mh = mx, my, mw, mh
        self.mark = np.zeros((mh, mw, 3), np.uint8)
        self.decoded = 0

    def overlap(self, x, y, w, h):
        return x < self.mx + self.mw and self.mx < x + w and y < self.my + self.mh and self.my < y + h

    def put(self, x, y, w, h, img):
        """img(h×w×3)のうち縞に重なる所を写す"""
        x0, y0 = max(x, self.mx), max(y, self.my)
        x1, y1 = min(x + w, self.mx + self.mw), min(y + h, self.my + self.mh)
        self.mark[y0 - self.my:y1 - self.my, x0 - self.mx:x1 - self.mx] = img[y0 - y:y1 - y, x0 - x:x1 - x]

    def rect(self, x, y, w, h, e):
        if e == 0:
            d = self.read(w * h * 4)
            if self.overlap(x, y, w, h):
                self.put(x, y, w, h, np.frombuffer(d, np.uint8).reshape(h, w, 4)[..., [2, 1, 0]])
        elif e == 7:
            self.tight(x, y, w, h)
        elif e == -239:
            self.read(w * h * 4 + (w + 7) // 8 * h)
        elif e in (-232, -258):
            pass
        elif e in (-223, -308):
            if e == -308:
                ns = self.u8(); self.read(3 + 16 * ns)
            self.w, self.h = w, h
        else:
            raise RuntimeError(f'エンコーディング {e}')

    def tight(self, x, y, w, h):
        ctl = self.u8()
        for i in range(4):
            if ctl & (1 << i):
                self.tz[i] = zlib.decompressobj()
        kind = ctl >> 4
        hit = self.overlap(x, y, w, h)
        if kind == 8:
            c = np.frombuffer(self.read(3), np.uint8)
            if hit:
                self.put(x, y, w, h, np.broadcast_to(c, (h, w, 3)))
        elif kind == 9:
            d = self.read(self.clen())
            if hit:
                self.decoded += 1
                self.put(x, y, w, h, np.asarray(Image.open(io.BytesIO(d)).convert('RGB')))
        elif kind < 8:
            stream = kind & 3
            filt = self.u8() if kind & 4 else 0
            if filt == 1:
                n = self.u8() + 1
                pal = np.frombuffer(self.read(n * 3), np.uint8).reshape(n, 3)
                if n == 2:
                    rb = (w + 7) // 8
                    d = self.tight_data(stream, rb * h)
                    if hit:
                        idx = np.unpackbits(np.frombuffer(d, np.uint8).reshape(h, rb), axis=1)[:, :w]
                        self.put(x, y, w, h, pal[idx])
                else:
                    d = self.tight_data(stream, w * h)
                    if hit:
                        self.put(x, y, w, h, pal[np.frombuffer(d, np.uint8).reshape(h, w)])
            elif filt == 0:
                d = self.tight_data(stream, w * h * 3)
                if hit:
                    self.put(x, y, w, h, np.frombuffer(d, np.uint8).reshape(h, w, 3))
            elif filt == 2:         # 勾配(TightVNC が劣化なしで使う)。縞に要る行・列だけ戻す
                d = self.tight_data(stream, w * h * 3)
                if hit:
                    r = np.frombuffer(d, np.uint8).reshape(h, w, 3).astype(np.int32)
                    rows = min(h, self.my + self.mh - y)
                    cols = min(w, self.mx + self.mw - x)
                    out = np.zeros((h, w, 3), np.int32)
                    for j in range(rows):
                        up = out[j - 1] if j else np.zeros((w, 3), np.int32)
                        left = np.zeros(3, np.int32)
                        upleft = np.zeros(3, np.int32)
                        for i in range(cols):
                            p = np.clip(left + up[i] - upleft, 0, 255)
                            left = (r[j, i] + p) & 255
                            out[j, i] = left
                            upleft = up[i]
                    self.put(x, y, w, h, out.astype(np.uint8))
            else:
                raise RuntimeError(f'Tight の濾過 {filt}')
        else:
            raise RuntimeError(f'Tight の種類 {kind}')

    def frame_no(self):
        bits = self.mark[4:12, :, :].mean(axis=(0, 2)).reshape(-1, 16)[:, 4:12].mean(axis=1) > 128
        return int(''.join('1' if b else '0' for b in bits), 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--pid', type=int)
    ap.add_argument('--scene', default='office', choices=['office', 'video', 'idle'])
    ap.add_argument('--quality', default='hq', choices=list(QUALITY))
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--password')
    ap.add_argument('--label', default='')
    a = ap.parse_args()

    out = os.path.join(ROOT, 'build', 'test', f'src-{a.port}.json')
    if os.path.exists(out):
        os.remove(out)
    src = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'srcwin.py'), '--scene', a.scene,
                            '--seconds', str(a.seconds + 3), '--out', out], stdout=subprocess.DEVNULL)
    time.sleep(1.5)

    c = Receiver(a.port, a.password)
    c.setup(200, 200, 16 * 16, 16)
    c.set_encodings(QUALITY[a.quality])
    c.request(0)
    seen = {}
    updates = 0
    b0 = None
    t_start = None
    cpu0 = None
    deadline = None
    while True:
        t = c.u8()
        if t == 3:
            c.read(3); ln = struct.unpack('>i', c.read(4))[0]; c.read(abs(ln)); continue
        if t == 2:
            continue
        if t != 0:
            raise RuntimeError(f'メッセージ {t}')
        c.read(1)
        n = c.u16()
        c.request(1)            # 先出し
        for _ in range(n):
            x, y, w, h, e = struct.unpack('>HHHHi', c.read(12))
            if e == -224:
                break
            c.rect(x, y, w, h, e)
        now = time.perf_counter()
        if t_start is None:
            t_start, b0, cpu0 = now + 0.5, None, None    # 最初の全体の絵の後 0.5 秒は数えない
            deadline = t_start + a.seconds
            continue
        if now < t_start:
            continue
        if b0 is None:
            b0 = c.bytes
            cpu0 = cpu_seconds(a.pid) if a.pid else None
            t_start = now
        updates += 1
        fn = c.frame_no()
        seen.setdefault(fn, now)
        if now >= deadline:
            break
    dt = time.perf_counter() - t_start
    by = c.bytes - b0
    cpu = (cpu_seconds(a.pid) - cpu0) / dt * 100 if a.pid else float('nan')
    c.s.close()
    src.wait()
    info = json.load(open(out, encoding='utf-8'))
    times = info['times']
    lat = sorted((t - times[f]) * 1000 for f, t in seen.items() if f < len(times) and t >= times[f])
    drawn = sum(1 for tt in times if t_start <= tt <= t_start + dt)
    got = sum(1 for f, t in seen.items() if f < len(times) and t_start <= times[f] <= t_start + dt)
    med = lat[len(lat) // 2] if lat else float('nan')
    p95 = lat[int(len(lat) * 0.95)] if lat else float('nan')
    print(f'{a.label:14s} {a.scene:6s} {a.quality:8s} 更新 {updates / dt:6.1f}/秒  見えた {got / dt:5.1f}/秒'
          f' (描いた {drawn / dt:4.1f})  遅れ 中央 {med:5.1f}ms 95% {p95:5.1f}ms'
          f'  {by * 8 / dt / 1e6:7.1f} Mbps  CPU {cpu:5.1f}%')


if __name__ == '__main__':
    main()
