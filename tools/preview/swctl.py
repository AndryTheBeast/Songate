"""Songate on the console, dev only: drives the Switch dev build's remote control (source/devconsole.cpp) through its own port
(5055: FTP is Sphaira's and stops while Songate runs). Nothing of it ships.

    python tools/preview/swctl.py run "pad 1" "wait 30" "shot s.bmp"     (each argument a line; prints when done)
    python tools/preview/swctl.py get <name in the settings folder> <local file>
    python tools/preview/swctl.py shot <local.png>                      (the screen now)
    python tools/preview/swctl.py items <local.txt>
    python tools/preview/swctl.py stats
SW_IP must name the console's address on your network."""
import os
import socket
import sys
import time

IP = os.environ.get('SW_IP', '')
PORT = 5055

# platform.hpp's bits
BTN = dict(A=1, B=2, X=4, Y=8, LS=16, RS=32, L=64, R=128, ZL=256, ZR=512, PLUS=1024, MINUS=2048,
           LEFT=4096, UP=8192, RIGHT=16384, DOWN=32768)


class Switch:
    def __init__(self, ip=IP):
        self.ip = ip
        self.seq = int(time.time()) % 100000

    def _req(self, head, body=b'', timeout=20):
        last = None
        for attempt in range(4):
            try:
                with socket.create_connection((self.ip, PORT), timeout=timeout) as s:
                    s.sendall(head.encode() + b'\n' + body)
                    data = b''
                    while True:
                        k = s.recv(65536)
                        if not k:
                            break
                        data += k
                    return data
            except OSError as e:
                last = e
                time.sleep(1.5)
        raise RuntimeError(f'the console does not answer on {self.ip}:{PORT}: {last}')

    def alive(self):
        try:
            with socket.create_connection((self.ip, PORT), timeout=3):
                return True
        except OSError:
            return False

    def send(self, *lines):
        body = ('\n'.join(lines) + '\n').encode()
        return self._req(f'CTL {len(body)}', body) == b'OK\n'

    def get(self, name):
        data = self._req('GET ' + name, timeout=60)
        head, _, rest = data.partition(b'\n')
        n = int(head or b'-1')
        if n < 0:
            return None
        return rest[:n]

    def put(self, name, data):
        """A file written on the console (a new .nro as sdmc:/switch/Songate/Songate.nro.gfnew; then "pending", "relaunch")."""
        return self._req(f'PUT {len(data)} {name}', data, timeout=180) == b'OK\n'

    def update_app(self, nro_path):
        """The dev app replaced: sent beside the old one, swapped in at the close as an update's, Songate started again."""
        data = open(nro_path, 'rb').read()
        if not self.put('sdmc:/switch/Songate/Songate.nro.gfnew', data):
            return False
        self.send('pending sdmc:/switch/Songate/Songate.nro', 'relaunch')
        return True

    def delete(self, name):
        self._req('DEL ' + name)

    def run(self, *lines, timeout=180):
        """The lines queued at once; returns the "done" marker's words once the app has done them all."""
        self.seq += 1
        marker = f'devdone-{self.seq}.txt'
        self.delete(marker)
        self.send(*lines, f'done {marker}')
        end = time.time() + timeout
        while time.time() < end:
            got = self.get(marker)
            if got is not None:
                self.delete(marker)
                return got.decode(errors='replace').strip()
            time.sleep(0.4)
        return None

    def fetch(self, name, local, keep=False):
        data = self.get(name)
        if data is None:
            return None
        with open(local, 'wb') as fh:
            fh.write(data)
        if not keep:
            self.delete(name)
        return data

    def shot(self, local):
        if self.run('shot devshot.bmp', 'wait 3') is None:
            return None
        bmp = local[:-4] + '.bmp' if not local.endswith('.bmp') else local
        if self.fetch('devshot.bmp', bmp) is None:
            return None
        if not local.endswith('.bmp'):
            try:
                from PIL import Image
                Image.open(bmp).save(local)
                os.remove(bmp)
            except ImportError:
                return bmp
        return local

    def items(self, local=None):
        if self.run('items devitems.txt', 'wait 6') is None:
            return None
        data = self.get('devitems.txt')
        self.delete('devitems.txt')
        text = (data or b'').decode('utf-8', errors='replace')
        if local:
            with open(local, 'w', encoding='utf-8') as fh:
                fh.write(text)
        return text

    def stats(self):
        if self.run('stats devstats.txt') is None:
            return None
        return (self.get('devstats.txt') or b'').decode().strip()

    def hitches(self):
        if self.run('hitches devhitches.txt') is None:
            return None
        return (self.get('devhitches.txt') or b'').decode().strip()


def main():
    a = sys.argv[1:]
    sw = Switch()
    if a[0] == 'run':
        print(sw.run(*a[1:]))
    elif a[0] == 'get':
        print('got', sw.fetch(a[1], a[2], keep=True) is not None)
    elif a[0] == 'shot':
        print(sw.shot(a[1]))
    elif a[0] == 'items':
        print(len(sw.items(a[1]) or ''), 'bytes')
    elif a[0] == 'stats':
        print(sw.stats())
    elif a[0] == 'hitches':
        print(sw.hitches())
    elif a[0] == 'update':
        print(sw.update_app(a[1]))
    elif a[0] == 'alive':
        print(sw.alive())


if __name__ == '__main__':
    main()
