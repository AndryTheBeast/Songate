/*
 * Gatefold NX background player · decoder.cpp
 * Picking a decoder by the file's first bytes (the extension only breaks ties), the buffered
 * reader, and the conversions every decoder needs.
 */
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "dec_common.hpp"

// ---------------------------------------------------------------- Reader

bool Reader::open(const char* path) {
    close();
    f_ = fopen(path, "rb");
    if (!f_) return false;
    path_ = path;
    setvbuf(f_, nullptr, _IONBF, 0);   // our own buffer below; two layers of copying buy nothing
    fseek(f_, 0, SEEK_END);
    size_ = uint64_t(ftell(f_));
    fseek(f_, 0, SEEK_SET);
    buf_ = static_cast<uint8_t*>(malloc(Buf));
    pos_ = bufStart_ = bufLen_ = 0;
    return buf_ != nullptr;
}

// ---------------------------------------------------------------- the read-ahead

extern "C" size_t gfHeapSize;   // main.cpp
void (*readAheadLog)(const char* fmt, ...) = nullptr;

/** A Reader's read-ahead: the file's bytes [start, start + len) in ring, a ring of cap whose first byte is at head; gen moves on
 *  whenever the ring starts again elsewhere (a seek), so a block read for the place before is dropped. The read-ahead thread
 *  opens its own handle on the file (file) and closes it. All of it under raLock. */
struct RaState {
    std::string path, name;
    FILE* file = nullptr;
    uint8_t* ring = nullptr;
    size_t cap = 0, head = 0, len = 0, loggedCap = 0;
    uint64_t start = 0, size = 0, pos = 0;   // pos: where its Reader reads (what the read-ahead picks by)
    uint32_t gen = 0;
    bool busy = false, end = false, closed = false;
};

#ifdef GF_RATEST
std::atomic<int> raTestStallMs{ 0 };   // (tools/bgtest/join/bgread.cpp: each read of the card held up this long)
#endif

namespace {
std::mutex raLock;                  // every RaState and the two lists
std::vector<RaState*> raStates;     // being read ahead
std::vector<RaState*> raGone;       // let go of by their Readers: the read-ahead thread closes their files and frees them

uint64_t raNowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}   // namespace

/** How big a ring the heap spares now: up to RaMax, RaReserve kept, in whole blocks. */
size_t Reader::ringRoom() {
    const struct mallinfo mi = mallinfo();
    const size_t used = size_t(mi.uordblks) + RaReserve;
    const size_t room = gfHeapSize > used ? gfHeapSize - used : 0;
    return std::min(RaMax, room) / RaChunk * RaChunk;
}

/** A ring started smaller than RaMax (beside another song) made whole once there is room: its bytes kept, in order. */
void Reader::growRing() {
    if (ringRoom() < RaMax) return;   // (counted as if a whole new one: the old one goes after)
    uint8_t* bigger = static_cast<uint8_t*>(malloc(RaMax));
    if (!bigger) return;
    uint8_t* old = nullptr;
    {
        std::lock_guard<std::mutex> l(raLock);
        RaState* r = ra_;
        for (size_t i = 0; i < r->len; i++) bigger[i] = r->ring[(r->head + i) % r->cap];
        old = r->ring;
        r->ring = bigger;
        r->head = 0;
        r->cap = RaMax;
    }
    free(old);
}

void Reader::startReadAhead() {
    raWanted_ = true;
    if (!f_ || ra_) return;
    // Up to RaMax, RaReserve left beside it (a join's echo, a track picked by hand fading in, the next song's tables); the next
    // song, opened beside the one playing (Automix's prep, a join), takes what is left over RaMin and grows to RaMax once the
    // song before has gone (growRing). Its ring is what carries the join: without one there, a 3.5 s stall of the card was
    // heard mid-join (the user's console, 2026-11-22). Without even RaMin, read() tries again every 64 reads. Nothing here
    // touches the card: the read-ahead thread opens its handle.
    const size_t cap = ringRoom();
    if (cap < RaMin) {
        if (!raRefusedLogged_ && readAheadLog) {   // (the dev build: once per song, why it reads the card itself for now)
            raRefusedLogged_ = true;
            const struct mallinfo mi = mallinfo();
            readAheadLog("no room for read ahead yet: %u KB of the heap free, %s", unsigned((gfHeapSize - std::min(gfHeapSize, size_t(mi.uordblks))) / 1024),
                         path_.substr(path_.rfind('/') + 1).c_str());
        }
        return;
    }
    uint8_t* ring = static_cast<uint8_t*>(malloc(cap));
    if (!ring) return;
    RaState* r = new (std::nothrow) RaState;
    if (!r) { free(ring); return; }
    r->path = path_;
    r->name = path_.substr(path_.rfind('/') + 1);
    r->ring = ring;
    r->cap = cap;
    r->start = r->pos = pos_;
    r->size = size_;
    std::lock_guard<std::mutex> l(raLock);
    raStates.push_back(r);
    ra_ = r;
}

/** What the ring holds from pos_ on, into dst (up to n); the bytes far behind pos_ let go, for room. Under raLock. */
size_t Reader::takeFromRing(uint8_t* dst, size_t n) {
    RaState* r = ra_;
    size_t done = 0;
    while (done < n && pos_ >= r->start && pos_ < r->start + r->len) {
        const size_t off = size_t(pos_ - r->start), at = (r->head + off) % r->cap;
        const size_t k = std::min({ n - done, r->len - off, r->cap - at });
        memcpy(dst + done, r->ring + at, k);
        done += k;
        pos_ += k;
    }
    if (pos_ >= r->start + RaBack && pos_ <= r->start + r->len) {   // (a little kept behind: a decoder that steps back a few bytes)
        const size_t drop = size_t(pos_ - RaBack - r->start);
        r->head = (r->head + drop) % r->cap;
        r->start += drop;
        r->len -= drop;
    }
    r->pos = pos_;
    return done;
}

bool readAheadPass() {
    static uint8_t block[Reader::RaChunk];
    // the read-ahead blocks let go of: their files closed and their memory freed here, never on a decoding thread
    std::vector<RaState*> gone;
    {
        std::lock_guard<std::mutex> l(raLock);
        gone.swap(raGone);
    }
    for (RaState* g : gone) {
        if (g->file) fclose(g->file);
        free(g->ring);
        delete g;
    }
    RaState* r = nullptr;
    uint64_t at = 0, ahead = ~uint64_t(0);
    size_t want = 0;
    uint32_t gen = 0;
    FILE* file = nullptr;
    std::string fresh;   // a song just given its ring, or a bigger one (the dev build's log)
    unsigned freshKB = 0;
    {
        std::lock_guard<std::mutex> l(raLock);
        for (RaState* x : raStates) {
            const uint64_t end = x->start + x->len;
            if (x->end || end >= x->size || x->cap - x->len < Reader::RaChunk) continue;
            const uint64_t a = end > x->pos ? end - x->pos : 0;
            if (a < ahead) { ahead = a; r = x; }
        }
        if (!r) return !gone.empty();
        at = r->start + r->len;
        want = size_t(std::min<uint64_t>(Reader::RaChunk, r->size - at));
        gen = r->gen;
        file = r->file;
        r->busy = true;
        if (r->loggedCap != r->cap && readAheadLog) {   // (its ring started, or grown)
            r->loggedCap = r->cap;
            fresh = r->name;
            freshKB = unsigned(r->cap / 1024);
        }
    }
    if (!fresh.empty()) readAheadLog("read ahead: %u KB, %s", freshKB, fresh.c_str());
    const uint64_t t0 = raNowMs();
    bool opened = false;
    if (!file) {   // (its own handle, opened here: a card's stall holds only this thread)
        file = fopen(r->path.c_str(), "rb");
        if (file) setvbuf(file, nullptr, _IONBF, 0);
        opened = true;
    }
    size_t got = 0;
    if (file && fseek(file, long(at), SEEK_SET) == 0) got = fread(block, 1, want, file);
#ifdef GF_RATEST
    if (raTestStallMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(raTestStallMs.load()));   // (bgread: the card's stall)
#endif
    const uint64_t ms = raNowMs() - t0;
    if (ms > 100 && readAheadLog)
        readAheadLog("slow card read (read ahead, not heard): %llu ms; %llu KB were ready ahead", (unsigned long long)ms,
                     (unsigned long long)(ahead / 1024));
    std::lock_guard<std::mutex> l(raLock);
    r->busy = false;
    if (opened) r->file = file;
    if (r->closed) {   // (its Reader went while this read was under way: this thread frees it on its next pass)
        raGone.push_back(r);
        return true;
    }
    if (got == 0) {
        r->end = true;   // (the file stopped answering: the Reader reads the card itself, as before)
        return true;
    }
    if (r->gen == gen && r->start + r->len == at && r->cap - r->len >= got) {
        size_t done = 0;
        while (done < got) {
            const size_t w = (r->head + r->len) % r->cap;
            const size_t k = std::min(got - done, r->cap - w);
            memcpy(r->ring + w, block + done, k);
            done += k;
            r->len += k;
        }
    }
    return true;
}

void Reader::close() {
    if (ra_) {   // let go of at once: the read-ahead thread closes its file and frees it (after its read, if one is under way)
        std::lock_guard<std::mutex> l(raLock);
        RaState* r = ra_;
        r->closed = true;
        const auto it = std::find(raStates.begin(), raStates.end(), r);
        if (it != raStates.end()) raStates.erase(it);
        if (!r->busy) raGone.push_back(r);
        ra_ = nullptr;
    }
    raWanted_ = false;
    if (f_) fclose(f_);
    f_ = nullptr;
    free(buf_);
    buf_ = nullptr;
}

bool Reader::seek(uint64_t pos) {
    if (pos > size_) return false;
    pos_ = pos;
    return true;
}

size_t Reader::read(void* dst, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    size_t done = 0;
    if (raWanted_ && (++raTries_ & 63) == 0) {
        if (!ra_) startReadAhead();
        else if (ra_->cap < RaMax) growRing();   // (cap changes only here: read without the lock)
    }
    if (ra_) {
        std::lock_guard<std::mutex> l(raLock);
        done = takeFromRing(d, n);
        if (done == n || pos_ >= size_) return done;
        // not in the ring (a seek, or the read-ahead behind): read from the card as before, the ring starting again after it
        RaState* r = ra_;
        r->gen++;
        r->head = r->len = 0;
        r->start = r->pos = std::min(size_, pos_ + (n - done));
        r->end = false;
    }
    while (done < n && pos_ < size_) {
        if (pos_ >= bufStart_ && pos_ < bufStart_ + bufLen_) {
            const size_t off = size_t(pos_ - bufStart_);
            const size_t k = std::min(n - done, bufLen_ - off);
            memcpy(d + done, buf_ + off, k);
            done += k;
            pos_ += k;
            continue;
        }
        // Big reads skip the buffer; small ones refill it from here.
        if (n - done >= Buf) {
            if (fseek(f_, long(pos_), SEEK_SET) != 0) break;
            const size_t k = fread(d + done, 1, n - done, f_);
            if (k == 0) break;
            done += k;
            pos_ += k;
            continue;
        }
        if (fseek(f_, long(pos_), SEEK_SET) != 0) break;
        bufStart_ = pos_;
        bufLen_ = fread(buf_, 1, Buf, f_);
        if (bufLen_ == 0) break;
    }
    return done;
}

uint8_t Reader::u8() { uint8_t b = 0; read(&b, 1); return b; }
uint16_t Reader::u16() { uint8_t b[2] = {}; read(b, 2); return uint16_t(b[0] << 8 | b[1]); }
uint32_t Reader::u32() { uint8_t b[4] = {}; read(b, 4); return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
uint64_t Reader::u64() { const uint64_t hi = u32(); return hi << 32 | u32(); }

// ---------------------------------------------------------------- conversions

void toStereo(const int16_t* in, int frames, int channels, int16_t* out) {
    if (channels == 2) { memmove(out, in, size_t(frames) * 4); return; }
    if (channels == 1) {
        for (int i = frames - 1; i >= 0; i--) out[i * 2] = out[i * 2 + 1] = in[i];   // backwards: in and out may overlap
        return;
    }
    for (int i = 0; i < frames; i++) { out[i * 2] = in[i * channels]; out[i * 2 + 1] = in[i * channels + 1]; }
}

static void putUtf8(std::string& s, unsigned c) {
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | c >> 6); s += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += char(0xE0 | c >> 12); s += char(0x80 | (c >> 6 & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    else { s += char(0xF0 | c >> 18); s += char(0x80 | (c >> 12 & 0x3F)); s += char(0x80 | (c >> 6 & 0x3F)); s += char(0x80 | (c & 0x3F)); }
}

// ID3v2 encodings: 0 Latin-1, 1 UTF-16 with a BOM, 2 UTF-16BE, 3 UTF-8.
std::string tagText(const uint8_t* p, size_t n, int encoding) {
    std::string s;
    if (encoding == 3) s.assign(reinterpret_cast<const char*>(p), n);
    else if (encoding == 0) for (size_t i = 0; i < n; i++) putUtf8(s, p[i]);
    else {
        bool be = encoding == 2;
        size_t i = 0;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
        for (; i + 1 < n; i += 2) {
            unsigned c = be ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                const unsigned lo = be ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 3] << 8 | p[i + 2]);
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            if (c == 0) break;
            putUtf8(s, c);
        }
    }
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// ---------------------------------------------------------------- picking one

static std::string extension(const char* path) {
    const char* dot = strrchr(path, '.');
    std::string e = dot ? dot + 1 : "";
    for (auto& c : e) c = char(tolower(c));
    return e;
}

static std::unique_ptr<Decoder> openAny(const char* path);

std::unique_ptr<Decoder> openDecoder(const char* path) {
    std::unique_ptr<Decoder> d = openAny(path);
    if (d) d->readAhead();
    return d;
}

static std::unique_ptr<Decoder> openAny(const char* path) {
    uint8_t head[12] = {};
    if (FILE* f = fopen(path, "rb")) {
        const size_t got = fread(head, 1, sizeof head, f);
        fclose(f);
        if (got < sizeof head) return nullptr;
    } else {
        return nullptr;
    }
    if (!memcmp(head + 4, "ftyp", 4)) return openMp4(path);
    if (!memcmp(head, "fLaC", 4)) return openFlac(path);
    if (!memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) return openWav(path);
    // (an ID3 tag in front of a FLAC, as some rippers write it: the FLAC reader passes over the tag itself)
    if (!memcmp(head, "ID3", 3) && extension(path) == "flac") return openFlac(path);
    if (!memcmp(head, "ID3", 3) || (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)) return openMp3(path);
    // Odd files (an ID3 tag in front of a FLAC, MP4 without ftyp first): trust the extension.
    const std::string e = extension(path);
    if (e == "m4a" || e == "mp4" || e == "aac" || e == "m4b") return openMp4(path);
    if (e == "flac") return openFlac(path);
    if (e == "mp3") return openMp3(path);
    if (e == "wav") return openWav(path);
    return nullptr;
}
