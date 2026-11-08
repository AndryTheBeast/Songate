/*
 * Songate · view_server.cpp
 * The Server tab (BETA 3, item 8, stage 1b): a Subsonic server's music, as the library's pages show their own: its artists (round
 * tiles), an artist's albums (square tiles), an album's songs. What it shows is asked of the Subsonic worker and drawn as it comes;
 * covers are fetched and decoded there, and only made into textures here, a few a frame.
 */
#include <algorithm>
#include <cmath>
#include <random>

#include "app.hpp"
#include "tracksort.hpp"
#include "lyricsnet.hpp"
#include "words.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {

struct Grid { int cols; float tileW, tileH, x0; };
Grid gridOf(float x, float w) {   // (as the library's grids: view_grid.cpp)
    const float inner = w - 36;
    const int cols = std::max(2, int(inner / 200));
    const float tw = inner / cols;
    return { cols, tw, 10 + (tw - 20) + 8 + 20 + 2 + 18 + 10, x + 18 };
}

// the list views' sizes (view_tracks.cpp's)
constexpr float RowH = 40, CovRowH = 60, GroupH = 76, ColHeadH = 38;
struct Tiles { int cols; float w, cover, h; };
Tiles tilesFor(float w) {
    const float inner = w - 32;
    const int cols = std::max(2, int(inner / 180));
    const float tw = inner / cols, cover = tw - 16;
    return { cols, tw, cover, 8 + cover + 10 + 20 + 18 + 14 };
}
const char* const SortNames[] = { "By artist", "By title", "By album", "By year", "By genre", "By length" };

std::pair<std::string, std::string> idName(const std::string& q) {
    const size_t c = q.find('\x1f');
    return c == std::string::npos ? std::make_pair(q, std::string()) : std::make_pair(q.substr(0, c), q.substr(c + 1));
}

}  // namespace

// ------------------------------------------------------------------ the server's queue (stage 2)

/** Server songs, from `start`: a queue of their own, shown on Now playing as an outside source with working controls. The console's
 *  background player hands over to the app meanwhile (its sound never reaches Songate's mix), and gets it back after. */
void App::playServer(const std::vector<Subsonic::Song>& songs, int start, const std::string& name) {
    if (songs.empty()) return;
    libSwitchTo_ = -1;   // (a queue chosen: Library's switch waiting for the song playing to end is not wanted any more)
    if (tog.stage != Tog::Off) {   // a session: as a playlist of yours (the user, 2026-11-07: a server playlist said "Not while listening together")
        if (!togetherOn()) { toast("Not while listening together starts"); return; }
        if (!tog.host) { toast("The queue is " + tog.peerName + "'s for now: change it there"); return; }
        srvq_.songs = songs;   // the starter: its queue is this one now; the others hear of it first, then it plays (togetherPlayServer)
        srvq_.name = name;
        srvq_.on = true;
        srvq_.session = false;
        srvq_.pos = std::clamp(start, 0, int(songs.size()) - 1);
        togetherPlayServer(srvq_.pos);
        return;
    }
    if (!srvq_.on && now.track >= 0 && player.loaded()) localSavedAt_ = player.position();   // (Library's switch back finds it there)
    if (player.remote()) {
        bg.stop();
        bg.disconnect();
        player.setRemote(nullptr);
        srvq_.hadBackground = true;
    } else if (player.loaded()) {
        player.stop();
    }
    const Subsonic::View sv = subsonic.view();
    std::string kind = sv.kind.empty() ? std::string("your server") : sv.kind;
    kind[0] = char(toupper(uint8_t(kind[0])));
    srvq_.on = true;
    srvq_.songs = songs;
    srvq_.name = name;
    extBegin(kind);
    serverPlayAt(std::clamp(start, 0, int(songs.size()) - 1));
}

/** The server's queue for server-queue.tsv: "on <0|1>\tpos\tat\tname" (on: it was the one playing), then a song a line. "" none. */
std::string App::serverQueueText() {
    const ServerQueue& k = srvq_.on ? srvq_ : srvSaved_;
    if (k.songs.empty() || subsonic.view().state == Subsonic::SignedOut) return {};
    const double at = srvq_.on ? (player.loaded() && srvq_.waitFor.empty() ? player.position() : srvSeek_) : srvSavedAt_;
    auto clean = [](std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; };
    char head[96];
    snprintf(head, sizeof head, "%d\t%d\t%.2f\t", srvq_.on ? 1 : 0, k.pos, at);
    std::string out = head + clean(k.name) + "\n";
    for (const Subsonic::Song& s : k.songs) {
        char nums[96];
        snprintf(nums, sizeof nums, "%d\t%d\t%d\t%d\t%.2f", s.track, s.disc, s.year, s.bitRate, s.duration);
        out += clean(s.id) + "\t" + clean(s.title) + "\t" + clean(s.artist) + "\t" + clean(s.artistId) + "\t" + clean(s.album) + "\t" +
               clean(s.albumId) + "\t" + clean(s.cover) + "\t" + clean(s.suffix) + "\t" + clean(s.contentType) + "\t" + clean(s.genre) + "\t" +
               clean(s.parent) + "\t" + nums + "\n";
    }
    return out;
}

/** At launch: the server's queue of the last run, paused where it was when it was the one playing; else kept for Library's switch. */
void App::restoreServerQueue(bool mayPlay) {
    if (subsonic.view().state == Subsonic::SignedOut || tog.stage != Tog::Off) return;
    FILE* f = fopen(store.path("server-queue.tsv").c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[8192];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    std::vector<std::string> lines;
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        lines.push_back(text.substr(a, b - a));
        a = b + 1;
    }
    auto fields = [](const std::string& l) {
        std::vector<std::string> o;
        size_t a = 0;
        for (size_t b; (b = l.find('\t', a)) != std::string::npos; a = b + 1) o.push_back(l.substr(a, b - a));
        o.push_back(l.substr(a));
        return o;
    };
    if (lines.empty()) return;
    const std::vector<std::string> h = fields(lines[0]);
    if (h.size() < 4) return;
    ServerQueue k;
    const bool wasOn = atoi(h[0].c_str()) == 1;
    k.pos = atoi(h[1].c_str());
    const double at = atof(h[2].c_str());
    k.name = h[3];
    for (size_t i = 1; i < lines.size() && k.songs.size() < 20000; i++) {
        const std::vector<std::string> p = fields(lines[i]);
        if (p.size() < 16 || p[0].empty()) continue;
        Subsonic::Song s;
        s.id = p[0]; s.title = p[1]; s.artist = p[2]; s.artistId = p[3]; s.album = p[4]; s.albumId = p[5]; s.cover = p[6];
        s.suffix = p[7]; s.contentType = p[8]; s.genre = p[9]; s.parent = p[10];
        s.track = atoi(p[11].c_str()); s.disc = atoi(p[12].c_str()); s.year = atoi(p[13].c_str()); s.bitRate = atoi(p[14].c_str());
        s.duration = atof(p[15].c_str());
        k.songs.push_back(std::move(s));
    }
    if (k.songs.empty()) return;
    k.pos = std::clamp(k.pos, 0, int(k.songs.size()) - 1);
    if (wasOn && mayPlay) {   // it was the one playing: back, paused at its place (yours, restored before it, kept for Library's switch)
        srvLoadPaused_ = true;
        srvSeek_ = at;
        playServer(k.songs, k.pos, k.name);
    } else {
        srvSaved_ = k;
        srvSavedAt_ = at;
    }
}

std::string App::serverPrint() {
    const Subsonic::View sv = subsonic.view();
    if (sv.state != Subsonic::SignedIn || sv.server.empty()) return {};
    std::string a = sv.server;
    for (char& c : a) c = char(tolower(uint8_t(c)));
    while (!a.empty() && a.back() == '/') a.pop_back();
    return Identity::textHash("subsonic\n" + a);
}

together::Song App::serverSongAcross(const Subsonic::Song& s) {
    together::Song o;
    o.loose = Identity::looseKey(s.artist, std::string(), s.album, s.title);
    o.duration = s.duration;
    o.server = serverPrint();
    o.sid = s.id;
    return o;
}

void App::serverPlayAt(int i) {
    if (i < 0 || i >= int(srvq_.songs.size())) return;
    if (tog.stage != Tog::Off && togetherPlayServer(i)) return;   // (a session: the starter decides, for everyone)
    srvq_.pos = i;
    serverShow(i);
    const Subsonic::Song& s = srvq_.songs[size_t(i)];
    std::string path;
    if (subsonic.downloaded(s.id, path) || (subsonic.fetched(s.id, path) && !path.empty())) {
        player.load(path, srvLoadPaused_);
        if (srvLoadPaused_ && srvSeek_ > 1 && (s.duration <= 0 || srvSeek_ < s.duration - 1)) player.seek(srvSeek_);
        if (srvLoadPaused_ && srvResumeAfter_) player.togglePause();   // (a handoff: from its place, playing)
        srvLoadPaused_ = srvResumeAfter_ = false;
        srvFile_ = path;
        wave.request(path);   // its waveform, as yours
        subsonic.askFileInfo(s.id, path);
        srvInfoFor_ = s.id;
        srvq_.waitFor.clear();
    } else {
        player.stop();
        subsonic.askSong(s.id, s.suffix, true);   // (before the covers and lists waiting)
        srvq_.waitFor = s.id;   // (serverTick plays it when it is on the card)
    }
}

void App::serverShow(int i) {
    if (i < 0 || i >= int(srvq_.songs.size())) return;
    const Subsonic::Song& s = srvq_.songs[size_t(i)];
    lfmHeard = 0;   // Last.fm: a play of its own (lastfmTick)
    lfmDone = lfmSaid = false;
    extInfo(s.title, s.artist, s.album);
    now.info.duration = s.duration;
    std::string codec = s.suffix;
    for (char& c : codec) c = char(toupper(uint8_t(c)));
    now.info.codec = codec;   // (its file's own, once it is read: serverTick)
    now.info.bitsPerSample = 0;
    now.info.sampleRate = 0;
    now.info.bitrateKbps = s.bitRate;
    now.info.date = s.year > 0 ? std::to_string(s.year) : std::string();
    now.info.genre = s.genre;
    srvInfoFor_.clear();
    srvq_.coverFor.clear();
    if (!s.cover.empty()) {
        std::vector<uint8_t> b;
        if (subsonic.cover(s.cover, b) && !b.empty()) extCover(std::move(b));
        else { subsonic.askCover(s.cover, 600); srvq_.coverFor = s.cover; }
    }
    if (i + 1 < int(srvq_.songs.size())) subsonic.askSong(srvq_.songs[size_t(i + 1)].id, srvq_.songs[size_t(i + 1)].suffix);   // the next, ahead
    // its lyrics: what Songate's folder or the server has (serverTick takes them; none, LRCLIB as for a song of yours)
    now.lyrics.clear();
    now.synced = false;
    now.lyricsSource.clear();
    now.lyricsFromFile = now.lyricsInSongate = false;
    lyrOn = LyrOnline{};
    srvLyrFor_ = s.id;
    subsonic.askLyrics(s.id, s.artist, s.title, lyrnet::savedPath("server:" + s.id));
}

/** Every frame: a song come to the card plays; a cover come shows; after the queue, the background player gets the music back. */
void App::serverTick() {
    serverCoverPump();
    libSwitchTick();
    if (bool toMusic = false; subsonic.takeBatchDone(toMusic)) {
        if (toMusic) { rescan(); toast("Downloads are in your library"); }
        else toast("Downloads finished");
    }
    if (srvRescanAfterWipe_ && !subsonic.wiping()) { srvRescanAfterWipe_ = false; rescan(); }
    if (!srvOpenNew_.empty()) {
        if (const auto* pls = serverPlaylistList())
            for (const auto& p : *pls)
                if (p.name == srvOpenNew_) { srvOpenNew_.clear(); openServer(5, p.id, p.name); break; }
    }
    if (!srvSendName_.empty()) {
        if (subsonic.view().state != Subsonic::SignedIn) srvSendName_.clear();
        else if (serverSongList() && serverPlaylistList()) { const std::string n = srvSendName_; const std::vector<int> t = srvSendTracks_; sendPlaylistToServer(n, t); }
    }
    // its file read: the format as your songs show it ("AAC LC", kHz, bits, kbps)
    if (!srvInfoFor_.empty()) {
        Subsonic::FileInfo fi;
        if (subsonic.fileInfo(srvInfoFor_, fi)) {
            if (srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size()) && srvq_.songs[size_t(srvq_.pos)].id == srvInfoFor_ && !fi.codec.empty()) {
                now.info.codec = fi.codec;
                now.info.sampleRate = fi.sampleRate;
                now.info.bitsPerSample = fi.bits;
                if (fi.kbps) now.info.bitrateKbps = fi.kbps;
            }
            srvInfoFor_.clear();
        }
    }
    // its lyrics came: shown; none, asked online (Settings › Lyrics › Online), the answer handled as for a song of yours
    if (!srvLyrFor_.empty() && srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size()) && srvq_.songs[size_t(srvq_.pos)].id == srvLyrFor_) {
        std::string text, source;
        if (subsonic.lyrics(srvLyrFor_, text, source)) {
            const Subsonic::Song& so = srvq_.songs[size_t(srvq_.pos)];
            srvLyrFor_.clear();
            if (!text.empty()) {
                parseLyrics(text);
                std::string kind = subsonic.view().kind;   // (the server by its name: "Navidrome")
                if (!kind.empty()) kind[0] = char(toupper(uint8_t(kind[0])));
                now.lyricsSource = source == "the server" && !kind.empty() ? kind : source;
                now.lyricsFromFile = source == "Songate's folder";
                now.lyricsInSongate = now.lyricsFromFile;
            } else if (store.lyricsOnline && !so.artist.empty()) {
                lyrnet::Query q;
                q.kind = lyrnet::Ask::Lyrics;
                q.ticket = ++lyrOn.ticket;
                q.path = "server:" + so.id;
                q.title = so.title;
                q.artist = so.artist;
                q.album = so.album;
                q.duration = so.duration;
                lyrOn.path = q.path;
                lyrOn.state = LyrOnline::Looking;
                lyrNet.ask(q);
            }
        }
    } else if (!srvq_.on) srvLyrFor_.clear();
    // the server's track playing: now playing told once, the play once half of it (or 4 minutes) was heard
    if (srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size())) {
        const Subsonic::Song& so = srvq_.songs[size_t(srvq_.pos)];
        if (srvPlayFor_ != so.id) { srvPlayFor_ = so.id; srvPlayHeard_ = 0; srvPlaySaid_ = srvPlayDone_ = false; }
        if (togetherGuest()) srvPlaySaid_ = srvPlayDone_ = true;   // (a session: only the device that started it counts the play, as Last.fm)
        if (isPlaying() && player.loaded()) {
            if (!srvPlaySaid_) { srvPlaySaid_ = true; subsonic.scrobble(so.id, false); }
            srvPlayHeard_ += dt;
            const double need = Lastfm::threshold(so.duration);
            if (!srvPlayDone_ && need > 0 && srvPlayHeard_ >= need) { srvPlayDone_ = true; subsonic.scrobble(so.id, true); }
        }
    }
    if (subsonic.view().state == Subsonic::SignedIn) {
        unsigned gen = 0;
        std::vector<Subsonic::Song> st;
        if (srvStarredGen_ == ~0u && serverAsked_.insert("ST").second) subsonic.askStarred();
        if (subsonic.starred(st, gen) && gen != srvStarredGen_) { srvStarred_ = std::move(st); srvStarredGen_ = gen; }
        if (time >= srvLikeAt_) { srvLikeAt_ = time + 2; serverLikesSync(); }
    } else if (srvStarredGen_ != ~0u) {
        srvStarred_.clear();
        srvStarredGen_ = ~0u;
        srvLikeSig_ = 0;
    }
    if (firstServer_) {   // signed in from the first card: its music In the library, on Albums; or why not, and the way back
        const Subsonic::View fv = subsonic.view();
        if (fv.state == Subsonic::SignedIn) {
            firstServer_ = false;
            store.serverPlace = 1;
            persistSoon(0.3f);
            if (store.musicRoot.empty()) navigate(View::Albums, -1, std::string(), false);
            toast("Signed in: your server's music is in your library");
            if (tourAsk) { tourAsk = false; offerTour(); }   // a first start: the tour, offered as after choosing a folder (the user, 2026-11-06)
        } else if (fv.state == Subsonic::Failed) {
            firstServer_ = false;
            const std::string found = firstFound_;
            confirm(PLI_CLOUD, "Welcome to Songate", "Could not sign in", fv.server, fv.error, "Check the address (with its port, as http://nas.local:4533), "
                    "your user and your password, and that " GF_THIS_CARD " is on the same network as the server.",
                    "Try again", [this, found] { firstServer(found); });
            sheet.askSafe = true;
            sheet.askYesIcon = ICON_REFRESH;
            sheet.askNoCancel = true;
            sheet.askAlt = "Music on " GF_THIS_CARD;
            sheet.askAltYes = [this, found] { subsonic.signOut(); firstLocal(found); };
        }
    }
    if (!serverAsked_.empty() && subsonic.view().state != Subsonic::SignedIn && subsonic.view().state != Subsonic::SigningIn) serverForget();
    if (!srvq_.on) {
        if (srvq_.hadBackground && tog.stage == Tog::Off) {   // (a session gives it back when it ends: togetherEnd)
            srvq_.hadBackground = false;
            if (store.background) setBackground(true);
        }
        return;
    }
    if (!srvq_.waitFor.empty()) {
        std::string p;
        if (subsonic.fetched(srvq_.waitFor, p)) {
            const std::string id = srvq_.waitFor;
            srvq_.waitFor.clear();
            if (p.empty()) toast("This song could not be had from the server");
            else if (srvq_.pos >= 0 && srvq_.songs[size_t(srvq_.pos)].id == id) {
                player.load(p, srvLoadPaused_);
                if (srvLoadPaused_ && srvSeek_ > 1) player.seek(srvSeek_);
                if (srvLoadPaused_ && srvResumeAfter_) player.togglePause();   // (a handoff: from its place, playing)
                srvLoadPaused_ = srvResumeAfter_ = false;
                srvFile_ = p;
                wave.request(p);
                subsonic.askFileInfo(id, p);
                srvInfoFor_ = id;
            }
        }
    }
    if (!srvq_.coverFor.empty()) {
        std::vector<uint8_t> b;
        if (subsonic.cover(srvq_.coverFor, b)) {
            srvq_.coverFor.clear();
            if (!b.empty()) extCover(std::move(b));
        }
    }
}

/** The server's queue ends (a song of the library plays, or an iPhone over AirPlay). */
void App::serverLeave() {
    if (srvq_.session) {   // a session's server songs: kept for the session (a song of yours plays now), never for Library's switch
        srvq_.on = false;
        srvq_.waitFor.clear();
        extOn = false;
        extClearAt = 0;
        return;
    }
    if (srvq_.on && !srvq_.songs.empty()) {   // kept: Library's switch brings it back where it was
        srvSaved_ = srvq_;
        srvSaved_.on = false;
        srvSavedAt_ = player.loaded() ? player.position() : 0;
    }
    const bool had = srvq_.hadBackground;
    srvq_ = ServerQueue{};
    srvq_.hadBackground = had;   // (serverTick gives the background player its music back, the frame after)
    extOn = false;
    extClearAt = 0;
}

void App::openServer(int level, const std::string& id, const std::string& name) {
    if (level == 5) {   // a playlist: its tracks asked again (the user, 2026-11-06: "3 likes in the server but when i open only one is here")
        if (srvPage_[0].key == "P:" + id) srvPage_[0] = SrvPage{};
        serverAsked_.erase("P:" + id);
        srvPlaylistsGen_ = ~0u;   // (and the list, for its counts)
        serverAsked_.erase("PL");
    }
    navigate(View::Server, level, id + "\x1f" + name);
}

/** A cover from the server: its texture once it came (asked for once; none: nullptr for good). */
SDL_Texture* App::serverCover(const std::string& id, int size) {
    if (id.empty()) return nullptr;
    if (auto it = serverCovers_.find(id); it != serverCovers_.end()) return it->second;
    if (serverAsked_.insert("C:" + id).second) {
        subsonic.askCover(id, size);
    }
    return nullptr;
}

/** The library's .tile, with a server cover: 10 px padding, 10 px-rounded cover (round for artists), title, subtitle. */
bool App::serverTile(Id id, const std::string& cover, float x, float y, float w, bool round, const std::string& title, const std::string& sub, uint8_t flags,
                     const char* none) {
    const float cs = w - 20;
    const float h = 10 + cs + 8 + 20 + 2 + 18 + 10;
    const bool hit = ui.item(id, { x, y, w, h }, 14, flags);
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (hot) g.roundRect(x, y, w, h, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float rad = round ? cs / 2 : 10;
    if (SDL_Texture* t = serverCover(cover)) g.image(t, x + 10, y + 10, cs, cs, rad);
    else {   // not come yet, or none: the same box, with its kind's mark
        g.roundRect(x + 10, y + 10, cs, cs, rad, pal.text, 0.06f);
        g.icon(none ? none : round ? ICON_MIC : ICON_DISC, x + 10 + cs / 2, y + 10 + cs / 2, cs * 0.28f, pal.text3);
    }
    g.roundStroke(x + 10, y + 10, cs, cs, rad, 1, pal.text, 0.09f);
    const float ty = y + 10 + cs + 8, tx = round ? x + w / 2 : x + 10;
    const Align al = round ? Align::Center : Align::Left;
    g.text(title, { css::Base, Weight::SemiBold }, tx, ty, pal.text, 1, al, int(cs));
    g.text(sub, { css::Sm }, tx, ty + 22, pal.text3, 1, al, int(cs));
    return hit;
}

/** The server's albums, for the library's Albums while its music is In the library: asked once, copied once they came. */
const std::vector<Subsonic::Album>& App::serverAlbums() {
    if (subsonic.view().state != Subsonic::SignedIn) return serverAlbums_;
    if (serverAlbumsGen_ == ~0u) {
        std::vector<Subsonic::Album> list;
        if (subsonic.allAlbums(list)) { serverAlbums_ = std::move(list); serverAlbumsGen_ = ++srvl_.gen; }
        else if (serverAsked_.insert("L").second) subsonic.askAllAlbums();
    }
    return serverAlbums_;
}

const std::vector<Subsonic::Artist>* App::serverArtistList() {
    if (!srvl_.hA) {
        if (subsonic.artists(srvl_.artists)) { srvl_.hA = true; srvl_.gen++; }
        else if (serverAsked_.insert("A").second) subsonic.askArtists();
    }
    return srvl_.hA ? &srvl_.artists : nullptr;
}
const std::vector<Subsonic::Genre>* App::serverGenreList() {
    if (!srvl_.hG) {
        if (subsonic.genres(srvl_.genres)) { srvl_.hG = true; srvl_.gen++; }
        else if (serverAsked_.insert("G").second) subsonic.askGenres();
    }
    return srvl_.hG ? &srvl_.genres : nullptr;
}
const std::vector<Subsonic::Song>* App::serverSongList() {
    if (!srvl_.hS) {
        if (subsonic.allSongs(srvl_.songs)) { srvl_.hS = true; srvl_.gen++; }
        else if (serverAsked_.insert("S").second) subsonic.askAllSongs();
    }
    return srvl_.hS ? &srvl_.songs : nullptr;
}
const std::vector<Subsonic::Entry>* App::serverFolderList() {
    if (!srvl_.hF) {
        if (subsonic.folders(srvl_.folders)) { srvl_.hF = true; srvl_.gen++; }
        else if (serverAsked_.insert("F").second) subsonic.askFolders();
    }
    return srvl_.hF ? &srvl_.folders : nullptr;
}

/** One page's list from the server (an artist's albums, a genre's, a folder's contents), copied once it came. */
const App::SrvPage& App::serverPage(char kind, const std::string& id, int slot) {
    SrvPage& p = srvPage_[slot];
    const std::string key = std::string(1, kind) + ":" + id;
    if (p.key != key) p = SrvPage{ key };
    if (!p.have) {
        if (kind == 'R') p.have = subsonic.artist(id, p.albums);
        else if (kind == 'G') p.have = subsonic.genreAlbums(id, p.albums);
        else if (kind == 'P') {
            Subsonic::Playlist pl;
            p.have = subsonic.playlist(id, pl, p.songs);
            if (p.have) { p.name = pl.name; p.cover = pl.cover; }
        } else p.have = subsonic.dir(id, p.name, p.entries);
        if (!p.have && serverAsked_.insert(key).second) {
            if (kind == 'R') subsonic.askArtist(id);
            else if (kind == 'G') subsonic.askGenreAlbums(id);
            else if (kind == 'P') subsonic.askPlaylist(id);
            else subsonic.askDir(id);
        }
    }
    return p;
}

const std::vector<Subsonic::Playlist>* App::serverPlaylistList() {
    unsigned gen = 0;
    std::vector<Subsonic::Playlist> list;
    if (subsonic.playlists(list, gen)) { if (gen != srvPlaylistsGen_) { srvPlaylists_ = std::move(list); srvPlaylistsGen_ = gen; } }
    else if (serverAsked_.insert("PL").second) subsonic.askPlaylists();
    return srvPlaylistsGen_ == ~0u ? nullptr : &srvPlaylists_;
}

/** A playlist of yours sent to the server (the user: playlists "both ways"): its tracks the server also has (by artist and title), as a
 *  new playlist of the same name there, or that one's tracks replaced. Needs the server's list of tracks (asked when missing). */
void App::sendPlaylistToServer(const std::string& name, const std::vector<int>& tracks) {
    const std::vector<Subsonic::Song>* all = serverSongList();
    const std::vector<Subsonic::Playlist>* pls = serverPlaylistList();
    if (!all || !pls) {   // its lists first: sent as soon as they come (serverTick)
        srvSendName_ = name;
        srvSendTracks_ = tracks;
        toast("Sending “" + name + "” once the server answers");
        return;
    }
    srvSendName_.clear();
    auto low = [](std::string t) { for (char& ch : t) ch = char(tolower(uint8_t(ch))); return t; };
    std::unordered_map<std::string, std::string> byKey;
    for (const auto& so : *all) byKey.emplace(low(so.artist) + "\x1f" + low(so.title), so.id);
    std::vector<std::string> ids;
    for (int t : tracks) {
        if (t < 0 || t >= int(lib.tracks.size())) continue;
        if (auto it = byKey.find(low(lib.tracks[size_t(t)].artist) + "\x1f" + low(lib.tracks[size_t(t)].title)); it != byKey.end()) ids.push_back(it->second);
    }
    std::string kind = subsonic.view().kind;
    kind = kind.empty() ? std::string("the server") : std::string(1, char(toupper(uint8_t(kind[0])))) + kind.substr(1);
    if (ids.empty()) { toast("None of these tracks are on " + kind); return; }
    std::string existing;
    for (const auto& p : *pls) if (p.name == name) { existing = p.id; break; }
    subsonic.sendPlaylist(name, existing, ids);
    srvPlaylistsGen_ = ~0u;   // (asked again: the new one shows)
    serverAsked_.erase("PL");
    toast("Sent “" + name + "” to " + kind + ": " + std::to_string(ids.size()) + " of " + std::to_string(tracks.size()) + " tracks are there");
}

/** A server playlist saved as one of yours: its tracks you have here (by artist and title), in its order. */
void App::savePlaylistHere(const std::string& name, const std::vector<Subsonic::Song>& songs) {
    auto low = [](std::string t) { for (char& ch : t) ch = char(tolower(uint8_t(ch))); return t; };
    std::unordered_map<std::string, std::string> byKey;
    for (const Track& t : lib.tracks) if (!t.title.empty()) byKey.emplace(low(t.artist) + "\x1f" + low(t.title), t.path);
    std::vector<std::string> paths;
    for (const auto& so : songs) if (auto it = byKey.find(low(so.artist) + "\x1f" + low(so.title)); it != byKey.end()) paths.push_back(it->second);
    if (paths.empty()) { toast("None of its tracks are on " GF_THIS_CARD ": download them first"); return; }
    const std::string as = store.freePlaylistName(name);
    store.playlists.push_back({ as, std::string(), paths });
    store.save();
    toast("Saved “" + as + "”: " + std::to_string(paths.size()) + " of " + std::to_string(songs.size()) + " tracks are on " GF_THIS_CARD);
}

/** The Playlists page with Server chosen: the server's playlists, in the page's views (tiles, with covers, the list); a press opens one. */
void App::drawServerPlaylists(float x, float y, float w, float h, float cy, float oy, ListMode mode) {
    const std::vector<Subsonic::Playlist>* pls = serverPlaylistList();
    if (!pls) { g.textC("Asking the server…", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center); area("playlists").content = cy - oy + 120; return; }
    if (pls->empty()) {
        g.textC("No playlists on the server", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center);
        g.textC("New playlist makes one; a playlist of yours can be sent from its page", { css::Base }, x + w / 2, cy + 92, pal.text3, 1, Align::Center);
        area("playlists").content = cy - oy + 140;
        return;
    }
    const int n = int(pls->size());
    auto info = [](const Subsonic::Playlist& p) { return std::to_string(p.songs) + (p.songs == 1 ? " track" : " tracks"); };
    if (mode == ListMode::Grid || mode == ListMode::Albums) {
        const float inner = w - 32;
        const int cols = std::max(2, int(inner / 200));
        const float tw = inner / cols, th = 10 + (tw - 20) + 8 + 20 + 2 + 18 + 10;
        for (int k = 0; k < n; k++) {
            const float tx = x + 16 + (k % cols) * tw, ty = cy + (k / cols) * th;
            const Subsonic::Playlist& p = (*pls)[size_t(k)];
            if (ty + th < y - 40 || ty > y + h + 40) continue;
            const Id tid = uid("pls.srv", int(uid(p.id) & 0x7fffffff));
            if (serverTile(tid, p.cover, tx, ty, tw, false, p.name, info(p), k == 0 ? DefaultFocus : 0, ICON_LIST)) openServer(5, p.id, p.name);
            if (ui.focused(tid)) hoverSrvPl_ = k;
            g.roundRect(tx + 18, ty + 18, 30, 30, 9, pal.bg, 0.72f);   // its icon, small, over the cover's corner: what the sidebar shows (as yours)
            g.icon(serverPlaylistIcon(p.id), tx + 33, ty + 33, 16, pal.text);
        }
        area("playlists").content = cy - oy + ((n + cols - 1) / cols) * th + 32;
        return;
    }
    const float rowH = mode == ListMode::List ? 52 : 64, cov = rowH - 4 - 16;
    for (int k = 0; k < n; k++) {
        const Subsonic::Playlist& p = (*pls)[size_t(k)];
        const float ry = cy + k * rowH, rx = x + 14, rw = w - 28;
        const Id id = uid("pls.srv", int(uid(p.id) & 0x7fffffff));
        if (ui.item(id, { rx, ry, rw, rowH - 4 }, 10, k == 0 ? DefaultFocus : 0)) openServer(5, p.id, p.name);
        if (ui.focused(id)) hoverSrvPl_ = k;
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        g.roundRect(rx + 16, mid - cov / 2, cov, cov, 8, pal.surface2);   // its icon in its square, as yours
        g.icon(serverPlaylistIcon(p.id), rx + 16 + cov / 2, mid, cov * 0.5f, pal.text2);
        const float tx = rx + 16 + cov + 14;
        if (mode == ListMode::List) {
            g.textC(p.name, { css::Base, Weight::SemiBold }, tx, mid, pal.text, 1, Align::Left, int(rw * 0.5f));
            g.textC(info(p), { css::Sm }, rx + rw - 48, mid, pal.text3, 1, Align::Right, int(rw * 0.3f));
        } else {
            g.text(p.name, { css::Base, Weight::SemiBold }, tx, mid - 19, pal.text, 1, Align::Left, int(rw - 160));
            g.text(info(p), { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(rw - 160));
        }
        g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    area("playlists").content = cy - oy + n * rowH + 24;
}

/** A genre's first four album covers (for its tile, as the library's genres show four), asked when its tile first shows. */
const std::vector<std::string>& App::serverGenreCovers(const std::string& genre) {
    static const std::vector<std::string> none;
    if (auto it = srvGenreCovers_.find(genre); it != srvGenreCovers_.end()) return it->second;
    std::vector<Subsonic::Album> list;
    if (!subsonic.genreAlbums(genre, list)) {
        if (serverAsked_.insert("G:" + genre).second) subsonic.askGenreAlbums(genre);
        return none;
    }
    std::vector<std::string> covers;
    for (const auto& al : list) {
        if (al.cover.empty() || std::find(covers.begin(), covers.end(), al.cover) != covers.end()) continue;
        covers.push_back(al.cover);
        if (covers.size() == 4) break;
    }
    return srvGenreCovers_[genre] = std::move(covers);
}

/** Four server covers as one (coverQuad's way): fewer than four, the first alone; none, the genre's mark. */
void App::serverQuad(const std::vector<std::string>& covers, float x, float y, float size, float rad) {
    if (covers.size() < 4) {
        SDL_Texture* t = covers.empty() ? nullptr : serverCover(covers[0]);
        if (t) g.image(t, x, y, size, size, rad);
        else { g.roundRect(x, y, size, size, rad, pal.surface2); g.icon(ICON_TAG, x + size / 2, y + size / 2, size * 0.3f, pal.text3); }
        g.roundStroke(x, y, size, size, rad, 1, pal.text, 0.09f);
        return;
    }
    const float half = size / 2;
    for (int i = 0; i < 4; i++) {
        const int col = i % 2, row = i / 2;
        const float qx = x + col * half - (col ? 0.5f : 0), qy = y + row * half - (row ? 0.5f : 0);
        const float r4[4] = { col == 1 && row == 0 ? rad : 0, col == 1 && row == 1 ? rad : 0, col == 0 && row == 1 ? rad : 0, col == 0 && row == 0 ? rad : 0 };
        if (SDL_Texture* t = serverCover(covers[size_t(i)])) g.imageCorners(t, qx, qy, half + 0.5f, half + 0.5f, r4);
        else g.roundRectCorners(qx, qy, half + 0.5f, half + 0.5f, r4, pal.surface2);
    }
    g.roundStroke(x, y, size, size, rad, 1, pal.text, 0.09f);
}

/** The server's mark on a tile's cover corner (its music In the library). */
void App::serverBadge(float tx, float ty, float tileW, bool down) {
    const float cs = tileW - 20, bx = tx + 10 + cs - 20, by = ty + 10 + 20;
    g.circle(bx, by, 15, pal.bg, 0.92f);
    g.icon(down ? ICON_DOWNLOAD : PLI_CLOUD, bx, by, down ? 15 : 16, pal.accentInk);
}

std::string App::serverDlRoot() const {
    if (store.serverDlPlace == 1 && !store.musicRoot.empty()) return store.musicRoot + "/Songate downloads";
    return std::string(Store::Dir) + "/server-downloads";
}

/** Sign out (and Forget this server): what the server left here, and what to do with it. Remove everything takes it all away with
 *  the server's settings, so Songate is as it was before (the user: "not a single mention of the server stuff ... except in the library
 *  settings"); Keep downloads leaves the downloaded tracks (in the music folder they stay in your library). */
void App::serverSignOutCard() {
    const Subsonic::View sv = subsonic.view();
    int songs = 0, albums = 0;
    long long bytes = 0;
    subsonic.dlCounts(songs, bytes, albums);
    const long long cache = subsonic.cacheBytes();
    auto mb = [](long long b) { return std::to_string(int((b + 512 * 1024) / (1024 * 1024))) + " MB"; };
    std::string kind = sv.kind.empty() ? std::string("the server") : sv.kind;
    if (!sv.kind.empty()) kind[0] = char(toupper(uint8_t(kind[0])));
    std::string what = songs ? "Downloaded: " + std::to_string(songs) + (songs == 1 ? " track" : " tracks") + ", " + mb(bytes) + ". " : std::string("Nothing downloaded. ");
    what += "Kept to play again: " + mb(cache) + ". Remove everything takes all of it away, with the server's settings: Songate is as it was before the "
            "server. Keep downloads leaves the downloaded tracks here (in your music folder they stay in your library).";
    confirm(PLI_CLOUD, "Subsonic server", "Sign out of " + kind + "?", sv.server, "What should stay on this device?", what, "Remove everything",
            [this] { serverSignedOut(true); });
    sheet.askAlt = "Keep downloads";
    sheet.askAltYes = [this] { serverSignedOut(false); };
}

void App::serverSignedOut(bool removeAll) {
    if (srvq_.on) { serverLeave(); player.stop(); }
    srvSaved_ = ServerQueue{};   // its queue forgotten, here and for the next launch (nothing of the server left: the user, 2026-11-06)
    store.removeFile("server-queue.tsv");
    if (removeAll) {
        int songs = 0, albums = 0;
        long long bytes = 0;
        subsonic.dlCounts(songs, bytes, albums);
        srvRescanAfterWipe_ = songs > 0 && store.serverDlPlace == 1;   // (tracks in the music folder: the library read again after)
        store.serverPlace = 0;
        store.serverTab = 0;
        store.libSource = 0;
        store.serverDlPlace = 0;
        store.stopDownloads = false;
        srvPlIcons_.clear();
        store.removeFile("server-playlist-icons.tsv");   // (gone, as before the server: the user, 2026-11-06)
        applyJobStops();
        srvFilter_.clear();
    }
    subsonic.wipeLocal(removeAll);   // (the songs kept to play again go either way; downloads too with Remove everything)
    subsonic.signOut();
    persistSoon(0.3f);
    if (route.view == View::Server) navigate(View::Albums, -1, std::string(), false);
    toast(removeAll ? "Signed out: everything from the server is removed" : "Signed out: your downloads stay on this device");
}

/** A server genre's tile: its four covers, name, albums and songs (the library's genre tile). */
bool App::serverGenreTile(Id tid, const Subsonic::Genre& gn, float tx, float ty, float tileW, uint8_t flags) {
    const float cs = tileW - 20, th = 10 + cs + 8 + 20 + 2 + 18 + 10;
    const bool hit = ui.item(tid, { tx, ty, tileW, th }, 14, flags);
    const bool hot = ui.pressed(tid) || (ui.focused(tid) && ui.showFocusRing());
    if (hot) g.roundRect(tx, ty, tileW, th, 14, pal.text, ui.pressed(tid) ? 0.11f : 0.065f);
    serverQuad(serverGenreCovers(gn.name), tx + 10, ty + 10, cs, 10);
    g.text(gn.name, { css::Base, Weight::SemiBold }, tx + 10, ty + 10 + cs + 8, pal.text, 1, Align::Left, int(cs));
    g.text(std::to_string(gn.albums) + (gn.albums == 1 ? " album · " : " albums · ") + std::to_string(gn.songs) + (gn.songs == 1 ? " track" : " tracks"), { css::Sm },
           tx + 10, ty + 10 + cs + 30, pal.text3, 1, Align::Left, int(cs));
    return hit;
}

/** The heart: a server track's star while one plays, else the library track's Liked. */
bool App::currentLiked() {
    if (srvq_.on) return srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size()) && subsonic.isStarred(srvq_.songs[size_t(srvq_.pos)].id);
    return now.track >= 0 && !extOn && store.isLiked(lib.tracks[size_t(now.track)].path);
}
void App::toggleCurrentLike() {
    const bool liked = currentLiked();
    if (srvq_.on) {
        if (srvq_.pos < 0 || srvq_.pos >= int(srvq_.songs.size())) return;
        subsonic.star(srvq_.songs[size_t(srvq_.pos)].id, !liked);
    } else {
        if (now.track < 0 || extOn) return;
        store.toggleLiked(lib.tracks[size_t(now.track)].path);
        store.save();
    }
    toast(liked ? "Removed from Liked" : "Added to Liked");
}

/** Liked ↔ starred, for the tracks both have (by artist and title): a like on either side reaches the other, and so does taking one
 *  away, told apart by what both had at the last sync (the ledger). Run when Liked, the stars or the server's tracks changed. */
void App::serverLikesSync() {
    const std::vector<Subsonic::Song>* all = serverSongList();
    if (!all || srvStarredGen_ == ~0u || lib.tracks.empty()) return;
    const size_t sig = store.liked.size() * 1000003u + srvStarredGen_ * 131u + srvl_.gen * 7u + lib.tracks.size() + size_t(indexGen) * 17u;
    if (sig == srvLikeSig_) return;
    srvLikeSig_ = sig;
    auto low = [](std::string t) { for (char& ch : t) ch = char(tolower(uint8_t(ch))); return t; };
    auto key = [&](const std::string& artist, const std::string& title) { return low(artist) + "\x1f" + low(title); };
    std::unordered_map<std::string, std::string> srvByKey;   // key → the server's track
    for (const auto& so : *all) if (!so.title.empty()) srvByKey.emplace(key(so.artist, so.title), so.id);
    std::unordered_map<std::string, std::string> pathByKey;   // key → your track
    std::unordered_map<std::string, std::string> keyByPath;
    for (const Track& t : lib.tracks) {
        if (!t.tagged || t.title.empty()) continue;
        const std::string k = key(t.artist, t.title);
        if (!srvByKey.count(k)) continue;
        pathByKey.emplace(k, t.path);
        keyByPath.emplace(t.path, k);
    }
    if (pathByKey.empty()) return;
    std::unordered_set<std::string> mine, theirs;
    for (const auto& p : store.liked) if (auto it = keyByPath.find(p); it != keyByPath.end()) mine.insert(it->second);
    for (const auto& so : srvStarred_) { const std::string k = key(so.artist, so.title); if (pathByKey.count(k)) theirs.insert(k); }
    const std::unordered_set<std::string> ledger = subsonic.likeLedger();
    std::unordered_set<std::string> both, keys = mine;
    keys.insert(theirs.begin(), theirs.end());
    keys.insert(ledger.begin(), ledger.end());
    bool likedChanged = false;
    int toServer = 0, fromServer = 0;
    for (const auto& k : keys) {
        if (!pathByKey.count(k)) continue;
        const bool L = mine.count(k) > 0, S = theirs.count(k) > 0, known = ledger.count(k) > 0;
        if (L && S) { both.insert(k); continue; }
        if (L && !S) {
            if (known) { store.toggleLiked(pathByKey[k]); likedChanged = true; fromServer++; }   // unstarred there: unliked here
            else { subsonic.star(srvByKey[k], true); both.insert(k); toServer++; }                // liked here: starred there
        } else if (!L && S) {
            if (known) { subsonic.star(srvByKey[k], false); toServer++; }                         // unliked here: unstarred there
            else { store.toggleLiked(pathByKey[k]); likedChanged = true; both.insert(k); fromServer++; }   // starred there: liked here
        }
    }
    if (likedChanged) store.save();
    if (both != ledger) subsonic.setLikeLedger(both);
    if (toServer || fromServer) logLine("Server likes: " + std::to_string(toServer) + " to the server, " + std::to_string(fromServer) + " from it");
    srvLikeSig_ = store.liked.size() * 1000003u + srvStarredGen_ * 131u + srvl_.gen * 7u + lib.tracks.size() + size_t(indexGen) * 17u;
}

/** The Playlist page while the server's tracks play: the queue they play in, as a playlist's page (its cover, name, Current track,
 *  Clear, the views), the one playing lit; a press plays from there. */
void App::drawServerQueue(float x, float y, float w, float h) {
    const bool narrow = w < kNarrowList;
    const float cs = narrow ? 96.f : 120.f;
    const float headH = narrow ? 18 + cs + 14 + 34 + 10 + 36 + 16 : 18 + 120 + 14;
    std::string kind = subsonic.view().kind;
    kind = kind.empty() ? std::string("your server") : std::string(1, char(toupper(uint8_t(kind[0])))) + kind.substr(1);
    ScrollArea& sa = area("srv.queue");
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    g.clip(x, y, w, h);
    const float oy = y - sa.y, cyTop = oy + 18;
    const Subsonic::Song* cur = srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size()) ? &srvq_.songs[size_t(srvq_.pos)] : nullptr;
    if (SDL_Texture* t = cur ? serverCover(cur->cover, 500) : nullptr) g.image(t, x + 28, cyTop, cs, cs, 12);
    else { g.roundRect(x + 28, cyTop, cs, cs, 12, pal.surface2); g.icon(ICON_LIST, x + 28 + cs / 2, cyTop + cs / 2, cs * 0.36f, pal.text3); }
    const std::string title = srvq_.name.empty() ? "From " + kind : srvq_.name;
    double secs = 0;
    for (const auto& so : srvq_.songs) secs += so.duration;
    const int m = int(secs / 60);
    const std::string meta = std::to_string(srvq_.songs.size()) + (srvq_.songs.size() == 1 ? " track · " : " tracks · ") +
                             (m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min" : std::to_string(m) + " min") + " · from " + kind;
    const float tx = narrow ? x + 28 : x + 28 + cs + 22;
    if (!narrow) {
        kicker("Playlist", tx, cyTop + 10, pal.text3);
        g.text(title, { css::Xxl, Weight::Bold }, tx - 2, cyTop + 20, pal.text, 1, Align::Left, int(w - (tx - x) - 30));
        g.text(meta, { css::Sm }, tx, cyTop + 60, pal.text3, 1, Align::Left, int(w - (tx - x) - 30));
    } else {
        kicker("Playlist", x + 28 + cs + 16, cyTop + 20, pal.text3);
        g.text(title, { css::Lg, Weight::Bold }, x + 28 + cs + 16, cyTop + 32, pal.text, 1, Align::Left, int(w - cs - 72));
        g.text(meta, { css::Sm }, x + 28 + cs + 16, cyTop + 62, pal.text3, 1, Align::Left, int(w - cs - 72));
    }
    int mode = store.viewPlaylist;
    std::vector<int> order(srvq_.songs.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
    const float by = narrow ? cyTop + cs + 14 : cyTop + 120 - 34;
    float bx = tx;
    bool clicked = false;
    bx += button(uid("srv.qcur"), ICON_PLAY, "Current track", bx, by, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked && srvq_.pos >= 0) {   // glides to the one playing
        const std::vector<ListEntry> layout = serverLayout(srvq_.songs, order, w, ListMode(mode));
        sa.target = headH + listRowY(layout, srvq_.pos) - (h - 40) / 2;
        sa.clamp();
    }
    clicked = false;
    bx += button(uid("srv.qclear"), ICON_X, "Clear", bx, by, false, &clicked) + 8;
    viewSwitch("srv.qview", x + w - 28, narrow ? by + 34 + 10 + 18 : by + 17, mode);
    store.viewPlaylist = mode;
    const float bodyH = drawServerBody("srv.q", srvq_.songs, order, x, oy + headH, w, y, h, ListMode(mode), false, [this](int i) { serverPlayAt(i); });
    sa.content = headH + bodyH;
    g.unclip();
    ui.popArea();
    if (clicked) {   // Clear: the server's queue ends, nothing plays
        serverLeave();
        player.stop();
        toast("Playlist cleared");
    }
}

std::string App::lyrHerePath() const {
    if (srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size())) return "server:" + srvq_.songs[size_t(srvq_.pos)].id;
    return now.track >= 0 ? lib.tracks[size_t(now.track)].path : std::string();
}

/** A server track's … (the user, 2026-11-06: as your songs' menu): what fits a song that is not a file of yours. */
void App::openServerMenu(const Subsonic::Song& s, int idx) {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::SrvMenu;
    srvMenuSong_ = s;
    srvMenuIdx_ = idx;
    sheet.returnFocus = ui.focusedId();
    area("sheet", int(Sheet::SrvMenu) * 100).target = area("sheet", int(Sheet::SrvMenu) * 100).y = 0;
    ui.focusDefault();
}

void App::serverPlaylistChanged(const std::string& id) {
    if (!id.empty()) {
        if (srvPage_[0].key == "P:" + id) srvPage_[0].have = false;   // (asked again; its songs stay shown meanwhile)
        serverAsked_.erase("P:" + id);
    }
    srvPlaylistsGen_ = ~0u;
    serverAsked_.erase("PL");
}

/** New playlist on the server (the Playlists page's Server, the sidebar's +, Add to a playlist…'s New playlist…): a name, as yours. */
void App::serverNewPlaylist(const std::vector<std::string>& songIds) {
    std::string name = "Playlist " + std::to_string(srvPlaylists_.size() + 1);
    if (!keyboard("New playlist", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name[0] == ' ') name.erase(0, 1);
    if (name.empty()) return;
    for (const auto& p : srvPlaylists_) if (p.name == name) { toast("There is already a playlist called “" + name + "”"); return; }
    subsonic.newPlaylist(name, songIds);
    serverPlaylistChanged("");
    if (songIds.empty()) srvOpenNew_ = name;   // (opened once the server lists it)
    toast("Made the playlist “" + name + "”" + (songIds.empty() ? std::string() : ", with this track"));
}

void App::serverRenamePlaylist(const std::string& id, const std::string& old) {
    std::string name = old;
    if (!keyboard("Rename the playlist", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name[0] == ' ') name.erase(0, 1);
    if (name.empty() || name == old) return;
    for (const auto& p : srvPlaylists_) if (p.id != id && p.name == name) { toast("There is already a playlist called “" + name + "”"); return; }
    subsonic.updatePlaylist(id, name, {}, {});
    if (srvPage_[0].key == "P:" + id) srvPage_[0].name = name;
    for (auto& p : srvPlaylists_) if (p.id == id) p.name = name;
    serverPlaylistChanged(id);
    if (route.view == View::Server && route.ref == 5 && route.query.rfind(id + "\x1f", 0) == 0) route.query = id + "\x1f" + name;
}

void App::serverDeletePlaylist(const std::string& id, const std::string& name, int songs) {
    std::string kind = subsonic.view().kind;
    kind = kind.empty() ? std::string("the server") : std::string(1, char(toupper(uint8_t(kind[0])))) + kind.substr(1);
    confirm(PLI_CLOUD, "Delete", name, kind + " · " + std::to_string(songs) + (songs == 1 ? " track" : " tracks"), "Delete this playlist?",
            "It leaves " + kind + ", for every app that uses it. The tracks stay on the server and in your library.", "Delete",
            [this, id, name] {
                subsonic.deletePlaylist(id);
                srvPlaylists_.erase(std::remove_if(srvPlaylists_.begin(), srvPlaylists_.end(), [&](const auto& p) { return p.id == id; }), srvPlaylists_.end());
                serverPlaylistChanged(id);
                if (route.view == View::Server && route.ref == 5) { store.plSource = 1; navigate(View::Playlists, -1, std::string(), false); }
                toast("Deleted the playlist “" + name + "”");
            });
}

const char* App::serverPlaylistIcon(const std::string& id) {
    if (!srvPlIconsRead_) {   // (a small file, read once)
        srvPlIconsRead_ = true;
        if (FILE* f = fopen(store.path("server-playlist-icons.tsv").c_str(), "rb")) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                std::string l = line;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                const size_t t = l.find('\t');
                if (t != std::string::npos) srvPlIcons_[l.substr(0, t)] = l.substr(t + 1);
            }
            fclose(f);
        }
    }
    auto it = srvPlIcons_.find(id);
    if (it != srvPlIcons_.end()) if (const char* svg = playlistIconSvg(it->second)) return svg;
    return PLI_CLOUD;
}

void App::setServerPlaylistIcon(const std::string& id, const std::string& key) {
    serverPlaylistIcon(id);   // (read first)
    if (key.empty()) srvPlIcons_.erase(id);
    else srvPlIcons_[id] = key;
    std::string out;
    for (const auto& [k, v] : srvPlIcons_) out += k + "\t" + v + "\n";
    store.writeFile("server-playlist-icons.tsv", std::move(out));   // (the writer thread)
}

void App::openServerPlaylistMenu(const Subsonic::Playlist& p, bool icons) {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = icons ? Sheet::SrvPlIcon : Sheet::SrvPlMenu;
    srvMenuPl_ = p;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

std::vector<std::string> App::serverGenreSongIds(const std::string& genre) {
    std::vector<std::string> ids;
    if (const auto* all = serverSongList()) for (const auto& so : *all) if (so.genre == genre) ids.push_back(so.id);
    return ids;
}

/** A server genre's page: Add as playlist (a server playlist of its tracks as they are now), or In playlists (pressed: Delete). */
void App::serverGenrePlaylistButton(const std::string& genre, float& bx, float by) {
    const Subsonic::Playlist* have = nullptr;
    if (const auto* pls = serverPlaylistList()) for (const auto& p : *pls) if (p.name == genre) { have = &p; break; }
    bool c = false;
    bx += button(uid("sgen.addpl"), have ? ICON_LIST : ICON_PLUS, have ? "In playlists" : "Add as playlist", bx, by, false, &c, have != nullptr) + 8;
    if (!c) return;
    if (have) { serverDeletePlaylist(have->id, have->name, have->songs); return; }
    const std::vector<std::string> ids = serverGenreSongIds(genre);
    if (ids.empty()) { toast("Asking the server for its tracks: try again in a moment"); return; }
    subsonic.newPlaylist(genre, ids);
    serverPlaylistChanged("");
    toast("Added “" + genre + "” to the server's playlists");
}

/** The server's Genres: Add all as playlists, each a server playlist of its tracks as they are now (asked first, with the count). */
void App::addServerGenresAsPlaylists() {
    const auto* gens = serverGenreList();
    const auto* pls = serverPlaylistList();
    if (!gens || !pls || !serverSongList()) { toast("Asking the server: try again in a moment"); return; }
    std::vector<std::string> add;
    for (const auto& gn : *gens) {
        bool in = false;
        for (const auto& p : *pls) in = in || p.name == gn.name;
        if (!in) add.push_back(gn.name);
    }
    if (add.empty()) { toast("Every genre is in the server's playlists already"); return; }
    std::string kind = subsonic.view().kind;
    kind = kind.empty() ? std::string("the server") : std::string(1, char(toupper(uint8_t(kind[0])))) + kind.substr(1);
    const int n = int(add.size());
    const std::string count = std::to_string(n) + (n == 1 ? " genre" : " genres");
    confirm(ICON_LIST, "Genres", kind + "'s genres", count + " not in its playlists yet", "Add " + count + " as playlists?",
            "Each becomes a playlist on " + kind + ", named after its genre, with its tracks as they are now (a playlist on the server does not "
            "follow the genre later). Genres already in its playlists stay as they are.",
            "Add all", [this, add] {
                int added = 0;
                for (const auto& g1 : add) {
                    const std::vector<std::string> ids = serverGenreSongIds(g1);
                    if (ids.empty()) continue;
                    subsonic.newPlaylist(g1, ids);
                    added++;
                }
                serverPlaylistChanged("");
                toast("Added " + std::to_string(added) + (added == 1 ? " playlist" : " playlists"));
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_PLUS;
}

void App::serverQueueAdd(const Subsonic::Song& s, bool next) {
    if (!srvq_.on) { playServer({ s }, 0, s.album); return; }
    const size_t at = next ? size_t(std::max(0, srvq_.pos) + 1) : srvq_.songs.size();
    srvq_.songs.insert(srvq_.songs.begin() + long(std::min(at, srvq_.songs.size())), s);
    if (next) subsonic.askSong(s.id, s.suffix);   // (ahead, as the next one is)
}

/** Library's switch pressed. Its music Mixed in: nothing more. Apart: what plays now finishes, or is paused; then the other side's last
 *  track comes back, paused where it was (each side's queue kept). Nothing playing: at once. */
void App::libSourceChanged() {
    if (store.serverPlace != 0 || subsonic.view().state != Subsonic::SignedIn) { libSwitchTo_ = -1; return; }
    const int playingSide = srvq_.on ? 1 : 0;
    if (playingSide == store.libSource) { libSwitchTo_ = -1; return; }   // (back to the side playing: nothing to do)
    libSwitchTo_ = store.libSource;
    libSwitchTrack_ = now.track;
    libSwitchPos_ = srvq_.pos;
    if (!isPlaying()) libSwitchNow();
}

/** The switch pressed with a page of the sidebar's open: that page of the other library (the user, 2026-11-07: "The content when I'm already
 *  inside the albums tab, artists, etc does not refresh when I switch from local to server"). Only when its music is Apart (Mixed in: one
 *  library, its pages show both). Library itself already shows whichever side; a playlist goes to Playlists (the sidebar's list changed). */
void App::libSourceFollow() {
    if (store.serverPlace != 0 || subsonic.view().state != Subsonic::SignedIn) return;
    const View v = route.view;
    if (store.libSource == 1) {   // to the server's
        if (v == View::Albums || (v == View::Album && libFrom != View::Library)) openServerSection(0);
        else if (v == View::Album) navigate(View::Library);
        else if (v == View::Artists || v == View::Artist) openServerSection(1);
        else if (v == View::Genres || v == View::Genre) openServerSection(2);
        else if (v == View::Folders) { if (folderFrom == View::Library) navigate(View::Library); else openServerSection(3); }
        else if (v == View::List) navigate(View::Playlists);
        return;
    }
    if (v != View::Server) return;   // to yours
    int st = -1;
    if (route.ref == 6) st = std::clamp(atoi(route.query.c_str()), 0, 4);   // (4, Tracks: Library)
    else if (route.ref == 2) st = 0;
    else if (route.ref == 1) st = 1;
    else if (route.ref == 3) st = 2;
    else if (route.ref == 4) st = 3;
    if (route.ref == 5) navigate(View::Playlists);
    else if (st == 0) navigate(View::Albums);
    else if (st == 1) navigate(View::Artists);
    else if (st == 2) navigate(View::Genres);
    else if (st == 3) { navigate(View::Folders, lib.rootFolders.empty() ? -1 : lib.rootFolders[0]); folderFrom = View::Folders; }
    else navigate(View::Library);
}

void App::libSwitchTick() {
    if (libSwitchTo_ < 0) return;
    if (store.serverPlace != 0 || subsonic.view().state != Subsonic::SignedIn) { libSwitchTo_ = -1; return; }
    // the song ended (another began, or nothing plays), or it was paused
    const bool moved = libSwitchTo_ == 1 ? (srvq_.on || now.track != libSwitchTrack_) : (!srvq_.on || srvq_.pos != libSwitchPos_);
    if (moved || !isPlaying()) libSwitchNow();
}

void App::libSwitchNow() {
    const int to = libSwitchTo_;
    libSwitchTo_ = -1;
    if (to == 1) {   // to the server's: its last track, paused where it was
        if (!srvq_.on && now.track >= 0 && player.loaded()) localSavedAt_ = player.position();
        if (srvSaved_.songs.empty()) { if (isPlaying()) player.togglePause(); return; }
        const ServerQueue sq = srvSaved_;
        srvLoadPaused_ = true;
        srvSeek_ = srvSavedAt_;
        playServer(sq.songs, std::clamp(sq.pos, 0, int(sq.songs.size()) - 1), sq.name);
        toast("Back to " + (subsonic.view().kind.empty() ? std::string("the server") : extFrom) + "'s “" + sq.songs[size_t(std::clamp(sq.pos, 0, int(sq.songs.size()) - 1))].title + "”");
        return;
    }
    // to yours: the server's queue kept, your last track back, paused where it was
    if (srvq_.on) { serverLeave(); player.stop(); }
    if (q.pos >= 0 && q.pos < int(q.order.size())) {
        const int t = q.tracks[size_t(q.order[size_t(q.pos)])];
        loadTrack(t, true);
        const double dur = lib.tracks[size_t(t)].duration;
        if (player.loaded() && localSavedAt_ > 1 && (dur <= 0 || localSavedAt_ < dur - 1)) player.seek(localSavedAt_);
        toast("Back to “" + lib.tracks[size_t(t)].title + "”");
    }
}

/** Signed out (or another server): what was copied, the covers' textures and what was asked are forgotten. */
void App::serverForget() {
    serverAlbums_.clear();
    serverAlbumsGen_ = ~0u;
    srvl_ = SrvLists{ false, false, false, false, {}, {}, {}, {}, srvl_.gen + 1 };
    srvPage_[0] = SrvPage{};
    srvPage_[1] = SrvPage{};
    srvGenreCovers_.clear();
    serverAsked_.clear();
    for (auto& [k, t] : serverCovers_) if (t) SDL_DestroyTexture(t);
    serverCovers_.clear();
}

/** Songs as rows (the album page's): its number, title, artist and album, length; the one playing lit; a press plays from there.
 *  Only the rows in view are drawn. Returns the rows' height. */
float App::drawServerSongs(const char* key, const std::vector<Subsonic::Song>& songs, const std::string& name, float x, float top, float w, float y, float h) {
    const float rowH = 52;
    const std::string playingId = srvq_.on && srvq_.pos >= 0 ? srvq_.songs[size_t(srvq_.pos)].id : std::string();
    for (size_t i = 0; i < songs.size(); i++) {
        const float ry = top + float(i) * rowH;
        if (ry + rowH < y || ry > y + h) continue;
        const Subsonic::Song& s = songs[i];
        const Id rid = uid(key, int(i));
        if (ui.item(rid, { x + 18, ry, w - 36, rowH - 4 }, 10)) playServer(songs, int(i), name);
        const bool hot = ui.pressed(rid) || (ui.focused(rid) && ui.showFocusRing());
        if (!playingId.empty() && s.id == playingId) g.roundRect(x + 18, ry, w - 36, rowH - 4, 10, pal.accent, 0.12f);
        if (hot) g.roundRect(x + 18, ry, w - 36, rowH - 4, 10, pal.text, ui.pressed(rid) ? 0.11f : 0.065f);
        const float my = ry + (rowH - 4) / 2;
        g.textNum(std::to_string(i + 1), { css::Sm }, x + 52, my - g.lineHeight({ css::Sm }) / 2.f, pal.text3, 1, Align::Right);
        const std::string dur = s.duration > 0 ? fmtTime(s.duration) : "";
        const float dw = float(g.measureNum(dur, { css::Sm }));
        const float tw = w - 36 - 52 - 24 - dw - 24;
        g.textC(s.title, { css::Base, Weight::SemiBold }, x + 70, my - 9, pal.text, 1, Align::Left, int(tw));
        g.textC(s.artist + (s.album.empty() ? "" : " · " + s.album), { css::Sm }, x + 70, my + 10, pal.text3, 1, Align::Left, int(tw));
        g.textNum(dur, { css::Sm }, x + w - 36, my - g.lineHeight({ css::Sm }) / 2.f, pal.text3, 1, Align::Right);
    }
    return float(songs.size()) * rowH;
}

std::string App::serverCountNote() {
    if (store.serverPlace != 1 || serverAlbums().empty()) return {};
    const Subsonic::View sv = subsonic.view();
    std::string kind = sv.kind.empty() ? std::string("your server") : sv.kind;
    kind[0] = char(toupper(uint8_t(kind[0])));
    return " · " + std::to_string(serverAlbums_.size()) + " on " + kind;
}

/** Covers decoded on the worker: textures from them, three a frame at most (each frame; nothing while none came). */
void App::serverCoverPump() {
    subsonic.takeCovers(serverReady_);
    for (int k = 0; k < 3 && !serverReady_.empty(); k++) {
        auto [id, s] = serverReady_.front();
        serverReady_.erase(serverReady_.begin());
        SDL_Texture* t = nullptr;
        if (s) {
            t = SDL_CreateTextureFromSurface(renderer_, s);
            if (t) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
            SDL_FreeSurface(s);
        }
        if (auto it = serverCovers_.find(id); it != serverCovers_.end() && it->second) SDL_DestroyTexture(it->second);
        serverCovers_[id] = t;
    }
}

// ------------------------------------------------------------------ the four list views, for the server's tracks

std::vector<App::ListEntry> App::serverLayout(const std::vector<Subsonic::Song>& all, const std::vector<int>& order, float w, ListMode mode) const {
    std::vector<ListEntry> out;
    float y = 0;
    if (mode == ListMode::Grid) {
        const Tiles t = tilesFor(w);
        for (int i = 0; i < int(order.size()); i += t.cols) { out.push_back({ ListEntry::TileRow, i, y, t.h }); y += t.h; }
        return out;
    }
    out.push_back({ ListEntry::ColumnHead, -1, y, ColHeadH });
    y += ColHeadH;
    std::string last = "\x1f";
    for (int i = 0; i < int(order.size()); i++) {
        if (mode == ListMode::Albums) {   // a header whenever the album changes
            const std::string& a = all[size_t(order[size_t(i)])].albumId;
            if (a != last || a.empty()) { out.push_back({ ListEntry::GroupHead, i, y, GroupH }); y += GroupH; }
            last = a;
        }
        const float h = mode == ListMode::Covers ? CovRowH : RowH;
        out.push_back({ ListEntry::Row, i, y, h });
        y += h;
    }
    return out;
}

/** trackRow's and coverRow's look (widgets.cpp, view_tracks.cpp) for a server track. */
bool App::serverRow(Id id, const Subsonic::Song& t, int number, float x, float y, float w, float h, bool covers, bool extended) {
    const float rh = covers ? h - 4 : h, rad = covers ? 10.f : 8.f;
    const bool hit = ui.item(id, { x, y, w, rh }, rad);
    hoverServer(id, t, number - 1);
    const bool playing = serverPlaying(t) && player.loaded();
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (playing) g.roundRect(x, y, w, rh, rad, pal.accent, 0.09f);
    if (hot) g.roundRect(x, y, w, rh, rad, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float cy = y + rh / 2;
    const std::string dur = t.duration > 0 ? fmtTime(t.duration) : "";
    if (w < kNarrowList) {   // narrow (a phone upright): number or cover, the title over artist · album, the length
        float tx = x + 12 + 28 + 12;
        if (covers) {
            const float cov = rh - 16;
            if (SDL_Texture* tex = serverCover(t.cover)) g.image(tex, x + 12, cy - cov / 2, cov, cov, 7);
            else g.roundRect(x + 12, cy - cov / 2, cov, cov, 7, pal.surface2);
            if (playing) { g.roundRect(x + 12, cy - cov / 2, cov, cov, 7, { 0, 0, 0 }, 0.38f); eqGlyph(x + 12 + cov / 2 - 9, cy, { 1, 1, 1 }); }
            tx = x + 12 + cov + 12;
        } else if (playing) eqGlyph(x + 12 + 28 - 13, cy, pal.accentInk);
        else g.textC(std::to_string(number), { css::Sm }, x + 12 + 28, cy, pal.text3, 1, Align::Right);
        const float tw = w - (tx - x) - 64;
        g.text(t.title, { css::Base, playing ? Weight::SemiBold : Weight::Regular }, tx, cy - 20, playing ? pal.accentInk : pal.text, 1, Align::Left, int(tw));
        g.text(t.artist + (t.album.empty() ? "" : " · " + t.album), { css::Sm }, tx, cy + 2, pal.text3, 1, Align::Left, int(tw));
        g.textC(dur, { css::Sm }, x + w - 16, cy, pal.text3, 1, Align::Right);
        return hit;
    }
    const float pad = 16, gap = 14, inner = w - pad * 2;
    float cx = x + pad;
    if (playing) eqGlyph(cx + 44 - 13, cy, pal.accentInk);
    else g.textC(std::to_string(number), { css::Sm }, cx + 44, cy, pal.text3, 1, Align::Right);
    cx += 44 + gap;
    if (covers) {
        const float fixed = 44 + 64 + gap * (extended ? 5 : 3) + (extended ? 64 : 0);
        const float fr = (inner - fixed) / (extended ? 4.f : 3.f);
        const float cov = rh - 16;
        if (SDL_Texture* tex = serverCover(t.cover)) g.image(tex, cx, cy - cov / 2, cov, cov, 7);
        else g.roundRect(cx, cy - cov / 2, cov, cov, 7, pal.surface2);
        const float tx = cx + cov + 12, tw = fr * 2 - cov - 12;
        g.text(t.title, { css::Base, playing ? Weight::SemiBold : Weight::Regular }, tx, cy - 20, playing ? pal.accentInk : pal.text, 1, Align::Left, int(tw));
        g.text(t.artist, { css::Sm }, tx, cy + 2, pal.text3, 1, Align::Left, int(tw));
        cx += fr * 2 + gap;
        g.textC(t.album, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
        cx += fr + gap;
        if (extended) {
            g.textC(t.year > 0 ? std::to_string(t.year) : "", { css::Base }, cx, cy, pal.text2, 1, Align::Left, 64);
            cx += 64 + gap;
            g.textC(t.genre, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
        }
        g.textC(dur, { css::Sm }, x + w - pad, cy, pal.text3, 1, Align::Right);
        return hit;
    }
    const float fixed = 44 + 64 + gap * 4, fr = (inner - fixed) / 3.6f;
    g.textC(t.title, { css::Base, playing ? Weight::SemiBold : Weight::Regular }, cx, cy, playing ? pal.accentInk : pal.text, 1, Align::Left, int(fr * 1.6f));
    cx += fr * 1.6f + gap;
    g.textC(t.artist, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
    cx += fr + gap;
    g.textC(t.album, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
    cx += fr + gap;
    g.textC(dur, { css::Sm }, cx + 64, cy, pal.text3, 1, Align::Right);
    return hit;
}

/** trackTile's look for a server track. */
bool App::serverTrackTile(Id id, const Subsonic::Song& t, float x, float y, float w, float cover) {
    const float h = 8 + cover + 10 + 20 + 18 + 6;
    const bool hit = ui.item(id, { x, y, w, h }, 14);
    hoverServer(id, t);
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    const bool playing = serverPlaying(t) && player.loaded();
    if (hot) g.roundRect(x, y, w, h, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float cx = x + 8, cy = y + 8;
    if (SDL_Texture* tex = serverCover(t.cover)) g.image(tex, cx, cy, cover, cover, 12);
    else { g.roundRect(cx, cy, cover, cover, 12, pal.surface2); g.icon(ICON_DISC, cx + cover / 2, cy + cover / 2, cover * 0.28f, pal.text3); }
    g.roundStroke(cx, cy, cover, cover, 12, 1, pal.text, 0.09f);
    if (playing) {
        g.roundRect(cx, cy, cover, cover, 12, { 0, 0, 0 }, 0.38f);
        eqGlyph(cx + cover / 2 - 9, cy + cover / 2, { 1, 1, 1 });
    } else if (hot) {
        g.circle(cx + cover - 31, cy + cover - 31, 21, pal.accent);
        g.icon(ICON_PLAY, cx + cover - 31, cy + cover - 31, 18, pal.onAccent);
    }
    g.text(t.title, { css::Base, Weight::SemiBold }, cx, cy + cover + 10, playing ? pal.accentInk : pal.text, 1, Align::Left, int(cover));
    g.text(t.artist, { css::Sm }, cx, cy + cover + 32, pal.text3, 1, Align::Left, int(cover));
    return hit;
}

/** groupHead's look: the album's cover, title, "artist · year · n tracks", and Album to open it. */
void App::serverGroupHead(const char* key, int index, const std::vector<Subsonic::Song>& all, const std::vector<int>& order, float x, float y, float w) {
    const Subsonic::Song& t = all[size_t(order[size_t(index)])];
    const float cov = GroupH - 4 - 20, cy = y + (GroupH - 4) / 2 + 2;
    if (SDL_Texture* tex = serverCover(t.cover)) g.image(tex, x + 16, cy - cov / 2, cov, cov, 8);
    else g.roundRect(x + 16, cy - cov / 2, cov, cov, 8, pal.surface2);
    const float tx = x + 16 + cov + 14;
    int n = 0;
    for (int i = index; i < int(order.size()) && all[size_t(order[size_t(i)])].albumId == t.albumId; i++) n++;
    std::string meta3 = t.artist;
    if (t.year > 0) meta3 += " · " + std::to_string(t.year);
    meta3 += " · " + std::to_string(n) + (n == 1 ? " track" : " tracks");
    const float bw = g.measure("Album", { css::Sm, Weight::SemiBold }) + 32 + 24;
    g.text(t.album, { css::Md, Weight::Bold }, tx, cy - 22, pal.text, 1, Align::Left, int(w - (tx - x) - bw - 30));
    g.text(meta3, { css::Sm }, tx, cy + 2, pal.text3, 1, Align::Left, int(w - (tx - x) - bw - 30));
    if (!t.albumId.empty()) {
        bool clicked = false;
        button(uid(std::string(key) + ".grp", index), ICON_PLAY, "Album", x + w - 16 - bw, cy - 17, false, &clicked);
        if (clicked) openServer(2, t.albumId, t.album);
    }
}

/** drawTrackBody for the server's tracks: what lies in [clipY, clipY + clipH) from top; play(i) when the i-th is chosen. */
float App::drawServerBody(const char* key, const std::vector<Subsonic::Song>& all, const std::vector<int>& order, float x, float top, float w, float clipY,
                          float clipH, ListMode mode, bool extended, const std::function<void(int)>& play) {
    const std::vector<ListEntry> layout = serverLayout(all, order, w, mode);
    const Tiles tl = tilesFor(w);
    float height = 0;
    for (const auto& e : layout) {
        height = std::max(height, e.y + e.h);
        const float ey = top + e.y;
        if (ey + e.h < clipY - 80 || ey > clipY + clipH + 80) continue;
        switch (e.kind) {
        case ListEntry::ColumnHead: listColumns(x, ey, w, mode, extended); break;
        case ListEntry::GroupHead: serverGroupHead(key, e.index, all, order, x + 14, ey, w - 28); break;
        case ListEntry::Row:
            if (e.index == bodyGrab) {   // picked up (Move): lifted, the accent around it
                const float rh = mode == ListMode::Covers ? e.h - 4 : e.h;
                g.roundRect(x + 14, ey, w - 28, rh, mode == ListMode::Covers ? 10.f : 8.f, pal.accent, 0.14f);
                g.roundStroke(x + 14, ey, w - 28, rh, mode == ListMode::Covers ? 10.f : 8.f, 2, pal.accent, 0.95f);
            }
            if (serverRow(uid(std::string(key) + ".r", e.index), all[size_t(order[size_t(e.index)])], e.index + 1, x + 14, ey, w - 28, e.h,
                          mode == ListMode::Covers, extended))
                play(e.index);
            break;
        case ListEntry::TileRow:
            for (int c2 = 0; c2 < tl.cols && e.index + c2 < int(order.size()); c2++) {
                const int i = e.index + c2;
                if (serverTrackTile(uid(std::string(key) + ".r", i), all[size_t(order[size_t(i)])], x + 16 + c2 * tl.w, ey, tl.w, tl.cover)) play(i);
            }
            break;
        }
    }
    return height + 24;
}

/** drawSubfolders' look for the server's folders: tiles (Grid, by album) or rows (with covers, the plain list). */
float App::serverFolders(const char* key, const std::vector<const Subsonic::Entry*>& dirs, float x, float cy, float w, float y, float h, ListMode mode) {
    const int n = int(dirs.size());
    if (!n) return 0;
    if (mode == ListMode::Grid || mode == ListMode::Albums) {
        const float inner = w - 32;
        const int cols = std::max(2, int(inner / 200));
        const float tw = inner / cols, cover = tw - 16, th = 8 + cover + 10 + 20 + 18 + 16;
        const int rows = (n + cols - 1) / cols;
        for (int r = 0; r < rows; r++) {
            const float ry = cy + r * th;
            if (ry + th < y - 40 || ry > y + h + 40) continue;
            for (int c2 = 0; c2 < cols && r * cols + c2 < n; c2++) {
                const Subsonic::Entry& e = *dirs[size_t(r * cols + c2)];
                const float tx = x + 16 + c2 * tw;
                const Id id = uid(key, int(uid(e.id) & 0x7fffffff));
                const bool hit = ui.item(id, { tx, ry, tw, th - 8 }, 14);
                const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
                if (hot) g.roundRect(tx, ry, tw, th - 8, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
                if (SDL_Texture* tex = serverCover(e.cover)) g.image(tex, tx + 8, ry + 8, cover, cover, 12);
                else { g.roundRect(tx + 8, ry + 8, cover, cover, 12, pal.surface2); g.icon(ICON_FOLDER, tx + 8 + cover / 2, ry + 8 + cover / 2, cover * 0.3f, pal.text3); }
                g.roundStroke(tx + 8, ry + 8, cover, cover, 12, 1, pal.text, 0.09f);
                g.text(e.name, { css::Base, Weight::SemiBold }, tx + 8, ry + 8 + cover + 10, pal.text, 1, Align::Left, int(cover));
                g.text("Folder", { css::Sm }, tx + 8, ry + 8 + cover + 32, pal.text3, 1, Align::Left, int(cover));
                if (hit) openServer(4, e.id, e.name);
            }
        }
        return rows * th + 8;
    }
    const float rowH = mode == ListMode::List ? 48 : 60, cov = rowH - 4 - 16;
    for (int i = 0; i < n; i++) {
        const Subsonic::Entry& e = *dirs[size_t(i)];
        const float ry = cy + i * rowH, rx = x + 14, rw = w - 28;
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        const Id id = uid(key, int(uid(e.id) & 0x7fffffff));
        if (ui.item(id, { rx, ry, rw, rowH - 4 }, 10)) openServer(4, e.id, e.name);
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        if (SDL_Texture* tex = serverCover(e.cover)) g.image(tex, rx + 16, mid - cov / 2, cov, cov, 6);
        else { g.roundRect(rx + 16, mid - cov / 2, cov, cov, 6, pal.surface2); g.icon(ICON_FOLDER, rx + 16 + cov / 2, mid, cov * 0.5f, pal.text2); }
        const float tx = rx + 16 + cov + 14;
        if (mode == ListMode::List) {
            g.textC(e.name, { css::Base, Weight::SemiBold }, tx, mid, pal.text, 1, Align::Left, int(rw * 0.5f));
            g.textC("Folder", { css::Sm }, rx + rw - 48, mid, pal.text3, 1, Align::Right, int(rw * 0.35f));
        } else {
            g.text(e.name, { css::Base, Weight::SemiBold }, tx, mid - 19, pal.text, 1, Align::Left, int(rw - 140));
            g.text("Folder", { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(rw - 140));
        }
        g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    return n * rowH;
}

void App::drawServer(float x, float y, float w, float h, bool inLibrary) {
    const Subsonic::View sv = subsonic.view();
    std::string kind = sv.kind.empty() ? std::string("Server") : sv.kind;
    kind[0] = char(toupper(uint8_t(kind[0])));
    if (sv.state != Subsonic::SignedIn) {
        viewHead("Server", "Not signed in", "Sign in under Settings › Library › Subsonic server", x, y, w);
        return;
    }
    auto askOnce = [this](const std::string& key, const std::function<void()>& ask) {
        if (serverAsked_.insert(key).second) ask();
    };
    const auto [id, name] = idName(route.query);
    const int level = inLibrary ? 0 : std::clamp(route.ref, 0, 6);   // 6: one view on its own (the sidebar's), route.query its number
    const Grid gl = gridOf(x, w);
    const bool narrow = w < kNarrowList;
    // a grid of n tiles under `top` in its own scroll area; draw(i, tx, ty) draws one
    auto grid = [&](const char* key, int sid, float top, int n, const std::function<void(int, float, float)>& draw) {
        ScrollArea& sa = area(key, sid);
        sa.view = { x, top, w, y + h - top };
        const int rows = (n + gl.cols - 1) / gl.cols;
        sa.content = rows * gl.tileH + 24;
        ui.pushArea(&sa);
        g.clip(x, top, w, y + h - top);
        const int firstRow = std::max(0, int(sa.y / gl.tileH) - 1), lastRow = std::min(rows, int((sa.y + sa.view.h) / gl.tileH) + 2);
        for (int r = firstRow; r < lastRow; r++)
            for (int cc = 0; cc < gl.cols; cc++) {
                const int i = r * gl.cols + cc;
                if (i >= n) break;
                draw(i, gl.x0 + cc * gl.tileW, top + r * gl.tileH - sa.y);
            }
        g.unclip();
        ui.popArea();
    };
    auto shuffled = [](std::vector<Subsonic::Song> v) {
        static std::mt19937 rng{ SDL_GetTicks() };
        std::shuffle(v.begin(), v.end(), rng);
        return v;
    };

    if (level == 0 || level == 6) {   // the tab: Albums, Artists, Genres, Folders, Tracks, as Library's switch (6: one, the sidebar's)
        static const char* tabs[] = { "Albums", "Artists", "Genres", "Folders", "Tracks" };
        const bool single = level == 6;
        const int tab = single ? std::clamp(atoi(route.query.c_str()), 0, 4) : std::clamp(store.serverTab, 0, 4);   // (its playlists: the Playlists tab's Server)
        const std::vector<Subsonic::Album>* albums = tab == 0 ? &serverAlbums() : nullptr;
        const std::vector<Subsonic::Artist>* arts = tab == 1 ? serverArtistList() : nullptr;
        const std::vector<Subsonic::Genre>* gens = tab == 2 ? serverGenreList() : nullptr;
        const std::vector<Subsonic::Entry>* folds = tab == 3 ? serverFolderList() : nullptr;
        const std::vector<Subsonic::Song>* songs = tab == 4 ? serverSongList() : nullptr;
        auto count = [](size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
        std::string sub = "Asking the server…";
        if (tab == 0 && serverAlbumsGen_ != ~0u) sub = count(albums->size(), "album", "albums") + (sv.songs >= 0 ? " · " + count(size_t(sv.songs), "track", "tracks") : "");
        if (tab == 1 && arts) sub = count(arts->size(), "artist", "artists");
        if (tab == 2 && gens) sub = count(gens->size(), "genre", "genres");
        if (tab == 3 && folds) sub = count(folds->size(), "folder", "folders");
        if (tab == 4 && songs) {
            double secs = 0;
            for (const auto& so : *songs) secs += so.duration;
            const int m = int(secs / 60);
            sub = count(songs->size(), "track", "tracks") + " · " + (m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min" : std::to_string(m) + " min");
            if (!srvFilter_.empty()) sub += " · matching “" + srvFilter_ + "”";
        }
        const int room = int(w - 56 - (narrow ? 0 : inLibrary ? 700 : 520));
        float cy = y + 18 + 8;
        kicker(inLibrary ? std::string("Library") : kind, x + 28, cy, pal.text3);
        cy += 10;
        g.text(tabs[tab], { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, room);
        cy += 38;
        g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, room);
        int picked = -1;
        float top = cy + 22 + 14;
        const std::vector<std::string> names = { "Albums", "Artists", "Genres", "Folders", "Tracks" };
        if (single) {}   // (the sidebar's: no switch, as your own Albums, Artists, Genres and Folders pages)
        else if (!narrow) {
            segmented(uid("srv.tab"), x + w - 28, y + 52, names, tab, &picked);   // (Local | Server: the title bar's)
        }
        else { segmented(uid("srv.tab"), x + w - 28, top + 17, names, tab, &picked, 0, w - 56, 1); top += 34 + 16; }
        if (picked >= 0 && picked != tab) { store.serverTab = picked; persistSoon(0.5f); }

        if (tab == 0) {   // the library's album grid, with its sort select
            static const char* sorts[] = { "By artist", "By title", "By year" };
            const Font sf{ css::Sm };
            // in the header, right of the count, the grid just under it, as your Albums' (drawAlbumGrid); a row of its own under the views
            // when they take the header's line (narrow)
            const bool inHead = !narrow || single;
            const float sw = g.measure(sorts[srvSort_], sf) + 20 + 26, sx = x + w - 28 - sw, sy = inHead ? top - 14 - 32 : top;
            const Id sid = uid("srv.sort");
            if (ui.item(sid, { sx, sy, sw, 32 }, 8)) srvSort_ = (srvSort_ + 1) % 3;
            g.roundRect(sx, sy, sw, 32, 8, pal.text, ui.focused(sid) && ui.showFocusRing() ? 0.11f : 0.065f);
            g.roundStroke(sx, sy, sw, 32, 8, 1, pal.text, 0.15f);
            g.textC(sorts[srvSort_], sf, sx + 10, sy + 16, pal.text);
            g.icon(ICON_CHEV, sx + sw - 16, sy + 16, 14, pal.text3);
            // the order: made again only when the sort or the albums changed
            static std::vector<int> order;
            static int orderSort = -1;
            static unsigned orderGen = ~0u;
            if (orderSort != srvSort_ || orderGen != serverAlbumsGen_ || order.size() != albums->size()) {
                orderSort = srvSort_;
                orderGen = serverAlbumsGen_;
                order.resize(albums->size());
                for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
                auto low = [](std::string t) { for (char& ch : t) ch = char(tolower(uint8_t(ch))); return t; };
                std::vector<std::pair<std::string, std::string>> k(albums->size());
                for (size_t i = 0; i < k.size(); i++) {
                    const auto& al = (*albums)[i];
                    k[i] = srvSort_ == 1 ? std::make_pair(low(al.name), low(al.artist))
                         : srvSort_ == 2 ? std::make_pair(std::to_string(9999 - al.year), low(al.artist)) : std::make_pair(low(al.artist), low(al.name));
                }
                std::stable_sort(order.begin(), order.end(), [&](int p, int q) { return k[size_t(p)] < k[size_t(q)]; });
            }
            grid("srv.albums", 0, inHead ? top : top + 46, int(order.size()), [&](int i, float tx, float ty) {
                const Subsonic::Album& al = (*albums)[size_t(order[size_t(i)])];
                const Id tid = uid("srv.al", int(uid(al.id) & 0x7fffffff));
                if (i < gl.cols) ui.link(tid, 0, -1, sid);
                if (serverTile(tid, al.cover, tx, ty, gl.tileW, false, al.name, al.artist, i == 0 ? DefaultFocus : 0)) openServer(2, al.id, al.name);
                if (subsonic.albumDownload(al.id).state == 2) serverBadge(tx, ty, gl.tileW, true);   // downloaded: the arrow
            });
        } else if (tab == 1 && arts) {   // its artists, round
            grid("srv.artists", 0, top, int(arts->size()), [&](int i, float tx, float ty) {
                const Subsonic::Artist& a = (*arts)[size_t(i)];
                if (serverTile(uid("srv.ar", int(uid(a.id) & 0x7fffffff)), a.cover, tx, ty, gl.tileW, true, a.name,
                               std::to_string(a.albums) + (a.albums == 1 ? " album" : " albums"), i == 0 ? DefaultFocus : 0))
                    openServer(1, a.id, a.name);
            });
        } else if (tab == 2 && gens) {   // its genres, four covers each, as the library's
            {   // as Folders' Add all as playlists
                bool c = false;
                button(uid("sgen.addall"), ICON_PLUS, narrow ? "Add all" : "Add all as playlists", x + 28, top, false, &c, false, false, DefaultFocus);
                if (c) addServerGenresAsPlaylists();
                top += 34 + 16;
            }
            grid("srv.genres", 0, top, int(gens->size()), [&](int i, float tx, float ty) {
                const Subsonic::Genre& gn = (*gens)[size_t(i)];
                if (serverGenreTile(uid("srv.gn", int(uid(gn.name) & 0x7fffffff)), gn, tx, ty, gl.tileW, i == 0 ? DefaultFocus : 0)) openServer(3, gn.name, gn.name);
            });
        } else if (tab == 3 && folds) {   // as Library › Folders: Play, Shuffle (everything), the views without by album, the folders
            const std::vector<Subsonic::Song>* every = serverSongList();
            bool clicked = false;
            float bx = x + 28;
            bx += button(uid("srv.fplay"), ICON_PLAY, "Play", bx, top, true, &clicked, false, false, DefaultFocus) + 8;
            if (clicked && every && !every->empty()) playServer(*every, 0, kind);
            clicked = false;
            button(uid("srv.fshuffle"), ICON_SHUFFLE, "Shuffle", bx, top, false, &clicked);
            if (clicked && every && !every->empty()) playServer(shuffled(*every), 0, kind);
            int m = store.viewFolders == 0 ? 2 : store.viewFolders;
            viewSwitch("srv.fview", x + w - 28, top + 17, m, 1u);
            if (m != (store.viewFolders == 0 ? 2 : store.viewFolders)) store.viewFolders = m;
            std::vector<const Subsonic::Entry*> dirs;
            std::vector<Subsonic::Song> loose;
            for (const auto& e : *folds) { if (e.dir) dirs.push_back(&e); else loose.push_back(e.song); }
            const float lt = top + 34 + 14;
            ScrollArea& sa = area("srv.folders");
            sa.view = { x, lt, w, y + h - lt };
            ui.pushArea(&sa);
            g.clip(x, lt, w, y + h - lt);
            float hh = serverFolders("srv.fo", dirs, x, lt - sa.y, w, lt, y + h - lt, ListMode(m));
            if (!loose.empty()) {
                std::vector<int> order(loose.size());
                for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
                hh += 16 + drawServerBody("srv.floose", loose, order, x, lt - sa.y + hh + 16, w, lt, y + h - lt, ListMode(m), false,
                                          [&](int i) { playServer(loose, i, kind); });
            }
            sa.content = hh + 24;
            g.unclip();
            ui.popArea();
        } else if (tab == 4 && songs) {   // as Library › Tracks: Play, Shuffle · filter, sort, direction, the four views; the letters
            // sorted and filtered once a change, as Library's (tracksort.hpp)
            if (srvTracks_.gen != srvl_.gen || srvTracks_.sort != store.tracksSort || srvTracks_.desc != store.tracksDesc || srvTracks_.filter != srvFilter_) {
                srvTracks_.gen = srvl_.gen;
                srvTracks_.sort = store.tracksSort;
                srvTracks_.desc = store.tracksDesc;
                srvTracks_.filter = srvFilter_;
                std::vector<tracksort::SortRow> rows;
                rows.reserve(songs->size());
                for (int i = 0; i < int(songs->size()); i++) {
                    const auto& so = (*songs)[size_t(i)];
                    rows.push_back({ i, so.title, so.artist, so.album, so.genre, so.year > 0 ? std::to_string(so.year) : std::string(), so.track, so.duration });
                }
                tracksort::sortRows(rows, store.tracksSort, store.tracksDesc, srvFilter_, srvTracks_.list, srvTracks_.letters);
            }
            const std::vector<int>& list = srvTracks_.list;
            auto inOrder = [&](bool shuffle) {
                std::vector<Subsonic::Song> v;
                v.reserve(list.size());
                for (int i : list) v.push_back((*songs)[size_t(i)]);
                return shuffle ? shuffled(std::move(v)) : v;
            };
            bool clicked = false;
            float bx = x + 28;
            const float by = top;
            bx += button(uid("srv.tplay"), ICON_PLAY, "Play", bx, by, true, &clicked, false, false, DefaultFocus) + 8;
            if (clicked && !list.empty()) playServer(inOrder(false), 0, "Tracks");
            clicked = false;
            bx += button(uid("srv.tshuffle"), ICON_SHUFFLE, "Shuffle", bx, by, false, &clicked) + 8;
            if (clicked && !list.empty()) playServer(inOrder(true), 0, "Tracks");
            float ty = narrow ? by + 34 + 10 : by;
            const float fx0 = narrow ? x + 28 : bx;
            float rx = x + w - 28;
            {   // (upright: right of Play and Shuffle where they fit, else on a line of their own, as Library's)
                int mode = store.viewTracks;
                if (!narrow) rx -= viewSwitch("srv.tview", rx, by + 17, mode) + 8;
                else {
                    const float vy = bx + 6 + 38 * 4 <= x + w - 28 ? by : by + 34 + 10;
                    if (vy > by) ty += 34 + 10;
                    viewSwitch("srv.tview", x + w - 28, vy + 17, mode);
                }
                store.viewTracks = mode;
            }
            const Id did = uid("srv.tdir");
            if (ui.item(did, { rx - 36, ty, 36, 34 }, 9)) { store.tracksDesc = !store.tracksDesc; persistSoon(0.5f); }
            g.roundRect(rx - 36, ty, 36, 34, 9, pal.text, ui.focused(did) && ui.showFocusRing() ? 0.11f : 0.065f);
            g.icon(store.tracksDesc ? ICON_SORT_UP : ICON_SORT_DOWN, rx - 18, ty + 17, 17, pal.text2);
            rx -= 36 + 8;
            const Font sf{ css::Sm };
            const int so = std::clamp(store.tracksSort, 0, 5);
            const float sw = g.measure(SortNames[so], sf) + 20 + 26;
            const Id sid = uid("srv.tsort");
            if (ui.item(sid, { rx - sw, ty, sw, 34 }, 8)) { store.tracksSort = (store.tracksSort + 1) % 6; persistSoon(0.5f); }
            g.roundRect(rx - sw, ty, sw, 34, 8, pal.text, ui.focused(sid) && ui.showFocusRing() ? 0.11f : 0.065f);
            g.roundStroke(rx - sw, ty, sw, 34, 8, 1, pal.text, 0.15f);
            g.textC(SortNames[so], sf, rx - sw + 10, ty + 17, pal.text);
            g.icon(ICON_CHEV, rx - 16, ty + 17, 14, pal.text3);
            rx -= sw + 8;
            static const char* const FilterBy[] = { "Filter artists…", "Filter titles…", "Filter albums…", "Filter years…", "Filter genres…", "Filter titles…" };
            const std::string ft = srvFilter_.empty() ? FilterBy[so] : srvFilter_;
            const float fw = narrow ? std::max(80.f, rx - fx0 - 8) : std::min(220.f, std::max(120.f, rx - fx0 - 8));
            const Id fid = uid("srv.tfilter");
            if (ui.item(fid, { rx - fw, ty, fw, 34 }, 8)) {
                std::string q = srvFilter_;
                static const char* const Header[] = { "Filter by artist", "Filter by title", "Filter by album", "Filter by year", "Filter by genre", "Filter by title" };
                if (keyboard(Header[so], srvFilter_, q)) srvFilter_ = q;
            }
            g.roundRect(rx - fw, ty, fw, 34, 8, pal.text, ui.focused(fid) && ui.showFocusRing() ? 0.11f : 0.065f);
            g.roundStroke(rx - fw, ty, fw, 34, 8, 1, pal.text, 0.15f);
            g.icon(ICON_SEARCH, rx - fw + 16, ty + 17, 14, pal.text3);
            g.textC(ft, sf, rx - fw + 30, ty + 17, srvFilter_.empty() ? pal.text3 : pal.text, 1, Align::Left, int(fw - 40));

            const float lt = ty + 34 + 14;
            const bool index = !srvTracks_.letters.empty() && store.viewTracks != int(ListMode::Grid);
            const float lw = w - (index ? 26 : 0);
            ScrollArea& sa = area("srv.tracks");
            sa.view = { x, lt, w, y + h - lt };
            ui.pushArea(&sa);
            g.clip(x, lt, w, y + h - lt);
            if (list.empty()) {
                g.textC(srvFilter_.empty() ? "No tracks on the server" : "No tracks match", { css::Lg, Weight::SemiBold }, x + w / 2, lt + 80, pal.text2, 1, Align::Center);
                sa.content = 0;
            } else {
                sa.content = drawServerBody("srv.trk", *songs, list, x, lt - sa.y, lw, lt, y + h - lt, ListMode(store.viewTracks), true,
                                            [&](int i) { std::vector<Subsonic::Song> v = inOrder(false); playServer(v, i, "Tracks"); });
            }
            g.unclip();
            ui.popArea();
            if (index) {   // #-to-Z down the side, only the letters present
                const auto& L = srvTracks_.letters;
                const float top2 = lt + 8, avail = y + h - top2 - 8;
                const float step = std::min(22.f, avail / L.size());
                const std::vector<ListEntry> layout = serverLayout(*songs, list, lw, ListMode(store.viewTracks));
                for (int i = 0; i < int(L.size()); i++) {
                    const float ly = top2 + i * step;
                    const Id id = uid("srv.az", L[size_t(i)].first);
                    if (ui.item(id, { x + w - 24, ly, 22, step }, 6)) {
                        sa.target = listRowY(layout, L[size_t(i)].second);
                        sa.clamp();
                    }
                    const bool hot = ui.focused(id) && ui.showFocusRing();
                    g.textC(std::string(1, L[size_t(i)].first), { css::Xs, Weight::SemiBold }, x + w - 13, ly + step / 2, hot ? pal.text : pal.text2, 1, Align::Center);
                }
            }
        }

        return;
    }

    if (level == 5) {   // a server playlist: its cover and line, Play, Shuffle, Save to this device; its tracks in the playlists' view
        const SrvPage& pg = serverPage('P', id);
        const std::string title = pg.name.empty() ? name : pg.name;
        const float cs = narrow ? 96.f : 120.f;
        const float headH = narrow ? 18 + cs + 14 + 34 + 10 + 36 + 16 : 18 + 120 + 14;
        ScrollArea& sa = area("srv.plpage", int(uid(id) & 0x7fffffff));
        sa.view = { x, y, w, h };
        ui.pushArea(&sa);
        g.clip(x, y, w, h);
        const float oy = y - sa.y, cyTop = oy + 18;
        if (SDL_Texture* t = serverCover(pg.cover, 500)) g.image(t, x + 28, cyTop, cs, cs, 12);
        else { g.roundRect(x + 28, cyTop, cs, cs, 12, pal.surface2); g.icon(ICON_LIST, x + 28 + cs / 2, cyTop + cs / 2, cs * 0.36f, pal.text3); }
        const float tx = narrow ? x + 28 : x + 28 + cs + 22;
        float ty = narrow ? cyTop + 2 : cyTop + 2;
        if (!narrow) {
            kicker("Playlist", tx, ty + 8, pal.text3);
            g.text(title, { css::Xxl, Weight::Bold }, tx - 2, ty + 18, pal.text, 1, Align::Left, int(w - (tx - x) - 30));
        }
        double secs = 0;
        for (const auto& so : pg.songs) secs += so.duration;
        const int m = int(secs / 60);
        const std::string meta = !pg.have ? std::string("Asking the server…") : std::to_string(pg.songs.size()) + (pg.songs.size() == 1 ? " track · " : " tracks · ") +
                                                                                  (m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min" : std::to_string(m) + " min");
        if (!narrow) g.text(meta, { css::Sm }, tx, ty + 58, pal.text3, 1, Align::Left, int(w - (tx - x) - 30));
        else {
            kicker("Playlist", x + 28 + cs + 16, cyTop + 20, pal.text3);
            g.text(title, { css::Lg, Weight::Bold }, x + 28 + cs + 16, cyTop + 32, pal.text, 1, Align::Left, int(w - cs - 72));
            g.text(meta, { css::Sm }, x + 28 + cs + 16, cyTop + 62, pal.text3, 1, Align::Left, int(w - cs - 72));
        }
        const float by = narrow ? cyTop + cs + 14 : cyTop + 120 - 34;
        float bx = tx;
        bool clicked = false;
        bx += button(uid("srv.plplay"), ICON_PLAY, "Play", bx, by, true, &clicked, false, false, DefaultFocus) + 8;
        if (clicked && !pg.songs.empty()) playServer(pg.songs, 0, title);
        clicked = false;
        bx += button(uid("srv.plshuffle"), ICON_SHUFFLE, "Shuffle", bx, by, false, &clicked) + 8;
        if (clicked && !pg.songs.empty()) playServer(shuffled(pg.songs), 0, title);
        clicked = false;
        bx += button(uid("srv.plrename"), ICON_TAG, "Rename", bx, by, false, &clicked) + 8;
        if (clicked) serverRenamePlaylist(id, title);
        clicked = false;
        bx += button(uid("srv.plsave"), ICON_PLUS, "Save to " GF_THIS_CARD, bx, by, false, &clicked) + 8;
        if (clicked && pg.have) savePlaylistHere(title, pg.songs);
        {   // its sidebar icon, as a round button wearing it: the icon grid opens at once (as yours)
            const Id iid = uid("srv.plicon");
            hint(iid, "The playlist's icon");
            if (ui.item(iid, { bx, by, 34, 34 }, 17)) {
                Subsonic::Playlist p;
                p.id = id;
                p.name = title;
                p.songs = int(pg.songs.size());
                openServerPlaylistMenu(p, true);
            }
            const bool hot = ui.pressed(iid) || (ui.focused(iid) && ui.showFocusRing());
            g.roundRect(bx, by, 34, 34, 17, pal.accent, hot ? 0.24f : 0.14f);
            g.roundStroke(bx, by, 34, 34, 17, 1, pal.accent, 0.45f);
            g.icon(serverPlaylistIcon(id), bx + 17, by + 17, 17, pal.accentInk);
            bx += 34 + 8;
        }
        clicked = false;
        bx += button(uid("srv.pldel"), ICON_X, "Delete", bx, by, false, &clicked, false, true) + 8;   // (always the last, as on yours)
        if (clicked) serverDeletePlaylist(id, title, int(pg.songs.size()));
        int mode = store.viewPlaylist;
        viewSwitch("srv.plview", x + w - 28, narrow ? by + 34 + 10 + 18 : by + 17, mode);
        store.viewPlaylist = mode;
        std::vector<int> order(pg.songs.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
        bodyGrab = grab.kind == Grab::SrvList && srvGrabPl_ == id ? grab.pos : -1;
        const float bodyH = drawServerBody("srv.plt", pg.songs, order, x, oy + headH, w, y, h, ListMode(mode), false, [&](int i) { playServer(pg.songs, i, title); });
        bodyGrab = -1;
        if (pg.have && pg.songs.empty())
            g.textC("This playlist has no tracks: add them from a track's menu (" GF_MENU_KEY ")", { css::Base }, x + w / 2, oy + headH + 70, pal.text3, 1, Align::Center);
        sa.content = headH + bodyH;
        g.unclip();
        ui.popArea();
        return;
    }

    if (level == 1 || level == 3) {   // an artist's page or a genre's, as the library's (view_grid.cpp drawArtist)
        const bool genre = level == 3;
        const SrvPage& pg = serverPage(genre ? 'G' : 'R', id);
        const auto& albums = pg.albums;
        // its tracks: from the server's list of every track (asked once), the artist's by id, the genre's by name
        static std::vector<Subsonic::Song> tracks;
        static std::string tracksFor;
        static unsigned tracksGen = ~0u;
        const std::vector<Subsonic::Song>* all = serverSongList();
        if (all && (tracksFor != route.query || tracksGen != srvl_.gen)) {
            tracksFor = route.query;
            tracksGen = srvl_.gen;
            tracks.clear();
            for (const auto& so : *all) if (genre ? so.genre == id : so.artistId == id) tracks.push_back(so);
        } else if (!all && tracksFor != route.query) { tracks.clear(); tracksFor.clear(); }
        // a genre's artists: the server's artists with an album in it
        std::vector<const Subsonic::Artist*> inGenre;
        if (genre) {
            if (const auto* arts = serverArtistList())
                for (const auto& a : *arts)
                    if (std::any_of(albums.begin(), albums.end(), [&](const Subsonic::Album& al) { return al.artistId == a.id; })) inGenre.push_back(&a);
        }
        std::string cover;
        if (!genre) if (const auto* arts = serverArtistList()) for (const auto& a : *arts) if (a.id == id) { cover = a.cover; break; }

        const float ncs = std::min(w - 60, 240.f);
        const std::vector<std::string> nameLines = narrow ? g.wrap(name, { css::Xxl, Weight::Bold }, int(w - 60), 2) : std::vector<std::string>{ name };
        const float heroH = narrow ? 22 + ncs + 18 + 24 + nameLines.size() * 38 + 8 + 22 + 16 + 34 + 12 : 26 + 220 + 22;
        static int artistTabS = 0, genreTabS = 0;
        int& chosen = genre ? genreTabS : artistTabS;
        int tab = std::clamp(chosen, 0, 2);
        if (!genre && tab == 1) tab = 0;
        const float switchH = 56 + (narrow && tab == 2 ? 46.f : 0.f);
        const int gridItems = tab == 1 ? int(inGenre.size()) : int(albums.size());
        const int rows = (gridItems + gl.cols - 1) / gl.cols;
        const float gridH = (tab == 2 || gridItems == 0) ? 0 : 10 + rows * gl.tileH;
        static float pageTracksH = 0;   // the tracks' height, as drawn last frame (their layout depends on the view)
        const float songsH = tab == 2 ? pageTracksH : 0;
        ScrollArea& sa = area(genre ? "srv.genre" : "srv.artist", int(uid(id) & 0x7fffffff));
        sa.view = { x, y, w, h };
        sa.content = heroH + switchH + gridH + songsH + 24;
        ui.pushArea(&sa);
        g.clip(x, y, w, h);
        const float oy = y - sa.y;
        const float cs = narrow ? ncs : 220.f;
        const float cx = narrow ? x + (w - cs) / 2 : x + 30, cyTop = oy + (narrow ? 22 : 26);
        if (genre) serverQuad(serverGenreCovers(id), cx, cyTop, cs, 16);
        else if (SDL_Texture* t = serverCover(cover, 500)) { g.image(t, cx, cyTop, cs, cs, cs / 2); g.roundStroke(cx, cyTop, cs, cs, cs / 2, 1, pal.text, 0.09f); }
        else { g.roundRect(cx, cyTop, cs, cs, cs / 2, pal.surface2); g.icon(ICON_MIC, cx + cs / 2, cyTop + cs / 2, cs * 0.28f, pal.text3); }
        const float tx = narrow ? x + 30 : cx + 220 + 28, tw = w - (tx - x) - 30;
        float ty = narrow ? cyTop + cs + 18 : cyTop + 220 - 34 - 16 - 22 - 8 - 48 - 6 - 18;
        kicker(genre ? "Genre" : "Artist", tx, ty + 8, pal.text3);
        ty += 24;
        if (narrow) for (const auto& l : nameLines) { g.text(l, { css::Xxl, Weight::Bold }, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw)); ty += 38; }
        else { g.text(name, { 46, Weight::ExtraBold }, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw)); ty += 48; }
        ty += 8;
        std::string meta2 = !pg.have ? std::string("Asking the server…") : std::to_string(albums.size()) + (albums.size() == 1 ? " album" : " albums");
        if (pg.have && all) {
            double secs = 0;
            for (const auto& so : tracks) secs += so.duration;
            const int m = int(secs / 60);
            meta2 += " · " + std::to_string(tracks.size()) + (tracks.size() == 1 ? " track · " : " tracks · ") +
                     (m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min" : std::to_string(m) + " min");
        }
        g.text(meta2, { css::Base }, tx, ty, pal.text2, 1, Align::Left, int(tw));
        ty += 22 + 16;
        float bx = tx;
        bool clicked = false;
        bx += button(uid("sar.play"), ICON_PLAY, "Play", bx, ty, true, &clicked, false, false, DefaultFocus) + 8;
        if (clicked && !tracks.empty()) playServer(tracks, 0, name);
        clicked = false;
        bx += button(uid("sar.shuffle"), ICON_SHUFFLE, "Shuffle", bx, ty, false, &clicked) + 8;
        if (clicked && !tracks.empty()) playServer(shuffled(tracks), 0, name);
        if (genre) serverGenrePlaylistButton(id, bx, ty);   // as a folder's Add as playlist

        float gy = oy + heroH + switchH;
        {   // the switch, as the library's artist page: Albums | Tracks (a genre: Albums | Artists | Tracks)
            const float sy = oy + heroH + 22;
            std::string count;
            if (tab == 0) count = std::to_string(albums.size()) + (albums.size() == 1 ? " album" : " albums");
            else if (tab == 1) count = std::to_string(inGenre.size()) + (inGenre.size() == 1 ? " artist" : " artists");
            else count = std::to_string(tracks.size()) + (tracks.size() == 1 ? " track" : " tracks");
            g.textC(count, { css::Lg, Weight::Bold }, x + 30, sy, pal.text);
            int picked = -1;
            float rx = x + w - 30;
            if (tab == 2) {   // on Tracks the four views take the right edge, as the library's (upright: under the switch)
                int mode = store.viewGenre;
                if (!narrow) rx -= viewSwitch(genre ? "sgen.view" : "sar.view", rx, sy, mode) + 10;
                else viewSwitch(genre ? "sgen.view" : "sar.view", rx, sy + 46, mode);
                store.viewGenre = mode;
            }
            const std::vector<std::string> names = genre ? std::vector<std::string>{ "Albums", "Artists", "Tracks" } : std::vector<std::string>{ "Albums", "Tracks" };
            const int shown = genre ? tab : (tab == 2 ? 1 : 0);
            segmented(uid(genre ? "sgen.tab" : "sar.tab"), rx, sy, names, shown, &picked);
            if (picked >= 0 && picked != shown) chosen = genre ? picked : (picked == 1 ? 2 : 0);
        }
        if (tab == 1) {
            for (int i = 0; i < int(inGenre.size()); i++) {
                const float ty2 = gy + (i / gl.cols) * gl.tileH;
                if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
                const Subsonic::Artist& a = *inGenre[size_t(i)];
                if (serverTile(uid("sgen.ar", int(uid(a.id) & 0x7fffffff)), a.cover, gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, true, a.name,
                               std::to_string(a.albums) + (a.albums == 1 ? " album" : " albums")))
                    openServer(1, a.id, a.name);
            }
        } else if (tab == 0) {
            for (int i = 0; i < int(albums.size()); i++) {
                const float ty2 = gy + (i / gl.cols) * gl.tileH;
                if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
                const Subsonic::Album& al = albums[size_t(i)];
                const std::string sub = genre ? al.artist : al.year > 0 ? std::to_string(al.year) : std::to_string(al.songs) + (al.songs == 1 ? " track" : " tracks");
                if (serverTile(uid("srv.al", int(uid(al.id) & 0x7fffffff)), al.cover, gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, false, al.name, sub))
                    openServer(2, al.id, al.name);
                if (subsonic.albumDownload(al.id).state == 2) serverBadge(gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, true);
            }
        } else {
            if (!all) g.textC("Asking the server…", { css::Base }, x + w / 2, gy + 40, pal.text3, 1, Align::Center);
            std::vector<int> order(tracks.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
            pageTracksH = drawServerBody(genre ? "sgen.trk" : "sar.trk", tracks, order, x, gy, w, y, h, ListMode(store.viewGenre), false,
                                         [&](int i) { playServer(tracks, i, name); });
        }
        g.unclip();
        ui.popArea();
        return;
    }

    if (level == 4) {   // a folder, as the library's folder page: Play, Shuffle, the views; its folders, then its tracks
        const SrvPage& pg = serverPage('D', id);
        std::vector<const Subsonic::Entry*> dirs;
        std::vector<Subsonic::Song> songs;
        for (const auto& e : pg.entries) { if (e.dir) dirs.push_back(&e); else songs.push_back(e.song); }
        const std::string title = pg.name.empty() ? name : pg.name;
        std::string sub = !pg.have ? std::string("Asking the server…") : std::to_string(dirs.size()) + (dirs.size() == 1 ? " folder · " : " folders · ") +
                                                                              std::to_string(songs.size()) + (songs.size() == 1 ? " track" : " tracks");
        const float headBottom = viewHead("Folder", title, sub, x, y, w);
        const bool own = !songs.empty();
        const int shownMode = !own && store.viewFolders == 0 ? 2 : store.viewFolders;
        bool clicked = false;
        float bx = x + 28;
        if (own) {
            bx += button(uid("srv.dplay"), ICON_PLAY, "Play", bx, headBottom, true, &clicked, false, false, DefaultFocus) + 8;
            if (clicked) playServer(songs, 0, title);
            clicked = false;
            button(uid("srv.dshuffle"), ICON_SHUFFLE, "Shuffle", bx, headBottom, false, &clicked);
            if (clicked) playServer(shuffled(songs), 0, title);
        }
        float vy = headBottom;
        {
            int m = shownMode;
            if (narrow && bx + 6 + 38.f * (own ? 4 : 3) > x + w - 28) vy += 34 + 10;   // upright: a line of its own where no room
            viewSwitch("srv.dview", x + w - 28, vy + 17, m, own ? 0u : 1u);
            if (m != shownMode) store.viewFolders = m;
        }
        const float lt = vy + 34 + 16;
        ScrollArea& sa = area("srv.dir", int(uid(id) & 0x7fffffff));
        sa.view = { x, lt, w, y + h - lt };
        ui.pushArea(&sa);
        g.clip(x, lt, w, y + h - lt);
        float hh = serverFolders("srv.fo", dirs, x, lt - sa.y, w, lt, y + h - lt, ListMode(shownMode));
        if (own) {
            std::vector<int> order(songs.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
            hh += 16 + drawServerBody("srv.dsong", songs, order, x, lt - sa.y + hh + 16, w, lt, y + h - lt, ListMode(shownMode), false,
                                      [&](int i) { playServer(songs, i, title); });
        } else if (pg.have && dirs.empty()) {
            g.textC("This folder is empty", { css::Lg, Weight::SemiBold }, x + w / 2, lt + 60, pal.text2, 1, Align::Center);
        }
        sa.content = hh + 24;
        g.unclip();
        ui.popArea();
        return;
    }

    // an album's songs: its cover and what it is, then the songs
    Subsonic::Album al;
    std::vector<Subsonic::Song> songs;
    const bool have = subsonic.album(id, al, songs);
    if (!have) askOnce("L:" + id, [this, id = id] { subsonic.askAlbum(id); });
    const float cs = narrow ? std::min(w - 60, 220.f) : 200.f, rowH = 46;
    const float heroH = narrow ? 22 + cs + 18 + 24 + 38 + 30 + 12 + 46 : 26 + cs + 26;
    ScrollArea& sa = area("srv.album", int(uid(id) & 0x7fffffff));
    sa.view = { x, y, w, h };
    sa.content = heroH + songs.size() * rowH + 24;
    ui.pushArea(&sa);
    g.clip(x, y, w, h);
    const float oy = y - sa.y;
    const float cx = narrow ? x + (w - cs) / 2 : x + 30, cy = oy + (narrow ? 22 : 26);
    if (SDL_Texture* t = serverCover(have ? al.cover : std::string(), 500)) g.image(t, cx, cy, cs, cs, 12);
    else {
        g.roundRect(cx, cy, cs, cs, 12, pal.text, 0.06f);
        g.icon(ICON_DISC, cx + cs / 2, cy + cs / 2, cs * 0.28f, pal.text3);
    }
    g.roundStroke(cx, cy, cs, cs, 12, 1, pal.text, 0.09f);
    const float tx = narrow ? x + 30 : cx + cs + 28, tw = w - (tx - x) - 30;
    float ty = narrow ? cy + cs + 18 : cy + cs - 24 - 46 - 26 - 46;
    kicker("Album", tx, ty + 8, pal.text3);
    ty += 24;
    g.text(have ? al.name : name, { narrow ? css::Xxl : 40, Weight::ExtraBold }, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw));
    ty += narrow ? 38 : 46;
    std::string meta;
    if (have) {
        meta = al.artist;
        if (al.year > 0) meta += " · " + std::to_string(al.year);
        meta += " · " + std::to_string(songs.size()) + (songs.size() == 1 ? " track" : " tracks");
        if (al.duration > 0) meta += " · " + std::to_string(int(std::lround(al.duration / 60))) + " min";
    } else {
        meta = "Asking the server…";
    }
    g.text(meta, { css::Base }, tx, ty, pal.text2, 1, Align::Left, int(tw));
    const Subsonic::DlState dl = have ? subsonic.albumDownload(al.id) : Subsonic::DlState{};
    if (have && !songs.empty()) {
        bool clicked = false;
        float bx = tx + button(uid("srv.play"), ICON_PLAY, "Play", tx, ty + 30, true, &clicked, false, false) + 8;
        if (clicked) playServer(songs, 0, al.name);
        clicked = false;
        if (dl.state == 0) {   // Download: the whole album kept (Settings › Library › Subsonic server › Downloads: where)
            button(uid("srv.dl"), ICON_DOWNLOAD, "Download", bx, ty + 30, false, &clicked, false, false);
            if (clicked) {
                subsonic.downloadAlbum(al.id, serverDlRoot());
                toast("Downloading “" + al.name + "”");
            }
        } else if (dl.state == 1) {   // coming: Cancel (what came goes too), and how far
            bx += button(uid("srv.dl"), ICON_X, "Cancel", bx, ty + 30, false, &clicked, false, false) + 14;
            if (clicked) { subsonic.removeDownload(al.id); toast("Download cancelled"); }
            const std::string where = store.stopDownloads ? "Downloads stopped · " : "Downloading · ";
            g.textNum(where + std::to_string(dl.have) + " of " + std::to_string(dl.total > 0 ? dl.total : int(songs.size())), { css::Sm }, bx,
                      ty + 30 + 18 - g.lineHeight({ css::Sm }) / 2.f, pal.text3);
        } else {   // all here: Remove
            bool c2 = false;
            bx += button(uid("srv.dlx"), ICON_X, "Remove download", bx, ty + 30, false, &c2, false, false) + 14;
            if (c2) { subsonic.removeDownload(al.id); toast("Removed “" + al.name + "” from this device"); }
            g.icon(ICON_DOWNLOAD, bx + 8, ty + 30 + 18, 15, pal.accentInk);
            g.textC("Downloaded", { css::Sm }, bx + 22, ty + 30 + 18, pal.text3);
        }
    }

    float ry = oy + heroH;
    for (size_t i = 0; i < songs.size(); i++, ry += rowH) {
        if (ry + rowH < y || ry > y + h) continue;
        const Subsonic::Song& s = songs[i];
        const Id rid = uid("srv.song", int(i));
        if (ui.item(rid, { x + 18, ry, w - 36, rowH - 4 }, 10, i == 0 ? DefaultFocus : 0)) playServer(songs, int(i), al.name);
        hoverServer(rid, s);
        const bool hot = ui.pressed(rid) || (ui.focused(rid) && ui.showFocusRing());
        const bool playing = srvq_.on && srvq_.pos >= 0 && srvq_.songs[size_t(srvq_.pos)].id == s.id;
        if (playing) g.roundRect(x + 18, ry, w - 36, rowH - 4, 10, pal.accent, 0.12f);
        if (hot) g.roundRect(x + 18, ry, w - 36, rowH - 4, 10, pal.text, ui.pressed(rid) ? 0.11f : 0.065f);
        if (playing && !srvq_.waitFor.empty()) g.textC("Getting it from the server…", { css::Xs }, x + w - 36 - 70, ry + (rowH - 4) / 2, pal.accentInk, 1, Align::Right);
        const float my = ry + (rowH - 4) / 2;
        g.textNum(s.track > 0 ? std::to_string(s.track) : "·", { css::Sm }, x + 52, my - g.lineHeight({ css::Sm }) / 2.f, pal.text3, 1, Align::Right);
        const std::string dur = s.duration > 0 ? fmtTime(s.duration) : "";
        const float dw = float(g.measureNum(dur, { css::Sm }));
        const bool other = !s.artist.empty() && s.artist != al.artist;   // (a guest: the song's own artist beside its title)
        const float titleW = w - 36 - 52 - 24 - dw - 24;
        g.textC(s.title, { css::Base, Weight::SemiBold }, x + 70, other ? my - 9 : my, pal.text, 1, Align::Left, int(titleW));
        if (other) g.textC(s.artist, { css::Sm }, x + 70, my + 10, pal.text3, 1, Align::Left, int(titleW));
        g.textNum(dur, { css::Sm }, x + w - 36, my - g.lineHeight({ css::Sm }) / 2.f, pal.text3, 1, Align::Right);
        std::string kept;
        if (dl.state && subsonic.downloaded(s.id, kept)) g.icon(ICON_DOWNLOAD, x + w - 36 - dw - 20, my, 15, pal.accentInk);   // on this device
    }
    g.unclip();
    ui.popArea();
}
