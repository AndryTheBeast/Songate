/*
 * Songate · otherapps.cpp
 * Other apps' music (Android; Settings › Playback › Other apps; the user, 2026-11-18): what another app plays, seen through
 * Android's media sessions (OtherApps.java, once Notification access allows it), shown as an outside source (extOn, as AirPlay's):
 * its song, artist, album, cover and place, its own buttons pressed from Songate (play, pause, skip, seek), what Songate cannot do
 * there greyed (the heart, Up next, the equalizer, the order). Songate's own music first: shown only while Songate's is paused.
 * Lyrics: found online as any song's, never kept (lyrics_online.cpp: "app:" songs).
 */
#include "app.hpp"
#include "icons.inc"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef GF_ANDROID

namespace {
std::string field(const std::string& s, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t at = s.rfind("\n" + k);
    at = s.compare(0, k.size(), k) == 0 ? 0 : at == std::string::npos ? std::string::npos : at + 1;
    if (at == std::string::npos) return {};
    const size_t end = s.find('\n', at);
    return s.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
}
}  // namespace

void App::otherTick() {
    const bool want = store.otherApps;
    if (want != otherWatching_) {
        if (!plat::otherWatch(want)) return;   // (Android's side not there yet: the next frame asks again)
        otherWatching_ = want;
        otherSeen_ = ~0u;
        SDL_Log("Other apps: %s", want ? "on (Settings › Playback › Other apps)" : "off");
    }
    if (!want) { if (extApp_) otherEnd(); return; }
    // read again when the app says something changed, and once a second (its place)
    const unsigned gen = plat::otherGen();
    if (gen == otherSeen_ && time - otherAt_ < 1) return;
    otherSeen_ = gen;
    otherAt_ = time;
    std::string st = plat::otherState();
#ifdef GF_DEV
    // (dev builds: <config>/mock-other.txt stands in for another app's session, its "key=value" lines as OtherApps.state() gives
    // them; for the emulator's tests, where no music app can play without its maker's sign-in)
    if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-other.txt").c_str(), "rb")) {
        char buf[2048];
        const size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        st.assign(buf, n);
        otherAt_ = time - 0.9;   // (read again soon: the test changes it)
    }
#endif
    const std::string title = field(st, "title");
    // not shown: nothing playing elsewhere, Songate's own music playing (it comes first), or another outside source (AirPlay, a
    // server's queue) on screen. And it takes over only once it plays (the user, 2026-11-19, through the SongateOS session: a song of
    // yours paused stays; another app's paused or stopped session never takes its place). Once shown it stays while paused, so
    // its own Play is here
    const bool otherPlays = field(st, "playing") == "1";
    if (title.empty() || isPlaying() || srvq_.on || (extOn && !extApp_) || (!extApp_ && !otherPlays)) {
        const char* why = title.empty() ? (st.empty() ? "no session" : "no title") : isPlaying() ? "Songate's own music plays"
                        : srvq_.on ? "a server's queue" : extOn && !extApp_ ? "another outside source" : "it is not playing";
        if (why != otherWhy_) { otherWhy_ = why; SDL_Log("Other apps: not shown (%s)", why); }
        if (extApp_) otherEnd();
        return;
    }
    otherWhy_ = nullptr;
    const std::string pkg = field(st, "pkg"), label = field(st, "label"), artist = field(st, "artist"), album = field(st, "album");
    const double duration = atof(field(st, "duration").c_str()) / 1000.0, pos = atof(field(st, "pos").c_str()) / 1000.0;
    if (!extApp_) {
        extBegin(label);
        extApp_ = true;
        now.info.codec = label.empty() ? pkg : label;   // (Android does not say the format: the app's name in the chip; the user, 2026-11-18)
        now.info.bitsPerSample = 0;
        now.info.sampleRate = 0;
        SDL_Log("Other apps: %s shown", pkg.c_str());
    }
    extFrom = label.empty() ? pkg : label;
    now.info.codec = extFrom;
    extPlaying_ = otherPlays;
    extActions_ = atoll(field(st, "actions").c_str());
    extLike_ = field(st, "like") == "1";
    if (const bool liked = field(st, "liked") == "1"; liked == extLiked_ || time > extLikeHold_) { extLiked_ = liked; extLikeHold_ = 0; }
    const std::string key = artist + "\x1f" + title;
    // Stop after this song: the app went on to its next one by itself near the end of the last (updateSleep's watch missed it):
    // paused now (a skip earlier in the song is the user's own, left playing)
    if (key != extKey_ && !extKey_.empty() && stopAfterCurrent && extPlaying_ && otherPlays && now.info.duration > 1 &&
        shownPosition() >= now.info.duration - 2.5) {
        extControl(2);
        extPlaying_ = false;
        stopAfterCurrent = false;
        SDL_Log("Other apps: paused after the song (stop after this song)");
    }
    if (key != extKey_) {   // another song: its words, no lyrics yet (looked up online: lyrics_online.cpp)
        extKey_ = key;
        extInfo(title, artist, album);
        now.lyrics.clear();
        now.synced = false;
        lyrOn = LyrOnline{};
    }
    const std::string art = field(st, "art");
    if (art != extArt_) { extArt_ = art; extCover(plat::otherArt()); }
    if (pkg != extPkg_) {   // its app's icon: the badge on the player bar's cover, the chip on Now playing's
        extPkg_ = pkg;
        if (extIcon_) { SDL_DestroyTexture(extIcon_); extIcon_ = nullptr; }
        std::vector<uint32_t> px;
        if (plat::otherIcon(48, px) && (extIcon_ = SDL_CreateTexture(g.renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 48, 48))) {
            SDL_UpdateTexture(extIcon_, nullptr, px.data(), 48 * 4);
            SDL_SetTextureBlendMode(extIcon_, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(extIcon_, SDL_ScaleModeLinear);
        }
    }
    extProgress(pos, duration);
}

void App::otherEnd() {
    if (!extApp_) return;
    SDL_Log("Other apps: no longer shown");
    extApp_ = false;
    extPlaying_ = false;
    extLike_ = extLiked_ = false;
    extLikeHold_ = 0;
    extKey_.clear();
    extArt_.clear();
    if (otherVizOn_) { plat::otherVizStop(); otherVizOn_ = false; }
    lyrOn = LyrOnline{};
    now.lyrics.clear();
    extEnd();
}

void App::seekShown(double sec) {
    if (extApp_) {
        plat::otherControl(5, sec * 1000);
        extProgress(std::max(0.0, sec), now.info.duration);   // (shown there at once; the app's own word follows)
        return;
    }
    player.seek(sec);
}

void App::extControl(int cmd) { plat::otherControl(cmd, 0); }

namespace {
double recAt = -10;
bool recOk = false;
}  // namespace

bool App::otherVizFeed(float* pcm, int& rate) {
    // Android's audio recording permission, read once a second (and its dialog's answer: refused, the switch goes back off)
    if (time - recAt >= 1) {
        recAt = time;
        recOk = plat::recordAllowed();
        if (plat::recordAnswer() == 2 && !recOk && store.otherSpectrum) {
            store.otherSpectrum = false;
            persistSoon(0.3f);
            SDL_Log("Other apps: audio recording refused: the spectrum off");
        }
    }
    const int N = Spectrum::N;
#ifdef GF_DEV
    // (dev builds: a "viz=synth" line in mock-other.txt stands in for Android's output: a beat and a few tones that move, for the
    // emulator, where no music app plays)
    {
        static double synthAt = -10;
        static bool synth = false;
        if (time - synthAt >= 1) {
            synthAt = time;
            synth = false;
            if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-other.txt").c_str(), "rb")) {
                char buf[2048];
                const size_t n = fread(buf, 1, sizeof buf, f);
                fclose(f);
                synth = std::string(buf, n).find("viz=synth") != std::string::npos;
            }
        }
        if (synth && extApp_ && store.otherSpectrum) {
            otherWave_.resize(1024);
            const double t0 = time;
            const float kick = float(std::pow(std::max(0.0, 1 - std::fmod(t0, 0.5) / 0.25), 2.0));
            for (int i = 0; i < 1024; i++) {
                const double t = t0 + i / 44100.0;
                const float v = 0.5f * kick * float(std::sin(2 * M_PI * 60 * t)) + 0.2f * float(std::sin(2 * M_PI * (300 + 200 * std::sin(t0)) * t)) +
                                0.1f * float(std::sin(2 * M_PI * 2500 * t)) + 0.05f * float(std::sin(2 * M_PI * 7000 * t));
                otherWave_[size_t(i)] = uint8_t(std::clamp(int(128 + v * 127), 0, 255));
            }
            std::fill(pcm, pcm + N, 0.f);
            for (int i = 0; i < 1024; i++) pcm[(N - 1024) / 2 + i] = (int(otherWave_[size_t(i)]) - 128) / 128.f * 2.f;
            rate = 44100;
            return true;
        }
    }
#endif
    const bool want = extApp_ && store.otherSpectrum && recOk;
    if (!want) {
        if (otherVizOn_) { plat::otherVizStop(); otherVizOn_ = false; }
        return false;
    }
    int hz = 0;
    if (!plat::otherViz(otherWave_, hz)) return false;
    otherVizOn_ = true;
    // 8-bit, 128 silence: placed in the middle of the analysis window, silence around it, twice as loud for the window's share
    // (Spectrum's Hann over N: the slice sits where it is near 1, where the whole window averages 0.5)
    const int n = std::min(int(otherWave_.size()), N);
    std::fill(pcm, pcm + N, 0.f);
    for (int i = 0; i < n; i++) pcm[(N - n) / 2 + i] = (int(otherWave_[size_t(i)]) - 128) / 128.f * 2.f;
    rate = hz;
    return true;
}

void App::otherSpecAllow() {
    store.otherSpectrum = true;
    store.otherSpecAsked = true;
    persistSoon(0.3f);
    recAt = -10;   // (read again at once)
    if (!plat::recordAllowed()) plat::askRecord();   // (Android's own dialog; SongateOS may have allowed it already)
    SDL_Log("Other apps: spectrum on");
}

bool App::otherSpecAsk(float x, float y, float w, float h) {
    // Where the bars go, the first time another app's song shows (the user chose B, 2026-11-19): Spectrum for <app>, Allow or Not
    // now; either answer puts it away for good (the switch stays in Settings › Playback › Other apps)
    if (!extApp_ || store.otherSpectrum || store.otherSpecAsked || h < 34) return false;
    const Font bf{ css::Sm, Weight::SemiBold };
    const float allowW = 32 + g.measure("Allow", bf), notW = 32 + g.measure("Not now", bf);
    const float by = y + (h - 34) / 2;
    const float textX = x + 4 + 16 + 8, textW = w - (textX - x) - allowW - notW - 8 - 12;
    if (textW < 60) return false;
    g.icon(ICON_WAVE, x + 4 + 8, by + 17, 16, pal.text2);
    g.textC("Spectrum for " + extFrom, { css::Sm }, textX, by + 17, pal.text2, 1, Align::Left, int(textW));
    bool c = false;
    button(uid("ph.specnot"), nullptr, "Not now", x + w - allowW - 8 - notW, by, false, &c, false, false, 0, notW);
    if (c) { store.otherSpecAsked = true; persistSoon(0.3f); SDL_Log("Other apps: spectrum not now"); }
    button(uid("ph.specallow"), nullptr, "Allow", x + w - allowW, by, true, &c, false, false, 0, allowW);
    if (c) otherSpecAllow();
    return true;
}

bool App::extCan(long long action) const { return !extApp_ || extActions_ == 0 || (extActions_ & action) != 0; }

#else
void App::otherTick() {}
void App::otherEnd() {}
void App::seekShown(double sec) { player.seek(sec); }
bool App::extCan(long long) const { return true; }
void App::extControl(int) {}
bool App::otherVizFeed(float*, int&) { return false; }
bool App::otherSpecAsk(float, float, float, float) { return false; }
void App::otherSpecAllow() {}
#endif
