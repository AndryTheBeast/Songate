"""
Songate · tools/clicktest/filelag.py
Listening together, decision T10: two files of one song, say a FLAC and an M4A, may not start at the same sample. Reads only.
Matches files by name in two folders, decodes 30 s of each as the app would (FFmpeg, 48 kHz), and finds the lag by correlation.

  python filelag.py <folder a> <folder b> [how many]
"""
import os, random, subprocess, sys
import numpy as np

RATE = 48000
A, B = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 60
EXT = (".mp3", ".m4a", ".flac")


def index(root):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            if f.lower().endswith(EXT):
                out.setdefault(os.path.splitext(f)[0].lower(), []).append(os.path.join(d, f))
    return out


def pcm(path):
    r = subprocess.run(["ffmpeg", "-v", "error", "-t", "30", "-i", path, "-ac", "1", "-ar", str(RATE), "-f", "f32le", "-"],
                       capture_output=True)
    return np.frombuffer(r.stdout, dtype=np.float32).astype(np.float64)


ia, ib = index(A), index(B)
names = sorted(set(ia) & set(ib))
print(f"{len(ia)} and {len(ib)} names, {len(names)} in both")
random.seed(10)
pick = random.sample(names, min(N, len(names)))
rows = []
for n in pick:
    pa, pb = ia[n][0], ib[n][0]
    a, b = pcm(pa), pcm(pb)
    k = min(len(a), len(b))
    if k < RATE * 5 or a[:k].std() < 1e-4 or b[:k].std() < 1e-4:
        continue
    a, b = a[:k] - a[:k].mean(), b[:k] - b[:k].mean()
    m = 1 << int(np.ceil(np.log2(2 * k)))
    c = np.fft.irfft(np.fft.rfft(a, m) * np.conj(np.fft.rfft(b, m)), m)
    i = int(np.argmax(np.abs(c)))
    lag = i if i < m // 2 else i - m
    q = abs(c[i]) / np.sqrt((a * a).sum() * (b * b).sum())
    rows.append((lag / RATE * 1e3, q, os.path.splitext(pa)[1], os.path.splitext(pb)[1], n))
rows.sort(key=lambda r: abs(r[0]))
good = [r for r in rows if r[1] > 0.5]
lags = np.array([r[0] for r in good])
print(f"measured {len(rows)}; alike (correlation over 0.5): {len(good)}")
if len(good):
    print(f"lag ms: median {np.median(lags):+.2f}, |lag| p50 {np.percentile(abs(lags),50):.2f} p90 {np.percentile(abs(lags),90):.2f} max {abs(lags).max():.2f}")
    for lim in (0.5, 2, 5, 20, 50):
        print(f"  within {lim} ms: {int((abs(lags) <= lim).sum())} of {len(good)}")
for r in rows[-8:]:
    print(f"  {r[0]:+9.2f} ms  corr {r[1]:.2f}  {r[2]} {r[3]}  {r[4][:50].encode('ascii','replace').decode()}")
print("not alike:", len(rows) - len(good))
