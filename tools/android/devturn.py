"""Songate for Android, dev only: the black bar after a turn and a visit to the home screen (the user, 2026-11-10: "rotate the
   phone, go to the phone's home screen, come back: a black bar sometimes appears"). Turns, leaves and returns in every order, a
   screenshot after each return, and says which have a band of pure black along an edge (the page never has one: its colours come
   from the cover). Nothing of it ships.
       python -I tools/android/devturn.py [rounds] [out folder]"""
import os
import subprocess
import sys
import time

SERIAL = os.environ.get("SONGATE_SERIAL", "emulator-5554")
ADB = r"C:\Android\sdk\platform-tools\adb.exe"
PKG = "io.github.andrythebeast.songate"
ACT = PKG + "/.SongateActivity"


def adb(*a, binary=False):
    r = subprocess.run([ADB, "-s", SERIAL, *a], capture_output=True)
    return r.stdout if binary else r.stdout.decode("utf-8", "replace")


def turn(r):
    adb("shell", "settings put system accelerometer_rotation 0")
    adb("shell", f"settings put system user_rotation {r}")


def shot(path):
    with open(path, "wb") as f:
        f.write(adb("exec-out", "screencap", "-p", binary=True))


def bands(path):
    """How many pixels of pure black run along each edge (left, top, right, bottom)."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()

    def black_col(x):
        return all(max(px[x, y]) <= 2 for y in range(0, h, 7))

    def black_row(y):
        return all(max(px[x, y]) <= 2 for x in range(0, w, 7))

    l = next((x for x in range(w) if not black_col(x)), w)
    r = next((x for x in range(w) if not black_col(w - 1 - x)), w)
    t = next((y for y in range(h) if not black_row(y)), h)
    b = next((y for y in range(h) if not black_row(h - 1 - y)), h)
    return (w, h), (l, t, r, b)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "..", "build", "devturn")
    os.makedirs(out, exist_ok=True)
    adb("shell", "am", "start", "-n", ACT)
    time.sleep(6)
    # each: (name, steps); a step is a turn (0 upright, 1 and 3 sideways), "home", "back" (to Songate), or a wait in seconds
    plans = [
        ("turn-home-back", [1, 1.5, "home", 1.5, "back"]),
        ("turn-home-quick", [1, 0.2, "home", 0.3, "back"]),
        ("home-turn-back", ["home", 1, 1, 1.5, "back"]),
        ("upright-home-back", [0, 1.5, "home", 1.5, "back"]),
        ("upright-home-quick", [0, 0.2, "home", 0.3, "back"]),
        ("home-upright-back", ["home", 1, 0, 1.5, "back"]),
        ("other-side-home-back", [3, 1.5, "home", 1.5, "back"]),
        ("side-to-side-home", [1, 0.3, 3, 0.2, "home", 1, "back"]),
        ("recents-back", [0, 1, "recents", 1.5, "back"]),
        ("turn-recents-back", [1, 0.3, "recents", 1.5, "back"]),
    ]
    found = 0
    for n in range(rounds):
        for name, steps in plans:
            for s in steps:
                if s == "home":
                    adb("shell", "input keyevent KEYCODE_HOME")
                elif s == "recents":
                    adb("shell", "input keyevent KEYCODE_APP_SWITCH")
                elif s == "back":
                    adb("shell", "am", "start", "-n", ACT)
                elif isinstance(s, float):
                    time.sleep(s)
                else:
                    turn(s)
            # two shots: 0.7 s after the return and 2 s later. The bar this looks for stays; black on all four sides in the first
            # shot only is Android's own return animation (Songate's window growing from its icon: seen 2026-11-11, one run in
            # three, after the side-to-side turn), said but not counted
            lines = []
            for late, wait in (("a", 0.7), ("b", 2.0)):
                time.sleep(wait)
                p = os.path.join(out, f"{n}-{name}-{late}.png")
                shot(p)
                size, b = bands(p)
                bad = any(v >= 8 for v in b) and not all(v >= min(size) for v in b)
                all_black = all(v >= min(size) for v in b)
                lines.append([late, size, b, bad, all_black])
            arriving = lines[0][3] and not lines[1][3] and all(v >= 8 for v in lines[0][2])
            for late, size, b, bad, all_black in lines:
                note = "  (all black)" if all_black else ""
                if bad:
                    note = "  (gone 2 s later: Android's return animation, not counted)" if arriving and late == "a" else "  <-- BLACK BAR"
                    found += 0 if arriving and late == "a" else 1
                print(f"{n} {name:24s} {late} {size[0]}x{size[1]} black l,t,r,b = {b}" + note, flush=True)
    turn(0)
    print("BLACK BARS:", found)


main()
