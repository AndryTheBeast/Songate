"""Songate for Android, dev only: SongateOS phase 2 (session 21; the user's picks, 2026-11-16) on the emulator's simulated R4.
   python tools/android/devphase2.py [serial]          (default: emulator-5554; the dev APK installed first)
Checks, against the simulated service (mock-r4.txt):
- Connectivity: Wi-Fi's buttons one width (Forget, Connect, Open, Add); Bluetooth's two columns each one width
  (Disconnect/Connect; Forget/Look/Pair);
- the connected earbuds' card: battery, noise control, a mode change (shown, then the earbuds' answer), the codec and a change;
- the volume pop-up (held open: mock-ui.txt vol=hold): the noise line, a mode picked there;
- Sound & DAC while Bluetooth plays: the earbuds named with codec and rate, "Also connected", the gain's note, Plays on a switch;
- Appearance › Screen: Turns off after (Never among them), the brightness; Never chosen: Playback's Dim the screen can be set,
  Keep awake gone;
- Storage: On this player (the card ejected, then used again), USB (Files picked);
- About: This player.
The settings folder is set aside first and put back at the end. Ends with ALL GOOD or the steps that failed."""
import os
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import PKG, SERIAL, adb, results, run_as, screen, step, tap_at, tap_item, wait   # noqa: E402

stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devphase2-{stamp}"
print(f"devphase2 on {SERIAL}, {stamp}", flush=True)


def has(s, t):
    return s is not None and any(x[0] == t for x in s.texts)


def has_in(s, t):
    return s is not None and any(t in x[0] for x in s.texts)


def scroll(down=True, a=0.75, b=0.45):
    s = screen()
    X = int(12 * s.scale)
    if not down:
        a, b = b, a
    adb("shell", "input", "swipe", str(X), str(int(s.h * a * s.scale)), str(X), str(int(s.h * b * s.scale)), "400")
    time.sleep(1.2)


def top():
    for _ in range(6):
        scroll(False, 0.75, 0.3)


def find_scroll(cond, tries=8):
    """Scrolled down until cond(screen) holds"""
    for _ in range(tries):
        s = screen()
        if cond(s):
            return s
        scroll()
    return None


def tap_text(t0, tries=8):
    for _ in range(tries):
        s = screen()
        hit = next((t for t in s.texts if t[0] == t0), None)
        if hit and s.h * 0.1 < hit[2] < s.h - 170:
            tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
            time.sleep(1.3)
            return True
        scroll()
    return False


def kicker(word, tries=8):
    """A card's title (drawn letter by letter): tapped, scrolled to first"""
    for _ in range(tries):
        s = screen()
        lines = {}
        for t, x, y, w, h in s.texts:
            if len(t) == 1:
                lines.setdefault(round(y), []).append((x, t, h))
        for y, v in lines.items():
            v.sort()
            if "".join(c for _, c, _ in v).strip() == word and s.h * 0.1 < y < s.h - 170:
                tap_at(s, v[0][0] + 20, y + v[0][2] / 2)
                time.sleep(1.3)
                return True
        scroll()
    return False


def widths(s, prefix):
    return sorted({v[2] for k, v in s.items.items() if k.startswith(prefix)})


def page(name):
    tap_item("ph.nav#4")
    time.sleep(1.8)
    top()
    return tap_text(name)


def restart(mock="", where=0):
    adb("shell", "am", "force-stop", PKG)
    run_as(f'printf "{mock}" > files/Songate/mock-ui.txt; rm -f files/Songate/items.txt files/Songate/start-guard.txt; '
           'touch files/Songate/mock-r4.txt files/Songate/test-items; sed -i "/^foldOpen=/d; /^volWhere=/d" files/Songate/settings.txt; '
           f'echo volWhere={where} >> files/Songate/settings.txt')
    adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
    wait(lambda s: "ph.nav#4" in s.items, 30)
    time.sleep(3)


adb("shell", "input", "keyevent", "224")
adb("shell", "svc", "power", "stayon", "true")
adb("shell", "settings", "put", "system", "user_rotation", "0")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}")
try:
    restart()
    # ---- Connectivity: one width for each column of buttons
    page("Connectivity")
    tap_item("sys.conntab[0]")
    time.sleep(2)
    s = find_scroll(lambda s: any(k.startswith("sys.wifi.hidden") for k in s.items))
    allw = widths(screen(), "sys.wifi.") if s else []
    step("Wi-Fi: Forget, Connect, Open and Add one width", s is not None and len(allw) == 1, str(allw))
    top()
    tap_item("sys.conntab[1]")
    time.sleep(3)
    # (three titled cards now, the earbuds' first: the page folds them, one open at a time)
    kicker("PAIRED DEVICES")
    s = find_scroll(lambda s: any(k.startswith("sys.bt.f") for k in s.items))
    c1 = widths(s, "sys.bt.c#") if s else []   # (Connect/Disconnect: not sys.bt.codec, the codec choice; session 23)
    c2a = widths(s, "sys.bt.f") if s else []
    top()
    kicker("NEW DEVICES")
    s = find_scroll(lambda s: any(k.startswith("sys.bt.look") for k in s.items))
    c2 = sorted(set(c2a + (widths(s, "sys.bt.look") + widths(s, "sys.bt.p") if s else [])))
    step("Bluetooth: Disconnect/Connect one width, Forget/Look/Pair one width", len(c1) == 1 and len(c2) == 1, f"{c1} {c2}")
    # ---- the earbuds' card (values in fixed-width digits: not one text in items.txt)
    top()
    kicker("LIBERTY 4 NC")
    top()
    s = wait(lambda s: has(s, "Noise control"), 8)
    step("Bluetooth: the earbuds' card (battery, noise control)", has(s, "Left") and has(s, "Noise control") and has(s, "Wind reduction"))
    tap_item("sys.eb.mode[1]")
    s = wait(lambda s: has(s, "Voices"), 4)
    step("... Transparency picked: its own choice (Full, Voices) at once", s is not None)
    time.sleep(2)
    s = find_scroll(lambda s: has(s, "Codec"))
    step("... the codec: AAC and SBC, its rate", has(s, "Codec") and has_in(s, "44.1 kHz · 16-bit"))
    tap_item("sys.bt.codec[1]")
    time.sleep(2.5)
    s = screen()
    step("... SBC picked: the service's answer shows it", "sys.bt.codec[1]" in s.items)
    tap_item("sys.bt.codec[0]")   # (back to AAC)
    time.sleep(2)
    # ---- Sound & DAC while Bluetooth plays
    page("Sound & DAC")
    s = wait(lambda s: has_in(s, "Bluetooth · AAC"), 8)
    step("Sound & DAC: the earbuds named, Bluetooth · AAC · 44.1 kHz / 16-bit", s is not None and has(s, "Soundcore Liberty 4 NC") and
         has_in(s, "44.1 kHz / 16-bit"))
    step("... Also connected: the wired output", has_in(s, "Also connected: Headphones 4.4 mm"))
    step("... Gain: Not used while Bluetooth plays", has(s, "Not used while Bluetooth plays"))
    kicker("OUTPUT")
    s = wait(lambda s: "r4.playson[0]" in s.items, 5)
    step("... Plays on: a switch between the two", s is not None and "r4.playson[1]" in s.items)
    # ---- the volume pop-up's noise line (Bottom, held open: the page's tabs stay clear of it; the simulated earbuds live in this
    # run only, so the change is read back without a restart)
    restart("vol=hold", 2)
    adb("shell", "input", "keyevent", "24")
    s = wait(lambda s: has(s, "Transparency") and has(s, "Normal"), 10)
    step("Volume pop-up: the noise line (Cancelling, Transparency, Normal)", s is not None and has(s, "Cancelling"))
    if s:
        hit = next(t for t in s.texts if t[0] == "Normal")
        tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
    time.sleep(2.5)
    page("Connectivity")
    tap_item("sys.conntab[1]")
    s = wait(lambda s: "sys.eb.mode[2]" in s.items, 8)
    step("... Normal picked there: the earbuds' card says Normal (no Cancelling or Transparency row)", s is not None and
         not has(s, "Adaptive") and not has(s, "Voices"))
    # the Side pop-up: its noise round opens a card of the modes, as the output's round opens its list (the user, 2026-11-16)
    restart("vol=hold", 1)
    adb("shell", "input", "keyevent", "24")
    s = wait(lambda s: "vol.noise#9" in s.items, 10)
    step("Side pop-up: the noise round", s is not None)
    tap_item("vol.noise#9")
    s = wait(lambda s: "vol.noiserow#1" in s.items, 5)
    step("... its card: the modes (Cancelling, Transparency, Normal)", s is not None and has(s, "Transparency") and has(s, "Normal"))
    tap_item("vol.noiserow#1")
    s = wait(lambda s: "vol.noiserow#1" not in s.items, 5)
    step("... Transparency picked: the card closes", s is not None)
    restart()
    # ---- Appearance › Screen, and Playback
    page("Appearance")
    kicker("SCREEN")
    s = wait(lambda s: has(s, "Turns off after"), 5)
    step("Appearance › Screen: Turns off after first, Never among them, the brightness", s is not None and has(s, "Never") and
         has(s, "Screen brightness"))
    n = next((k for k in (s.items if s else {}) if k.startswith("sys.timeout[") and k.endswith("[7]")), None)
    tap_item(n or "sys.timeout[7]")
    time.sleep(2)
    page("Playback")
    kicker("SLEEP")
    s = find_scroll(lambda s: has(s, "Dim the screen"))
    step("Playback: Keep awake gone; Dim the screen can be set (Never chosen)", s is not None and not has_in(s, "awake") and
         has(s, "After this long without input; any button brings it back"))
    page("Appearance")
    kicker("SCREEN")
    tap_item("sys.timeout[3]")   # (back to 2 min)
    time.sleep(1.5)
    # ---- Storage and USB
    page("Storage")
    s = wait(lambda s: has(s, "MUSIC"), 6)
    step("Storage: On this player (internal, the card and their room)", s is not None and has(s, "Internal storage") and has_in(s, "free of"))
    tap_item("sys.vol#1")
    s = wait(lambda s: has_in(s, "Ejected"), 10)
    step("... the card ejected", s is not None)
    tap_item("sys.vol#1")
    s = wait(lambda s: has_in(s, "free of 134 GB"), 10)
    step("... and used again", s is not None)
    kicker("USB")
    s = wait(lambda s: has(s, "Use it for"), 5)
    step("... USB: Computer, Use it for (Charging, Files, Photos, MIDI)", s is not None and has(s, "Computer") and has(s, "MIDI"))
    tap_item("sys.usb[1]")
    time.sleep(1.5)
    step("... Files picked", "sys.usb[1]" in screen().items)
    tap_item("sys.usb[0]")
    time.sleep(1)
    # ---- About
    page("About")
    s = find_scroll(lambda s: has(s, "Memory"))
    step("About: This player (SongateOS, Android, the service, memory, battery)", s is not None and has(s, "Settings service") and
         has(s, "Battery") and has_in(s, "Charging"))
finally:
    adb("shell", "svc", "power", "stayon", "false")
    adb("shell", "am", "force-stop", PKG)
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && "
                 "rm -f files/Songate/test-items files/Songate/items.txt; ls files/Songate/settings.txt")
    print("settings folder put back:", "yes" if "settings.txt" in out else "NO", flush=True)

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
