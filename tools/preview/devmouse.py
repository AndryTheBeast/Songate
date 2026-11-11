"""Songate for PC, dev only: the mouse puts lists in order the touch way (handles, dragged). Drives the dev build through pcdrive.

    python tools/preview/devmouse.py [look|settings|page]

Frames go to tools/preview/out/mouse/. Nothing of it ships."""
import sys
import time
from pathlib import Path

import pcdrive

OUT = pcdrive.HERE / 'out' / 'mouse'
PLUS, DOWN, A = 1 << 10, 1 << 15, 1 << 0


def drag(pc, x0, y0, x1, y1, shot=None, secs=0.8):
    pc.ctl(f'down {x0:.0f} {y0:.0f}')
    time.sleep(0.15)
    n = 20
    for k in range(1, n + 1):
        pc.ctl(f'move {x0 + (x1 - x0) * k / n:.0f} {y0 + (y1 - y0) * k / n:.0f}')
        time.sleep(secs / n)
    if shot:
        time.sleep(0.2)
        pc.shot(OUT / shot)
    pc.ctl(f'up {x1:.0f} {y1:.0f}')
    time.sleep(0.6)


def names(s, pre):
    return sorted(k for k in s.items if k.startswith(pre))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'look'
    OUT.mkdir(parents=True, exist_ok=True)
    pc = pcdrive.PC()
    try:
        pc.wait(lambda s: s.w > 0, 40)
        pc.restore()
        time.sleep(4)
        pc.clear_notices()
        pc.size(1280, 720, screen=(1920, 1080), dpi=96)
        s = pc.fresh_screen(1)
        if what == 'look':
            pc.shot(OUT / 'look.jpg')
            print(sorted(s.items)[:200])
            return
        if what == 'settings':
            for step in sys.argv[2:]:
                if step.startswith('item:'):
                    print(step, pc.tap_item(step[5:]))
                elif step.startswith('text:'):
                    print(step, pc.tap_text(step[5:]))
                elif step.startswith('shot:'):
                    pc.shot(OUT / (step[5:] + '.jpg'))
                elif step.startswith('pad:'):
                    pc.ctl(f'pad {int(step[4:])}')
                    time.sleep(0.5)
                elif step.startswith('tap:'):
                    x, y = step[4:].split(',')
                    pc.tap(float(x), float(y))
                elif step.startswith('rclick:'):   # (the mouse there, then +, as a right click is)
                    x, y = step[7:].split(',')
                    pc.ctl(f'move {x} {y}')
                    time.sleep(0.3)
                    pc.ctl(f'pad {PLUS}')
                    time.sleep(0.6)
                elif step.startswith('drag:'):
                    v = step[5:].split(',')
                    drag(pc, *(float(n) for n in v[:4]), shot=(v[4] + '.jpg') if len(v) > 4 else None)
                elif step.startswith('sleep:'):
                    time.sleep(float(step[6:]))
                elif step.startswith('raw:'):
                    pc.ctl(step[4:].replace(',', ' '))
                    time.sleep(0.3)
                elif step.startswith('dump:'):
                    s = pc.fresh_screen(0.4)
                    print(step, [(k, s.items[k]) for k in names(s, step[5:])][:40])
                elif step.startswith('texts'):
                    s = pc.fresh_screen(0.4)
                    print([(t[0], int(t[1]), int(t[2])) for t in s.texts][:120])
    finally:
        pc.quit()


main()
