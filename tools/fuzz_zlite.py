"""zdeflate / zinflate をランダムな入力で本家 zlib と突き合わせる。

    python tools/fuzz_zlite.py [回数]
"""
import os, random, zlib, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'tools', 'test_zlite.exe')
T = os.path.join(ROOT, 'build', 'tools', 'zdata', 'fz')
os.makedirs(os.path.dirname(T), exist_ok=True)


def sample(rnd, n):
    parts = []
    while sum(map(len, parts)) < n:
        k = rnd.random()
        if k < 0.3:
            parts.append(os.urandom(rnd.randrange(1, 50)))
        elif k < 0.6:
            parts.append(bytes([rnd.randrange(256)]) * rnd.randrange(1, 600))
        elif parts and k < 0.9:
            p = rnd.choice(parts)
            parts.append(p[:rnd.randrange(1, len(p) + 1)])
        else:
            parts.append(bytes(rnd.randrange(4) for _ in range(rnd.randrange(1, 300))))
    return b''.join(parts)[:n]


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rnd = random.Random(7)
    bad = 0
    for it in range(count):
        data = sample(rnd, rnd.randrange(1, 6000) if it % 3 else rnd.randrange(1, 200000))
        open(T, 'wb').write(data)
        lv = rnd.randrange(10)
        ch = rnd.choice([1, 7, 100, 4096, 65536, 1 << 20])
        r = subprocess.run([EXE, 'comp', T, T + '.z', str(lv), str(ch)], capture_output=True, text=True)
        back = zlib.decompressobj().decompress(open(T + '.z', 'rb').read()) if r.returncode == 0 else None
        r2 = subprocess.run([EXE, 'round', T, str(lv), str(ch)], capture_output=True, text=True)
        if back != data or r2.returncode:
            bad += 1
            print('NG', it, len(data), lv, ch, r.stdout.strip(), r2.stdout.strip())
    print(f'{count} 回中 不一致 {bad}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
