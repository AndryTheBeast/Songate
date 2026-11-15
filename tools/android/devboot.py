"""Songate for Android, dev build, Songate as the home screen: a restart of the DEVICE. Songate starts with it, before Android has
the card (or any storage) ready, so its music folder is not there yet (found on a Hiby R4, 2026-11-14: an empty library after a
reboot, until Songate was started again). Plays a shuffled queue, restarts the device, and reads: the library is back by itself
with as many tracks as before, and the queue from before with its song.
THIS RESTARTS THE DEVICE.    python devboot.py      (SONGATE_SERIAL names the device; never the emulator by default: say it)"""
import os
import re
import subprocess
import sys
import time

from devlib import adb, screen, wait, tap_item, clear_notices, step, run_as, PKG, SERIAL

bad = 0


def check(name, ok, detail=""):
    global bad
    bad += 0 if ok else 1
    step(name, bool(ok), detail)


def tracks():
    """Library (upright): its count of tracks. (Tried three times: right after a boot a tap can land before the page is up.)"""
    for _ in range(3):
        tap_item('ph.nav#1')
        time.sleep(1.5)
        if 'lib.source[0]' in screen().items:
            tap_item('lib.source[0]')
            time.sleep(1.0)
        for t in screen().texts:
            m = re.match(r'(\d+) tracks?', t[0])
            if m:
                return int(m[1])
    return -1


if 'SONGATE_SERIAL' not in os.environ:
    print('Name the device (SONGATE_SERIAL): this test restarts it.')
    sys.exit(1)
home = adb('shell', 'cmd', 'package', 'resolve-activity', '--brief', '-c', 'android.intent.category.HOME', '-a', 'android.intent.action.MAIN')
if PKG not in home:
    print('Songate is not the home app on this device: set it in Settings > Home screen first.')
    sys.exit(1)
adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')
run_as('touch files/Songate/test-items')
adb('shell', 'am', 'force-stop', PKG)
adb('shell', 'am', 'start', '-W', '-a', 'android.intent.action.MAIN', '-c', 'android.intent.category.HOME')
time.sleep(9)
for _ in range(3):
    clear_notices(screen())
    time.sleep(0.8)
n0 = tracks()
check('before: a library', n0 > 0, str(n0))
s = wait(lambda s: 'ph.lshuffle' in s.items, 10)
tap_item('ph.lshuffle')
s = wait(lambda s: s.playing and s.track not in ('', '-'), 15)
check('before: a shuffled queue playing', s)
song = s.track if s else ''
print('    playing:', song, '|', n0, 'tracks')
tap_item('ph.nav')
time.sleep(1.0)
tap_item('ph.play')   # paused: what a start brings back is the song, not its sound
time.sleep(6)         # (the queue saved)
kept = len([l for l in run_as('cat files/Songate/session.txt').splitlines() if l.startswith('track=')])
check('before: the queue saved', kept > 1, str(kept))

print('    restarting the device …')
adb('reboot')
time.sleep(20)
t0 = time.time()
while time.time() - t0 < 240:
    if adb('shell', 'getprop', 'sys.boot_completed').strip() == '1':
        break
    time.sleep(3)
check('the device is back', adb('shell', 'getprop', 'sys.boot_completed').strip() == '1')
adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')
# Songate comes by itself (the home screen); what it shows from its first seconds on
first, back = None, None
t0 = time.time()
while time.time() - t0 < 150:
    adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')
    s = screen()
    if s.w > 0:
        if first is None:
            first = (round(time.time() - t0), s.track)
            shots = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'next-session', 'devboot')
            os.makedirs(shots, exist_ok=True)
            with open(os.path.join(shots, 'first-seen.png'), 'wb') as f:   # (the card likely still mounting: the cover from the cache)
                f.write(subprocess.run([r'C:\Android\sdk\platform-tools\adb.exe', '-s', SERIAL, 'exec-out', 'screencap', '-p'],
                                       capture_output=True).stdout)
        if s.track == song:
            back = round(time.time() - t0)
            break
    time.sleep(2)
print('    Songate first seen', first, '| the song back after', back, 's')
check('Songate came up by itself as the home screen', first is not None)
check('the queue from before is back, its song current, by itself', back is not None, screen().track)
for _ in range(3):
    clear_notices(screen())
    time.sleep(0.8)
n1 = -1
t1 = time.time()
while n1 < 0 and time.time() - t1 < 40:   # (right after a boot the screen may not answer yet)
    n1 = tracks()
    if n1 < 0:
        time.sleep(3)
# Songate started once and was never ended: vold ends any app with a file open on a card it is (re)mounting (2026-11-15)
starts = len(re.findall(r'Start proc \d+:' + re.escape(PKG) + '/', adb('logcat', '-d', '-b', 'all')))
died = adb('logcat', '-d', '-b', 'events').count('am_proc_died') and len(re.findall(r'am_proc_died: \[\d+,\d+,' + re.escape(PKG), adb('logcat', '-d', '-b', 'events')))
told = re.findall(r'vold\s*: Sending \w+ to pid \d+ \(SDLActivity', adb('logcat', '-d', '-b', 'all'))
check('Songate started once after the boot, never ended', starts == 1 and not died, f'{starts} starts, {died} ended')
check('vold never had to end Songate (nothing open on the card while it mounted)', not told, '; '.join(told))
check('the library is back by itself, as many tracks as before', n1 == n0, f'{n1} of {n0}')
saved = len([l for l in run_as('cat files/Songate/session.txt').splitlines() if l.startswith('track=')])
check('the saved queue was not written over while the folder was away', saved == kept, f'{saved} of {kept}')
run_as('rm -f files/Songate/test-items files/Songate/items.txt')
print('ALL GOOD' if bad == 0 else f'{bad} FAILED')
