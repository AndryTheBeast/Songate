"""Songate for Android, dev only: devsizes.py's walks as contact sheets, the build before over the build after, one sheet for each
   screen and way it is held (JPEG, at most 2400 wide), labelled with the screen, its width in dp and its shape.
       python -I tools/android/devsheet.py <before folder> <after folder> <out folder>
   Nothing of it ships."""
import glob
import os
import re
import sys
from math import gcd

from PIL import Image, ImageDraw, ImageFont

DENS = {"720x1280-16x9": 320, "1080x1920-16x9": 420, "1080x2400-20x9": 420, "1080x2520-21x9": 420, "1080x2640-22x9": 420, "904x2316-cover": 420,
        "1768x2208-fold": 420, "1200x2000-tab": 240, "1600x2560-tab": 320, "1536x2048-4x3": 320, "1080x2400-big": 540, "1080x2400-small": 356}


def head(folder):
    """What the app made of the window: from the first stop's items ("scale s size w h")."""
    f = sorted(glob.glob(os.path.join(folder, "*.items.txt")))
    if not f:
        return "not run"
    t = open(f[0], encoding="utf-8").read()
    m = re.search(r"scale ([\d.]+) size (\d+) (\d+)", t)
    layout = "the phone's layout" if any("ph.nav" in open(p, encoding="utf-8").read() for p in f) else "the console's layout"
    return f"app {m[2]}x{m[3]} at {m[1]} px each, {layout}" if m else "?"


def main():
    before, after, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    font = ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf", 22)
    small = ImageFont.truetype(r"C:\Windows\Fonts\segoeui.ttf", 15)
    names = sorted({os.path.basename(p) for p in glob.glob(os.path.join(before, "*-*")) + glob.glob(os.path.join(after, "*-*")) if os.path.isdir(p)})
    for name in names:
        screen, way = name.rsplit("-", 1)
        w, h = [int(v) for v in screen.split("-")[0].split("x")]
        d = DENS.get(screen, 420) / 160
        if way == "sideways":
            w, h = h, w
        g = gcd(w, h)
        shape = f"{max(w, h) / min(w, h) * 9:.1f}:9"
        rows = []
        for tag, folder in (("BEFORE", os.path.join(before, name)), ("AFTER", os.path.join(after, name))):
            pngs = sorted(glob.glob(os.path.join(folder, "*.png")))
            rows.append((tag, folder, pngs))
        upright = h > w
        cols = 8 if upright else 6
        if not any(r[2] for r in rows):
            continue
        tw = min(300 if upright else 392, (2400 - 12) // cols - 8)
        th = int(tw * h / w)
        blocks = []
        for tag, folder, pngs in rows:
            n = (len(pngs) + cols - 1) // cols
            blocks.append((tag, folder, pngs, n))
        H = 12 + sum(40 + n * (th + 26) for _, _, _, n in blocks)
        sheet = Image.new("RGB", (cols * (tw + 8) + 16, H), (20, 20, 26))
        dr = ImageDraw.Draw(sheet)
        y = 8
        for tag, folder, pngs, n in blocks:
            dr.text((10, y + 4), f"{tag}  ·  {w}x{h} px, {w / d:.0f}x{h / d:.0f} dp, {shape} {way}  ·  {head(folder)}", font=font,
                    fill=(255, 205, 120) if tag == "BEFORE" else (140, 235, 170))
            y += 40
            for i, p in enumerate(pngs):
                x = 8 + (i % cols) * (tw + 8)
                yy = y + (i // cols) * (th + 26)
                dr.text((x, yy), os.path.basename(p)[:-4], font=small, fill=(190, 195, 215))
                im = Image.open(p).convert("RGB")
                k = min(tw / im.width, th / im.height)   # (a frame turned the other way, the Canvas from upright: whole, in its cell)
                im = im.resize((max(1, int(im.width * k)), max(1, int(im.height * k))), Image.LANCZOS)
                sheet.paste(im, (x + (tw - im.width) // 2, yy + 22 + (th - im.height) // 2))
            y += n * (th + 26)
        sheet.save(os.path.join(out, f"{name}.jpg"), quality=82)
        print(name, sheet.size)


main()
