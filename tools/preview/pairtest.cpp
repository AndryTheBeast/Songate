/* Connections' cryptography, attacked on its own (source/pairing.hpp). No network. Prints each check, then ALL GOOD or SOME FAILED.
 *   clang++ -std=c++20 -Isource -c tools/preview/pairtest.cpp ; with source/pairing.cpp and source/monocypher.c (clang). */
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "pairing.hpp"

using namespace pairing;

static int failed = 0, passed = 0;
static void check(const char* what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? passed : failed)++;
}
static std::mt19937_64 rng(20261031);   // test values only: a real device uses the system's randomness
static void fill(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = uint8_t(rng()); }

struct Run { bool okA, okB, sameKey, macsPass; uint8_t keyA[kKey], keyB[kKey]; };
static Run pairUp(const std::string& codeA, const std::string& codeB, const uint8_t* idA, const uint8_t* idB) {
    Cpace a, b;
    uint8_t ra[kKey], rb[kKey];
    fill(ra, kKey);
    fill(rb, kKey);
    cpaceStart(a, codeA, idA, idB, ra);
    cpaceStart(b, codeB, idB, idA, rb);
    Run r;
    const uint8_t ya[kKey] = {}; (void)ya;
    uint8_t pa[kKey], pb[kKey];
    memcpy(pa, a.mine, kKey);
    memcpy(pb, b.mine, kKey);
    r.okA = cpaceFinish(a, pb, idA, idB, r.keyA);
    r.okB = cpaceFinish(b, pa, idB, idA, r.keyB);
    r.sameKey = memcmp(r.keyA, r.keyB, kKey) == 0;
    uint8_t ma[kMac], mb[kMac];
    confirmation(r.keyA, idA, ma);
    confirmation(r.keyB, idB, mb);
    r.macsPass = confirmationOk(r.keyB, idA, ma) && confirmationOk(r.keyA, idB, mb);
    return r;
}

int main() {
    uint8_t idA[kId], idB[kId];
    fill(idA, kId);
    fill(idB, kId);

    Run good = pairUp("482913", "482913", idA, idB);
    check("the right code: both get the same key, both proofs pass", good.okA && good.okB && good.sameKey && good.macsPass);
    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        char c[8];
        snprintf(c, sizeof c, "%06d", i == 482913 ? 0 : i);
        Run w = pairUp("482913", c, idA, idB);
        bad += w.sameKey || w.macsPass;
    }
    check("2,000 wrong codes: none gives the same key or passes", bad == 0);
    Run off = pairUp("482913", "482914", idA, idB);
    check("one digit off: no key, no proof", !off.sameKey && !off.macsPass);
    uint8_t idC[kId];
    fill(idC, kId);
    Cpace a, m;
    uint8_t ra[kKey], rm[kKey], ka[kKey], km[kKey];
    fill(ra, kKey);
    fill(rm, kKey);
    cpaceStart(a, "482913", idA, idB, ra);
    cpaceStart(m, "482913", idC, idB, rm);
    uint8_t pa[kKey], pm[kKey];
    memcpy(pa, a.mine, kKey);
    memcpy(pm, m.mine, kKey);
    cpaceFinish(a, pm, idA, idB, ka);
    cpaceFinish(m, pa, idC, idB, km);
    check("the right code from a device with another id: refused", memcmp(ka, km, kKey) != 0);

    uint8_t zero[kKey] = {}, sk[kKey], one[kKey] = { 1 };
    Cpace z;
    fill(ra, kKey);
    cpaceStart(z, "482913", idA, idB, ra);
    Cpace z2 = z;
    check("a zero public value (small subgroup): refused", !cpaceFinish(z, zero, idA, idB, sk));
    check("a low-order public value: refused", !cpaceFinish(z2, one, idA, idB, sk));

    uint8_t mac[kMac];
    confirmation(good.keyA, idA, mac);
    check("a proof sent back to its sender: refused", !confirmationOk(good.keyA, idB, mac));
    mac[3] ^= 1;
    check("a proof with one bit changed: refused", !confirmationOk(good.keyA, idA, mac));

    // the remembered keys
    uint8_t sa[kKey], sb[kKey], pubA[kKey], pubB[kKey], ea[kKey], eb[kKey], epa[kKey], epb[kKey], k1[kKey], k2[kKey], se[kKey], spe[kKey], k3[kKey], k4[kKey];
    fill(sa, kKey); fill(sb, kKey); fill(ea, kKey); fill(eb, kKey); fill(se, kKey);
    publicKey(sa, pubA); publicKey(sb, pubB); publicKey(ea, epa); publicKey(eb, epb); publicKey(se, spe);
    bool ok1 = knownFinish(sa, pubB, ea, epa, epb, idA, idB, k1), ok2 = knownFinish(sb, pubA, eb, epb, epa, idB, idA, k2);
    check("remembered keys: both get the same key", ok1 && ok2 && memcmp(k1, k2, kKey) == 0);
    knownFinish(se, pubB, ea, epa, epb, idA, idB, k3);   // a stranger who knows the public keys, claiming A's id
    knownFinish(sb, pubA, eb, epb, epa, idB, idA, k4);
    check("a stranger with only the public keys: refused", memcmp(k3, k4, kKey) != 0);
    uint8_t recorded[kKey];
    fill(eb, kKey);
    publicKey(eb, epb);
    knownFinish(sa, pubB, ea, epa, epb, idA, idB, recorded);
    check("a new connection gets a new key", memcmp(k1, recorded, kKey) != 0);

    // the link
    Channel x, y;
    openChannel(x, k1, idA, idB);
    openChannel(y, k2, idB, idA);
    const char msg[] = "hello from one device";
    std::vector<uint8_t> plain;
    auto b0 = seal(x, reinterpret_cast<const uint8_t*>(msg), sizeof msg);
    check("a message arrives whole", unseal(y, b0.data(), b0.size(), plain) && plain.size() == sizeof msg && !memcmp(plain.data(), msg, sizeof msg));
    check("the same message again (replay): refused", !unseal(y, b0.data(), b0.size(), plain));
    auto b1 = seal(x, reinterpret_cast<const uint8_t*>(msg), sizeof msg);
    auto b2 = seal(x, reinterpret_cast<const uint8_t*>(msg), sizeof msg);
    check("two sealings of the same text differ", b1 != b2);
    check("a message skipped: refused", !unseal(y, b2.data(), b2.size(), plain));
    auto t = b1;
    t[t.size() / 2] ^= 0x40;
    check("one bit changed: refused", !unseal(y, t.data(), t.size(), plain));
    t = b1;
    t.resize(t.size() - 1);
    check("cut short: refused", !unseal(y, t.data(), t.size(), plain));
    check("shorter than its proof: refused", !unseal(y, b1.data(), 3, plain));
    check("the real one still arrives after all that", unseal(y, b1.data(), b1.size(), plain));
    auto back = seal(y, reinterpret_cast<const uint8_t*>(msg), sizeof msg);
    Channel x2 = x;
    x2.received = 1;
    check("a message reflected to its sender: refused", [&] { Channel r = y; return !unseal(r, back.data(), back.size(), plain); }());
    check("the other way works", unseal(x, back.data(), back.size(), plain));
    check("the one skipped earlier, in its turn", unseal(y, b2.data(), b2.size(), plain));
    std::vector<uint8_t> big(kMaxFrame);
    fill(big.data(), big.size());
    auto bb = seal(x, big.data(), big.size());
    check("a whole megabyte", unseal(y, bb.data(), bb.size(), plain) && plain == big);
    Channel other;
    openChannel(other, k3, idA, idB);
    auto cross = seal(other, reinterpret_cast<const uint8_t*>(msg), sizeof msg);
    check("a message under another key: refused", !unseal(y, cross.data(), cross.size(), plain));

    // listening together's clock
    {
        uint8_t session[kKey], other[kKey], a[kId], b[kId];
        memset(session, 7, kKey); memset(other, 8, kKey); memset(a, 1, kId); memset(b, 2, kId);
        DatagramKeys ka, kb, stranger;
        openDatagrams(ka, session, a, b);
        openDatagrams(kb, session, b, a);
        openDatagrams(stranger, other, b, a);
        const uint8_t msg[25] = "the time is now";
        uint8_t box[25 + kDatagramExtra], box2[25 + kDatagramExtra], out[25];
        sealDatagram(ka, msg, sizeof msg, box);
        sealDatagram(ka, msg, sizeof msg, box2);
        check("a datagram from a key holder opens", unsealDatagram(kb, box, sizeof box, out) && !memcmp(out, msg, sizeof msg));
        check("the same datagram twice: refused", !unsealDatagram(kb, box, sizeof box, out));
        check("a lost one is no harm: the next opens", unsealDatagram(kb, box2, sizeof box2, out));
        check("an older one after a newer: refused", !unsealDatagram(kb, box, sizeof box, out));
        sealDatagram(ka, msg, sizeof msg, box);
        check("another session's key: refused", !unsealDatagram(stranger, box, sizeof box, out));
        box[20] ^= 1;
        check("changed by one bit: refused", !unsealDatagram(kb, box, sizeof box, out));
        box[20] ^= 1;
        box[0] ^= 1;
        check("a counter changed: refused", !unsealDatagram(kb, box, sizeof box, out));
        box[0] ^= 1;
        check("too short: refused", !unsealDatagram(kb, box, kDatagramExtra - 1, out));
        DatagramKeys back;
        openDatagrams(back, session, a, b);
        uint8_t mine[25 + kDatagramExtra];
        sealDatagram(kb, msg, sizeof msg, mine);
        check("its own sent back to the sender: refused", !unsealDatagram(kb, mine, sizeof mine, out));
    }
    printf("%d passed, %d failed: %s\n", passed, failed, failed ? "SOME FAILED" : "ALL GOOD");
    return failed ? 1 : 0;
}
