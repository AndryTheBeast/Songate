"""The console (dev only, session 23): waits for a file in Songate's settings folder (a self test's result), asking every 30 s so the
asking does not disturb the frames measured; then fetches it and the logs. python tools/preview/swwait.py <name> <local dir> [max s]"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from swctl import Switch  # noqa: E402

name, out = sys.argv[1], sys.argv[2]
limit = float(sys.argv[3]) if len(sys.argv) > 3 else 7200
os.makedirs(out, exist_ok=True)
sw = Switch()
t0 = time.time()
while time.time() - t0 < limit:
    try:
        data = sw.get(name)
    except RuntimeError as e:
        print('no answer:', e, flush=True)
        data = None
    if data:
        open(os.path.join(out, name), 'wb').write(data)
        print('got', name, len(data), 'after', round(time.time() - t0), 's', flush=True)
        for extra in ('log.txt', 'bg-log.txt', 'shutdown-trace.txt', 'stall.txt'):
            d = sw.get(extra)
            if d:
                open(os.path.join(out, extra), 'wb').write(d)
        h = sw.hitches()
        open(os.path.join(out, 'hitches.txt'), 'w', encoding='utf-8').write(h or '')
        print('hitches', len((h or '').splitlines()), flush=True)
        break
    time.sleep(30)
else:
    print('TIMEOUT waiting for', name)
