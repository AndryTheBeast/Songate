"""Songate for Android, dev only (session 23): a Settings page's folded cards, every one listed (where, how big) and each opened in turn
with what it holds. python tools/android/devfolds.py <serial> <Page name>"""
import os
import sys
import subprocess
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
page = sys.argv[2] if len(sys.argv) > 2 else "Playback"
from devlib import *   # noqa: E402,F403

stamp = time.strftime("%Y%m%d-%H%M%S")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; [ -d files/Songate ] && mv files/Songate files/aside/before-devfolds-{stamp}; mkdir -p files/Songate; touch files/Songate/test-items")
run_as("echo musicRoot=/storage/0000-0000/Music > files/Songate/settings.txt; echo opening=0 >> files/Songate/settings.txt")   # (no quotes: run_as wraps it in its own)
adb("shell", "appops", "set", "--uid", PKG, "MANAGE_EXTERNAL_STORAGE", "allow")
adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
time.sleep(6)
for _ in range(3):
    clear_notices(screen())
    time.sleep(1)
tap_item("ph.nav#4"); time.sleep(0.8)
if not screen().has_text("Find a setting"):
    adb("shell", "input", "keyevent", "4"); time.sleep(0.8)
tap_text(page); time.sleep(1.2)
s = screen()
print("screen", s.w, s.h, "scale", s.scale)
for k, v in sorted(s.items.items(), key=lambda kv: kv[1][1]):
    if k.startswith("set.fold"):
        print(" ", k, v)
print("words:", [t[0] for t in s.texts if t[2] > 0][:60])
for _ in range(4):
    adb("shell", "input", "swipe", "40", "1500", "40", "900", "400"); time.sleep(0.8)
    s = screen()
    print("--- after a swipe:", {k: v for k, v in s.items.items() if k.startswith("set.fold") or k == "set.swipe"})
shot = subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout
os.makedirs("../../docs/next-session/session23/shots/android", exist_ok=True)
open(f"../../docs/next-session/session23/shots/android/folds-{page}-bottom.png", "wb").write(shot)
print("all words at the bottom:", [t[0] for t in s.texts][-40:])
adb("shell", "am", "force-stop", PKG)
run_as(f"rm -rf files/Songate; [ -d files/aside/before-devfolds-{stamp} ] && mv files/aside/before-devfolds-{stamp} files/Songate")
print("settings put back")
