"""Songate for Android, dev only: walk the dev build through its pages and actions, a screenshot and devaudit's findings at each stop.
   python tools/android/devcrawl.py <out folder> [upright|sideways] [route ...]
Routes are below (ROUTES): each starts from Songate opened afresh on its Now playing and is a list of steps:
   "name"            tap the control of that name          "text:words"   tap the first words that hold them
   "hold:name"       a long press on the control           "key:4"        an Android key (4 back)
   "type:words"      type into the typing card and OK       "swipe:up"     scroll the page (up shows what is below)
   "wait:1.5"        wait                                   "shot:label"   screenshot, items and findings under that label
   "?name"           tap the control when it is there (no fault when it is not)
   "pages:label"     the page scrolled to its end: a shot at each screen of it (label, label-2, ...), then back at its top
   "drag:name:dx:dy" a finger down on the control, moved by dx, dy (the app's pixels) and kept down (a list in sort mode: a handle
                     or a tile dragged; a shot shows it held)     "lift"   the finger lifted where it is
At the end: <out>/report.txt (every stop's findings) and contact sheets <out>/sheet-NN.png, six stops each. The settings
folder is the device's own: routes change nothing for good (what they open they close; a playlist they make they remove)."""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from devlib import ADB, PKG, SERIAL, adb, run_as, screen, tap_at, tap_item, tap_text, wait, clear_notices   # noqa: E402
from devaudit import audit   # noqa: E402

OUT = sys.argv[1]
TURN = sys.argv[2] if len(sys.argv) > 2 else "upright"
ONLY = sys.argv[3:]
os.makedirs(OUT, exist_ok=True)
stops = []
missing = []
finger = [0.0, 0.0]   # where a "drag:" step's finger is, until "lift"

N = "ph.nav"   # the bottom bar upright: ph.nav (Now playing), #1 Library, #2 Search, #3 Playlists, #4 Settings

ROUTES = {
    # ---- Now playing
    "now": ["shot:now", "ph.more", "wait:0.8", "shot:now-more", "key:4", "ph.queue", "wait:0.8", "shot:now-upnext", "key:4",
            "ph.lyrics", "wait:1", "shot:now-lyrics", "?ph.lmore", "wait:0.8", "shot:now-lyrics-more", "key:4", "key:4",
            "ph.sleep", "wait:0.8", "shot:now-sleep", "key:4", "ph.order", "wait:0.5", "shot:now-order", "ph.order", "ph.order", "ph.order",
            "ph.devices", "wait:0.8", "shot:now-devices", "key:4", "hold:ph.eq", "wait:1", "shot:now-eq-hold", "key:4",
            "ph.eq", "wait:0.6", "shot:now-eq-tap", "ph.eq",
            "hold:ph.seek", "shot:now-seek-touched", "wait:1.2", "shot:now-seek-after"],
    # ---- the Canvas from upright: its button turns the screen sideways and opens it; closed, upright again
    "canvas-turn": ["ph.canvas", "wait:3", "shot:canvas-sideways", "key:4", "wait:3", "shot:canvas-closed-upright"],
    # ---- Now playing's second page: Lyrics and Palette; Up next: its Playlist button (the whole page), a row dragged by its handle (sort mode)
    "now2": ["ph.lyrics", "wait:1.2", "shot:lyrics-tab", "text:Palette", "wait:0.8", "shot:palette-tab", "text:Lyrics", "key:4", "wait:1",
             "ph.queue", "wait:1", "shot:upnext", "hold:ph.qrow*2", "wait:0.8", "shot:upnext-row-menu", "text:Move", "wait:1", "shot:upnext-sorting",
             "drag:ph.qsort*:0:100", "shot:upnext-held", "lift", "wait:0.6", "shot:upnext-moved", "?sort.done", "wait:0.5", "?sh.close", "wait:0.5",
             "?ph.qpage", "wait:1.2", "shot:playlist-page", "swipe:up", "shot:playlist-page-below", "key:4"],
    # ---- the Library page and what it opens (each part from the Library tab again: back from a page returns to the one before)
    "library": [N + "#1", "wait:0.8", "shot:lib", "swipe:up", "shot:lib-below",
                "reset:" + N + "#1", "ph.lrow", "wait:1", "shot:tracks", "trk.filter", "wait:0.8", "shot:tracks-filter", "key:4", "trk.sort", "wait:0.8", "shot:tracks-sort", "key:4",
                "swipe:up", "shot:tracks-below", "hold:trk.r*", "wait:0.8", "shot:track-menu", "key:4",
                "reset:" + N + "#1", "ph.lrow#1", "wait:1", "shot:albums", "swipe:up", "shot:albums-below",
                "reset:" + N + "#1", "ph.lalbum", "wait:1", "shot:album", "swipe:up", "shot:album-below", "hold:row", "wait:0.8", "shot:album-track-menu", "key:4",
                "reset:" + N + "#1", "ph.lrow#2", "wait:1", "shot:artists", "swipe:up", "shot:artists-below",
                "reset:" + N + "#1", "ph.lrow#3", "wait:1", "shot:genres",
                "reset:" + N + "#1", "ph.lrow#4", "wait:1", "shot:folders",
                "reset:" + N + "#1", "ph.lrow#5", "wait:1", "shot:liked"],
    # ---- Search
    "search": [N + "#2", "wait:0.8", "shot:search", "ph.sfield", "wait:0.8", "shot:search-card", "type:Geoxor", "wait:1.2", "shot:search-tracks",
               "text:Albums", "wait:0.8", "shot:search-albums", "text:Artists", "wait:0.8", "shot:search-artists",
               "text:Genres", "wait:0.8", "shot:search-genres", "text:Playlists", "wait:0.8", "shot:search-playlists",
               "text:Lyrics", "wait:0.8", "shot:search-lyrics", "text:Songs", "wait:0.5"],
    # ---- Playlists (each part from the Playlists tab again)
    "playlists": [N + "#3", "wait:0.8", "shot:playlists", "pls.new", "wait:0.8", "shot:pl-new", "text:Automix playlist", "wait:1.2", "shot:pl-automix", "key:4", "wait:0.5",
                  "reset:" + N + "#3", "pls.new", "wait:0.6", "text:Smart playlist", "wait:1", "shot:pl-smart", "?sp.row", "wait:1", "shot:pl-smart-form", "key:4", "key:4",
                  "reset:" + N + "#3", "pls.new", "wait:0.6", "text:Mood playlist", "wait:1", "shot:pl-mood", "key:4",
                  "reset:" + N + "#3", "pls.new", "wait:0.6", "text:Import a playlist", "wait:1", "shot:pl-import", "key:4",
                  "reset:" + N + "#3", "pls.item#-1", "wait:1", "shot:pl-liked"],
    # ---- Settings: the list and each page (by its name, from the top of the list; four screens of each page)
    "settings": [N + "#4", "wait:0.8", "shot:set", "swipe:up", "shot:set-below"]
                + sum([["reset:" + N + "#4", "?ph.back", "wait:0.5", "swipe:down", "swipe:down"] + (["swipe:up"] if k >= 7 else []) + [f"text:{c}", "wait:1",
                        f"pages:{c.lower()}", "key:4"]
                       for k, c in enumerate(["Appearance", "Playback", "Equalizer", "Library", "Storage", "Lyrics", "Canvas", "Visualizer",
                                              "Controls", "Devices", "Last.fm", "Reset", "Updates", "Support", "Debug", "About"])], []),
    # ---- the Canvas and its three layouts
    "s-canvas": ["reset:nav.now", "pb.canvas", "wait:1.5", "shot:canvas", "cv.layout", "wait:1", "shot:cover", "cv.layout#1", "wait:1", "shot:lyrics",
               "cv.layout#2", "wait:1", "shot:minimal", "cv.sleep", "wait:0.8", "shot:sleep", "key:4", "key:4"],
    # ---- a playlist of your own: made, its page, renamed (the typing card), its icon sheet, removed (its question answered)
    "ownlist": ["reset:" + N + "#3", "pls.new", "wait:0.6", "text:A playlist of your own", "type:CrawlList", "wait:1.2", "shot:made",
                "pl.rename", "wait:0.8", "shot:rename", "key:4", "wait:0.6", "pl.icon", "wait:0.8", "shot:icons", "key:4",
                "pl.remove", "wait:0.8", "shot:remove-ask", "?sp.yes", "?sp.confirm", "wait:1", "shot:removed"],
    # ---- Up next: a row held (its menu)
    "upnext": ["ph.queue", "wait:0.8", "hold:ph.qrow*", "wait:0.8", "shot:row-menu", "key:4", "key:4"],
    # ---- the tour, upright: its first steps
    "tour": ["reset:" + N + "#4", "wait:0.5", "swipe:up", "swipe:up", "text:About", "wait:1", "?set.tour", "wait:1.5", "shot:tour-1",
             "text:Next", "wait:1.2", "shot:tour-2", "text:Next", "wait:1.2", "shot:tour-3", "text:Next", "wait:1.2", "shot:tour-4",
             "text:Next", "wait:1.2", "shot:tour-5", "key:4", "key:4", "key:4", "key:4", "key:4", "key:4"],
    # ---- Files (the sidebar shows it for this route only: SETUP below)
    "files": ["reset:" + N + "#1", "ph.lrow#6", "wait:1", "shot:root", "text:SD card", "wait:1", "shot:card", "text:Music", "wait:1",
              "shot:music", "hold:fl.row*", "wait:0.8", "shot:file-menu", "key:4", "fl.select", "wait:0.8", "shot:select", "key:4"],
    # ---- sideways (routes named s-…, run with "sideways"): the layout one to one with the PC's, its sidebar and player bar
    "s-now": ["reset:nav.now", "wait:0.8", "shot:now", "np.more", "wait:0.8", "shot:more", "key:4", "np.tab#1", "wait:0.8", "shot:lyrics",
              "np.tab#2", "wait:0.8", "shot:palette", "pb.sleep", "wait:0.8", "shot:sleep", "key:4", "pb.queue", "wait:1", "shot:queue",
              "reset:nav.now", "hold:pb.eq", "wait:1", "shot:eq-hold", "reset:nav.now", "pb.canvas", "wait:1.5", "shot:canvas", "key:4", "wait:0.8",
              "tb.sidebar", "wait:0.8", "shot:sidebar-folded", "tb.sidebar", "wait:0.6"],
    "s-library": ["reset:nav.library", "wait:1", "shot:library", "swipe:up", "shot:library-below", "reset:nav.albums", "wait:1", "shot:albums",
                  "reset:nav.artists", "wait:1", "shot:artists", "reset:nav.genres", "wait:1", "shot:genres", "reset:nav.folders", "wait:1",
                  "shot:folders", "reset:nav.queue", "wait:1", "shot:playlist", "reset:nav.pl#-1", "wait:1", "shot:liked"],
    "s-search": ["tb.search", "wait:0.8", "type:Geoxor", "wait:1.2", "shot:search", "swipe:up", "shot:search-below"],
    "s-playlists": ["reset:nav.playlists", "wait:1", "shot:playlists", "nav.newpl", "wait:0.8", "shot:new", "text:Automix playlist", "wait:1.2",
                    "shot:automix", "key:4", "wait:0.5", "reset:nav.playlists", "nav.newpl", "wait:0.6", "text:Smart playlist", "wait:1", "shot:smart",
                    "key:4", "reset:nav.playlists", "nav.newpl", "wait:0.6", "text:Mood playlist", "wait:1", "shot:mood", "key:4"],
    "s-settings": ["reset:nav.settings", "wait:1", "shot:settings"]
                  + sum([["reset:nav.settings", f"text:{c}", "wait:1", f"shot:{c.lower()}", "swipe:up", f"shot:{c.lower()}-2", "swipe:up",
                          f"shot:{c.lower()}-3"]
                         for c in ["Appearance", "Playback", "Equalizer", "Library", "Storage", "Lyrics", "Canvas", "Visualizer",
                                   "Controls", "Devices", "Last.fm", "Reset", "Updates", "Support", "Debug", "About"]], []),
    "s-pages": ["reset:nav.pl#-1", "wait:1", "shot:liked", "reset:nav.albums", "wait:1", "text:Twilight", "wait:1.2", "shot:album",
                "reset:nav.artists", "wait:1", "text:Geoxor", "wait:1.2", "shot:artist", "swipe:up", "shot:artist-below",
                "reset:nav.library", "wait:1", "hold:trk.r*", "wait:1", "shot:track-menu", "key:4",
                "reset:nav.now", "pb.canvas", "wait:1.5", "shot:canvas", "key:4", "wait:0.8",
                "reset:nav.queue", "wait:1", "shot:queue", "reset:nav.playlists", "wait:1", "nav.newpl", "wait:0.8", "text:A playlist of your own",
                "type:CrawlSide", "wait:1.5", "shot:own-list", "?pl.remove", "wait:0.8", "?sp.yes", "wait:1", "shot:own-removed"],
    # ---- a server signed in (run by name: these need one): Local | Server on Library, Search and Playlists, the server's pages
    "server": [N + "#1", "wait:1", "shot:lib-local", "lib.source[1]", "wait:2.5", "shot:lib-server", "ph.lrow", "wait:2.5", "shot:songs", "key:4", "wait:0.8",
               "ph.lrow#1", "wait:2", "shot:albums", "key:4", "wait:0.8", "ph.lsalbum", "wait:2.5", "shot:album", "key:4", "wait:0.8",
               "ph.lrow#2", "wait:2", "shot:artists", "key:4", "wait:0.8", "ph.lrow#3", "wait:2", "shot:genres", "key:4", "wait:0.8",
               "ph.lrow#4", "wait:2", "shot:folders", "key:4", "wait:0.8", N + "#3", "wait:2", "shot:playlists", "pls.srv*", "wait:2.5", "shot:playlist",
               "key:4", N + "#2", "wait:1", "shot:search-empty", "ph.sfield", "wait:0.8", "type:abba", "wait:2.5", "shot:search-songs",
               "text:Albums", "wait:1.5", "shot:search-albums", "text:Artists", "wait:1.5", "shot:search-artists", "text:Playlists", "wait:1.5",
               "shot:search-playlists", "text:Songs", "wait:0.5", N + "#1", "wait:1", "lib.source[0]", "wait:1.5"],
    # ---- the Playlists page: its menu's Move by touch: sort mode, a tile dragged (needs two of yours; made and removed here)
    "move": ["reset:" + N + "#3", "pls.new", "wait:0.6", "text:A playlist of your own", "type:CrawlA", "wait:1.2", "reset:" + N + "#3",
             "pls.new", "wait:0.6", "text:A playlist of your own", "type:CrawlB", "wait:1.2", "reset:" + N + "#3", "wait:1", "shot:two",
             "hold:pls.item#*2", "wait:0.8", "shot:menu", "text:Move", "wait:1", "shot:sorting", "drag:pls.sort*:-150:40", "shot:held", "lift", "wait:0.6", "shot:moved", "?sort.done", "wait:0.5",
             "text:CrawlA", "wait:1", "pl.remove", "wait:0.8", "?sp.yes", "wait:1", "reset:" + N + "#3", "wait:0.8", "text:CrawlB", "wait:1", "pl.remove",
             "wait:0.8", "?sp.yes", "wait:1", "shot:removed"],
    # ---- what the sidebar carries above Settings, upright: the work's cards and a newer version's banner at the head of Settings' list
    # (test-jobs and test-update in the settings folder for this route only: SETUP below)
    "work": ["reset:" + N + "#4", "?ph.back", "wait:1.5", "shot:banner", "side.update", "wait:1", "shot:offer", "text:Later", "wait:2", "shot:cards",
             "side.job", "wait:1", "shot:jobs-list", "key:4"],
    # ---- the mini player and the sheet of a track's menu from Now playing
    "mini": [N + "#1", "wait:0.6", "shot:mini", "ph.mini", "wait:1", "shot:mini-opened"],
}


def shot(label):
    time.sleep(0.6)
    png = subprocess.run([ADB, "-s", SERIAL, "exec-out", "screencap", "-p"], capture_output=True).stdout
    path = os.path.join(OUT, f"{len(stops):03d}-{label}.png")
    open(path, "wb").write(png)
    s = screen()
    raw = run_as("cat files/Songate/items.txt 2>/dev/null")
    open(path[:-4] + ".items.txt", "w", encoding="utf-8").write(raw)
    found = audit(s)
    stops.append({"label": label, "png": path, "size": [s.w, s.h], "found": found})
    print(f"  {label:24} {len(found):3} findings" + ("".join(f"\n      {k:16} {t}" for k, t in found[:12])))


def long_press(name):
    s = clear_notices(screen())
    if name not in s.items:
        return False
    x, y, w, h = s.items[name]
    px, py = str(int((x + w / 2) * s.scale)), str(int((y + h / 2) * s.scale))
    adb("shell", "input", "swipe", px, py, px, py, "800")
    return True


def swipe(direction):
    s = screen()
    W, H = s.w * s.scale, s.h * s.scale
    x = str(int(W / 2))
    a, b = (int(H * 0.7), int(H * 0.3)) if direction == "up" else (int(H * 0.3), int(H * 0.7))
    adb("shell", "input", "swipe", x, str(a), x, str(b), "350")
    time.sleep(0.9)


def resolve(name):
    """A control by name, or "prefix*": the first of that list on screen, below the top bar ("prefix*2": the second down)."""
    nth = 0
    if name.endswith("*2"):
        name, nth = name[:-1], 1
    if not name.endswith("*"):
        return name
    s = screen()
    on = sorted((v[1], k) for k, v in s.items.items() if k.startswith(name[:-1]) and v[1] > 60 and v[1] + v[3] < s.h - 150)
    return on[nth][1] if len(on) > nth else name


def reset(tab):
    """Whatever is open closed (a sheet, a card: back until none is left), then that tab of the bottom bar."""
    for _ in range(5):
        s = screen()
        if not any(r == "Modal" for r in s.region.values()) and tab in s.items:
            break
        adb("shell", "input", "keyevent", "4")
        time.sleep(0.7)
    before = set(screen().items)
    if not tap_item(tab):
        missing.append(f"reset: no {tab}")
    time.sleep(0.8)
    if set(screen().items) == before:   # nothing changed: a tap lost (the opening, a card going away), or already there: once more
        tap_item(tab)
        time.sleep(0.8)


SIZE = os.environ.get("CRAWL_SIZE")
SIZE_BEFORE = run_as("grep ^uiSize= files/Songate/settings.txt").strip()
SETUP = {"files": ("sed -i s/^hiddenTabs=.*/hiddenTabs=0/ files/Songate/settings.txt", "sed -i s/^hiddenTabs=0$/hiddenTabs=64/ files/Songate/settings.txt"),
         "work": ("touch files/Songate/test-jobs; echo 2.0.0 > files/Songate/test-update", "rm -f files/Songate/test-jobs files/Songate/test-update")}
NEEDS_SERVER = {"server"}   # run only when named: a server signed in


def turn():
    want = "0" if TURN == "upright" else "1"
    for _ in range(20):
        adb("shell", "settings", "put", "system", "user_rotation", want)
        if f"mRotation={want}" in adb("shell", "dumpsys", "window", "displays"):
            break
        time.sleep(0.5)
    time.sleep(1)


def run(name, steps):
    print(f"route {name}")
    adb("shell", "am", "force-stop", PKG)
    if name in SETUP:
        run_as(SETUP[name][0])
    if SIZE is not None:   # CRAWL_SIZE: sideways' Size for this crawl (0 Standard, 1 Medium, 2 Largest); put back at the end
        run_as(f"sed -i s/^uiSize=.*/uiSize={SIZE}/ files/Songate/settings.txt")
    adb("shell", "am", "start", "-W", "-n", f"{PKG}/.SongateActivity")
    turn()   # (Android puts the turn back while its home screen shows: set again with Songate in front)
    wait(lambda s: "ph.nav" in s.items or "nav.now" in s.items, 15)
    time.sleep(4)   # the opening
    tap_item("ph.nav") or tap_item("nav.now")
    time.sleep(0.8)
    for st in steps:
        if st.startswith("shot:"):
            shot(f"{name}-{st[5:]}" if not st[5:].startswith(name) else st[5:])
        elif st.startswith("pages:"):
            label, before = st[6:], None
            for k in range(14):   # a shot at each screen of the page, until a swipe no longer moves it
                now = sorted(t[0] for t in screen().texts)
                if now == before:
                    break
                before = now
                shot(f"{name}-{label}" + (f"-{k + 1}" if k else ""))
                swipe("up")
            for _ in range(3):
                swipe("down")
        elif st.startswith("wait:"):
            time.sleep(float(st[5:]))
        elif st.startswith("key:"):
            adb("shell", "input", "keyevent", st[4:])
            time.sleep(0.7)
        elif st.startswith("text:"):
            if not tap_text(st[5:]):
                missing.append(f"{name}: no words '{st[5:]}'")
            time.sleep(0.6)
        elif st.startswith("reset:"):
            reset(st[6:])
        elif st.startswith("hold:"):
            if not long_press(resolve(st[5:])):
                missing.append(f"{name}: no control {st[5:]} to hold")
            time.sleep(0.4)
        elif st.startswith("type:"):
            time.sleep(0.6)
            adb("shell", "input", "text", st[5:])
            time.sleep(0.4)
            adb("shell", "input", "keyevent", "66")
            time.sleep(0.8)
        elif st.startswith("swipe:"):
            swipe(st[6:])
        elif st.startswith("drag:"):
            ctl, dx, dy = st[5:].rsplit(":", 2)
            s = screen()
            ctl = resolve(ctl)
            if ctl not in s.items:
                missing.append(f"{name}: no control {ctl} to drag")
                continue
            x, y, w, h = s.items[ctl]
            finger[:] = [(x + w / 2) * s.scale, (y + h / 2) * s.scale]
            adb("shell", "input", "motionevent", "DOWN", str(int(finger[0])), str(int(finger[1])))
            time.sleep(0.25)
            for k in range(1, 9):
                adb("shell", "input", "motionevent", "MOVE", str(int(finger[0] + float(dx) * s.scale * k / 8)), str(int(finger[1] + float(dy) * s.scale * k / 8)))
            finger[:] = [finger[0] + float(dx) * s.scale, finger[1] + float(dy) * s.scale]
            time.sleep(0.5)
        elif st == "lift":
            adb("shell", "input", "motionevent", "UP", str(int(finger[0])), str(int(finger[1])))
            time.sleep(0.6)
        elif st.startswith("?"):
            tap_item(st[1:])
            time.sleep(0.6)
        else:
            if not tap_item(resolve(st)):
                missing.append(f"{name}: no control {st} (on {', '.join(list(screen().items)[:8])}…)")
            time.sleep(0.6)


def sheets():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return
    font = ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf", 26)
    per = 6
    for k in range(0, len(stops), per):
        group = stops[k:k + per]
        ims = [Image.open(st["png"]).convert("RGB") for st in group]
        if not ims:
            continue
        upright = ims[0].height > ims[0].width
        tw = 360 if upright else 620
        th = int(ims[0].height * tw / ims[0].width)
        cols = per if upright else 3
        rows = (len(ims) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (tw + 12) + 12, rows * (th + 52) + 12), (20, 20, 26))
        dr = ImageDraw.Draw(sheet)
        for i, (st, im) in enumerate(zip(group, ims)):
            x, y = 12 + (i % cols) * (tw + 12), 12 + (i // cols) * (th + 52)
            sheet.paste(im.resize((tw, th)), (x, y + 40))
            dr.text((x, y + 4), f"{os.path.basename(st['png'])[:3]} {st['label'][:26]} ({len(st['found'])})", font=font, fill=(200, 210, 255))
        sheet.save(os.path.join(OUT, f"sheet-{k // per:02d}.png"))


adb("shell", "settings", "put", "system", "accelerometer_rotation", "0")
adb("shell", "settings", "put", "system", "user_rotation", "0" if TURN == "upright" else "1")
time.sleep(2)
run_as("touch files/Songate/test-items")
for name, steps in ROUTES.items():
    if (name in ONLY) if ONLY else (name.startswith("s-") == (TURN == "sideways") and name not in NEEDS_SERVER):   # upright's routes, or sideways' (s-…)
        run(name, steps)
        if name in SETUP:   # what the route's setup changed, put back
            adb("shell", "am", "force-stop", PKG)
            run_as(SETUP[name][1])
adb("shell", "am", "force-stop", PKG)
if SIZE is not None and SIZE_BEFORE:
    run_as(f"sed -i s/^uiSize=.*/{SIZE_BEFORE}/ files/Songate/settings.txt")
with open(os.path.join(OUT, "report.txt"), "w", encoding="utf-8") as f:
    for st in stops:
        f.write(f"{os.path.basename(st['png'])} {st['size']}\n")
        for k, t in st["found"]:
            f.write(f"    {k:16} {t}\n")
    f.write("\nmissing:\n" + "\n".join(missing) + "\n")
json.dump(stops, open(os.path.join(OUT, "stops.json"), "w", encoding="utf-8"), indent=1)
sheets()
print(f"{len(stops)} stops; {sum(len(s['found']) for s in stops)} findings; {len(missing)} steps missed")
for m in missing:
    print("  missed:", m)
