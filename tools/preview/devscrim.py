"""Songate for PC, dev only: the sidebar opened over a narrow window's page, the mouse over the page: no outline round the page.
python tools/preview/devscrim.py . Nothing of it ships."""
import time

import pcdrive

pc = pcdrive.PC()
try:
    pc.wait(lambda s: s.w > 0, 40)
    pc.restore()
    time.sleep(8)
    pc.clear_notices()
    pc.size(2000, 1240, screen=(2560, 1600), dpi=192)
    time.sleep(1)
    pc.clear_notices()
    pc.tap(27, 24)          # the title bar's sidebar button
    time.sleep(1)
    pc.ctl('move 700 300')  # over the page
    time.sleep(0.8)
    pc.shot(pcdrive.HERE / 'out' / 'mouse' / 'scrim.jpg')
finally:
    pc.quit()
