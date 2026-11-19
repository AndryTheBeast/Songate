"""Songate for Android, dev only: Songate as SongateOS's only launcher (the SongateOS session's prompt, 2026-11-16), on the EMULATOR.
   python tools/android/devlauncher.py <part> [serial]      parts: entry | guard | unlock   (default serial: emulator-5554)
entry   The home entry (HomeAlias) on a device that is not the R4: off after Songate's first start (Android offers Songate as home
        to nobody who did not ask). (Android lets no shell switch it on or reset it, so only this side can be checked here.)
guard   The start guard: two starts in a row that ended early make the next a careful one (its word in the log, the count 3 while
        it runs); 20 s running removes the count; the start after is an ordinary one.
unlock  Before the first unlock: a PIN is set, the emulator restarted (adb reboot), Songate's activity started while still locked
        (it is direct-boot aware): it waits, nothing of the app starts; the PIN typed, it starts. The PIN is cleared and the lock
        screen turned off again at the end, whatever happened.
Every adb call has its own time limit (a frozen emulator stops the part, it does not hang it). Songate's settings folder is set
aside first and put back at the end. Never on the user's own devices. Ends with ALL GOOD or the steps that failed."""
import os
import subprocess
import sys
import time

PART = sys.argv[1] if len(sys.argv) > 1 else ""
if len(sys.argv) > 2:
    os.environ["SONGATE_SERIAL"] = sys.argv[2]
from devlib import ADB, PKG, SERIAL, results, step   # noqa: E402

if PART not in ("entry", "guard", "unlock"):
    sys.exit(__doc__)
if not SERIAL.startswith("emulator-"):
    sys.exit("devlauncher.py sets a PIN and restarts the device: the emulator only")
PIN = "1111"
stamp = time.strftime("%Y%m%d-%H%M%S")
ASIDE = f"files/aside/devlauncher-{stamp}"
print(f"devlauncher {PART} on {SERIAL}, {stamp}", flush=True)


def adb(*args, secs=20):
    try:
        return subprocess.run([ADB, "-s", SERIAL, *args], capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=secs).stdout
    except subprocess.TimeoutExpired:
        print(f"  (adb {' '.join(args[:3])} gave no answer in {secs} s)", flush=True)
        return ""


def run_as(cmd, secs=20):
    return adb("shell", f"run-as {PKG} sh -c '{cmd}'", secs=secs)


def booted(secs=180):
    end = time.time() + secs
    while time.time() < end:
        if adb("shell", "getprop", "sys.boot_completed", secs=5).strip() == "1":
            return True
        time.sleep(3)
    return False


def guard():
    return run_as("cat files/Songate/start-guard.txt 2>/dev/null").strip()


def log_text():
    return run_as("cat files/Songate/log.txt 2>/dev/null")


def start():
    adb("shell", "am", "start", "-n", f"{PKG}/.SongateActivity")


def stop():
    adb("shell", "am", "force-stop", PKG)


def aside():
    adb("shell", "input", "keyevent", "224")   # (the screen woken first: a sleeping device runs no frames)
    adb("shell", "svc", "power", "stayon", "true")
    stop()
    run_as(f"mkdir -p files/aside; cp -r files/Songate {ASIDE}; rm -f files/Songate/start-guard.txt files/Songate/items.txt", secs=180)   # (the covers: a big copy)


def put_back():
    adb("shell", "svc", "power", "stayon", "false")
    stop()
    out = run_as(f"[ -f {ASIDE}/settings.txt ] && rm -rf files/Songate && mv {ASIDE} files/Songate && rm -f files/Songate/test-items files/Songate/items.txt "
                 "files/Songate/start-guard.txt; ls files/Songate/settings.txt")
    print("  settings folder put back:", "yes" if "settings.txt" in out else "NO (" + ASIDE + " kept)", flush=True)


if PART == "entry":
    aside()
    try:
        start()
        time.sleep(6)
        out = adb("shell", "dumpsys", "package", PKG)
        part = out[out.find("disabledComponents:"):][:400] if "disabledComponents:" in out else ""
        step("not the R4: the home entry off after Songate's start", "HomeAlias" in part)
        q = adb("shell", "cmd", "package", "query-activities", "-a", "android.intent.action.MAIN", "-c", "android.intent.category.HOME")
        step("... Android offers Songate as home to nobody", PKG not in q)
    finally:
        put_back()

elif PART == "guard":
    aside()
    try:
        start()
        time.sleep(4)
        step("an ordinary start: its count while it runs is 1", guard() == "1", guard())
        stop()
        start()
        time.sleep(4)
        step("ended early, started again: the count is 2", guard() == "2", guard())
        stop()
        start()
        time.sleep(8)
        log = log_text()
        step("after two early ends: a careful start (its word in the log)", "a careful start" in log, log[-300:])
        step("... the count is 3 while it runs", guard() == "3", guard())
        time.sleep(15)
        step("... 20 s running: the count removed", guard() == "", guard())
        stop()
        start()
        time.sleep(5)
        step("the start after: an ordinary one (its count 1: no early ends before it)", guard() == "1", guard())
        adb("shell", "input", "keyevent", "3")   # Home: Songate leaves the front cleanly
        time.sleep(3)
        step("left the front (Home) before 20 s: a good start too, the count removed", guard() == "", guard())
        stop()
    finally:
        put_back()

elif PART == "unlock":
    lock_none_before = "true" in adb("shell", "locksettings", "get-disabled")
    aside()
    try:
        adb("shell", "locksettings", "set-disabled", "false")
        adb("shell", "locksettings", "set-pin", PIN)
        print("  restarting the emulator (adb reboot)", flush=True)
        adb("reboot", secs=30)
        time.sleep(10)
        step("restarted", booted(240))
        time.sleep(5)
        step("still locked (Songate's own folder not readable)", "settings.txt" not in run_as("ls files/Songate", secs=10))
        adb("logcat", "-c")
        start()   # (direct-boot aware: Android starts it before the unlock)
        time.sleep(6)
        lg = adb("logcat", "-d", "-s", "Songate:*")
        step("started before the unlock: Songate's activity waits", "before the first unlock: waiting" in lg, lg[-300:])
        step("... nothing of the app started yet", "unlocked: starting" not in lg)
        adb("shell", "input", "keyevent", "224")
        adb("shell", "input", "keyevent", "82")
        time.sleep(1.5)
        adb("shell", "input", "text", PIN)
        adb("shell", "input", "keyevent", "66")
        time.sleep(8)
        lg = adb("logcat", "-d", "-s", "Songate:*")
        step("the PIN typed: Songate starts", "unlocked: starting" in lg)
        step("... the app runs (its log written now)", "settings.txt" in run_as("ls files/Songate"))
    finally:
        # (the lock screen as before whatever happened; the PIN needs the user unlocked, so it is typed once more first)
        adb("shell", "input", "keyevent", "82")
        adb("shell", "input", "text", PIN)
        adb("shell", "input", "keyevent", "66")
        time.sleep(2)
        adb("shell", "locksettings", "clear", "--old", PIN)
        if lock_none_before:
            adb("shell", "locksettings", "set-disabled", "true")
        print("  lock screen off again:", "true" in adb("shell", "locksettings", "get-disabled"), flush=True)
        put_back()

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
