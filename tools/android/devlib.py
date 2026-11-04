"""Songate for Android, dev only: what the dev build shows (items.txt, written while test-items is in its settings folder) and taps by name.
   Used by devtest.py and devtap.py. Nothing of it ships."""
import os
import re
import subprocess
import sys
import time

SERIAL = os.environ.get("SONGATE_SERIAL", "emulator-5554")
ADB = r"C:\Android\sdk\platform-tools\adb.exe"
PKG = "io.github.andrythebeast.songate"
results = []


def adb(*args, check=False):
    r = subprocess.run([ADB, "-s", SERIAL, *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return r.stdout


def run_as(cmd):
    return adb("shell", f"run-as {PKG} sh -c '{cmd}'")


class Screen:
    def __init__(self, text):
        self.scale, self.w, self.h, self.playing, self.track = 1.0, 0, 0, False, ""
        self.items, self.texts = {}, []
        self.region = {}   # a control's region (Main, Modal, ...)
        self.top = []      # words drawn over the page (TOP lines: sheets, cards, toasts), as texts
        self.cut = set()   # words shortened to fit (their index in texts)
        m = re.search(r"scale ([\d.]+) size (\d+) (\d+) pos [-\d.]+ playing (\d) vol [\d.]+ track (.*?):", text)
        if m:
            self.scale, self.w, self.h, self.playing, self.track = float(m[1]), int(m[2]), int(m[3]), m[4] == "1", m[5]
        for line in text.splitlines():
            t = re.match(r"\s+(\w+)\s+(\S+)\s+(-?\d+),\s*(-?\d+)\s+(\d+)x(\d+)", line)
            if t:
                self.items[t[2]] = tuple(int(v) for v in t.group(3, 4, 5, 6))
                self.region[t[2]] = t[1]
            elif line.startswith("TEXT ") or line.startswith("TOP "):
                top = line.startswith("TOP ")
                head, _, s = line[4 if top else 5:].partition("|")
                v = [float(n) for n in head.split()]
                x, y, w, h = v[:4]
                if len(v) > 4 and v[4]:
                    self.cut.add(len(self.texts))
                if top:
                    self.top.append(len(self.texts))
                self.texts.append((s, x, y, w, h))

    def has_text(self, s):
        return any(s in t[0] for t in self.texts)


def screen():
    return Screen(run_as("cat files/Songate/items.txt 2>/dev/null"))


notices = []


def wait(cond, secs=12, what=""):
    end = time.time() + secs
    while time.time() < end:
        s = screen()
        if cond(s):
            return s
        clear_notices(s)
        time.sleep(0.4)
    return None


def tap_at(s, x, y):
    adb("shell", "input", "tap", str(int(x * s.scale)), str(int(y * s.scale)))


def clear_notices(s):
    """A new user's notice (the covers one) comes when it is ready, over whatever is there: answered Not now, and said at the end."""
    while "notice.later" in s.items:
        notices.append(next((t[0] for t in s.texts if t[0].startswith("Covers") or len(t[0]) > 20), "a notice"))
        x, y, w, h = s.items["notice.later"]
        tap_at(s, x + w / 2, y + h / 2)
        time.sleep(0.8)
        s = screen()
    return s


def tap_item(name):
    s = clear_notices(screen())
    if name not in s.items:
        return False
    x, y, w, h = s.items[name]
    tap_at(s, x + w / 2, y + h / 2)
    return True


def tap_text(sub):
    s = clear_notices(screen())
    for t, x, y, w, h in s.texts:
        if sub in t:
            tap_at(s, x + w / 2, y + h / 2)
            return True
    return False


def step(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("  ok    " if ok else "  FAIL  ") + name + (f"  ({detail})" if detail and not ok else ""))
    if not ok:   # what was there instead
        now = screen()
        print("          on screen: " + "; ".join(t[0] for t in now.texts if len(t[0]) > 2)[:300])
        print("          controls:  " + " ".join(list(now.items)[:16]))


def session_state():
    out = adb("shell", "dumpsys", "media_session")
    i = out.find(f"package={PKG}")
    m = re.search(r"state=PlaybackState \{state=([A-Z]+)", out[i:i + 2000]) if i >= 0 else None
    return m[1] if m else "none"
