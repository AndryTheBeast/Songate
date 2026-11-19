"""Songate for Android, dev only: Settings › Playback › Volume › Stays for (the user, 2026-11-17): how long Songate's volume pop-up
stays after the last press. On the emulator's simulated R4 (its output line gives the pop-up an item to see).
   python tools/android/devvolstay.py [serial]          (default: emulator-5554; the dev APK installed first)
Checks: 5 s: still there after 3.5 s, gone after 7; 1 s: gone after 2.5; settings.txt keeps the choice. The settings folder is set
aside first and put back at the end. Ends with ALL GOOD or the steps that failed."""
import os
import re
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import PKG, SERIAL, adb, results, run_as, screen, step, tap_at, tap_item, wait   # noqa: E402
from devphase2_helpers import kicker, scroll   # noqa: E402

stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devvolstay-{stamp}"
print(f"devvolstay on {SERIAL}, {stamp}", flush=True)


def top():
    for _ in range(6):
        scroll(False, 0.75, 0.3)


def volume_card():
    tap_item("ph.nav#4")
    time.sleep(1.8)
    top()
    for _ in range(6):
        s = screen()
        hit = next((t for t in s.texts if t[0] == "Playback"), None)
        if hit and s.h * 0.1 < hit[2] < s.h - 170:
            tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
            break
        scroll()
    time.sleep(1.5)
    if not any(t[0] == "Volume keys show" for t in screen().texts):   # (folded: opened; open: a tap on its title would fold it)
        kicker("VOLUME")
    for _ in range(6):   # (items are listed off screen too: the row's words in sight)
        s = screen()
        t = next((t for t in s.texts if t[0] == "Stays for"), None)
        if t and s.h * 0.15 < t[2] < s.h * 0.6:
            break
        scroll(True, 0.7, 0.5)


def popped_for(secs):
    """A volume key, then whether the pop-up is still there after secs"""
    tap_item("ph.nav")
    time.sleep(1.5)
    adb("shell", "input", "keyevent", "24")
    time.sleep(secs)
    return "vol.out" in screen().items


adb("shell", "input", "keyevent", "224")
adb("shell", "svc", "power", "stayon", "true")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}")
run_as('rm -f files/Songate/items.txt files/Songate/start-guard.txt files/Songate/mock-ui.txt; touch files/Songate/mock-r4.txt files/Songate/test-items; '
       'sed -i "/^foldOpen=/d; /^volPop=/d; /^volStay=/d; /^volWhere=/d" files/Songate/settings.txt; echo volWhere=0 >> files/Songate/settings.txt')
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    wait(lambda s: "ph.nav#4" in s.items, 30)
    time.sleep(3)
    volume_card()
    s = screen()
    if os.environ.get("DEVSHOT"):   # (dev: where the page is)
        import subprocess
        from devlib import ADB
        open(os.environ["DEVSHOT"], "wb").write(subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout)
    step("Playback › Volume: Stays for (1 s, 1.5 s, 3 s, 5 s)", "set.volstay[3]" in s.items and any(t[0] == "Stays for" for t in s.texts))
    tap_item("set.volstay[3]")
    time.sleep(1)
    step("5 s: still there after 3.5 s", popped_for(3.5))
    time.sleep(3.5)
    step("... gone after 7 s", "vol.out" not in screen().items)
    volume_card()
    tap_item("set.volstay[0]")
    time.sleep(1)
    step("1 s: gone after 2.5 s", not popped_for(2.5))
    adb("shell", "input", "keyevent", "3")   # Home: Songate writes what changed
    time.sleep(2.5)
    m = re.search(r"^volStay=(.*)$", run_as("cat files/Songate/settings.txt"), re.M)
    step("settings.txt keeps it (volStay=1.0)", m is not None and m[1].strip() == "1.0", m[1] if m else "none")
finally:
    adb("shell", "svc", "power", "stayon", "false")
    adb("shell", "am", "force-stop", PKG)
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && "
                 "rm -f files/Songate/test-items files/Songate/items.txt; ls files/Songate/settings.txt")
    print("settings folder put back:", "yes" if "settings.txt" in out else "NO", flush=True)

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
