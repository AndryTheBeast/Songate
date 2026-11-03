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
 * connect-key.txt and connect-peers.tsv stay on this device: they are left out of backups, and never logged or reported.
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
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
    /** Sends one message (at most pairing::kMaxFrame bytes) over the link, in order; dropped if there is none. */
    void send(std::vector<uint8_t> message);
    size_t outboxBytes();
    /** Files are moving: the link answers at once rather than on its slow beat. */
    void hurry() { hurryUntil_ = seconds_() + 2; }
    /** Listening together's messages (they begin with a t), since the last call. */
    std::vector<std::vector<uint8_t>> receiveTogether();
    /** Listening together: pings go both ways over UDP while it is on. Both send, so that one open firewall is enough. */
    void clockWanted(bool on);
    /** The other device's steady clock minus this one's, in seconds, at `at` on this one, and how fast that changes (s/s). The quick
     *  round trips of the last 30 s only. Any thread. */
    struct Clock { int samples = 0; double at = 0, offset = 0, slope = 0, rtt = 0; };
    Clock clock();
    /** What came over the link since the last call. */
    std::vector<std::vector<uint8_t>> receive();
    /** The same for music files, which begin with an f: a copy lasts hours, and goes on beside a sync. */
    std::vector<std::vector<uint8_t>> receiveFiles();

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
    void sendClock(bool answer, double t1, double t2);
    void startHandshake(Link& l);
    void finishPairing(Link& l, const uint8_t* publicKey, const std::string& name);
    void dropLink(const char* why);
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
    std::vector<std::vector<uint8_t>> outbox_, inbox_, filesIn_, togetherIn_;
    struct ClockSample { double t, rtt, offset; };
    std::vector<ClockSample> clockSamples_;   // the worker's
    std::atomic<bool> clockOn_{ false };
    Clock clock_;
    std::atomic<double> hurryUntil_{ 0 };
    static double seconds_();
    size_t outboxBytes_ = 0;
    std::string linkedId_;                 // who the link is up to, "" none

    // the worker's own, during a session
    std::string code_;
    double codeUntil_ = 0;
    std::string pendingPeer_, pendingCode_;
    double pendingUntil_ = 0;
    uintptr_t udp_ = ~uintptr_t(0), listen_ = ~uintptr_t(0);
    Link* link_ = nullptr;
};
