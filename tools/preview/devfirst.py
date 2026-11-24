"""Songate for PC, dev only (session 23): a true first start, no settings folder at all (GF_CONFIG an empty folder): what comes up,
a screenshot, then the first card's answers (this PC's music, D:/Music read only) and the library read. python tools/preview/devfirst.py <out>"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
cfg = HERE / 'out' / 'first-cfg'
shutil.rmtree(cfg, ignore_errors=True)
cfg.mkdir(parents=True)
os.environ['SONGATE_PCCFG'] = str(cfg)
sys.path.insert(0, str(HERE))
import pcdrive  # noqa: E402

# (pcdrive writes a settings.txt: the folder emptied again before the app starts, so it is a true first start)
pc = pcdrive.PC.__new__(pcdrive.PC)
pcdrive.WORK.mkdir(parents=True, exist_ok=True)
pc.ctl_path, pc.items_path = pcdrive.WORK / 'ctl.txt', pcdrive.WORK / 'items.txt'
e = os.environ.copy()
e.update(GF_CONFIG=str(cfg), GF_READONLY='D:/Music', GF_CTL=str(pc.ctl_path), GF_ITEMS=str(pc.items_path), SDL_AUDIODRIVER='dummy')
pc.log = open(pcdrive.WORK / 'app-log.txt', 'ab')
pc.app = subprocess.Popen([str(pcdrive.EXE)], cwd=str(pcdrive.EXE.parent), env=e, stdout=pc.log, stderr=subprocess.STDOUT)
try:
    s = pc.wait(lambda s: s.w > 0 and len(s.items) > 2, 30)
    time.sleep(4)
    s = pc.screen()
    pc.shot(out / 'pc-first-1.jpg')
    print('first screen:', [t[0] for i, t in enumerate(s.texts) if i in s.top][:12])
    print('controls:', sorted(s.items)[:20])
    print('alive:', pc.app.poll() is None, '| folder now holds:', sorted(p.name for p in (cfg / 'Songate').glob('*')) if (cfg / 'Songate').exists() else 'nothing')
finally:
    pc.quit()
