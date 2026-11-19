"""Songate for Android, dev only: Settings' folded cards (the user chose B, 2026-11-16: one card open at a time, on pages with three
titled cards or more) and the renamed cards (Letter rail, Battery info, Battery icon).
   python tools/android/devfold.py [serial]          (default: emulator-5554; the dev APK installed first)
Checks on Appearance: its first card open, the others folded (their rows not drawn); a title opens its card and folds the one
before; a tap on the open one folds it too; the page keeps its open card after leaving it, and settings.txt keeps it; Find a
setting's results are never folded; the new titles. The settings folder is set aside first and put back at the end.
Ends with ALL GOOD or the steps that failed."""
import os
import re
import subprocess
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import ADB, PKG, SERIAL, results, screen, step, tap_at, tap_item, wait   # noqa: E402

stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devfold-{stamp}"
print(f"devfold on {SERIAL}, {stamp}", flush=True)


def adb(*args, secs=30):
    try:
        return subprocess.run([ADB, "-s", SERIAL, *args], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=secs).stdout
    except subprocess.TimeoutExpired:
        return ""


def run_as(cmd, secs=30):
    return adb("shell", f"run-as {PKG} sh -c '{cmd}'", secs=secs)


def tap_exact(t0, pause=1.4):
    s = screen()
    hit = next((t for t in s.texts if t[0] == t0), None)
    if hit:
        tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
        time.sleep(pause)
    return hit is not None


def has(s, t):
    return s is not None and any(x[0] == t for x in s.texts)


def kickers(s):
    """The card titles on screen (drawn letter by letter, spaced: joined back by their line)"""
    lines = {}
    for t, x, y, w, h in s.texts:
        if len(t) == 1:
            lines.setdefault(round(y), []).append((x, t))
    return {"".join(c for _, c in sorted(v)).strip() for v in lines.values() if len(v) > 3}


def folds(s):
    return sorted(k for k in s.items if k.startswith("set.fold:Appearance/"))


def scroll(down=True):
    s = screen()
    X = int(12 * s.scale)
    a, b = (0.75, 0.4) if down else (0.4, 0.85)
    adb("shell", "input", "swipe", str(X), str(int(s.h * a * s.scale)), str(X), str(int(s.h * b * s.scale)), "400")
    time.sleep(1.2)


def tap_fold(card):
    """A card's title, scrolled into sight first (a long page: the folded titles can be under the mini player), then tapped"""
    key = "set.fold:Appearance/" + card
    for _ in range(10):
        s = screen()
        if key in s.items and s.h * 0.12 < s.items[key][1] < s.h - 160:
            tap_item(key)
            time.sleep(1.2)
            return True
        y = s.items[key][1] if key in s.items else s.h
        scroll(y > s.h * 0.5)
    return False


def appearance():
    tap_item("ph.nav#4")
    time.sleep(1.5)
    tap_exact("Appearance")
    for _ in range(4):
        scroll(False)


adb("shell", "input", "keyevent", "224")
adb("shell", "svc", "power", "stayon", "true")
adb("shell", "settings", "put", "system", "user_rotation", "0")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}; rm -f files/Songate/items.txt files/Songate/start-guard.txt files/Songate/mock-ui.txt; "
       "sed -i \"/^foldOpen=/d\" files/Songate/settings.txt; touch files/Songate/test-items", secs=180)
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    wait(lambda s: "ph.nav#4" in s.items, 25)
    time.sleep(2)
    appearance()
    s = screen()
    step("Appearance: its first card open (Brightness shown)", has(s, "Brightness"))
    seen = set()
    for _ in range(6):
        seen |= set(folds(screen()))
        scroll()
    print("  cards:", ", ".join(k.split("/", 1)[1] for k in seen), flush=True)
    step("... the others folded: their titles only (Background motion not drawn)", "set.fold:Appearance/Background" in seen and not has(screen(), "Background motion"))
    for _ in range(6):
        scroll(False)
    tap_fold("Background")
    for _ in range(4):
        scroll(False)
    s = screen()
    step("a title: its card opens, the one before folds", has(s, "Background motion") and not has(s, "Brightness"))
    tap_fold("Background")
    s = screen()
    step("the open one's title: it folds too (all folded)", not has(s, "Background motion") and not has(s, "Brightness"))
    for _ in range(4):
        scroll(False)
    allf = set()
    for _ in range(3):
        allf |= kickers(screen())
        scroll()
    print("  all folded:", ", ".join(sorted(allf)), flush=True)
    step("the new titles: Letter rail, Battery info, Battery icon (and no LOCAL)",
         {"LETTER RAIL", "BATTERY INFO", "BATTERY ICON"} <= allf and not any("LOCAL" in k for k in allf))
    tap_fold("Sidebar")
    adb("shell", "input", "keyevent", "4")
    time.sleep(1)
    tap_exact("Appearance")
    for _ in range(4):
        scroll(False)
    s = screen()
    step("back on the page: the card left open is open (Sidebar)", s and any(t[0].startswith("Sidebar opacity") or t[0] == "Show" for t in s.texts) or "set.side" in str(s.items))
    adb("shell", "input", "keyevent", "3")   # Home: Songate writes what changed
    time.sleep(2.5)
    m = re.search(r"^foldOpen=(.*)$", run_as("cat files/Songate/settings.txt"), re.M)
    step("settings.txt keeps it", m and "Appearance>Sidebar" in m[1], m[1] if m else "none")
    adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
    time.sleep(2)
    # Find a setting: its results never folded
    tap_item("ph.nav#4")
    time.sleep(1.5)
    adb("shell", "input", "keyevent", "4")
    time.sleep(1)
    tap_item("ph.nav#4")
    time.sleep(1.2)
    tap_exact("Find a setting", 1.0)
    if wait(lambda s: "skb.k#104" in s.items, 4):
        for ch, idx in (("b", 33), ("l", 28), ("u", 16), ("r", 13)):   # (its keys: the letters page's order)
            tap_item(f"skb.k#{idx}")
            time.sleep(0.25)
        tap_item("skb.away")
        time.sleep(1.2)
    s = screen()
    step("Find a setting (blur): its result shown, nothing folded", has(s, "Background blur") and not any(k.startswith("set.fold:") for k in s.items))
finally:
    adb("shell", "svc", "power", "stayon", "false")
    adb("shell", "am", "force-stop", PKG)
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && "
                 "rm -f files/Songate/test-items files/Songate/items.txt; ls files/Songate/settings.txt")
    print("settings folder put back:", "yes" if "settings.txt" in out else "NO", flush=True)

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
