/*
 * Gatefold NX · view_lists.cpp
 * Track lists: #view-playlist (play queue, Liked, folder playlists, search),
 * #view-album, and the Folders browser that Gatefold NX adds.
 */
#include "words.hpp"
#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <strings.h>
#include <sys/stat.h>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {
constexpr float RowH = 40;   // .vrow is 36 px on the PC; a little taller for fingers
}

// Search's filter tabs (the user, 2026-11-09): the library's switch's names, Genres between Artists and Playlists, the lyrics last
static const std::vector<std::string> kSearchTabs = { "Songs", "Albums", "Artists", "Genres", "Playlists", "Lyrics" };

void App::drawTrackList(float x, float y, float w, float h) {
    const View v = route.view;
    searchSync();   // (a Search page's indices follow the index and the playlists)
    if (v == View::Queue && srvq_.on) { drawServerQueue(x, y, w, h); return; }   // the server's tracks playing: their own queue
    if (v == View::Search && store.libSource == 1 && subsonic.view().state == Subsonic::SignedIn) { drawServerSearch(x, y, w, h); return; }   // the server chosen: its lists
    const std::vector<int> tracks = listTracks(v, route.ref, route.query);

    std::string kick = "Playlist", title, sub, playName;
    const char* placeholder = ICON_LIST;
    if (v == View::Queue) {
        // the queue wears the kind and icon of the playlist it came from, as that playlist's own page
        // (Automix: the user, 2026-10-29; Smart and the rest: 2026-10-29, session 6)
        if (const int src = queuePlaylist(); src >= 0) {
            kick = playlistKind(src);
            placeholder = playlistIcon(src);
        } else if (q.name == "Liked") placeholder = ICON_HEART;
        title = q.name.empty() ? "Nothing queued" : q.name;
        playName = q.name;
    } else if (v == View::Search) {
        kick = "Search";
        title = "“" + route.query + "”";
        playName = "Search: " + route.query;
        placeholder = ICON_SEARCH;
    } else if (route.ref < 0) {
        title = playName = "Liked";
        placeholder = ICON_HEART;
    } else if (route.ref < int(store.playlists.size())) {
        const FolderPlaylist& p = store.playlists[route.ref];
        kick = playlistKind(route.ref);
        title = playName = p.name;
        placeholder = playlistIcon(route.ref);
    }
    sub = fmtTotal(tracks);
    if (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size())) {   // an Automix playlist building up
        const FolderPlaylist& p = store.playlists[route.ref];
        if (p.automix && p.waiting && !p.manual) {
            int more = 0;
            for (const auto& path : p.tracks) more += pathIndex.count(path) && !automixReady(path) ? 1 : 0;
            if (more > 0) sub += "  ·  " + std::to_string(more) + (more == 1 ? " more song being analysed" : " more songs being analysed");
        }
        // a Mood playlist while the library's moods are measured: as an Automix playlist building up
        if (isMoodPlaylist(p) && moodTotal_ > moodDone_) {
            const int more = moodTotal_ - moodDone_;
            sub += "  ·  " + std::to_string(more) + (more == 1 ? " more song being analysed" : " more songs being analysed");
        }
    }
    if (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && !store.playlists[route.ref].own()) {
        const FolderPlaylist& p = store.playlists[route.ref];
        sub = (p.smart() ? smartSummary(p.path) : p.path) + "  ·  " + sub;
    }

    ScrollArea& sa = area(v == View::Search ? "search" : "list", int(v) * 1000 + route.ref);
    // narrow (a phone upright): a smaller cover, the actions on a line of their own under it, round, and no view buttons
    const bool narrow = w < kNarrowList;
    const float cs = narrow ? 96.f : 120.f;
    // upright: the views right of the actions where they fit (search's Play, Shuffle and New search leave room), else on a line under
    // them (the user, 2026-11-06: "The android port changed search again": the line under them was empty space there)
    const float headH = narrow ? 18 + cs + 14 + 34 + (plViewsOwnLine_ ? 10 + 36.f : 0.f) + 16 : 18 + 120 + 14;
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    const float oy = y - sa.y;
    // Room between the rows for the joins (a playlist's page, not the grid): Automix playlists, and the queue while it plays
    // one, its joins the same (the user, 2026-10-30: "the temporary automix playlist should also show them")
    const bool mixable = (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && store.playlists[route.ref].automix) ||
                         (v == View::Queue && automixQueue());
    // no Grid there (the user, 2026-10-30: "in Automix playlists this view makes no sense": tiles have no order to join): shown
    // with covers instead, your choice for other playlists kept
    const ListMode mode = mixable && ListMode(store.viewPlaylist) == ListMode::Grid ? ListMode::Covers : ListMode(store.viewPlaylist);
    // the joins always shown: no Mix button to hide them (the user, 2026-10-30: it read as turning the mixing off)
    const float joinGap = mixable && mode != ListMode::Grid ? 46 : 0;
    // Search: its filter tabs (the user, 2026-11-09): Songs, Albums, Artists, Genres,
    // Playlists, Lyrics, each as that list's own page shows it; upright (a phone) the page has its own pills, and both groups
    const bool tabs = v == View::Search && !narrow;
    const int stab = tabs ? searchTab_ : 0;
    const bool playRow = !tabs || stab == 0 || stab == 5;   // Play and Shuffle: the songs found, or those whose lyrics have the words
    float segW = 0, viewsW = 6 + 38.f * 4;
    if (tabs) {
        const Font sf{ css::Sm, Weight::SemiBold };
        segW = 6;
        for (const auto& t : kSearchTabs) segW += g.measure(t, sf) + 26;   // (as segmented's)
        viewsW = 6 + 38.f * 4;   // (the filter keeps the four views' room on every tab)
        if (stab != 0) {
            auto cnt = [](size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
            const LyricIndex::Progress lp0 = lyrIndex.progress();
            sub = stab == 1 ? cnt(searchAlbums_.size(), "album", "albums") : stab == 2 ? cnt(searchArtists_.size(), "artist", "artists")
                : stab == 3 ? cnt(searchGenres_.size(), "genre", "genres") : stab == 4 ? cnt(searchPlaylists_.size(), "playlist", "playlists")
                : cnt(searchLyrics_.size(), "song with the words in its lyrics", "songs with the words in their lyrics") + (lp0.read < lp0.total ? " so far" : "");
        }
    }

    // .pl-head: cover, kicker, title, sub, actions
    const float cx = x + 28, cyTop = oy + 18;
    if (!tracks.empty()) {
        coverQuad(tracks, cx, cyTop, cs, 12);   // .pl-cover.quad (no halo, as on the other pages)
    } else {
        g.roundRect(cx, cyTop, cs, cs, 12, pal.surface2);
        g.icon(placeholder, cx + cs / 2, cyTop + cs / 2, 40, pal.text3, 0.7f);
    }
    const float tx = cx + cs + (narrow ? 16 : 22), tw = w - (tx - x) - 28;
    if (kick == "Automix playlist") automixKicker(kick, tx, cyTop + 8);   // with a little magic (widgets.cpp)
    else kicker(kick, tx, cyTop + 8, pal.text3);
    g.text(title, { css::Xxl, Weight::Bold }, tx, cyTop + 16, pal.text, 1, Align::Left, int(tw));
    g.text(sub, { css::Sm }, tx, cyTop + 58, pal.text3, 1, Align::Left, int(tw));
    float bx = narrow ? cx : tx;
    const float by = narrow ? cyTop + cs + 14 : cyTop + 120 - 34;
    bool clicked = false;
    // The page's playlist, as it was when the frame began: Export and Open folder go to another page part way through
    // (route.ref is then the picker's −1 or a folder's index), and the buttons after them read the playlist again
    const int ref = route.ref;
    const bool ownPage = v == View::List && ref >= 0 && ref < int(store.playlists.size());
    // The actions after Play and Shuffle shrink to round icon buttons when their labels would run into the views
    // (a folder playlist with Games has seven); the playlist's icon button is always one.
    bool compact = false;
    // An Automix playlist plays in its own order: no Shuffle on its page, nor on the Playlist page while one plays (the user,
    // 2026-10-29; App::holdOrderForAutomix)
    const bool noShuffle = (v == View::Queue && automixQueue()) || (ownPage && store.playlists[ref].automix);
    {
        const Font bf{ css::Sm, Weight::SemiBold };
        auto bw = [&](const char* label) { return 16 + 24 + g.measure(label, bf) + 16 + 8.f; };
        float need = playRow ? bw("Play") + (noShuffle ? 0 : bw("Shuffle")) : 0.f;
        if (v == View::Queue) need += bw("Current song") + bw("Clear");
        else if (v == View::Search) need += bw("New search");
        else if (ownPage && !store.playlists[ref].folder()) need += bw("Delete") + bw("Rename") + (store.playlists[ref].automix ? bw("Order again") : 0);
        else if (ref >= 0) need += bw("Open folder") + bw("Delete");
        if (v == View::List && bgShown()) need += bw("Games");
        if (v != View::Search) need += bw("Export");
        // (Send: Liked and your own plain playlists only; no Automix, smart or mood playlist goes to a server: the user, 2026-11-06)
        if (v == View::List && subsonic.view().state == Subsonic::SignedIn &&
            (ref < 0 || (ref < int(store.playlists.size()) && store.playlists[size_t(ref)].own() && !store.playlists[size_t(ref)].automix)))
            need += bw("Send to Navidrome");
        if (ownPage) need += 34 + 8;
        compact = narrow || tx + need > x + w - 28 - viewsW - 16 - (tabs ? segW + 12 : 0);
    }
    // A secondary action: labelled, or (compact) round with its icon only
    auto action = [&](const char* key, const char* icon, const char* label, bool toggled = false, bool danger = false) {
        if (!compact) {
            bool c = false;
            bx += button(uid(key), icon, label, bx, by, false, &c, toggled, danger) + 8;
            return c;
        }
        const Id id = uid(key);
        hint(id, label);   // round with its icon only: its name when focused (App::drawHint)
        const bool c = ui.item(id, { bx, by, 34, 34 }, 17);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        g.roundRect(bx, by, 34, 34, 17, toggled ? pal.accent : pal.text, toggled ? 0.18f : hot ? 0.11f : 0.065f);
        g.roundStroke(bx, by, 34, 34, 17, 1, toggled ? pal.accent : pal.text, toggled ? 0.5f : 0.15f);
        g.icon(icon, bx + 17, by + 17, 16, danger ? dangerInk() : toggled ? pal.accentInk : pal.text);
        bx += 34 + 8;
        return c;
    };
    std::vector<int> playList = tracks;
    if (tabs && stab == 5) {   // Lyrics: the songs found by their lyrics, in the hits' order
        playList.clear();
        for (const auto& hh : searchLyrics_) if (auto ix = pathIndex.find(hh.path); ix != pathIndex.end()) playList.push_back(ix->second);
        playName = "Lyrics: " + route.query;
    }
    if (playRow) {
        bx += button(uid("pl.play"), ICON_PLAY, "Play", bx, by, true, &clicked, false, false, DefaultFocus) + 8;
        if (clicked && !playList.empty()) { if (v == View::Queue) playAt(0); else playTracks(playList, 0, playName); }
        if (!noShuffle) {
            bx += button(uid("pl.shuffle"), ICON_SHUFFLE, "Shuffle", bx, by, false, &clicked) + 8;
            if (clicked && !playList.empty()) playTracks(playList, 0, playName, true);
        }
    }
    if (v == View::Queue) {
        clicked = action("pl.jump", ICON_DISC, "Current song");
        const int cur = q.pos;   // the list is in play order
        if (clicked && cur >= 0) {
            // Gatefold's "Current track": the list glides there (the row need not exist yet, it is
            // only drawn once on screen) and the focus lands on it when it arrives.
            sa.target = headH + listRowY(listLayout(tracks, w, mode, joinGap), cur) - (h - RowH) / 2;
            sa.clamp();
            listJump = uid("pl.r", cur);
        }
        if (!tracks.empty()) {
            if (action("pl.clear", ICON_X, "Clear")) {
                queueClear();
                toast("Cleared the playlist");
            }
        }
    } else if (v == View::Search) {
        if (action("pl.search", ICON_SEARCH, "New search")) openSearch();
    } else if (v == View::List && ref >= 0 && ref < int(store.playlists.size()) && !store.playlists[ref].folder()) {
        if (action("pl.rename", ICON_TAG, "Rename")) renamePlaylist(ref);
        // an Automix playlist: ordered again for the best joins (also after it was moved by hand: lit then)
        if (store.playlists[ref].automix && action("pl.order", ICON_REFRESH, "Order again", store.playlists[ref].manual)) orderAutomixList(ref, true);
    } else if (ref >= 0 && ref < int(store.playlists.size())) {
        if (action("pl.folder", ICON_FOLDER, "Open folder")) navigate(View::Folders, lib.folderByPath(store.playlists[ref].path));
    }

    // Games: the game events that play this playlist (the background player), lit while any does
    if (v == View::List && bgShown()) {
        const uint64_t h = listHash(title);
        const bool used = (bgs.cfg.onOpen == 4 && bgs.cfg.lists[0] == h) || (bgs.cfg.onHome == 4 && bgs.cfg.lists[1] == h) ||
                          (bgs.cfg.onClose == 4 && bgs.cfg.lists[2] == h) || (bgs.cfg.onReturn == 4 && bgs.cfg.lists[3] == h);
        if (action("pl.games", ICON_GAME, "Games", used)) openListActions(title);
    }

    // Export, as an .m3u8 in a folder chosen on the SD card (m3u.hpp): any list here but search results
    if (v != View::Search) {
        if (action("pl.export", ICON_DOWNLOAD, "Export")) openExportPicker(playName.empty() ? title : playName, tracks);   // as listed: the queue in play order
    }
    // Send to the server (signed in; the user: playlists "both ways"): its tracks the server has, as a playlist of the same name there
    if (v == View::List && subsonic.view().state == Subsonic::SignedIn &&
        (ref < 0 || (ref < int(store.playlists.size()) && store.playlists[size_t(ref)].own() && !store.playlists[size_t(ref)].automix))) {
        std::string kind = subsonic.view().kind;
        static std::string label;
        label = "Send to " + (kind.empty() ? std::string("the server") : std::string(1, char(toupper(uint8_t(kind[0])))) + kind.substr(1));
        if (action("pl.send", PLI_CLOUD, label.c_str())) sendPlaylistToServer(playName.empty() ? title : playName, tracks);
    }

    // The playlist's sidebar icon, as a round button wearing it: the icon grid opens at once
    if (ownPage) {
        const Id iid = uid("pl.icon");
        hint(iid, "The playlist's icon");
        if (ui.item(iid, { bx, by, 34, 34 }, 17)) openPlaylistIcons(ref);
        const bool hot = ui.pressed(iid) || (ui.focused(iid) && ui.showFocusRing());
        g.roundRect(bx, by, 34, 34, 17, pal.accent, hot ? 0.24f : 0.14f);
        g.roundStroke(bx, by, 34, 34, 17, 1, pal.accent, 0.45f);
        g.icon(playlistIcon(ref), bx + 17, by + 17, 18, pal.accentInk);
        bx += 34 + 8;
    }

    // Delete, always the last of the actions (asked first on every playlist, folder ones too)
    if (ownPage && store.playlists[ref].folder()) {
        if (action("pl.remove", ICON_X, "Delete", false, true)) confirmRemoveFolderPlaylist(ref, int(tracks.size()));
    } else if (ownPage) {
        if (action("pl.remove", ICON_X, "Delete", false, true)) {
            // asked first, as a track's Delete is (the sheet's card)
            const FolderPlaylist& p = store.playlists[ref];
            const bool smart = p.smart();
            const std::string count = std::to_string(tracks.size()) + (tracks.size() == 1 ? " track" : " tracks");
            confirm(playlistIcon(ref), "Delete", p.name, std::string(playlistKind(ref)) + " · " + count,
                    "Delete this playlist?",
                    smart ? "It leaves the sidebar. Your songs, their play counts and Liked are not touched."
                          : "It leaves the sidebar, and its order is lost. The songs stay " GF_ON_CARD " and in your library.",
                    "Delete", [this, ref, name = p.name] {
                        if (ref < 0 || ref >= int(store.playlists.size()) || store.playlists[ref].name != name) return;
                        erasePlaylist(ref);
                        toast("Deleted the playlist “" + name + "”");
                    });
        }
    }

    // the four views, right of the actions (.pl-modes); Search upright keeps none, as it was (the user, 2026-11-06: "The android port
    // changed search again")
    float segRight = x + w - 28;   // Search's filter tabs go left of the views
    if (narrow && v == View::Search) plViewsOwnLine_ = false;
    else if (tabs && stab == 5) plViewsOwnLine_ = false;   // Lyrics: the hits, no views
    else if (tabs && stab != 0) {   // the tab's list in its page's views: Albums', Artists', Genres', Playlists' (three: By album would be the tiles again)
        int& field = stab == 1 ? store.viewAlbums : stab == 2 ? store.viewArtists : stab == 3 ? store.viewGenres : store.viewPlaylists;
        int m = field == 0 ? 2 : field;
        segRight -= viewSwitch("pl.view", x + w - 28, by + 17, m, 1u) + 12;
        if (m != field) { field = m; persistSoon(0.3f); }
        plViewsOwnLine_ = false;
    } else {
        int m = int(mode);
        const float vw = 6 + 38.f * (mixable ? 3 : 4);
        plViewsOwnLine_ = narrow && bx + vw > x + w - 28;
        segRight -= viewSwitch("pl.view", x + w - 28, plViewsOwnLine_ ? by + 34 + 10 + 18 : by + 17, m, mixable ? 1u << int(ListMode::Grid) : 0u) + 12;
        if (m != int(mode)) store.viewPlaylist = m;
    }
    if (tabs) {   // Songs · Albums · Artists · Genres · Playlists · Lyrics, in one place whatever the tab's views (none, three or four)
        segRight = x + w - 28 - (6 + 38.f * 4) - 12;
        int picked = -1;
        segmented(uid("pl.stab"), segRight, by + 17, kSearchTabs, stab, &picked, 0, 0, 0, 34);
        if (picked >= 0 && picked != stab) { searchTab_ = picked; sa.target = sa.y = 0; }
    }

    // the tracks (no panel behind them: the background shows through, as everywhere here)
    // Search: two groups when the lyrics found songs or are still being read (the user, 2026-11-05: "A: two groups"): Songs, then
    // In the lyrics (drawLyricHits)
    const LyricIndex::Progress lp = lyrIndex.progress();
    const bool lyricGroup = v == View::Search && !tabs && (!searchLyrics_.empty() || lp.read < lp.total);   // (upright: both groups on one page, as before the tabs)
    float ly = oy + headH;
    if (lyricGroup && !tracks.empty()) { kicker("Songs", x + 28, ly + 12, pal.text3); ly += 30; }
    // Liked: the server's starred tracks after yours (signed in); with none of yours, they alone (no "Tracks you like appear here")
    const bool srvGroup = v == View::List && route.ref < 0 && !srvStarred_.empty() && subsonic.view().state == Subsonic::SignedIn;
    if (tabs && stab != 0) {   // a tab's own list: its page's tiles or rows, or the lyrics' hits (drawSearchTab)
        g.clip(x, y, w, h);
        const float bodyH = drawSearchTab(x, ly, w, y, h);
        sa.content = headH + bodyH;
        g.unclip();
        ui.popArea();
        return;
    }
    if (tracks.empty() && !lyricGroup && !srvGroup) {
        listColumns(x, ly, w, mode == ListMode::Grid ? ListMode::List : mode, false);
        const char* msg = v == View::Search ? "No songs match" : v == View::Queue ? "Play something from the library or a folder"
                        : ref < 0 ? "Songs you like appear here"
                        : ref < int(store.playlists.size()) && store.playlists[ref].smart() ? "Nothing matches yet"
                                                                                                         : "This playlist has no songs";
        g.textC(msg, { css::Lg, Weight::SemiBold }, x + w / 2, ly + 90, pal.text2, 1, Align::Center);
    }
    g.clip(x, y, w, h);
    bodyGrab = v == View::Queue && grab.kind == Grab::Queue && !grab.upNext ? grab.pos
             : v == View::List && grab.kind == Grab::List && grab.ref == route.ref ? grab.pos : -1;
    // (no letter rail here: a playlist has its own order, an Automix one above all; the user, 2026-11-10)
    float bodyH = tracks.empty() && srvGroup ? -18.f : drawTrackBody("pl", tracks, x, ly, w, y, h, mode, false, [&](int i) {
        if (v == View::Queue) playAt(i);   // i is the position in play order
        else playTracks(tracks, i, playName);
    }, joinGap);
    if (v == View::Search && lyricGroup) bodyH = (ly - (oy + headH)) + bodyH + drawLyricHits(x, ly + bodyH, w, y, h);   // In the lyrics
    if (srvGroup) {   // Liked: the server's starred
        std::string kind = subsonic.view().kind;
        kind = kind.empty() ? std::string("your server") : std::string(1, char(toupper(uint8_t(kind[0])))) + kind.substr(1);
        const float gy = ly + bodyH + 18;
        // not those already in your Liked above (the same artist and title: the like sync keeps them as one)
        static std::vector<int> order;
        static unsigned orderStars = ~0u;
        static size_t orderLiked = ~size_t(0);
        if (orderStars != srvStarredGen_ || orderLiked != tracks.size()) {
            orderStars = srvStarredGen_;
            orderLiked = tracks.size();
            auto low = [](std::string t) { for (char& ch : t) ch = char(tolower(uint8_t(ch))); return t; };
            std::unordered_set<std::string> mine;
            for (int t : tracks) mine.insert(low(lib.tracks[size_t(t)].artist) + "" + low(lib.tracks[size_t(t)].title));
            order.clear();
            for (size_t i = 0; i < srvStarred_.size(); i++)
                if (!mine.count(low(srvStarred_[i].artist) + "" + low(srvStarred_[i].title))) order.push_back(int(i));
        }
        if (!order.empty()) kicker("Starred on " + kind, x + 28, gy + 12, pal.text3);
        const float sh = order.empty() ? -48.f : drawServerBody("pl.srv", srvStarred_, order, x, gy + 30, w, y, h, mode == ListMode::Albums ? ListMode::Covers : mode, false,
                                        [&](int i) { playServer(srvStarred_, i, "Liked"); });
        bodyH += 18 + 30 + sh;
    }
    bodyGrab = -1;
    // Mix: after each track (but the last) a line down to the next, and on it a pill saying the two are joined by Automix
    // (a streaming service's "Auto"). Only a sign for 2.0 (the user, 2026-10-29: "all in on Auto"; editing a join later perhaps): it
    // opens nothing and takes no focus. (The join editor, view_join.cpp, stays in the code, out of reach.)
    if (joinGap > 0) {
        const std::vector<ListEntry> layout = listLayout(tracks, w, mode, joinGap);
        const bool covers = mode == ListMode::Covers;
        const float lx = covers ? x + 14 + 16 + 44 + 14 + (60 - 4 - 16) / 2.f : x + 14 + 16 + 30;   // the cover's middle; by the number
        const Font lf{ css::Sm, Weight::SemiBold };
        for (const ListEntry& e : layout) {
            if (e.kind != ListEntry::Row || e.index + 1 >= int(tracks.size())) continue;
            const float g0 = ly + e.y + (covers ? e.h - 4 : e.h), g1 = ly + e.y + e.h + joinGap;
            if (g1 < y - 4 || g0 > y + h + 4) continue;
            const char* label = automix::name(automix::Preset::Auto);
            const float ph = 30, pt = (g0 + g1) / 2 - ph / 2, pl = lx - 15, pw = 4 + 22 + 10 + g.measure(label, lf) + 12;   // (no chevron: nothing to open)
            // the two lines linking the tracks, above and below the pill
            g.rect(lx - 1, g0 + 3, 2, pt - g0 - 3, pal.text, 0.22f);
            g.rect(lx - 1, pt + ph, 2, g1 - (pt + ph) - 3, pal.text, 0.22f);
            g.roundRect(pl, pt, pw, ph, 10, pal.surface2);
            g.roundRect(pl, pt, pw, ph, 10, pal.text, 0.05f);
            g.roundRect(pl + 4, pt + 4, 22, 22, 6, pal.accent, 1.f);
            g.icon(ICON_SPARKLE, pl + 15, pt + 15, 14, pal.onAccent);
            g.textC(label, lf, pl + 36, pt + ph / 2, pal.text, 1);
        }
    }
    if (v == View::Queue) {
        if (bodyFocus >= 0) hoverQueue = bodyFocus;
        if (grab.pos >= 0 && grab.kind == Grab::Queue && !grab.upNext) grab.cols = bodyCols;
    }
    sa.content = headH + bodyH;
    g.unclip();
    if (listJump && ui.hasItem(listJump) && std::fabs(sa.y - sa.target) < RowH) {
        ui.focus(listJump);
        listJump = 0;
    }
    ui.popArea();
}

/** A folder playlist's Delete (its page, or the Folders page's "In playlists"): the same card as your own playlists'. */
/** The first launch (the user, 2026-10-27: always asked): the card's Music folder offered, with the guided tour or without
 *  (the user, 2026-10-28), the picker under the card for another. No Cancel: Another folder, B or × leave the picker. */
void App::welcomeCard(const std::string& found) {
    confirm(ICON_FOLDER, "Welcome to Songate", "Where is your music?", "On " GF_THIS_CARD ": " + found, "Read your music from " + found + "?",
            "Songate makes albums, artists and genres from the tags of the files in one folder and the folders inside it, "
            "whatever their layout. Settings › Library can change it later.",
            "Use it and take the tour", [this, found] {
                tourAsk = false;
                tourPending = true;   // App::update starts it once the music is read (tour.cpp)
                useMusicFolder(found);
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_SPARKLE;
    sheet.askNoCancel = true;
    sheet.askAlt = "Just use it";
    sheet.askAltYes = [this, found] { tourAsk = false; useMusicFolder(found); };
    sheet.askAlt2 = "Another folder";
    sheet.askAlt2Yes = [] {};   // the picker is already under the card (the tour is offered once a folder is chosen: tourAsk)
}

void App::firstCard(const std::string& found) {
    confirm(ICON_MUSIC, "Welcome to Songate", "Where is your music?", "Your own files, or your own server", "Where is the music you want to play?",
            "In a folder " GF_ON_CARD ", read from its files' tags, or on a music server of yours: Navidrome, Airsonic, Gonic… any server with the "
            "Subsonic API. Settings › Library can add the other later.",
            "On " GF_THIS_CARD, [this, found] { firstLocal(found); });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_FOLDER;
    sheet.askNoCancel = true;
    sheet.askAlt = "On my server";
    sheet.askAltYes = [this, found] { firstServer(found); };
}

/** This device's music: Android's access first where not given, then the welcome card (or the picker under it, none found). */
void App::firstLocal(const std::string& found) {
#ifdef GF_ANDROID
    if (!storageOk_) { accessCard(); return; }
#endif
    if (!found.empty()) welcomeCard(found);
}

/** A server's music: its address, user and password (Settings' own steps); cancelled, the first card again. */
void App::firstServer(const std::string& found) {
    firstFound_ = found;
    subsonicSignIn();
    if (subsonic.view().state == Subsonic::SignedOut) { firstCard(found); return; }
    firstServer_ = true;   // signing in: serverTick shows how it went
}

#ifdef GF_ANDROID
/** Android lets Songate see the music only once allowed, on a page of its own (Android 10: a dialog): the card that asks, as a
 *  first start's first step or when access was taken away. Not now leaves the music folder page, its Allow access there. */
void App::accessCard() {
    const bool first = store.musicRoot.empty();
    confirm(ICON_FOLDER, first ? "Welcome to Songate" : "Your music", "Let Songate see your music",
            first ? "Android asks once" : "Android no longer lets Songate see " + store.musicRoot,
            "Allow Songate to read your files?",
            "Android shows a page of its own: turn on Songate there, then come back. Songate reads your music with its lyrics and covers, "
            "and changes a file only when you edit its tags or delete it.",
            "Allow access", [] { plat::askStorage(); });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_FOLDER;
    sheet.askNoCancel = true;
    sheet.askAlt = "Not now";
    sheet.askAltYes = [] {};
}

/** Access given (seen once a second, Songate back from Android's page): the music offered as on any first start, or read again. */
void App::storageGiven() {
    storageOk_ = true;
    pickerGen++;   // the folder page lists the volumes again
    if (sheet.page == Sheet::Confirm && sheet.askButton == "Allow access") closeSheet();
    if (store.musicRoot.empty()) {
        const std::string found = plat::musicFolder();
        if (!found.empty()) welcomeCard(found);
    } else {
        rescan();
    }
}
#endif

void App::confirmRemoveFolderPlaylist(int ref, int count) {
    if (ref < 0 || ref >= int(store.playlists.size())) return;
    const FolderPlaylist& p = store.playlists[ref];
    confirm(ICON_FOLDER, "Delete", p.name, "Folder playlist · " + std::to_string(count) + (count == 1 ? " track" : " tracks"),
            "Delete this playlist?", "It leaves the sidebar. The folder and its songs stay " GF_ON_CARD "; Folders can add it again.",
            "Delete", [this, ref, name = p.name, path = p.path] {
                if (ref >= int(store.playlists.size()) || store.playlists[ref].name != name || store.playlists[ref].path != path) return;
                erasePlaylist(ref);
                toast("Deleted the playlist “" + name + "”");
            });
}

void App::drawAlbum(float x, float y, float w, float h) {
    const int a = routeAlbum();
    if (a < 0) { viewHead("Album", "Not in the library", "Its tags changed or the music folder was rescanned.", x, y, w); return; }
    const Album& al = lib.albums[a];

    ScrollArea& sa = area("album", a);
    // narrow (a phone upright): the cover on top, the title, its line and the actions under it (Play and Shuffle named, the rest
    // round with their icons)
    const bool narrow = w < kNarrowList;
    const float ncs = std::min(w - 60, 240.f);
    const Font h1{ narrow ? css::Xxl : 46, narrow ? Weight::Bold : Weight::ExtraBold };
    const float lineH = narrow ? 38.f : 48.f;
    const std::vector<std::string> lines = g.wrap(al.title, h1, int(narrow ? w - 60 : w - (x + 30 + 220 + 28 - x) - 30), 2);
    const float heroH = narrow ? 22 + ncs + 18 + 24 + lines.size() * lineH + 8 + 22 + 16 + 34 + 18 : 26 + 220 + 22;
    sa.view = { x, y, w, h };
    sa.content = heroH + 30 + 8 + al.tracks.size() * RowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // .album-hero::before (the album's accent, fading down) and the cover's halo are left out: on the console the
    // tint covered only the content area and clashed with the sidebar and player bar. The app's background stays.
    const float cx = narrow ? x + (w - ncs) / 2 : x + 30, cyTop = oy + (narrow ? 22 : 26);
    coverBox(al.tracks[0], cx, cyTop, narrow ? ncs : 220, 12);

    // h1 + .album-meta + .album-actions, bottom-aligned with the cover (narrow: under it)
    const float tx = narrow ? x + 30 : cx + 220 + 28, tw = w - (tx - x) - 30;
    float ty = narrow ? cyTop + ncs + 18 : cyTop + 220 - 34 - 16 - 22 - 8 - lines.size() * 48 - 6 - 18;
    kicker("Album", tx, ty + 8, pal.text3);
    ty += 18 + 6;
    for (const auto& l : lines) { g.text(l, h1, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw)); ty += lineH; }
    ty += 8;
    float mx = tx;
    if (!al.artist.empty()) mx += g.text(al.artist, { css::Base, Weight::SemiBold }, mx, ty, pal.text);
    std::string rest = (al.year.empty() ? "" : " · " + al.year) + " · " + fmtTotal(al.tracks);
    g.text(rest, { css::Base }, mx, ty, pal.text2, 1, Align::Left, int(tx + tw - mx));
    ty += 22 + 16;

    float bx = tx;
    bool clicked = false;
    const std::string name = al.title;
    bx += button(uid("al.play"), ICON_PLAY, "Play", bx, ty, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked) playTracks(al.tracks, 0, name);
    bx += button(uid("al.shuffle"), ICON_SHUFFLE, "Shuffle", bx, ty, false, &clicked) + 8;
    if (clicked) playTracks(al.tracks, 0, name, true);
    // the rest: named, or (narrow) round with the icon only, their names as hints
    auto more = [&](const char* key, const char* icon, const std::string& label) {
        if (!narrow) { bool c = false; bx += button(uid(key), icon, label, bx, ty, false, &c) + 8; return c; }
        const Id id = uid(key);
        hint(id, label);
        const bool c = ui.item(id, { bx, ty, 34, 34 }, 17);
        g.roundRect(bx, ty, 34, 34, 17, pal.text, ui.pressed(id) || (ui.focused(id) && ui.showFocusRing()) ? 0.11f : 0.065f);
        g.roundStroke(bx, ty, 34, 34, 17, 1, pal.text, 0.15f);
        g.icon(icon, bx + 17, ty + 17, 16, pal.text);
        bx += 34 + 8;
        return c;
    };
    if (more("al.artist", ICON_MIC, "Artist")) openArtist(al.artist);
    if (more("al.folder", ICON_FOLDER, "Folder")) navigate(View::Folders, lib.folderByPath(lib.tracks[al.tracks[0]].folder));
    {
        const std::string& gs = lib.tracks[al.tracks[0]].genre;
        const std::string g1 = gs.substr(0, gs.find(';'));
        if (!g1.empty() && more("al.genre", ICON_TAG, g1)) openGenre(g1);
    }

    const float ly = oy + heroH;
    listHead(x, ly, w, false);
    g.clip(x, std::max(y, ly + 30), w, y + h - std::max(y, ly + 30));
    const float top = ly + 30 + 8;
    for (int i = 0; i < int(al.tracks.size()); i++) {
        const float ry = top + i * RowH;
        if (ry + RowH < y - RowH || ry > y + h + RowH) continue;
        if (trackRow(uid("row", i), al.tracks[i], i + 1, x + 14, ry, w - 28, RowH, false)) playTracks(al.tracks, i, name);
    }
    g.unclip();
    ui.popArea();
}

void App::drawFolders(float x, float y, float w, float h) {
    if (lib.folders.empty()) {
        viewHead("Folders", "No music folder", GF_NO_MUSIC, x, y, w);
        return;
    }
    int f = route.ref;
    if (f < 0 || f >= int(lib.folders.size())) f = route.ref = lib.rootFolders[0];
    const FolderNode& node = lib.folders[f];
    const bool isRoot = node.parent < 0;
    const int total = node.last - node.first;
    // By album groups a folder's own tracks; a folder of sub-folders only would show the tiles twice, so it has three views
    const bool ownTracks = !lib.folders[f].tracks.empty();
    const int shownMode = !ownTracks && store.viewFolders == 0 ? 2 : store.viewFolders;
    const ListMode mode = ListMode(shownMode);

    ScrollArea& sa = area("folders", f);
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // .view-head
    std::string sub = node.path;
    if (!node.children.empty()) sub += "  ·  " + std::to_string(node.children.size()) + (node.children.size() == 1 ? " folder" : " folders");
    sub += "  ·  " + std::to_string(total) + (total == 1 ? " track" : " tracks");
    float cy = oy + 18 + 8;
    kicker("Folders", x + 28, cy, pal.text3);
    cy += 10;
    g.text(isRoot ? "Music" : node.name, { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    // Actions: play everything below, keep it as a playlist, go up; the four views on the right
    float bx = x + 28;
    bool clicked = false;
    const std::string name = isRoot ? "Music" : node.name;
    // (everything below it is gathered at the press: every frame, it walked the folder's whole tree)
    bx += button(uid("fo.play"), ICON_PLAY, "Play", bx, cy, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked) if (const std::vector<int> all = lib.tracksUnder(f); !all.empty()) playTracks(all, 0, name);
    bx += button(uid("fo.shuffle"), ICON_SHUFFLE, "Shuffle", bx, cy, false, &clicked) + 8;
    if (clicked) if (const std::vector<int> all = lib.tracksUnder(f); !all.empty()) playTracks(all, 0, name, true);
    const bool narrow = w < kNarrowList;
    // narrow (a phone upright): named buttons as everywhere else, a line further down where they do not fit (two round "+"
    // looked the same and said nothing: the user's test, 2026-11-04)
    auto more = [&](const char* key, const char* icon, const std::string& label, float& at, bool on = false) {
        if (narrow) {
            const float bw = 16 + 24 + g.measure(label, { css::Sm, Weight::SemiBold }) + 16;
            if (bx + bw > x + w - 28) { bx = x + 28; at += 42; }
        }
        bool c = false;
        bx += button(uid(key), icon, label, bx, at, false, &c, on) + 8;
        return c;
    };
    const int pl = store.playlistByPath(node.path);
    if (more("fo.addpl", pl >= 0 ? ICON_LIST : ICON_PLUS, pl >= 0 ? "In playlists" : "Add as playlist", cy, pl >= 0)) {
        if (pl >= 0) {
            confirmRemoveFolderPlaylist(pl, total);   // asks first, as on the playlist's own page
        } else {
            // Two folders of one name ("CD1" of two albums) would be one list to the game actions and the overlay
            const std::string as = store.freePlaylistName(name);
            store.playlists.push_back({ as, node.path, {} });
            toast("Added “" + as + "” to playlists");
            store.save();
        }
    }
    if (!node.children.empty() && more("fo.addall", ICON_PLUS, narrow ? "Add all" : "Add all as playlists", cy)) addSubfoldersAsPlaylists(f);
    if (!isRoot && more("fo.up", ICON_BACK, "Up", cy)) { navigate(View::Folders, node.parent, {}, false); ui.focus(uid("fo.dir", f)); }
    {
        int m = shownMode;
        const float vw = 6 + 38.f * (ownTracks ? 4 : 3);
        if (narrow && bx + vw > x + w - 28) { cy += 34 + 10; }   // upright: on a line of its own where the buttons leave no room
        viewSwitch("fo.view", x + w - 28, cy + 17, m, ownTracks ? 0u : 1u);
        if (m != shownMode) store.viewFolders = m;   // a folder without tracks leaves a chosen "by album" as it is
    }
    cy += 34 + 16;

    g.clip(x, y, w, h);
    // the #-to-Z rail (Settings › Appearance › Letter rail): the sub-folders' names, or, in a folder of songs alone, the songs' titles
    // in their order (the user, 2026-11-09: "inside a folder"; not on tiles)
    const bool railDirs = railOn(8u) && !node.children.empty();
    const bool railSongs = railOn(8u) && node.children.empty() && !node.tracks.empty() && mode != ListMode::Grid;
    const bool rail = railDirs || railSongs;
    const float lw = rail ? w - kRailW : w, listTop = cy;
    cy += drawSubfolders("fo.dir", node, x, cy, lw, y, h, mode);

    // Tracks directly in this folder, in the chosen view
    float bodyH = 0;
    if (!node.tracks.empty()) {
        cy += 16;
        bodyH = drawTrackBody("fo", node.tracks, x, cy, lw, y, h, mode, false, [&](int i) { playTracks(node.tracks, i, name); });
    } else if (node.children.empty()) {
        g.textC("This folder is empty", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center);
    }
    sa.content = cy - oy + bodyH + 24;
    g.unclip();
    ui.popArea();
    if (railDirs) {
        const auto& letters = railLettersFor("fo", std::to_string(libEpoch) + ":" + std::to_string(f) + ":" + std::to_string(node.children.size()), [&] {
            std::vector<std::string> names;
            for (int c : node.children) names.push_back(lib.folders[size_t(c)].name);
            return names;
        });
        const bool tiles = mode == ListMode::Grid || mode == ListMode::Albums;
        const int cols = std::max(2, int((lw - 36) / 200));
        const float tw = (lw - 36) / cols, th = 10 + (tw - 20) + 8 + 20 + 2 + 18 + 10, rowH = mode == ListMode::List ? 48 : 60;
        const float railTop = y + (listTop - oy);
        letterRail("fo", x + w - 24, railTop, y + h - railTop, letters, sa, [&](int row) { return (listTop - oy) + (tiles ? float(row / cols) * th : float(row) * rowH); });
    } else if (railSongs) {   // the songs begin 16 under where the sub-folders would
        const auto& letters = railLettersFor("fo.songs", std::to_string(indexGen) + ":" + std::to_string(f) + ":" + std::to_string(node.tracks.size()), [&] {
            std::vector<std::string> names;
            for (int t : node.tracks) names.push_back(lib.tracks[size_t(t)].title);
            return names;
        });
        const std::vector<ListEntry> layout = listLayout(node.tracks, lw, mode);
        const float top = listTop + 16 - oy, railTop = y + top;
        letterRail("fo.songs", x + w - 24, railTop, y + h - railTop, letters, sa, [&](int row) { return top + listRowY(layout, row); });
    }
}

/** A folder's sub-folders in the chosen view (Folders, and Library → Folders): tiles with four covers in the big
 *  views, rows with one in the small ones. Returns the height drawn. */
float App::drawSubfolders(const char* idp, const FolderNode& node, float x, float cy, float w, float y, float h, ListMode mode) {
    const int nSub = int(node.children.size());
    if (!nSub) return 0;
    if (mode == ListMode::Grid || mode == ListMode::Albums) {
        // .tile with the folder's four covers (Gatefold 1.4.1's Folders), laid out as the album grid is (view_grid.cpp gridFor)
        const float inner = w - 36;
        const int cols = std::max(2, int(inner / 200));
        const float tw = inner / cols, cover = tw - 20, th = 10 + cover + 8 + 20 + 2 + 18 + 10;
        const int rows = (nSub + cols - 1) / cols;
        for (int r = 0; r < rows; r++) {
            const float ry = cy + r * th;
            if (ry + th < y - 40 || ry > y + h + 40) continue;
            for (int c = 0; c < cols && r * cols + c < nSub; c++) {
                const int ch = node.children[r * cols + c];
                if (folderTile(uid(idp, ch), ch, x + 18 + c * tw, ry, tw, cover)) navigate(View::Folders, ch);
            }
        }
        return rows * th;
    }
    // rows: a cover (or the folder icon), the name, what is inside, a chevron
    const float rowH = mode == ListMode::List ? 48 : 60, cov = rowH - 4 - 16;
    for (int i = 0; i < nSub; i++) {
        const int c = node.children[i];
        const FolderNode& ch = lib.folders[c];
        const float ry = cy + i * rowH, rx = x + 14, rw = w - 28;
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        const Id id = uid(idp, c);
        if (ui.item(id, { rx, ry, rw, rowH - 4 }, 10)) navigate(View::Folders, c);
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        if (ch.last > ch.first && meta.art(ch.first).state == AlbumArt::Ready) {
            coverBox(ch.first, rx + 16, mid - cov / 2, cov, 6);
        } else {
            if (ch.last > ch.first) meta.wantTags(ch.first);
            g.roundRect(rx + 16, mid - cov / 2, cov, cov, 6, pal.surface2);
            g.icon(ICON_FOLDER, rx + 16 + cov / 2, mid, cov * 0.5f, pal.text2);
        }
        const int n = ch.last - ch.first;
        std::string info = std::to_string(n) + (n == 1 ? " track" : " tracks");
        if (!ch.children.empty()) info = std::to_string(ch.children.size()) + (ch.children.size() == 1 ? " folder · " : " folders · ") + info;
        if (store.playlistByPath(ch.path) >= 0) info += " · in playlists";
        const float tx = rx + 16 + cov + 14;
        if (mode == ListMode::List) {
            g.textC(ch.name, { css::Base, Weight::SemiBold }, tx, mid, pal.text, 1, Align::Left, int(rw * 0.5f));
            g.textC(info, { css::Sm }, rx + rw - 48, mid, pal.text3, 1, Align::Right, int(rw * 0.35f));
        } else {
            g.text(ch.name, { css::Base, Weight::SemiBold }, tx, mid - 19, pal.text, 1, Align::Left, int(rw - 140));
            g.text(info, { css::Sm }, tx, mid + 1, pal.text3, 1, Align::Left, int(rw - 140));
        }
        g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    return nSub * rowH;
}

/** "Add all as playlists" (Folders, Library → Folders): every sub-folder of a folder becomes a folder playlist;
 *  those already in are left as they are, a taken name gets "<name> 2". Asks first, with the count. */
/** A genre's page: Add as playlist (a smart Genre playlist: it follows the genre), or In playlists (pressed: Delete, asked first). */
void App::genrePlaylistButton(const std::string& genre, float& bx, float by) {
    const std::string spec = "smart:genre:" + genre;
    const int pl = store.playlistByPath(spec);
    bool c = false;
    bx += button(uid("gen.addpl"), pl >= 0 ? ICON_LIST : ICON_PLUS, pl >= 0 ? "In playlists" : "Add as playlist", bx, by, false, &c, pl >= 0) + 8;
    if (!c) return;
    if (pl >= 0) {
        const FolderPlaylist& p = store.playlists[size_t(pl)];
        const int count = int(smartTracks(p.path).size());
        confirm(ICON_TAG, "Delete", p.name, "Genre playlist · " + std::to_string(count) + (count == 1 ? " track" : " tracks"), "Delete this playlist?",
                "It leaves the sidebar. The genre and its songs stay " GF_ON_CARD "; Genres can add it again.", "Delete", [this, pl, name = p.name, spec] {
                    if (pl >= int(store.playlists.size()) || store.playlists[size_t(pl)].path != spec) return;
                    erasePlaylist(pl);
                    toast("Deleted the playlist “" + name + "”");
                });
        return;
    }
    const std::string as = store.freePlaylistName(genre);
    store.playlists.push_back({ as, spec, {} });
    toast("Added “" + as + "” to playlists");
    store.save();
}

/** Genres: Add all as playlists: every genre not in playlists yet, each a smart Genre playlist (asked first, with the count). */
void App::addGenresAsPlaylists() {
    std::vector<std::string> add;
    for (const Artist& gn : genres) if (store.playlistByPath("smart:genre:" + gn.name) < 0) add.push_back(gn.name);
    if (add.empty()) { toast("Every genre is in playlists already"); return; }
    const int n = int(add.size());
    const std::string count = std::to_string(n) + (n == 1 ? " genre" : " genres");
    confirm(ICON_LIST, "Genres", "Your genres", count + " not in playlists yet", "Add " + count + " as playlists?",
            "Each becomes a playlist in the sidebar, named after its genre, with its songs; it follows the genre as your library changes. "
            "Genres already in playlists stay as they are.",
            "Add all", [this] {
                int added = 0;
                for (const Artist& gn : genres) {
                    const std::string spec = "smart:genre:" + gn.name;
                    if (store.playlistByPath(spec) >= 0) continue;
                    store.playlists.push_back({ store.freePlaylistName(gn.name), spec, {} });
                    added++;
                }
                store.save();
                toast("Added " + std::to_string(added) + (added == 1 ? " playlist" : " playlists"));
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_PLUS;
}

void App::addSubfoldersAsPlaylists(int f) {
    if (f < 0 || f >= int(lib.folders.size())) return;
    std::vector<int> add;
    for (int c : lib.folders[f].children)
        if (store.playlistByPath(lib.folders[c].path) < 0) add.push_back(c);
    if (add.empty()) { toast("Every folder here is in playlists already"); return; }
    const int n = int(add.size());
    const std::string count = std::to_string(n) + (n == 1 ? " folder" : " folders");
    confirm(ICON_LIST, "Folders", lib.folders[f].parent < 0 ? "Music" : lib.folders[f].name, count + " not in playlists yet",
            "Add " + count + " as playlists?",
            "Each becomes a playlist in the sidebar, named after its folder, with the songs in it and in the folders inside it. "
            "Folders already in playlists stay as they are.",
            "Add all", [this, f, path = lib.folders[f].path] {
                if (f >= int(lib.folders.size()) || lib.folders[f].path != path) return;   // rescanned meanwhile
                int added = 0;
                for (int c : lib.folders[f].children) {
                    const FolderNode& ch = lib.folders[c];
                    if (store.playlistByPath(ch.path) >= 0) continue;
                    store.playlists.push_back({ store.freePlaylistName(ch.name), ch.path, {} });
                    added++;
                }
                store.save();
                toast("Added " + std::to_string(added) + (added == 1 ? " playlist" : " playlists"));
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_PLUS;
}

/**
 * Browsing the SD card's folders: for the music folder ("Use this folder"); as View::PickPlaylist, for a playlist
 * file to play or import (+ → Import a playlist file…: .m3u and .m3u8 files are listed after the folders); as
 * View::PickExport, for the folder an exported playlist goes in ("Export here"; the files there are shown, not picked).
 */
void App::drawPicker(float x, float y, float w, float h) {
    const bool exporting = route.view == View::PickExport;
    const bool files = route.view == View::PickPlaylist || exporting;
    const View self = route.view;
    std::string path = route.query.empty() ? std::string(plat::root()) : route.query;
    const bool isRoot = plat::isRoot(path);
    auto join = [](const std::string& a, const std::string& b) { return plat::join(a, b); };
    auto parent = [&](const std::string& p) { return plat::parentDir(p); };
    // Listing the card is cheap but not free: keep the last listing.
    static std::string listedPath;
    static bool listedFiles = false;
    static unsigned listedGen = ~0u;
    static std::vector<std::string> dirs, lists;
    if (listedPath != path || listedFiles != files || listedGen != pickerGen) {
        listedGen = pickerGen;
        dirs = listDirs(path);
#ifdef GF_ANDROID
        if (!storageOk_) dirs.clear();   // the volumes are known, nothing in them can be seen yet: nothing to choose
#endif
        lists = files ? listPlaylistFiles(path) : std::vector<std::string>();
        listedPath = path;
        listedFiles = files;
    }
    const int nDirs = int(dirs.size()), nRows = nDirs + int(lists.size());

    ScrollArea& sa = area(exporting ? "pickex" : files ? "pickpl" : "picker", int(uid(path) & 0x7fffffff));
    const float headH = 18 + 8 + 10 + 38 + 22 + 16 + 34 + 16;
    const float rowH = 52;
    sa.view = { x, y, w, h };
    sa.content = headH + nRows * rowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    float cy = oy + 18 + 8;
    const bool choosing = !files && folderFor >= 0;   // Settings → Storage and reset: a browser's start folder
    kicker(exporting ? "Export “" + exportName + "” to" : files ? "Import a playlist file" : choosing ? folderPickName() : "Music folder", x + 28, cy, pal.text3);
    cy += 10;
    g.text(isRoot ? GF_CARD_NAME : shownName(path), { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    std::string sub = (path == "/" ? std::string(GF_CARD_NAME) : path) + "  ·  " + std::to_string(dirs.size()) + (dirs.size() == 1 ? " folder" : " folders");
    if (files) sub += lists.empty() ? std::string("  ·  no playlist files")
                                    : "  ·  " + std::to_string(lists.size()) + (lists.size() == 1 ? " playlist file" : " playlist files");
    else if (choosing) sub += folderPickField().empty() ? std::string("  ·  now its own default") : "  ·  now " + folderPickField();
    else if (!store.musicRoot.empty()) sub += "  ·  now reading " + store.musicRoot;
#ifdef GF_ANDROID
    const bool noAccess = !storageOk_ && !files && !choosing;   // Android has not let Songate see the files yet (accessCard)
    if (noAccess) sub = "Songate cannot see your files yet: Android asks you to allow it once, on a page of its own";
#endif
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    float bx = x + 28;
    bool clicked = false;
    if (choosing) {
        bx += button(uid("pk.use"), ICON_CHECK, "Choose this folder", bx, cy, true, &clicked, false, false, isRoot ? 0 : DefaultFocus) + 8;
        // Backups inside Gatefold's own settings folder would be copied into themselves, and cleared by a fresh start
        const std::string cfg = Store::Dir;
        if (clicked && folderFor == 4 && strncasecmp(path.c_str(), cfg.c_str(), cfg.size()) == 0 && (path.size() == cfg.size() || path[cfg.size()] == '/')) {
            toast("Backups cannot go in Songate's own settings folder");
            clicked = false;
        }
        if (clicked) {
            ui.popArea();
            folderPickField() = path;
            const std::string what = folderPickName();
            folderFor = -1;
            persistSoon(0.3f);
            back();
            toast(what + ": " + (isRoot ? std::string(plat::rootName()) : path));
            return;
        }
    }
#ifdef GF_ANDROID
    else if (noAccess) {
        bx += button(uid("pk.access"), ICON_FOLDER, "Allow access", bx, cy, true, &clicked, false, false, DefaultFocus) + 8;
        if (clicked) plat::askStorage();
    }
#endif
    else if (!files) {
        bx += button(uid("pk.use"), ICON_FOLDER, "Use this folder", bx, cy, true, &clicked, false, false, isRoot ? 0 : DefaultFocus) + 8;
        if (clicked) { ui.popArea(); useMusicFolder(path); return; }
    }
    if (exporting) {
        bx += button(uid("pk.export"), ICON_DOWNLOAD, "Export here", bx, cy, true, &clicked, false, false, DefaultFocus) + 8;
        if (clicked) { ui.popArea(); exportPlaylist(path); return; }
        bx += button(uid("pk.mkdir"), ICON_PLUS, "New folder", bx, cy, false, &clicked) + 8;
        if (clicked) {
            std::string name;
            if (keyboard("New folder", "", name)) {
                while (!name.empty() && name.back() == ' ') name.pop_back();
                while (!name.empty() && name[0] == ' ') name.erase(0, 1);
                const std::string made = join(path, name);
                struct stat st;
                if (name.empty()) {
                    // nothing typed: nothing made
                } else if (name[0] == '.' || name.find_first_of("/\\:*?\"<>|") != std::string::npos) {
                    toast("A folder name cannot start with a dot or hold / \\ : * ? \" < > |");
                } else if (stat(made.c_str(), &st) == 0) {
                    toast("“" + name + "” is already here");
                } else if (mkdir(made.c_str(), 0777) != 0) {
                    toast("Could not make the folder “" + name + "”");
                } else {
                    listedPath.clear();   // list it again
                    ui.popArea();
                    navigate(self, -1, made, false);
                    toast("Made the folder “" + name + "”");
                    return;
                }
            }
        }
    }
    if (!isRoot) {
        bx += button(uid("pk.up"), ICON_BACK, "Up", bx, cy, false, &clicked, false, false, files && !exporting && nRows == 0 ? DefaultFocus : 0) + 8;
        if (clicked) { navigate(self, -1, parent(path), false); ui.focus(uid("pk.dir", int(uid(path) & 0x7fffffff))); }
    }
    if (files || choosing || !store.musicRoot.empty()) {
        bx += button(uid("pk.cancel"), ICON_X, "Cancel", bx, cy, false, &clicked, false, false, files && !exporting && isRoot && nRows == 0 ? DefaultFocus : 0) + 8;
        if (clicked) {
            ui.popArea();
            folderFor = -1;
            if (files) leavePlaylistPicker();
            else back();
            return;
        }
    }
    cy += 34 + 16;

    g.clip(x, y, w, h);
    for (int i = 0; i < nRows; i++) {
        const float ry = cy + i * rowH, rx = x + 14, rw = w - 28;
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        const bool isFile = i >= nDirs;
        const std::string& name = isFile ? lists[size_t(i - nDirs)] : dirs[size_t(i)];
        const std::string child = join(path, name);
        const Id id = uid("pk.dir", int(uid(child) & 0x7fffffff));
        // exporting: the files already there are shown (a name taken is asked about), not picked
        const bool pickable = !(exporting && isFile);
        if (pickable && ui.item(id, { rx, ry, rw, rowH - 4 }, 10, (isRoot || files) && !exporting && i == 0 ? DefaultFocus : 0)) {
            if (isFile) openPlaylistFile(child);
            else navigate(self, -1, child, !files && !choosing);   // the playlist browsers and a chosen folder: one entry in the history
        }
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        if (isFile) {
            // a playlist file: the accent tile with the list icon, its name without the extension, then the extension
            const size_t dot = name.find_last_of('.');
            g.roundRect(rx + 16, mid - 18, 36, 36, 8, pal.accent, pickable ? 0.16f : 0.08f);
            g.icon(ICON_LIST, rx + 34, mid, 18, pickable ? pal.accentInk : pal.text3);
            g.textC(name.substr(0, dot), { css::Base, Weight::SemiBold }, rx + 16 + 36 + 14, mid, pickable ? pal.text : pal.text3, 1, Align::Left, int(rw - 220));
            g.textC("Playlist file · ." + name.substr(dot + 1), { css::Sm }, rx + rw - 44, mid, pal.text3, 1, Align::Right);
        } else {
            const bool current = !files && strcasecmp(child.c_str(), (choosing ? folderPickField() : store.musicRoot).c_str()) == 0;
            g.roundRect(rx + 16, mid - 18, 36, 36, 8, current ? pal.accent : pal.surface2, current ? 0.25f : 1.f);
            g.icon(ICON_FOLDER, rx + 34, mid, 18, current ? pal.accentInk : pal.text2);
            g.textC(isRoot ? shownName(join(path, name)) : name, { css::Base, Weight::SemiBold }, rx + 16 + 36 + 14, mid, current ? pal.accentInk : pal.text, 1, Align::Left, int(rw - 160));
            if (current) g.textC(choosing ? "chosen now" : "current music folder", { css::Sm }, rx + rw - 44, mid, pal.text3, 1, Align::Right);
        }
        if (pickable) g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    if (nRows == 0)
        g.textC(files ? "No folders or playlist files here" : "No folders here", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center);
    g.unclip();
    ui.popArea();
}

/** Search's In the lyrics (BETA 3; the user, 2026-11-05: "A: two groups", "Start of words"): each song whose lyrics have the words,
 *  its cover, name and artist, and the line, the words lit. Pressed: a synced line asks where to play from (playLyricHit). */
float App::drawLyricHits(float x, float top, float w, float clipY, float clipH) {
    float y = top + 10;
    const LyricIndex::Progress p = lyrIndex.progress();
    kicker("In the lyrics", x + 28, y + 12, pal.text3);
    if (p.read < p.total) {   // still reading: how far, in fixed-width digits (a count that changes must not move its line)
        const std::string s = "Reading lyrics · " + std::to_string(p.read) + " of " + std::to_string(p.total);
        g.textNum(s, { css::Xs }, x + w - 42, y + 12 - g.lineHeight({ css::Xs }) / 2.f, pal.text3, 1, Align::Right);
    }
    y += 30;
    if (searchLyrics_.empty()) {
        g.text(p.read < p.total ? "Nothing yet in the songs read so far" : "No lyrics have these words", { css::Base }, x + 28, y + 4, pal.text3);
        return y + 40 - top;
    }
    // the words searched for, folded, to light them where they start a word
    std::vector<std::string> words;
    {
        std::string wd;
        for (char c : foldText(route.query) + " ") {
            if (c == ' ') { if (!wd.empty()) words.push_back(wd); wd.clear(); }
            else wd += c;
        }
    }
    for (size_t i = 0; i < searchLyrics_.size(); i++) {
        const LyricIndex::Hit& h = searchLyrics_[i];
        auto ix = pathIndex.find(h.path);
        if (ix == pathIndex.end()) continue;
        const int t = ix->second;
        const float rh = 74;
        // only the hits in view are drawn (the self test on the console: a search for "a" drew hundreds a frame, 240 ms each)
        if (y + rh < clipY - rh || y > clipY + clipH + rh) { y += rh; continue; }
        const Id id = uid("ls.hit", int64_t(i));
        if (ui.item(id, { x + 14, y, w - 28, rh - 4 }, 10)) playLyricHit(i);
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(x + 14, y, w - 28, rh - 4, 10, pal.text, 0.065f);
        coverBox(t, x + 28, y + 7, 40, 6);
        const Track& tr = lib.tracks[size_t(t)];
        const float tx = x + 28 + 40 + 14, tw = w - (tx - x) - 90;
        g.text(tr.title, { css::Base, Weight::SemiBold }, tx, y + 7, pal.text, 1, Align::Left, int(tw));
        g.text(trackSub(t), { css::Sm }, tx, y + 27, pal.text3, 1, Align::Left, int(tw));
        // the line: “…”, each word searched for lit where it starts a word
        std::vector<int> off;
        const std::string fold = foldText(h.line, &off);
        std::vector<char> lit(h.line.size(), 0);
        for (const auto& wd : words)
            for (size_t p0 = fold.find(wd); p0 != std::string::npos; p0 = fold.find(wd, p0 + 1)) {
                if (p0 > 0 && (isalnum((unsigned char)fold[p0 - 1]) || (unsigned char)fold[p0 - 1] >= 0x80 || fold[p0 - 1] == '\'')) continue;
                const int a = off[p0], b = p0 + wd.size() < off.size() ? off[p0 + wd.size()] : int(h.line.size());
                for (int k = a; k < b; k++) lit[size_t(k)] = 1;
            }
        const Font f{ css::Sm }, fb{ css::Sm, Weight::SemiBold };
        float px = tx;
        const float right = tx + tw;
        px += g.text("“", f, px, y + 46, pal.text2);
        for (size_t k = 0; k < h.line.size() && px < right - 20;) {
            size_t e = k;
            while (e < h.line.size() && lit[e] == lit[k]) e++;
            const std::string piece = h.line.substr(k, e - k);
            px += g.text(piece, lit[k] ? fb : f, px, y + 46, lit[k] ? pal.accentInk : pal.text2, 1, Align::Left, int(right - px - 12));
            k = e;
        }
        g.text("”", f, px, y + 46, pal.text2);
        g.textNumC(fmtTime(tr.duration), { css::Sm }, x + w - 42, y + rh / 2 - 2, pal.text3, 1, Align::Right);
        y += rh;
    }
    return y + 24 - top;
}

/** A lyrics hit pressed: the songs found by their lyrics play, from this one. A synced line: a card asks where from (the user,
 *  2026-11-05: "a card asking the user both, staying all consistent with other card we already have"); plain lyrics: the start. */
void App::playLyricHit(size_t i) {
    if (i >= searchLyrics_.size()) return;
    std::vector<int> tracks;
    int at = 0;
    for (size_t k = 0; k < searchLyrics_.size(); k++) {
        auto ix = pathIndex.find(searchLyrics_[k].path);
        if (ix == pathIndex.end()) continue;
        if (k == i) at = int(tracks.size());
        tracks.push_back(ix->second);
    }
    if (tracks.empty()) return;
    const std::string name = "Lyrics: " + route.query;
    const LyricIndex::Hit h = searchLyrics_[i];
    auto play = [this, tracks, at, name, h](bool fromLine) {
        playTracks(tracks, at, name);
        if (fromLine) { lyrSeekPath_ = h.path; lyrSeekAt_ = h.at; lyrSeekUntil_ = time + 10; }
    };
    if (h.at < 0) { play(false); return; }
    const Track& tr = lib.tracks[size_t(tracks[size_t(at)])];
    confirm(ICON_QUOTE, "In the lyrics", tr.title, tr.artist, "Play from this line?", "“" + h.line + "” · " + fmtTime(h.at), "From the line",
            [play] { play(true); });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_PLAY;
    sheet.askAlt = "From the start";
    sheet.askAltYes = [play] { play(false); };
}
