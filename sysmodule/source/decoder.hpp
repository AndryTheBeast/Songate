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
/** The dev build's log line for a read that took long (the engine's bgLog; null: none). */
extern void (*readAheadLog)(const char* fmt, ...);

// ---------------------------------------------------------------- shared helpers

struct RaState;   // (decoder.cpp)

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
    /** From here on its bytes come from a ring of RaMax that readAheadPass keeps filled ahead of where it reads (8 s of a
     *  256 kbit/s song: the console's card stalls run to 5 s); without room for one yet, it reads the card itself as before. */
    void startReadAhead();
    // RaReserve: what a ring leaves free (a join's echo, a track picked by hand fading in: each asks for about 320 KB);
    // RaMin: the least worth having (4 s of a 256 kbit/s song)
    static constexpr size_t RaMax = 256 * 1024, RaMin = 128 * 1024, RaReserve = 320 * 1024, RaBack = 32 * 1024, RaChunk = 32 * 1024;
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
    bool raWanted_ = false;    // asked for: without room then, tried again every 64 reads (the other song closed)
    bool raRefusedLogged_ = false;   // (the dev build's log has said it had no room)
    uint32_t raTries_ = 0;
    size_t takeFromRing(uint8_t* dst, size_t n);
    static size_t ringRoom();
    void growRing();
};

/** Mono, 3+ channels and 8/24/32-bit all come out as interleaved stereo s16. */
void toStereo(const int16_t* in, int frames, int channels, int16_t* out);
