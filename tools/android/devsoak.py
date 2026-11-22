"""Session 18: the Android dev build soaked for leaks on the emulator (emulator-5554), its own library and settings.

    python andsoak.py <cycles>

Each cycle: the five tabs upright with a scroll on each, an album opened and left, the next song, the Server side of Library and
Search when signed in, a turn sideways and back, and a visit to Android's home screen and back. After each cycle Android's own
figures for the app (dumpsys meminfo): total PSS, native heap, graphics, and the views and activities it counts.
Nothing is pressed that changes the library or the settings (the Local | Server switch is put back each cycle)."""
import re
import sys
import time

sys.path.insert(0, r'C:\Users\andre\Desktop\Songate\tools\android')
from devlib import adb, screen, tap_item, PKG   # noqa: E402

ACT = PKG + '/.SongateActivity'
cycles = int(sys.argv[1])


def mem():
    t = adb('shell', 'dumpsys', 'meminfo', PKG)
    out = {}
    for key, pat in (('pss', r'TOTAL PSS:\s+(\d+)'), ('native', r'Native Heap:\s+(\d+)'), ('graphics', r'Graphics:\s+(\d+)'), ('java', r'Java Heap:\s+(\d+)'),
                     ('views', r'Views:\s+(\d+)'), ('activities', r'Activities:\s+(\d+)'), ('binders', r'Local Binders:\s+(\d+)')):
        m = re.search(pat, t)
        out[key] = int(m[1]) if m else -1
    return out


def tap(name, pause=0.8):
    ok = tap_item(name)
    time.sleep(pause)
    return ok


def swipe_up(n=1):
    for _ in range(n):
        adb('shell', 'input', 'swipe', '540', '1500', '540', '700', '250')
        time.sleep(0.5)


def first(prefix):
    for name in screen().items:
        if name.startswith(prefix):
            return name
    return None


adb('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0')
adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
adb('shell', 'am', 'force-stop', PKG)
adb('shell', 'am', 'start', '-W', '-n', ACT)
time.sleep(10)
for _ in range(3):
    tap('notice.later', 1.0)
log = open(__file__.replace('.py', '.csv'), 'w')
log.write('cycle,seconds,pss_mb,native_mb,graphics_mb,java_mb,views,activities\n')
t0 = time.time()
signed = 'lib.source[1]' in screen().items or True
try:
    for n in range(cycles + 1):
        if n:
            for tab in ('ph.nav', 'ph.nav#1', 'ph.nav#2', 'ph.nav#3', 'ph.nav#4'):
                tap(tab, 0.7)
                swipe_up(1 + n % 3)
            tap('ph.nav#1', 0.8)                      # Library: an album's page and back
            a = first('ph.lsalbum') or first('lib.tile')
            if a:
                tap(a, 1.2)
                swipe_up(1)
                adb('shell', 'input', 'keyevent', '4')
                time.sleep(0.8)
            if 'lib.source[1]' in screen().items:     # signed in to a server: its side of the library, a scroll, and back to Local
                tap('lib.source[1]', 1.5)
                swipe_up(2 + n % 4)
                tap('lib.source[0]', 1.0)
            tap('ph.mnext', 1.0)                      # the next song: its cover (the notification's too), its lyrics
            tap('ph.nav', 0.8)
            adb('shell', 'settings', 'put', 'system', 'user_rotation', '1')
            time.sleep(1.8)
            adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
            time.sleep(1.2)
            adb('shell', 'am', 'start', '-n', ACT)
            time.sleep(1.8)
            adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
            time.sleep(1.8)
        m = mem()
        row = (n, time.time() - t0, m['pss'] / 1024, m['native'] / 1024, m['graphics'] / 1024, m['java'] / 1024, m['views'], m['activities'])
        log.write('%d,%.0f,%.1f,%.1f,%.1f,%.1f,%d,%d\n' % row)
        log.flush()
        if n % 5 == 0 or n == cycles:
            print('cycle %3d: %5.0f s, total %.0f MB, native %.0f MB, graphics %.0f MB, java %.1f MB, %d views, %d activities' % row)
            sys.stdout.flush()
finally:
    log.close()
    adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
