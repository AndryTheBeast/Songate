/*
 * Gatefold NX · tools/bgtest/join/bgfade.cpp (its stand-ins are bgjoin.cpp's: made by a script, session 18)
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

// ---------------------------------------------------------------------------------------------------------------- bgfade
// bgfade <a> <b> <out.wav> [fade out seconds] [fade in seconds]
// a plays from 0:10; 3 s later a game event starts b (the queue replaced and PlayFade asked, as Engine::playPlaylist and
// playRandomFolder end). Printed: every 250 ms the output's level (RMS, dB) and the file loaded. Before 2.0.1 a was cut where it
// stood; now it fades out over Fade out, and b starts in the silence and fades in over Fade in.
#include <cmath>
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: bgfade <a> <b> <out.wav> [fade out] [fade in]\n"); return 2; }
    const std::string pa = argv[1], pb = argv[2], out = argv[3];
    const float fadeOut = argc > 4 ? float(atof(argv[4])) : 2.f, fadeIn = argc > 5 ? float(atof(argv[5])) : 1.5f;
    {
        const std::u8string u = (fs::u8path(out).parent_path() / "bgfade-card").u8string();
        g_card.assign(u.begin(), u.end());
    }
    fs::remove_all(fs::u8path(g_card));
    fs::create_directories(fs::u8path(g_card) / "config" / "songate-nx");
    Engine e;
    mutexInit(&e.m_);
    e.fader_.init(GFBG_RATE, 0);
    GfbgConfig c = e.config();
    c.crossfade = 0;
    c.fadeOut = fadeOut;
    c.fadeIn = fadeIn;
    e.setConfig(c);
    GfbgEq eq = e.eq();
    eq.on = 0;
    e.setEq(eq);
    e.setVolume(1);
    e.vol_ = 1;
    auto queue = [&](const std::string& path, const char* name) {
        std::vector<u8> q(sizeof(GfbgQueueHeader) + 4);
        GfbgQueueHeader h = {};
        h.count = 1;
        snprintf(h.name, sizeof h.name, "%s", name);
        const std::string paths = path + '\0';
        h.pathBytes = u32(paths.size());
        memcpy(q.data(), &h, sizeof h);
        const s32 order[1] = { 0 };
        memcpy(q.data() + sizeof h, order, 4);
        q.insert(q.end(), paths.begin(), paths.end());
        return e.setQueue(q.data(), q.size(), 1);
    };
    if (!queue(pa, "Before the game")) { fprintf(stderr, "queue refused\n"); return 1; }
    e.playAt(0, false, 10, 1);

    std::vector<int16_t> pcm;
    s16 buf[1536 * 2];
    u64 frames = 0, nextLook = GFBG_RATE / 4;
    double sum = 0;
    u64 count = 0;
    bool asked = false;
    const u64 total = u64((3 + fadeOut + fadeIn + 3) * GFBG_RATE);
    printf("fade out %.1f s, fade in %.1f s; the event comes at 3.00 s\n", double(fadeOut), double(fadeIn));
    while (frames < total) {
        g_ticks = frames * 19200000ull / GFBG_RATE;
        if (!asked && frames >= 3 * u64(GFBG_RATE)) {   // the game event: as playPlaylist does it
            asked = true;
            Lock l(&e.m_);
            queue(pb, "The game's playlist");
            e.cmds_.push_back({ Engine::Cmd::PlayFade, 0, false, double(e.cfg_.fadeIn), 0, 0 });
        }
        {
            Lock l(&e.m_);
            e.runCommands();
        }
        const int n = e.produce(buf, 1536);
        if (n <= 0) { fprintf(stderr, "silence at %.2f s\n", double(frames) / GFBG_RATE); break; }
        for (int i = 0; i < n * 2; i++) { sum += double(buf[i]) * buf[i]; count++; }
        pcm.insert(pcm.end(), buf, buf + n * 2);
        frames += u64(n);
        if (frames >= nextLook) {
            const double rms = count ? std::sqrt(sum / double(count)) / 32768.0 : 0;
            const std::string name = fs::u8path(e.playing_).filename().string();
            printf("%5.2f s  %6.1f dB  %s\n", double(frames) / GFBG_RATE, rms > 0 ? 20 * std::log10(rms) : -99.0, name.c_str());
            sum = 0;
            count = 0;
            nextLook += GFBG_RATE / 4;
        }
    }
    wav(out, pcm, GFBG_RATE);
    return 0;
}
