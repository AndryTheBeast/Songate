"""Songate for PC, dev only: the text card's Cancel and OK, pressed with real mouse messages (the card runs its own loop, where
devctl's taps do not reach). python tools/preview/devtextcard.py . Nothing of it ships."""
import ctypes
import time
from ctypes import wintypes

import pcdrive

u = ctypes.windll.user32
u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # real pixels, as the app's window has them
import sys
REAL = "real" in sys.argv
OUT = pcdrive.HERE / 'out' / 'mouse'


def click(h, x, y):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    u.PostMessageW(h, 0x0200, 0, lp)      # WM_MOUSEMOVE
    time.sleep(0.05)
    u.PostMessageW(h, 0x0201, 1, lp)      # WM_LBUTTONDOWN
    time.sleep(0.08)
    u.PostMessageW(h, 0x0202, 0, lp)      # WM_LBUTTONUP
    time.sleep(0.6)


def real_click(h, x, y):
    was = wintypes.POINT()
    u.GetCursorPos(ctypes.byref(was))
    p = wintypes.POINT(int(x), int(y))
    u.ClientToScreen(h, ctypes.byref(p))
    u.SetForegroundWindow(h)
    u.SetCursorPos(p.x, p.y)
    time.sleep(0.15)
    u.mouse_event(2, 0, 0, 0, 0)
    time.sleep(0.08)
    u.mouse_event(4, 0, 0, 0, 0)
    time.sleep(0.2)
    u.SetCursorPos(was.x, was.y)
    time.sleep(0.5)


def card_open(pc):
    try:
        return 'focus textcard' in pc.items_path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return False


def grab(h, name):
    from PIL import ImageGrab
    r = wintypes.RECT()
    u.GetClientRect(h, ctypes.byref(r))
    p = wintypes.POINT(0, 0)
    u.ClientToScreen(h, ctypes.byref(p))
    im = ImageGrab.grab((p.x, p.y, p.x + r.right, p.y + r.bottom), all_screens=True)
    OUT.mkdir(parents=True, exist_ok=True)
    im.convert('RGB').save(OUT / name)
    return r.right, r.bottom


def main():
    pc = pcdrive.PC()
    try:
        pc.wait(lambda s: s.w > 0, 40)
        pc.restore()
        time.sleep(9)
        pc.clear_notices()
        time.sleep(2)
        pc.clear_notices()
        h = pc.hwnd()
        u.SetForegroundWindow(h)
        for which in ('cancel', 'ok'):
            s = pc.fresh_screen(1)
            cw, chh = 0, 0
            pc.tap(s.w / 2, 14)   # the title bar's search
            time.sleep(1.0)
            print(which, 'card open:', card_open(pc))
            cw, chh = grab(h, f'tc-{which}-open.png')
            fit = cw / s.w
            # the card's own layout (view_textcard.cpp)
            w = min(620.0, s.w - 40.0)
            pad, field_h, button_h = 24, 46, 36
            hh = pad + 26 + 14 + field_h + 18 + button_h + pad
            x = (s.w - w) / 2
            room = chh / fit
            y = max(8.0, min(max(40.0, s.h * 0.28 - hh / 2), (room - hh) / 2))
            by = y + pad + 26 + 14 + field_h + 18
            okx = x + w - pad - 92
            cx = (okx + 46) if which == 'ok' else (okx - 8 - 46)
            (real_click if REAL else click)(h, cx * fit, (by + 18) * fit)
            time.sleep(0.8)
            pc.ctl('move 5 5')
            time.sleep(0.6)
            print(which, 'card still open after the click:', card_open(pc), 'at', round(cx * fit), round((by + 18) * fit), 'client', cw, chh, 'app', s.w, s.h)
            grab(h, f'tc-{which}-after.png')
            if card_open(pc):
                u.PostMessageW(h, 0x0100, 0x1B, 0)   # Esc
                u.PostMessageW(h, 0x0101, 0x1B, 0)
                time.sleep(0.8)
    finally:
        pc.quit()


main()
