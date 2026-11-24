"""Songate for PC, dev only (session 23): the numbers that change while shown, drawn with fixed-width digits. Opens the Battery
sheet and drags the seek bar, a screenshot of each, and checks that the words around a number keep their place as it changes.
Settings of its own: SONGATE_PCCFG (default tools/preview/out/pc-config2, a copy of pc-config). python tools/preview/devdigits.py <outdir>"""
import os
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
os.environ.setdefault('SONGATE_PCCFG', str(HERE / 'out' / 'pc-config2'))
cfg2 = Path(os.environ['SONGATE_PCCFG'])
if not (cfg2 / 'Songate').exists():
    shutil.copytree(HERE / 'out' / 'pc-config' / 'Songate', cfg2 / 'Songate',
                    ignore=shutil.ignore_patterns('selftest-*', 'log*.txt'))
sys.path.insert(0, str(HERE))
import pcdrive  # noqa: E402

out = Path(sys.argv[1] if len(sys.argv) > 1 else '.')
out.mkdir(parents=True, exist_ok=True)
pc = pcdrive.PC()
ok = True
try:
    pc.wait(lambda s: 'tb.battery' in s.items, 30)
    time.sleep(6)
    for _ in range(3):   # (a new user's notices come some seconds in, one after the other)
        pc.clear_notices()
        time.sleep(1.2)
    # the seek bar's tip while dragged: its box the same width at two different times
    pc.tap_text('Library')   # (the sidebar's) then its Shuffle: a song playing, so the seek bar has a length
    time.sleep(1.0)
    s = pc.screen()
    shuf = next((k for k in s.items if 'shuffle' in k.lower()), None)
    print('shuffle item', shuf)
    if shuf:
        pc.tap_item(shuf)
    time.sleep(2.0)
    s = pc.screen()
    print('playing', s.playing, s.track)
    seek = 'pb.seek' if 'pb.seek' in s.items else None
    sx, sy, sw, sh = (397, 738, 486, 30)   # (the player bar's seek at 1280 wide, when it is not a named item)
    if seek:
        sx, sy, sw, sh = s.items[seek]
    if s.playing:
        x, y, w, h = sx, sy, sw, sh
        pc.ctl(f'down {x + w * 0.10:.0f} {y + h / 2:.0f}')
        time.sleep(0.2)
        pc.ctl(f'move {x + w * 0.11:.0f} {y + h / 2:.0f}')
        time.sleep(0.3)
        pc.shot(out / 'pc-seek-tip-a.jpg')
        a = [t for t in pc.screen().texts if ':' in t[0] and t[2] < y]
        pc.ctl(f'move {x + w * 0.55:.0f} {y + h / 2:.0f}')
        time.sleep(0.3)
        pc.shot(out / 'pc-seek-tip-b.jpg')
        b = [t for t in pc.screen().texts if ':' in t[0] and t[2] < y]
        pc.ctl(f'up {x + w * 0.10:.0f} {y + h / 2:.0f}')   # (let go where it began: the song stays near its start)
        print('tip a', a[:2], 'tip b', b[:2])
    # the sleep timer: 15 minutes, then its sheet again: "Pauses in 14:5x" counting down, two shots a second apart
    pc.tap_item('pb.sleep')
    time.sleep(1.0)
    pc.tap_text('15 minutes')
    time.sleep(1.0)
    pc.tap_item('pb.sleep')
    time.sleep(1.0)
    pc.shot(out / 'pc-sleep-sheet-a.jpg')
    ta = next((t for t in pc.screen().texts if t[0].startswith('Pauses in')), None)
    time.sleep(1.1)
    pc.shot(out / 'pc-sleep-sheet-b.jpg')
    tb = next((t for t in pc.screen().texts if t[0].startswith('Pauses in')), None)
    print('sleep title a', ta, 'b', tb)
    pc.ctl('pad 2')
    time.sleep(0.6)
    pc.tap_item('pb.sleep')   # (the timer off again: Off)
    time.sleep(1.0)
    pc.tap_text('Off')
    time.sleep(0.8)
    # the Battery sheet
    pc.tap_item('tb.battery')
    time.sleep(1.2)
    pc.shot(out / 'pc-battery-sheet.jpg')
    s = pc.screen()
    print('battery sheet texts:', [t[0] for i, t in enumerate(s.texts) if i in s.top][:30])
finally:
    pc.quit()
