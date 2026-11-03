/*
 * Songate · identity.cpp
 * See identity.hpp. normalise() is written out here rather than left to a library: both devices must get the same text from
 * the same tags, on every platform, for ever. Changing it changes every loose key.
 */
#include "identity.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unordered_set>

#include "cores.hpp"
#include "metadata.hpp"
extern "C" {
#include "monocypher.h"
}

namespace {

constexpr size_t kPiece = 65536;
constexpr size_t kHash = 16;

std::string hex(const uint8_t* p, size_t n) {
    static const char* const digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}

void appendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | c >> 6); s += char(0x80 | (c & 63)); }
    else if (c < 0x10000) { s += char(0xE0 | c >> 12); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
    else { s += char(0xF0 | c >> 18); s += char(0x80 | ((c >> 12) & 63)); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
}

uint32_t nextUtf8(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    const int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (!more) return c < 0x80 ? c : 0xFFFD;
    uint32_t v = c & (0x3F >> more);
    for (int k = 0; k < more && i < s.size(); k++) v = v << 6 | ((unsigned char)s[i++] & 63);
    return v;
}

// U+00C0 to U+017F, lower case without its accent (æ, ð, ø, þ and the like have none to lose, and stay)
const char* latin(uint32_t c) {
    static const char* const k[] = {
        "a","a","a","a","a","a","\xC3\xA6","c","e","e","e","e","i","i","i","i","\xC3\xB0","n","o","o","o","o","o",nullptr,"\xC3\xB8","u","u","u","u","y","\xC3\xBE","ss",
        "a","a","a","a","a","a","\xC3\xA6","c","e","e","e","e","i","i","i","i","\xC3\xB0","n","o","o","o","o","o",nullptr,"\xC3\xB8","u","u","u","u","y","\xC3\xBE","y",
        "a","a","a","a","a","a","c","c","c","c","c","c","c","c","d","d","\xC4\x91","\xC4\x91","e","e","e","e","e","e","e","e","e","e","g","g","g","g",
        "g","g","g","g","h","h","\xC4\xA7","\xC4\xA7","i","i","i","i","i","i","i","i","i","\xC4\xB1","ij","ij","j","j","k","k","\xC4\xB8","l","l","l","l","l",
        "l","l","l","\xC5\x82","\xC5\x82","n","n","n","n","n","n","n","\xC5\x8B","\xC5\x8B","o","o","o","o","o","o","\xC5\x93","\xC5\x93","r","r","r","r","r","r",
        "s","s","s","s","s","s","s","s","t","t","t","t","\xC5\xA7","\xC5\xA7","u","u","u","u","u","u","u","u","u","u","u","u","w","w","y","y","y","z","z","z","z","z","z","s" };
    return c >= 0xC0 && c <= 0x17F ? k[c - 0xC0] : nullptr;
}

/** Lower case, no accents, no combining marks, & as a word. */
std::string fold(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        uint32_t c = nextUtf8(s, i);
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '&') { o += " and "; continue; }
        if (c >= 0x300 && c <= 0x36F) continue;                 // a combining mark
        if (const char* l = latin(c)) { o += l; continue; }
        if (c >= 0x391 && c <= 0x3A9) c += 0x20;                // Greek
        else if (c >= 0x410 && c <= 0x42F) c += 0x20;           // Cyrillic
        else if (c >= 0x400 && c <= 0x40F) c += 0x50;
        else if (c >= 0xFF21 && c <= 0xFF3A) c = c - 0xFF21 + 'a';   // full-width letters
        else if (c >= 0xFF41 && c <= 0xFF5A) c = c - 0xFF41 + 'a';
        else if (c >= 0xFF10 && c <= 0xFF19) c = c - 0xFF10 + '0';
        appendUtf8(o, c);
    }
    return o;
}

/** A bracket holding "feat", "remaster", "deluxe" and the like says nothing of which song it is. */
void dropBrackets(std::string& s) {
    static const char* const words[] = { "feat", "ft", "with", "remaster", "explicit", "deluxe", "bonus", "version", "edition" };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '(' && s[i] != '[') continue;
        size_t best = std::string::npos, len = 0;
        for (const char* w : words) {
            const size_t p = s.find(w, i + 1);
            if (p < best) { best = p; len = strlen(w); }
        }
        if (best == std::string::npos) continue;
        const size_t close = s.find_first_of(")]", best + len);
        if (close == std::string::npos) continue;
        s.replace(i, close - i + 1, " ");
    }
}

bool wordChar(const std::string& s, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    return c >= 0x80 || c == '_' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/** "feat.", "ft." or "featuring" as a word: it and all that follows go. */
void dropFeaturing(std::string& s) {
    for (size_t i = 0; i < s.size();) {
        if (!wordChar(s, i)) { i++; continue; }
        size_t e = i;
        while (e < s.size() && wordChar(s, e)) e++;
        const std::string w = s.substr(i, e - i);
        if (w == "feat" || w == "ft" || w == "featuring") { s.erase(i); return; }
        i = e;
    }
}

bool punctuation(uint32_t c) {
    return (c >= 0x80 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) || c == 0xD7 || c == 0xF7 || (c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) ||
           (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || c == 0xFFFD;
}

/** Words of letters and digits, one space between them. */
std::string words(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const size_t at = i;
        const uint32_t c = nextUtf8(s, i);
        const bool word = c >= 0x80 ? !punctuation(c) : wordChar(s, at);
        if (word) o.append(s, at, i - at);
        else if (!o.empty() && o.back() != ' ') o += ' ';
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string readAll(const std::string& p) {
    std::string out;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return out;
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    fclose(f);
    return out;
}

}  // namespace

std::string Identity::normalise(const std::string& s) {
    std::string t = fold(s);
    dropBrackets(t);
    dropFeaturing(t);
    return words(t);
}

std::string Identity::looseKey(const std::string& artist, const std::string& albumArtist, const std::string& album, const std::string& title) {
    // the first artist named: "A; B" and "A, B" are lists in one tag
    std::string a = artist.empty() ? albumArtist : artist;
    a = a.substr(0, a.find(';'));
    a = a.substr(0, a.find(','));
    const std::string na = normalise(a), nt = normalise(title);
    if (na.empty() || nt.empty()) return {};
    const std::string key = na + "\n" + normalise(album) + "\n" + nt;
    uint8_t h[kHash];
    crypto_blake2b(h, kHash, reinterpret_cast<const uint8_t*>(key.data()), key.size());
    return hex(h, kHash);
}

std::string Identity::exactHash(const std::string& path, int64_t size) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, kHash);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(uint64_t(size) >> (8 * i));
    crypto_blake2b_update(&c, len, 8);
    std::vector<uint8_t> buf(kPiece);
    const int64_t mid = size / 2 - int64_t(kPiece / 2), end = size - int64_t(kPiece);
    const int64_t at[3] = { 0, mid > 0 ? mid : 0, end > 0 ? end : 0 };
    bool ok = true;
    for (int64_t o : at) {
        if (fseek(f, long(o), SEEK_SET) != 0) { ok = false; break; }
        const size_t n = fread(buf.data(), 1, kPiece, f);
        crypto_blake2b_update(&c, buf.data(), n);
    }
    fclose(f);
    uint8_t h[kHash];
    crypto_blake2b_final(&c, h);
    return ok ? hex(h, kHash) : std::string();
}

void Identity::start(const std::string& dir) {
    dir_ = dir;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void Identity::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    stopRun_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Identity::want(std::vector<Want> tracks) {
    std::lock_guard<std::mutex> g(m_);
    want_ = std::move(tracks);
    fresh_ = true;
    stopRun_ = true;
    prog_ = Progress{};
    prog_.total = int(want_.size());
    prog_.running = true;
    busy_ = true;
    wake_.notify_all();
}

void Identity::forget() {
    std::lock_guard<std::mutex> g(m_);
    forget_ = true;
    stopRun_ = true;
    wake_.notify_all();
}

Identity::Progress Identity::progress() {
    std::lock_guard<std::mutex> g(m_);
    return prog_;
}

bool Identity::find(const std::string& path, Known& out) {
    std::lock_guard<std::mutex> g(m_);
    auto it = known_.find(path);
    if (it == known_.end()) return false;
    out = it->second;
    return true;
}

void Identity::load() {
    const std::string text = readAll(dir_ + "/identity.tsv");
    std::unordered_map<std::string, Known> file;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        const std::string l = text.substr(a, b - a);
        a = b + 1;
        // path, size, date, exact, loose, length
        const size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? t1 : l.find('\t', t1 + 1), t3 = t2 == std::string::npos ? t2 : l.find('\t', t2 + 1);
        const size_t t4 = t3 == std::string::npos ? t3 : l.find('\t', t3 + 1), t5 = t4 == std::string::npos ? t4 : l.find('\t', t4 + 1);
        if (t5 == std::string::npos) continue;
        Known k;
        k.size = atoll(l.c_str() + t1 + 1);
        k.date = atoll(l.c_str() + t2 + 1);
        k.exact = l.substr(t3 + 1, t4 - t3 - 1);
        k.loose = l.substr(t4 + 1, t5 - t4 - 1);
        k.duration = atof(l.c_str() + t5 + 1);
        file[l.substr(0, t1)] = std::move(k);
    }
    std::lock_guard<std::mutex> g(m_);
    known_ = std::move(file);
    knownCount_ = known_.size();
    loaded_ = true;
}

void Identity::save() {
    std::string out;
    {
        // no lock: only this thread changes known_, and others may be asking for an identity meanwhile
        out.reserve(known_.size() * 160);
        char n[96];
        for (const auto& kv : known_) {
            snprintf(n, sizeof n, "\t%lld\t%lld\t", (long long)kv.second.size, (long long)kv.second.date);
            out += kv.first;
            out += n;
            out += kv.second.exact + "\t" + kv.second.loose;
            snprintf(n, sizeof n, "\t%.3f\n", kv.second.duration);
            out += n;
        }
    }
    const std::string p = dir_ + "/identity.tsv", tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return; }
    remove(p.c_str());
    rename(tmp.c_str(), p.c_str());
}

void Identity::run() {
    cores::moveHere(cores::Loud);   // whole-library work: only in time nothing else on the core wants
    std::unique_lock<std::mutex> g(m_);
    while (!quit_) {
        wake_.wait(g, [&] { return quit_ || fresh_ || forget_; });
        if (quit_) break;
        if (forget_) {
            forget_ = false;
            known_.clear();
            knownCount_ = 0;
            loaded_ = true;   // nothing to read back
            g.unlock();
            remove((dir_ + "/identity.tsv").c_str());
            g.lock();
            if (!fresh_) { prog_ = Progress{}; continue; }
        }
        fresh_ = false;
        stopRun_ = false;
        std::vector<Want> list = std::move(want_);
        want_.clear();
        const bool needLoad = !loaded_;
        g.unlock();
        if (needLoad) load();
        const double t0 = seconds();
        double hashing = 0;
        int done = 0, hashed = 0, failed = 0;
        int gentle = 0;
        for (const Want& w : list) {
            if (stopRun_) break;
            // the card is shared with covers and music: a pause now and then, so that this never fills it
            if (++gentle % 24 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(8));
            struct stat st {};
            Known k;
            bool have = false;
            if (stat(w.path.c_str(), &st) != 0) failed++;
            else {
                {
                    std::lock_guard<std::mutex> l(m_);
                    auto it = known_.find(w.path);
                    if (it != known_.end() && it->second.size == int64_t(st.st_size) && it->second.date == int64_t(st.st_mtime)) { k = it->second; have = true; }
                }
                // nothing waits for a file already known, and flat out this held the console's frames to 50-90 ms (2026-11-01):
                // the whole library, spread over a minute or so
                if (have) std::this_thread::sleep_for(std::chrono::milliseconds(16));
                if (!have) {
                    const double h0 = seconds();
                    k.size = int64_t(st.st_size);
                    k.date = int64_t(st.st_mtime);
                    k.exact = exactHash(w.path, k.size);
                    hashing += seconds() - h0;
                    hashed++;
                    if (k.exact.empty()) failed++;
                }
                if (w.edited) {   // an edit does not make it another song: the file's own tags say which it is
                    TrackInfo info;
                    if (readTrackInfo(w.path, info, false, false)) k.loose = looseKey(info.artist, info.albumArtist, info.album, info.title);
                    else k.loose = looseKey(w.artist, w.albumArtist, w.album, w.title);
                } else {
                    k.loose = looseKey(w.artist, w.albumArtist, w.album, w.title);
                }
                k.duration = w.duration;
                std::lock_guard<std::mutex> l(m_);
                if (!k.exact.empty()) known_[w.path] = std::move(k);
            }
            done++;
            std::lock_guard<std::mutex> l(m_);
            prog_.done = done;
            prog_.hashed = hashed;
            prog_.failed = failed;
            prog_.hashSeconds = hashing;
            prog_.seconds = seconds() - t0;
        }
        g.lock();
        if (forget_) { prog_.running = false; continue; }   // Clear pressed meanwhile: nothing to keep
        if (!stopRun_) {   // a whole run: a file deleted or moved is no longer ours to offer
            std::unordered_set<std::string> here;
            here.reserve(list.size());
            for (const Want& w : list) here.insert(w.path);
            for (auto it = known_.begin(); it != known_.end();) it = here.count(it->first) ? std::next(it) : known_.erase(it);
        }
        g.unlock();
        save();
        g.lock();
        prog_.seconds = seconds() - t0;
        knownCount_ = known_.size();
        if (!fresh_) { prog_.running = false; busy_ = false; }
    }
}
