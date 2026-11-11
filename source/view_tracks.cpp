/*
 * Gatefold NX · view_tracks.cpp
 * The four ways Gatefold 1.4.2 shows a list of tracks, shared by the playlists, the queue,
 * Folders and Library → Tracks: grouped by album (.vhead over .vrow), with covers (.vrow.wc),
 * as tiles (.vtile) and as a plain list (.vrow). The switch between them is the four-icon
 * control (i-v-albums, i-v-covers, i-v-grid, i-v-list). Also the 4-cover mosaic of playlists
 * and folders (.pl-cover.quad).
 *
 * Gatefold puts a darker panel behind these lists; here they sit on the background itself,
 * as the rest of the Switch build does.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"

namespace {

// .vrow, a little taller than the PC's 36 for fingers
// (a phone upright: the plain row holds two lines, the title over the artist, and at 40 they ran into each other; the user,
// 2026-11-11: "super close one song to another compared to landscape". 52 there, as before everywhere else)
#define RowH (css::W < 600 ? 52.f : 40.f)
constexpr float CovRowH = 60;     // .vrow.wc: --rowc 56, + 4
constexpr float GroupH = 76;      // .vhead: --head 72, + 4
constexpr float ColHeadH = 38;    // .list-head 30 + 8 below

/** The grid's columns: tiles about 180 px wide, at least two. */
struct Tiles { int cols; float w, cover, h; };
Tiles tilesFor(float w) {
    const float inner = w - 32;
    const int cols = std::max(2, int(inner / 180));
    const float tw = inner / cols, cover = tw - 16;
    return { cols, tw, cover, 8 + cover + 10 + 20 + 18 + 14 };
}

}  // namespace

// ============================================================ layout

std::vector<App::ListEntry> App::listLayout(const std::vector<int>& tracks, float w, ListMode mode, float joinGap) const {
    std::vector<ListEntry> out;
    float y = 0;
    if (mode == ListMode::Grid) {
        const Tiles t = tilesFor(w);
        for (int i = 0; i < int(tracks.size()); i += t.cols) { out.push_back({ ListEntry::TileRow, i, y, t.h }); y += t.h; }
        return out;
    }
    out.push_back({ ListEntry::ColumnHead, -1, y, ColHeadH });
    y += ColHeadH;
    int lastAlbum = -2;
    for (int i = 0; i < int(tracks.size()); i++) {
        if (mode == ListMode::Albums) {
            // a header whenever the album changes, as Gatefold groups consecutive tracks
            const int a = lib.tracks[tracks[i]].albumIdx;
            if (a != lastAlbum || a < 0) { out.push_back({ ListEntry::GroupHead, i, y, GroupH }); y += GroupH; }
            lastAlbum = a;
        }
        const float h = mode == ListMode::Covers ? CovRowH : RowH;
        out.push_back({ ListEntry::Row, i, y, h });
        y += h;
        if (i + 1 < int(tracks.size())) y += joinGap;
    }
    return out;
}

float App::listRowY(const std::vector<ListEntry>& layout, int index) const {
    for (const auto& e : layout) {
        if (e.kind == ListEntry::Row && e.index == index) return e.y;
        if (e.kind == ListEntry::TileRow && index >= e.index && index < e.index + 64) {
            // the tile row holding it (rows hold cols tiles; the next row starts further on)
            const auto next = &e + 1;
            if (next == layout.data() + layout.size() || index < next->index) return e.y;
        }
    }
    return 0;
}

// ============================================================ pieces

/** The four-icon view switch (.pl-modes), right-aligned at rx; returns its width. */
float App::viewSwitch(const char* key, float rx, float cy, int& mode, unsigned hidden) {
    static const char* icons[4] = { ICON_V_ALBUMS, ICON_V_COVERS, ICON_V_GRID, ICON_V_LIST };
    int shown = 0;
    for (int i = 0; i < 4; i++) shown += !(hidden & (1u << i));
    const float bw = 38, h = 36, w = 6 + bw * shown, x0 = rx - w;
    g.roundRect(x0, cy - h / 2, w, h, 12, pal.text, 0.05f);
    g.roundStroke(x0, cy - h / 2, w, h, 12, 1, pal.text, 0.09f);
    // From the biggest to the smallest (the user's ask): tiles, by album, with covers, the plain list. The numbers stay
    // what they were (settings.txt), only the order shown changes.
    static const int order[4] = { int(ListMode::Grid), int(ListMode::Albums), int(ListMode::Covers), int(ListMode::List) };
    int slot = 0;
    for (int i : order) {
        if (hidden & (1u << i)) continue;   // a view that means nothing here (Playlists and Folders have no "by album")
        const float bx = x0 + 3 + slot++ * bw;
        const Id id = uid(key, i);
#ifdef GF_ANDROID
        // a finger: the whole height of the bar and 4 more above and below, 44, as the segmented choices (drawn the same)
        const SDL_FRect hitR{ bx, cy - h / 2 - 4, bw, h + 8 };
#else
        const SDL_FRect hitR{ bx, cy - h / 2 + 3, bw, h - 6 };
#endif
        if (ui.item(id, hitR, 9)) { mode = i; persistSoon(0.5f); }
        const bool on = mode == i, hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (on) g.roundRect(bx, cy - h / 2 + 3, bw, h - 6, 9, pal.text, 0.13f);
        else if (hot) g.roundRect(bx, cy - h / 2 + 3, bw, h - 6, 9, pal.text, 0.065f);
        g.icon(icons[i], bx + bw / 2, cy, 17, on ? pal.text : pal.text2);
    }
    return w;
}

/**
 * .pl-cover.quad: the covers of the first four albums, two by two; one cover when there are
 * fewer than four different ones.
 */
void App::coverQuad(const std::vector<int>& tracks, float x, float y, float size, float rad, float alpha) {
    std::vector<int> picks;
    std::vector<int> seen;
    for (int t : tracks) {
        const int a = lib.tracks[t].albumIdx;
        const int key = a >= 0 ? a : -1 - t;   // tracks without an album each count as their own
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        picks.push_back(t);
        if (picks.size() == 4) break;
        if (seen.size() > 64) break;
    }
    if (picks.size() < 4) {
        if (!picks.empty()) coverBox(picks[0], x, y, size, rad, alpha);
        else { g.roundRect(x, y, size, size, rad, pal.surface2, alpha); g.icon(ICON_LIST, x + size / 2, y + size / 2, size * 0.36f, pal.text3, alpha); }
        return;
    }
    const float half = size / 2;
    // Each quarter rounds only its outer corner, and reaches half a pixel over the middle so no
    // seam shows: the four read as one cover.
    for (int i = 0; i < 4; i++) {
        const int col = i % 2, row = i / 2;
        const float qx = x + col * half - (col ? 0.5f : 0), qy = y + row * half - (row ? 0.5f : 0);
        const float r4[4] = { col == 1 && row == 0 ? rad : 0, col == 1 && row == 1 ? rad : 0, col == 0 && row == 1 ? rad : 0, col == 0 && row == 0 ? rad : 0 };
        AlbumArt& a = meta.art(picks[i]);
        if (a.state == AlbumArt::Ready) g.imageCorners(a.tex, qx, qy, half + 0.5f, half + 0.5f, r4, alpha);
        else g.roundRectCorners(qx, qy, half + 0.5f, half + 0.5f, r4, pal.surface2, alpha);
    }
    g.roundStroke(x, y, size, size, rad, 1, pal.text, 0.09f * alpha);
}

/** Column labels for the row modes (.list-head). */
void App::listColumns(float x, float y, float w, ListMode mode, bool extended) {
    if (w < kNarrowList) { g.rect(x, y + 29, w, 1, pal.text, 0.09f); return; }   // a narrow list (a phone upright): no columns to name
    const Font f{ css::Xs, Weight::SemiBold };
    const float pad = 30, gap = 14;
    float cx = x + pad;
    const float cy = y + 15;
    const float inner = w - pad * 2;
    g.textSpaced("#", f, cx + 44, cy, 0.08f, pal.text3, 1, Align::Right);
    cx += 44 + gap;
    if (mode == ListMode::Covers) {
        // .vrow.wc: # | cover + title / artist | album | (year | genre) | time
        const float fixed = 44 + 64 + gap * (extended ? 5 : 3) + (extended ? 64 : 0);
        const float fr = (inner - fixed) / (extended ? 4.f : 3.f);
        g.textSpaced("TITLE", f, cx, cy, 0.08f, pal.text3);
        cx += fr * 2 + gap;
        g.textSpaced("ALBUM", f, cx, cy, 0.08f, pal.text3);
        cx += fr + gap;
        if (extended) {
            g.textSpaced("YEAR", f, cx, cy, 0.08f, pal.text3);
            cx += 64 + gap;
            g.textSpaced("GENRE", f, cx, cy, 0.08f, pal.text3);
            cx += fr + gap;
        }
    } else {
        const float fixed = 44 + 64 + gap * 4;
        const float fr = (inner - fixed) / 3.6f;
        g.textSpaced("TITLE", f, cx, cy, 0.08f, pal.text3);
        cx += fr * 1.6f + gap;
        g.textSpaced("ARTIST", f, cx, cy, 0.08f, pal.text3);
        cx += fr + gap;
        g.textSpaced("ALBUM", f, cx, cy, 0.08f, pal.text3);
        cx += fr + gap;
    }
    g.textSpaced("TIME", f, x + w - pad, cy, 0.08f, pal.text3, 1, Align::Right);
    g.rect(x, y + 29, w, 1, pal.text, 0.09f);
}

/** .vrow.wc: number, cover, title over artist, album (and year, genre), length. */
bool App::coverRow(Id id, int track, int number, float x, float y, float w, float h, bool extended) {
    const Track& t = lib.tracks[track];
    meta.wantTags(track);
    const bool hit = ui.item(id, { x, y, w, h - 4 }, 10);
    if (ui.focused(id)) hoverTrack = track;
    const bool playing = now.track == track && player.loaded();
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    if (playing) g.roundRect(x, y, w, h - 4, 10, pal.accent, 0.09f);
    if (hot) g.roundRect(x, y, w, h - 4, 10, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    if (w < kNarrowList) {   // narrow (a phone upright): the cover, the title over artist · album, the length
        const float cy = y + (h - 4) / 2, cov = h - 4 - 16, tx = x + 12 + cov + 12, tw = w - (tx - x) - 64;
        coverBox(track, x + 12, cy - cov / 2, cov, 7);
        if (playing) { g.roundRect(x + 12, cy - cov / 2, cov, cov, 7, { 0, 0, 0 }, 0.38f); eqGlyph(x + 12 + cov / 2 - 9, cy, { 1, 1, 1 }); }
        g.text(t.title, { css::Base, playing ? Weight::SemiBold : Weight::Regular }, tx, cy - 20, playing ? pal.accentInk : pal.text, 1, Align::Left, int(tw));
        g.text(t.artist + (t.album.empty() ? "" : " · " + t.album), { css::Sm }, tx, cy + 2, pal.text3, 1, Align::Left, int(tw));
        if (!rowBare_) g.textNumC(t.duration > 0 ? fmtTime(t.duration) : "", { css::Sm }, x + w - 16, cy, pal.text3, 1, Align::Right);
        return hit;
    }
    const float pad = 16, gap = 14, inner = w - pad * 2;
    const float fixed = 44 + 64 + gap * (extended ? 5 : 3) + (extended ? 64 : 0);
    const float fr = (inner - fixed) / (extended ? 4.f : 3.f);
    const float cy = y + (h - 4) / 2;
    float cx = x + pad;
    if (playing) eqGlyph(cx + 44 - 13, cy, pal.accentInk);
    else g.textNumC(std::to_string(number), { css::Sm }, cx + 44, cy, pal.text3, 1, Align::Right);
    cx += 44 + gap;
    const float cov = h - 4 - 16;
    coverBox(track, cx, cy - cov / 2, cov, 7);
    const float tx = cx + cov + 12, tw = fr * 2 - cov - 12;
    g.text(t.title, { css::Base, playing ? Weight::SemiBold : Weight::Regular }, tx, cy - 20, playing ? pal.accentInk : pal.text, 1, Align::Left, int(tw));
    g.text(t.artist, { css::Sm }, tx, cy + 2, pal.text3, 1, Align::Left, int(tw));
    cx += fr * 2 + gap;
    g.textC(t.album, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
    cx += fr + gap;
    if (extended) {
        g.textC(t.date.substr(0, 4), { css::Base }, cx, cy, pal.text2, 1, Align::Left, 64);
        cx += 64 + gap;
        g.textC(t.genre, { css::Base }, cx, cy, pal.text2, 1, Align::Left, int(fr));
    }
    if (!rowBare_) g.textNumC(t.duration > 0 ? fmtTime(t.duration) : "", { css::Sm }, x + w - pad, cy, pal.text3, 1, Align::Right);
    return hit;
}

/** .vtile: the cover, title and artist; the play glyph on focus, an equaliser on the one playing. */
bool App::trackTile(Id id, int track, float x, float y, float w, float cover) {
    const Track& t = lib.tracks[track];
    meta.wantTags(track);
    const float h = 8 + cover + 10 + 20 + 18 + 6;
    const bool hit = ui.item(id, { x, y, w, h }, 14);
    if (ui.focused(id)) hoverTrack = track;
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    const bool playing = now.track == track && player.loaded();
    if (hot) g.roundRect(x, y, w, h, 14, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
    const float cx = x + 8, cy = y + 8;
    coverBox(track, cx, cy, cover, 12);
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

/** .vhead: the album's cover, title, "artist · year · n tracks", and Album to play it whole. */
void App::groupHead(const char* key, int index, int track, float x, float y, float w) {
    const Track& t = lib.tracks[track];
    const float cov = GroupH - 4 - 20, cy = y + (GroupH - 4) / 2 + 2;
    coverBox(track, x + 16, cy - cov / 2, cov, 8);
    const float tx = x + 16 + cov + 14;
    std::string album = t.album.empty() ? baseName(t.folder) : t.album;
    std::string meta3 = t.albumArtist.empty() ? t.artist : t.albumArtist;
    if (t.date.size() >= 4) meta3 += " · " + t.date.substr(0, 4);
    int n = 0;
    if (t.albumIdx >= 0) n = int(lib.albums[t.albumIdx].tracks.size());
    if (n) meta3 += " · " + std::to_string(n) + (n == 1 ? " track" : " tracks");
    const float bw = g.measure("Album", { css::Sm, Weight::SemiBold }) + 32 + 24;
    g.text(album, { css::Md, Weight::Bold }, tx, cy - 22, pal.text, 1, Align::Left, int(w - (tx - x) - bw - 30));
    g.text(meta3, { css::Sm }, tx, cy + 2, pal.text3, 1, Align::Left, int(w - (tx - x) - bw - 30));
    if (t.albumIdx >= 0) {
        bool clicked = false;
        button(uid(std::string(key) + ".grp", index), ICON_PLAY, "Album", x + w - 16 - bw, cy - 17, false, &clicked);
        if (clicked) playTracks(lib.albums[t.albumIdx].tracks, 0, album);
    }
}

// ============================================================ the body

/**
 * Draws tracks from top (in the scroll area's coordinates, already offset) in the given mode,
 * only what lies in [clipY, clipY + clipH). play(i) runs when row i is chosen. Returns the height.
 */
float App::drawTrackBody(const char* key, const std::vector<int>& tracks, float x, float top, float w, float clipY, float clipH, ListMode mode,
                         bool extended, const std::function<void(int)>& play, float joinGap) {
    const std::vector<ListEntry> layout = listLayout(tracks, w, mode, joinGap);
    const Tiles tl = tilesFor(w);
    bodyFocus = -1;
    bodyCols = mode == ListMode::Grid ? tl.cols : 1;
    // A row picked up to be moved: lifted, with the accent around it, so it reads as held.
    auto lifted = [&](int i, float rx, float ry, float rw, float rh, float rad) {
        if (i != bodyGrab) return;
        g.roundRect(rx, ry, rw, rh, rad, pal.accent, 0.14f);
        g.roundStroke(rx, ry, rw, rh, rad, 2, pal.accent, 0.95f);
    };
    // In sort mode (by touch: App::TouchSort; the caller gives the list's scroll area in bodySortArea): each row has the sidebar
    // sorting's handle in the place of its length, a tile is dragged whole; each is drawn where sortAt puts it (its place, sliding
    // to it, or under the finger) and the one held last, lifted off the page. No album heads then (the caller shows With covers).
    ScrollArea* const sortArea = bodySortArea;
    const bool sort = sortArea != nullptr;
    const std::string rk = std::string(key) + ".r", sk = std::string(key) + ".sort";
    const int n = int(tracks.size());
    const bool covers = mode == ListMode::Covers, grid = mode == ListMode::Grid;
    const float tileH = 8 + tl.cover + 10 + 20 + 18 + 6, rowPitch = (covers ? CovRowH : RowH) + joinGap;
    // where item i belongs (its whole place: a row with the gap under it, a tile's cell)
    auto slot = [&](int i) -> SDL_FRect {
        if (grid) return { x + 16 + (i % tl.cols) * tl.w, top + (i / tl.cols) * tl.h, tl.w, tl.h };
        return { x + 14, top + ColHeadH + i * rowPitch, w - 28, rowPitch };
    };
    if (sort) sortRun(n, 0, n - 1, *sortArea, slot, grid);
    const int floats = sort ? (tsort_.held >= 0 ? tsort_.held : tsort_.settle) : -1;
    auto item = [&](int i, float px, float py, bool fl) {
        const Id id = uid(rk, i);
        if (grid) {
            if (fl) sortFloatBox(px, py, tl.w, tileH, 14);
            lifted(i, px, py, tl.w, tileH, 14);
            const bool hit = trackTile(id, tracks[i], px, py, tl.w, tl.cover);
            if (sort) sortGrip(sk.c_str(), i, { px, py, tl.w, tileH }, { px, py, tl.w, tileH });   // (over the tile: the whole of it drags)
            else if (hit) play(i);
        } else {
            const float eh = covers ? CovRowH : RowH, rh = covers ? eh - 4 : eh, rad = covers ? 10.f : 8.f;
            if (fl) sortFloatBox(px, py, w - 28, rh, rad);
            lifted(i, px, py, w - 28, rh, rad);
            rowBare_ = sort;
            const bool hit = covers ? coverRow(id, tracks[i], i + 1, px, py, w - 28, eh, extended) : trackRow(id, tracks[i], i + 1, px, py, w - 28, eh, true);
            rowBare_ = false;
            if (sort) sortHandle(sk.c_str(), i, px + w - 28 - 16, py + rh / 2, { px, py, w - 28, rh });   // (a tap on the row plays nothing meanwhile)
            else if (hit) play(i);
        }
        if (ui.focused(id)) bodyFocus = i;
    };
    float height = 0;
    for (const auto& e : layout) {
        height = std::max(height, e.y + e.h);
        const float ey = top + e.y;
        if (ey + e.h < clipY - 80 || ey > clipY + clipH + 80) continue;
        switch (e.kind) {
        case ListEntry::ColumnHead: listColumns(x, ey, w, mode, extended); break;
        case ListEntry::GroupHead: groupHead(key, e.index, tracks[e.index], x + 14, ey, w - 28); break;
        case ListEntry::Row: {
            if (e.index == floats) break;
            const SDL_FPoint at = sort ? sortAt(e.index, slot(e.index)) : SDL_FPoint{ x + 14, ey };
            item(e.index, at.x, at.y, false);
            break;
        }
        case ListEntry::TileRow:
            for (int c = 0; c < tl.cols && e.index + c < n; c++) {
                const int i = e.index + c;
                if (i == floats) continue;
                const SDL_FPoint at = sort ? sortAt(i, slot(i)) : SDL_FPoint{ x + 16 + c * tl.w, ey };
                item(i, at.x, at.y, false);
            }
            break;
        }
    }
    if (floats >= 0 && floats < n) {   // lifted off the page, over the others (its shadow not cut at the list's sides)
        const SDL_FPoint at = sortAt(floats, slot(floats));
        g.clip(x - 8, clipY, w + 16, clipH);
        item(floats, at.x, at.y, true);
        g.clip(x, clipY, w, clipH);
    }
    return height + 24;
}
