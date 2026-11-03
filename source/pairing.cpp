/*
 * Songate · pairing.cpp
 * See pairing.hpp. Every hash starts with a label of its own, so a value made for one purpose is never valid for another.
 */
#include "pairing.hpp"

#include <cstring>

extern "C" {
#include "monocypher.h"
}

namespace pairing {
namespace {

bool lower(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, kId) < 0; }

struct Hash {
    crypto_blake2b_ctx c;
    Hash(size_t size, const char* label) { crypto_blake2b_init(&c, size); add(label, strlen(label) + 1); }
    Hash(size_t size, const uint8_t* key, const char* label) { crypto_blake2b_keyed_init(&c, size, key, kKey); add(label, strlen(label) + 1); }
    void add(const void* p, size_t n) { crypto_blake2b_update(&c, static_cast<const uint8_t*>(p), n); }
    void end(uint8_t* out) { crypto_blake2b_final(&c, out); }
};

void addPair(Hash& h, const uint8_t* a, const uint8_t* b, size_t n) {
    if (memcmp(a, b, n) < 0) { h.add(a, n); h.add(b, n); }
    else { h.add(b, n); h.add(a, n); }
}

void nonceOf(uint64_t counter, uint8_t* nonce) {
    memset(nonce, 0, 24);
    for (int i = 0; i < 8; i++) nonce[i] = uint8_t(counter >> (8 * i));
}

bool isZero(const uint8_t* k) {
    static const uint8_t zero[kKey] = {};
    return crypto_verify32(k, zero) == 0;
}

}  // namespace

void wipe(void* p, size_t n) { crypto_wipe(p, n); }

void cpaceStart(Cpace& c, const std::string& code, const uint8_t* idA, const uint8_t* idB, const uint8_t* random) {
    uint8_t hidden[kKey], generator[kKey];
    Hash h(kKey, "songate pairing generator 1");
    h.add(code.data(), code.size());
    addPair(h, idA, idB, kId);
    h.end(hidden);
    crypto_elligator_map(generator, hidden);
    memcpy(c.secret, random, kKey);
    crypto_x25519(c.mine, c.secret, generator);
    crypto_wipe(hidden, sizeof hidden);
    crypto_wipe(generator, sizeof generator);
}

bool cpaceFinish(Cpace& c, const uint8_t* theirs, const uint8_t* myId, const uint8_t* theirId, uint8_t* sessionKey) {
    uint8_t shared[kKey];
    crypto_x25519(shared, c.secret, theirs);
    const bool ok = !isZero(shared);
    Hash h(kKey, "songate pairing key 1");
    h.add(shared, kKey);
    addPair(h, c.mine, theirs, kKey);
    addPair(h, myId, theirId, kId);
    h.end(sessionKey);
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(c.secret, kKey);
    return ok;
}

void publicKey(const uint8_t* secret, uint8_t* publicOut) { crypto_x25519_public_key(publicOut, secret); }

bool knownFinish(const uint8_t* mySecret, const uint8_t* theirPublic, const uint8_t* myEphemeral, const uint8_t* myEphemeralPublic,
                 const uint8_t* theirEphemeralPublic, const uint8_t* myId, const uint8_t* theirId, uint8_t* sessionKey) {
    uint8_t ee[kKey], ss[kKey], es[kKey], se[kKey], myPublic[kKey];
    crypto_x25519(ee, myEphemeral, theirEphemeralPublic);
    crypto_x25519(ss, mySecret, theirPublic);
    crypto_x25519(es, myEphemeral, theirPublic);          // mine fresh, theirs long-term
    crypto_x25519(se, mySecret, theirEphemeralPublic);    // and the other way
    crypto_x25519_public_key(myPublic, mySecret);
    const bool ok = !isZero(ee) && !isZero(ss) && !isZero(es) && !isZero(se);
    Hash h(kKey, "songate link key 1");
    h.add(ee, kKey);
    h.add(ss, kKey);
    // es on one side is se on the other: in a fixed order, the lower id's fresh key first
    if (lower(myId, theirId)) { h.add(es, kKey); h.add(se, kKey); }
    else { h.add(se, kKey); h.add(es, kKey); }
    addPair(h, myEphemeralPublic, theirEphemeralPublic, kKey);
    addPair(h, myPublic, theirPublic, kKey);
    addPair(h, myId, theirId, kId);
    h.end(sessionKey);
    for (uint8_t* k : { ee, ss, es, se }) crypto_wipe(k, kKey);
    return ok;
}

void confirmation(const uint8_t* sessionKey, const uint8_t* senderId, uint8_t* mac) {
    Hash h(kMac, sessionKey, "songate confirm 1");
    h.add(senderId, kId);
    h.end(mac);
}

bool confirmationOk(const uint8_t* sessionKey, const uint8_t* senderId, const uint8_t* mac) {
    uint8_t want[kMac];
    confirmation(sessionKey, senderId, want);
    return crypto_verify16(want, mac) == 0;
}

void openChannel(Channel& c, const uint8_t* sessionKey, const uint8_t* myId, const uint8_t* theirId) {
    auto direction = [&](const uint8_t* from, uint8_t* key) {
        Hash h(kKey, sessionKey, "songate direction 1");
        h.add(from, kId);
        h.end(key);
    };
    direction(myId, c.sendKey);
    direction(theirId, c.receiveKey);
    c.sent = c.received = 0;
}

void openDatagrams(DatagramKeys& k, const uint8_t* sessionKey, const uint8_t* myId, const uint8_t* theirId) {
    auto direction = [&](const uint8_t* from, uint8_t* key) {
        Hash h(kKey, sessionKey, "songate clock 1");
        h.add(from, kId);
        h.end(key);
    };
    direction(myId, k.sendKey);
    direction(theirId, k.receiveKey);
    k.sent = k.newest = 0;
}

void sealDatagram(DatagramKeys& k, const uint8_t* plain, size_t size, uint8_t* out) {
    const uint64_t n = ++k.sent;
    uint8_t nonce[24];
    nonceOf(n, nonce);
    memcpy(out, nonce, 8);
    crypto_aead_lock(out + kDatagramExtra, out + 8, k.sendKey, nonce, nullptr, 0, plain, size);
}

bool unsealDatagram(DatagramKeys& k, const uint8_t* box, size_t size, uint8_t* plain) {
    if (size < kDatagramExtra) return false;
    uint64_t n = 0;
    for (int i = 7; i >= 0; i--) n = n << 8 | box[i];
    if (n <= k.newest) return false;
    uint8_t nonce[24];
    nonceOf(n, nonce);
    if (crypto_aead_unlock(plain, box + 8, k.receiveKey, nonce, nullptr, 0, box + kDatagramExtra, size - kDatagramExtra) != 0) {
        crypto_wipe(plain, size - kDatagramExtra);
        return false;
    }
    k.newest = n;
    return true;
}

std::vector<uint8_t> seal(Channel& c, const uint8_t* plain, size_t size) {
    std::vector<uint8_t> box(kMac + size);
    uint8_t nonce[24];
    nonceOf(c.sent++, nonce);
    crypto_aead_lock(box.data() + kMac, box.data(), c.sendKey, nonce, nullptr, 0, plain, size);
    return box;
}

bool unseal(Channel& c, const uint8_t* box, size_t size, std::vector<uint8_t>& plain) {
    if (size < kMac) return false;
    plain.resize(size - kMac);
    uint8_t nonce[24];
    nonceOf(c.received, nonce);
    if (crypto_aead_unlock(plain.data(), box, c.receiveKey, nonce, nullptr, 0, box + kMac, size - kMac) != 0) {
        crypto_wipe(plain.data(), plain.size());
        plain.clear();
        return false;
    }
    c.received++;
    return true;
}

}  // namespace pairing
