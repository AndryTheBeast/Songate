"""Songate for PC, dev only: the real window, really resized, and what is on the screen meanwhile.

    python tools/preview/devresize.py <tag> [--exe <Songate.exe>] [--out <folder>] [--mouse]

It starts the dev build on pcdrive.py's own settings (never %APPDATA%; D:\\Music read only), puts the window on the screen and:
  drag      the right edge taken in from nearly the whole screen to under the least size and back out, a few pixels at a time
            (SetWindowPos, as fast as the app answers; with --mouse the real mouse drags the real edge, Windows' own loop)
  fold      the edge taken slowly across the width where the sidebar folds, and back: a strip of what the screen showed
  hold      the edge taken across that width and held still (the mouse, with --mouse): the fold goes on while it is held
  jumps     maximised, restored, the left half of the screen, a quarter, full screen and back: the screen a moment after each
Each grab is of the screen itself (what a person would see), and is checked: the player bar's right end, as drawn at rest, must be
at the window's right end in every one (a stale frame has it elsewhere, a stretched one has it wider or narrower).
The app says how many frames it drew and how long they took. A visible window is used and closed at the end. Nothing of it ships."""
import argparse
import ctypes
import sys
import time
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pcdrive   # noqa: E402

u = ctypes.windll.user32
ctypes.windll.shcore.SetProcessDpiAwareness(2)
SWP = 0x0004 | 0x0010   # NOZORDER | NOACTIVATE


def rect(h):
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top


def place(h, x, y, w, hh):
    u.SetWindowPos(h, 0, int(x), int(y), int(w), int(hh), SWP)


def inside(h):
    """What the app draws on: the window without the frame Windows keeps round a maximised one."""
    r = wintypes.RECT()
    u.GetClientRect(h, ctypes.byref(r))
    p = wintypes.POINT(0, 0)
    u.ClientToScreen(h, ctypes.byref(p))
    return p.x, p.y, r.right, r.bottom


def grab(h, part=None):
    """The screen where the window is. part: only its top left, this many pixels (a grab of the whole takes a third of a second)."""
    from PIL import ImageGrab
    x, y, w, hh = inside(h)
    gw, gh = (min(w, part[0]), min(hh, part[1])) if part else (w, hh)
    return ImageGrab.grab(bbox=(max(0, x), max(0, y), x + gw, y + gh), all_screens=True), w, hh


def corner(im, k):
    """The player bar's right end (its volume and the Canvas button), k real pixels to an app pixel at rest."""
    w, h = im.size
    cw, ch = int(150 * k), int(70 * k)
    return im.crop((w - cw, h - ch, w, h)).convert('L')


def differs(a, b):
    """How unlike two corners are: 0 the same, about 1 unrelated (mean absolute difference over what the reference varies by)."""
    import numpy as np
    if a.size != b.size:
        return 9.0
    x, y = np.asarray(a, dtype=np.float32), np.asarray(b, dtype=np.float32)
    return float(np.abs(x - y).mean() / max(4.0, float(np.abs(y - y.mean()).mean())))


def strip(frames, title, path, cols=4, cell=600):
    from PIL import Image, ImageDraw, ImageFont
    try:
        font = ImageFont.truetype('segoeui.ttf', 20)
        small = ImageFont.truetype('segoeui.ttf', 15)
    except OSError:
        font = small = ImageFont.load_default()
    th = []
    for name, im in frames:
        k = min(1.0, cell / im.width)
        th.append((name, im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)))
    cw, ch = cell + 12, max(t.height for _, t in th) + 34
    rows = (len(th) + cols - 1) // cols
    out = Image.new('RGB', (cw * cols + 12, ch * rows + 52), (24, 24, 28))
    d = ImageDraw.Draw(out)
    d.text((12, 10), title, fill=(240, 240, 240), font=font)
    for i, (name, t) in enumerate(th):
        x, y = 12 + (i % cols) * cw, 48 + (i // cols) * ch
        d.text((x, y), name, fill=(190, 190, 190), font=small)
        out.paste(t, (x, y + 24))
    path.parent.mkdir(parents=True, exist_ok=True)
    out.save(path, quality=88)


class Mouse:
    """The real mouse (SendInput): the edge held and moved, so Windows runs its own resize loop."""
    class MI(ctypes.Structure):
        _fields_ = [('dx', wintypes.LONG), ('dy', wintypes.LONG), ('data', wintypes.DWORD), ('flags', wintypes.DWORD),
                    ('time', wintypes.DWORD), ('extra', ctypes.c_void_p)]

    class INPUT(ctypes.Structure):
        pass

    def __init__(self):
        Mouse.INPUT._fields_ = [('type', wintypes.DWORD), ('mi', Mouse.MI)]
        self.sw, self.sh = u.GetSystemMetrics(0), u.GetSystemMetrics(1)

    def send(self, flags, x=None, y=None):
        i = Mouse.INPUT()
        i.type = 0
        if x is not None:
            i.mi.dx, i.mi.dy = int(x * 65535 / (self.sw - 1)), int(y * 65535 / (self.sh - 1))
            flags |= 0x8000 | 0x0001
        i.mi.flags = flags
        u.SendInput(1, ctypes.byref(i), ctypes.sizeof(i))

    def move(self, x, y):
        self.send(0, x, y)

    def down(self):
        self.send(0x0002)

    def up(self):
        self.send(0x0004)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tag')
    ap.add_argument('--exe')
    ap.add_argument('--out')
    ap.add_argument('--mouse', action='store_true')
    a = ap.parse_args()
    out = Path(a.out) if a.out else HERE / 'out' / 'resize'
    out.mkdir(parents=True, exist_ok=True)
    sw, sh = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
    X, Y, H = 40, 40, min(sh - 160, 1440)
    wide = sw - 120
    pc = pcdrive.PC(exe=a.exe, window=f'{X} {Y} {wide} {H} 0')
    report = []
    say = lambda s: (print(s, flush=True), report.append(s))
    mouse = Mouse() if a.mouse else None
    try:
        pc.wait(lambda s: s.w > 0, 40)
        pc.restore()
        h = pc.hwnd()
        u.SetForegroundWindow(h)
        place(h, X, Y, wide, H)
        time.sleep(1.0)
        for _ in range(24):   # a first start's notices come when they are ready: each answered Not now
            pc.clear_notices()
            time.sleep(0.5)
        pc.tap_item('nav.albums')
        time.sleep(1.0)
        st = pc.stats()
        k = st['scale']
        fold_px = int(1170 * k)   # the window's width, in real pixels, where the sidebar folds
        least_px = int(1000 * k)
        say(f'{a.tag}: screen {sw}x{sh}, window {wide}x{H}, {k:g} real pixels to an app pixel, {st["hz"]} Hz; the sidebar folds under {fold_px} px, '
            f'everything shrinks under {least_px} px')
        ref, _, _ = grab(h)
        ref_corner = corner(ref, k)

        def set_width(w):
            if mouse:
                mouse.move(X + w - 2, Y + H // 2)
            else:
                place(h, X, Y, w, H)

        def begin(w):
            if mouse:
                mouse.move(X + w - 2, Y + H // 2)
                time.sleep(0.15)
                mouse.down()
                time.sleep(0.15)

        def end():
            if mouse:
                mouse.up()
                time.sleep(0.1)
                mouse.move(X + int(300 * k), Y + 6)   # (away from the edge, over nothing that lights under it)
                time.sleep(0.2)

        def run(name, widths, pause, check_from, grab_every=1, after=0.0, part=None):
            """The edge through these widths; grabs on the way, each checked while the window is over check_from wide."""
            pc.stats()
            frames, bad, n = [], [], 0
            t0 = time.perf_counter()
            begin(widths[0])
            for i, w in enumerate(widths):
                set_width(w)
                if pause:
                    time.sleep(pause)
                if grab_every and i % grab_every == 0:
                    im, gw, _ = grab(h, part)
                    frames.append((f'{gw} px wide, {time.perf_counter() - t0:.2f} s', im))
                    if gw >= check_from and not part:
                        n += 1
                        dv = differs(corner(im, k), ref_corner)
                        if dv > 0.5:
                            if not bad:   # the first of them, its right end as the screen had it
                                im.save(out / f'{a.tag}-{name.split()[0]}-trailing-whole.jpg', quality=85); im.crop((im.width - min(im.width, 1100), im.height - min(im.height, 420), im.width, im.height)).save(
                                    out / f'{a.tag}-{name.split()[0]}-trailing.jpg', quality=90)
                            bad.append((gw, round(dv, 2)))
            t_end = time.perf_counter() + after
            while time.perf_counter() < t_end:
                im, gw, _ = grab(h, part)
                frames.append((f'{gw} px wide, {time.perf_counter() - t0:.2f} s (the edge held still)', im))
            took = time.perf_counter() - t0
            end()
            st = pc.stats()
            say(f'  {name}: {len(widths)} steps in {took:.2f} s; the app drew {st["frames"]} frames, {st["mean"]:.1f} ms apart on average, '
                f'the longest {st["worst"]:.1f} ms, the longest work for one {st["work"]:.1f} ms; {n} grabs checked, {len(bad)} with the '
                f'bar\'s right end out of place {bad[:6] if bad else ""}')
            return frames

        # 1. drag: in to under the least size, and out again
        lo = max(int(800 * k * 1.02), least_px - int(160 * k))
        widths = list(range(wide, lo, -int(6 * k))) + list(range(lo, wide, int(6 * k)))
        fr = run('drag in and out', widths, 0.004, least_px + 8, grab_every=9)
        strip(fr[:: max(1, len(fr) // 12)][:12], f'{a.tag}: the right edge dragged in and out ({"the mouse" if mouse else "SetWindowPos"})', out / f'{a.tag}-drag.jpg')

        # 2. fold: slowly across the fold's width, held, and back
        place(h, X, Y, fold_px + int(60 * k), H)
        time.sleep(1.2)
        part = (int(760 * k), int(420 * k))   # the window's top left: the sidebar and the page's start (a small grab is a quick one)
        place(h, X, Y, fold_px + int(30 * k), H)
        time.sleep(1.2)
        widths = list(range(fold_px + int(30 * k), fold_px + int(8 * k), -int(2 * k))) + [fold_px + int(2 * k), fold_px - int(2 * k), fold_px - int(4 * k), fold_px - int(6 * k), fold_px - int(8 * k)]
        fr = run('across the fold, inwards', widths, 0.004, least_px + 8, grab_every=len(widths) - 4, after=0.8, part=part)
        strip(fr[:16], f'{a.tag}: the edge taken in across {fold_px} px (1170 app pixels) and held there: the window\'s top left', out / f'{a.tag}-fold-in.jpg')
        time.sleep(0.8)
        st_back = 0 if a.tag.startswith('before') else 40   # (the way back is 40 app pixels later, since tonight)
        back = fold_px + int(st_back * k)
        widths = list(range(fold_px - int(12 * k), back - int(8 * k), int(2 * k))) + [back - int(4 * k), back + int(2 * k), back + int(4 * k), back + int(6 * k), back + int(8 * k)]
        fr = run('across the fold, outwards', widths, 0.004, least_px + 8, grab_every=len(widths) - 4, after=0.8, part=part)
        strip(fr[:16], f'{a.tag}: the edge taken out again across {fold_px} px and held: the window\'s top left', out / f'{a.tag}-fold-out.jpg')

        # 3. to and fro about the fold's width: how often it folds
        place(h, X, Y, fold_px + int(8 * k), H)
        time.sleep(1.0)
        flips, last = 0, None
        begin(fold_px + int(8 * k))
        for i in range(16):
            set_width(fold_px + int((8 if i % 2 == 0 else -8) * k))
            time.sleep(0.75)
            s = pc.screen()
            open_now = 'nav.now' in s.items and s.items['nav.now'][2] > 150
            if last is not None and open_now != last:
                flips += 1
            last = open_now
        end()
        say(f'  to and fro about the fold (8 app pixels either side of it, 16 times): the sidebar folded or opened {flips} times')

        # 4. jumps
        def jump(name, fn, settle=(0.05, 0.25, 0.8)):
            pc.stats()
            fn()
            frames = []
            t0 = time.perf_counter()
            for s in settle:
                time.sleep(max(0, s - (time.perf_counter() - t0)))
                im, gw, gh = grab(h)
                frames.append((f'{name}: {gw}x{gh} after {time.perf_counter() - t0:.2f} s', im))
            st = pc.stats()
            ok = differs(corner(frames[-1][1], st['scale']), ref_corner) if abs(st['scale'] - k) < 0.001 else -1
            say(f'  {name}: window {rect(h)[2]}x{rect(h)[3]}, layout {st["w"]}x{st["h"]} at {st["scale"]:g}; longest frame {st["worst"]:.1f} ms'
                + (f'; the bar\'s right end {"in place" if ok < 0.5 else "OUT OF PLACE"}' if ok >= 0 else ''))
            return frames

        if mouse:   # (the jumps are the same with or without it, and the mouse over the window lights what it is on)
            return
        place(h, X, Y, int(1280 * k * 0.9), int(720 * k * 0.9))
        time.sleep(0.8)
        fr = []
        fr += jump('maximised', lambda: u.ShowWindow(h, 3))
        fr += jump('restored', lambda: u.ShowWindow(h, 9))
        fr += jump('left half', lambda: place(h, 0, 0, sw // 2, sh - 96))
        fr += jump('a quarter', lambda: place(h, 0, 0, sw // 2, (sh - 96) // 2))
        fr += jump('back', lambda: place(h, X, Y, int(1280 * k * 0.9), int(720 * k * 0.9)))
        F11 = 0x7A
        fr += jump('full screen (F11)', lambda: (u.SetForegroundWindow(h), u.PostMessageW(h, 0x0100, F11, 0x00570001), u.PostMessageW(h, 0x0101, F11, 0xC0570001)))
        fr += jump('out of full screen', lambda: (u.PostMessageW(h, 0x0100, F11, 0x00570001), u.PostMessageW(h, 0x0101, F11, 0xC0570001)))
        strip(fr, f'{a.tag}: maximised, restored, snapped, full screen: the screen 0.05, 0.25 and 0.8 s after each', out / f'{a.tag}-jumps.jpg', cols=3, cell=800)
    finally:
        pc.quit()
    (out / f'{a.tag}-resize.txt').write_text('\n'.join(report) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
