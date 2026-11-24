"""The console (dev only, session 23): the battery's sheet and Settings > Battery info (Charging speed: Charger | Battery), each shown
and shot, read and put back as it was. python tools/preview/swbattery.py <out folder>"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from swctl import Switch  # noqa: E402

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
sw = Switch()


def items():
    t = sw.items() or ''
    found = {}
    for l in t.splitlines():
        m = re.match(r'\s+(\w+)\s+(\S.*?)\s+(-?\d+),\s*(-?\d+)\s+(\d+)x(\d+)', l)
        if m:
            found[m.group(2)] = tuple(int(v) for v in m.group(3, 4, 5, 6))
    return found, t


def tap(name, found):
    x, y, w, h = found[name]
    sw.run(f'tap {x + w / 2:.0f} {y + h / 2:.0f}', 'wait 40')


f, _ = items()
print('battery button:', f.get('tb.battery'))
tap('tb.battery', f)
time.sleep(1.0)
sw.shot(os.path.join(out, 'console-battery-sheet.png'))
f, t = items()
print('sheet words:', [l.split('|', 1)[1] for l in t.splitlines() if l.startswith('TOP ')][:40])
sw.run('pad 2', 'wait 30')   # B closes it
