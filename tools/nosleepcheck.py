"""接続している間だけ、スリープと画面の消灯を止めているか確かめる(nosleep)。

    python tools/nosleepcheck.py

サーバー(ポート 5991、127.0.0.1)とクライアントをこの PC で動かし、管理者の powercfg /requests
(この PC の UAC は確認なしで昇格する)で、電源の要求(SYSTEM と DISPLAY)に iivnc の理由が出ているかを見る。
  1. つないでいる間: サーバーとクライアントの両方が SYSTEM と DISPLAY に出る
  2. クライアントを閉じた後: どちらも消える
  3. nosleep=0(両方): つないでも出ない
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SEXE = os.path.join(ROOT, 'iivnc-server.exe')
CEXE = os.path.join(os.path.dirname(ROOT), 'iivnc-client', 'iivnc-client.exe')
DIR = os.path.join(ROOT, 'build', 'nosleep')
PORT = 5991
REASONS = {'サーバー': 'iivnc-server: ビューアが接続している', 'クライアント': 'iivnc-client: サーバーにつないでいる'}


def requests():
    """powercfg /requests の 区分 → 中身"""
    out = os.path.join(DIR, 'req.txt')
    if os.path.exists(out):
        os.remove(out)
    script = os.path.join(DIR, 'req.ps1')
    open(script, 'w', encoding='utf-8-sig').write(
        f"powercfg /requests | Out-File -Encoding utf8 '{out}'\n")
    subprocess.run(['powershell', '-NoProfile', '-Command',
                    f"Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList "
                    f"'-NoProfile','-ExecutionPolicy','Bypass','-File','{script}'"])
    text = open(out, encoding='utf-8-sig').read()
    sec, cur = {}, None
    for line in text.splitlines():
        m = re.match(r'^([A-Z]+):\s*$', line)
        if m:
            cur = m.group(1)
            sec[cur] = ''
        elif cur:
            sec[cur] += line + '\n'
    return sec


def state(sec):
    return {who: [k for k in ('SYSTEM', 'DISPLAY') if r in sec.get(k, '')] for who, r in REASONS.items()}


def run(nosleep):
    sini = os.path.join(DIR, 'server.ini')
    cini = os.path.join(DIR, 'client.ini')
    open(sini, 'w', encoding='utf-8', newline='\n').write(f'port={PORT}\nlisten=127.0.0.1\nnotify=0\nlog=1\nnosleep={nosleep}\n')
    open(cini, 'w', encoding='utf-8', newline='\n').write(f'log=1\nnosleep={nosleep}\n')
    sp = subprocess.Popen([SEXE, '-ini', sini])
    time.sleep(1.5)
    cp = subprocess.Popen([CEXE, f'127.0.0.1::{PORT}', '-ini', cini, '-idleexit', '600000'])
    time.sleep(3)
    during = state(requests())
    cp.kill()
    time.sleep(2)
    after = state(requests())
    subprocess.run([SEXE, '-ini', sini, '-exit'])
    return during, after


def main():
    os.makedirs(DIR, exist_ok=True)
    ok = True
    during, after = run(1)
    for who in REASONS:
        good = during[who] == ['SYSTEM', 'DISPLAY'] and after[who] == []
        ok &= good
        print(f'  {"OK" if good else "NG"}  nosleep=1 {who}: つないでいる間 {during[who]}、閉じた後 {after[who]}')
    during, after = run(0)
    for who in REASONS:
        good = during[who] == [] and after[who] == []
        ok &= good
        print(f'  {"OK" if good else "NG"}  nosleep=0 {who}: つないでいる間 {during[who]}、閉じた後 {after[who]}')
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
