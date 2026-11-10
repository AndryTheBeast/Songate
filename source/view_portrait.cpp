/*
 * Songate · view_portrait.cpp (dev only: Android's GF_DEV builds, never shipped)
 * Mock-ups of the portrait layout for the user to choose from (session 12; the user: "The Android App will have a portrait design
 * stylised portrait ease of use ... using the UI language from songate", docs/next-session/android-design-notes.md): drawn with
 * Songate's own pieces (its fonts, icons, palette, chips, buttons, the waveform seek bar, the wordmark) from the real library and the
 * song playing, at the size landscape has. Not interactive: pictures of the layout, chosen by portrait-mock.txt in the settings folder
 * (0 Now playing A, 1 Now playing B, 2 Library, 3 Search, 4 a sheet, 5 Settings).
 */
#if defined(GF_ANDROID) && defined(GF_DEV)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "app.hpp"
#include "icons.inc"

namespace {
constexpr float kPad = 18;      // the page's side margin
constexpr float kNavH = 68;     // the bottom bar
constexpr float kMiniH = 64;    // the mini player
}  // namespace

void App::drawPortraitMock(int which) {
    const float W = float(css::W), H = float(css::H);
    const bool has = now.track >= 0;
    const TrackInfo& in = now.info;
    auto mix = [](const Rgb& p, const Rgb& q, float t) { return Rgb{ p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t }; };
    auto cover = [&](int track, float x, float y, float s, float r) {
        AlbumArt& a = meta.art(track);
        if (a.state == AlbumArt::Ready) g.image(a.tex, x, y, s, s, r);
        else {
            g.roundRect(x, y, s, s, r, pal.surface2);
            g.icon(ICON_MUSIC, x + s / 2, y + s / 2, s * 0.4f, pal.text3, 0.6f);
        }
    };
    auto hair = [&](float x, float y, float w) { g.rect(x, y, w, 1, pal.text, 0.07f); };

    // ---- the pieces every page shares
    // The top: Songate's wordmark, the clock and the battery, as the title bar has them (the phone's own status bar is hidden)
    auto top = [&](bool overCover) {
        if (overCover) g.rectV(0, 0, W, 120, pal.bg, 0.75f, pal.bg, 0);
        g.wordmark(kPad - 2, 30, mix(pal.accent, pal.text, 0.12f), mix(pal.accent2, pal.text, 0.2f), mix(pal.accent2, pal.text, 0.78f), pal.glow, 0.4f, -1, 1);
        const std::time_t tt = std::time(nullptr);
        char clock[8];
        std::strftime(clock, sizeof clock, "%H:%M", std::localtime(&tt));
        unsigned pct = 0;
        float rx = W - kPad;
        if (plat::batteryLevel(pct)) {
            g.roundStroke(rx - 24, 24, 24, 12, 3, 1.5f, pal.text2);
            g.roundRect(rx - 21.5f, 26.5f, 19 * pct / 100.f, 7, 1.5f, pal.text2);
            rx -= 34;
        }
        g.textC(clock, { css::Sm, Weight::SemiBold }, rx, 30, pal.text2, 1, Align::Right);
    };
    // The bottom bar: the sidebar's destinations, a phone's five, as the collapsed sidebar shows them: icons, the one here tinted
    auto nav = [&](int active) {
        const float y0 = H - kNavH;
        g.rect(0, y0, W, kNavH, pal.bg, 0.86f);
        g.rect(0, y0, W, 1, pal.text, 0.09f);
        static const char* const items[5] = { ICON_DISC, ICON_GRID, ICON_SEARCH, ICON_HEART, ICON_GEAR };
        const float cw = W / 5;
        for (int i = 0; i < 5; i++) {
            const float cx = cw * i + cw / 2, cy = y0 + kNavH / 2;
            const bool on = i == active;
            if (on) g.roundRect(cx - 34, cy - 20, 68, 40, 20, pal.accent, 0.14f);
            g.icon(items[i], cx, cy, 24, on ? pal.accent : pal.text2);
        }
    };
    // Glass, as Songate's hints and the Canvas's controls are: a dark, see-through pill over a picture
    auto glass = [&](float x, float y, float w, float h) {
        g.roundRect(x, y, w, h, h / 2, pal.bg, 0.6f);
        g.roundStroke(x, y, w, h, h / 2, 1, pal.text, 0.12f);
    };
    auto glassButton = [&](const char* icon, float cx, float cy, float d, bool on = false) {
        glass(cx - d / 2, cy - d / 2, d, d);
        g.icon(icon, cx, cy, d * 0.46f, on ? pal.accent : pal.text);
    };
    // The mini player: the player bar folded to one row, a card over the page, its progress a thin line along its foot
    auto mini = [&]() {
        const float x = 10, y = H - kNavH - kMiniH - 8, w = W - 20, h = kMiniH, cy = y + h / 2;
        g.roundRect(x, y, w, h, 16, pal.surface, 0.92f);
        g.roundStroke(x, y, w, h, 16, 1, pal.text, 0.09f);
        for (float r : { 28.f, 32.f }) g.glow(x + 12 + 22, cy, r, pal.glow, 0.22f);
        if (now.cover) g.image(now.cover, x + 12, cy - 22, 44, 44, 8);
        else { g.roundRect(x + 12, cy - 22, 44, 44, 8, pal.surface2); g.icon(ICON_MUSIC, x + 34, cy, 18, pal.text3); }
        const float tx = x + 12 + 44 + 12, tw = w - (tx - x) - 130;
        marqueeLine(has ? in.title : "—", { css::Base, Weight::SemiBold }, tx, cy - 20, tw, pal.text);
        marqueeLine(has ? in.artist : "", { css::Sm }, tx, cy + 1, tw, pal.text3);
        const bool liked = has && store.isLiked(lib.tracks[now.track].path);
        g.icon(liked ? ICON_HEART_F : ICON_HEART, x + w - 112, cy, 20, liked ? pal.accent : pal.text2);
        for (float r : { 22.f, 25.f }) g.glow(x + w - 66, cy, r, pal.glow, 0.25f);
        g.circle(x + w - 66, cy, 19, pal.text);
        g.icon(isPlaying() ? ICON_PAUSE : ICON_PLAY, x + w - 66, cy, 17, pal.bg);
        g.icon(ICON_NEXT, x + w - 26, cy, 20, pal.text2);
        const double dur = in.duration;
        const float frac = dur > 0 ? float(std::clamp(shownPosition() / dur, 0.0, 1.0)) : 0;
        g.roundRect(x + 16, y + h - 4, w - 32, 2, 1, pal.text, 0.12f);
        g.roundRectH(x + 16, y + h - 4, (w - 32) * frac, 2, 1, pal.accent2, pal.accent);
    };
    // The seek bar on a phone (the user, 2026-11-04: "the scrub will turn into a waveform animation after no touch is sensed", "Make
    // the transition seamless", "The waveform animation is the one from canvas and now playing, with the bars that react to music"):
    // touched, Songate's seek bar (the song's waveform, its played part lit); let go, it becomes Now playing's visualizer. One shape
    // between them: each of the visualizer's bars is the seek bar's thin bars in its stretch, which slide together and widen until they
    // close into it, move from the middle down onto its foot, take its height from the music and its colours; and back. The last of it
    // is drawSpectrum itself. (Mock: a touch acted out every 6 s, 2 s long, for the recording.)
    auto seekBar = [&](float x, float y, float w, float h) {
        static float p = 1;   // 0 the seek bar, 1 the visualizer
        const bool touched = std::fmod(time, 6.0) < 2.0;
        const float target = touched ? 0.f : 1.f;
        const float step = store.motion ? dt / 0.5f : 1.f;
        p = target > p ? std::min(target, p + step) : std::max(target, p - step);
        const float e = p * p * (3 - 2 * p);   // eased both ways
        if (e >= 0.999f && spec.bands > 0) { drawSpectrum(x, y, w, h); return; }
        const double dur = in.duration;
        const float frac = dur > 0 ? float(std::clamp(shownPosition() / dur, 0.0, 1.0)) : 0;
        const float cy = y + h / 2, foot = y + h;
        const bool waveNow = store.seekWave && wave.ready() && has && !playingFile().empty() && wave.path() == playingFile();
        const auto& pk = wave.peaks();
        const int bars = int(w / 3);
        // the visualizer's columns, as drawSpectrum lays them
        const int n = std::max(1, spec.bands);
        const float gap = std::max(2.f, std::round(w / n * 0.28f)), colW = (w - gap * (n - 1)) / n;
        for (int i = 0; i < bars; i++) {
            float v = 0.3f;
            if (waveNow && !pk.empty()) {
                const size_t a = size_t(i) * pk.size() / bars, b = std::max(a + 1, size_t(i + 1) * pk.size() / bars);
                v = 0;
                for (size_t j = a; j < b && j < pk.size(); j++) v = std::max(v, pk[j]);
            }
            const float seekH = std::max(1.5f, v * h * 0.9f);
            // its column, and its place within it
            const int k = std::min(n - 1, i * n / bars), i0 = (k * bars + n - 1) / n, i1 = ((k + 1) * bars + n - 1) / n;
            const int cnt = std::max(1, i1 - i0), at = i - i0;
            const float colX = x + k * (colW + gap);
            const float specH = std::max(2.f, (spec.bands > 0 ? spec.bars[k] : 0.f) * h * 0.94f);
            // from the seek bar's place and size to its share of the column
            const float bx = x + i * 3 + (colX + at * colW / cnt - (x + i * 3)) * e;
            const float bw = 2 + (colW / cnt + 0.5f - 2) * e;
            const float bh = seekH + (specH - seekH) * e;
            const float by = (cy - seekH / 2) + ((foot - specH) - (cy - seekH / 2)) * e;
            // the colours: played (the gradient along it) or not (dim) → the visualizer's (its gradient by height)
            const float t = float(i) / bars;
            const bool played = x + i * 3 < x + frac * w;
            const Rgb seekC = played ? Rgb{ pal.accent2.r + (pal.accent.r - pal.accent2.r) * t, pal.accent2.g + (pal.accent.g - pal.accent2.g) * t,
                                            pal.accent2.b + (pal.accent.b - pal.accent2.b) * t }
                                     : pal.text;
            const float seekA = played ? 1.f : 0.13f;
            const float ht = specH / h;
            const Rgb specC{ pal.accent2.r + (pal.accent.r - pal.accent2.r) * ht, pal.accent2.g + (pal.accent.g - pal.accent2.g) * ht,
                             pal.accent2.b + (pal.accent.b - pal.accent2.b) * ht };
            const Rgb c{ seekC.r + (specC.r - seekC.r) * e, seekC.g + (specC.g - seekC.g) * e, seekC.b + (specC.b - seekC.b) * e };
            g.rect(bx, by, bw, bh, c, seekA + (1 - seekA) * e);
        }
        // the visualizer's own bars (their rounded tops) and its peak caps come in over the last part, so nothing appears at once
        const float late = std::clamp((e - 0.55f) / 0.45f, 0.f, 1.f);
        if (late > 0 && spec.bands > 0)
            for (int k = 0; k < n; k++) {
                const float bh = std::max(2.f, spec.bars[k] * h * 0.94f), bx = x + k * (colW + gap), t = bh / h;
                const Rgb top{ pal.accent2.r + (pal.accent.r - pal.accent2.r) * t, pal.accent2.g + (pal.accent.g - pal.accent2.g) * t,
                               pal.accent2.b + (pal.accent.b - pal.accent2.b) * t };
                g.roundRectV(bx, foot - bh, colW, bh, std::min({ colW / 2, 4.f, bh / 2 }), top, pal.accent2, late);
                if (store.vizPeaks) g.rect(bx, std::max(y, foot - spec.peaks[k] * h * 0.94f - 3), colW, 1.5f, pal.accent, 0.55f * late);
            }
    };
    auto header = [&](const char* kick, const std::string& title, const std::string& sub, float y) {
        kicker(kick, kPad, y, pal.text3);
        g.text(title, { css::Xxl, Weight::Bold }, kPad, y + 10, pal.text, 1, Align::Left, int(W - kPad * 2));
        if (!sub.empty()) g.text(sub, { css::Sm }, kPad, y + 52, pal.text3, 1, Align::Left, int(W - kPad * 2));
    };
    auto searchField = [&](float y, const std::string& q, const char* hintText) {
        g.roundRect(kPad, y, W - kPad * 2, 44, 12, pal.text, 0.065f);
        g.roundStroke(kPad, y, W - kPad * 2, 44, 12, 1, q.empty() ? pal.text : pal.accent, q.empty() ? 0.09f : 0.55f);
        g.icon(ICON_SEARCH, kPad + 24, y + 22, 18, pal.text3);
        g.textC(q.empty() ? hintText : q, { css::Base }, kPad + 44, y + 22, q.empty() ? pal.text3 : pal.text);
        if (!q.empty()) g.icon(ICON_X, W - kPad - 22, y + 22, 16, pal.text3);
    };
    // A row of a list: a cover, two lines, a time; the one playing in the accent
    auto trackRow = [&](int t, float y) {
        const Track& tr = lib.tracks[t];
        cover(t, kPad, y + 8, 48, 8);
        const bool here = t == now.track;
        g.text(tr.title, { css::Base, Weight::SemiBold }, kPad + 62, y + 12, here ? pal.accent : pal.text, 1, Align::Left, int(W - kPad * 2 - 62 - 60));
        g.text(tr.artist + (tr.album.empty() ? "" : " · " + tr.album), { css::Sm }, kPad + 62, y + 36, pal.text3, 1, Align::Left, int(W - kPad * 2 - 62 - 60));
        g.textC(fmtTime(tr.duration), { css::Sm }, W - kPad, y + 32, pal.text3, 1, Align::Right);
    };

    // ---- Now playing: the cover is the screen's top, the song's name and its buttons on it under the thumb (the user, 2026-11-04:
    // "something like poweramp, a nice portrait player", "No up next visible, up next should be like a icon that then open the up
    // next queue"); under it a row of small round buttons; the lower half the seek bar and the transport, large
    // lp: 0 Now playing, 1 its lyrics page (the user, 2026-11-04: "there should be a lyrics button that then transitions to the lyrics
    // page with a smaller cover"): the cover glides from the page's width to a small square at the top left; what is on it and under it
    // fades and settles away; the song's name beside the small cover, the lyrics and the mini player come in
    auto nowPlaying = [&](float lp) {
        top(false);
        const float lpe = lp < 0.5f ? 4 * lp * lp * lp : 1 - std::pow(-2 * lp + 2, 3.f) / 2;   // the Canvas's layout morph's cubic
        // what leaves fades over the first half, what comes over the last 60 %: they overlap, the page never empty but for the cover
        const float out = std::clamp(1 - lp / 0.5f, 0.f, 1.f), in2 = std::clamp((lp - 0.4f) / 0.6f, 0.f, 1.f);
        const float bigS = W - 32, smallS = 72;
        const float cs = bigS + (smallS - bigS) * lpe, cx0 = 16 + (kPad - 16) * lpe, cy0 = 60 + (66 - 60) * lpe, cr = 22 + (10 - 22) * lpe;
        g.setFade(1 - lpe);
        for (float r : { cs * 0.5f, cs * 0.56f }) g.glow(cx0 + cs / 2, cy0 + cs / 2, r, pal.glow, 0.18f);
        g.setFade(1);
        if (now.cover) g.image(now.cover, cx0, cy0, cs, cs, cr);
        else g.roundRect(cx0, cy0, cs, cs, cr, pal.surface2);
        g.roundStroke(cx0, cy0, cs, cs, cr, 1, pal.text, 0.09f);
        // the lyrics page's: the name beside the small cover, its menu, the lyrics, the mini player
        if (in2 > 0) {
            g.setFade(in2);
            const float tx = kPad + smallS + 14, tw2 = W - tx - kPad - 44, dy = 12 * (1 - in2);
            g.text(has ? in.title : "Songate", { css::Lg, Weight::Bold }, tx, 70 + dy, pal.text, 1, Align::Left, int(tw2));
            g.text(has ? in.artist : "", { css::Sm, Weight::SemiBold }, tx, 98 + dy, pal.text2, 1, Align::Left, int(tw2));
            g.text(fmtTime(in.duration) + (in.codec.empty() ? "" : "  ·  " + in.codec), { css::Xs }, tx, 120 + dy, pal.text3, 1, Align::Left, int(tw2));
            iconButton(uid("pm.lmore"), ICON_MORE, W - kPad - 18, 102, false, 40, 20);
            // the lyrics: Songate's own look, the line sung bright, the others dimmed; the line sung a third of the way down
            const float top0 = 160, bottom0 = H - kNavH - kMiniH - 16, lw = W - kPad * 2;
            const Font lf{ css::Xl, Weight::SemiBold };
            const float lh = std::round(lf.size * 1.32f);
            g.clip(0, top0, W, bottom0 - top0);
            if (now.lyrics.empty()) {
                g.textC("No lyrics for this song", { css::Base }, W / 2, (top0 + bottom0) / 2, pal.text3, 1, Align::Center);
            } else {
                const double at = shownPosition() + now.lrcOffset;
                int cur = 0;
                if (now.synced) for (int i = 0; i < int(now.lyrics.size()); i++) if (now.lyrics[i].t <= at) cur = i;
                std::vector<std::vector<std::string>> wrapped;
                for (const auto& l : now.lyrics) wrapped.push_back(g.wrap(l.text.empty() ? " " : l.text, lf, int(lw), 4));
                float curY = 0, yy = 0;
                for (int i = 0; i < int(wrapped.size()); i++) { if (i == cur) curY = yy; yy += wrapped[i].size() * lh + 12; }
                float ly = top0 + (bottom0 - top0) * 0.32f - curY + 30 * (1 - in2);
                for (int i = 0; i < int(wrapped.size()); i++) {
                    const bool sung = !now.synced || i == cur;
                    for (const auto& line : wrapped[i]) {
                        if (ly > top0 - lh && ly < bottom0) g.text(line, lf, W / 2, ly, pal.text, sung ? 1.f : 0.38f, Align::Center);
                        ly += lh;
                    }
                    ly += 12;
                }
            }
            g.unclip();
            g.setFade(1);
        }
        g.setFade(out);
        // on the cover's foot: the name and the artist in glass, the heart left, Lyrics, Up next and the song's menu right
        const float by = cy0 + cs - 16 - 22;   // the buttons' row (centres)
        const bool liked = has && store.isLiked(lib.tracks[now.track].path);
        glassButton(liked ? ICON_HEART_F : ICON_HEART, cx0 + 16 + 22, by, 44, liked);
        glassButton(ICON_MORE, cx0 + cs - 16 - 22, by, 44);
        glassButton(ICON_QUEUE, cx0 + cs - 16 - 22 - 52, by, 44);
        glassButton(ICON_QUOTE, cx0 + cs - 16 - 22 - 104, by, 44, lp > 0);
        const Font tf{ css::Lg, Weight::Bold }, af{ css::Sm, Weight::SemiBold };
        const float maxW = cs - 32 - 28;
        const std::string artistText = has ? in.artist + (in.album.empty() || in.album == in.title ? "" : " · " + in.album) : "";
        const float aw = std::min(maxW, float(g.measure(artistText, af))) + 28, ay = by - 22 - 10 - 32;
        glass(cx0 + 16, ay, aw, 32);
        g.textC(artistText, af, cx0 + 30, ay + 16, pal.text2, 1, Align::Left, int(aw - 28));
        const std::string titleText = has ? in.title : "Songate";
        const float tw = std::min(maxW, float(g.measure(titleText, tf))) + 32, ty = ay - 8 - 44;
        glass(cx0 + 16, ty, tw, 44);
        g.textC(titleText, tf, cx0 + 32, ty + 22, pal.text, 1, Align::Left, int(tw - 32));
        // under the cover: the player bar's smaller buttons, round, two each side (settling down as the lyrics come)
        float y = 60 + bigS + 44 + 40 * lpe;
        glassButton(ICON_CANVAS, kPad + 20, y, 40);
        glassButton(ICON_MOON, kPad + 20 + 52, y, 40);
        glassButton(ICON_EQ, W - kPad - 20 - 52, y, 40);
        static const char* const orderIcons[4] = { ICON_ORDER, ICON_REPEAT, ICON_REPEAT1, ICON_SHUFFLE };
        glassButton(orderIcons[std::clamp(int(shownOrder()), 0, 3)], W - kPad - 20, y, 40, shownOrder() != Order::Default);
        // the lower half: the waveform seek bar, its times, the transport large, and what the file is
        // (spread evenly between the small buttons and the bar: on a tall phone the transport sits where the thumb rests)
        const float room = (H - kNavH) - (y + 30), gapK = room / 9;
        y += 30 + gapK * 1.6f;
        seekBar(kPad, y, W - kPad * 2, 40);
        g.text(fmtTime(shownPosition()), { css::Xs }, kPad, y + 48, pal.text3);
        g.text(fmtTime(in.duration), { css::Xs }, W - kPad, y + 48, pal.text3, 1, Align::Right);
        y += 40 + gapK * 3.2f;
        const float mid = W / 2;
        iconButton(uid("pm.sac"), ICON_PAUSE_C, kPad + 20, y, false, 44, 20);
        iconButton(uid("pm.prev"), ICON_PREV, mid - 96, y, false, 60, 30);
        for (float r : { 50.f, 56.f, 62.f }) g.glow(mid, y, r, pal.glow, 0.3f);
        g.circle(mid, y, 46, pal.text);
        g.icon(isPlaying() ? ICON_PAUSE : ICON_PLAY, mid, y, 38, pal.bg);
        iconButton(uid("pm.next"), ICON_NEXT, mid + 96, y, false, 60, 30);
        iconButton(uid("pm.devices"), ICON_LINK, W - kPad - 20, y, false, 44, 20);
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
        if (in2 > 0) {   // the mini player, risen from the bar
            g.setFade(in2);
            mini();
            g.setFade(1);
        }
        nav(0);
    };
    // ---- Up next: the queue as a sheet, opened by its button on the cover (the Playlist page's rows, the one playing lit)
    auto upNext = [&]() {
        nowPlaying(0);
        g.rect(0, 0, W, H, { 0, 0, 0 }, 0.55f);
        const float sh = H * 0.62f, y0 = H - sh;
        g.roundRect(0, y0, W, sh + 40, 24, pal.surface, 0.98f);
        g.roundStroke(0, y0, W, sh + 40, 24, 1, pal.text, 0.09f);
        g.roundRect(W / 2 - 20, y0 + 10, 40, 5, 2.5f, pal.text, 0.25f);
        kicker("Playlist", kPad, y0 + 40, pal.text3);
        g.text("Up next", { css::Xl, Weight::Bold }, kPad, y0 + 52, pal.text);
        g.textC(std::to_string(q.order.size()) + " tracks", { css::Sm }, W - kPad - 56, y0 + 68, pal.text3, 1, Align::Right);
        iconButton(uid("pm.qclose"), ICON_X, W - kPad - 16, y0 + 68, false, 40, 18);
        float y = y0 + 104;
        hair(kPad, y, W - kPad * 2);
        y += 6;
        for (int k = 0; k < 12 && y + 64 < H; k++) {
            const int p = q.pos + k;
            if (p >= int(q.order.size())) break;
            if (k == 0) g.roundRect(kPad - 10, y + 2, W - kPad * 2 + 20, 60, 12, pal.accent, 0.09f);
            trackRow(q.tracks[q.order[p]], y);
            y += 64;
        }
    };

    // ---- Library: what is in it as large rows, each with its round icon, then albums
    auto library = [&]() {
        top(false);
        header("Library", "Library", std::to_string(lib.tracks.size()) + " tracks  ·  " + std::to_string(lib.albums.size()) + " albums", 76);
        float bx = kPad;
        bool c = false;
        bx += button(uid("pm.play"), ICON_PLAY, "Play", bx, 152, true, &c) + 8;
        button(uid("pm.shuffle"), ICON_SHUFFLE, "Shuffle", bx, 152, false, &c);
        struct Row { const char* icon; const char* name; int count; };
        const Row rows[6] = { { ICON_LIST, "Songs", int(lib.tracks.size()) }, { ICON_V_COVERS, "Albums", int(lib.albums.size()) },
                              { ICON_MIC, "Artists", int(artists.size()) }, { ICON_TAG, "Genres", int(genres.size()) },
                              { ICON_FOLDER, "Folders", int(lib.folders.size()) }, { ICON_HEART, "Liked", int(store.liked.size()) } };
        float y = 210;
        g.roundRect(kPad - 8, y, W - kPad * 2 + 16, 6 * 58, 16, pal.text, 0.04f);
        for (int i = 0; i < 6; i++, y += 58) {
            const float cy = y + 29;
            // each its own colour, from the cover's swatches (Palette), the accent when there are few
            const Rgb c = pal.swatchCount > 1 ? mix(pal.swatches[i % pal.swatchCount].c, pal.text, 0.25f) : pal.accent;
            g.circle(kPad + 22, cy, 21, c, 0.22f);
            g.icon(rows[i].icon, kPad + 22, cy, 19, c);
            g.textC(rows[i].name, { css::Md, Weight::SemiBold }, kPad + 58, cy, pal.text);
            g.textC(std::to_string(rows[i].count), { css::Sm }, W - kPad - 28, cy, pal.text3, 1, Align::Right);
            g.icon(ICON_CHEV, W - kPad - 8, cy, 14, pal.text3);
            if (i < 5) hair(kPad + 58, y + 57, W - kPad * 2 - 58);
        }
        y += 28;
        kicker("Albums", kPad, y, pal.text3);
        y += 18;
        const float cs = (W - kPad * 2 - 2 * 14) / 3;
        for (int i = 0; i < 6 && i < int(lib.albums.size()); i++) {
            const Album& al = lib.albums[size_t(i) * 7 % lib.albums.size()];
            const float x = kPad + (i % 3) * (cs + 14), yy = y + (i / 3) * (cs + 52);
            if (yy + cs > H - kNavH - kMiniH - 20) break;
            if (!al.tracks.empty()) cover(al.tracks[0], x, yy, cs, 12);
            g.text(al.title, { css::Sm, Weight::SemiBold }, x, yy + cs + 8, pal.text, 1, Align::Left, int(cs));
            g.text(al.artist, { css::Xs }, x, yy + cs + 26, pal.text3, 1, Align::Left, int(cs));
        }
        mini();
        nav(1);
    };

    // ---- Search: the field on top, its pills, Play and Shuffle for what it found, the results grouped
    auto search = [&]() {
        top(false);
        // a word of the artist playing, as someone would type it
        std::string qtext = has ? in.artist.substr(0, in.artist.find_first_of(" ,&")) : "a";
        if (qtext.empty()) qtext = "a";
        searchField(64, qtext, searchHint());
        static const char* pills[6] = { "All", "Songs", "Albums", "Artists", "Folders", "Lyrics" };
        float px = kPad;
        for (int i = 0; i < 6; i++) {
            const float pw = g.measure(pills[i], { css::Sm, Weight::SemiBold }) + 28;
            g.roundRect(px, 124, pw, 32, 16, i == 0 ? pal.accent : pal.text, i == 0 ? 0.16f : 0.065f);
            g.textC(pills[i], { css::Sm, Weight::SemiBold }, px + pw / 2, 140, i == 0 ? pal.accent : pal.text2, 1, Align::Center);
            px += pw + 8;
        }
        std::vector<int> found;
        for (int t = 0; t < int(lib.tracks.size()) && found.size() < 6; t++) {
            const std::string& a = lib.tracks[t].artist;
            if (strcasestr(a.c_str(), qtext.c_str())) found.push_back(t);
        }
        float bx = kPad;
        bool c = false;
        bx += button(uid("pm.splay"), ICON_PLAY, "Play", bx, 176, true, &c) + 8;
        button(uid("pm.sshuffle"), ICON_SHUFFLE, "Shuffle", bx, 176, false, &c);
        g.textC(std::to_string(found.size()) + (found.size() == 1 ? " track" : " tracks"), { css::Sm }, W - kPad, 193, pal.text3, 1, Align::Right);
        float y = 236;
        kicker("Artists", kPad, y, pal.text3);
        y += 16;
        if (!found.empty()) {
            const float s = 52;
            AlbumArt& a = meta.art(found[0]);
            if (a.state == AlbumArt::Ready) g.image(a.tex, kPad, y, s, s, s / 2);
            else g.circle(kPad + s / 2, y + s / 2, s / 2, pal.surface2);
            g.textC(lib.tracks[found[0]].artist, { css::Md, Weight::SemiBold }, kPad + s + 14, y + s / 2, pal.text);
        }
        y += 72;
        kicker("Songs", kPad, y, pal.text3);
        y += 10;
        for (int t : found) {
            if (y + 64 > H - kNavH - kMiniH - 16) break;
            trackRow(t, y);
            y += 64;
        }
        mini();
        nav(2);
    };

    // ---- a sheet: the track menu risen from the foot of the page, the mini player and the bar under its veil
    auto sheetMock = [&]() {
        library();
        g.rect(0, 0, W, H, { 0, 0, 0 }, 0.55f);
        const float sh = 640, y0 = H - sh;
        g.roundRect(0, y0, W, sh + 40, 24, pal.surface, 0.98f);
        g.roundStroke(0, y0, W, sh + 40, 24, 1, pal.text, 0.09f);
        g.roundRect(W / 2 - 20, y0 + 10, 40, 5, 2.5f, pal.text, 0.25f);
        float y = y0 + 30;
        if (now.cover) g.image(now.cover, kPad, y, 64, 64, 10);
        g.text(has ? in.title : "", { css::Lg, Weight::SemiBold }, kPad + 80, y + 8, pal.text, 1, Align::Left, int(W - kPad * 2 - 140));
        g.text(has ? in.artist + " · " + in.album : "", { css::Sm }, kPad + 80, y + 38, pal.text3, 1, Align::Left, int(W - kPad * 2 - 140));
        iconButton(uid("pm.close"), ICON_X, W - kPad - 16, y + 32, false, 40, 18);
        y += 84;
        hair(kPad, y, W - kPad * 2);
        struct Act { const char* icon; const char* label; bool danger; };
        static const Act acts[9] = { { ICON_PLAY, "Play next", false }, { ICON_QUEUE, "Add to the queue", false }, { ICON_PLUS, "Add to a playlist", false },
                                     { ICON_HEART, "Add to Liked", false }, { ICON_V_COVERS, "Go to the album", false }, { ICON_MIC, "Go to the artist", false },
                                     { ICON_TAG, "Edit its tags", false }, { ICON_INFO, "Details", false }, { ICON_X, "Delete", true } };
        y += 6;
        for (const Act& a : acts) {
            const Rgb red{ 0.9f, 0.38f, 0.31f };
            g.icon(a.icon, kPad + 14, y + 26, 20, a.danger ? red : pal.text2);
            g.textC(a.label, { css::Md }, kPad + 48, y + 26, a.danger ? red : pal.text);
            y += 56;
        }
    };

    // ---- Settings: its categories as large rows, an icon, a name and one line of what is inside
    auto settings = [&]() {
        top(false);
        header("Songate", "Settings", "", 76);
        searchField(140, "", "Find a setting");
        struct Cat { const char* icon; const char* name; const char* what; };
        static const Cat cats[12] = {
            { ICON_SPARKLE, "Appearance", "Colours, size, the sidebar, screen edges" }, { ICON_PLAY, "Playback", "Order, crossfade, levelling, sleep" },
            { ICON_EQ, "Equalizer", "Ten bands, bass, treble, presets" },             { ICON_GRID, "Library", "The music folder, rescan, covers" },
            { ICON_FOLDER, "Storage", "Where browsers open, what is kept" },            { ICON_LIST, "Lyrics", "The Canvas's lyrics, timing" },
            { ICON_CANVAS, "Canvas", "Layouts, what it shows" },                         { ICON_WAVE, "Visualizer", "Bands, colours, motion" },
            { ICON_SLIDERS, "Controls", "Buttons, the pointer" },                        { ICON_LINK, "Devices", "Pairing, together, AirPlay" },
            { ICON_MUSIC, "Last.fm", "Scrobbles and now playing" },                      { ICON_REFRESH, "Reset", "Settings or everything" } };
        float y = 204;
        g.roundRect(kPad - 8, y, W - kPad * 2 + 16, 12 * 68, 16, pal.text, 0.04f);
        for (int i = 0; i < 12; i++, y += 68) {
            if (y + 68 > H - kNavH - kMiniH - 16) break;
            const float cy = y + 34;
            g.roundRect(kPad + 2, cy - 20, 40, 40, 12, pal.text, 0.065f);
            g.icon(cats[i].icon, kPad + 22, cy, 19, pal.text2);
            g.text(cats[i].name, { css::Md, Weight::SemiBold }, kPad + 58, cy - 21, pal.text);
            g.text(cats[i].what, { css::Sm }, kPad + 58, cy + 2, pal.text3, 1, Align::Left, int(W - kPad * 2 - 100));
            g.icon(ICON_CHEV, W - kPad - 8, cy, 14, pal.text3);
            if (i < 11) hair(kPad + 58, y + 67, W - kPad * 2 - 58);
        }
        mini();
        nav(4);
    };

    switch (which) {
    case 0: nowPlaying(0); break;
    case 1: upNext(); break;
    case 2: library(); break;
    case 3: search(); break;
    case 4: sheetMock(); break;
    case 6: {   // Now playing and its lyrics page, the button acted out every 5 s, for the recording
        static float lp = 0;
        const float target = std::fmod(time, 10.0) >= 5.0 ? 1.f : 0.f, step = store.motion ? dt / 0.7f : 1.f;
        lp = target > lp ? std::min(target, lp + step) : std::max(target, lp - step);
        nowPlaying(lp);
        break;
    }
    default: settings(); break;
    }
}
#endif
