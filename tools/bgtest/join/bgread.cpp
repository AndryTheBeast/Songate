/*
 * Songate · tools/bgtest/join/bgread.cpp
 * The background player's read-ahead (decoder.cpp, readAheadPass), checked on the PC: each file decoded whole three ways,
 * the sound compared bit for bit:
 *   plain   no read-ahead thread: every read goes to the file, as before the read-ahead
 *   ahead   a read-ahead thread keeping the ring filled, the decoder reading at its own pace
 *   seeks   the same with seeks (30 %, back to 10 %, 70 %), against the plain decode with the same seeks
 * then 40 opens and closes while the thread reads (a close waits for its block), and a stalled thread (2 s) the decoder
 * reads through from the ring.
 *   bgread <file> [<file> ...]
 */
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>
#include <switch.h>

#include "decoder.hpp"

namespace fs = std::filesystem;

extern "C" size_t gfHeapSize;
size_t gfHeapSize = 0x180000;
u64 g_ticks = 0;

static std::atomic<size_t> g_used{ 600 * 1024 };   // (a playing song, its tables and buffers: room for a whole ring)
struct mallinfo mallinfo_() { return { g_used.load() }; }
FILE* hostFopen(const char* p, const char* m) { return _wfopen(fs::u8path(p).wstring().c_str(), fs::u8path(m).wstring().c_str()); }
int hostStat(const char*, struct stat*) { return -1; }
int hostMkdir(const char*, int) { return -1; }
int hostRemove(const char*) { return -1; }
int hostRename(const char*, const char*) { return -1; }

extern std::atomic<int> raTestStallMs;   // (decoder.cpp, built with GF_RATEST)
static std::atomic<bool> pumpOn{ false }, pumpQuit{ false }, pumpStall{ false };
static std::atomic<long> passes{ 0 };

static void pump() {
    while (!pumpQuit) {
        if (!pumpOn || pumpStall) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        if (readAheadPass()) passes++;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static uint64_t fnv(uint64_t h, const int16_t* p, size_t n) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n * 2; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/** The whole file decoded (with seeks at those fractions of its length, after that many reads), its sound's hash. */
static uint64_t decode(const std::string& path, bool ahead, bool seeks, bool slow, long* frames) {
    pumpOn = ahead;
    auto d = openDecoder(path.c_str());
    if (!d) { printf("  cannot open\n"); return 0; }
    static int16_t buf[1024 * 2];
    uint64_t h = 1469598103934665603ull;
    long n = 0, reads = 0;
    const double at[] = { .3, .1, .7 };
    int next = 0;
    for (;;) {
        if (seeks && next < 3 && reads == 200 + next * 300) { d->seek(d->duration * at[next]); h = fnv(h, nullptr, 0) ^ uint64_t(next + 1); next++; }
        const int r = d->read(buf, 1024);
        if (r <= 0) break;
        h = fnv(h, buf, size_t(r) * 2);
        n += r;
        reads++;
        if (slow && reads % 40 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    *frames = n;
    d.reset();
    pumpOn = false;
    return h;
}

int main(int argc, char** argv) {
    std::thread t(pump);
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        const std::string p = argv[i];
        printf("%s\n", fs::u8path(p).filename().u8string().c_str());
        long f1 = 0, f2 = 0, f3 = 0, f4 = 0;
        const uint64_t a = decode(p, false, false, false, &f1);
        passes = 0;
        const uint64_t b = decode(p, true, false, true, &f2);
        const long pa = passes;
        const uint64_t c = decode(p, false, true, false, &f3);
        const uint64_t e = decode(p, true, true, true, &f4);
        printf("  plain %016llx %ld frames\n  ahead %016llx %ld frames (%ld blocks read ahead) %s\n  seeks %016llx / %016llx %s\n",
               (unsigned long long)a, f1, (unsigned long long)b, f2, pa, a == b && f1 == f2 ? "SAME" : "DIFFERENT",
               (unsigned long long)c, (unsigned long long)e, c == e && f3 == f4 ? "SAME" : "DIFFERENT");
        bad += !(a == b && f1 == f2 && c == e && f3 == f4 && pa > 0);
        // no ring free when it opens (every one of the pool's held by other songs) and one given to it halfway (those songs
        // gone): the same sound
        {
            pumpOn = true;
            std::vector<std::unique_ptr<Decoder>> others;
            for (int i = 0; i < Reader::RaSlots; i++) others.push_back(openDecoder(p.c_str()));
            auto d = openDecoder(p.c_str());
            static int16_t buf[2048];
            uint64_t h = 1469598103934665603ull;
            long n = 0, reads = 0;
            for (;;) {
                if (reads == 400) others.clear();
                const int r = d->read(buf, 1024);
                if (r <= 0) break;
                h = fnv(h, buf, size_t(r) * 2);
                n += r;
                if (++reads % 40 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            d.reset();
            pumpOn = false;
            g_used = 600 * 1024;
            printf("  grown   %016llx %ld frames %s\n", (unsigned long long)h, n, h == a && n == f1 ? "SAME" : "DIFFERENT");
            bad += !(h == a && n == f1);
        }
        // opens and closes while the thread reads
        pumpOn = true;
        for (int k = 0; k < 40; k++) {
            auto d = openDecoder(p.c_str());
            int16_t buf[2048];
            for (int j = 0; j < (k % 7) * 5; j++) d->read(buf, 1024);
            std::this_thread::sleep_for(std::chrono::microseconds(300 * (k % 5)));
        }
        pumpOn = false;
        // a stall: the ring filled, then the thread held up 2 s while the decoder reads on (as the card's stall on the console)
        {
            pumpOn = true;
            auto d = openDecoder(p.c_str());
            int16_t buf[2048];
            d->read(buf, 1024);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));   // (the ring fills)
            pumpStall = true;
            long got = 0;
            const auto s0 = std::chrono::steady_clock::now();
            for (int j = 0; j < 400; j++) { const int r = d->read(buf, 1024); if (r <= 0) break; got += r; }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
            pumpStall = false;
            printf("  stalled thread: %.1f s of sound read from the ring in %.1f ms\n", got / double(d->rate), ms);
            d.reset();
            pumpOn = false;
        }
        // a song closed while the read-ahead is held up 3 s mid-read (the card's stall at a join's end): the close returns at
        // once, and its ring is freed by the read-ahead thread once its read ends
        {
            raTestStallMs = 3000;   // (from its first read: the ring's very first block is the one held up)
            const long passes0 = passes;
            pumpOn = true;
            auto d = openDecoder(p.c_str());
            int16_t buf[2048];
            d->read(buf, 1024);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));   // (the thread now inside a held-up read)
            if (passes != passes0) printf("  (a pass finished early: the read was not held)\n");
            const auto s0 = std::chrono::steady_clock::now();
            d.reset();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
            raTestStallMs = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(3200));   // (its read ends; the next pass frees it)
            pumpOn = false;
            printf("  closed mid-stall: the close took %.1f ms %s\n", ms, ms < 50 ? "SAME" : "DIFFERENT");
            bad += !(ms < 50);
        }
    }
    pumpQuit = true;
    t.join();
    printf(bad ? "FAILED %d\n" : "ALL SAME\n", bad);
    return bad;
}
