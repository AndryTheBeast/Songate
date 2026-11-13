"""Songate for Android, dev build: Settings > Appearance > Screen rotation (Auto / Portrait / Landscape). With the device's own
auto-rotate ON and the device turned (the emulator's sensor), Portrait stays upright and Landscape stays sideways; Auto follows;
the choice is kept over a restart.    python devrotate.py [folder for frames]    (SONGATE_SERIAL names the device)"""
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


def turn(deg):   # the device itself turned: the emulator's sensors (auto-rotate on, so Android follows it)
    adb('emu', 'rotate') if False else None
    acc = {0: '0:9.81:0', 90: '9.81:0:0'}[deg]
    adb('emu', 'sensor', 'set', 'acceleration', acc)
    time.sleep(3.0)


def upright():
    s = screen()
    return s.h > s.w


def pick(word):
    """Settings > Appearance > Screen rotation: the choice named, whichever way the screen is."""
    s = screen()
    if not any(t[0] == 'Screen rotation' for t in s.texts):
        if 'ph.nav#4' in s.items:
            tap_item('ph.nav#4')
        else:
            tap_text('Settings')
        time.sleep(1.2)
        tap_text('Appearance')
        time.sleep(1.2)
        for _ in range(30):
            s = screen()
            if any(t[0] == 'Screen rotation' for t in s.texts) and any(t[0] == word for t in s.texts):
                break
            sw, sh = [int(v) for v in adb('shell', 'wm', 'size').split()[-1].split('x')]
            if s.w > s.h:
                sw, sh = max(sw, sh), min(sw, sh)
            adb('shell', 'input', 'swipe', str(sw // 2), str(int(sh * 0.7)), str(sw // 2), str(int(sh * 0.4)), '350')
            time.sleep(0.7)
    s = screen()
    hit = next((t for t in s.texts if t[0] == word), None)   # (the choice itself: the row's own line names all three too)
    if hit:
        adb('shell', 'input', 'tap', str(int((hit[1] + hit[3] / 2) * s.scale)), str(int((hit[2] + hit[4] / 2) * s.scale)))
        time.sleep(3.0)
    return bool(hit)


try:
    adb('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '1')
    turn(0)
    adb('shell', 'am', 'force-stop', PKG)
    adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
    time.sleep(9)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(0.8)
    check('starts upright (Auto)', upright())
    turn(90)
    check('Auto, the device turned: sideways', not upright())
    turn(0)
    check('Auto, turned back: upright', upright())

    check('the row and its Portrait choice', pick('Portrait'))
    shot('rotation-row-portrait')
    turn(90)
    check('Portrait, the device turned with auto-rotate on: stays upright', upright())
    turn(0)

    check('its Landscape choice', pick('Landscape'))
    check('Landscape chosen, the device upright: sideways', not upright())
    shot('rotation-row-landscape')
    adb('shell', 'am', 'force-stop', PKG)
    adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
    time.sleep(9)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(0.8)
    check('kept over a restart: sideways from the start', not upright())
    check('saved', 'screenTurn=2' in run_as('cat files/Songate/settings.txt'))

    check('its Auto choice', pick('Auto'))
    check('Auto again, the device upright: upright', upright())
    turn(90)
    check('Auto again, turned: sideways', not upright())
finally:
    turn(0)
    adb('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0')
    adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
    adb('shell', 'am', 'force-stop', PKG)
print('\nALL GOOD' if not bad else f'\n{bad} FAILED')
sys.exit(1 if bad else 0)
