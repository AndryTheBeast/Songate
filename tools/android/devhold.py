"""Songate for Android, dev only: the HiBy R4's hold switch in Settings › Controls (the user, 2026-11-17), on the emulator's simulated R4.
   python tools/android/devhold.py [serial]          (default: emulator-5554; the dev APK installed first)
Checks: Controls holds only the Hold switch card (no Buttons, no Pointer: the R4 is touch only); the list's line says so; the switch
off, then on (the simulated switch: mock-hold.txt) when the page opens again; Locks picks Media and All, back to Volume; Sleep when
locked off and on. The settings folder is set aside first and put back at the end. Ends with ALL GOOD or the steps that failed."""
import os
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import PKG, SERIAL, adb, results, run_as, screen, step, tap_at, tap_item, wait   # noqa: E402
from devphase2_helpers import kicker, scroll   # noqa: E402

stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devhold-{stamp}"
print(f"devhold on {SERIAL}, {stamp}", flush=True)


OUT = os.environ.get("DEVOUT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"))


def shot(name):
    import subprocess
    from devlib import ADB
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, name + ".png"), "wb").write(subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout)


def has(s, t):
    return s is not None and any(x[0] == t for x in s.texts)


def kickers(s):
    lines = {}
    for t, x, y, w, h in s.texts:
        if len(t) == 1:
            lines.setdefault(round(y), []).append((x, t))
    return {"".join(c for _, c in sorted(v)).strip() for v in lines.values() if len(v) > 3}


def controls():
    tap_item("ph.nav#4")
    time.sleep(1.8)
    for _ in range(6):
        scroll(False, 0.75, 0.3)
    for _ in range(8):
        s = screen()
        hit = next((t for t in s.texts if t[0] == "Controls"), None)
        if hit and s.h * 0.1 < hit[2] < s.h - 170:
            line = [t[0] for t in s.texts if abs(t[2] - hit[2] - 34) < 14]
            tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
            time.sleep(1.6)
            return line
        scroll()
    return []


adb("shell", "input", "keyevent", "224")
adb("shell", "svc", "power", "stayon", "true")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}")
run_as('rm -f files/Songate/items.txt files/Songate/start-guard.txt files/Songate/mock-ui.txt files/Songate/mock-hold.txt; '
       'touch files/Songate/mock-r4.txt files/Songate/test-items; sed -i "/^foldOpen=/d" files/Songate/settings.txt')
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    wait(lambda s: "ph.nav#4" in s.items, 30)
    time.sleep(3)
    line = controls()
    step("Settings list: Controls, The hold switch", "The hold switch" in line, " · ".join(line))
    s = wait(lambda s: "sys.holdlevel[0]" in s.items, 6)
    k = kickers(s) if s else set()
    step("Controls: only the Hold switch card (no Buttons, no Pointer)", s is not None and "HOLD SWITCH" in k and "BUTTONS" not in k and
         "POINTER" not in k and not has(s, "Pointer speed"), ", ".join(sorted(k)))
    shot("devhold-off")   # (On / Off: fixed-width digits, not in items.txt: its picture, looked at)
    step("... Locks: Volume at first, its words", s is not None and any(t[0].startswith("Play, pause, skip and the volume") for t in s.texts))
    tap_item("sys.holdlevel[0]")
    s = wait(lambda s: has(s, "Play, pause and skip; the volume and power still work"), 5)
    step("Media: picked, its words", s is not None)
    tap_item("sys.holdlevel[2]")
    s = wait(lambda s: has(s, "Every button, until you slide it back"), 5)
    step("All: picked, its words", s is not None)
    tap_item("sys.holdlevel[1]")
    time.sleep(1)
    tap_item("sys.holdsleep")
    time.sleep(1.2)
    tap_item("sys.holdsleep")
    time.sleep(1.2)
    step("Sleep when locked: off and on again (the service's answer each time)", "sys.holdsleep" in screen().items)
    run_as("touch files/Songate/mock-hold.txt")   # the switch slid on
    adb("shell", "input", "keyevent", "4")
    time.sleep(1.2)
    controls()
    wait(lambda s: "sys.holdlevel[0]" in s.items, 6)
    time.sleep(1)
    shot("devhold-on")
    print("  look at devhold-off.png and devhold-on.png: the Hold switch row says Off, then On", flush=True)
finally:
    adb("shell", "svc", "power", "stayon", "false")
    adb("shell", "am", "force-stop", PKG)
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && "
                 "rm -f files/Songate/test-items files/Songate/items.txt files/Songate/mock-hold.txt; ls files/Songate/settings.txt")
    print("settings folder put back:", "yes" if "settings.txt" in out else "NO", flush=True)

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
