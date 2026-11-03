"""Settings › Last.fm's key: makes source/lastfm_key.inc (git-ignored) from lastfm.key (git-ignored), for builds that can sign in
(source/lastfm.cpp unscrambles it).

    python tools/lastfm-key.py [lastfm.key]

lastfm.key: two lines, the API key and the shared secret (https://www.last.fm/api/account/create). Never commit either file.
Without them a build simply cannot sign in.

As tools/report-key.py: the key and secret, joined, are XORed with a xorshift32 stream (its seed: four random numbers here, and
0xC2B2AE35 in lastfm.cpp), each byte then put at a random place among random bytes, the places in another table: no text of it
is in the build. This slows someone down; it cannot stop them. Prints neither."""
import os, re, secrets, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'lastfm.key')
lines = [l.strip() for l in open(src, encoding='utf-8').read().splitlines() if l.strip()]
assert len(lines) == 2, 'lastfm.key: the API key, then the shared secret'
key, secret = lines
assert re.fullmatch('[0-9a-f]{32}', key) and re.fullmatch('[0-9a-f]{32}', secret), 'each 32 hex characters'
payload = (key + secret).encode('ascii')

seed = [secrets.randbits(32) for _ in range(4)]
x = seed[0] ^ seed[1] ^ seed[2] ^ seed[3] ^ 0xC2B2AE35
enc = []
for b in payload:
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    enc.append(b ^ ((x >> 8) & 0xFF))
total = len(payload) + 24 + secrets.randbelow(40)
places = list(range(total))
for i in range(total - 1, 0, -1):   # Fisher-Yates with the system's randomness
    j = secrets.randbelow(i + 1)
    places[i], places[j] = places[j], places[i]
data = [secrets.randbits(8) for _ in range(total)]
place = places[:len(payload)]
for i, p in enumerate(place): data[p] = enc[i]

def rows(v, per=16):
    return ',\n    '.join(', '.join(str(a) for a in v[i:i + per]) for i in range(0, len(v), per))

out = os.path.join(root, 'source', 'lastfm_key.inc')
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// Made by tools/lastfm-key.py from lastfm.key. Git-ignored: never commit it (see lastfm.hpp).\n#pragma once\n#include <cstddef>\n#include <cstdint>\n')
    f.write('namespace lastfm_key {\n')
    f.write(f'constexpr size_t kLen = {len(payload)}, kKeyLen = {len(key)};\n')
    f.write(f'static const uint32_t kSeed[4] = {{ {", ".join(str(s) for s in seed)} }};\n')
    f.write(f'static const uint8_t kData[{total}] = {{\n    {rows(data)} }};\n')
    f.write(f'static const uint16_t kPlace[{len(place)}] = {{\n    {rows(place)} }};\n')
    f.write('}\n')
print(out, f'{len(payload)} bytes among {total}')
