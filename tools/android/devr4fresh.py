"""Songate for Android, dev build: a NEW user on the HiBy R4 (SongateOS; the user, 2026-11-15): the opening off (no bells either)
and the screen upright always (Settings › Appearance › Screen rotation: Portrait). On the emulator with the simulated R4
(mock-r4.txt): the device's own settings are kept aside first (every small file; art, models and the server cache stay) and put
back exactly as they were at the end, whatever happens; then a start without settings.txt.
    python devr4fresh.py      (SONGATE_SERIAL: the device; the emulator by default; never on a real device's own folder)"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import adb, screen, wait, run_as, PKG, SERIAL  # noqa: E402

if not SERIAL.startswith("emulator-"):
    sys.exit("emulator only: it moves the settings aside")

fails = []


def check(what, ok, detail=""):
    print(("OK   " if ok else "FAIL ") + what + (f"  ({detail})" if detail else ""))
    if not ok:
        fails.append(what)


KEEP = "files/SongateKeep"
adb("shell", "am", "force-stop", PKG)
had_mock = "mock-r4.txt" in run_as("ls files/Songate")
run_as(f"rm -rf {KEEP}; mkdir {KEEP}")
run_as(f"for f in files/Songate/*; do [ -f \"$f\" ] && cp -p \"$f\" {KEEP}/; done")
kept = run_as(f"ls {KEEP}").split()
check("settings kept aside", "settings.txt" in kept, f"{len(kept)} files")
try:
    run_as("rm -f files/Songate/settings.txt")
    if not had_mock:
        run_as("printf 'r4=1\\n' > files/Songate/mock-r4.txt")
    run_as("touch files/Songate/test-items")
    adb("shell", "input", "keyevent", "WAKEUP")
    adb("logcat", "-c")
    adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
    time.sleep(8)
    s = wait(lambda s: s.w > 0, 20)
    acts = adb("shell", "dumpsys", "activity", "activities")
    line = next((l.strip() for l in acts.splitlines() if "SongateActivity" in l and "orientation" in l.lower()), "")
    req = [l.strip() for l in acts.splitlines() if "mRequestedOrientation" in l or "requestedOrientation" in l]
    check("the screen held upright (requested portrait)", any("1" in r.split("=")[-1] or "PORTRAIT" in r.upper() for r in req) or "portrait" in line.lower(), "; ".join(req[:2]) or line[:120])
    log = adb("logcat", "-d")
    check("no opening played", "opening" not in " ".join(l for l in log.splitlines() if "Songate" in l and "opening start" in l), "")
    check("upright layout", bool(s) and s.h > s.w, f"{s.w}x{s.h}" if s else "no screen")
finally:
    adb("shell", "am", "force-stop", PKG)
    run_as(f"rm -f files/Songate/settings.txt files/Songate/test-items files/Songate/items.txt")
    if not had_mock:
        run_as("rm -f files/Songate/mock-r4.txt")
    run_as(f"for f in {KEEP}/*; do cp -p \"$f\" files/Songate/; done; rm -rf {KEEP}")
    back = run_as("ls files/Songate")
    check("the emulator's own settings back", "settings.txt" in back.split())
print("ALL GOOD" if not fails else f"{len(fails)} FAILED: {fails}")
sys.exit(1 if fails else 0)
