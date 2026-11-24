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
    size_t cap = 0, head = 0, len = 0;
    uint64_t start = 0, size = 0, pos = 0;   // pos: where its Reader reads (what the read-ahead picks by)
    uint32_t gen = 0;
    int slot = -1;   // which of raSlots holds its ring; -1 and ring null while it has none (no memory yet, or given back)
    size_t need = 0;         // the ring its bitrate calls for (Reader::startReadAhead)
    uint64_t askAt = 0;      // a ring, or a bigger one, is asked for again from then on (ms)
    bool busy = false, end = false, closed = false;
    bool filled = false;   // the read-ahead has put bytes in since the ring last started (the dev build's "ran dry")
};

#ifdef GF_RATEST
std::atomic<int> raTestStallMs{ 0 };   // (tools/bgtest/join/bgread.cpp: each read of the card held up this long)
#endif

// The memory the big rings are in comes from outside: the sysmodule takes each from the system as a block of its own, of any
// size (Engine::start sets these); left null, as the PC test leaves them, it is malloc's, and the system always has room.
void* (*readAheadTake)(size_t bytes) = nullptr;   // null when the system says no
void (*readAheadGive)(void* block) = nullptr;
size_t (*readAheadSystemFree)() = nullptr;        // what the system's pool has free now, in bytes

namespace {
std::mutex raLock;                  // every RaState, the two lists and the slots
std::vector<RaState*> raStates;     // being read ahead
std::vector<RaState*> raGone;       // let go of by their Readers: the read-ahead thread closes their files and frees them

// A song's ring is as big as its bitrate calls for (GF_RA_COVER_MS of it: what it holds is all the sound has while a read is
// held up by the card), and where it comes from goes by what the console can spare. All of it under raLock:
//   full    from the system, a block of the song's own, as long as all the rings together stay within GF_RA_FULL_MB: when the
//           system's pool has GF_RA_FULL_FREE_MB free (counting what the rings already hold)
//   medium  the same within GF_RA_MED_MB, when it has GF_RA_MED_FREE_MB free
//   small   Reader::RaSmall a song from the system, when it has GF_RA_SMALL_FREE_MB free: the heap's ring, kept off the heap
//   light   Reader::RaSmall from the heap, as 2.1.1 had, when the heap has it beside RaReserve: only when the system has none
//           to spare or says no. (A song that needs little, an MP3, an AAC, took its ring from the heap in every tier at
//           first: two of them joining with the echo left the 2 MB heap 270 KB, and 256 KB was 5.5 s of a 330 kbit/s
//           song with nothing over: two ran dry. The console, 2026-11-23.)
//   none    the song reads the card itself, as 2.0 did
// A ring from the system goes back when its song closes, and every ring after GF_RA_IDLE_S without music (stopped, paused,
// paused for a game): a console that is not playing pays nothing for them. A song that got less than it needs (the join's
// incoming song, the limit reached) is given its whole ring once there is room (raGrow).
// Never from the heap for the big ones: a 1 MB block asked of the heap at every song was not there from the third song on
// (cut up by the songs before, 2.8 MB of it free: the console, 2026-11-23), and malloc said so without a word.
struct RaSlot { uint8_t* mem = nullptr; size_t size = 0; bool used = false, heap = false; };
RaSlot raSlots[Reader::RaSlots];
size_t raTaken = 0;                 // what the rings hold of the system's memory now
bool raPlaying = true;              // music is heard (readAheadPlaying); the PC test never says otherwise
uint64_t raQuietSince = 0;          // when it stopped (ms); 0 while playing
const char* raTier = "";            // (the dev build's log: the tier the last ring came from)
size_t raSongMost = 0;              // the most one song may take in the tier raRoom last found

uint64_t raNowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/** How much more the rings may take from the system now: the tier's limit, less what they hold. */
size_t raRoom() {
    const size_t sysFree = (readAheadSystemFree ? readAheadSystemFree() : ~size_t(0) / 2) + raTaken;   // (as if they held nothing)
    size_t limit = 0;
    raTier = "light";
    if (sysFree >= (size_t(GF_RA_FULL_FREE_MB) << 20)) { limit = size_t(GF_RA_FULL_MB) << 20; raTier = "full"; }
    else if (sysFree >= (size_t(GF_RA_MED_FREE_MB) << 20)) { limit = size_t(GF_RA_MED_MB) << 20; raTier = "medium"; }
    else if (sysFree >= (size_t(GF_RA_SMALL_FREE_MB) << 20)) { limit = Reader::RaSmall * Reader::RaSlots; raTier = "small"; }
    // (small: the heap's ring, from the system: one each for a join's two songs, not all of it for the first)
    raSongMost = limit == Reader::RaSmall * Reader::RaSlots ? Reader::RaSmall : limit;
    return limit > raTaken ? limit - raTaken : 0;
}

/** A block of the system's for a ring of up to want, when that is more than the heap's small ring would be; null: none. */
uint8_t* raTake(size_t want, size_t* got) {
    const size_t room = raRoom();
    const size_t size = std::min({ want, room, raSongMost }) / Reader::RaChunk * Reader::RaChunk;
    if (size < Reader::RaSmall) return nullptr;   // (less than the heap's would be: not worth a block)
    uint8_t* mem = static_cast<uint8_t*>(readAheadTake ? readAheadTake(size) : malloc(size));
    if (!mem) { raTier = "light"; return nullptr; }   // (the system said no)
    raTaken += size;
    *got = size;
    return mem;
}

void raGive(RaSlot& sl) {
    if (sl.heap) free(sl.mem);
    else {
        if (readAheadGive) readAheadGive(sl.mem); else free(sl.mem);
        raTaken -= sl.size;
    }
    sl = RaSlot{};
}

/** A ring for r, from the best tier there is; false: none for now (asked again in a while). */
bool raAttach(RaState* r) {
    const uint64_t now = raNowMs();
    if (now < r->askAt) return false;
    int i = 0;
    while (i < Reader::RaSlots && raSlots[i].used) i++;
    if (i == Reader::RaSlots) return false;   // (a third song at once: the one before is not yet let go of; asked again next pass)
    RaSlot& sl = raSlots[i];
    size_t size = 0;
    uint8_t* mem = raTake(r->need, &size);   // (every song's, an MP3's small one too: the heap's are the last resort)
    if (!mem) raTier = "light";
    if (mem) {
        sl.heap = false;
    } else {
        const struct mallinfo mi = mallinfo();
        if (gfHeapSize >= size_t(mi.uordblks) + Reader::RaSmall + Reader::RaReserve) mem = static_cast<uint8_t*>(malloc(Reader::RaSmall));
        if (!mem) {
            r->askAt = now + 2000;
            if (readAheadLog) readAheadLog("read ahead: no ring for now (tier none: no room on the heap for a small one), %s", r->name.c_str());
            return false;
        }
        size = Reader::RaSmall;
        sl.heap = true;
    }
    sl.mem = mem;
    sl.size = size;
    sl.used = true;
    r->slot = i;
    r->ring = mem;
    r->cap = size;
    r->head = r->len = 0;
    r->start = r->pos;
    r->gen++;
    r->filled = false;
    r->end = false;
    r->askAt = now + 2000;   // (raGrow: not before)
    if (readAheadLog)
        readAheadLog("read ahead: %u KB ring (tier %s; it wants %u KB; %u KB of the system's held), %s", unsigned(size / 1024), raTier,
                     unsigned(r->need / 1024), unsigned(raTaken / 1024), r->name.c_str());
    return true;
}

/** r's ring back to where it came from; r reads the card itself until it has another. */
void raDetach(RaState* r) {
    if (r->slot < 0) return;
    raGive(raSlots[r->slot]);
    r->slot = -1;
    r->ring = nullptr;
    r->cap = r->head = r->len = 0;
    r->start = r->pos;
    r->gen++;
    r->filled = false;
}

/** A song whose ring is smaller than it needs (the limit was reached beside another song, or the system had none) given
 *  the bigger one once there is room: its bytes kept, in order. Looked at every two seconds. */
void raGrow(RaState* r) {
    const uint64_t now = raNowMs();
    if (r->slot < 0 || r->cap >= r->need || now < r->askAt) return;
    r->askAt = now + 2000;
    RaSlot& sl = raSlots[r->slot];
    // (its own block goes back after the new one is taken: for that moment the rings hold both, so the new one may only be
    //  what the limit leaves beside the old)
    const size_t room = raRoom();
    if (std::min({ r->need, room, raSongMost }) / Reader::RaChunk * Reader::RaChunk <= r->cap) return;   // (no bigger to be had)
    size_t size = 0;
    uint8_t* mem = raTake(r->need, &size);
    if (!mem) return;
    for (size_t i = 0; i < r->len; i++) mem[i] = r->ring[(r->head + i) % r->cap];
    raGive(sl);
    sl.mem = mem;
    sl.size = size;
    sl.used = true;
    sl.heap = false;
    r->ring = mem;
    r->cap = size;
    r->head = 0;
    if (readAheadLog)
        readAheadLog("read ahead: ring grown to %u KB (tier %s; %u KB of the system's held), %s", unsigned(size / 1024), raTier, unsigned(raTaken / 1024),
                     r->name.c_str());
}
}   // namespace

void readAheadPlaying(bool playing) {
    std::lock_guard<std::mutex> l(raLock);
    if (playing) raQuietSince = 0;
    else if (raPlaying || !raQuietSince) raQuietSince = raNowMs();
    raPlaying = playing;
}

void Reader::startReadAhead(int kbps) {
    if (!f_ || ra_) return;
    // Only asked for here: the read-ahead thread gives it a ring on its next pass (raAttach), and takes it back when the music
    // has been quiet for a while. Nothing here touches the card or the system.
    RaState* r = new (std::nothrow) RaState;
    if (!r) return;
    r->path = path_;
    r->name = path_.substr(path_.rfind('/') + 1);
    r->start = r->pos = pos_;
    r->size = size_;
    // GF_RA_COVER_MS of the song at its bitrate (1411 kbit/s, a CD's, when it is not known), what is kept behind, in whole blocks
    const uint64_t bytes = uint64_t(kbps > 0 ? kbps : 1411) * 125 * GF_RA_COVER_MS / 1000 + RaBack;
    r->need = size_t(std::clamp<uint64_t>((bytes + RaChunk - 1) / RaChunk * RaChunk, RaLeast, uint64_t(GF_RA_FULL_MB) << 20));
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
        {
            std::lock_guard<std::mutex> l(raLock);
            raDetach(g);   // (its ring is free for the next song only now: nothing reads into it any more)
        }
        delete g;
    }
    RaState* r = nullptr;
    uint64_t at = 0, ahead = ~uint64_t(0);
    size_t want = 0;
    uint32_t gen = 0;
    FILE* file = nullptr;
    {
        std::lock_guard<std::mutex> l(raLock);
        // Quiet for GF_RA_IDLE_S: every ring back. (No read is under way: this thread is the only one that reads ahead, and
        // it is here.)
        if (!raPlaying && raQuietSince && raNowMs() - raQuietSince >= uint64_t(GF_RA_IDLE_S) * 1000) {
            bool had = false;
            for (RaState* x : raStates) { had = had || x->slot >= 0; raDetach(x); }
            if (had && readAheadLog) readAheadLog("read-ahead memory: given back (no music for %d s)", int(GF_RA_IDLE_S));
        }
        if (raPlaying)
            for (RaState* x : raStates) {
                if (x->closed) continue;
                if (x->slot < 0) raAttach(x);
                else raGrow(x);
            }
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
    }
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
        r->filled = true;
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
    if (ra_) {
        std::lock_guard<std::mutex> l(raLock);
        done = takeFromRing(d, n);
        if (done == n || pos_ >= size_) return done;
        // not in the ring (a seek, or the read-ahead behind): read from the card as before, the ring starting again after it
        RaState* r = ra_;
        // (the dev build) read to its end while the song goes on: the read-ahead was held up longer than the ring lasts, and
        // this thread, the sound's, now waits for the card itself. A seek lands elsewhere and is not this.
        if (r->filled && pos_ == r->start + r->len && readAheadLog)
            readAheadLog("read ahead ran dry (%u KB ring): the sound's thread reads the card itself, %s", unsigned(r->cap / 1024), r->name.c_str());
        r->filled = false;
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
