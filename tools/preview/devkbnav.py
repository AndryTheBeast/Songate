"""The keyboard card's D-pad on the PC dev build, every row both ways, on the cards over a sheet, over a page and Search's.
Prints, per card, whether Right reaches each row's last key and Left its first, and anything the focus went to outside the card."""
import os, re, sys, time
sys.path.insert(0, r'C:\Users\andre\Desktop\Songate\tools\preview')
import pcdrive

A, B, DOWN, UP, RIGHT, LEFT, PLUS = 1, 2, 1 << 15, 1 << 13, 1 << 14, 1 << 12, 1 << 10


def pad(pc, bits, k=1):
    for _ in range(k):
        pc.ctl(f'pad {bits}'); time.sleep(0.22)


def focus(pc):
    time.sleep(0.22)
    t = ''
    for _ in range(40):
        try:
            t = open(pc.items_path, encoding='utf-8', errors='replace').read(); break
        except OSError:
            time.sleep(0.05)
    m = re.search(r'focus (\S+)', t)
    return m[1] if m else '?'


def walk(pc, label):
    if not pc.wait(lambda s: 'skb.k' in s.items, 10):
        print(label, ': NO CARD', flush=True); return
    for _ in range(6):
        if focus(pc).startswith('skb.k'): break
        pad(pc, DOWN)
    pad(pc, UP, 4)   # the top row (Up past it may leave the card: that is noted, then back down)
    top = focus(pc)
    if not top.startswith('skb.k'):
        print(f'  {label}: Up from the keys goes to {top} (above the card, in sight)', flush=True)
        pad(pc, DOWN)
    bad, rows = [], []
    for r in range(5):
        start = focus(pc)
        right = [start]
        for _ in range(11):
            pad(pc, RIGHT); right.append(focus(pc))
        left = []
        for _ in range(11):
            pad(pc, LEFT); left.append(focus(pc))
        rows.append((start, right[-1], left[-1], len(set(right))))
        bad += [f for f in right + left if not f.startswith('skb.k')]
        pad(pc, DOWN)
    print(f'{label}:', flush=True)
    for start, far, back, n in rows:
        print(f'   row from {start}: Right reaches {far} ({n} keys), Left back to {back}', flush=True)
    print('   left the card:', sorted(set(bad)) or 'never', flush=True)
    pad(pc, B, 12)
    pad(pc, B)


pc = pcdrive.PC(fresh=True)
try:
    time.sleep(6); pc.clear_notices(); time.sleep(1); pc.clear_notices()
    # 1. a sheet under the card: the Automix picker's name
    pc.tap_item('nav.newpl'); time.sleep(1)
    pc.tap_text('Automix playlist'); time.sleep(1.5)
    pc.wait(lambda s: 'ap.r' in s.items, 40)
    pad(pc, LEFT); time.sleep(0.4)
    pc.tap_item('ap.r'); time.sleep(0.5)
    pc.tap_item('ap.done'); time.sleep(1.2)
    walk(pc, 'Automix name (over a sheet)')
    pc.ctl('pad 2'); time.sleep(0.6); pc.tap_item('ap.x'); time.sleep(1)
    # 2. a page under the card: Settings' search
    pc.tap_item('nav.settings'); time.sleep(1.5)
    s = pc.screen(); find = [n for n in s.items if 'find' in n or n.startswith('set.search')]
    print('settings search items:', find[:4], flush=True)
    if find:
        pad(pc, LEFT); time.sleep(0.3); pc.tap_item(find[0]); time.sleep(1)
        walk(pc, 'Settings search (over a page)')
    # 3. Search's own
    pc.tap_item('tb.search'); time.sleep(1.2)
    walk(pc, 'Search')
finally:
    pc.quit()
