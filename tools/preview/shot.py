#!/usr/bin/env python3
"""Drives a preview instance of its own and saves frames as JPEGs: a screenshot tool for checking a build without the live page.

    python tools/preview/shot.py <step> <step> ...

Steps: "wait 1.5" | "tap x y" | "hold x y s" | "press B" (A B X Y L R ZL ZR PLUS MINUS LEFT RIGHT UP DOWN) | "key text" (the
keyboard's answer) | "wheel x y n" | "drag x1 y1 x2 y2 [s]" (a finger down, moved there over s seconds, 0.8 unless given, and
lifted) | "down x y" / "move x y [s]" / "up x y" (the same in parts, for a frame with the finger still down) | "shot name" (saves tools/preview/out/shots/<name>.jpg, 1280x720). Coordinates are the
app's (1280x720). Speaks streamshim.cpp's protocol like record.py, with the exe in tools/preview/out.

The card is tools/preview/out/shot-card: a copy of the live environment's config (settings, caches, so the library is there at once)
made on the first run, with Music a junction to the same folder; the live instance is never touched. The server's account file and
the kept queues are not copied, so the instance streams nothing and wakes no session. Dev only.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent.parent
OUT = HERE / 'out'
LIVE_CFG = HERE / 'out-live' / 'card' / 'sdmc' / 'config' / 'songate-nx'
CARD = OUT / 'shot-card'
CFG = CARD / 'sdmc' / 'config' / 'songate-nx'
SHOTS = OUT / 'shots'
MUSIC = Path(os.environ.get('GF_LIVE_MUSIC', r'D:\Music'))
PORT = int(os.environ.get('GF_SHOT_PORT', '8798'))
BUTTONS = {'A': 0, 'B': 1, 'X': 2, 'Y': 3, 'L': 6, 'R': 7, 'ZL': 8, 'ZR': 9, 'PLUS': 10, 'MINUS': 11,
           'LEFT': 12, 'UP': 13, 'RIGHT': 14, 'DOWN': 15}

latest = {'jpeg': None, 'n': 0}


def reader(conn):
    buf = bytearray()
    while True:
        try:
            data = conn.recv(1 << 20)
        except OSError:
            break
        if not data:
            break
        buf += data
        at = 0
        while len(buf) - at >= 5:
            kind, n = buf[at], struct.unpack_from('<I', buf, at + 1)[0]
            if len(buf) - at < 5 + n:
                break
            body = bytes(buf[at + 13:at + 5 + n])
            if kind == 1:
                latest['jpeg'] = body
                latest['n'] += 1
            at += 5 + n
        del buf[:at]


def prepare():
    (CARD / 'sdmc').mkdir(parents=True, exist_ok=True)
    music = CARD / 'sdmc' / 'Music'
    if not music.exists():
        subprocess.run(['cmd', '/c', 'mklink', '/J', str(music), str(MUSIC)], check=True, capture_output=True)
    if not (CFG / 'settings.txt').exists():
        CFG.parent.mkdir(parents=True, exist_ok=True)
        if LIVE_CFG.exists():
            shutil.copytree(LIVE_CFG, CFG, ignore=shutil.ignore_patterns('*.log', 'app-log*'), dirs_exist_ok=True)
        else:
            CFG.mkdir(parents=True, exist_ok=True)
            shutil.copy(HERE / 'live' / 'card-settings.txt', CFG / 'settings.txt')
    # a fresh instance must not wake the server's session, nor keep the live one's streams
    for name in os.listdir(CFG):
        if (name.endswith('-account') or name.endswith('-account.keep') or name.endswith('-queue.tsv')) and not name.startswith('settings'):
            (CFG / name).unlink()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    prepare()
    SHOTS.mkdir(exist_ok=True)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', PORT))
    srv.listen(1)
    env = os.environ.copy()
    for k in ('GF_SHOW', 'GF_SCRIPT'):
        env.pop(k, None)
    env.update(GF_ROOT=str(CARD), GF_ROMFS=str(PROJECT / 'romfs'), GF_STREAM=f'127.0.0.1:{PORT}', SDL_AUDIODRIVER='dummy',
               GF_PROTECT='sdmc:/Music')
    log = open(OUT / 'shot-app-log.txt', 'ab')
    exe = Path(os.environ.get('GF_SHOT_EXE', OUT / 'gatefold-preview.exe'))   # (another build of the preview, to compare)
    app = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stdout=log, stderr=subprocess.STDOUT)
    srv.settimeout(30)
    conn, _ = srv.accept()
    conn.sendall(b'q 90\nf 2\nz 100\n')
    threading.Thread(target=reader, args=(conn,), daemon=True).start()
    send = lambda s: conn.sendall((s + '\n').encode('utf-8'))
    t0 = time.monotonic()
    while latest['n'] < 5 and time.monotonic() - t0 < 30:   # the first frames
        time.sleep(0.1)
    saved = []
    at = (0.0, 0.0)
    for step in sys.argv[1:]:
        w = step.split()
        if not w:
            continue
        if w[0] == 'wait':
            time.sleep(float(w[1]))
        elif w[0] == 'tap':
            send(f'd {w[1]} {w[2]}')
            time.sleep(0.07)
            send(f'u {w[1]} {w[2]}')
            time.sleep(0.35)
        elif w[0] == 'hold':
            send(f'd {w[1]} {w[2]}')
            time.sleep(float(w[3]))
            send(f'u {w[1]} {w[2]}')
            time.sleep(0.35)
        elif w[0] in ('drag', 'down', 'move', 'up'):
            # a finger, moved in small steps as a real one is (a list in sort mode: a row follows it)
            if w[0] == 'down':
                send(f'd {w[1]} {w[2]}')
                at = (float(w[1]), float(w[2]))
                time.sleep(0.15)
            elif w[0] == 'up':
                send(f'u {w[1]} {w[2]}')
                time.sleep(0.35)
            else:
                if w[0] == 'drag':
                    x0, y0, x1, y1 = (float(v) for v in w[1:5])
                    secs = float(w[5]) if len(w) > 5 else 0.8
                    send(f'd {x0:.0f} {y0:.0f}')
                    time.sleep(0.15)
                else:
                    (x0, y0), x1, y1 = at, float(w[1]), float(w[2])
                    secs = float(w[3]) if len(w) > 3 else 0.8
                steps = max(2, int(secs / 0.02))
                for k in range(1, steps + 1):
                    send(f'm {x0 + (x1 - x0) * k / steps:.1f} {y0 + (y1 - y0) * k / steps:.1f}')
                    time.sleep(secs / steps)
                at = (x1, y1)
                if w[0] == 'drag':
                    time.sleep(0.1)
                    send(f'u {x1:.0f} {y1:.0f}')
                    time.sleep(0.35)
        elif w[0] == 'press':
            send(f'b {1 << BUTTONS[w[1]]}')
            time.sleep(0.12)
            send('b 0')
            time.sleep(0.35)
        elif w[0] == 'wheel':
            send(f'p {w[1]} {w[2]}')
            send(f'w {w[1]} {w[2]} {w[3]}')
            time.sleep(0.4)
        elif w[0] == 'key':
            send('k ' + step.split(None, 1)[1])
            time.sleep(0.6)
        elif w[0] == 'snap':   # the latest frame at once (a fade caught on its way: several in a row)
            p = SHOTS / (w[1] + '.jpg')
            p.write_bytes(latest['jpeg'])
            saved.append(str(p))
        elif w[0] == 'shot':
            time.sleep(0.5)
            n0 = latest['n']
            t1 = time.monotonic()
            while latest['n'] < n0 + 2 and time.monotonic() - t1 < 3:
                time.sleep(0.05)
            p = SHOTS / (w[1] + '.jpg')
            p.write_bytes(latest['jpeg'])
            saved.append(str(p))
    send('quit')
    try:
        app.wait(10)
    except subprocess.TimeoutExpired:
        app.kill()
    print('frames', latest['n'])
    for p in saved:
        print(p)


if __name__ == '__main__':
    main()
