/*
 * Gatefold NX · view_grid.cpp
 * #view-library (album tiles), #view-artists and the artist page, and the same
 * two views for genres. Albums, artists and genres come from tags, whatever the
 * folder layout is.
 */
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {
constexpr float RowH = 40;

struct GridLayout {
    int cols;
    float tileW, tileH, x0;
};
GridLayout gridFor(float x, float w) {
    const float inner = w - 36;
    const int cols = std::max(2, int(inner / 200));
    const float tw = inner / cols;
    const float cover = tw - 20;
    return { cols, tw, 10 + cover + 8 + 20 + 2 + 18 + 10, x + 18 };
}
}  // namespace

/** .tile: 10 px padding, 10 px-rounded cover (round for artists), title, subtitle. */
bool App::tile(Id id, int coverTrack, float x, float y, float w, bool round, const std::string& title, const std::string& sub,
               const std::vector<int>* quad, uint8_t flags) {
    const float cover = w - 20;
    const float h = 10 + cover + 8 + 20 + 2 + 18 + 10;
    const bool hit = ui.item(id, { x, y, w, h }, 14, flags);
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (hot) g.roundRect(x, y, w, h, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float rad = round ? cover / 2 : 10;
    // Gatefold's shadow under the cover (0 10px 24px -14px) is left out at the user's request, as on Now playing
    if (quad) coverQuad(*quad, x + 10, y + 10, cover, rad);   // it draws its own outline
    else {
        coverBox(coverTrack, x + 10, y + 10, cover, rad);
        g.roundStroke(x + 10, y + 10, cover, cover, rad, 1, pal.text, 0.09f);
    }
    if (hot && !round) {
        // .tile .cov .play — shown on hover in Gatefold, on focus here
        g.circle(x + 10 + cover - 31, y + 10 + cover - 31, 21, pal.accent);
        g.icon(ICON_PLAY, x + 10 + cover - 30, y + 10 + cover - 31, 18, pal.onAccent);
    }
    const float ty = y + 10 + cover + 8;
    const float tx = round ? x + w / 2 : x + 10;
    const Align al = round ? Align::Center : Align::Left;
    g.text(title, { css::Base, Weight::SemiBold }, tx, ty, pal.text, 1, al, int(cover));
    g.text(sub, { css::Sm }, tx, ty + 22, pal.text3, 1, al, int(cover));
    return hit;
}

/** The album grid below a header ending at headBottom: the sidebar's Albums and Library → Albums. */
void App::drawAlbumGrid(float x, float y, float w, float h, float headBottom) {
    // .lib-tools: the sort select, as a button that cycles
    static const char* sorts[] = { "By artist", "By title", "By year", "By folder" };
    const Font sf{ css::Sm };
    const float sw = g.measure(sorts[librarySort], sf) + 20 + 26, sx = x + w - 28 - sw, sy = headBottom - 14 - 32;
    const Id sid = uid("lib.sort");
    if (ui.item(sid, { sx, sy, sw, 32 }, 8)) {
        librarySort = (librarySort + 1) % 4;
        rebuildIndex();
    }
    const bool hot = ui.focused(sid) && ui.showFocusRing();
    g.roundRect(sx, sy, sw, 32, 8, pal.text, hot ? 0.11f : 0.065f);
    g.roundStroke(sx, sy, sw, 32, 8, 1, pal.text, 0.15f);
    g.textC(sorts[librarySort], sf, sx + 10, sy + 16, pal.text);
    g.icon(ICON_CHEV, sx + sw - 16, sy + 16, 14, pal.text3);
    const bool reading = meta.taggedCount() < int(lib.tracks.size());
    if (reading) {
        const int pct = lib.tracks.empty() ? 100 : meta.taggedCount() * 100 / int(lib.tracks.size());
        g.textNum("Reading tags · " + std::to_string(pct) + "%", { css::Xs }, sx - 14, sy + 16 - g.lineHeight({ css::Xs }) / 2.f, pal.text3, 1, Align::Right);
    }

    if (lib.albums.empty() && albumMix().empty()) {   // (the server's albums, where they are In the library, are enough)
        if (store.serverPlace == 1 && subsonic.view().state != Subsonic::SignedOut && lib.tracks.empty()) {   // the server's alone, still coming
            g.textC("Asking the server…", { css::Lg, Weight::SemiBold }, x + w / 2, headBottom + 90, pal.text2, 1, Align::Center);
            return;
        }
        const char* msg = lib.tracks.empty() ? "No music found" : reading ? "Reading tags…" : "No album tags";
        const std::string hint = lib.tracks.empty()
                                   ? "Choose your music folder in Settings."
                                   : reading ? "Albums appear as their tags are read. Your music is already in Folders, Artists and search."
                                             : "These files have no album tags. Browse them in Folders or Artists, or search with Y.";
        g.textC(msg, { css::Lg, Weight::SemiBold }, x + w / 2, headBottom + 90, pal.text2, 1, Align::Center);
        g.textC(hint, { css::Base }, x + w / 2, headBottom + 120, pal.text3, 1, Align::Center, int(w - 80));
        if (lib.tracks.empty()) {
            bool c = false;
            const float bw = g.measure("Choose music folder", { css::Sm, Weight::SemiBold }) + 56;
            button(uid("lib.pick"), ICON_FOLDER, "Choose music folder", x + (w - bw) / 2, headBottom + 150, true, &c, false, false, DefaultFocus);
            if (c) navigate(View::PickFolder, -1, plat::root());
        }
        return;
    }

    const GridLayout gl = gridFor(x, w);
    ScrollArea& sa = area("library");
    sa.view = { x, headBottom, w, y + h - headBottom };
    // the server's albums among yours (Its music: In the library; the user chose "A, mixed in"), each with the server's mark
    const std::vector<std::pair<int, int>>& order = albumMix();
    const std::vector<Subsonic::Album>& srv = serverAlbums_;
    const int rows = (int(order.size()) + gl.cols - 1) / gl.cols;
    sa.content = rows * gl.tileH + 24;
    ui.pushArea(&sa);
    g.clip(x, headBottom, w, y + h - headBottom);
    const int firstRow = std::max(0, int(sa.y / gl.tileH) - 1);
    const int lastRow = std::min(rows, int((sa.y + sa.view.h) / gl.tileH) + 2);
    for (int r = firstRow; r < lastRow; r++)
        for (int c = 0; c < gl.cols; c++) {
            const int i = r * gl.cols + c;
            if (i >= int(order.size())) break;
            const float tx = gl.x0 + c * gl.tileW, ty = headBottom + r * gl.tileH - sa.y;
            const auto [local, server] = order[size_t(i)];
            if (local >= 0) {
                const Album& al = lib.albums[size_t(local)];
                const Id tid = uid("lib.tile", int(uid(al.key) & 0x7fffffff));
                // The first album is where the page opens (not the sort button above it); Up from the top row goes to
                // the sort button, which the layout put out of reach from the left (the search field was nearer)
                if (r == 0) ui.link(tid, 0, -1, sid);
                if (tile(tid, al.tracks[0], tx, ty, gl.tileW, false, al.title, al.artist, nullptr, i == 0 ? DefaultFocus : 0)) openAlbum(local);
            } else if (server < int(srv.size())) {
                const Subsonic::Album& al = srv[size_t(server)];
                const Id tid = uid("srv.al", int(uid(al.id) & 0x7fffffff));
                if (r == 0) ui.link(tid, 0, -1, sid);
                if (serverTile(tid, al.cover, tx, ty, gl.tileW, false, al.name, al.artist, i == 0 ? DefaultFocus : 0)) openServer(2, al.id, al.name);
                serverBadge(tx, ty, gl.tileW, subsonic.albumDownload(al.id).state == 2);   // the server's mark; downloaded, the arrow (the user's A)
            }
        }
    g.unclip();
    ui.popArea();
}

/** The Albums grid's order: yours (albumOrder), and while the server's music is In the library its albums among them, by the same
 *  sort (artist or title; by year and by folder they follow yours). Made again only when one of them changed, not each frame. */
const std::vector<std::pair<int, int>>& App::albumMix() {
    const bool withServer = store.serverPlace == 1 && !serverAlbums().empty();
    const unsigned srvGen = withServer ? serverAlbumsGen_ : ~0u;
    if (albumMixIndex_ == indexGen && albumMixSort_ == librarySort && albumMixSrv_ == srvGen && albumMixLocal_ == albumOrder.size()) return albumMix_;
    albumMixIndex_ = indexGen;
    albumMixSort_ = librarySort;
    albumMixSrv_ = srvGen;
    albumMixLocal_ = albumOrder.size();
    albumMix_.clear();
    for (int i : albumOrder) albumMix_.push_back({ i, -1 });
    if (!withServer) return albumMix_;
    {
        auto low = [](std::string t) { for (char& ch : t) ch = char(tolower(uint8_t(ch))); return t; };
        std::unordered_set<std::string> mine;
        for (int i : albumOrder) mine.insert(low(lib.albums[size_t(i)].artist) + "\x1f" + low(lib.albums[size_t(i)].title));
        for (int i = 0; i < int(serverAlbums_.size()); i++)
            if (!mine.count(low(serverAlbums_[size_t(i)].artist) + "\x1f" + low(serverAlbums_[size_t(i)].name))) albumMix_.push_back({ -1, i });
    }
    if (librarySort <= 1) {
        auto low = [](std::string s) { for (char& c : s) c = char(tolower(uint8_t(c))); return s; };
        std::vector<std::pair<std::string, std::string>> key(albumMix_.size());
        for (size_t k = 0; k < albumMix_.size(); k++) {
            const auto [l, sv] = albumMix_[k];
            const std::string ar = l >= 0 ? lib.albums[size_t(l)].artist : serverAlbums_[size_t(sv)].artist;
            const std::string ti = l >= 0 ? lib.albums[size_t(l)].title : serverAlbums_[size_t(sv)].name;
            key[k] = librarySort == 1 ? std::make_pair(low(ti), low(ar)) : std::make_pair(low(ar), low(ti));
        }
        std::vector<size_t> idx(albumMix_.size());
        for (size_t k = 0; k < idx.size(); k++) idx[k] = k;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t p, size_t q) { return key[p] < key[q]; });
        std::vector<std::pair<int, int>> sorted;
        for (size_t k : idx) sorted.push_back(albumMix_[k]);
        albumMix_ = std::move(sorted);
    }
    return albumMix_;
}

void App::drawArtists(float x, float y, float w, float h, bool genre, bool inLibrary) {
    const std::vector<Artist>& list = genre ? genres : artists;
    // the server's among yours (Its music: In the library), one tile for a name both have (the user: "One tile, both inside")
    const std::vector<std::pair<int, int>>& mix = artistMix(genre);
    const size_t theirs = mix.size() - list.size();
    std::string sub = genre && list.empty() && !theirs ? std::string("No genre tags yet") : std::to_string(list.size()) + (genre ? (list.size() == 1 ? " genre" : " genres") : (list.size() == 1 ? " artist" : " artists"));
    if (theirs) {
        std::string kind = subsonic.view().kind;
        kind = kind.empty() ? std::string("your server") : std::string(1, char(toupper(uint8_t(kind[0])))) + kind.substr(1);
        sub += " · " + std::to_string(theirs) + " more on " + kind;
    }
    float headBottom = viewHead("Library", genre ? "Genres" : "Artists", sub, x, y, w);
    if (inLibrary) libraryTabsRow(x, w, y);
    if (genre && !genres.empty()) {   // as Folders' Add all as playlists
        float bx = x + 28;
        bool c = false;
        bx += button(uid("gen.addall"), ICON_PLUS, w < kNarrowList ? "Add all" : "Add all as playlists", bx, headBottom, false, &c, false, false, DefaultFocus) + 8;
        if (c) addGenresAsPlaylists();
        headBottom += 34 + 16;
    }
    const GridLayout gl = gridFor(x, w);
    ScrollArea& sa = area(genre ? "genres" : "artists");
    sa.view = { x, headBottom, w, y + h - headBottom };
    const int rows = (int(mix.size()) + gl.cols - 1) / gl.cols;
    sa.content = rows * gl.tileH + 24;
    ui.pushArea(&sa);
    g.clip(x, headBottom, w, y + h - headBottom);
    const int firstRow = std::max(0, int(sa.y / gl.tileH) - 1);
    const int lastRow = std::min(rows, int((sa.y + sa.view.h) / gl.tileH) + 2);
    for (int r = firstRow; r < lastRow; r++)
        for (int c = 0; c < gl.cols; c++) {
            const int i = r * gl.cols + c;
            if (i >= int(mix.size())) break;
            const float tx = gl.x0 + c * gl.tileW, ty = headBottom + r * gl.tileH - sa.y;
            const auto [local, server] = mix[size_t(i)];
            if (local < 0) {   // the server's alone: its tile, its mark; its page is the server's
                if (genre) {
                    const Subsonic::Genre& gn = srvl_.genres[size_t(server)];
                    if (serverGenreTile(uid("gen.tile", int(uid(gn.name) & 0x7fffffff)), gn, tx, ty, gl.tileW, i == 0 ? DefaultFocus : 0)) openServer(3, gn.name, gn.name);
                } else {
                    const Subsonic::Artist& a = srvl_.artists[size_t(server)];
                    if (serverTile(uid("art.tile", int(uid(a.name) & 0x7fffffff)), a.cover, tx, ty, gl.tileW, true, a.name,
                                   std::to_string(a.albums) + (a.albums == 1 ? " album" : " albums"), i == 0 ? DefaultFocus : 0))
                        openServer(1, a.id, a.name);
                }
                serverBadge(tx, ty, gl.tileW);
                continue;
            }
            const Artist& ar = list[size_t(local)];
            const int cover = ar.albums.empty() ? ar.tracks[0] : lib.albums[ar.albums[0]].tracks[0];
            std::string tsub = std::to_string(ar.tracks.size()) + (ar.tracks.size() == 1 ? " track" : " tracks");
            if (!ar.albums.empty()) tsub = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums") + (genre ? " · " + tsub : "");
            // artists are round, as in Gatefold; a genre shows the covers of its first four albums, as Folders does
            if (tile(uid(genre ? "gen.tile" : "art.tile", int(uid(ar.name) & 0x7fffffff)), cover, tx, ty, gl.tileW, !genre, ar.name, tsub,
                     genre ? &ar.tracks : nullptr, i == 0 ? DefaultFocus : 0))
                genre ? openGenre(ar.name) : openArtist(ar.name);
        }
    g.unclip();
    ui.popArea();
}

/** Artists (or genres): yours, and while the server's music is In the library the server's among them by name; a name both have is
 *  one tile (yours, its page holding the server's albums too: artistTwin_). Made again only when either changed. */
const std::vector<std::pair<int, int>>& App::artistMix(bool genre) {
    const int k = genre ? 1 : 0;
    const std::vector<Artist>& mine = genre ? genres : artists;
    const bool on = store.serverPlace == 1 && subsonic.view().state == Subsonic::SignedIn;
    size_t n = 0;
    if (on) {
        if (genre) { if (const auto* l = serverGenreList()) n = l->size(); }
        else if (const auto* l = serverArtistList()) n = l->size();
    }
    const unsigned srvKey = n ? srvl_.gen : ~0u;
    if (artistMixIndex_[k] == indexGen && artistMixSrv_[k] == srvKey && artistTwin_[k].size() == mine.size()) return artistMix_[k];
    artistMixIndex_[k] = indexGen;
    artistMixSrv_[k] = srvKey;
    artistTwin_[k].assign(mine.size(), -1);
    auto low = [](std::string t) { for (char& ch : t) ch = char(tolower(uint8_t(ch))); return t; };
    std::vector<std::pair<std::string, std::pair<int, int>>> all;
    std::unordered_map<std::string, int> byName;
    for (int i = 0; i < int(mine.size()); i++) {
        std::string key = low(mine[size_t(i)].name);
        byName.emplace(key, i);
        all.push_back({ std::move(key), { i, -1 } });
    }
    for (int j = 0; j < int(n); j++) {
        std::string key = low(genre ? srvl_.genres[size_t(j)].name : srvl_.artists[size_t(j)].name);
        if (auto it = byName.find(key); it != byName.end()) { artistTwin_[k][size_t(it->second)] = j; continue; }
        all.push_back({ std::move(key), { -1, j } });
    }
    if (n) std::stable_sort(all.begin(), all.end(), [](const auto& p, const auto& q) { return p.first < q.first; });
    artistMix_[k].clear();
    for (auto& e : all) artistMix_[k].push_back(e.second);
    return artistMix_[k];
}

/** The artists with tracks in a genre, by name, with how many of those tracks are theirs. */
const std::vector<std::pair<int, int>>& App::genreArtists(int gi) {
    if (gi == genreArtistsFor_ && indexGen == genreArtistsGen_) return genreArtists_;
    genreArtistsFor_ = gi;
    genreArtistsGen_ = indexGen;
    genreArtists_.clear();
    std::unordered_map<int, int> count;   // index into artists → tracks in this genre
    for (int t : genres[gi].tracks) {
        std::string key = lib.tracks[t].artist.empty() ? "Unknown artist" : lib.tracks[t].artist;
        for (char& c : key) c = char(tolower((unsigned char)c));
        if (auto it = artistByKey.find(key); it != artistByKey.end()) count[it->second]++;
    }
    for (const auto& [a, n] : count) genreArtists_.push_back({ a, n });
    // artists is sorted by name already, so its index orders them
    std::sort(genreArtists_.begin(), genreArtists_.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    return genreArtists_;
}

void App::drawArtist(float x, float y, float w, float h, bool genre) {
    const int ai = genre ? routeGenre() : routeArtist();
    if (ai < 0) { viewHead(genre ? "Genre" : "Artist", route.query, "Not in the library", x, y, w); return; }
    const Artist& ar = genre ? genres[ai] : artists[ai];
    const int lead = ar.albums.empty() ? ar.tracks[0] : lib.albums[ar.albums[0]].tracks[0];

    const GridLayout gl = gridFor(x, w);
    // narrow (a phone upright): the picture on top, the name, its line and Play and Shuffle under it
    const bool narrow = w < kNarrowList;
    const float ncs = std::min(w - 60, 240.f);
    // (narrow: the name on up to two lines, as an album's title)
    const std::vector<std::string> nameLines = narrow ? g.wrap(ar.name, { css::Xxl, Weight::Bold }, int(w - 60), 2) : std::vector<std::string>{ ar.name };
    const float heroH = narrow ? 22 + ncs + 18 + 24 + nameLines.size() * 38 + 8 + 22 + 16 + 34 + 12 : 26 + 220 + 22;
    // One tab at a time under a switch, as in Library: a genre has Albums, Artists and Tracks, an artist Albums and
    // Tracks. Tabs are numbered alike (0 Albums, 1 Artists, 2 Tracks); with no albums, Albums is greyed and Tracks shows.
    int& chosen = genre ? genreTab : artistTab;
    int tab = std::clamp(chosen, 0, 2);
    if (!genre && tab == 1) tab = 0;
    if (ar.albums.empty() && tab == 0) tab = 2;
    const std::vector<std::pair<int, int>>* inGenre = tab == 1 ? &genreArtists(ai) : nullptr;
    const float switchH = 56 + (narrow && tab == 2 ? 46.f : 0.f);   // the switch's row (upright, on Tracks: the views under it)
    // the server's albums of the same name (its music In the library), after yours, not the ones you have
    std::vector<const Subsonic::Album*> theirs;
    if (tab == 0) {
        const std::vector<std::pair<int, int>>& mixed = artistMix(genre);
        (void)mixed;
        const int k = genre ? 1 : 0;
        const int twin = size_t(ai) < artistTwin_[k].size() ? artistTwin_[k][size_t(ai)] : -1;
        if (twin >= 0) {
            const SrvPage& pg = genre ? serverPage('G', srvl_.genres[size_t(twin)].name, 1) : serverPage('R', srvl_.artists[size_t(twin)].id, 1);
            auto low = [](std::string t) { for (char& ch : t) ch = char(tolower(uint8_t(ch))); return t; };
            for (const Subsonic::Album& sa2 : pg.albums) {
                bool have = false;
                for (int al : ar.albums)
                    if (low(lib.albums[size_t(al)].title) == low(sa2.name) && low(lib.albums[size_t(al)].artist) == low(sa2.artist)) { have = true; break; }
                if (!have) theirs.push_back(&sa2);
            }
        }
    }
    const int gridItems = tab == 1 ? int(inGenre->size()) : int(ar.albums.size() + theirs.size());
    const int rows = (gridItems + gl.cols - 1) / gl.cols;
    const float gridH = (tab == 2 || gridItems == 0) ? 0 : 10 + rows * gl.tileH;
    const float songsH = (tab == 0 || tab == 1) ? 0 : 46 + 30 + 8 + ar.tracks.size() * RowH;
    ScrollArea& sa = area(genre ? "genre" : "artist", int(uid(ar.name) & 0x7fffffff));
    sa.view = { x, y, w, h };
    sa.content = heroH + switchH + gridH + songsH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // Gatefold tints the hero with the cover's accent and haloes the cover; on the console the tint covered
    // only the content area and clashed with the sidebar and player bar, so the page keeps the app's background.
    const float cs = narrow ? ncs : 220.f;
    const float cx = narrow ? x + (w - cs) / 2 : x + 30, cyTop = oy + (narrow ? 22 : 26);
    if (genre) coverQuad(ar.tracks, cx, cyTop, cs, 16);
    else coverBox(lead, cx, cyTop, cs, cs / 2);

    const float tx = narrow ? x + 30 : cx + 220 + 28, tw = w - (tx - x) - 30;
    float ty = narrow ? cyTop + cs + 18 : cyTop + 220 - 34 - 16 - 22 - 8 - 48 - 6 - 18;
    kicker(genre ? "Genre" : "Artist", tx, ty + 8, pal.text3);
    ty += 24;
    if (narrow) for (const auto& l : nameLines) { g.text(l, { css::Xxl, Weight::Bold }, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw)); ty += 38; }
    else { g.text(ar.name, { 46, Weight::ExtraBold }, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw)); ty += 48; }
    ty += 8;
    std::string meta2 = fmtTotal(ar.tracks);
    if (!ar.albums.empty()) meta2 = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album · " : " albums · ") + meta2;
    g.text(meta2, { css::Base }, tx, ty, pal.text2, 1, Align::Left, int(tw));
    ty += 22 + 16;
    float bx = tx;
    bool clicked = false;
    bx += button(uid("ar.play"), ICON_PLAY, "Play", bx, ty, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked) playTracks(ar.tracks, 0, ar.name);
    bx += button(uid("ar.shuffle"), ICON_SHUFFLE, "Shuffle", bx, ty, false, &clicked) + 8;
    if (clicked) playTracks(ar.tracks, 0, ar.name, true);
    if (genre) genrePlaylistButton(ar.name, bx, ty);   // as a folder's Add as playlist

    g.clip(x, y, w, h);
    float gy = oy + heroH + 10;
    {
        // .seg as in Library (Albums | Tracks | Folders): what this page holds, one view at a time
        const float sy = oy + heroH + 22;
        std::string count;
        if (tab == 0) count = std::to_string(ar.albums.size() + theirs.size()) + (ar.albums.size() + theirs.size() == 1 ? " album" : " albums");
        else if (tab == 1) count = std::to_string(inGenre->size()) + (inGenre->size() == 1 ? " artist" : " artists");
        else count = std::to_string(ar.tracks.size()) + (ar.tracks.size() == 1 ? " track" : " tracks");
        g.textC(count, { css::Lg, Weight::Bold }, x + 30, sy, pal.text);
        // On Tracks the four list views (as in Library → Tracks) take the right edge and the switch sits left of them
        float rx = x + w - 30;
        if (tab == 2) {
            int mode = store.viewGenre;
            if (!narrow) rx -= viewSwitch(genre ? "gen.view" : "ar.view", rx, sy, mode) + 10;
            else viewSwitch(genre ? "gen.view" : "ar.view", rx, sy + 46, mode);
            store.viewGenre = mode;
        }
        int picked = -1;
        const std::vector<std::string> names = genre ? std::vector<std::string>{ "Albums", "Artists", "Tracks" } : std::vector<std::string>{ "Albums", "Tracks" };
        const int shown = genre ? tab : (tab == 2 ? 1 : 0);
        segmented(uid(genre ? "gen.tab" : "ar.tab"), rx, sy, names, shown, &picked, ar.albums.empty() ? 1u : 0u);
        if (picked >= 0 && picked != shown) chosen = genre ? picked : (picked == 1 ? 2 : 0);
        gy = oy + heroH + switchH;
    }
    if (tab == 1) {
        // Artists: round, as in the Artists tab; the count is their tracks in this genre
        for (int i = 0; i < int(inGenre->size()); i++) {
            const Artist& p = artists[(*inGenre)[i].first];
            const int n = (*inGenre)[i].second;
            const float ty2 = gy + (i / gl.cols) * gl.tileH;
            if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
            const int cover = p.albums.empty() ? p.tracks[0] : lib.albums[p.albums[0]].tracks[0];
            if (tile(uid("gen.artist", int(uid(p.name) & 0x7fffffff)), cover, gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, true, p.name,
                     std::to_string(n) + (n == 1 ? " track here" : " tracks here")))
                openArtist(p.name);
        }
    } else if (tab != 2) {
        for (int i = 0; i < int(ar.albums.size()); i++) {
            const Album& al = lib.albums[ar.albums[i]];
            const std::string sub = al.year.empty() ? std::to_string(al.tracks.size()) + " tracks" : al.year;
            const float ty2 = gy + (i / gl.cols) * gl.tileH;
            if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
            if (tile(uid("ar.tile", int(uid(al.key) & 0x7fffffff)), al.tracks[0], gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, false, al.title, sub))
                openAlbum(ar.albums[i]);
        }
        for (int j = 0; j < int(theirs.size()); j++) {
            const int i = int(ar.albums.size()) + j;
            const Subsonic::Album& al = *theirs[size_t(j)];
            const float tx2 = gl.x0 + (i % gl.cols) * gl.tileW, ty2 = gy + (i / gl.cols) * gl.tileH;
            if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
            const std::string sub = genre ? al.artist : al.year > 0 ? std::to_string(al.year) : std::to_string(al.songs) + (al.songs == 1 ? " song" : " songs");
            if (serverTile(uid("ar.stile", int(uid(al.id) & 0x7fffffff)), al.cover, tx2, ty2, gl.tileW, false, al.name, sub)) openServer(2, al.id, al.name);
            serverBadge(tx2, ty2, gl.tileW, subsonic.albumDownload(al.id).state == 2);
        }
    }

    // The Tracks tab: the same list as Library → Tracks, in the view chosen beside the switch
    if (tab == 2) {
        const float top = oy + heroH + switchH;
        // false: a playlist's columns (no Genre column on a genre's own page)
        const float bodyH = drawTrackBody(genre ? "gen" : "ar", ar.tracks, x, top, w, y, h, ListMode(store.viewGenre), false,
                                          [&](int i) { playTracks(ar.tracks, i, ar.name); });
        sa.content = heroH + switchH + bodyH + 24;
    }
    g.unclip();
    ui.popArea();
}
