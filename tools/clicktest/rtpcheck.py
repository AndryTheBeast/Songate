"""
Songate · tools/clicktest/rtpcheck.py
Is what iOS sent really audio? rtp.bin holds the audio packets clicktest kept. Writes them as a CAF file with the ALAC settings iOS
announced, decodes that with FFmpeg, and prints what came out.

  python rtpcheck.py <rtp.bin> <out.wav> [frames per packet, default 352]
"""
import struct, subprocess, sys
import numpy as np

src, out = sys.argv[1], sys.argv[2]
fpp = int(sys.argv[3]) if len(sys.argv) > 3 else 352
d = open(src, 'rb').read()
pk, i = {}, 0
while i + 2 <= len(d):
    n = struct.unpack('<H', d[i:i + 2])[0]
    p = d[i + 2:i + 2 + n]
    i += 2 + n
    if len(p) > 12:
        pk.setdefault(struct.unpack('>H', p[2:4])[0], p[12:])
seqs = sorted(pk)
if not seqs:
    print('no packets')
    sys.exit(1)
gaps = 0
ordered = []
for a, b in zip(seqs, seqs[1:] + [seqs[-1] + 1]):
    ordered.append(pk[a])
    gaps += max(0, b - a - 1)
print(f'{len(seqs)} packets, {sum(map(len, ordered))} bytes, about {len(seqs) * fpp / 44100:.1f} s, {gaps} missing; first bytes {ordered[0][:8].hex()}')


def vlq(v):
    o = [v & 127]
    v >>= 7
    while v:
        o.insert(0, 128 | (v & 127))
        v >>= 7
    return bytes(o)


cookie = struct.pack('>IBBBBBBHIII', fpp, 0, 16, 40, 10, 14, 2, 255, 0, 0, 44100)
pakt = struct.pack('>qqii', len(ordered), len(ordered) * fpp, 0, 0) + b''.join(vlq(len(p)) for p in ordered)
caf = b'caff' + struct.pack('>HH', 1, 0)
caf += b'desc' + struct.pack('>q', 32) + struct.pack('>d', 44100.0) + b'alac' + struct.pack('>IIIII', 0, 0, fpp, 2, 0)
caf += b'kuki' + struct.pack('>q', len(cookie)) + cookie
caf += b'pakt' + struct.pack('>q', len(pakt)) + pakt
data = b''.join(ordered)
caf += b'data' + struct.pack('>q', len(data) + 4) + struct.pack('>I', 0) + data
open(out + '.caf', 'wb').write(caf)
r = subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', out + '.caf', '-f', 's16le', '-ac', '2', '-ar', '44100', '-'], capture_output=True)
print('ffmpeg:', r.stderr.decode(errors='replace')[:300] or 'no errors')
a = np.frombuffer(r.stdout, dtype=np.int16).astype(float) / 32768
if len(a) == 0:
    print('nothing decoded')
    sys.exit(1)
print(f'decoded {len(a) / 2 / 44100:.1f} s, peak {abs(a).max():.3f}, rms {np.sqrt((a * a).mean()):.4f}')
L, R = a[0::2], a[1::2]
print(f'left and right correlate {np.corrcoef(L, R)[0, 1]:.3f}; sample-to-sample correlation {np.corrcoef(L[:-1], L[1:])[0, 1]:.3f} (music is near 1, noise near 0)')
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', out + '.caf', out])
