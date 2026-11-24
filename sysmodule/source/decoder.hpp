/*
 * Gatefold NX background player · decoder.hpp
 * Small, streaming decoders that fit a sysmodule's few MB: AAC in MP4/M4A (our own demuxer and
 * Helix), MP3 (minimp3), FLAC (dr_flac) and WAV. Nothing here uses libnx, so the PC test
 * (tools/bgtest) runs the very same code.
 *
 * Every decoder hands out interleaved 16-bit stereo, whatever the file has.
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

struct Tags {
    std::string title, artist, album;
    int coverKind = 0;          // 0 none, 1 JPEG, 2 PNG (as GfbgTrackInfo)
    uint64_t coverOffset = 0;   // in the file
    uint32_t coverSize = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    /** Up to maxFrames stereo frames into out; 0 at the end, negative on a broken file. */
    virtual int read(int16_t* out, int maxFrames) = 0;
    virtual bool seek(double seconds) = 0;
    /** Its file read ahead from here on (readAheadPass); openDecoder calls it once the file is open. */
    virtual void readAhead() {}

    int rate = 0;          // the file's sample rate
    int channels = 0;      // the file's channel count (the output is stereo regardless)
    int codec = 0;         // GfbgCodec
    int bitrate = 0;       // kbit/s
    double duration = 0;
    Tags tags;
};

/** Opens path with whichever decoder its contents call for; null when none can. */
std::unique_ptr<Decoder> openDecoder(const char* path);

/** The read-ahead: one step of it (the engine's read-ahead thread runs it over and over): the open file with the least
 *  read ahead gets its next block from the card, into its Reader's ring. False when every ring is full (time to rest).
 *  A card that stops answering for a few seconds (the user's console, 2026-11-21: 3.4 to 4.9 s on one read, mid-song) then
 *  holds up this thread, not the sound: the audio thread reads what is already in memory. */
bool readAheadPass();
/** Whether music is heard now (the engine's watch thread says so every tick): the rings' memory is only held while it is, and
 *  for GF_RA_IDLE_S after (decoder.cpp). */
void readAheadPlaying(bool playing);
/** Where the rings' memory comes from (decoder.cpp): the sysmodule's are the system's (Engine::start); null: malloc's. */
extern void* (*readAheadTake)(size_t bytes);
extern void (*readAheadGive)(void* block);
extern size_t (*readAheadSystemFree)();
/** The dev build's log line for a read that took long (the engine's bgLog; null: none). */
extern void (*readAheadLog)(const char* fmt, ...);

// ---------------------------------------------------------------- shared helpers

struct RaState;   // (decoder.cpp)

// The read-ahead's memory (decoder.cpp's tiers): how much of a song a ring holds, what the rings may take from the system in
// all and how much the system's pool must have free for that, and how long after the music stops it all goes back. A test
// build may name others: make DEFINES="-DGF_RA_FULL_FREE_MB=16 -DGF_RA_IDLE_S=10"
#ifndef GF_RA_COVER_MS
#define GF_RA_COVER_MS 5500     // the card's stalls ran to 5 s
#endif
#ifndef GF_RA_FULL_MB
#define GF_RA_FULL_MB 2         // full: 1.2 MB for a 24-bit FLAC and 0.8 MB for the next, or two CD-quality FLACs whole
#endif
#ifndef GF_RA_FULL_FREE_MB
#define GF_RA_FULL_FREE_MB 10
#endif
#ifndef GF_RA_MED_MB
#define GF_RA_MED_MB 1          // medium
#endif
#ifndef GF_RA_MED_FREE_MB
#define GF_RA_MED_FREE_MB 6
#endif
#ifndef GF_RA_SMALL_FREE_MB
#define GF_RA_SMALL_FREE_MB 3   // small: 256 KB a song from the system; below it, light: the same from the heap
#endif
#ifndef GF_RA_IDLE_S
#define GF_RA_IDLE_S 60
#endif

/** A FILE* with a read buffer of its own: newlib's is small, and SD reads like big blocks. */
class Reader {
public:
    ~Reader() { close(); }
    bool open(const char* path);
    void close();
    size_t read(void* dst, size_t n);
    bool seek(uint64_t pos);
    uint64_t tell() const { return pos_; }
    uint64_t size() const { return size_; }
    bool readAt(uint64_t pos, void* dst, size_t n) { return seek(pos) && read(dst, n) == n; }
    /** From here on its bytes come from a ring that readAheadPass keeps filled ahead of where it reads, once the read-ahead
     *  thread has a ring for it (decoder.cpp's tiers); without one, it reads the card itself as before. */
    void startReadAhead(int kbps);   // kbps: the song's bitrate (0: not known), which sizes its ring
    // RaSlots: the rings there can be at once (a join plays two songs). RaSmall: a ring's size from the heap (2.1.1's 256 KB:
    // 5 s of a 350 kbit/s song, 2 s of a 1000 kbit/s FLAC), and RaReserve what the heap keeps free beside it (a join's second
    // decoder, a track picked by hand fading in). A ring from the system is as big as its song needs (decoder.cpp).
    static constexpr int RaSlots = 2;
    // RaLeast: the smallest ring asked of the system (8 s of a 330 kbit/s song).
    static constexpr size_t RaSmall = 256 * 1024, RaLeast = 384 * 1024, RaReserve = 320 * 1024, RaBack = 32 * 1024, RaChunk = 32 * 1024;
    uint32_t u32();
    uint16_t u16();
    uint8_t u8();
    uint64_t u64();

private:
    FILE* f_ = nullptr;
    std::string path_;
    uint64_t pos_ = 0, size_ = 0;
    uint64_t bufStart_ = 0;
    size_t bufLen_ = 0;
    static constexpr size_t Buf = 16384;
    uint8_t* buf_ = nullptr;
    // the read-ahead: its ring and file in a block of their own (decoder.cpp), under the read-ahead's lock; a Reader that closes
    // lets go of it at once and the read-ahead thread closes its file and frees it, after its own read if one is under way: a
    // close never waits on the card (it waited, the sound's thread holding the engine's lock through a card's stall, and the
    // player stopped answering: the user's console, 2026-11-22)
    RaState* ra_ = nullptr;
    size_t takeFromRing(uint8_t* dst, size_t n);
};

/** Mono, 3+ channels and 8/24/32-bit all come out as interleaved stereo s16. */
void toStereo(const int16_t* in, int frames, int channels, int16_t* out);
