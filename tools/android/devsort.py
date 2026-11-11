"""Songate for Android, dev only: every list that can be put in order by hand, sorted by touch (the menu's Move after a finger: the
list's sort mode, a drag by the handle or of a whole tile, Done) with asserts and a frame at each step.
   python tools/android/devsort.py <out folder> [upright|sideways] [part ...]
Parts (all but server when none is named): upnext, page (the Playlist page: rows, then tiles), playlists (the Playlists page and a
playlist of yours: three made here and removed), restart (the saved order after the app is closed), server (the server's playlists
and one of its playlists' songs: needs a server signed in; a song moved there is moved back).
The settings folder is the device's own: what the test changes it puts back at the end (the session and its queues are saved first
and restored; the playlists it makes it removes; the server playlists' order file is removed if there was none). Emulator only:
SONGATE_SERIAL picks the device, emulator-5554 unless given."""
import io
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import ADB, PKG, SERIAL, adb, run_as, screen, tap_at, tap_item, tap_text, wait, step, results, clear_notices   # noqa: E402

OUT = sys.argv[1]
TURN = sys.argv[2] if len(sys.argv) > 2 else "upright"
PARTS = sys.argv[3:] or ["upnext", "page", "playlists", "restart"]
UP = TURN == "upright"
PRE = "a-" if UP else "as-"   # the frames' names: Android upright, Android sideways
os.makedirs(OUT, exist_ok=True)
KEEP = ["session.txt", "server-queue.tsv", "settings.txt", "playlists.txt", "lists.txt", "server-playlist-order.tsv"]
kept = {}
finger = [0, 0]


def read_file(name):
    r = subprocess.run([ADB, "-s", SERIAL, "exec-out", f"run-as {PKG} sh -c 'cat files/Songate/{name} 2>/dev/null; echo; echo __END__$?'"], capture_output=True)
    body, _, tail = r.stdout.rpartition(b"\n__END__")
    return body if tail.strip() == b"0" else None   # the file as it is, to the byte (the echo's own newline went with the mark); None: no such file


def write_file(name, data):
    subprocess.run([ADB, "-s", SERIAL, "exec-in", f"run-as {PKG} sh -c 'cat > files/Songate/{name}'"], input=data, capture_output=True)


def shot(label):
    png = subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout
    try:
        from PIL import Image
        Image.open(io.BytesIO(png)).convert("RGB").save(os.path.join(OUT, PRE + label + ".jpg"), quality=88)
    except ImportError:
        open(os.path.join(OUT, PRE + label + ".png"), "wb").write(png)


def turn():
    want = "0" if UP else "1"
    for _ in range(20):
        adb("shell", "settings", "put", "system", "user_rotation", want)
        if f"mRotation={want}" in adb("shell", "dumpsys", "window", "displays"):
            break
        time.sleep(0.5)
    time.sleep(1.5)


def start():
    adb("shell", "am", "force-stop", PKG)
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    turn()
    wait(lambda s: "ph.nav" in s.items or "nav.now" in s.items, 20)
    time.sleep(4)   # the opening


def hold(name):
    s = clear_notices(screen())
    if name not in s.items:
        return False
    x, y, w, h = s.items[name]
    px, py = str(int((x + min(w / 2, 120)) * s.scale)), str(int((y + h / 2) * s.scale))
    adb("shell", "input", "swipe", px, py, px, py, "800")
    time.sleep(0.9)
    return True


def names(prefix, s=None):
    """The controls of that list on screen, top to bottom, left to right."""
    s = s or screen()
    return [k for _, _, k in sorted((v[1], v[0], k) for k, v in s.items.items() if k == prefix or k.startswith(prefix + "#"))]


def finger_down(name, s=None):
    s = s or screen()
    x, y, w, h = s.items[name]
    finger[0], finger[1] = (x + w / 2) * s.scale, (y + h / 2) * s.scale
    adb("shell", "input", "motionevent", "DOWN", str(int(finger[0])), str(int(finger[1])))
    time.sleep(0.25)


def finger_move(dx, dy, steps=8):
    """By dx, dy of the app's own pixels, in steps, the finger staying down."""
    k = screen().scale
    x0, y0 = finger
    for i in range(1, steps + 1):
        adb("shell", "input", "motionevent", "MOVE", str(int(x0 + dx * k * i / steps)), str(int(y0 + dy * k * i / steps)))
    finger[0], finger[1] = x0 + dx * k, y0 + dy * k
    time.sleep(0.5)


def finger_up():
    adb("shell", "input", "motionevent", "UP", str(int(finger[0])), str(int(finger[1])))
    time.sleep(0.8)


def words_at(s, name):
    """The first words drawn inside a control's box (a row's or a tile's name)."""
    x, y, w, h = s.items[name]
    inside = sorted((ty, tx, t) for t, tx, ty, tw, th in s.texts if tx >= x - 2 and tx < x + w and ty >= y - 2 and ty < y + h and len(t) > 1)
    return inside[0][2] if inside else ""


def move_from_menu(row):
    """The row's menu by a long touch, then Move: the list's sort mode."""
    ok = hold(row)
    time.sleep(0.4)
    ok = ok and tap_text("Move")
    time.sleep(1.2)
    return ok


def done():
    ok = tap_item("sort.done")
    time.sleep(0.8)
    return ok


def titles(rows, s=None):
    s = s or screen()
    return [words_at(s, r) for r in rows if r in s.items]


def sort_rows(label, row_prefix, grip_prefix, pitch, which=1, by=1.6):
    """A list of rows: Move on a row, the handles show, one dragged past the next, lifted, Done. Returns the names before and after."""
    rows = names(row_prefix)
    row = rows[which] if len(rows) > which else None
    step(f"{label}: Move in the row's menu", row and move_from_menu(row))
    s = screen()
    grips = names(grip_prefix, s)
    step(f"{label}: sort mode shows a handle on the rows and Done", len(grips) >= 2 and "sort.done" in s.items, f"{len(grips)} handles")
    shot(label + "-1-sort")
    if len(grips) < 2:
        return [], []
    before = titles(names(row_prefix, s), s)
    track = s.track
    finger_down(grips[0], s)
    finger_move(0, pitch * by)
    shot(label + "-2-held")
    finger_up()
    time.sleep(0.6)
    s = screen()
    after = titles(names(row_prefix, s), s)
    shot(label + "-3-dropped")
    step(f"{label}: the row dragged changed places with the next", before[:4] != after[:4] and sorted(before[:4]) == sorted(after[:4]), f"{before[:4]} -> {after[:4]}")
    step(f"{label}: the song playing is the same", s.track == track, f"{track} -> {s.track}")
    return before, after


def end_by_tap(label, row_prefix, grip_prefix):
    """A tap on a row in sort mode ends the mode and plays nothing."""
    s = screen()
    track, rows = s.track, names(row_prefix, s)
    if rows:
        x, y, w, h = s.items[rows[min(3, len(rows) - 1)]]
        tap_at(s, x + 60, y + h / 2)
    time.sleep(1.2)
    s = screen()
    step(f"{label}: a tap elsewhere ends sort mode, plays nothing", not names(grip_prefix, s) and "sort.done" not in s.items and s.track == track,
         f"{len(names(grip_prefix, s))} handles, {track} -> {s.track}")


def play_local():
    """A local queue to sort: Library on Local, Songs, a song played; then Now playing."""
    nav = "ph.nav" if UP else "nav.now"
    for _ in range(4):   # (a first tap can be lost to the opening)
        if UP:
            tap_item("ph.nav#1")
            time.sleep(1)
            tap_item("lib.source[0]")
            time.sleep(1.2)
            tap_item("ph.lrow")
        else:
            tap_item("lib.source[0]")
            time.sleep(1.2)
            tap_item("nav.library")
            time.sleep(1.2)
            if "trk.r#2" not in screen().items:
                tap_text("Songs")
        time.sleep(1.5)
        if "trk.r#2" in screen().items:
            break
        adb("shell", "input", "keyevent", "4")
        time.sleep(1)
    ok = tap_item("trk.r#2")
    time.sleep(2.5)
    s = screen()
    step("a local song plays (a queue of this device's songs)", ok and s.playing, s.track)
    tap_item(nav)
    time.sleep(1.2)


def part_upnext():
    if UP:
        tap_item("ph.queue")
        time.sleep(1.2)
        row, grip, pitch = "ph.qrow", "ph.qsort", 64
    else:
        tap_item("np.tab")
        time.sleep(0.8)
        row, grip, pitch = "np.next", "np.sort", 52
    shot("upnext-0")
    sort_rows("upnext", row, grip, pitch, which=2 if UP else 1)
    step("upnext: Done ends sort mode", done() and not names(grip))
    shot("upnext-4-done")
    # again: ended by a tap on a row, then by back
    rows = names(row)
    step("upnext: Move again", len(rows) > 2 and move_from_menu(rows[2]) and len(names(grip)) >= 2)
    end_by_tap("upnext", row, grip)
    rows = names(row)
    if len(rows) > 2 and move_from_menu(rows[2]):
        adb("shell", "input", "keyevent", "4")
        time.sleep(1)
        s = screen()
        step("upnext: back ends sort mode and stays on the page", not names(grip, s) and bool(names(row, s)))
    if UP:
        tap_item("ph.qclose")
        time.sleep(0.8)


def drag_onto(a, b, label, extra=(0, 0)):
    """A finger down on control a, moved to the middle of control b (and a little past it), a frame, lifted."""
    s = screen()
    ax, ay, aw, ah = s.items[a]
    bx, by, bw, bh = s.items[b]
    finger_down(a, s)
    finger_move(bx + bw / 2 - (ax + aw / 2) + extra[0], by + bh / 2 - (ay + ah / 2) + extra[1])
    shot(label + "-2-held")
    finger_up()
    time.sleep(0.6)
    shot(label + "-3-dropped")


def open_page():
    if UP:
        tap_item("ph.nav")
        time.sleep(1)
        tap_item("ph.queue")
        time.sleep(1.2)
        tap_item("ph.qpage")
    else:
        tap_item("nav.queue")
    time.sleep(1.5)


def part_page():
    open_page()
    tap_item("pl.view#1")   # with covers
    time.sleep(0.8)
    shot("page-0")
    sort_rows("page", "pl.r", "pl.sort", 60, which=2)
    # held near the foot of a long list, the list moves under the row by itself; back at the top edge it comes back
    s = screen()
    grips = names("pl.sort", s)
    if len(grips) >= 2:
        first = titles(names("pl.r", s), s)[:1]
        x, y, w, h = s.items[grips[1]]
        foot = (s.h - (150 if UP else 100)) - (y + h / 2)   # just over the mini player upright, the player bar sideways
        finger_down(grips[1], s)
        finger_move(0, foot)
        time.sleep(2.0)
        shot("page-scroll-2-held-at-the-foot")
        now = screen()
        step("page: a row held near the foot scrolls the list by itself", titles(names("pl.r", now), now)[:1] != first, f"{first}")
        finger_move(0, (68 if UP else 60) - (y + h / 2 + foot))   # to the list's top edge, under the top bar
        time.sleep(4.5)
        finger_up()
        time.sleep(0.8)
        now = screen()
        step("page: held near the top it scrolls back, the row with it (the page's head shows again)", "pl.play" in now.items and titles(names("pl.r", now), now)[:1] != first,
             f"{first} / {titles(names('pl.r', now), now)[:1]}")
    step("page: Done ends sort mode", done() and not names("pl.sort"))
    # tiles: the whole tile drags, any way
    tap_item("pl.view#2")
    time.sleep(1)
    rows = names("pl.r")
    step("page tiles: Move in a tile's menu", len(rows) > 2 and move_from_menu(rows[2]))
    s = screen()
    grips = names("pl.sort", s)
    step("page tiles: sort mode, every tile takes the finger, Done shows", len(grips) >= 3 and "sort.done" in s.items, f"{len(grips)}")
    shot("page-tiles-1-sort")
    if len(grips) >= 3:
        before = titles(names("pl.r", s), s)
        track = s.track
        drag_onto(grips[0], grips[1], "page-tiles", extra=(10, 0))
        s = screen()
        after = titles(names("pl.r", s), s)
        step("page tiles: the tile dragged sideways changed places", after[:2] == [before[1], before[0]], f"{before[:3]} -> {after[:3]}")
        grips = names("pl.sort", s)
        drag_onto(grips[0], grips[2], "page-tiles-down", extra=(0, 10))   # … and down a row (two across upright)
        s = screen()
        after2 = titles(names("pl.r", s), s)
        step("page tiles: a tile dragged to another row changed places", after2[:3] == [after[1], after[2], after[0]], f"{after[:4]} -> {after2[:4]}")
        step("page tiles: the song playing is the same", s.track == track)
    step("page tiles: Done ends sort mode", done() and not names("pl.sort"))
    tap_item("pl.view#1")
    time.sleep(0.8)


LISTS = ["SortA", "SortB", "SortC"]


def open_playlists(source=0):
    """The Playlists page, on Local (0) or Server (1)."""
    for _ in range(5):   # (a first tap can be lost to the opening; a sheet or a page in the way is left with back)
        s = screen()
        if any(r == "Modal" for r in s.region.values()):
            adb("shell", "input", "keyevent", "4")
            time.sleep(0.8)
        if UP:
            tap_item("ph.nav#3")
        else:
            tap_item("nav.playlists")
        time.sleep(1.2)
        if "pls.new" in screen().items:
            break
        adb("shell", "input", "keyevent", "4")
        time.sleep(0.8)
    tap_item(f"lib.source[{source}]")
    time.sleep(1.5 if source else 1)


def list_names(s=None):
    s = s or screen()
    return [t for t in titles(names("pls.item", s), s) if t in LISTS]


def part_playlists():
    open_playlists()
    for n in LISTS:   # three playlists of your own (removed at the end)
        if n in list_names():
            continue
        tap_item("pls.new")
        time.sleep(0.8)
        tap_text("A playlist of your own")
        time.sleep(0.8)
        adb("shell", "input", "text", n)
        time.sleep(0.4)
        adb("shell", "input", "keyevent", "66")
        time.sleep(1.2)
        open_playlists()
    tap_item("pls.view#2")
    time.sleep(0.8)
    before = list_names()
    step("playlists: three made", before == LISTS, str(before))
    shot("playlists-0")
    items = [k for k in names("pls.item") if k != "pls.item#-1"]
    step("playlists: Move in a tile's menu", len(items) >= 3 and move_from_menu(items[1]))
    s = screen()
    grips = names("pls.sort", s)
    step("playlists: sort mode, the tiles take the finger (Liked does not), Done shows", len(grips) == len(items) and "sort.done" in s.items, f"{len(grips)} of {len(items)}")
    shot("playlists-1-sort")
    if len(grips) >= 3:
        drag_onto(grips[0], grips[2], "playlists", extra=(0, 8))   # the first to the third's place: across and down
        after = list_names()
        step("playlists: a tile dragged across the grid took the third place", after == [LISTS[1], LISTS[2], LISTS[0]], str(after))
    step("playlists: Done ends sort mode", done() and not names("pls.sort"))
    # the row views: a handle in the chevron's place
    tap_item("pls.view#1")
    time.sleep(0.8)
    items = [k for k in names("pls.item") if k != "pls.item#-1"]
    step("playlists rows: Move in a row's menu", len(items) >= 3 and move_from_menu(items[1]))
    s = screen()
    grips = names("pls.sort", s)
    step("playlists rows: a handle on each row but Liked", len(grips) == len(items), f"{len(grips)} of {len(items)}")
    shot("playlists-rows-1-sort")
    if len(grips) >= 3:
        was = list_names(s)
        drag_onto(grips[2], grips[0], "playlists-rows", extra=(0, -8))   # the last to the first's place
        now = list_names()
        step("playlists rows: the row dragged up is first", now == [was[2], was[0], was[1]], f"{was} -> {now}")
    step("playlists rows: Done ends sort mode", done() and not names("pls.sort"))
    tap_item("pls.view#2")
    time.sleep(0.8)
    kept["order"] = list_names()
    # a playlist of yours: three songs added, one moved
    if UP:
        tap_item("ph.nav#1")
        time.sleep(1)
        tap_item("ph.lrow")
    else:
        tap_item("nav.library")
        time.sleep(1.2)
        if "trk.r#2" not in screen().items:
            tap_text("Songs")
    time.sleep(1.5)
    for k in (1, 2, 3):
        hold(f"trk.r#{k}")
        time.sleep(0.3)
        tap_text("Add to a playlist")
        time.sleep(0.8)
        s = screen()   # (the sheet's own row: the last drawn of that name; sideways the sidebar has one too)
        mine = [t for t in s.texts if t[0] == "SortA"]
        if mine:
            tap_at(s, mine[-1][1] + mine[-1][3] / 2, mine[-1][2] + mine[-1][4] / 2)
        time.sleep(1)
        if any(r == "Modal" for r in screen().region.values()):
            adb("shell", "input", "keyevent", "4")
            time.sleep(0.8)
    open_playlists()
    tap_text("SortA")
    time.sleep(1.5)
    tap_item("pl.view#1")
    time.sleep(0.8)
    rows = names("pl.r")
    step("a playlist of yours: three songs in it", len(rows) == 3, str(len(rows)))
    shot("list-0")
    before, after = sort_rows("list", "pl.r", "pl.sort", 60, which=1, by=1.2)
    step("a playlist of yours: Done ends sort mode", done() and not names("pl.sort"))
    kept["songs"] = titles(names("pl.r"))
    shot("list-4-done")


def part_restart():
    """The orders made above, after the app is closed and opened again."""
    if "order" not in kept:
        return
    adb("shell", "input", "keyevent", "3")   # home: what must be saved is saved
    time.sleep(2)
    start()
    open_playlists()
    step("restart: the playlists keep their order", list_names() == kept["order"], f"{list_names()} / {kept['order']}")
    tap_text("SortA")
    time.sleep(1.5)
    now = titles(names("pl.r"))
    step("restart: the playlist's songs keep their order", now == kept["songs"], f"{now} / {kept['songs']}")


def part_server():
    """The server's playlists (their order is Songate's own) and one of its playlists' songs (sent to the server: moved back)."""
    open_playlists(1)
    tap_item("pls.view#2")
    time.sleep(1.5)
    items = names("pls.srv")
    step("server: two playlists there to sort", len(items) >= 2, f"{len(items)}")
    if len(items) < 2:
        return
    shot("srv-playlists-0")
    before = titles(items)
    step("server playlists: Move in a tile's menu", move_from_menu(items[1]))
    s = screen()
    grips = names("pls.srvsort", s)
    step("server playlists: sort mode, the tiles take the finger, Done shows", len(grips) == len(items) and "sort.done" in s.items, f"{len(grips)} of {len(items)}")
    shot("srv-playlists-1-sort")
    if len(grips) >= 2:
        drag_onto(grips[0], grips[1], "srv-playlists", extra=(8, 0))
        after = titles(names("pls.srv"))
        step("server playlists: the tile dragged changed places", after[:2] == [before[1], before[0]], f"{before} -> {after}")
        grips = names("pls.srvsort")
        drag_onto(grips[1], grips[0], "srv-playlists-back", extra=(-8, 0))
        step("server playlists: dragged back, as they were", titles(names("pls.srv")) == before)
    step("server playlists: Done ends sort mode", done() and not names("pls.srvsort"))
    tap_item("pls.view#1")   # the rows: a handle in the chevron's place
    time.sleep(1)
    items = names("pls.srv")
    if move_from_menu(items[1]):
        s = screen()
        grips = names("pls.srvsort", s)
        step("server playlists rows: a handle on each row", len(grips) == len(items), f"{len(grips)} of {len(items)}")
        shot("srv-playlists-rows-1-sort")
        drag_onto(grips[0], grips[1], "srv-playlists-rows", extra=(0, 8))
        step("server playlists rows: the row dragged changed places", titles(names("pls.srv"))[:2] == [before[1], before[0]])
        grips = names("pls.srvsort")
        drag_onto(grips[1], grips[0], "srv-playlists-rows-back", extra=(0, -8))
        step("server playlists rows: dragged back", titles(names("pls.srv")) == before)
        done()
    tap_item("pls.view#2")
    time.sleep(1)
    # a playlist of the server's with three songs or more: one moved, then moved back
    for it in names("pls.srv"):
        tap_item(it)
        time.sleep(2.5)
        tap_item("srv.plview#1")
        time.sleep(1)
        rows = names("srv.plt.r")
        if len(rows) >= 3:
            break
        adb("shell", "input", "keyevent", "4")
        time.sleep(1.2)
    else:
        step("server playlist: one with three songs", False)
        return
    shot("srv-list-0")
    before = titles(rows)
    step("server playlist: Move in the row's menu", move_from_menu(rows[1]))
    s = screen()
    grips = names("srv.plt.sort", s)
    step("server playlist: sort mode shows a handle on the rows and Done", len(grips) >= 3 and "sort.done" in s.items, f"{len(grips)}")
    shot("srv-list-1-sort")
    if len(grips) >= 3:
        before = titles(names("srv.plt.r", s), s)
        drag_onto(grips[0], grips[1], "srv-list", extra=(0, 8))
        after = titles(names("srv.plt.r"))
        step("server playlist: the row dragged changed places", after[:2] == [before[1], before[0]], f"{before[:3]} -> {after[:3]}")
        grips = names("srv.plt.sort")
        drag_onto(grips[1], grips[0], "srv-list-back", extra=(0, -8))
        step("server playlist: dragged back, as it was", titles(names("srv.plt.r"))[:3] == before[:3])
    step("server playlist: Done ends sort mode", done() and not names("srv.plt.sort"))
    time.sleep(1.5)   # (its order goes to the server)
    adb("shell", "input", "keyevent", "4")
    time.sleep(1)
    open_playlists(0)


def remove_lists():
    for n in LISTS:
        open_playlists()
        if n not in list_names():
            continue
        tap_text(n)
        time.sleep(1.2)
        tap_item("pl.remove")
        time.sleep(0.8)
        tap_item("sp.yes")
        time.sleep(1)


def main():
    adb("shell", "settings", "put", "system", "accelerometer_rotation", "0")
    adb("shell", "am", "force-stop", PKG)
    time.sleep(0.5)
    for f in KEEP:
        kept[f] = read_file(f)
    run_as("touch files/Songate/test-items")
    try:
        start()
        if any(p in PARTS for p in ("upnext", "page")):
            play_local()
        for p in PARTS:
            fn = globals().get("part_" + p)
            if fn:
                print(f"-- {p} ({TURN})")
                fn()
        if "playlists" in PARTS:
            remove_lists()
    finally:
        adb("shell", "am", "force-stop", PKG)
        time.sleep(0.6)
        for f in KEEP:
            data = kept[f]
            if data is None:
                run_as(f"rm -f files/Songate/{f}")
            else:
                write_file(f, data)
                if read_file(f) != data:
                    print(f"  NOT RESTORED: {f} (kept in {OUT}/kept-{f})")
                    open(os.path.join(OUT, "kept-" + f), "wb").write(data)
        adb("shell", "settings", "put", "system", "user_rotation", "0")
    bad = [n for n, ok in results if not ok]
    print(("ALL GOOD" if not bad else f"{len(bad)} FAILED") + f": {len(results)} checks, {TURN}")


if __name__ == "__main__":
    main()
