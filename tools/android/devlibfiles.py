"""Songate for Android, dev only: the Sidebar card gone while the screen is held upright for good, and Files in Library (the user,
2026-11-16): Settings › Storage › Files › Files in Library shows a Files row on the upright Library page, between Folders and Liked.
   python tools/android/devlibfiles.py [serial]          (default: emulator-5554; the dev APK installed first)
The settings folder is set aside first and put back at the end. Ends with ALL GOOD or the steps that failed."""
import os
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import PKG, SERIAL, adb, results, run_as, screen, step, tap_at, tap_item, wait   # noqa: E402
from devphase2_helpers import kicker, scroll   # noqa: E402

stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devlibfiles-{stamp}"
print(f"devlibfiles on {SERIAL}, {stamp}", flush=True)


def has(s, t):
    return s is not None and any(x[0] == t for x in s.texts)


def top():
    for _ in range(6):
        scroll(False, 0.75, 0.3)


def kickers(s):
    lines = {}
    for t, x, y, w, h in s.texts:
        if len(t) == 1:
            lines.setdefault(round(y), []).append((x, t))
    return {"".join(c for _, c in sorted(v)).strip() for v in lines.values() if len(v) > 3}


def all_kickers():
    seen = set()
    top()
    for _ in range(8):
        seen |= kickers(screen())
        scroll()
    return seen


def page(name):
    tap_item("ph.nav#4")
    time.sleep(1.8)
    top()
    for _ in range(8):
        s = screen()
        hit = next((t for t in s.texts if t[0] == name), None)
        if hit and s.h * 0.1 < hit[2] < s.h - 170:
            tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
            time.sleep(1.4)
            return True
        scroll()
    return False


def library_rows():
    """The Library page's rows, top to bottom (their names)"""
    tap_item("ph.nav#1")
    time.sleep(2)
    s = screen()
    names = [t for t in s.texts if t[0] in ("Songs", "Albums", "Artists", "Genres", "Folders", "Files", "Liked")]
    return [t[0] for t in sorted(names, key=lambda t: t[2])]


adb("shell", "input", "keyevent", "224")
adb("shell", "svc", "power", "stayon", "true")
adb("shell", "settings", "put", "system", "user_rotation", "0")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}")
run_as('rm -f files/Songate/items.txt files/Songate/start-guard.txt files/Songate/mock-ui.txt files/Songate/mock-r4.txt; '
       'touch files/Songate/test-items; sed -i "/^foldOpen=/d; /^screenTurn=/d; /^libFiles=/d" files/Songate/settings.txt')
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    wait(lambda s: "ph.nav#4" in s.items, 30)
    time.sleep(3)
    page("Appearance")
    k = all_kickers()
    step("Rotation Auto: the Sidebar card there", "SIDEBAR" in k, ", ".join(sorted(k)))
    top()
    kicker("LAYOUT")
    tap_item("set.turn[1]")   # Portrait
    time.sleep(3)
    if os.environ.get("DEVSHOT"):   # (dev: what the screen shows after the change)
        import subprocess
        from devlib import ADB
        open(os.environ["DEVSHOT"], "wb").write(subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout)
    k = all_kickers()
    step("Rotation Portrait: no Sidebar card", "SIDEBAR" not in k and "LAYOUT" in k, ", ".join(sorted(k)))
    # Files in Library, on: between Folders and Liked
    page("Storage")
    kicker("FILES")
    s = wait(lambda s: has(s, "Files in Library"), 5)
    step("Storage › Files: Files in Library", s is not None)
    if s and "set.libfiles" in s.items:
        tap_item("set.libfiles")   # (as the sidebar's Files tab was: hidden at first, so this shows it)
        time.sleep(1)
    rows = library_rows()
    step("Library: Files between Folders and Liked", "Files" in rows and rows.index("Files") == rows.index("Folders") + 1 and
         rows.index("Liked") == rows.index("Files") + 1, " · ".join(rows))
    page("Storage")
    if "set.libfiles" not in screen().items:   # (the card kept open since: a tap on its title would fold it)
        kicker("FILES")
    tap_item("set.libfiles")
    time.sleep(1)
    rows = library_rows()
    step("... off: no Files row", "Files" not in rows and "Liked" in rows, " · ".join(rows))
    page("Appearance")
    kicker("LAYOUT")
    tap_item("set.turn[0]")   # Auto again
    time.sleep(1.5)
finally:
    adb("shell", "svc", "power", "stayon", "false")
    adb("shell", "am", "force-stop", PKG)
    adb("shell", "settings", "put", "system", "user_rotation", "0")
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && "
                 "rm -f files/Songate/test-items files/Songate/items.txt; ls files/Songate/settings.txt")
    print("settings folder put back:", "yes" if "settings.txt" in out else "NO", flush=True)

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
