"""Songate for Android, dev build: Settings > Visualizer > Seek bar > Seek bar animation (session 19): Quick / Smooth / Wave, how the
seek bar turns back into the visualizer after a scrub, upright. The row is there with its words, each choice is kept in the
settings, and with each one a scrub on Now playing comes back to the visualizer and the song goes on. What was chosen before is put
back at the end.    python devseekanim.py [folder for frames]      (SONGATE_SERIAL names the device; the emulator by default)"""
import re
import subprocess
import sys
import time
from pathlib import Path

from devlib import adb, screen, wait, tap_item, tap_text, clear_notices, step, run_as, PKG, ADB, SERIAL

out = Path(sys.argv[1]) if len(sys.argv) > 1 else None
bad = 0


def check(name, ok, detail=""):
    global bad
    bad += 0 if ok else 1
    step(name, bool(ok), detail)


def shot(name):
    if out:
        out.mkdir(parents=True, exist_ok=True)
        (out / f'{name}.png').write_bytes(subprocess.run([ADB, '-s', SERIAL, 'exec-out', 'screencap', '-p'], capture_output=True).stdout)


def saved():
    m = re.search(r'^seekAnim=(\d)', run_as('cat files/Songate/settings.txt'), re.M)
    return int(m[1]) if m else None


def find_row():
    """Settings (upright) > Visualizer, scrolled until the row's choices are clear of the bars."""
    tap_item('ph.nav#4')
    time.sleep(1.2)
    for _ in range(10):
        if tap_text('Visualizer'):
            break
        adb('shell', 'input', 'swipe', '540', '1500', '540', '1000', '500')
        time.sleep(1.2)
    time.sleep(1.2)
    for _ in range(12):
        s = screen()
        if 'set.seekanim[2]' in s.items and 110 < s.items['set.seekanim[2]'][1] < s.h - 190:
            time.sleep(1.2)
            return screen()
        adb('shell', 'input', 'swipe', '540', '1500', '540', '1050', '500')
        time.sleep(1.3)
    return None


def scrub_and_back():
    """Now playing: a scrub, then nothing for two seconds; the song still plays and the page is as it was."""
    tap_item('ph.nav')
    s = wait(lambda s: 'ph.seek' in s.items, 8)
    if not s:
        return False
    if not s.playing:
        tap_item('ph.play')
        s = wait(lambda s: s.playing and 'ph.seek' in s.items, 8)
        if not s:
            return False
    x, y, w, h = s.items['ph.seek']
    cy = str(int((y + h / 2) * s.scale))
    adb('shell', 'input', 'swipe', str(int((x + w * 0.3) * s.scale)), cy, str(int((x + w * 0.5) * s.scale)), cy, '600')
    time.sleep(2.2)
    s = screen()
    return s.playing and 'ph.seek' in s.items


adb('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0')
adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')
run_as('touch files/Songate/test-items')
adb('shell', 'am', 'force-stop', PKG)
adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
time.sleep(9)
for _ in range(3):
    clear_notices(screen())
    time.sleep(0.8)
was = saved()
try:
    s = find_row()
    check('Settings › Visualizer › Seek bar: Seek bar animation, three choices', s)
    if s:
        words = ' '.join(t[0] for t in s.texts)
        check('its words: the name, the description, Quick / Smooth / Wave',
              all(w in words for w in ('Seek bar animation', 'How the bars come back after scrubbing', 'Quick', 'Smooth', 'Wave')), words[-300:])
        check('no word cut short', not s.cut, ' | '.join(s.texts[i][0] for i in s.cut))
        shot('seekanim-row')
        for k, name in ((0, 'Quick'), (2, 'Wave'), (1, 'Smooth')):
            s = find_row() if f'set.seekanim[{k}]' not in screen().items else screen()
            tap_item(f'set.seekanim[{k}]')
            ok = any(saved() == k or time.sleep(0.5) for _ in range(10))
            check(f'{name} chosen: kept in the settings', ok, str(saved()))
            check(f'{name}: a scrub on Now playing, back to the visualizer, the song goes on', scrub_and_back())
            adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
            time.sleep(0.8)
finally:
    if was is not None and saved() != was:
        if find_row():
            tap_item(f'set.seekanim[{was}]')
            time.sleep(1.5)
print('ALL GOOD' if bad == 0 else f'{bad} FAILED')
