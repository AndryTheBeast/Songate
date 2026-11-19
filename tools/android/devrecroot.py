"""Songate for Android, dev only: Settings › Updates › SongateOS's Recovery and Root cards (the user chose A, 2026-11-17), against the
emulator's simulated R4 (mock-r4.txt).
   python tools/android/devrecroot.py [serial]          (default: emulator-5554; the dev APK installed first)
Checks: the two cards between the version and Options; Recovery: TWRP, ready, Switch to OrangeFox, and Update (a newer TWRP);
a switch makes it OrangeFox; Root: Off, Install Magisk asked first (Install), then Restart now; after it, Magisk 31.1 with Remove
and Keep root after updates; Remove asked first. The settings folder is set aside first and put back. Ends with ALL GOOD or the failures."""
import os
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import PKG, SERIAL, adb, results, run_as, screen, step, tap_at, tap_item, wait   # noqa: E402
from devphase2_helpers import kicker, scroll   # noqa: E402

stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devrecroot-{stamp}"
print(f"devrecroot on {SERIAL}, {stamp}", flush=True)


def has(s, t):
    return s is not None and any(x[0] == t for x in s.texts)


def kickers(s):
    lines = {}
    for t, x, y, w, h in s.texts:
        if len(t) == 1:
            lines.setdefault(round(y), []).append((x, t))
    return {"".join(c for _, c in sorted(v)).strip() for v in lines.values() if len(v) > 3}


def open_card(word):
    s = screen()
    if word == "RECOVERY" and has(s, "Ready for SongateOS updates"):
        return
    if word == "ROOT" and ("sys.root" in s.items or "sys.root.restart" in s.items):
        return
    kicker(word)


def updates():
    tap_item("ph.nav#4")
    time.sleep(1.8)
    for _ in range(6):
        scroll(False, 0.75, 0.3)
    for _ in range(8):
        s = screen()
        hit = next((t for t in s.texts if t[0] == "Updates"), None)
        if hit and s.h * 0.1 < hit[2] < s.h - 170:
            tap_at(s, hit[1] + hit[3] / 2, hit[2] + hit[4] / 2)
            break
        scroll()
    time.sleep(1.5)
    tap_item("set.updtab[1]")
    time.sleep(2)


def yes():
    s = wait(lambda s: "sp.yes" in s.items, 4)
    if s:
        tap_item("sp.yes")
        time.sleep(1.5)
    return s is not None


adb("shell", "input", "keyevent", "224")
adb("shell", "svc", "power", "stayon", "true")
adb("shell", "am", "force-stop", PKG)
run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}")
run_as('rm -f files/Songate/items.txt files/Songate/start-guard.txt files/Songate/mock-ui.txt; touch files/Songate/mock-r4.txt files/Songate/test-items; '
       'sed -i "/^foldOpen=/d" files/Songate/settings.txt')
adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")
try:
    wait(lambda s: "ph.nav#4" in s.items, 30)
    time.sleep(3)
    updates()
    k = kickers(screen())
    step("Updates › SongateOS: Recovery and Root cards", {"RECOVERY", "ROOT"} <= k, ", ".join(sorted(k)))
    open_card("RECOVERY")
    s = wait(lambda s: has(s, "Ready for SongateOS updates"), 5)
    step("Recovery: TWRP, ready, Switch to OrangeFox", s is not None and has(s, "Switch to OrangeFox") and any(t[0].startswith("TWRP") for t in s.texts))
    step("... Update: a newer TWRP", has(s, "A newer TWRP") and "sys.rec.update" in s.items)
    tap_item("sys.rec.switch")
    s = wait(lambda s: has(s, "Switch to TWRP"), 6)
    step("Switch: now OrangeFox (Switch to TWRP offered)", s is not None and any(t[0].startswith("OrangeFox") for t in s.texts))
    open_card("ROOT")
    s = wait(lambda s: has(s, "Install Magisk"), 5)
    step("Root: Off, Install Magisk", s is not None and has(s, "Off"))
    tap_item("sys.root")
    s = wait(lambda s: has(s, "Install Magisk?"), 4)
    step("... asked first: Install Magisk?", s is not None and any(t[0].startswith("Root apps can change anything") for t in s.texts))
    yes()
    s = wait(lambda s: "sys.root.restart" in s.items, 6)
    step("Installed: Restart the player to finish, Restart now", s is not None and has(s, "Restart the player to finish"))
    tap_item("sys.root.restart")
    s = wait(lambda s: has(s, "Root is on"), 6)
    step("After the restart: Magisk 31.1, Root is on, Remove, Keep root after updates", s is not None and has(s, "Magisk 31.1") and
         has(s, "Remove") and has(s, "Keep root after updates"))
    tap_item("sys.root")
    s = wait(lambda s: has(s, "Remove Magisk?"), 4)
    step("Remove: asked first", s is not None)
    yes()
    s = wait(lambda s: "sys.root.restart" in s.items, 6)
    step("... removed: Restart now", s is not None)
finally:
    adb("shell", "svc", "power", "stayon", "false")
    adb("shell", "am", "force-stop", PKG)
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && "
                 "rm -f files/Songate/test-items files/Songate/items.txt; ls files/Songate/settings.txt")
    print("settings folder put back:", "yes" if "settings.txt" in out else "NO", flush=True)

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
