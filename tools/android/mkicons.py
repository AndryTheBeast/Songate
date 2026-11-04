"""Songate for Android: Android drawables from source/icons.inc, so that what Android draws for Songate (the music controls on the
lock screen and in the notification, the status bar) uses Songate's own icons. Writes android/app/src/main/res/drawable/ic_*.xml.
    python tools/android/mkicons.py
"""
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'android', 'app', 'src', 'main', 'res', 'drawable')
ICONS = {'ICON_PLAY': 'ic_play', 'ICON_PAUSE': 'ic_pause', 'ICON_PREV': 'ic_prev', 'ICON_NEXT': 'ic_next', 'ICON_HEART': 'ic_heart',
         'ICON_HEART_F': 'ic_heart_f', 'ICON_ORDER': 'ic_order', 'ICON_REPEAT': 'ic_repeat', 'ICON_REPEAT1': 'ic_repeat1',
         'ICON_SHUFFLE': 'ic_shuffle'}

src = open(os.path.join(ROOT, 'source', 'icons.inc'), encoding='utf-8').read()
svgs = dict(re.findall(r'inline constexpr char (ICON_\w+)\[\] = R"SVG\((.*?)\)SVG";', src))


def attrs(tag):
    return dict(re.findall(r'([\w-]+)="([^"]*)"', tag))


def paths(svg):
    """Each shape as Android path data with its own fill or stroke, the <svg>'s defaults under the shape's own."""
    top = attrs(re.match(r'<svg[^>]*>', svg).group(0))
    out = []
    for tag, body in re.findall(r'<(path|rect|circle)([^>]*)/>', svg):
        a = dict(top)
        a.update(attrs(body))
        if tag == 'path':
            d = a['d']
        elif tag == 'rect':   # a rounded rectangle as a path
            x, y, w, h, r = (float(a.get(k, 0)) for k in ('x', 'y', 'width', 'height', 'rx'))
            d = (f'M{x + r},{y}h{w - 2 * r}a{r},{r} 0 0 1 {r},{r}v{h - 2 * r}a{r},{r} 0 0 1 {-r},{r}h{-(w - 2 * r)}'
                 f'a{r},{r} 0 0 1 {-r},{-r}v{-(h - 2 * r)}a{r},{r} 0 0 1 {r},{-r}z')
        else:
            cx, cy, r = float(a['cx']), float(a['cy']), float(a['r'])
            d = f'M{cx - r},{cy}a{r},{r} 0 1 0 {2 * r},0a{r},{r} 0 1 0 {-2 * r},0z'
        p = [f'android:pathData="{d}"']
        if a.get('fill', '#ffffff') not in ('none',):
            p.append('android:fillColor="#FFFFFFFF"')
        if a.get('stroke', 'none') != 'none':
            p += ['android:strokeColor="#FFFFFFFF"', f'android:strokeWidth="{a.get("stroke-width", "1")}"',
                  f'android:strokeLineCap="{a.get("stroke-linecap", "butt")}"', f'android:strokeLineJoin="{a.get("stroke-linejoin", "miter")}"']
        out.append('    <path ' + '\n          '.join(p) + ' />')
    return top.get('viewBox', '0 0 24 24').split()[2:], out


def write(name, svg_list):
    view = None
    body = []
    for s in svg_list:
        view, ps = paths(s)
        body += ps
    with open(os.path.join(OUT, name + '.xml'), 'w', newline='\n') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<!-- tools/android/mkicons.py, from source/icons.inc: do not edit -->\n'
                f'<vector xmlns:android="http://schemas.android.com/apk/res/android" android:width="24dp" android:height="24dp"\n'
                f'    android:viewportWidth="{view[0]}" android:viewportHeight="{view[1]}">\n' + '\n'.join(body) + '\n</vector>\n')


os.makedirs(OUT, exist_ok=True)
for key, name in ICONS.items():
    write(name, [svgs[key]])
write('ic_notify', [svgs['ICON_BRAND_ARCH'], svgs['ICON_BRAND_NOTE']])   # the status bar: the gate and its note, one colour
print(len(ICONS) + 1, 'written')
