"""Session 18: the PC dev build soaked for leaks. Scratch settings; the library is D:/Music, READ ONLY (GF_READONLY takes that
folder); signed in to the Navidrome test server when --server is given.

    python pcsoak.py <tag> <cycles> [--server]

Each cycle: every page of the sidebar, an album and an artist opened and left, a search, three Settings categories, the next
song started, the window laid out at another size and back; with --server also the server's library tabs, a server album and
a server search. After each cycle: the process's private bytes and working set (Windows' own counters), its handles and
threads. A leak shows as private bytes that keep climbing cycle after cycle once the caches have filled."""
import os
import shutil
import sys
import time
from pathlib import Path

import psutil

HERE = Path(__file__).resolve().parent
OLD = Path(r'C:\Users\andre\AppData\Local\Temp\claude\2499b8fca18fc9e4\fc83201e-e139-4196-b1f9-d0f07f9e3780\scratchpad')
tag, cycles = sys.argv[1], int(sys.argv[2])
server = '--server' in sys.argv
cfg = HERE / f'soak-{tag}'
if cfg.exists():
    shutil.rmtree(cfg)
home = cfg / 'Songate'
home.mkdir(parents=True)
os.environ['SONGATE_PCCFG'] = str(cfg)
os.environ['GF_LIVE_MUSIC'] = 'D:/Music'
sys.path.insert(0, r'C:\Users\andre\Desktop\GateFold-NX\tools\preview')
import pcdrive   # noqa: E402

(home / 'settings.txt').write_text('musicRoot=D:/Music\nopening=0\nwindow=60 60 1280 720 0\nmotion=1\nbackground=0\n' + ('libSource=0\n' if server else ''))
if server:
    shutil.copy(OLD / 'account.txt', home / 'subsonic-account.txt')
log = open(HERE / f'soak-{tag}.csv', 'w', encoding='utf-8')
log.write('cycle,seconds,private_mb,working_mb,handles,threads\n')

pc = pcdrive.PC()
proc = psutil.Process(pc.app.pid)
t0 = time.time()


def sample(n):
    m = proc.memory_info()
    row = (n, time.time() - t0, m.private / 1e6, m.rss / 1e6, proc.num_handles(), proc.num_threads())
    w = pc.stats()['text'].split()   # the app's own C heap, walked (devctl's stats): apart from the graphics driver's memory
    heap = (int(w[w.index('heap') + 1]) / 1e6, int(w[w.index('heap') + 2])) if 'heap' in w and len(w) > w.index('heap') + 2 else (0.0, 0)
    srv = int(w[w.index('srvcovers') + 1]) if 'srvcovers' in w and len(w) > w.index('srvcovers') + 1 else -1   # server covers held as textures
    log.write('%d,%.0f,%.1f,%.1f,%d,%d' % row + ',%.3f,%d' % heap + ',%d\n' % srv)
    log.flush()
    return row


def texts(s):
    return [t[0] for t in s.texts]


def tap_first(prefix, pause=0.9):
    s = pc.screen()
    for name in s.items:
        if name.startswith(prefix):
            x, y, w, h = s.items[name]
            pc.tap(x + w / 2, y + h / 2, pause)
            return True
    return False


WORDS = ['love', 'night', 'the', 'remix', 'fire', 'you', 'blue', 'dance', 'heart', 'sun']
try:
    pc.wait(lambda s: s.w > 0, 60)
    # the first read of 3,074 files' tags, and the first cards answered
    for _ in range(90):
        time.sleep(2)
        pc.clear_notices()
        s = pc.screen()
        if any('Albums' == t for t in texts(s)) and not any('Reading' in t for t in texts(s)):
            break
    time.sleep(20)
    pc.clear_notices()
    print('start', '%d s, private %.0f MB, working %.0f MB, %d handles, %d threads' % sample(0)[1:])
    same = '--same' in sys.argv          # every cycle the very same steps: then any growth at all is a leak
    only = next((x[7:].split(',') for x in sys.argv if x.startswith('--only=')), None)   # pages,album,search,next,settings,server,size
    want = lambda step: only is None or step in only
    for n in range(1, cycles + 1):
        v = 3 if same else n
        if want('pages'):
            for page in ('Now playing', 'Playlist', 'Albums', 'Artists', 'Genres', 'Folders', 'Library'):
                pc.tap_text(page, pause=0.5, secs=2)
                pc.ctl(f'wheel 800 400 -{3 + v % 5}')   # another part of the list each cycle: other covers, other words
                time.sleep(0.3)
        if want('album'):
            pc.tap_text('Albums', pause=0.6, secs=2)
            pc.ctl(f'wheel 800 400 -{(v * 7) % 60}')
            time.sleep(0.4)
            if tap_first('lib.tile', 1.0):       # an album's page, then back
                pc.ctl('pad 2')                  # B
                time.sleep(0.5)
        if want('search'):
            pc.ctl('search ' + WORDS[v % len(WORDS)])
            time.sleep(1.0)
        if want('next'):
            tap_first('pb.next', 0.8)            # the next song: its cover, its lyrics, its decoder
        if want('settings'):
            pc.tap_text('Settings', pause=0.6, secs=2)
            for cat in ('Appearance', 'Playback', 'Library'):
                pc.tap_text(cat, pause=0.4, secs=2)
        if server and want('server'):
            tap_first('lib.source[1]', 1.2)               # the title bar's switch: Server
            for page in ('Albums', 'Artists', 'Library'):
                pc.tap_text(page, pause=0.9, secs=2)
                pc.ctl(f'wheel 800 400 -{2 + (v * 3) % 40}')
                time.sleep(0.8)
            pc.ctl('search ' + WORDS[(v + 3) % len(WORDS)])
            time.sleep(1.2)
            tap_first('lib.source[0]', 0.8)
        if want('size'):
            pc.size(1600 if v % 2 else 1000, 900 if v % 2 else 640)   # laid out at another size: every word made again
            time.sleep(0.4)
            pc.size(0, 0)
            time.sleep(0.4)
        if only:
            time.sleep(1.0)
        row = sample(n)
        if n % 5 == 0 or n == cycles:
            print('cycle %3d: %5.0f s, private %.0f MB, working %.0f MB, %d handles, %d threads' % row)
            sys.stdout.flush()
finally:
    log.close()
    pc.quit()
