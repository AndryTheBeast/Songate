/*
 * Songate · connect.hpp
 * Connections: two Songates on one network find each other, pair, and stay linked. Off until allowed, and it holds no socket then.
 *
 * Finding: a worker at cores::Net says hello by UDP broadcast on every network this device is on, and to a typed address if
 * there is one; whoever hears it answers directly. A hello carries only the protocol number, a random id, the device's name and
 * whether it is offering a code: nothing of the library.
 *
 * Pairing: one device shows a 6-digit code for two minutes; the other types it. One wrong try throws the code away (pairing.hpp).
 * Afterwards each remembers the other's public key, and no other device gets past the first message. A device that cannot be
 * called (a PC whose firewall said no) is asked over UDP to call out instead, so one open side is enough.
 *
 * Links: one to every paired device here at once (BETA 3, the user, 2026-11-05: sync with several devices), each with its own
 * queues, so a message always says which device it is for or from. Nothing changes on the wire: each link is the same two-device link.
 *
 * connect-key.txt and connect-peers.tsv stay on this device: they are left out of backups, and never logged or reported.
 */
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pairing.hpp"

class Connect {
public:
    static constexpr int kProtocol = 2;
    static constexpr int kPort = 47810;
    static constexpr int kCodeSeconds = 120;

    /** A device heard on this network. */
    struct Peer {
        std::string id, name, address;
        int protocol = 0;
        bool offering = false, heardBroadcast = false, heardDirect = false;
        double seenAt = 0;
    };
    /** A device paired with this one. */
    struct Paired {
        std::string id, name;
        bool here = false, linked = false;   // heard on this network now; the encrypted link is up
    };
    struct View {
        bool looking = false;
        std::string me, name, id, error, code;   // code: the one shown, "" none
        int codeSeconds = 0;
        bool pairing = false;                // a code was typed: at work
        std::vector<Peer> peers;             // not paired
        std::vector<Paired> paired;
        int sent = 0, heard = 0, refused = 0;
    };

    void start(const std::string& dir);
    void stop();
    /** Looking begins, or ends: sockets open and close with it. Every frame, cheap when nothing changes. */
    void want(bool on);
    /** Also says hello to this address, for networks that carry no broadcasts ("" none). */
    void aim(const std::string& address);

    void offerCode();
    void cancelCode();
    /** The code shown on that device. */
    void enterCode(const std::string& peerId, const std::string& code);
    void forget(const std::string& id);

    /** The encrypted link: is it up to this device? */
    bool linkedTo(const std::string& id);
    /** Sends one message (at most pairing::kMaxFrame bytes) to that device, in order; dropped if it is not linked. */
    void send(const std::string& id, std::vector<uint8_t> message);
    size_t outboxBytes(const std::string& id);
    /** Files are moving: the links answer at once rather than on their slow beat. */
    void hurry() { hurryUntil_ = seconds_() + 2; }
    /** A message and the device it came from. */
    struct From {
        std::string peer;
        std::vector<uint8_t> msg;
    };
    /** Listening together's messages (they begin with a t), from every device, since the last call. */
    std::vector<From> receiveTogether();
    /** Listening together: pings go both ways over UDP with these devices (none: off). Both send, so that one open firewall is enough.
     *  clock() is this device's offset to the first of them (a listener's: the starter); the starter only answers. */
    void clockWanted(const std::vector<std::string>& ids);
    /** The other device's steady clock minus this one's, in seconds, at `at` on this one, and how fast that changes (s/s). The quick
     *  round trips of the last 30 s only. Any thread. */
    struct Clock { int samples = 0; double at = 0, offset = 0, slope = 0, rtt = 0; };
    Clock clock();
    /** What came from that device since the last call (sync's messages). */
    std::vector<std::vector<uint8_t>> receive(const std::string& id);
    /** The same from every device but `except`. */
    std::vector<From> receiveAll(const std::string& except = "");
    /** Music files' messages, which begin with an f, from every device: a copy lasts hours, and goes on beside a sync. */
    std::vector<From> receiveFiles();

    View view();
    /** What happened, in words for the screen, since the last call. */
    std::vector<std::string> takeNews();
    /** Lines for the app's log; no key, code or file in them. */
    std::vector<std::string> takeLog();

private:
    struct Remembered {
        uint8_t id[pairing::kId];
        uint8_t publicKey[pairing::kKey];
        std::string name;
        double triedAt = -100;
    };
    struct Link;

    void run();
    void session();
    void loadKeys();
    void savePeers();
    void note(const std::string& s);
    void news(const std::string& s);
    Remembered* remembered(const uint8_t* id);

    // the worker's parts of a session (connect.cpp)
    void onDatagram(const uint8_t* in, size_t size, uint32_t address, uint16_t port);
    void sendHello();
    void onFrame(Link& l, const uint8_t* p, size_t n);
    void onClock(const uint8_t* in, size_t size, uint32_t address);
    void sendClock(Link& l, bool answer, double t1, double t2);
    void startHandshake(Link& l);
    void finishPairing(Link& l, const uint8_t* publicKey, const std::string& name);
    void dropLink(Link& l, const char* why);
    void sweepLinks();
    Link* linkTo(const uint8_t* id);
    void call(const uint8_t* id, uint32_t address, int mode);
    void refreshView();

    std::vector<uint32_t> lastNets_;   // the worker's
    bool netSeen_ = false, netChanged_ = false;
    std::string dir_, aim_, name_;
    uint8_t id_[pairing::kId] = {};
    uint8_t secret_[pairing::kKey] = {}, public_[pairing::kKey] = {};
    bool keysLoaded_ = false;

    std::thread thread_;   // joined in stop(), never detached
    std::mutex m_;
    std::condition_variable wake_;
    bool quit_ = false, wanted_ = false;
    View view_;
    std::vector<Peer> heard_;              // all heard, paired or not
    std::vector<Remembered> paired_;
    std::vector<std::string> log_, news_;

    // asked of the worker
    bool wantCode_ = false, dropCode_ = false;
    std::string typedPeer_, typedCode_, forget_;
    // each device's queues, by its id
    struct Box {
        std::vector<std::vector<uint8_t>> out, in, files, together;
        size_t outBytes = 0;
        bool up = false;
    };
    std::map<std::string, Box> boxes_;
    struct ClockSample { double t, rtt, offset; };
    std::vector<ClockSample> clockSamples_;   // the worker's
    std::atomic<bool> clockOn_{ false };
    std::vector<std::string> clockPeers_;  // listening together's devices; the first one's clock is measured
    Clock clock_;
    std::atomic<double> hurryUntil_{ 0 };
    static double seconds_();

    // the worker's own, during a session
    std::string code_;
    double codeUntil_ = 0;
    std::string pendingPeer_, pendingCode_;
    double pendingUntil_ = 0;
    uintptr_t udp_ = ~uintptr_t(0), listen_ = ~uintptr_t(0);
    std::vector<Link*> links_;             // one a device; a dropped one goes at the end of the loop (sweepLinks)
    std::vector<std::string> clockPeersW_; // clockPeers_, as the worker last read it
    bool clockPeer(const std::string& hex) const { return std::find(clockPeersW_.begin(), clockPeersW_.end(), hex) != clockPeersW_.end(); }
};
