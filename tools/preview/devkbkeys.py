"""The PC's keyboard taking over Songate's card (dev build): real key messages to the window. A name: a key opens the typing card
with its letter; Search: a key types into the pill, no card. Also the badges: none after a key."""
import ctypes, os, re, sys, time
from ctypes import wintypes
sys.path.insert(0, r'C:\Users\andre\Desktop\Songate\tools\preview')
import pcdrive

u = ctypes.windll.user32
A, B, DOWN, UP, RIGHT, LEFT, Y = 1, 2, 1 << 15, 1 << 13, 1 << 14, 1 << 12, 1 << 3
OUT = str(pcdrive.HERE / 'out' / 'kbkeys')
os.makedirs(OUT, exist_ok=True)


def items(pc):
    for _ in range(40):
        try:
            return open(pc.items_path, encoding='utf-8', errors='replace').read()
        except OSError:
            time.sleep(0.05)
    return ''


def pad(pc, bits, k=1):
    for _ in range(k):
        pc.ctl(f'pad {bits}'); time.sleep(0.3)


def key(h, vk, ch=None):
    scan = u.MapVirtualKeyW(vk, 0)
    u.PostMessageW(h, 0x0100, vk, 1 | (scan << 16))           # WM_KEYDOWN
    if ch:
        u.PostMessageW(h, 0x0102, ord(ch), 1 | (scan << 16))  # WM_CHAR
    time.sleep(0.08)
    u.PostMessageW(h, 0x0101, vk, 1 | (scan << 16) | (3 << 30))   # WM_KEYUP
    time.sleep(0.5)


pc = pcdrive.PC(fresh=True)
try:
    time.sleep(6); pc.clear_notices(); time.sleep(1); pc.clear_notices()
    h = pc.hwnd()
    # 1. a name: the Automix picker's
    pc.tap_item('nav.newpl'); time.sleep(1)
    pc.tap_text('Automix playlist'); time.sleep(1.5)
    pc.wait(lambda s: 'ap.r' in s.items, 40)
    pad(pc, LEFT); pc.tap_item('ap.r'); time.sleep(0.5); pc.tap_item('ap.done'); time.sleep(1.2)
    print('name card up:', bool(pc.wait(lambda s: 'skb.k' in s.items, 10)), flush=True)
    key(h, 0x58)   # X
    time.sleep(1.0)
    t = items(pc)
    print('after a key:', t.splitlines()[0][:120] if t else '?', flush=True)
    # the typing card: Esc cancels it
    key(h, 0x1B)
    time.sleep(1.0)
    print('after Esc:', items(pc).splitlines()[0][:120], flush=True)
    pc.ctl('pad 2'); time.sleep(0.5); pc.tap_item('ap.x'); time.sleep(1)
    # 2. Search: Y opens its card after a pad; a key then types into the pill
    pad(pc, Y); time.sleep(1)
    s = pc.screen(); print('search card up:', 'skb.k' in s.items, flush=True)
    pc.shot(os.path.join(OUT, '1-search-card.png'))
    key(h, 0x41)   # a
    key(h, 0x42)   # b
    time.sleep(0.8)
    s = pc.screen()
    print('after keys: card', 'skb.k' in s.items, '| texts with ab:', [x[0] for x in s.texts if 'ab' in x[0].lower()][:4], flush=True)
    pc.shot(os.path.join(OUT, '2-search-typed.png'))
finally:
    pc.quit()
