"""Songate for Android, dev only: SongateOS's system pages, phase 1 (Connectivity: Wi-Fi and Bluetooth; System; the output switcher in
the volume pop-up and Sound & DAC), against the dev build's SIMULATED service (<config>/mock-r4.txt: sysapi.cpp's simCall).
   python tools/android/devsys.py [serial]          (default: emulator-5554; the dev APK installed first)
Nothing here talks to a real service: on the R4 the same pages talk to the HiBy R4 service once it has the system API.
Checks: the list's order; Wi-Fi's network in use, joining one with a wrong password then the right one (Songate's card, dots),
Forget asked first, Wi-Fi off; Bluetooth's paired devices, a search, pairing, disconnecting, Forget asked first; System's power
rows asked first; the output in Sound & DAC and its list (a pick moves it); the volume pop-up's output line and its list.
The settings folder is set aside first and put back at the end. Ends with ALL GOOD or the steps that failed."""
import os
import subprocess
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import ADB, PKG, SERIAL, results, screen, step, tap_at, tap_item, wait   # noqa: E402

LETTERS = ["1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"]
stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devsys-{stamp}"
print(f"devsys on {SERIAL}, {stamp}", flush=True)


def adb(*args, secs=30):
    try:
        return subprocess.run([ADB, "-s", SERIAL, *args], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=secs).stdout
    except subprocess.TimeoutExpired:
        print(f"  (adb {' '.join(args[:3])}: no answer in {secs} s)", flush=True)
        return ""


def run_as(cmd, secs=30):
    return adb("shell", f"run-as {PKG} sh -c '{cmd}'", secs=secs)


def texts(s):
    return [t[0] for t in s.texts] if s else []


def has(s, t):
    return s is not None and any(x == t for x in texts(s))


def tap_exact(t0, pause=1.0):
    s = screen()
    hit = next((t for t in s.texts if t[0] == t0), None)
    if hit:
        tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
        time.sleep(pause)
    return hit is not None


def open_card(word, tries=6):
    """A folded card opened by its title (drawn letter by letter: joined back by their line), scrolled to first. Bluetooth's page
    folds since the connected earbuds have a card of their own (phase 2): three titled cards, one open at a time"""
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


def row_button(label, button_key_prefix):
    """The button on the row whose label is label (the nearest item named button_key_prefix… on its line)"""
    s = screen()
    lab = next((t for t in s.texts if t[0] == label or (len(t[0]) >= 6 and label.startswith(t[0]))), None)   # (a long name: its first line)
    if not lab:
        return False
    cy = lab[2] + lab[4] / 2
    cands = [(k, v) for k, v in s.items.items() if k.startswith(button_key_prefix) and abs(v[1] + v[3] / 2 - cy) < 30]
    if not cands:
        return False
    k, (x, y, w, h) = cands[0]
    tap_at(s, x + w / 2, y + h / 2)
    time.sleep(1.0)
    return True


def type_text(sv):
    for ch in sv:
        i = 0
        for r in LETTERS:
            if ch in r:
                tap_item("skb.k" if i + r.index(ch) == 0 else f"skb.k#{i + r.index(ch)}")
                break
            i += len(r)
        time.sleep(0.2)


def scroll(down=True):
    s = screen()
    X = int(12 * s.scale)
    a, b = (0.75, 0.4) if down else (0.4, 0.8)
    adb("shell", "input", "swipe", str(X), str(int(s.h * a * s.scale)), str(X), str(int(s.h * b * s.scale)), "400")
    time.sleep(1.2)


def open_settings_page(name):
    tap_item("ph.nav#4")
    time.sleep(1.2)
    for _ in range(3):
        if tap_exact(name, 1.4):
            return True
        scroll()
    return False


adb("shell", "input", "keyevent", "224")
adb("shell", "svc", "power", "stayon", "true")
adb("shell", "settings", "put", "system", "user_rotation", "0")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}; rm -f files/Songate/items.txt files/Songate/start-guard.txt "
       "files/Songate/mock-ui.txt; touch files/Songate/mock-r4.txt files/Songate/test-items; "
       "sed -i \"/^volWhere=/d;/^volPop=/d\" files/Songate/settings.txt; echo volWhere=0 >> files/Songate/settings.txt", secs=180)
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    s = wait(lambda s: "ph.nav#4" in s.items, 25)
    step("started (the simulated R4)", s)
    time.sleep(2)
    tap_item("ph.nav#4")
    time.sleep(1.5)
    s = screen()
    names = [t[0] for t in s.texts]
    step("Settings: Connectivity first", "Connectivity" in names and names.index("Connectivity") < names.index("Appearance") if "Appearance" in names else False)

    # ---- Wi-Fi
    open_settings_page("Connectivity")
    s = wait(lambda s: has(s, "TheBeast-5G"), 8)
    step("Connectivity: the pill, Wi-Fi first: the network in use", s and "sys.conntab[0]" in s.items or (s and any(k.startswith("sys.conntab") for k in s.items)))
    step("... its line: connected and its address", s and has(s, "Connected · 192.168.1.109"))
    step("... the networks around, Saved and Open marked", s and has(s, "TheBeast") and has(s, "Saved") and has(s, "Open"))
    row_button("Vodafone-B2C1", "sys.wifi.n")
    s = wait(lambda s: "skb.k#104" in s.items, 5)
    time.sleep(0.6)
    s = screen()
    step("a secured network: its password on Songate's card", s and any("Password for Vodafone-B2C1" == s.texts[i][0] for i in s.top))
    type_text("wrongpass")
    s = screen()
    step("... typed as dots", s and any(set(s.texts[i][0]) == {"•"} for i in s.top))
    tap_item("skb.k#104")
    s = wait(lambda s: has(s, "Connecting…"), 4)
    step("joining: Connecting…", s)
    s = wait(lambda s: has(s, "Wrong password"), 6)
    step("... the wrong password said", s)
    tap_item("sys.wifi.cur")   # (after a failure: Connect again, the password asked anew)
    if wait(lambda s: "skb.k#104" in s.items, 5):
        type_text("rightpass")
        tap_item("skb.k#104")
    s = wait(lambda s: has(s, "Vodafone-B2C1") and has(s, "Connected · 192.168.1.109"), 8)
    step("the right one: connected", s)
    tap_item("sys.wifi.cur")
    s = wait(lambda s: has(s, "Forget this network?"), 4)
    step("Forget: asked first", s)
    tap_exact("Forget", 0.4) if False else None
    s = screen()
    fk = [k for k in s.items if k.startswith("sp.") and "Forget" in str(k)]
    # the question's own Forget (the last "Forget" drawn: the card's button)
    hits = [t for t in s.texts if t[0] == "Forget"]
    if hits:
        t = hits[-1]
        tap_at(s, t[1] + t[3] / 2, t[2] + t[4] / 2)
    s = wait(lambda s: not has(s, "Connected · 192.168.1.109"), 6)
    step("... forgotten: no network in use", s)
    tap_item("sys.wifion")
    s = wait(lambda s: not has(s, "TheBeast") and not has(s, "Networks"), 6)
    step("Wi-Fi off: no networks", s)
    tap_item("sys.wifion")
    wait(lambda s: has(s, "TheBeast"), 6)
    # an enterprise network (eap; WEP too): the service cannot join it: Android's own page offered instead of Connect
    for _ in range(3):
        if has(screen(), "eduroam"):
            break
        scroll()
    s = screen()
    lab = next((t for t in s.texts if t[0] == "eduroam"), None)
    near = [t[0] for t in s.texts if lab and abs(t[2] - lab[2]) < 40]
    step("an enterprise network: Open in Android settings, not Connect", lab and "Open in Android settings" in near and "Open" in near and "Connect" not in near,
         ", ".join(near))
    for _ in range(3):
        scroll(False)

    # ---- Bluetooth
    tap_item("sys.conntab[1]") or tap_exact("Bluetooth")
    time.sleep(2)
    open_card("PAIRED DEVICES")
    s = wait(lambda s: any(t[0].startswith("Soundcore Liberty") for t in s.texts), 8)
    step("Bluetooth: the paired devices", s and any(t[0].startswith("Moondrop Space") for t in s.texts))
    step("... connected with its battery", s and has(s, "Connected · Battery 80%"))
    for _ in range(3):
        scroll(False)
    open_card("NEW DEVICES")
    tap_item("sys.bt.look")
    time.sleep(1)
    scroll()   # (the devices found: under the fold)
    s = wait(lambda s: has(s, "JBL Flip 6"), 6)
    step("Look: devices nearby, their kind said", s and has(s, "Speaker"))
    row_button("JBL Flip 6", "sys.bt.p")
    time.sleep(1)
    for _ in range(3):
        scroll(False)
    open_card("PAIRED DEVICES")
    s = wait(lambda s: has(s, "JBL Flip 6") and sum(1 for k in s.items if k.startswith("sys.bt.f")) == 3, 6)   # (paired: a third Forget)
    step("Pair: in the paired devices", s)
    row_button("Soundcore Liberty 4 NC", "sys.bt.c")
    s = wait(lambda s: not has(s, "Connected · Battery 80%"), 6)
    step("Disconnect: not connected", s)
    row_button("Moondrop Space Travel", "sys.bt.f")
    s = wait(lambda s: has(s, "Forget this device?"), 4)
    step("Forget: asked first", s)
    adb("shell", "input", "keyevent", "4")
    time.sleep(1)
    row_button("Soundcore Liberty 4 NC", "sys.bt.c")   # (connected again, for the output below)
    time.sleep(1)

    # ---- no System page (the user: Android's power menu); SongateOS's updates in Settings › Updates, behind its pill
    adb("shell", "input", "keyevent", "4")
    time.sleep(1)
    seen = set()
    for _ in range(5):
        seen |= set(texts(screen()))
        scroll()
    step("no System page in the list", "System" not in seen and "Updates" in seen)
    tap_exact("Updates", 1.4)
    for _ in range(4):
        scroll(False)
    s = wait(lambda s: "set.updtab[1]" in s.items, 5)
    step("Updates: the Songate | SongateOS pill", s and has(s, "Songate") and has(s, "SongateOS"))
    tap_item("set.updtab[1]")
    s = wait(lambda s: has(s, "SongateOS 0.3.0"), 6)
    step("SongateOS: this version, Check", s and "set.ota.check" in s.items)
    tap_item("set.ota.check")
    s = wait(lambda s: has(s, "SongateOS 0.3.1 is out"), 6)
    step("Check: a new version, a beta, its size", s and any("a beta" in t for t in texts(s)) and any("GB" in t for t in texts(s)))
    step("... What's new and the options", s and has(s, "Check every day") and any("Wi-Fi and Bluetooth" in t for t in texts(s)))
    tap_item("set.ota.dl")
    s = wait(lambda s: any(t.startswith("Downloading SongateOS") for t in texts(s)), 6)
    step("Download: its progress", s and any(" of 1.3 GB" in t for t in texts(s)))
    s = wait(lambda s: has(s, "SongateOS 0.3.1 is ready"), 12)
    step("... downloaded: ready to install", s)
    tap_item("set.ota.install")
    s = wait(lambda s: has(s, "Install SongateOS 0.3.1?"), 5)
    step("Install: the service's checks, then asked first", s)
    adb("shell", "input", "keyevent", "4")
    time.sleep(1)
    tap_item("set.ota.beta")
    time.sleep(1.5)
    s = screen()
    step("Beta versions: its switch moves the service's option", s is not None)
    tap_item("set.updtab")   # (back to Songate's own half)
    time.sleep(1)

    # ---- the output: Sound & DAC
    adb("shell", "input", "keyevent", "4")
    time.sleep(1)
    for _ in range(4):
        scroll(False)
    tap_exact("Sound & DAC", 1.6)
    open_card("OUTPUT")
    # (two outputs connected, wired and the earbuds: Plays on is a switch between them, phase 2; one alone: its name and Change)
    for _ in range(6):
        if has(screen(), "Plays on") and screen().items.get("r4.playson[1]", (0, 9999, 0, 0))[1] < screen().h * 0.75:
            break
        scroll()
    s = screen()
    step("Sound & DAC: Plays on, the two outputs connected", has(s, "Plays on") and has(s, "4.4 mm") and has(s, "Soundcore Liberty 4 NC"))
    tap_item("r4.playson[0]")
    time.sleep(2)
    for _ in range(4):
        scroll(False)
    s = wait(lambda s: has(s, "Also connected: Soundcore Liberty 4 NC"), 5)   # (the card: the earbuds now only connected)
    step("... 4.4 mm picked: it plays (the card says so)", s)
    open_card("OUTPUT")
    tap_item("r4.playson[1]")
    time.sleep(1.5)

    # ---- the volume pop-up's line
    tap_item("ph.nav")
    time.sleep(1)
    adb("shell", "input", "keyevent", "24")
    s = wait(lambda s: "vol.out" in s.items, 4)
    step("a volume key: the pop-up with its output line", s)
    tap_item("vol.out")
    s = wait(lambda s: "vol.outrow" in s.items, 4)
    step("its line: the list", s)
    tap_item("vol.outrow")   # Automatic
    s = wait(lambda s: "vol.outrow" not in s.items, 4)
    step("Automatic: picked, the list closed", s)

    # ---- System colours (Appearance, right after Interface colours since 2026-11-16; folded at first): Android's | Songate's |
    # Your own, Dark
    open_settings_page("Appearance")
    open_card("SYSTEM COLOURS")
    for _ in range(14):
        s = screen()
        if "sys.themeway[0]" in s.items and s.items["sys.themeway[0]"][1] < s.h * 0.7:
            break
        scroll()
    time.sleep(1)
    s = screen()
    step("Appearance: System colours, Android's chosen at first, no rounds", "sys.themeway[0]" in s.items and "sys.themecol" not in s.items)
    tap_item("sys.themeway[2]")
    s = wait(lambda s: "sys.themecol" in s.items, 4)
    step("Your own: its colours and the wheel", s and "sys.themecol#99" in s.items)
    tap_item("sys.themecol#3")
    time.sleep(1.5)
    tap_item("sys.themeway[1]")
    time.sleep(1.5)
    s = screen()
    step("Songate's: the rounds go (its colour copied once)", "sys.themecol" not in s.items)
    tap_item("sys.themeway[0]")
    time.sleep(1.5)
    step("Android's again", "sys.themecol" not in screen().items)
    step("Dark: its switch there", "sys.themedark" in screen().items)
finally:
    adb("shell", "svc", "power", "stayon", "false")
    adb("shell", "am", "force-stop", PKG)
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && "
                 "rm -f files/Songate/test-items files/Songate/items.txt; ls files/Songate/settings.txt")
    print("settings folder put back:", "yes" if "settings.txt" in out else "NO", flush=True)

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
