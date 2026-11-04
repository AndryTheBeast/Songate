"""Songate for Android, dev only: what may be wrong on the screen the dev build shows now (devlib.py's items.txt).
   python tools/android/devaudit.py            the screen now, its problems one a line
   from devaudit import audit; audit(screen()) -> [(kind, text)]
Kinds: words-overlap (two words drawn over each other), words-off (words past the screen's side), words-cut (words that end in …),
control-off (a control past the side), control-overlap (two controls that cross without one holding the other), small (a control
under 36 units, about 36 dp, on a side), words-across (words half over a control's edge). Drawn words are recorded before any clip,
so words scrolled under a page's edge can be reported: the screenshot says. Nothing of it ships."""
import sys

MIN_TOUCH = 36
FIXED = ("ph.nav", "ph.mini", "ph.mlike", "ph.mplay", "ph.mnext", "ph.back", "nav.", "tb.", "pb.")


def inter(a, b):
    """The overlap of two boxes (x, y, w, h): its width and height, 0 when they do not meet."""
    w = min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0])
    h = min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1])
    return (w, h) if w > 0 and h > 0 else (0, 0)


def holds(a, b, slack=1.5):
    return a[0] - slack <= b[0] and a[1] - slack <= b[1] and a[0] + a[2] + slack >= b[0] + b[2] and a[1] + a[3] + slack >= b[1] + b[3]


def audit(s, skip_items=()):
    out = []
    W, H = s.w, s.h
    on = lambda b: b[1] + b[3] > 0 and b[1] < H and b[0] + b[2] > 0 and b[0] < W   # on screen at all
    # with something over the page (a sheet, a card), only that layer: the page under it is dimmed and takes no touch
    layered = bool(s.top) or any(r == "Modal" for r in s.region.values())
    texts = [(t, (x, y, w, h), i in s.cut) for i, (t, x, y, w, h) in enumerate(s.texts)
             if t.strip() and on((x, y, w, h)) and (not layered or i in s.top)]
    items = [(k, v) for k, v in s.items.items() if on(v) and not any(k.startswith(p) for p in skip_items)
             and (not layered or s.region.get(k) == "Modal")]
    # words drawn over other words
    seen = set()
    for i, (ta, a, _) in enumerate(texts):
        for tb, b, _ in texts[i + 1:]:
            if a == b:
                continue   # the same words twice in one place (a glow, a shadow)
            w, h = inter(a, b)
            if w > 3 and h > min(a[3], b[3]) * 0.35:
                key = (ta, tb)
                if key not in seen:
                    seen.add(key)
                    out.append(("words-overlap", f"'{ta}' at {a[0]:.0f},{a[1]:.0f} and '{tb}' at {b[0]:.0f},{b[1]:.0f}"))
    for t, b, cut in texts:
        if b[0] < -1 or b[0] + b[2] > W + 1:
            out.append(("words-off", f"'{t}' {b[0]:.0f}..{b[0] + b[2]:.0f} of {W}"))
        if cut:
            out.append(("words-cut", f"'{t}' at {b[0]:.0f},{b[1]:.0f} ({b[2]:.0f} wide)"))
    for k, b in items:
        if b[0] < -1 or b[0] + b[2] > W + 1:
            out.append(("control-off", f"{k} {b[0]}..{b[0] + b[2]} of {W}"))
        if (b[2] < MIN_TOUCH or b[3] < MIN_TOUCH) and b[2] * b[3] > 0:
            out.append(("small", f"{k} {b[2]}x{b[3]}"))
    fixed = lambda k: k.startswith(FIXED)   # the bars that stay while the page scrolls under them
    for i, (ka, a) in enumerate(items):
        for kb, b in items[i + 1:]:
            if fixed(ka) != fixed(kb):
                continue   # a page's row passing under a bar: the page's own clip hides it
            w, h = inter(a, b)
            if w > 2 and h > 2 and not holds(a, b) and not holds(b, a):
                out.append(("control-overlap", f"{ka} {a} and {kb} {b}"))
    for t, tb, _ in texts:
        for k, b in items:
            w, h = inter(tb, b)
            if w > 3 and h > 3 and not holds(b, tb, 2) and w < tb[2] - 3 and tb[0] < b[0] + b[2] - 2 and tb[0] + tb[2] > b[0] + 2:
                if tb[0] < b[0] - 2 or tb[0] + tb[2] > b[0] + b[2] + 2:   # across its left or right edge (above and below: rows of a list)
                    out.append(("words-across", f"'{t}' {tb[0]:.0f}..{tb[0] + tb[2]:.0f} over {k}'s edge {b[0]}..{b[0] + b[2]}"))
    return out


if __name__ == "__main__":
    from devlib import screen
    s = screen()
    print(f"size {s.w}x{s.h}, {len(s.items)} controls, {len(s.texts)} words")
    for kind, text in audit(s):
        print(f"  {kind:16} {text}")
