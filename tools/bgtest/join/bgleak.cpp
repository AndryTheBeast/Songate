/*
 * Gatefold NX · tools/bgtest/join/bgleak.cpp (its stand-ins are bgjoin.cpp's: made by a script, session 18)
 * Automix in the background player (protocol 6), checked on the PC: the sysmodule's own engine.cpp, decoders and resampler
 * play an Automix playlist of two tracks through their join, as the console would, into a WAV. The audio thread's work
 * (commands, produce) and the watch thread's (watchAutomix, 4 a second) are run in turn on the test's clock.
 *
 *   bgjoin <music root> <a> <b> <out.wav> <analysis.tsv> [model 0|1] [pre seconds]
 * a and b are paths under the music root (queued as PC paths); analysis.tsv has "path<TAB>…<TAB>mixan::Info::pack()" lines (mixtrain's cache)
 * or "path<TAB>pack" (mix.tsv) for them. The card (sdmc:/) is a scratch folder next to out.wav; the music is read where it
 * is. The output starts `pre` seconds (12) before the join, as `mixtest --render` does, so the two can be compared.
 */
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <mutex>
#include <switch.h>

#include "engine.hpp"   // (pre.h opens its private part to the test)

namespace fs = std::filesystem;

extern "C" size_t gfHeapSize;
size_t gfHeapSize = 0x180000;
u64 g_ticks = 0;
static std::string g_card, g_music;
static size_t g_used = 0;

struct mallinfo mallinfo_() { return { g_used }; }

struct Lock {   // (engine.cpp's own is in its unnamed namespace)
    Mutex* m;
    explicit Lock(Mutex* x) : m(x) { mutexLock(m); }
    ~Lock() { mutexUnlock(m); }
};

static std::string host(const char* p) {
    std::string s = p;
    if (s.rfind("sdmc:/Music/", 0) == 0) return g_music + "/" + s.substr(12);
    if (s.rfind("sdmc:/", 0) == 0) return g_card + "/" + s.substr(6);
    return s;
}
FILE* hostFopen(const char* p, const char* m) {
    const std::string h = host(p);
    return _wfopen(fs::u8path(h).wstring().c_str(), fs::u8path(m).wstring().c_str());
}
int hostStat(const char* p, struct stat* s) {
    std::error_code e;
    const fs::path f = fs::u8path(host(p));
    const auto st = fs::status(f, e);
    if (e || !fs::exists(st)) return -1;
    memset(s, 0, sizeof *s);
    s->st_mode = fs::is_directory(st) ? _S_IFDIR : _S_IFREG;
    if (!fs::is_directory(st)) s->st_size = long(fs::file_size(f, e));
    return 0;
}
int hostMkdir(const char* p, int) { std::error_code e; fs::create_directories(fs::u8path(host(p)), e); return e ? -1 : 0; }
int hostRemove(const char* p) { std::error_code e; return (fs::remove)(fs::u8path(host(p)), e) ? 0 : -1; }
int hostRename(const char* a, const char* b) { std::error_code e; (fs::rename)(fs::u8path(host(a)), fs::u8path(host(b)), e); return e ? -1 : 0; }

static void wav(const std::string& path, const std::vector<int16_t>& s, int rate) {
    std::ofstream f(fs::u8path(path), std::ios::binary);
    const uint32_t data = uint32_t(s.size() * 2), riff = 36 + data, fmt = 16, br = uint32_t(rate) * 4;
    const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    f.write("RIFF", 4); f.write((const char*)&riff, 4); f.write("WAVEfmt ", 8); f.write((const char*)&fmt, 4);
    f.write((const char*)&pcm, 2); f.write((const char*)&ch, 2); f.write((const char*)&rate, 4); f.write((const char*)&br, 4);
    f.write((const char*)&align, 2); f.write((const char*)&bits, 2); f.write("data", 4); f.write((const char*)&data, 4);
    f.write((const char*)s.data(), std::streamsize(s.size() * 2));
}

// ---------------------------------------------------------------------------------------------------------------- bgleak
// bgleak <rounds> <file> <file> [<file> ...]      (songs of any of the player's formats, PC paths)
// Each round: a queue of all the files (every other round an Automix queue), then for each file: started at some place, played
// a little, sought, played, skipped, paused and resumed, and a game event's start (PlayFade) into the next; the watch thread's
// Automix work 4 times a second of music; then stopped. Every tenth of the rounds: the heap's used bytes and blocks.
#include <malloc.h>
static void heapUsed(size_t& bytes, size_t& blocks) {
    bytes = blocks = 0;
    _HEAPINFO h;
    h._pentry = nullptr;
    while (_heapwalk(&h) == _HEAPOK)
        if (h._useflag == _USEDENTRY) { bytes += h._size; blocks++; }
}
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: bgleak <rounds> <file> <file> ...\n"); return 2; }
    const int rounds = atoi(argv[1]);
    std::vector<std::string> files;
    for (int i = 2; i < argc; i++) files.push_back(argv[i]);
    {
        const std::u8string u = (fs::temp_directory_path() / "bgleak-card").u8string();
        g_card.assign(u.begin(), u.end());
    }
    fs::remove_all(fs::u8path(g_card));
    fs::create_directories(fs::u8path(g_card) / "config" / "songate-nx");
    auto* ep = new Engine;
    Engine& e = *ep;
    mutexInit(&e.m_);
    e.fader_.init(GFBG_RATE, 0);
    GfbgConfig c = e.config();
    c.crossfade = 2;
    c.fadeOut = 0.2f;
    c.fadeIn = 0.2f;
    e.setConfig(c);
    e.setVolume(1);
    s16 buf[1536 * 2];
    u64 frames = 0, nextWatch = 0;
    auto run = [&](double seconds) {
        for (u64 end = frames + u64(seconds * GFBG_RATE); frames < end;) {
            g_ticks = frames * 19200000ull / GFBG_RATE;
            {
                Lock l(&e.m_);
                e.runCommands();
            }
            if (frames >= nextWatch) { e.watchAutomix(); nextWatch = frames + GFBG_RATE / 4; }
            const int n = e.produce(buf, 1536);
            frames += u64(n > 0 ? n : 1536);
        }
    };
    auto cmd = [&](Engine::Cmd k) { Lock l(&e.m_); e.cmds_.push_back(k); };
    size_t b0 = 0, n0 = 0;
    for (int r = 0; r <= rounds; r++) {
        if (r % std::max(1, rounds / 10) == 0) {
            size_t b, n;
            heapUsed(b, n);
            if (r == std::max(1, rounds / 10)) { b0 = b; n0 = n; }   // (after the first tenth: everything that grows once has grown)
            printf("round %5d: heap %9zu bytes in %6zu blocks", r, b, n);
            if (r > std::max(1, rounds / 10)) printf("   since round %d: %+lld bytes, %+lld blocks", std::max(1, rounds / 10), (long long)b - (long long)b0, (long long)n - (long long)n0);
            printf("\n");
            fflush(stdout);
        }
        if (r == rounds) break;
        std::vector<u8> q(sizeof(GfbgQueueHeader) + files.size() * 4);
        GfbgQueueHeader h = {};
        h.count = u32(files.size());
        h.flags = r % 2 ? GfbgQueue_Automix : 0;
        snprintf(h.name, sizeof h.name, "Round %d", r);
        std::string paths;
        for (const auto& f : files) paths += f + '\0';
        h.pathBytes = u32(paths.size());
        memcpy(q.data(), &h, sizeof h);
        for (size_t i = 0; i < files.size(); i++) { const s32 o = s32(i); memcpy(q.data() + sizeof h + i * 4, &o, 4); }
        q.insert(q.end(), paths.begin(), paths.end());
        if (!e.setQueue(q.data(), q.size(), 1)) { fprintf(stderr, "queue refused in round %d\n", r); return 1; }
        for (size_t i = 0; i < files.size(); i++) {
            e.playAt(s32(i), false, 5.0 + double((r * 7 + int(i) * 13) % 40), 1);
            run(0.25);
            cmd({ Engine::Cmd::Seek, 0, false, 20.0 + double(r % 30), 1 });
            run(0.15);
            cmd({ Engine::Cmd::Next, 0, false, 0, 1 });
            run(0.2);
            cmd({ Engine::Cmd::Toggle, 0, false, 0, 1 });
            run(0.1);
            cmd({ Engine::Cmd::Toggle, 0, false, 0, 1 });
            run(0.1);
            cmd({ Engine::Cmd::PlayFade, s32((i + 1) % files.size()), false, double(e.cfg_.fadeIn), 0, 0 });
            run(0.6);
        }
        cmd({ Engine::Cmd::Stop, 0, false, 0, 0 });
        run(0.05);
    }
    return 0;
}
