"""Songate for Android, dev build: Settings > Home screen > Use Songate as the home screen. The switch asks Android (its own
dialog, answered here as a user would); the home button then returns to Songate; the device's apps are listed and one opens;
the switch off gives the home screen back to the device's own. The device's own home app is put back at the end whatever happens.
    python devhome.py [folder for frames]      (SONGATE_SERIAL names the device; the emulator by default)"""
import re
import subprocess
import sys
import time
from pathlib import Path

from devlib import adb, screen, wait, tap_item, clear_notices, step, PKG, ADB, SERIAL

out = Path(sys.argv[1]) if len(sys.argv) > 1 else None
bad = 0
ROLE = 'android.app.role.HOME'


def check(name, ok, detail=""):
    global bad
    bad += 0 if ok else 1
    step(name, bool(ok), detail)


def shot(name):
    if out:
        out.mkdir(parents=True, exist_ok=True)
        (out / f'{name}.png').write_bytes(subprocess.run([ADB, '-s', SERIAL, 'exec-out', 'screencap', '-p'], capture_output=True).stdout)


def holders():
    return adb('shell', 'cmd', 'role', 'get-role-holders', ROLE).strip()


def front():
    m = re.search(r'mResumedActivity[^\n]* ([\w.]+)/', adb('shell', 'dumpsys', 'activity', 'activities'))
    if not m:
        m = re.search(r'topResumedActivity[^\n]* ([\w.]+)/', adb('shell', 'dumpsys', 'activity', 'activities'))
    return m[1] if m else ''


def dialog_nodes():
    adb('shell', 'uiautomator', 'dump', '/sdcard/songate-ui.xml')
    xml = adb('shell', 'cat', '/sdcard/songate-ui.xml')
    nodes = []
    for m in re.finditer(r'<node[^>]*?text="([^"]*)"[^>]*?bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml):
        nodes.append((m[1], (int(m[2]) + int(m[4])) // 2, (int(m[3]) + int(m[5])) // 2))
    return nodes


def tap_node(words):
    for text, x, y in dialog_nodes():
        if text.strip().lower() in words:
            adb('shell', 'input', 'tap', str(x), str(y))
            time.sleep(1.2)
            return text
    return None


def open_page():
    s = screen()
    if 'ph.nav#4' in s.items:
        tap_item('ph.nav#4')
    time.sleep(1.5)
    last = None
    for _ in range(16):
        s = screen()
        hit = next((t for t in s.texts if t[0] == 'Home screen'), None)
        # (pressed only where it has come to rest, clear of the small player and the bars)
        if hit and 110 < hit[2] < s.h - 190 and last is not None and abs(last - hit[2]) < 1:
            adb('shell', 'input', 'tap', str(int((hit[1] + hit[3] / 2) * s.scale)), str(int((hit[2] + hit[4] / 2) * s.scale)))
            return bool(wait(lambda s: 'set.home' in s.items, 8))
        if hit and 110 < hit[2] < s.h - 190:
            last = hit[2]
            time.sleep(0.8)
            continue
        last = None
        adb('shell', 'input', 'swipe', '540', '1500', '540', '1000', '600')
        time.sleep(1.6)
    return False


before = holders()
print('    the device\'s home app:', before)
try:
    adb('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0')
    adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
    adb('shell', 'am', 'force-stop', PKG)
    adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
    time.sleep(9)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(0.8)
    check('Settings: the Home screen page', open_page())
    s = screen()
    check('its switch; Show Apps there; five buttons in the bottom bar', 'set.home' in s.items and 'set.apps' in s.items and 'ph.nav#5' not in s.items)
    shot('home-1-off')
    tap_item('set.home')
    time.sleep(2.5)
    shot('home-2-android-asks')
    nodes = dialog_nodes()
    print('    Android asks:', [n[0] for n in nodes if n[0]][:8])
    check("Android's own dialog names Songate", any(n[0] == 'Songate' for n in nodes))
    tap_node({'songate'})
    said = tap_node({'set as default', 'set default', 'ok', 'allow'})
    time.sleep(2.0)
    check('answered: ' + str(said), said)
    check('Android: Songate holds the home screen', PKG in holders(), holders())
    # (Android starts Songate afresh as it becomes the home app: it is back on its first page)
    time.sleep(6)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(0.8)
    if 'set.home' not in screen().items:
        check('Songate again after Android restarted it: the Home screen page', open_page())
    s = screen()
    check('Show Apps: its switch', 'set.apps' in s.items)
    if 'ph.nav#5' in s.items:   # (left on by an earlier run: off first, so the press below is the one that adds the button)
        tap_item('set.apps')
        check('Show Apps off: five buttons', wait(lambda s: 'ph.nav#5' not in s.items, 6))
    tap_item('set.apps')
    s = wait(lambda s: 'ph.nav#5' in s.items, 6)
    check('Show Apps on: a sixth button in the bottom bar', s)
    shot('home-3-settings-on')
    tap_item('ph.nav#5')
    s = wait(lambda s: any(k.startswith('ph.app') for k in s.items), 8)
    check('the Apps page: the device\'s apps', s)
    time.sleep(2.5)
    s = screen()
    cells = sorted((k for k in s.items if k.startswith('ph.app')), key=lambda k: (s.items[k][1], s.items[k][0]))
    print('    apps on the page:', len(cells), [t[0] for t in s.texts if t[2] > 100][:16])
    shot('home-4-apps-page')
    x, y, w, h = s.items[cells[0]]
    adb('shell', 'input', 'tap', str(int((x + w / 2) * s.scale)), str(int((y + h / 2) * s.scale)))
    time.sleep(3.5)
    check('an app opens from the page (the first one)', front() not in ('', PKG), front())
    adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
    time.sleep(2.5)
    check('the home button returns to Songate', front() == PKG, front())
    s = screen()
    check('Songate is as it was left (the Apps page)', any(k.startswith('ph.app') for k in s.items))
    adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
    time.sleep(1.5)
    check('Back leaves the Apps page (Settings under it)', 'set.home' in screen().items)
    tap_item('set.home')
    time.sleep(3.0)
    check('the switch off: the device\'s own home app again', PKG not in holders(), holders())
    # (Android may show its own home screen now, Songate behind it: opened again before it is read)
    adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
    time.sleep(9)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(0.8)
    s = screen()
    check('and the sixth button is gone', 'ph.nav#4' in s.items and 'ph.nav#5' not in s.items)
    adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
    time.sleep(2.5)
    check('the home button leaves Songate again', front() != PKG, front())
finally:
    if PKG in holders() and before and PKG not in before:
        adb('shell', 'cmd', 'role', 'add-role-holder', ROLE, before)
    adb('shell', 'pm', 'disable', PKG + '/.HomeAlias') if False else None
    adb('shell', 'am', 'force-stop', PKG)
    print('    the device\'s home app at the end:', holders())
print('\nALL GOOD' if not bad else f'\n{bad} FAILED')
sys.exit(1 if bad else 0)
