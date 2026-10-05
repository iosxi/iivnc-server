"""ファイルのコピー＆貼り付け(filexfer.c)を、この PC の中で確かめる。

    python tools/fxcheck.py [c2s|s2c|viewonly|all]
    python tools/fxcheck.py --service c2s|s2c     (サービスとして登録済みの build/svctest/fx.ini 相手。登録・解除は別に)

サーバー(ポート 5993、127.0.0.1)とクライアントをこの PC で動かす。同じ PC なのでクリップボードは 1 つ。
送る側だけが「つながったら今のクリップボードのファイルを渡す」(サーバーは ini の fxoffer=1、クライアントは
-fxoffer)。受ける側はそれを本物のクリップボードに置き、tools/pastetest.exe(エクスプローラーの貼り付けと同じく、
貼り付け先フォルダのドロップの受け口へ落とす)で貼り付けて、SHA-256 を比べる。
  c2s: クライアントでコピー → サーバーで貼り付け
  s2c: サーバーでコピー → クライアントで貼り付け
  viewonly: 見るだけ(クライアントの -viewonly / サーバーの viewonly=1)では一覧も中身も渡らない
クリップボードの文字は保存して最後に戻す。Windows のコピーの画面が一瞬出ることがある。
"""
import hashlib, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SEXE = os.path.join(ROOT, 'iivnc-server.exe')
CEXE = os.path.join(os.path.dirname(ROOT), 'iivnc-client', 'iivnc-client.exe')
DIR = os.path.join(ROOT, 'build', 'fxtest')
SRC = os.path.join(DIR, 'src')
SERVICE = '--service' in sys.argv
if SERVICE:
    sys.argv.remove('--service')
PORT = 5992 if SERVICE else 5993
SVC_INI = os.path.join(ROOT, 'build', 'svctest', 'fx.ini')
PASTE = os.path.join(ROOT, 'build', 'tools', 'pastetest.exe')     # tools/build-tools.bat で作る


def ps(cmd):
    r = subprocess.run(['powershell', '-NoProfile', '-STA', '-Command', cmd], capture_output=True)
    return r.stdout.decode('cp932', 'replace').strip()


def make_data():
    if os.path.exists(SRC):
        shutil.rmtree(SRC)
    os.makedirs(os.path.join(SRC, 'フォルダ', '入れ子'))
    with open(os.path.join(SRC, 'big.bin'), 'wb') as f:
        f.write(os.urandom(50 << 20))
    open(os.path.join(SRC, 'zero.txt'), 'wb').close()
    open(os.path.join(SRC, '日本語の名前 a..b.txt'), 'w', encoding='utf-8').write('こんにちは\n' * 1000)
    open(os.path.join(SRC, 'フォルダ', 'mid.dat'), 'wb').write(os.urandom(700_000))
    open(os.path.join(SRC, 'フォルダ', '入れ子', 'deep.txt'), 'wb').write(b'deep' * 12345)
    return ['big.bin', 'zero.txt', '日本語の名前 a..b.txt', 'フォルダ']


def tree_hashes(base):
    out = {}
    for root, dirs, files in os.walk(base):
        for d in dirs:
            out[os.path.relpath(os.path.join(root, d), base) + '\\'] = 'dir'
        for f in files:
            p = os.path.join(root, f)
            out[os.path.relpath(p, base)] = hashlib.sha256(open(p, 'rb').read()).hexdigest()
    return out


LOG_FROM = {}      # サービスの記録は前からあるので、試験を始めた位置より後だけを見る


def log_text(path):
    if not os.path.exists(path):
        return ''
    with open(path, 'rb') as f:
        f.seek(LOG_FROM.get(path, 0))
        return f.read().decode('utf-8', 'replace')


def log_has(path, text):
    return text in log_text(path)


def wait_log(path, text, sec=10):
    for _ in range(int(sec * 20)):
        if log_has(path, text):
            return True
        time.sleep(0.05)
    return False


def run(mode):
    """mode: c2s / s2c / viewonly-client / viewonly-server"""
    items = make_data()
    dest = os.path.join(DIR, 'dest-' + mode)
    if os.path.exists(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    sini = SVC_INI if SERVICE else os.path.join(DIR, 'server.ini')
    slog = sini[:-4] + '.log'
    cini = os.path.join(DIR, 'client.ini')
    clog = cini[:-4] + '.log'
    for f in ((clog,) if SERVICE else (slog, clog)):
        if os.path.exists(f):
            os.remove(f)
    LOG_FROM[slog] = os.path.getsize(slog) if SERVICE and os.path.exists(slog) else 0
    sv = f'port={PORT}\nlisten=127.0.0.1\nnotify=0\nlog=1\n'
    if mode == 's2c':
        sv += 'fxoffer=1\n'
    if mode == 'viewonly-server':
        sv += 'viewonly=1\n'
    if not SERVICE:
        open(sini, 'w', encoding='utf-8', newline='\n').write(sv)
    open(cini, 'w', encoding='utf-8', newline='\n').write('log=1\n')
    # クリップボードにファイルを置く(エクスプローラーでコピーしたのと同じ CF_HDROP)
    paths = ','.join("'" + os.path.join(SRC, i).replace("'", "''") + "'" for i in items)
    # サービスの s2c は、つないだあとで利用者がコピーする(サービスのトレイが見つけて分身へ渡す)
    copy_later = SERVICE and mode == 's2c'
    if not copy_later:
        ps(f'Set-Clipboard -Path {paths}')
    sp = None if SERVICE else subprocess.Popen([SEXE, '-ini', sini])
    time.sleep(1.5)
    cargs = [CEXE, f'127.0.0.1::{PORT}', '-ini', cini, '-idleexit', '600000']
    if mode in ('c2s', 'viewonly-client', 'viewonly-server'):
        cargs.append('-fxoffer')
    if mode == 'viewonly-client':
        cargs.append('-viewonly')
    if copy_later:
        cargs.append('-fxnowatch')      # 同じ PC なので、クライアントがこのコピーを相手へ渡さないように
    cp = subprocess.Popen(cargs)
    if copy_later:
        wait_log(clog, 'コピー＆貼り付け: 使える', 8)
        time.sleep(0.5)
        ps(f'Set-Clipboard -Path {paths}')
    ok = True
    try:
        receiver_log = slog if mode in ('c2s', 'viewonly-client', 'viewonly-server') else clog
        placed = wait_log(receiver_log, 'クリップボードに置きました', 8)
        if mode.startswith('viewonly'):
            ok = not placed
            hello = open(clog, encoding='utf-8').read() if os.path.exists(clog) else ''
            print(f'  {"OK" if ok else "NG"}  {mode}: 一覧が{"渡った" if placed else "渡らなかった"}(期待: 渡らない)'
                  f'  クライアントの記録: {[l[24:].strip() for l in hello.splitlines() if "コピー＆貼り付け" in l]}')
            return ok
        if not placed:
            print(f'  NG  {mode}: 受ける側がクリップボードに置かなかった')
            return False
        t0 = time.time()
        # エクスプローラーの貼り付けと同じく、貼り付け先のドロップの受け口へ落とす(tools/pastetest.c)
        r = subprocess.run([PASTE, dest], capture_output=True, timeout=300)
        pasted = r.stdout.decode('cp932', 'replace').strip()
        want = tree_hashes(SRC)
        got = {}
        for _ in range(50):
            got = tree_hashes(dest)
            if got == want:
                break
            time.sleep(0.1)
        dt = time.time() - t0
        ok = got == want
        miss = sorted(set(want) - set(got)) + [k for k in want if k in got and got[k] != want[k]]
        print(f'  {"OK" if ok else "NG"}  {mode}: {len(want)} 件(フォルダを含む)  一致 {sum(1 for k in want if got.get(k) == want[k])}'
              f'  {dt:.1f} 秒  {pasted}' + (f'  違うもの {miss[:5]}' if miss else ''))
    finally:
        cp.kill()
        if sp:
            subprocess.run([SEXE, '-ini', sini, '-exit'])
            if sp.poll() is None:
                sp.kill()
    for name, path in (('サーバー', slog), ('クライアント', clog)):
        if os.path.exists(path):
            for l in log_text(path).splitlines():
                if any(k in l for k in ('ファイル', 'コピー＆貼り付け', '貼り付け', 'COM')):
                    print(f'    {name}: {l[24:].rstrip()}')
    return ok


def main():
    os.makedirs(DIR, exist_ok=True)
    which = sys.argv[1] if len(sys.argv) > 1 else 'all'
    saved = ps('Get-Clipboard -Raw')
    ok = True
    try:
        modes = {'c2s': ['c2s'], 's2c': ['s2c'], 'viewonly': ['viewonly-client', 'viewonly-server'],
                 'all': ['c2s', 's2c', 'viewonly-client', 'viewonly-server']}[which]
        for m in modes:
            ok &= run(m)
    finally:
        if saved:
            open(os.path.join(DIR, 'saved.txt'), 'w', encoding='utf-8').write(saved)
            ps(f"Set-Clipboard -Value (Get-Content -Raw -Encoding UTF8 '{os.path.join(DIR, 'saved.txt')}')")
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
