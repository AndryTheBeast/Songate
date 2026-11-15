"""Songate for Android, dev build: the music's card still mounting at the start (a boot's; dev: mock-cardwait.txt makes the card
"mount" N seconds in). While it mounts: the library and the queue with its song are there from Songate's own copies, nothing plays;
Play shows "The SD card isn't ready yet: it plays as soon as it is" and the song starts by itself once the card is there.
    python devcardwait.py          (SONGATE_SERIAL: the device; the emulator by default)"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import adb, screen, wait, tap_item, run_as, clear_notices, PKG, SERIAL  # noqa: E402

WAIT = 25
fails = []


def check(what, ok, detail=""):
    print(("OK   " if ok else "FAIL ") + what + (f"  ({detail})" if detail else ""))
    if not ok:
        fails.append(what)


def tracks():
    for _ in range(3):
        tap_item("ph.nav#1")
        time.sleep(1.2)
        if "lib.source[0]" in screen().items:   # Local: the card's music (a server's songs need no card)
            tap_item("lib.source[0]")
            time.sleep(1.0)
        for t in screen().texts:
            m = re.match(r"(\d+) tracks?", t[0])
            if m:
                return int(m[1])
    return -1


adb("shell", "input", "keyevent", "WAKEUP")
run_as("touch files/Songate/test-items")
run_as("rm -f files/Songate/mock-cardwait.txt")
adb("shell", "am", "force-stop", PKG)
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
time.sleep(7)
wait(lambda s: s.w > 0, 20)
clear_notices(screen())
n0 = tracks()
wait(lambda s: "ph.lshuffle" in s.items, 10)
tap_item("ph.lshuffle")
s = wait(lambda s: s.playing and s.track not in ("", "-"), 15)
song = s.track if s else ""
tap_item("ph.nav")
time.sleep(1)
tap_item("ph.play")   # paused, saved
time.sleep(4)
adb("shell", "input", "keyevent", "HOME")   # left as a user leaves it: everything saved (the server's queue: not the one playing)
time.sleep(3)
print("before:", n0, "tracks, the queue's song", repr(song))
head = run_as("head -1 files/Songate/server-queue.tsv 2>/dev/null").split("\t")[0]
check("before: a library and a queue", n0 > 0 and song)

# the start with the card mounting
run_as(f'printf "{WAIT}\\n" > files/Songate/mock-cardwait.txt')
adb("shell", "am", "force-stop", PKG)
t0 = time.time()
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
time.sleep(8)
wait(lambda s: s.w > 0, 20)
clear_notices(screen())
s = screen()
check("mounting: the queue's song is shown", s.track == song, s.track)
tap_item("ph.nav")
time.sleep(1.5)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "next-session", "devcardwait")
os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "mounting-now-playing.png"), "wb") as f:   # the cover from Songate's own cache, while it waits
    f.write(subprocess.run([r"C:\Android\sdk\platform-tools\adb.exe", "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout)
check("mounting: nothing plays", not s.playing)
n1 = tracks()
check("mounting: the library is there", n1 == n0, f"{n1} of {n0}")
tap_item("ph.nav")
time.sleep(1)
tap_item("ph.play")
time.sleep(0.6)
s = screen()
check("Play: the warning", s.has_text("The SD card isn't ready yet: it plays as soon as it is"), [t for t, *_ in s.texts if "SD" in t][:2])
check("Play: still nothing plays", not s.playing)
left = WAIT - (time.time() - t0)
s = wait(lambda s: s.playing, max(5, left + 25))   # (in the 2 min after a boot the card must also stay mounted 15 s)
print(f"    played {time.time() - t0:.0f} s after the start (the card at {WAIT} s)")
check("the card there: it plays by itself, the same song", bool(s) and s.track == song, s.track if s else "")
tap_item("ph.play")   # paused again
run_as("rm -f files/Songate/mock-cardwait.txt")
print("ALL GOOD" if not fails else f"{len(fails)} FAILED: {fails}")
sys.exit(1 if fails else 0)
