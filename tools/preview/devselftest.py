"""Songate for PC, dev only: runs Settings > Debug > Self test unattended (GF_SELFTEST, GF_SELFTEST_QUIT) on the scratch settings
of pcdrive.py, the library read only, and copies selftest-<level>.txt out. GF_EMU passes through (the console emulated).

    python tools/preview/devselftest.py <0|1|2> <out.txt> [timeout s]"""
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import pcdrive  # noqa: E402

level = int(sys.argv[1])
out = sys.argv[2]
limit = float(sys.argv[3]) if len(sys.argv) > 3 else 3600
name = ['basic', 'advanced', 'full'][level]
cfg = pcdrive.CFG / 'Songate'
res = cfg / f'selftest-{name}.txt'
if res.exists():
    res.unlink()
env = {'GF_SELFTEST': str(level), 'GF_SELFTEST_QUIT': '1'}
if os.environ.get('GF_EMU'):
    env['GF_EMU'] = os.environ['GF_EMU']
t0 = time.time()
pc = pcdrive.PC(env=env)
try:
    pc.app.wait(timeout=limit)
except Exception:
    print('TIMEOUT: still running after', limit, 's')
    pc.app.kill()
print('ran', round(time.time() - t0), 's, exit', pc.app.returncode)
if res.exists():
    shutil.copy(res, out)
    print('result', out)
else:
    print('NO RESULT FILE')
