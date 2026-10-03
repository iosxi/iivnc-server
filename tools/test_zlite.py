"""zdeflate / zinflate を本家 zlib(Python)と突き合わせる。

    python tools/test_zlite.py

1. 自前の deflate で圧縮したストリームを zlib.decompressobj で展開して元と比べる
   (区切りごとに独立に圧縮したものをつないで 1 本のストリームになっているか)。
2. zlib で Z_SYNC_FLUSH 区切りに圧縮したものを、自前の inflate で区切りごとに展開して比べる。
3. 自前同士の往復。
試すデータは画面に似せた絵(文字・UI・写真・グラデーション)の画素、ソースの文字、乱数。
"""
import os, sys, zlib, random, subprocess
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, 'build', 'tools', 'test_zlite.exe')
TMP = os.path.join(ROOT, 'build', 'tools', 'zdata')
os.makedirs(TMP, exist_ok=True)


def desktop_like(w=1920, h=1080, seed=1):
    rnd = random.Random(seed)
    im = Image.new('RGB', (w, h), (243, 243, 243))
    d = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype('C:/Windows/Fonts/YuGothM.ttc', 15)
    except OSError:
        font = ImageFont.load_default()
    # 写真っぽい領域
    photo = Image.effect_noise((640, 400), 60).convert('RGB')
    grad = Image.linear_gradient('L').resize((640, 400)).convert('RGB')
    photo = Image.blend(photo, grad, 0.6).filter(ImageFilter.GaussianBlur(2))
    im.paste(photo, (1200, 100))
    # ウィンドウと文字
    for k in range(6):
        x, y = rnd.randrange(0, w - 700), rnd.randrange(0, h - 500)
        d.rectangle([x, y, x + 680, y + 480], fill=(255, 255, 255), outline=(200, 200, 200))
        d.rectangle([x, y, x + 680, y + 30], fill=(rnd.randrange(200, 255),) * 3)
        for line in range(20):
            text = ''.join(rnd.choice('abcdefghijklmnopqrstuvwxyz あいうえおかきくけこ漢字表示') for _ in range(60))
            d.text((x + 10, y + 40 + line * 22), text, fill=(20, 20, 20), font=font)
    return im


def bgrx(im):
    r, g, b = im.split()
    return Image.merge('RGBA', (b, g, r, Image.new('L', im.size, 255))).tobytes()


def datasets():
    out = {}
    out['desktop'] = bgrx(desktop_like())
    src = b''
    for root, _, files in os.walk(os.path.join(ROOT, 'src')):
        for f in files:
            if f.endswith(('.c', '.h')):
                src += open(os.path.join(root, f), 'rb').read()
    out['text'] = src * 4
    out['random'] = os.urandom(300000)
    out['zeros'] = bytes(500000)
    out['tiny'] = b'abcabcabcabcab'
    return out


def run(*args):
    r = subprocess.run([EXE, *args], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        raise SystemExit('FAILED: ' + ' '.join(args))
    return r.stdout.strip()


def main():
    ok = True
    for name, data in datasets().items():
        p = os.path.join(TMP, name + '.bin')
        open(p, 'wb').write(data)
        # 1. 自前 deflate → zlib
        for level in (0, 1, 3, 6, 9):
            for chunk in (65536, 7777):
                z = p + f'.{level}.{chunk}.z'
                line = run('comp', p, z, str(level), str(chunk))
                comp = open(z, 'rb').read()
                d = zlib.decompressobj()
                back = d.decompress(comp)
                good = back == data
                ok &= good
                if chunk == 65536 or not good:
                    print(f'{name:8s} {line}  zlib展開 {"一致" if good else "不一致!"}  (参考 zlib level{max(level,1)}: '
                          f'{len(zlib.compress(data, max(level, 1))) * 100 / max(len(data), 1):.1f}%)')
        # 2. zlib → 自前 inflate
        c = zlib.compressobj(6)
        comp, lens, pos = b'', [], 0
        rnd = random.Random(2)
        while pos < len(data):
            n = min(len(data) - pos, rnd.randrange(1, 100000))
            piece = c.compress(data[pos:pos + n]) + c.flush(zlib.Z_SYNC_FLUSH)
            comp += piece
            lens.append((len(piece), n))
            pos += n
        zp = p + '.zlib'
        open(zp, 'wb').write(comp)
        open(zp + '.lens', 'w').write(''.join(f'{a} {b}\n' for a, b in lens))
        line = run('infl', zp, zp + '.lens', zp + '.out')
        good = open(zp + '.out', 'rb').read() == data
        ok &= good
        print(f'{name:8s} zlib→自前展開: {line}  {"一致" if good else "不一致!"}')
        # 3. 往復
        print(f'{name:8s} {run("round", p, "1", "65536")}')
    print('ALL OK' if ok else 'SOME FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
