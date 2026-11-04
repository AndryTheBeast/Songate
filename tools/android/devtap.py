"""Songate for Android, dev only: drive the dev build by hand, by name (test-items must be in its settings folder; devlib.py).
   python tools/android/devtap.py [list] [<control>] [text:<words>] [key:<code>] [wait:<seconds>] [shot:<file.png>] ...
Each argument in turn: list prints the controls and the words on screen; a control's name taps it; text: taps the first words that
hold <words>; key: sends an Android key (4 back, 3 home); shot: saves a screenshot. SONGATE_SERIAL picks the device."""
import sys
import time

from devlib import ADB, SERIAL, adb, screen, tap_item, tap_text
import subprocess

for a in sys.argv[1:]:
    if a == "list":
        s = screen()
        print(f"size {s.w}x{s.h} scale {s.scale} playing {int(s.playing)} track {s.track}")
        print("controls: " + " ".join(s.items))
        print("words:    " + " | ".join(t[0] for t in s.texts if len(t[0]) > 1))
    elif a.startswith("text:"):
        print(("tapped " if tap_text(a[5:]) else "no words ") + a[5:])
    elif a.startswith("key:"):
        adb("shell", "input", "keyevent", a[4:])
    elif a.startswith("wait:"):
        time.sleep(float(a[5:]))
        continue
    elif a.startswith("shot:"):
        with open(a[5:], "wb") as f:
            f.write(subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout)
    else:
        print(("tapped " if tap_item(a) else "no control ") + a)
    time.sleep(1.0)
