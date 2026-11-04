"""Songate for Android: a scripted run of the dev build on the emulator (or a phone), every step checked from what is on screen.
   python tools/android/devtest.py [serial]          (default: emulator-5554; the dev APK installed first: tools/android/build.sh)
The dev build writes what it shows (every control by name, every text, where) to items.txt when test-items is in its settings folder
(platform_android.cpp, App::frame), as GF_ITEMS does on the PC. The run starts as a new user (no settings, no access), and puts the
settings that were there back at its end. Ends with ALL GOOD or the steps that failed. Dev only: nothing of it ships."""
import io
import os
import re
import subprocess
import sys
import time

if len(sys.argv) > 1:
    os.environ["SONGATE_SERIAL"] = sys.argv[1]
from devlib import *   # noqa: E402,F403  (adb, run_as, screen, wait, tap_item, tap_text, step, results, notices, ...)

stamp = time.strftime("%Y%m%d-%H%M%S")
print(f"devtest on {SERIAL}, {stamp}")
adb("shell", "am", "force-stop", PKG)
adb("shell", "settings", "put", "system", "accelerometer_rotation", "0")
adb("shell", "settings", "put", "system", "user_rotation", "0")
for _ in range(20):   # the turn done before the first start (left sideways by a run or by hand, the app would start sideways)
    if re.search(r"mRotation=0", adb("shell", "dumpsys", "window", "displays")):
        break
    time.sleep(0.5)
# a new user: the settings there moved aside (put back at the end), no access to the music
run_as(f"mkdir -p files/aside; [ -d files/Songate ] && mv files/Songate files/aside/before-devtest-{stamp}; mkdir -p files/Songate; touch files/Songate/test-items")
adb("shell", "appops", "set", "--uid", PKG, "MANAGE_EXTERNAL_STORAGE", "default")
adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
for _ in range(20):   # upright again with Songate in front (Android may put a turn back while its home screen shows)
    adb("shell", "settings", "put", "system", "user_rotation", "0")
    if "mRotation=0" in adb("shell", "dumpsys", "window", "displays"):
        break
    time.sleep(0.5)
try:
    shots = []   # the opening, upright: a few moments of it (a first start takes its own time to begin)
    for _ in range(10):
        shots.append(subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout)
        time.sleep(0.3)
    s = wait(lambda s: s.has_text("Allow Songate to read your files?") and "sp.yes" in s.items, 15)
    step("first start without access: the access card", s)
    try:
        from PIL import Image
        best = (0, 0)
        for shot in shots:
            im = Image.open(io.BytesIO(shot)).convert("L")
            w, h = im.size
            mid = max(im.getpixel((w // 2 + d, y)) for y in range(int(h * 0.4), int(h * 0.6), 3) for d in range(-200, 200, 8))
            edge = max(max(im.getpixel((x, y)) for y in range(int(h * 0.35), int(h * 0.65), 4)) for x in (2, w - 3))
            if mid > best[0]:
                best = (mid, edge)
        mid, edge = best
        step("the opening upright: the name inside the screen", mid > 80 and edge < mid * 0.5, f"edge {edge}, middle {mid}")
    except ImportError:
        print("  (PIL missing: the opening not checked)")
    adb("shell", "appops", "set", "--uid", PKG, "MANAGE_EXTERNAL_STORAGE", "allow")   # (the switch on Android's page)
    s = wait(lambda s: s.has_text("Where is your music?"), 10)
    step("access given: Where is your music? offers a folder", s)
    tap_text("Just use it")
    s = wait(lambda s: "ph.nav#1" in s.items and not s.has_text("Where is your music?"), 20)
    step("the folder chosen: the phone layout upright", s)
    tap_item("ph.nav#1")
    s = wait(lambda s: "ph.lrow" in s.items and any(re.search(r"[1-9]\d* tracks", t[0]) for t in s.texts), 30)
    step("Library: its rows and a track count", s)
    tap_item("ph.lshuffle")
    s = wait(lambda s: s.playing, 15)
    step("Shuffle plays", s, "not playing")
    n = len(notices)
    s = wait(lambda s: "notice.later" in s.items, 25)
    if s:
        clear_notices(s)
    step("a new library: the covers notice comes, Not now closes it", s and "notice.later" not in screen().items)
    del notices[n:]
    tap_item("ph.nav")
    s = wait(lambda s: "ph.play" in s.items, 5)
    step("Now playing upright: its transport", s)
    tap_item("ph.queue")
    s = wait(lambda s: "ph.qclose" in s.items, 5)
    step("Up next opens as a sheet", s)
    adb("shell", "input", "keyevent", "4")
    s = wait(lambda s: "ph.qclose" not in s.items, 5)
    step("back closes Up next", s)
    tap_item("ph.lyrics")
    s = wait(lambda s: "ph.lcover" in s.items, 5)
    step("the lyrics page opens", s)
    adb("shell", "input", "keyevent", "4")
    s = wait(lambda s: "ph.play" in s.items and "ph.lcover" not in s.items, 5)
    step("back returns to Now playing", s)
    adb("shell", "settings", "put", "system", "user_rotation", "1")
    s = wait(lambda s: s.w > s.h and s.playing, 8)
    step("turned sideways: laid out wide, still playing", s)
    step("no Canvas button upright, one sideways", "pb.canvas" in s.items if s else False)
    tap_item("pb.canvas")
    s = wait(lambda s: "cv.play" in s.items, 5)
    step("sideways: the Canvas opens", s)
    adb("shell", "settings", "put", "system", "user_rotation", "0")
    s = wait(lambda s: s.h > s.w and s.playing and "ph.play" in s.items, 8)
    step("turned back upright: the phone layout, still playing", s)
    step("turned upright with the Canvas open: it closed, and no Canvas button", s and "cv.play" not in s.items and "ph.canvas" not in s.items)
    clear_notices(screen())
    adb("shell", "input", "keyevent", "3")
    time.sleep(3)
    step("Home: the music plays on (the media session)", session_state() == "PLAYING", session_state())
    adb("shell", "input", "keyevent", "KEYCODE_MEDIA_PAUSE")
    time.sleep(2)
    step("the media session's pause pauses", session_state() == "PAUSED", session_state())
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    s = wait(lambda s: "ph.nav#4" in s.items, 8)
    step("back in front", s)
    tap_item("ph.nav#4")
    s = wait(lambda s: "set.cat" in s.items and s.has_text("Appearance"), 5)
    step("Settings upright: the list of categories", s)
    tap_item("set.cat")
    s = wait(lambda s: s.has_text("Saturation"), 5)
    step("a category's page", s)
    adb("shell", "input", "keyevent", "4")
    s = wait(lambda s: "set.cat" in s.items, 5)
    step("back returns to the list", s)
    tap_item("ph.nav#2")
    s = wait(lambda s: "ph.sfield" in s.items, 5)
    step("Search upright: its field", s)
    tap_item("ph.sfield")
    time.sleep(0.8)
    adb("shell", "input", "text", "Geoxor")   # into the app's own typing card (view_textcard.cpp)
    time.sleep(0.5)
    adb("shell", "input", "keyevent", "66")   # Enter: its OK
    s = wait(lambda s: s.has_text("Albums") and s.has_text("Artists") and any(re.search(r"\d+:\d\d", t[0]) for t in s.texts), 8)
    step("typed: tracks, and the Albums and Artists pills", s)
    tap_item("ph.nav#1")
    s = wait(lambda s: "ph.lrow" in s.items, 5)
    step("Library: no Files row while the sidebar hides Files (a new user's)", s and "ph.lrow#6" not in s.items)
    # Files shown in the sidebar (Settings › Appearance › Sidebar), as a user who turned it on: the settings file of this run's own folder
    adb("shell", "am", "force-stop", PKG)
    run_as("sed -i s/^hiddenTabs=.*/hiddenTabs=0/ files/Songate/settings.txt")
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    wait(lambda s: "ph.nav#1" in s.items, 10)
    time.sleep(4)   # the opening
    tap_item("ph.nav#1")
    s = wait(lambda s: "ph.lrow#6" in s.items and s.has_text("Files"), 8)
    step("Library: the Files row after Liked when the sidebar shows Files", s)
    tap_item("ph.lrow#6")
    s = wait(lambda s: s.has_text("This device") and s.has_text("Internal storage") and s.has_text("SD card"), 8)
    step("Files upright: Internal storage and SD card", s)
    tap_text("SD card")
    s = wait(lambda s: any(t[0] == "SD card" and t[4] > 30 for t in s.texts) and s.has_text("/storage/"), 8)
    step("in the card: its name as the title", s)
    adb("shell", "input", "keyevent", "4")
    adb("shell", "input", "keyevent", "4")
    s = wait(lambda s: "ph.lrow#6" in s.items, 6)
    step("back, back: the Library page", s)
    tap_item("ph.nav#3")
    s = wait(lambda s: "pls.new" in s.items, 5)
    step("Playlists upright", s)
    tap_item("pls.new")
    wait(lambda s: "sp.row#7000" in s.items, 5)
    tap_item("sp.row#7000")   # A playlist of your own: the typing card
    time.sleep(0.8)
    adb("shell", "input", "text", "DevtestList")
    time.sleep(0.4)
    adb("shell", "input", "keyevent", "66")
    s = wait(lambda s: "pl.icon" in s.items and s.has_text("DevtestList"), 6)
    step("a new playlist, named through the typing card", s)
    tap_item("pl.icon")
    s = wait(lambda s: "sp.icon" in s.items, 5)
    tiles = [v for k, v in s.items.items() if k.startswith("sp.icon")] if s else []
    overlap = any(a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]
                  for i, a in enumerate(tiles) for b in tiles[i + 1:])
    inside = all(t[0] >= 0 and t[0] + t[2] <= s.w for t in tiles) if s else False
    step("its icon sheet: tiles apart and on screen, the words say tap", s and tiles and not overlap and inside and s.has_text("tap one"),
         f"{len(tiles)} tiles, overlap {overlap}, inside {inside}")
    tap_item("sp.icon#3")
    s = wait(lambda s: "sp.icon" not in s.items and "pl.icon" in s.items, 5)
    step("an icon tapped: the sheet closes", s)
    # Settings › Appearance › Sidebar upright: a tab dragged by its handle (the user chose the handle, 2026-11-04)
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    time.sleep(1.5)
    tap_item("ph.nav#4"); time.sleep(0.8)
    if not screen().has_text("Find a setting"):
        adb("shell", "input", "keyevent", "4"); time.sleep(0.8)
    tap_text("Appearance"); time.sleep(1)
    def handles():
        s = screen()
        return s, sorted((v[1], k) for k, v in s.items.items() if k.startswith("set.tabdrag") and 80 < v[1] < 560)
    for _ in range(14):
        s, hs = handles()
        if len(hs) >= 4: break
        adb("shell", "input", "swipe", "250", "1500", "250", "1100", "500"); time.sleep(0.9)
    time.sleep(1.2)
    s, hs = handles()
    order0 = run_as("grep ^tabOrder files/Songate/settings.txt").strip()
    step("Sidebar upright: a drag handle on every tab's line", len(hs) >= 4, f"{len(hs)} handles")
    if len(hs) >= 4:
        x, y, w, h = s.items[hs[1][1]]
        px, py = int((x + w / 2) * s.scale), int((y + h / 2) * s.scale)
        adb("shell", "input", "swipe", str(px), str(py), str(px), str(py + int(56 * 2 * s.scale)), "1500")
        time.sleep(2)
        order1 = run_as("grep ^tabOrder files/Songate/settings.txt").strip()
        step("a tab dragged two places down: the order saved", order1 != order0 and order1.startswith("tabOrder="), f"{order0} -> {order1}")
        s, hs = handles()
        x, y, w, h = s.items[hs[3][1]] if len(hs) > 3 else (0, 0, 0, 0)
        if w:
            px, py = int((x + w / 2) * s.scale), int((y + h / 2) * s.scale)
            adb("shell", "input", "swipe", str(px), str(py), str(px), str(py - int(56 * 2 * s.scale)), "1500")
            time.sleep(2)
        step("dragged back up: the order as it was", run_as("grep ^tabOrder files/Songate/settings.txt").strip() == order0)
    adb("shell", "input", "keyevent", "4"); time.sleep(0.6)
    # Swiped away from the recent apps while playing (Settings › Playback › Keep playing when swiped away): on, it plays on; off, it ends
    def in_recents():
        return f"A={PKG}" in adb("shell", "dumpsys", "activity", "recents") or f"{PKG}/" in adb("shell", "dumpsys", "activity", "recents")
    def swipe_away():   # its card in the recent apps swiped up (again if the first swipe did not take it)
        for _ in range(3):
            adb("shell", "input", "keyevent", "187")
            time.sleep(2)
            adb("shell", "input", "swipe", "540", "1300", "540", "150", "150")
            time.sleep(3)
            if not in_recents():
                break
        adb("shell", "input", "keyevent", "3")
        time.sleep(1)
    def ensure_playing():
        s = screen()
        if not s.playing:
            tap_item("ph.nav"); time.sleep(0.8); tap_item("ph.play")
        return wait(lambda s: s.playing, 6)
    def alive():
        return adb("shell", "pidof", PKG).strip() != ""
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    time.sleep(2)
    step("playing before swiping away", ensure_playing())
    swipe_away()
    step("swiped away, the switch on: still running and playing", alive() and session_state() == "PLAYING", session_state())
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    time.sleep(2)
    tap_item("ph.nav#4"); time.sleep(0.8); tap_text("Playback"); time.sleep(1)
    for _ in range(8):
        if screen().has_text("Keep playing when swiped away"):
            break
        adb("shell", "input", "swipe", "250", "1700", "250", "800", "400"); time.sleep(0.8)
    s = screen()
    step("Settings › Playback: the switch, on", "set.swipe" in s.items and s.has_text("Keep playing when swiped away"))
    time.sleep(1.5)   # the page's glide after the scroll, settled
    tap_item("set.swipe")
    time.sleep(1.2)
    step("the switch turned off (saved)", wait(lambda s: "swipeKeep=0" in run_as("cat files/Songate/settings.txt"), 4) if False else
         any("swipeKeep=0" in run_as("cat files/Songate/settings.txt") or time.sleep(0.5) for _ in range(8)))
    step("playing again", ensure_playing())
    swipe_away()
    step("swiped away, the switch off: Songate ended with its music", not alive(), "still running" if alive() else "")
finally:
    adb("shell", "am", "force-stop", PKG)
    run_as(f"mv files/Songate files/aside/devtest-{stamp}; [ -d files/aside/before-devtest-{stamp} ] && mv files/aside/before-devtest-{stamp} files/Songate")
    adb("shell", "appops", "set", "--uid", PKG, "MANAGE_EXTERNAL_STORAGE", "allow")
failed = [n for n, ok in results if not ok]
for n in notices:
    print(f"  (answered Not now: {n})")
print("\n" + ("ALL GOOD" if not failed else f"{len(failed)} FAILED: " + "; ".join(failed)))
sys.exit(1 if failed else 0)
