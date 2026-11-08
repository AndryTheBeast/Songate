/*
 * Songate · airplay.cpp
 * See airplay.hpp. How iOS speaks was read from shairport-sync and from what it said to tools/clicktest; Apple publishes nothing of it.
 */
#include "airplay.hpp"
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
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "airport_key.inc"
#include "cores.hpp"
#include "loudness.hpp"
#include "net.hpp"
#include "platform.hpp"

namespace {

constexpr int kRtspPort = 5005, kAudio = 6000, kControl = 6001, kTiming = 6002;   // (5000 is the console's FTP)
constexpr size_t kMaxRequest = 2 * 1024 * 1024;   // a cover arrives in one (iOS: a few hundred KB at most)
constexpr int kSlots = 512, kPacket = 2048;
constexpr unsigned kPrime = 48000, kTooFull = 48000 * 3;   // how much waits before the sound starts; more than this and it is cut back
constexpr double kAskAfter = 0.1, kGiveUpAfter = 0.6;      // a packet missing: asked for again, then played as silence
// what the announcement says this receiver is (both the console's own mDNS and Windows' say the same)
constexpr const char* kTxt[] = { "txtvers=1", "ch=2", "cn=0,1", "et=0,1", "ek=1", "sr=44100", "ss=16", "sv=false", "da=true", "pw=false", "md=0,1,2",
                                 "tp=UDP", "vn=65537", "vs=105.1", "sf=0x4", "am=Songate" };

/** This device's wall clock as NTP seconds (since 1900): what the sender's clock is compared with. */
double ntpLocal() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count() + 2208988800.0;
}
double ntp64(const uint8_t* p) {
    const uint32_t s = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    const uint32_t f = uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7];
    return double(s) + double(f) / 4294967296.0;
}
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
int rng(void*, unsigned char* o, size_t n) { return plat::randomBytes(o, n) ? 0 : -1; }
// Sockets on the console and on the PC (session 11): a Winsock SOCKET held as a number, -1 for none, as the console's are
#ifdef _WIN32
using Sock = intptr_t;
Sock newSocket(int type) { const SOCKET s = socket(AF_INET, type, 0); return s == INVALID_SOCKET ? -1 : Sock(s); }
void closeSock(Sock s) { closesocket(SOCKET(s)); }
void nonBlocking(Sock s) { u_long on = 1; ioctlsocket(SOCKET(s), FIONBIO, &on); }
bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
using Sock = int;
Sock newSocket(int type) { return socket(AF_INET, type, 0); }
void closeSock(Sock s) { close(s); }
void nonBlocking(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN; }
#endif

void put16(std::vector<uint8_t>& o, unsigned v) { o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
void put32(std::vector<uint8_t>& o, unsigned v) { put16(o, v >> 16); put16(o, v & 0xffff); }
void dnsName(std::vector<uint8_t>& o, const std::vector<std::string>& labels) {
    for (const std::string& l : labels) { o.push_back(uint8_t(std::min<size_t>(l.size(), 63))); o.insert(o.end(), l.begin(), l.begin() + std::ptrdiff_t(std::min<size_t>(l.size(), 63))); }
    o.push_back(0);
}
void record(std::vector<uint8_t>& o, const std::vector<std::string>& owner, unsigned type, unsigned cls, unsigned ttl, const std::vector<uint8_t>& data) {
    dnsName(o, owner);
    put16(o, type);
    put16(o, cls);
    put32(o, ttl);
    put16(o, unsigned(data.size()));
    o.insert(o.end(), data.begin(), data.end());
}
bool holds(const uint8_t* d, size_t n, const char* s) {
    const size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(d + i, s, k)) return true;
    return false;
}
std::vector<uint8_t> unbase64(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '=' || s.back() == '\r')) s.pop_back();
    while (s.size() % 4) s += '=';
    std::vector<uint8_t> out(s.size() + 1);
    size_t n = 0;
    if (mbedtls_base64_decode(out.data(), out.size(), &n, reinterpret_cast<const unsigned char*>(s.data()), s.size()) != 0) n = 0;
    out.resize(n);
    return out;
}
std::string lineOf(const std::string& text, const char* key) {
    const size_t s = text.find(key);
    if (s == std::string::npos) return "";
    const size_t from = s + strlen(key), e = text.find_first_of("\r\n", from);
    return text.substr(from, e == std::string::npos ? e : e - from);
}

}  // namespace

struct AirPlay::Impl {
    AirPlay& a;
    Sock mdns = -1, listener = -1, client = -1, media[3] = { -1, -1, -1 };
    uint32_t ip = 0;
    uint8_t hw[6] = {};
    std::string instance, host, in;
    std::vector<uint8_t> announce;

    mbedtls_pk_context pk;
    mbedtls_rsa_context* rsa = nullptr;
    mbedtls_aes_context aes;
    uint8_t iv[16] = {};
    bool haveAes = false;

    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int16_t pcm[8192 * 2];
    loud::Meter* meter = nullptr;   // this song's, from its start or a skip (levelling: App::airplayTick)
    float meterIn[8192 * 2];
    long long meterFrames = 0;

    struct Slot { bool have = false, asked = false; int size = 0; double waitingSince = 0; uint8_t data[kPacket]; };
    Slot* slots = nullptr;
    bool haveNext = false;
    uint16_t nextSeq = 0, topSeq = 0;
    sockaddr_in control{};
    sockaddr_in timingTo{};          // the sender's timing port: asked what time it is, to follow its clock
    bool haveTimingTo = false;
    double lastTimeAsk = 0;
    int timeAsks = 0;
    struct Probe { double offset, rtt; };
    std::vector<Probe> probes;
    bool haveControl = false;
    std::string onceId;   // allowed once: for this stream, until it ends

    explicit Impl(AirPlay& owner) : a(owner) {
        mbedtls_pk_init(&pk);
        mbedtls_aes_init(&aes);
        mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(kAirportKey), sizeof kAirportKey, nullptr, 0);
        if (mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA) rsa = mbedtls_pk_rsa(pk);
        if (rsa && mbedtls_rsa_complete(rsa) != 0) rsa = nullptr;
        slots = new Slot[kSlots];
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
    }
    ~Impl() {
        delete meter;
        endStream();
        closeAll();
        av_packet_free(&pkt);
        av_frame_free(&frame);
        delete[] slots;
        mbedtls_pk_free(&pk);
        mbedtls_aes_free(&aes);
    }

    void closeAll() {
        for (Sock* s : { &mdns, &listener, &client, &cand, &media[0], &media[1], &media[2] }) if (*s >= 0) { closeSock(*s); *s = -1; }
        unadvertise();
        in.clear();
        candIn.clear();
        clientAllowed = false;
        if (ask.on) endAsk();   // (a question still up ends with the sockets)
    }

    void setName() {
        const std::string name = plat::deviceName();
        uint64_t h = 1469598103934665603ull;
        for (char c : name) h = (h ^ uint8_t(c)) * 1099511628211ull;
        char id[16];
        for (int i = 0; i < 6; i++) hw[i] = uint8_t(h >> (i * 8));
        hw[0] = uint8_t((hw[0] | 2) & 0xFE);   // a local address, as a made-up one should be
        snprintf(id, sizeof id, "%02X%02X%02X%02X%02X%02X", hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
        instance = std::string(id) + "@Songate on " + name;
        host = std::string("songate-") + id;
        for (char& c : host) c = char(tolower(uint8_t(c)));
    }

    std::vector<uint8_t> buildAnnounce(unsigned ttl) {
        const std::vector<std::string> service = { "_raop", "_tcp", "local" }, me = { instance, "_raop", "_tcp", "local" }, hostName = { host, "local" };
        std::vector<uint8_t> o, d;
        put16(o, 0); put16(o, 0x8400); put16(o, 0); put16(o, 4); put16(o, 0); put16(o, 0);
        dnsName(d, me);
        record(o, service, 12, 1, ttl ? 4500 : 0, d);
        d.clear();
        put16(d, 0); put16(d, 0); put16(d, kRtspPort);
        dnsName(d, hostName);
        record(o, me, 33, 0x8001, ttl, d);
        d.clear();
        for (const char* t : kTxt) {
            d.push_back(uint8_t(strlen(t)));
            d.insert(d.end(), t, t + strlen(t));
        }
        record(o, me, 16, 0x8001, ttl, d);
        d.assign(reinterpret_cast<uint8_t*>(&ip), reinterpret_cast<uint8_t*>(&ip) + 4);
        record(o, hostName, 1, 0x8001, ttl, d);
        return o;
    }

#ifdef _WIN32
    // Windows already answers mDNS on 5353 (its DNS client; Bonjour too, if installed): this device is announced through it, and
    // it answers iOS's questions (DnsServiceRegister, Windows 10 1903 on)
    PDNS_SERVICE_INSTANCE service = nullptr;
    DNS_SERVICE_REGISTER_REQUEST registered{};
    bool advertised = false;
    static VOID WINAPI onRegistered(DWORD, PVOID, PDNS_SERVICE_INSTANCE inst) { if (inst) DnsServiceFreeInstance(inst); }
    static std::wstring wide(const std::string& u) {
        std::wstring w(u.size() + 1, L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), int(u.size()), &w[0], int(w.size()));
        w.resize(size_t(std::max(n, 0)));
        return w;
    }
    bool advertise() {
        wchar_t hostW[256];
        DWORD hn = 256;
        if (!GetComputerNameExW(ComputerNameDnsHostname, hostW, &hn)) return false;
        const std::wstring name = wide(instance) + L"._raop._tcp.local", hostName = std::wstring(hostW, hn) + L".local";
        std::vector<std::wstring> keys, values;
        for (const char* t : kTxt) {
            const std::string kv = t;
            keys.push_back(wide(kv.substr(0, kv.find('='))));
            values.push_back(wide(kv.substr(kv.find('=') + 1)));
        }
        std::vector<PCWSTR> k, v;
        for (size_t i = 0; i < keys.size(); i++) { k.push_back(keys[i].c_str()); v.push_back(values[i].c_str()); }
        IP4_ADDRESS a4 = ip;
        service = DnsServiceConstructInstance(name.c_str(), hostName.c_str(), &a4, nullptr, WORD(kRtspPort), 0, 0, DWORD(k.size()), k.data(), v.data());
        if (!service) return false;
        registered = DNS_SERVICE_REGISTER_REQUEST{};
        registered.Version = DNS_QUERY_REQUEST_VERSION1;
        registered.pServiceInstance = service;
        registered.pRegisterCompletionCallback = onRegistered;
        advertised = DnsServiceRegister(&registered, nullptr) == DNS_REQUEST_PENDING;
        return advertised;
    }
    void unadvertise() {   // goodbye: iOS lists it no longer
        if (advertised) DnsServiceDeRegister(&registered, nullptr);
        advertised = false;
        if (service) DnsServiceFreeInstance(service);
        service = nullptr;
    }
#else
    void unadvertise() {}
#endif

    bool open() {
        setName();
        const std::vector<plat::Net> nets = plat::networks();
        if (nets.empty()) return false;
        ip = nets[0].address;
        int on = 1;
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_ANY);
#ifndef _WIN32
        mdns = newSocket(SOCK_DGRAM);
        setsockopt(mdns, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        setsockopt(mdns, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
        me.sin_port = htons(5353);
        if (bind(mdns, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) return false;
        ip_mreq group{};
        group.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
        group.imr_interface.s_addr = ip;
        if (setsockopt(mdns, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof group) != 0) return false;
        in_addr out{};
        out.s_addr = ip;
        setsockopt(mdns, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof out);
        u_char ttl = 255;
        setsockopt(mdns, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        nonBlocking(mdns);
#endif
        listener = newSocket(SOCK_STREAM);
        if (listener < 0) return false;
#ifdef _WIN32
        setsockopt(SOCKET(listener), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);   // (not shared with another program)
#endif
        me.sin_port = htons(kRtspPort);
        if (bind(listener, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0 || listen(listener, 2) != 0) return false;
        nonBlocking(listener);
        for (int i = 0; i < 3; i++) {
            media[i] = newSocket(SOCK_DGRAM);
            if (media[i] < 0) return false;
            me.sin_port = htons(uint16_t(kAudio + i));
            if (bind(media[i], reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) return false;
            nonBlocking(media[i]);
        }
#ifdef _WIN32
        if (!advertise()) return false;
#else
        announce = buildAnnounce(120);
#endif
        return true;
    }

    void sayAnnounce(const std::vector<uint8_t>& what) {
        sockaddr_in g{};
        g.sin_family = AF_INET;
        g.sin_port = htons(5353);
        g.sin_addr.s_addr = inet_addr("224.0.0.251");
        sendto(mdns, reinterpret_cast<const char*>(what.data()), int(what.size()), 0, reinterpret_cast<sockaddr*>(&g), sizeof g);
    }

    void onMdns() {
        uint8_t d[1500];
        sockaddr_in from{};
        socklen_t len = sizeof from;
        int n;
        while ((n = int(recvfrom(mdns, reinterpret_cast<char*>(d), sizeof d, 0, reinterpret_cast<sockaddr*>(&from), &len))) > 0) {
            if (from.sin_addr.s_addr == ip || n <= 12 || (d[2] & 0x80) || !holds(d, size_t(n), "_raop")) continue;
            sayAnnounce(announce);
            // asked from another port (a sender whose own receiver holds 5353, as Songate's on the console: the user, 2026-11-05, "the
            // switch is not finding the android airplay"): the answer goes straight back to it as well (mDNS's legacy unicast)
            if (ntohs(from.sin_port) != 5353)
                sendto(mdns, reinterpret_cast<const char*>(announce.data()), int(announce.size()), 0, reinterpret_cast<sockaddr*>(&from), sizeof from);
        }
    }

    std::string answerChallenge(const std::string& challenge) {
        const std::vector<uint8_t> c = unbase64(challenge);
        if (c.size() != 16 || !rsa) return "";
        uint8_t data[32] = {};
        memcpy(data, c.data(), 16);
        memcpy(data + 16, &ip, 4);
        memcpy(data + 20, hw, 6);
        const size_t size = mbedtls_rsa_get_len(rsa);
        std::vector<uint8_t> em(size, 0xFF), sig(size);
        em[0] = 0;
        em[1] = 1;
        em[size - 33] = 0;
        memcpy(em.data() + size - 32, data, 32);
        if (mbedtls_rsa_private(rsa, rng, nullptr, em.data(), sig.data()) != 0) return "";
        std::string out(size * 2, 0);
        size_t n = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out.size(), &n, sig.data(), size) != 0) return "";
        out.resize(n);
        while (!out.empty() && out.back() == '=') out.pop_back();
        return out;
    }

    /** The session's keys and its ALAC settings: only what a sender of ours can mean. */
    /** Whether a stream's description can be played, changing nothing: checked before anything is switched over to it (a device
     *  under a remembered name with a broken one took the stream over and was then refused: session 11's takeover test). */
    bool playable(const std::string& body) {
        if (!rsa) return false;
        const std::vector<uint8_t> w = unbase64(lineOf(body, "a=rsaaeskey:")), i = unbase64(lineOf(body, "a=aesiv:"));
        if (w.size() != mbedtls_rsa_get_len(rsa) || i.size() != 16) return false;
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
        uint8_t key[32];
        size_t n = 0;
        const bool unwrapped = mbedtls_rsa_rsaes_oaep_decrypt(rsa, rng, nullptr, MBEDTLS_RSA_PRIVATE, nullptr, 0, &n, w.data(), key, sizeof key) == 0 && n == 16;
        mbedtls_platform_zeroize(key, sizeof key);
        if (!unwrapped) return false;
        unsigned pt, fp, compat, bits, pb, mb, kb, ch, run, maxBytes, avg, rate;
        if (sscanf(lineOf(body, "a=fmtp:").c_str(), "%u %u %u %u %u %u %u %u %u %u %u %u", &pt, &fp, &compat, &bits, &pb, &mb, &kb, &ch, &run, &maxBytes, &avg, &rate) != 12) return false;
        return fp >= 1 && fp <= 4096 && bits == 16 && ch == 2 && rate == 44100;
    }

    bool announced(const std::string& body) {
        endStream();
        if (!rsa) return false;
        const std::string wrapped = lineOf(body, "a=rsaaeskey:"), ivText = lineOf(body, "a=aesiv:"), fmtp = lineOf(body, "a=fmtp:");
        const std::vector<uint8_t> w = unbase64(wrapped), i = unbase64(ivText);
        if (w.size() != mbedtls_rsa_get_len(rsa) || i.size() != 16) return false;
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
        uint8_t key[32];
        size_t n = 0;
        if (mbedtls_rsa_rsaes_oaep_decrypt(rsa, rng, nullptr, MBEDTLS_RSA_PRIVATE, nullptr, 0, &n, w.data(), key, sizeof key) != 0 || n != 16) return false;
        mbedtls_aes_setkey_dec(&aes, key, 128);
        mbedtls_platform_zeroize(key, sizeof key);
        memcpy(iv, i.data(), 16);
        haveAes = true;
        unsigned pt, fp, compat, bits, pb, mb, kb, ch, run, maxBytes, avg, rate;
        if (sscanf(fmtp.c_str(), "%u %u %u %u %u %u %u %u %u %u %u %u", &pt, &fp, &compat, &bits, &pb, &mb, &kb, &ch, &run, &maxBytes, &avg, &rate) != 12) return false;
        if (fp < 1 || fp > 4096 || bits != 16 || ch != 2 || rate != 44100) return false;
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
        dec = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!dec) return false;
        uint8_t* x = static_cast<uint8_t*>(av_mallocz(36 + AV_INPUT_BUFFER_PADDING_SIZE));
        auto be32 = [&](int at, unsigned v) { x[at] = uint8_t(v >> 24); x[at + 1] = uint8_t(v >> 16); x[at + 2] = uint8_t(v >> 8); x[at + 3] = uint8_t(v); };
        be32(0, 36);
        memcpy(x + 4, "alac", 4);
        be32(12, fp);
        x[16] = uint8_t(compat); x[17] = uint8_t(bits); x[18] = uint8_t(pb); x[19] = uint8_t(mb); x[20] = uint8_t(kb); x[21] = uint8_t(ch);
        x[22] = uint8_t(run >> 8); x[23] = uint8_t(run);
        be32(24, maxBytes);
        be32(28, avg);
        be32(32, rate);
        dec->extradata = x;
        dec->extradata_size = 36;
        dec->sample_rate = 44100;
        av_channel_layout_default(&dec->ch_layout, 2);
        if (avcodec_open2(dec, codec, nullptr) < 0) { endStream(); return false; }
        return true;
    }

    void endStream() {
        if (dec) avcodec_free_context(&dec);
        if (swr) swr_free(&swr);
        haveAes = haveControl = haveNext = false;
        if (slots) for (int i = 0; i < kSlots; i++) slots[i].have = false;
    }

    void push(const int16_t* p, int frames) {
        unsigned w = a.written_.load(std::memory_order_relaxed);
        const unsigned free = Ring - (w - a.read_.load(std::memory_order_acquire));
        if (unsigned(frames) > free) return;
        for (int i = 0; i < frames; i++, w++) {
            a.ring_[(w & (Ring - 1)) * 2] = p[i * 2];
            a.ring_[(w & (Ring - 1)) * 2 + 1] = p[i * 2 + 1];
        }
        a.written_.store(w, std::memory_order_release);
        const unsigned fill = w - a.read_.load(std::memory_order_acquire);
        if (!a.primed_ && fill >= kPrime) a.primed_ = true;
        if (fill > kTooFull) a.read_.store(w - kPrime, std::memory_order_release);   // (its clock ran ahead of this one)
        measure(p, frames);
    }

    /** A new song or a skip: its loudness from here (the song before's would level this one). */
    void newSong() {
        delete meter;
        meter = new loud::Meter(48000, 2);
        meterFrames = 0;
        std::lock_guard<std::mutex> g(a.m_);
        a.view_.lufs = a.view_.peak = a.view_.measured = 0;
    }

    void measure(const int16_t* p, int frames) {
        if (!meter) newSong();
        for (int done = 0; done < frames;) {
            const int n = std::min(frames - done, 8192);
            for (int i = 0; i < n * 2; i++) meterIn[i] = p[(done * 2) + i] * (1.f / 32768.f);
            meter->push(meterIn, n);
            done += n;
        }
        const long long before = meterFrames;
        meterFrames += frames;
        if (meterFrames / 48000 == before / 48000) return;   // once a second
        const loud::Meter::Result r = meter->result();
        std::lock_guard<std::mutex> g(a.m_);
        a.view_.lufs = std::isfinite(r.lufs) ? r.lufs : 0;
        a.view_.peak = r.truePeak;
        a.view_.measured = std::isfinite(r.lufs) ? double(meterFrames) / 48000 : 0;
    }

    void decode(uint8_t* p, int size) {
        if (!dec || !haveAes || size <= 12) return;
        {   // where this packet's sound will start in the ring, and its RTP time: the audio thread places it on the sender's clock
            const unsigned k = a.mapAt_.load(std::memory_order_relaxed);
            a.mapPos_[k % AirPlay::MapN].store(a.written_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            a.mapRtp_[k % AirPlay::MapN].store(be32(p + 4), std::memory_order_relaxed);
            a.mapAt_.store(k + 1, std::memory_order_release);
        }
        uint8_t* audio = p + 12;
        const size_t n = size_t(size - 12), whole = n & ~size_t(15);
        uint8_t v[16];
        memcpy(v, iv, 16);
        if (whole) mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, whole, v, audio, audio);
        if (av_new_packet(pkt, int(n)) < 0) return;
        memcpy(pkt->data, audio, n);
        const int rc = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (rc < 0) return;
        while (avcodec_receive_frame(dec, frame) == 0) {
            if (!swr) {
                AVChannelLayout stereo;
                av_channel_layout_default(&stereo, 2);
                if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &frame->ch_layout, AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0) {
                    av_frame_unref(frame);
                    return;
                }
            }
            uint8_t* out[1] = { reinterpret_cast<uint8_t*>(pcm) };
            const int got = swr_convert(swr, out, 8192, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (got > 0) push(pcm, got);
            av_frame_unref(frame);
        }
    }

    void silence(int frames) {
        memset(pcm, 0, size_t(std::min(frames, 8192)) * 4);
        push(pcm, std::min(frames, 8192));
    }

    void take(const uint8_t* p, int n, double now, bool retransmit) {
        if (n <= 12 || n > kPacket) return;
        const uint16_t seq = uint16_t(p[2] << 8 | p[3]);
        if (!haveNext) { haveNext = true; nextSeq = topSeq = seq; }
        const int ahead = int16_t(seq - nextSeq);
        if (ahead < 0 || ahead >= kSlots) { if (ahead >= kSlots) { flushTo(seq); } else return; }
        if (int16_t(seq - topSeq) > 0) {
            for (uint16_t s = uint16_t(topSeq + 1); s != seq; s++) { Slot& g = slots[s % kSlots]; g.have = false; g.asked = false; g.waitingSince = now; }
            topSeq = seq;
        }
        Slot& s = slots[seq % kSlots];
        s.have = true;
        s.size = n;
        memcpy(s.data, p, size_t(n));
        if (retransmit) { std::lock_guard<std::mutex> g(a.m_); a.view_.repaired++; }
    }

    void flushTo(uint16_t seq) {
        for (int i = 0; i < kSlots; i++) slots[i].have = false;
        nextSeq = topSeq = seq;
    }

    void drain(double now) {
        if (!haveNext) return;
        while (int16_t(topSeq - nextSeq) >= 0) {
            Slot& s = slots[nextSeq % kSlots];
            if (s.have) {
                decode(s.data, s.size);
                s.have = false;
                nextSeq++;
                continue;
            }
            const double waited = now - s.waitingSince;
            if (waited > kGiveUpAfter) {
                silence(383);
                { std::lock_guard<std::mutex> g(a.m_); a.view_.lost++; }
                nextSeq++;
                continue;
            }
            if (waited > kAskAfter && !s.asked && haveControl) {
                s.asked = true;
                uint8_t ask[8] = { 0x80, 0xD5, 0, 1, uint8_t(nextSeq >> 8), uint8_t(nextSeq), 0, 1 };
                sendto(media[1], reinterpret_cast<const char*>(ask), sizeof ask, 0, reinterpret_cast<sockaddr*>(&control), sizeof control);
            }
            break;
        }
    }

    /** Title, artist and album out of a DMAP block: tag, 4 bytes of length, value. */
    void metadata(const std::string& b) {
        auto tag = [&](const char* t) {
            const size_t at = b.find(t);
            if (at == std::string::npos || at + 8 > b.size()) return std::string();
            const size_t n = size_t(uint8_t(b[at + 4])) << 24 | size_t(uint8_t(b[at + 5])) << 16 | size_t(uint8_t(b[at + 6])) << 8 | uint8_t(b[at + 7]);
            if (n > 256 || at + 8 + n > b.size()) return std::string();
            std::string v = b.substr(at + 8, n);
            for (char& c : v) if (uint8_t(c) < 0x20) c = ' ';
            return v;
        };
        const std::string title = tag("minm"), artist = tag("asar"), album = tag("asal");
        bool other;
        {
            std::lock_guard<std::mutex> g(a.m_);
            other = title != a.view_.title || artist != a.view_.artist;
            a.view_.title = title;
            a.view_.artist = artist;
            a.view_.album = album;
        }
        if (other) newSong();
    }

    void onRequest(const std::string& head, const std::string& body, uint32_t peer, bool fromCand = false) {
        const std::string method = head.substr(0, head.find(' '));
        std::string cseq = lineOf(head, "CSeq:");
        if (cseq.size() > 16) cseq.resize(16);
        std::string reply = "RTSP/1.0 200 OK\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n";
        const std::string challenge = lineOf(head, "Apple-Challenge:");
        if (!challenge.empty()) {
            std::string c = challenge;
            while (!c.empty() && c[0] == ' ') c.erase(0, 1);
            const std::string answer = answerChallenge(c);
            if (!answer.empty()) reply += "Apple-Response: " + answer + "\r\n";
        }
        if (method == "OPTIONS") {
            reply += "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n";
        } else if (method == "ANNOUNCE") {
            std::string name;
            const std::string id = deviceOf(body, peer, name);
            if (known(id)) { takeAnnounce(fromCand, head, body, cseq); return; }
            if (ask.on) {   // one question at a time
                reply = "RTSP/1.0 403 Forbidden\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n";
            } else {
                ask = Ask{ true, fromCand, id, head, body, cseq, peer, seconds() + 20 };
                std::lock_guard<std::mutex> g(a.m_);
                a.view_.asking = true;
                a.view_.askId = id;
                a.view_.askName = name.empty() ? "A device" : name;
                a.answer_ = -1;
                return;   // answered by checkAsk
            }
        } else if (fromCand || !clientAllowed) {
            // not allowed (yet): nothing it asks is done
            reply = "RTSP/1.0 403 Forbidden\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n";
        } else if (method == "SETUP") {
            const size_t p = head.find("control_port=");
            if (p != std::string::npos) {
                const int port = atoi(head.c_str() + p + 13);
                if (port > 0 && port < 65536) {
                    control = sockaddr_in{};
                    control.sin_family = AF_INET;
                    control.sin_port = htons(uint16_t(port));
                    control.sin_addr.s_addr = peer;
                    haveControl = true;
                }
            }
            const size_t tp = head.find("timing_port=");
            if (tp != std::string::npos) {
                const int port = atoi(head.c_str() + tp + 12);
                if (port > 0 && port < 65536) {
                    timingTo = sockaddr_in{};
                    timingTo.sin_family = AF_INET;
                    timingTo.sin_port = htons(uint16_t(port));
                    timingTo.sin_addr.s_addr = peer;
                    haveTimingTo = true;
                    timeAsks = 0;
                    probes.clear();
                    a.haveClock_ = false;
                    a.haveSync_ = false;
                }
            }
            char t[140];
            snprintf(t, sizeof t, "Session: 1\r\nAudio-Jack-Status: connected\r\nTransport: RTP/AVP/UDP;unicast;mode=record;server_port=%d;control_port=%d;timing_port=%d\r\n", kAudio, kControl, kTiming);
            reply += t;
        } else if (method == "RECORD") {
            reply += "Audio-Latency: 11025\r\nSession: 1\r\n";
            a.primed_ = false;
            a.clear_ = true;
            std::lock_guard<std::mutex> g(a.m_);
            a.view_.streaming = true;
        } else if (method == "FLUSH") {
            a.primed_ = false;
            a.clear_ = true;
            haveNext = false;
            for (int i = 0; i < kSlots; i++) slots[i].have = false;
            newSong();   // a skip or a seek: measured again from here
        } else if (method == "TEARDOWN") {
            hangUp();
        } else if (method == "SET_PARAMETER") {
            const std::string type = lineOf(head, "Content-Type:");
            if (type.find("text/parameters") != std::string::npos) {
                const std::string v = lineOf(body, "volume:");
                if (!v.empty()) {
                    const float db = float(atof(v.c_str()));
                    std::lock_guard<std::mutex> g(a.m_);
                    a.view_.gain = db <= -30 ? 0.f : std::pow(10.f, std::clamp(db, -30.f, 0.f) / 20);
                }
                // where the song is: its start, the sample playing and its end, as RTP times at 44.1 kHz (they wrap)
                unsigned s0 = 0, s1 = 0, s2 = 0;
                if (sscanf(lineOf(body, "progress:").c_str(), " %u/%u/%u", &s0, &s1, &s2) == 3) {
                    std::lock_guard<std::mutex> g(a.m_);
                    a.view_.position = double(uint32_t(s1 - s0)) / 44100;
                    a.view_.duration = double(uint32_t(s2 - s0)) / 44100;
                    a.view_.progressGen++;
                }
            } else if (type.find("dmap") != std::string::npos) {
                metadata(body);
            } else if (type.find("image/") != std::string::npos) {
                // its cover: a JPEG or PNG (image/none: none); only by its signature, never trusted for more
                const bool jpeg = body.size() > 3 && uint8_t(body[0]) == 0xFF && uint8_t(body[1]) == 0xD8;
                const bool png = body.size() > 8 && body.compare(1, 3, "PNG") == 0;
                std::lock_guard<std::mutex> g(a.m_);
                a.cover_.assign(body.begin(), jpeg || png ? body.end() : body.begin());
                a.view_.coverGen++;
            }
        } else if (method != "GET_PARAMETER" && method != "PAUSE") {
            reply = "RTSP/1.0 404 Not Found\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n";
        }
        reply += "\r\n";
        sendOn(fromCand, reply);
    }

    /** Whether this device may play: one allowed before plays at once; another waits up to 20 s for the question's answer. */
    /** Who a device is: by the name iOS gives it. Its DACP-ID is new with each connection (the user's iPhone sent two for one name),
     *  so "Always" asked every time. A name can be copied by another phone on this network: the price of no password. */
    static std::string deviceOf(const std::string& body, uint32_t peer, std::string& name) {
        name = lineOf(body, "i=");
        for (char& c : name) if (uint8_t(c) < 0x20) c = ' ';
        name = name.substr(0, 64);
        while (!name.empty() && name[0] == ' ') name.erase(0, 1);
        if (!name.empty()) return "name:" + name;
        char b[24];   // (no name sent: its address, for this network only)
        snprintf(b, sizeof b, "ip:%08x", unsigned(peer));
        return b;
    }

    bool known(const std::string& id) {
        if (id == onceId) return true;
        std::lock_guard<std::mutex> g(a.m_);
        return std::find(a.allowed_.begin(), a.allowed_.end(), id) != a.allowed_.end();
    }

    // ---- connections. One plays (client, allowed once its ANNOUNCE was); while it does, one newcomer may wait beside it (cand),
    // and takes over only when its own ANNOUNCE is allowed: anyone else on the network connecting no longer ends your stream
    // (the user, 2026-11-02, the stranger tests). Nothing but OPTIONS and ANNOUNCE is answered before that: RECORD, SETUP and
    // SET_PARAMETER from a device never allowed get 403, so none can take the player over without the question.
    bool clientAllowed = false;
    Sock cand = -1;
    uint32_t candAddress = 0;
    std::string candIn;
    double candSince = 0;
    // The question, while its ANNOUNCE waits for an answer: the loop runs on meanwhile (the stream playing is not held up), and
    // the answer is sent when it comes, or 403 after 20 s. One at a time: another unknown device meanwhile hears 403.
    struct Ask { bool on = false, fromCand = false; std::string id, head, body, cseq; uint32_t peer = 0; double until = 0; } ask;

    void sendOn(bool fromCand, const std::string& reply) {
        const Sock fd = fromCand ? cand : client;
        if (fd >= 0) send(fd, reply.data(), int(reply.size()), 0);
    }

    void dropCand() {
        if (cand >= 0) closeSock(cand);
        cand = -1;
        candIn.clear();
        if (ask.on && ask.fromCand) endAsk();
    }

    void endAsk() {
        ask.on = false;
        std::lock_guard<std::mutex> g(a.m_);
        a.view_.asking = false;
    }

    /** An allowed ANNOUNCE: a newcomer becomes the one that plays (the old stream ends), then it is taken as before. */
    void takeAnnounce(bool fromCand, const std::string& head, const std::string& body, const std::string& cseq) {
        if (!playable(body)) {   // refused as it is: the stream playing goes on, a newcomer is closed
            sendOn(fromCand, "RTSP/1.0 415 Unsupported Media Type\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n\r\n");
            if (fromCand) dropCand();
            return;
        }
        if (fromCand) {
            if (client >= 0) closeSock(client);
            hangUp();
            client = cand;
            clientAddress = candAddress;
            in = std::move(candIn);
            cand = -1;
            candIn.clear();
        }
        std::string reply = "RTSP/1.0 200 OK\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n";
        clientAllowed = announced(body);
        if (!clientAllowed) reply = "RTSP/1.0 415 Unsupported Media Type\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n";
        else {
            {
                std::lock_guard<std::mutex> g(a.m_);
                a.view_ = View{};
                std::string who = lineOf(body, "i=");
                for (char& c : who) if (uint8_t(c) < 0x20) c = ' ';
                a.view_.peer = who.substr(0, 64);
            }
            newSong();   // a new stream: measured from its start (outside the lock: newSong takes it)
        }
        (void)head;
        sendOn(false, reply + "\r\n");
    }

    /** Every turn of the loop: the question's answer, or its time up. */
    void checkAsk(double now) {
        if (!ask.on) return;
        int answer;
        {
            std::lock_guard<std::mutex> g(a.m_);
            answer = a.answer_;
        }
        if (answer < 0 && now < ask.until) return;
        const Ask was = ask;
        endAsk();
        if (answer == 1) {
            onceId = was.id;
            takeAnnounce(was.fromCand, was.head, was.body, was.cseq);
            onceId = was.id;   // (taking over hung the old stream up, which forgets the once)
        } else {
            sendOn(was.fromCand, "RTSP/1.0 403 Forbidden\r\nServer: AirTunes/105.1\r\nCSeq:" + was.cseq + "\r\n\r\n");
            if (was.fromCand) dropCand();
        }
    }

    void hangUp() {
        endStream();
        onceId.clear();
        clientAllowed = false;
        a.primed_ = false;
        a.clear_ = true;
        std::lock_guard<std::mutex> g(a.m_);
        a.view_.streaming = false;
    }

    void closeClient() {
        if (client >= 0) closeSock(client);
        client = -1;
        in.clear();
        if (ask.on && !ask.fromCand) endAsk();
        hangUp();
    }

    /** Requests off one connection: the one that plays, or the newcomer beside it. */
    void onClient(bool fromCand) {
        const Sock fd = fromCand ? cand : client;
        std::string& buffer = fromCand ? candIn : in;
        const uint32_t peer = fromCand ? candAddress : clientAddress;
        auto drop = [&] { if (fromCand) dropCand(); else closeClient(); };
        char buf[4096];
        const int got = int(recv(fd, buf, sizeof buf, 0));
        if (got == 0 || (got < 0 && !wouldBlock())) { drop(); return; }
        if (got < 0) return;
        buffer.append(buf, size_t(got));
        if (buffer.size() > kMaxRequest) { drop(); return; }
        for (;;) {
            const size_t end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) return;
            const std::string head = buffer.substr(0, end + 2);
            const size_t cl = head.find("Content-Length:");
            const size_t size = cl == std::string::npos ? 0 : size_t(std::max(0, atoi(head.c_str() + cl + 15)));
            if (size > kMaxRequest) { drop(); return; }
            if (buffer.size() < end + 4 + size) return;
            const std::string body = buffer.substr(end + 4, size);
            buffer.erase(0, end + 4 + size);
            onRequest(head, body, peer, fromCand);
            // (a newcomer taken over: its rest is the one that plays' now; either connection may have closed)
            if ((fromCand ? cand : client) < 0) return;
        }
    }

    uint32_t clientAddress = 0;

    void loop() {
        double announceAt = 0, netAt = 0, stale = 0;
        int told = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> g(a.m_);
                if (a.quit_ || !a.wanted_) break;
            }
            if (a.stopNow_.exchange(false, std::memory_order_acq_rel) && client >= 0) {   // the sheet's Stop (the user, 2026-11-05: "it is
                closeClient();                                                         // not stopping with this button")
                dropCand();
            }
            const double t = seconds();
            if (mdns >= 0 && t >= announceAt) {   // (on the PC, Windows announces it)
                sayAnnounce(announce);
                announceAt = t + (told++ < 6 ? 1 : 30);
            }
            if (t >= netAt) {
                netAt = t + 2;
                const std::vector<plat::Net> nets = plat::networks();
#ifdef _WIN32
                // a PC has several networks: the one an iPhone reached is gone, or none is left
                if (nets.empty() || std::none_of(nets.begin(), nets.end(), [&](const plat::Net& w) { return w.address == ip; })) { hangUp(); return; }
#else
                if (nets.empty() || nets[0].address != ip) { hangUp(); return; }
#endif
            }
            fd_set rd;
            FD_ZERO(&rd);
            Sock top = 0;
            for (Sock s : { mdns, listener, client, cand, media[0], media[1], media[2] }) if (s >= 0) { FD_SET(s, &rd); top = std::max(top, s); }
            timeval tv{ 0, 10 * 1000 };
            if (select(int(top) + 1, &rd, nullptr, nullptr, &tv) < 0) { hangUp(); return; }   // (Windows ignores the count)
            const double now = seconds();
            if (mdns >= 0 && FD_ISSET(mdns, &rd)) onMdns();
            if (FD_ISSET(listener, &rd)) {
                sockaddr_in from{};
                socklen_t len = sizeof from;
                const Sock c = Sock(accept(listener, reinterpret_cast<sockaddr*>(&from), &len));
                if (c >= 0 && ask.on) {
                    closeSock(c);   // a question is up: one at a time, and a new connection must not end it (nor its asker)
                } else if (c >= 0) {
                    nonBlocking(c);
#ifdef _WIN32
                    // the challenge is answered with the address it reached: on a PC with several networks, not the first one's
                    sockaddr_in mine{};
                    socklen_t ml = sizeof mine;
                    if (getsockname(SOCKET(c), reinterpret_cast<sockaddr*>(&mine), &ml) == 0) ip = mine.sin_addr.s_addr;
#endif
                    if (client >= 0 && clientAllowed) {   // a stream is allowed: the newcomer waits beside it
                        dropCand();
                        cand = c;
                        candAddress = from.sin_addr.s_addr;
                        candSince = now;
                    } else {
                        closeClient();
                        client = c;
                        clientAddress = from.sin_addr.s_addr;
                    }
                }
            }
            if (client >= 0 && FD_ISSET(client, &rd)) onClient(false);
            if (cand >= 0 && FD_ISSET(cand, &rd)) onClient(true);
            checkAsk(now);
            if (cand >= 0 && !(ask.on && ask.fromCand) && now - candSince > 15) dropCand();   // a newcomer that never asks to play
            uint8_t p[kPacket + 4];
            sockaddr_in src{};
            socklen_t sl = sizeof src;
            int n;
            while (media[0] >= 0 && (n = int(recvfrom(media[0], reinterpret_cast<char*>(p), sizeof p, 0, reinterpret_cast<sockaddr*>(&src), &sl))) > 0)
                if (src.sin_addr.s_addr == clientAddress && (p[1] & 0x7f) == 0x60) take(p, n, now, false);
            while (media[1] >= 0 && (n = int(recvfrom(media[1], reinterpret_cast<char*>(p), sizeof p, 0, reinterpret_cast<sockaddr*>(&src), &sl))) > 0) {
                if (src.sin_addr.s_addr != clientAddress) continue;
                if ((p[1] & 0x7f) == 0x56 && n > 4 + 12) take(p + 4, n - 4, now, true);
                else if ((p[1] & 0x7f) == 0x54 && n >= 20) {   // sync: RTP time p+4 is heard at the sender's time p+8
                    a.syncRtp_.store(be32(p + 4), std::memory_order_relaxed);
                    a.syncNtp_.store(ntp64(p + 8), std::memory_order_relaxed);
                    a.haveSync_.store(true, std::memory_order_release);
                }
            }
            if (haveTimingTo && client >= 0 && (timeAsks < 5 ? now - lastTimeAsk > 0.3 : now - lastTimeAsk > 3)) {
                // what time is it there: our time sent, theirs as it came and as they answered (both in the answer)
                uint8_t q[32] = {};
                q[0] = 0x80;
                q[1] = 0xD2;
                q[3] = 7;
                const double t0 = ntpLocal();
                const uint32_t s0 = uint32_t(t0), f0 = uint32_t((t0 - std::floor(t0)) * 4294967296.0);
                for (int k = 0; k < 4; k++) { q[24 + k] = uint8_t(s0 >> (24 - 8 * k)); q[28 + k] = uint8_t(f0 >> (24 - 8 * k)); }
                sendto(media[2], reinterpret_cast<const char*>(q), sizeof q, 0, reinterpret_cast<sockaddr*>(&timingTo), sizeof timingTo);
                lastTimeAsk = now;
                timeAsks++;
            }
            while (media[2] >= 0 && (n = int(recvfrom(media[2], reinterpret_cast<char*>(p), sizeof p, 0, reinterpret_cast<sockaddr*>(&src), &sl))) > 0) {
                if ((p[1] & 0x7f) == 0x53 && n >= 32 && src.sin_addr.s_addr == clientAddress) {
                    // an answer: the offset from the quickest of the last eight (the least delayed on the way)
                    const double t3 = ntpLocal(), t0 = ntp64(p + 8), t1 = ntp64(p + 16), t2 = ntp64(p + 24);
                    const double rtt = (t3 - t0) - (t2 - t1), off = ((t1 - t0) + (t2 - t3)) / 2;
                    if (t0 > 0 && rtt >= 0 && rtt < 1) {
                        probes.push_back({ off, rtt });
                        if (probes.size() > 8) probes.erase(probes.begin());
                        const Probe best = *std::min_element(probes.begin(), probes.end(), [](const Probe& x, const Probe& y) { return x.rtt < y.rtt; });
                        a.clockOffset_.store(best.offset, std::memory_order_relaxed);
                        a.haveClock_.store(true, std::memory_order_release);
                    }
                    continue;
                }
                if ((p[1] & 0x7f) != 0x52 || n < 32 || src.sin_addr.s_addr != clientAddress) continue;
                uint8_t out[32];
                memcpy(out, p, 8);
                out[1] = 0xD3;
                memcpy(out + 8, p + 24, 8);
                const uint64_t us = uint64_t(now * 1e6);
                for (int k = 0; k < 8; k++) out[16 + k] = out[24 + k] = uint8_t(us >> (56 - 8 * k));
                sendto(media[2], reinterpret_cast<const char*>(out), sizeof out, 0, reinterpret_cast<sockaddr*>(&src), sl);
            }
            drain(now);
            (void)stale;
        }
        if (mdns >= 0 && !announce.empty()) sayAnnounce(buildAnnounce(0));   // goodbye: iOS lists it no longer
        unadvertise();   // (the PC's: through Windows)
    }
};

void AirPlay::start(const std::string& deviceName) {
    name_ = deviceName;
    ring_ = new int16_t[size_t(Ring) * 2];
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void AirPlay::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
        wanted_ = false;
    }
    wake_.notify_all();
    answered_.notify_all();   // a question waiting ends with Songate
    if (thread_.joinable()) thread_.join();
    delete[] ring_;
    ring_ = nullptr;
}

void AirPlay::allow(std::vector<std::string> ids) {
    std::lock_guard<std::mutex> g(m_);
    allowed_ = std::move(ids);
}

std::vector<uint8_t> AirPlay::takeCover() {
    std::lock_guard<std::mutex> g(m_);
    return std::move(cover_);
}

void AirPlay::answer(bool yes) {
    {
        std::lock_guard<std::mutex> g(m_);
        if (!view_.asking || answer_ >= 0) return;   // the first answer counts: a Cancel seen after Allow changes nothing
        answer_ = yes ? 1 : 0;
    }
    answered_.notify_all();
}

void AirPlay::want(bool on) {
    std::lock_guard<std::mutex> g(m_);
    if (wanted_ == on) return;
    wanted_ = on;
    wake_.notify_all();
}

AirPlay::View AirPlay::view() {
    std::lock_guard<std::mutex> g(m_);
    return view_;
}

int AirPlay::read(int16_t* out, int frames) {
    // Asked again after a gap: Songate was suspended (HOME). What waited from before is old: it is dropped, and the sound starts
    // again with what the iPhone sends now (the user, 2026-11-02: "it plays the last piece of the music before the home action and
    // then syncs time with the iphone"). The hook asks every 20 to 40 ms while it runs
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool away = lastRead_ > 0 && now - lastRead_ > 0.5;
    lastRead_ = now;
    if (away) { clear_ = false; primed_ = false; read_.store(written_.load(std::memory_order_acquire), std::memory_order_release); return 0; }
    if (clear_.exchange(false)) read_.store(written_.load(std::memory_order_acquire), std::memory_order_release);
    if (haveClock_.load(std::memory_order_acquire) && haveSync_.load(std::memory_order_acquire)) {
        // Each sample when the sender's clock says: the RTP time at the ring's head (from the packet it came in), against the one
        // that should be heard as this buffer comes out (about 45 ms from now, plus how far ahead this device plays)
        unsigned r = read_.load(std::memory_order_relaxed);
        const unsigned w = written_.load(std::memory_order_acquire);
        const unsigned mapAt = mapAt_.load(std::memory_order_acquire);
        bool found = false;
        unsigned bestPos = 0;
        uint32_t bestRtp = 0;
        for (int k = 0; k < MapN && k < int(mapAt); k++) {
            const unsigned i = (mapAt - 1 - unsigned(k)) % MapN;
            const unsigned pos = mapPos_[i].load(std::memory_order_relaxed);
            if (int(r - pos) >= 0 && int(r - pos) < 48000 * 4) { bestPos = pos; bestRtp = mapRtp_[i].load(std::memory_order_relaxed); found = true; break; }
        }
        if (found && w != r) {
            const double heard = ntpLocal() + 0.045 - ahead_.load(std::memory_order_relaxed) + clockOffset_.load(std::memory_order_relaxed);
            const double want = double(syncRtp_.load(std::memory_order_relaxed)) + (heard - syncNtp_.load(std::memory_order_relaxed)) * 44100.0;
            const uint32_t head = bestRtp + uint32_t(double(r - bestPos) * 44100.0 / 48000.0);
            const double err = double(int32_t(head - uint32_t(int64_t(want)))) / 44100.0;   // + the head is early: wait; - late: skip
            if (std::fabs(err) < 4) {
                if (err > 0.06) return 0;   // not yet: silence, nothing taken
                if (err < -0.06) {          // late: what should have been heard is skipped
                    const unsigned skip = std::min(w - r, unsigned(-err * 48000));
                    r += skip;
                    read_.store(r, std::memory_order_release);
                }
                const unsigned avail = w - r;
                if (!avail) return 0;
                int n = int(std::min<unsigned>(avail, unsigned(frames)));
                // a few milliseconds off: a frame dropped or said twice in this buffer, unheard
                int nudge = err < -0.003 ? 1 : err > 0.003 ? -1 : 0;
                int o = 0;
                for (int i = 0; i < n && o < frames; i++, r++) {
                    if (nudge == 1 && i == n / 2) { nudge = 0; continue; }   // one dropped
                    out[o * 2] = ring_[(r & (Ring - 1)) * 2];
                    out[o * 2 + 1] = ring_[(r & (Ring - 1)) * 2 + 1];
                    o++;
                    if (nudge == -1 && i == n / 2 && o < frames) { out[o * 2] = out[(o - 1) * 2]; out[o * 2 + 1] = out[(o - 1) * 2 + 1]; o++; nudge = 0; }
                }
                read_.store(r, std::memory_order_release);
                return o;
            }
        }
        // (no clock to place it on yet, or a wild one: the old way below)
    }
    if (!primed_) return 0;
    unsigned r = read_.load(std::memory_order_relaxed);
    const unsigned avail = written_.load(std::memory_order_acquire) - r;
    if (!avail) { primed_ = false; return 0; }   // ran dry: waits to fill again
    const int n = int(std::min<unsigned>(avail, unsigned(frames)));
    for (int i = 0; i < n; i++, r++) {
        out[i * 2] = ring_[(r & (Ring - 1)) * 2];
        out[i * 2 + 1] = ring_[(r & (Ring - 1)) * 2 + 1];
    }
    read_.store(r, std::memory_order_release);
    return n;
}

void AirPlay::run() {
    cores::moveHere(cores::Net);
    std::unique_lock<std::mutex> g(m_);
    while (!quit_) {
        wake_.wait(g, [&] { return quit_ || wanted_; });
        if (quit_) break;
        g.unlock();
        std::string e;
        if (net::begin(e)) {
            Impl im(*this);
            if (im.open()) im.loop();
            else { std::lock_guard<std::mutex> v(m_); view_.error = "AirPlay could not open on this network"; }
        }
        // the network came back, or it was off: looked at again in 2 s; closing does not wait for them (it waited the whole
        // 2 s on every exit: shutdown-trace.txt, 2026-11-05)
        g.lock();
        wake_.wait_for(g, std::chrono::seconds(2), [&] { return quit_; });
    }
}

#endif  // GF_AIRPLAY
