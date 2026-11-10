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

int App::looseTracks() {
    static unsigned forIndex = ~0u;
    static int kept = 0;
    if (forIndex != indexGen) {
        forIndex = indexGen;
        kept = int(std::count_if(lib.tracks.begin(), lib.tracks.end(), [](const Track& t) { return t.albumIdx < 0; }));
    }
    return kept;
}

/** The album grid below a header ending at headBottom: the sidebar's Albums and Library → Albums. */
void App::drawAlbumGrid(float x, float y, float w, float h, float headBottom, bool toolsRow) {
    // .lib-tools: the sort select, as a button that cycles
    static const char* sorts[] = { "By artist", "By title", "By year", "By folder" };
    const Font sf{ css::Sm };
    // the three views at the right (tiles, rows with covers, the plain list: the user, 2026-11-09), the sort select left of them
    int amode = store.viewAlbums;
    // (on Library's row their centre is the 34 px buttons' of its other tabs, 17 under the row's top: at 16 the switch sat a pixel
    // above Songs', Genres' and Folders'; the user, 2026-11-10)
    const float tcy = toolsRow ? headBottom - 46 + 17 : headBottom - 30;
    const float vsw = lib.albums.empty() ? 0.f : viewSwitch("alb.view", x + w - 28, tcy, amode, 1u) + 8;
    if (amode != store.viewAlbums) { store.viewAlbums = amode; persistSoon(0.3f); }
    const float sw = g.measure(sorts[librarySort], sf) + 20 + 26, sx = x + w - 28 - vsw - sw, sy = tcy - 16;
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

    if (lib.albums.empty()) {
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

    const std::vector<std::pair<int, int>>& order = albumMix();
    // the #-to-Z rail (Settings › Appearance) when the sort has letters: by artist, by title
    const bool rail = railOn(1u) && (librarySort == 0 || librarySort == 1) && !order.empty();
    static std::vector<std::pair<char, int>> railLetters;
    static unsigned railIndex = ~0u, railSrv = ~0u;
    static int railSort = -1;
    static size_t railN = 0;
    if (rail && (railIndex != indexGen || railSrv != srvl_.gen || railSort != librarySort || railN != order.size())) {
        railIndex = indexGen; railSrv = srvl_.gen; railSort = librarySort; railN = order.size();
        std::vector<std::string> names;
        names.reserve(order.size());
        for (const auto& e : order) names.push_back(librarySort == 1 ? lib.albums[size_t(e.first)].title : lib.albums[size_t(e.first)].artist);
        railLetters = lettersOf(names);
    }
    const bool tiles = amode == int(ListMode::Grid) || amode == int(ListMode::Albums);
    const float rowH = amode == int(ListMode::List) ? 48.f : 60.f, lw = rail ? w - kRailW : w;
    const GridLayout gl = gridFor(x, lw);
    ScrollArea& sa = area("library");
    sa.view = { x, headBottom, w, y + h - headBottom };
    const int rows = tiles ? (int(order.size()) + gl.cols - 1) / gl.cols : int(order.size());
    sa.content = (tiles ? rows * gl.tileH : rows * rowH) + 24;
    ui.pushArea(&sa);
    g.clip(x, headBottom, w, y + h - headBottom);
    if (!tiles) {   // rows, as the Artists page's
        for (int i = 0; i < int(order.size()); i++) {
            const float ry = headBottom + i * rowH - sa.y;
            if (ry + rowH < headBottom - rowH || ry > y + h + rowH) continue;
            const int local = order[size_t(i)].first;
            const Id tid = uid("lib.tile", int(uid(lib.albums[size_t(local)].key) & 0x7fffffff));
            if (i == 0) ui.link(tid, 0, -1, sid);
            if (albumRow(tid, local, x + 14, ry, lw - 28, rowH, amode == int(ListMode::List), i == 0 ? DefaultFocus : 0)) openAlbum(local);
        }
    }
    const int firstRow = std::max(0, int(sa.y / gl.tileH) - 1);
    const int lastRow = std::min(rows, int((sa.y + sa.view.h) / gl.tileH) + 2);
    for (int r = firstRow; tiles && r < lastRow; r++)
        for (int c = 0; c < gl.cols; c++) {
            const int i = r * gl.cols + c;
            if (i >= int(order.size())) break;
            const float tx = gl.x0 + c * gl.tileW, ty = headBottom + r * gl.tileH - sa.y;
            const int local = order[size_t(i)].first;
            const Album& al = lib.albums[size_t(local)];
            const Id tid = uid("lib.tile", int(uid(al.key) & 0x7fffffff));
            // The first album is where the page opens (not the sort button above it); Up from the top row goes to
            // the sort button, which the layout put out of reach from the left (the search field was nearer)
            if (r == 0) ui.link(tid, 0, -1, sid);
            if (tile(tid, al.tracks[0], tx, ty, gl.tileW, false, al.title, al.artist, nullptr, i == 0 ? DefaultFocus : 0)) openAlbum(local);
        }
    g.unclip();
    ui.popArea();
    if (rail) letterRail("lib", x + w - 24, headBottom, y + h - headBottom, railLetters, sa, [&](int row) { return tiles ? float(row / gl.cols) * gl.tileH : float(row) * rowH; });
}

bool App::albumRow(Id id, int album, float x, float y, float w, float rowH, bool list, uint8_t flags) {
    const Album& al = lib.albums[size_t(album)];
    const float rr = rowH - 4, cov = rr - 16;
    const bool hit = ui.item(id, { x, y, w, rr }, 10, flags);
    if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(x, y, w, rr, 10, pal.text, 0.065f);
    const float mid = y + rr / 2;
    const int cover = al.tracks.empty() ? -1 : al.tracks[0];
    if (cover >= 0 && meta.art(cover).state == AlbumArt::Ready) coverBox(cover, x + 16, mid - cov / 2, cov, 6);
    else {
        if (cover >= 0) meta.wantTags(cover);
        g.roundRect(x + 16, mid - cov / 2, cov, cov, 6, pal.surface2);
        g.icon(ICON_ALBUMS, x + 16 + cov / 2, mid, cov * 0.5f, pal.text2);
    }
    std::string info = al.artist;
    if (!al.year.empty()) info += " · " + al.year;
    info += " · " + std::to_string(al.tracks.size()) + (al.tracks.size() == 1 ? " track" : " tracks");
    const float tx = x + 16 + cov + 14;
    if (list) {
        g.textC(al.title, { css::Base, Weight::SemiBold }, tx, mid, pal.text, 1, Align::Left, int(w * 0.5f));
        g.textC(info, { css::Sm }, x + w - 48, mid, pal.text3, 1, Align::Right, int(w * 0.35f));
    } else {
        g.text(al.title, { css::Base, Weight::SemiBold }, tx, mid - 19, pal.text, 1, Align::Left, int(w - 140));
        g.text(info, { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(w - 140));
    }
    g.icon(ICON_CHEV, x + w - 24, mid, 18, pal.text3);
    return hit;
}

/** The Albums grid's order: yours (albumOrder), as pairs the grid reads. Made again only when the index or the sort changed. */
const std::vector<std::pair<int, int>>& App::albumMix() {
    if (albumMixIndex_ == indexGen && albumMixSort_ == librarySort && albumMixLocal_ == albumOrder.size()) return albumMix_;
    albumMixIndex_ = indexGen;
    albumMixSort_ = librarySort;
    albumMixLocal_ = albumOrder.size();
    albumMix_.clear();
    for (int i : albumOrder) albumMix_.push_back({ i, -1 });
    return albumMix_;
}

void App::drawArtists(float x, float y, float w, float h, bool genre, bool inLibrary) {
    const std::vector<Artist>& list = genre ? genres : artists;
    const std::vector<std::pair<int, int>>& mix = artistMix(genre);
    std::string sub = genre && list.empty() ? std::string("No genre tags yet") : std::to_string(list.size()) + (genre ? (list.size() == 1 ? " genre" : " genres") : (list.size() == 1 ? " artist" : " artists"));
    float headBottom = viewHead("Library", genre ? "Genres" : "Artists", sub, x, y, w);
    const float tabsW = inLibrary ? libraryTabsRow(x, w, y) : 0;
    const bool narrow = w < kNarrowList;
    int gmode = genre ? store.viewGenres : store.viewArtists;   // the page's view (as Folders'): 2 tiles, 1 rows with covers, 3 the plain list
    const bool tools = genre && !genres.empty();
    if (!genre && !mix.empty()) {   // Artists: the views in the head's line, where Albums has its sort (the user, 2026-11-08)
        // in Library, under its tabs: a row of their own, as Library › Albums' sort and views (the user, 2026-11-10: "Row under the
        // tabs"; in the head's line they sat over the tabs' lower edge)
        if (tabsW > 0) { viewSwitch("art.view", x + w - 28, headBottom + 17, gmode, 1u); headBottom += 46; }
        else viewSwitch("art.view", x + w - 28, headBottom - 30, gmode, 1u);
        if (gmode != store.viewArtists) { store.viewArtists = gmode; persistSoon(0.3f); }
    }
    if (tools) {   // as Folders: Play and Shuffle everything, Add all as playlists, the views (the user, 2026-11-08)
        auto everything = [&] {   // (made at the press: every genre's songs, every frame, was the library copied sixty times a second)
            std::vector<int> all;
            for (const Artist& gn : genres) all.insert(all.end(), gn.tracks.begin(), gn.tracks.end());
            return all;
        };
        float bx = x + 28;
        bool c = false;
        bx += button(uid("gen.play"), ICON_PLAY, "Play", bx, headBottom, true, &c, false, false, DefaultFocus) + 8;
        if (c) if (const std::vector<int> all = everything(); !all.empty()) playTracks(all, 0, "Genres");
        c = false;
        bx += button(uid("gen.shuffle"), ICON_SHUFFLE, "Shuffle", bx, headBottom, false, &c) + 8;
        if (c) if (const std::vector<int> all = everything(); !all.empty()) playTracks(all, 0, "Genres", true);
        c = false;
        bx += button(uid("gen.addall"), ICON_PLUS, narrow ? "Add all" : "Add all as playlists", bx, headBottom, false, &c) + 8;
        if (c) addGenresAsPlaylists();
        viewSwitch("gen.view", x + w - 28, headBottom + 17, gmode, 1u);   // (By album would be the tiles again: three views, as Folders')
        if (gmode != store.viewGenres) { store.viewGenres = gmode; persistSoon(0.3f); }
        headBottom += 34 + 16;
    }
    const bool tiles = gmode == int(ListMode::Grid) || gmode == int(ListMode::Albums);
    // the #-to-Z rail (Settings › Appearance): the names in the grid's order, once per change
    const bool rail = railOn(genre ? 4u : 2u) && !mix.empty();
    static std::vector<std::pair<char, int>> railLetters[2];
    static unsigned railIndex[2] = { ~0u, ~0u };
    static size_t railN[2] = { 0, 0 };
    const int k = genre ? 1 : 0;
    if (rail && (railIndex[k] != indexGen || railN[k] != mix.size())) {
        railIndex[k] = indexGen; railN[k] = mix.size();
        std::vector<std::string> names;
        names.reserve(mix.size());
        for (const auto& e : mix) names.push_back(list[size_t(e.first)].name);
        railLetters[k] = lettersOf(names);
    }
    const float lw = rail ? w - kRailW : w;
    const GridLayout gl = gridFor(x, lw);
    const float rowH = gmode == int(ListMode::List) ? 48 : 60;
    ScrollArea& sa = area(genre ? "genres" : "artists");
    sa.view = { x, headBottom, w, y + h - headBottom };
    const int rows = tiles ? (int(mix.size()) + gl.cols - 1) / gl.cols : int(mix.size());
    sa.content = tiles ? rows * gl.tileH + 24 : rows * rowH + 24;
    ui.pushArea(&sa);
    g.clip(x, headBottom, w, y + h - headBottom);
    if (!tiles) {   // rows, as Folders' sub-folder rows: a cover (round for an artist, or the mark), the name, what is inside, a chevron
        const float cov = rowH - 4 - 16, crad = genre ? 6.f : cov / 2;
        for (int i = 0; i < int(mix.size()); i++) {
            const float ry = headBottom + i * rowH - sa.y, rx = x + 14, rw = lw - 28;
            if (ry + rowH < headBottom - rowH || ry > y + h + rowH) continue;
            const Artist& ar = list[size_t(mix[size_t(i)].first)];
            const std::string& name = ar.name;
            const Id id = uid(genre ? "gen.row" : "art.row", int(uid(name) & 0x7fffffff));
            if (ui.item(id, { rx, ry, rw, rowH - 4 }, 10, i == 0 && !tools ? DefaultFocus : 0)) { if (genre) openGenre(name); else openArtist(name); }
            if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
            const float mid = ry + (rowH - 4) / 2;
            const int cover = ar.albums.empty() ? ar.tracks[0] : lib.albums[ar.albums[0]].tracks[0];
            if (meta.art(cover).state == AlbumArt::Ready) coverBox(cover, rx + 16, mid - cov / 2, cov, crad);
            else {
                meta.wantTags(cover);
                g.roundRect(rx + 16, mid - cov / 2, cov, cov, crad, pal.surface2);
                g.icon(genre ? ICON_TAG : ICON_MIC, rx + 16 + cov / 2, mid, cov * 0.5f, pal.text2);
            }
            std::string info = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums");
            if (genre || ar.albums.empty()) info = (genre ? info + " · " : std::string()) + std::to_string(ar.tracks.size()) + (ar.tracks.size() == 1 ? " track" : " tracks");
            const float tx = rx + 16 + cov + 14;
            if (gmode == int(ListMode::List)) {
                g.textC(name, { css::Base, Weight::SemiBold }, tx, mid, pal.text, 1, Align::Left, int(rw * 0.5f));
                g.textC(info, { css::Sm }, rx + rw - 48, mid, pal.text3, 1, Align::Right, int(rw * 0.35f));
            } else {
                g.text(name, { css::Base, Weight::SemiBold }, tx, mid - 19, pal.text, 1, Align::Left, int(rw - 140));
                g.text(info, { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(rw - 140));
            }
            g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
        }
    }
    const int firstRow = std::max(0, int(sa.y / gl.tileH) - 1);
    const int lastRow = std::min(rows, int((sa.y + sa.view.h) / gl.tileH) + 2);
    for (int r = firstRow; tiles && r < lastRow; r++)
        for (int c = 0; c < gl.cols; c++) {
            const int i = r * gl.cols + c;
            if (i >= int(mix.size())) break;
            const float tx = gl.x0 + c * gl.tileW, ty = headBottom + r * gl.tileH - sa.y;
            const Artist& ar = list[size_t(mix[size_t(i)].first)];
            const int cover = ar.albums.empty() ? ar.tracks[0] : lib.albums[ar.albums[0]].tracks[0];
            std::string tsub = std::to_string(ar.tracks.size()) + (ar.tracks.size() == 1 ? " track" : " tracks");
            if (!ar.albums.empty()) tsub = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums") + (genre ? " · " + tsub : "");
            // artists are round, as in Gatefold; a genre shows the covers of its first four albums, as Folders does
            if (tile(uid(genre ? "gen.tile" : "art.tile", int(uid(ar.name) & 0x7fffffff)), cover, tx, ty, gl.tileW, !genre, ar.name, tsub,
                     genre ? &ar.tracks : nullptr, i == 0 && !tools ? DefaultFocus : 0))
                genre ? openGenre(ar.name) : openArtist(ar.name);
        }
    g.unclip();
    ui.popArea();
    if (rail) letterRail(genre ? "gen" : "art", x + w - 24, headBottom, y + h - headBottom, railLetters[k], sa,
                         [&](int row) { return tiles ? float(row / gl.cols) * gl.tileH : float(row) * rowH; });
}

/** Artists (or genres): yours, in the index's order, as pairs the grid reads. Made again only when the index changed. */
const std::vector<std::pair<int, int>>& App::artistMix(bool genre) {
    const int k = genre ? 1 : 0;
    const std::vector<Artist>& mine = genre ? genres : artists;
    if (artistMixIndex_[k] == indexGen && artistMix_[k].size() == mine.size()) return artistMix_[k];
    artistMixIndex_[k] = indexGen;
    artistMix_[k].clear();
    for (int i = 0; i < int(mine.size()); i++) artistMix_[k].push_back({ i, -1 });
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

    // narrow (a phone upright): the picture on top, the name, its line and Play and Shuffle under it
    const bool narrow = w < kNarrowList;
    const float ncs = std::min(w - 60, 240.f);
    // (narrow: the name on up to two lines, as an album's title)
    const std::vector<std::string> nameLines = narrow ? g.wrap(ar.name, { css::Xxl, Weight::Bold }, int(w - 60), 2) : std::vector<std::string>{ ar.name };
    // wide: a playlist page's head (the user, 2026-11-09: "the genres header could be smaller like how playlists does it"):
    // the 120 cover, the kicker, name and line beside it, Play and Shuffle on the cover's last line
    const float heroH = narrow ? 22 + ncs + 18 + 24 + nameLines.size() * 38 + 8 + 22 + 16 + 34 + 12 : 18 + 120 + 14;
    // One tab at a time under a switch, as in Library: a genre has Albums, Artists and Tracks, an artist Albums and
    // Tracks. Tabs are numbered alike (0 Albums, 1 Artists, 2 Tracks); with no albums, Albums is greyed and Tracks shows.
    int& chosen = genre ? genreTab : artistTab;
    int tab = std::clamp(chosen, 0, 2);
    if (!genre && tab == 1) tab = 0;
    if (ar.albums.empty() && tab == 0) tab = 2;
    const std::vector<std::pair<int, int>>* inGenre = tab == 1 ? &genreArtists(ai) : nullptr;
    // a genre's Albums tab sorts as the Albums page does (by artist, by title, by year) and has its three views (the user, 2026-11-09)
    static const char* const kAlbumSorts[] = { "By artist", "By title", "By year" };
    const bool genreAlbums = genre && tab == 0;
    std::vector<int> albs = ar.albums;
    if (genreAlbums) {   // (sorted when the genre, its sort or the index changes, not every frame)
        static std::vector<int> sorted;
        static int forGenre = -1, forSort = -1;
        static unsigned forIndex = ~0u;
        if (forGenre != ai || forSort != genreSort || forIndex != indexGen || sorted.size() != albs.size()) {
            forGenre = ai; forSort = genreSort; forIndex = indexGen;
            auto low = [](std::string v) { for (char& c : v) c = char(tolower(uint8_t(c))); return v; };
            sorted = albs;
            std::stable_sort(sorted.begin(), sorted.end(), [&](int p, int q) {
                const Album &A = lib.albums[size_t(p)], &B = lib.albums[size_t(q)];
                if (genreSort == 2 && A.year != B.year) return !A.year.empty() && (B.year.empty() || A.year < B.year);
                if (genreSort == 0 && low(A.artist) != low(B.artist)) return low(A.artist) < low(B.artist);
                return low(A.title) < low(B.title);
            });
        }
        albs = sorted;
    }
    const int amode = genreAlbums ? store.viewAlbums : int(ListMode::Grid);
    const bool albumTiles = amode == int(ListMode::Grid) || amode == int(ListMode::Albums);
    const float albumRowH = amode == int(ListMode::List) ? 48.f : 60.f;
    // the #-to-Z rail (Settings › Appearance › Letter rail: Genres for a genre's page, Artists for an artist's) down the tab's list:
    // the albums when their sort has letters, the artists, the songs in their order (not on tiles)
    const bool rail = railOn(genre ? 4u : 2u) && (tab == 0 ? (genreAlbums && !albs.empty() && genreSort <= 1)
                                                  : tab == 1 ? !inGenre->empty()
                                                             : (!ar.tracks.empty() && store.viewGenre != int(ListMode::Grid)));
    const float lw = rail ? w - kRailW : w;
    const GridLayout gl = gridFor(x, lw);
    const float switchH = 56 + (narrow && (tab == 2 || genreAlbums) ? 46.f : 0.f);   // the switch's row (upright, on Songs and a genre's Albums: the views under it)
    const int gridItems = tab == 1 ? int(inGenre->size()) : int(albs.size());
    const int rows = (gridItems + gl.cols - 1) / gl.cols;
    const float gridH = (tab == 2 || gridItems == 0) ? 0 : 10 + (tab == 0 && !albumTiles ? gridItems * albumRowH : rows * gl.tileH);
    const float songsH = (tab == 0 || tab == 1) ? 0 : 46 + 30 + 8 + ar.tracks.size() * RowH;
    ScrollArea& sa = area(genre ? "genre" : "artist", int(uid(ar.name) & 0x7fffffff));
    sa.view = { x, y, w, h };
    sa.content = heroH + switchH + gridH + songsH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // Gatefold tints the hero with the cover's accent and haloes the cover; on the console the tint covered
    // only the content area and clashed with the sidebar and player bar, so the page keeps the app's background.
    const float cs = narrow ? ncs : 120.f;
    const float cx = narrow ? x + (w - cs) / 2 : x + 28, cyTop = oy + (narrow ? 22 : 18);
    if (genre) coverQuad(ar.tracks, cx, cyTop, cs, narrow ? 16 : 12);
    else coverBox(lead, cx, cyTop, cs, cs / 2);

    std::string meta2 = fmtTotal(ar.tracks);
    if (!ar.albums.empty()) meta2 = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album · " : " albums · ") + meta2;
    const float tx = narrow ? x + 30 : cx + cs + 22, tw = w - (tx - x) - (narrow ? 30 : 28);
    float ty = narrow ? cyTop + cs + 18 : cyTop + cs - 34;   // (wide: the actions' line, the cover's last)
    if (narrow) {
        kicker(genre ? "Genre" : "Artist", tx, ty + 8, pal.text3);
        ty += 24;
        for (const auto& l : nameLines) { g.text(l, { css::Xxl, Weight::Bold }, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw)); ty += 38; }
        ty += 8;
        g.text(meta2, { css::Base }, tx, ty, pal.text2, 1, Align::Left, int(tw));
        ty += 22 + 16;
    } else {   // .pl-head: the kicker, name and line beside the cover, as a playlist's
        kicker(genre ? "Genre" : "Artist", tx, cyTop + 8, pal.text3);
        g.text(ar.name, { css::Xxl, Weight::Bold }, tx, cyTop + 16, pal.text, 1, Align::Left, int(tw));
        g.text(meta2, { css::Sm }, tx, cyTop + 58, pal.text3, 1, Align::Left, int(tw));
    }
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
        if (tab == 0) count = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums");
        else if (tab == 1) count = std::to_string(inGenre->size()) + (inGenre->size() == 1 ? " artist" : " artists");
        else count = std::to_string(ar.tracks.size()) + (ar.tracks.size() == 1 ? " track" : " tracks");
        g.textC(count, { css::Lg, Weight::Bold }, x + 30, sy, pal.text);
        // On Songs the four list views (as in Library → Songs) take the right edge and the switch sits left of them
        float rx = x + w - 30;
        if (tab == 2) {
            int mode = store.viewGenre;
            if (!narrow) rx -= viewSwitch(genre ? "gen.view" : "ar.view", rx, sy, mode) + 10;
            else viewSwitch(genre ? "gen.view" : "ar.view", rx, sy + 46, mode);
            store.viewGenre = mode;
        }
        if (genreAlbums && !albs.empty()) {   // a genre's Albums: the three views and the sort, as the Albums page's head (upright: under the switch)
            int m = amode;
            const float vy = narrow ? sy + 46 : sy;
            float vx = narrow ? x + w - 30 : rx;
            vx -= viewSwitch("gen.aview", vx, vy, m, 1u) + 10;
            if (m != amode) { store.viewAlbums = m; persistSoon(0.3f); }
            bool c = false;
            vx -= sortButton("gen.sort", kAlbumSorts[genreSort], vx, vy, &c) + 10;
            if (c) genreSort = (genreSort + 1) % 3;
            if (!narrow) rx = vx;
        }
        int picked = -1;
        const std::vector<std::string> names = genre ? std::vector<std::string>{ "Albums", "Artists", "Songs" } : std::vector<std::string>{ "Albums", "Songs" };
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
        for (int i = 0; i < int(albs.size()); i++) {
            const Album& al = lib.albums[size_t(albs[size_t(i)])];
            if (!albumTiles) {   // rows, as the Albums page's
                const float ry = gy + i * albumRowH;
                if (ry + albumRowH < y - albumRowH || ry > y + h + albumRowH) continue;
                if (albumRow(uid("ar.row", int(uid(al.key) & 0x7fffffff)), albs[size_t(i)], x + 14, ry, lw - 28, albumRowH, amode == int(ListMode::List))) openAlbum(albs[size_t(i)]);
                continue;
            }
            const std::string sub = al.year.empty() ? std::to_string(al.tracks.size()) + " tracks" : al.year;
            const float ty2 = gy + (i / gl.cols) * gl.tileH;
            if (ty2 + gl.tileH < y - gl.tileH || ty2 > y + h + gl.tileH) continue;
            if (tile(uid("ar.tile", int(uid(al.key) & 0x7fffffff)), al.tracks[0], gl.x0 + (i % gl.cols) * gl.tileW, ty2, gl.tileW, false, al.title, sub))
                openAlbum(albs[size_t(i)]);
        }
    }

    // The Songs tab: the same list as Library → Songs, in the view chosen beside the switch
    if (tab == 2) {
        const float top = oy + heroH + switchH;
        // false: a playlist's columns (no Genre column on a genre's own page)
        const float bodyH = drawTrackBody(genre ? "gen" : "ar", ar.tracks, x, top, lw, y, h, ListMode(store.viewGenre), false,
                                          [&](int i) { playTracks(ar.tracks, i, ar.name); });
        sa.content = heroH + switchH + bodyH + 24;
    }
    g.unclip();
    ui.popArea();
    if (rail) {   // the tab's names in its order; the rail keeps the list's unscrolled place, as a playlist page's
        const std::string stamp = std::to_string(indexGen) + ":" + std::to_string(ai) + ":" + std::to_string(tab) + ":" + std::to_string(genreSort);
        const auto& letters = railLettersFor(genre ? "gen.pg" : "ar.pg", stamp, [&] {
            std::vector<std::string> names;
            if (tab == 0) for (int a : albs) names.push_back(genreSort == 1 ? lib.albums[size_t(a)].title : lib.albums[size_t(a)].artist);
            else if (tab == 1) for (const auto& e : *inGenre) names.push_back(artists[size_t(e.first)].name);
            else for (int t : ar.tracks) names.push_back(lib.tracks[size_t(t)].title);
            return names;
        });
        const float listTop = heroH + switchH;
        const std::vector<ListEntry> layout = tab == 2 ? listLayout(ar.tracks, lw, ListMode(store.viewGenre)) : std::vector<ListEntry>{};
        const float railTop = y + listTop;
        letterRail(genre ? "gen.pg" : "ar.pg", x + w - 24, railTop, y + h - railTop, letters, sa, [&](int row) {
            return listTop + (tab == 2 ? listRowY(layout, row) : tab == 0 && !albumTiles ? float(row) * albumRowH : float(row / gl.cols) * gl.tileH);
        });
    }
}

float App::sortButton(const char* key, const std::string& label, float rx, float cy, bool* clicked) {
    const Font sf{ css::Sm };
    const float sw = g.measure(label, sf) + 20 + 26, sx = rx - sw, sy = cy - 16;
    const Id sid = uid(key);
    if (ui.item(sid, { sx, sy, sw, 32 }, 8)) *clicked = true;
    const bool hot = ui.focused(sid) && ui.showFocusRing();
    g.roundRect(sx, sy, sw, 32, 8, pal.text, hot ? 0.11f : 0.065f);
    g.roundStroke(sx, sy, sw, 32, 8, 1, pal.text, 0.15f);
    g.textC(label, sf, sx + 10, cy, pal.text);
    g.icon(ICON_CHEV, sx + sw - 16, cy, 14, pal.text3);
    return sw;
}


/** Search's tabs but Songs (view_lists.cpp drawTrackList): what matched among your albums, artists, genres and playlists, each as its
 *  own page shows it (the tiles, or rows in the page's view), and the lyrics' hits. Drawn in the page's scroll area under `top`; its height. */
float App::drawSearchTab(float x, float top, float w, float clipY, float clipH) {
    const int tab = searchTab_;
    if (tab == 5) return drawLyricHits(x, top - 10, w, clipY, clipH);   // In the lyrics: its kicker, how far the reading is, the hits
    const std::vector<int>& found = tab == 1 ? searchAlbums_ : tab == 2 ? searchArtists_ : tab == 3 ? searchGenres_ : searchPlaylists_;
    if (found.empty()) {
        const char* msg = tab == 1 ? "No albums match" : tab == 2 ? "No artists match" : tab == 3 ? "No genres match" : "No playlists match";
        g.textC(msg, { css::Lg, Weight::SemiBold }, x + w / 2, top + 90, pal.text2, 1, Align::Center);
        return 160;
    }
    const int mode = tab == 1 ? store.viewAlbums : tab == 2 ? store.viewArtists : tab == 3 ? store.viewGenres : (store.viewPlaylists == 0 ? 2 : store.viewPlaylists);
    const bool tiles = mode == int(ListMode::Grid) || mode == int(ListMode::Albums);
    const GridLayout gl = gridFor(x, w);
    const float rowH = tab == 4 ? (mode == int(ListMode::List) ? 52.f : 64.f) : (mode == int(ListMode::List) ? 48.f : 60.f);
    const int n = int(found.size());
    auto seen = [&](float ty, float th) { return ty + th >= clipY - th && ty <= clipY + clipH + th; };
    if (tab == 1) {   // albums: the Albums page's tiles, or its rows
        for (int i = 0; i < n; i++) {
            const int ai = found[size_t(i)];
            const Album& al = lib.albums[size_t(ai)];
            if (tiles) {
                const float tx = gl.x0 + (i % gl.cols) * gl.tileW, ty = top + (i / gl.cols) * gl.tileH;
                if (!seen(ty, gl.tileH)) continue;
                if (tile(uid("srch.al", ai), al.tracks[0], tx, ty, gl.tileW, false, al.title, al.artist, nullptr, i == 0 ? DefaultFocus : 0)) openAlbum(ai);
                continue;
            }
            const float ry = top + i * rowH;
            if (!seen(ry, rowH)) continue;
            if (albumRow(uid("srch.al", ai), ai, x + 14, ry, w - 28, rowH, mode == int(ListMode::List), i == 0 ? DefaultFocus : 0)) openAlbum(ai);
        }
        return (tiles ? float((n + gl.cols - 1) / gl.cols) * gl.tileH : n * rowH) + 24;
    }
    if (tab == 2 || tab == 3) {   // artists (round) and genres (four covers): the Artists / Genres page's tiles or rows
        const bool genre = tab == 3;
        const std::vector<Artist>& list = genre ? genres : artists;
        const float cov = rowH - 4 - 16, crad = genre ? 6.f : cov / 2;
        for (int i = 0; i < n; i++) {
            const Artist& ar = list[size_t(found[size_t(i)])];
            const int cover = ar.albums.empty() ? ar.tracks[0] : lib.albums[size_t(ar.albums[0])].tracks[0];
            std::string info = std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums");
            if (genre || ar.albums.empty()) info = (genre ? info + " · " : std::string()) + std::to_string(ar.tracks.size()) + (ar.tracks.size() == 1 ? " track" : " tracks");
            const Id id = uid(genre ? "srch.gn" : "srch.ar", found[size_t(i)]);
            if (tiles) {
                const float tx = gl.x0 + (i % gl.cols) * gl.tileW, ty = top + (i / gl.cols) * gl.tileH;
                if (!seen(ty, gl.tileH)) continue;
                if (tile(id, cover, tx, ty, gl.tileW, !genre, ar.name, info, genre ? &ar.tracks : nullptr, i == 0 ? DefaultFocus : 0)) {
                    if (genre) openGenre(ar.name); else openArtist(ar.name);
                }
                continue;
            }
            const float ry = top + i * rowH, rx = x + 14, rw = w - 28;
            if (!seen(ry, rowH)) continue;
            if (ui.item(id, { rx, ry, rw, rowH - 4 }, 10, i == 0 ? DefaultFocus : 0)) { if (genre) openGenre(ar.name); else openArtist(ar.name); }
            if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
            const float mid = ry + (rowH - 4) / 2;
            if (meta.art(cover).state == AlbumArt::Ready) coverBox(cover, rx + 16, mid - cov / 2, cov, crad);
            else {
                meta.wantTags(cover);
                g.roundRect(rx + 16, mid - cov / 2, cov, cov, crad, pal.surface2);
                g.icon(genre ? ICON_TAG : ICON_MIC, rx + 16 + cov / 2, mid, cov * 0.5f, pal.text2);
            }
            const float tx = rx + 16 + cov + 14;
            if (mode == int(ListMode::List)) {
                g.textC(ar.name, { css::Base, Weight::SemiBold }, tx, mid, pal.text, 1, Align::Left, int(rw * 0.5f));
                g.textC(info, { css::Sm }, rx + rw - 48, mid, pal.text3, 1, Align::Right, int(rw * 0.35f));
            } else {
                g.text(ar.name, { css::Base, Weight::SemiBold }, tx, mid - 19, pal.text, 1, Align::Left, int(rw - 140));
                g.text(info, { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(rw - 140));
            }
            g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
        }
        return (tiles ? float((n + gl.cols - 1) / gl.cols) * gl.tileH : n * rowH) + 24;
    }
    // playlists (-1 Liked): the Playlists page's tiles (four covers, the sidebar icon in the corner) or rows
    const float inner = w - 32;
    const int cols = std::max(2, int(inner / 200));
    const float tw = inner / cols, cover = tw - 20, th = 10 + cover + 8 + 20 + 2 + 18 + 10, cov = rowH - 4 - 16;
    for (int i = 0; i < n; i++) {
        const int ref = found[size_t(i)];
        const std::string name = ref < 0 ? std::string("Liked") : store.playlists[size_t(ref)].name;
        const Id id = uid("srch.pl", ref);
        if (tiles) {
            const float tx = x + 16 + (i % cols) * tw, ty = top + (i / cols) * th;
            if (!seen(ty, th)) continue;
            const std::vector<int> t = playlistTracks(ref);
            const bool hit = ui.item(id, { tx, ty, tw, th }, 14, i == 0 ? DefaultFocus : 0);
            const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
            if (hot) g.roundRect(tx, ty, tw, th, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
            if (!t.empty()) coverQuad(t, tx + 10, ty + 10, cover, 10);
            else {
                g.roundRect(tx + 10, ty + 10, cover, cover, 10, pal.surface2);
                g.icon(playlistIcon(ref), tx + 10 + cover / 2, ty + 10 + cover / 2, cover * 0.3f, pal.text3, 0.8f);
            }
            g.roundRect(tx + 18, ty + 18, 30, 30, 9, pal.bg, 0.72f);
            g.icon(playlistIcon(ref), tx + 33, ty + 33, 16, pal.text);
            if (hot) {
                g.circle(tx + 10 + cover - 31, ty + 10 + cover - 31, 21, pal.accent);
                g.icon(ICON_CHEV, tx + 10 + cover - 30, ty + 10 + cover - 31, 18, pal.onAccent);
            }
            const float ly = ty + 10 + cover + 8;
            g.text(name, { css::Base, Weight::SemiBold }, tx + 10, ly, pal.text, 1, Align::Left, int(cover));
            g.text(std::to_string(t.size()) + (t.size() == 1 ? " track" : " tracks"), { css::Sm }, tx + 10, ly + 22, pal.text3, 1, Align::Left, int(cover));
            if (hit) navigate(View::List, ref);
            continue;
        }
        const float ry = top + i * rowH, rx = x + 14, rw = w - 28;
        if (!seen(ry, rowH)) continue;
        if (ui.item(id, { rx, ry, rw, rowH - 4 }, 10, i == 0 ? DefaultFocus : 0)) navigate(View::List, ref);
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        const std::vector<int> t = playlistTracks(ref);
        const std::string info = std::to_string(t.size()) + (t.size() == 1 ? " track" : " tracks");
        g.roundRect(rx + 16, mid - cov / 2, cov, cov, 8, pal.surface2);
        g.icon(playlistIcon(ref), rx + 16 + cov / 2, mid, cov * 0.5f, ref < 0 ? pal.accentInk : pal.text2);
        const float tx = rx + 16 + cov + 14;
        if (mode == int(ListMode::List)) {
            g.textC(name, { css::Base, Weight::SemiBold }, tx, mid, pal.text, 1, Align::Left, int(rw * 0.5f));
            g.textC(info, { css::Sm }, rx + rw - 48, mid, pal.text3, 1, Align::Right, int(rw * 0.3f));
        } else {
            g.text(name, { css::Base, Weight::SemiBold }, tx, mid - 19, pal.text, 1, Align::Left, int(rw - 160));
            g.text(info, { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(rw - 160));
        }
        g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    return (tiles ? float((n + cols - 1) / cols) * th : n * rowH) + 24;
}
