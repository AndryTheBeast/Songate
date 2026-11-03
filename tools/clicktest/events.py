"""
Songate · tools/clicktest/events.py
For a recording with seeks, pauses and skips in it: analyze.py expects one unbroken stream. Here each chirp is found alone, and every
second of the recording gets the offset from this PC's chirp to the console's nearest one.

  python events.py <folder with mic.wav>
"""
import sys, wave
import numpy as np

RATE, LEN = 48000, 960


def chirp(up):
    x = np.linspace(0, 1, LEN)
    hz = 1500 + 2000 * x if up else 7000 - 3000 * x
    return np.sin(np.cumsum(2 * np.pi * hz / RATE)) * (0.5 - 0.5 * np.cos(2 * np.pi * x))


def envelope(sig, c):
    m = 1 << int(np.ceil(np.log2(len(sig) + len(c))))
    y = np.fft.irfft(np.fft.rfft(sig, m) * np.fft.rfft(c[::-1], m), m)
    h = np.zeros(m); h[0] = 1; h[1:m // 2] = 2; h[m // 2] = 1
    return np.abs(np.fft.ifft(np.fft.fft(y) * h))[:len(sig)]


with wave.open(sys.argv[1] + "/mic.wav") as w:
    a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float) / 32768


def peaks(e):
    tall = np.percentile(e, 99.9) * 0.45
    out, k = [], 0
    idx = np.where(e > tall)[0]
    while k < len(idx):
        i = idx[k]
        hi = min(len(e) - 2, i + 960)
        j = i + int(np.argmax(e[i:hi]))
        if j > 0:
            l, c, r = e[j - 1], e[j], e[j + 1]
            out.append(j + 0.5 * (l - r) / (l - 2 * c + r))
        k = int(np.searchsorted(idx, i + 24000))   # the next chirp is about a second on; half a second is safe
    return np.array(out)


pc, sw = peaks(envelope(a, chirp(True))), peaks(envelope(a, chirp(False)))
print(f"chirps: PC {len(pc)}, console {len(sw)}")
last = None
for p in pc:
    if len(sw) == 0:
        break
    i = int(np.argmin(np.abs(sw - p)))
    d = (sw[i] - p) / RATE * 1e3
    if abs(d) > 400:
        print(f"{p/RATE:7.1f} s  no partner")
        continue
    mark = "  <--" if last is not None and abs(d - last) > 3 else ""
    print(f"{p/RATE:7.1f} s  {d:+8.2f} ms{mark}")
    last = d
