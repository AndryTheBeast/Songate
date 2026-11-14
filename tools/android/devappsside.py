"""Songate for Android, dev build: the Apps page sideways (session 19). With Songate as the home screen and Show Apps on, the
sidebar has an Apps tab; its page is the upright Apps page's grid with larger tiles; a turn keeps the page open both ways; a finger
held on an app opens its menu; Settings > Appearance > Sidebar lists the tab, and its switch takes it out of the sidebar; with the
home screen given back the tab is gone. The device's own home app and its rotation are put back at the end whatever happens.
    python devappsside.py [folder for frames]      (SONGATE_SERIAL names the device; the emulator by default)"""
import re
import subprocess
import sys
import time
from pathlib import Path

from devlib import adb, screen, wait, tap_item, tap_text, clear_notices, step, PKG, ADB, SERIAL

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


def dialog_nodes():
    adb('shell', 'uiautomator', 'dump', '/sdcard/songate-ui.xml')
    xml = adb('shell', 'cat', '/sdcard/songate-ui.xml')
    return [(m[1], (int(m[2]) + int(m[4])) // 2, (int(m[3]) + int(m[5])) // 2)
            for m in re.finditer(r'<node[^>]*?text="([^"]*)"[^>]*?bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml)]


def tap_node(words):
    for text, x, y in dialog_nodes():
        if text.strip().lower() in words:
            adb('shell', 'input', 'tap', str(x), str(y))
            time.sleep(1.2)
            return text
    return None


def open_page():
    """Upright: Settings, its Home screen page."""
    s = screen()
    if 'ph.nav#4' in s.items:
        tap_item('ph.nav#4')
    time.sleep(1.5)
    last = None
    for _ in range(16):
        s = screen()
        hit = next((t for t in s.texts if t[0] == 'Home screen'), None)
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


def turn(sideways):
    adb('shell', 'settings', 'put', 'system', 'user_rotation', '1' if sideways else '0')
    s = wait(lambda s: (s.w > s.h) == sideways and s.w > 0, 10)
    time.sleep(1.5)
    return s


def apps(s):
    return sorted((k for k in s.items if k.startswith('ph.app') and not k.startswith('ph.appsort')), key=lambda k: (s.items[k][1], s.items[k][0]))


def start():
    adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
    time.sleep(9)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(0.8)


before = holders()
print('    the device\'s home app:', before)
try:
    adb('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0')
    adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
    adb('shell', 'am', 'force-stop', PKG)
    start()
    # sideways with the home screen not Songate's: no Apps tab
    s = turn(True)
    check('sideways, Songate not the home screen: no Apps tab in the sidebar', s and 'nav.now' in s.items and 'nav.apps' not in s.items)
    turn(False)
    check('Settings: the Home screen page', open_page())
    tap_item('set.home')
    time.sleep(2.5)
    tap_node({'songate'})
    said = tap_node({'set as default', 'set default', 'ok', 'allow'})
    time.sleep(2.0)
    check("Android's dialog answered; Songate holds the home screen", said and PKG in holders(), holders())
    time.sleep(6)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(0.8)
    if 'set.home' not in screen().items:
        open_page()
    for _ in range(3):   # (a notice may come over the page just now)
        clear_notices(screen())
        time.sleep(0.8)
    s = screen()
    words = ' '.join(t[0] for t in s.texts)   # (it wraps: read as one run of words)
    check('Show Apps says both places', 'a tab in the sidebar sideways' in words, words)
    shot('side-0-show-apps-words')
    if 'ph.nav#5' not in s.items:
        tap_item('set.apps')
    check('Show Apps on: a sixth button upright', wait(lambda s: 'ph.nav#5' in s.items, 6))
    tap_item('ph.nav#5')
    s = wait(lambda s: len(apps(s)) > 3, 8)
    check('upright: the Apps page', s)
    time.sleep(2)
    s = screen()
    n_up = len(apps(s))
    row_up = sum(1 for k in apps(s) if s.items[k][1] == s.items[apps(s)[0]][1])
    print('    upright:', n_up, 'apps in view,', row_up, 'across; tile cell', s.items[apps(s)[0]][2:])
    shot('side-1-upright-page')

    # the turn: the same page, the sidebar's tab lit
    s = turn(True)
    s = wait(lambda s: 'nav.apps' in s.items and len(apps(s)) > 3, 8)
    check('turned sideways: the Apps page still open, its tab in the sidebar', s)
    time.sleep(2.5)
    s = screen()
    cells = apps(s)
    across = sum(1 for k in cells if s.items[k][1] == s.items[cells[0]][1]) if cells else 0
    print('    sideways:', s.w, s.h, 'scale', s.scale, '|', len(cells), 'apps in view,', across, 'across; cell', s.items[cells[0]][2:] if cells else '')
    check('more across than upright, larger cells', cells and across > row_up and s.items[cells[0]][3] > 120)
    check('its head: Apps, and the count', any(t[0] == 'Apps' and t[4] > 30 for t in s.texts) and any(re.fullmatch(r'\d+ apps?', t[0]) for t in s.texts),
          ' | '.join(t[0] for t in s.texts if t[2] < 140)[:200])
    check('no word cut short', not s.cut, ' | '.join(s.texts[i][0] for i in s.cut))
    shot('side-2-page')

    # another tab and back
    tap_item('nav.albums')
    check('another tab leaves it', wait(lambda s: not apps(s), 6))
    tap_item('nav.apps')
    s = wait(lambda s: len(apps(s)) > 3, 6)
    check('the Apps tab opens it', s)
    time.sleep(1.5)

    # a finger held on an app: its menu
    s = screen()
    x, y, w, h = s.items[apps(s)[1]]
    px, py = str(int((x + w / 2) * s.scale)), str(int((y + h / 2) * s.scale))
    adb('shell', 'input', 'swipe', px, py, px, py, '900')
    time.sleep(1.5)
    s = screen()
    menu = [s.texts[i][0] for i in s.top]
    check('a finger held on an app: its menu (Open, Icon…, Move, Rename, Hide)', all(any(w_ == m for m in menu) for w_ in ('Open', 'Move', 'Rename', 'Hide')), ' | '.join(menu)[:200])
    shot('side-3-menu')
    adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
    time.sleep(1.2)
    check('Back closes the menu, the page stays', len(apps(screen())) > 3)

    # an app opens, home returns to the page
    s = screen()
    x, y, w, h = s.items[apps(s)[0]]
    adb('shell', 'input', 'tap', str(int((x + w / 2) * s.scale)), str(int((y + h / 2) * s.scale)))
    time.sleep(3.5)
    act = adb('shell', 'dumpsys', 'activity', 'activities')
    m = re.search(r'ResumedActivity[^\n]* ([\w.]+)/', act)
    check('a tap opens the app', m and m[1] != PKG, m[1] if m else '')
    adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
    time.sleep(3)
    s = wait(lambda s: len(apps(s)) > 3, 8)
    check('the home button: Songate again, on the Apps page', s)

    # upright again: the page still open there
    turn(False)
    s = wait(lambda s: 'ph.nav#5' in s.items and len(apps(s)) > 3, 8)
    check('turned upright: the Apps page still open', s)
    adb('shell', 'input', 'keyevent', 'KEYCODE_BACK')
    time.sleep(1.2)
    check('Back leaves it upright', not apps(screen()))

    # Settings > Appearance > Sidebar: the tab's row and switch
    turn(True)
    tap_item('nav.settings')
    time.sleep(1.2)
    tap_text('Appearance')
    time.sleep(1.2)
    found = None
    for _ in range(14):
        s = screen()
        if 'set.tab#256' in s.items and 80 < s.items['set.tab#256'][1] < s.h - 150:
            found = s
            break
        adb('shell', 'input', 'swipe', '1500', '800', '1500', '520', '500')
        time.sleep(1.3)
    check('Settings › Appearance › Sidebar lists Apps, with a switch', found)
    shot('side-4-sidebar-row')
    if found:
        tap_item('set.tab#256')
        check('its switch off: the tab leaves the sidebar', wait(lambda s: 'nav.apps' not in s.items and 'nav.now' in s.items, 6))
        shot('side-5-tab-off')
        tap_item('set.tab#256')
        check('on again: the tab is back', wait(lambda s: 'nav.apps' in s.items, 6))

    # the home screen given back: no tab
    turn(False)
    check('upright, Settings: the Home screen page', open_page())
    tap_item('set.home')
    time.sleep(3.0)
    check('the switch off: the device\'s own home app again', PKG not in holders(), holders())
    start()
    s = turn(True)
    check('sideways, the home screen given back: no Apps tab', s and 'nav.now' in s.items and 'nav.apps' not in s.items)
finally:
    adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
    adb('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '1')
    if PKG in holders() and before and PKG not in before:
        adb('shell', 'cmd', 'role', 'add-role-holder', ROLE, before)
print('ALL GOOD' if bad == 0 else f'{bad} FAILED')
