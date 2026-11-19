/*
 * Songate · view_setup.cpp
 * SongateOS's first-boot setup (cap `setup`, service v52; the screens and their words the user chose, 2026-11-18): Welcome, Wi-Fi,
 * Time zone, Music, Sound, Colour, Hold switch, Bluetooth, Updates (the user, 2026-11-18), Done. The whole screen while it shows; each screen in between is
 * Settings' own cards (drawSettings with setupPane_), so a row works and reads as it does there. Shown at the start when the
 * service says the setup is not done; setup_done on the last screen (Android's setup flags with it: the bars come back).
 */
#include "app.hpp"

#include <algorithm>
#include <cmath>

#include "sysapi.hpp"

#ifdef GF_ANDROID
#include "hibyr4.hpp"
#include "icons.inc"
#endif

namespace {
constexpr int kSteps = 10;   // 0 Welcome … 9 Done; the dots count the eight between
constexpr int kWelcome = 0, kWifi = 1, kZone = 2, kMusic = 3, kSound = 4, kColour = 5, kHold = 6, kBt = 7, kUpdates = 8, kDone = 9;

std::string lower(std::string s) {
    for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string gmt(int minutes) {
    if (minutes == 0) return "GMT+0";
    char b[16];
    const int a = std::abs(minutes);
    if (a % 60) snprintf(b, sizeof b, "GMT%c%d:%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(b, sizeof b, "GMT%c%d", minutes < 0 ? '-' : '+', a / 60);
    return b;
}
}  // namespace

#ifdef GF_ANDROID

void App::setupGo(int step) {
    if (setupStep_ == kBt && step != kBt && sysapi::bt().discovering) sysapi::act("bt_discover", "stop", {}, "bt");
    if (step == kUpdates && !sysapi::has("ota")) step = setupStep_ < kUpdates ? kDone : kBt;   // (no updater: no Updates screen)
    SDL_Log("setup: screen %d to %d", setupStep_, step);   // (in the log: what the setup did, on a real device)
    setupFrom_ = setupStep_;
    setupStep_ = step;
    setupAt_ = time;
    area("settings.pane").target = area("settings.pane").y = 0;
    area("setup.zones").target = area("setup.zones").y = 0;
    if (step == kWifi) connTab_ = 0;
    if (step == kBt) {
        connTab_ = 1;
    }
    if (step == kSound && r4::on()) r4::refresh();
    ui.focusDefault();
}

void App::setupTick() {
    if (!r4Dev_) return;
    // Songate's own first start, held (App::init): now when there is no setup to wait for (no service, an older one), or once the
    // service says it is done; a setup due goes first, and it waits until the setup ends
    if (firstHeld_ && setupStep_ < 0 && !setupFinishing_) {
        const sysapi::Avail av = sysapi::avail();
        const bool noSetup = av == sysapi::Avail::No || (av == sysapi::Avail::Yes && !sysapi::has("setup"));
        if (noSetup || (setupChecked_ && sysapi::setup().known && sysapi::setup().done)) {
            SDL_Log("setup: Songate's own first start now (%s)", noSetup ? "no setup" : "the setup done");
            firstStartNow();
        }
    }
    // At the start: the service asked once whether its setup is due (it says when it is known; an older service has no `setup`)
    if (!setupChecked_ && sysapi::has("setup")) {
        sysapi::watch("setup", true);
        const sysapi::Setup s = sysapi::setup();
        if (s.known) {
            setupChecked_ = true;
            SDL_Log("setup: done=%d, showing=%d (notices: covers %d/%d, lyrics %d, tour %d)", s.done, !s.done, appCoverNotice, coverNotice,
                    lyrNotice, tourStep);
            if (!s.done) setupGo(kWelcome);
            else sysapi::watch("setup", false);
        } else if (time - setupAskedAt_ >= 2) {   // no answer yet (the service still starting): asked again every 2 s
            setupAskedAt_ = time;
            sysapi::readAgain("setup");
        }
    }
    if (setupFinishing_) {   // setup_done's answer (its area read again after it)
        const sysapi::Setup s = sysapi::setup();
        if (s.known) {
            setupFinishing_ = false;
            if (s.done) sysapi::watch("setup", false);
            else setupGo(kDone);   // (refused: the reason shows as any other; tap again)
        }
    }
    if (setupStep_ < 0) return;
    sysapi::watch("time", setupStep_ == kZone);
    if (setupStep_ == kZone) sysapi::zonesWanted();
    sysapi::watch("theme", setupStep_ == kColour);
    sysapi::watch("hold", setupStep_ == kHold);
    // Bluetooth's screen looks for headphones by itself, once, as soon as Bluetooth is known to be on
    static double lookedFor = -1;
    if (setupStep_ == kBt && lookedFor != setupAt_ && sysapi::has("bt")) {
        const sysapi::Bt b = sysapi::bt();
        if (b.known) {
            lookedFor = setupAt_;
            if (b.enabled && !b.discovering) sysapi::act("bt_discover", "start", {}, "bt");
        }
    }
}

void App::drawSetup() {
    const float W = float(css::W), H = float(css::H);
    const float cw = std::min(W - 36, 520.f), cx = std::round((W - cw) / 2);
    // the screen comes in from the side it was asked from, as Songate's pages do (Motion off: at once)
    const float t = store.motion ? std::clamp(float(time - setupAt_) / 0.28f, 0.f, 1.f) : 1.f;
    const float e = 1 - (1 - t) * (1 - t) * (1 - t);
    const float dir = setupFrom_ > setupStep_ ? -1.f : 1.f;
    // (a fade over the whole screen: Settings' cards in it draw as they always do)
    (void)dir;
    const float dx = 0, a = 1;
    auto mix = [](const Rgb& p, const Rgb& q, float k) { return Rgb{ p.r + (q.r - p.r) * k, p.g + (q.g - p.g) * k, p.b + (q.b - p.b) * k }; };
    auto veil = [&] { if (e < 1) g.rect(0, 0, W, H, pal.bg, 1 - e); };
    const Font titleF{ css::Xxl, Weight::ExtraBold }, subF{ css::Base };
    const float footH = 18 + 52 + (setupStep_ == kWifi || setupStep_ == kBt ? 40 : 0) + 22;
    const float top = 34;

    // the big button at the foot (and Skip under it, on the screens that have one)
    auto bigButton = [&](const char* key, const std::string& label, float y) {
        const Id id = uid(key);
        const bool hit = ui.item(id, { cx, y, cw, 52 }, 16, DefaultFocus);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        const Rgb bg = hot ? Rgb{ pal.accent.r * 0.88f + pal.text.r * 0.12f, pal.accent.g * 0.88f + pal.text.g * 0.12f, pal.accent.b * 0.88f + pal.text.b * 0.12f }
                           : pal.accent;
        g.roundRect(cx, y, cw, 52, 16, bg, a);
        g.textC(label, { css::Md, Weight::SemiBold }, cx + cw / 2, y + 26, pal.onAccent, a, Align::Center);
        return hit;
    };
    auto skipLink = [&](const char* key, const std::string& label, float y) {
        const Font f{ css::Base };
        const float lw = g.measureF(label, f) + 32;
        const Id id = uid(key);
        const bool hit = ui.item(id, { cx + (cw - lw) / 2, y, lw, 36 }, 18);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        g.textC(label, f, cx + cw / 2, y + 18, hot ? pal.text : pal.text2, a, Align::Center);
        return hit;
    };
    // the screens between Welcome and Done: a dot each, the one shown long, in the accent
    auto dots = [&](float y) {
        const int n = kSteps - 2, on = setupStep_ - 1;
        const float dw = 6, longW = 18, gap = 5;
        float total = longW + (n - 1) * dw + (n - 1) * gap, x = cx + (cw - total) / 2;
        for (int i = 0; i < n; i++) {
            const float w = i == on ? longW : dw;
            g.roundRect(x, y, w, 6, 3, i == on ? pal.accent : pal.text, i == on ? 1.f : 0.18f);
            x += w + gap;
        }
    };
    auto heading = [&](const std::string& title, const std::string& sub, float y) {
        g.text(title, titleF, cx + dx, y, pal.text, a);
        y += 46;
        for (const auto& line : g.wrap(sub, subF, int(cw), 3)) { g.text(line, subF, cx + dx, y, pal.text2, a); y += 22; }
        return y + 14;
    };

    // ---- Welcome and Done: the middle of the screen
    if (setupStep_ == kWelcome || setupStep_ == kDone) {
        const float my = (H - footH) / 2;
        if (setupStep_ == kWelcome) {
            // "SongateOS" as one word in Songate's own letters, gradient and glow (the boot logo's; the user, 2026-11-18)
            const float big = 1.6f, ww = g.wordmarkOSW() * big;
            const float wx = std::round((W - ww) / 2 + dx);
            g.icon(ICON_SPARKLE, W / 2 + dx, my - 70, 34, pal.accent, a);
            g.wordmarkOS(wx, my, mix(pal.accent, pal.text, 0.12f), mix(pal.accent2, pal.text, 0.2f), mix(pal.accent2, pal.text, 0.78f), pal.glow, 0.4f, -1, a, big);
            g.textC("Your music, set up in a minute", subF, W / 2 + dx, my + 50, pal.text2, a, Align::Center);
            if (bigButton("setup.start", "Start", H - footH + 18)) setupGo(kWifi);
        } else {
            g.roundRect(W / 2 - 30 + dx, my - 92, 60, 60, 30, pal.accent, a);
            g.icon(ICON_CHECK, W / 2 + dx, my - 62, 30, pal.onAccent, a);
            g.textC("All set", titleF, W / 2 + dx, my, pal.text, a, Align::Center);
            int k = 0;
            for (const auto& line : g.wrap("Everything here is in Settings whenever you want it", subF, int(cw), 3))
                g.textC(line, subF, W / 2 + dx, my + 44 + 22 * k++, pal.text2, a, Align::Center);
            // Take the tour (the user, 2026-11-19): the setup ends as with Start listening, then the R4's tour asks how deep to go
            bool tour = false;
            {
                const float ty = H - footH + 18 - 52 - 12;
                const Id id = uid("setup.tour");
                tour = ui.item(id, { cx, ty, cw, 52 }, 16);
                const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
                g.roundRect(cx, ty, cw, 52, 16, pal.text, (hot ? 0.12f : 0.05f) * a);
                g.roundStroke(cx, ty, cw, 52, 16, 1, pal.text, 0.18f * a);
                g.textC("Take the tour", { css::Md, Weight::SemiBold }, cx + cw / 2, ty + 26, pal.text, a, Align::Center);
            }
            if (tour) { tourPending = true; tourAsk = false; }
            if (bigButton("setup.finish", "Start listening", H - footH + 18) || tour) {
                if (sysapi::bt().discovering) sysapi::act("bt_discover", "stop", {}, "bt");
                // (Android's setup flags too: the bars come back). Closed at once; its answer read anew (setupForget): refused, the
                // Done screen comes back to tap again (the service's word, 2026-11-18)
                sysapi::setupForget();
                sysapi::act("setup_done", "", {}, "setup");
                sysapi::watch("time", false);
                setupFinishing_ = true;
                setupStep_ = -1;
                ui.focusDefault();
            }
        }
        veil();
        return;
    }

    // ---- the screens between: dots, the title and its line, the cards, the foot
    dots(top);
    static const char* const titles[] = { "", "Wi-Fi", "Time zone", "Music", "Sound", "Colour", "Hold switch", "Bluetooth", "Updates", "" };
    static const char* const subs[] = { "", "For the clock, lyrics and updates. Songate plays without it",
                                        "With no mobile network, the R4 takes the time from here and from Wi-Fi",
                                        "Songate plays the music on your microSD card", "Change any of it later in Settings › Sound & DAC",
                                        "The system's colour: Songate's buttons, the volume and the lights",
                                        "The switch on top of the player, slid to lock",
                                        "Put your headphones in pairing mode. You can do this later too",
                                        "SongateOS and Songate update together", "" };
    const float bodyTop = heading(titles[setupStep_], subs[setupStep_], top + 26);
    const float bodyH = H - footH - bodyTop;

    if (setupStep_ == kZone) {
        // Find a city (Songate's keyboard), then the zones that match, the R4's own first
        const sysapi::Time tz = sysapi::time();
        const Id fid = uid("setup.find");
        const float fy = bodyTop;
        if (ui.item(fid, { cx, fy, cw, 44 }, 12)) openTextKb(setupZoneQ_, [this](const std::string& s) { setupZoneQ_ = s; area("setup.zones").target = area("setup.zones").y = 0; }, "setup.zone");
        g.roundRect(cx + dx, fy, cw, 44, 12, pal.text, (ui.pressed(fid) ? 0.09f : 0.05f) * a);
        g.roundStroke(cx + dx, fy, cw, 44, 12, 1, pal.text, 0.12f * a);
        g.icon(ICON_SEARCH, cx + dx + 22, fy + 22, 18, pal.text3, a);
        g.textC(setupZoneQ_.empty() ? "Find a city" : setupZoneQ_, subF, cx + dx + 42, fy + 22, setupZoneQ_.empty() ? pal.text3 : pal.text, a,
                Align::Left, int(cw - 60));
        typingMark("setup.zone", cx, fy, cw, 44, 12, cx + 42, setupZoneQ_, subF, cw - 60);
        const float ly = fy + 44 + 12, lh = std::max(60.f, bodyTop + bodyH - ly - 8);
        const std::vector<sysapi::Zone> all = sysapi::zones();
        const std::string q = lower(setupZoneQ_);
        std::vector<const sysapi::Zone*> shown;
        for (const sysapi::Zone& z : all)
            if (q.empty() || lower(z.name).find(q) != std::string::npos || lower(z.longName).find(q) != std::string::npos ||
                lower(z.id).find(q) != std::string::npos)
                shown.push_back(&z);
        ScrollArea& sa = area("setup.zones");
        const float rowH = 60;
        sa.view = { cx - 8, ly, cw + 16, lh };
        sa.content = shown.size() * rowH + 12;
        // the screen opens on the R4's own zone, in the list's middle (once, when both are known)
        static double scrolledFor = -1;
        if (scrolledFor != setupAt_ && tz.known && !shown.empty() && setupZoneQ_.empty()) {
            for (size_t i = 0; i < shown.size(); i++)
                if (shown[i]->id == tz.zone) { sa.target = sa.y = std::max(0.f, float(i) * rowH - lh / 2 + rowH / 2); break; }
            scrolledFor = setupAt_;
        }
        sa.clamp();
        ui.pushArea(&sa);
        g.clip(cx - 8, ly, cw + 16, lh);
        g.roundRect(cx + dx, ly - sa.y, cw, std::max(rowH, float(shown.size()) * rowH), 18, pal.surface, 0.55f * a);
        g.roundStroke(cx + dx, ly - sa.y, cw, std::max(rowH, float(shown.size()) * rowH), 18, 1, pal.text, 0.09f * a);
        if (all.empty()) g.textC("…", subF, cx + cw / 2, ly + rowH / 2, pal.text3, a, Align::Center);
        else if (shown.empty()) g.textC("No city by that name", subF, cx + 20 + dx, ly + rowH / 2, pal.text3, a);
        for (size_t i = 0; i < shown.size(); i++) {
            const float ry = ly - sa.y + float(i) * rowH;
            if (ry + rowH < ly || ry > ly + lh) { ui.item(uid("setup.z", int64_t(i)), { cx, ry, cw, rowH }, 12); continue; }
            const sysapi::Zone& z = *shown[i];
            const bool cur = z.id == tz.zone;
            const Id id = uid("setup.z", int64_t(i));
            if (ui.item(id, { cx, ry, cw, rowH }, 12) && !cur) {
                if (tz.autoZone) sysapi::act("time_set", "auto_zone", { { "value", "0" } }, "time");   // (a zone of its own: not the automatic one)
                sysapi::act("time_set", "zone", { { "value", z.id } }, "time");
            }
            if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(cx + 6 + dx, ry + 4, cw - 12, rowH - 8, 12, pal.text, 0.06f * a);
            if (i > 0) g.rect(cx + 20 + dx, ry, cw - 40, 1, pal.text, 0.09f * a);
            const Rgb ink = cur ? pal.accentInk : pal.text;
            g.text(z.name, { css::Md, Weight::SemiBold }, cx + 20 + dx, ry + 10, ink, a, Align::Left, int(cw - 130));
            g.text(z.longName, { css::Sm }, cx + 20 + dx, ry + 33, cur ? pal.accentInk : pal.text3, a, Align::Left, int(cw - 130));
            if (cur) g.icon(ICON_CHECK, cx + cw - 30 + dx, ry + rowH / 2, 18, pal.accentInk, a);
            else g.textNum(gmt(z.offset), { css::Sm }, cx + cw - 20 + dx, ry + rowH / 2 - g.lineHeight({ css::Sm }) / 2.f, pal.text2, a, Align::Right);
        }
        g.unclip();
        ui.popArea();
    } else {
        // Settings' own cards for this screen (its rows work as they do there)
        static const SetupPane panes[] = {
            {}, { "Connectivity", "", {}, {}, true }, {},
            { "Library", "Where it is", { "Music folder" }, {}, false },
            { "Sound & DAC", "Sound & DAC", { "3.5 mm", "Gain", "Bit-perfect output" }, {}, false },
            { "Appearance", "", {}, { "System colours" }, false },
            { "Controls", "Hold switch", { "Locks", "Sleep when locked" }, {}, false },
            { "Connectivity", "", {}, { "", "Paired devices", "New devices" }, true },   // (not the earbuds' card: Settings has it)
            { "Updates", "", { "Check for updates automatically", "Beta updates" }, {}, false }, {} };   // (the service's own two settings, in the setup's words: one card, no title)
        setupPane_ = &panes[setupStep_];
        drawSettings(cx, bodyTop, cw, bodyH);
        setupPane_ = nullptr;
        if (setupStep_ == kMusic) {   // (a line right under the card: how Songate sorts what it finds)
            const ScrollArea& pa = area("settings.pane");
            float ty = bodyTop + pa.content - 40 - pa.y + 10;   // (the card's foot: its 16 px gap and the 24 below it taken off)
            for (const auto& line : g.wrap("Albums and playlists come from the songs' tags, not from folders", { css::Sm }, int(cw - 8), 3)) {
                g.text(line, { css::Sm }, cx + 4, ty, pal.text3, a);
                ty += 18;
            }
        }
    }

    // ---- the foot: Continue (and Skip), over the page's colour so a long list passes under it
    const float fy = H - footH;
    g.rect(0, fy, W, H - fy, pal.bg, 0.92f);
    if (bigButton("setup.next", "Continue", fy + 18)) setupGo(setupStep_ + 1);
    if (setupStep_ == kWifi && skipLink("setup.skip", "Skip, use it offline", fy + 18 + 52 + 4)) setupGo(setupStep_ + 1);
    if (setupStep_ == kBt && skipLink("setup.skip", "Skip", fy + 18 + 52 + 4)) setupGo(setupStep_ + 1);
    veil();
}

#else   // (SongateOS only: the HiBy R4)

void App::setupGo(int) {}
void App::setupTick() {}
void App::drawSetup() {}

#endif
