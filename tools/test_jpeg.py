"""jpegenc を PIL(libjpeg)と比べる。画質(PSNR)・大きさ・速さ。WIC の展開の速さも測る。

    python tools/test_jpeg.py
"""
import io, os, subprocess, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_zlite import desktop_like, bgrx  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'tools', 'test_jpeg.exe')
TMP = os.path.join(ROOT, 'build', 'tools', 'jdata')
os.makedirs(TMP, exist_ok=True)


def psnr(a, b):
    mse = np.mean((np.asarray(a, np.float64) - np.asarray(b, np.float64)) ** 2)
    return 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)


def main():
    im = desktop_like(1920, 1080)
    raw = os.path.join(TMP, 'desk.bgrx')
    open(raw, 'wb').write(bgrx(im))
    ok = True
    print('品質 間引き  自前:大きさ PSNR 時間(1920x1080 全体)  | PIL:大きさ PSNR')
    for q in (30, 60, 80, 95):
        for ss, pil_ss in ((0, 0), (2, 2)):
            out = os.path.join(TMP, f'q{q}s{ss}.jpg')
            r = subprocess.run([EXE, 'enc', raw, '1920', '1080', str(q), str(ss), out], capture_output=True, text=True, encoding='utf-8')
            total, first, ms = r.stdout.split()
            dec = Image.open(out).convert('RGB')
            p1 = psnr(im, dec)
            buf = io.BytesIO()
            im.save(buf, 'JPEG', quality=q, subsampling=pil_ss)
            p2 = psnr(im, Image.open(io.BytesIO(buf.getvalue())).convert('RGB'))
            name = {0: '4:4:4', 2: '4:2:0'}[ss]
            print(f'{q:4d} {name}  {int(total):9d} {p1:5.2f} {float(ms):6.1f}ms | {len(buf.getvalue()):9d} {p2:5.2f}')
            if p1 < p2 - 1.0:
                ok = False
    # タイルごと(128x128)の速さと大きさ
    for tile in (64, 128, 256):
        r = subprocess.run([EXE, 'enc', raw, '1920', '1080', '95', '0', os.path.join(TMP, 't.jpg'), str(tile)],
                           capture_output=True, text=True, encoding='utf-8')
        total, first, ms = r.stdout.split()
        print(f'タイル {tile}x{tile} 品質95 4:4:4: 合計 {int(total)} バイト  {float(ms):.1f}ms')
        if tile == 128:
            dec = Image.open(os.path.join(TMP, 't.jpg')).convert('RGB')
            print(f'   先頭タイルを PIL で展開: {dec.size} PSNR {psnr(im.crop((0, 0, 128, 128)), dec):.2f}')
    for f in ('t.jpg', 'q95s0.jpg'):
        r = subprocess.run([EXE, 'wicdec', os.path.join(TMP, f), '200'], capture_output=True, text=True, encoding='utf-8')
        print(f'WIC 展開 {f}: {r.stdout.strip()}')
    # 壊れていないか: 大きさ・品質・間引きを変えた JPEG を WIC で展開し、警告(Corrupt JPEG data など)が出ないこと
    import random
    rnd = random.Random(3)
    bad = 0
    for k in range(60):
        w, h = rnd.randrange(1, 300), rnd.randrange(1, 300)
        q, ss = rnd.choice([1, 15, 50, 80, 95, 100]), rnd.choice([0, 1, 2])
        crop = np.asarray(im)[:h, :w][:, :, ::-1]
        raw2 = os.path.join(TMP, 'r.bgrx')
        a = np.zeros((h, w, 4), np.uint8); a[:, :, :3] = crop; a[:, :, 3] = 255
        open(raw2, 'wb').write(a.tobytes())
        out = os.path.join(TMP, 'r.jpg')
        subprocess.run([EXE, 'enc', raw2, str(w), str(h), str(q), str(ss), out], capture_output=True)
        r = subprocess.run([EXE, 'wicdec', out, '1'], capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.stderr.strip() or r.returncode:
            bad += 1
            print(f'  警告 {w}x{h} q{q} s{ss}: {r.stderr.strip()[:80]}')
    print(f'WIC の警告: 60 枚中 {bad} 枚')
    ok &= bad == 0
    print('OK' if ok else 'NG: PSNR が PIL より 1dB 以上低い')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
