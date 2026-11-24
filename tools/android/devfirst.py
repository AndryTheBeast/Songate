"""Songate for Android, dev only: the first start's first steps, one at a time, with a screenshot after each (session 23's repro of
the access card that never came). python tools/android/devfirst.py [serial] [outdir]. Puts the settings that were there back."""
import os
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else "."
os.makedirs(OUT, exist_ok=True)
from devlib import *   # noqa: E402,F403


def shot(name):
    data = subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout
    with open(os.path.join(OUT, name + ".png"), "wb") as f:
        f.write(data)


def show(label):
    s = screen()
    print(f"== {label}: controls {' '.join(sorted(s.items))}")
    print("   top:", " | ".join(s.texts[i][0] for i in s.top)[:400])
    return s


stamp = time.strftime("%Y%m%d-%H%M%S")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; [ -d files/Songate ] && mv files/Songate files/aside/before-devfirst-{stamp}; mkdir -p files/Songate; touch files/Songate/test-items")
adb("shell", "appops", "set", "--uid", PKG, "MANAGE_EXTERNAL_STORAGE", "default")
adb("shell", "appops", "set", PKG, "MANAGE_EXTERNAL_STORAGE", "default")
adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
try:
    s = wait(lambda s: s.has_text("On my server") and "sp.yes" in s.items, 20)
    show("first card")
    shot("1-first-card")
    tap_item("sp.yes")
    for t in (0.3, 1.0, 2.5):
        time.sleep(t)
        show(f"after On this device +{t}s")
    shot("2-after-this-device")
finally:
    adb("shell", "am", "force-stop", PKG)
    run_as(f"rm -rf files/Songate; [ -d files/aside/before-devfirst-{stamp} ] && mv files/aside/before-devfirst-{stamp} files/Songate")
    print("settings put back")
