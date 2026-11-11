"""Songate for PC, dev only: every page at many window sizes, screens and scales, without resizing a real window (GF_CTL's made-up
sizes: source/devctl.cpp). Frames, a contact sheet for each size, and what devaudit.py finds wrong in each.

    python tools/preview/devsizes.py <tag> [--exe <Songate.exe>] [--sizes a,b] [--pages a,b] [--out <folder>]

<tag> names the run (before, after): frames go to tools/preview/out/sizes/<tag>/<size>/<page>.jpg, sheets to <out> (default the same
folder) as <tag>-<size>.jpg, findings to <tag>-findings.txt. Sizes are given as on a screen at 100 %: at another scale the window has
that many times the pixels. The library is D:\\Music, read only; the settings are pcdrive.py's own. Nothing of it ships."""
import argparse
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'android'))
import pcdrive   # noqa: E402
from devaudit import audit   # noqa: E402

# name: (window w, h, screen w, h, scale %): all but the scale as at 100 %
SIZES = {
    '0800x500': (800, 500, 1920, 1080, 100),
    '0900x600': (900, 600, 1920, 1080, 100),
    '1000x600': (1000, 600, 1920, 1080, 100),
    '1024x768': (1024, 768, 1920, 1080, 100),
    '1160x700': (1160, 700, 1920, 1080, 100),
    '1180x700': (1180, 700, 1920, 1080, 100),
    '1280x720': (1280, 720, 1920, 1080, 100),
    '1366x768-max': (1366, 768, 1366, 768, 100),
    '1600x900': (1600, 900, 1920, 1080, 100),
    '1920x1080-max': (1920, 1080, 1920, 1080, 100),
    '2560x1440-max': (2560, 1440, 2560, 1440, 100),
    '3440x1440-max': (3440, 1440, 3440, 1440, 100),
    '0960x1080-half': (960, 1080, 1920, 1080, 100),
    '0640x1080-third': (640, 1080, 1920, 1080, 100),
    '1280x1024-max': (1280, 1024, 1280, 1024, 100),
    # other scales: the same screen in the hand, more pixels
    '1536x864-max-125': (1536, 864, 1536, 864, 125),      # a 1920x1080 screen at 125 %
    '1280x720-max-150': (1280, 720, 1280, 720, 150),      # a 1920x1080 screen at 150 %
    '1280x800-max-200': (1280, 800, 1280, 800, 200),      # a 2560x1600 screen at 200 %
    '0900x560-200': (900, 560, 1280, 800, 200),           # a window on it
    '2560x1440-max-150': (2560, 1440, 2560, 1440, 150),   # a 3840x2160 screen at 150 %
    '1920x1080-max-200': (1920, 1080, 1920, 1080, 200),   # a 3840x2160 screen at 200 %
    '1000x640-150': (1000, 640, 2560, 1440, 150),
}
BASE = (1280, 720, 1280, 720, 100)
B, PLUS, MINUS = 2, 1024, 2048


def first(s, prefix):
    for k, r in s.items.items():
        if k.startswith(prefix):
            return k
    return None


def nav(pc, name):
    if not pc.tap_item(name):
        print('  (no', name, ')')


def go_album(pc):
    nav(pc, 'nav.albums')
    pc.ctl('wheel 700 400 40')   # (to the top: the page before may have left it scrolled)
    time.sleep(1.2)
    s = pc.fresh_screen()
    k = next((k for k, r in s.items.items() if k.startswith('lib.tile#') and r[1] > 120), None)
    if k:
        pc.tap_item(k)


def go_albums_scrolled(pc):
    nav(pc, 'nav.albums')
    pc.ctl('wheel 700 400 -14')
    time.sleep(1.2)


def go_menu(pc):
    nav(pc, 'nav.library')
    s = pc.fresh_screen()
    if 'trk.r#1' in s.items:
        x, y, w, h = s.items['trk.r#1']
        pc.ctl(f'move {x + w / 2:.0f} {y + h / 2:.0f}')
        time.sleep(0.4)
        pc.ctl(f'pad {PLUS}')
        time.sleep(0.7)


def go_settings(n):
    def go(pc):
        nav(pc, 'nav.settings')
        nav(pc, 'set.cat' + (f'#{n}' if n else ''))
    return go


def go_lyrics(pc):
    nav(pc, 'nav.now')
    nav(pc, 'np.tab#1')


def go_now(pc):
    nav(pc, 'nav.now')
    nav(pc, 'np.tab')


def go_canvas(pc):
    nav(pc, 'nav.now')
    pc.ctl(f'pad {MINUS}')
    time.sleep(1.5)


def go_update(pc):
    nav(pc, 'nav.library')
    nav(pc, 'side.update')
    time.sleep(0.5)
    s = pc.fresh_screen()
    k = first(s, 'upd.') or first(s, 'side.upd')
    if not any(r == 'Modal' for r in s.region.values()):
        print('  (the update sheet did not open:', [k for k in s.items if 'upd' in k], ')')


def go_jobs(pc):
    nav(pc, 'nav.library')
    nav(pc, 'side.job')
    time.sleep(0.5)


# name: (how to get there at the base size, what to press after its frames, the environment it needs)
PAGES = {
    'songs': (lambda pc: nav(pc, 'nav.library'), 0, None),
    'albums': (lambda pc: nav(pc, 'nav.albums'), 0, None),
    'albums-scrolled': (go_albums_scrolled, 0, None),
    'album': (go_album, 0, None),
    'now': (go_now, 0, None),
    'now-lyrics': (go_lyrics, 0, None),
    'search': (lambda pc: (pc.ctl('search love'), time.sleep(1.2)), 0, None),
    'playlists': (lambda pc: nav(pc, 'nav.playlists'), 0, None),
    'playlist': (lambda pc: nav(pc, 'nav.pl#-1'), 0, None),
    'settings-0': (go_settings(0), 0, None),
    'settings-1': (go_settings(1), 0, None),
    'settings-2': (go_settings(2), 0, None),
    'settings-4': (go_settings(4), 0, None),
    'settings-9': (go_settings(9), 0, None),
    'menu': (go_menu, B, None),
    'canvas': (go_canvas, MINUS, None),
    'update': (go_update, B, 'update'),
    'jobs': (go_jobs, B, 'jobs'),
}
ENVS = {None: None, 'update': {'GF_FAKEUPDATE': '2.0.0'}, 'jobs': {'GF_JOBS': '1'}}
COUNTED = ('words-overlap', 'words-off', 'control-off', 'control-overlap', 'words-across')


def px(size):
    w, h, sw, sh, pct = size
    k = pct / 100
    return round(w * k), round(h * k), round(sw * k), round(sh * k), round(96 * k)


def set_size(pc, size, settle=1.1):
    w, h, sw, sh, dpi = px(size)
    pc.size(w, h, screen=(sw, sh), dpi=dpi, settle=settle)


def setup(pc):
    """Something playing, and a few songs liked, so Now playing and Liked have something to show."""
    pc.wait(lambda s: s.w > 0, 40)
    pc.restore()
    set_size(pc, BASE)
    for _ in range(14):   # the notices of a first start, as they come
        time.sleep(1)
        pc.clear_notices()
    nav(pc, 'nav.library')
    s = pc.wait(lambda s: 'trk.r#2' in s.items, 60)
    if not s:
        print('no songs listed: is the library read?')
        return
    if s.track in ('-', ''):
        nav(pc, 'trk.r#2')
        time.sleep(1.5)
    liked = pcdrive.CFG / 'Songate' / 'liked.txt'
    if not liked.exists() or len(liked.read_text(encoding='utf-8', errors='replace').split('\n')) < 4:
        for _ in range(4):
            nav(pc, 'pb.like')
            nav(pc, 'pb.next')
            time.sleep(0.8)
    s = pc.fresh_screen()
    if s.playing:
        nav(pc, 'pb.play')   # paused: the frames compare


def sheet(frames, title, path, cols=4, cell=600):
    from PIL import Image, ImageDraw, ImageFont
    try:
        font = ImageFont.truetype('segoeui.ttf', 20)
        small = ImageFont.truetype('segoeui.ttf', 15)
    except OSError:
        font = small = ImageFont.load_default()
    thumbs = []
    for name, im, note in frames:
        k = min(1.0, cell / im.width)
        t = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS) if k < 1 else im
        thumbs.append((name, t, note))
    cw = max(t.width for _, t, _ in thumbs) + 12
    ch = max(t.height for _, t, _ in thumbs) + 34
    rows = (len(thumbs) + cols - 1) // cols
    out = Image.new('RGB', (cw * cols + 12, ch * rows + 52), (24, 24, 28))
    d = ImageDraw.Draw(out)
    d.text((12, 10), title, fill=(240, 240, 240), font=font)
    for i, (name, t, note) in enumerate(thumbs):
        x, y = 12 + (i % cols) * cw, 48 + (i // cols) * ch
        d.text((x, y), name + ('   ' + note if note else ''), fill=(255, 190, 120) if note else (190, 190, 190), font=small)
        out.paste(t, (x, y + 24))
    path.parent.mkdir(parents=True, exist_ok=True)
    if out.width > 2500:
        out = out.resize((2500, round(out.height * 2500 / out.width)), Image.LANCZOS)
    out.save(path, quality=88)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tag')
    ap.add_argument('--exe')
    ap.add_argument('--sizes')
    ap.add_argument('--pages')
    ap.add_argument('--out')
    a = ap.parse_args()
    sizes = [s for s in SIZES if not a.sizes or any(s.startswith(w) for w in a.sizes.split(','))]
    pages = [p for p in PAGES if not a.pages or p in a.pages.split(',')]
    root = HERE / 'out' / 'sizes' / a.tag
    out = Path(a.out) if a.out else root
    frames = {s: [] for s in sizes}
    findings = []
    for fake in ENVS:
        mine = [p for p in pages if PAGES[p][2] == fake]
        if not mine:
            continue
        pc = pcdrive.PC(env=ENVS[fake], exe=a.exe)
        try:
            setup(pc)
            for p in mine:
                go, after, _ = PAGES[p]
                set_size(pc, BASE, settle=0.6)
                pc.clear_notices()
                go(pc)
                time.sleep(0.5)
                for sname in sizes:
                    set_size(pc, SIZES[sname])
                    s = pc.fresh_screen(0.3)
                    if 'notice.later' in s.items:   # (a notice came over the page meanwhile)
                        pc.clear_notices()
                        s = pc.fresh_screen(0.6)
                    im = pc.shot(root / sname / (p + '.jpg'))
                    found = [f for f in audit(s) if f[0] in COUNTED]
                    note = f'{s.w}x{s.h} at {s.scale:g}' + (f' · {len(found)} found' if found else '')
                    frames[sname].append((p, im, note))
                    for kind, text in found:
                        findings.append(f'{sname:20} {p:16} {kind:16} {text}')
                    print(f'{sname:20} {p:16} layout {s.w}x{s.h} scale {s.scale:g} found {len(found)}', flush=True)
                if after:
                    set_size(pc, BASE, settle=0.4)
                    pc.ctl(f'pad {after}')
                    time.sleep(0.6)
        finally:
            pc.quit()
    for sname in sizes:
        if frames[sname]:
            w, h, sw, sh, pct = SIZES[sname]
            pw, ph, psw, psh, _ = px(SIZES[sname])
            title = f'{a.tag}: a {pw}x{ph} window on a {psw}x{psh} screen at {pct} %  ({w}x{h} as at 100 %)'
            sheet(frames[sname], title, out / f'{a.tag}-{sname}.jpg')
    (out / f'{a.tag}-findings.txt').write_text('\n'.join(findings) + '\n', encoding='utf-8')
    print(len(findings), 'findings')


if __name__ == '__main__':
    main()
