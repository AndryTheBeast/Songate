"""Songate for Android, dev only: helpers the dev tests share for Settings' folded cards (session 21)."""
import time

from devlib import adb, screen, tap_at


def scroll(down=True, a=0.75, b=0.45):
    s = screen()
    X = int(12 * s.scale)
    if not down:
        a, b = b, a
    adb("shell", "input", "swipe", str(X), str(int(s.h * a * s.scale)), str(X), str(int(s.h * b * s.scale)), "400")
    time.sleep(1.2)


def kicker(word, tries=8):
    """A card's title (drawn letter by letter: joined back by their line): tapped, scrolled to first"""
    for _ in range(tries):
        s = screen()
        lines = {}
        for t, x, y, w, h in s.texts:
            if len(t) == 1:
                lines.setdefault(round(y), []).append((x, t, h))
        for y, v in lines.items():
            v.sort()
            if "".join(c for _, c, _ in v).strip() == word and s.h * 0.1 < y < s.h - 170:
                tap_at(s, v[0][0] + 20, y + v[0][2] / 2)
                time.sleep(1.3)
                return True
        scroll()
    return False
