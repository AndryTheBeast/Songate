"""Songate for Android, dev build: skips by hand and reads where each new song is over its first 6.5 s (Now playing's time): a song
that runs ahead (the user's Hiby R4, 2026-11-15: "it played 2 seconds and then skipped 6 seconds"; SDL's AAudio write dropped buffers,
tools/android/sdl-patches/SDL_aaudio.c). Exits 1 when any song jumped.    python devskip.py [skips]   (SONGATE_SERIAL: the device)"""
import re
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from devlib import adb, screen, wait, tap_item, run_as, clear_notices, PKG  # noqa: E402

N = int(sys.argv[1]) if len(sys.argv) > 1 else 10


def pos(s):
    # the song's time under the seek bar (texts below the top bar: the clock is at its top)
    times = [(y, t) for t, x, y, w, h in s.texts if re.fullmatch(r"\d+:\d\d", t) and y > 100]
    if not times:
        chars = sorted([(y, x, t) for t, x, y, w, h in s.texts if len(t) == 1 and y > 100])
        rows = {}
        for y, x, t in chars:
            rows.setdefault(round(y), []).append((x, t))
        for y in sorted(rows):
            line = "".join(t for x, t in sorted(rows[y]))
            m = re.search(r"(\d+):(\d\d)", line)
            if m:
                return int(m[1]) * 60 + int(m[2])
        return -1
    m = re.fullmatch(r"(\d+):(\d\d)", sorted(times)[0][1])
    return int(m[1]) * 60 + int(m[2])


def rate(s):
    joined = "".join(t for t, *_ in s.texts if len(t) == 1)
    m = re.search(r"([\d.]+) kHz", joined)
    return m[1] if m else "?"


adb("shell", "input", "keyevent", "WAKEUP")
run_as("touch files/Songate/test-items")
adb("shell", "am", "force-stop", PKG)
adb("logcat", "-c")
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
time.sleep(7)
wait(lambda s: s.w > 0, 20)
clear_notices(screen())
tap_item("ph.nav")
time.sleep(1)
tap_item("ph.play")
time.sleep(2)
bad = 0
for i in range(N):
    tap_item("ph.next")
    t0 = time.time()
    seen = []
    jumped = False
    for at in (1.0, 2.0, 3.0, 4.0, 5.0, 6.5):
        while time.time() - t0 < at:
            time.sleep(0.05)
        s = screen()
        p = pos(s)
        el = time.time() - t0
        seen.append(f"{el:.1f}s->{p}")
        if p > el + 2.5:
            jumped = True
    print(f"skip {i + 1:2}: {s.track[:30]:30} {rate(s):>6} kHz  " + "  ".join(seen) + ("   <-- JUMPED" if jumped else ""))
    bad += jumped
tap_item("ph.play")
print(f"{bad} of {N} jumped ahead")
for l in adb("logcat", "-d").splitlines():
    if "SongateAudio" in l:
        print("   ", l[19:])
run_as("rm -f files/Songate/test-items files/Songate/items.txt")
sys.exit(1 if bad else 0)
