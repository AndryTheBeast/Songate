"""Songate for PC, dev only (session 23): the Canvas opened and closed a few times while a song plays, every frame over 18 ms with its
parts (devctl "hitches"). SONGATE_PCCFG must be absolute. python tools/preview/devcanvas.py [times]"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import pcdrive  # noqa: E402

times = int(sys.argv[1]) if len(sys.argv) > 1 else 4
pc = pcdrive.PC()
hp = pcdrive.WORK / 'h.txt'
try:
    pc.wait(lambda s: 'tb.battery' in s.items, 30)
    time.sleep(6)
    for _ in range(3):
        pc.clear_notices()
        time.sleep(1)
    pc.tap_text('Library')
    time.sleep(1)
    s = pc.screen()
    shuf = next((k for k in s.items if 'shuffle' in k.lower()), None)
    if shuf:
        pc.tap_item(shuf)
    time.sleep(3)
    pc.ctl('hitches ' + str(hp).replace('\\', '/'))
    for i in range(times):
        pc.tap_item('pb.canvas')
        time.sleep(2.0)
        pc.ctl('hitches ' + str(hp).replace('\\', '/'))
        time.sleep(0.2)
        print(f'open {i}:', hp.read_text().strip() if hp.exists() else '(none)')
        pc.ctl('pad 2')   # B closes it
        time.sleep(1.5)
        pc.ctl('hitches ' + str(hp).replace('\\', '/'))
        time.sleep(0.2)
        print(f'close {i}:', hp.read_text().strip() if hp.exists() else '(none)')
finally:
    pc.quit()
