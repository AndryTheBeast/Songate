"""Songate for Android, dev build: the native sample rate (the user's ROM prompt, 2026-11-15). Plays songs one after another by hand
and reads, for each, its rate from Now playing's badge and the rate Android's own mixer has for Songate's track
(dumpsys media.audio_flinger, read only): they must be the same. Songate's log (SongateAudio) must show one opening of the device
per change of rate and none between songs of one rate.
    python devrate.py [songs]      (SONGATE_SERIAL: the device; the emulator by default)"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import adb, screen, wait, tap_item, run_as, clear_notices, PKG  # noqa: E402

args = sys.argv[1:]
FOLDER = ""
if "--folder" in args:   # a folder of the music played in its order instead of a shuffle (the test tones: 44.1, 48, 96 kHz)
    i = args.index("--folder")
    FOLDER = args[i + 1]
    del args[i:i + 2]
SONGS = int(args[0]) if args else 8
fails = []


def check(what, ok, detail=""):
    print(("OK   " if ok else "FAIL ") + what + (f"  ({detail})" if detail else ""))
    if not ok:
        fails.append(what)


def badge_rate(s):
    """The song's rate as Now playing's badge says it, in Hz (its letters come one by one: put together)."""
    joined = "".join(t for t, *_ in s.texts if len(t) == 1)
    m = re.search(r"([\d.]+) kHz", joined)
    if not m:
        return 0
    known = {"8", "11.025", "16", "22.05", "32", "44.1", "48", "88.2", "96", "176.4", "192", "352.8", "384"}
    v = m[1]
    while v and v not in known:   # (the codec's name runs into it: "MP3" "44.1 kHz")
        v = v[1:]
    return int(round(float(v) * 1000)) if v else 0


def track_rate(pid):
    """Songate's active track in Android's mixer: its SRate (0: none)."""
    for line in adb("shell", "dumpsys", "media.audio_flinger").splitlines():
        f = line.split()
        # [Type] Id Active Client Session PortId S Flags Format ChnMask SRate ... (Type is blank on most rows)
        if "yes" in f:
            a = f.index("yes")
            if len(f) > a + 8 and f[a + 1] == pid:
                try:
                    track_rate.fmt = f[a + 6]   # (00000005 float, 00000001 16-bit)
                    return int(f[a + 8])
                except ValueError:
                    return 0
    return 0


adb("shell", "input", "keyevent", "WAKEUP")
run_as("touch files/Songate/test-items")
adb("shell", "am", "force-stop", PKG)
adb("logcat", "-c")
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
time.sleep(7)
wait(lambda s: s.w > 0, 20)
clear_notices(screen())
pid = adb("shell", "pidof", PKG).strip()
tap_item("ph.nav#1")
time.sleep(1.2)
if "lib.source[0]" in screen().items:   # Local: the files themselves
    tap_item("lib.source[0]")
    time.sleep(1)
if FOLDER:
    time.sleep(4)   # (files new since the last start: the folder's walk finds them)
    from devlib import tap_text  # noqa: E402
    tap_text("Folders")
    time.sleep(1.5)
    for _ in range(12):
        s = screen()
        hit = [(x, y, w, h) for t, x, y, w, h in s.texts if t == FOLDER]
        if hit and 80 < hit[0][1] < s.h - 160:
            x, y, w, h = hit[0]
            adb("shell", "input", "tap", str(int((x + w / 2) * s.scale)), str(int((y + h / 2) * s.scale)))
            break
        adb("shell", "input", "swipe", "40", "1000", "40", "500", "300")
        time.sleep(1)
    time.sleep(1.5)
    tap_item("fo.play")
else:
    wait(lambda s: "ph.lshuffle" in s.items, 10)
    tap_item("ph.lshuffle")
wait(lambda s: s.playing, 15)
tap_item("ph.nav")
time.sleep(1)

seen = []
for i in range(SONGS):
    time.sleep(3)
    s = screen()
    want, got = badge_rate(s), track_rate(pid)
    seen.append(want)
    fmt = getattr(track_rate, "fmt", "")
    print(f"    {s.track[:40]:40}  badge {want:6} Hz   Android's track {got:6} Hz, format {fmt}")
    check(f"song {i + 1}: Android's track at its own rate", want > 0 and got == want, f"{got} for {want}")
    # float on the normal path; bit-perfect (a USB DAC, SongateOS) opens the song's own format: 00000001 16-bit, 00000006 packed
    # 24-bit, 00000003 32-bit (Songate's log says when it claimed it)
    claimed = any("bit-perfect on" in l for l in adb("logcat", "-d").splitlines()[-400:])
    check(f"song {i + 1}: Android's track in float, or bit-perfect in its own format", fmt == "00000005" or (claimed and fmt in ("00000001", "00000006", "00000003")),
          fmt + (" (bit-perfect)" if claimed and fmt != "00000005" else ""))
    if i + 1 < SONGS:
        tap_item("ph.next")
tap_item("ph.play")   # paused at the end
time.sleep(1)

opens = [l for l in adb("logcat", "-d").splitlines() if "SongateAudio: opened" in l and "(the start)" not in l]
changes = sum(1 for a, b in zip(seen, seen[1:]) if a != b)
first = 2   # (the start opens at 48 kHz; the song brought back at the start, and the first one played, may each open it again)
print(f"    {len(opens)} openings for {changes} changes of rate (+ up to {first} for the start)")
check("one opening per change of rate, none between songs of one rate", len(opens) <= changes + first, f"{len(opens)} openings")
for l in adb("logcat", "-d").splitlines():   # bit-perfect: what Android was asked and said
    if "SongateAudio: bit-perfect" in l or "Android says bit-perfect" in l:
        print("   ", l[19:])
run_as("rm -f files/Songate/test-items files/Songate/items.txt")
print("ALL GOOD" if not fails else f"{len(fails)} FAILED: {fails}")
sys.exit(1 if fails else 0)
