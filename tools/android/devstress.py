"""Songate for Android, dev only: the app opened, closed, turned and left at the worst moments, over and over, music playing; after
   each sequence what must still be true is checked (the user, 2026-11-11: "Do a stress test to the app, open, close, change
   direction while opening, etc").
       python tools/android/devstress.py [out folder] [sequence ...]      (not with -I: it imports devlib beside it)
   Sequences are below (SEQS), run in that order when none is named. After each run of a sequence:
     - the process: there, or gone when the sequence ends it (pidof)
     - logcat since the sequence began: a crash (FATAL EXCEPTION, Fatal signal, an abort's tombstone), an ANR
     - the screen: no band of pure black along an edge 0.8 s and 2.8 s after (devturn's check), not all black at the second look
     - the app answers a tap (another page comes up)
     - the music: Android's session says PLAYING or PAUSED, and while playing the place in the song moves
     - the settings: settings.txt whole, the library's list as long as before
   The settings folder is copied first and put back at the end (the monkey presses anything), the network is off while the monkey
   runs (nothing it presses reaches a server), and the SD card's music is counted before and after. The emulator's size, density
   and turn are put back whatever happens. Nothing of it ships."""
import os
import random
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import ADB, PKG, SERIAL, adb, run_as, screen, tap_at, tap_item, clear_notices, session_state   # noqa: E402

ACT = PKG + "/.SongateActivity"
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..", "build", "devstress")
ONLY = sys.argv[2:]
os.makedirs(OUT, exist_ok=True)
FAULTS = ("died", "crash", "anr", "bar", "black", "deaf", "music", "settings")
table = {}     # sequence -> {"runs": n, fault: count}
notes = []     # every fault, said once
lib_size = 0


def sh(cmd):
    return adb("shell", cmd)


def turn(r):
    sh("settings put system accelerometer_rotation 0")
    sh(f"settings put system user_rotation {r}")


def key(k):
    sh(f"input keyevent {k}")


def start(wait=False):
    adb("shell", "am", "start", *(["-W"] if wait else []), "-n", ACT)


def alive():
    return adb("shell", "pidof", PKG).strip() != ""


def raw_items():
    return run_as("cat files/Songate/items.txt 2>/dev/null")


def position():
    m = re.search(r"pos (-?[\d.]+) playing (\d)", raw_items())
    return (float(m[1]), m[2] == "1") if m else (-1.0, False)


def shot(path):
    with open(path, "wb") as f:
        f.write(subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout)


def bands(path):
    """How many pixels of pure black run along each edge (left, top, right, bottom): devturn.py's."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    col = lambda x: all(max(px[x, y]) <= 2 for y in range(0, h, 7))
    row = lambda y: all(max(px[x, y]) <= 2 for x in range(0, w, 7))
    l = next((x for x in range(w) if not col(x)), w)
    r = next((x for x in range(w) if not col(w - 1 - x)), w)
    t = next((y for y in range(h) if not row(y)), h)
    b = next((y for y in range(h) if not row(h - 1 - y)), h)
    return (w, h), (l, t, r, b)


def at_page(s):
    """A page with its bar of destinations (upright) or its sidebar, nothing over it."""
    return ("ph.nav" in s.items or "nav.now" in s.items) and not any(r == "Modal" for r in s.region.values())


def wait_ui(secs=20):
    """The app in front and drawing: a frame's controls written since this began (a page, a sheet, Up next, the Canvas with its
    controls away: each is the app drawing)."""
    run_as("rm -f files/Songate/items.txt")
    end = time.time() + secs
    while time.time() < end:
        s = clear_notices(screen())
        if s.w > 0 and (s.items or s.texts):
            return s
        time.sleep(0.4)
    return None


def close_layers():
    """Back, until a page is all there is: a sheet, Up next (kept open through a turn), the Canvas (its controls may be away)."""
    for _ in range(6):
        if at_page(screen()):
            return
        key(4)
        time.sleep(1.2)


def ensure_playing():
    s = wait_ui(15)
    if not s:
        return False
    close_layers()
    for _ in range(4):   # (a press during the opening is lost: again)
        if position()[1]:
            return True
        if "ph.nav" in screen().items:
            tap_item("ph.nav")
            time.sleep(0.8)
            tap_item("ph.play")
        else:
            tap_item("pb.play")
        for _ in range(6):
            time.sleep(0.5)
            if position()[1]:
                return True
    return False


def log_mark():
    adb("logcat", "-c")
    adb("logcat", "-b", "crash", "-c")


def log_faults():
    """(crashes, ANRs, lines) in logcat since log_mark, for Songate's process."""
    out = adb("logcat", "-d", "-b", "main,system,crash", "-v", "brief")
    crashes, anrs, lines = 0, 0, []
    for ln in out.splitlines():
        if "FATAL EXCEPTION" in ln or "Fatal signal" in ln or "destroyed mutex" in ln or ("Abort message" in ln):
            lines.append(ln.strip()[:220])
            if "FATAL EXCEPTION" in ln or "Fatal signal" in ln:
                crashes += 1
        elif "ANR in " + PKG in ln:
            anrs += 1
            lines.append(ln.strip()[:220])
    # a tombstone written for Songate (the abort at its end wrote one each time, with no dialog)
    if PKG in out and re.search(r"Tombstone written to", out) and crashes == 0:
        crashes += 1
        lines.append("a tombstone written")
    return crashes, anrs, lines


def fault(seq, kind, what, frame=None):
    table[seq][kind] += 1
    line = f"{seq} #{table[seq]['runs']}: {kind}: {what}"
    notes.append(line)
    print("      FAULT " + line, flush=True)
    if frame and os.path.exists(frame):
        keep = os.path.join(OUT, f"fault-{seq}-{table[seq]['runs']}-{kind}.png")
        try:
            os.replace(frame, keep)
        except OSError:
            pass


def check(seq, expect_alive=True, music=True):
    """What must be true after a run of a sequence. expect_alive None: either is fine (it must only be clean)."""
    t = table[seq]
    t["runs"] += 1
    up = alive()
    crashes, anrs, lines = log_faults()
    if crashes:
        fault(seq, "crash", "; ".join(lines[:3]))
    if anrs:
        fault(seq, "anr", "; ".join(lines[:2]))
    if expect_alive is False:
        if up:
            fault(seq, "died", "still running where it should have ended")
        return
    if expect_alive and not up:
        fault(seq, "died", "the process is gone")
    if not up:
        start(True)   # gone where either is fine: it must start again
    start()
    s = wait_ui(20)
    if not s:
        p = os.path.join(OUT, "tmp-deaf.png")
        shot(p)
        fault(seq, "deaf", "no page came up in 20 s", p)
        return
    # the screen: two looks
    time.sleep(0.8)
    a = os.path.join(OUT, "tmp-a.png")
    shot(a)
    time.sleep(2.0)
    b = os.path.join(OUT, "tmp-b.png")
    shot(b)
    (size, ba), (_, bb) = bands(a), bands(b)
    if all(v >= min(size) for v in bb):
        fault(seq, "black", f"all black 2.8 s after, {size[0]}x{size[1]}", b)
    elif any(v >= 8 for v in bb):
        fault(seq, "bar", f"black l,t,r,b = {bb} of {size[0]}x{size[1]}", b)
    # it answers a tap: another page
    close_layers()
    s = clear_notices(screen())
    phone = "ph.nav" in s.items
    first, second = ("ph.nav#1", "ph.nav") if phone else ("nav.library", "nav.now")
    before = sorted(s.items)
    answered = False
    for target in (first, second, first):
        tap_item(target)
        time.sleep(1.0)
        now = sorted(screen().items)
        if now != before:
            answered = True
            break
    if not answered:
        p = os.path.join(OUT, "tmp-deaf.png")
        shot(p)
        fault(seq, "deaf", "a tap on the bar changed nothing", p)
    # the music
    if music:
        st = session_state()
        p0, playing = position()
        time.sleep(1.5)
        p1, playing1 = position()
        if st not in ("PLAYING", "PAUSED") and (playing1 or st != "none"):   # (none: nothing played since it started)
            fault(seq, "music", f"Android's session says {st}")
        elif playing and playing1 and not (p1 > p0 or p1 < 1.5):
            fault(seq, "music", f"playing but its place stays at {p0:.2f}")
        elif (st == "PLAYING") != playing1 and (st == "PLAYING") != position()[1]:
            fault(seq, "music", f"the session says {st}, the app playing={playing1}")
    # the settings
    cfg = run_as("cat files/Songate/settings.txt")
    size = run_as("wc -c < files/Songate/library.tsv").strip()
    if "uiSize=" not in cfg or cfg.count("\n") < 60:
        fault(seq, "settings", f"settings.txt has {cfg.count(chr(10))} lines")
    if lib_size and size != lib_size:
        fault(seq, "settings", f"library.tsv is {size} bytes, was {lib_size}")


def fresh(way=0):
    """Closed, to start held that way. The turn is set with the app still in front: Android keeps the turn an app was last shown
    with and puts it back when one opens from the home screen, whatever was set meanwhile (seen 2026-11-11)."""
    if not alive():
        start(True)
        time.sleep(1.5)
    else:
        start()
    turn(way)
    time.sleep(1.2)
    sh(f"am force-stop {PKG}")
    time.sleep(0.4)


# ---------------------------------------------------------------- the sequences (each: name -> a function that runs it and checks)

def cold_turn(seq):
    """A cold start, turned 0.1 to 2 s after it was asked for; from upright and from sideways."""
    for way in (0, 1):
        for d in (0.1, 0.3, 0.6, 1.0, 2.0):
            fresh(way)
            log_mark()
            start()
            time.sleep(d)
            turn(1 - way)
            time.sleep(3)
            check(seq)
            turn(way)
            time.sleep(1.5)


def flip_ten(seq):
    """Turned back and forth ten times, fast; then the other way round too (1 to 3: no upright between)."""
    for plan in ((1, 0) * 5, (1, 3) * 5, (0, 1, 3, 0, 3, 1, 0, 1, 3, 0)):
        ensure_playing()
        log_mark()
        for r in plan:
            turn(r)
            time.sleep(0.15)
        turn(0)
        time.sleep(2)
        check(seq)


def start_home(seq):
    """Home pressed while it starts, then opened again."""
    for d in (0.1, 0.3, 0.6, 1.0, 2.0):
        fresh()
        log_mark()
        start()
        time.sleep(d)
        key(3)
        time.sleep(1.5)
        start()
        check(seq)


def start_back(seq):
    """Back pressed while it starts (it may leave: then it must start again)."""
    for d in (0.1, 0.3, 0.6, 1.0, 2.0):
        fresh()
        log_mark()
        start()
        time.sleep(d)
        key(4)
        time.sleep(1.5)
        check(seq, expect_alive=None)


def kill_loop(seq):
    """Stopped by force and started again, twenty times, at any moment of its start; the last four turned as well."""
    log_mark()
    for n in range(20):
        sh(f"am force-stop {PKG}")
        start()
        time.sleep(random.choice((0.2, 0.5, 0.9, 1.5, 2.5, 4.0)))
        if n >= 16:
            turn(n % 2)
        if n % 5 == 4:
            check(seq)
            log_mark()
    turn(0)


def start_twice(seq):
    """Started while already running: from in front, from home, five times in a row."""
    ensure_playing()
    for plan in ("front", "home", "burst"):
        log_mark()
        if plan == "home":
            key(3)
            time.sleep(1)
        for _ in range(5 if plan == "burst" else 1):
            start()
        time.sleep(1.5)
        check(seq)


def in_recents():
    out = sh("dumpsys activity recents")
    return f"A={PKG}" in out or f"{PKG}/" in out


def swipe_away():
    w, h = [int(v) for v in re.search(r"(\d+)x(\d+)\s*$", sh("wm size").strip()).groups()]
    for _ in range(3):
        key(187)
        time.sleep(2)
        sh(f"input swipe {w // 2} {int(h * 0.55)} {w // 2} {int(h * 0.06)} 150")
        time.sleep(3)
        if not in_recents():
            break
    key(3)
    time.sleep(1)


def set_keep(on):
    """Settings › Playback › Keep playing when swiped away, in the file, the app closed."""
    sh(f"am force-stop {PKG}")
    run_as(f"sed -i s/^swipeKeep=.*/swipeKeep={1 if on else 0}/ files/Songate/settings.txt")


def swipe_keep(seq):
    """Swiped away from the recent apps while playing, the switch on: it plays on, and its screen comes back."""
    set_keep(True)
    for _ in range(3):
        start(True)
        ensure_playing()
        log_mark()
        swipe_away()
        time.sleep(1)
        if session_state() != "PLAYING":
            fault(seq, "music", f"swiped away with the switch on: the session says {session_state()}")
        check(seq)


def swipe_end(seq):
    """... the switch off: it ends with its music, cleanly, and starts again."""
    set_keep(False)
    for _ in range(3):
        start(True)
        ensure_playing()
        log_mark()
        swipe_away()
        time.sleep(2)
        check(seq, expect_alive=False)
        log_mark()
        start(True)
        check(seq)
    set_keep(True)


def sheet_turn(seq):
    """Turned as a sheet opens (the song's menu, Up next, the sleep timer), and as it closes."""
    for name in ("ph.more", "ph.queue", "ph.sleep", "ph.devices"):
        fresh()
        start(True)
        ensure_playing()
        tap_item("ph.nav")
        time.sleep(0.8)
        log_mark()
        tap_item(name)
        turn(1)
        time.sleep(1.5)
        key(4)
        turn(0)
        time.sleep(2)
        check(seq)


def page_turn(seq):
    """Turned as the page changes, each tab of the bar in turn."""
    ensure_playing()
    for n in range(8):
        log_mark()
        s = screen()
        tabs = [k for k in s.items if k.startswith("ph.nav")] or [k for k in ("nav.now", "nav.library", "nav.albums", "nav.playlists", "nav.settings") if k in s.items]
        if tabs:
            tap_item(tabs[n % len(tabs)])
        turn((n + 1) % 2)
        time.sleep(1.2)
        if n % 2 == 1:
            check(seq)
    turn(0)
    time.sleep(1.5)


def canvas_turn(seq):
    """The Canvas button upright (the screen turns for it), then at once: a turn, home, back, the button again."""
    for then in ("turn", "home", "back", "twice", "home-late", "back-late"):
        fresh()
        start(True)
        ensure_playing()
        tap_item("ph.nav")
        time.sleep(0.8)
        log_mark()
        tap_item("ph.canvas")
        if then == "turn":
            turn(1)
            time.sleep(0.4)
            turn(0)
        elif then == "home":
            key(3)
            time.sleep(1.5)
            start()
        elif then == "back":
            key(4)
        elif then == "twice":
            tap_item("ph.canvas")
        elif then == "home-late":
            time.sleep(0.7)
            key(3)
            time.sleep(1.5)
            start()
        elif then == "back-late":
            time.sleep(0.7)
            key(4)
        time.sleep(4)
        check(seq, expect_alive=None)
        turn(0)
        time.sleep(1.5)
        r = sh("dumpsys window displays")
        if "mRotation=1" in r or "ROTATION_90" in r.split("mCurrentRotation")[-1][:20]:
            close_layers()
            time.sleep(2.5)


def back_spam(seq):
    """Back, thirty times as fast as it goes, from Settings' deepest page and from Now playing (it may leave: cleanly)."""
    for origin in ("settings", "now", "sheet"):
        fresh()
        start(True)
        ensure_playing()
        if origin == "settings":
            tap_item("ph.nav#4")
            time.sleep(0.8)
        elif origin == "sheet":
            tap_item("ph.nav")
            time.sleep(0.6)
            tap_item("ph.more")
        log_mark()
        sh("for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do input keyevent 4; done")
        time.sleep(2)
        check(seq, expect_alive=None)


def tab_spam(seq):
    """The bar's tabs pressed sixty times, no waiting; then the same with a turn in the middle."""
    for with_turn in (False, True):
        ensure_playing()
        s = clear_notices(screen())
        tabs = [s.items[k] for k in sorted(s.items) if k.startswith("ph.nav")]
        if not tabs:
            tabs = [s.items[k] for k in ("nav.now", "nav.library", "nav.albums", "nav.playlists", "nav.settings") if k in s.items]
        pts = [(int((x + w / 2) * s.scale), int((y + h / 2) * s.scale)) for x, y, w, h in tabs]
        log_mark()
        order = [pts[random.randrange(len(pts))] for _ in range(60)]
        sh("; ".join(f"input tap {x} {y}" for x, y in order[:30]))
        if with_turn:
            turn(1)
        sh("; ".join(f"input tap {x} {y}" for x, y in order[30:]))
        turn(0)
        time.sleep(2)
        check(seq)


def size_changes(seq):
    """The window's size and density changed under it while it runs (a fold opened, split screen, Display size)."""
    ensure_playing()
    plans = [("720x1280", 320), ("1768x2208", 420), ("904x2316", 420), ("1600x2560", 320), ("1080x2400", 540), ("1080x1920", 420), ("1536x2048", 320), ("1080x2640", 356)]
    try:
        for n, (size, dens) in enumerate(plans):
            log_mark()
            sh(f"wm size {size}")
            time.sleep(0.2 if n % 2 else 1.0)
            sh(f"wm density {dens}")
            if n % 3 == 2:
                turn(1)
            time.sleep(2.5)
            check(seq)
            turn(0)
    finally:
        sh("wm size reset")
        sh("wm density reset")
        turn(0)
        time.sleep(2.5)
    log_mark()
    check(seq)


def screen_off(seq):
    """The screen off and on (the phone locked), playing; off as a sheet opens; off while turned."""
    for plan in ("plain", "sheet", "sideways", "quick"):
        ensure_playing()
        log_mark()
        if plan == "sheet":
            tap_item("ph.nav")
            time.sleep(0.6)
            tap_item("ph.more")
        if plan == "sideways":
            turn(1)
            time.sleep(1.2)
        key(223)   # sleep
        time.sleep(0.3 if plan == "quick" else 4)
        if session_state() != "PLAYING":
            fault(seq, "music", f"the screen off: the session says {session_state()}")
        key(224)   # wake
        time.sleep(0.8)
        sh("wm dismiss-keyguard")
        key(82)
        time.sleep(1.5)
        check(seq)
        turn(0)
        time.sleep(1)


def trim_memory(seq):
    """Android asking for memory back, in front and out of sight, each level."""
    ensure_playing()
    for level in ("RUNNING_MODERATE", "RUNNING_LOW", "RUNNING_CRITICAL"):
        log_mark()
        sh(f"am send-trim-memory {PKG} {level}")
        time.sleep(1.5)
        check(seq)
    for level in ("UI_HIDDEN", "BACKGROUND", "MODERATE", "COMPLETE"):
        log_mark()
        key(3)
        time.sleep(1.2)
        sh(f"am send-trim-memory {PKG} {level}")
        time.sleep(2)
        if session_state() != "PLAYING":
            fault(seq, "music", f"out of sight, {level}: the session says {session_state()}")
        check(seq)


def monkey(seq):
    """Android's monkey: 3000 presses, drags, backs and turns anywhere in the app, 30 ms apart, four seeds. The network is off."""
    sh("svc wifi disable; svc data disable")
    try:
        for seed in (11, 2026, 77, 4242):
            fresh()
            start(True)
            ensure_playing()
            log_mark()
            out = sh(f"monkey -p {PKG} -s {seed} --throttle 30 --pct-syskeys 0 --pct-anyevent 0 --pct-appswitch 2 --pct-rotation 4 "
                     f"--pct-touch 55 --pct-motion 25 --pct-nav 6 --pct-majornav 6 --pct-trackball 0 --pct-pinchzoom 2 --pct-flip 0 "
                     f"--ignore-timeouts --ignore-security-exceptions --monitor-native-crashes -v 3000")
            done = re.search(r"Events injected: (\d+)", out)
            bad = [ln for ln in out.splitlines() if "CRASH" in ln or "NOT RESPONDING" in ln or "aborted" in ln.lower()]
            print(f"      seed {seed}: {done[1] if done else '?'} events" + (f"; {bad[:2]}" if bad else ""), flush=True)
            if bad:
                fault(seq, "crash", f"monkey seed {seed}: {bad[0][:160]}")
            turn(0)
            sh("wm dismiss-keyguard")
            time.sleep(2)
            # whatever it left open (the typing card, a sheet, the tour): closed before the look
            for _ in range(4):
                key(4)
                time.sleep(0.4)
            check(seq, expect_alive=None, music=False)
    finally:
        sh("svc wifi enable; svc data enable")
        turn(0)


SEQS = {"cold-turn": cold_turn, "flip-ten": flip_ten, "start-home": start_home, "start-back": start_back, "kill-loop": kill_loop,
        "start-twice": start_twice, "swipe-keep": swipe_keep, "swipe-end": swipe_end, "sheet-turn": sheet_turn, "page-turn": page_turn,
        "canvas-turn": canvas_turn, "back-spam": back_spam, "tab-spam": tab_spam, "size-changes": size_changes, "screen-off": screen_off,
        "trim-memory": trim_memory, "monkey": monkey}


def music_files():
    return sh("find /storage/*/Music /storage/emulated/0/Music -type f 2>/dev/null | sort").splitlines()


def main():
    global lib_size
    random.seed(16)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    sh(f"am force-stop {PKG}")
    files_before = music_files()
    run_as(f"mkdir -p files/aside; cp -r files/Songate files/aside/before-devstress-{stamp}; touch files/Songate/test-items")
    lib_size = run_as("wc -c < files/Songate/library.tsv").strip()
    try:
        start(True)
        time.sleep(1.5)
        turn(0)
        time.sleep(4)
        print("playing at the start:", ensure_playing(), flush=True)
        for name, fn in SEQS.items():
            if ONLY and name not in ONLY:
                continue
            table[name] = dict.fromkeys(("runs",) + FAULTS, 0)
            print(f"{name}: {fn.__doc__.strip().splitlines()[0]}", flush=True)
            try:
                fn(name)
            except Exception as e:   # the script's own fault: said, and the next sequence all the same
                notes.append(f"{name}: the script failed: {e!r}")
                print("      script:", repr(e), flush=True)
            t = table[name]
            print(f"   {t['runs']} runs; " + ", ".join(f"{k} {t[k]}" for k in FAULTS), flush=True)
    finally:
        sh("wm size reset; wm density reset; svc wifi enable; svc data enable")
        start(True)   # (the turn is put back with the app in front: fresh())
        time.sleep(1.5)
        turn(0)
        time.sleep(1.5)
        sh(f"am force-stop {PKG}")
        # the settings as they were
        run_as(f"[ -d files/aside/before-devstress-{stamp} ] && rm -rf files/Songate && mv files/aside/before-devstress-{stamp} files/Songate")
        gone = sorted(set(files_before) - set(music_files()))
        lines = [f"{'sequence':14} {'runs':>4} " + " ".join(f"{k:>8}" for k in FAULTS)]
        for name, t in table.items():
            lines.append(f"{name:14} {t['runs']:4} " + " ".join(f"{t[k]:8}" for k in FAULTS))
        lines.append("")
        lines.append("died: the process gone (or still there) against what the sequence expects; crash: FATAL / Fatal signal / tombstone in logcat;")
        lines.append("anr: not responding; bar: a black band along an edge; black: all black 2.8 s after; deaf: no answer to a tap;")
        lines.append("music: the session or the song's place wrong; settings: settings.txt or the library's list damaged")
        lines.append("")
        lines.extend(notes or ["no faults"])
        lines.append("")
        lines.append(f"music files: {len(files_before)} before" + (f", {len(gone)} GONE: {gone[:5]}" if gone else ", none gone"))
        text = "\n".join(lines)
        with open(os.path.join(OUT, f"results-{stamp}.txt"), "w", encoding="utf-8") as f:
            f.write(text + "\n")
        for tmp in ("tmp-a.png", "tmp-b.png", "tmp-deaf.png"):
            try:
                os.remove(os.path.join(OUT, tmp))
            except OSError:
                pass
        print("\n" + text)
        total = sum(t[k] for t in table.values() for k in FAULTS)
        print("\nSTRESS FAULTS:", total)


main()
