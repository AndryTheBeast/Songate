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

/** Settings › Library › Subsonic server's Sign in: its address, your user, your password (hidden as typed), one after another. Only a
 *  token made from the password is kept (subsonic.hpp); the password itself is wiped here. */
void App::subsonicSignIn() {
    const Subsonic::View v = subsonic.view();
    char server[256] = {}, user[128] = {}, pw[256] = {};
    plat::TextResult r = plat::askText("Subsonic server", "Its address, as http://nas.local:4533", v.server.c_str(), server, sizeof server);
    if (r == plat::TextResult::Unavailable) { toast("The keyboard is not available in applet mode"); return; }
    if (r != plat::TextResult::Done || !*server) return;
    if (plat::askText("Subsonic server", "Your username on it", v.user.c_str(), user, sizeof user) != plat::TextResult::Done || !*user) return;
    plat::textSecret() = true;
    r = plat::askText("Subsonic server", "Your password: only a token made from it is kept", "", pw, sizeof pw);
    plat::textSecret() = false;
    std::string p = pw;
    volatile char* w = pw;
    for (size_t i = 0; i < sizeof pw; i++) w[i] = 0;
    if (r != plat::TextResult::Done || p.empty()) return;
    subsonic.signIn(server, user, p);   // (p is wiped there)
}
#include "icons.inc"
extern "C" {
#include "qrcodegen.h"
}

void App::allowLastfm() {
    confirm(ICON_MUSIC, "Last.fm", "Scrobble to Last.fm?", "Last.fm, an account of your own",
            "Allow Songate to use Last.fm?",
            "Once you sign in, Songate sends Last.fm the artist, title, album, track number and length of what you play, and when you "
            "played it. For Mood playlists it also asks for each song's tags, by its artist and title, once. Only tracks with tags, never "
            "your files' names. Last.fm sees " GF_YOUR_CONSOLE "'s IP address and shows your plays on your profile. You can turn this off "
            "at any time.",
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
    // listening together: only the device that started it tells Last.fm (the user, 2026-11-07: "only the host send info to last.fm");
    // a song heard here in a session is never counted here, also when the session ends part way through it
    if (togetherGuest()) lfmSaid = lfmDone = true;
    if (srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size())) {   // a server's song: its tags (the user, 2026-11-07: "Last.fm
        // scrobbing is not working while streaming server music"; it counted nothing, or the last song of yours)
        const Subsonic::Song& so = srvq_.songs[size_t(srvq_.pos)];
        auto songOf = [&](int64_t when, Lastfm::Song& out) {
            if (so.title.empty() || so.artist.empty()) return false;
            out.artist = so.artist;
            out.title = so.title;
            out.album = so.album;
            out.trackNo = so.track;
            out.duration = int((now.info.duration > 0 ? now.info.duration : so.duration) + 0.5);
            out.when = when;
            return true;
        };
        const bool loaded = player.loaded() && srvq_.waitFor.empty();
        if (!lfmSaid && isPlaying() && loaded) {
            lfmSaid = true;
            Lastfm::Song s;
            if (lastfm.announcing() && songOf(0, s)) lastfm.nowPlaying(std::move(s));
        }
        if (isPlaying() && loaded && !lfmDone) {
            if (lfmHeard <= 0) lfmBegan = int64_t(std::time(nullptr));
            lfmHeard += dt;
            const double need = Lastfm::threshold(now.info.duration > 0 ? now.info.duration : so.duration);
            if (need > 0 && lfmHeard >= need) {
                lfmDone = true;
                Lastfm::Song s;
                if (lastfm.keeping() && songOf(lfmBegan, s)) lastfm.scrobble(std::move(s));
            }
        }
    } else if (now.track >= 0 && now.track < int(lib.tracks.size())) {
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
