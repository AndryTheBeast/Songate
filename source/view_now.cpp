/*
 * Gatefold NX · view_now.cpp
 * #view-now: cover on the left, .np-info on the right (kicker, title, artist,
 * album, badges, actions, spectrum, tabs, panel), and the full-screen Canvas.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {
float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }
}  // namespace

void App::drawNow(float x, float y, float w, float h) {
    // .np grid: padding clamp(16px, 3vh, 40px) clamp(20px, 4vw, 64px); gap clamp(24px, 4vw, 64px)
    const float padH = clampf(css::W * 0.04f, 20, 64), gap = clampf(css::W * 0.04f, 24, 64);
#ifdef GF_WINDOWED
    const float art = std::max(200.f, std::min({ css::H * 0.52f, css::W * 0.40f, 620.f, w - padH * 2 - gap - 440 }));   // a tall window: the info keeps its room
#else
    const float art = std::min({ css::H * 0.52f, css::W * 0.40f, 620.f });
#endif
#if defined(GF_WINDOWED) && !defined(GF_ANDROID)
    // A wide window (a large screen, an ultrawide one): the cover and its column are as wide as they get (620 and 760), and what
    // room is left over is shared either side of them, not all left on the right
    if (const float need = padH * 2 + art + gap + 760; w > need) {
        x += std::floor((w - need) / 2);
        w = need;
    }
#endif
    const float iw = std::min(760.f, w - padH * 2 - art - gap);
    const bool has = now.track >= 0 || extOn;   // (an outside source: AirPlay)
    // Automix joining (automixFocus): the cover glides from its place to the middle of the screen and grows, frosted with
    // the hint, while the info (and the sidebar and bars: App::draw) fade out; as the join ends it glides back and they return
    const int mode = store.mixAnimNow;   // Settings › Appearance › Automix: Full, Immersive, Simple
    const Focus fc = automixFocus(mode);
    const float m = fc.move;
    // Full: to the middle of the screen, 1.35× (the interface steps aside: App::draw); Immersive: to the middle of the page,
    // 1.2× (the sidebar and the bars stay; only the info fades); Simple: in place
    const bool imm = mode == 1;
    const float big = imm ? std::min(h - 48.f, art * 1.2f) : std::min(css::H - 140.f, art * 1.35f);
    const float art2 = art + (big - art) * m;
    const float rx = x + padH, ry = y + (h - art) / 2;   // at rest
    const float gx = imm ? x + w / 2 : css::W / 2.f, gy = imm ? y + h / 2 : css::H / 2.f;
    const float ax = rx + (gx - big / 2 - rx) * m, ay = ry + (gy - big / 2 - ry) * m;
    const float ix = x + padH + art + gap + 28 * (1 - fc.ui);   // the info at its own place, drifting right as it fades
    // Simple: the whole page changes from the outgoing song to the next from left to right, the cover too (the user,
    // 2026-10-29): each section across its own width, all on the join's clock (App::pageWipe_)
    struct PageOff { PageWipe& p; ~PageOff() { p.on = false; } } pageOff{ pageWipe_ };
    if (mode == 2 && has && mixOld.on) pageWipe_.on = true;
    // the info column, from the title to Up next or the lyrics, swipes as one (App::wipeGroup_)
    struct GroupOff { WipeGroup& w; ~GroupOff() { w.on = false; } } groupOff{ wipeGroup_ };
    if (pageWipe_.on) wipeGroup_ = { true, ix, ix + iw };
    if (m > 0) {   // drawn last by App::draw, over the bars and outside the page's clip
        glideCover_ = { ax, ay, art2, art2 };
        glideOn_ = true;
    } else {
        drawNowCover(ax, ay, art2);
    }
    if (fc.ui <= 0.001f) return;   // the info fully faded: not drawn at all (App::draw)
    if (fc.ui < 1) g.setFade(fc.ui);
    // the pocket lock: a pill under the cover, centred between it and the player bar (the user, 2026-11-07), the cover at rest
    if (m < 0.01f) pocketPill(rx + art / 2, ry + art + (y + h - (ry + art)) / 2);

    // ---- .np-info, measured first so the panel can take what is left
    // .np-title: Inter 760, clamp(30px, 4.2vw, 52px), line-height 1.04; a title that needs two lines steps down
    // to 42 px, because at 720 p every line it takes is a row of Up next.
    std::string titleText = has ? now.info.title : "Songate";
#ifdef GF_PREVIEW
    if (const char* t = getenv("GF_NP_TITLE"); t && has) titleText = t;   // (the preview: a title to try the fitting with)
#endif
    const TitleFit fit = fitTitle(titleText, iw);   // (a copy: fitTitle's cache may move under the outgoing song's)
    auto badgesOf = [&](const TrackInfo& in) {
        std::vector<std::string> bs;
        if (!in.codec.empty()) bs.push_back(in.codec);
        char b[48];
        if (in.bitsPerSample && in.codec != "MP3") { snprintf(b, sizeof b, "%d-bit", in.bitsPerSample); bs.push_back(b); }
        if (in.sampleRate) { snprintf(b, sizeof b, "%g kHz", in.sampleRate / 1000.0); bs.push_back(b); }
        if (in.bitrateKbps) bs.push_back(std::to_string(in.bitrateKbps) + " kbps");
        return bs;
    };
    std::vector<std::string> badges;
    if (has) badges = badgesOf(now.info);
    // Gatefold's column is sized for a desktop window; on 720 p the fixed rows are tightened and the spectrum
    // shrinks (then folds away) so .np-panel keeps at least four Up next rows and five synced lyric lines.
    const float padT = 16, padB = 10;
    const float titleH = 4 + fit.h + 2;
    auto fixedH = [&](float vizH) {
        return 17 + titleH + 4 + 31 + 2 + 22 + (badges.empty() ? 0 : 10 + 22) + (has ? 12 + 34 : 0) + (vizH > 0 ? 14 + vizH : 0) + 8 + 35 + 6;
    };
    const float avail = h - padT - padB;
    // What the panel must keep: four .up-rows, but for a two-line title three rows and the whole Palette tab
    // (drawPaletteTab's height) are enough, and the bars keep their height rather than shrinking to a sliver.
    const float paletteH = 8 + 26 + 10 + 4 * 30 + 8 + 20;
    const float wantPanel = fit.lines.size() > 1 ? std::max(8 + 3 * 52.f, paletteH) : 8 + 4 * 52;
    // (and where even its smallest leaves Up next too little, a short window: it folds away, the panel keeps its rows)
    float vizRoom = avail - fixedH(0) - 14 - wantPanel;
    // a short screen (a phone on Medium or Largest): the spectrum keeps its row and Up next shows three (the user, 2026-11-05: "the android
    // app no longer display the waveform spectrum in now playing landscape"; "Spectrum stays, 3 rows")
    if (store.showSpectrum && vizRoom < 28) vizRoom = avail - fixedH(0) - 14 - (8 + 3 * 52.f);
    const float vizH = store.showSpectrum && vizRoom >= 22 ? clampf(vizRoom, 22, 46) : 0;
    const float panelH = std::max(96.f, avail - fixedH(vizH));
    float cy = y + padT;

    // .np-kicker
    eqGlyph(ix, cy + 8, pal.accentInk, isPlaying() || (extOn && !srvq_.on));   // (AirPlay plays: the bars move with it)
    const std::string state = srvq_.on ? (isPlaying() ? "Playing from your server" : "Paused") : extOn ? "Playing over AirPlay" : !has ? "Nothing playing" : isPlaying() ? "Playing" : "Paused";
    kicker(state, ix + 13 + 10, cy + 8, pal.accentInk, 0.14f);
    cy += 17 + 4;
    // the title, .np-artist and .np-album; in a Simple join (Settings › Appearance › Automix) the outgoing song's give way
    // to the next one's from left to right, timed to the join (the user, 2026-10-29). The next one's title decides the layout
    auto meta = [&](bool old) {
        const TrackInfo& in = old ? mixOld.info : now.info;
        const TitleFit tfit = old ? fitTitle(in.title, iw) : fit;
        float yy = cy;
        for (const auto& l : tfit.lines) { g.text(l, tfit.font, ix - 2, yy - tfit.font.size * 0.08f, pal.text, 1, Align::Left, int(iw)); yy += tfit.lh; }
        if (!has) return;
        yy = cy + fit.h + 2 + 4;
        std::string al = in.album;
        if (in.date.size() >= 4) al += (al.empty() ? "" : " · ") + in.date.substr(0, 4);
        // too long for the column: they scroll as the player bar's (one clock: a new song starts them again)
        marqueeLine(in.artist, { 24, Weight::SemiBold }, ix, yy, iw, pal.text);
        marqueeLine(al, { css::Md }, ix, yy + 31 + 2, iw, pal.text2);
    };
    if (mode == 2 && has && mixOld.on) {
        // (across the texts themselves, the wider song's: across the whole column a short title changed in a moment)
        float wide = 0;
        for (const TrackInfo* in : { &mixOld.info, &now.info }) {
            const TitleFit wf = fitTitle(in->title, iw);
            for (const auto& l : wf.lines) wide = std::max(wide, float(g.measure(l, wf.font)));
            wide = std::max({ wide, float(g.measure(in->artist, { 24, Weight::SemiBold })), float(g.measure(in->album, { css::Md })) + 60 });
        }
        wiped(ix, ix + std::min(iw, wide), cy, cy + fit.h + 6 + 31 + 2 + 22, meta);
    }
    else meta(false);
    cy += fit.h + 2 + 4 + 31 + 2 + 22;
    // .np-badges: the codec chip is the accent one
    if (!badges.empty()) {
        cy += 10;
        auto badgeRow = [&](bool old) {   // (in a Simple join, the outgoing song's give way to the next one's)
            const std::vector<std::string> bs = old ? badgesOf(mixOld.info) : badges;
            float bx = ix;
            for (size_t i = 0; i < bs.size(); i++) bx += chip(bs[i], bx, cy + 11, i == 0) + 6;
            // The first genre, as a chip that opens its page (a server track's: the server's genre); a server's track, then where from
            const int tr = old ? mixOld.track : now.track;
            const bool server = srvq_.on && !old;
            if (!server && (extOn || tr < 0 || tr >= int(lib.tracks.size()))) return;
            const std::string& gs = server ? now.info.genre : lib.tracks[tr].genre;
            const std::string g1 = gs.substr(0, gs.find(';'));
            if (!g1.empty()) {
                const Font gf{ css::Xs, Weight::SemiBold };
                const float gw = g.measureSpaced(g1, gf, 0.02f) + 16 + 18;
                const Id gid = uid("np.genre");
                if (!old && ui.item(gid, { bx, cy, gw, 22 }, 6)) { if (server) openServer(3, g1, g1); else openGenre(g1); }
                const bool hot = !old && (ui.pressed(gid) || (ui.focused(gid) && ui.showFocusRing()));
                g.roundRect(bx, cy, gw, 22, 6, pal.text, hot ? 0.13f : 0.065f);
                g.roundStroke(bx, cy, gw, 22, 6, 1, pal.text, 0.09f);
                g.icon(ICON_TAG, bx + 13, cy + 11, 12, pal.text2);
                g.textSpaced(g1, gf, bx + 8 + 18, cy + 11, 0.02f, pal.text2);
                bx += gw + 6;
            }
            if (server) {   // where it plays from: the server, as a chip of its own (Library › Server)
                const Font sf{ css::Xs, Weight::SemiBold };
                const float sw = g.measureSpaced(extFrom, sf, 0.02f) + 16 + 18;
                const Id sid = uid("np.server");
                if (ui.item(sid, { bx, cy, sw, 22 }, 6)) { store.libSource = 1; libSourceChanged(); navigate(View::Library); }
                const bool hot = ui.pressed(sid) || (ui.focused(sid) && ui.showFocusRing());
                g.roundRect(bx, cy, sw, 22, 6, pal.text, hot ? 0.13f : 0.065f);
                g.roundStroke(bx, cy, sw, 22, 6, 1, pal.text, 0.09f);
                g.icon(PLI_CLOUD, bx + 13, cy + 11, 13, pal.text2);
                g.textSpaced(extFrom, sf, bx + 8 + 18, cy + 11, 0.02f, pal.text2);
            }
        };
        if (pageWipe_.on) wiped(ix, ix + iw, cy, cy + 22, badgeRow);
        else badgeRow(false);
        cy += 22;
    }
    // .np-actions; an outside source has none of a song here, only where it comes from
    // Album, Artist and Folder with their names where the row fits the column (it always does at 1280 × 720); in a narrower one (a
    // short window, a tablet upright: the row ran past the page's edge) round with the icon only, their names as hints, as a
    // playlist's page does with its actions
    const bool actsRound = [&] {
        const Font bf{ css::Sm, Weight::SemiBold };
        float need = 34;   // (the menu's button)
        for (const char* l : { "Add to Liked", "Album", "Artist", "Folder" }) need += 16 + 24 + g.measure(l, bf) + 16 + 8;
        return need > iw;
    }();
    auto act = [&](const char* key, const char* icon, const char* label, float& bx, float cy) {
        bool c = false;
        if (!actsRound) { bx += button(uid(key), icon, label, bx, cy, false, &c) + 8; return c; }
        const Id id = uid(key);
        hint(id, label);
#ifdef GF_ANDROID
        c = ui.item(id, { bx - 4, cy - 4, 42, 42 }, 21);   // a finger: 42, as the icon buttons (drawn the same)
#else
        c = ui.item(id, { bx, cy, 34, 34 }, 17);
#endif
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        g.roundRect(bx, cy, 34, 34, 17, pal.text, hot ? 0.11f : 0.065f);
        g.roundStroke(bx, cy, 34, 34, 17, 1, pal.text, 0.15f);
        g.icon(icon, bx + 17, cy + 17, 16, pal.text);
        bx += 34 + 8;
        return c;
    };
    if (srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size())) {   // a server's track: as one of yours (the user, 2026-11-06)
        cy += 12;
        const Subsonic::Song& so = srvq_.songs[size_t(srvq_.pos)];
        const bool liked = currentLiked();
        float bx = ix;
        bool clicked = false;
        bx += button(uid("np.like"), liked ? ICON_HEART_F : ICON_HEART, liked ? "Liked" : "Add to Liked", bx, cy, false, &clicked, liked, false, DefaultFocus) + 8;
        if (clicked) toggleCurrentLike();
        if (!so.albumId.empty()) {
            if (act("np.album", ICON_GRID, "Album", bx, cy)) openServer(2, so.albumId, so.album);
        }
        if (!so.artistId.empty()) {
            if (act("np.artist", ICON_MIC, "Artist", bx, cy)) openServer(1, so.artistId, so.artist);
        }
        if (!so.parent.empty()) {
            if (act("np.folder", ICON_FOLDER, "Folder", bx, cy)) openServer(4, so.parent, so.album);
        }
        {   // .np-actions .more: the server track's menu (also + on the controller)
            const Id mid = uid("np.more");
            if (ui.item(mid, { bx, cy, 34, 34 }, 17)) openServerMenu(so);
            const bool hot = ui.pressed(mid) || (ui.focused(mid) && ui.showFocusRing());
            g.roundRect(bx, cy, 34, 34, 17, pal.text, hot ? 0.11f : 0.065f);
            g.roundStroke(bx, cy, 34, 34, 17, 1, pal.text, 0.15f);
            g.icon(ICON_MORE, bx + 17, cy + 17, 16, pal.text);
        }
        cy += 34;
    } else if (extOn) {
        cy += 12;
        float lx = ix;
        g.icon(ICON_LINK, lx + 9, cy + 17, 16, pal.text2);
        g.textC(srvq_.on ? "From " + extFrom + (srvq_.name.empty() ? "" : " · " + srvq_.name) : "From " + extFrom + " · play, pause and skip there", { css::Sm }, lx + 26,
                cy + 17, pal.text2, 1, Align::Left, int(iw - (lx - ix) - 26));
        cy += 34;
    } else if (has) {
        cy += 12;
        const Track& t = lib.tracks[now.track];
        const bool liked = store.isLiked(t.path);
        float bx = ix;
        bool clicked = false;
        bx += button(uid("np.like"), liked ? ICON_HEART_F : ICON_HEART, liked ? "Liked" : "Add to Liked", bx, cy, false, &clicked, liked, false,
                     DefaultFocus) + 8;
        if (clicked) { store.toggleLiked(t.path); store.save(); toast(liked ? "Removed from Liked" : "Added to Liked"); }
        if (t.albumIdx >= 0) {
            if (act("np.album", ICON_GRID, "Album", bx, cy)) goToAlbum(t.albumIdx);
        }
        if (act("np.artist", ICON_MIC, "Artist", bx, cy)) openArtist(t.artist);
        if (act("np.folder", ICON_FOLDER, "Folder", bx, cy) && tabFor(Store::TabFolders, "folder")) navigate(View::Folders, lib.folderByPath(t.folder));
        {
            // .np-actions .more: the track menu (also + on the controller)
            const Id mid = uid("np.more");
            if (ui.item(mid, { bx, cy, 34, 34 }, 17)) openTrackMenu(now.track);
            const bool hot = ui.pressed(mid) || (ui.focused(mid) && ui.showFocusRing());
            g.roundRect(bx, cy, 34, 34, 17, pal.text, hot ? 0.11f : 0.065f);
            g.roundStroke(bx, cy, 34, 34, 17, 1, pal.text, 0.15f);
            g.icon(ICON_MORE, bx + 17, cy + 17, 16, pal.text);
        }
        cy += 34;
    }
    // #viz
    const float vizTop = cy;   // (the Lyrics tab's swipe light stops above it)
    if (vizH > 0) {
        cy += 14;
        drawSpectrum(ix, cy, iw, vizH);
        cy += vizH;
    }
    // .np-tabs
    cy += 8;
    static const char* tabs[] = { "Up next", "Lyrics", "Palette" };
    float tx = ix;
    const Font tabF{ css::Sm, Weight::SemiBold };
    for (int i = 0; i < 3; i++) {
        const float tw = g.measure(tabs[i], tabF) + 24;
        const Id id = uid("np.tab", i);
        if (ui.item(id, { tx, cy, tw, 35 }, 6)) { npTab = i; area("np.panel").target = area("np.panel").y = 0; }
        const bool on = npTab == i, hot = ui.focused(id) && ui.showFocusRing();
        g.textC(tabs[i], tabF, tx + 12, cy + 17, on || hot ? pal.text : pal.text3);
        if (on) g.roundRect(tx + 12, cy + 34, tw - 24, 2, 1, pal.accent);
        tx += tw + 4;
    }
    g.rect(ix, cy + 35, iw, 1, pal.text, 0.09f);
    cy += 35 + 6;

    // The next track's title, artist and album in these fonts, rendered ahead a couple a frame (Gfx::warm),
    // so a track change does not begin with a frame spent rendering them.
    if (has && !extOn) {
        const std::vector<int> nx = upNext(1);
        if (!nx.empty() && nx[0] != now.track) {
            const Track& n = lib.tracks[nx[0]];
            const TitleFit nf = fitTitle(n.title, iw);
            for (const auto& l : nf.lines) g.warm(l, nf.font, int(iw));
            g.warm(n.artist, { 24, Weight::SemiBold }, int(iw));
            std::string al = n.album;
            if (n.date.size() >= 4) al += (al.empty() ? "" : " · ") + n.date.substr(0, 4);
            g.warm(al, { css::Md }, int(iw));
        }
    }

    // .np-panel
    const float ph = std::min(panelH, y + h - padB - cy);
    if (npTab == 0) drawUpNext(ix, cy, iw, ph);
    else if (npTab == 1) drawLyrics(ix, cy, iw, ph);
    else drawPaletteTab(ix, cy, iw, ph);
    // the column's swipe light: down to the panel's foot, but on the Lyrics tab only to above the waveform (its lyrics fade
    // behind the hint instead: the user, 2026-10-29)
    groupMagic(y + padT, npTab == 1 ? vizTop : cy + ph);
    g.setFade(1);
}

std::string App::titleTrim(const std::string& text, Font f, float w) {
    static std::vector<std::pair<std::string, std::string>> cache;   // (a few: each layout's, the outgoing song's, the next one's)
    const std::string key = std::to_string(f.size) + "|" + std::to_string(int(f.weight)) + "|" + std::to_string(int(w)) + "|" + text;
    for (const auto& c : cache) if (c.first == key) return c.second;
    if (cache.size() >= 16) cache.erase(cache.begin());
    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '-')) s.pop_back();
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        return s;
    };
    std::string main = text;
    if (g.wrap(text, f, int(w), 2).size() > 1) {   // the part in brackets at its end (feat., from the series…, Remastered…), or after " - ": left out
        const char close = text.empty() ? 0 : text.back();
        if (close == ')' || close == ']') {
            const char open = close == ')' ? '(' : '[';
            int depth = 0;
            for (size_t i = text.size(); i-- > 0;) {
                if (text[i] == close) depth++;
                else if (text[i] == open && --depth == 0) {
                    const std::string head = trim(text.substr(0, i)), inner = trim(text.substr(i + 1, text.size() - i - 2));
                    if (!head.empty() && !inner.empty()) main = head;
                    break;
                }
            }
        } else if (const size_t d = text.rfind(" - "); d != std::string::npos && d > 0) {
            const std::string head = trim(text.substr(0, d)), tail = trim(text.substr(d + 3));
            if (!head.empty() && !tail.empty()) main = head;
        }
    }
    cache.push_back({ key, main });
    return main;
}

const App::TitleFit& App::fitTitle(const std::string& text, float w) {
    // kept: the current song's, the outgoing one's and the next one's, measured once each, not every frame
    static std::vector<std::pair<std::string, TitleFit>> cache;
    const std::string key = std::to_string(int(w)) + "|" + text;
    for (const auto& c : cache) if (c.first == key) return c.second;
    if (cache.size() >= 8) cache.erase(cache.begin());
    TitleFit r;
    auto fits = [&](const std::string& s, Font f, int n) { return int(g.wrap(s, f, int(w), n + 1).size()) <= n; };
    const std::string main = titleTrim(text, r.font, w);
    // one line at its full size; else two at 42 (at 720 p every line it takes is a row of Up next), cut with … past that
    r.lines = { main };
    if (!fits(main, r.font, 1)) { r.font.size = 42; r.lines = g.wrap(main, r.font, int(w), 2); }
    r.lh = std::round(r.font.size * 1.04f);
    r.h = r.lines.size() * r.lh;
    cache.push_back({ key, r });
    return cache.back().second;
}

// ------------------------------------------------------------------ the pocket lock (the Switch)

bool App::pocketAvailable() {
#ifdef __SWITCH__
    static int can = -1;   // asked once: the pads open, or not (never each frame)
    if (can < 0) { bool up, down; can = plat::volumeKeys(up, down) ? 1 : 0; }
    return can == 1;   // (its unlock needs them read)
#elif defined(GF_PREVIEW)
    return getenv("GF_POCKET") != nullptr;   // (the preview: to see the pill; there is no unlock)
#else
    return false;
#endif
}

/** Now playing's lock: a pill, "Lock" and its padlock, centred on (cx, cy) (Settings › Controls › Pocket lock shows or hides it). */
void App::pocketPill(float cx, float cy) {
    if (!store.pocketButton || !pocketAvailable()) return;
    const Font f{ css::Sm, Weight::SemiBold };
    const float w = g.measure("Lock", f) + 32 + 24;
    bool clicked = false;
    const Id id = uid("np.pocket");
    hint(id, "The screen off, buttons and touch ignored; " + pocketHowWords() + " to unlock");
    button(id, ICON_LOCK, "Lock", cx - w / 2, cy - 17, false, &clicked, false, false, NoDefault);   // (the ring opens on Add to Liked, as before the pill)
    if (clicked) pocketLock(true);
}

std::string App::pocketHowWords() const {
    const int k = pocketLocked_ ? pocketKeyNow_ : store.pocketKey;
    if (k == 3) return "press Capture twice";
    const char* key = k == 0 ? "Volume −" : k == 1 ? "Volume +" : "a volume button";
    return store.pocketHow == 0 ? std::string("hold ") + key + " " + std::to_string(store.pocketHold) + " s" : std::string("press ") + key + " twice";
}

void App::pocketLock(bool on) {
    if (on == pocketLocked_) return;
    pocketLocked_ = on;
    pocketKeyAt_ = -1;
    pocketFirstAt_ = -10;
    pocketKeyNow_ = store.pocketKey;
    // Capture: its short presses taken while locked (no screenshots); refused, the volume buttons unlock this time (never stuck)
    if (on && pocketKeyNow_ == 3 && !plat::takeCapture(true)) pocketKeyNow_ = 2;
    if (!on) plat::takeCapture(false);
    bool up = false, down = false;
    plat::volumeKeys(up, down);
    pocketKeyWas_ = pocketKeyNow_ == 0 ? down : pocketKeyNow_ == 1 ? up : pocketKeyNow_ == 2 ? (up || down) : false;   // (a key already down starts nothing)
    if (on) {
        ui.touchUp(-10000, -10000);   // (the finger on the lock lifts nowhere: no touch is left half way)
        pocketHintUntil_ = time + 2.5;   // the screen says how to unlock, then goes off
        pocketLightOff_ = false;
        if (!plat::outputVolume(pocketVolIdle_)) pocketVolIdle_ = -1;
        const bool homeHeld = plat::blockHome(true);   // (HOME does nothing while locked; run from the album it stays the system's)
        const int pads = store.pocketPads ? plat::disconnectControllers() : 0;
        logLine(std::string(homeHeld ? "Pocket lock: on, HOME held back" : "Pocket lock: on (HOME stays the system's)") +
                (pads > 0 ? ", " + std::to_string(pads) + " controller(s) off" : "") +
                (store.pocketKey == 3 ? (pocketKeyNow_ == 3 ? ", Capture unlocks" : ", Capture refused: the volume buttons unlock") : ""));
    } else {
        if (pocketLightOff_) plat::backlightOff(false);
        pocketLightOff_ = false;
        plat::blockHome(false);
        logLine("Pocket lock: off");
        toast("Unlocked");
    }
}

/** Every frame while locked (input() hands it nothing else): the unlock on the volume buttons; a press lights the hint. */
void App::pocketTick(bool pressed) {
    if (!plat::inFocus()) { pocketLock(false); return; }   // HOME, or another applet: never left dark
    bool up = false, down = false;
    if (!plat::volumeKeys(up, down)) { pocketLock(false); return; }
    if (pocketKeyNow_ == 3) {   // Capture: pressed twice within 0.7 s (its short presses are all the system hands over)
        const int n = plat::capturePresses();
        if (n > 1 || (n == 1 && time - pocketFirstAt_ <= 0.7)) { pocketLock(false); return; }
        if (n == 1) pocketFirstAt_ = time;
        pressed = pressed || n > 0;
    }
    const bool key = pocketKeyNow_ == 0 ? down : pocketKeyNow_ == 1 ? up : pocketKeyNow_ == 2 ? (up || down) : false;
    const bool edge = key && !pocketKeyWas_, released = !key && pocketKeyWas_;
    pocketKeyWas_ = key;
    // The volume (the system changes it with these keys; the user, 2026-11-07): set to hold, a quick press still changes it and a
    // hold does not; set to press twice, a hold still changes it and the quick presses do not. What the key did is undone once
    // it is known what it was (a press past 0.3 s is a hold)
    constexpr double kTap = 0.3;
    // the volume before the press: the one read while the key was up (by the frame the key reads down, the system has stepped it)
    if (edge) { pocketPressAt_ = time; pocketVol0_ = pocketVolIdle_; if (pocketVol0_ < 0 && !plat::outputVolume(pocketVol0_)) pocketVol0_ = -1; }
    if (pocketVol0_ >= 0 && pocketPressAt_ >= 0) {
        const double held = time - pocketPressAt_;
        int v = -1;
        const bool keep = store.pocketHow == 0 ? (key && held >= kTap)                  // holding: kept where it began
                                               : ((key && held < kTap) || (released && held < kTap));   // tapping: undone
        if (keep && plat::outputVolume(v) && v != pocketVol0_) plat::setOutputVolume(pocketVol0_);
    }
    if (released) pocketPressAt_ = -1;
    if (!key) { int v = -1; if (plat::outputVolume(v)) pocketVolIdle_ = v; }   // (after any putting back above)
    if (pocketKeyNow_ == 3) {
    } else if (store.pocketHow == 0) {   // held
        if (!key) pocketKeyAt_ = -1;
        else if (pocketKeyAt_ < 0) pocketKeyAt_ = time;
        else if (time - pocketKeyAt_ >= store.pocketHold) { pocketGuard_ = pocketVol0_ >= 0; pocketLock(false); return; }
    } else if (edge) {            // pressed twice, within 0.7 s
        if (time - pocketFirstAt_ <= 0.7) { pocketGuard_ = pocketVol0_ >= 0; pocketLock(false); return; }
        pocketFirstAt_ = time;
    }
    if ((pressed || edge) && store.pocketHint) pocketHintUntil_ = time + 3;
    // the light: on while the hint shows, else off (asked again each second: waking from sleep brings it back)
    const bool lit = time < pocketHintUntil_;
    if (lit && pocketLightOff_) { plat::backlightOff(false); pocketLightOff_ = false; }
    else if (!lit && (!pocketLightOff_ || time - pocketLitAt_ > 1)) { plat::backlightOff(true); pocketLightOff_ = true; pocketLitAt_ = time; }
}

/** Unlocked by a volume key still down: its volume kept where the press began until it is let go (the user, 2026-11-07: held on past
 *  the unlock, the volume ran down to nothing). */
void App::pocketGuardTick() {
    bool up = false, down = false;
    if (!plat::volumeKeys(up, down)) { pocketGuard_ = false; return; }
    const bool key = pocketKeyNow_ == 0 ? down : pocketKeyNow_ == 1 ? up : pocketKeyNow_ == 2 ? (up || down) : false;
    int v = -1;
    if (key && pocketVol0_ >= 0 && plat::outputVolume(v) && v != pocketVol0_) plat::setOutputVolume(pocketVol0_);
    if (!key) pocketGuard_ = false;
}

/** Over everything while locked: black, and the hint while it is lit. */
void App::drawPocket() {
    if (!pocketLocked_) return;
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 1);
    if (time >= pocketHintUntil_) return;
    const float a = std::clamp(float(pocketHintUntil_ - time) / 0.4f, 0.f, 1.f) * 0.55f;   // dim, and fading out at its end
    const float cx = css::W / 2.f, cy = css::H / 2.f;
    g.icon(ICON_LOCK, cx, cy - 34, 34, pal.text, a);
    g.textC("Locked", { css::Lg, Weight::SemiBold }, cx, cy + 10, pal.text, a, Align::Center);
    std::string how = pocketHowWords();
    how[0] = char(toupper(uint8_t(how[0])));
    g.textC(how + " to unlock", { css::Base }, cx, cy + 40, pal.text2, a, Align::Center);
    if (store.pocketHow == 0 && pocketKeyAt_ >= 0) {   // held: how far
        const float f = std::clamp(float(time - pocketKeyAt_) / float(store.pocketHold), 0.f, 1.f);
        g.roundRect(cx - 80, cy + 64, 160, 4, 2, pal.text, 0.12f * a / 0.55f);
        g.roundRect(cx - 80, cy + 64, 160 * f, 4, 2, pal.accent, a);
    }
}

/**
 * Now playing's cover at (x, y), `art` wide: the cover crossfade (the new one over the old, so a frosted or glass layer
 * over them never thins mid-way), its bass pulse, and Automix's frost and hint during a join.
 * viz.js applyBeat: scale 1 + min(.03, beat·.07) from 50% 60%, halo .3 + min(.45, beat·1.1). Settings → Visualizer scales
 * both (Gatefold's full strength reads as a twitch on a handheld); the scale is eased so a kick swells. Gatefold's halo
 * behind the cover (and its beat flare) and its drop shadow are left out at the user's request: on the console's dark
 * backgrounds the halo read as a smudge and the shadow as a dark rim under the cover.
 */
void App::drawNowCover(float ax, float ay, float art) {
    const bool has = now.track >= 0 || extOn;
    const float s = art * (1 + coverBeat), sx = ax + (art - s) / 2, sy = ay + (art - s) * 0.6f;
    g.roundRect(sx, sy, s, s, 16, pal.surface);
    const float k = now.fade;
    if (pageWipe_.on) {
        // a Simple join: the outgoing cover gives way to the next one as the page's edge crosses it (the user, 2026-10-29)
        wipedImage(sx, sx + s, sy, sy + s, false, !now.cover, [&](bool old) {   // (the magic: its frost's, over it)
            if (SDL_Texture* c = mixCover(old, now.cover, now.prevCover)) g.image(c, sx, sy, s, s, 16, 1);
            else {
                g.icon(ICON_MUSIC, sx + s / 2, sy + s / 2 - 14, 64, pal.text3, 0.6f);
                g.textC("No cover", { css::Base }, sx + s / 2, sy + s / 2 + 38, pal.text3, 1, Align::Center);
            }
        });
    } else {
        if (now.prevCover && k < 1) g.image(now.prevCover, sx, sy, s, s, 16, 1);
        if (now.cover) g.image(now.cover, sx, sy, s, s, 16, k);
        if (!now.cover) {
            // .art-empty
            g.icon(ICON_MUSIC, sx + s / 2, sy + s / 2 - 14, 64, pal.text3, 0.6f * (1 - (now.prevCover ? 1 - k : 0)));
            g.textC(has ? "No cover" : "", { css::Base }, sx + s / 2, sy + s / 2 + 38, pal.text3, 1, Align::Center);
        }
    }
    if (has && time < mixUntil) {
        // the hint holds still; above the cover rather than on it in Simple, or as Settings › Appearance › Automix › The hint
        // says (the user, 2026-10-29)
        // … or in the lyrics' place, when they are on screen (drawLyrics)
        const bool above = nowHintAbove();
        // (with or without lyrics: the tab's place is the hint's either way, "No lyrics" fading as they would; the user,
        // 2026-10-29)
        const bool inLyrics = npTab == 1 && hintInLyrics(false);
        drawAutomixCover(sx, sy, s, ax + art / 2, ay + art / 2, art, 16, !above && !inLyrics);
        if (above && !inLyrics) {
            constexpr float K = 1.3f;
            const float cw = 36 * K + float(g.measure(mixLabel, { int(std::round(css::Xs * K)), Weight::SemiBold }));
            automixChip(mixLabel, ax + art / 2 - cw / 2, std::max(ay - 26, 18 * K), K, automixCoverEase(), false, true);
        }
    }
    g.roundStroke(sx, sy, s, s, 16, 1, pal.text, 0.08f);
}

float App::mixWipe() const {
    if (!mixOld.on || time < mixFrom) return 0;
    // on the join's own clock, as the lap round the cover (drawAutomixCover): the swipes end as it closes ("the swipe
    // animation should be fully timed with Automix", the user, 2026-10-29; an eased curve looked done well before it)
    if (mixUntil <= mixFrom) return 1;
    return float(std::clamp((time - mixFrom) / (mixUntil - mixFrom), 0.0, 1.0));
}

App::Focus App::automixFocus(int mode) const {
    Focus f;
    if (now.track < 0 || mixUntil <= mixShownAt || time < mixShownAt || time >= mixUntil) return f;
    auto clamp01 = [](double v) { return float(std::clamp(v, 0.0, 1.0)); };
    // the cover's pulse rests through every join, eased over 0.4 s each way (the user, 2026-10-28)
    const float q = std::min(clamp01((time - mixShownAt) / 0.4), clamp01((mixUntil - time) / 0.4));
    f.quiet = q * q * (3 - 2 * q);
    if (mode == 2 || !store.motion || mixShowLen < 3) return f;   // Simple: nothing moves   // too short to carry the glide: the frost alone
    // as the Canvas's layout morph (view_canvas.cpp): 0.9 s, cubic in and out for the move; what leaves fades over the first
    // 40 % (quadratic), what returns over the last 40 % (cubic ease-out)
    auto inOut = [](float p) { return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.f) / 2; };
    const float pin = clamp01((time - mixShownAt) / 0.9), pout = clamp01((time - (mixUntil - 0.9)) / 0.9);
    f.move = inOut(pin) * (1 - inOut(pout));
    const float l = 1 - clamp01(pin / 0.4);
    f.ui = l * l;
    if (pout > 0) {   // (from wherever it had gone: a show cut short, endJoinShow, may end before it left)
        const float r = clamp01((pout - 0.6) / 0.4);
        f.ui = std::max(f.ui, 1 - (1 - r) * (1 - r) * (1 - r));
    }
    return f;
}

void App::drawSpectrum(float x, float y, float w, float h, float played) {
    // viz.js drawSpectrum: 56 bars, gap max(2, w/n·0.28), gradient accent-2 (bottom) → accent (top), peak caps at 55%
    const int n = spec.bands;
    const float gap = std::max(2.f, std::round(w / n * 0.28f));
    const float bw = (w - gap * (n - 1)) / n;
    if (bw <= 0) return;
    for (int i = 0; i < n; i++) {
        const float bh = std::max(2.f, spec.bars[i] * h * 0.94f);
        const float bx = x + i * (bw + gap);
        const float t = bh / h;
        const Rgb top{ pal.accent2.r + (pal.accent.r - pal.accent2.r) * t, pal.accent2.g + (pal.accent.g - pal.accent2.g) * t,
                       pal.accent2.b + (pal.accent.b - pal.accent2.b) * t };
        // played (0..1, a phone upright's Now playing): what has played stays bright, the rest at 40%, as the Canvas's
        const float a = played < 0 || bx + bw <= x + w * played ? 1.f : 0.4f;
        g.roundRectV(bx, y + h - bh, bw, bh, std::min({ bw / 2, 4.f, bh / 2 }), top, pal.accent2, a);
    }
    for (int i = 0; i < n && store.vizPeaks; i++) {
        const float py = y + h - spec.peaks[i] * h * 0.94f - 3, bx = x + i * (bw + gap);
        g.rect(bx, std::max(y, py), bw, 1.5f, pal.accent, 0.55f * (played < 0 || bx + bw <= x + w * played ? 1.f : 0.4f));
    }
}

void App::drawUpNext(float x, float y, float w, float h) {
    if (srvq_.on) {   // the server's queue: what comes after this one, as rows (a press plays it)
        ScrollArea& sa = area("np.srv");
        const int from = srvq_.pos + 1, n = std::max(0, std::min(int(srvq_.songs.size()) - from, 30));
        sa.view = { x - 8, y, w + 16, h };
        sa.content = 8 + n * 52.f + 8;
        if (n == 0) { g.textC("Nothing after this one", { css::Base }, x + w / 2, y + 40, pal.text3, 1, Align::Center); return; }
        ui.pushArea(&sa);
        g.clip(x - 8, y, w + 16, h);
        for (int k = 0; k < n; k++) {
            const float ry = y + 8 + k * 52.f - sa.y;
            if (ry + 52 < y || ry > y + h) continue;
            const Subsonic::Song& s = srvq_.songs[size_t(from + k)];
            const Id id = uid("np.srvnext", k);
            // as your own Up next's rows: a 40 px cover | title + artist · album | duration (the user, 2026-11-07: the Switch showed
            // no covers here, the phone's sheet did)
            if (ui.item(id, { x - 8, ry, w + 16, 52 }, 8)) { serverPlayAt(from + k); break; }
            hoverServer(id, s);
            if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(x - 8, ry, w + 16, 52, 8, pal.text, 0.065f);
            if (SDL_Texture* tex = serverCover(s.cover)) g.image(tex, x, ry + 6, 40, 40, 6);
            else g.roundRect(x, ry + 6, 40, 40, 6, pal.surface2);
            const std::string dur = s.duration > 0 ? fmtTime(s.duration) : "";
            const int dw = g.measure(dur, { css::Sm });
            const int tw = int(w - 52 - dw - 20);
            g.text(s.title, { css::Base, Weight::SemiBold }, x + 52, ry + 7, pal.text, 1, Align::Left, tw);
            g.text(s.artist + (s.album.empty() ? "" : " · " + s.album), { css::Sm }, x + 52, ry + 27, pal.text3, 1, Align::Left, tw);
            g.textNumC(dur, { css::Sm }, x + w, ry + 26, pal.text3, 1, Align::Right);
        }
        g.unclip();
        ui.popArea();
        return;
    }
    if (extOn) {   // what plays next is the other device's
        g.textC("What plays next is up to " + extFrom, { css::Base }, x + w / 2, y + 40, pal.text3, 1, Align::Center);
        return;
    }
    ScrollArea& sa = area("np.panel");
    const std::vector<int> nextTracks = upNext(15);
    sa.view = { x - 8, y, w + 16, h };
    sa.content = 8 + nextTracks.size() * 52.f + 8;
    // a Simple join (Settings › Appearance › Automix): the outgoing song's Up next gives way to the next one's, from left
    // to right, timed to the join (the user, 2026-10-29)
    const bool give = store.mixAnimNow == 2 && mixOld.on && !mixOld.upNext.empty();
    if (nextTracks.empty() && !give) {
        g.textC(q.tracks.empty() ? "—" : "This is the last song of the playlist", { css::Base }, x + w / 2, y + 40, pal.text3, 1, Align::Center);
        return;
    }
    ui.pushArea(&sa);
    g.clip(x - 8, y, w + 16, h);
    // In sort mode (by touch: its menu's Move after a finger): the rows that can move (after the song playing, the first 15, none
    // wrapped round from the start) show the sidebar sorting's handle in the place of their length; each is drawn where sortAt
    // puts it, the one held last, lifted off the panel. Items are play positions there (queueMove's).
    const bool sort = sorting(Grab::Queue, true);
    const int qn = int(q.order.size());
    const int sortLo = q.pos + 1, sortHi = std::min(qn - 1, q.pos + 15);
    auto slot = [&](int pos) -> SDL_FRect { return { x - 8, y + 8 + (pos - q.pos - 1) * 52.f - sa.y, w + 16, 52 }; };
    if (sort) sortRun(qn, sortLo, sortHi, sa, slot, false);
    const int floats = sort ? (tsort_.held >= 0 ? tsort_.held : tsort_.settle) : -1;
    auto rows = [&](const std::vector<int>& list, bool live) {   // live: the list playing (the old one only shows)
        auto row = [&](int i, float ry, bool fl) {
            const int t = list[i];
            const Track& tr = lib.tracks[t];
            meta.wantTags(t);
            // .up-row: 40 px cover | title + artist · album | duration
            const int pos = (q.pos + 1 + i) % std::max(1, qn);   // in play order (wrapped round with Repeat)
            const bool movable = live && sort && pos == q.pos + 1 + i && pos <= sortHi;
            if (fl) sortFloatBox(x - 8, ry, w + 16, 52, 8);
            if (live) {
                const Id id = uid("np.next", i);
                if (ui.item(id, { x - 8, ry, w + 16, 52 }, 8) && !sort) playAt(pos);   // (in sort mode a tap on a row plays nothing)
                if (ui.focused(id)) { hoverTrack = t; hoverQueue = pos; hoverUpNext = true; }
                if (grab.kind == Grab::Queue && grab.upNext && grab.pos == pos) {   // picked up with the track menu's Move
                    g.roundRect(x - 8, ry, w + 16, 52, 8, pal.accent, 0.14f);
                    g.roundStroke(x - 8, ry, w + 16, 52, 8, 2, pal.accent, 0.95f);
                }
                if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(x - 8, ry, w + 16, 52, 8, pal.text, 0.065f);
            }
            coverBox(t, x, ry + 6, 40, 6);
            const std::string dur = tr.duration > 0 ? fmtTime(tr.duration) : "";
            const int dw = g.measure(dur, { css::Sm });
            const int tw = int(w - 52 - dw - 20);
            g.text(tr.title, { css::Base, Weight::SemiBold }, x + 52, ry + 7, pal.text, 1, Align::Left, tw);
            g.text(trackSub(t), { css::Sm }, x + 52, ry + 27, pal.text3, 1, Align::Left, tw);
            if (movable) sortHandle("np.sort", pos, x + w - 6, ry + 26, { x - 8, ry, w + 16, 52 });
            else g.textNumC(dur, { css::Sm }, x + w, ry + 26, pal.text3, 1, Align::Right);
        };
        int floatI = -1;
        for (int i = 0; i < int(list.size()); i++) {
            const int pos = q.pos + 1 + i;
            const bool mine = live && sort && pos < qn && pos <= sortHi;   // (a row sort mode may move)
            if (mine && pos == floats) { floatI = i; continue; }
            const float ry = mine ? sortAt(pos, slot(pos)).y : y + 8 + i * 52 - sa.y;
            if (ry + 52 < y - 60 || ry > y + h + 60) continue;
            row(i, ry, false);
        }
        if (floatI >= 0) {   // lifted off the panel, over the others (its shadow not cut at the panel's sides)
            g.clip(x - 8 - 12, y, w + 16 + 24, h);
            row(floatI, sortAt(floats, slot(floats)).y, true);
            g.clip(x - 8, y, w + 16, h);
        }
    };
    if (give) wiped(x - 8, x + w + 8, y, y + h, [&](bool old) { rows(old ? mixOld.upNext : nextTracks, !old); });
    else rows(nextTracks, true);
    g.unclip();
    ui.popArea();
}

void App::drawLyricsOf(float x, float y, float w, float h, const std::vector<LyricLine>& lyrics, bool synced, const std::string& source,
                       double at, ScrollArea& sa, bool live) {
    sa.view = { x, y, w, h };
    if (live && drawLyrOnline(x, y, w, h)) return;   // found online: looking, a card first, Not these (lyrics_online.cpp)
    if (lyrics.empty()) {
        // .lyr-msg; the server's song: no file to put anywhere
        g.text("No lyrics", { css::Lg, Weight::SemiBold }, x, y + 28, pal.text2);
        const std::string line = live && srvq_.on ? "The server has none for this song." : "Put an .lrc file with the song's name next to it, or a LYRICS tag.";
        float ty = y + 56;   // (wrapped where it does not fit: a phone upright)
        for (const auto& l : g.wrap(line, { css::Base }, int(w), 3)) {
            g.text(l, { css::Base }, x, ty, pal.text3, 1, Align::Left, int(w));
            ty += 22;
        }
        return;
    }
    // .lyr-bar: what they are, and for synced ones the timing control on the right
    const bool onlineBar = live && lyrOnlineBar(x, y, w);   // found online, playing, not kept: Keep and Not these
    if (!onlineBar) g.textC(std::string(synced ? "Synced" : "Plain text") + " · " + source, { css::Xs }, x, y + 14, pal.text3);
    if (live && !onlineBar && lyricTimingShown()) {
        const Font cf{ css::Xs, Weight::SemiBold };
        const float cw = lyricSyncControl(nullptr, 0, 0, 24, cf, 0, false);   // measured first, then drawn against the right edge
        lyricSyncControl("np.sync", x + w - cw, y + 14, 24, cf, 1, true);
    }
    const float barH = onlineBar ? 46 : 30;   // (its buttons 9 px under the tabs' line, as the found-online card)
    const float top = y + barH, vh = h - barH;
    sa.view = { x, top, w, vh };

    const Font f = synced ? Font{ css::Lg + 1, Weight::Bold } : Font{ css::Base };
    const float lh = synced ? std::round((css::Lg + 1) * 1.32f) : 22;
    int cur = -1;
    if (synced) {
        const double pos = at + 0.15;
        for (int i = 0; i < int(lyrics.size()); i++) if (lyrics[i].t <= pos) cur = i;
    }
    // Lay out wrapped lines once per frame (cheap: wrap results are cached by Gfx).
    std::vector<std::pair<int, std::string>> rows;
    for (int i = 0; i < int(lyrics.size()); i++) {
        auto parts = lyrics[i].text.empty() ? std::vector<std::string>{ "♪" } : g.wrap(lyrics[i].text, f, int(w - 12), 4);
        for (auto& p : parts) rows.push_back({ i, p });
    }
    const float gapLine = synced ? 8 : 0;
    std::vector<float> rowY(rows.size());
    float yy = 0;
    for (size_t r = 0; r < rows.size(); r++) {
        if (r > 0 && rows[r].first != rows[r - 1].first) yy += gapLine;
        rowY[r] = yy;
        yy += lh;
    }
    sa.content = yy + vh * 0.6f;
    // Scrolled by hand (touch, right stick, D-pad onto a line): left there for 4 s, then back to the line playing.
    if (synced && (!live || ui.now() - sa.manual > 4)) {
        // .lyr-track follows the current line, a third of the way down; before the first line, the top (it kept the scroll
        // of the song before: an Automix join's next lyrics sat out of sight until their first line, 2026-10-29)
        if (cur < 0) sa.target = 0;
        for (size_t r = 0; r < rows.size(); r++) if (rows[r].first == cur) { sa.target = std::max(0.f, rowY[r] - vh * 0.32f); break; }
    }
    if (!live) sa.y = sa.target;   // (the outgoing song's: no hand on it, it stays on its line)
    if (live) ui.pushArea(&sa);
    g.clip(x, top, w, vh);
    for (size_t r = 0; r < rows.size(); r++) {
        const float ry = top + rowY[r] - sa.y;
        if (ry + lh < top - 40 || ry > top + vh + 40) continue;
        const int li = rows[r].first;
        float a = 1;
        Rgb c = pal.text3;
        if (synced) {
            if (li == cur) c = pal.text;
            else if (li < cur) a = 0.42f;
            else a = std::abs(li - cur) <= 2 ? 0.75f : 0.6f;
            // Fade the edges like the CSS mask on .lyr
            const float e = std::min(sa.y > 1 ? ry - top : 1e9f, top + vh - ry - lh);
            a *= clampf(e / 24 + 0.15f, 0.15f, 1);
            if (live && (r == 0 || rows[r - 1].first != li)) {
                const Id id = uid("np.lyr", li);
                if (ui.item(id, { x, ry, w, lh }, 6)) seekLyric(li);
            }
        } else {
            c = pal.text2;
        }
        if (synced && lyrics[li].text.empty()) lyricDots(x, ry + lh / 2, 7, li == cur ? pal.text : c, a, li == cur);   // a break
        else g.text(rows[r].second, f, x, ry, c, a, Align::Left, int(w - 12));
    }
    g.unclip();
    if (live) ui.popArea();
}

void App::drawLyrics(float x, float y, float w, float h) {
    // a Simple join (Settings › Appearance › Automix): the lyrics fade out, the hint takes their place, the next song's fade
    // back in (App::hintInLyrics, as the Canvas: the user, 2026-10-29); the outgoing song's until the join's middle
    if (store.mixAnimNow == 2) {
        const float e = automixCoverEase();
        if (e > 0.002f) {
            const float f0 = g.fade();
            g.setFade(f0 * (1 - e));
            if (mixOld.on && mixWipe() < 0.5f)
                drawLyricsOf(x, y, w, h, mixOld.lyrics, mixOld.synced, mixOld.lyricsSource, mixOld.lyricsAt + (time - mixOld.at), area("np.panel.old"), false);
            else drawLyricsOf(x, y, w, h, now.lyrics, now.synced, now.lyricsSource, lyricsPos(), area("np.panel"), true);
            g.setFade(f0);
            if (hintInLyrics(false)) lyricsHint(x, y, w, h, 1.6f);   // (no lyrics: still their place, as drawNowCover's inLyrics)
            return;
        }
    }
    drawLyricsOf(x, y, w, h, now.lyrics, now.synced, now.lyricsSource, lyricsPos(), area("np.panel"), true);
}

/**
 * The lyrics' timing, as a pill like the Canvas' layout switch: "Earlier", the nudge, "Later". Each press moves
 * them 0.1 s; the value in the middle puts them back in time with the file. key nullptr: only measures.
 */
float App::lyricSyncControl(const char* key, float x, float cy, float h, const Font& f, float alpha, bool live) {
    const float n = lyricNudge();
    char v[24];
    if (std::fabs(n) < 0.05f) snprintf(v, sizeof v, "0.0 s");
    else snprintf(v, sizeof v, "%s%.1f s", n > 0 ? "+" : "−", double(std::fabs(n)));
    const char* labels[3] = { "Earlier", v, "Later" };
    float ws[3], total = 0;
    for (int i = 0; i < 3; i++) {
        // the middle sized for the widest value, so the pill does not change width as it moves
        ws[i] = (i == 1 ? std::max(g.measure("+10.0 s", f), g.measure(v, f)) : g.measure(labels[i], f)) + h * 0.8f;
        total += ws[i];
    }
    if (!key) return total;
    g.roundRect(x, cy - h / 2, total, h, h / 2, pal.text, 0.08f * alpha);
    float bx = x;
    for (int i = 0; i < 3; i++) {
        const Id id = uid(key, i);
        if (live && ui.item(id, { bx, cy - h / 2, ws[i], h }, h / 2)) nudgeLyrics(i == 0 ? -0.1f : i == 2 ? 0.1f : 0.f);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (hot) g.roundRect(bx, cy - h / 2, ws[i], h, h / 2, pal.text, 0.1f * alpha);
        const bool value = i == 1, moved = std::fabs(n) >= 0.05f;
        if (value) g.textNum(labels[i], f, bx + ws[i] / 2, cy - g.lineHeight(f) / 2.f, moved ? pal.accentInk : pal.text3, alpha, Align::Center);   // (the nudge: fixed-width digits)
        else g.textC(labels[i], f, bx + ws[i] / 2, cy, pal.text2, alpha, Align::Center);
        bx += ws[i];
    }
    return total;
}

void App::drawPaletteTab(float x, float y, float w, float h) {
    // a Simple join: the outgoing song's colours give way to the next one's from left to right with the column's swipe
    // (the user, 2026-10-29; they switched as the next cover came in)
    if (store.mixAnimNow == 2 && mixOld.on) {
        wiped(x, x + w, y, y + h, [&](bool old) { drawPaletteOf(x, y, w, h, old ? mixOld.palette : target, !old); });
        return;
    }
    drawPaletteOf(x, y, w, h, target, true);
}

void App::drawPaletteOf(float x, float y, float w, float h, const Palette& target, bool live) {
    ScrollArea& sa = area("np.panel");
    sa.view = { x, y, w, h };
    // two columns of roles; one where a name and its colour would meet (a phone upright: "Secondary text" ran into its number)
    const int palCols = (w - 24) / 2 < 200 ? 1 : 2, palRows = 8 / palCols;
    sa.content = 8 + 26 + 10 + palRows * 30 + 30;
    const Id id = uid("np.palette");
    if (live) {
        ui.pushArea(&sa);
        ui.item(id, { x, y, w, 40 }, 8);
    }
    g.clip(x, y, w, h);
    float cy = y + 8 - sa.y;
    // .swatches: widths follow each cluster's share of the cover
    float sx = x;
    for (int i = 0; i < target.swatchCount; i++) {
        const float sw = std::max(16.f, std::round(target.swatches[i].share * 160));
        g.roundRect(sx, cy, sw, 26, 7, target.swatches[i].c);
        g.roundStroke(sx, cy, sw, 26, 7, 1, pal.text, 0.09f);
        sx += sw + 6;
    }
    cy += 26 + 10;
    auto hex = [](const Rgb& c) {
        char b[8];
        snprintf(b, sizeof b, "#%02x%02x%02x", int(c.r * 255 + 0.5f), int(c.g * 255 + 0.5f), int(c.b * 255 + 0.5f));
        return std::string(b);
    };
    const std::pair<const char*, Rgb> roles[8] = { { "Background", target.bg }, { "Surface", target.surface },     { "Accent", target.accent },
                                                   { "Accent · text", target.accentInk }, { "Second accent", target.accent2 }, { "Glow", target.glow },
                                                   { "Text", target.text },             { "Secondary text", target.text2 } };
    const float colW = palCols == 1 ? w : (w - 24) / 2;
    for (int i = 0; i < 8; i++) {
        const float rx = x + (i % palCols) * (colW + 24), ry = cy + (i / palCols) * 30;
        g.roundRect(rx, ry + 4, 22, 22, 6, roles[i].second);
        g.roundStroke(rx, ry + 4, 22, 22, 6, 1, pal.text, 0.09f);
        g.textC(roles[i].first, { css::Base }, rx + 32, ry + 15, pal.text2);
        g.textC(hex(roles[i].second), { css::Sm }, rx + colW, ry + 15, pal.text3, 1, Align::Right);
    }
    cy += palRows * 30 + 8;
    char desc[96];
    snprintf(desc, sizeof desc, "Dark scheme · %s · average lightness %d%%",
             target.monochrome ? "monochrome cover" : ("clusters: " + std::to_string(target.clusters)).c_str(), int(target.avgL * 100));
    g.text(desc, { css::Sm }, x, cy, pal.text3);
    g.unclip();
    if (live) ui.popArea();
}
