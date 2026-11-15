"""Songate on the HiBy R4 (the user's own ROM): Settings › Sound & DAC and Appearance › Status LED, through the ROM's service.

    SONGATE_SERIAL=a5185cc4 python tools/android/devr4.py

Needs the dev build on the R4 and the ROM's HiBy service (its sys.hibyr4.* properties are read with getprop to see what Songate
set; nothing is ever set from here but through Songate's own screen). Each setting it changes is put back by Songate before the
next step, and the end compares every sys.hibyr4.* setting with how it found them.
Never touches line out: it only checks that choosing it asks first, and answers no.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import adb, screen, wait, tap_item, run_as, clear_notices, PKG, SERIAL  # noqa: E402

if SERIAL.startswith("emulator"):
    sys.exit("devr4: run it on the HiBy R4 (SONGATE_SERIAL=a5185cc4)")

fails = []
SETTINGS = ("gain", "filter", "balance", "out35", "out44", "lo_gain", "remote", "vol_steps", "vol_restore", "vol_boot",
            "led_mode", "led_color", "led_bright", "led_effect", "led_charge", "led_screen",
            "led_back", "led_back_charge", "led_back_notif", "led_back_beat", "led_notif", "led_notif_color", "led_lowbat",
            "led_lowbat_level", "led_quality", "led_q_cd", "led_q_hires", "led_q_ultra", "led_beat")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "next-session", "devr4")
os.makedirs(OUT, exist_ok=True)


def shot(name):
    with open(os.path.join(OUT, name + ".png"), "wb") as f:
        f.write(subprocess.run([r"C:\Android\sdk\platform-tools\adb.exe", "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout)


def prop(key):
    return adb("shell", "getprop", "sys.hibyr4." + key).strip()


def props():
    return {k: prop(k) for k in SETTINGS}


def check(what, ok, detail=""):
    print(("OK   " if ok else "FAIL ") + what + (f"  ({detail})" if detail else ""))
    if not ok:
        fails.append(what)


def swipe(dy):
    y0 = 1000 if dy > 0 else 300
    adb("shell", "input", "swipe", "40", str(y0), "40", str(y0 - dy), "400")
    time.sleep(1.3)


def open_cat(name):
    tap_item("ph.nav#4"); time.sleep(1.2)
    for _ in range(4):
        if screen().has_text("Find a setting"):
            break
        adb("shell", "input", "keyevent", "BACK"); time.sleep(0.5)
    for _ in range(3):
        swipe(-700)
    for _ in range(8):
        s = screen()
        for t, x, y, w, h in s.texts:
            if t == name and 60 < y < s.h - 160:
                adb("shell", "input", "tap", str(int((x + w / 2) * s.scale)), str(int((y + h / 2) * s.scale)))
                time.sleep(1.5)
                return True
        swipe(400)
    return False


def find(key, tries=14):
    for _ in range(tries):
        s = screen()
        if key in s.items and 120 < s.items[key][1] < s.h * 0.7:
            return s
        swipe(250 if key not in s.items or s.items[key][1] >= s.h * 0.7 else -250)
    return screen()


def until(fn, secs=3.0):
    end = time.time() + secs
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.25)
    return fn()


def flip(key_base, prop_key, values):
    """Taps the other choice, sees the ROM take it, taps the first again and sees it back."""
    start = prop(prop_key)
    if start not in values:
        check(f"{prop_key}: a known value", False, start)
        return
    i0 = values.index(start)
    i1 = (i0 + 1) % len(values)
    find(f"{key_base}[{i1}]")
    tap_item(f"{key_base}[{i1}]")
    check(f"{prop_key} → {values[i1]}", until(lambda: prop(prop_key) == values[i1]), prop(prop_key))
    find(f"{key_base}[{i0}]")
    tap_item(f"{key_base}[{i0}]")
    check(f"{prop_key} back to {start}", until(lambda: prop(prop_key) == start), prop(prop_key))


found = props()
print("the R4 as found:", found)
if not prop("ready"):
    print("note: sys.hibyr4.ready is empty")
adb("shell", "input", "keyevent", "WAKEUP")
had_items = "yes" in run_as("[ -e files/Songate/test-items ] && echo yes")
run_as("touch files/Songate/test-items")
adb("shell", "am", "force-stop", PKG)
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
time.sleep(6)
wait(lambda s: s.w > 0, 20)
clear_notices(screen())
shot("start")
log = adb("logcat", "-d", "-t", "400")
check("the start: Songate knows it is the R4 (no opening)", "HiBy R4: the settings service is there" in log or "the settings service" in log, "")

# Sound & DAC: listed, its header, its rows
check("Sound & DAC is listed", open_cat("Sound & DAC"))
time.sleep(1)
shot("sound-dac")
s = screen()
heads = ("Headphone", "Line out", "Balanced", "Nothing plugged in")
check("the header says what plays through", any(t in heads for t, *_ in s.texts), [t for t, *_ in s.texts][:8])
flip("r4.gain", "gain", ["low", "high"])
flip("r4.filter", "filter", ["fast_rolloff", "slow_rolloff"])
flip("r4.logain", "lo_gain", ["low", "high"])

# line out asks first; answered no, nothing changes
if prop("out35") == "headphone":
    find("r4.out35[1]")
    tap_item("r4.out35[1]")
    time.sleep(1)
    check("line out asks first", screen().has_text("Switch to line out?"))
    adb("shell", "input", "keyevent", "BACK")
    time.sleep(1.2)
    check("answered no: still headphone", prop("out35") == "headphone", prop("out35"))

# Status LED: its own page; the mode, then (in Custom) a switch of each card
check("Status LED is listed", open_cat("Status LED"))
time.sleep(1)
shot("status-led")
s = find("r4.ledmode[0]", 6)
check("its LED mode row", "r4.ledmode[0]" in s.items)
mode = prop("led_mode")
modes = ["hiby", "off", "custom"]
if mode in modes:
    other = 1 if mode != "off" else 0
    tap_item(f"r4.ledmode[{other}]")
    check(f"led_mode → {modes[other]}", until(lambda: prop("led_mode") == modes[other]), prop("led_mode"))
    find(f"r4.ledmode[{modes.index(mode)}]", 6)
    tap_item(f"r4.ledmode[{modes.index(mode)}]")
    check(f"led_mode back to {mode}", until(lambda: prop("led_mode") == mode), prop("led_mode"))


def toggle(key, prop_key):
    start = prop(prop_key)
    if start not in ("0", "1"):
        check(f"{prop_key}: a known value", False, start)
        return
    want = "0" if start == "1" else "1"
    find(key)
    tap_item(key)
    check(f"{prop_key} → {want}", until(lambda: prop(prop_key) == want), prop(prop_key))
    find(key)
    tap_item(key)
    check(f"{prop_key} back to {start}", until(lambda: prop(prop_key) == start), prop(prop_key))


if prop("led_mode") == "custom":
    toggle("r4.ledbeat", "led_beat")
    toggle("r4.backcharge", "led_back_charge")
    flip("r4.back", "led_back", ["screen", "on", "off", "music"])
    find("r4.ledlow")
    time.sleep(0.8)
    shot("status-led-alerts")
else:
    print("note: LED mode is not Custom: the v11 cards are not shown, their switches not tried")

# Equalizer › Automatic preamp: the ROM's eq field 1 only; its manual preamp (field 2) and the bands untouched
eq0 = prop("eq")
if eq0.count(",") == 13:
    open_cat("Equalizer")
    s = find("r4.eqauto", 30)
    check("Automatic preamp is in Equalizer", "r4.eqauto" in s.items)
    f0 = eq0.split(",")
    tap_item("r4.eqauto")
    ok = until(lambda: prop("eq").split(",")[1] != f0[1])
    f1 = prop("eq").split(",")
    check("the switch changes the ROM's eq field 1", ok, ",".join(f1))
    check("... and nothing else of it", f1[:1] + f1[2:] == f0[:1] + f0[2:], f"{eq0} -> {','.join(f1)}")
    find("r4.eqauto", 6)
    tap_item("r4.eqauto")
    check("the switch back", until(lambda: prop("eq") == eq0), prop("eq"))
else:
    print("note: no sys.hibyr4.eq to read: Automatic preamp not tried")

# the R4's start: Appearance has no Opening card
open_cat("Appearance")
seen = False
for _ in range(30):
    if "set.opening" in screen().items:
        seen = True
        break
    swipe(500)
check("Appearance has no Opening card on the R4", not seen)

after = props()
changed = {k: (found[k], after[k]) for k in SETTINGS if found[k] != after[k]}
check("every setting as it was found", not changed, changed)
if not had_items:
    run_as("rm -f files/Songate/test-items")
print("ALL GOOD" if not fails else f"{len(fails)} FAILED: {fails}")
sys.exit(1 if fails else 0)
