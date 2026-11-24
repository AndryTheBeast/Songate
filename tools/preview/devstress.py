"""Songate for PC, dev only (session 23): the whole library queued, the queue scrolled as fast as the wheel goes, then buttons mashed
(D-pad, B, L, R, ZL, ZR: never A, X or minus, so nothing is chosen or removed), every frame over 18 ms kept with its parts. Settings of
its own (SONGATE_PCCFG, absolute), D:/Music read only. python tools/preview/devstress.py <out folder> [mash seconds]"""
import os
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
import pcdrive  # noqa: E402

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
mash_s = float(sys.argv[2]) if len(sys.argv) > 2 else 60
B, L, R, ZL, ZR = 2, 64, 128, 256, 512
LEFT, UP, RIGHT, DOWN = 4096, 8192, 16384, 32768
hp = pcdrive.WORK / 'h.txt'
pc = pcdrive.PC()


def hitches(name):
    pc.ctl('hitches ' + str(hp).replace('\\', '/'))
    time.sleep(0.3)
    text = hp.read_text() if hp.exists() else ''
    (out / f'{name}-hitches.txt').write_text(text)
    rows = [l.split() for l in text.splitlines() if l.strip()]
    worst = max((float(r[1]) for r in rows), default=0)
    print(f'{name}: {len(rows)} frames over 18 ms, worst {worst:.1f} ms; {pc.stats()["text"][:160]}', flush=True)


try:
    pc.wait(lambda s: 'tb.battery' in s.items, 40)
    time.sleep(8)
    for _ in range(3):
        pc.clear_notices()
        time.sleep(1.5)
    pc.tap_text('Library')
    time.sleep(1.0)
    s = pc.screen()
    play = next((k for k in s.items if k.endswith('.play') and not k.startswith('pb.')), None)
    print('play all with', play, flush=True)
    pc.tap_item(play)
    time.sleep(2.5)
    pc.stats()
    pc.ctl('hitches ' + str(hp).replace('\\', '/'))
    # the queue: 3,074 rows, scrolled down and up as fast as a wheel turns
    pc.tap_text('Playlist')
    time.sleep(1.5)
    pc.shot(out / 'queue-top.jpg')
    for k in range(60):
        pc.ctl('wheel 800 400 -12')
        time.sleep(0.03)
    pc.shot(out / 'queue-down.jpg')
    for k in range(60):
        pc.ctl('wheel 800 400 12')
        time.sleep(0.03)
    hitches('queue-scroll')
    # the albums grid the same way
    pc.tap_text('Albums')
    time.sleep(1.5)
    for k in range(80):
        pc.ctl('wheel 800 400 -10')
        time.sleep(0.03)
    pc.shot(out / 'albums-down.jpg')
    hitches('albums-scroll')
    # mashing: a button every frame or two, at random
    keys = [LEFT, UP, RIGHT, DOWN, B, L, R, ZL, ZR]
    random.seed(23)
    end = time.time() + mash_s
    n = 0
    while time.time() < end:
        pc.ctl(f'pad {random.choice(keys)}')
        n += 1
        time.sleep(random.choice((0.008, 0.016, 0.03)))
    time.sleep(1.5)
    pc.shot(out / 'after-mash.jpg')
    hitches(f'mash-{n}-presses')
    print('alive after:', pc.app.poll() is None, flush=True)
finally:
    pc.quit()
