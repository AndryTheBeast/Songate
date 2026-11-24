"""Songate for Android, dev build: the Apps page's app menu (a finger held on an app): Icon..., Move (the tiles dragged into order),
Hide and Settings' Hidden apps, App settings; and Home screen as Settings' first page. Songate is made the home screen through
its own switch and Android's dialog (devhome.py tests those); the switch is turned off again at the end, and the device's own
home app put back whatever happens.     python devapps.py [folder for frames]      (SONGATE_SERIAL names the device)"""
import re
import subprocess
import sys
import time
from pathlib import Path

from devlib import adb, screen, wait, tap_item, clear_notices, step, run_as, PKG, ADB, SERIAL

out = Path(sys.argv[1]) if len(sys.argv) > 1 else None
bad = 0
ROLE = 'android.app.role.HOME'
APPS_ON = ('grep -v ^appsTab= files/Songate/settings.txt > files/Songate/settings.new; echo appsTab=1 >> files/Songate/settings.new; '
           'mv files/Songate/settings.new files/Songate/settings.txt')
APPS_OFF = 'grep -v ^appsTab= files/Songate/settings.txt > files/Songate/settings.new; mv files/Songate/settings.new files/Songate/settings.txt'


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
    m = re.search(r'ResumedActivity[^\n]* ([\w.]+)/', adb('shell', 'dumpsys', 'activity', 'activities'))
    return m[1] if m else ''


def px(s, x, y):
    return str(int(x * s.scale)), str(int(y * s.scale))


def tap_word(word):
    s = screen()
    hit = next((t for t in s.texts if t[0] == word), None)
    if hit:
        adb('shell', 'input', 'tap', *px(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2))
        time.sleep(1.5)
    return bool(hit)


def cells(s):
    return sorted((k for k in s.items if re.fullmatch(r'ph\.app(#\d+)?', k)), key=lambda k: (s.items[k][1], s.items[k][0]))


def names(s):
    """The apps' names on the page, in reading order (the words under the tiles)."""
    found = []
    for k in cells(s):
        x, y, w, h = s.items[k]
        under = [t for t in s.texts if x - 4 <= t[1] + t[3] / 2 <= x + w + 4 and y + 60 <= t[2] <= y + h + 4]
        found.append(under[0][0] if under else '?')
    return found


def hold(s, key):
    x, y, w, h = s.items[key]
    X, Y = px(s, x + w / 2, y + 36)
    adb('shell', 'input', 'swipe', X, Y, X, Y, '900')
    time.sleep(1.5)


def saved():
    return run_as('cat files/Songate/apps.tsv 2>/dev/null')


def start():
    adb('shell', 'am', 'force-stop', PKG)
    adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
    time.sleep(10)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(0.8)


def set_rows(s):
    return sorted((k for k in s.items if k.startswith('set.cat')), key=lambda k: s.items[k][1])


def home_page():
    """Settings, its first page (Home screen): open, its switch in view."""
    tap_item('ph.nav#4')
    time.sleep(1.5)
    s = screen()
    if 'set.home' in s.items:
        return s
    rows = set_rows(s)
    if rows:
        x, y, w, h = s.items[rows[0]]
        adb('shell', 'input', 'tap', *px(s, x + w / 2, y + h / 2))
    return wait(lambda s: 'set.home' in s.items, 8)


def dialog_tap(words):
    adb('shell', 'uiautomator', 'dump', '/sdcard/songate-ui.xml')
    xml = adb('shell', 'cat', '/sdcard/songate-ui.xml')
    for m in re.finditer(r'<node[^>]*?text="([^"]*)"[^>]*?bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', xml):
        if m[1].strip().lower() in words:
            adb('shell', 'input', 'tap', str((int(m[2]) + int(m[4])) // 2), str((int(m[3]) + int(m[5])) // 2))
            time.sleep(1.2)
            return m[1]
    return None


def apps_page():
    tap_item('ph.nav#5')
    s = wait(lambda s: len(cells(s)) > 3, 8)
    time.sleep(2.0)
    return screen() if s else None


before = holders()
print("    the device's home app:", before)
try:
    adb('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0')
    adb('shell', 'settings', 'put', 'system', 'user_rotation', '0')
    adb('shell', 'am', 'force-stop', PKG)
    run_as('rm -f files/Songate/apps.tsv')
    run_as(APPS_ON)
    start()

    # Settings: Home screen is its first page
    tap_item('ph.nav#4')
    time.sleep(1.5)
    s = screen()
    rows = set_rows(s)
    first = None
    if rows:
        x, y, w, h = s.items[rows[0]]
        first = next((t[0] for t in s.texts if y <= t[2] <= y + h and t[1] > x + 40 and len(t[0]) > 3), None)
    check('Settings: Home screen is the first page', first == 'Home screen', str(first))
    shot('apps-0-settings-first')
    check('... and it opens on the home screen switch', home_page())
    if PKG not in holders():
        tap_item('set.home')
        time.sleep(2.5)
        dialog_tap({'songate'})
        dialog_tap({'set as default', 'set default', 'ok', 'allow'})
        time.sleep(9)   # (Android starts Songate afresh as it becomes the home app)
        for _ in range(3):
            clear_notices(screen())
            time.sleep(0.8)
    check('Songate is the home screen', PKG in holders(), holders())
    check('upright: a sixth button', 'ph.nav#5' in screen().items)

    s = apps_page()
    check('the Apps page', s)
    order0 = names(s)
    print('    apps:', order0)

    # a finger held: the menu
    hold(s, cells(s)[1])
    s = screen()
    words = [t[0] for t in s.texts]
    want = ['Open', 'Icon…', 'Move', 'Rename', 'Hide', 'App settings', 'Uninstall']
    check('a finger held on the second app: its menu, its name at the head', all(w in words for w in want) and order0[1] in words,
          'missing ' + str([w for w in want if w not in words]))
    shot('apps-1-menu')

    # Icon…: a symbol chosen, kept
    tap_word('Icon…')
    s = wait(lambda s: any(k.startswith('sp.appicon') for k in s.items), 6)
    check('Icon…: the symbols', s)
    time.sleep(0.8)
    shot('apps-2-symbols')
    s = screen()
    tiles = sorted((k for k in s.items if k.startswith('sp.appicon')), key=lambda k: (s.items[k][1], s.items[k][0]))
    x, y, w, h = s.items[tiles[3]]
    adb('shell', 'input', 'tap', *px(s, x + w / 2, y + h / 2))
    time.sleep(1.5)
    check('a symbol chosen: the menu closed, the choice written',
          not any(k.startswith('sp.appicon') for k in screen().items) and '\tapp:' in saved(), saved()[:160])

    # Hide, and Settings' Hidden apps
    s = screen()
    hidden_name = names(s)[2]
    hold(s, cells(s)[2])
    tap_word('Hide')
    time.sleep(1.2)
    s = screen()
    check(f'Hide: "{hidden_name}" is off the page', hidden_name not in names(s), str(names(s)[:5]))
    s = home_page()
    s = wait(lambda s: 'set.unhide' in s.items, 6)
    check('Settings › Home screen: Hidden apps', s)
    shot('apps-3-hidden-row')
    tap_item('set.unhide')
    time.sleep(1.5)
    s = apps_page()
    check('Show all: it is back', s and hidden_name in names(s), str(names(s)[:6]) if s else '')

    # Move: the tiles dragged into order
    order1 = names(s)
    hold(s, cells(s)[0])
    tap_word('Move')
    time.sleep(1.5)
    s = screen()
    check('Move: Done is offered', any(t[0] == 'Done' for t in s.texts))
    c = cells(s)
    x0, y0, w0, h0 = s.items[c[0]]
    x2, y2, w2, h2 = s.items[c[2]]
    # (a finger as a hand moves it: down, across in steps, held a moment over its new place, up)
    fx, fy = x0 + w0 / 2, y0 + 36
    tx, ty = x2 + w2 / 2 + 8, y2 + 36
    adb('shell', 'input', 'motionevent', 'DOWN', *px(s, fx, fy))
    time.sleep(0.25)
    adb('shell', 'input', 'motionevent', 'MOVE', *px(s, fx + 3, fy + 3))   # (taken: the tile is under the finger from here)
    time.sleep(0.6)
    for k in range(1, 13):
        adb('shell', 'input', 'motionevent', 'MOVE', *px(s, fx + (tx - fx) * k / 12, fy + (ty - fy) * k / 12))
        time.sleep(0.06)
    time.sleep(0.7)
    shot('apps-4-sorting')
    adb('shell', 'input', 'motionevent', 'UP', *px(s, tx, ty))
    time.sleep(1.5)
    tap_word('Done')
    time.sleep(1.5)
    order2 = names(screen())
    print('    before the drag:', order1[:4], ' after:', order2[:4])
    check('the first app dragged to the third place: the two between moved up', order2[:3] == [order1[1], order1[2], order1[0]])
    start()
    s = apps_page()
    check('the order kept over a restart', s and names(s)[:3] == order2[:3], str(names(s)[:4]) if s else '')

    # App settings: Android's page
    hold(s, cells(s)[1])
    tap_word('App settings')
    time.sleep(3.0)
    check("App settings: Android's page for the app", 'settings' in front(), front())
    adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
    time.sleep(2.5)
    check('the home button: Songate', front() == PKG, front())
finally:
    try:
        if PKG in holders():   # the switch off again: Android's own home app
            adb('shell', 'am', 'start', '-W', '-n', PKG + '/.SongateActivity')
            time.sleep(6)
            if home_page():
                tap_item('set.home')
                time.sleep(3)
    except Exception as e:   # noqa: BLE001
        print('    (the switch could not be turned off:', e, ')')
    if PKG in holders() and before and PKG not in before:
        adb('shell', 'cmd', 'role', 'add-role-holder', ROLE, before)
    adb('shell', 'am', 'force-stop', PKG)
    run_as('rm -f files/Songate/apps.tsv')
    run_as(APPS_OFF)
    print("    the device's home app at the end:", holders())
print('\nALL GOOD' if not bad else f'\n{bad} FAILED')
sys.exit(1 if bad else 0)
