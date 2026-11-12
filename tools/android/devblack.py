"""devturn's own sequences, and at an all-black return: what Android and Songate say. Emulator only."""
import io, subprocess, sys, time
from pathlib import Path
from PIL import Image
ADB = r"C:\Android\sdk\platform-tools\adb.exe"
SERIAL = "emulator-5554"
PKG = "io.github.andrythebeast.songate"
ACT = PKG + "/.SongateActivity"
OUT = Path(__file__).resolve().parent / "blackhunt2"
OUT.mkdir(exist_ok=True)
def adb(*a, binary=False):
    r = subprocess.run([ADB, "-s", SERIAL, *a], capture_output=True)
    return r.stdout if binary else r.stdout.decode("utf-8", "replace")
def look():
    png = adb("exec-out", "screencap", "-p", binary=True)
    im = Image.open(io.BytesIO(png)).convert("L").resize((48, 48))
    return png, max(im.tobytes())
plans = [
    ("turn-home-back", [1, 1.5, "home", 1.5, "back"]), ("turn-home-quick", [1, 0.2, "home", 0.3, "back"]),
    ("home-turn-back", ["home", 1, 1, 1.5, "back"]), ("upright-home-back", [0, 1.5, "home", 1.5, "back"]),
    ("upright-home-quick", [0, 0.2, "home", 0.3, "back"]), ("home-upright-back", ["home", 1, 0, 1.5, "back"]),
    ("other-side-home-back", [3, 1.5, "home", 1.5, "back"]), ("side-to-side-home", [1, 0.3, 3, 0.2, "home", 1, "back"]),
    ("recents-back", [0, 1, "recents", 1.5, "back"]), ("turn-recents-back", [1, 0.3, "recents", 1.5, "back"]),
]
adb("shell", "settings put system accelerometer_rotation 0")
adb("shell", "am", "start", "-n", ACT)
time.sleep(6)
found = 0
for n in range(int(sys.argv[1])):
    for name, steps in plans:
        adb("logcat", "-c")
        for s in steps:
            if s == "home": adb("shell", "input keyevent KEYCODE_HOME")
            elif s == "recents": adb("shell", "input keyevent KEYCODE_APP_SWITCH")
            elif s == "back": adb("shell", "am", "start", "-n", ACT)
            elif isinstance(s, int): adb("shell", f"settings put system user_rotation {s}")
            else: time.sleep(s)
        time.sleep(2.8)
        png, mx = look()
        if mx > 3:
            continue
        found += 1
        tag = f"{n}-{name}"
        (OUT / f"{tag}-0.png").write_bytes(png)
        win = adb("shell", "dumpsys", "window")
        focus = [l.strip() for l in win.splitlines() if "mCurrentFocus" in l or "mFocusedApp" in l]
        power = [l.strip() for l in adb("shell", "dumpsys", "power").splitlines() if "mWakefulness=" in l]
        rot = [l.strip() for l in adb("shell", "dumpsys", "window", "displays").splitlines() if "mRotation=" in l][:1]
        it1 = adb("shell", f"run-as {PKG} cat files/Songate/items.txt")
        time.sleep(1)
        it2 = adb("shell", f"run-as {PKG} cat files/Songate/items.txt")
        (OUT / f"{tag}-items.txt").write_text(it2, encoding="utf-8")
        (OUT / f"{tag}-logcat.txt").write_text(adb("logcat", "-d"), encoding="utf-8")
        print("BLACK", tag, focus, power, rot, "| items first line:", it2.splitlines()[:1], "| items changing:", it1 != it2)
        for w in (3, 6, 12):
            time.sleep(3 if w == 3 else w / 2)
            print("   later", w, look()[1])
        print("   log:", [l[-150:] for l in adb("logcat", "-d", "-s", "Songate:W").splitlines()[-2:]])
        adb("shell", "input", "tap", "1200", "500")
        time.sleep(1.5)
        png, mx = look()
        (OUT / f"{tag}-aftertap.png").write_bytes(png)
        print("   after a tap:", mx)
adb("shell", "settings put system user_rotation 0")
print("black:", found)
