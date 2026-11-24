"""Dev only (session 23): the console emulation's factors, fitted. The same self-test walk on the console and on the PC (selftest-full.txt
of each), step by step: the console's work over the PC's (worst and mean), their median and spread; then, given an emulated PC run, which
steps go over 25 ms on both, on the console only, on the PC only. python tools/preview/devemufit.py <console.txt> <pc.txt> [pc-emulated.txt]"""
import re
import statistics
import sys

STEP = re.compile(r'\s*(\d+) (.+?)\s+(\d+) frames, worst\s+([\d.]+) ms \(work\s+([\d.]+), mean\s+([\d.]+)\), (\d+) over 25')


def steps(path):
    out = {}
    for l in open(path, encoding='utf-8', errors='replace'):
        m = STEP.match(l)
        if m:
            out[m.group(2).strip()] = dict(worst=float(m.group(4)), work=float(m.group(5)), mean=float(m.group(6)), over=int(m.group(7)))
    return out


con, pc = steps(sys.argv[1]), steps(sys.argv[2])
common = [k for k in con if k in pc]
rw = [con[k]['work'] / pc[k]['work'] for k in common if pc[k]['work'] >= 1.0]
rm = [con[k]['mean'] / pc[k]['mean'] for k in common if pc[k]['mean'] >= 0.3]
q = lambda v: (statistics.quantiles(v, n=4) if len(v) >= 4 else [0, 0, 0])
print(f'steps in both: {len(common)}')
print(f'worst work, console / PC: median {statistics.median(rw):.2f}, quartiles {q(rw)[0]:.2f} .. {q(rw)[2]:.2f} ({len(rw)} steps)')
print(f'mean work,  console / PC: median {statistics.median(rm):.2f}, quartiles {q(rm)[0]:.2f} .. {q(rm)[2]:.2f} ({len(rm)} steps)')
print(f'console: {sum(1 for k in common if con[k]["over"])} steps with a frame over 25 ms; PC native: {sum(1 for k in common if pc[k]["over"])}')
if len(sys.argv) > 3:
    emu = steps(sys.argv[3])
    both = [k for k in common if k in emu and con[k]['over'] and emu[k]['over']]
    conly = [k for k in common if k in emu and con[k]['over'] and not emu[k]['over']]
    eonly = [k for k in common if k in emu and not con[k]['over'] and emu[k]['over']]
    print(f'emulated vs console, steps with a frame over 25 ms: both {len(both)}, console only {len(conly)}, emulation only {len(eonly)}')
    for name, ks in (('console only', conly), ('emulation only', eonly)):
        print(f'  {name}: ' + '; '.join(ks[:12]))
