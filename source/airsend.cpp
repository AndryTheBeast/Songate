/*
 * Songate · airsend.cpp
 * See airsend.hpp. The sender's side of what airplay.cpp receives: RAOP as iTunes spoke it (read from shairport-sync, raop_play and
 * what airplay.cpp takes in), which every AirPlay 1 speaker plays.
 */
#include "airsend.hpp"
#ifdef GF_AIRPLAY

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include "airport_key.inc"
#include "cores.hpp"
#include "net.hpp"
#include "platform.hpp"

namespace {

#ifdef _WIN32
using Sock = intptr_t;
Sock newSocket(int type) { const SOCKET s = socket(AF_INET, type, 0); return s == INVALID_SOCKET ? -1 : Sock(s); }
void closeSock(Sock s) { if (s >= 0) closesocket(SOCKET(s)); }
void nonBlocking(Sock s, bool on = true) { u_long v = on ? 1 : 0; ioctlsocket(SOCKET(s), FIONBIO, &v); }
bool inProgress() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
using Sock = int;
Sock newSocket(int type) { return socket(AF_INET, type, 0); }
void closeSock(Sock s) { if (s >= 0) close(s); }
void nonBlocking(Sock s, bool on = true) { const int f = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK)); }
bool inProgress() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN; }
#endif

constexpr int kFrames = 352;               // ALAC frames a packet, as iTunes sends
constexpr int kRate = 44100;
constexpr uint32_t kLatency = 2 * kRate;   // the speakers play two seconds behind what is sent (AirSend::delay)
constexpr int kHistory = 1024;             // packets kept to send again (about 8 s)

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
int rng(void*, unsigned char* o, size_t n) { return plat::randomBytes(o, n) ? 0 : -1; }

/** NTP time (seconds since 1900, 32.32): the clock the sync packets and the timing answers share. */
void ntpNow(uint32_t& sec, uint32_t& frac) {
    using namespace std::chrono;
    const double t = duration<double>(system_clock::now().time_since_epoch()).count() + 2208988800.0;
    sec = uint32_t(t);
    frac = uint32_t((t - std::floor(t)) * 4294967296.0);
}
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

std::string base64(const uint8_t* d, size_t n) {
    std::string out(n * 4 / 3 + 8, '\0');
    size_t len = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out.size(), &len, d, n);
    out.resize(len);
    while (!out.empty() && out.back() == '=') out.pop_back();   // (AirPlay's way: no padding)
    return out;
}

std::string hex(const uint8_t* d, size_t n) {
    static const char* x = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += x[d[i] >> 4]; s += x[d[i] & 15]; }
    return s;
}

std::string dotted(uint32_t a) {   // network order
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&a);
    char t[20];
    snprintf(t, sizeof t, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return t;
}

/** ALAC, uncompressed ("escape") frames: a stereo element, its sample count, the samples big-endian, the end tag. */
struct Bits {
    std::vector<uint8_t> o;
    int used = 0;   // bits in the last byte
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if (used == 0) o.push_back(0);
            if (v >> i & 1) o.back() |= uint8_t(0x80 >> used);
            used = (used + 1) & 7;
        }
    }
};
std::vector<uint8_t> alacRaw(const int16_t* lr, int frames) {
    Bits b;
    b.o.reserve(size_t(frames) * 4 + 16);
    b.put(1, 3);        // a channel pair
    b.put(0, 4);        // its instance
    b.put(0, 12);       // unused
    b.put(1, 1);        // its sample count follows
    b.put(0, 2);        // no bytes shifted
    b.put(1, 1);        // escape: uncompressed
    b.put(uint32_t(frames), 32);
    for (int i = 0; i < frames * 2; i++) b.put(uint16_t(lr[i]), 16);
    b.put(7, 3);        // the end
    return b.o;
}

/** A DNS name at `at` (compressed or not), as dotted labels; `at` moves past it. */
bool dnsName(const uint8_t* d, size_t n, size_t& at, std::string& out) {
    out.clear();
    size_t p = at;
    bool jumped = false;
    for (int hops = 0; hops < 32; hops++) {
        if (p >= n) return false;
        const uint8_t len = d[p];
        if (len == 0) { if (!jumped) at = p + 1; return true; }
        if ((len & 0xC0) == 0xC0) {
            if (p + 1 >= n) return false;
            if (!jumped) at = p + 2;
            p = size_t(len & 0x3F) << 8 | d[p + 1];
            jumped = true;
            continue;
        }
        if (p + 1 + len > n) return false;
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(d + p + 1), len);
        p += 1 + len;
    }
    return false;
}

std::string lower(std::string s) { for (char& c : s) c = char(tolower(uint8_t(c))); return s; }

}  // namespace

// ------------------------------------------------------------------ the worker's state

struct AirSend::Impl {
    AirSend& a;
    explicit Impl(AirSend& owner) : a(owner) {}

    // ---- what is known of the network's speakers (under m)
    struct Known {
        AirSend::Speaker s;
        std::map<std::string, std::string> txt;
        bool encrypt = true;   // et=1 offered (RSA): our own way; et=0 only: plain
        double seen = 0;
    };
    std::mutex m;
    std::map<std::string, Known> known;
    std::map<std::string, float> savedVol;
    std::string myName;   // this device's, in the stream's description (under m)
    std::vector<std::string> dropped;   // speakers that hung up while playing (under m)
    // the song now, for a speaker that starts playing later (under m)
    std::string metaTitle, metaArtist, metaAlbum;
    double metaDuration = 0, metaPosition = 0, metaAt = 0;
    std::shared_ptr<const std::vector<uint8_t>> art;
    bool browsing = false, quit = false;
    std::condition_variable wake;

    // ---- a chosen speaker's session
    struct Cmd { enum Kind { Volume, Flush, Meta, Art, Teardown } kind; float vol = 0; uint16_t seq = 0; uint32_t rtp = 0; std::string a, b, c; uint32_t start = 0, now = 0, end = 0;
                 std::shared_ptr<const std::vector<uint8_t>> img; };
    struct Session {
        std::string id, ip, name;
        int port = 0;
        bool encrypt = true;
        std::thread th;
        std::mutex qm;
        std::condition_variable qcv;
        std::deque<Cmd> q;
        std::atomic<int> state{ 0 };   // 0 setting up, 1 playing, 2 failed, 3 done
        std::string why;               // when failed (under Impl::m)
        sockaddr_in audio{}, control{}, timing{};
        bool firstPacket = true, firstSync = true;
        double lastSync = 0;
        bool closing = false;
    };
    std::vector<std::shared_ptr<Session>> sessions;   // the pump's (under m for changes)

    // ---- the stream: one for every speaker
    Sock audioSock = -1, controlSock = -1, timingSock = -1;
    int controlPort = 0, timingPort = 0;
    uint8_t key[16]{}, iv[16]{};
    mbedtls_aes_context aes{};   // (zeroed: freed safely even when setUp never ran)
    std::string wrappedKey;    // base64, RSA-OAEP with the AirPort key's public half
    std::atomic<uint16_t> seq{ 0 };
    std::atomic<uint32_t> rtp{ 0 };
    uint32_t ssrc = 0;
    std::string clientId, activeRemote;
    struct Sent { uint16_t seq = 0; std::vector<uint8_t> enc, plain; bool have = false; };
    std::vector<Sent> history;
    SwrContext* swr = nullptr;
    int swrRate = 0;
    std::vector<int16_t> pending;   // 44.1 kHz stereo, not yet in a packet
    std::atomic<bool> flushWanted{ false };
    double lastMetaAt = -10;

    // ---- discovery
#ifdef _WIN32
    DNS_SERVICE_CANCEL browseCancel{};
    bool browseOn = false;
    std::vector<std::wstring> toResolve;   // under m
    std::set<std::wstring> resolving;      // under m
#else
    Sock mdns = -1;     // port 5353, the group's: every answer (when this device's own receiver does not hold it alone)
    Sock asker = -1;    // a port of its own: answers sent straight back to it (legacy unicast), whoever holds 5353
    bool have5353 = false;
    int heardAnswers = 0, sentAsks = 0;
    double lastAsk = -10;
    int asks = 0;
    std::map<std::string, std::pair<int, std::string>> srv;   // instance → port, host
    std::map<std::string, std::map<std::string, std::string>> txts;
    std::map<std::string, uint32_t> hosts;                    // host → address
    std::set<std::string> instances;
    std::map<std::string, std::string> asSent;                // lowered → as the speaker wrote it (the name shown keeps its case)
#endif
    std::vector<uint32_t> mine;   // this device's addresses (its own receiver is not offered to itself)

    bool setUp() {
        mine.clear();
        mbedtls_aes_init(&aes);
        plat::randomBytes(key, sizeof key);
        plat::randomBytes(iv, sizeof iv);
        mbedtls_aes_setkey_enc(&aes, key, 128);
        {   // the key, wrapped as the speakers unwrap it
            mbedtls_pk_context pk;
            mbedtls_pk_init(&pk);
            mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(kAirportKey), sizeof kAirportKey, nullptr, 0);
            if (mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA) {
                mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
                if (mbedtls_rsa_complete(rsa) == 0) {
                    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
                    std::vector<uint8_t> out(mbedtls_rsa_get_len(rsa));
                    if (mbedtls_rsa_rsaes_oaep_encrypt(rsa, rng, nullptr, MBEDTLS_RSA_PUBLIC, nullptr, 0, sizeof key, key, out.data()) == 0)
                        wrappedKey = base64(out.data(), out.size());
                }
            }
            mbedtls_pk_free(&pk);
        }
        uint8_t r[16];
        plat::randomBytes(r, sizeof r);
        uint16_t s0;
        memcpy(&s0, r, 2);
        seq = s0;
        uint32_t t0;
        memcpy(&t0, r + 2, 4);
        rtp = t0;
        memcpy(&ssrc, r + 6, 4);
        clientId = hex(r + 8, 8);
        uint32_t ar;
        memcpy(&ar, r + 4, 4);
        activeRemote = std::to_string(ar);
        history.resize(kHistory);
        for (const plat::Net& n : plat::networks()) mine.push_back(n.address);

        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_ANY);
        audioSock = newSocket(SOCK_DGRAM);
        controlSock = newSocket(SOCK_DGRAM);
        timingSock = newSocket(SOCK_DGRAM);
        if (audioSock < 0 || controlSock < 0 || timingSock < 0) return false;
        for (Sock s : { controlSock, timingSock }) {
            me.sin_port = 0;
            if (bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) return false;
            nonBlocking(s);
        }
        nonBlocking(audioSock);
        socklen_t len = sizeof me;
        getsockname(controlSock, reinterpret_cast<sockaddr*>(&me), &len);
        controlPort = ntohs(me.sin_port);
        len = sizeof me;
        getsockname(timingSock, reinterpret_cast<sockaddr*>(&me), &len);
        timingPort = ntohs(me.sin_port);
        return true;
    }

    void tearDown() {
        {
            std::lock_guard<std::mutex> g(m);
            for (auto& s : sessions) {
                std::lock_guard<std::mutex> q(s->qm);
                s->q.push_back({ Cmd::Teardown });
                s->closing = true;
                s->qcv.notify_all();
            }
        }
        for (auto& s : sessions) if (s->th.joinable()) s->th.join();
        sessions.clear();
        browseStop();
        closeSock(audioSock);
        closeSock(controlSock);
        closeSock(timingSock);
        audioSock = controlSock = timingSock = -1;
        if (swr) swr_free(&swr);
        mbedtls_aes_free(&aes);
        mbedtls_platform_zeroize(key, sizeof key);
    }

    // ================================================================ discovery

    /** A speaker heard of: the list, kept up to date (under m). */
    void found(const std::string& instance, uint32_t addr, int port, const std::map<std::string, std::string>& txt) {
        if (!addr || !port) return;
        auto am = txt.find("am");
        const std::string model = am != txt.end() ? am->second : "";
        if (model == "Songate" && std::find(mine.begin(), mine.end(), addr) != mine.end()) return;   // this device's own receiver
        Known& k = known[instance];
        k.s.id = instance;
        const size_t at = instance.find('@');
        k.s.name = at != std::string::npos ? instance.substr(at + 1) : instance;
        k.s.model = model;
        k.s.address = dotted(addr);
        k.s.port = port;
        k.txt = txt;
        bool et0 = false, et1 = false;
        auto et = txt.find("et");
        if (et == txt.end()) et0 = true;
        else {
            for (size_t i = 0; i < et->second.size(); i++) {
                if (et->second[i] == '0' && (i + 1 == et->second.size() || et->second[i + 1] == ',')) et0 = true;
                if (et->second[i] == '1' && (i + 1 == et->second.size() || et->second[i + 1] == ',')) et1 = true;
            }
        }
        k.encrypt = et1 || !et0;
        const bool homePod = model.rfind("AudioAccessory", 0) == 0;
        // (a speaker that asked for pairing once stays unsupported for this run: Session marks it)
        if (k.s.state.find("not supported") == std::string::npos) k.s.supported = (et0 || et1) && !homePod;
        if (!k.s.supported && k.s.state.empty()) k.s.state = "AirPlay 2 only, not supported";
        auto v = savedVol.find(instance);
        if (v != savedVol.end() && !k.s.chosen) k.s.volume = v->second;
        k.seen = seconds();
    }

#ifdef _WIN32
    static std::string narrow(const wchar_t* w) {
        if (!w) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(std::max(n, 1)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        if (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    static VOID WINAPI onBrowse(DWORD status, PVOID ctx, PDNS_RECORD rec) {
        auto* self = static_cast<Impl*>(ctx);
        if (status == ERROR_SUCCESS) {
            std::lock_guard<std::mutex> g(self->m);
            for (PDNS_RECORD r = rec; r; r = r->pNext)
                if (r->wType == DNS_TYPE_PTR && r->Data.PTR.pNameHost) self->toResolve.push_back(r->Data.PTR.pNameHost);
        }
        if (rec) DnsRecordListFree(rec, DnsFreeRecordList);
    }
    static VOID WINAPI onResolved(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
        auto* self = static_cast<Impl*>(ctx);
        if (status == ERROR_SUCCESS && inst && inst->pszInstanceName && inst->ip4Address) {
            std::string name = narrow(inst->pszInstanceName);
            const std::string tail = "._raop._tcp.local";
            if (name.size() > tail.size() && lower(name.substr(name.size() - tail.size())) == tail) name.resize(name.size() - tail.size());
            std::map<std::string, std::string> txt;
            for (DWORD i = 0; i < inst->dwPropertyCount; i++) txt[narrow(inst->keys[i])] = narrow(inst->values[i]);
            std::lock_guard<std::mutex> g(self->m);
            self->found(name, *inst->ip4Address, inst->wPort, txt);
            self->resolving.erase(inst->pszInstanceName);
        }
        if (inst) DnsServiceFreeInstance(inst);
    }
    void browseStart() {
        if (browseOn) return;
        DNS_SERVICE_BROWSE_REQUEST req{};
        req.Version = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex = 0;
        req.QueryName = L"_raop._tcp.local";
        req.pBrowseCallback = &Impl::onBrowse;
        req.pQueryContext = this;
        browseOn = DnsServiceBrowse(&req, &browseCancel) == DNS_REQUEST_PENDING;
    }
    void browseStop() {
        if (!browseOn) return;
        DnsServiceBrowseCancel(&browseCancel);
        browseOn = false;
    }
    void browseStep() {
        std::vector<std::wstring> names;
        {
            std::lock_guard<std::mutex> g(m);
            names.swap(toResolve);
        }
        for (const std::wstring& n : names) {
            {
                std::lock_guard<std::mutex> g(m);
                if (resolving.count(n)) continue;
                resolving.insert(n);
            }
            DNS_SERVICE_RESOLVE_REQUEST req{};
            req.Version = DNS_QUERY_REQUEST_VERSION1;
            req.InterfaceIndex = 0;
            req.QueryName = const_cast<PWSTR>(n.c_str());
            req.pResolveCompletionCallback = &Impl::onResolved;
            req.pQueryContext = this;
            DNS_SERVICE_CANCEL c{};
            if (DnsServiceResolve(&req, &c) != DNS_REQUEST_PENDING) { std::lock_guard<std::mutex> g(m); resolving.erase(n); }
        }
    }
#else
    void browseStart() {
        if (mdns >= 0 || asker >= 0) return;
        asker = newSocket(SOCK_DGRAM);
        if (asker >= 0) {
            sockaddr_in any{};
            any.sin_family = AF_INET;
            any.sin_addr.s_addr = htonl(INADDR_ANY);
            any.sin_port = 0;
            if (bind(asker, reinterpret_cast<sockaddr*>(&any), sizeof any) != 0) { closeSock(asker); asker = -1; }
            else {
                if (!mine.empty()) {
                    in_addr out{};
                    out.s_addr = mine[0];
                    setsockopt(asker, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof out);
                }
                nonBlocking(asker);
            }
        }
        asks = 0;
        lastAsk = -10;
        mdns = newSocket(SOCK_DGRAM);
        if (mdns < 0) return;
        int on = 1;
        setsockopt(mdns, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
        setsockopt(mdns, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_ANY);
        me.sin_port = htons(5353);
        if (bind(mdns, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) { closeSock(mdns); mdns = -1; have5353 = false; return; }
        have5353 = true;
        ip_mreq group{};
        group.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
        group.imr_interface.s_addr = mine.empty() ? htonl(INADDR_ANY) : mine[0];
        setsockopt(mdns, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof group);
        if (!mine.empty()) {
            in_addr out{};
            out.s_addr = mine[0];
            setsockopt(mdns, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof out);
        }
        nonBlocking(mdns);
    }
    void browseStop() {
        closeSock(mdns);
        closeSock(asker);
        mdns = asker = -1;
    }
    void ask() {
        uint8_t q[64];
        size_t n = 0;
        const uint8_t head[12] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
        memcpy(q, head, 12);
        n = 12;
        for (const char* l : { "_raop", "_tcp", "local" }) { const size_t len = strlen(l); q[n++] = uint8_t(len); memcpy(q + n, l, len); n += len; }
        q[n++] = 0;
        q[n++] = 0; q[n++] = 12;   // PTR
        q[n++] = 0; q[n++] = 1;    // IN (answers to the group: every listener learns, our receiver answers that way too)
        sockaddr_in g{};
        g.sin_family = AF_INET;
        g.sin_port = htons(5353);
        g.sin_addr.s_addr = inet_addr("224.0.0.251");
        if (asker >= 0) sendto(asker, reinterpret_cast<const char*>(q), int(n), 0, reinterpret_cast<sockaddr*>(&g), sizeof g);
        if (mdns >= 0) sendto(mdns, reinterpret_cast<const char*>(q), int(n), 0, reinterpret_cast<sockaddr*>(&g), sizeof g);
        sentAsks++;
    }
    void heard(const uint8_t* d, size_t n) {
        if (n < 12 || !(d[2] & 0x80)) return;   // answers only
        heardAnswers++;
        const int qd = d[4] << 8 | d[5], an = d[6] << 8 | d[7], ns = d[8] << 8 | d[9], ar = d[10] << 8 | d[11];
        size_t at = 12;
        std::string name;
        for (int i = 0; i < qd; i++) { if (!dnsName(d, n, at, name) || at + 4 > n) return; at += 4; }
        std::set<std::string> touched;
        for (int i = 0; i < an + ns + ar; i++) {
            if (!dnsName(d, n, at, name) || at + 10 > n) return;
            const int type = d[at] << 8 | d[at + 1];
            const size_t rd = size_t(d[at + 8] << 8 | d[at + 9]);
            at += 10;
            if (at + rd > n) return;
            const std::string key = lower(name);
            if (type == 33 || type == 16) asSent.emplace(key, name);
            if (type == 12 && key == "_raop._tcp.local") {
                size_t p = at;
                std::string inst;
                if (dnsName(d, n, p, inst)) { instances.insert(lower(inst)); touched.insert(lower(inst)); asSent[lower(inst)] = inst; }
            } else if (type == 33 && rd >= 7) {
                size_t p = at + 6;
                std::string target;
                if (dnsName(d, n, p, target)) { srv[key] = { d[at + 4] << 8 | d[at + 5], lower(target) }; touched.insert(key); }
            } else if (type == 16) {
                std::map<std::string, std::string> kv;
                for (size_t p = at; p < at + rd;) {
                    const size_t len = d[p];
                    if (p + 1 + len > at + rd) break;
                    const std::string s(reinterpret_cast<const char*>(d + p + 1), len);
                    const size_t eq = s.find('=');
                    if (eq != std::string::npos) kv[s.substr(0, eq)] = s.substr(eq + 1);
                    p += 1 + len;
                }
                txts[key] = kv;
                touched.insert(key);
            } else if (type == 1 && rd == 4) {
                uint32_t a4;
                memcpy(&a4, d + at, 4);
                hosts[key] = a4;
                for (auto& [inst, sv] : srv) if (sv.second == key) touched.insert(inst);
            }
            at += rd;
        }
        std::lock_guard<std::mutex> g(m);
        for (const std::string& inst : touched) {
            auto s = srv.find(inst);
            auto t = txts.find(inst);
            if (s == srv.end() || t == txts.end()) continue;
            auto h = hosts.find(s->second.second);
            if (h == hosts.end()) continue;
            auto as = asSent.find(inst);
            std::string shown = as != asSent.end() ? as->second : inst;   // (matched lowered; shown as the speaker wrote it)
            const std::string tail = "._raop._tcp.local";
            if (shown.size() > tail.size() && lower(shown.substr(shown.size() - tail.size())) == tail) shown.resize(shown.size() - tail.size());
            found(shown, h->second, s->second.first, t->second);
        }
    }
    void browseStep() {
        if (mdns < 0 && asker < 0) return;
        const double now = seconds();
        if ((asks < 3 && now - lastAsk > 1) || now - lastAsk > 8) { ask(); lastAsk = now; asks++; }
        uint8_t d[4096];
        for (Sock s : { mdns, asker }) {
            if (s < 0) continue;
            for (int k = 0; k < 16; k++) {
                sockaddr_in from{};
                socklen_t len = sizeof from;
                const int got = int(recvfrom(s, reinterpret_cast<char*>(d), sizeof d, 0, reinterpret_cast<sockaddr*>(&from), &len));
                if (got <= 0) break;
                heard(d, size_t(got));
            }
        }
    }
#endif

    // ================================================================ a speaker's session (its own thread)

    struct Reply { int status = 0; std::map<std::string, std::string> head; };

    static bool sendAll(Sock s, const std::string& d) {
        size_t done = 0;
        while (done < d.size()) {
            const int n = int(send(s, d.data() + done, int(d.size() - done), 0));
            if (n <= 0) return false;
            done += size_t(n);
        }
        return true;
    }
    static bool readReply(Sock s, Reply& r) {
        std::string buf;
        char c[1024];
        size_t end = std::string::npos;
        while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
            const int n = int(recv(s, c, sizeof c, 0));
            if (n <= 0 || buf.size() > 65536) return false;
            buf.append(c, size_t(n));
        }
        int status = 0;
        if (sscanf(buf.c_str(), "RTSP/1.0 %d", &status) != 1) return false;
        r.status = status;
        size_t p = buf.find("\r\n") + 2;
        while (p < end) {
            const size_t e = buf.find("\r\n", p);
            const std::string line = buf.substr(p, e - p);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string v = line.substr(colon + 1);
                while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                r.head[lower(line.substr(0, colon))] = v;
            }
            p = e + 2;
        }
        size_t body = 0;
        auto cl = r.head.find("content-length");
        if (cl != r.head.end()) body = size_t(std::max(0, atoi(cl->second.c_str())));
        size_t have = buf.size() - (end + 4);
        while (have < body && body < 1 << 20) {   // (read and dropped: nothing a speaker sends back is needed)
            const int n = int(recv(s, c, int(std::min(sizeof c, body - have)), 0));
            if (n <= 0) return false;
            have += size_t(n);
        }
        return true;
    }

    void fail(Session& s, const std::string& why) {
        std::lock_guard<std::mutex> g(m);
        s.why = why;
        s.state = 2;
        auto k = known.find(s.id);
        if (k != known.end()) {
            k->second.s.state = why;
            if (why.find("not supported") != std::string::npos) k->second.s.supported = false;
        }
    }

    void sessionRun(std::shared_ptr<Session> sp) {
        Session& s = *sp;
        Sock t = newSocket(SOCK_STREAM);
        if (t < 0) { fail(s, "Couldn't connect"); return; }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(uint16_t(s.port));
        to.sin_addr.s_addr = inet_addr(s.ip.c_str());
        // connect, three seconds at most
        nonBlocking(t, true);
        bool up = connect(t, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0;
        if (!up && inProgress()) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(t, &w);
            timeval tv{ 3, 0 };
            if (select(int(t + 1), nullptr, &w, nullptr, &tv) > 0) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(t, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                up = err == 0;
            }
        }
        nonBlocking(t, false);
        if (!up) { closeSock(t); fail(s, "Couldn't connect"); return; }
#ifdef _WIN32
        DWORD ms = 4000;
        setsockopt(t, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
        setsockopt(t, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
        timeval tv{ 4, 0 };
        setsockopt(t, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(t, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        sockaddr_in me{};
        socklen_t ml = sizeof me;
        getsockname(t, reinterpret_cast<sockaddr*>(&me), &ml);
        const std::string localIp = dotted(me.sin_addr.s_addr);
        uint8_t sid[4];
        plat::randomBytes(sid, sizeof sid);
        const std::string url = "rtsp://" + localIp + "/" + std::to_string(uint32_t(sid[0]) << 24 | uint32_t(sid[1]) << 16 | uint32_t(sid[2]) << 8 | sid[3]);
        int cseq = 0;
        std::string session;
        auto request = [&](const std::string& method, const std::string& target, const std::string& extra, const std::string& type, const std::string& body, Reply& r) {
            std::string q = method + " " + target + " RTSP/1.0\r\nCSeq: " + std::to_string(++cseq) + "\r\nUser-Agent: iTunes/7.6.2 (Windows; N;)\r\n" +
                            "Client-Instance: " + clientId + "\r\nDACP-ID: " + clientId + "\r\nActive-Remote: " + activeRemote + "\r\n";
            if (!session.empty()) q += "Session: " + session + "\r\n";
            q += extra;
            if (!type.empty()) q += "Content-Type: " + type + "\r\n";
            q += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            q += body;
            return sendAll(t, q) && readReply(t, r);
        };
        auto refused = [&](int status) {
            return status == 401 ? std::string("Asks for a password: not supported")
                 : status == 403 || status == 470 || status == 455 ? std::string("Needs AirPlay 2 pairing: not supported")
                 : "Refused (" + std::to_string(status) + ")";
        };
        Reply r;
        if (!request("OPTIONS", "*", "", "", "", r)) { closeSock(t); fail(s, "No answer"); return; }
        if (r.status != 200) { closeSock(t); fail(s, refused(r.status)); return; }
        std::string sdp = "v=0\r\no=iTunes " + std::to_string(ssrc) + " 0 IN IP4 " + localIp + "\r\ns=iTunes\r\nc=IN IP4 " + s.ip +
                          "\r\nt=0 0\r\nm=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\na=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n";
        if (s.encrypt) sdp += "a=rsaaeskey:" + wrappedKey + "\r\na=aesiv:" + base64(iv, 16) + "\r\n";
        {   // who plays: its name, where iOS puts its own (the user, 2026-11-05: "make it appear From a Switch and not from an iPhone")
            std::lock_guard<std::mutex> g(m);
            std::string n = myName;
            for (char& c : n) if (uint8_t(c) < 0x20) c = ' ';
            if (!n.empty()) sdp = sdp.substr(0, sdp.find("t=0 0")) + "i=" + n.substr(0, 64) + "\r\n" + sdp.substr(sdp.find("t=0 0"));
        }
        if (!request("ANNOUNCE", url, "", "application/sdp", sdp, r)) { closeSock(t); fail(s, "No answer"); return; }
        if (r.status != 200) { closeSock(t); fail(s, refused(r.status)); return; }
        const std::string transport = "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port=" + std::to_string(controlPort) +
                                      ";timing_port=" + std::to_string(timingPort) + "\r\n";
        if (!request("SETUP", url, transport, "", "", r)) { closeSock(t); fail(s, "No answer"); return; }
        if (r.status != 200) { closeSock(t); fail(s, refused(r.status)); return; }
        session = r.head.count("session") ? r.head["session"] : "1";
        const size_t semi = session.find(';');
        if (semi != std::string::npos) session.resize(semi);
        int serverPort = 0, theirControl = 0, theirTiming = 0;
        {
            const std::string tr = r.head["transport"];
            auto num = [&](const char* k) { const size_t p = tr.find(k); return p == std::string::npos ? 0 : atoi(tr.c_str() + p + strlen(k)); };
            serverPort = num("server_port=");
            theirControl = num("control_port=");
            theirTiming = num("timing_port=");
        }
        if (!serverPort) { closeSock(t); fail(s, "No audio port"); return; }
        auto addr = [&](int port) { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(uint16_t(port)); a.sin_addr.s_addr = inet_addr(s.ip.c_str()); return a; };
        s.audio = addr(serverPort);
        s.control = addr(theirControl ? theirControl : serverPort + 1);
        s.timing = addr(theirTiming ? theirTiming : serverPort + 2);
        const uint16_t sq = seq.load();
        const uint32_t rt = rtp.load();
        if (!request("RECORD", url, "Range: npt=0-\r\nRTP-Info: seq=" + std::to_string(sq) + ";rtptime=" + std::to_string(rt) + "\r\n", "", "", r)) {
            closeSock(t); fail(s, "No answer"); return;
        }
        if (r.status != 200) { closeSock(t); fail(s, refused(r.status)); return; }
        float vol0 = 0.6f;
        {
            std::lock_guard<std::mutex> g(m);
            auto k = known.find(s.id);
            if (k != known.end()) vol0 = k->second.s.volume;
        }
        auto volumeBody = [](float v) {
            char b[48];
            snprintf(b, sizeof b, "volume: %.6f\r\n", v <= 0.001f ? -144.0 : -30.0 + 30.0 * double(v));
            return std::string(b);
        };
        request("SET_PARAMETER", url, "", "text/parameters", volumeBody(vol0), r);
        {
            std::lock_guard<std::mutex> g(m);
            auto k = known.find(s.id);
            if (k != known.end()) k->second.s.state = "Playing";
        }
        s.state = 1;   // the pump sends it the sound from now on
        {   // the song now, at once: its details and its cover
            std::lock_guard<std::mutex> g(m);
            if (!metaTitle.empty() || !metaArtist.empty()) {
                const uint32_t now = rtp.load() - kLatency;
                const double pos = metaPosition + (seconds() - metaAt);
                Cmd c{ Cmd::Meta };
                c.a = metaTitle;
                c.b = metaArtist;
                c.c = metaAlbum;
                c.start = now - uint32_t(std::max(0.0, pos) * kRate);
                c.now = now;
                c.end = c.start + uint32_t(std::max(0.0, metaDuration) * kRate);
                std::lock_guard<std::mutex> q(s.qm);
                s.q.push_back(c);
                if (art) { Cmd a{ Cmd::Art }; a.img = art; a.now = now; s.q.push_back(a); }
            }
        }
        // then what changes: its volume, a flush, the song's details, the end
        for (;;) {
            Cmd c;
            bool gone = false;
            {
                std::unique_lock<std::mutex> g(s.qm);
                while (s.q.empty() && !gone) {
                    // nothing to say: every half second, whether the speaker hung up (its Stop, or it went away)
                    if (s.qcv.wait_for(g, std::chrono::milliseconds(500), [&] { return !s.q.empty(); })) break;
                    fd_set rd;
                    FD_ZERO(&rd);
                    FD_SET(t, &rd);
                    timeval z{ 0, 0 };
                    if (select(int(t + 1), &rd, nullptr, nullptr, &z) > 0) {
                        char peek;
                        if (recv(t, &peek, 1, MSG_PEEK) <= 0) gone = true;
                    }
                }
                if (!gone) {
                    c = s.q.front();
                    s.q.pop_front();
                }
            }
            if (gone) {
                {
                    std::lock_guard<std::mutex> g(m);
                    dropped.push_back(s.name);
                    auto k = known.find(s.id);
                    if (k != known.end()) k->second.s.chosen = false;
                }
                fail(s, "Stopped on the speaker");
                break;
            }
            bool ok = true;
            if (c.kind == Cmd::Teardown) { request("TEARDOWN", url, "", "", "", r); break; }
            if (c.kind == Cmd::Volume) ok = request("SET_PARAMETER", url, "", "text/parameters", volumeBody(c.vol), r);
            else if (c.kind == Cmd::Flush) ok = request("FLUSH", url, "RTP-Info: seq=" + std::to_string(c.seq) + ";rtptime=" + std::to_string(c.rtp) + "\r\n", "", "", r);
            else if (c.kind == Cmd::Art && c.img && !c.img->empty()) {
                const std::vector<uint8_t>& im = *c.img;
                const bool png = im.size() > 1 && im[0] == 0x89;
                ok = request("SET_PARAMETER", url, "RTP-Info: rtptime=" + std::to_string(c.now) + "\r\n", png ? "image/png" : "image/jpeg",
                             std::string(reinterpret_cast<const char*>(im.data()), im.size()), r);
            }
            else if (c.kind == Cmd::Meta) {
                // DMAP: a listing item with the title, artist and album; then where it is (start/now/end, in RTP time)
                auto tag = [](std::string& o, const char* t, const std::string& v) {
                    o.append(t, 4);
                    const uint32_t n = uint32_t(v.size());
                    char b[4] = { char(n >> 24), char(n >> 16), char(n >> 8), char(n) };
                    o.append(b, 4);
                    o += v;
                };
                std::string inner, item;
                tag(inner, "minm", c.a);
                tag(inner, "asar", c.b);
                tag(inner, "asal", c.c);
                tag(item, "mlit", inner);
                ok = request("SET_PARAMETER", url, "RTP-Info: rtptime=" + std::to_string(c.now) + "\r\n", "application/x-dmap-tagged", item, r);
                if (ok && c.end > c.start) {
                    const std::string prog = "progress: " + std::to_string(c.start) + "/" + std::to_string(c.now) + "/" + std::to_string(c.end) + "\r\n";
                    ok = request("SET_PARAMETER", url, "", "text/parameters", prog, r);
                }
            }
            if (!ok) { fail(s, "The connection dropped"); break; }
        }
        closeSock(t);
        if (s.state != 2) s.state = 3;
    }

    // ================================================================ the pump (the worker)

    void addSession(const std::string& id) {
        auto k = known.find(id);
        if (k == known.end() || !k->second.s.supported) return;
        for (auto& s : sessions) if (s->id == id && s->state < 2 && !s->closing) return;
        auto sp = std::make_shared<Session>();
        sp->id = id;
        sp->ip = k->second.s.address;
        sp->port = k->second.s.port;
        sp->name = k->second.s.name;
        sp->encrypt = k->second.encrypt && !wrappedKey.empty();
        k->second.s.state = "Connecting";
        sessions.push_back(sp);
        sp->th = std::thread([this, sp] { sessionRun(sp); });
    }

    void dropSession(const std::string& id) {
        for (auto& s : sessions) {
            if (s->id != id || s->closing) continue;
            std::lock_guard<std::mutex> q(s->qm);
            s->q.push_back({ Cmd::Teardown });
            s->closing = true;
            s->qcv.notify_all();
        }
    }

    /** Sessions over (failed, or told to end): their threads joined, the list kept short (under m). */
    void reap() {
        for (auto it = sessions.begin(); it != sessions.end();) {
            Session& s = **it;
            if (s.state >= 2 && s.th.joinable()) {
                s.th.join();
                if (s.state == 2) {   // failed: it is no longer chosen
                    auto k = known.find(s.id);
                    if (k != known.end()) k->second.s.chosen = false;
                }
            }
            if (s.state >= 2 && !s.th.joinable()) { it = sessions.erase(it); continue; }
            ++it;
        }
    }

    void encryptPayload(uint8_t* p, size_t n) {
        const size_t whole = n & ~size_t(15);
        if (!whole) return;
        uint8_t v[16];
        memcpy(v, iv, 16);
        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, whole, v, p, p);
    }

    /** 352 frames of 44.1 kHz stereo: a packet, sent to every speaker playing, kept to send again. */
    void sendPacket(const int16_t* lr) {
        const uint16_t sq = seq.load();
        const uint32_t rt = rtp.load();
        std::vector<uint8_t> alac = alacRaw(lr, kFrames);
        std::vector<uint8_t> plain(12 + alac.size());
        plain[0] = 0x80;
        plain[1] = 0x60;
        plain[2] = uint8_t(sq >> 8);
        plain[3] = uint8_t(sq);
        put32(&plain[4], rt);
        put32(&plain[8], ssrc);
        memcpy(&plain[12], alac.data(), alac.size());
        std::vector<uint8_t> enc = plain;
        encryptPayload(&enc[12], enc.size() - 12);
        for (auto& s : sessions) {
            if (s->state != 1 || s->closing) continue;
            std::vector<uint8_t>& p = s->encrypt ? enc : plain;
            if (s->firstPacket) {   // the stream's start: its marker
                const uint8_t was = p[1];
                p[1] = 0xE0;
                sendto(audioSock, reinterpret_cast<const char*>(p.data()), int(p.size()), 0, reinterpret_cast<sockaddr*>(&s->audio), sizeof s->audio);
                p[1] = was;
                s->firstPacket = false;
            } else {
                sendto(audioSock, reinterpret_cast<const char*>(p.data()), int(p.size()), 0, reinterpret_cast<sockaddr*>(&s->audio), sizeof s->audio);
            }
        }
        Sent& h = history[sq % kHistory];
        h.seq = sq;
        h.enc = std::move(enc);
        h.plain = std::move(plain);
        h.have = true;
        seq = uint16_t(sq + 1);
        rtp = rt + kFrames;
    }

    /** Every second, to each speaker: which RTP time is heard now (two seconds behind the newest sent), on the shared clock. */
    void sync(Session& s, double now) {
        uint8_t p[20];
        p[0] = s.firstSync ? 0x90 : 0x80;
        p[1] = 0xD4;
        p[2] = 0;
        p[3] = 7;
        const uint32_t rt = rtp.load();
        put32(p + 4, rt - kLatency);
        uint32_t sec, frac;
        ntpNow(sec, frac);
        put32(p + 8, sec);
        put32(p + 12, frac);
        put32(p + 16, rt);
        sendto(controlSock, reinterpret_cast<const char*>(p), sizeof p, 0, reinterpret_cast<sockaddr*>(&s.control), sizeof s.control);
        s.firstSync = false;
        s.lastSync = now;
    }

    void onControl() {
        uint8_t d[64];
        for (int k = 0; k < 32; k++) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const int n = int(recvfrom(controlSock, reinterpret_cast<char*>(d), sizeof d, 0, reinterpret_cast<sockaddr*>(&from), &len));
            if (n <= 0) break;
            if (n < 8 || (d[1] & 0x7F) != 0x55) continue;   // a speaker asks again for packets it lost
            const uint16_t first = uint16_t(d[4] << 8 | d[5]), count = uint16_t(d[6] << 8 | d[7]);
            for (auto& s : sessions) {
                if (s->state != 1 || s->control.sin_addr.s_addr != from.sin_addr.s_addr) continue;
                for (uint16_t i = 0; i < std::min<uint16_t>(count, 64); i++) {
                    const uint16_t want = uint16_t(first + i);
                    const Sent& h = history[want % kHistory];
                    if (!h.have || h.seq != want) continue;
                    const std::vector<uint8_t>& p = s->encrypt ? h.enc : h.plain;
                    std::vector<uint8_t> o(4 + p.size());
                    o[0] = 0x80;
                    o[1] = 0xD6;
                    o[2] = uint8_t(want >> 8);
                    o[3] = uint8_t(want);
                    memcpy(&o[4], p.data(), p.size());
                    sendto(controlSock, reinterpret_cast<const char*>(o.data()), int(o.size()), 0, reinterpret_cast<sockaddr*>(&s->control), sizeof s->control);
                }
                break;
            }
        }
    }

    void onTiming() {
        uint8_t d[64];
        for (int k = 0; k < 32; k++) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const int n = int(recvfrom(timingSock, reinterpret_cast<char*>(d), sizeof d, 0, reinterpret_cast<sockaddr*>(&from), &len));
            if (n <= 0) break;
            if (n < 32 || (d[1] & 0x7F) != 0x52) continue;   // a timing question: the asked time, ours when it came, ours as we answer
            uint8_t o[32] = {};
            o[0] = 0x80;
            o[1] = 0xD3;
            o[2] = d[2];
            o[3] = d[3];
            memcpy(o + 8, d + 24, 8);
            uint32_t sec, frac;
            ntpNow(sec, frac);
            put32(o + 16, sec);
            put32(o + 20, frac);
            put32(o + 24, sec);
            put32(o + 28, frac);
            sendto(timingSock, reinterpret_cast<const char*>(o), sizeof o, 0, reinterpret_cast<sockaddr*>(&from), sizeof from);
        }
    }

    /** The mix that came since: to 44.1 kHz, into packets. */
    void pump() {
        unsigned r = a.read_.load(std::memory_order_relaxed);
        const unsigned w = a.written_.load(std::memory_order_acquire);
        if (flushWanted.exchange(false)) {
            r = w;   // dropped: what was said before the change
            pending.clear();
            a.read_.store(r, std::memory_order_release);
            const uint16_t sq = seq.load();
            const uint32_t rt = rtp.load();
            for (auto& s : sessions) {
                if (s->state != 1 || s->closing) continue;
                std::lock_guard<std::mutex> q(s->qm);
                s->q.push_back({ Cmd::Flush, 0, sq, rt });
                s->qcv.notify_all();
            }
        }
        const int inRate = a.rate_.load();
        if (!swr || swrRate != inRate) {
            if (swr) swr_free(&swr);
            AVChannelLayout stereo;
            av_channel_layout_default(&stereo, 2);
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kRate, &stereo, AV_SAMPLE_FMT_S16, inRate, 0, nullptr) < 0 || swr_init(swr) < 0) {
                if (swr) swr_free(&swr);
                a.read_.store(w, std::memory_order_release);
                return;
            }
            swrRate = inRate;
        }
        static int16_t in[4096 * 2], out[4800 * 2];
        while (w - r > 0) {
            const unsigned n = std::min(w - r, 4096u);
            for (unsigned i = 0; i < n; i++) {
                in[i * 2] = a.ring_[((r + i) & (Ring - 1)) * 2];
                in[i * 2 + 1] = a.ring_[((r + i) & (Ring - 1)) * 2 + 1];
            }
            r += n;
            const uint8_t* src[1] = { reinterpret_cast<const uint8_t*>(in) };
            uint8_t* dst[1] = { reinterpret_cast<uint8_t*>(out) };
            const int got = swr_convert(swr, dst, 4800, src, int(n));
            if (got > 0) pending.insert(pending.end(), out, out + got * 2);
        }
        a.read_.store(r, std::memory_order_release);
        size_t used = 0;
        while (pending.size() - used >= size_t(kFrames) * 2) {
            sendPacket(&pending[used]);
            used += size_t(kFrames) * 2;
        }
        if (used) pending.erase(pending.begin(), pending.begin() + long(used));
    }

    void loop() {
        while (true) {
            bool wantBrowse;
            {
                std::lock_guard<std::mutex> g(m);
                if (quit) break;
                wantBrowse = browsing;
                bool anyChosen = false;
                for (auto& [id, k] : known) anyChosen = anyChosen || k.s.chosen;
                wantBrowse = wantBrowse || anyChosen;
                // what the main thread chose: sessions to start and to end
                for (auto& [id, k] : known) {
                    bool live = false;
                    for (auto& s : sessions) live = live || (s->id == id && s->state < 2 && !s->closing);
                    if (k.s.chosen && !live) addSession(id);
                    if (!k.s.chosen && live) dropSession(id);
                }
                reap();
                bool tap = false, here = false;
                for (auto& s : sessions) {
                    if (s->closing) continue;
                    tap = tap || s->state <= 1;
                    here = here || s->state == 1;
                }
                if (tap != a.tapping_.load()) {
                    if (tap) { a.read_.store(a.written_.load()); pending.clear(); }
                    a.tapping_ = tap;
                }
                a.silence_ = here;
                // states, kept current for the list
                for (auto& s : sessions) {
                    auto k = known.find(s->id);
                    if (k == known.end()) continue;
                    if (s->state == 1 && !s->closing) k->second.s.playing = true;
                    if (s->state >= 2 || s->closing) k->second.s.playing = false;
                }
                // speakers long silent, and not chosen, leave the list
                const double now = seconds();
                for (auto it = known.begin(); it != known.end();) {
                    if (!it->second.s.chosen && now - it->second.seen > 90) it = known.erase(it);
                    else ++it;
                }
            }
            if (wantBrowse) browseStart();
            else browseStop();
            browseStep();
            onControl();
            onTiming();
            if (a.tapping_) pump();
            const double now = seconds();
            for (auto& s : sessions) if (s->state == 1 && !s->closing && now - s->lastSync >= 1.0) sync(*s, now);
            // wait for a speaker's packet, or 5 ms
            fd_set rd;
            FD_ZERO(&rd);
            Sock top = 0;
            for (Sock s : { controlSock, timingSock }) { FD_SET(s, &rd); top = std::max(top, s); }
#ifndef _WIN32
            if (mdns >= 0) { FD_SET(mdns, &rd); top = std::max(top, mdns); }
            if (asker >= 0) { FD_SET(asker, &rd); top = std::max(top, asker); }
#endif
            timeval tv{ 0, 5000 };
            select(int(top + 1), &rd, nullptr, nullptr, &tv);
        }
    }
};

// ------------------------------------------------------------------ the main thread's side

AirSend::AirSend() : impl_(std::make_unique<Impl>(*this)) {}
AirSend::~AirSend() { stop(); }

void AirSend::start() {
    if (thread_.joinable()) return;
    if (!ring_) ring_ = new int16_t[Ring * 2]();
    impl_->quit = false;
    thread_ = std::thread([this] { run(); });
}

void AirSend::run() {
    cores::moveHere(cores::Net);
    // The console's sockets need its network started first (net::begin, as the receiver does; the user, 2026-11-05: "the switch is
    // not finding the android airplay": every socket failed at launch and the sender ended there). Not there yet: again in 2 s
    for (;;) {
        {
            std::lock_guard<std::mutex> g(impl_->m);
            if (impl_->quit) return;
        }
        std::string err;
        if (net::begin(err) && impl_->setUp()) {
            impl_->loop();
            impl_->tearDown();
            return;
        }
        impl_->tearDown();
        for (int i = 0; i < 20; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> g(impl_->m);
            if (impl_->quit) return;
        }
    }
}

void AirSend::stop() {
    {
        std::lock_guard<std::mutex> g(impl_->m);
        impl_->quit = true;
    }
    if (thread_.joinable()) thread_.join();
    tapping_ = false;
    silence_ = false;
    delete[] ring_;
    ring_ = nullptr;
}

void AirSend::setName(const std::string& name) {
    std::lock_guard<std::mutex> g(impl_->m);
    impl_->myName = name;
}

void AirSend::browse(bool on) {
    std::lock_guard<std::mutex> g(impl_->m);
    impl_->browsing = on;
}

std::vector<AirSend::Speaker> AirSend::speakers() {
    std::lock_guard<std::mutex> g(impl_->m);
    std::vector<Speaker> v;
    for (auto& [id, k] : impl_->known) v.push_back(k.s);
    std::sort(v.begin(), v.end(), [](const Speaker& x, const Speaker& y) {
        if (x.supported != y.supported) return x.supported;
        return x.name < y.name;
    });
    return v;
}

void AirSend::choose(const std::string& id, bool on) {
    std::lock_guard<std::mutex> g(impl_->m);
    auto k = impl_->known.find(id);
    if (k == impl_->known.end() || (on && !k->second.s.supported)) return;
    k->second.s.chosen = on;
    if (on) k->second.s.state = "Connecting";
    else k->second.s.state.clear();
}

void AirSend::chooseNone() {
    std::lock_guard<std::mutex> g(impl_->m);
    for (auto& [id, k] : impl_->known) {
        if (k.s.chosen) k.s.state.clear();
        k.s.chosen = false;
    }
}

void AirSend::volume(const std::string& id, float v) {
    std::lock_guard<std::mutex> g(impl_->m);
    auto k = impl_->known.find(id);
    if (k == impl_->known.end()) return;
    k->second.s.volume = v;
    impl_->savedVol[id] = v;
    for (auto& s : impl_->sessions) {
        if (s->id != id || s->state != 1 || s->closing) continue;
        std::lock_guard<std::mutex> q(s->qm);
        // only the newest volume waits: a slider dragged asks once per step at most
        for (auto it = s->q.begin(); it != s->q.end();) it = it->kind == Impl::Cmd::Volume ? s->q.erase(it) : it + 1;
        s->q.push_back({ Impl::Cmd::Volume, v });
        s->qcv.notify_all();
    }
}

void AirSend::flush() { impl_->flushWanted = true; }

void AirSend::artwork(std::vector<uint8_t> image) {
    std::lock_guard<std::mutex> g(impl_->m);
    impl_->art = image.empty() ? nullptr : std::make_shared<const std::vector<uint8_t>>(std::move(image));
    if (!impl_->art) return;
    const uint32_t now = impl_->rtp.load() - kLatency;
    for (auto& s : impl_->sessions) {
        if (s->state != 1 || s->closing) continue;
        std::lock_guard<std::mutex> q(s->qm);
        Impl::Cmd c{ Impl::Cmd::Art };
        c.img = impl_->art;
        c.now = now;
        s->q.push_back(c);
        s->qcv.notify_all();
    }
}

void AirSend::metadata(const std::string& title, const std::string& artist, const std::string& album, double duration, double position) {
    std::lock_guard<std::mutex> g(impl_->m);
    impl_->metaTitle = title;
    impl_->metaArtist = artist;
    impl_->metaAlbum = album;
    impl_->metaDuration = duration;
    impl_->metaPosition = position;
    impl_->metaAt = seconds();
    const uint32_t now = impl_->rtp.load() - kLatency;
    const uint32_t start = now - uint32_t(std::max(0.0, position) * kRate);
    const uint32_t end = start + uint32_t(std::max(0.0, duration) * kRate);
    for (auto& s : impl_->sessions) {
        if (s->state != 1 || s->closing) continue;
        std::lock_guard<std::mutex> q(s->qm);
        Impl::Cmd c{ Impl::Cmd::Meta };
        c.a = title;
        c.b = artist;
        c.c = album;
        c.start = start;
        c.now = now;
        c.end = end;
        s->q.push_back(c);
        s->qcv.notify_all();
    }
}

void AirSend::feed(const void* samples, int frames, bool float32, int channels, int rate) {
    if (!ring_ || frames <= 0 || channels < 1) return;
    rate_.store(rate, std::memory_order_relaxed);
    unsigned w = written_.load(std::memory_order_relaxed);
    const unsigned free = Ring - (w - read_.load(std::memory_order_acquire));
    if (unsigned(frames) > free) return;   // the worker is behind: dropped rather than waited for (never on the audio thread)
    for (int i = 0; i < frames; i++, w++) {
        int16_t l, r;
        if (float32) {
            const float* f = static_cast<const float*>(samples) + i * channels;
            l = int16_t(std::clamp(f[0], -1.f, 1.f) * 32767.f);
            r = int16_t(std::clamp(channels > 1 ? f[1] : f[0], -1.f, 1.f) * 32767.f);
        } else {
            const int16_t* s = static_cast<const int16_t*>(samples) + i * channels;
            l = s[0];
            r = channels > 1 ? s[1] : s[0];
        }
        ring_[(w & (Ring - 1)) * 2] = l;
        ring_[(w & (Ring - 1)) * 2 + 1] = r;
    }
    written_.store(w, std::memory_order_release);
}

void AirSend::setVolumes(const std::vector<std::pair<std::string, float>>& v) {
    std::lock_guard<std::mutex> g(impl_->m);
    for (const auto& [id, vol] : v) impl_->savedVol[id] = std::clamp(vol, 0.f, 1.f);
}

std::vector<std::string> AirSend::takeDropped() {
    std::lock_guard<std::mutex> g(impl_->m);
    return std::move(impl_->dropped);
}

std::string AirSend::status() {
    std::lock_guard<std::mutex> g(impl_->m);
    char b[160];
#ifdef _WIN32
    snprintf(b, sizeof b, "Windows DNS-SD %s; %d known", impl_->browseOn ? "browsing" : "idle", int(impl_->known.size()));
#else
    snprintf(b, sizeof b, "5353 %s, own port %s; %d questions, %d answers; %d known", impl_->have5353 ? "held" : "not held",
             impl_->asker >= 0 ? "open" : "closed", impl_->sentAsks, impl_->heardAnswers, int(impl_->known.size()));
#endif
    return b;
}

std::vector<std::pair<std::string, float>> AirSend::volumes() {
    std::lock_guard<std::mutex> g(impl_->m);
    return { impl_->savedVol.begin(), impl_->savedVol.end() };
}

#endif
