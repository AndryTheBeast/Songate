/*
 * Songate · pairing.hpp
 * The cryptography of Connections, with no sockets in it so that it can be tested on its own (tools/preview/pairtest.cpp).
 *
 * Pairing: a 6-digit code becomes a shared key by CPace, a password-authenticated key exchange: someone watching learns nothing
 * they could try codes against afterwards, and someone in the middle gets one guess, after which the code is thrown away. Each
 * device then gives the other its long-term public key, under that shared key.
 * Afterwards: X25519, the two long-term keys and a fresh pair for each connection. Both sides prove they hold the keys, then every
 * message goes through XChaCha20-Poly1305 with a counter for a nonce, so a message changed, repeated or dropped ends the link.
 * Monocypher does the arithmetic. Nothing is ever sent in the clear after the greeting.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pairing {

constexpr size_t kId = 16, kKey = 32, kMac = 16;
constexpr size_t kMaxFrame = 1024 * 1024;

/** One side of CPace. */
struct Cpace {
    uint8_t secret[kKey], mine[kKey];
};

/** mine is then sent to the other side. random: 32 random bytes. The two ids in either order. */
void cpaceStart(Cpace& c, const std::string& code, const uint8_t* idA, const uint8_t* idB, const uint8_t* random);
/** The session key from the other side's value; false if that value is one no honest device sends. */
bool cpaceFinish(Cpace& c, const uint8_t* theirs, const uint8_t* myId, const uint8_t* theirId, uint8_t* sessionKey);

/** From a device's long-term secret: its public key. */
void publicKey(const uint8_t* secret, uint8_t* publicOut);
/** A paired connection's session key. Both sides get the same one only if each holds the secret behind the public key the other remembers. */
bool knownFinish(const uint8_t* mySecret, const uint8_t* theirPublic, const uint8_t* myEphemeral, const uint8_t* myEphemeralPublic,
                 const uint8_t* theirEphemeralPublic, const uint8_t* myId, const uint8_t* theirId, uint8_t* sessionKey);

/** Proof that a side holds the session key; each side's differs, so one cannot be sent back to its sender. */
void confirmation(const uint8_t* sessionKey, const uint8_t* senderId, uint8_t* mac);
bool confirmationOk(const uint8_t* sessionKey, const uint8_t* senderId, const uint8_t* mac);

/** The two directions of a link. */
struct Channel {
    uint8_t sendKey[kKey], receiveKey[kKey];
    uint64_t sent = 0, received = 0;
};
void openChannel(Channel& c, const uint8_t* sessionKey, const uint8_t* myId, const uint8_t* theirId);
std::vector<uint8_t> seal(Channel& c, const uint8_t* plain, size_t size);
/** false: changed, repeated, out of order or too short; the link must end. */
bool unseal(Channel& c, const uint8_t* box, size_t size, std::vector<uint8_t>& plain);

/** Listening together's clock: datagrams, which may be lost or arrive late, so each carries its own counter and the two directions
 *  have keys of their own, apart from the link's. A counter is taken once: what is repeated or older than the newest is refused. */
struct DatagramKeys {
    uint8_t sendKey[kKey], receiveKey[kKey];
    uint64_t sent = 0, newest = 0;
};
constexpr size_t kDatagramExtra = 8 + kMac;   // what sealing adds
void openDatagrams(DatagramKeys& k, const uint8_t* sessionKey, const uint8_t* myId, const uint8_t* theirId);
/** out: room for size + kDatagramExtra. */
void sealDatagram(DatagramKeys& k, const uint8_t* plain, size_t size, uint8_t* out);
/** false: changed, repeated, older or too short. plain: room for size - kDatagramExtra. */
bool unsealDatagram(DatagramKeys& k, const uint8_t* box, size_t size, uint8_t* plain);

void wipe(void* p, size_t n);

}  // namespace pairing
