"""iivnc-server のアイコンを作る。

    python tools/make-icon.py

src/iivnc-server.ico         ふだん(青い画面)
src/iivnc-server-active.ico  誰かが接続中(緑の画面)
src/iivnc-server.png         README 用
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def draw(size, screen, accent):
    s = 1024
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 画面の枠
    d.rounded_rectangle([60, 140, 964, 760], radius=90, fill=(40, 44, 52, 255))
    d.rounded_rectangle([120, 200, 904, 700], radius=40, fill=screen)
    # 台
    d.rectangle([452, 760, 572, 860], fill=(40, 44, 52, 255))
    d.rounded_rectangle([300, 840, 724, 920], radius=40, fill=(40, 44, 52, 255))
    # 送る矢印(右上へ)
    d.polygon([(560, 300), (800, 300), (800, 540), (720, 460), (520, 660), (440, 580), (640, 380)], fill=accent)
    return im.resize((size, size), Image.LANCZOS)


def save(name, screen, accent):
    imgs = [draw(n, screen, accent) for n in SIZES]
    path = os.path.join(ROOT, 'src', name)
    imgs[-1].save(path, format='ICO', sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])
    return imgs[-1]


big = save('iivnc-server.ico', (52, 120, 246, 255), (255, 255, 255, 255))
save('iivnc-server-active.ico', (22, 163, 74, 255), (255, 255, 255, 255))
big.resize((128, 128), Image.LANCZOS).save(os.path.join(ROOT, 'src', 'iivnc-server.png'))
print('ok')
