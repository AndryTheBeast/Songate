"""Songate for Android: checks an APK before it goes anywhere (the installer's checkinstaller.py, for Android).
   python tools/android/checkapk.py [path/to/app.apk]
Every file inside is unpacked and searched for the secrets (report.key's token, lastfm.key's lines), whole, in other encodings and in
8-byte pieces; they are never printed. Also: the key files stay out of git, FFmpeg has no GPL or non-free parts, the LGPL libraries
are files of their own. Ends with ALL GOOD or the failures."""
import base64, io, os, re, subprocess, sys, zipfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
APK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'android', 'app', 'build', 'outputs', 'apk', 'debug', 'app-debug.apk')
failed = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what)
    if not ok: failed.append(what)


def git(*a):
    return subprocess.run(['git', '-C', REPO, *a], capture_output=True, text=True).stdout.strip()


z = zipfile.ZipFile(APK)
files = {n: z.read(n) for n in z.namelist() if not n.endswith('/')}
print(f'{os.path.basename(APK)}: {len(files)} files, {os.path.getsize(APK):,} bytes')

print('\nlibraries')
abis = sorted({n.split('/')[1] for n in files if n.startswith('lib/')})
check(bool(abis), 'native code for ' + ', '.join(abis))
for abi in abis:
    for lib in ('libmain.so', 'libSDL2.so', 'libavcodec.so', 'libavformat.so', 'libavutil.so', 'libswresample.so', 'libmpg123.so'):
        check(f'lib/{abi}/{lib}' in files, f'{abi}: {lib}' + (' (LGPL: a file of its own, replaceable)' if lib.startswith(('libav', 'libsw', 'libmpg')) else ''))
    av = files.get(f'lib/{abi}/libavcodec.so', b'')
    check(av and not re.search(rb'--enable-(gpl|nonfree)', av), f'{abi}: FFmpeg has no GPL or non-free parts')
check(any(n.startswith('assets/romfs/fonts/') for n in files), "the app's fonts are in assets/romfs")

print('\nsecrets (never printed)')


def variants(t):
    return [t, t.decode('latin1').encode('utf-16-le'), base64.b64encode(t), t[::-1]]


secrets = []
keyf = os.path.join(REPO, 'report.key')
lines = open(keyf, 'rb').read().split(b'\n') if os.path.isfile(keyf) else []
token = lines[1].strip() if len(lines) > 1 else b''
check(len(token) > 20, "report.key's line 2 was read")
if token: secrets.append(('the token', token))
lf = os.path.join(REPO, 'lastfm.key')
if os.path.isfile(lf):
    for i, l in enumerate(open(lf, 'rb').read().split(b'\n')[:2]):
        if l.strip(): secrets.append((f"lastfm.key's line {i + 1}", l.strip()))
else:
    print('    (no lastfm.key on this PC, so no Last.fm secret to look for)')
blobs = dict(files)
blobs['(the APK itself)'] = open(APK, 'rb').read()
for name, t in secrets:
    hits = [f for f, d in blobs.items() if any(v in d for v in variants(t))]
    check(not hits, f'{name} is in no file, whole ({len(blobs)} files)' + (': ' + ', '.join(hits) if hits else ''))
    pieces = {t[i:i + 8] for i in range(len(t) - 7)}
    hits = [f for f, d in blobs.items() if any(p in d for p in pieces)]
    check(not hits, f'no 8-byte piece of {name} in any file' + (': ' + ', '.join(hits) if hits else ''))
for f in ('report.key', 'lastfm.key', 'source/report_key.inc', 'source/lastfm_key.inc'):
    check(git('check-ignore', f) == f, f'{f} is excluded from git')

print('\n' + ('ALL GOOD' if not failed else f'{len(failed)} FAILED'))
sys.exit(1 if failed else 0)
