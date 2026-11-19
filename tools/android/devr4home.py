"""Songate for Android, dev only: the HiBy R4 as its only launcher (the user, 2026-11-16), on the emulator's simulated R4 (mock-r4.txt).
   python tools/android/devr4home.py [serial]          (default: emulator-5554; the dev APK installed first)
Checks, starting from settings that had the Apps tab switched off: no Home screen page in Settings; the Apps button in the bottom
bar upright and the Apps tab in the sidebar sideways; Appearance › Sidebar has no switch for Apps; an app's menu says Hide comes
back with Show all in that menu, and Show all brings a hidden app back. The settings folder is copied aside first and put back at
the end (the test's own folder, mock-r4.txt with it, deleted). Ends with ALL GOOD or the steps that failed.
Not for the real R4 (it is one already: no mock needed there, and its settings are the user's)."""
import os
import re
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import *   # noqa: E402,F403

stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devr4home-{stamp}"
print(f"devr4home on {SERIAL}, {stamp}")


def rotate(r):
    adb("shell", "settings", "put", "system", "accelerometer_rotation", "0")
    adb("shell", "settings", "put", "system", "user_rotation", str(r))
    return wait(lambda s: s.w > 0 and (s.w > s.h) == (r == 1), 10)


def hold(name):
    s = screen()
    x, y, w, h = s.items[name]
    X, Y = int((x + w / 2) * s.scale), int((y + h / 2) * s.scale)
    adb("shell", "input", "swipe", str(X), str(Y), str(X), str(Y), "900")


def sheet_texts(s):
    return [s.texts[i][0] for i in s.top] if s else []


def tiles_of(s):
    """The Apps page's tiles fully in sight (not under the mini player and the bar), in reading order"""
    return sorted((k for k in s.items if k.startswith("ph.app") and k[6:7] in ("", "#")
                   and s.items[k][1] >= 0 and s.items[k][1] + s.items[k][3] < s.h * 0.78),
                  key=lambda k: s.items[k][1] * 10000 + s.items[k][0])


def tile_name(s, k):
    x, y, w, h = s.items[k]
    return next((t[0] for t in s.texts if x <= t[1] + t[3] / 2 <= x + w and y + h * 0.55 <= t[2] + t[4] / 2 <= y + h + 4), None)


def per_row(s):
    t = tiles_of(s)
    return sum(1 for k in t if s.items[k][1] == s.items[t[0]][1]) if t else 0


def tap_exact(t0):
    s = screen()
    hit = next((t for t in reversed(s.texts) if t[0] == t0), None)
    if hit:
        tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
        time.sleep(0.7)
    return hit is not None


def open_gear():
    tap_item("ph.appset")
    return wait(lambda s: "Apps page" in sheet_texts(s), 5)


def close_sheet():
    adb("shell", "input", "keyevent", "4")
    time.sleep(1.2)


adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}; rm -f files/Songate/items.txt files/Songate/mock-ui.txt; "
       # the simulated R4, and the Apps tab off as it might have been before (appsTab=0, its sidebar bit 256 hidden)
       "touch files/Songate/mock-r4.txt; sed -i s/^appsTab=1/appsTab=0/ files/Songate/settings.txt; "
       "sed -i s/^hiddenTabs=.*/hiddenTabs=320/ files/Songate/settings.txt; touch files/Songate/test-items")
rotate(0)
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    s = wait(lambda s: "ph.nav#4" in s.items, 20)
    step("started upright (the simulated R4)", s)
    step("the bottom bar's Apps button, though Show Apps was off", s and "ph.nav#5" in s.items)
    tap_item("ph.nav#4")
    time.sleep(1.5)
    for _ in range(3):   # the list of categories from its top
        s = screen()
        X = int(12 * s.scale)
        adb("shell", "input", "swipe", str(X), str(int(s.h * 0.3 * s.scale)), str(X), str(int(s.h * 0.85 * s.scale)), "300")
        time.sleep(0.6)
    time.sleep(1)
    seen = set()
    for _ in range(6):
        s = screen()
        seen |= {t[0] for t in s.texts}
        X = int(12 * s.scale)
        adb("shell", "input", "swipe", str(X), str(int(s.h * 0.8 * s.scale)), str(X), str(int(s.h * 0.4 * s.scale)), "400")
        time.sleep(1)
    step("Settings: no Home screen page", "Home screen" not in seen and "Appearance" in seen, ", ".join(sorted(seen))[:200])
    step("... the R4's own pages there", "Sound & DAC" in seen and "Status LED" in seen)

    # the Apps page and an app's menu
    tap_item("ph.nav#5")
    s = wait(lambda s: "ph.app" in s.items or "ph.app#1" in s.items, 8)
    step("the Apps page", s)
    first = tiles_of(s)[0] if s and tiles_of(s) else None
    if first:
        name0 = tile_name(s, first)
        hold(first)
        s = wait(lambda s: any(t == "Hide" for t in sheet_texts(s)), 5)
        step("an app's menu: Hide says the page's settings bring it back",
             s and "Off the Apps page; the page's settings bring it back" in sheet_texts(s))
        step("... no Show all in an app's menu", s and "Show all" not in sheet_texts(s))
        tap_exact("Hide")
        time.sleep(1.5)
        s = screen()
        step("Hide: the app off the Apps page", name0 and not s.has_text(name0), name0)
        s = open_gear()
        step("the page's gear: its settings, the hidden app listed", s and name0 in sheet_texts(s)
             and "1 app is hidden from the Apps page" in sheet_texts(s))
        tap_exact("Show all")
        time.sleep(1)
        close_sheet()
        step("Show all: the hidden app back", name0 and screen().has_text(name0), name0)

        # the page's settings, one by one (the test's copy of the settings: put back at the end)
        names_before = [tile_name(screen(), k) for k in tiles_of(screen())]
        open_gear()
        tap_exact("A to Z")
        close_sheet()
        s = screen()
        names = [tile_name(s, k) for k in tiles_of(s)]
        step("Order › A to Z", names and names == sorted(names, key=str.lower), ", ".join(map(str, names[:6])))
        hold(tiles_of(s)[0])
        s = wait(lambda s: any(t == "Hide" for t in sheet_texts(s)), 5)
        step("... no Move while the page is A to Z", s and "Move" not in sheet_texts(s))
        close_sheet()
        # Most used: an app opened from the page twice comes first (Android's Settings, the last by name here)
        target = next((k for k in tiles_of(screen()) if tile_name(screen(), k) == "Settings"), None)
        for _ in range(2):
            if target:
                tap_item(target)
                time.sleep(2.5)
                adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
                wait(lambda s: "ph.app" in s.items, 8)
                time.sleep(1)
        open_gear()
        tap_exact("Most used")
        close_sheet()
        s = screen()
        step("Order › Most used: the app opened twice first", target and tile_name(s, tiles_of(s)[0]) == "Settings",
             tile_name(s, tiles_of(s)[0]) if tiles_of(s) else "")
        open_gear()
        tap_exact("Your own")
        tap_exact("Small")
        close_sheet()
        s = screen()
        step("Tile size › Small: five across", per_row(s) == 5, per_row(s))
        open_gear()
        tap_exact("Large")
        close_sheet()
        s = screen()
        step("Tile size › Large: three across", per_row(s) == 3, per_row(s))
        open_gear()
        tap_exact("Medium")
        tap_exact("Names under the tiles")
        close_sheet()
        s = screen()
        step("Names under the tiles off: no names", not any(n and s.has_text(n) for n in names_before[:4]))
        open_gear()
        tap_exact("Names under the tiles")
        tap_exact("Only the apps you installed")
        close_sheet()
        s = screen()
        left = [tile_name(s, k) for k in tiles_of(s)]
        step("Only the apps you installed: Android's own off, its Settings kept", "Settings" in left and len(left) < len(names_before),
             ", ".join(map(str, left)))
        open_gear()
        tap_exact("Only the apps you installed")
        close_sheet()
        s = screen()
        step("Find an app: its field on the page", "ph.appfind" in s.items)
        tap_item("ph.appfind")
        wait(lambda s: "skb.k#104" in s.items, 5)
        tap_item("skb.k#18")   # o
        time.sleep(0.6)
        tap_item("skb.away")   # (the card closed: the page's tiles there to read, the word kept)
        time.sleep(1)
        s = screen()
        left = [tile_name(s, k) for k in tiles_of(s)]
        step("... o typed: only the apps with an o in their name", left and all(n and "o" in n.lower() for n in left), ", ".join(map(str, left)))
        tap_item("ph.appfind")
        wait(lambda s: "skb.k#104" in s.items, 5)
        tap_item("skb.k#101")
        tap_item("skb.away")
        time.sleep(0.8)
        open_gear()
        tap_exact("Find an app")
        close_sheet()
        step("Find an app off: no field", "ph.appfind" not in screen().items)
        open_gear()
        tap_exact("Find an app")
        close_sheet()
        adb("shell", "input", "keyevent", "3")   # Home is Songate here: settings.txt written
        time.sleep(2.5)
        m = re.search(r"^appsPage=(.*)$", run_as("cat files/Songate/settings.txt"), re.M)
        step("the page's settings kept in settings.txt", m and m[1].strip() == "0 1 0 1 1 0", m[1] if m else "none")
        uses = run_as("cat files/Songate/apps.tsv")
        step("... and how often an app was opened, in apps.tsv", re.search(r"com\.android\.settings/\S+\t[^\n]*\t2\s*$", uses, re.M), uses[:200])
        adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
        wait(lambda s: s.w > 0, 8)
    else:
        step("an app's tile to hold", False, "none")

    # sideways: the sidebar's Apps tab, no switch for it in Appearance › Sidebar
    adb("shell", "input", "keyevent", "4")
    s = rotate(1)
    step("turned sideways", s)
    time.sleep(1.5)
    s = screen()
    step("the sidebar's Apps tab", s.has_text("Apps"))
    tap_text("Settings")
    time.sleep(1.5)
    tap_text("Appearance")
    time.sleep(1.5)
    found = False
    for _ in range(25):
        s = screen()
        if any(t[0] == "Apps" for t in s.texts) and "set.tab#64" in s.items:   # the Files row's switch: the tab rows are here
            found = True
            break
        X = int((s.w - 8) * s.scale)
        adb("shell", "input", "swipe", str(X), str(int(s.h * 0.8 * s.scale)), str(X), str(int(s.h * 0.4 * s.scale)), "400")
        time.sleep(0.9)
    step("Appearance › Sidebar: the tab rows", found)
    step("... no switch for Apps (set.tab#256)", found and "set.tab#256" not in s.items)
finally:
    rotate(0)
    adb("shell", "am", "force-stop", PKG)
    run_as(f"rm -rf files/Songate && mv {ASIDE} files/Songate && rm -f files/Songate/test-items files/Songate/items.txt")
    print("settings put back:", "yes" if "settings.txt" in run_as("ls files/Songate") else "NO")

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
