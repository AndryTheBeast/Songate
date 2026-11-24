"""The Switch preview (the console's UI built for the PC, 1280x720), dev only (session 23): Settings > Debug > Self test run unattended
(GF_SELFTEST, GF_SELFTEST_QUIT) on a card of its own: tools/preview/out/st-card, sdmc:/Music a junction to the library, guarded read
only (GF_PROTECT), the config a copy of the live environment's. python tools/preview/devpreviewst.py <0|1|2> <out.txt> [timeout s]"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent.parent
OUT = HERE / 'out'
CARD = OUT / 'st-card'
CFG = CARD / 'sdmc' / 'config' / 'songate-nx'
LIVE_CFG = HERE / 'out-live' / 'card' / 'sdmc' / 'config' / 'songate-nx'
MUSIC = Path(os.environ.get('GF_LIVE_MUSIC', r'D:\Music'))

level = int(sys.argv[1])
dest = Path(sys.argv[2])
limit = float(sys.argv[3]) if len(sys.argv) > 3 else 3600
name = ['basic', 'advanced', 'full'][level]
(CARD / 'sdmc').mkdir(parents=True, exist_ok=True)
music = CARD / 'sdmc' / 'Music'
if not music.exists():
    subprocess.run(['cmd', '/c', 'mklink', '/J', str(music), str(MUSIC)], check=True, capture_output=True)
if not (CFG / 'settings.txt').exists():
    CFG.parent.mkdir(parents=True, exist_ok=True)
    if LIVE_CFG.exists():
        shutil.copytree(LIVE_CFG, CFG, ignore=shutil.ignore_patterns('*.log', 'app-log*', '*-account*', '*-queue.tsv'), dirs_exist_ok=True)
    else:
        CFG.mkdir(parents=True, exist_ok=True)
        shutil.copy(HERE / 'live' / 'card-settings.txt', CFG / 'settings.txt')
res = CFG / f'selftest-{name}.txt'
if res.exists():
    res.unlink()
env = os.environ.copy()
for k in ('GF_SHOW', 'GF_SCRIPT', 'GF_STREAM'):
    env.pop(k, None)
env.update(GF_ROOT=str(CARD), GF_ROMFS=str(PROJECT / 'romfs'), SDL_AUDIODRIVER='dummy', GF_PROTECT='sdmc:/Music',
           GF_SELFTEST=str(level), GF_SELFTEST_QUIT='1')
exe = Path(os.environ.get('GF_SHOT_EXE', OUT / 'gatefold-preview.exe'))
t0 = time.time()
with open(OUT / 'st-app-log.txt', 'ab') as log:
    app = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        app.wait(timeout=limit)
    except subprocess.TimeoutExpired:
        print('TIMEOUT: still running after', limit, 's')
        app.kill()
print('ran', round(time.time() - t0), 's, exit', app.returncode)
if res.exists():
    shutil.copy(res, dest)
    print('result', dest)
else:
    print('NO RESULT FILE')
