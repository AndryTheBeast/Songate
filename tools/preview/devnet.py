"""Songate for PC, dev only (session 23): the network that stalls mid-request, and Songate closed while it does. libcurl takes
HTTPS_PROXY / HTTP_PROXY from the environment: they point at a local "black hole" that accepts and never answers (stall) or at a port
nobody listens on (refused). Settings > Updates > Check pressed; then the app closed through its tray Quit while the request hangs, and
how long the close took (shutdown-trace.txt). python tools/preview/devnet.py <stall|refused> <out>"""
import os
import socket
import sys
import threading
import time
from pathlib import Path

mode = sys.argv[1]
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
HERE = Path(__file__).resolve().parent
cfg = HERE / 'out' / 'net-cfg'
os.environ['SONGATE_PCCFG'] = str(cfg)
sys.path.insert(0, str(HERE))
import pcdrive  # noqa: E402

held = []
if mode == 'stall':
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', 18999))
    srv.listen(16)

    def hole():
        while True:
            c, _ = srv.accept()
            held.append(c)   # (read nothing back, answer nothing: the request waits)
    threading.Thread(target=hole, daemon=True).start()
    proxy = 'http://127.0.0.1:18999'
else:
    proxy = 'http://127.0.0.1:9'   # (nothing listens: refused at once)
env = {k: proxy for k in ('HTTPS_PROXY', 'HTTP_PROXY', 'https_proxy', 'http_proxy', 'ALL_PROXY')}
pc = pcdrive.PC(env=env)
pc.quit()   # (its settings made: Updates turned on in them, then started again)
st = cfg / "Songate" / "settings.txt"
st.write_text("\n".join([l for l in st.read_text().splitlines() if not l.startswith("updates=")] + ["updates=1", "updateAtStart=0"]) + "\n")
pc = pcdrive.PC(env=env)
t_quit = None
try:
    pc.wait(lambda s: 'tb.battery' in s.items, 40)
    time.sleep(6)
    for _ in range(3):
        pc.clear_notices()
        time.sleep(1)
    pc.tap_text('Settings')
    time.sleep(1)
    pc.tap_text('Updates')
    time.sleep(1.2)
    pc.tap_item('set.updcheck')
    time.sleep(float(os.environ.get("DEVNET_WAIT", "4")))
    s = pc.screen()
    pc.shot(out / f'net-{mode}-updates.jpg')
    print('after the wait:', [t[0] for t in s.texts if 'heck' in t[0] or 'ownload' in t[0] or 'try' in t[0].lower() or 'error' in t[0].lower()][:6])
    print('connections held by the black hole:', len(held))
    st = pc.stats()
    print('frames while waiting:', st['text'][:120])
finally:
    t0 = time.time()
    pc.quit()
    t_quit = time.time() - t0
    print(f'quit took {t_quit:.1f} s, exit code {pc.app.returncode}')
    tr = cfg / 'Songate' / 'shutdown-trace.txt'
    if tr.exists():
        print('shutdown-trace:', tr.read_text(errors='replace').strip().splitlines()[-1][:300])
