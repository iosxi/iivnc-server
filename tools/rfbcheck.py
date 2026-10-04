"""iivnc-server を、自前とは別に書いた RFB の受け手で確かめる。

    python tools/rfbcheck.py [--port 5999] [--password pw]

サーバーは -testsrc で動かしておく(tools/test.ps1 が用意する)。
各エンコーディングで受けた絵を、Raw で受けた絵と画素単位で比べる。
JPEG は PSNR で比べる。動く絵(スクロール = CopyRect)を追いかけた後も比べる。
"""
import argparse, socket, struct, sys, time, zlib, io
import numpy as np
from PIL import Image

# ---------------------------------------------------------------- DES(VNC 認証用)
_IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
       57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
_FP = [40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
       36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25]
_E = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
      24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1]
_P = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
_PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
        63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4]
_PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
        41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
_SH = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
_S = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8, 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5, 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1, 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9, 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6, 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8, 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6, 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2, 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11]]


def _perm(v, bits, tab):
    o = 0
    for t in tab:
        o = (o << 1) | ((v >> (bits - t)) & 1)
    return o


def des_encrypt(key, block):
    k = _perm(int.from_bytes(key, 'big'), 64, _PC1)
    c, d = k >> 28, k & 0xFFFFFFF
    subs = []
    for s in _SH:
        c = ((c << s) | (c >> (28 - s))) & 0xFFFFFFF
        d = ((d << s) | (d >> (28 - s))) & 0xFFFFFFF
        subs.append(_perm((c << 28) | d, 56, _PC2))
    b = _perm(int.from_bytes(block, 'big'), 64, _IP)
    l, r = b >> 32, b & 0xFFFFFFFF
    for sk in subs:
        x = _perm(r, 32, _E) ^ sk
        f = 0
        for j in range(8):
            six = (x >> (42 - 6 * j)) & 63
            f = (f << 4) | _S[j][((six >> 4) & 2 | six & 1) * 16 + ((six >> 1) & 15)]
        l, r = r, l ^ _perm(f, 32, _P)
    return _perm((r << 32) | l, 64, _FP).to_bytes(8, 'big')


def vnc_response(password, challenge):
    pw = password.encode('latin-1')[:8].ljust(8, b'\0')
    key = bytes(int(f'{b:08b}'[::-1], 2) for b in pw)
    return des_encrypt(key, challenge[:8]) + des_encrypt(key, challenge[8:])


# ---------------------------------------------------------------- RFB の受け手
ENC = {'raw': 0, 'copyrect': 1, 'tight': 7, 'zrle': 16}


class Client:
    def __init__(self, port, password=None, host='127.0.0.1'):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b''
        self.bytes = 0
        ver = self.read(12)
        assert ver.startswith(b'RFB 003.'), ver
        self.s.sendall(b'RFB 003.008\n')
        n = self.read(1)[0]
        if n == 0:
            raise RuntimeError('拒否: ' + self.read(self.u32()).decode())
        types = self.read(n)
        if 2 in types and password is not None:
            self.s.sendall(b'\x02')
            ch = self.read(16)
            self.s.sendall(vnc_response(password, ch))
        elif 1 in types:
            self.s.sendall(b'\x01')
        else:
            raise RuntimeError(f'認証の種類 {list(types)}')
        if self.u32() != 0:
            raise RuntimeError('認証失敗: ' + self.read(self.u32()).decode())
        self.s.sendall(b'\x01')
        self.w, self.h = struct.unpack('>HH', self.read(4))
        self.pf = self.read(16)
        self.name = self.read(self.u32()).decode('utf-8')
        self.fb = np.zeros((self.h, self.w, 3), np.uint8)
        self.bpp = 4
        self.pf16 = False
        self.zrle = zlib.decompressobj()
        self.tz = [zlib.decompressobj() for _ in range(4)]
        self.cursor = None
        self.pointer = None
        self.rects = {}
        self.qemu_ack = False

    def read(self, n):
        while len(self.buf) < n:
            d = self.s.recv(1 << 20)
            if not d:
                raise EOFError('切れた')
            self.buf += d
            self.bytes += len(d)
        r, self.buf = self.buf[:n], self.buf[n:]
        return r

    def u8(self): return self.read(1)[0]
    def u16(self): return struct.unpack('>H', self.read(2))[0]
    def u32(self): return struct.unpack('>I', self.read(4))[0]

    def set_encodings(self, encs):
        self.s.sendall(struct.pack('>BBH', 2, 0, len(encs)) + b''.join(struct.pack('>i', e) for e in encs))

    def set_pf_565(self):
        self.s.sendall(struct.pack('>BBBB', 0, 0, 0, 0) + struct.pack('>BBBBHHHBBB3x', 16, 16, 0, 1, 31, 63, 31, 11, 5, 0))
        self.bpp = 2
        self.pf16 = True

    def request(self, incremental, x=0, y=0, w=None, h=None):
        self.s.sendall(struct.pack('>BBHHHH', 3, incremental, x, y, w or self.w, h or self.h))

    # 画素(相手の形式) → RGB
    def pixels(self, data, n):
        if self.pf16:
            v = np.frombuffer(data, '<u2', n).astype(np.uint32)
            r = ((v >> 11) & 31) * 255 // 31; g = ((v >> 5) & 63) * 255 // 63; b = (v & 31) * 255 // 31
            return np.stack([r, g, b], -1).astype(np.uint8)
        a = np.frombuffer(data, np.uint8, n * 4).reshape(n, 4)
        return a[:, [2, 1, 0]]

    def cpixels(self, data, n):
        if self.pf16:
            return self.pixels(data, n)
        a = np.frombuffer(data, np.uint8, n * 3).reshape(n, 3)
        return a[:, [2, 1, 0]]

    def update(self):
        t = self.u8()
        while t != 0:
            if t == 3:          # ServerCutText
                self.read(3)
                ln = struct.unpack('>i', self.read(4))[0]
                self.read(abs(ln))
            elif t == 1:        # 色の表
                self.read(3); n = self.u16() if False else None
                raise RuntimeError('色の表は使っていない')
            elif t == 2:
                pass
            else:
                raise RuntimeError(f'知らないメッセージ {t}')
            t = self.u8()
        self.read(1)
        n = self.u16()
        for _ in range(n):
            x, y, w, h, e = struct.unpack('>HHHHi', self.read(12))
            self.rects[e] = self.rects.get(e, 0) + 1
            if e == 0:
                self.fb[y:y + h, x:x + w] = self.pixels(self.read(w * h * self.bpp), w * h).reshape(h, w, 3)
            elif e == 1:
                sx, sy = struct.unpack('>HH', self.read(4))
                self.fb[y:y + h, x:x + w] = self.fb[sy:sy + h, sx:sx + w].copy()
            elif e == 16:
                self.do_zrle(x, y, w, h)
            elif e == 7:
                self.do_tight(x, y, w, h)
            elif e == -239:
                self.read(w * h * self.bpp)
                mask = self.read((w + 7) // 8 * h)
                self.cursor = (w, h, x, y)
                self.cursor_bits = sum(bin(b).count('1') for b in mask)   # 見える画素の数
            elif e == -232:
                self.pointer = (x, y)
            elif e == -258:
                self.qemu_ack = True
            elif e in (-223, -308):
                if e == -308:
                    ns = self.u8(); self.read(3); self.read(16 * ns)
                self.w, self.h = w, h
                self.fb = np.zeros((h, w, 3), np.uint8)
            elif e == -224:
                break
            else:
                raise RuntimeError(f'知らないエンコーディング {e}')
        return n

    def do_zrle(self, x, y, w, h):
        data = self.zrle.decompress(self.read(self.u32()))
        p = 0
        cp = 2 if self.pf16 else 3
        for ty in range(y, y + h, 64):
            for tx in range(x, x + w, 64):
                tw, th = min(64, x + w - tx), min(64, y + h - ty)
                sub = data[p]; p += 1
                if sub == 0:
                    px = self.cpixels(data[p:p + tw * th * cp], tw * th); p += tw * th * cp
                    self.fb[ty:ty + th, tx:tx + tw] = px.reshape(th, tw, 3)
                elif sub == 1:
                    self.fb[ty:ty + th, tx:tx + tw] = self.cpixels(data[p:p + cp], 1)[0]; p += cp
                elif 2 <= sub <= 16:
                    pal = self.cpixels(data[p:p + sub * cp], sub); p += sub * cp
                    bits = 1 if sub == 2 else 2 if sub <= 4 else 4
                    rb = (tw * bits + 7) // 8
                    raw = np.frombuffer(data[p:p + rb * th], np.uint8).reshape(th, rb); p += rb * th
                    bitsarr = np.unpackbits(raw, axis=1)
                    idx = np.zeros((th, tw), np.int64)
                    for k in range(bits):
                        idx = (idx << 1) | bitsarr[:, k:tw * bits:bits][:, :tw]
                    self.fb[ty:ty + th, tx:tx + tw] = pal[idx]
                elif sub == 128 or sub >= 130:
                    pal = None
                    if sub >= 130:
                        n = sub - 128
                        pal = self.cpixels(data[p:p + n * cp], n); p += n * cp
                    out = np.zeros((tw * th, 3), np.uint8)
                    k = 0
                    while k < tw * th:
                        if pal is None:
                            c = self.cpixels(data[p:p + cp], 1)[0]; p += cp
                            ln = 1
                            while True:
                                b = data[p]; p += 1; ln += b
                                if b != 255: break
                        else:
                            i = data[p]; p += 1
                            ln = 1
                            if i & 128:
                                while True:
                                    b = data[p]; p += 1; ln += b
                                    if b != 255: break
                            c = pal[i & 127]
                        out[k:k + ln] = c
                        k += ln
                    self.fb[ty:ty + th, tx:tx + tw] = out.reshape(th, tw, 3)
                else:
                    raise RuntimeError(f'ZRLE の知らない種類 {sub}')

    def clen(self):
        b = self.u8(); n = b & 0x7F
        if b & 0x80:
            b = self.u8(); n |= (b & 0x7F) << 7
            if b & 0x80:
                n |= self.u8() << 14
        return n

    def tight_data(self, stream, size):
        if size < 12:
            return self.read(size)
        d = self.tz[stream].decompress(self.read(self.clen()))
        assert len(d) == size, (len(d), size)
        return d

    def do_tight(self, x, y, w, h):
        ctl = self.u8()
        for i in range(4):
            if ctl & (1 << i):
                self.tz[i] = zlib.decompressobj()
        kind = ctl >> 4
        if kind == 8:
            self.fb[y:y + h, x:x + w] = np.frombuffer(self.read(3), np.uint8)
        elif kind == 9:
            img = Image.open(io.BytesIO(self.read(self.clen()))).convert('RGB')
            assert img.size == (w, h), (img.size, w, h)
            self.fb[y:y + h, x:x + w] = np.asarray(img)
        elif kind < 8:
            stream = kind & 3
            filt = self.u8() if kind & 4 else 0
            if filt == 0:
                d = self.tight_data(stream, w * h * 3)
                self.fb[y:y + h, x:x + w] = np.frombuffer(d, np.uint8).reshape(h, w, 3)
            elif filt == 1:
                n = self.u8() + 1
                pal = np.frombuffer(self.read(n * 3), np.uint8).reshape(n, 3)
                if n == 2:
                    rb = (w + 7) // 8
                    d = np.frombuffer(self.tight_data(stream, rb * h), np.uint8).reshape(h, rb)
                    idx = np.unpackbits(d, axis=1)[:, :w]
                else:
                    idx = np.frombuffer(self.tight_data(stream, w * h), np.uint8).reshape(h, w)
                self.fb[y:y + h, x:x + w] = pal[idx]
            else:
                raise RuntimeError(f'Tight の濾過 {filt} は使っていないはず')
        else:
            raise RuntimeError(f'Tight の種類 {kind}')


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)


def reference(port, password, encs=(0,)):
    c = Client(port, password)
    c.set_encodings(list(encs))
    c.request(0)
    c.update()
    c.s.close()
    return c.fb


def check_static(port, password, label, encs, pf16=False, min_psnr=None):
    ref = reference(port, password)
    if pf16:
        # サーバーの換算 (v*max+127)/255 と、受け手の戻し v*255/max
        r = ref.astype(np.uint32)
        ref = np.stack([((r[..., 0] * 31 + 127) // 255) * 255 // 31,
                        ((r[..., 1] * 63 + 127) // 255) * 255 // 63,
                        ((r[..., 2] * 31 + 127) // 255) * 255 // 31], -1).astype(np.uint8)
    c = Client(port, password)
    if pf16:
        c.set_pf_565()
    c.set_encodings(encs)
    t0 = time.perf_counter()
    c.request(0)
    c.update()
    dt = time.perf_counter() - t0
    c.s.close()
    p = psnr(ref, c.fb)
    ok = p >= (min_psnr if min_psnr else 99)
    print(f'  {label:34s} {c.bytes:9d} バイト {dt * 1000:7.1f}ms  PSNR {p:5.1f}  矩形 {dict(c.rects)}  {"OK" if ok else "NG"}')
    return ok


def check_animated(port, password, label, encs, min_psnr=None):
    """動く絵を追いかけ、止まったら Raw の絵と比べる"""
    c = Client(port, password)
    c.set_encodings(encs)
    c.request(0)
    c.update()
    frames = 0
    t0 = time.perf_counter()
    c.s.settimeout(1.5)
    while True:
        c.request(1)
        try:
            c.update()
            frames += 1
        except socket.timeout:
            break
    dt = time.perf_counter() - t0 - 1.5
    c.s.settimeout(10)
    ref = reference(port, password)
    p = psnr(ref, c.fb)
    ok = p >= (min_psnr if min_psnr else 99)
    print(f'  {label:34s} 更新 {frames:4d} 回 {frames / max(dt, 1e-6):6.1f} 回/秒 {c.bytes:9d} バイト  PSNR {p:5.1f}  矩形 {dict(c.rects)}  {"OK" if ok else "NG"}')
    c.s.close()
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=5999)
    ap.add_argument('--password')
    ap.add_argument('--mode', default='static', choices=['static', 'animated', 'auth'])
    a = ap.parse_args()
    ok = True
    if a.mode == 'auth':
        try:
            Client(a.port, 'wrongpw!')
            print('  違うパスワードで通ってしまった NG'); ok = False
        except RuntimeError as e:
            print(f'  違うパスワード: {e}  OK')
        c = Client(a.port, a.password)
        print(f'  正しいパスワード: 接続 {c.w}x{c.h} 名前 "{c.name}"  OK')
        c.s.close()
    elif a.mode == 'static':
        Q = lambda q: -32 + q
        ok &= check_static(a.port, a.password, 'Raw', [0])
        ok &= check_static(a.port, a.password, 'ZRLE', [16])
        ok &= check_static(a.port, a.password, 'ZRLE 16bpp(565)', [16], pf16=True)
        ok &= check_static(a.port, a.password, 'Raw 16bpp(565)', [0], pf16=True)
        ok &= check_static(a.port, a.password, 'Tight(ロスレス)', [7])
        ok &= check_static(a.port, a.password, 'Tight 圧縮 6', [7, -256 + 6])
        ok &= check_static(a.port, a.password, 'Tight + JPEG 画質 9', [7, Q(9)], min_psnr=40)
        ok &= check_static(a.port, a.password, 'Tight + JPEG 画質 5', [7, Q(5)], min_psnr=27)
        ok &= check_static(a.port, a.password, 'Tight + JPEG 画質 0', [7, Q(0)], min_psnr=22)
    else:
        ok &= check_animated(a.port, a.password, 'Tight + CopyRect', [7, 1, -239, -232, -258])
        ok &= check_animated(a.port, a.password, 'ZRLE + CopyRect', [16, 1, -239])
        ok &= check_animated(a.port, a.password, 'Tight(CopyRect なし)', [7])
        ok &= check_animated(a.port, a.password, 'Raw', [0])
        ok &= check_animated(a.port, a.password, 'Tight + JPEG 8 + CopyRect', [7, 1, -32 + 8], min_psnr=35)
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
