"""Songate for PC, dev only (session 23): the dev build started N times on a settings folder of its own, each watched for 15 s: did it
live, how did it end (exit code), what did it print. For a start that died at once with nothing in its log.
python tools/preview/devlaunch.py <times> [fresh]"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXE = Path(os.environ.get('SONGATE_EXE', HERE.parent.parent / 'build-windows-check' / 'Songate.exe'))
n = int(sys.argv[1]) if len(sys.argv) > 1 else 5
fresh = 'fresh' in sys.argv
cfg = HERE / 'out' / 'launch-cfg'
for i in range(n):
    if fresh or not cfg.exists():
        shutil.rmtree(cfg, ignore_errors=True)
        (cfg / 'Songate').mkdir(parents=True)
        (cfg / 'Songate' / 'settings.txt').write_text('musicRoot=D:/Music\nopening=0\nwindow=60 60 1280 720 0\nmotion=1\nbackground=0\n')
    env = os.environ.copy()
    env.update(GF_CONFIG=str(cfg), GF_READONLY='D:/Music', SDL_AUDIODRIVER='dummy')
    log = cfg / f'out-{i}.txt'
    with open(log, 'wb') as out:
        p = subprocess.Popen([str(EXE)], cwd=str(EXE.parent), env=env, stdout=out, stderr=subprocess.STDOUT)
        t0 = time.time()
        while time.time() - t0 < 15 and p.poll() is None:
            time.sleep(0.2)
        alive = p.poll() is None
        code = p.returncode
        if alive:
            p.kill()
            p.wait()
    print(f'start {i}: {"alive after 15 s" if alive else f"ENDED after {time.time() - t0:.1f} s, exit {code} (0x{(code or 0) & 0xffffffff:08x})"}; '
          f'printed {log.stat().st_size} bytes; log.txt {"there" if (cfg / "Songate" / "log.txt").exists() else "MISSING"}')
