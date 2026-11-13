"""Songate for Android, dev build: a player's own buttons (pause, next, previous) with Songate open in front. Found on a Hiby R4
(2026-11-13): they worked from anywhere but inside the app. Sends the media keys to the device while Songate is in front and
reads what Songate plays.    python devkeys.py     (SONGATE_SERIAL names the device; the emulator by default)"""
import re
import sys
import time

from devlib import adb, screen, wait, tap_item, clear_notices, step, run_as, PKG

bad = 0


def check(name, ok, detail=""):
    global bad
    bad += 0 if ok else 1
    step(name, bool(ok), detail)


def pos():
    m = re.search(r"pos ([-\d.]+)", run_as("cat files/Songate/items.txt 2>/dev/null")[:200])
    return float(m[1]) if m else -1


def key(name):
    adb('shell', 'input', 'keyevent', name)
    time.sleep(2.5)
    return screen()


adb('shell', 'am', 'force-stop', PKG)
adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
time.sleep(9)
for _ in range(3):
    clear_notices(screen())
    time.sleep(0.8)
was = screen().track
tap_item('ph.nav#1')   # Library, Local, Shuffle: a queue of many songs (one song alone has no next)
time.sleep(1.5)
if 'lib.source[0]' in screen().items:
    tap_item('lib.source[0]')
s = wait(lambda s: 'ph.lshuffle' in s.items, 20)
check('Library, Local: its Shuffle', s)
tap_item('ph.lshuffle')
s = wait(lambda s: s.playing and s.track != was, 15)
check('playing a shuffled queue, Songate in front', s)
t0 = screen().track
print('    playing:', t0)

s = key('KEYCODE_MEDIA_NEXT')
t1 = s.track
check('Next: another song, still playing', t1 != t0 and s.playing, f'still "{t1}"')
s = key('KEYCODE_MEDIA_PLAY_PAUSE')
check('Play/pause: paused', not s.playing)
s = key('KEYCODE_MEDIA_PLAY_PAUSE')
check('Play/pause again: playing', s.playing)
s = key('KEYCODE_MEDIA_PAUSE')
check('Pause: paused', not s.playing)
s = key('KEYCODE_MEDIA_PLAY')
check('Play: playing', s.playing)
s = key('KEYCODE_MEDIA_NEXT')
t2 = s.track
check('Next again: a third song', t2 != t1, f'still "{t2}"')
time.sleep(4)
p0 = pos()
s = key('KEYCODE_MEDIA_PREVIOUS')
p1 = pos()
check('Previous, some seconds in: the song from its start', s.track == t2 and 0 <= p1 < p0, f'{p0:.1f} s -> {p1:.1f} s, "{s.track}"')
s = key('KEYCODE_HEADSETHOOK')
check('A headset button: paused', not s.playing)
s = key('KEYCODE_HEADSETHOOK')
check('A headset button again: playing', s.playing)
adb('shell', 'input', 'keyevent', 'KEYCODE_MEDIA_PAUSE')
adb('shell', 'am', 'force-stop', PKG)
print('\nALL GOOD' if not bad else f'\n{bad} FAILED')
sys.exit(1 if bad else 0)
