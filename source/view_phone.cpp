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
#include <random>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "app.hpp"
#include "hibyr4.hpp"
#include "icons.inc"
#include "plicons.inc"
#include "appicons.inc"
#include "words.hpp"
#include "platform.hpp"
#include <unordered_map>

namespace {
constexpr float kPad = 18;      // the page's side margin
constexpr float kTopH = 52;     // the wordmark's line
constexpr float kNavH = 68;     // the bottom bar
constexpr float kMiniH = 64;    // the mini player
constexpr int kLibraryHome = -7;   // View::Library's ref for the phone's Library page (its rows); any other: the landscape one
// The Apps page (Songate as the home screen): the device's apps. Their order, their symbols, their names here and which are hidden
// are Songate's own (apps.tsv in its folder: the app, its symbol's key, its name here, hidden, a line each in the page's order;
// written when one of those changes, never otherwise). An app's own icon, in grey, for the ones Songate has no symbol for.
struct AppEntry { std::string name, app, icon, custom; bool hidden = false; };
std::vector<AppEntry> gApps;   // every app installed, in the page's order (the hidden ones too)
double gAppsAt = -100;
bool gAppsRead = false;
std::unordered_map<std::string, SDL_Texture*> gAppIcons;   // nullptr: asked, none

// Songate's symbols for kinds of app: what Icon… offers before the playlists' symbols, and what an app gets by itself (appSymbol).
// The keys are saved (apps.tsv, after "app:"): never rename one.
struct AppKind { const char* key; const char* svg; };
const AppKind kAppKinds[] = {
    { "gear", ICON_GEAR },     { "folder", ICON_FOLDER },   { "camera", APP_CAMERA }, { "clock", APP_CLOCK },   { "calendar", ICON_CALENDAR },
    { "globe", APP_GLOBE },    { "mail", APP_MAIL },        { "image", ICON_IMAGE },  { "video", APP_VIDEO },   { "music", ICON_MUSIC },
    { "person", APP_PERSON },  { "phone", APP_PHONE },      { "chat", APP_CHAT },     { "calc", APP_CALC },     { "bag", APP_BAG },
    { "pin", APP_PIN },        { "download", ICON_DOWNLOAD }, { "cloud", APP_CLOUD }, { "search", ICON_SEARCH }, { "apps", ICON_APPS } };

/** Songate's own symbol for an app, by what its name and package say it is; nullptr: none (its own icon, in grey). */
const char* appSymbol(const std::string& name, const std::string& app) {
    std::string s = name + " " + app;
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, const char*> kinds[] = {
        { "settings", ICON_GEAR },      { "documentsui", ICON_FOLDER }, { "files", ICON_FOLDER },   { "filemanager", ICON_FOLDER },
        { "explorer", ICON_FOLDER },    { "camera", APP_CAMERA },       { "clock", APP_CLOCK },     { "calendar", ICON_CALENDAR },
        { "chrome", APP_GLOBE },        { "browser", APP_GLOBE },       { "firefox", APP_GLOBE },   { "gmail", APP_MAIL },
        { "mail", APP_MAIL },           { "photos", ICON_IMAGE },       { "gallery", ICON_IMAGE },  { "youtube", APP_VIDEO },
        { "video", APP_VIDEO },         { "music", ICON_MUSIC },        { "spotify", ICON_MUSIC },  { "tidal", ICON_MUSIC },
        { "qobuz", ICON_MUSIC },        { "contacts", APP_PERSON },     { "dialer", APP_PHONE },    { "phone", APP_PHONE },
        { "messag", APP_CHAT },         { "mms", APP_CHAT },            { "calculator", APP_CALC }, { "vending", APP_BAG },
        { "market", APP_BAG },          { "fdroid", APP_BAG },          { "maps", APP_PIN },        { "download", ICON_DOWNLOAD },
        { "drive", APP_CLOUD },         { "cloud", APP_CLOUD },         { "search", ICON_SEARCH } };
    for (const auto& [key, icon] : kinds)
        if (s.find(key) != std::string::npos) return icon;
    return nullptr;
}

/** The symbol an app wears: the one chosen for it (Icon…), else Songate's for its kind, else none. */
const char* appWears(const AppEntry& e) {
    if (e.icon.rfind("app:", 0) == 0) {
        for (const AppKind& k : kAppKinds) if (e.icon.compare(4, std::string::npos, k.key) == 0) return k.svg;
    } else if (!e.icon.empty()) {
        if (const char* svg = playlistIconSvg(e.icon)) return svg;
    }
    return appSymbol(e.name, e.app);
}

const std::string& appName(const AppEntry& e) { return e.custom.empty() ? e.name : e.custom; }

int appAt(const std::string& app) {
    for (size_t i = 0; i < gApps.size(); i++) if (gApps[i].app == app) return int(i);
    return -1;
}

/** The apps shown: each one's place in gApps. */
std::vector<int> appsShown() {
    std::vector<int> v;
    for (size_t i = 0; i < gApps.size(); i++) if (!gApps[i].hidden) v.push_back(int(i));
    return v;
}

float smooth(float p) { return p * p * (3 - 2 * p); }
float inOutCubic(float p) { return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.f) / 2; }   // the Canvas's layout morph's
}  // namespace

void App::drawPhone() {
    const float W = float(css::W), H = float(css::H);
    const bool has = now.track >= 0 || extOn;
    const TrackInfo& in = now.info;
    if (route.view == View::Apps) {   // turned upright on the sidebar's Apps page: the same page here, over the one before it
        if (!history.empty()) { route = history.back(); history.pop_back(); }
        else route = { View::Now, -1, "", 0 };
        phoneApps_ = appsTab();
    }
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
    const bool liked = currentLiked();   // (a server track's: its star)
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
#ifndef GF_ANDROID   // (Android has its own back: no arrow here, the user 2026-11-11)
        if (!rootView(v) && !history.empty()) {
            if (iconButton(uid("ph.back"), ICON_BACK, kPad + 12 + edge_.tbL, kTopH / 2, false, 40, 20)) back();
            wx += 36;
        }
#endif
        g.wordmark(wx, kTopH / 2, mix(pal.accent, pal.text, 0.12f), mix(pal.accent2, pal.text, 0.2f), mix(pal.accent2, pal.text, 0.78f), pal.glow, 0.4f, -1, 1);
        // a sync or a copy between devices running: the title bar's two arrows, right of the name (as sideways)
        drawSyncMark(wx + Gfx::wordmarkW() + 12 + 6.8f, kTopH / 2 + 2.8f);
        const std::time_t tt = std::time(nullptr);
        char clock[8];
        std::strftime(clock, sizeof clock, "%H:%M", std::localtime(&tt));
        unsigned pct = 0;
        float rx = W - kPad - edge_.tbR;
        // the battery as the title bar has it: its states' colours and animations, what you put beside it, its card when pressed
        if (plat::batteryLevel(pct)) rx = drawBatteryCluster(rx, kTopH / 2, pct, wx + Gfx::wordmarkW() + 16 + (syncIconA > 0 ? 18 : 0));
        g.textNum(clock, { css::Base, Weight::SemiBold }, rx, kTopH / 2 - g.lineHeight({ css::Base, Weight::SemiBold }) / 2.f, pal.text2, 1, Align::Right);
    };

    // The bottom bar: the sidebar's destinations, a phone's five, as the collapsed sidebar shows them
    auto nav = [&] {
        const float y0 = H - kNavH;
        g.rect(0, y0, W, kNavH, pal.bg, 0.86f);
        g.rect(0, y0, W, 1, pal.text, 0.09f);
        struct Dest { const char* icon; const char* hintText; View view; };
        static const Dest dests[6] = { { ICON_DISC, "Now playing", View::Now }, { ICON_GRID, "Library", View::Library }, { ICON_SEARCH, "Search", View::Search },
                                       { ICON_HEART, "Playlists", View::Playlists }, { ICON_GEAR, "Settings", View::Settings },
                                       { ICON_APPS, "Apps", View::Now } };   // (the sixth: Settings › Home screen › Show Apps; a page over the page)
        const int nDest = appsTab() ? 6 : 5;
        const float cw = W / nDest, pillW = std::min(68.f, cw - 6);
        int active = -1;
        if (phoneApps_) active = 5;
        else if (v == View::Now || v == View::Queue) active = 0;   // (the playlist playing: Up next's whole page)
        else if (v == View::Search) active = 2;
        else if (v == View::Playlists || v == View::List || (v == View::Server && route.ref == 5)) active = 3;   // (a server playlist's page too)
        else if (v == View::Settings) active = 4;
        else if (v != View::PickFolder && v != View::PickPlaylist && v != View::PickExport) active = 1;   // the library's pages
        for (int i = 0; i < nDest; i++) {
            const float cx = cw * i + cw / 2, cy = y0 + kNavH / 2;
            const Id id = uid("ph.nav", i);
            hint(id, dests[i].hintText);
            if (ui.item(id, { cw * i, y0, cw, kNavH }, 20)) {
                const bool wasApps = phoneApps_;
                phoneApps_ = i == 5;
                if (i == 5) { if (wasApps) area("ph.apps").target = 0; }
                else if (wasApps && dests[i].view == v) {}   // (Apps lay over this very page: it is simply there again)
                // the destination already showing, pressed again: its page back at its top
                else if (i == 1) { if (v == View::Library && route.ref == kLibraryHome) area("ph.libhome").target = 0; else navigate(View::Library, kLibraryHome); }
                else if (i == 2) { if (v != View::Search) navigate(View::Search, -1, std::string()); else area("search", int(View::Search) * 1000 + route.ref).target = 0; }
                else if (i == 3) { if (v != View::Playlists) navigate(View::Playlists); else area("playlists").target = 0; }
                else if (i == 4) {
                    if (v == View::Settings && !settingsPhoneOpen_ && settingsQuery.empty()) area("settings.phone").target = 0;
                    settingsPhoneOpen_ = settingsPhoneDirect_ = false;
                    settingsQuery.clear();
                    if (v != View::Settings) navigate(View::Settings);
                }
                else if (dests[i].view != v) navigate(dests[i].view);
                else if (phoneLyrics_) phoneLyrics_ = false;   // (Now playing on its second page: the first)
            }
            const bool on = i == active;
            if (on) g.roundRect(cx - pillW / 2, cy - 20, pillW, 40, 20, pal.accent, 0.14f);
            else if (ui.pressed(id)) g.roundRect(cx - pillW / 2, cy - 20, pillW, 40, 20, pal.text, 0.08f);
            g.icon(dests[i].icon, cx, cy, 24, on ? pal.accent : pal.text2);
            if (i == 4) {
                // What the folded sidebar shows above Settings, on Settings' own button here: a newer version's dot, or the
                // work running in the background as its thin bar (its card is at the head of Settings' list: phoneWorkCard)
                const JobStack st = jobStack();
                if (updateOffer() >= 0) {
                    g.circle(cx + 13, cy - 11, 5.5f, pal.bg);
                    g.circle(cx + 13, cy - 11, 4, pal.accent);
                } else if (st.body > 0.01f && st.front >= 0) {
                    const Job& j = jobCards[st.front].job;
                    const float bw = 26, bx = cx - bw / 2, by = cy + 17;
                    g.roundRect(bx, by, bw, 3, 1.5f, pal.text, 0.14f * st.body);
                    if (j.total > 0) g.roundRect(bx, by, std::max(3.f, bw * std::clamp(float(j.done) / float(j.total), 0.f, 1.f)), 3, 1.5f, pal.accent, st.body);
                    else {   // busy without a count: a segment sliding along (the card's)
                        const float t = float(std::fmod(time, 1.6) / 1.6), seg = bw * 0.3f;
                        const float sx = bx + (bw + seg) * t - seg, l = std::max(bx, sx), r = std::min(bx + bw, sx + seg);
                        if (r > l) g.roundRect(l, by, r - l, 3, 1.5f, pal.accent, st.body);
                    }
                }
            }
        }
    };

    // The mini player: the player bar folded into a card over the page; its progress a thin line along its foot. Tap it: Now playing
    auto mini = [&](float a = 1) {
        const float x = 10, y = H - kNavH - kMiniH - 8, w = W - 20, h = kMiniH, cy = y + h / 2;
        g.setFade(a);
        const Id card = uid("ph.mini");
        if (ui.item(card, { x, y, w - 150, h }, 16)) {
            // on Now playing's lyrics page it is the small cover's press: back to the cover (the user, 2026-11-11: "should do the
            // same as a cover click above"); anywhere else it opens Now playing
            if (v == View::Now && phoneLyrics_) phoneLyrics_ = false;
            else navigate(View::Now);
        }
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
        g.icon(isPlaying() || (extOn && !srvq_.on) ? ICON_PAUSE : ICON_PLAY, x + w - 66, cy, 17, pal.bg);
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
        // (to the scrub in half a second, so it answers the finger at once; back as a wave, 0.95 s from its first column to its last)
        const bool seekBack = time - phoneSeekAt_ > 0.5 && !(ui.focused(sid) && ui.showFocusRing());
        const int seekVar = store.seekAnim;   // Settings › Visualizer › Seek bar › Seek bar animation: 0 Quick, 1 Smooth, 2 Wave
        step(phoneSeekP_, seekBack, seekBack ? (seekVar == 1 ? 0.75f : seekVar == 2 ? 0.95f : 0.5f) : 0.5f);   // back to the visualizer half a second after the finger (the user: "0.5/1s")
        const float e = smooth(phoneSeekP_);
        float frac = dur > 0 ? float(std::clamp(shownPosition() / dur, 0.0, 1.0)) : 0;
        if (drag >= 0) frac = std::clamp((drag - x) / w, 0.f, 1.f);
        const float foot = y + h, cy = y + h / 2;
        if (phoneSeekP_ >= 1 && spec.bands > 0) drawSpectrum(x, y, w, h, frac);   // the song's place: the bars before it bright (the Canvas's look)
        else {
            const bool waveNow = store.seekWave && wave.ready() && !playingFile().empty() && wave.path() == playingFile();
            const auto& pk = wave.peaks();
            const Gfx::Pitch pt = g.barPitch(2, 3);   // the scrub's 2 px bars every 3, in whole real pixels (as the player bar's)
            const float x0 = g.onPixel(x);
            const int bars = int(w / pt.step);
            const int n = std::max(1, spec.bands);
            const float gap = std::max(2.f, std::round(w / n * 0.28f)), colW = (w - gap * (n - 1)) / n;
            // One shape all the way (the user, session 19: "the still square bars abruptly turn into round ones, it should be
            // smooth"; and of a first try that faded the round columns in over the square bars: "still a snap at the end").
            // First the scrub's thin bars gather into their columns (ex: there by 45 % of a column's way, each column's bars at one
            // height, one colour, side by side, so a column is a plain rectangle); from there each column is drawn as that one
            // shape, moving to the music's height while its corners grow round and its colour takes the visualizer's fade (rt),
            // and it ends as drawSpectrum draws it: nothing is left to change when that takes over.
            // A wave out from the song's place (the user's pick of three, session 19: "B, the wave"): each column goes its own way
            // (eOf), the ones at the song's place first, the far ends last (spread: how much later), each gently in and out
            // (ease), gathered by 45 % of its way and rounding over the rest.
            // The three the user saw in the clips are all kept, as a setting: Quick (every column together, gathered by 60 % of the
            // way), Smooth (the same, longer, gentler in and out, rounding over more of the way), Wave (Smooth's, spread out).
            const float gatherAt = seekVar ? 0.45f : 0.6f, spread = seekVar == 2 ? 0.9f : 0.f;
            auto ease = [&](float p) { return seekVar ? p * p * p * (p * (p * 6 - 15) + 10) : smooth(p); };
            auto eOf = [&](int k) {
                const float d = std::fabs((k + 0.5f) / n - frac);
                return ease(std::clamp(phoneSeekP_ * (1 + spread) - d * spread, 0.f, 1.f));
            };
            auto exOf = [&](float ek) { return smooth(std::clamp(ek / gatherAt, 0.f, 1.f)); };
            auto rtOf = [&](float ek) { return smooth(std::clamp((ek - gatherAt) / (1 - gatherAt), 0.f, 1.f)); };
            auto colOf = [&](int i) { return std::min(n - 1, i * n / bars); };
            auto seekOf = [&](int i, float& sh, Rgb& sc, float& sa) {   // the scrub's own bar i: height, colour, brightness
                float vv = 0.3f;
                if (waveNow && !pk.empty()) {
                    const size_t a = size_t(i) * pk.size() / bars, b = std::max(a + 1, size_t(i + 1) * pk.size() / bars);
                    vv = 0;
                    for (size_t j = a; j < b && j < pk.size(); j++) vv = std::max(vv, pk[j]);
                }
                sh = std::max(1.5f, vv * h * 0.9f);
                const bool played = i * pt.step < frac * w;
                sc = played ? grad(float(i) / bars) : pal.text;
                sa = played ? 1.f : (drag >= 0 ? 0.2f : 0.13f);
            };
            // each column's bars as one: their mean height, colour and brightness
            static std::vector<float> mH, mA;
            static std::vector<Rgb> mC;
            static std::vector<int> mN;
            mH.assign(size_t(n), 0.f); mA.assign(size_t(n), 0.f); mC.assign(size_t(n), Rgb{ 0, 0, 0 }); mN.assign(size_t(n), 0);
            if (e > 0)
                for (int i = 0; i < bars; i++) {
                    float sh, sa; Rgb sc;
                    seekOf(i, sh, sc, sa);
                    const size_t k = size_t(colOf(i));
                    mH[k] += sh; mA[k] += sa; mC[k].r += sc.r; mC[k].g += sc.g; mC[k].b += sc.b; mN[k]++;
                }
            for (size_t k = 0; k < size_t(n); k++)
                if (mN[k] > 0) { const float q = 1.f / mN[k]; mH[k] *= q; mA[k] *= q; mC[k].r *= q; mC[k].g *= q; mC[k].b *= q; }
            auto specHOf = [&](int k) { return std::max(2.f, (spec.bands > 0 ? spec.bars[k] : 0.f) * h * 0.94f); };
            auto specAOf = [&](int k) { return x + k * (colW + gap) + colW <= x + frac * w ? 1.f : 0.4f; };   // the visualizer's own brightness, played or not
            {
                for (int i = 0; i < bars; i++) {
                    const int k = colOf(i), i0 = (k * bars + n - 1) / n, i1 = ((k + 1) * bars + n - 1) / n;
                    const float e = eOf(k), ex = exOf(e);   // (its column's own way there)
                    if (ex >= 1) continue;
                    float seekH, seekA; Rgb seekC;
                    seekOf(i, seekH, seekC, seekA);
                    const int cnt = std::max(1, i1 - i0), at = i - i0;
                    const float colX = x + k * (colW + gap), specH = specHOf(k);
                    const float h1 = seekH + (mH[size_t(k)] - seekH) * ex;                  // towards its column's own height …
                    const float a1 = seekA + (mA[size_t(k)] - seekA) * ex;
                    const Rgb c1 = mix(seekC, mC[size_t(k)], ex);
                    const float bx = x0 + i * pt.step + (colX + at * colW / cnt - (x0 + i * pt.step)) * ex;   // … and its place in it
                    const float bw = pt.bar + (colW / cnt + 0.5f - pt.bar) * ex;
                    const float bh = h1 + (specH - h1) * e;                                 // the column on its way to the music's
                    const float by = (cy - h1 / 2) + ((foot - specH) - (cy - h1 / 2)) * e;
                    g.rect(bx, by, bw, bh, mix(c1, grad(specH / h), e), a1 + (specAOf(k) - a1) * e);
                }
            }
            {
                for (int k = 0; k < n; k++) {
                    const float e = eOf(k), rt = rtOf(e);
                    if (exOf(e) < 1) continue;
                    const float colX = x + k * (colW + gap), specH = specHOf(k), h1 = mH[size_t(k)];
                    const float bh = h1 + (specH - h1) * e, by = (cy - h1 / 2) + ((foot - specH) - (cy - h1 / 2)) * e;
                    const Rgb flat = mix(mC[size_t(k)], grad(specH / h), e);
                    const float pa = mA[size_t(k)] + (specAOf(k) - mA[size_t(k)]) * e;
                    const float rad = spec.bands > 0 ? std::min({ colW / 2, 4.f, bh / 2 }) * rt : 0.f;
                    g.roundRectV(colX, by, colW, bh, rad, flat, mix(flat, pal.accent2, spec.bands > 0 ? rt : 0.f), pa);
                    if (store.vizPeaks && spec.bands > 0 && rt > 0)
                        g.rect(colX, std::max(y, foot - spec.peaks[k] * h * 0.94f - 3), colW, 1.5f, pal.accent, 0.55f * pa * rt);
                }
            }
        }
        if (drag >= 0 && dur > 0) {   // as the player bar's: the time under the finger
            const std::string tip = fmtTime(frac * dur);
            const float tw = g.measure(tip, { css::Xs }) + 16, tx = std::clamp(x + w * frac - tw / 2, x, x + w - tw);
            g.roundRect(tx, y - 30, tw, 24, 6, pal.surface2);
            g.textC(tip, { css::Xs }, tx + tw / 2, y - 18, pal.text, 1, Align::Center);
        }
        g.textNum(fmtTime(drag >= 0 ? frac * dur : shownPosition()), { css::Xs }, x, foot + 8, pal.text3);
        g.textNum(fmtTime(dur), { css::Xs }, x + w, foot + 8, pal.text3, 1, Align::Right);
    };

    // ---- Now playing, and its lyrics page (lp: 0 → 1): the cover glides from the page's width to a small square top left
    auto nowPlaying = [&] {
        const float lp = phoneLyricsP_, lpe = inOutCubic(lp);
        const float out = std::clamp(1 - lp / 0.5f, 0.f, 1.f), in2 = std::clamp((lp - 0.4f) / 0.6f, 0.f, 1.f);
        // The cover: the page's width, less where the screen is short for it (16:9, a fold's cover screen, a wide window): what is
        // under it (the buttons' row, the seek bar, the transport, the chips) needs kLowerMin, or the chips ran under the bar
        constexpr float kLowerMin = 236, kLowerEven = 290;   // the least; from kLowerEven up, spread evenly as it always was
        const float smallS = 72, top0 = kTopH + 8;
        const float bigS = std::max(240.f, std::min(W - 32, H - kNavH - top0 - 44 - 30 - kLowerMin)), bigX = (W - bigS) / 2;
        const float cs = bigS + (smallS - bigS) * lpe, cx0 = bigX + (kPad - bigX) * lpe, cy0 = top0 + 6 * lpe, cr = 22 + (10 - 22) * lpe;
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
            marqueeLine(has ? in.title : "Songate", { css::Lg, Weight::Bold }, tx, top0 + 10 + dy, tw2, pal.text);   // (long ones move, as on the cover)
            marqueeLine(has ? in.artist : "", { css::Sm, Weight::SemiBold }, tx, top0 + 38 + dy, tw2, pal.text2);
            g.text(fmtTime(in.duration) + (in.codec.empty() ? "" : "  ·  " + in.codec), { css::Xs }, tx, top0 + 60 + dy, pal.text3, 1, Align::Left, int(tw2));
            if ((now.track >= 0 || srvq_.on) && iconButton(uid("ph.lmore"), ICON_MORE, W - kPad - 18, top0 + 42, false, 40, 20)) {
                if (srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size())) openServerMenu(srvq_.songs[size_t(srvq_.pos)]);
                else openTrackMenu(now.track);
            }
            // Now playing's tabs but Up next (a sheet here): Lyrics and Palette, as sideways draws them
            float ly0 = top0 + smallS + 20;
            const float ly1 = H - kNavH - kMiniH - 16;
            {
                static const char* const tabs[2] = { "Lyrics", "Palette" };
                const Font tabF{ css::Sm, Weight::SemiBold };
                float ttx = kPad - 12;
                for (int i = 0; i < 2; i++) {
                    const float ttw = g.measure(tabs[i], tabF) + 24;
                    const Id id = uid("ph.ntab", i);
                    // (a finger: 44 high, drawn as sideways' 35)
                    if (lp >= 0.999f && ui.item(id, { ttx, ly0 - 5, ttw, 44 }, 6)) { phoneNowTab_ = i; area("np.panel").target = area("np.panel").y = 0; }
                    const bool on = phoneNowTab_ == i;
                    g.textC(tabs[i], tabF, ttx + 12, ly0 + 17, on ? pal.text : pal.text3);
                    if (on) g.roundRect(ttx + 12, ly0 + 34, ttw - 24, 2, 1, pal.accent);
                    ttx += ttw + 4;
                }
                g.rect(kPad, ly0 + 35, W - kPad * 2, 1, pal.text, 0.09f);
                ly0 += 35 + 6;
            }
            if (phoneNowTab_ == 1) {
                g.clip(0, ly0, W, ly1 - ly0);
                drawPaletteOf(kPad, ly0 + 30 * (1 - in2), W - kPad * 2, ly1 - ly0, target, lp >= 0.999f);
                g.unclip();
            } else if (lp >= 0.999f) drawLyricsOf(kPad, ly0, W - kPad * 2, ly1 - ly0, now.lyrics, now.synced, now.lyricsSource, lyricsPos(), area("ph.lyrics"), true);
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
        if ((!extOn || srvq_.on) && gb("ph.like", liked ? ICON_HEART_F : ICON_HEART, cx0 + 16 + 22, by, 44, liked)) { if (srvq_.on) toggleCurrentLike(); else toggleLike(); }
        if ((now.track >= 0 || srvq_.on) && gb("ph.more", ICON_MORE, cx0 + cs - 16 - 22, by, 44)) {
            if (srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size())) openServerMenu(srvq_.songs[size_t(srvq_.pos)]);   // a server track's
            else openTrackMenu(now.track);
        }
        if (gb("ph.queue", ICON_QUEUE, cx0 + cs - 16 - 22 - 52, by, 44, phoneQueue_)) phoneQueue_ = true;
        if (gb("ph.lyrics", ICON_QUOTE, cx0 + cs - 16 - 22 - 104, by, 44, phoneLyrics_)) phoneLyrics_ = true;
        const Font tf{ css::Lg, Weight::Bold }, af{ css::Sm, Weight::SemiBold };
        const float maxW = cs - 32 - 28;
        const std::string artistText = has ? in.artist + (in.album.empty() || in.album == in.title ? "" : " · " + in.album) : "";
        const float ay = by - 22 - 10 - 32;
        if (!artistText.empty()) {
            const float aw = std::min(maxW, float(g.measure(artistText, af))) + 28;
            glass(cx0 + 16, ay, aw, 32);
            // (too long for its glass: it moves, as Now playing's long lines do sideways; the user, 2026-11-11: "do not show the full
            // text if it is big")
            marqueeLine(artistText, af, cx0 + 30, ay + 16 - g.lineHeight(af) / 2.f, aw - 28, pal.text2);
        }
        const std::string titleText = has ? in.title : "Songate";
        const float tw = std::min(maxW, float(g.measure(titleText, tf))) + 36, ty = ay - 8 - 44;
        glass(cx0 + 16, ty, tw, 44);
        marqueeLine(titleText, tf, cx0 + 34, ty + 22 - g.lineHeight(tf) / 2.f, tw - 36, pal.text);
        // under the cover: the player bar's smaller buttons (settling down as the lyrics come): Sleep on the left, its time left
        // beside it as the player bar has it; the Canvas, Equalizer and Order on the right. The Canvas is sideways' only (the
        // user, 2026-11-11: "Portrait can have a button to activate canvas but it switches to landscape"): its button turns the
        // screen, the Canvas opens there, and closing it gives the screen back (App::openCanvasTurning)
        float y = top0 + bigS + 44 + 40 * lpe;
        hint(uid("ph.sleep"), "Sleep timer");
        hint(uid("ph.canvas"), "Canvas");
        hint(uid("ph.eq"), "Equalizer (hold for its settings)");
        const double sl = sleepLeft();
        if (gb("ph.sleep", ICON_MOON, kPad + 20, y, 40, sl > 0)) openSleep();
        if (sl > 0) g.textNum(fmtTime(std::ceil(sl)), { css::Xs, Weight::SemiBold }, kPad + 40 + 8, y - g.lineHeight({ css::Xs, Weight::SemiBold }) / 2.f, pal.accentInk);
        if (gb("ph.canvas", ICON_CANVAS, W - kPad - 20 - 104, y, 40, canvasTurn_ != 0)) openCanvasTurning();
        if (gb("ph.eq", ICON_EQ, W - kPad - 20 - 52, y, 40, store.eq.on != 0, Hold)) toggleEq();
        if (live && ui.longPressed(uid("ph.eq"))) openEqualizer();
        static const char* const orderIcons[4] = { ICON_ORDER, ICON_REPEAT, ICON_REPEAT1, ICON_SHUFFLE };
        const Order order = shownOrder();
        static const char* const orderHints[4] = { "Default order", "Repeat playlist", "Repeat song", "Shuffle" };   // (the player bar's)
        hint(uid("ph.order"), orderHints[std::clamp(int(order), 0, 3)]);
        if (gb("ph.order", orderIcons[std::clamp(int(order), 0, 3)], W - kPad - 20, y, 40, order != Order::Default) && !controlsOff()) cycleOrder();
        // the lower half, spread evenly: the seek bar ⇄ visualizer, the transport large, what the file is
        // (a short screen: the three gaps close up towards their least, kLowerMin's, before the cover gives way)
        const float room = (H - kNavH) - (y + 30), gapK = std::max(room, kLowerEven) / 9;
        const float tight = 1 - std::clamp((room - 200) / (kLowerEven - 200), 0.f, 1.f);
        const float gap1 = gapK * 1.6f * (1 - tight), gap2 = gapK * 3.2f + (74 - gapK * 3.2f) * tight, gap3 = gapK * 2.6f + (67 - gapK * 2.6f) * tight;
        y += 30 + gap1;
        phoneToastBottom_ = gap1 >= 46 ? y - 12 : 0;   // a toast in the gap above the seek bar, where there is one (App::drawToast)
        if (live) seekBar(kPad, y, W - kPad * 2, 40);
        else if (spec.bands > 0) {
            const double d = now.info.duration;
            drawSpectrum(kPad, y, W - kPad * 2, 40, d > 0 ? float(std::clamp(shownPosition() / d, 0.0, 1.0)) : 0.f);
        }
        y += 40 + gap2;
        const float mid = W / 2;
        const bool off = controlsOff();
        if (live && !off && iconButton(uid("ph.sac"), ICON_PAUSE_C, kPad + 20, y, stopAfterCurrent, 44, 20)) {
            stopAfterCurrent = !stopAfterCurrent;
            toast(stopAfterCurrent ? "Stop after the current song" : "Keep playing after this song");
        }
        if (live && !off && iconButton(uid("ph.prev"), ICON_PREV, mid - 96, y, false, 60, 30)) prev();
        const Id pid = uid("ph.play");
        if (live && !off && ui.item(pid, { mid - 46, y - 46, 92, 92 }, 46)) togglePlay();
        for (float r : { 50.f, 56.f, 62.f }) g.glow(mid, y, r, pal.glow, 0.3f);
        g.circle(mid, y, 46 * (ui.pressed(pid) ? 0.95f : 1.f), pal.text, off ? 0.35f : 1.f);
        g.icon(isPlaying() || (extOn && !srvq_.on) ? ICON_PAUSE : ICON_PLAY, mid, y, 38, pal.bg);
        if (live && !off && iconButton(uid("ph.next"), ICON_NEXT, mid + 96, y, false, 60, 30)) next(false);
        // Devices: lit during a session, and while AirPlay is on either way (upright its card has AirPlay's speakers too, the
        // title bar's two buttons in one)
        hint(uid("ph.sac"), stopAfterCurrent ? "Stop after this song: on" : "Stop after this song");
        hint(uid("ph.devices"), "Devices");
        if (live && iconButton(uid("ph.devices"), ICON_LINK, W - kPad - 20, y, tog.stage != Tog::Off || airsend.silenceHere() || airplay.view().streaming, 44, 20)) openDevices();
        y += gap3;
        std::vector<std::string> bs;
        if (!in.codec.empty()) bs.push_back(in.codec);
        char b[48];
        if (in.bitsPerSample && in.codec != "MP3") { snprintf(b, sizeof b, "%d-bit", in.bitsPerSample); bs.push_back(b); }
        if (in.sampleRate) { snprintf(b, sizeof b, "%g kHz", in.sampleRate / 1000.0); bs.push_back(b); }
        // the HiBy R4's wired output, when it runs at another rate than the song (a join kept it, the setting is off, it is
        // changing): in the rate's chip, after an arrow (the user chose it, 2026-11-15)
        if (in.sampleRate) bs.back() += r4OutRateNote(in.sampleRate);
        if (in.bitrateKbps) bs.push_back(std::to_string(in.bitrateKbps) + " kbps");
        // a server's song: where it plays from, as a chip of its own after them (sideways' own: pressed, the library on Server)
        const bool fromServer = srvq_.on && !extFrom.empty();
        const Font cf{ css::Xs, Weight::SemiBold };
        const float srvW = fromServer ? g.measureSpaced(extFrom, cf, 0.02f) + 16 + 18 : 0;
        float total = fromServer ? srvW + 6 : 0;
        for (const auto& s : bs) total += g.measureSpaced(s, cf, 0.02f) + 16 + 6;   // (as App::chip measures them)
        // (the codec's chip, first: it sparkles while the song plays bit-perfect, App::sparkleChip)
        float bx = mid - (total - 6) / 2;
        for (size_t i = 0; i < bs.size(); i++) {
            const float cw = chip(bs[i], bx, y, i == 0);
            if (i == 0 && bitPerfectNow()) sparkleChip(bx, y, cw);
            bx += cw + 6;
        }
        if (fromServer) {
            const Id sid = uid("ph.server");
            if (live && ui.item(sid, { bx, y - 21, srvW, 42 }, 6)) { store.libSource = 1; libSourceChanged(); navigate(View::Library, kLibraryHome); }
            g.roundRect(bx, y - 11, srvW, 22, 6, pal.text, ui.pressed(sid) ? 0.13f : 0.065f);
            g.roundStroke(bx, y - 11, srvW, 22, 6, 1, pal.text, 0.09f);
            g.icon(PLI_CLOUD, bx + 13, y, 13, pal.text2);
            g.textSpaced(extFrom, cf, bx + 8 + 18, y, 0.02f, pal.text2);
        }
        g.setFade(1);
    };

    // ---- Up next: the queue as a sheet over Now playing, from its button; tap a row to play it, the veil or back to close
    auto upNextSheet = [&] {
        const float e = smooth(phoneQueueP_);
        const float sh = H * 0.66f, y0 = H - sh * e;
#ifdef GF_DEV
        g.saidLayer = sheet.page != Sheet::Closed ? 0 : 1;   // (tests: a sheet over the page; under a row's menu, the page itself)
#endif
        g.rect(0, 0, W, H, { 0, 0, 0 }, 0.55f * e);
        ui.region(Region::Modal);
        if (phoneQueue_ && ui.item(uid("ph.qveil"), { 0, 0, W, y0 }, 0)) phoneQueue_ = false;
        g.roundRect(0, y0, W, sh + 40, 24, pal.surface, 0.98f);
        g.roundStroke(0, y0, W, sh + 40, 24, 1, pal.text, 0.09f);
        g.roundRect(W / 2 - 20, y0 + 10, 40, 5, 2.5f, pal.text, 0.25f);
        kicker("Playlist", kPad, y0 + 38, pal.text3);
        g.text("Up next", { css::Xl, Weight::Bold }, kPad, y0 + 50, pal.text);
        const size_t qn = srvq_.on ? srvq_.songs.size() : q.order.size();
        // the count beside the title; right of them the whole playlist's page (sideways: the sidebar's Playlist and the player
        // bar's button: its order, Current song, Clear, Export) and the close
        g.textNum(std::to_string(qn) + (qn == 1 ? " track" : " tracks"), { css::Sm }, kPad + g.measure("Up next", { css::Xl, Weight::Bold }) + 12,
                  y0 + 67 - g.lineHeight({ css::Sm }) / 2.f, pal.text3);
        if (iconButton(uid("ph.qclose"), ICON_X, W - kPad - 16, y0 + 66, false, 40, 18)) phoneQueue_ = false;
        {
            const float bw = 16 + 24 + g.measure("Playlist", { css::Sm, Weight::SemiBold }) + 16;
            bool open = false;
            button(uid("ph.qpage"), ICON_LIST, "Playlist", W - kPad - 40 - 6 - bw, y0 + 66 - 17, false, &open);
            if (open && phoneQueue_) { phoneQueue_ = false; navigate(View::Queue); }
        }
        const float listTop = y0 + 100, listH = H - listTop;
        g.rect(kPad, listTop - 1, W - kPad * 2, 1, pal.text, 0.07f);
        ScrollArea& sa = area("ph.queue");
        if (extOn && !srvq_.on) {   // (as sideways: what plays next is the other device's)
            g.textC("What plays next is up to " + extFrom, { css::Base }, W / 2, listTop + 48, pal.text3, 1, Align::Center, int(W - kPad * 2));
            ui.region(Region::Main);
            return;
        }
        const int n = srvq_.on ? std::max(0, int(srvq_.songs.size()) - srvq_.pos) : q.pos >= 0 ? int(q.order.size()) - q.pos : 0;
        // a row picked up (its menu's Move): the list follows it as the arrows move it
        if (!srvq_.on && grab.kind == Grab::Queue && grab.pos >= q.pos && grab.pos < q.pos + n) {
            const float gy = 8 + (grab.pos - q.pos) * 64.f;
            if (gy < sa.target) sa.target = gy - 8;
            else if (gy + 64 > sa.target + listH - 80) sa.target = gy + 64 - (listH - 80);
        }
        sa.view = { 0, listTop, W, listH };
        sa.content = 8 + n * 64.f + 16;
        ui.pushArea(&sa);
        g.clip(0, listTop, W, listH);
        // In sort mode (by touch: the song menu's Move): the rows that can move (the first 15 after the song playing, sideways'
        // limit) show the sidebar sorting's handle in the place of their length; each is drawn where sortAt puts it, the one held
        // last, lifted off the sheet. Items are play positions (queueMove's).
        const bool sort = !srvq_.on && sorting(Grab::Queue, true);
        const int qSize = int(q.order.size());
        const int sortLo = q.pos + 1, sortHi = std::min(qSize - 1, q.pos + 15);
        auto slot = [&](int pos) -> SDL_FRect { return { kPad - 10, listTop + 8 + (pos - q.pos) * 64.f - sa.y, W - kPad * 2 + 20, 64 }; };
        if (sort) sortRun(qSize, sortLo, sortHi, sa, slot, false);
        const int floats = sort ? (tsort_.held >= 0 ? tsort_.held : tsort_.settle) : -1;
        auto ownRow = [&](int k, float ry, bool fl) {
            const int pos = q.pos + k, t = q.tracks[q.order[pos]];
            const Track& tr = lib.tracks[t];
            meta.wantTags(t);
            const Id id = uid("ph.qrow", pos);
            const bool movable = sort && pos >= sortLo && pos <= sortHi;
            if (fl) sortFloatBox(kPad - 10, ry, W - kPad * 2 + 20, 60, 12);
            if (ui.item(id, { kPad - 10, ry, W - kPad * 2 + 20, 60 }, 12) && pos != q.pos && !sort) playAt(pos);   // (in sort mode a tap plays nothing)
            if (ui.focused(id)) { hoverTrack = t; hoverQueue = pos; hoverUpNext = true; }   // (its menu: Up next's, with Move)
            if (grab.kind == Grab::Queue && grab.pos == pos) {   // picked up with the song menu's Move (as sideways' Up next)
                if (ui.focusedId() != id) ui.focus(id);   // (the focus with it: Move's own aims at sideways' rows)
                g.roundRect(kPad - 10, ry, W - kPad * 2 + 20, 60, 12, pal.accent, 0.14f);
                g.roundStroke(kPad - 10, ry, W - kPad * 2 + 20, 60, 12, 2, pal.accent, 0.95f);
            } else if (k == 0) g.roundRect(kPad - 10, ry, W - kPad * 2 + 20, 60, 12, pal.accent, 0.09f);
            else if (ui.pressed(id)) g.roundRect(kPad - 10, ry, W - kPad * 2 + 20, 60, 12, pal.text, 0.065f);
            coverBox(t, kPad, ry + 6, 48, 8);
            const float textW = W - kPad * 2 - 62 - 56;
            g.text(tr.title, { css::Base, Weight::SemiBold }, kPad + 62, ry + 10, k == 0 ? pal.accent : pal.text, 1, Align::Left, int(textW));
            g.text(trackSub(t), { css::Sm }, kPad + 62, ry + 33, pal.text3, 1, Align::Left, int(textW));
            if (movable) sortHandle("ph.qsort", pos, W - kPad - 4, ry + 30, { kPad - 10, ry, W - kPad * 2 + 20, 60 });
            else g.textNumC(tr.duration > 0 ? fmtTime(tr.duration) : "", { css::Sm }, W - kPad, ry + 30, pal.text3, 1, Align::Right);
        };
        for (int k = 0; k < n; k++) {
            const bool mine = sort && q.pos + k >= sortLo && q.pos + k <= sortHi;
            if (mine && q.pos + k == floats) continue;
            const float ry = mine ? sortAt(q.pos + k, slot(q.pos + k)).y : listTop + 8 + k * 64 - sa.y;
            if (ry + 64 < listTop || ry > H) continue;
            if (srvq_.on) {   // the server's tracks: the same rows, their covers from the server
                const int pos = srvq_.pos + k;
                const Subsonic::Song& so = srvq_.songs[size_t(pos)];
                const Id id = uid("ph.qsrow", pos);
                if (ui.item(id, { kPad - 10, ry, W - kPad * 2 + 20, 60 }, 12) && pos != srvq_.pos) serverPlayAt(pos);
                hoverServer(id, so);
                if (k == 0) g.roundRect(kPad - 10, ry, W - kPad * 2 + 20, 60, 12, pal.accent, 0.09f);
                else if (ui.pressed(id)) g.roundRect(kPad - 10, ry, W - kPad * 2 + 20, 60, 12, pal.text, 0.065f);
                if (SDL_Texture* tex = serverCover(so.cover)) g.image(tex, kPad, ry + 6, 48, 48, 8);
                else g.roundRect(kPad, ry + 6, 48, 48, 8, pal.surface2);
                const float textW = W - kPad * 2 - 62 - 56;
                g.text(so.title, { css::Base, Weight::SemiBold }, kPad + 62, ry + 10, k == 0 ? pal.accent : pal.text, 1, Align::Left, int(textW));
                g.text(so.artist + (so.album.empty() ? "" : " · " + so.album), { css::Sm }, kPad + 62, ry + 33, pal.text3, 1, Align::Left, int(textW));
                g.textNumC(so.duration > 0 ? fmtTime(so.duration) : "", { css::Sm }, W - kPad, ry + 30, pal.text3, 1, Align::Right);
                continue;
            }
            ownRow(k, ry, false);
        }
        if (floats >= sortLo && floats <= sortHi && floats - q.pos < n) ownRow(floats - q.pos, sortAt(floats, slot(floats)).y, true);   // lifted, over the others
        g.unclip();
        ui.popArea();
        ui.region(Region::Main);
    };

    // ---- Library: what is in it as large rows, each with its round icon in a colour of the cover's; then a row of albums
    auto libraryHome = [&](float y0, float bottom) {
        ScrollArea& sa = area("ph.libhome");
        sa.view = { 0, y0, W, bottom - y0 };
        const float cs = (W - kPad * 2 - 2 * 14) / 3;
        // the rows shown: Files while the sidebar shows it (Settings › Appearance › Sidebar; the user chose this, 2026-11-04: "A and 2"),
        // the server's while signed in and its music has a tab of its own (Settings › Library › Subsonic server)
        std::vector<int> shown = { 0, 1, 2, 3, 4, 5 };
        if (!(store.hiddenTabs & Store::tabBit(Store::TabFiles))) shown.push_back(6);
        const Subsonic::View sv = subsonic.view();
        // Signed in to a server: Local | Server at the head, the title bar's own switch (the user, 2026-11-06: the whole app's
        // library; 2026-11-09: separate libraries). On Server the rows, their counts and the albums are the server's, as the
        // sidebar's tabs are sideways
        const bool srv = serverSidebar();
        const std::vector<Subsonic::Album>* salb = srv ? &serverAlbums() : nullptr;
        const int nAlbums = srv ? int(salb->size()) : int(lib.albums.size());
        const int albumRows = int(std::min(nAlbums, 9) + 2) / 3;
        sa.content = 110 + float(shown.size()) * 58 + 46 + albumRows * (cs + 52) + 16;
        ui.pushArea(&sa);
        g.clip(0, y0, W, bottom - y0);
        float y = y0 + 16 - sa.y;
        kicker("Library", kPad, y, pal.text3);
        const float srcW = librarySource(W - kPad, y + 30, 36);
        g.text("Library", { css::Xxl, Weight::Bold }, kPad, y + 10, pal.text, 1, Align::Left, int(W - kPad * 2 - (srcW > 0 ? srcW + 12 : 0)));
        if (srv) {
            std::string sub = sv.songs >= 0 ? std::to_string(sv.songs) + " tracks" : std::string();
            if (nAlbums > 0) sub += (sub.empty() ? "" : "  ·  ") + std::to_string(nAlbums) + " albums";
            g.text(sub.empty() ? "Asking the server…" : sub, { css::Sm }, kPad, y + 52, pal.text3);
        } else
            g.text(std::to_string(lib.tracks.size()) + " tracks  ·  " + std::to_string(lib.albums.size()) + " albums", { css::Sm }, kPad, y + 52, pal.text3);
        y += 78;
        float bx = kPad;
        bool c = false;
        // Play and Shuffle: your library's, or the server's
        if (!srv) {
            bx += button(uid("ph.lplay"), ICON_PLAY, "Play", bx, y, true, &c) + 8;
            if (c && !lib.tracks.empty()) { std::vector<int> all(lib.tracks.size()); for (int i = 0; i < int(all.size()); i++) all[i] = i; playTracks(all, 0, "Library", false); }
            c = false;
            button(uid("ph.lshuffle"), ICON_SHUFFLE, "Shuffle", bx, y, false, &c);
            if (c && !lib.tracks.empty()) { std::vector<int> all(lib.tracks.size()); for (int i = 0; i < int(all.size()); i++) all[i] = i; playTracks(all, 0, "Library", true); }
            y += 58;
        } else {
            // the server's, as its Songs page has them (the user, 2026-11-11: "the server library does not have play or shuffle like
            // the local library"): every song the server has told, in its order or shuffled
            auto serverAll = [&](bool shuffle) {
                const std::vector<Subsonic::Song>* all = serverSongList();
                if (!all) { toast("Asking the server: try again in a moment"); return; }
                if (all->empty()) return;
                std::vector<Subsonic::Song> v = *all;
                if (shuffle) { std::mt19937 rng{ std::random_device{}() }; std::shuffle(v.begin(), v.end(), rng); }
                playServer(v, 0, "Library");
            };
            bx += button(uid("ph.splay"), ICON_PLAY, "Play", bx, y, true, &c) + 8;
            if (c) serverAll(false);
            c = false;
            button(uid("ph.sshuffle"), ICON_SHUFFLE, "Shuffle", bx, y, false, &c);
            if (c) serverAll(true);
            y += 58;
        }
        struct Row { const char* icon; const char* name; int count; };
        Row rows[7] = { { ICON_LIST, "Songs", int(lib.tracks.size()) }, { ICON_ALBUMS, "Albums", int(lib.albums.size()) },
                        { ICON_MIC, "Artists", int(artists.size()) },     { ICON_TAG, "Genres", int(genres.size()) },
                        { ICON_FOLDER, "Folders", int(lib.folders.size()) }, { ICON_HEART, "Liked", int(store.liked.size()) },
                        { PLI_SDCARD, "Files", -1 } };
        if (srv) {   // the server's counts, as the sidebar's (those it has told so far)
            rows[0].count = sv.songs;
            rows[1].count = nAlbums > 0 ? nAlbums : -1;
            const auto* la = serverArtistList();
            rows[2].count = la ? int(la->size()) : -1;
            const auto* lg = serverGenreList();
            rows[3].count = lg ? int(lg->size()) : -1;
            rows[4].count = -1;
            if (const auto* lf = serverFolderList()) { int d = 0; for (const auto& e : *lf) d += e.dir; rows[4].count = d; }
        }
        const int nRows = int(shown.size());
        g.roundRect(kPad - 8, y, W - kPad * 2 + 16, nRows * 58, 16, pal.text, 0.04f);
        for (int k = 0; k < nRows; k++, y += 58) {
            const int i = shown[size_t(k)];
            const float cy = y + 29;
            const Id id = uid("ph.lrow", i);
            if (ui.item(id, { kPad - 8, y, W - kPad * 2 + 16, 58 }, 12)) {
                if (srv && i <= 4) openServerSection(i == 0 ? 4 : i - 1);   // (its Songs, Albums, Artists, Genres, Folders)
                else switch (i) {
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
            if (srv && i <= 4) g.iconOnDisc(PLI_CLOUD, kPad + 22 + 15, cy + 14, 7.5f, 10, pal.bg, 0.95f, pal.accentInk, 1);   // the server's mark (the sidebar's)
            g.textC(rows[i].name, { css::Md, Weight::SemiBold }, kPad + 58, cy, pal.text);
            if (rows[i].count >= 0) g.textNum(std::to_string(rows[i].count), { css::Sm }, W - kPad - 28, cy - g.lineHeight({ css::Sm }) / 2.f, pal.text3, 1, Align::Right);
            g.icon(ICON_CHEV, W - kPad - 8, cy, 14, pal.text3);
            if (k < nRows - 1) g.rect(kPad + 58, y + 57, W - kPad * 2 - 58, 1, pal.text, 0.07f);
        }
        y += 28;
        kicker("Albums", kPad, y, pal.text3);
        y += 18;
        for (int i = 0; srv && i < std::min(nAlbums, 9); i++) {   // the server's first albums: their covers from it
            const Subsonic::Album& al = (*salb)[size_t(i)];
            const float x = kPad + (i % 3) * (cs + 14), yy = y + (i / 3) * (cs + 52);
            const Id id = uid("ph.lsalbum", i);
            if (ui.item(id, { x, yy, cs, cs + 44 }, 12)) openServer(2, al.id, al.name);
            if (SDL_Texture* tex = serverCover(al.cover)) g.image(tex, x, yy, cs, cs, 12);
            else { g.roundRect(x, yy, cs, cs, 12, pal.text, 0.06f); g.icon(ICON_DISC, x + cs / 2, yy + cs / 2, cs * 0.28f, pal.text3); }
            g.text(al.name, { css::Sm, Weight::SemiBold }, x, yy + cs + 8, pal.text, 1, Align::Left, int(cs));
            g.text(al.artist, { css::Xs }, x, yy + cs + 26, pal.text3, 1, Align::Left, int(cs));
        }
        for (int i = 0; !srv && i < int(std::min<size_t>(lib.albums.size(), 9)); i++) {
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

    // ---- Search: its field on top (Android's keyboard through Songate's text card), Local | Server beside it while signed in to a
    // server (the title bar's switch: what is searched), then sideways' own filter (the user, 2026-11-09): Songs, Albums, Artists,
    // Genres, Playlists and Lyrics, the same control, across the page; under it what that one found, as its own page shows it
    auto searchPage = [&](float y0, float bottom) {
        const float fy = y0 + 8;
        const float srcW = librarySourceW(), fw = W - kPad * 2 - (srcW > 0 ? srcW + 8 : 0);
        const Id fid = uid("ph.sfield");
        if (ui.item(fid, { kPad, fy, fw, 44 }, 12)) openSearch();
        const std::string& qtext = route.query;
        g.roundRect(kPad, fy, fw, 44, 12, pal.text, ui.pressed(fid) ? 0.1f : 0.065f);
        g.roundStroke(kPad, fy, fw, 44, 12, 1, qtext.empty() ? pal.text : pal.accent, qtext.empty() ? 0.09f : 0.55f);
        g.icon(ICON_SEARCH, kPad + 24, fy + 22, 18, pal.text3);
        // (beside the Local | Server switch the field is too short for the tabs' names: the server's own short line, never a cut one)
        const char* hint = srcW > 0 ? (store.libSource == 1 ? "Search the server" : "Search your music") : searchHint();
        g.textC(qtext.empty() ? hint : qtext, { css::Base }, kPad + 44, fy + 22, qtext.empty() ? pal.text3 : pal.text, 1, Align::Left, int(fw - 90));
        if (skb_.open) {   // Search's keyboard is up: typing here (the caret after the words, the field lit)
            g.roundStroke(kPad, fy, fw, 44, 12, 1.5f, pal.accent, 0.8f);
            const float cxr = kPad + 44 + (qtext.empty() ? 0.f : std::min(g.measureF(qtext, { css::Base }), fw - 90)) + 1;
            if (std::fmod(time - skb_.at, 1.06) < 0.62) g.rect(cxr, fy + 22 - 11, 1.5f, 22, pal.text);
        }
        if (!qtext.empty() && iconButton(uid("ph.sclear"), ICON_X, kPad + fw - 22, fy + 22, false, 36, 16)) navigate(View::Search, -1, std::string(), false);
        if (srcW > 0) librarySource(W - kPad, fy + 22, 44);
        if (qtext.empty()) {
            const bool srv = serverSidebar();
            const std::vector<std::string> lines = g.wrap(srv ? "Find a song, an album, an artist, a genre or a playlist on the server"
                                                              : "Find a song by its title, its artist, its album or a line of its lyrics", { css::Sm }, int(W - kPad * 4), 3);
            float ty = fy + 110 - lines.size() * 9.f;
            for (const auto& l : lines) { g.textC(l, { css::Sm }, W / 2, ty, pal.text3, 1, Align::Center); ty += 18; }
            return;
        }
        const float py = fy + 44 + 10;
        {
            static const std::vector<std::string> local = { "Songs", "Albums", "Artists", "Genres", "Playlists", "Lyrics" };
            static const std::vector<std::string> server = { "Songs", "Albums", "Artists", "Genres", "Playlists" };   // (its songs carry no lyrics to read)
            const bool srv = serverSidebar();
            const int cur = srv ? std::min(searchTab_, 4) : searchTab_;
            int picked = -1;
            segmented(uid("ph.stab"), W - kPad, py + 18, srv ? server : local, cur, &picked, 0, W - kPad * 2, 2, 36);
            if (picked >= 0 && picked != cur) {
                searchTab_ = picked;
                ScrollArea& la = area("search", int(View::Search) * 1000 + route.ref);
                la.target = la.y = 0;
            }
        }
        const float ly = py + 36 + 4;
        drawTrackList(0, ly, W, bottom - ly);
    };

    // ---- the page
    // An Automix join on Now playing: Full, the only one upright (the user, 2026-11-04): the cover glides to the middle of the screen
    // and grows, frosted with the hint (drawNowCover), while everything else fades out, and back as the join ends (automixFocus)
    const bool joining = v == View::Now && now.track >= 0 && mixUntil > mixShownAt && time >= mixShownAt && time < mixUntil;
    const Focus fc = joining ? automixFocus(0) : Focus{};
    phoneJoinMove_ = joining ? fc.move : -1;
    g.setFadeBase(fc.ui);   // (the page's own fades, the lyrics page's and the mini player's, under it)
    if (grab.kind == Grab::Queue && grab.upNext && grab.pos >= 0 && !phoneQueue_) grabDrop();   // Up next closed with a row picked up: put down
    // (long lines start their round again with each song, as the player bar does sideways: it is not drawn here)
    if (now.track != marqueeTrack || now.info.title != marqueeTitle) { marqueeTrack = now.track; marqueeTitle = now.info.title; marqueeAt = time; }
    top();
    const float pageTop = kTopH, pageBottom = H - kNavH - (v == View::Now ? 0 : kMiniH + 16);
    // The Apps page (the bottom bar's sixth button, while Songate is the home screen and Show Apps is on): a grid of the device's
    // other apps, four across, each on Songate's own tile: Songate's symbol for it, or its own icon in grey. A finger held on one:
    // its menu (openAppMenu). Its Move: the tiles dragged into order, as the Playlists page's (App::sortRun), Done to end.
    if (phoneApps_ && !appsTab()) phoneApps_ = false;
    if (phoneApps_) {
        appsGrid(0, pageTop, W, H - kNavH - kMiniH - 16, kPad, 4, false);
        mini();
        nav();
        appsAct();
        return;
    }
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
    case View::Server: drawServer(0, pageTop, W, pageBottom - pageTop); break;
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

/**
 * What the sidebar carries above Settings, at the head of Settings' list on a phone upright (no sidebar there): the work running in
 * the background as its stack of cards (the same cards: pressed, the list of them, a job's menu with Stop, or its Settings row),
 * and in their place a newer version's banner while it is offered (pressed: its sheet, App::openUpdateOffer). One fades out, then
 * the other in, as sideways. Returns the height it takes in the list, its gap included: 0 with nothing to show.
 */
float App::phoneWorkCard(float x, float y, float w) {
    const int offer = updateOffer();
    const float want = offer >= 0 ? 1.f : 0.f;
    updBanner_ = store.motion ? updBanner_ + (want - updBanner_) * (1 - std::exp(-dt / 0.12f)) : want;
    if (std::fabs(updBanner_ - want) < 0.004f) updBanner_ = want;
    const float need = jobsHeight(false);   // (the front card and a peek for each behind it; the banner: one card's)
    if (need < 0.5f) return 0;
    const JobStack st = jobStack();
    const float ub = updBanner_, fadeWas = g.fade();
    const float bottom = y + need - 8, frontH = 44, peek = 7;
    if (ub < 0.5f) {
        g.setFade(fadeWas * (1 - ub * 2));
        // only the strip above the front card (it is see-through), inside the page's own clip, which is put back after
        SDL_Renderer* rr = g.renderer();
        const bool hadClip = SDL_RenderIsClipEnabled(rr);
        SDL_Rect was{};
        SDL_RenderGetClipRect(rr, &was);
        const SDL_Rect strip{ int(std::floor(x)), int(std::floor(y - 4)), int(std::ceil(w)), int(std::ceil(std::max(0.f, bottom - frontH - (y - 4)))) };
        SDL_Rect cut = strip;
        if (hadClip && !SDL_IntersectRect(&strip, &was, &cut)) cut = { 0, 0, 0, 0 };
        SDL_RenderSetClipRect(rr, &cut);
        for (int k = int(std::ceil(st.behind - 0.01f)); k >= 1; k--) {
            const float part = std::min(1.f, st.behind - (k - 1));
            const float inset = 8.f * k, top = bottom - frontH - peek * std::min(float(k), st.behind);
            g.roundRect(x + inset, top, w - inset * 2, frontH, 10, pal.text, (0.07f - 0.015f * k) * part);
            g.roundStroke(x + inset, top, w - inset * 2, frontH, 10, 1, pal.text, (0.12f - 0.03f * k) * part);
        }
        SDL_RenderSetClipRect(rr, hadClip ? &was : nullptr);
        if (st.body > 0.01f) drawJobCard(st, x, bottom, w, false);
    }
    g.setFade(fadeWas);
    if (ub >= 0.5f && offer >= 0) {
        g.setFade(fadeWas * (ub * 2 - 1));
        drawUpdateBanner(x, bottom, w, false, offer);
        g.setFade(fadeWas);
    }
    return need + 8;
}

/** Upright: where a page ends over the bar of five (and the mini player, off Now playing): what floats over the foot sits there (Done, while a list is in sort mode). */
float App::phoneFoot() const { return route.view == View::Now ? css::H - 18.f : css::H - kNavH - kMiniH - 8 - 10; }
// ------------------------------------------------------------ the Apps page's apps: what Songate keeps about them, an app's menu

/** The apps installed, asked again every few seconds while the page shows (now: at once): Songate's order kept, a new app after
 *  the others, one that went dropped. The first time, apps.tsv is read. */
void App::appsRefresh(bool now) {
    if (!now && time - gAppsAt < 6 && time >= gAppsAt) return;
    gAppsAt = time;
    std::vector<AppEntry> kept = gApps;
    if (!gAppsRead) {
        gAppsRead = true;
        std::string text;
        if (FILE* f = fopen(store.path("apps.tsv").c_str(), "rb")) {
            char buf[4096];
            for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, k);
            fclose(f);
        }
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> cell;
            for (size_t c = 0;;) {
                const size_t t = line.find('\t', c);
                cell.push_back(line.substr(c, t == std::string::npos ? std::string::npos : t - c));
                if (t == std::string::npos) break;
                c = t + 1;
            }
            if (cell.size() < 4 || cell[0].empty()) continue;
            kept.push_back({ {}, cell[0], cell[1], cell[2], cell[3] == "1" });
        }
    }
    const auto installed = plat::apps();   // by name
    std::vector<AppEntry> next;
    for (const AppEntry& e : kept)
        for (const auto& [name, app] : installed)
            if (app == e.app) { next.push_back({ name, app, e.icon, e.custom, e.hidden }); break; }
    for (const auto& [name, app] : installed) {
        bool have = false;
        for (const AppEntry& e : next) if (e.app == app) { have = true; break; }
        if (!have) next.push_back({ name, app, {}, {}, false });
    }
    gApps = std::move(next);
}

/** apps.tsv: written when the order, a symbol, a name or what is hidden changed, at once (a few lines). */
void App::appsSave() {
    std::string out;
    for (const AppEntry& e : gApps) {
        std::string custom = e.custom;
        for (char& c : custom) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        out += e.app + "\t" + e.icon + "\t" + custom + "\t" + (e.hidden ? "1" : "0") + "\n";
    }
    if (FILE* f = fopen(store.path("apps.tsv").c_str(), "wb")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
}

/** The sort mode's move: the app shown at from goes to to, the ones between closing up (the hidden ones keep out of it). */
void App::appsMove(int from, int to) {
    const std::vector<int> shown = appsShown();
    const int n = int(shown.size());
    if (from < 0 || to < 0 || from >= n || to >= n || from == to) return;
    const int d = to > from ? 1 : -1;
    const AppEntry moved = gApps[size_t(shown[size_t(from)])];
    for (int k = from; k != to; k += d) gApps[size_t(shown[size_t(k)])] = gApps[size_t(shown[size_t(k + d)])];
    gApps[size_t(shown[size_t(to)])] = moved;
}

int App::appsHidden() {
    appsRefresh();
    int n = 0;
    for (const AppEntry& e : gApps) n += e.hidden ? 1 : 0;
    return n;
}

void App::appsShowAll() {
    for (AppEntry& e : gApps) e.hidden = false;
    appsSave();
    toast("Every app is on the Apps page again");
}

/** An app's menu: a finger held on its tile. */
/** The Apps page's grid in x, top0, w down to bottom: the device's other apps, `cols` across, each on Songate's own tile (its
 *  symbol for the app, or the app's own icon in grey). A tap opens one; a finger held on it (or the menu button, sideways) opens
 *  its menu (openAppMenu). Its Move: the tiles dragged into order, as the Playlists page's (App::sortRun), Done to end.
 *  Upright (the bottom bar's sixth button) and sideways (the sidebar's Apps tab, drawApps) are this same page. */
void App::appsGrid(float x, float top0, float w, float bottom, float pad, int cols, bool wide) {
    const bool sort = sorting(Grab::Apps);
    if (!sort) appsRefresh();
    const std::vector<int> shown = appsShown();
    const int n = int(shown.size());
    // sideways the tiles are larger (the user, session 19, from the mock-ups: "B, larger tiles"): 84 where upright has 60
    const float tile = wide ? 84 : 60, cellH = wide ? 136 : 108, ts = tile / 60;
    const float cw = (w - 2 * pad) / cols;
    const float head = wide ? 112 : 62;
    ScrollArea& a = area("ph.apps");
    a.view = { x, top0, w, bottom - top0 };
    a.content = head + ((n + cols - 1) / cols) * cellH + 20;
    g.clip(x, top0, w, bottom - top0);
    ui.pushArea(&a);
    if (wide) {   // a page beside the sidebar: its head as the other pages' (drawPlaylists': kicker, title, a count under it)
        float cy = top0 - a.y + 18 + 8;
        kicker("Home screen", x + pad, cy, pal.text3);
        cy += 10;
        g.text("Apps", { css::Xxl, Weight::Bold }, x + pad, cy, pal.text);
        cy += 38;
        g.text(std::to_string(n) + (n == 1 ? " app" : " apps"), { css::Sm }, x + pad, cy + 4, pal.text3);
    } else g.text("Apps", { css::Xl, Weight::Bold }, x + pad, top0 + 16 - a.y, pal.text);
    const float gridTop = top0 + head - a.y;
    auto slot = [&](int i) -> SDL_FRect { return { x + pad + cw * (i % cols) + 2, gridTop + (i / cols) * cellH, cw - 4, cellH - 6 }; };
    if (sort) sortRun(n, 0, n - 1, a, slot, true);
    const int floats = sort ? (tsort_.held >= 0 ? tsort_.held : tsort_.settle) : -1;
    int loaded = 0, menu = -1;
    std::string open;
    appHover_ = -1;
    for (int k = 0; k <= n; k++) {
        const bool fl = k == n;   // (after the others: the tile held, or settling)
        if (fl && (floats < 0 || floats >= n)) break;
        const int i = fl ? floats : k;
        if (!fl && i == floats) continue;
        const SDL_FRect home = slot(i);
        const SDL_FPoint at = sort ? sortAt(i, home) : SDL_FPoint{ home.x, home.y };
        if (!fl && (at.y + home.h < top0 || at.y > bottom)) continue;
        const AppEntry& e = gApps[size_t(shown[size_t(i)])];
        const SDL_FRect cell{ at.x, at.y, home.w, home.h };
        const float cx = cell.x + cell.w / 2;
        if (fl) sortFloatBox(cell.x, cell.y, cell.w, cell.h, 16);
        const Id id = uid("ph.app", i);
        const bool hit = ui.item(id, cell, 16) && !sort;   // (in sort mode a tap opens nothing)
        if (sort) sortGrip("ph.appsort", i, cell, cell);   // (the whole tile drags)
        if (!sort && ui.focused(id)) appHover_ = shown[size_t(i)];
        if (!sort && (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing()))) g.roundRect(cell.x, cell.y, cell.w, cell.h, 16, pal.text, 0.07f);
        const float tx = cx - tile / 2, tyy = cell.y + 8;
        g.roundRect(tx, tyy, tile, tile, 18 * ts, pal.text, 0.07f);
        g.roundStroke(tx, tyy, tile, tile, 18 * ts, 1, pal.text, 0.1f);
        if (const char* symbol = appWears(e)) {
            g.icon(symbol, cx, tyy + tile / 2, 28 * ts, pal.accent);
        } else {
            auto it = gAppIcons.find(e.app);
            if (it == gAppIcons.end() && loaded < 2) {   // (two a frame: Android draws each on this thread)
                loaded++;
                std::vector<uint32_t> px;
                SDL_Texture* t = nullptr;
                if (plat::appIcon(e.app, 96, px) && (t = SDL_CreateTexture(g.renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 96, 96))) {
                    for (uint32_t& q : px) {   // grey, its own shape kept: it sits in Songate's colours, not its maker's
                        const uint32_t l = (((q >> 16) & 255) * 77 + ((q >> 8) & 255) * 150 + (q & 255) * 29) >> 8;
                        q = (q & 0xff000000u) | (l << 16) | (l << 8) | l;
                    }
                    SDL_UpdateTexture(t, nullptr, px.data(), 96 * 4);
                    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
                }
                it = gAppIcons.emplace(e.app, t).first;
            }
            if (it != gAppIcons.end() && it->second) g.image(it->second, cx - 20 * ts, tyy + tile / 2 - 20 * ts, 40 * ts, 40 * ts, 10 * ts, 0.92f);
        }
        g.text(appName(e), { css::Sm }, cx, tyy + tile + 8, pal.text2, 1, Align::Center, int(cw - 6));
        if (!sort && ui.touchHeld() == id) menu = shown[size_t(i)];
        else if (hit) open = e.app;
    }
    ui.popArea();
    g.unclip();
    appMenuAsk_ = menu;   // (acted on after the page is drawn: appsAct)
    appOpenAsk_ = open;
}

/** What the grid was asked this frame, once everything over it is drawn (it changes nothing the page's loop still reads). */
void App::appsAct() {
    const int menu = appMenuAsk_;
    const std::string open = appOpenAsk_;
    appMenuAsk_ = -1;
    appOpenAsk_.clear();
    if (menu >= 0) openAppMenu(menu);
    else if (!open.empty()) plat::openApp(open);
}

/** Sideways: the sidebar's Apps tab's page, the same grid across the page's width. */
void App::drawApps(float x, float y, float w, float h) {
    appsGrid(x, y, w, y + h, 28, std::max(4, int((w - 56) / 185)), true);   // (six across on a phone's 20:9 screen)
    appsAct();
}

/** The menu button on the Apps page: the menu of the app under focus. */
void App::appsPlusMenu() {
    if (appHover_ >= 0 && appHover_ < int(gApps.size())) openAppMenu(appHover_);
}

void App::openAppMenu(int at) {
    if (at < 0 || at >= int(gApps.size())) return;
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::AppMenu;
    sheet.arg = gApps[size_t(at)].app;
    sheet.name = appName(gApps[size_t(at)]);
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

void App::appSheetHead(const char*& icon, std::string& kick, std::string& title, std::string& sub) {
    const int at = appAt(sheet.arg);
    const char* symbol = at >= 0 ? appWears(gApps[size_t(at)]) : nullptr;
    icon = symbol ? symbol : ICON_APPS;
    kick = "App";
    title = at >= 0 ? appName(gApps[size_t(at)]) : sheet.name;
    sub = sheet.page == Sheet::AppIcon ? "Its symbol on the Apps page: " GF_TAP " one" : "More for this app";
}

void App::appMenuRows(std::vector<SheetRow>& rows) {
    const int at = appAt(sheet.arg);
    if (at < 0) { closeSheet(); return; }   // (it went while the menu was open)
    const std::string app = sheet.arg;
    const char* symbol = appWears(gApps[size_t(at)]);
    rows.push_back({ SheetRow::Action, ICON_PLAY, "Open", "", "", [this, app] {
                         closeSheet();
                         plat::openApp(app);
                     } });
    rows.push_back({ SheetRow::Action, symbol ? symbol : ICON_APPS, "Icon…", "Its symbol on the Apps page", "", [this] {
                         sheet.page = Sheet::AppIcon;
                         ui.focusDefault();
                     } });
    if (appsShown().size() > 1)
        rows.push_back({ SheetRow::Action, ICON_SORT_DOWN, "Move", "", "", [this, app] {
                             closeSheet();
                             grabDrop();
                             sortEnd();
                             const std::vector<int> shown = appsShown();
                             const int me = appAt(app);
                             tsort_ = TouchSort{};
                             tsort_.on = true;
                             tsort_.kind = Grab::Apps;
                             tsort_.ran = true;
                             for (size_t i = 0; i < shown.size(); i++) if (shown[i] == me) tsort_.show = int(i);
                             toast("Drag them into order, then " GF_TAP " Done");
                         } });
    rows.push_back({ SheetRow::Action, ICON_TAG, "Rename", "", "", [this, app] {
                         closeSheet();
                         const int i = appAt(app);
                         if (i < 0) return;
                         std::string name = appName(gApps[size_t(i)]);
                         if (!keyboard("Rename the app", name, name)) return;
                         while (!name.empty() && name.back() == ' ') name.pop_back();
                         while (!name.empty() && name[0] == ' ') name.erase(0, 1);
                         const int j = appAt(app);
                         if (j < 0) return;
                         gApps[size_t(j)].custom = name == gApps[size_t(j)].name ? std::string() : name;   // (emptied: its own name again)
                         appsSave();
                     } });
    rows.push_back({ SheetRow::Action, ICON_X, "Hide", "Off the Apps page; Settings › Home screen brings it back", "", [this, app] {
                         closeSheet();
                         const int i = appAt(app);
                         if (i < 0) return;
                         gApps[size_t(i)].hidden = true;
                         appsSave();
                         toast("Hidden");
                     } });
    rows.push_back({ SheetRow::Action, ICON_INFO, "App settings", "Android's page for this app", "", [this, app] {
                         closeSheet();
                         plat::appPage(app, false);
                     } });
    rows.push_back({ SheetRow::Action, ICON_X, "Uninstall", "Android asks before it removes it", "", [this, app] {
                         closeSheet();
                         plat::appPage(app, true);
                         gAppsAt = -100;   // (the page asks again at once when Songate is back in front)
                     } });
}

/** Icon…'s tiles: the first follows the app's kind (none chosen), then Songate's symbols for kinds of app, then the playlists'. */
int App::appIconCount() const { return 1 + int(std::size(kAppKinds)) + int(std::size(kPlaylistIcons)); }

void App::appIconGrid(float x, float ry, float w, float by, float bodyH, int cols, float tile, float rowH) {
    const int at = appAt(sheet.arg);
    if (at < 0) { closeSheet(); return; }
    AppEntry& e = gApps[size_t(at)];
    const int nk = int(std::size(kAppKinds)), n = appIconCount();
    const float gap = (w - 48 - cols * tile) / (cols - 1);
    const char* automatic = appSymbol(e.name, e.app);
    for (int k = 0; k < n; k++) {
        const char* svg = k == 0 ? (automatic ? automatic : ICON_SPARKLE) : k <= nk ? kAppKinds[k - 1].svg : kPlaylistIcons[k - 1 - nk].svg;
        const std::string key = k == 0 ? std::string() : k <= nk ? std::string("app:") + kAppKinds[k - 1].key : std::string(kPlaylistIcons[k - 1 - nk].key);
        const float tx = x + 24 + (k % cols) * (tile + gap), ty = ry + 2 + (k / cols) * rowH;
        const Id tid = uid("sp.appicon", k);
        const bool on = e.icon == key;
        const bool hit = ui.item(tid, { tx, ty, tile, tile }, 14, on ? DefaultFocus : 0);
        if (ty + tile > by - 4 && ty < by + bodyH + 4) {
            const bool hot = ui.pressed(tid) || (ui.focused(tid) && ui.showFocusRing());
            g.roundRect(tx, ty, tile, tile, 14, on ? pal.accent : pal.text, on ? 0.18f : hot ? 0.1f : 0.05f);
            if (on) g.roundStroke(tx, ty, tile, tile, 14, 1.5f, pal.accent, 0.6f);
            g.icon(svg, tx + tile / 2, ty + tile / 2, 24, on ? pal.accentInk : hot ? pal.text : pal.text2);
        }
        if (hit) {
            e.icon = key;
            appsSave();
            closeSheet();
            break;
        }
    }
}

/** Whether Android has Songate as its home screen now (asked once a second; the switch asks again at once). */
bool App::homeHeld() {
    if (time - homeHeldAt_ > 1 || time < homeHeldAt_) {
        homeHeld_ = plat::homeHeld();
        homeHeldAt_ = time;
    }
    return homeHeld_;
}

#endif
