/*
 * Songate · view_phone.cpp (Android)
 * A phone held upright: the same app, its own layout, made for one hand (the user, 2026-11-03/04: "a portrait design stylised
 * portrait ease of use ... using the UI language from songate", "Do not copy poweramp", approved from the mock-ups,
 * docs/next-session/mockups-portrait-*.png and portrait-seek-and-lyrics.mp4). Songate's own pieces throughout: the wordmark, its
 * buttons, chips, rows, the waveform seek bar, the visualizer, the lyrics; the page under a top line and over a bar of five
 * destinations, the mini player between them. Now playing has the cover across the top with the song on it, Up next and the lyrics
 * page behind buttons; every other page is the landscape one, laid into the phone's width.
 */
#ifdef GF_ANDROID
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {
constexpr float kPad = 18;      // the page's side margin
constexpr float kTopH = 52;     // the wordmark's line
constexpr float kNavH = 68;     // the bottom bar
constexpr float kMiniH = 64;    // the mini player
constexpr int kLibraryHome = -7;   // View::Library's ref for the phone's Library page (its rows); any other: the landscape one
float smooth(float p) { return p * p * (3 - 2 * p); }
float inOutCubic(float p) { return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.f) / 2; }   // the Canvas's layout morph's
}  // namespace

void App::drawPhone() {
    const float W = float(css::W), H = float(css::H);
    const bool has = now.track >= 0 || extOn;
    const TrackInfo& in = now.info;
    const View v = route.view;
    auto mix = [](const Rgb& p, const Rgb& q, float t) { return Rgb{ p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t }; };
    auto grad = [&](float t) { return mix(pal.accent2, pal.accent, std::clamp(t, 0.f, 1.f)); };
    auto step = [&](float& p, bool on, float secs) {
        const float s = store.motion ? dt / secs : 1.f;
        p = on ? std::min(1.f, p + s) : std::max(0.f, p - s);
    };
    step(phoneLyricsP_, phoneLyrics_ && v == View::Now, 0.7f);
    step(phoneQueueP_, phoneQueue_, 0.35f);
    if (v != View::Now) phoneQueue_ = false;

    // ---- shared pieces
    auto glass = [&](float x, float y, float w, float h, float a = 1) {
        g.roundRect(x, y, w, h, h / 2, pal.bg, 0.6f * a);
        g.roundStroke(x, y, w, h, h / 2, 1, pal.text, 0.12f * a);
    };
    // a round glass button over the cover: pressed it darkens, on it is in the accent
    auto glassButton = [&](Id id, const char* icon, float cx, float cy, float d, bool on = false, uint8_t flags = 0) {
        const bool hit = ui.item(id, { cx - d / 2, cy - d / 2, d, d }, d / 2, flags);
        glass(cx - d / 2, cy - d / 2, d, d);
        if (ui.pressed(id)) g.circle(cx, cy, d / 2, pal.text, 0.12f);
        g.icon(icon, cx, cy, d * 0.46f, on ? pal.accent : pal.text);
        return hit;
    };
    const bool liked = now.track >= 0 && !extOn && store.isLiked(lib.tracks[now.track].path);
    auto toggleLike = [&] {
        if (now.track < 0 || extOn) return;
        store.toggleLiked(lib.tracks[now.track].path);
        store.save();
        toast(liked ? "Removed from Liked" : "Added to Liked");
    };
    auto rootView = [&](View x) { return x == View::Now || (x == View::Library && route.ref == kLibraryHome) || x == View::Search || x == View::Playlists || (x == View::Settings && !settingsPhoneOpen_ && settingsQuery.empty()); };

    // The top: back where there is a page to go back to, the wordmark, the clock and the battery (the title bar's, the system's bar hidden)
    auto top = [&] {
        float wx = kPad - 2 + edge_.tbL;   // clear of a camera at its ends (Settings › Appearance › Screen edges)
        if (!rootView(v) && !history.empty()) {
            if (iconButton(uid("ph.back"), ICON_BACK, kPad + 12 + edge_.tbL, kTopH / 2, false, 40, 20)) back();
            wx += 36;
        }
        g.wordmark(wx, kTopH / 2, mix(pal.accent, pal.text, 0.12f), mix(pal.accent2, pal.text, 0.2f), mix(pal.accent2, pal.text, 0.78f), pal.glow, 0.4f, -1, 1);
        const std::time_t tt = std::time(nullptr);
        char clock[8];
        std::strftime(clock, sizeof clock, "%H:%M", std::localtime(&tt));
        unsigned pct = 0;
        float rx = W - kPad - edge_.tbR;
        if (plat::batteryLevel(pct)) {
            const float by = kTopH / 2 - 6;
            g.roundStroke(rx - 24, by, 24, 12, 3, 1.5f, pal.text2);
            g.roundRect(rx - 21.5f, by + 2.5f, 19 * pct / 100.f, 7, 1.5f, pal.text2);
            rx -= 34;
        }
        g.textC(clock, { css::Sm, Weight::SemiBold }, rx, kTopH / 2, pal.text2, 1, Align::Right);
    };

    // The bottom bar: the sidebar's destinations, a phone's five, as the collapsed sidebar shows them
    auto nav = [&] {
        const float y0 = H - kNavH;
        g.rect(0, y0, W, kNavH, pal.bg, 0.86f);
        g.rect(0, y0, W, 1, pal.text, 0.09f);
        struct Dest { const char* icon; const char* hintText; View view; };
        static const Dest dests[5] = { { ICON_DISC, "Now playing", View::Now }, { ICON_GRID, "Library", View::Library }, { ICON_SEARCH, "Search", View::Search },
                                       { ICON_HEART, "Playlists", View::Playlists }, { ICON_GEAR, "Settings", View::Settings } };
        const float cw = W / 5;
        int active = -1;
        if (v == View::Now) active = 0;
        else if (v == View::Search) active = 2;
        else if (v == View::Playlists || v == View::List) active = 3;
        else if (v == View::Settings) active = 4;
        else if (v != View::PickFolder && v != View::PickPlaylist && v != View::PickExport) active = 1;   // the library's pages
        for (int i = 0; i < 5; i++) {
            const float cx = cw * i + cw / 2, cy = y0 + kNavH / 2;
            const Id id = uid("ph.nav", i);
            hint(id, dests[i].hintText);
            if (ui.item(id, { cw * i, y0, cw, kNavH }, 20)) {
                if (i == 1) navigate(View::Library, kLibraryHome);
                else if (i == 2) navigate(View::Search, -1, v == View::Search ? route.query : std::string());
                else if (i == 4) { settingsPhoneOpen_ = settingsPhoneDirect_ = false; settingsQuery.clear(); if (v != View::Settings) navigate(View::Settings); }
                else if (dests[i].view != v) navigate(dests[i].view);
            }
            const bool on = i == active;
            if (on) g.roundRect(cx - 34, cy - 20, 68, 40, 20, pal.accent, 0.14f);
            else if (ui.pressed(id)) g.roundRect(cx - 34, cy - 20, 68, 40, 20, pal.text, 0.08f);
            g.icon(dests[i].icon, cx, cy, 24, on ? pal.accent : pal.text2);
        }
    };

    // The mini player: the player bar folded into a card over the page; its progress a thin line along its foot. Tap it: Now playing
    auto mini = [&](float a = 1) {
        const float x = 10, y = H - kNavH - kMiniH - 8, w = W - 20, h = kMiniH, cy = y + h / 2;
        g.setFade(a);
        const Id card = uid("ph.mini");
        if (ui.item(card, { x, y, w - 150, h }, 16)) navigate(View::Now);
        g.roundRect(x, y, w, h, 16, pal.surface, 0.94f);
        g.roundStroke(x, y, w, h, 16, 1, pal.text, 0.09f);
        if (ui.pressed(card)) g.roundRect(x, y, w, h, 16, pal.text, 0.05f);
        for (float r : { 28.f, 32.f }) g.glow(x + 12 + 22, cy, r, pal.glow, 0.22f);
        if (now.cover) g.image(now.cover, x + 12, cy - 22, 44, 44, 8);
        else { g.roundRect(x + 12, cy - 22, 44, 44, 8, pal.surface2); g.icon(ICON_MUSIC, x + 34, cy, 18, pal.text3); }
        const float tx = x + 12 + 44 + 12, tw = w - (tx - x) - 140;
        marqueeLine(has ? in.title : "—", { css::Base, Weight::SemiBold }, tx, cy - 20, tw, pal.text);
        marqueeLine(has ? in.artist : "", { css::Sm }, tx, cy + 1, tw, pal.text3);
        if (!extOn && iconButton(uid("ph.mlike"), liked ? ICON_HEART_F : ICON_HEART, x + w - 112, cy, liked, 40, 20)) toggleLike();
        const Id pid = uid("ph.mplay");
        if (!controlsOff() && ui.item(pid, { x + w - 88, cy - 22, 44, 44 }, 22)) togglePlay();
        for (float r : { 22.f, 25.f }) g.glow(x + w - 66, cy, r, pal.glow, 0.25f);
        g.circle(x + w - 66, cy, 19 * (ui.pressed(pid) ? 0.94f : 1.f), pal.text, controlsOff() ? 0.35f : 1.f);
        g.icon(isPlaying() || extOn ? ICON_PAUSE : ICON_PLAY, x + w - 66, cy, 17, pal.bg);
        if (!controlsOff() && iconButton(uid("ph.mnext"), ICON_NEXT, x + w - 24, cy, false, 40, 20)) next(false);
        const double dur = in.duration;
        const float frac = dur > 0 ? float(std::clamp(shownPosition() / dur, 0.0, 1.0)) : 0;
        g.roundRect(x + 16, y + h - 4, w - 32, 2, 1, pal.text, 0.12f);
        g.roundRectH(x + 16, y + h - 4, (w - 32) * frac, 2, 1, pal.accent2, pal.accent);
        g.setFade(1);
    };

    // The seek bar: touched, Songate's waveform scrub; let go (after a moment), Now playing's visualizer; one shape between them
    // (the user, 2026-11-04: "the scrub will turn into a waveform animation after no touch is sensed", "Make the transition
    // seamless", the visualizer "with the bars that react to music"). Each visualizer column gathers the scrub's thin bars in its
    // stretch, sliding together and widening, dropping from the middle to its foot, taking the music's height and its colours.
    auto seekBar = [&](float x, float y, float w, float h) {
        const Id sid = uid("ph.seek");
        const bool locked = controlsOff();
        if (!locked) ui.item(sid, { x, y - 8, w, h + 16 }, 6, Slider | NoSideEntry);
        const float drag = locked ? -1.f : ui.sliderDrag(sid);
        if (drag >= 0 || ui.pressed(sid)) phoneSeekAt_ = time;
        const double dur = in.duration;
        if (!locked && dur > 0) {
            if (const int st = ui.sliderStep(sid)) seekBy(st * 5);
            if (ui.sliderReleased(sid)) player.seek(std::clamp((ui.lastDragX() - x) / w, 0.f, 1.f) * dur);
        }
        step(phoneSeekP_, time - phoneSeekAt_ > 0.5 && !(ui.focused(sid) && ui.showFocusRing()), 0.5f);   // back to the visualizer half a second after the finger (the user: "0.5/1s")
        const float e = smooth(phoneSeekP_);
        float frac = dur > 0 ? float(std::clamp(shownPosition() / dur, 0.0, 1.0)) : 0;
        if (drag >= 0) frac = std::clamp((drag - x) / w, 0.f, 1.f);
        const float foot = y + h, cy = y + h / 2;
        if (e >= 0.999f && spec.bands > 0) drawSpectrum(x, y, w, h, frac);   // the song's place: the bars before it bright (the Canvas's look)
        else {
            const bool waveNow = wave.ready() && now.track >= 0 && wave.path() == lib.tracks[now.track].path;
            const auto& pk = wave.peaks();
            const Gfx::Pitch pt = g.barPitch(2, 3);   // the scrub's 2 px bars every 3, in whole real pixels (as the player bar's)
            const float x0 = g.onPixel(x);
            const int bars = int(w / pt.step);
            const int n = std::max(1, spec.bands);
            const float gap = std::max(2.f, std::round(w / n * 0.28f)), colW = (w - gap * (n - 1)) / n;
            for (int i = 0; i < bars; i++) {
                float vv = 0.3f;
                if (waveNow && !pk.empty()) {
                    const size_t a = size_t(i) * pk.size() / bars, b = std::max(a + 1, size_t(i + 1) * pk.size() / bars);
                    vv = 0;
                    for (size_t j = a; j < b && j < pk.size(); j++) vv = std::max(vv, pk[j]);
                }
                const float seekH = std::max(1.5f, vv * h * 0.9f);
                const int k = std::min(n - 1, i * n / bars), i0 = (k * bars + n - 1) / n, i1 = ((k + 1) * bars + n - 1) / n;
                const int cnt = std::max(1, i1 - i0), at = i - i0;
                const float colX = x + k * (colW + gap);
                const float specH = std::max(2.f, (spec.bands > 0 ? spec.bars[k] : 0.f) * h * 0.94f);
                const float bx = x0 + i * pt.step + (colX + at * colW / cnt - (x0 + i * pt.step)) * e;
                const float bw = pt.bar + (colW / cnt + 0.5f - pt.bar) * e;
                const float bh = seekH + (specH - seekH) * e;
                const float by = (cy - seekH / 2) + ((foot - specH) - (cy - seekH / 2)) * e;
                const bool played = i * pt.step < frac * w;
                const Rgb seekC = played ? grad(float(i) / bars) : pal.text;
                const float seekA = played ? 1.f : (drag >= 0 ? 0.2f : 0.13f);
                const Rgb c = mix(seekC, grad(specH / h), e);
                const float specA = bx + bw <= x + frac * w ? 1.f : 0.4f;   // to the visualizer's own brightness, played or not
                g.rect(bx, by, bw, bh, c, seekA + (specA - seekA) * e);
            }
            const float late = std::clamp((e - 0.55f) / 0.45f, 0.f, 1.f);
            if (late > 0 && spec.bands > 0)
                for (int k = 0; k < n; k++) {
                    const float bh = std::max(2.f, spec.bars[k] * h * 0.94f), bx = x + k * (colW + gap);
                    const float pa = late * (bx + colW <= x + frac * w ? 1.f : 0.4f);
                    g.roundRectV(bx, foot - bh, colW, bh, std::min({ colW / 2, 4.f, bh / 2 }), grad(bh / h), pal.accent2, pa);
                    if (store.vizPeaks) g.rect(bx, std::max(y, foot - spec.peaks[k] * h * 0.94f - 3), colW, 1.5f, pal.accent, 0.55f * pa);
                }
        }
        if (drag >= 0 && dur > 0) {   // as the player bar's: the time under the finger
            const std::string tip = fmtTime(frac * dur);
            const float tw = g.measure(tip, { css::Xs }) + 16, tx = std::clamp(x + w * frac - tw / 2, x, x + w - tw);
            g.roundRect(tx, y - 30, tw, 24, 6, pal.surface2);
            g.textC(tip, { css::Xs }, tx + tw / 2, y - 18, pal.text, 1, Align::Center);
        }
        g.text(fmtTime(drag >= 0 ? frac * dur : shownPosition()), { css::Xs }, x, foot + 8, pal.text3);
        g.text(fmtTime(dur), { css::Xs }, x + w, foot + 8, pal.text3, 1, Align::Right);
    };

    // ---- Now playing, and its lyrics page (lp: 0 → 1): the cover glides from the page's width to a small square top left
    auto nowPlaying = [&] {
        const float lp = phoneLyricsP_, lpe = inOutCubic(lp);
        const float out = std::clamp(1 - lp / 0.5f, 0.f, 1.f), in2 = std::clamp((lp - 0.4f) / 0.6f, 0.f, 1.f);
        const float bigS = W - 32, smallS = 72, top0 = kTopH + 8;
        const float cs = bigS + (smallS - bigS) * lpe, cx0 = 16 + (kPad - 16) * lpe, cy0 = top0 + 6 * lpe, cr = 22 + (10 - 22) * lpe;
        g.setFade(1 - lpe);
        for (float r : { cs * 0.5f, cs * 0.56f }) g.glow(cx0 + cs / 2, cy0 + cs / 2, r, pal.glow, 0.18f);
        g.setFade(1);
        phoneCoverRest_ = { cx0, cy0, cs, cs };
        if (phoneJoinMove_ < 0 || lp > 0.01f) {   // (an Automix join: the cover is the glide's, drawn after the page)
            if (now.cover) g.image(now.cover, cx0, cy0, cs, cs, cr);
            else { g.roundRect(cx0, cy0, cs, cs, cr, pal.surface2); g.icon(ICON_MUSIC, cx0 + cs / 2, cy0 + cs / 2, cs * 0.3f, pal.text3, 0.6f); }
            g.roundStroke(cx0, cy0, cs, cs, cr, 1, pal.text, 0.09f);
        }
        // the lyrics page: the song beside the small cover, its menu, the lyrics (Songate's own: tap a line to go there), the mini player
        if (in2 > 0) {
            g.setFade(in2);
            const float tx = kPad + smallS + 14, tw2 = W - tx - kPad - 44, dy = 12 * (1 - in2);
            if (ui.item(uid("ph.lcover"), { kPad, top0 + 6, smallS, smallS }, 10)) phoneLyrics_ = false;   // the small cover: back
            g.text(has ? in.title : "Songate", { css::Lg, Weight::Bold }, tx, top0 + 10 + dy, pal.text, 1, Align::Left, int(tw2));
            g.text(has ? in.artist : "", { css::Sm, Weight::SemiBold }, tx, top0 + 38 + dy, pal.text2, 1, Align::Left, int(tw2));
            g.text(fmtTime(in.duration) + (in.codec.empty() ? "" : "  ·  " + in.codec), { css::Xs }, tx, top0 + 60 + dy, pal.text3, 1, Align::Left, int(tw2));
            if (now.track >= 0 && iconButton(uid("ph.lmore"), ICON_MORE, W - kPad - 18, top0 + 42, false, 40, 20)) openTrackMenu(now.track);
            const float ly0 = top0 + smallS + 26, ly1 = H - kNavH - kMiniH - 16;
            if (lp >= 0.999f) drawLyricsOf(kPad, ly0, W - kPad * 2, ly1 - ly0, now.lyrics, now.synced, now.lyricsSource, lyricsPos(), area("ph.lyrics"), true);
            else {   // coming or going: a picture of it, rising into place (nothing to touch until it is there)
                g.clip(0, ly0, W, ly1 - ly0);
                const float was = g.fade();
                g.setFade(was * in2);
                drawLyricsOf(kPad, ly0 + 30 * (1 - in2), W - kPad * 2, ly1 - ly0, now.lyrics, now.synced, now.lyricsSource, lyricsPos(), area("ph.lyrics"), false);
                g.setFade(was);
                g.unclip();
            }
            g.setFade(1);
            mini(in2);
        }
        if (out <= 0) return;
        g.setFade(out);
        const bool live = out >= 0.999f;   // what leaves takes no touch on its way
        auto gb = [&](const char* key, const char* icon, float cx, float cy, float d, bool on = false, uint8_t flags = 0) {
            if (live) return glassButton(uid(key), icon, cx, cy, d, on, flags);
            glass(cx - d / 2, cy - d / 2, d, d);
            g.icon(icon, cx, cy, d * 0.46f, on ? pal.accent : pal.text);
            return false;
        };
        // on the cover's foot: the name and the artist in glass, the heart left; Lyrics, Up next and the song's menu right
        const float by = cy0 + cs - 16 - 22;
        hint(uid("ph.like"), liked ? "Remove from Liked" : "Add to Liked");
        if (!extOn && gb("ph.like", liked ? ICON_HEART_F : ICON_HEART, cx0 + 16 + 22, by, 44, liked)) toggleLike();
        if (now.track >= 0 && gb("ph.more", ICON_MORE, cx0 + cs - 16 - 22, by, 44)) openTrackMenu(now.track);
        if (gb("ph.queue", ICON_QUEUE, cx0 + cs - 16 - 22 - 52, by, 44, phoneQueue_)) phoneQueue_ = true;
        if (gb("ph.lyrics", ICON_QUOTE, cx0 + cs - 16 - 22 - 104, by, 44, phoneLyrics_)) phoneLyrics_ = true;
        const Font tf{ css::Lg, Weight::Bold }, af{ css::Sm, Weight::SemiBold };
        const float maxW = cs - 32 - 28;
        const std::string artistText = has ? in.artist + (in.album.empty() || in.album == in.title ? "" : " · " + in.album) : "";
        const float ay = by - 22 - 10 - 32;
        if (!artistText.empty()) {
            const float aw = std::min(maxW, float(g.measure(artistText, af))) + 28;
            glass(cx0 + 16, ay, aw, 32);
            g.textC(artistText, af, cx0 + 30, ay + 16, pal.text2, 1, Align::Left, int(aw - 28));
        }
        const std::string titleText = has ? in.title : "Songate";
        const float tw = std::min(maxW, float(g.measure(titleText, tf))) + 36, ty = ay - 8 - 44;
        glass(cx0 + 16, ty, tw, 44);
        g.textC(titleText, tf, cx0 + 34, ty + 22, pal.text, 1, Align::Left, int(tw - 36));
        // under the cover: the player bar's smaller buttons (settling down as the lyrics come); no Canvas upright (the user,
        // 2026-11-04: "Canvas should not exist in portrait"): Sleep on the left, Equalizer and Order on the right
        float y = top0 + bigS + 44 + 40 * lpe;
        if (gb("ph.sleep", ICON_MOON, kPad + 20, y, 40, sleepLeft() > 0)) openSleep();
        if (gb("ph.eq", ICON_EQ, W - kPad - 20 - 52, y, 40, store.eq.on != 0, Hold)) toggleEq();
        if (live && ui.longPressed(uid("ph.eq"))) openEqualizer();
        static const char* const orderIcons[4] = { ICON_ORDER, ICON_REPEAT, ICON_REPEAT1, ICON_SHUFFLE };
        const Order order = shownOrder();
        if (gb("ph.order", orderIcons[std::clamp(int(order), 0, 3)], W - kPad - 20, y, 40, order != Order::Default) && !controlsOff()) cycleOrder();
        // the lower half, spread evenly: the seek bar ⇄ visualizer, the transport large, what the file is
        const float room = (H - kNavH) - (y + 30), gapK = room / 9;
        y += 30 + gapK * 1.6f;
        phoneToastBottom_ = y - 12;   // a toast in the gap above the seek bar (App::drawToast)
        if (live) seekBar(kPad, y, W - kPad * 2, 40);
        else if (spec.bands > 0) {
            const double d = now.info.duration;
            drawSpectrum(kPad, y, W - kPad * 2, 40, d > 0 ? float(std::clamp(shownPosition() / d, 0.0, 1.0)) : 0.f);
        }
        y += 40 + gapK * 3.2f;
        const float mid = W / 2;
        const bool off = controlsOff();
        if (live && !off && iconButton(uid("ph.sac"), ICON_PAUSE_C, kPad + 20, y, stopAfterCurrent, 44, 20)) {
            stopAfterCurrent = !stopAfterCurrent;
            toast(stopAfterCurrent ? "Stop after the current track" : "Keep playing after this track");
        }
        if (live && !off && iconButton(uid("ph.prev"), ICON_PREV, mid - 96, y, false, 60, 30)) prev();
        const Id pid = uid("ph.play");
        if (live && !off && ui.item(pid, { mid - 46, y - 46, 92, 92 }, 46)) togglePlay();
        for (float r : { 50.f, 56.f, 62.f }) g.glow(mid, y, r, pal.glow, 0.3f);
        g.circle(mid, y, 46 * (ui.pressed(pid) ? 0.95f : 1.f), pal.text, off ? 0.35f : 1.f);
        g.icon(isPlaying() || extOn ? ICON_PAUSE : ICON_PLAY, mid, y, 38, pal.bg);
        if (live && !off && iconButton(uid("ph.next"), ICON_NEXT, mid + 96, y, false, 60, 30)) next(false);
        if (live && iconButton(uid("ph.devices"), ICON_LINK, W - kPad - 20, y, tog.stage != Tog::Off, 44, 20)) openDevices();
        y += gapK * 2.6f;
        std::vector<std::string> bs;
        if (!in.codec.empty()) bs.push_back(in.codec);
        char b[48];
        if (in.bitsPerSample && in.codec != "MP3") { snprintf(b, sizeof b, "%d-bit", in.bitsPerSample); bs.push_back(b); }
        if (in.sampleRate) { snprintf(b, sizeof b, "%g kHz", in.sampleRate / 1000.0); bs.push_back(b); }
        if (in.bitrateKbps) bs.push_back(std::to_string(in.bitrateKbps) + " kbps");
        float total = 0;
        for (const auto& s : bs) total += g.measure(s, { css::Xs, Weight::SemiBold }) + 20 + 6;
        float bx = mid - (total - 6) / 2;
        for (size_t i = 0; i < bs.size(); i++) bx += chip(bs[i], bx, y, i == 0) + 6;
        g.setFade(1);
    };

    // ---- Up next: the queue as a sheet over Now playing, from its button; tap a row to play it, the veil or back to close
    auto upNextSheet = [&] {
        const float e = smooth(phoneQueueP_);
        const float sh = H * 0.66f, y0 = H - sh * e;
#ifdef GF_DEV
        g.saidLayer = 1;   // (tests: a sheet over the page)
#endif
        g.rect(0, 0, W, H, { 0, 0, 0 }, 0.55f * e);
        ui.region(Region::Modal);
        if (phoneQueue_ && ui.item(uid("ph.qveil"), { 0, 0, W, y0 }, 0)) phoneQueue_ = false;
        g.roundRect(0, y0, W, sh + 40, 24, pal.surface, 0.98f);
        g.roundStroke(0, y0, W, sh + 40, 24, 1, pal.text, 0.09f);
        g.roundRect(W / 2 - 20, y0 + 10, 40, 5, 2.5f, pal.text, 0.25f);
        kicker("Playlist", kPad, y0 + 38, pal.text3);
        g.text("Up next", { css::Xl, Weight::Bold }, kPad, y0 + 50, pal.text);
        g.textC(std::to_string(q.order.size()) + (q.order.size() == 1 ? " track" : " tracks"), { css::Sm }, W - kPad - 56, y0 + 66, pal.text3, 1, Align::Right);
        if (iconButton(uid("ph.qclose"), ICON_X, W - kPad - 16, y0 + 66, false, 40, 18)) phoneQueue_ = false;
        const float listTop = y0 + 100, listH = H - listTop;
        g.rect(kPad, listTop - 1, W - kPad * 2, 1, pal.text, 0.07f);
        ScrollArea& sa = area("ph.queue");
        const int n = q.pos >= 0 ? int(q.order.size()) - q.pos : 0;
        sa.view = { 0, listTop, W, listH };
        sa.content = 8 + n * 64.f + 16;
        ui.pushArea(&sa);
        g.clip(0, listTop, W, listH);
        for (int k = 0; k < n; k++) {
            const float ry = listTop + 8 + k * 64 - sa.y;
            if (ry + 64 < listTop || ry > H) continue;
            const int pos = q.pos + k, t = q.tracks[q.order[pos]];
            const Track& tr = lib.tracks[t];
            meta.wantTags(t);
            const Id id = uid("ph.qrow", pos);
            if (ui.item(id, { kPad - 10, ry, W - kPad * 2 + 20, 60 }, 12) && pos != q.pos) playAt(pos);
            if (ui.focused(id)) { hoverTrack = t; hoverQueue = pos; }
            if (k == 0) g.roundRect(kPad - 10, ry, W - kPad * 2 + 20, 60, 12, pal.accent, 0.09f);
            else if (ui.pressed(id)) g.roundRect(kPad - 10, ry, W - kPad * 2 + 20, 60, 12, pal.text, 0.065f);
            coverBox(t, kPad, ry + 6, 48, 8);
            const float textW = W - kPad * 2 - 62 - 56;
            g.text(tr.title, { css::Base, Weight::SemiBold }, kPad + 62, ry + 10, k == 0 ? pal.accent : pal.text, 1, Align::Left, int(textW));
            g.text(trackSub(t), { css::Sm }, kPad + 62, ry + 33, pal.text3, 1, Align::Left, int(textW));
            g.textC(tr.duration > 0 ? fmtTime(tr.duration) : "", { css::Sm }, W - kPad, ry + 30, pal.text3, 1, Align::Right);
        }
        g.unclip();
        ui.popArea();
        ui.region(Region::Main);
    };

    // ---- Library: what is in it as large rows, each with its round icon in a colour of the cover's; then a row of albums
    auto libraryHome = [&](float y0, float bottom) {
        ScrollArea& sa = area("ph.libhome");
        sa.view = { 0, y0, W, bottom - y0 };
        const float cs = (W - kPad * 2 - 2 * 14) / 3;
        const int albumRows = int(std::min<size_t>(lib.albums.size(), 9) + 2) / 3;
        sa.content = 110 + 6 * 58 + 46 + albumRows * (cs + 52) + 16;
        ui.pushArea(&sa);
        g.clip(0, y0, W, bottom - y0);
        float y = y0 + 16 - sa.y;
        kicker("Library", kPad, y, pal.text3);
        g.text("Library", { css::Xxl, Weight::Bold }, kPad, y + 10, pal.text);
        g.text(std::to_string(lib.tracks.size()) + " tracks  ·  " + std::to_string(lib.albums.size()) + " albums", { css::Sm }, kPad, y + 52, pal.text3);
        y += 78;
        float bx = kPad;
        bool c = false;
        bx += button(uid("ph.lplay"), ICON_PLAY, "Play", bx, y, true, &c) + 8;
        if (c && !lib.tracks.empty()) { std::vector<int> all(lib.tracks.size()); for (int i = 0; i < int(all.size()); i++) all[i] = i; playTracks(all, 0, "Library", false); }
        c = false;
        button(uid("ph.lshuffle"), ICON_SHUFFLE, "Shuffle", bx, y, false, &c);
        if (c && !lib.tracks.empty()) { std::vector<int> all(lib.tracks.size()); for (int i = 0; i < int(all.size()); i++) all[i] = i; playTracks(all, 0, "Library", true); }
        y += 58;
        struct Row { const char* icon; const char* name; int count; };
        const Row rows[7] = { { ICON_LIST, "Tracks", int(lib.tracks.size()) }, { ICON_V_COVERS, "Albums", int(lib.albums.size()) },
                              { ICON_MIC, "Artists", int(artists.size()) },     { ICON_TAG, "Genres", int(genres.size()) },
                              { ICON_FOLDER, "Folders", int(lib.folders.size()) }, { ICON_HEART, "Liked", int(store.liked.size()) },
                              { PLI_SDCARD, "Files", -1 } };
        // Files after them while the sidebar shows it (Settings › Appearance › Sidebar; the user chose this, 2026-11-04: "A and 2")
        const int nRows = !(store.hiddenTabs & Store::tabBit(Store::TabFiles)) ? 7 : 6;
        g.roundRect(kPad - 8, y, W - kPad * 2 + 16, nRows * 58, 16, pal.text, 0.04f);
        for (int i = 0; i < nRows; i++, y += 58) {
            const float cy = y + 29;
            const Id id = uid("ph.lrow", i);
            if (ui.item(id, { kPad - 8, y, W - kPad * 2 + 16, 58 }, 12)) {
                switch (i) {
                case 0: store.libTab = 1; navigate(View::Library); break;
                case 1: navigate(View::Albums); break;
                case 2: navigate(View::Artists); break;
                case 3: navigate(View::Genres); break;
                case 4: navigate(View::Folders); break;
                case 6: openFiles(filesStart()); break;
                default: navigate(View::List, -1); break;
                }
            }
            if (ui.pressed(id)) g.roundRect(kPad - 8, y, W - kPad * 2 + 16, 58, 12, pal.text, 0.06f);
            Rgb col = pal.swatchCount > 1 ? mix(pal.swatches[i % pal.swatchCount].c, pal.text, 0.25f) : pal.accent;
            // a cover's colour as dark as the page (or as light, in a light theme) left the icon unseen: nearer the text then
            auto lum = [](const Rgb& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; };
            if (std::fabs(lum(col) - lum(pal.bg)) < 0.35f && pal.swatchCount > 1) col = mix(pal.swatches[i % pal.swatchCount].c, pal.text, 0.62f);
            g.circle(kPad + 22, cy, 21, col, 0.22f);
            g.icon(rows[i].icon, kPad + 22, cy, 19, col);
            g.textC(rows[i].name, { css::Md, Weight::SemiBold }, kPad + 58, cy, pal.text);
            if (rows[i].count >= 0) g.textC(std::to_string(rows[i].count), { css::Sm }, W - kPad - 28, cy, pal.text3, 1, Align::Right);
            g.icon(ICON_CHEV, W - kPad - 8, cy, 14, pal.text3);
            if (i < nRows - 1) g.rect(kPad + 58, y + 57, W - kPad * 2 - 58, 1, pal.text, 0.07f);
        }
        y += 28;
        kicker("Albums", kPad, y, pal.text3);
        y += 18;
        for (int i = 0; i < int(std::min<size_t>(lib.albums.size(), 9)); i++) {
            const Album& al = lib.albums[size_t(i)];
            const float x = kPad + (i % 3) * (cs + 14), yy = y + (i / 3) * (cs + 52);
            const Id id = uid("ph.lalbum", i);
            if (ui.item(id, { x, yy, cs, cs + 44 }, 12)) navigate(View::Album, -1, al.key);
            if (!al.tracks.empty()) coverBox(al.tracks[0], x, yy, cs, 12);
            g.text(al.title, { css::Sm, Weight::SemiBold }, x, yy + cs + 8, pal.text, 1, Align::Left, int(cs));
            g.text(al.artist, { css::Xs }, x, yy + cs + 26, pal.text3, 1, Align::Left, int(cs));
        }
        g.unclip();
        ui.popArea();
    };

    // ---- Search: its field on top (Android's keyboard through Songate's text card), what it found under it as Songate lists it
    auto searchPage = [&](float y0, float bottom) {
        const float fy = y0 + 8;
        const Id fid = uid("ph.sfield");
        if (ui.item(fid, { kPad, fy, W - kPad * 2, 44 }, 12)) openSearch();
        const std::string& qtext = route.query;
        g.roundRect(kPad, fy, W - kPad * 2, 44, 12, pal.text, ui.pressed(fid) ? 0.1f : 0.065f);
        g.roundStroke(kPad, fy, W - kPad * 2, 44, 12, 1, qtext.empty() ? pal.text : pal.accent, qtext.empty() ? 0.09f : 0.55f);
        g.icon(ICON_SEARCH, kPad + 24, fy + 22, 18, pal.text3);
        g.textC(qtext.empty() ? "Artist, album or track" : qtext, { css::Base }, kPad + 44, fy + 22, qtext.empty() ? pal.text3 : pal.text, 1, Align::Left, int(W - kPad * 2 - 90));
        if (!qtext.empty() && iconButton(uid("ph.sclear"), ICON_X, W - kPad - 22, fy + 22, false, 36, 16)) navigate(View::Search, -1, std::string(), false);
        if (qtext.empty()) {
            g.textC("Find a song by its title, its artist or its album", { css::Sm }, W / 2, fy + 110, pal.text3, 1, Align::Center);
            return;
        }
        // its pills: what it found, as tracks (Songate's search list), albums or artists (tiles, as their pages show them)
        static std::string foundFor;
        static unsigned foundGen = ~0u;
        static std::vector<int> albumsFound, artistsFound;
        if (foundFor != qtext || foundGen != indexGen) {   // once a search, not each frame
            foundFor = qtext;
            foundGen = indexGen;
            albumsFound.clear();
            artistsFound.clear();
            for (int i = 0; i < int(lib.albums.size()); i++)
                if (strcasestr(lib.albums[i].title.c_str(), qtext.c_str()) || strcasestr(lib.albums[i].artist.c_str(), qtext.c_str())) albumsFound.push_back(i);
            for (int i = 0; i < int(artists.size()); i++)
                if (strcasestr(artists[i].name.c_str(), qtext.c_str())) artistsFound.push_back(i);
        }
        const float py = fy + 56;
        const std::string labels[3] = { "Tracks", "Albums · " + std::to_string(albumsFound.size()), "Artists · " + std::to_string(artistsFound.size()) };
        float px = kPad;
        for (int i = 0; i < 3; i++) {
            const float pw = g.measure(labels[i], { css::Sm, Weight::SemiBold }) + 28;
            const Id id = uid("ph.pill", i);
            if (ui.item(id, { px, py, pw, 34 }, 17)) phoneSearchKind_ = i;
            const bool on = phoneSearchKind_ == i;
            g.roundRect(px, py, pw, 34, 17, on ? pal.accent : pal.text, on ? 0.16f : ui.pressed(id) ? 0.11f : 0.065f);
            if (on) g.roundStroke(px, py, pw, 34, 17, 1, pal.accent, 0.45f);
            g.textC(labels[i], { css::Sm, Weight::SemiBold }, px + pw / 2, py + 17, on ? pal.accentInk : pal.text2, 1, Align::Center);
            px += pw + 8;
        }
        const float ly = py + 34 + 8;
        if (phoneSearchKind_ == 0) { drawTrackList(0, ly, W, bottom - ly); return; }
        const std::vector<int>& found = phoneSearchKind_ == 1 ? albumsFound : artistsFound;
        if (found.empty()) {
            g.textC(phoneSearchKind_ == 1 ? "No albums match" : "No artists match", { css::Base }, W / 2, ly + 60, pal.text3, 1, Align::Center);
            return;
        }
        ScrollArea& sa = area("ph.found", phoneSearchKind_);
        const float tw = (W - kPad * 2) / 2, th = 10 + (tw - 20) + 8 + 20 + 2 + 18 + 10;
        sa.view = { 0, ly, W, bottom - ly };
        sa.content = ((found.size() + 1) / 2) * th + 16;
        ui.pushArea(&sa);
        g.clip(0, ly, W, bottom - ly);
        for (int k = 0; k < int(found.size()); k++) {
            const float tx = kPad + (k % 2) * tw, ty = ly + 8 + (k / 2) * th - sa.y;
            if (ty + th < ly || ty > bottom) continue;
            if (phoneSearchKind_ == 1) {
                const Album& al = lib.albums[found[k]];
                if (tile(uid("ph.salbum", found[k]), al.tracks[0], tx, ty, tw, false, al.title, al.artist)) navigate(View::Album, -1, al.key);
            } else {
                const Artist& ar = artists[found[k]];
                const int cover = ar.albums.empty() ? ar.tracks[0] : lib.albums[ar.albums[0]].tracks[0];
                const int n = int(ar.tracks.size());
                if (tile(uid("ph.sartist", found[k]), cover, tx, ty, tw, true, ar.name, std::to_string(n) + (n == 1 ? " track" : " tracks"))) openArtist(ar.name);
            }
        }
        g.unclip();
        ui.popArea();
    };

    // ---- the page
    // An Automix join on Now playing: Full, the only one upright (the user, 2026-11-04): the cover glides to the middle of the screen
    // and grows, frosted with the hint (drawNowCover), while everything else fades out, and back as the join ends (automixFocus)
    const bool joining = v == View::Now && now.track >= 0 && mixUntil > mixShownAt && time >= mixShownAt && time < mixUntil;
    const Focus fc = joining ? automixFocus(0) : Focus{};
    phoneJoinMove_ = joining ? fc.move : -1;
    g.setFadeBase(fc.ui);   // (the page's own fades, the lyrics page's and the mini player's, under it)
    top();
    const float pageTop = kTopH, pageBottom = H - kNavH - (v == View::Now ? 0 : kMiniH + 16);
    if (v == View::Now) {
        const bool modal = ui.modal();
        if (phoneQueue_) ui.setModal(true);   // under the open sheet the page takes no touch; once it is closing, the page answers at once
        nowPlaying();
        nav();   // under the sheet, as every sheet covers it
        g.setFadeBase(1);
        if (joining && phoneLyricsP_ <= 0.01f) {   // the join's cover, over everything that faded
            const SDL_FRect r = phoneCoverRest_;
            const float big = std::min(W - 16, H - 220), m = fc.move;
            const float s = r.w + (big - r.w) * m;
            const float cx = r.x + r.w / 2 + (W / 2 - (r.x + r.w / 2)) * m, cy = r.y + r.h / 2 + (H / 2 - (r.y + r.h / 2)) * m;
            drawNowCover(cx - s / 2, cy - s / 2, s);
        }
        ui.setModal(modal || !phoneQueue_);   // and the sheet going away takes none
        if (phoneQueueP_ > 0) upNextSheet();
        ui.setModal(modal);
        return;
    }
    g.clip(0, pageTop, W, pageBottom - pageTop);
    switch (v) {
    case View::Library:
        if (route.ref == kLibraryHome) libraryHome(pageTop, pageBottom);
        else drawLibrary(0, pageTop, W, pageBottom - pageTop);
        break;
    case View::Search: searchPage(pageTop, pageBottom); break;
    case View::Albums: drawAlbums(0, pageTop, W, pageBottom - pageTop); break;
    case View::Artists: drawArtists(0, pageTop, W, pageBottom - pageTop); break;
    case View::Artist: drawArtist(0, pageTop, W, pageBottom - pageTop); break;
    case View::Genres: drawArtists(0, pageTop, W, pageBottom - pageTop, true); break;
    case View::Genre: drawArtist(0, pageTop, W, pageBottom - pageTop, true); break;
    case View::Album: drawAlbum(0, pageTop, W, pageBottom - pageTop); break;
    case View::Folders: drawFolders(0, pageTop, W, pageBottom - pageTop); break;
    case View::Settings: drawSettings(0, pageTop, W, pageBottom - pageTop); break;
    case View::Files: drawFiles(0, pageTop, W, pageBottom - pageTop); break;
    case View::Playlists: drawPlaylists(0, pageTop, W, pageBottom - pageTop); break;
    case View::PickFolder:
    case View::PickPlaylist:
    case View::PickExport: drawPicker(0, pageTop, W, pageBottom - pageTop); break;
    default: drawTrackList(0, pageTop, W, pageBottom - pageTop); break;
    }
    g.unclip();
    mini();
    nav();
}
#endif
