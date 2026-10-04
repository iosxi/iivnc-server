"""設定画面の「ネットワークの許可」(ファイアウォールの、この exe の規則を数える・消す)を確かめる。

    python tools/fwcheck.py [exe]

利用者の本物の規則には触れないよう、exe を build/fwtest/ に写し、その写しを対象にした仮の規則を
管理者の PowerShell で作る(TCP 許可・パブリック、UDP ブロック・プライベート。どちらも無効にしておく)。
名前は Windows が作るものと同じ「iivnc-server (VNC サーバー)」にして、同じ名前の本物の規則
(別の場所の exe 向け)を巻き込まないことも確かめる。
写しを -settings で開き、欄の文を読み、「許可を消す」を押す(WM_COMMAND。この PC の UAC は
確認なしで昇格する)。押したあと、写しの規則が 0 件、本物の規則の数が変わらないことを見る。
exe を渡すと、その exe(iivnc-client.exe など)で同じことをする(接続の画面のボタンを押す)。
"""
import ctypes, os, shutil, subprocess, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC_EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'iivnc-server.exe')
NAME = os.path.basename(SRC_EXE)
IS_CLIENT = 'client' in NAME
DIR = os.path.join(ROOT, 'build', 'fwtest')
EXE = os.path.join(DIR, NAME)
RULE_NAME = 'iivnc-client (VNC ビューア)' if IS_CLIENT else 'iivnc-server (VNC サーバー)'
IDC_FWTEXT, IDC_FWREMOVE = (1009, 1011) if IS_CLIENT else (1022, 1023)

u = ctypes.WinDLL('user32', use_last_error=True)
u.FindWindowExW.restype = wintypes.HWND
u.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetDlgItem.restype = wintypes.HWND
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
u.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u.IsWindowEnabled.argtypes = [wintypes.HWND]


def ps(cmd, admin=False):
    if not admin:
        return subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True,
                              encoding='cp932', errors='replace').stdout.strip()
    script = os.path.join(DIR, 'admin.ps1')
    out = os.path.join(DIR, 'admin.out')
    open(script, 'w', encoding='utf-8-sig').write(f'& {{ {cmd} }} *> "{out}"\n')
    subprocess.run(['powershell', '-NoProfile', '-Command',
                    f"Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList "
                    f"'-NoProfile','-ExecutionPolicy','Bypass','-File','{script}'"])
    return open(out, encoding='utf-8', errors='replace').read().strip() if os.path.exists(out) else ''


def count(path):
    return int(ps(f"@((New-Object -ComObject HNetCfg.FwPolicy2).Rules | ? {{ $_.ApplicationName -ieq '{path}' }}).Count") or -1)


def text(h):
    b = ctypes.create_unicode_buffer(400)
    u.GetWindowTextW(h, b, 400)
    return b.value


def find_dialog(pid):
    h = None
    while True:
        h = u.FindWindowExW(None, h, '#32770', None)
        if not h:
            return None
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid:
            return h


def main():
    os.makedirs(DIR, exist_ok=True)
    shutil.copy2(SRC_EXE, EXE)
    ini = os.path.join(DIR, 'fw.ini')
    open(ini, 'w', encoding='utf-8', newline='\n').write(
        'log=1\n' if IS_CLIENT else 'port=5997\nlisten=127.0.0.1\nnotify=0\nlog=1\n')
    real = SRC_EXE.replace('/', '\\')
    real_before = count(real)
    mk = ''.join(f"$r = New-Object -ComObject HNetCfg.FWRule; $r.Name = '{RULE_NAME}'; $r.ApplicationName = '{EXE}'; "
                 f"$r.Protocol = {proto}; $r.Direction = 1; $r.Action = {act}; $r.Enabled = $false; $r.Profiles = {prof}; "
                 f"(New-Object -ComObject HNetCfg.FwPolicy2).Rules.Add($r); "
                 for proto, act, prof in ((6, 1, 4), (17, 0, 2)))
    ps(mk, admin=True)
    print(f'  仮の規則を作った: 写し {count(EXE)} 件、本物 {real_before} 件')
    args = [EXE, '-ini', ini] + ([] if IS_CLIENT else ['-settings'])
    p = subprocess.Popen(args)
    ok = True
    try:
        d = None
        for _ in range(50):
            d = find_dialog(p.pid)
            if d:
                break
            time.sleep(0.1)
        time.sleep(0.8)
        t, b = u.GetDlgItem(d, IDC_FWTEXT), u.GetDlgItem(d, IDC_FWREMOVE)
        before = text(t) if not IS_CLIENT else text(b)
        print(f'  押す前: 「{before}」 ボタン {"有効" if u.IsWindowEnabled(b) else "無効"}')
        ok &= bool(u.IsWindowEnabled(b))
        u.PostMessageW(d, 0x111, IDC_FWREMOVE, b)        # WM_COMMAND(BN_CLICKED)
        time.sleep(6)
        after = text(t)
        print(f'  押した後: 「{after}」 ボタン {"有効" if u.IsWindowEnabled(b) else "無効"}')
        ok &= not u.IsWindowEnabled(b)
    finally:
        if IS_CLIENT:
            p.kill()
        else:
            subprocess.run([EXE, '-ini', ini, '-exit'])
            if p.poll() is None:
                p.kill()
    left, real_after = count(EXE), count(real)
    print(f'  写しの規則の残り: {left} 件、本物の規則: {real_before} → {real_after} 件')
    ok &= left == 0 and real_after == real_before
    if left:
        ps(f"$p = New-Object -ComObject HNetCfg.FwPolicy2; @($p.Rules | ? {{ $_.ApplicationName -ieq '{EXE}' }}) | "
           f"% {{ $n = 'iivnc-cleanup-' + [guid]::NewGuid(); $_.Name = $n; $p.Rules.Remove($n) }}", admin=True)
        print(f'  後始末した: 残り {count(EXE)} 件')
    log = os.path.join(DIR, 'fw.log')
    if os.path.exists(log):
        print(''.join('  ' + l for l in open(log, encoding='utf-8') if 'ファイアウォール' in l))
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
