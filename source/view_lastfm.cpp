/*
 * Songate · view_lastfm.cpp
 * Settings › Last.fm's side of the app (the worker is lastfm.cpp): the prompt before anything goes online, what counts as played,
 * and the sign-in page's QR code.
 *
 * Played: the time heard while a track is the current one, as Stats counts it, so pause and seek add nothing; during a crossfade
 * or an Automix join it counts for the outgoing track until the switch, then for the incoming one. While the background player
 * plays, it does the counting (bg-scrobbles.txt).
 */
#include "words.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>

#include "app.hpp"
#include "icons.inc"
extern "C" {
#include "qrcodegen.h"
}

void App::allowLastfm() {
    confirm(ICON_MUSIC, "Last.fm", "Scrobble to Last.fm?", "Last.fm, an account of your own",
            "Allow Songate to use Last.fm?",
            "Once you sign in, Songate sends Last.fm the artist, title, album, track number and length of what you play, and when you "
            "played it. Only tracks with tags, never your files' names. Last.fm sees " GF_YOUR_CONSOLE "'s IP address and shows your "
            "plays on your profile. You can turn this off at any time.",
            "Allow", [this] {
                store.lastfm = true;
                persistSoon(0.3f);
                toast("Last.fm allowed: sign in below");
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
}

bool App::lastfmSong(const Track& t, double duration, int64_t when, Lastfm::Song& out) const {
    if (!t.tagged || (t.fromName & 3) || t.title.empty() || t.artist.empty()) return false;   // tags only, never a file's name
    out.artist = t.artist;
    out.title = t.title;
    if (!(t.fromName & 4)) out.album = t.album;
    out.albumArtist = t.albumArtist;
    out.trackNo = t.trackNo;
    out.duration = int(duration + 0.5);
    out.when = when;
    return true;
}

/** One of the background player's, matched to the library. */
void App::lastfmHeard(const std::string& path, int64_t when) {
    auto it = pathIndex.find(path);
    if (it == pathIndex.end()) return;
    const Track& t = lib.tracks[size_t(it->second)];
    Lastfm::Song s;
    if (lastfmSong(t, t.duration, when > 0 ? when : int64_t(std::time(nullptr)), s)) lastfm.scrobble(std::move(s));
}

void App::lastfmTick() {
    lastfm.set(store.lastfm, store.lastfmScrobble, store.lastfmNow, player.remote());
    if (lfmQr && !lastfm.waitingForSignIn()) {   // here, between frames: never while a frame may still draw it
        SDL_DestroyTexture(lfmQr);
        lfmQr = nullptr;
    }
    if (now.track >= 0 && now.track < int(lib.tracks.size())) {
        const Track& t = lib.tracks[size_t(now.track)];
        const double dur = now.info.duration > 0 ? now.info.duration : t.duration;
        if (!lfmSaid && isPlaying() && !now.loading && t.tagged) {
            lfmSaid = true;
            Lastfm::Song s;
            if (lastfm.announcing() && lastfmSong(t, dur, 0, s)) lastfm.nowPlaying(std::move(s));
        }
        if (!player.remote() && isPlaying() && !lfmDone) {
            if (lfmHeard <= 0) lfmBegan = int64_t(std::time(nullptr));
            lfmHeard += dt;
            const double need = Lastfm::threshold(dur);
            if (need > 0 && lfmHeard >= need) {
                lfmDone = true;
                Lastfm::Song s;
                if (lastfm.keeping() && lastfmSong(t, dur, lfmBegan, s)) lastfm.scrobble(std::move(s));
            }
        }
    }
    if (time < lfmBgAt) return;
    lfmBgAt = time + 1;
    for (const std::string& l : lastfm.takeLog()) logLine(l);
    auto found = lastfm.takeBackground();
    if (!lastfm.keeping()) {   // nothing is kept while Last.fm is off
        lfmBgWait.clear();
        return;
    }
    lfmBgWait.insert(lfmBgWait.end(), found.begin(), found.end());
    if (lfmBgWait.empty() || lib.tracks.empty() || meta.taggedCount() < int(lib.tracks.size())) return;   // the library first
    for (const auto& h : lfmBgWait) lastfmHeard(h.first, h.second);
    lfmBgWait.clear();
}

SDL_Texture* App::lastfmCode(int& side) {
    if (!lfmQr) {
        const Lastfm::View v = lastfm.view();
        static uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(15)], work[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
        if (v.url.empty() || !qrcodegen_encodeText(v.url.c_str(), work, code, qrcodegen_Ecc_MEDIUM, 1, 15, qrcodegen_Mask_AUTO, true)) return nullptr;
        constexpr int kQuiet = 4;   // the white margin a reader needs
        const int n = qrcodegen_getSize(code), full = n + kQuiet * 2;
        std::vector<uint32_t> px(size_t(full) * size_t(full), 0xFFFFFFFFu);
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (qrcodegen_getModule(code, x, y)) px[size_t(y + kQuiet) * size_t(full) + size_t(x + kQuiet)] = 0xFF000000u;
        lfmQr = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, full, full);
        if (!lfmQr) return nullptr;
        SDL_UpdateTexture(lfmQr, nullptr, px.data(), full * 4);
        SDL_SetTextureScaleMode(lfmQr, SDL_ScaleModeNearest);   // each module sharp
        lfmQrSide = full;
    }
    side = lfmQrSide;
    return lfmQr;
}
