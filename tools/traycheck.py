"""サービスのときのトレイ(-tray)で、アイコンを 1 回左クリックすると設定画面が 1 枚だけ開くか確かめる。

    python tools/traycheck.py

-tray を起動し、シェルが左クリック 1 回で送る通知(WM_LBUTTONUP、続けて NIN_SELECT)をトレイの窓へ送る。
開いた設定画面(管理者の -svcsettings。この PC の UAC は確認なしで昇格する)の枚数を数える。
続けて、もう 1 回クリックしても 2 枚目が開かず、1 枚のままかも見る。
サービスは登録しない(-tray は 3 秒ごとに登録を確かめ、無ければふだんのトレイ常駐で動き直すので、
その前に済ませる。動き直したものは -exit で止める)。設定画面は管理者なので、管理者の PowerShell で閉じる。
利用者のマウス・キーボードは使わない。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, 'iivnc-server.exe')
DIR = os.path.join(ROOT, 'build', 'traytest')
INI = os.path.join(DIR, 'tray.ini')
WM_APP_TRAY = 0x8000 + 1
WM_LBUTTONUP, NIN_SELECT = 0x0202, 0x0400

u = ctypes.WinDLL('user32', use_last_error=True)
u.FindWindowExW.restype = wintypes.HWND
u.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
u.FindWindowW.restype = wintypes.HWND
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]


def settings_windows():
    """設定画面(#32770「iivnc-server」)を開いているプロセスの一覧"""
    pids, h = [], None
    while True:
        h = u.FindWindowExW(None, h, '#32770', 'iivnc-server')
        if not h:
            return pids
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        pids.append(p.value)


def close_elevated():
    script = os.path.join(DIR, 'close.ps1')
    open(script, 'w', encoding='utf-8-sig').write(
        "Get-CimInstance Win32_Process -Filter \"Name='iivnc-server.exe'\" | "
        f"? {{ $_.CommandLine -like '*svcsettings*' -and $_.CommandLine -like '*{INI}*' }} | "
        "% { Stop-Process -Id $_.ProcessId -Force }\n")
    subprocess.run(['powershell', '-NoProfile', '-Command',
                    f"Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList "
                    f"'-NoProfile','-ExecutionPolicy','Bypass','-File','{script}'"])


def click(tray):
    u.PostMessageW(tray, WM_APP_TRAY, 0, WM_LBUTTONUP | (1 << 16))
    u.PostMessageW(tray, WM_APP_TRAY, 0, NIN_SELECT | (1 << 16))


def main():
    os.makedirs(DIR, exist_ok=True)
    open(INI, 'w', encoding='utf-8', newline='\n').write('port=5994\nlisten=127.0.0.1\nnotify=0\nlog=1\n')
    before = len(settings_windows())
    p = subprocess.Popen([EXE, '-ini', INI, '-tray'])
    ok = True
    try:
        tray = None
        for _ in range(30):
            h = None
            while True:
                h = u.FindWindowExW(None, h, None, 'iivnc-server')
                if not h:
                    break
                pid = wintypes.DWORD()
                u.GetWindowThreadProcessId(h, ctypes.byref(pid))
                if pid.value == p.pid:
                    tray = h
                    break
            if tray:
                break
            time.sleep(0.05)
        click(tray)
        time.sleep(0.4)
        click(tray)                                     # もう 1 回(すでに開いていれば増えない)
        time.sleep(4)
        n = len(settings_windows()) - before
        ok = n == 1
        print(f'  {"OK" if ok else "NG"}  左クリック 2 回で開いた設定画面: {n} 枚(期待 1)')
    finally:
        close_elevated()
        subprocess.run([EXE, '-ini', INI, '-exit'])
        if p.poll() is None:
            p.kill()
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
