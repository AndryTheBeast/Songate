"""Songate for PC, dev only (session 23): albums opened one by one (devctl "album <n>"), every frame over 18 ms with its parts
("hitches"), to find what made one album page take 396 ms of work in the self test. python tools/preview/devalbums.py <from> <to> [out]"""
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
import pcdrive  # noqa: E402

a0, a1 = int(sys.argv[1]), int(sys.argv[2])
out = Path(sys.argv[3]) if len(sys.argv) > 3 else pcdrive.WORK / 'albums-hitches.txt'
pc = pcdrive.PC()
try:
    pc.wait(lambda s: 'tb.battery' in s.items, 30)
    time.sleep(6)
    pc.clear_notices()
    time.sleep(1)
    pc.clear_notices()
    hp = pcdrive.WORK / 'h.txt'
    pc.ctl('hitches ' + str(hp).replace('\\', '/'))   # (the start's, set aside)
    lines = []
    for i in range(a0, a1 + 1):
        pc.ctl(f'album {i}')
        time.sleep(0.5)
        pc.ctl('hitches ' + str(hp).replace('\\', '/'))
        time.sleep(0.1)
        got = hp.read_text().strip() if hp.exists() else ''
        title = pc.screen().texts
        name = next((t[0] for t in title if t[3] > 300 and t[2] < 200), '?')
        for l in got.splitlines():
            lines.append(f'album {i} ({name}): {l}')
            print(lines[-1])
    out.write_text('\n'.join(lines) + '\n', encoding='utf-8')
finally:
    pc.quit()
log = (pcdrive.CFG / 'Songate' / 'log.txt').read_text(encoding='utf-8', errors='replace').splitlines()
print('\n'.join(l for l in log if 'devctl: album' in l)[-3000:])
