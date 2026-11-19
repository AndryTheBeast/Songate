"""Songate for Android, dev only: Songate's own keyboard card for names and numbers (session 21; the user chose N1, E2 upright, S1).
   python tools/android/devkb.py [serial]          (default: emulator-5554; the dev APK installed first)
Checks: Search's card still says Search; New playlist asks its name on the card (title, "Done"), Done makes it, a tap above makes
nothing; an EQ band held: upright the number pad (the band follows each key, nothing past ±12, a tap above puts the EQ back);
sideways the whole-width card on its symbols page; Connect's address on the symbols page; the server's password as dots (when the
Sign in row is there). The settings folder is copied aside first and put back at the end (the test's own folder deleted).
Ends with ALL GOOD or the steps that failed."""
import os
import subprocess
import re
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import *   # noqa: E402,F403

LETTERS = ["1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"]
SYMBOLS = ["1234567890", "-/:;()&@'\"", ".,?!#%+=*", "_$~<>[]"]
stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devkb-{stamp}"
print(f"devkb on {SERIAL}, {stamp}")


def key_idx(ch, rows):
    i = 0
    for r in rows:
        if ch in r:
            return i + r.index(ch)
        i += len(r)
    return None


def key(idx):
    return tap_item("skb.k" if idx == 0 else f"skb.k#{idx}")


def type_text(s, rows=LETTERS):
    for ch in s:
        key(103 if ch == " " else key_idx(ch, rows))
        time.sleep(0.25)


def card_texts(s):
    """The words drawn over the page (the card's title, field and keys)"""
    return [s.texts[i][0] for i in s.top] if s else []


def field(s):
    """The card's field: the drawn line right under the title (the TOP text between the title and the first key row)"""
    if not s or "skb.k#104" not in s.items:
        return None
    ky = min(v[1] for k, v in s.items.items() if k.startswith("skb.k"))
    cand = [s.texts[i] for i in s.top if s.texts[i][2] < ky and s.texts[i][2] > ky - 60]
    return cand[0][0] if cand else ""


def hold(name):
    s = screen()
    x, y, w, h = s.items[name]
    X, Y = int((x + w / 2) * s.scale), int((y + h / 2) * s.scale)
    adb("shell", "input", "swipe", str(X), str(Y), str(X), str(Y), "900")


def rotate(r):
    adb("shell", "settings", "put", "system", "accelerometer_rotation", "0")
    adb("shell", "settings", "put", "system", "user_rotation", str(r))
    return wait(lambda s: s.w > 0 and (s.w > s.h) == (r == 1), 10)


def setting(name):
    return re.search(rf"^{name}=(.*)$", run_as("cat files/Songate/settings.txt"), re.M)


def saved_eq():
    adb("shell", "input", "keyevent", "3")   # Home: Songate saves what changed
    time.sleep(2.5)
    m = setting("eqBands")
    adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
    wait(lambda s: s.w > 0, 10)
    time.sleep(1)
    return m[1] if m else None


def open_eq():
    s = screen()
    if "set.eqval#3" not in s.items:
        if "ph.eq" not in s.items and "pb.eq" not in s.items:   # (Now playing first: its EQ button)
            tap_item("ph.nav") or tap_text("Now playing")
            s = wait(lambda s: "ph.eq" in s.items or "pb.eq" in s.items, 6) or screen()
        hold("ph.eq" if "ph.eq" in s.items else "pb.eq")   # (a tap turns the EQ on or off: held, its page opens)
    s = wait(lambda s: "set.eqval#3" in s.items, 8)
    side = s and s.w > s.h
    for _ in range(8):   # the band values up out of the card's way
        s = screen()
        if "set.eqval#3" not in s.items or s.items["set.eqval#3"][1] < s.h * (0.2 if side else 0.42):
            break
        X = int((s.w - 8 if side else 12) * s.scale)
        adb("shell", "input", "swipe", str(X), str(int(s.h * 0.8 * s.scale)), str(X), str(int(s.h * 0.45 * s.scale)), "500")
        time.sleep(1.2)
    return screen()


adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}; rm -f files/Songate/mock-ui.txt files/Songate/mock-kbopen.txt files/Songate/items.txt; "
       # (signed out in the test's copy only: the Sign in row and its three steps; cancelled at the password, nothing sent)
       "rm -f files/Songate/subsonic-account.txt; "
       # (Devices on in the test's copy: Connect's Can't see it? row)
       "sed -i s/^connections=0/connections=1/ files/Songate/settings.txt; "
       # (Reports on in the test's copy: the report form to type in; it is closed unsent)
       "sed -i s/^reports=0/reports=1/ files/Songate/settings.txt; touch files/Songate/test-items")
rotate(0)
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    s = wait(lambda s: "ph.nav#2" in s.items, 20)
    step("started upright", s)

    # ---- Search: its card as before
    tap_item("ph.nav#2")
    time.sleep(1)
    if "skb.k#104" not in screen().items:
        tap_text("Songs, Albums, Artists")
    s = wait(lambda s: "skb.k#104" in s.items, 5)
    time.sleep(0.8)   # (its keys' words drawn the frames after it opens)
    s = screen()
    step("Search: the card, its blue key says Search", "skb.k#104" in s.items and "Search" in card_texts(s))
    tap_item("skb.away")
    time.sleep(0.8)

    # ---- New playlist: a name on the card
    tap_item("ph.nav#3")
    s = wait(lambda s: s.has_text("New playlist"), 6)
    before = len(re.findall(r"^\S", run_as("cat files/Songate/playlists.txt 2>/dev/null"), re.M))
    tap_text("New playlist")
    time.sleep(1.2)
    tap_text("A playlist of your own")
    s = wait(lambda s: "skb.k#104" in s.items, 5)
    time.sleep(0.8)   # (the menu under it closes the frame after)
    s = screen()
    step("New playlist: the card, titled New playlist", s and "New playlist" in card_texts(s))
    step("... its blue key says Done (N1)", s and "Done" in card_texts(s))
    f0 = field(s) or ""
    step("... the field holds the name offered", f0.startswith("Playlist "), f0)
    type_text(" kb")
    s = screen()
    step("... typed letters join the name", (field(s) or "").endswith(" kb"), field(s))
    key(104)
    s = wait(lambda s: "skb.k#104" not in s.items and s.has_text(" kb"), 6)
    step("Done: the playlist made under that name", s)
    name_made = f0 + " kb"
    tap_item("ph.nav#3")
    time.sleep(1)
    tap_text("New playlist")
    time.sleep(1.2)
    tap_text("A playlist of your own")
    s = wait(lambda s: "skb.k#104" in s.items, 5)
    type_text("zz")
    tap_item("skb.away")
    s = wait(lambda s: "skb.k#104" not in s.items, 5)
    time.sleep(1)
    step("a tap above: closed, nothing made", s and not screen().has_text("zz"))

    # ---- the EQ, upright: the number pad
    tap_item("ph.nav")
    time.sleep(1)
    eq0 = saved_eq()
    s = open_eq()
    step("the Equalizer's band values", "set.eqval#3" in s.items)
    hold("set.eqval#3")
    s = wait(lambda s: "skb.k#104" in s.items, 5)
    step("held a band upright: the number pad (a minus key, no letters)", s and "skb.k#9" in s.items and "skb.k#20" not in s.items and "skb.k#102" not in s.items)
    step("... titled by the band, dB beside the value", s and any("in dB" in t for t in card_texts(s)) and "dB" in card_texts(s))
    key(9); key(2)   # − 3
    time.sleep(0.6)
    s = screen()
    step("... − 3 typed (the value first shown is replaced)", field(s) == "−3", field(s))
    key(101); key(101); key(0); key(4)   # back to nothing, then 1 5: 15 is past +12
    s = screen()
    step("... nothing past +12 (1 then 5 stays 1)", field(s) == "1", field(s))
    tap_item("skb.away")
    wait(lambda s: "skb.k#104" not in s.items, 5)
    step("a tap above: the EQ as it was", saved_eq() == eq0, eq0)
    s = open_eq()
    hold("set.eqval#3")
    wait(lambda s: "skb.k#104" in s.items, 5)
    key(9); key(3)   # − 4
    key(104)
    wait(lambda s: "skb.k#104" not in s.items, 5)
    eq1 = saved_eq()
    step("Done: the band at −4", eq1 and eq1.split()[3] == "-4.0", eq1)

    # ---- sideways: the whole-width card on its symbols page
    s = rotate(1)
    step("turned sideways", s)
    s = open_eq()
    hold("set.eqval#3")
    s = wait(lambda s: "skb.k#104" in s.items, 5)
    step("held a band sideways: the whole-width card", s and "skb.k#102" in s.items and "skb.k#103" in s.items)
    step("... on its symbols page", s and "/" in card_texts(s) and "q" not in card_texts(s))
    key(key_idx("-", SYMBOLS)); key(key_idx("2", SYMBOLS)); key(103); key(key_idx("#", SYMBOLS))   # − 2, a space and # do nothing
    s = screen()
    step("... − 2 typed, the space and # ignored", field(s) == "−2", field(s))
    key(104)
    wait(lambda s: "skb.k#104" not in s.items, 5)
    eq2 = saved_eq()
    step("Done sideways: the band at −2", eq2 and eq2.split()[3] == "-2.0", eq2)
    # the band as it was before the test (typed back, so the preset state stays the app's own)
    s = open_eq()
    hold("set.eqval#3")
    wait(lambda s: "skb.k#104" in s.items, 5)
    want = int(float(eq0.split()[3])) if eq0 else 0
    for ch in str(want).replace("-", "-"):
        key(key_idx(ch, SYMBOLS))
    key(104)
    wait(lambda s: "skb.k#104" not in s.items, 5)
    rotate(0)

    # ---- Connect's address: the symbols page; the server's password: dots
    def settings_row(category, item):
        tap_item("ph.nav#4")
        time.sleep(1)
        for _ in range(3):   # the list of categories from its top
            s = screen()
            X = int(12 * s.scale)
            adb("shell", "input", "swipe", str(X), str(int(s.h * 0.3 * s.scale)), str(X), str(int(s.h * 0.85 * s.scale)), "300")
            time.sleep(0.6)
        time.sleep(1)
        for _ in range(10):   # ... down to this one
            s = screen()
            if any(t[0] == category and t[2] < s.h * 0.8 for t in s.texts):
                break
            X = int(12 * s.scale)
            adb("shell", "input", "swipe", str(X), str(int(s.h * 0.8 * s.scale)), str(X), str(int(s.h * 0.4 * s.scale)), "400")
            time.sleep(1.2)
        time.sleep(1)
        s = screen()
        hit = next((t for t in s.texts if t[0] == category), None)
        if hit:
            tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
        time.sleep(1)
        for _ in range(20):
            s = screen()
            if item in s.items and s.items[item][1] > s.h * 0.12 and s.items[item][1] + s.items[item][3] < s.h - 150:   # (clear of the title and the mini player)
                time.sleep(1.5)   # (the list still gliding: a tap now only stops it)
                return screen()
            X = int(12 * s.scale)
            adb("shell", "input", "swipe", str(X), str(int(s.h * 0.8 * s.scale)), str(X), str(int(s.h * 0.4 * s.scale)), "400")
            time.sleep(0.8)
        return None

    s = settings_row("Devices", "set.cnaim")
    if s:
        tap_item("set.cnaim")
        s = wait(lambda s: "skb.k#104" in s.items, 5)
        step("Connect's address: the card on its symbols page", s and "/" in card_texts(s) and "skb.k#102" in s.items)
        tap_item("skb.away")
        wait(lambda s: "skb.k#104" not in s.items, 5)
    else:
        step("Connect's Can't see it? row", False, "not found")
    s = settings_row("Library", "set.subin")
    if s:
        tap_item("set.subin")
        s = wait(lambda s: "skb.k#104" in s.items, 5)
        step("server sign-in: its address on the card", s and any("Its address" in t for t in card_texts(s)))
        type_text("x")   # (an address that goes nowhere: the steps are cancelled at the password, nothing is sent)
        key(104)
        s = wait(lambda s: any("Your username" in t for t in card_texts(s)), 5)
        step("... then the username", s)
        type_text("u")
        key(104)
        s = wait(lambda s: any("Your password" in t for t in card_texts(s)), 5)
        step("... then the password", s)
        type_text("abc")
        s = screen()
        step("... the password drawn as dots, never its letters", field(s) == "•••" and not any(t[0] == "abc" for t in s.texts), field(s))
        tap_item("skb.away")
        wait(lambda s: "skb.k#104" not in s.items, 5)
    else:
        step("server sign-in: the Sign in row", False, "not found")

    # ---- the support report (the user, 2026-11-16: "everything that happens in songate should have the songate keyboard"):
    # its title on the card, its sentences in a field that wraps; the form closed after, nothing sent
    s = settings_row("Support", "set.repbug")
    if s:
        tap_item("set.repbug")
        time.sleep(1.2)
        tap_text("Add a title")
        s = wait(lambda s: "skb.k#104" in s.items, 5)
        time.sleep(0.6)
        s = screen()
        step("report: the title on the card", s and "A short title" in card_texts(s))
        type_text("skip")
        key(104)
        s = wait(lambda s: "skb.k#104" not in s.items and s.has_text("skip"), 5)
        step("... Done: the title in the form", s)
        tap_text("Tell us")
        s = wait(lambda s: "skb.k#104" in s.items, 5)
        time.sleep(0.6)
        s = screen()
        step("report: what happened, on the card", s and "What happened, and what you expected" in card_texts(s))
        type_text("the song stops when i skip twice and the next one waits")
        time.sleep(0.6)
        s = screen()
        ky = min(v[1] for k, v in s.items.items() if k.startswith("skb.k"))
        lines = [s.texts[i][0] for i in s.top if ky - 120 < s.texts[i][2] < ky and s.texts[i][0] not in ("What happened, and what you expected",)]
        step("... the sentence wrapped over lines in the field", len(lines) >= 2, " | ".join(lines))
        subprocess.run([ADB, "-s", SERIAL, "shell", "screencap", "-p", "/sdcard/devkb-report.png"])
        subprocess.run([ADB, "-s", SERIAL, "pull", "/sdcard/devkb-report.png", os.path.join(os.path.dirname(__file__), "..", "..", "docs", "next-session", "session21", "devkb-report.png")],
                       capture_output=True)
        subprocess.run([ADB, "-s", SERIAL, "shell", "rm", "/sdcard/devkb-report.png"])
        key(104)
        s = wait(lambda s: "skb.k#104" not in s.items, 5)
        step("... Done: back to the form", s)
        adb("shell", "input", "keyevent", "4")   # the form closed: nothing sent
        time.sleep(1)
    else:
        step("Support's Report a bug row", False, "not found")
finally:
    rotate(0)
    adb("shell", "am", "force-stop", PKG)
    # the test's folder deleted, the one from before back (with no sign of the test)
    run_as(f"rm -rf files/Songate && mv {ASIDE} files/Songate && rm -f files/Songate/test-items files/Songate/items.txt")
    print("settings put back:", "yes" if "settings.txt" in run_as("ls files/Songate") else "NO")

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
