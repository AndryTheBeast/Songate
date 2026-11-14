"""Songate for Android, dev build: a second copy of the activity in a running process (Android 12, Songate as the home app: the home
entry is started beside the ordinary one, in the same process; found on a Hiby R4, 2026-11-14, as a crash at "Set as default").
Needs Songate already holding the home screen on the device. Starts it as an ordinary app, presses Home (the second copy), presses
Home again, starts the ordinary entry over the home one, and reads each time: no crash, one copy, Songate in front.
    python devdup.py      (SONGATE_SERIAL names the device; the emulator by default)"""
import re
import time

from devlib import adb, step, PKG

bad = 0


def check(name, ok, detail=""):
    global bad
    bad += 0 if ok else 1
    step(name, bool(ok), detail)


def pid():
    return adb('shell', 'pidof', PKG).strip()


def crashes():
    return len([l for l in adb('logcat', '-b', 'crash', '-d').splitlines() if 'Fatal signal' in l and 'SDL' in l])


def copies():
    out = adb('shell', 'dumpsys', 'activity', 'activities')
    return sorted(set(re.findall(r'Hist #\d+: ActivityRecord\{\w+ u0 ' + re.escape(PKG) + r'/(\S+) t\d+', out)))


def front():
    m = re.search(r'ResumedActivity[^\n]* ([\w.]+)/(\S+)', adb('shell', 'dumpsys', 'activity', 'activities'))
    return (m[1], m[2]) if m else ('', '')


def asked_afresh(since):
    return 'a second copy of the activity' in adb('logcat', '-d', '-s', 'Songate:W', '-T', since)


home = adb('shell', 'cmd', 'package', 'resolve-activity', '--brief', '-c', 'android.intent.category.HOME', '-a', 'android.intent.action.MAIN')
if PKG not in home:
    print('Songate is not the home app on this device: set it in Settings > Home screen first.')
    raise SystemExit(1)
adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')   # (a sleeping device pauses Songate as it starts: nothing to test)
time.sleep(1.5)
if 'mWakefulness=Awake' not in adb('shell', 'dumpsys', 'power'):
    print('The device is asleep and did not wake: wake it and run this again.')
    raise SystemExit(1)
before = crashes()
adb('shell', 'am', 'force-stop', PKG)
adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
time.sleep(7)
p1 = pid()
check('Songate open as an ordinary app', p1 and copies() == ['.SongateActivity'], f'{p1} {copies()}')

t0 = adb('shell', 'date', '+%m-%d\\ %H:%M:%S.000').strip()
adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
time.sleep(8)
p2 = pid()
check('the home button (a second copy asked for): no crash', crashes() == before, f'{crashes() - before} new')
check('… the running one was closed in order and Songate started afresh', asked_afresh(t0) and p2 and p2 != p1, f'{p1} -> {p2}')
check('… one copy, the home one, in front', copies() == ['.HomeAlias'] and front()[0] == PKG, f'{copies()} {front()}')

adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
time.sleep(4)
check('the home button again: the same Songate, nothing restarted', pid() == p2 and copies() == ['.HomeAlias'] and front()[0] == PKG, f'{pid()} {copies()}')

adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')   # (as a notification's press or another launcher would)
time.sleep(8)
p3 = pid()
check('the ordinary entry over the home one: no crash, one copy, in front', crashes() == before and len(copies()) == 1 and front()[0] == PKG,
      f'{crashes() - before} new; {copies()} {front()}; pid {p2} -> {p3}')
print('    (the process', 'stayed' if p3 == p2 else 'was started afresh', 'for it)')

adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
time.sleep(8)
check('the home button after that: no crash, one copy, in front', crashes() == before and len(copies()) == 1 and front()[0] == PKG, f'{copies()} {front()} pid {pid()}')
print('ALL GOOD' if bad == 0 else f'{bad} FAILED')
