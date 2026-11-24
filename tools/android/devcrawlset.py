"""Songate for Android, dev only (session 23): devcrawl.py on settings of its own, a library already chosen (the SD card's Music, access
given), so the routes find a set-up Songate rather than a first start. The device's own settings are put aside first and back after.
python tools/android/devcrawlset.py <serial> <out folder> [upright|sideways] [route ...]"""
import os
import subprocess
import sys
import time

os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import *   # noqa: E402,F403

out, rest = sys.argv[2], sys.argv[3:]
stamp = time.strftime("%Y%m%d-%H%M%S")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; [ -d files/Songate ] && mv files/Songate files/aside/before-crawlset-{stamp}; mkdir -p files/Songate; touch files/Songate/test-items")
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
    time.sleep(8)
    for _ in range(3):
        clear_notices(screen())
        time.sleep(1.5)
    adb("shell", "am", "force-stop", PKG)
    r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "devcrawl.py"), out] + rest,
                       env=dict(os.environ, SONGATE_SERIAL=SERIAL))
finally:
    adb("shell", "am", "force-stop", PKG)
    adb("shell", "settings", "put", "system", "user_rotation", "0")
    run_as(f"rm -rf files/Songate; [ -d files/aside/before-crawlset-{stamp} ] && mv files/aside/before-crawlset-{stamp} files/Songate")
    print("settings put back")
