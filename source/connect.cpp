/*
 * Songate · connect.cpp
 * See connect.hpp. Every packet is checked before anything in it is believed: its size, its mark, its lengths. On a stream, each
 * message is a 4-byte length and its bytes: a greeting, then values for the key exchange, a proof, and everything after that sealed.
 */
#include "connect.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Sock = SOCKET;
static inline Sock S(uintptr_t v) { return Sock(v); }
constexpr Sock kNoSock = INVALID_SOCKET;
static void closeSock(Sock s) { closesocket(s); }
static void nonBlocking(Sock s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
static bool wouldBlock() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Sock = int;
static inline Sock S(uintptr_t v) { return int(intptr_t(v)); }
constexpr Sock kNoSock = -1;
static void closeSock(Sock s) { close(s); }
static void nonBlocking(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
#endif

#include "cores.hpp"
#include "net.hpp"
#include "platform.hpp"

using namespace pairing;

namespace {

constexpr char kMark[8] = { 'S', 'O', 'N', 'G', 'A', 'T', 'E', '1' };
constexpr size_t kNameMax = 64, kHead = 8 + 1 + 1 + 1 + kId + 1;
constexpr double kHelloEvery = 2, kGoneAfter = 15, kHandshakeSeconds = 6, kCallEvery = 10, kPingEvery = 5, kLinkSilence = 16;
constexpr uintptr_t kNone = ~uintptr_t(0);
enum : uint8_t { Hello = 1, Answer = 2, CallMe = 3, ClockPing = 4 };
constexpr double kClockEvery = 0.2, kClockWindow = 30, kClockQuick = 0.0015;   // as measured with tools/clicktest: quick means within 1.5 ms of the quickest
constexpr size_t kClockPlain = 1 + 3 * 8;
enum : uint8_t { ModePair = 1, ModeKnown = 2 };
enum : uint8_t { MsgIntroduce = 1, MsgPing = 2, MsgGoodbye = 3, MsgData = 4 };

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** The wall clock: it goes on through a console's sleep, where the steady one may not. */
double wallSeconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

constexpr double kAsleep = 8;   // the worker's loop not run for this long: a sleep or a freeze (its longest wait is a message written: 2 s twice)

std::string hexOf(const uint8_t* p, size_t n) {
    static const char* const digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}

bool unhex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        if (sscanf(s.c_str() + i * 2, "%2x", &v) != 1) return false;
        out[i] = uint8_t(v);
    }
    return true;
}

/** Only what can be shown: UTF-8 kept, control characters and tabs dropped. */
std::string cleanName(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) if (p[i] >= 0x20 && p[i] != 0x7F) s += char(p[i]);
    return s.empty() ? std::string("A device") : s;
}

std::string dotted(uint32_t beAddress) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&beAddress);
    char t[20];
    snprintf(t, sizeof t, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return t;
}

sockaddr_in addressOf(uint32_t be, int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = be;
    return a;
}

}  // namespace

/** One TCP connection, from the first byte to its end. */
struct Connect::Link {
    uintptr_t sock = kNone;
    bool connecting = false, incoming = false;
    uint8_t mode = 0;
    uint8_t their[kId] = {};
    bool knowTheirs = false, greeted = false, keyed = false, proved = false, up = false, wroteConfirm = false;
    std::string code;
    Cpace cpace{};
    uint8_t ephemeral[kKey] = {}, ephemeralPublic[kKey] = {}, session[kKey] = {};
    Channel channel{};
    DatagramKeys clockKeys{};
    uint32_t addressBe = 0;
    double clockAt = 0;
    std::vector<uint8_t> in, out;   // out: sealed and waiting for room, so that both sides can send at once
    double startedAt = 0, lastIn = 0, pingAt = 0;
    std::string address;

    ~Link() {
        pairing::wipe(ephemeral, sizeof ephemeral);
        pairing::wipe(session, sizeof session);
        pairing::wipe(&channel, sizeof channel);
        pairing::wipe(&clockKeys, sizeof clockKeys);
        pairing::wipe(&cpace, sizeof cpace);
    }
};

namespace {

bool writeAll(uintptr_t sock, const uint8_t* p, size_t n) {
    const double until = seconds() + 2;
    while (n) {
        const int w = int(send(S(sock), reinterpret_cast<const char*>(p), int(n), 0));
        if (w > 0) { p += w; n -= size_t(w); continue; }
        if (w < 0 && !wouldBlock()) return false;
        if (seconds() > until) return false;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(S(sock), &fds);
        timeval tv{ 0, 50 * 1000 };
        select(int(S(sock)) + 1, nullptr, &fds, nullptr, &tv);
    }
    return true;
}

bool writeFrame(uintptr_t sock, const uint8_t* p, size_t n) {
    const uint8_t len[4] = { uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n) };
    return writeAll(sock, len, 4) && writeAll(sock, p, n);
}

}  // namespace

void Connect::start(const std::string& dir) {
    dir_ = dir;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void Connect::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
        wanted_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    pairing::wipe(secret_, sizeof secret_);
}

void Connect::want(bool on) {
    std::lock_guard<std::mutex> g(m_);
    if (wanted_ == on) return;
    wanted_ = on;
    wake_.notify_all();
}

void Connect::aim(const std::string& address) {
    std::lock_guard<std::mutex> g(m_);
    aim_ = address;
}

void Connect::offerCode() {
    std::lock_guard<std::mutex> g(m_);
    wantCode_ = true;
    wake_.notify_all();
}

void Connect::cancelCode() {
    std::lock_guard<std::mutex> g(m_);
    dropCode_ = true;
    wake_.notify_all();
}

void Connect::enterCode(const std::string& peerId, const std::string& code) {
    std::lock_guard<std::mutex> g(m_);
    typedPeer_ = peerId;
    typedCode_ = code;
    view_.pairing = true;
    wake_.notify_all();
}

void Connect::forget(const std::string& id) {
    std::lock_guard<std::mutex> g(m_);
    forget_ = id;
    wake_.notify_all();
}

double Connect::seconds_() { return seconds(); }

bool Connect::linkedTo(const std::string& id) {
    std::lock_guard<std::mutex> g(m_);
    return !linkedId_.empty() && linkedId_ == id;
}

void Connect::send(std::vector<uint8_t> message) {
    std::lock_guard<std::mutex> g(m_);
    if (linkedId_.empty()) return;
    outboxBytes_ += message.size();
    outbox_.push_back(std::move(message));
    wake_.notify_all();
}

size_t Connect::outboxBytes() {
    std::lock_guard<std::mutex> g(m_);
    return outboxBytes_;
}

std::vector<std::vector<uint8_t>> Connect::receiveFiles() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<std::vector<uint8_t>> out;
    out.swap(filesIn_);
    return out;
}

std::vector<std::vector<uint8_t>> Connect::receiveTogether() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<std::vector<uint8_t>> out;
    out.swap(togetherIn_);
    return out;
}

void Connect::clockWanted(bool on) {
    if (clockOn_ == on) return;
    clockOn_ = on;
    std::lock_guard<std::mutex> g(m_);
    if (on) clock_ = Clock{};
}

Connect::Clock Connect::clock() {
    std::lock_guard<std::mutex> g(m_);
    return clock_;
}

std::vector<std::vector<uint8_t>> Connect::receive() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<std::vector<uint8_t>> out;
    out.swap(inbox_);
    return out;
}

Connect::View Connect::view() {
    std::lock_guard<std::mutex> g(m_);
    return view_;
}

std::vector<std::string> Connect::takeNews() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<std::string> out;
    out.swap(news_);
    return out;
}

std::vector<std::string> Connect::takeLog() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

void Connect::note(const std::string& s) {
    std::lock_guard<std::mutex> g(m_);
    log_.push_back("Connections: " + s);
}

void Connect::news(const std::string& s) {
    std::lock_guard<std::mutex> g(m_);
    news_.push_back(s);
}

Connect::Remembered* Connect::remembered(const uint8_t* id) {
    for (Remembered& r : paired_) if (!memcmp(r.id, id, kId)) return &r;
    return nullptr;
}

void Connect::run() {
    cores::moveHere(cores::Net);
    std::unique_lock<std::mutex> g(m_);
    while (!quit_) {
        wake_.wait(g, [&] { return quit_ || wanted_; });
        if (quit_) break;
        g.unlock();
        session();
        g.lock();
        view_.looking = false;
        view_.peers.clear();
        view_.code.clear();
        view_.pairing = false;
        for (Paired& p : view_.paired) p.here = p.linked = false;
    }
}

/** Who this device is, made once and kept. */
void Connect::loadKeys() {
    if (keysLoaded_) return;
    const std::string kp = dir_ + "/connect-key.txt", pp = dir_ + "/connect-peers.tsv";
    char b[200] = {};
    size_t n = 0;
    if (FILE* f = fopen(kp.c_str(), "rb")) { n = fread(b, 1, sizeof b - 1, f); fclose(f); }
    std::string t(b, n);
    pairing::wipe(b, sizeof b);
    const size_t nl = t.find('\n');
    bool ok = nl != std::string::npos && unhex(t.substr(0, nl), id_, kId) && unhex(t.substr(nl + 1, kKey * 2), secret_, kKey);
    pairing::wipe(&t[0], t.size());
    if (!ok) {
        if (!plat::randomBytes(id_, kId) || !plat::randomBytes(secret_, kKey)) {
            std::lock_guard<std::mutex> g(m_);
            view_.error = "This device gave no random numbers";
            wanted_ = false;
            return;
        }
        std::string out = hexOf(id_, kId) + "\n" + hexOf(secret_, kKey) + "\n";
        const std::string tmp = kp + ".tmp";
        if (FILE* f = fopen(tmp.c_str(), "wb")) {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
            remove(kp.c_str());
            rename(tmp.c_str(), kp.c_str());
        }
        pairing::wipe(&out[0], out.size());
    }
    pairing::publicKey(secret_, public_);
    // id, public key, name
    std::string peers;
    if (FILE* f = fopen(pp.c_str(), "rb")) {
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) peers.append(buf, got);
        fclose(f);
    }
    std::vector<Remembered> list;
    size_t a = 0;
    while (a < peers.size()) {
        size_t e = peers.find('\n', a);
        if (e == std::string::npos) e = peers.size();
        const std::string l = peers.substr(a, e - a);
        a = e + 1;
        const size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? t1 : l.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        Remembered r;
        if (unhex(l.substr(0, t1), r.id, kId) && unhex(l.substr(t1 + 1, t2 - t1 - 1), r.publicKey, kKey)) {
            r.name = l.substr(t2 + 1);
            list.push_back(std::move(r));
        }
    }
    paired_ = std::move(list);
    name_ = plat::deviceName();
    keysLoaded_ = true;
}

void Connect::savePeers() {
    std::string out;
    for (const Remembered& r : paired_) out += hexOf(r.id, kId) + "\t" + hexOf(r.publicKey, kKey) + "\t" + r.name + "\n";
    const std::string p = dir_ + "/connect-peers.tsv", tmp = p + ".tmp";
    if (out.empty()) { remove(p.c_str()); return; }
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    remove(p.c_str());
    rename(tmp.c_str(), p.c_str());
}

void Connect::sendHello() {
    uint8_t out[kHead + kNameMax];
    std::string target;
    {
        std::lock_guard<std::mutex> g(m_);
        target = aim_;
    }
    auto build = [&](uint8_t kind) {
        memcpy(out, kMark, 8);
        out[8] = uint8_t(kProtocol);
        out[9] = kind;
        out[10] = code_.empty() ? 0 : 1;
        memcpy(out + 11, id_, kId);
        const size_t n = std::min(name_.size(), kNameMax);
        out[11 + kId] = uint8_t(n);
        memcpy(out + kHead, name_.data(), n);
        return kHead + n;
    };
    const size_t n = build(Hello);
    int sent = 0;
    auto say = [&](uint32_t be) {
        const sockaddr_in to = addressOf(be, kPort);
        if (sendto(S(udp_), reinterpret_cast<const char*>(out), int(n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) == int(n)) sent++;
    };
    std::string me;
    const std::vector<plat::Net> nets = plat::networks();
    // Sockets opened before a network went away stay open and say nothing, but carry nothing when it is back (airplane mode: the user,
    // 2026-11-02). A network back, or an address gone: opened afresh. A new one beside the old, as a PC's virtual adapter, drops nothing
    std::vector<uint32_t> addresses;
    for (const plat::Net& w : nets) addresses.push_back(w.address);
    if (netSeen_) {
        bool lost = lastNets_.empty() && !addresses.empty();
        for (uint32_t a : lastNets_) if (std::find(addresses.begin(), addresses.end(), a) == addresses.end()) lost = true;
        if (lost) netChanged_ = true;
    }
    netSeen_ = true;
    lastNets_ = std::move(addresses);
    for (const plat::Net& w : nets) {
        me += (me.empty() ? "" : ", ") + dotted(w.address);
        say(w.broadcast);
    }
    if (nets.empty()) say(0xFFFFFFFFu);
    if (!target.empty()) {
        in_addr ia{};
        if (inet_pton(AF_INET, target.c_str(), &ia) == 1) say(ia.s_addr);
    }
    std::lock_guard<std::mutex> g(m_);
    view_.me = me;
    view_.name = name_;
    view_.id = hexOf(id_, kId);
    view_.sent += sent;
}

namespace {
void putDouble(uint8_t* p, double v) { memcpy(p, &v, 8); }
double getDouble(const uint8_t* p) { double v; memcpy(&v, p, 8); return v; }
}  // namespace

/** t1: when the ping left; t2: when it arrived. An answer's own leaving time is taken as late as can be. */
void Connect::sendClock(bool answer, double t1, double t2) {
    if (!link_ || !link_->up || udp_ == kNone) return;
    uint8_t plain[kClockPlain], out[10 + kClockPlain + kDatagramExtra];
    memcpy(out, kMark, 8);
    out[8] = uint8_t(kProtocol);
    out[9] = ClockPing;
    plain[0] = answer ? 1 : 0;
    putDouble(plain + 1, t1);
    putDouble(plain + 9, t2);
    putDouble(plain + 17, seconds());
    sealDatagram(link_->clockKeys, plain, sizeof plain, out + 10);
    const sockaddr_in to = addressOf(link_->addressBe, kPort);
    sendto(S(udp_), reinterpret_cast<const char*>(out), int(sizeof out), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

void Connect::onClock(const uint8_t* in, size_t size, uint32_t address) {
    const double t4 = seconds();
    if (!link_ || !link_->up || address != link_->addressBe || size != 10 + kClockPlain + kDatagramExtra) return;
    uint8_t plain[kClockPlain];
    if (!unsealDatagram(link_->clockKeys, in + 10, size - 10, plain)) return;
    if (!plain[0]) { if (clockOn_) sendClock(true, getDouble(plain + 17), t4); return; }
    if (!clockOn_) return;
    const double t1 = getDouble(plain + 1), t2 = getDouble(plain + 9), t3 = getDouble(plain + 17);
    const double rtt = (t4 - t1) - (t3 - t2);
    if (!(t1 <= t4) || !(t2 <= t3) || rtt < 0 || t4 - t1 > 2) return;   // not one of ours, or from long ago
    std::vector<ClockSample>& all = clockSamples_;
    all.push_back({ t4, rtt, ((t2 - t1) + (t3 - t4)) / 2 });
    all.erase(all.begin(), std::find_if(all.begin(), all.end(), [&](const ClockSample& s) { return s.t >= t4 - kClockWindow; }));
    double least = 1e9;
    for (const ClockSample& s : all) least = std::min(least, s.rtt);
    double n = 0, mt = 0, mo = 0;
    for (const ClockSample& s : all) if (s.rtt <= least + kClockQuick) { mt += s.t - t4; mo += s.offset; n++; }
    mt /= n;
    mo /= n;
    double stt = 0, sto = 0;
    for (const ClockSample& s : all) if (s.rtt <= least + kClockQuick) { const double a = s.t - t4 - mt; stt += a * a; sto += a * (s.offset - mo); }
    std::lock_guard<std::mutex> g(m_);
    clock_.samples = int(all.size());
    clock_.at = t4 + mt;
    clock_.offset = mo;
    clock_.slope = n >= 20 && stt > 0 ? sto / stt : 0;
    clock_.rtt = least;
}

void Connect::onDatagram(const uint8_t* in, size_t size, uint32_t address, uint16_t port) {
    if (size >= 10 && !memcmp(in, kMark, 8) && in[9] == ClockPing) { onClock(in, size, address); return; }
    if (size < kHead || memcmp(in, kMark, 8) != 0) { std::lock_guard<std::mutex> g(m_); view_.refused++; return; }
    const uint8_t proto = in[8], kind = in[9];
    const size_t nameLen = in[11 + kId];
    if ((kind != Hello && kind != Answer && kind != CallMe) || nameLen > kNameMax || kHead + nameLen != size) {
        std::lock_guard<std::mutex> g(m_);
        view_.refused++;
        return;
    }
    const uint8_t* pid = in + 11;
    if (!memcmp(pid, id_, kId)) return;   // our own broadcast, come back
    if (kind == CallMe) {
        // it could not reach us. Only for a device paired with us, or one whose code we hold; to anyone else nothing, and no row
        if (proto == kProtocol && !link_) {
            if (remembered(pid)) call(pid, address, ModeKnown);
            else if (!code_.empty()) call(pid, address, ModePair);
        }
        return;
    }
    const std::string idText = hexOf(pid, kId), addr = dotted(address);
    bool fresh = false;
    {
        std::lock_guard<std::mutex> g(m_);
        view_.heard++;
        auto it = std::find_if(heard_.begin(), heard_.end(), [&](const Peer& p) { return p.id == idText; });
        if (it == heard_.end()) {
            fresh = true;
            heard_.push_back({});
            it = heard_.end() - 1;
            it->id = idText;
        }
        it->name = cleanName(in + kHead, nameLen);
        it->address = addr;
        it->protocol = proto;
        it->offering = in[10] != 0;
        it->seenAt = seconds();
        (kind == Hello ? it->heardBroadcast : it->heardDirect) = true;
    }
    if (fresh) note(std::string("found ") + (proto == kProtocol ? "a device" : "a device of another version") + " at " + addr);
    if (proto != kProtocol) return;   // shown as such, and no further
    if (kind == Hello) {
        uint8_t out[kHead + kNameMax];
        memcpy(out, kMark, 8);
        out[8] = uint8_t(kProtocol);
        out[9] = Answer;
        out[10] = code_.empty() ? 0 : 1;
        memcpy(out + 11, id_, kId);
        const size_t n = std::min(name_.size(), kNameMax);
        out[11 + kId] = uint8_t(n);
        memcpy(out + kHead, name_.data(), n);
        const sockaddr_in to = addressOf(address, port);
        sendto(S(udp_), reinterpret_cast<const char*>(out), int(kHead + n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
}

/** A connection out; the handshake starts once it is made. */
void Connect::call(const uint8_t* id, uint32_t address, int mode) {
    if (link_) return;
    Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSock) return;
    nonBlocking(s);
    const sockaddr_in to = addressOf(address, kPort);
    const int rc = connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    if (rc != 0 && !wouldBlock()) { closeSock(s); return; }
    link_ = new Link;
    link_->sock = uintptr_t(s);
    link_->connecting = rc != 0;
    link_->mode = uint8_t(mode);
    memcpy(link_->their, id, kId);
    link_->knowTheirs = true;
    link_->startedAt = seconds();
    link_->address = dotted(address);
    link_->addressBe = address;
    if (mode == ModePair) link_->code = !pendingCode_.empty() ? pendingCode_ : code_;
    if (!link_->connecting) startHandshake(*link_);
}

void Connect::startHandshake(Link& l) {
    uint8_t g[8 + 2 + kId];
    memcpy(g, kMark, 8);
    g[8] = uint8_t(kProtocol);
    g[9] = l.mode;
    memcpy(g + 10, id_, kId);
    if (!writeFrame(l.sock, g, sizeof g)) dropLink("the connection failed");
}

void Connect::dropLink(const char* why) {
    if (!link_) return;
    Link* l = link_;
    link_ = nullptr;
    if (l->up) note(std::string("link ended: ") + why);
    {
        std::lock_guard<std::mutex> g(m_);
        linkedId_.clear();
        outbox_.clear();
        outboxBytes_ = 0;
    }
    if (l->sock != kNone) closeSock(S(l->sock));
    delete l;
}

void Connect::finishPairing(Link& l, const uint8_t* publicKey, const std::string& name) {
    Remembered* r = remembered(l.their);
    if (!r) {
        paired_.push_back({});
        r = &paired_.back();
        memcpy(r->id, l.their, kId);
    }
    memcpy(r->publicKey, publicKey, kKey);
    r->name = name;
    savePeers();
    code_.clear();
    codeUntil_ = 0;
    pendingPeer_.clear();
    pairing::wipe(&pendingCode_[0], pendingCode_.size());
    pendingCode_.clear();
    note("paired");
    news("Paired with " + name);
}

void Connect::onFrame(Link& l, const uint8_t* p, size_t n) {
    if (!l.greeted) {
        // the greeting: who it is, and what for
        if (n != 8 + 2 + kId || memcmp(p, kMark, 8) != 0) { dropLink("not a Songate"); return; }
        if (p[8] != uint8_t(kProtocol)) {
            note("a device of another version was turned away");
            news("That device has another version of Songate: update both");
            dropLink("another version");
            return;
        }
        const uint8_t mode = p[9];
        const uint8_t* who = p + 10;
        if (l.incoming) {
            l.mode = mode;
            memcpy(l.their, who, kId);
            if (mode == ModePair) {
                const bool mine = !code_.empty(), typed = !pendingCode_.empty() && pendingPeer_ == hexOf(who, kId);
                if (!mine && !typed) { dropLink("no code waiting"); return; }
                l.code = typed ? pendingCode_ : code_;
            } else if (mode != ModeKnown || !remembered(who)) {
                dropLink("not paired");   // a stranger: nothing is told, nothing is read
                return;
            }
            startHandshake(l);
            if (!link_) return;
        } else if (mode != l.mode || memcmp(l.their, who, kId) != 0) {
            dropLink("not the device called");
            return;
        }
        l.greeted = true;
        uint8_t r[kKey];
        if (!plat::randomBytes(r, kKey)) { dropLink("no random numbers"); return; }
        if (l.mode == ModePair) {
            cpaceStart(l.cpace, l.code, id_, l.their, r);
            if (!writeFrame(l.sock, l.cpace.mine, kKey)) { dropLink("the connection failed"); return; }
        } else {
            memcpy(l.ephemeral, r, kKey);
            pairing::publicKey(l.ephemeral, l.ephemeralPublic);
            if (!writeFrame(l.sock, l.ephemeralPublic, kKey)) { dropLink("the connection failed"); return; }
        }
        pairing::wipe(r, sizeof r);
        return;
    }
    if (!l.keyed) {
        if (n != kKey) { dropLink("a bad key"); return; }
        bool ok;
        if (l.mode == ModePair) {
            ok = cpaceFinish(l.cpace, p, id_, l.their, l.session);
        } else {
            Remembered* r = remembered(l.their);
            ok = r && knownFinish(secret_, r->publicKey, l.ephemeral, l.ephemeralPublic, p, id_, l.their, l.session);
        }
        if (!ok) { dropLink("a bad key"); return; }
        l.keyed = true;
        uint8_t mac[kMac];
        confirmation(l.session, id_, mac);
        if (!writeFrame(l.sock, mac, kMac)) { dropLink("the connection failed"); return; }
        return;
    }
    if (!l.proved) {
        if (n != kMac || !confirmationOk(l.session, l.their, p)) {
            if (l.mode == ModePair) {
                // the wrong code. The one shown goes with its single try
                if (l.code == code_) { code_.clear(); codeUntil_ = 0; }
                pairing::wipe(&pendingCode_[0], pendingCode_.size());
                pendingCode_.clear();
                pendingPeer_.clear();
                note("a wrong code");
                news("That code was wrong. Show a new one on the other device");
            } else {
                note("a device failed to prove who it is");
            }
            dropLink("the proof failed");
            return;
        }
        l.proved = true;
        openChannel(l.channel, l.session, id_, l.their);
        openDatagrams(l.clockKeys, l.session, id_, l.their);
        l.lastIn = seconds();
        if (l.mode == ModePair) {
            std::vector<uint8_t> msg(1 + kKey);
            msg[0] = MsgIntroduce;
            memcpy(msg.data() + 1, public_, kKey);
            msg.insert(msg.end(), name_.begin(), name_.begin() + std::ptrdiff_t(std::min(name_.size(), kNameMax)));
            const std::vector<uint8_t> box = seal(l.channel, msg.data(), msg.size());
            if (!writeFrame(l.sock, box.data(), box.size())) dropLink("the connection failed");
        } else {
            l.up = true;
            l.pingAt = seconds() + kPingEvery;
            clockSamples_.clear();   // (the last estimate stays until there are new ones: the two clocks did not move)
            {
                std::lock_guard<std::mutex> g(m_);
                linkedId_ = hexOf(l.their, kId);
            }
            note("link up");
        }
        return;
    }
    std::vector<uint8_t> plain;
    if (!unseal(l.channel, p, n, plain) || plain.empty()) { dropLink("a message failed its check"); return; }
    l.lastIn = seconds();
    switch (plain[0]) {
    case MsgIntroduce: {
        if (l.mode != ModePair || plain.size() < 1 + kKey || plain.size() > 1 + kKey + kNameMax) { dropLink("a bad introduction"); return; }
        const std::string name = cleanName(plain.data() + 1 + kKey, plain.size() - 1 - kKey);
        finishPairing(l, plain.data() + 1, name);
        dropLink("paired");   // the next connection is by the remembered keys
        return;
    }
    case MsgPing: return;
    case MsgData: {
        if (!l.up) { dropLink("data before the link was up"); return; }
        std::lock_guard<std::mutex> g(m_);
        const char kind = plain.size() > 1 ? char(plain[1]) : 0;
        (kind == 'f' ? filesIn_ : kind == 't' ? togetherIn_ : inbox_).emplace_back(plain.begin() + 1, plain.end());
        if (plain.size() > 1 && plain[1] == 'f') hurryUntil_ = seconds() + 2;
        return;
    }
    case MsgGoodbye: dropLink("the other device left"); return;
    default: return;   // of a later version of this protocol: ignored
    }
}

void Connect::refreshView() {
    const double t = seconds();
    std::lock_guard<std::mutex> g(m_);
    heard_.erase(std::remove_if(heard_.begin(), heard_.end(), [&](const Peer& p) { return t - p.seenAt > kGoneAfter; }), heard_.end());
    view_.peers.clear();
    view_.paired.clear();
    for (const Peer& p : heard_) {
        uint8_t id[kId];
        if (!unhex(p.id, id, kId) || !remembered(id)) view_.peers.push_back(p);
    }
    for (const Remembered& r : paired_) {
        Paired p;
        p.id = hexOf(r.id, kId);
        p.name = r.name;
        p.here = std::any_of(heard_.begin(), heard_.end(), [&](const Peer& h) { return h.id == p.id; });
        p.linked = link_ && link_->up && !memcmp(link_->their, r.id, kId);
        view_.paired.push_back(std::move(p));
    }
    view_.code = code_;
    view_.codeSeconds = code_.empty() ? 0 : std::max(0, int(codeUntil_ - t));
    view_.pairing = !pendingCode_.empty() || !typedCode_.empty();
}

/** While allowed: sockets, reopened when the network goes and comes back (console sleep drops them). */
void Connect::session() {
    loadKeys();
    if (!keysLoaded_) return;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#elif defined(GF_CURL)
    std::string e;
    if (!net::begin(e)) {
        std::lock_guard<std::mutex> g(m_);
        view_.error = e;
        wanted_ = false;
        return;
    }
#endif
    {
        std::lock_guard<std::mutex> g(m_);
        view_ = View{};
        view_.looking = true;
        heard_.clear();
    }
    double helloAt = 0, openAt = 0;
    netSeen_ = netChanged_ = false;
    // This loop runs many times a second: seconds that passed without it, on either clock, were a sleep or a freeze. Console sleep
    // drops the sockets, but the network is the same when it wakes, so nothing else notices (the user, 2026-11-03: the link never came
    // back after the console woke, and the session ended): opened again as when the network changes
    double lastLoop = seconds(), lastWall = wallSeconds();
    static uint8_t buf[64 * 1024];   // (one worker)
    auto closeSockets = [&] {
        dropLink("the network went away");
        if (udp_ != kNone) closeSock(S(udp_));
        if (listen_ != kNone) closeSock(S(listen_));
        udp_ = listen_ = kNone;
    };
    for (;;) {
        std::string typedPeer, typedCode, forget;
        bool wantCode = false, dropCode = false;
        {
            std::lock_guard<std::mutex> g(m_);
            if (quit_ || !wanted_) break;
            std::swap(typedPeer, typedPeer_);
            std::swap(typedCode, typedCode_);
            std::swap(forget, forget_);
            std::swap(wantCode, wantCode_);
            std::swap(dropCode, dropCode_);
        }
        const double t = seconds(), wall = wallSeconds();
        if (t - lastLoop > kAsleep || wall - lastWall > kAsleep) {
            netChanged_ = true;
            helloAt = 0;   // (opened again at the next hello: now)
        }
        lastLoop = t;
        lastWall = wall;
        if (dropCode) { code_.clear(); codeUntil_ = 0; }
        if (wantCode) {
            uint32_t r = 0;
            do {
                if (!plat::randomBytes(reinterpret_cast<uint8_t*>(&r), sizeof r)) break;
            } while (r >= 4294000000u);   // evenly spread over a million
            char c[8];
            snprintf(c, sizeof c, "%06u", r % 1000000u);
            code_ = c;
            codeUntil_ = t + kCodeSeconds;
            helloAt = 0;   // say so now
        }
        if (!code_.empty() && t > codeUntil_) { code_.clear(); news("The code ran out of time"); }
        if (!forget.empty()) {
            uint8_t id[kId];
            if (unhex(forget, id, kId)) {
                if (link_ && !memcmp(link_->their, id, kId)) dropLink("forgotten");
                paired_.erase(std::remove_if(paired_.begin(), paired_.end(), [&](const Remembered& r) { return !memcmp(r.id, id, kId); }), paired_.end());
                savePeers();
            }
        }
        if (!typedCode.empty()) {
            pendingPeer_ = typedPeer;
            pendingCode_ = typedCode;
            pendingUntil_ = t + 12;
            pairing::wipe(&typedCode[0], typedCode.size());
            uint8_t id[kId];
            uint32_t addr = 0;
            {
                std::lock_guard<std::mutex> g(m_);
                for (const Peer& p : heard_) if (p.id == pendingPeer_) { in_addr ia{}; if (inet_pton(AF_INET, p.address.c_str(), &ia) == 1) addr = ia.s_addr; }
            }
            if (addr && unhex(pendingPeer_, id, kId)) {
                dropLink("pairing");
                call(id, addr, ModePair);
            }
        }
        if (!pendingCode_.empty() && t > pendingUntil_) {
            pairing::wipe(&pendingCode_[0], pendingCode_.size());
            pendingCode_.clear();
            pendingPeer_.clear();
            news("The other device did not answer. Are both on this network?");
        }
        if (udp_ == kNone && t >= openAt) {
            openAt = t + 5;
            Sock u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Sock l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (u != kNoSock && l != kNoSock) {
                int on = 1;
                setsockopt(u, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
                setsockopt(u, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
                setsockopt(l, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
                const sockaddr_in a = addressOf(htonl(INADDR_ANY), kPort);
                const bool bound = bind(u, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
                const bool listening = bind(l, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 && listen(l, 2) == 0;
                if (bound) {
                    nonBlocking(u);
                    udp_ = uintptr_t(u);
                } else {
                    closeSock(u);
                    note("the UDP port could not be opened");
                }
                if (bound && listening) {
                    nonBlocking(l);
                    listen_ = uintptr_t(l);
                } else {
                    closeSock(l);
                    if (bound) note("the TCP port could not be opened");
                }
            } else {
                if (u != kNoSock) closeSock(u);
                if (l != kNoSock) closeSock(l);
            }
        }
        if (udp_ != kNone && t >= helloAt) {
            helloAt = t + kHelloEvery;
            sendHello();
            if (netChanged_) {
                netChanged_ = false;
                note("the network changed: opening again");
                closeSockets();
                openAt = helloAt = 0;
                continue;
            }
        }
        // a paired device here and no link: the lower id calls; if that fails, the other is asked to
        if (udp_ != kNone && !link_) {
            for (Remembered& r : paired_) {
                std::string addr;
                {
                    std::lock_guard<std::mutex> g(m_);
                    const std::string idText = hexOf(r.id, kId);
                    for (const Peer& p : heard_) if (p.id == idText && p.protocol == kProtocol) addr = p.address;
                }
                if (addr.empty() || t - r.triedAt < kCallEvery) continue;
                r.triedAt = t;
                in_addr ia{};
                if (inet_pton(AF_INET, addr.c_str(), &ia) != 1) continue;
                if (memcmp(id_, r.id, kId) < 0) call(r.id, ia.s_addr, ModeKnown);
                break;
            }
        }
        if (link_) {
            if (!link_->up && t - link_->startedAt > kHandshakeSeconds) {
                // not reached, or no answer: ask it to call us instead
                const bool notReached = link_->connecting && link_->mode != 0 && !link_->incoming;
                uint8_t id[kId];
                memcpy(id, link_->their, kId);
                const std::string addr = link_->address;
                dropLink("no answer");
                (void)id;
                if (notReached && udp_ != kNone) {
                    uint8_t out[kHead + kNameMax];
                    memcpy(out, kMark, 8);
                    out[8] = uint8_t(kProtocol);
                    out[9] = CallMe;
                    out[10] = 0;
                    memcpy(out + 11, id_, kId);
                    const size_t n = std::min(name_.size(), kNameMax);
                    out[11 + kId] = uint8_t(n);
                    memcpy(out + kHead, name_.data(), n);
                    in_addr ia{};
                    if (inet_pton(AF_INET, addr.c_str(), &ia) == 1) {
                        const sockaddr_in to = addressOf(ia.s_addr, kPort);
                        sendto(S(udp_), reinterpret_cast<const char*>(out), int(kHead + n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
                    }
                }
            } else if (link_->up && t - link_->lastIn > kLinkSilence) {
                dropLink("silence");
            } else if (link_->up && t >= link_->pingAt) {
                link_->pingAt = t + kPingEvery;
                // in the queue with everything else: sealed messages must arrive in the order they were sealed, and never inside one another
                const uint8_t ping = MsgPing;
                const std::vector<uint8_t> box = seal(link_->channel, &ping, 1);
                const uint8_t len[4] = { uint8_t(box.size() >> 24), uint8_t(box.size() >> 16), uint8_t(box.size() >> 8), uint8_t(box.size()) };
                link_->out.insert(link_->out.end(), len, len + 4);
                link_->out.insert(link_->out.end(), box.begin(), box.end());
            }
        }
        if (link_ && link_->up && clockOn_ && t >= link_->clockAt) {
            link_->clockAt = t + kClockEvery;
            sendClock(false, 0, 0);
        }
        // what the app gave us: sealed in order, then sent as room allows, the reading going on meanwhile
        if (link_ && link_->up && link_->out.size() < 128 * 1024) {
            std::vector<std::vector<uint8_t>> send;
            {
                std::lock_guard<std::mutex> g(m_);
                if (!outbox_.empty()) {
                    send.push_back(std::move(outbox_.front()));
                    outbox_.erase(outbox_.begin());
                    outboxBytes_ -= send.back().size();
                }
            }
            for (auto& m : send) {
                m.insert(m.begin(), MsgData);
                const std::vector<uint8_t> box = seal(link_->channel, m.data(), m.size());
                const uint8_t len[4] = { uint8_t(box.size() >> 24), uint8_t(box.size() >> 16), uint8_t(box.size() >> 8), uint8_t(box.size()) };
                link_->out.insert(link_->out.end(), len, len + 4);
                link_->out.insert(link_->out.end(), box.begin(), box.end());
            }
        }
        if (link_ && !link_->out.empty()) {
            const int w = int(::send(S(link_->sock), reinterpret_cast<const char*>(link_->out.data()), int(std::min<size_t>(link_->out.size(), 64 * 1024)), 0));
            if (w > 0) link_->out.erase(link_->out.begin(), link_->out.begin() + w);
            else if (w < 0 && !wouldBlock()) dropLink("the connection failed");
        }
        refreshView();
        if (udp_ == kNone) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }

        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(S(udp_), &rd);
        Sock top = S(udp_);
        if (listen_ != kNone) { FD_SET(S(listen_), &rd); top = std::max(top, S(listen_)); }
        if (link_) {
            if (link_->connecting) FD_SET(S(link_->sock), &wr);
            else FD_SET(S(link_->sock), &rd);
            if (!link_->connecting && !link_->out.empty()) FD_SET(S(link_->sock), &wr);
            top = std::max(top, S(link_->sock));
        }
        timeval tv{ 0, (link_ && (!link_->out.empty() || outboxBytes_ || seconds() < hurryUntil_.load())) ? 1000 : clockOn_ ? 10 * 1000 : 100 * 1000 };
        const int ready = select(int(top) + 1, &rd, &wr, nullptr, &tv);
        if (ready < 0) {   // the network went away under it
            closeSockets();
            continue;
        }
        if (ready == 0) continue;
        if (FD_ISSET(S(udp_), &rd)) {
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            const int got = int(recvfrom(S(udp_), reinterpret_cast<char*>(buf), 512, 0, reinterpret_cast<sockaddr*>(&from), &fl));
            if (got > 0) onDatagram(buf, size_t(got), from.sin_addr.s_addr, ntohs(from.sin_port));
        }
        if (listen_ != kNone && FD_ISSET(S(listen_), &rd)) {
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            Sock c = accept(S(listen_), reinterpret_cast<sockaddr*>(&from), &fl);
            if (c != kNoSock) {
                if (link_) closeSock(c);   // one at a time
                else {
                    nonBlocking(c);
                    link_ = new Link;
                    link_->sock = uintptr_t(c);
                    link_->incoming = true;
                    link_->startedAt = seconds();
                    link_->address = dotted(from.sin_addr.s_addr);
                    link_->addressBe = from.sin_addr.s_addr;
                }
            }
        }
        if (!link_) continue;
        if (link_->connecting) {
            if (!FD_ISSET(S(link_->sock), &wr)) continue;
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(S(link_->sock), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
            if (err) continue;   // left to the timeout, which asks it to call us
            link_->connecting = false;
            startHandshake(*link_);
            continue;
        }
        if (!FD_ISSET(S(link_->sock), &rd)) continue;
        const int got = int(recv(S(link_->sock), reinterpret_cast<char*>(buf), sizeof buf, 0));
        if (got < 0 && wouldBlock()) continue;
        if (got == 0) { dropLink("the other device closed it"); continue; }
        if (got < 0) { if (!wouldBlock()) dropLink("the connection failed"); continue; }
        link_->in.insert(link_->in.end(), buf, buf + got);
        size_t at = 0;
        while (link_ && link_->in.size() - at >= 4) {
            const uint8_t* b = link_->in.data() + at;
            const size_t n = size_t(b[0]) << 24 | size_t(b[1]) << 16 | size_t(b[2]) << 8 | b[3];
            const size_t most = link_->proved ? kMaxFrame + kMac : 64;   // nothing large before it has proved itself
            if (n > most) { dropLink("a message too large"); break; }
            if (link_->in.size() - at < 4 + n) break;
            std::vector<uint8_t> frame(b + 4, b + 4 + n);
            at += 4 + n;
            onFrame(*link_, frame.data(), frame.size());
        }
        if (link_ && at) link_->in.erase(link_->in.begin(), link_->in.begin() + std::ptrdiff_t(at));
    }
    if (link_ && link_->up && link_->out.empty()) {   // (with data still queued, the far side simply sees the link end)
        const uint8_t bye = MsgGoodbye;
        const std::vector<uint8_t> box = seal(link_->channel, &bye, 1);
        writeFrame(link_->sock, box.data(), box.size());
    }
    closeSockets();
    code_.clear();
    pendingCode_.clear();
    pendingPeer_.clear();
#ifdef _WIN32
    WSACleanup();
#endif
}
