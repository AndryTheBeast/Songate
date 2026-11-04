"""Songate's icon for Android: the PC's icon (pc-icon.py: the gate and its note in the console's neon, on the dark tile) as an adaptive
icon, so each launcher can give it its own shape. The tile's gradient is the background layer, the glow, the gate and the note the
foreground; a one-colour mark for Android 13's themed icons; and the PC's rounded tile itself as the plain icon. Writes
android/app/src/main/res/mipmap-*.

    python tools/brand/android-icon.py          (Pillow, PyMuPDF)
"""
import io
import os

import fitz
import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', '..', 'android', 'app', 'src', 'main', 'res')
S = 1024


def svg(body, view='0 0 100 100'):
    doc = fitz.open('svg', f'<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" viewBox="{view}">{body}</svg>'.encode())
    pix = doc[0].get_pixmap(alpha=True)
    return Image.open(io.BytesIO(pix.tobytes('png'))).convert('RGBA')


def painted(mask, stops, ax, ay):
    """A shape in white (MuPDF's SVG has no gradients) coloured along the direction (ax, ay), its stops as (place, r, g, b)."""
    yy, xx = np.mgrid[0:S, 0:S] / (S - 1)
    t = (xx * ax + yy * ay) / (ax + ay)
    rgb = np.stack([np.interp(t, [s[0] for s in stops], [s[c] for s in stops]) for c in (1, 2, 3)], axis=2)
    out = np.dstack([rgb, np.asarray(mask.getchannel('A'), dtype=float)])
    return Image.fromarray(np.uint8(np.round(out)), 'RGBA')


# The PC's, unchanged (pc-icon.py)
NEON = [(0.18, 22, 214, 255), (0.45, 106, 123, 255), (0.66, 255, 63, 192), (0.86, 255, 45, 74)]
TILE = [(0, 16, 26, 66), (1, 5, 7, 15)]
PLACE = 'transform="translate(11.5 10.5) scale(0.77)"'
ARCH = f'<g {PLACE}><path d="M16 91V47a34 34 0 0 1 68 0V91" fill="none" stroke="#ffffff" stroke-width="17.5" stroke-linecap="round"/></g>'
NOTE = (f'<g {PLACE}><circle cx="43.5" cy="72.5" r="11.5" fill="#ffffff"/>'
        '<path d="M51 72V40.5l13.5 6" fill="none" stroke="#ffffff" stroke-width="9" stroke-linecap="round" stroke-linejoin="round"/></g>')

# Adaptive layers are 108 dp, of which launchers keep the middle 72 dp: drawn as the PC draws them, then shrunk so that the PC's tile
# (3..97 of 100) is that middle (18..90 dp), its mark in the same place on it
# (the mark itself a little smaller than on the PC's tile, K: a round launcher keeps only a 66 dp circle, and the gate's feet reach
# past it)
F = (72 / 108) / 0.94
K = 0.84


def placed(img):
    out = Image.new('RGBA', (S, S))
    size = round(S * F * K)
    out.alpha_composite(img.resize((size, size), Image.LANCZOS), ((S - size) // 2, (S - size) // 2))
    return out


background = painted(svg('<rect x="0" y="0" width="100" height="100" fill="#ffffff"/>'), TILE, 1, 1)   # the tile's gradient, edge to edge
arch = painted(svg(ARCH), NEON, 1, 0.55)
glow = arch.filter(ImageFilter.GaussianBlur(S * 0.035))
glow.putalpha(glow.getchannel('A').point(lambda a: int(a * 0.75)))
foreground = Image.new('RGBA', (S, S))
foreground.alpha_composite(glow)
foreground.alpha_composite(arch)
foreground.alpha_composite(svg(NOTE))
foreground = placed(foreground)
mono = placed(svg(ARCH + NOTE))   # white: the launcher colours it

# The plain icon: the PC's whole tile, as the .ico has it
tile = painted(svg('<rect x="3" y="3" width="94" height="94" rx="21" fill="#ffffff"/>'), TILE, 1, 1)
tglow = painted(svg(ARCH), NEON, 1, 0.55).filter(ImageFilter.GaussianBlur(S * 0.035))
tglow.putalpha(tglow.getchannel('A').point(lambda a: int(a * 0.75)))
plain = tile.copy()
plain.alpha_composite(tglow)
from PIL import ImageChops
plain.putalpha(ImageChops.multiply(plain.getchannel('A'), tile.getchannel('A')))
plain.alpha_composite(painted(svg(ARCH), NEON, 1, 0.55))
plain.alpha_composite(svg(NOTE))

for name, k in (('mdpi', 1), ('hdpi', 1.5), ('xhdpi', 2), ('xxhdpi', 3), ('xxxhdpi', 4)):
    d = os.path.join(RES, 'mipmap-' + name)
    os.makedirs(d, exist_ok=True)
    layer = round(108 * k)
    for img, file in ((background, 'ic_launcher_background.png'), (foreground, 'ic_launcher_foreground.png'), (mono, 'ic_launcher_monochrome.png')):
        img.resize((layer, layer), Image.LANCZOS).save(os.path.join(d, file))
    plain.resize((round(48 * k), round(48 * k)), Image.LANCZOS).save(os.path.join(d, 'ic_launcher.png'))
d = os.path.join(RES, 'mipmap-anydpi-v26')
os.makedirs(d, exist_ok=True)
with open(os.path.join(d, 'ic_launcher.xml'), 'w', newline='\n') as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n'
            '<!-- tools/brand/android-icon.py: the PC\'s icon, in layers the launcher shapes -->\n'
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@mipmap/ic_launcher_background" />\n'
            '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
            '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n'
            '</adaptive-icon>\n')
# a preview: the layers as three launchers would cut them (circle, squircle, rounded square), and the plain icon
prev = Image.new('RGBA', (4 * 300, 300), (230, 230, 230, 255))
full = background.copy()
full.alpha_composite(foreground)
full = full.resize((432, 432), Image.LANCZOS).crop((72, 72, 360, 360)).resize((256, 256), Image.LANCZOS)   # the middle 72 of 108
for i, shape in enumerate(('circle', 'squircle', 'rounded')):
    m = Image.new('L', (256, 256))
    from PIL import ImageDraw
    dm = ImageDraw.Draw(m)
    if shape == 'circle': dm.ellipse((0, 0, 255, 255), fill=255)
    elif shape == 'squircle': dm.rounded_rectangle((0, 0, 255, 255), radius=90, fill=255)
    else: dm.rounded_rectangle((0, 0, 255, 255), radius=40, fill=255)
    c = Image.new('RGBA', (256, 256))
    c.paste(full, (0, 0), m)
    prev.alpha_composite(c, (i * 300 + 22, 22))
prev.alpha_composite(plain.resize((256, 256), Image.LANCZOS), (3 * 300 + 22, 22))
prev.save(os.path.join(HERE, 'android-icon-preview.png'))
print('written')
