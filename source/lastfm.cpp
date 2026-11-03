/*
 * Songate · lastfm.cpp
 * See lastfm.hpp. Last.fm's errors arrive as XML, with HTTP 200 or 403: net::form hands back the body whatever the status.
 * Retried: 11 and 16, which ask for it, 29 (too many requests), and no answer at all. 9 asks for a new sign-in and the queue
 * stays. Anything else is dropped and logged.
 */
#include "lastfm.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#include "cores.hpp"
#include "md5.hpp"
#include "words.hpp"

#if __has_include("lastfm_key.inc")
#include "lastfm_key.inc"
#define LASTFM_HAS_KEY 1
#else
#define LASTFM_HAS_KEY 0
#endif

namespace {

constexpr int kBatch = 50;
constexpr size_t kQueueMax = 5000;
constexpr int64_t kClockFloor = 1577836800;   // 2020: a clock before this has never been set
constexpr int64_t kClockSlack = 120;          // under two minutes off: left alone
constexpr double kPoll = 5;                   // seconds between asking whether the page was allowed
constexpr double kBackoffMax = 1800;

double steady() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#if LASTFM_HAS_KEY
/** The key, then the secret, put back together into out (size at least kLen + 1). */
void unscramble(char* out) {
    using namespace lastfm_key;
    uint32_t x = kSeed[0] ^ kSeed[1] ^ kSeed[2] ^ kSeed[3] ^ 0xC2B2AE35u;
    for (size_t i = 0; i < kLen; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = char(kData[kPlace[i]] ^ uint8_t(x >> 8));
    }
    out[kLen] = 0;
}
#endif

void wipe(std::string& s) {
    volatile char* d = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); i++) d[i] = 0;
    s.clear();
}

[[maybe_unused]] std::string encode(const std::string& s) {   // (a build without the key sends nothing)
    static const char* const hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string unescapeXml(std::string s) {
    static const std::pair<const char*, const char*> kNames[] = { { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" }, { "&#39;", "'" }, { "&amp;", "&" } };
    for (const auto& n : kNames)
        for (size_t p = s.find(n.first); p != std::string::npos; p = s.find(n.first, p + 1)) s.replace(p, strlen(n.first), n.second);
    return s;
}

/** The text of the first <name>…</name>, "" if none. */
std::string element(const std::string& xml, const std::string& name) {
    const size_t a = xml.find("<" + name + ">");
    if (a == std::string::npos) return {};
    const size_t b = a + name.size() + 2, e = xml.find("</" + name + ">", b);
    return e == std::string::npos ? std::string() : unescapeXml(xml.substr(b, e - b));
}

/** "Sat, 31 Oct 2026 12:00:00 GMT" in seconds since 1970, 0 if it is not that. */
[[maybe_unused]] int64_t httpDate(const std::string& s) {
    int day = 0, year = 0, h = 0, m = 0, sec = 0;
    char mon[4] = {};
    const size_t comma = s.find(',');
    if (comma == std::string::npos || sscanf(s.c_str() + comma + 1, " %d %3s %d %d:%d:%d", &day, mon, &year, &h, &m, &sec) != 6) return 0;
    static const char* const kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* f = strstr(kMonths, mon);
    if (!f || (f - kMonths) % 3 != 0) return 0;
    int mo = int(f - kMonths) / 3 + 1;
    // days from civil (Howard Hinnant's), so no time zone comes into it
    int y = year - (mo <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * unsigned(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + unsigned(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = int64_t(era) * 146097 + int64_t(doe) - 719468;
    return days * 86400 + h * 3600 + m * 60 + sec;
}

std::string readAll(const std::string& p) {
    std::string out;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return out;
    char b[16384];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    fclose(f);
    return out;
}

bool writeAll(const std::string& p, const std::string& data) {
    const std::string tmp = p + ".tmp";   // next to it and renamed over it: a power cut never loses the old one
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return false; }
    remove(p.c_str());
    return rename(tmp.c_str(), p.c_str()) == 0;
}

std::string field(std::string s) {
    for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> c;
    size_t a = 0;
    for (;;) {
        const size_t b = line.find('\t', a);
        c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return c;
        a = b + 1;
    }
}

template <typename F>
void eachLine(const std::string& text, F f) {
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        size_t e = b;
        if (e > a && text[e - 1] == '\r') e--;
        if (e > a) f(text.substr(a, e - a));
        a = b + 1;
    }
}

}  // namespace

bool Lastfm::available() { return LASTFM_HAS_KEY != 0; }

void Lastfm::start(const std::string& dir) {
    dir_ = dir;
    quit_ = false;
    prog_.cancel = false;
    thread_ = std::thread([this] { run(); });
}

void Lastfm::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    prog_.cancel = true;   // a request under way ends now
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Lastfm::signIn() {
    std::lock_guard<std::mutex> g(m_);
    if (sign_ == Sign::Asking || sign_ == Sign::Waiting) return;
    sign_ = Sign::Asking;
    signErr_.clear();
    wake_.notify_all();
}

void Lastfm::cancelSignIn() {
    std::lock_guard<std::mutex> g(m_);
    sign_ = Sign::None;
    waitingSignIn_ = false;
    wipe(token_);
    url_.clear();
}

void Lastfm::signOut() {
    std::lock_guard<std::mutex> g(m_);
    signOut_ = true;
    wake_.notify_all();
}

void Lastfm::nowPlaying(Song s) {
    std::lock_guard<std::mutex> g(m_);
    now_ = std::move(s);
    hasNow_ = true;
    wake_.notify_all();
}

void Lastfm::scrobble(Song s) {
    std::lock_guard<std::mutex> g(m_);
    // One play, counted on both sides of a handover between Songate and the background player (each starts the track's time
    // again): the second one began within the first one's length. Repeat track begins a whole length later.
    if (s.artist == lastArtist_ && s.title == lastTitle_ && llabs(s.when - lastWhen_) + 1 < std::max(lastDuration_, s.duration)) return;
    lastArtist_ = s.artist;
    lastTitle_ = s.title;
    lastWhen_ = s.when;
    lastDuration_ = s.duration;
    queue_.push_back(std::move(s));
    queueDirty_ = true;
    wake_.notify_all();
}

Lastfm::View Lastfm::view() {
    std::lock_guard<std::mutex> g(m_);
    View v;
    v.signedIn = signedIn_;
    v.lapsed = lapsed_;
    v.user = user_;
    v.url = url_;
    v.error = signErr_;
    v.status = status_;
    v.sign = sign_;
    v.waiting = int(queue_.size());
    v.expires = signUntil_;
    return v;
}

std::vector<std::pair<std::string, int64_t>> Lastfm::takeBackground() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<std::pair<std::string, int64_t>> out;
    out.swap(bgFound_);
    return out;
}

std::vector<std::string> Lastfm::takeLog() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

void Lastfm::note(const std::string& s) { log_.push_back("Last.fm: " + s); }

void Lastfm::fail(const std::string& why) {
    status_ = why;
    backoff_ = backoff_ <= 0 ? 60 : std::min(kBackoffMax, backoff_ * 2);
    holdUntil_ = steady() + backoff_;
}

void Lastfm::trim() {
    if (queue_.size() <= kQueueMax) return;
    const size_t over = queue_.size() - kQueueMax;
    queue_.erase(queue_.begin(), queue_.begin() + std::ptrdiff_t(over));
    queueDirty_ = true;
    note(std::to_string(over) + " of the oldest waiting were dropped: at most 5,000 are kept");
}

void Lastfm::loadFiles() {
    std::vector<std::string> session;
    eachLine(readAll(dir_ + "/lastfm-session.txt"), [&](const std::string& l) { session.push_back(l); });
    std::deque<Song> file;
    // when, duration, track number, artist, title, album, album artist
    eachLine(readAll(dir_ + "/scrobbles.tsv"), [&](const std::string& l) {
        const auto c = split(l);
        if (c.size() < 7 || c[3].empty() || c[4].empty()) return;
        Song s;
        s.when = atoll(c[0].c_str());
        s.duration = atoi(c[1].c_str());
        s.trackNo = atoi(c[2].c_str());
        s.artist = c[3];
        s.title = c[4];
        s.album = c[5];
        s.albumArtist = c[6];
        file.push_back(std::move(s));
    });
    std::lock_guard<std::mutex> g(m_);
    if (session.size() == 2) {
        user_ = session[0];
        key_ = session[1];
        signedIn_ = true;
    }
    queue_.insert(queue_.begin(), file.begin(), file.end());   // before anything queued while it was read
}

void Lastfm::saveQueue() {
    std::string out;
    {
        std::lock_guard<std::mutex> g(m_);
        if (!queueDirty_) return;
        queueDirty_ = false;
        for (const Song& s : queue_)
            out += std::to_string(s.when) + "\t" + std::to_string(s.duration) + "\t" + std::to_string(s.trackNo) + "\t" + field(s.artist) + "\t" +
                   field(s.title) + "\t" + field(s.album) + "\t" + field(s.albumArtist) + "\n";
    }
    const std::string p = dir_ + "/scrobbles.tsv";
    if (out.empty()) remove(p.c_str());
    else writeAll(p, out);
}

void Lastfm::saveSession() {
    std::string out;
    {
        std::lock_guard<std::mutex> g(m_);
        if (!key_.empty()) out = user_ + "\n" + key_ + "\n";
    }
    const std::string p = dir_ + "/lastfm-session.txt";
    if (out.empty()) remove(p.c_str());
    else writeAll(p, out);
    wipe(out);
}

/** "time<TAB>path" lines from the background player. Renamed first, as Stats does: what it writes meanwhile starts a new file. */
void Lastfm::readBackground() {
    const std::string file = dir_ + "/bg-scrobbles.txt", taken = dir_ + "/bg-scrobbles.take";
    remove(taken.c_str());
    if (rename(file.c_str(), taken.c_str()) != 0) return;
    std::vector<std::pair<std::string, int64_t>> got;
    eachLine(readAll(taken), [&](const std::string& l) {
        const size_t t = l.find('\t');
        if (t != std::string::npos && t + 1 < l.size()) got.emplace_back(l.substr(t + 1), atoll(l.c_str()));
    });
    remove(taken.c_str());
    std::lock_guard<std::mutex> g(m_);
    bgFound_.insert(bgFound_.end(), got.begin(), got.end());
}

Lastfm::Answer Lastfm::call(Params p, bool sign) {
    Answer a;
#if LASTFM_HAS_KEY
    using namespace lastfm_key;
    char k[kLen + 1];
    unscramble(k);
    p["api_key"] = std::string(k, kKeyLen);
    if (sign) {
        std::string s;
        for (const auto& kv : p) { s += kv.first; s += kv.second; }   // (the map: sorted by name; values as UTF-8, unescaped)
        s.append(k + kKeyLen, kLen - kKeyLen);
        p["api_sig"] = md5::hex(s);
        wipe(s);
    }
    { volatile char* d = k; for (size_t i = 0; i <= kLen; i++) d[i] = 0; }
    std::string body;
    for (const auto& kv : p) { if (!body.empty()) body += '&'; body += encode(kv.first) + "=" + encode(kv.second); }
    for (auto& kv : p) wipe(kv.second);
    const char* url = "https://ws.audioscrobbler.com/2.0/";
#ifdef GF_DEV
    if (const char* over = getenv("GF_LASTFM")) url = over;   // a stand-in for Last.fm, for tests
#endif
    long status = 0;
    std::string date;
    const bool got = net::form(url, body, a.body, status, date, a.err, &prog_);
    wipe(body);
    if (!got) return a;
    if (const int64_t server = httpDate(date)) {
        std::lock_guard<std::mutex> g(m_);
        clockOff_ = server - int64_t(std::time(nullptr));
        clockSeen_ = true;
    }
    if (a.body.find("status=\"ok\"") != std::string::npos) { a.reached = true; return a; }
    const size_t e = a.body.find("<error code=\"");
    if (e != std::string::npos) {
        a.reached = true;
        a.code = atoi(a.body.c_str() + e + 13);
        return a;
    }
    a.err = "Last.fm answered " + std::to_string(status);
#else
    (void)p;
    (void)sign;
    a.err = "This build cannot reach Last.fm";
#endif
    return a;
}

void Lastfm::signStep() {
    std::unique_lock<std::mutex> g(m_);
    if (sign_ == Sign::Asking) {
        g.unlock();
        Answer a = call({ { "method", "auth.getToken" } }, true);
        g.lock();
        if (sign_ != Sign::Asking) return;   // cancelled meanwhile
        const std::string token = a.reached && !a.code ? element(a.body, "token") : std::string();
        if (token.empty()) {
            sign_ = Sign::Failed;
            signErr_ = a.reached ? "Last.fm did not start a sign-in (error " + std::to_string(a.code) + ")" : a.err;
            return;
        }
        token_ = token;
#if LASTFM_HAS_KEY
        char k[lastfm_key::kLen + 1];
        unscramble(k);
        url_ = "https://www.last.fm/api/auth/?api_key=" + std::string(k, lastfm_key::kKeyLen) + "&token=" + token;
        { volatile char* d = k; for (size_t i = 0; i <= lastfm_key::kLen; i++) d[i] = 0; }
#endif
        sign_ = Sign::Waiting;
        waitingSignIn_ = true;
        signUntil_ = int64_t(std::time(nullptr)) + 3600;   // a token lasts an hour
        pollAt_ = steady() + kPoll;
        return;
    }
    if (sign_ != Sign::Waiting) return;
    if (int64_t(std::time(nullptr)) > signUntil_) {
        sign_ = Sign::Failed;
        waitingSignIn_ = false;
        signErr_ = "The page ran out of time: sign in again";
        return;
    }
    if (steady() < pollAt_) return;
    pollAt_ = steady() + kPoll;
    const std::string token = token_;
    g.unlock();
    Answer a = call({ { "method", "auth.getSession" }, { "token", token } }, true);
    g.lock();
    if (sign_ != Sign::Waiting) return;   // cancelled meanwhile
    if (a.reached && !a.code) {
        const std::string name = element(a.body, "name"), key = element(a.body, "key");
        if (name.empty() || key.empty()) {
            sign_ = Sign::Failed;
            waitingSignIn_ = false;
            signErr_ = "Last.fm's answer could not be read";
            return;
        }
        user_ = name;
        key_ = key;
        signedIn_ = true;
        lapsed_ = false;
        sign_ = Sign::None;
        waitingSignIn_ = false;
        wipe(token_);
        url_.clear();
        status_.clear();
        backoff_ = holdUntil_ = 0;
        g.unlock();
        saveSession();
        return;
    }
    // 14: not allowed yet, asked again. No answer: the network came and went, asked again
    if (a.reached && a.code != 14) {
        sign_ = Sign::Failed;
        waitingSignIn_ = false;
        signErr_ = a.code == 15 || a.code == 4 ? std::string("The page ran out of time: sign in again") : "Last.fm refused the sign-in (error " + std::to_string(a.code) + ")";
    }
}

/** Never retried: a track that started a while ago is no use late. */
bool Lastfm::sendNow(const Song& s) {
    std::string key;
    {
        std::lock_guard<std::mutex> g(m_);
        key = key_;
    }
    Params p = { { "method", "track.updateNowPlaying" }, { "sk", key }, { "artist", s.artist }, { "track", s.title } };
    wipe(key);
    if (!s.album.empty()) p["album"] = s.album;
    if (!s.albumArtist.empty()) p["albumArtist"] = s.albumArtist;
    if (s.trackNo > 0) p["trackNumber"] = std::to_string(s.trackNo);
    if (s.duration > 0) p["duration"] = std::to_string(s.duration);
    Answer a = call(std::move(p), true);
    if (a.reached && a.code == 9) {
        std::lock_guard<std::mutex> g(m_);
        wipe(key_);
        signedIn_ = false;
        lapsed_ = true;
        return false;
    }
    return a.reached && !a.code;
}

/** Last.fm's time, seen on some answer. An unsigned request that changes nothing, if none came yet. */
bool Lastfm::clockKnown() {
    std::string user;
    {
        std::lock_guard<std::mutex> g(m_);
        if (clockSeen_) return true;
        user = user_;
    }
    Answer a = call({ { "method", "user.getInfo" }, { "user", user } }, false);
    std::lock_guard<std::mutex> g(m_);
    if (!a.reached && !clockSeen_) fail(a.err.empty() ? std::string("No internet connection") : a.err);
    return clockSeen_;
}

void Lastfm::sendBatch() {
    // A clock that has never been set dates everything wrong: nothing goes until it is right (the status says so).
    if (int64_t(std::time(nullptr)) < kClockFloor) {
        std::lock_guard<std::mutex> g(m_);
        status_ = "Waiting: " GF_THIS_CONSOLE "'s clock is not set";
        return;
    }
    if (!clockKnown()) return;
    std::vector<Song> batch;
    std::string key;
    int64_t off = 0;
    {
        std::lock_guard<std::mutex> g(m_);
        for (size_t i = 0; i < queue_.size() && int(batch.size()) < kBatch; i++) batch.push_back(queue_[i]);
        key = key_;
        off = clockOff_;
    }
    if (batch.empty()) return;
    const int64_t serverNow = int64_t(std::time(nullptr)) + off;
    Params p = { { "method", "track.scrobble" }, { "sk", key } };
    wipe(key);
    for (size_t i = 0; i < batch.size(); i++) {
        const Song& s = batch[i];
        const std::string n = "[" + std::to_string(i) + "]";
        // over two minutes off: dated by Last.fm's clock, and never in its future
        const int64_t when = std::abs(off) > kClockSlack ? std::min(s.when + off, serverNow) : s.when;
        p["artist" + n] = s.artist;
        p["track" + n] = s.title;
        p["timestamp" + n] = std::to_string(when);
        if (!s.album.empty()) p["album" + n] = s.album;
        if (!s.albumArtist.empty()) p["albumArtist" + n] = s.albumArtist;
        if (s.trackNo > 0) p["trackNumber" + n] = std::to_string(s.trackNo);
        if (s.duration > 0) p["duration" + n] = std::to_string(s.duration);
    }
    Answer a = call(std::move(p), true);

    std::lock_guard<std::mutex> g(m_);
    if (!a.reached) {
        if (!prog_.cancel) fail(a.err);
        return;
    }
    if (a.code == 9) {
        wipe(key_);
        signedIn_ = false;
        lapsed_ = true;
        status_ = "Last.fm asks you to sign in again";
        return;
    }
    if (a.code == 11 || a.code == 16 || a.code == 29) {
        fail(a.code == 29 ? "Last.fm asks to wait a while" : "Last.fm is not available right now");
        return;
    }
    if (a.code) {
        queue_.erase(queue_.begin(), queue_.begin() + std::ptrdiff_t(batch.size()));
        queueDirty_ = true;
        note(std::to_string(batch.size()) + " dropped: error " + std::to_string(a.code));
        status_ = "Last.fm refused some (error " + std::to_string(a.code) + ")";
        return;
    }
    backoff_ = holdUntil_ = 0;
    // <scrobble> by <scrobble>, in the order sent; each that was ignored says why
    std::vector<int> codes;
    for (size_t at = a.body.find("<scrobble>"); at != std::string::npos; at = a.body.find("<scrobble>", at + 1)) {
        const size_t end = a.body.find("</scrobble>", at);
        const size_t ig = a.body.find("<ignoredMessage", at);
        int code = 0;
        if (ig != std::string::npos && (end == std::string::npos || ig < end)) {
            const size_t c = a.body.find("code=\"", ig);
            if (c != std::string::npos) code = atoi(a.body.c_str() + c + 6);
        }
        codes.push_back(code);
    }
    std::deque<Song> again;
    int old = 0, other = 0;
    for (size_t i = 0; i < codes.size() && i < batch.size(); i++) {
        if (codes[i] == 5) again.push_back(batch[i]);   // the daily limit: not refused, only too many today
        else if (codes[i] == 3) old++;
        else if (codes[i]) other++;
    }
    queue_.erase(queue_.begin(), queue_.begin() + std::ptrdiff_t(batch.size()));
    queue_.insert(queue_.begin(), again.begin(), again.end());
    queueDirty_ = true;
    if (old) note(std::to_string(old) + " dropped: too old for Last.fm");
    if (other) note(std::to_string(other) + " ignored by Last.fm");
    if (!again.empty()) {
        dayHold_ = (serverNow / 86400 + 1) * 86400 + 60;   // just after its next day begins
        status_ = "Waiting: Last.fm's limit for today was reached";
        return;
    }
    const time_t t = std::time(nullptr);
    char b[32];
    strftime(b, sizeof b, "Last sent at %H:%M", localtime(&t));
    status_ = b;
}

void Lastfm::run() {
    cores::moveHere(cores::Net);
    loadFiles();
    std::unique_lock<std::mutex> g(m_);
    while (!quit_) {
        if (signOut_) {
            signOut_ = false;
            wipe(key_);
            user_.clear();
            signedIn_ = false;
            lapsed_ = false;
            status_.clear();
            g.unlock();
            saveSession();
            g.lock();
            continue;
        }
        const double t = steady();
        // The background player's: at launch, then every few seconds while it plays
        if (bgFirst_ || (background_ && t >= bgAt_)) {
            bgFirst_ = false;
            bgAt_ = t + 5;
            g.unlock();
            readBackground();
            g.lock();
        }
        if (on_ && !quit_ && (sign_ == Sign::Asking || sign_ == Sign::Waiting)) {
            g.unlock();
            signStep();
            g.lock();
        }
        bool worked = false;
        if (on_ && signedIn_ && !quit_) {
            if (hasNow_) {
                hasNow_ = false;
                Song s = std::move(now_);
                if (nowOn_) {
                    g.unlock();
                    sendNow(s);
                    g.lock();
                    worked = true;
                }
            }
            if (scrobbling_ && !queue_.empty() && signedIn_ && steady() >= holdUntil_ && int64_t(std::time(nullptr)) >= dayHold_) {
                g.unlock();
                sendBatch();
                g.lock();
                worked = true;
            }
        } else {
            hasNow_ = false;   // never kept for later
        }
        trim();
        g.unlock();
        saveQueue();
        g.lock();
        if (quit_) break;
        if (worked && on_ && signedIn_ && scrobbling_ && !queue_.empty() && steady() >= holdUntil_ && status_.rfind("Waiting", 0) != 0) continue;   // the next 50
        wake_.wait_for(g, std::chrono::seconds(1));
    }
    g.unlock();
    saveQueue();
}
