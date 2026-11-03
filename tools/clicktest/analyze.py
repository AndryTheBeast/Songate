"""
Songate · tools/clicktest/analyze.py
What the microphone heard (clicktest's mic.wav): a matched filter for each device's chirp, then the offset of each pair, one a second.
Prints the offset at the start, the free-running drift, and the corrected part's spread. pings.csv gives the network's numbers.

  python analyze.py <folder with mic.wav and pings.csv> [free seconds]
Positive offset: the second device (the console, or `server`) is heard after this PC.
"""
import sys, wave, csv
import numpy as np

RATE, LEN = 48000, 960
folder = sys.argv[1] if len(sys.argv) > 1 else "."
free_s = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0


def chirp(up):
    x = np.linspace(0, 1, LEN)
    hz = 1500 + 2000 * x if up else 7000 - 3000 * x
    return np.sin(np.cumsum(2 * np.pi * hz / RATE)) * (0.5 - 0.5 * np.cos(2 * np.pi * x))


with wave.open(folder + "/mic.wav") as w:
    a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768
print(f"recorded {len(a)/RATE:.1f} s, peak {np.abs(a).max():.3f}, noise rms {np.sqrt(np.mean(a[:RATE]**2)):.5f}")


def envelope(sig, c):
    n = len(sig) + len(c)
    m = 1 << int(np.ceil(np.log2(n)))
    S = np.fft.rfft(sig, m)
    C = np.fft.rfft(c[::-1], m)
    y = np.fft.irfft(S * C, m)[: len(sig) + len(c) - 1]
    Y = np.fft.fft(y, m)                       # the analytic signal, so a peak does not depend on which cycle wins
    h = np.zeros(m); h[0] = 1; h[1 : m // 2] = 2; h[m // 2] = 1
    return np.abs(np.fft.ifft(Y * h))[: len(y)]


def peaks(env):
    """One peak a second: the frame of seconds that holds the most energy, then a walk along it."""
    sec = len(env) // RATE
    best, best_score = 0, -1
    for start in range(0, RATE, 48):
        idx = start + np.arange(sec - 1) * RATE
        s = sum(env[max(0, i - 240) : i + 240].max() for i in idx[: min(20, sec - 1)])
        if s > best_score:
            best, best_score = start, s
    out, at = [], float(best)
    floor = np.median(env) * 6
    for _ in range(sec + 1):
        lo, hi = int(at - 2400), int(at + 2400)
        if hi >= len(env):
            break
        if lo < 0:
            at += RATE
            continue
        k = lo + int(np.argmax(env[lo:hi]))
        if env[k] > floor and 0 < k < len(env) - 1:
            l, c, r = env[k - 1], env[k], env[k + 1]
            frac = 0.5 * (l - r) / (l - 2 * c + r) if l - 2 * c + r != 0 else 0
            out.append((k + frac, c))
            at = k + frac
        at += RATE
    return np.array(out)


def refine(env, raw):
    """A stream's chirps are a second apart almost exactly: each is looked for within 3 ms of where its neighbours say it is. A clap or a word
    near a chirp cannot win then. Returns frames, with nan where nothing that tall was there."""
    n = np.round((raw[:, 0] - raw[0, 0]) / RATE).astype(int)
    rest = raw[:, 0] - n * RATE
    tall = np.median(raw[:, 1])
    out = {}
    for j, i in enumerate(n):
        near = np.abs(n - i) <= 10
        want = np.median(rest[near]) + i * RATE
        lo, hi = int(want - 144), int(want + 144)
        if lo < 1 or hi >= len(env) - 1:
            continue
        k = lo + int(np.argmax(env[lo:hi]))
        if env[k] < tall * 0.5 or k in (lo, hi - 1):
            continue
        l, c, r = env[k - 1], env[k], env[k + 1]
        out[i] = (k + 0.5 * (l - r) / (l - 2 * c + r), c)
    return out


envp, envs = envelope(a, chirp(True)), envelope(a, chirp(False))
rp, rs = peaks(envp), peaks(envs)
fp, fs = refine(envp, rp), refine(envs, rs)
pc = np.array([fp[k] for k in sorted(fp)])
sw = np.array([fs[k] for k in sorted(fs)])
print(f"chirps found: this PC {len(pc)}, the other {len(sw)}")
if len(pc) < 3 or len(sw) < 3:
    print("too few chirps heard")
    sys.exit(1)
print(f"median height: this PC {np.median(pc[:,1]):.2f}, the other {np.median(sw[:,1]):.2f}")

t, d = [], []
for p, _ in pc:
    i = np.argmin(np.abs(sw[:, 0] - p))
    if abs(sw[i, 0] - p) < 0.5 * RATE:
        t.append(p / RATE)
        d.append((sw[i, 0] - p) / RATE * 1e3)
t, d = np.array(t), np.array(d)
t0 = t[0]
t = t - t0
print(f"pairs {len(t)}, first offset {d[0]:+.2f} ms")


def line(mask):
    if mask.sum() < 3:
        return None
    b, a2 = np.polyfit(t[mask], d[mask], 1)
    r = d[mask] - (a2 + b * t[mask])
    return a2, b, r


f = t < free_s
fit = line(f)
if fit:
    a2, b, r = fit
    print(f"free  {f.sum():4d} s: start {a2:+.2f} ms, drift {b*1e3:+.1f} ms/1000 s = {b*1e3:+.1f} ppm, spread round the line sd {r.std():.3f} ms")
g = t >= free_s + 40          # the controller has had time to pull in
if g.sum() >= 3:
    m = d[g]
    q = np.percentile(np.abs(m - np.median(m)), [50, 95, 100])
    print(f"corrected {g.sum():4d} s: median {np.median(m):+.2f} ms, sd {m.std():.3f}, |dev| p50 {q[0]:.2f} p95 {q[1]:.2f} max {q[2]:.2f} ms, last-first slope {(m[-1]-m[0])/(t[g][-1]-t[g][0])*1e3:+.1f} ms/1000 s")
np.savetxt(folder + "/offsets.csv", np.c_[t, d], delimiter=",", header="t_s,offset_ms", comments="", fmt="%.4f")

try:
    rows = list(csv.DictReader(open(folder + "/pings.csv")))
    rtt = np.array([float(x["rtt_ms"]) for x in rows])
    off = np.array([float(x["offset_ms"]) for x in rows])
    tp = np.array([float(x["t"]) for x in rows]) / 1e6
    print(f"pings {len(rtt)}: rtt min {rtt.min():.2f} median {np.median(rtt):.2f} p95 {np.percentile(rtt,95):.2f} max {rtt.max():.1f} ms")
    q = rtt <= rtt.min() + 1.5
    if q.sum() > 3:
        b, a2 = np.polyfit(tp[q] - tp[0], off[q], 1)
        r = off[q] - (a2 + b * (tp[q] - tp[0]))
        print(f"clocks: {b*1e3:+.1f} ppm apart; quick pings {q.sum()}, their offset sd {r.std():.3f} ms round the line")
except OSError:
    pass
