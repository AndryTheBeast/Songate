"""Songate for Android, dev only: the same short walk through the app at a spread of screens (wm size, wm density), upright and
   sideways, a screenshot and devaudit's findings at each stop. For the scale rule (App::fitWindow): nothing cut at any shape.
       python tools/android/devsizes.py <out folder> [screen ...]        (not with -I: it imports devlib beside it)
   Screens are below (SCREENS), by name. The emulator's own size and density are put back at the end, whatever happens.
   <out>/<screen>-<upright|sideways>/NN-label.png, and <out>/report.txt. Nothing of it ships."""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import ADB, PKG, SERIAL, adb, run_as, screen, tap_item, tap_text, wait, clear_notices   # noqa: E402
from devaudit import audit   # noqa: E402

# name: (width, height, density)
SCREENS = {
    "720x1280-16x9": (720, 1280, 320),
    "1080x1920-16x9": (1080, 1920, 420),
    "1080x2400-20x9": (1080, 2400, 420),
    "1080x2520-21x9": (1080, 2520, 420),
    "1080x2640-22x9": (1080, 2640, 420),
    "904x2316-cover": (904, 2316, 420),
    "1768x2208-fold": (1768, 2208, 420),
    "1200x2000-tab": (1200, 2000, 240),
    "1600x2560-tab": (1600, 2560, 320),
    "1536x2048-4x3": (1536, 2048, 320),
    "1080x2400-big": (1080, 2400, 540),     # Android's Display size at its largest
    "1080x2400-small": (1080, 2400, 356),   # ... and its smallest
}

N = "ph.nav"
# the phone's own layout (a bottom bar)
PHONE = [N + "#1", "wait:0.8", "shot:library", "ph.lalbum", "wait:1.2", "shot:album", "key:4", "wait:0.6",
         N, "wait:0.8", "shot:now", "ph.lyrics", "wait:1.2", "shot:lyrics", "key:4", "wait:0.8", "ph.more", "wait:0.9", "shot:song-menu", "key:4",
         N + "#2", "wait:0.8", "ph.sfield", "wait:0.8", "type:Geoxor", "wait:1.2", "shot:search",
         N + "#3", "wait:0.9", "shot:playlists",
         N + "#4", "?ph.back", "wait:1.5", "shot:settings", "side.update", "wait:1", "shot:update-sheet", "text:Later", "wait:2",
         "side.job", "wait:1", "shot:jobs-sheet", "key:4", "wait:0.6",
         "text:Appearance", "wait:1", "shot:appearance", "swipe:up", "shot:appearance-2", "swipe:up", "shot:appearance-3", "swipe:up", "shot:appearance-4",
         "key:4", "wait:0.6", "text:Playback", "wait:1", "shot:playback", "key:4"]
# the console's layout (a sidebar, a title bar, a player bar)
CONSOLE = ["nav.library", "wait:1", "shot:library", "nav.albums", "wait:1", "lib.tile*", "wait:1.2", "shot:album",
           "nav.now", "wait:0.8", "shot:now", "np.tab#1", "wait:1", "shot:lyrics", "np.tab", "np.more", "wait:0.9", "shot:song-menu", "key:4", "wait:0.5",
           "tb.search", "wait:0.8", "type:Geoxor", "wait:1.2", "shot:search",
           "nav.playlists", "wait:1", "shot:playlists",
           "nav.settings", "wait:1.5", "shot:settings", "side.update", "wait:1", "shot:update-sheet", "text:Later", "wait:2",
           "side.job", "wait:1", "shot:jobs-sheet", "key:4", "wait:0.6",
           "text:Appearance", "wait:1", "shot:appearance", "swipe:up", "shot:appearance-2", "swipe:up", "shot:appearance-3",
           "nav.settings", "text:Playback", "wait:1", "shot:playback",
           "nav.now", "wait:0.6", "pb.canvas", "wait:1.8", "shot:canvas", "key:4", "wait:0.8",
           "tb.sidebar", "wait:0.9", "shot:sidebar-toggled", "tb.sidebar", "wait:0.6"]

report = []


def swipe(direction):
    s = screen()
    W, H = s.w * s.scale, s.h * s.scale
    x = str(int(W * 0.6))
    a, b = (int(H * 0.7), int(H * 0.3)) if direction == "up" else (int(H * 0.3), int(H * 0.7))
    adb("shell", "input", "swipe", x, str(a), x, str(b), "350")
    time.sleep(0.9)


def turn(want):
    adb("shell", "settings", "put", "system", "accelerometer_rotation", "0")
    for _ in range(20):
        adb("shell", "settings", "put", "system", "user_rotation", want)
        if f"mRotation={want}" in adb("shell", "dumpsys", "window", "displays") or f"mRotation=ROTATION_{'90' if want == '1' else '0'}" in adb("shell", "dumpsys", "window", "displays"):
            break
        time.sleep(0.5)
    time.sleep(1)


def walk(out, name, way):
    folder = os.path.join(out, f"{name}-{way}")
    os.makedirs(folder, exist_ok=True)
    adb("shell", "am", "force-stop", PKG)
    run_as("touch files/Songate/test-items files/Songate/test-jobs; echo 2.0.0 > files/Songate/test-update")
    turn("0" if way == "upright" else "1")
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    turn("0" if way == "upright" else "1")
    s = wait(lambda s: "ph.nav" in s.items or "nav.now" in s.items, 20)
    time.sleep(4)   # the opening
    s = clear_notices(screen())
    phone = "ph.nav" in s.items
    head = f"{name} {way}: app {s.w}x{s.h} at scale {s.scale}, {'phone' if phone else 'console'} layout"
    print(head, flush=True)
    report.append(head)
    n = 0
    for st in (PHONE if phone else CONSOLE):
        if st.startswith("shot:"):
            time.sleep(0.6)
            clear_notices(screen())   # a new user's notice over the page: answered Not now first
            png = subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout
            path = os.path.join(folder, f"{n:02d}-{st[5:]}.png")
            open(path, "wb").write(png)
            s = screen()
            open(path[:-4] + ".items.txt", "w", encoding="utf-8").write(run_as("cat files/Songate/items.txt 2>/dev/null"))
            found = audit(s)
            report.append(f"  {n:02d}-{st[5:]:18} {len(found)} findings")
            report.extend(f"      {k:16} {t}" for k, t in found)
            print(f"  {n:02d}-{st[5:]:18} {len(found)}" + "".join(f"\n      {k:16} {t}" for k, t in found[:10]), flush=True)
            n += 1
        elif st.startswith("wait:"):
            time.sleep(float(st[5:]))
        elif st.startswith("key:"):
            adb("shell", "input", "keyevent", st[4:])
            time.sleep(0.7)
        elif st.startswith("text:"):
            if not tap_text(st[5:]):
                report.append(f"  missed: words '{st[5:]}'")
            time.sleep(0.6)
        elif st.startswith("type:"):
            time.sleep(0.6)
            adb("shell", "input", "text", st[5:])
            time.sleep(0.4)
            adb("shell", "input", "keyevent", "66")
            time.sleep(0.8)
        elif st.startswith("swipe:"):
            swipe(st[6:])
        elif st.startswith("?"):
            tap_item(st[1:])
            time.sleep(0.6)
        else:
            if st.endswith("*"):   # the first of that list on screen
                on = sorted((v[1], v[0], k) for k, v in screen().items.items() if k.startswith(st[:-1]) and v[1] > 60)
                st = on[0][2] if on else st
            if st == "ph.lalbum":   # (a short screen: Library's albums are under the mini player until the page is moved up)
                s = screen()
                if st in s.items and s.items[st][1] + s.items[st][3] / 2 > s.h - 150:
                    swipe("up")
            if not tap_item(st):
                report.append(f"  missed: control {st}")
            time.sleep(0.6)
    adb("shell", "am", "force-stop", PKG)


def main():
    out = sys.argv[1]
    names = sys.argv[2:] or list(SCREENS)
    os.makedirs(out, exist_ok=True)
    try:
        for name in names:
            w, h, d = SCREENS[name]
            turn("0")
            adb("shell", "wm", "size", f"{w}x{h}")
            adb("shell", "wm", "density", str(d))
            time.sleep(3)
            for way in ("upright", "sideways"):
                try:
                    walk(out, name, way)
                except Exception as e:   # the next screen all the same
                    report.append(f"{name} {way}: {e!r}")
                    print("  failed:", repr(e), flush=True)
    finally:
        adb("shell", "am", "force-stop", PKG)
        run_as("rm -f files/Songate/test-jobs files/Songate/test-update")
        turn("0")
        adb("shell", "wm", "size", "reset")
        adb("shell", "wm", "density", "reset")
        with open(os.path.join(out, "report.txt"), "a", encoding="utf-8") as f:
            f.write("\n".join(report) + "\n")
    print("done")


main()
