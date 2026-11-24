"""Songate for PC, dev only (session 23): which blocks a step leaves behind. The same step (an album opened and left, or the sidebar's
pages visited) done N times after a warm-up; the C heap's blocks counted by size before and after (devctl "heapsizes"); the sizes whose
count rose by about N or more are printed. SONGATE_PCCFG absolute. python tools/preview/devleak.py <album|pages> [N]"""
import os
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(__file__))
import pcdrive  # noqa: E402

what = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
pc = pcdrive.PC()
hp = pcdrive.WORK / 'heapsizes.txt'


def sizes():
    if hp.exists():
        hp.unlink()
    pc.ctl('heapsizes ' + str(hp).replace('\\', '/'))
    end = time.time() + 5
    while time.time() < end and not hp.exists():
        time.sleep(0.05)
    time.sleep(0.2)
    return Counter({int(a): int(b) for a, b in (l.split() for l in hp.read_text().splitlines() if l.strip())})


def step():
    if what == 'album':
        pc.tap_text('Albums', pause=0.6, secs=2)
        time.sleep(0.3)
        s = pc.screen()
        tile = next((k for k in s.items if k.startswith('lib.tile')), None)
        if tile:
            pc.tap_item(tile, 1.0)
            pc.ctl('pad 2')
            time.sleep(0.5)
    else:
        for page in ('Now playing', 'Playlist', 'Albums', 'Artists', 'Genres', 'Folders', 'Library'):
            pc.tap_text(page, pause=0.5, secs=2)


try:
    pc.wait(lambda s: 'tb.battery' in s.items, 40)
    time.sleep(8)
    for _ in range(3):
        pc.clear_notices()
        time.sleep(1)
    for _ in range(8):   # warm-up: caches filled
        step()
    a = sizes()
    for _ in range(n):
        step()
    b = sizes()
    grew = sorted(((b[k] - a[k], k) for k in set(a) | set(b) if b[k] - a[k] >= n * 0.8), reverse=True)
    print(f'{what}: blocks {sum(a.values())} -> {sum(b.values())} over {n} steps ({(sum(b.values()) - sum(a.values())) / n:+.1f} a step)')
    for d, k in grew[:25]:
        print(f'   size {k:6d}: +{d} ({d / n:.1f} a step)')
finally:
    pc.quit()
