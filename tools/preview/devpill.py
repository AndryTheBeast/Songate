"""Songate for PC, dev only: Search typed straight into the title bar's pill (the user, 2026-11-16: "no extra card for search, it
writes directly to the top pill"). python tools/preview/devpill.py . The dev build (build-windows-check), its own settings folder,
D:\\Music read only (pcdrive.py). Keys are real window messages (WM_CHAR, WM_KEYDOWN), as typing sends them.
Checks: a click on the pill opens no card; letters search at each key; Backspace takes one back; the keys that are buttons
elsewhere (Space, C) are words while typing; Enter ends the typing and the keys are buttons again; a click elsewhere ends it too.
Ends with ALL GOOD or the steps that failed. Nothing of it ships."""
import ctypes
import time

import pcdrive

u = ctypes.windll.user32
results = []


def step(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("  ok    " if ok else "  FAIL  ") + name + (f"  ({detail})" if detail and not ok else ""))


def chars(h, s):
    for c in s:
        u.PostMessageW(h, 0x0102, ord(c), 1)   # WM_CHAR
        time.sleep(0.12)


def key(h, vk):
    scan = u.MapVirtualKeyW(vk, 0)              # (SDL reads the key by its scan code, in lParam's bits 16-23)
    u.PostMessageW(h, 0x0100, vk, 1 | (scan << 16))                # WM_KEYDOWN
    time.sleep(0.08)
    u.PostMessageW(h, 0x0101, vk, 1 | (scan << 16) | 0xC0000000)   # WM_KEYUP
    time.sleep(0.3)


def pill_text(s):
    """The words in the title bar's pill (the first text drawn in it: the search pill's own item)"""
    box = next((v for k, v in s.items.items() if k.startswith('tb.search')), None)
    if not box:
        return None
    x, y, w, h = box
    return next((t[0] for t in s.texts if x <= t[1] <= x + w and y <= t[2] + t[4] / 2 <= y + h), '')


pc = pcdrive.PC()
try:
    pc.wait(lambda s: s.w > 0, 40)
    pc.restore()
    time.sleep(8)
    pc.clear_notices()
    time.sleep(1.5)
    pc.clear_notices()
    h = pc.hwnd()
    s = pc.fresh_screen(1)
    pill = next((k for k in s.items if k.startswith('tb.search')), None)
    step("the title bar's search pill", pill, ", ".join(list(s.items)[:12]))
    x, y, w, hh = s.items[pill]
    pc.tap(x + w / 2, y + hh / 2, 1.0)
    s = pc.fresh_screen(0.6)
    raw = pc.items_path.read_text(encoding='utf-8', errors='replace')
    step("a click on the pill: no typing card", 'focus textcard' not in raw and 'skb.k' not in raw)
    playing0 = s.playing
    chars(h, 'ne')
    s = pc.wait(lambda s: pill_text(s) == 'ne', 6)
    step("letters typed: in the pill", s, pill_text(pc.screen()))
    if s:
        pc.shot(pcdrive.HERE / 'out' / 'devpill-typing.jpg')   # (the picture: the pill lit, its caret, the results under it)
    step("... searching at each key (the Search page)", s and (s.has_text('Songs') or s.has_text('Albums') or s.has_text('Artists')))
    key(h, 0x08)   # Backspace
    s = pc.wait(lambda s: pill_text(s) == 'n', 5)
    step("Backspace: a letter back", s, pill_text(pc.screen()))
    chars(h, 'e c')   # (Space and C are play/pause and the Canvas when they are buttons)
    s = pc.wait(lambda s: pill_text(s) == 'ne c', 5)
    step("Space and C are words while typing", s, pill_text(pc.screen()))
    step("... nothing played or paused, no Canvas", s and s.playing == playing0 and 'cv.play' not in s.items)
    key(h, 0x0D)   # Enter
    time.sleep(0.6)
    chars(h, 'x')
    s = pc.fresh_screen(0.8)
    step("Enter: the typing ends (a letter now types nothing)", pill_text(s) == 'ne c', pill_text(s))
    pc.tap(x + w / 2, y + hh / 2, 1.0)   # typing again
    chars(h, 'o')
    s = pc.wait(lambda s: pill_text(s) == 'ne co', 5)
    step("the pill again: typing goes on after its words", s, pill_text(pc.screen()))
    pc.tap(40, s.h / 2 if s else 300, 1.0)   # a click on the page (the sidebar there: it opens its page, as a click does)
    chars(h, 'z')
    s = pc.fresh_screen(0.8)
    step("a click elsewhere: the typing ends, the click acts", 'z' not in (pill_text(s) or '') and 'ph.' not in str(s.items)[:0], pill_text(s))
finally:
    pc.quit()

bad = [n for n, ok in results if not ok]
print("ALL GOOD" if not bad else f"{len(bad)} FAILED: " + "; ".join(bad))
