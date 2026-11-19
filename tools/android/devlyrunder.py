"""Songate for Android, dev only: Settings › Lyrics › Under the cover (the user, 2026-11-17): upright Now playing, a song with synced
lyrics, after 4 s without a touch the line being sung in the buttons' place; a touch brings them back.
   python tools/android/devlyrunder.py [serial]          (default: emulator-5554; the dev APK installed first)
Checks: the setting (Off, This line, And the next) and settings.txt keeping it; then, if the song playing has synced lyrics (said
by the lyrics showing at all), the row's buttons gone after the wait and back after a touch; Off: they stay. The music is played
for the check and paused again. The settings folder is set aside first and put back at the end. Ends with ALL GOOD or the failures."""
import os
import re
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import PKG, SERIAL, adb, results, run_as, screen, step, tap_at, tap_item, wait   # noqa: E402
from devphase2_helpers import kicker, scroll   # noqa: E402


def kickers_on(s):
    lines = {}
    for t, x, y, w, h in s.texts:
        if len(t) == 1:
            lines.setdefault(round(y), []).append((x, t))
    return {"".join(c for _, c in sorted(v)).strip() for v in lines.values() if len(v) > 3}

stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devlyrunder-{stamp}"
print(f"devlyrunder on {SERIAL}, {stamp}", flush=True)


def lyrics_page():
    tap_item("ph.nav#4")
    time.sleep(1.8)
    for _ in range(6):
        scroll(False, 0.75, 0.3)
    for _ in range(8):
        s = screen()
        hit = next((t for t in s.texts if t[0] == "Lyrics"), None)
        if hit and s.h * 0.1 < hit[2] < s.h - 170:
            tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
            time.sleep(1.5)
            return True
        scroll()
    return False


def general_open():
    """General's card open (the page folds: one card at a time, the last one left open kept)"""
    if not any(t[0] == "Under the cover" for t in screen().texts):
        kicker("GENERAL")


def under(i):
    lyrics_page()
    general_open()
    tap_item(f"set.lyrunder[{i}]")
    time.sleep(1)


adb("shell", "input", "keyevent", "224")
adb("shell", "svc", "power", "stayon", "true")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}")
run_as('rm -f files/Songate/items.txt files/Songate/start-guard.txt files/Songate/mock-ui.txt; touch files/Songate/test-items; '
       'sed -i "/^foldOpen=/d; /^lyrUnder=/d" files/Songate/settings.txt')
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    wait(lambda s: "ph.nav#4" in s.items, 30)
    time.sleep(3)
    lyrics_page()
    k = kickers_on(screen())
    step("Settings › Lyrics: General, then Timing (Online below them)", {"GENERAL", "TIMING"} <= k, ", ".join(sorted(k)))
    general_open()
    s = wait(lambda s: "set.lyrunder[2]" in s.items, 5)
    step("Settings › Lyrics: Under the cover (Off, This line, And the next), This line at first",
         s is not None and any(t[0] == "Under the cover" for t in s.texts) and any(t[0] == "This line" for t in s.texts))
    under(2)
    adb("shell", "input", "keyevent", "3")   # Home: Songate writes what changed
    time.sleep(2.5)
    m = re.search(r"^lyrUnder=(.*)$", run_as("cat files/Songate/settings.txt"), re.M)
    step("settings.txt keeps it (And the next: 2)", m is not None and m[1].strip() == "2", m[1] if m else "none")
    adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
    time.sleep(2)
    under(1)
    tap_item("ph.nav")   # Now playing
    time.sleep(1.5)
    adb("shell", "input", "keyevent", "126")   # play
    time.sleep(6)
    s = screen()
    shown = "ph.canvas" not in s.items
    if shown:
        step("A song with synced lyrics: after 4 s the buttons give way (Canvas gone)", True)
        tap_at(s, s.w / 2, s.h * 0.3)
        s = wait(lambda s: "ph.canvas" in s.items, 3)
        step("... a touch: they come back", s is not None)
        under(0)
        tap_item("ph.nav")
        time.sleep(6)
        step("Off: the buttons stay", "ph.canvas" in screen().items)
    else:
        print("  (this song has no synced lyrics: the fade not checked)", flush=True)
    adb("shell", "input", "keyevent", "127")   # pause
finally:
    adb("shell", "svc", "power", "stayon", "false")
    adb("shell", "am", "force-stop", PKG)
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && "
                 "rm -f files/Songate/test-items files/Songate/items.txt; ls files/Songate/settings.txt")
    print("settings folder put back:", "yes" if "settings.txt" in out else "NO", flush=True)

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
