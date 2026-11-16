"""Songate for Android, dev build, the HiBy R4 with SongateOS: Sound & DAC › Each song at its own rate (SongateOS's out_rate,
2026-11-15). Plays a folder of songs at different rates (the test tones: 44.1, 48, 96 kHz) on WIRED headphones and reads, read
only: Songate's log (out_rate asked at each change of rate, the song let go once the output says it is ready) and Android's own
output thread (dumpsys media.audio_flinger: its sample rate, the family's rate for the song). Also checks the media session that
SongateOS asked for (choice A): Songate's, active, with its media-button receiver, from the start.
    python devoutrate.py --folder RateTest      (SONGATE_SERIAL: the R4; headphones plugged in)"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import adb, screen, wait, tap_item, tap_text, run_as, clear_notices, PKG  # noqa: E402

args = sys.argv[1:]
FOLDER = args[args.index("--folder") + 1] if "--folder" in args else "RateTest"
fails = []


def check(what, ok, detail=""):
    print(("OK   " if ok else "FAIL ") + what + (f"  ({detail})" if detail else ""))
    if not ok:
        fails.append(what)


def family(rate):
    fam = (44100, 88200, 176400, 352800) if rate % 11025 == 0 else (48000, 96000, 192000, 384000)
    return next((r for r in fam if r >= rate), fam[-1])


def output_rates():
    """The sample rates of Android's output threads now (read only)."""
    return {int(m) for m in re.findall(r"Sample rate:\s*(\d+)", adb("shell", "dumpsys", "media.audio_flinger"))}


adb("shell", "input", "keyevent", "WAKEUP")
run_as("touch files/Songate/test-items")
adb("shell", "am", "force-stop", PKG)
adb("logcat", "-c")
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
time.sleep(8)
wait(lambda s: s.w > 0, 20)
clear_notices(screen())

# the media session, from the start (paused, before anything plays)
ms = adb("shell", "dumpsys", "media_session")
block = ms[ms.find(f"package={PKG}") - 300: ms.find(f"package={PKG}") + 900] if f"package={PKG}" in ms else ""
check("media session: Songate's, from the start", bool(block))
check("media session: active", "active=true" in block)
check("media session: its media-button receiver", "mediaButtonReceiver=null" not in block and "MediaButtonReceiver" in block,
      re.search(r"mediaButtonReceiver=\S+", block)[0] if re.search(r"mediaButtonReceiver=\S+", block) else "")

tap_item("ph.nav#1")
time.sleep(1.2)
if "lib.source[0]" in screen().items:
    tap_item("lib.source[0]")
    time.sleep(1)
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
wait(lambda s: s.playing, 15)
tap_item("ph.nav")
time.sleep(1)

songs = 3
TIMELINE = []
for i in range(songs):
    log = []
    want = 0
    for _ in range(72):   # up to 18 s: the switch takes 2 to 3.5 s, the ROM waits up to 15 s to say so
        time.sleep(0.25)
        log = adb("logcat", "-d").splitlines()
        opened = [l for l in log if "SongateAudio: opened" in l]
        m = re.search(r"opened (\d+) Hz", opened[-1]) if opened else None
        want = family(int(m[1])) if m else 0
        if want and want in output_rates() and any(f"out_rate {want} ready" in l for l in log):
            break
    rates = output_rates()
    # the DAC's own rate, as the ROM reads it from HiBy's driver (getprop, read only): the real proof, bit-perfect or switched
    dac = 0
    for _ in range(12):
        dac = int(adb("shell", "getprop", "sys.hibyr4.st_rate").strip() or 0)
        if dac == want:
            break
        time.sleep(0.5)
    check(f"song {i + 1}: the DAC itself at the song's rate (st_rate)", dac == want, f"{dac} for {want}")
    asked = [l for l in log if "out_rate" in l and "asked" in l]
    ready = [l for l in log if f"out_rate {want} " in l and "let go" in l]
    print(f"    song {i + 1}: Songate's stream for {want} Hz; Android's outputs {sorted(rates)}")
    for l in (asked[-1:] + ready[-1:]):
        print("      ", l[l.find("HiBy R4"):])
    check(f"song {i + 1}: the wired output at the song's rate", want in rates, f"{want} in {sorted(rates)}")
    check(f"song {i + 1}: held until ready (or the rate already right)", bool(ready) and "NOT ready" not in ready[-1] or not asked, (ready or ["no switch"])[-1][-60:])
    if asked and ready:   # the screen kept going meanwhile: Songate's own clock and the log's agree (SongateOS: it stalled 4-5 s)
        def at(l):
            h, mi, s = l.split()[1].split(":")
            return int(h) * 3600 + int(mi) * 60 + float(s)
        wall = at(ready[-1]) - at(asked[-1])
        own = float(re.search(r"after ([\d.]+) s", ready[-1])[1])
        check(f"song {i + 1}: the screen not held while the output switched", wall - own < 1.0, f"{wall:.1f} s real, {own:.1f} s Songate's")
    if i + 1 < songs:
        TIMELINE += log
        adb("logcat", "-c")
        tap_item("ph.next")
tap_item("ph.play")   # paused at the end
time.sleep(1)
print("    the timeline (Songate's log):")
for l in TIMELINE + adb("logcat", "-d").splitlines():
    if "HiBy R4: out_rate" in l or "SongateAudio: opened" in l or "SongateAudio: closed" in l:
        print("      ", l[6:18], l[l.find("Songate"):][:110])
run_as("rm -f files/Songate/test-items files/Songate/items.txt")
print("ALL GOOD" if not fails else f"{len(fails)} FAILED: {fails}")
sys.exit(1 if fails else 0)
