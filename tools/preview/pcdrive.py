"""Songate for PC, dev only: starts the dev build (build-windows-check/Songate.exe unless SONGATE_EXE says another) on settings of
its own and drives it through GF_CTL (source/devctl.cpp) and GF_ITEMS. Used by devsizes.py and devresize.py. Nothing of it ships.

The settings folder is tools/preview/out/pc-config (GF_CONFIG): never %APPDATA%. The library is D:\\Music unless GF_LIVE_MUSIC says
another, read only (GF_READONLY takes that folder)."""
import os
import re
import subprocess
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent.parent
EXE = Path(os.environ.get('SONGATE_EXE', PROJECT / 'build-windows-check' / 'Songate.exe'))
CFG = Path(os.environ.get('SONGATE_PCCFG', HERE / 'out' / 'pc-config'))
MUSIC = os.environ.get('GF_LIVE_MUSIC', 'D:/Music').replace('\\', '/')
WORK = CFG / 'work'

SETTINGS = """musicRoot={music}
opening=0
window=60 60 1280 720 0
motion=1
"""


class Screen:
    def __init__(self, text):
        self.scale, self.w, self.h, self.playing, self.track = 1.0, 0, 0, False, ''
        self.items, self.texts, self.region = {}, [], {}
        self.top, self.cut = [], set()
        m = re.search(r'scale ([\d.]+) size (\d+) (\d+) pos [-\d.]+ playing (\d) vol [\d.]+ track (.*?):', text)
        if m:
            self.scale, self.w, self.h, self.playing, self.track = float(m[1]), int(m[2]), int(m[3]), m[4] == '1', m[5]
        for line in text.splitlines():
            t = re.match(r'\s+(\w+)\s+(\S+)\s+(-?\d+),\s*(-?\d+)\s+(\d+)x(\d+)', line)
            if t:
                self.items[t[2]] = tuple(int(v) for v in t.group(3, 4, 5, 6))
                self.region[t[2]] = t[1]
            elif line.startswith('TEXT ') or line.startswith('TOP '):
                top = line.startswith('TOP ')
                head, _, s = line[4 if top else 5:].partition('|')
                v = [float(n) for n in head.split()]
                if len(v) < 4:
                    continue
                if len(v) > 4 and v[4]:
                    self.cut.add(len(self.texts))
                if top:
                    self.top.append(len(self.texts))
                self.texts.append((s, v[0], v[1], v[2], v[3]))

    def has_text(self, s):
        return any(s in t[0] for t in self.texts)

    def text_at(self, s):
        for t in self.texts:
            if s in t[0]:
                return t
        return None


class PC:
    def __init__(self, env=None, exe=None, fresh=False, window=None):
        WORK.mkdir(parents=True, exist_ok=True)
        (CFG / 'Songate').mkdir(parents=True, exist_ok=True)
        st = CFG / 'Songate' / 'settings.txt'
        if fresh or not st.exists():
            st.write_text(SETTINGS.format(music=MUSIC), encoding='utf-8')
        if window:   # "x y w h max": where the real window opens
            lines = [l for l in st.read_text(encoding='utf-8').splitlines() if not l.startswith('window=')]
            st.write_text('\n'.join(lines + ['window=' + window]) + '\n', encoding='utf-8')
        self.ctl_path = WORK / 'ctl.txt'
        self.items_path = WORK / 'items.txt'
        for p in (self.ctl_path, self.items_path):
            if p.exists():
                p.unlink()
        e = os.environ.copy()
        e.update(GF_CONFIG=str(CFG), GF_READONLY=MUSIC, GF_CTL=str(self.ctl_path), GF_ITEMS=str(self.items_path),
                 SDL_AUDIODRIVER='dummy')
        e.update(env or {})
        self.log = open(WORK / 'app-log.txt', 'ab')
        exe = Path(exe or EXE)
        si = subprocess.STARTUPINFO()   # (as an ordinary window, whatever the shell this runs from was started as)
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 1
        self.app = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=e, stdout=self.log, stderr=subprocess.STDOUT, startupinfo=si)

    # ---- the app's side
    def ctl(self, *cmds, wait=3.0):
        tmp = self.ctl_path.with_suffix('.tmp')
        end = time.time() + wait
        while self.ctl_path.exists() and time.time() < end:   # the one before, not yet taken
            time.sleep(0.01)
        tmp.write_text('\n'.join(cmds) + '\n', encoding='utf-8')
        for _ in range(50):
            try:
                os.replace(tmp, self.ctl_path)
                break
            except OSError:
                time.sleep(0.01)
        while self.ctl_path.exists() and time.time() < end:
            time.sleep(0.005)
        return not self.ctl_path.exists()

    def screen(self):
        for _ in range(20):
            try:
                return Screen(self.items_path.read_text(encoding='utf-8', errors='replace'))
            except OSError:
                time.sleep(0.02)
        return Screen('')

    def fresh_screen(self, secs=0.5):
        """What it shows once the frames after now are in the file (written every tenth frame)."""
        time.sleep(secs)
        return self.screen()

    def wait(self, cond, secs=15):
        end = time.time() + secs
        while time.time() < end:
            s = self.screen()
            if cond(s):
                return s
            time.sleep(0.2)
        return None

    def tap(self, x, y, pause=0.6):
        self.ctl(f'tap {x:.0f} {y:.0f}')
        time.sleep(pause)

    def clear_notices(self):
        """A new user's notices (covers, lyrics for search) come when they are ready, over whatever is there: answered Not now."""
        for _ in range(4):
            s = self.screen()
            if 'notice.later' not in s.items:
                return
            x, y, w, h = s.items['notice.later']
            self.tap(x + w / 2, y + h / 2, 0.8)

    def tap_item(self, name, pause=0.6, secs=5):
        self.clear_notices()
        s = self.wait(lambda s: name in s.items, secs)
        if not s:
            return False
        x, y, w, h = s.items[name]
        self.tap(x + w / 2, y + h / 2, pause)
        return True

    def tap_text(self, words, pause=0.6, secs=5):
        s = self.wait(lambda s: s.text_at(words), secs)
        if not s:
            return False
        _, x, y, w, h = s.text_at(words)
        self.tap(x + w / 2, y + h / 2, pause)
        return True

    def size(self, w, h, screen=None, dpi=None, settle=0.5):
        cmds = [f'size {w} {h}']
        if screen is not None:
            cmds.append(f'screen {screen[0]} {screen[1]}')
        if dpi is not None:
            cmds.append(f'dpi {dpi}')
        self.ctl(*cmds)
        time.sleep(settle)

    def shot(self, path, quality=90):
        """A frame as drawn, to a JPEG (or the BMP itself when the name ends so)."""
        from PIL import Image
        path = Path(path)
        bmp = WORK / 'shot.bmp'
        if bmp.exists():
            bmp.unlink()
        self.ctl('shot ' + str(bmp).replace('\\', '/'))
        end = time.time() + 5
        while not bmp.exists() and time.time() < end:
            time.sleep(0.01)
        time.sleep(0.02)
        im = Image.open(bmp).convert('RGB')
        im.load()
        if path.suffix.lower() == '.bmp':
            im.save(path)
        elif path.suffix:
            path.parent.mkdir(parents=True, exist_ok=True)
            im.save(path, quality=quality)
        return im

    def stats(self):
        p = WORK / 'stats.txt'
        if p.exists():
            p.unlink()
        self.ctl('stats ' + str(p).replace('\\', '/'))
        end = time.time() + 3
        while not p.exists() and time.time() < end:
            time.sleep(0.01)
        time.sleep(0.02)
        w = p.read_text().split()
        return {'frames': int(w[1]), 'mean': float(w[3]), 'worst': float(w[5]), 'work': float(w[7]), 'hz': int(w[9]), 'scale': float(w[11]),
                'w': int(w[13]), 'h': int(w[14]), 'pw': int(w[16]), 'ph': int(w[17]), 'text': ' '.join(w)}

    def hwnd(self):
        import ctypes
        from ctypes import wintypes
        u = ctypes.windll.user32
        found = []
        pid = self.app.pid

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def each(h, _):
            p = wintypes.DWORD()
            u.GetWindowThreadProcessId(h, ctypes.byref(p))
            if p.value == pid and u.IsWindowVisible(h):
                cls = ctypes.create_unicode_buffer(64)
                u.GetClassNameW(h, cls, 64)
                if cls.value == 'SDL_app':
                    found.append(h)
            return True
        u.EnumWindows(each, 0)
        return found[0] if found else None

    def quit(self):
        import ctypes
        h = self.hwnd()
        if h:
            ctypes.windll.user32.PostMessageW(h, 0x0111, 40014, 0)   # WM_COMMAND, the tray menu's Quit (winshell.cpp): a whole exit, what it knows saved
        try:
            self.app.wait(45)
        except subprocess.TimeoutExpired:
            self.app.kill()
        self.log.close()


def _restore(self):
    """An ordinary window (started from some shells it opens maximised)."""
    import ctypes
    h = self.hwnd()
    if h:
        ctypes.windll.user32.ShowWindow(h, 9)
    time.sleep(0.3)


PC.restore = _restore
