"""Songate for Android, dev only (session 23): any dev script on settings of its own, a library already chosen (the SD card's Music, access
given), so the routes find a set-up Songate rather than a first start. The device's own settings are put aside first and back after.
python tools/android/devwith.py <serial> <script.py> [its arguments]"""
import os
import subprocess
import sys
import time

os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import *   # noqa: E402,F403

script, rest = sys.argv[2], sys.argv[3:]
stamp = time.strftime("%Y%m%d-%H%M%S")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; [ -d files/Songate ] && mv files/Songate files/aside/before-with-{stamp}; mkdir -p files/Songate; touch files/Songate/test-items; touch files/Songate/test-nonotice" + ("" if "devfold" in script else "; touch files/Songate/test-nofold"))   # (every card open but for devfold)
run_as("echo musicRoot=/storage/0000-0000/Music > files/Songate/settings.txt; echo opening=0 >> files/Songate/settings.txt")   # (no quotes: run_as wraps it in its own)
adb("shell", "appops", "set", "--uid", PKG, "MANAGE_EXTERNAL_STORAGE", "allow")
adb("shell", "settings", "put", "system", "user_rotation", "0")
try:
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    # the library read, and a new user's notices answered (covers, lyrics): then close, so the crawl starts it afresh
    end = time.time() + 120
    while time.time() < end:
        s = screen()
        clear_notices(s)
        if "ph.nav#1" in s.items and not any("Reading" in t[0] for t in s.texts):
            break
        time.sleep(2)
    # a new user's questions (covers, lyrics) answered "now", their short work let finish, so no script meets them again
    quiet = 0
    end = time.time() + 150
    while time.time() < end and quiet < 8:
        s = screen()
        if "notice.make" in s.items:
            tap_item("notice.make")
            quiet = 0
        elif any(k.startswith("side.job") or k.startswith("ph.job") for k in s.items) or any("Reading" in t[0] or "Making" in t[0] for t in s.texts):
            quiet = 0
        else:
            quiet += 1
        time.sleep(2)
    adb("shell", "input", "keyevent", "3")   # (home: Songate saves what it has as it leaves)
    time.sleep(3)
    adb("shell", "am", "force-stop", PKG)
    r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), script)] + rest,
                       env=dict(os.environ, SONGATE_SERIAL=SERIAL, PYTHONIOENCODING="utf-8"))
finally:
    adb("shell", "am", "force-stop", PKG)
    adb("shell", "settings", "put", "system", "user_rotation", "0")
    run_as(f"rm -rf files/Songate; [ -d files/aside/before-with-{stamp} ] && mv files/aside/before-with-{stamp} files/Songate")
    print("settings put back")
