"""Songate for Android, dev build: nothing kept running while paused (the SongateOS session, 2026-11-15: a paused Songate's stream
ran on silence, Android's AudioMix wakelock held for hours, ~40 % of the battery a day on standby). Plays, pauses, and after 10 s
reads (read only): Songate has no track in Android's mixer and no AudioMix wakelock; then plays again: its track is back and the
song goes on from where it was.
    python devidle.py      (SONGATE_SERIAL: the device; the emulator by default)"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import adb, screen, wait, tap_item, run_as, clear_notices, PKG  # noqa: E402

fails = []


def check(what, ok, detail=""):
    print(("OK   " if ok else "FAIL ") + what + (f"  ({detail})" if detail else ""))
    if not ok:
        fails.append(what)


def track_state(pid):
    """Songate's tracks in Android's mixer: how many, and whether one is active."""
    rows = [l for l in adb("shell", "dumpsys", "media.audio_flinger").splitlines() if re.search(rf"\s{pid}\s", l) and ("yes" in l.split() or " no " in l)]
    active = any("yes" in l.split() for l in rows)
    return len(rows), active


def audiomix(uid):
    return [l.strip() for l in adb("shell", "dumpsys", "power").splitlines() if "AudioMix" in l and (uid in l or "ws=" in l and uid in l)]


adb("shell", "input", "keyevent", "WAKEUP")
run_as("touch files/Songate/test-items")
adb("shell", "am", "force-stop", PKG)
adb("logcat", "-c")
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
time.sleep(7)
wait(lambda s: s.w > 0, 20)
clear_notices(screen())
pid = adb("shell", "pidof", PKG).strip()
uid = re.search(r"(?:userId|appId)=(\d+)", adb("shell", "dumpsys", "package", PKG))
uid = uid[1] if uid else ""
tap_item("ph.nav")
time.sleep(1)
if not screen().playing:
    tap_item("ph.play")
s = wait(lambda s: s.playing, 10)
check("playing", bool(s))
time.sleep(3)
n, active = track_state(pid)
check("playing: Songate's track is active in Android's mixer", active, f"{n} tracks")
tap_item("ph.play")   # pause
time.sleep(1)
p0 = screen()
check("paused", not p0.playing)
time.sleep(10)
n, active = track_state(pid)
check("paused 10 s: no active track of Songate's", not active, f"{n} tracks, active {active}")
def held_wakelocks():
    """The wakelocks held now (dumpsys power's "Wake Locks: size=N" list), not its history."""
    out, on = [], False
    for l in adb("shell", "dumpsys", "power").splitlines():
        if l.strip().startswith("Wake Locks: size="):
            on = True
            continue
        if on:
            if not l.strip():
                break
            out.append(l.strip())
    return out


wl = [l for l in held_wakelocks() if "AudioMix" in l and uid and uid in l]
check("paused 10 s: no AudioMix wakelock held for Songate", not wl, "; ".join(x[:140] for x in wl) or f"uid {uid}")
closed = any("closed after 5 s" in l for l in adb("logcat", "-d").splitlines())
check("Songate's log: closed while idle", closed)
tap_item("ph.play")   # play again
s = wait(lambda s: s.playing, 10)
time.sleep(2)
n, active = track_state(pid)
check("played again: its track is back and active", bool(s) and active, f"{n} tracks")
tap_item("ph.play")
run_as("rm -f files/Songate/test-items files/Songate/items.txt")
print("ALL GOOD" if not fails else f"{len(fails)} FAILED: {fails}")
sys.exit(1 if fails else 0)
