/*
 * Gatefold NX · shell.cpp
 * The frame around every view: #bg, #titlebar, #sidebar, #playerbar (app.css).
 */
#include "words.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {

/** var(--ease) = cubic-bezier(.22, .7, .18, 1), as ambient.js evaluates it. */
float ease(float t) {
    const float x1 = 0.22f, y1 = 0.7f, x2 = 0.18f, y2 = 1;
    auto bx = [&](float u) { return 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u; };
    auto by = [&](float u) { return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u; };
    float lo = 0, hi = 1;
    for (int i = 0; i < 18; i++) { const float m = (lo + hi) / 2; if (bx(m) < t) lo = m; else hi = m; }
    return by((lo + hi) / 2);
}

}  // namespace

// ------------------------------------------------------------------ screen edges (Android)

/** Settings › Appearance › Screen edges: Android's cutout boxes (any shape, any edge) and the rounded corners, made into how far each
 *  piece of the frame keeps from them. Step aside moves only a piece a box overlaps; Keep the side clear and Black strip keep every
 *  piece on a box's side clear of it (Black strip also paints the band, drawEdges); Use the whole screen moves nothing. The corners
 *  move pieces as Step aside does whatever the mode. Eased as the phone turns; Motion off, at once. */
void App::updateEdges() {
#ifdef GF_ANDROID
    static std::vector<plat::EdgeBox> raw;
    static float radius = 0;
    static int forW = 0, forH = 0;
    std::vector<plat::EdgeBox> got;
    float r = 0;
    int gw = 0, gh = 0;
    const unsigned gen = plat::screenEdges(got, r, gw, gh);
    const bool fresh = edgeGen_ == ~0u;
    if (gen != edgeGen_) {
        raw = std::move(got);
        radius = r;
        forW = gw;
        forH = gh;
        edgeGen_ = gen;
#ifdef GF_DEV
        // tests: edge-test.txt in the settings folder stands in for Android's boxes ("x y w h" a line, window pixels), for shapes no
        // device here has (a notch, two, a tablet's camera on a long edge); read once, as the app starts
        static const std::vector<plat::EdgeBox> fake = [this] {
            std::vector<plat::EdgeBox> v;
            if (FILE* f = fopen(store.path("edge-test.txt").c_str(), "r")) {
                plat::EdgeBox b{};
                while (fscanf(f, "%f %f %f %f", &b.x, &b.y, &b.w, &b.h) == 4) v.push_back(b);
                fclose(f);
            }
            return v;
        }();
        if (!fake.empty()) raw = fake;
        // for tests: what Android said, in window pixels (the screenshot cannot show a cutout)
        std::string l = "Screen edges: radius " + std::to_string(int(radius)) + ", boxes";
        for (const plat::EdgeBox& b : raw) l += " " + std::to_string(int(b.x)) + "," + std::to_string(int(b.y)) + " " + std::to_string(int(b.w)) + "x" + std::to_string(int(b.h));
        logLine(l);
#endif
    }
    const float W = float(css::W), H = float(css::H), s = scale_ > 0 ? scale_ : 1, extra = store.edgeExtra * 24;
    // the boxes in app pixels, with the extra room around each; none while they were measured for the window turned the other way
    // (a turn: the sideways cutout on the left kept a black band there upright until Android said again)
    const bool sameWay = forW <= 0 || forH <= 0 || (forW > forH) == (framePw_ > framePh_);
    std::vector<SDL_FRect> cuts, corners;
    for (const plat::EdgeBox& b : raw) {
        if (!sameWay) break;
        const float x0 = std::max(0.f, b.x / s - extra), y0 = std::max(0.f, b.y / s - extra);
        const float x1 = std::min(W, (b.x + b.w) / s + extra), y1 = std::min(H, (b.y + b.h) / s + extra);
        if (x1 > x0 && y1 > y0) cuts.push_back({ x0, y0, x1 - x0, y1 - y0 });
    }
    if (store.edgeCorners && radius > 0) {   // a square inside each corner's curve: past it, a piece is clear of the rounding
        const float c = radius / s * 0.3f;
        for (const SDL_FRect& k : { SDL_FRect{ 0, 0, c, c }, SDL_FRect{ W - c, 0, c, c }, SDL_FRect{ 0, H - c, c, c }, SDL_FRect{ W - c, H - c, c, c } })
            corners.push_back(k);
    }
    edgeBoxes_ = cuts;
    edgeBoxes_.insert(edgeBoxes_.end(), corners.begin(), corners.end());

    Edges w;
    // where each piece's content is: the sidebar's last row (Settings) and the page end 12 px above the player bar
    const float bandTop = css::TbH, bandBottom = H - css::BarH, contentBottom = bandBottom - 12;
    auto overlapsY = [](const SDL_FRect& b, float a, float z) { return b.y < z && b.y + b.h > a; };
    // Step aside, for one box: each piece it overlaps, just past it (the pieces' own margins: 10 and 18 px)
    auto step = [&](const SDL_FRect& b) {
        const bool left = b.x < W / 3, right = b.x + b.w > W * 2 / 3;
        if (overlapsY(b, 0, bandTop)) {
            if (left) w.tbL = std::max(w.tbL, b.x + b.w - 6);
            else if (right) w.tbR = std::max(w.tbR, W - b.x - 14);
            else if (b.x + b.w / 2 < W / 2) w.searchL = std::max(w.searchL, b.x + b.w + 8);   // a hole in the middle of a long edge
            else w.searchR = w.searchR > 0 ? std::min(w.searchR, b.x - 8) : b.x - 8;
        }
        if (overlapsY(b, bandTop, contentBottom)) {
            if (b.x < sideW) w.side = std::max(w.side, b.x + b.w - 6);
            if (b.x + b.w > W - 60) w.mainR = std::max(w.mainR, W - b.x);
        }
        if (overlapsY(b, bandBottom, H)) {
            if (left) w.barL = std::max(w.barL, b.x + b.w - 14);
            else if (right) w.barR = std::max(w.barR, W - b.x - 14);
        }
    };
    if (store.edgeMode == 0) for (const SDL_FRect& b : cuts) step(b);
    else if (store.edgeMode == 1 || store.edgeMode == 2) {
        // the side a box is on, whole: from the edge to its far side
        float L = 0, R = 0;
        for (const SDL_FRect& b : cuts) {
            if (b.x + b.w / 2 < W / 2 && b.x < W / 4) L = std::max(L, b.x + b.w);
            else if (b.x + b.w / 2 >= W / 2 && b.x + b.w > W * 3 / 4) R = std::max(R, W - b.x);
            else step(b);   // on a long edge, away from the sides: as Step aside
        }
        w.tbL = std::max(w.tbL, L);
        w.side = std::max(w.side, L);
        w.barL = std::max(w.barL, L);
        w.tbR = std::max(w.tbR, R);
        w.mainR = std::max(w.mainR, R);
        w.barR = std::max(w.barR, R);
        if (store.edgeMode == 2) { w.stripL = L; w.stripR = R; }
    }
    if (store.edgeMode != 3) for (const SDL_FRect& b : corners) step(b);
    edgeWant_ = w;

    // eased towards it (a turn moves the pieces across), at once with Motion off or at the first frame
    const float k = fresh || !store.motion ? 1.f : 1.f - std::exp(-dt / 0.07f);
    auto go = [&](float& v, float want) { v += (want - v) * k; if (std::fabs(want - v) < 0.05f) v = want; };
    go(edge_.tbL, w.tbL);
    go(edge_.tbR, w.tbR);
    go(edge_.side, w.side);
    go(edge_.barL, w.barL);
    go(edge_.barR, w.barR);
    go(edge_.mainR, w.mainR);
    go(edge_.stripL, w.stripL);
    go(edge_.stripR, w.stripR);
    edge_.searchL = w.searchL;   // places, not distances: they jump
    edge_.searchR = w.searchR;
#endif
}

/** Black strip's bands over everything, and Show where it is: the boxes outlined for a few seconds. */
void App::drawEdges() {
    if (edge_.stripL > 0.5f) g.rect(0, 0, edge_.stripL, css::H, { 0, 0, 0 });
    if (edge_.stripR > 0.5f) g.rect(css::W - edge_.stripR, 0, edge_.stripR, css::H, { 0, 0, 0 });
    if (time >= edgeShowUntil_) return;
    const float a = std::clamp(float(edgeShowUntil_ - time) / 0.5f, 0.f, 1.f);   // the last half second fades
    for (const SDL_FRect& b : edgeBoxes_) {
        g.roundRect(b.x, b.y, b.w, b.h, 6, pal.accent, 0.28f * a);
        g.roundStroke(b.x, b.y, b.w, b.h, 6, 2, pal.accent, 0.9f * a);
    }
}

void App::draw() {
    updateEdges();
#if defined(GF_ANDROID) && defined(GF_DEV)
    if (portraitMock_ >= 0 && css::H > css::W) {   // a portrait mock-up instead of the app (view_portrait.cpp)
        drawBackground();
        drawPortraitMock(portraitMock_);
        return;
    }
#endif
    hoverTrackShown = hoverTrack;
    hoverTrack = -1;
    hoverSrvShownOn_ = hoverSrvOn_;   // (a server track's row: its song, and its place on a server playlist's page)
    if (hoverSrvOn_) { hoverSrvShown_ = hoverSrv_; hoverSrvIdxShown_ = hoverSrvIdx_; }
    hoverSrvOn_ = false;
    hoverSrvIdx_ = -1;
    hoverSrvPlShown_ = hoverSrvPl_;
    hoverSrvPl_ = -1;
    hoverQueueShown = hoverQueue;
    hoverQueue = -1;
    hoverUpNextShown = hoverUpNext;
    hoverUpNext = false;
    plHoverShown = route.view == View::Playlists ? plHover : -2;
    ui.setModal(sheet.page != Sheet::Closed || ((coverNotice == 1 || appCoverNotice == 1 || lyrNotice == 1) && !canvas) || sleepPromptAt > 0 || tourStep >= 0 || je.open || ap.open);
    if (canvas) {
        g.holdCache(automixFocus(mixCanvasMode() == 0 ? 0 : 2).ui < 1);   // its text stays cached while an Automix join hides it (as Now playing's)
        drawCanvas();
        profMark(2);
#ifdef GF_DEV
        g.saidLayer = 1;   // (tests: the overlays from here)
#endif
        drawSheet();
        drawTour();
        drawSleepPrompt();
        drawToast();
        if (veilA > 0.005f) g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, veilA);
        drawFocusRing();
        drawHint();
        drawEdges();
        drawPointer();
        drawFrameTimes();
        profMark(5);
        return;
    }
    drawBackground();
    profMark(1);
#ifdef GF_ANDROID
    if (phonePortrait()) {   // a phone held upright: its own layout (view_phone.cpp), the same overlays over it
        ui.region(Region::Main);
        drawPhone();
        profMark(4);
#ifdef GF_DEV
        g.saidLayer = 1;   // (tests: the overlays from here)
#endif
        drawCoverNotice();
        drawJoinEditor();
        drawAutomixPicker();
        drawSheet();
        drawTour();
        drawSleepPrompt();
        drawToast();
        if (veilA > 0.005f) g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, veilA);
        drawFocusRing();
        drawHint();
        drawEdges();
        drawPointer();
        drawFrameTimes();
        profMark(5);
        warmCanvas();
        profMark(6);
        return;
    }
#endif
    // Automix joining on Now playing (automixFocus): how much of the interface shows. Fully faded, it is not drawn at all:
    // drawn at nothing it cost a whole frame's layout on the console, and the join's animation dropped frames (the user's
    // video, 2026-10-28: the light held for two or three frames, then jumped). Focus stays where it was meanwhile
    // (Full only: Immersive keeps the sidebar and the bars, fading the page's info alone; Simple fades nothing)
    const float shown = route.view == View::Now ? automixFocus(store.mixAnimNow == 0 ? 0 : 2).ui : 1.f;
    const bool hidden = shown <= 0.001f;
    g.holdCache(shown < 1);   // what it shows stays cached meanwhile, so its return is not a stutter

    const float mx = std::round(sideW + edge_.side), my = css::TbH, mw = css::W - mx - std::round(edge_.mainR), mh = css::H - css::TbH - css::BarH;
    ui.region(Region::Main);
    g.clip(mx, my, mw, mh);
    switch (route.view) {
    case View::Now: drawNow(mx, my, mw, mh); break;
    case View::Library: drawLibrary(mx, my, mw, mh); break;
    case View::Albums: drawAlbums(mx, my, mw, mh); break;
    case View::Artists: drawArtists(mx, my, mw, mh); break;
    case View::Artist: drawArtist(mx, my, mw, mh); break;
    case View::Genres: drawArtists(mx, my, mw, mh, true); break;
    case View::Genre: drawArtist(mx, my, mw, mh, true); break;
    case View::Album: drawAlbum(mx, my, mw, mh); break;
    case View::Folders: drawFolders(mx, my, mw, mh); break;
    case View::Settings: drawSettings(mx, my, mw, mh); break;
    case View::Files: drawFiles(mx, my, mw, mh); break;
    case View::Playlists: drawPlaylists(mx, my, mw, mh); break;
    case View::Server: drawServer(mx, my, mw, mh); break;
    case View::PickFolder:
        if (folderFor >= 0 && route.view != View::PickFolder) folderFor = -1;
        [[fallthrough]];
    case View::PickPlaylist:
    case View::PickExport: drawPicker(mx, my, mw, mh); break;
    default: drawTrackList(mx, my, mw, mh); break;
    }
    g.unclip();
    profMark(2);

    // ... the sidebar and the bars fade with the page's info, the cover glides over them all (drawNow's glideCover_; outside
    // the page's clip)
    if (shown < 1) g.setFade(shown);
    if (!hidden) {
        ui.region(Region::Side);
        drawSidebar();
        ui.region(Region::Title);
        drawTitlebar();
    }
    profMark(3);
    if (!hidden) {
        ui.region(Region::Bar);
        drawPlayerbar();
    }
    g.setFade(1);
    if (glideOn_) {
        drawNowCover(glideCover_.x, glideCover_.y, glideCover_.w);
        glideOn_ = false;
    }
    profMark(4);
#ifdef GF_DEV
    g.saidLayer = 1;   // (tests: the overlays from here)
#endif
    drawCoverNotice();
    drawJoinEditor();   // a playlist's join (view_join.cpp)
    drawAutomixPicker();   // a new Automix playlist's songs (view_automix.cpp)
    drawSheet();
    drawTour();   // over every page it shows (tour.cpp)
    drawSleepPrompt();
    drawToast();
    if (veilA > 0.005f) g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, veilA);   // Dim the screen, docked
    if (!hidden) {   // with the interface (the user: "if something is selected, it remains selected" while it faded)
        if (shown < 1) g.setFade(shown);
        drawFocusRing();
        g.setFade(1);
    }
    if (!hidden) drawHint();
    else hints_.clear();
    drawEdges();
#ifdef GF_WINDOWED
    drawWindowLights();
#endif
    drawPointer();
    drawPocket();
    drawFrameTimes();
    profMark(5);
    warmCanvas();
    profMark(6);
}

/** Settings → About → Frame times: the last 120 frames as bars (interval, and the work before the present inside), 16.7 and 33.3 ms marked. */
void App::drawFrameTimes() {
#ifndef GF_DEV
    return;   // dev builds only: a release has no Diagnostics card, so a setting saved by a dev build stays hidden
#endif
    if (!store.frameTimes) return;
    const float w = 120 * 3, h = 70, x = css::W - w - 16, y = 56;
    const std::vector<std::string> launch = g.wrap(startStats_, { 11 }, int(w), 3);   // the launch line wraps: it was cut off
    g.roundRect(x - 8, y - 8, w + 16, h + 58 + 15 * launch.size() + 4, 10, { 0, 0, 0 }, 0.6f);
    const float scale = h / 50;   // 50 ms at the top
    float sum = 0, worst = 0, work = 0;
    int slow = 0, worstAt = 0;
    for (int i = 0; i < 120; i++) {
        const int k = (frameAt_ + i) % 120;
        const float f = frameMs_[k], wk = workMs_[k];
        sum += f;
        work = std::max(work, wk);
        if (f > worst) { worst = f; worstAt = k; }
        slow += f > 20;
        const float bh = std::min(h, f * scale), wh = std::min(h, wk * scale);
        g.rect(x + i * 3, y + h - bh, 2, bh, f > 20 ? Rgb{ 1, 0.35f, 0.3f } : Rgb{ 0.4f, 0.85f, 0.5f }, 0.85f);
        g.rect(x + i * 3, y + h - wh, 2, wh, { 1, 1, 1 }, 0.55f);
    }
    for (float ms : { 16.7f, 33.3f }) g.rect(x, y + h - ms * scale, w, 1, { 1, 1, 1 }, 0.35f);
    // The figures change twice a second: a new text texture every frame would cost what it measures.
    if (time - frameStatsAt_ > 0.5 || frameStats_.empty()) {
        frameStatsAt_ = time;
        char buf[160];
        // the worst interval: the frame before's work, its present, and the rest (the system's loop, waits outside the frame)
        const float ww = spanWork_[worstAt], wp = spanPres_[worstAt];
        snprintf(buf, sizeof buf, "avg %.1f  max %.0f = w%.0f p%.0f o%.0f  work max %.1f  slow %d", sum / 120, worst, ww, wp,
                 std::max(0.f, worst - ww - wp), work, slow);
        frameStats_ = buf;
        // Average ms per frame since the last update: upd(ate) bg view side bar rest warm pres(ent)
        const double n = std::max(1, profFrames_);
        char pb[128];
        snprintf(pb, sizeof pb, "upd %.1f bg %.1f view %.1f side %.1f bar %.1f rest %.1f warm %.1f pres %.1f", profAcc_[0] / n,
                 profAcc_[1] / n, profAcc_[2] / n, profAcc_[3] / n, profAcc_[4] / n, profAcc_[5] / n, profAcc_[6] / n, profAcc_[7] / n);
        profStats_ = pb;
        std::fill(std::begin(profAcc_), std::end(profAcc_), 0.0);
        profFrames_ = 0;
    }
    g.text(frameStats_, { css::Xs, Weight::SemiBold }, x, y + h + 8, { 1, 1, 1 }, 0.9f);
    g.text(profStats_, { 11 }, x, y + h + 26, { 1, 1, 1 }, 0.8f);
    for (size_t i = 0; i < launch.size(); i++) g.text(launch[i], { 11 }, x, y + h + 42 + 15 * i, { 1, 1, 1 }, 0.8f);
}

/** The left stick's pointer: a translucent white dot in a soft dark ring, readable on any cover without glaring. */
#ifdef GF_WINDOWED
namespace {
constexpr float kLightY = css::TbH / 2, kLightR = 6.5f;
float lightX(int i) { return 21 + i * 20.f; }
}  // namespace

/** Which of the window's lights is at that place: 0 close, 1 minimise, 2 full screen, or -1. */
int App::lightAt(float px, float py) const {
    if (canvas || lightsP_ < 0.9f || py > css::TbH) return -1;
    for (int i = 0; i < 3; i++) if (std::fabs(px - lightX(i)) <= 10) return i;
    return -1;
}

/** 0 away, 1 all there, eased. */
float App::lightsShown() const { return lightsP_ * lightsP_ * (3 - 2 * lightsP_); }

/** The lights stay out of the way until the mouse reaches the window's top left corner (the system's cursor is there: in the resizing
 *  edge the app gets no mouse events, so it is asked where it is). They wait a moment for it to travel to them, and for as long
 *  as it stays near them. */
void App::updateLights() {
#ifdef GF_ANDROID
    lightsP_ = 0;   // no window to close, minimise or make full screen: the app is the screen
    return;
#endif
    SDL_ShowCursor(canvas && canvasIdle > 3 ? SDL_DISABLE : SDL_ENABLE);   // the arrow leaves the Canvas with its controls
    int gx = 0, gy = 0, wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(window_, &wx, &wy);
    SDL_GetWindowSize(window_, &ww, &wh);
    const int px = gx - wx, py = gy - wy;
    const bool inside = py >= 0 && py < wh && px < ww;
    constexpr int kWall = 3;
    const bool atWall = inside && px >= 0 && px < kWall && py / scale_ < css::TbH;   // the top left corner: the bar's height of the wall
    // An invisible area round them: the mouse near them a moment brings them out (the user, 2026-11-04: "difficult to invoke ...
    // when the mouse gets close it appears"; option 3 of three shown). The wait lets a quick click reach the sidebar button.
    const float ax = px / scale_, ay = py / scale_;
    const bool near = inside && px >= 0 && ay < css::TbH && ax < kLightsW + 8;
    lightsNear_ = near ? lightsNear_ + dt : 0;
    if (lightsNear_ > 0.35f) lightsHold_ = std::max(lightsHold_, 0.6f);
    if (atWall && cameFromInside_) lightsHold_ = 2.5f;
    else if (lightsHold_ > 0 && inside && px >= 0 && px / scale_ < kLightsW + 44 && py / scale_ < css::TbH) lightsHold_ = std::max(lightsHold_, 0.6f);
    lightsHold_ = std::max(0.f, lightsHold_ - dt);
    // the wall counts when reached from inside: the mouse only crossing it on its way in is not a request
    if (!atWall) cameFromInside_ = inside && px >= kWall;
    const float want = lightsHold_ > 0 && !canvas ? 1.f : 0.f;
    if (!store.motion) lightsP_ = want;
    else lightsP_ = want > lightsP_ ? std::min(want, lightsP_ + dt / 0.22f) : std::max(want, lightsP_ - dt / 0.22f);
}

void App::pressLight(int i) {
    if (i == 0) closeWindow();
    else if (i == 1) SDL_MinimizeWindow(window_);
    else fullApp_ = !fullApp_;
}

/** The window's controls, drawn into the app's own bar: red closes, yellow minimises, green goes full screen. Over everything, as
 *  they work whatever is open; their marks show while the mouse is near, grey when another window is in front. */
void App::drawWindowLights() {
    const float e = lightsShown();
    if (e <= 0.002f) return;
    static const Rgb lit[3] = { { 1.f, 0.373f, 0.341f }, { 0.996f, 0.737f, 0.180f }, { 0.157f, 0.784f, 0.251f } };
    static const Rgb ink[3] = { { 0.302f, 0, 0 }, { 0.6f, 0.345f, 0 }, { 0, 0.396f, 0.051f } };
    const bool front = SDL_GetKeyboardFocus() == window_;
    const bool near = pointerIsMouse_ && pointer.idle < 3 && pointer.y <= css::TbH && pointer.x <= kLightsW - 4;
    for (int i = 0; i < 3; i++) {
        const float cx = lightX(i) - (1 - e) * (22 - i * 4.f), cy = kLightY;   // in from the wall, the far one furthest
        const bool down = lightDown_ == i;
        if (front || near) {
            g.circle(cx, cy, kLightR, lit[i], e);
            if (down) g.circle(cx, cy, kLightR, { 0, 0, 0 }, 0.22f);
        } else g.circle(cx, cy, kLightR, pal.text, 0.2f * e);
        if (!near || e < 0.9f) continue;
        const float a = 3.2f;
        if (i == 0) {
            g.polyline({ { cx - a, cy - a }, { cx + a, cy + a } }, 1.6f, ink[i]);
            g.polyline({ { cx - a, cy + a }, { cx + a, cy - a } }, 1.6f, ink[i]);
        } else if (i == 1) {
            g.polyline({ { cx - a - 0.6f, cy }, { cx + a + 0.6f, cy } }, 1.8f, ink[i]);
        } else {   // two corners, pointing apart
            g.polyline({ { cx - a, cy + 0.6f }, { cx - a, cy + a }, { cx - 0.6f, cy + a } }, 1.6f, ink[i]);
            g.polyline({ { cx + a, cy - 0.6f }, { cx + a, cy - a }, { cx + 0.6f, cy - a } }, 1.6f, ink[i]);
        }
    }
}
#endif

void App::drawPointer() {
#ifdef GF_WINDOWED
    if (pointerIsMouse_) return;
#endif
    const float a = pointer.down ? 1.f : std::clamp(1 - (pointer.idle - Pointer::Hide + 0.4f) / 0.4f, 0.f, 1.f);
    if (a <= 0) return;
    const float r = pointer.down ? 7 : 9;
    g.glow(pointer.x, pointer.y + 1, r + 9, { 0, 0, 0 }, 0.30f * a);
    g.circle(pointer.x, pointer.y, r + 1.5f, { 0, 0, 0 }, 0.30f * a);
    g.circle(pointer.x, pointer.y, r, { 1, 1, 1 }, (pointer.down ? 0.75f : 0.55f) * a);
}

// ------------------------------------------------------------------ #bg

void App::drawBackground() {
    g.fill(pal.bg);

    // .bg-art: the cover, blurred, inset −6%, opacity .62, crossfading with the previous one
    const float ix = css::W * 0.06f, iy = css::H * 0.06f;
    const SDL_Rect full{ int(-ix), int(-iy), int(css::W + ix * 2), int(css::H + iy * 2) };
    auto art = [&](SDL_Texture* t, float a) {
        if (!t || a <= 0) return;
        SDL_SetTextureAlphaMod(t, Uint8(std::clamp(a, 0.f, 1.f) * 255));
        SDL_RenderCopy(renderer_, t, nullptr, &full);
    };
    const float k = ease(now.fade);
    art(now.prevBlur, 0.62f * (1 - k));
    art(now.blur, 0.62f * k);

    // .aurora: ambient.js — four 78vmax radial gradients drifting there and back
    struct Blob { float x, y, dx, dy, s, T, a; };
    const float m = std::max(css::W, css::H) / 100.f;
    const Blob blobs[4] = {
        { -24 * m, -30 * m, 14, 10, 1.15f, 46, 0.62f },
        { css::W + 30 * m - 78 * m, -18 * m, -12, 14, 0.9f, 58, 0.62f },
        { 8 * m, css::H + 44 * m - 78 * m, 16, -12, 1.1f, 64, 0.62f },
        { css::W + 18 * m - 78 * m, css::H + 36 * m - 78 * m, -14, -8, 1.2f, 52, 0.5f },
    };
    const float t = store.motion ? float(time) : 0;
    for (int i = 0; i < 4; i++) {
        const Blob& b = blobs[i];
        const float ph = std::fmod(t / b.T, 2.f), e = ease(ph < 1 ? ph : 2 - ph);
        const float r = 39 * m * (1 + (b.s - 1) * e);
        g.glow(b.x + 39 * m + b.dx * m * e, b.y + 39 * m + b.dy * m * e, r, pal.blobs[i], b.a);
    }

    // .veil: radial-gradient(120% 90% at 30% 20%, transparent, bg 55% at 70%) + linear bg 20% → 70%
    g.veil(css::W * 0.3f, css::H * 0.2f, css::W * 1.2f, css::H * 0.9f, pal.bg, 0.55f);
    g.rectV(0, 0, css::W, css::H, pal.bg, 0.2f, pal.bg, 0.7f);
}

// ------------------------------------------------------------------ #titlebar

void App::drawTitlebar() {
    const float cy = css::TbH / 2;
#ifdef GF_WINDOWED
    float x = 10 + (kLightsW - 10) * lightsShown() + edge_.tbL;   // it steps aside for the window's three lights (drawWindowLights), and a camera
#else
    float x = 10;
#endif
    hint(uid("tb.sidebar"), sidebarFolded() ? "Show the sidebar" : "Hide the sidebar");
    if (iconButton(uid("tb.sidebar"), ICON_SIDEBAR, x + 17, cy)) {
#ifdef GF_WINDOWED
        if (css::W < kNarrowW) sideOpenedNarrow_ = !sideOpenedNarrow_;   // for now: the setting is for windows with room
        else
#endif
        { store.sidebarCollapsed = !store.sidebarCollapsed; store.save(); }
    }
#ifdef GF_WINDOWED
    titleKeep_[0] = { x, 0, 34, css::TbH };
#endif
    x += 34 + 6;

    // .brand: the name alone, its own wordmark (the user, 2026-10-30: "better font, bold, magical", then the mark beside it
    // taken out: "just the name"): Fraunces Black in the theme's accents turning light, a glow that breathes, now and then
    // a shimmer across it (none of it moving with Settings › Appearance › Motion off). It comes and goes with the sidebar:
    // in from the left as it opens, the shimmer as it lands
    const float open = sideOpen();
    if (open > 0.02f) {
        const float in = std::clamp((open - 0.35f) / 0.65f, 0.f, 1.f), ina = in * in * (3 - 2 * in);
        auto mix = [](const Rgb& p, const Rgb& q, float t) { return Rgb{ p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t }; };
        const float wx = x + 2 - 10 * (1 - ina);
        constexpr double period = 9;   // a shimmer every 9 s, 1.3 s across
        const double since = time - sideOpenedAt_ - 0.22;   // (from the sidebar opening: the first as it lands)
        const float t = store.motion && since >= 0 ? float(std::fmod(since, period)) : -1;
        const float shine = t >= 0 && t < 1.3f ? -0.25f + 1.5f * t / 1.3f : -1;
        const float breathe = store.motion ? 0.5f + 0.5f * std::sin(float(time) * 2 * 3.14159265f / 4.5f) : 0.5f;
        // unfurled from the left as the sidebar opens (clipped to the open part)
        g.clip(wx - 12, 0, (g.wordmarkW() + 24) * std::min(1.f, in * 1.25f) + 12, css::TbH);
        g.wordmark(wx, cy, mix(pal.accent, pal.text, 0.12f), mix(pal.accent2, pal.text, 0.2f), mix(pal.accent2, pal.text, 0.78f), pal.glow,
                   0.32f + 0.18f * breathe + (shine > 0 && shine < 1 ? 0.15f * std::sin(shine * 3.14159265f) : 0.f), shine, ina);
        g.unclip();
    }

    // Local | Server: the whole app's library (the user, 2026-11-06: up here, "always there", gone when no server is signed in);
    // centred between the name and AirPlay, or the sidebar button and AirPlay when the name is away (the user, 2026-11-06);
    // the search box's height. Drawn after AirPlay (its place known); here, where it starts from
    const float srcW = librarySourceW();
    float srcFrom = 0;
    {
        const float in = std::clamp((open - 0.35f) / 0.65f, 0.f, 1.f), ina = in * in * (3 - 2 * in);
        srcFrom = (x - 14) + (g.wordmarkW() + 16) * ina;   // the sidebar glyph's right edge, to the name's (as seen: the glyphs, not their buttons)
    }
    const float srcNeed = srcW > 0 ? srcFrom + srcW + 32 + 24 : 0;   // (a narrow window: AirPlay no nearer; 24: the sync mark's room)
#ifdef GF_WINDOWED
    titleKeep_[4] = {};
#endif

    // Syncing (the user, 2026-11-02: "like the iPhones have when syncing with iTunes", top left): two arrows chasing each other
    // round a circle while a sync or a copy runs, just right of the name, or of the sidebar button when the name is away
    {
        bool busy = syncBusy;
#ifdef GF_DEV
        static const bool demo = getenv("GF_SYNCICON") != nullptr;   // the preview has no second device
        if (demo) busy = std::fmod(time, 8.0) < 5.0;
#endif
        syncIconA = std::clamp(syncIconA + (busy ? 1 : -1) * dt / 0.3f, 0.f, 1.f);
        if (syncIconA > 0) {
            const float in = std::clamp((open - 0.35f) / 0.65f, 0.f, 1.f), ina = in * in * (3 - 2 * in);
            // centred on the name's letters, not the bar: they are lower case but the S, from 20.5 to their baseline at 31
            // (measured in the preview), so their middle is 2.8 px under the bar's (the user: "isn't centered with the text");
            // on the bar's middle beside the sidebar button when the name is away
            const float r = 5.8f, a = syncIconA * syncIconA * (3 - 2 * syncIconA);
            const float ix = x + r + 1 + (g.wordmarkW() + 12) * ina, iy = cy + 2.8f * ina;
            constexpr float kPi = 3.14159265f;
            const float spin = store.motion ? float(std::fmod(time, 1.4) / 1.4) * 2 * kPi : 0.f;   // clockwise, a turn in 1.4 s
            std::vector<SDL_FPoint> arc;
            for (int k = 0; k < 2; k++) {
                // The classic sync mark (the user's picture, 2026-11-02): two arcs half a turn apart, each ending in a large solid
                // head whose base sits across the ring, centred on its line, and whose point runs on along the tangent. At the
                // title bar icons' weight: 18 px, Lucide's stroke of 2 in 24.
                const float a0 = spin + k * kPi, a1 = a0 + 2.2f;
                arc.clear();
                for (int i = 0; i <= 14; i++) {
                    const float t = a0 + (a1 - a0) * i / 14;
                    arc.push_back({ ix + r * std::cos(t), iy + r * std::sin(t) });
                }
                g.polyline(arc, 1.6f, pal.text2, a);
                const float px = ix + r * std::cos(a1), py = iy + r * std::sin(a1);
                const float tx = -std::sin(a1), ty = std::cos(a1), nx = std::cos(a1), ny = std::sin(a1), w = 2.7f, l = 3.6f;
                g.triangle({ px + nx * w, py + ny * w }, { px + tx * l, py + ty * l }, { px - nx * w, py - ny * w }, pal.text2, a);
            }
        }
    }

    // .search: centred in what is left of the bar; opens the console keyboard
#ifdef GF_WINDOWED
    // where it is with the lights away: it does not move with them; clear of a camera at either end or in the middle (Screen edges)
    float left = std::max({ sideW + edge_.side + 12, 10 + edge_.tbL + 40 + 120.f, edge_.searchL, srcNeed }), right = css::W - 150 - edge_.tbR;
#if defined(GF_AIRPLAY) || defined(GF_PREVIEW)
    left += 8 + 34;   // (AirPlay's button, before it)
#endif
    if (edge_.searchR > 0) right = std::min(right, edge_.searchR);
#else
#if defined(GF_AIRPLAY) || defined(GF_PREVIEW)
    const float left = std::max({ sideW + 12, x + 120, srcNeed }) + 8 + 34, right = css::W - 150;   // (AirPlay's button, before it)
#else
    const float left = std::max({ sideW + 12, x + 120, srcNeed }), right = css::W - 150;
#endif
#endif
    const float sw = std::min(520.f, right - left), sy = cy - 16;
    // with the sidebar folded: in the middle of the window (the user, 2026-11-07), clear of what is either side; it glides as the
    // sidebar opens and folds
    const float sxOpen = left + (right - left - sw) / 2, sxMid = std::clamp((css::W - sw) / 2, left, right - sw);
    const float sx = sxMid + (sxOpen - sxMid) * std::clamp(open, 0.f, 1.f);
    const Id sid = uid("tb.search");
    if (ui.item(sid, { sx, sy, sw, 32 }, 9)) openSearch();
#ifdef GF_WINDOWED
    titleKeep_[1] = { sx, 0, sw, css::TbH };
#endif
    const bool hot = ui.focused(sid) && ui.showFocusRing();
    g.roundRect(sx, sy, sw, 32, 9, hot ? pal.surface : pal.text, hot ? 1.f : 0.065f);
    g.roundStroke(sx, sy, sw, 32, 9, 1, hot ? pal.accent : pal.text, hot ? 0.55f : 0.09f);
    g.icon(ICON_SEARCH, sx + 12 + 8, cy, 16, pal.text3);
    const std::string q = route.view == View::Search ? route.query : std::string();
    g.textC(q.empty() ? searchHint() : q, { css::Sm }, sx + 12 + 16 + 8, cy, q.empty() ? pal.text3 : pal.text, 1, Align::Left,
            int(sw - 90));
    // <kbd>: the button that opens search
    const Font kf{ css::Xs };
#ifdef GF_WINDOWED
    const std::string searchKey = plat::capName("Y");
#else
    const char* const searchKey = "Y";
#endif
    const float kw = g.measure(searchKey, kf) + 12;
    g.roundStroke(sx + sw - 12 - kw, cy - 9, kw, 18, 5, 1, pal.text, 0.15f);
    g.textC(searchKey, kf, sx + sw - 12 - kw / 2, cy, pal.text3, 1, Align::Center);

#if defined(GF_AIRPLAY) || defined(GF_PREVIEW)
    {   // AirPlay (the user, 2026-11-05: "separate Airplay from Playing together", "in the left of search", "at the same distance from search
        // as playing together"): the speakers this device plays to, and an iPhone playing here; lit while either is on
        const float ax = sx - 8 - 17;
#ifdef GF_WINDOWED
        titleKeep_[3] = { ax - 17, 0, 34, css::TbH };
#endif
#ifdef GF_AIRPLAY
        const bool lit = airsend.silenceHere() || airplay.view().streaming;
#else
        const bool lit = false;   // (the preview: no AirPlay, the button only)
#endif
        if (iconButton(uid("tb.airplay"), ICON_AIRPLAY, ax, cy, lit)) openAirPlay();
    }
#endif
    if (srcW > 0) {   // Local | Server, centred in its room (to AirPlay's left edge; the search's, without it)
#if defined(GF_AIRPLAY) || defined(GF_PREVIEW)
        const float srcTo = sx - 8 - 17 - 9;   // (AirPlay's glyph)
#else
        const float srcTo = sx;
#endif
        const float lx = std::max(srcFrom + 16, (srcFrom + srcTo - srcW) / 2);
        librarySource(lx + srcW, cy, 32);
#ifdef GF_WINDOWED
        titleKeep_[4] = { lx, 0, srcW, css::TbH };
#endif
    }
    // Other devices: there once one is paired, lit during a session. Up here, on every screen: the player bar had no room (the user, 2026-11-02)
#ifdef GF_WINDOWED
    titleKeep_[2] = {};
#endif
    if (devicesPaired) {
        const float dx = sx + sw + 8 + 17;
#ifdef GF_WINDOWED
        titleKeep_[2] = { dx - 17, 0, 34, css::TbH };
#endif
        if (iconButton(uid("tb.devices"), ICON_LINK, dx, cy, tog.stage != Tog::Off)) openDevices();
    }

    // Where Windows has its caption buttons: the clock and the battery.
    const std::time_t tt = std::time(nullptr);
    const std::tm* lt = std::localtime(&tt);
    char clock[8];
    std::strftime(clock, sizeof clock, "%H:%M", lt);
    unsigned pct = 0;
    const bool haveBattery = plat::batteryLevel(pct);
    float rx = css::W - 18 - edge_.tbR;
    if (haveBattery) rx = drawBatteryCluster(rx, cy, pct, sx + sw + (devicesPaired ? 8 + 34 : 0) + 28);
    g.textNum(clock, { css::Base, Weight::SemiBold }, rx, cy - g.lineHeight({ css::Base, Weight::SemiBold }) / 2.f, pal.text2, 1, Align::Right);   // (numbers that change: fixed-width digits)
}

// ------------------------------------------------------------------ the battery

/** The battery at the right of a top bar (the title bar's; the phone's upright: the user, 2026-11-05: "apply the new battery
 *  customisation to android portrait mode too"): rx its right edge, cy its middle, limit where what is beside it must stop (the
 *  search bar, the wordmark), the clock's room kept. Returns the left of it all, where the clock goes. */
float App::drawBatteryCluster(float rx, float cy, unsigned pct, float limit) {
    constexpr float kClockGap = 10;   // more room left of it for the clock: its focus ring reaches 4 px outside it
    // BETA 3 (the user, 2026-11-05: "Let the user customise almost everything about the battery"): its colour, an animation for each
    // state (low; charging slowly, normally, fast; charged), and what shows beside it; all in Settings › Appearance › Battery
    // a bit bigger than BETA 2's, with the text and the clock (the user, 2026-11-05: "just a bit bigger")
    constexpr float bs = 1.2f;
    const float bw = 24 * bs, bx = rx - bw - 3;
    const bool charging = battery.charging == 1;
    {   // a new state cross-fades from the one before (a charger plugged in, the charge full); the fill glides to a new charge
        static int shownSt = -1, prevSt = -1;
        static double changedAt = 0, shownPct = -1;
        const int st = batteryState(pct);
        if (st != shownSt) { prevSt = shownSt; shownSt = st; changedAt = time; }
        const double want = double(pct);   // as HOS shows it, as the percentage beside it
        shownPct = shownPct < 0 || !store.motion ? want : want + (shownPct - want) * std::exp(-dt / 0.35);
        const float k = store.motion ? float(std::clamp((time - changedAt) / 0.45, 0.0, 1.0)) : 1.f;
        const float e = k * k * (3 - 2 * k);
        if (e < 1 && prevSt >= 0) drawBatteryIcon(bx, cy + 0.5f, bs, shownPct, prevSt, 1 - e);
        drawBatteryIcon(bx, cy + 0.5f, bs, shownPct, st, prevSt >= 0 ? e : 1.f);
    }
    // its area ends 8 px past the tip, as it starts 8 px before the text (the user, 2026-11-05: "a bit more to the left than the
    // right"): the body as drawBatteryIcon makes it (odd, whole), then the tip's 3 px at this size
    const float right = bx + (2 * std::round(12 * bs) + 1) + 3 * bs + 4;   // (body, the tip's gap and width)
    rx = bx - 10;
    // Beside it, what Settings › Appearance › Battery picks: the charge, the time to full or empty (App::sampleBattery), the
    // charger's watts while charging, the temperature, the voltage, the health
    // (in your order: Settings › Appearance › Battery)
    std::string label;
    auto add = [&](const std::string& s) { if (!s.empty()) label += (label.empty() ? "" : "  ·  ") + s; };
    const plat::BatteryInfo& bi = battery.info;
    for (int it : batteryItems()) {
        if (!*batteryShown(it)) continue;
        char t[40] = {};
        switch (it) {
            case 0: snprintf(t, sizeof t, "%u%%", pct); break;
            case 1: add(battery.est); break;
            case 2: if (charging && bi.chargerVolts * bi.chargerAmps > 0) snprintf(t, sizeof t, "%.0f W", bi.chargerVolts * bi.chargerAmps); break;
            case 3: if (bi.tempC > 0) snprintf(t, sizeof t, "%.0f °%s", store.batteryFahrenheit ? bi.tempC * 9 / 5 + 32 : bi.tempC, store.batteryFahrenheit ? "F" : "C"); break;
            case 4: if (bi.volts > 0) snprintf(t, sizeof t, "%.2f V", bi.volts); break;
            case 5: if (bi.healthPct > 0) snprintf(t, sizeof t, "health %.0f%%", bi.healthPct); break;
            case 6: if (bi.gauge) snprintf(t, sizeof t, "%s%.1f W", bi.gaugeAmps < 0 ? "\u2212" : "+", std::fabs(bi.gaugeVolts * bi.gaugeAmps)); break;
        }
        add(t);
    }
    // what does not fit between the search bar (and the devices button) and the clock is left out, from the end: the search bar is
    // never covered (the user's preview, 2026-11-05: everything on ran the clock into it)
    {
        const float clockW = float(g.measure("00:00", { css::Base, Weight::SemiBold })) + kClockGap;
        batteryRoom_ = rx - 14 - clockW - limit;
        while (!label.empty() && rx - 14 - float(g.measure(label, { css::Base })) - clockW < limit) {
            const size_t cut = label.rfind("  ·  ");
            label = cut == std::string::npos ? std::string() : label.substr(0, cut);
        }
    }
    if (!label.empty()) rx -= g.textNum(label, { css::Base }, rx, cy - g.lineHeight({ css::Base }) / 2.f, pal.text3, 1, Align::Right) + 14;
    else rx -= 4;
    // pressed (or a touch): everything about it, in its card
    const Id bid = uid("tb.battery");
    // (its focus ring is the app's, as every other's: App::drawFocusRing; a second of its own inside it made two, the user's
    // screenshot, 2026-11-05: "not consistent with normal outlines")
    if (ui.item(bid, { rx + 6, cy - 16, right - rx - 2, 32 }, 8)) openBattery();
    return rx - kClockGap;   // the clock clear of its focus ring (the user, 2026-11-05: "almost clipping the clock")
}


int App::batteryState(double pct) const {
    if (battery.charging == 1) return battery.full() ? 5 : 1 + std::clamp(battery.speed(), 1, 3);
    return pct <= store.batteryLowAt ? 1 : 0;
}

namespace {
Rgb lerpRgb(const Rgb& a, const Rgb& b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
const Rgb kBatGreen{ 0.36f, 0.80f, 0.46f }, kBatYellow{ 0.96f, 0.83f, 0.30f }, kBatRed{ 0.92f, 0.36f, 0.31f };
}  // namespace

Rgb App::batteryInk(int c, double pct) const {
    switch (c) {
        case 1: case 2: return pal.accentInk;   // the accent; the palette's gradient (its outline)
        case 3: {                               // by charge: red at empty, yellow at half, green at full, gradually
            const float t = float(std::clamp(pct, 0.0, 100.0) / 100.0);
            return t < 0.5f ? lerpRgb(kBatRed, kBatYellow, t * 2) : lerpRgb(kBatYellow, kBatGreen, (t - 0.5f) * 2);
        }
        case 4: return kBatGreen;
        case 5: return kBatYellow;
        case 6: return { 0.97f, 0.58f, 0.24f };   // orange
        case 7: return kBatRed;
        case 8: return { 0.38f, 0.62f, 0.98f };   // blue
        case 9: return { 0.67f, 0.48f, 0.96f };   // purple
        case 10: return { 0.96f, 0.47f, 0.70f };  // pink
        default: return pal.text2;                // as the text beside it
    }
}

std::vector<int> App::batteryItems() const {
    std::vector<int> v;
    for (int i : store.batteryOrder)
        if (i < 2 || (battery.infoOk && (i != 6 || battery.info.gauge))) v.push_back(i);
    return v;
}

void App::setBatteryItems(const std::vector<int>& vis) {
    size_t k = 0;
    for (int& i : store.batteryOrder)
        if (k < vis.size() && std::find(vis.begin(), vis.end(), i) != vis.end()) i = vis[k++];
}

bool* App::batteryShown(int item) {
    bool* f[7] = { &store.batteryPercent, &store.batteryEstimate, &store.batteryWatts, &store.batteryTemp, &store.batteryVolts, &store.batteryHealth,
                   &store.batteryPower };
    return f[std::clamp(item, 0, 6)];
}

/** The battery in a state, its colour and animation as Settings › Appearance › Battery states has them. Each animation's speed
 *  follows the state (charging: as fast as the charger gives), then Pace. Their light is the interface's own: Automix's soft band
 *  and halo, the opening's four-pointed stars, the palette's gradient. Every one eases in and out (the user, 2026-11-05: "even
 *  cleaner and smooth"): nothing starts, stops or jumps at once. alpha: the whole of it (a state cross-fading). */
void App::drawBatteryIcon(float bx, float cy, float s, double pct, int st, float alpha) {
    st = std::clamp(st, 0, Store::BatteryStates - 1);
    static const double kPeriod[Store::BatteryStates] = { 3.2, 1.6, 3.4, 2.2, 1.3, 3.4 };
    const double pace = store.batteryAnimPace == 0 ? 0.6 : store.batteryAnimPace == 2 ? 1.6 : 1.0;
    if (batPhaseAt_ != time) {   // once a frame, every state's phase moves on (the previews in Settings run with the title bar's)
        const double step = batPhaseAt_ < 0 ? 0 : std::clamp(time - batPhaseAt_, 0.0, 0.1);
        for (int i = 0; i < Store::BatteryStates; i++) batPhase_[i] = std::fmod(batPhase_[i] + step * pace / kPeriod[i], 1.0);
        batPhaseAt_ = time;
    }
    constexpr double kTau = 6.283185307179586;
    auto smooth = [](float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3 - 2 * x); };
    auto inOut = [](float x) { x = std::clamp(x, 0.f, 1.f); return x < 0.5f ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3.f) / 2; };
    const float ph = float(batPhase_[st]);
    const float wave = float(0.5 - 0.5 * std::cos(ph * kTau));   // 0 → 1 → 0, eased at both ends

    // whole pixels, an even height, centred on the text's middle (the user, 2026-11-05: "not centered relative to text": at 1.2 its
    // edges fell between pixels and the bottom one faded)
    // (odd width and height: its middle on a pixel's middle, where the bolt's odd size centres sharp; the user, 2026-11-05: the bolt
    // blurred and doubled at 1.2)
    const float bw = 2 * std::round(12 * s) + 1, bh = 2 * std::round(6 * s) + 1, by = std::round(cy - bh / 2);
    bx = std::round(bx);
    cy = by + bh / 2;
    const int col = store.batteryCol[st];
    const bool grad = col == 2;
    const Rgb ink = batteryInk(col, pct), lightC = grad ? pal.accent : ink;
    const int look = store.motion ? store.batteryLook[st] : 0;
    const Rgb white{ 1, 1, 1 };
    // The icon as it was (the user, 2026-11-05: "I want the old icon as it is"): its outline, the gap inside it, the tip standing
    // off the body
    const float tipX = bx + bw + 1 * s, tipW = 2 * s, tipEnd = tipX + tipW;
    // glow: Automix's halo round it, breathing; shimmer: a fainter one. Round the whole of it, the tip too (the user, 2026-11-05:
    // "the battery fade is not centered": round the body only, it reached further on the left)
    const float gw = tipEnd - bx;
    const float glowA = look == 3 ? (0.12f + 0.30f * wave) * alpha : look == 7 ? (0.08f + 0.12f * wave) * alpha : 0.f;
    if (look == 3) g.glowAround(bx, by, gw, bh, 3 * s, 8 * s, 8 * s, lightC, glowA);
    else if (look == 7) g.glowAround(bx, by, gw, bh, 3 * s, 6 * s, 6 * s, lightC, glowA);
    // The halo is outside its box only: the box's own corners beside the tip (above and below it, and the body's rounded corners
    // there) showed the dark behind it (the user's circles, 2026-11-05: "fill these black spots with the fade as well"). They take
    // the glow's light, as strong as at the edge; the body's inside is left as it was
    if (glowA > 0) {
        // the halo's box filled once, cut to the strip from the outline's outer edge to its end (the clip already set, Settings'
        // scrolling, kept: the two meet, then it comes back)
        SDL_Rect was{};
        const bool had = SDL_RenderIsClipEnabled(renderer_);
        if (had) SDL_RenderGetClipRect(renderer_, &was);
        const int x0 = int(std::ceil(bx + bw - 1.5f * s)), x1 = int(std::ceil(bx + gw)), y0 = int(std::floor(by)), y1 = int(std::ceil(by + bh));
        SDL_Rect cut{ x0, y0, x1 - x0, y1 - y0 };
        if (had) SDL_IntersectRect(&cut, &was, &cut);
        if (cut.w > 0 && cut.h > 0) {
            SDL_RenderSetClipRect(renderer_, &cut);
            g.glowInside(bx, by, gw, bh, 3 * s, lightC, glowA);   // (the halo's own outline: no seam where they meet)
            SDL_RenderSetClipRect(renderer_, had ? &was : nullptr);
        }
    }
    // its edge and tip: as the fill, or the colour Settings gives the outline (the palette's gradient: the accent; by charge: as it is)
    const Rgb edge = store.batteryOutline < 0 ? ink : batteryInk(store.batteryOutline, pct);
    g.roundStroke(bx, by, bw, bh, 3 * s, 1.5f * s, edge, alpha);
    g.roundRect(tipX, cy - 3 * s, tipW, 6 * s, 1 * s, edge, alpha);
    const float fx = bx + 2.5f * s, fy = by + 2.5f * s, fh = bh - 5 * s, fr = 1.5f * s;
    const float full = (bw - 5 * s) * float(std::clamp(pct, 0.0, 100.0)) / 100.f;
    auto fillAt = [&](float w, float a) {
        if (w <= 0.25f || a <= 0.005f) return;
        if (grad) g.roundRectH(fx, fy, w, fh, fr, pal.accent2, pal.accent, a * alpha);   // the palette's gradient, as the seek bar's
        else if (look == 7) {   // shimmer: the colour drifts through the fill, lighter and back
            const Rgb lite = lerpRgb(ink, white, 0.45f);
            g.roundRectH(fx, fy, w, fh, fr, lerpRgb(ink, lite, wave), lerpRgb(lite, ink, wave), a * alpha);
        } else g.roundRect(fx, fy, w, fh, fr, ink, a * alpha);
    };
    if (look == 2) fillAt(full, 0.45f + 0.55f * (1 - wave));                       // pulse: the fill breathes out and in
    else if (look == 5) fillAt(full, 0.2f + 0.8f * smooth((1 - wave - 0.2f) / 0.6f));   // blink: a soft one, never a flash
    else if (look == 4) {   // fill up: a faint fill always; the full one rises over it, eased, rests, and fades away to rise again
        fillAt(full, 0.28f);
        const float rise = inOut(ph / 0.55f), fade = 1 - smooth((ph - 0.72f) / 0.24f);
        fillAt(full * rise, 0.72f * fade + 0.0f);
    } else fillAt(full, 1);
    if ((look == 1 || look == 7) && full > 2) {   // sweep and shimmer: Automix's soft band of light through the fill, eased, then a rest
        const float band = (look == 7 ? 14.f : 10.f) * s;
        const float run = look == 7 ? ph : ph / 0.7f;   // (sweep rests for the last part of each turn)
        if (run < 1) {
            // charging, the light goes the way the charge does: left to right; on battery, right to left (the user, 2026-11-05)
            const float p = st >= 2 ? inOut(run) : 1 - inOut(run), a = float(std::sin(run * kTau / 2));
            g.bandInPill(fx, fy, full, fh, fx + p * (full + band) - band, band, white, (look == 7 ? 0.5f : 0.7f) * a * alpha);
        }
    }
    if (look == 6) {   // sparkle: the opening's four-pointed stars twinkle round it, one after another, growing and fading softly
        struct Star { float x, y, size, off; };
        static const Star stars[4] = { { -0.12f, -0.62f, 11, 0.0f }, { 0.62f, -0.80f, 8, 0.27f }, { 1.16f, 0.55f, 10, 0.52f }, { 0.28f, 0.82f, 7, 0.76f } };
        const Rgb sc = lerpRgb(lightC, white, 0.35f);
        for (const Star& p : stars) {
            const float tw = float(std::fmod(ph + p.off, 1.0));
            const float a = smooth(float(std::sin(tw * kTau / 2)));   // 0 → 1 → 0 over its turn, eased
            if (a > 0.01f) g.sparkle(bx + p.x * bw, cy + p.y * bh - 1.5f * s * tw, p.size * s * (0.4f + 0.6f * a), sc, a * alpha);
        }
    }
    // plugged in: the bolt over the fill, with a dark edge so it reads on the fill and off it (charging fast, animated: it beats too)
    if (st >= 2) {
        const float ba = st == 4 && look ? 0.72f + 0.28f * (1 - wave) : 1.f;
        // in the middle of the body (the tip left out), an odd whole size on the body's middle pixel; its dark edge the same bolt two
        // pixels larger behind it, centred on it (it was half a pixel off: a doubled, blurred bolt)
        const float bs = 2 * std::round(5.5f * s) + 1, mx = bx + bw / 2, my = by + bh / 2;   // (inside the outline, a pixel clear)
        g.icon(ICON_BOLT, mx, my, bs + 2, { 0, 0, 0 }, 0.45f * ba * alpha);
        g.icon(ICON_BOLT, mx, my, bs, pal.text, ba * alpha);
    }
}

// ------------------------------------------------------------------ #sidebar

void App::drawSidebar() {
    const float x0 = 10 + edge_.side, w = sideW - 20;   // the column steps aside for a camera (Screen edges): the page starts later too
    const bool collapsed = sideW < 120;
    // how far open (App::sideOpen): the labels come in over its second half, settling 8 px towards the icons
    const float open = sideOpen(), lab = std::clamp((open - 0.4f) / 0.6f, 0.f, 1.f), labA = lab * lab * (3 - 2 * lab);
    float y = css::TbH + 6;
    const Font navFont{ css::Base, Weight::SemiBold };

    auto navItem = [&](const char* key, const char* icon, const std::string& label, bool active, const std::string& badge, float h, Font f,
                       int ref = 0, bool playingHere = false) {
        const Id id = uid(key, ref);
        const bool hit = ui.item(id, { x0, y, w, h }, 9);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (active) {
            g.roundRect(x0, y, w, h, 9, pal.accent, 0.09f);
            g.roundRect(x0, y + 10, 3, h - 20, 1.5f, pal.accent);
        } else if (hot) {
            g.roundRect(x0, y, w, h, 9, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
        }
        const Rgb fg = active || hot ? pal.text : pal.text2;
        // the icon glides between the middle (closed) and its place before the label (open)
        const float ix = x0 + 12 + 9.5f + (w / 2 - 12 - 9.5f) * (1 - open);
        g.icon(icon, ix, y + h / 2, 19, active ? pal.accentInk : fg);
        if (labA > 0) {   // (kept clear of the icon: it settles in from the right)
            const float tx = ix + 9.5f + 12 + 8 * (1 - labA);
            float right = x0 + w - 12;
            // while it moves the label is cut by the sidebar's edge rather than shortened ("Now p…"), the count coming in last
            const bool moving = open < 0.999f;
            const float badgeA = moving ? std::pow(labA, 4.f) : 1.f;
            if (playingHere && labA > 0.6f) { eqGlyph(right - 13, y + h / 2, pal.accentInk); right -= 22; }
            else if (!badge.empty()) right -= g.textNum(badge, { css::Xs }, right, y + h / 2 - g.lineHeight({ css::Xs }) / 2.f, pal.text3, badgeA, Align::Right) + 8;
            // (inside the playlists' own clip there: the two met, and that one put back after)
            SDL_Renderer* rr = g.renderer();
            const bool hadClip = SDL_RenderIsClipEnabled(rr);
            SDL_Rect was{};
            if (moving) {
                SDL_RenderGetClipRect(rr, &was);
                const SDL_Rect row{ int(x0), int(std::floor(y)), int(w - 4), int(std::ceil(h)) };
                SDL_Rect cut = row;
                if (hadClip && !SDL_IntersectRect(&row, &was, &cut)) cut = { 0, 0, 0, 0 };
                SDL_RenderSetClipRect(rr, &cut);
            }
            g.textC(label, f, tx, y + h / 2, fg, labA, Align::Left, moving ? 0 : int(right - tx));
            if (moving) SDL_RenderSetClipRect(rr, hadClip ? &was : nullptr);
        }
        y += h + 2;
        return hit;
    };

    const View v = route.view;
    // The tabs in your order (Settings → Appearance → Sidebar, store.tabOrder); any can be hidden (store.hiddenTabs)
    // but Now playing. Settings always stays at the bottom.
    const unsigned hide = store.hiddenTabs;
    // A folder page lights the tab it belongs to: Folders, or Library when opened from its Folders tab
    // (or when the Folders tab is hidden, so something stays lit).
    const bool folderInLibrary = v == View::Folders && (folderFrom == View::Library || (hide & 16));
    // An artist's or a genre's page lights its own tab, or Library when that tab is hidden (App::tabFor)
    const bool artistInLibrary = v == View::Artist && (hide & Store::tabBit(Store::TabArtists));
    const bool genreInLibrary = v == View::Genre && (hide & Store::tabBit(Store::TabGenres));
    // Your lines: drawn only between two tabs that show (never first, last or twice in a row), in the rows' own gap
    std::vector<int> shownTabs;
    for (int tab : store.tabOrder) {
        if (Store::isLine(tab)) { if (!shownTabs.empty() && !Store::isLine(shownTabs.back())) shownTabs.push_back(tab); continue; }
        if (tab != Store::TabNow && (hide & Store::tabBit(tab))) continue;
        if (tab == Store::TabServer) continue;   // the server's music is in Library (Local | Server), not a tab of its own (the user, 2026-11-06)
        shownTabs.push_back(tab);
    }
    while (!shownTabs.empty() && Store::isLine(shownTabs.back())) shownTabs.pop_back();
    // A short window (a phone sideways at a large Size): the tabs come closer, 38 down to 30, as far as the playlists under
    // them need to keep three rows (the user, 2026-11-04: "some things start running out of space like playlists"). Only the rows
    // there are: with Liked alone a PC window kept BETA 2's 38 (session 13: every PC size had them closer)
    float tabH = 38;
    {
        int nTabs = 0, nLines = 0;
        for (int tab : shownTabs) (Store::isLine(tab) ? nLines : nTabs)++;
        const float footH = 2 + 8 + 1 + 11 + 38 + jobsHeight(collapsed);
        const int plRows = std::min(3, 1 + int(store.playlists.size()));   // Liked, then yours
        // under the tabs: the line (4 + 11), the Playlists header only while the labels show (28), the rows (34 each);
        // each tab is its height and a 2 px gap (navItem)
        const float room = css::H - css::BarH - 12 - footH - y - (15 + 28 * labA) - nLines * 11.f - plRows * 34.f;
        if (nTabs > 0) tabH = std::clamp(std::floor(room / nTabs) - 2, 30.f, 38.f);
    }
    // Albums' own icon, a stack of albums (Library keeps the four squares: the user, 2026-11-06, "F, no spine")
    auto albumsIcon = []() -> const char* { return ICON_ALBUMS; };
    const bool srvSide = serverSidebar();
    const bool sideMark = true;   // the cloud on those tabs while they show the server (the user, 2026-11-06: "B, cloud mark")
    for (int tab : shownTabs) {
        if (Store::isLine(tab)) {
            // every line in the sidebar the same: 6 px clear to the highlight above (the row's own 2 px gap + 4) and 6 below
            y += 4;
            g.rect(x0 + 8, y, w - 16, 1, pal.text, 0.09f);
            y += 7;
            continue;
        }
        switch (tab) {
        case Store::TabNow:
            if (navItem("nav.now", ICON_DISC, "Now playing", v == View::Now, "", tabH, navFont)) navigate(View::Now);
            break;
        case Store::TabQueue:
            if (navItem("nav.queue", ICON_LIST, "Playlist", v == View::Queue,
                        srvq_.on ? std::to_string(srvq_.songs.size()) : q.tracks.empty() ? "" : std::to_string(q.tracks.size()), tabH, navFont))
                navigate(View::Queue);
            break;
        case Store::TabAlbums:
        case Store::TabArtists:
        case Store::TabGenres:
        case Store::TabFolders:
            if (srvSide) {   // Library on Server, its music Apart: the server's (the Library switch says which)
                const int st = tab == Store::TabAlbums ? 0 : tab == Store::TabArtists ? 1 : tab == Store::TabGenres ? 2 : 3;
                static const char* const names[4] = { "Albums", "Artists", "Genres", "Folders" };
                const char* const icons[4] = { albumsIcon(), ICON_MIC, ICON_TAG, ICON_FOLDER };
                static const char* const keys[4] = { "nav.albums", "nav.artists", "nav.genres", "nav.folders" };
                std::string n;
                if (st == 0) { if (!serverAlbums().empty()) n = std::to_string(serverAlbums_.size()); }
                else if (st == 1) { if (const auto* l = serverArtistList()) n = std::to_string(l->size()); }
                else if (st == 2) { if (const auto* l = serverGenreList()) n = std::to_string(l->size()); }
                else if (const auto* l = serverFolderList()) { int d = 0; for (const auto& e : *l) d += e.dir; n = std::to_string(d); }
                const bool here = v == View::Server && ((route.ref == 6 && atoi(route.query.c_str()) == st) ||
                                                        (st == 0 && route.ref == 2) || (st == 1 && route.ref == 1) || (st == 2 && route.ref == 3) || (st == 3 && route.ref == 4));
                const float iy = y;
                if (navItem(keys[st], icons[st], names[st], here, n, tabH, navFont)) openServerSection(st);
                if (sideMark) {   // the server's mark on the icon
                    const float ix = x0 + 12 + 9.5f + (w / 2 - 12 - 9.5f) * (1 - open);
                    g.iconOnDisc(PLI_CLOUD, ix + 8, iy + tabH / 2 + 7, 7.5f, 10, pal.bg, 0.95f, pal.accentInk, 1);   // centred in its disc (the user, 2026-11-06)
                }
                break;
            }
            if (tab == Store::TabArtists) {
                if (navItem("nav.artists", ICON_MIC, "Artists", v == View::Artists || v == View::Artist, std::to_string(artists.size()), tabH, navFont))
                    navigate(View::Artists);
                break;
            }
            if (tab == Store::TabGenres) {
                if (navItem("nav.genres", ICON_TAG, "Genres", v == View::Genres || v == View::Genre, std::to_string(genres.size()), tabH, navFont))
                    navigate(View::Genres);
                break;
            }
            if (tab == Store::TabFolders) {
                if (navItem("nav.folders", ICON_FOLDER, "Folders", v == View::Folders && !folderInLibrary,
                            std::to_string(std::max(0, int(lib.folders.size()) - int(lib.rootFolders.size()))), tabH, navFont)) {
                    navigate(View::Folders, lib.rootFolders.empty() ? -1 : lib.rootFolders[0]);
                    folderFrom = View::Folders;   // the tab itself, even when clicked from Library
                }
                break;
            }
            if (navItem("nav.albums", albumsIcon(), "Albums", v == View::Albums || (v == View::Album && libFrom == View::Albums),
                        std::to_string(lib.albums.size()), tabH, navFont)) {
                libFrom = View::Albums;
                navigate(View::Albums);
            }
            break;
        case Store::TabLibrary: {
            const float iy = y;
            const int sn = srvSide ? subsonic.view().songs : -1;
            if (navItem("nav.library", ICON_GRID, "Library", v == View::Library || (v == View::Album && libFrom == View::Library) || folderInLibrary ||
                                                        artistInLibrary || genreInLibrary,
                        srvSide ? (sn >= 0 ? std::to_string(sn) : std::string()) : std::to_string(lib.tracks.size()), tabH, navFont)) {
                libFrom = View::Library;
                navigate(View::Library);
            }
            if (srvSide && sideMark) {   // (the server's mark)
                const float ix = x0 + 12 + 9.5f + (w / 2 - 12 - 9.5f) * (1 - open);
                g.iconOnDisc(PLI_CLOUD, ix + 8, iy + tabH / 2 + 7, 7.5f, 10, pal.bg, 0.95f, pal.accentInk, 1);
            }
            break;
        }
        case Store::TabFiles:
            if (navItem("nav.files", PLI_SDCARD, "Files", v == View::Files, "", tabH, navFont)) openFiles(filesStart());
            break;
        case Store::TabServer: {   // a Subsonic server's music (view_server.cpp)
            const Subsonic::View sv = subsonic.view();
            if (navItem("nav.server", PLI_CLOUD, "Server", v == View::Server, sv.songs >= 0 ? std::to_string(sv.songs) : "", tabH, navFont)) openServer(0, "", "");
            break;
        }
        }
    }

    // .nav-sep + .nav-label with the new-playlist button; the line as clear of the tab above as your lines are (6 px)
    y += 4;
    g.rect(x0 + 8, y, w - 16, 1, pal.text, 0.09f);
    y += 11;
    if (labA > 0) {   // (fading in with the labels; the rows under it make room as it does)
        const float fade0 = g.fade();
        g.setFade(fade0 * labA);
        // the header opens the Playlists page (all of them, and their order); lit while it shows
        const Id hid = uid("nav.playlists");
        const float hw = w - 36;
        if (ui.item(hid, { x0, y, hw, 24 }, 7)) navigate(View::Playlists);
        const bool here = v == View::Playlists, hot = ui.pressed(hid) || (ui.focused(hid) && ui.showFocusRing());
        if (here) g.roundRect(x0, y, hw, 24, 7, pal.accent, 0.09f);
        else if (hot) g.roundRect(x0, y, hw, 24, 7, pal.text, ui.pressed(hid) ? 0.11f : 0.065f);
        // Apart and signed in: which side's (the user, 2026-11-06); otherwise as it always was
        const bool apart = store.serverPlace == 0 && subsonic.view().state == Subsonic::SignedIn;
        kicker(!apart ? "Playlists" : store.libSource == 1 ? "Server playlists" : "Local playlists", x0 + 12, y + 12, here ? pal.accentInk : hot ? pal.text2 : pal.text3, 0.08f);
        hint(uid("nav.newpl"), "New playlist");
        if (iconButton(uid("nav.newpl"), ICON_PLUS, x0 + w - 12, y + 12, false, 24, 14)) {   // your own, or a smart one; the server's: a plain one
            if (serverPlaylistsShown()) serverNewPlaylist({});
            else openNewList();
        }
        g.setFade(fade0);
        y += 28 * labA;
    }

    // #playlists: Liked, then the folders added as playlists
    // The progress card sits above Settings while something runs in the background; the playlists make room.
    const float jobH = jobsHeight(collapsed);
    const float footH = 2 + 8 + 1 + 11 + 38 + jobH;
    const float listTop = y, listH = css::H - css::BarH - 12 - footH - listTop;
    ScrollArea& sa = area("side.playlists");
    const bool srvList = serverPlaylistsShown(), srvMixed = !srvList && serverPlaylistsMixed();
    const std::vector<Subsonic::Playlist>* spl = srvList || srvMixed ? serverPlaylistList() : nullptr;
    const int n = srvList ? (spl ? int(spl->size()) : 0) : 1 + int(store.playlists.size()) + (srvMixed && spl ? int(spl->size()) : 0);
    sa.view = { 0, listTop, sideW + edge_.side, listH };
    sa.content = n * 34.f;
    ui.pushArea(&sa);
    g.clip(0, listTop, sideW + edge_.side, listH);
    y = listTop - sa.y;
    const Font plFont{ css::Sm };
    const bool likedPlaying = q.name == "Liked" && isPlaying() && !srvq_.on;
    if (srvList) {   // the server's playlists (Playlists › Server): each opens its page
        if (spl)
            for (int i = 0; i < int(spl->size()); i++) {
                const Subsonic::Playlist& p = (*spl)[size_t(i)];
                const bool here = v == View::Server && route.ref == 5 && route.query.rfind(p.id + "\x1f", 0) == 0;
                if (navItem("nav.spl", serverPlaylistIcon(p.id), p.name, here, std::to_string(p.songs), 32, plFont, i, srvq_.on && srvq_.name == p.name && isPlaying()))
                    openServer(5, p.id, p.name);
            }
    } else if (navItem("nav.pl", ICON_HEART, "Liked", v == View::List && route.ref == -1, std::to_string(store.liked.size()), 32, plFont, -1, likedPlaying))
        navigate(View::List, -1);
    for (int i = 0; !srvList && i < int(store.playlists.size()); i++) {
        const FolderPlaylist& p = store.playlists[i];
        const int f = p.folder() ? lib.folderByPath(p.path) : -1;
        const int count = p.own() ? int(p.tracks.size()) : p.smart() ? int(smartTracks(p.path).size()) : f >= 0 ? lib.folders[f].last - lib.folders[f].first : 0;
        if (navItem("nav.pl", playlistIcon(i), p.name, v == View::List && route.ref == i, std::to_string(count), 32, plFont, i, q.name == p.name && isPlaying() && !srvq_.on))
            navigate(View::List, i);
    }
    if (srvMixed && spl)   // Mixed in: the server's after yours, with its cloud
        for (int i = 0; i < int(spl->size()); i++) {
            const Subsonic::Playlist& p = (*spl)[size_t(i)];
            const bool here = v == View::Server && route.ref == 5 && route.query.rfind(p.id + "\x1f", 0) == 0;
            if (navItem("nav.spl", serverPlaylistIcon(p.id), p.name, here, std::to_string(p.songs), 32, plFont, i, srvq_.on && srvq_.name == p.name && isPlaying()))
                openServer(5, p.id, p.name);
        }
    g.unclip();
    ui.popArea();

    // .side-foot
    y = css::H - css::BarH - 12 - 38 - 2;
    // One stack above Settings: the first job running in front, the others peeking out behind it (furthest drawn first).
    {
        const JobStack st = jobStack();
        const float bottom = y - 11 - 6, frontH = collapsed ? 22 : 44, peek = collapsed ? 4 : 7;
        // Only the strip above the front card is drawn: the front card is see-through, and the others must not show in it.
        // The cards behind are counted in fractions (a job leaving or arriving is part of one), so they slide, never jump.
        g.clip(x0, 0, w, bottom - frontH);
        for (int k = int(std::ceil(st.behind - 0.01f)); k >= 1; k--) {
            const float part = std::min(1.f, st.behind - (k - 1));
            const float inset = 8.f * k, top = bottom - frontH - peek * std::min(float(k), st.behind);
            g.roundRect(x0 + inset, top, w - inset * 2, frontH, 10, pal.text, (0.07f - 0.015f * k) * part);
            g.roundStroke(x0 + inset, top, w - inset * 2, frontH, 10, 1, pal.text, (0.12f - 0.03f * k) * part);
        }
        g.unclip();
        if (st.body > 0.01f) drawJobCard(st, x0, bottom, w, collapsed);
        // The last job stopped (or done) with the focus on the stack: Settings, just under where it was, takes it.
        else if (ui.focused(uid("side.job"))) ui.focus(uid("nav.settings", 0));
    }
    g.rect(x0 + 8, y - 11, w - 16, 1, pal.text, 0.09f);
    if (navItem("nav.settings", ICON_GEAR, "Settings", v == View::Settings, "", 38, navFont)) navigate(View::Settings);
}

// ------------------------------------------------------------------ #playerbar

void App::drawSeek(float x, float y, float w, float h, Id id) {
    const double dur = now.info.duration;
    const bool locked = togetherLocked();   // a session's starter alone seeks: shown, not reachable
    if (!locked) ui.item(id, { x, y - 4, w, h + 8 }, 6, Slider | NoSideEntry);   // reached with Up/Down only (it spans the controls below)
    float frac = dur > 0 ? float(std::clamp(shownPosition() / dur, 0.0, 1.0)) : 0;
    if (extOn && !srvq_.on) {   // an outside source (AirPlay): its time as it says it, a plain line, and seeking is on the device it comes from
        if (ui.sliderReleased(id) || ui.sliderStep(id)) toast("Seek on " + extFrom);
        const float cy = y + h / 2;
        g.roundRect(x, cy - 2, w, 4, 2, pal.text, 0.16f);
        g.roundRectH(x, cy - 2, w * frac, 4, 2, pal.accent2, pal.accent);
        return;
    }
    const float drag = locked ? -1 : ui.sliderDrag(id);
    float shown = drag >= 0 ? std::clamp((drag - x) / w, 0.f, 1.f) : frac;
    if (dur > 0 && !locked) {
        if (const int st = ui.sliderStep(id)) seekBy(st * 5);
        if (ui.sliderReleased(id)) {
            const float f = std::clamp((ui.lastDragX() - x) / w, 0.f, 1.f);
            player.seek(f * dur);
            shown = f;
        }
    }
    const float cy = y + h / 2;
    if (!store.seekWave) {   // Settings › Visualizer › Seek bar › Waveform off: a plain line, the part played lit (seeking as with the bars)
        g.roundRect(x, cy - 2, w, 4, 2, pal.text, 0.16f);
        g.roundRectH(x, cy - 2, w * shown, 4, 2, pal.accent2, pal.accent);
        return;
    }
    const bool waveNow = wave.ready() && !playingFile().empty() && wave.path() == playingFile();
    // an Automix join, Immersive or Simple: the outgoing song's waveform becomes the next one's, each bar on its way from the
    // one height to the other, timed to the join (the old one alone until the next one's is measured)
    const bool morph = store.mixAnimNow != 0 && mixOld.on && !mixOld.wave.empty();
    if (waveNow || morph) {
        // .seek.wave: 2 px bars every 3 px; played part in the accent-2 → accent gradient, the rest dim
        const auto& pk = waveNow ? wave.peaks() : mixOld.wave;
        const float mp = morph && waveNow ? mixWipe() : morph ? 0.f : 1.f;
        // as every section's (App::wipeEdge), across the waveform and the song's length beside it (one swipe: wipeGroup_)
        float soft;
        const float edgeX = wipeGroup_.on ? wipeEdge(wipeGroup_.x0, wipeGroup_.x1, mp, soft) : wipeEdge(x, x + w, mp, soft);
        const Gfx::Pitch pt = g.barPitch(2, 3);   // 2 px bars every 3, in whole real pixels
        const float x0 = g.onPixel(x);
        const int bars = int(w / pt.step);
        const float px = shown * w, cur = frac * w;
        auto barOf = [&](const std::vector<float>& q, int i) {
            const size_t a = size_t(i) * q.size() / bars, b = std::max(a + 1, size_t(i + 1) * q.size() / bars);
            float v = 0;
            for (size_t j = a; j < b && j < q.size(); j++) v = std::max(v, q[j]);
            return v;
        };
        for (int i = 0; i < bars; i++) {
            float v = barOf(pk, i);
            if (mp < 1) {   // left of the edge the next song's bar, right of it the outgoing one's, blended across it
                const float old = barOf(mixOld.wave, i), wf = waveNow ? std::clamp((edgeX - (x0 + i * pt.step)) / soft + 0.5f, 0.f, 1.f) : 0.f;
                v = old + (v - old) * wf;
            }
            const float bh = std::max(1.5f, v * h * 0.9f), bx = x0 + i * pt.step;
            const float t = float(i) / bars;
            if (i * pt.step < px) {
                const Rgb c{ pal.accent2.r + (pal.accent.r - pal.accent2.r) * t, pal.accent2.g + (pal.accent.g - pal.accent2.g) * t,
                             pal.accent2.b + (pal.accent.b - pal.accent2.b) * t };
                g.rect(bx, cy - bh / 2, pt.bar, bh, c);
            } else if (drag >= 0 && i * pt.step < cur) {
                g.rect(bx, cy - bh / 2, pt.bar, bh, pal.text3);
            } else {
                g.rect(bx, cy - bh / 2, pt.bar, bh, pal.text, 0.13f);
            }
        }
        if (ui.engaged(id)) {   // picked up with A: a knob where ←/→ move it
            g.circle(x + shown * w, cy, 9, pal.accent);
            g.circle(x + shown * w, cy, 6, pal.text);
        }
        if (morph && waveNow && mp > 0 && mp < 1)
            magicEdge(edgeX, wipeGroup_.on ? wipeGroup_.x0 : x, wipeGroup_.on ? wipeGroup_.x1 : x + w, y, y + h, mp);
    } else {
        // .seek .track / .fill / .knob
        const bool hot = drag >= 0 || (ui.focused(id) && ui.showFocusRing());
        const float th = hot ? 6 : 4;
        g.roundRect(x, cy - th / 2, w, th, th / 2, pal.text, 0.16f);
        g.roundRectH(x, cy - th / 2, w * shown, th, th / 2, pal.accent2, pal.accent);
        if (ui.engaged(id)) g.circle(x + w * shown, cy, 9, pal.accent);
        if (hot) g.circle(x + w * shown, cy, 6, pal.text);
    }
    if (drag >= 0 && dur > 0) {
        // .seek .tip
        const std::string tip = fmtTime(shown * dur);
        const float tw = g.measure(tip, { css::Xs }) + 16;
        const float tx = std::clamp(x + w * shown - tw / 2, x - 20, x + w - tw + 20);
        g.roundRect(tx, y - 30, tw, 24, 6, pal.surface2);
        g.textC(tip, { css::Xs }, tx + tw / 2, y - 18, pal.text, 1, Align::Center);
    }
}

void App::marqueeLine(const std::string& s, Font f, float x, float y, float w, const Rgb& c) {
    const int tw = g.measure(s, f);
    if (tw <= int(w)) { g.text(s, f, x, y, c); return; }
    // One text, one round's time, wherever it is drawn: the time is the player bar's artist's (13 px regular: 30 px/s and a 48 px
    // gap) for this text, and each size and weight moves as fast as it must to keep it, its gap in step with its size. So the
    // bar's artist and Now playing's (24 px semibold) start, move and come round together (the user, 2026-11-02: Now playing's
    // was slower, then took longer a round)
    constexpr float Rest = 3, Fade = 24, RefSpeed = 30, RefGap = 48;
    const float k = float(f.size) / css::Sm;
    const float round = (float(g.measure(s, { css::Sm })) + RefGap) / RefSpeed;   // seconds a round takes, rests apart
    const float loop = float(tw) + RefGap * k, Speed = loop / round;
    float off = 0;
    if (store.motion) {
        const float t = std::fmod(float(time - marqueeAt), Rest + round);
        off = std::max(0.f, t - Rest) * Speed;
    }
    // no fade at the left while it rests at its start, nor as the next round settles there
    const float fadeL = std::max(0.f, std::min({ Fade, off * 2, (loop - off) * 2 }));
    g.textScroll(s, f, x, y, w, off, fadeL, Fade, c);
    if (off > loop - w) g.textScroll(s, f, x, y, w, off - loop, fadeL, Fade, c);   // the next round coming in
}

void App::drawPlayerbar() {
    const float y0 = css::H - css::BarH, cy = y0 + css::BarH / 2;
    g.rect(0, y0, css::W, css::BarH, pal.bg, 0.8f);   // --panel-strong
    g.rect(0, y0, css::W, 1, pal.text, 0.09f);

    // its ends step aside for a camera (Screen edges)
    const float padL = 18 + edge_.barL, padR = 18 + edge_.barR, gap = 20, total = css::W - padL - padR - gap * 2;
    const float leftW = total / 4, centerW = total / 2;
    const float lx = padL, cx0 = padL + leftW + gap, rx = cx0 + centerW + gap;
    const bool hasTrack = now.track >= 0 || extOn;   // (an outside source: AirPlay)

    // .pb-track
    const SDL_FRect artR{ lx, cy - 28, 56, 56 };
    const Id artId = uid("pb.art");
    if (ui.item(artId, artR, 8)) navigate(View::Now);
    // A soft halo all around the cover, like the play button's (three faint layers that ease into the
    // background), a little wider: it reaches ~16 px past the sides and just past the corners.
    for (float r : { 36.f, 40.f, 44.f }) g.glow(artR.x + 28, artR.y + 28, r, pal.glow, 0.26f);
    // an Automix join, Immersive or Simple: the bar swipes as the page does (App::pageWipe_), each section across its own
    // width on the join's clock: its cover and frost, names, time and waveform
    struct PageOff { PageWipe& p; ~PageOff() { p.on = false; } } pageOff{ pageWipe_ };
    if (store.mixAnimNow != 0 && hasTrack && time < mixUntil && mixOld.on) pageWipe_.on = true;
    if (pageWipe_.on) {
        wipedImage(artR.x, artR.x + 56, artR.y, artR.y + 56, false, !now.cover, [&](bool old) {
            if (SDL_Texture* c = mixCover(old, now.cover, now.prevCover)) g.image(c, artR.x, artR.y, 56, 56, 8);
            else {
                g.roundRect(artR.x, artR.y, 56, 56, 8, pal.surface2);
                g.icon(ICON_MUSIC, artR.x + 28, artR.y + 28, 22, pal.text3, 0.6f);
            }
        });
    } else if (now.cover) {
        if (now.prevCover && now.fade < 1) g.image(now.prevCover, artR.x, artR.y, 56, 56, 8);
        g.image(now.cover, artR.x, artR.y, 56, 56, 8, ease(now.fade));
    } else {
        g.roundRect(artR.x, artR.y, 56, 56, 8, pal.surface2);
        g.icon(ICON_MUSIC, artR.x + 28, artR.y + 28, 22, pal.text3, 0.6f);
    }
    // an Automix join, Immersive or Simple (Settings › Appearance › Automix: the user, 2026-10-29): the player bar's cover
    // frosts as the big one does, without the hint; its title and artist and the seek bar's waveform give way to the next
    // song's, timed to the join (Full: the bar is not seen on Now playing then, and stays as it is elsewhere)
    const bool barMix = store.mixAnimNow != 0 && hasTrack && time < mixUntil;
    if (barMix) drawAutomixCover(artR.x, artR.y, 56, artR.x + 28, artR.y + 28, 56, 8, false, 1.5f);
    const float metaX = lx + 56 + 12, heartX = lx + leftW - 17;
    const int metaW = int(heartX - 17 - 12 - metaX);
    // A title or artist too long for its room scrolls slowly, in a loop, instead of ending in "…": it rests 3 s
    // at its start (each new track starts there), then moves at 30 px/s; the edge it is cut at fades
    // (Gatefold's 36 px mask, a little shorter here). With Motion off it does not move, the end just fades.
    if (now.track != marqueeTrack || now.info.title != marqueeTitle) { marqueeTrack = now.track; marqueeTitle = now.info.title; marqueeAt = time; }
    auto line = [&](const std::string& s, Font f, float y, const Rgb& c) { marqueeLine(s, f, metaX, y, float(metaW), c); };
    auto names = [&](bool old) {
        const TrackInfo& in = old ? mixOld.info : now.info;
        line(hasTrack ? in.title : "—", { css::Base, Weight::SemiBold }, cy - 21, pal.text);
        line(hasTrack ? in.artist : "", { css::Sm }, cy + 1, pal.text3);
    };
    if (barMix && mixOld.on) wiped(metaX, metaX + metaW, cy - 24, cy + 20, names);
    else names(false);
    const bool liked = currentLiked();   // (a server's track: its star)
    hint(uid("pb.like"), liked ? "Remove from Liked" : "Add to Liked");
    if ((!extOn || srvq_.on) && iconButton(uid("pb.like"), liked ? ICON_HEART_F : ICON_HEART, heartX, cy, liked) && (now.track >= 0 || srvq_.on)) toggleCurrentLike();

    // .pb-center: controls (42 px) + 6 px + seek (30 px), centred in the bar
    // controls 42 px, gap 6 px, seek 30 px (seekY is the seek row's centre)
    const float ctrlY = y0 + (css::BarH - 78) / 2 + 21, seekY = ctrlY + 21 + 6 + 15;
    const float mid = cx0 + centerW / 2;
    const Order order = shownOrder();   // (in a session, the other device shows the starter's)
    const char* orderIcon = order == Order::Shuffle ? ICON_SHUFFLE : order == Order::RepeatOne ? ICON_REPEAT1
                          : order == Order::RepeatAll ? ICON_REPEAT : ICON_ORDER;
    {
        // the order playing, as the button's toast names it (the user, 2026-10-29: no "Order:")
        static const char* orderHints[] = { "Default order", "Repeat playlist", "Repeat track", "Shuffle" };
        hint(uid("pb.order"), orderHints[int(order)]);
        hint(uid("pb.prev"), "Previous");
        hint(uid("pb.play"), extOn ? "Pause on " + extFrom : isPlaying() ? "Pause" : "Play");
        hint(uid("pb.next"), "Next");
        hint(uid("pb.sac"), stopAfterCurrent ? "Stop after this track: on" : "Stop after this track");
        hint(uid("pb.canvas"), "Canvas");
        hint(uid("pb.mute"), volumeShown <= 0.001f ? "Unmute" : "Mute");
        hint(uid("pb.queue"), "Playlist");
        hint(uid("pb.eq"), "Equalizer (hold for its settings)");
        hint(uid("pb.sleep"), "Sleep timer");
    }
    // The order and Stop after this track greyed with the rest while the controls are someone else's: an iPhone over AirPlay sends
    // nothing of its shuffle or repeat, and a session's starter decides (the user, 2026-11-02)
    const bool off = controlsOff();
    if (off) g.icon(orderIcon, mid - 21 - 10 - 34 - 10 - 17, ctrlY, 18, pal.text2, 0.35f);
    else if (iconButton(uid("pb.order"), orderIcon, mid - 21 - 10 - 34 - 10 - 17, ctrlY, order != Order::Default)) cycleOrder();
    // an outside source (AirPlay): previous, play and next are the iPhone's, greyed here and not pressable (no DACP control yet); a
    // session's starter's, when it alone changes what plays
    if (off) g.icon(ICON_PREV, mid - 21 - 10 - 17, ctrlY, 18, pal.text2, 0.35f);
    else if (iconButton(uid("pb.prev"), ICON_PREV, mid - 21 - 10 - 17, ctrlY)) prev();
    if (off) {
        g.circle(mid, ctrlY, 21, pal.text, 0.35f);
        g.icon(extOn || isPlaying() ? ICON_PAUSE : ICON_PLAY, mid, ctrlY, 20, pal.bg);
    } else {
        // .play-btn: 42 px circle in --text, icon in --bg
        const Id pid = uid("pb.play");
        if (ui.item(pid, { mid - 21, ctrlY - 21, 42, 42 }, 21)) togglePlay();
        const float s = ui.pressed(pid) ? 0.95f : (ui.focused(pid) && ui.showFocusRing()) ? 1.06f : 1.f;
        // box-shadow: 0 6px 22px -8px glow 90%, drawn as a soft halo all around the button. Three faint layers
        // instead of one strong one, so it eases into the background rather than ringing the edge; it is gone
        // 9 px out, before the seek wave (27 px below the centre).
        for (float r : { 24.f, 27.f, 30.f }) g.glow(mid, ctrlY, r, pal.glow, 0.3f);
        g.circle(mid, ctrlY, 21 * s, pal.text);
        if (isPlaying()) g.icon(ICON_PAUSE, mid, ctrlY, 20 * s, pal.bg);
        else g.icon(ICON_PLAY, mid, ctrlY, 20 * s, pal.bg);
    }
    if (off) g.icon(ICON_NEXT, mid + 21 + 10 + 17, ctrlY, 18, pal.text2, 0.35f);
    else if (iconButton(uid("pb.next"), ICON_NEXT, mid + 21 + 10 + 17, ctrlY)) next(false);
    if (off) g.icon(ICON_PAUSE_C, mid + 21 + 10 + 34 + 10 + 17, ctrlY, 18, pal.text2, 0.35f);
    else if (iconButton(uid("pb.sac"), ICON_PAUSE_C, mid + 21 + 10 + 34 + 10 + 17, ctrlY, stopAfterCurrent)) {
        stopAfterCurrent = !stopAfterCurrent;
        toast(stopAfterCurrent ? "Stop after the current track" : "Keep playing after this track");
    }

    // .pb-seek: 48 | 1fr | 48, gap 10
    const float seekW = std::min(760.f, centerW), sx = mid - seekW / 2;
    const float timeTop = seekY - g.lineHeight({ css::Xs }) / 2.f;   // (the time that runs: fixed-width digits, so it does not move)
    g.textNum(hasTrack ? fmtTime(shownPosition()) : "0:00", { css::Xs }, sx + 48, timeTop, pal.text3, 1, Align::Right);
    auto durText = [&](bool old) { g.textNum(hasTrack ? fmtTime(old ? mixOld.info.duration : now.info.duration) : "0:00", { css::Xs }, sx + seekW - 48, timeTop, pal.text3); };
    // the song's length swipes with the waveform, as one (the user, 2026-10-29: "just the waveform's effect")
    if (pageWipe_.on) wipeGroup_ = { true, sx + 58, sx + seekW - 48 + float(g.measure("00:00", { css::Xs })) };
    if (pageWipe_.on) wiped(sx + seekW - 48, sx + seekW, seekY - 10, seekY + 10, durText);
    else durText(false);
    drawSeek(sx + 58, seekY - 15, seekW - 116, 30, uid("pb.seek"));
    wipeGroup_.on = false;

    // .pb-right: queue, volume, canvas
    float x = rx + leftW;
    if (iconButton(uid("pb.canvas"), ICON_CANVAS, x - 17, cy)) openCanvas(true);
    x -= 34 + 14;   // Gatefold has the mini-player button here; keep the slider clear of the Canvas button
    float vol = volumeShown;
    slider(uid("pb.vol"), x - 110, cy, 110, volumeShown, &vol);
    if (vol != volumeShown) { volumeShown = vol; player.setVolume(vol); }
    x -= 110 + 4 + 7;
    static float beforeMute = 1;
    if (iconButton(uid("pb.mute"), volumeShown <= 0.001f ? ICON_VOL_X : ICON_VOL, x - 17, cy)) {
        if (volumeShown > 0.001f) { beforeMute = volumeShown; volumeShown = 0; }
        else volumeShown = beforeMute > 0.01f ? beforeMute : 1;
        player.setVolume(volumeShown);
    }
    x -= 34 + 8;
    if (iconButton(uid("pb.queue"), ICON_QUEUE, x - 17, cy, route.view == View::Queue)) navigate(View::Queue);
    // The equalizer: lit while it is on. A press turns it on or off; a long press opens Settings → Equalizer.
    x -= 34 + 8;
    if (iconButton(uid("pb.eq"), ICON_EQ, x - 17, cy, store.eq.on != 0, 34, 18, Hold)) toggleEq();
    if (ui.longPressed(uid("pb.eq"))) openEqualizer();
    // The sleep timer: lit while it runs (or stop-after is on, its "End of this track"), the time left beside it.
    x -= 34 + 8;
    const double sl = sleepLeft();
    if (iconButton(uid("pb.sleep"), ICON_MOON, x - 17, cy, sl > 0)) openSleep();
    if (sl > 0) g.textNum(fmtTime(std::ceil(sl)), { css::Xs, Weight::SemiBold }, x - 34 - 4, cy - g.lineHeight({ css::Xs, Weight::SemiBold }) / 2.f, pal.accentInk, 1, Align::Right);
}

// ------------------------------------------------------------------ overlays

/** What runs in the background now, by slot: the library's own work (one step at a time), the overlay's covers, levelling. */
void App::backgroundJobs(Job (&jobs)[JobSlots], bool (&busy)[JobSlots]) {
    const int n = int(lib.tracks.size());
    const int pos = meta.sweepPos(), cached = meta.coversCached();
    if (libCheck_.joinable() && !libCheckDone_) { jobs[0] = { "Checking the music folder", 0, 0, SetLibrary, Stoppable::None, "Music folder" }; busy[0] = true; }
    else if (n > 0 && meta.taggedCount() < n) { jobs[0] = { "Reading tags", meta.taggedCount(), n, SetLibrary, Stoppable::None, "Tags read" }; busy[0] = true; }
    else if (!store.stopCovers && pos > 0 && pos < n && cached < n) {
        jobs[0] = { "Preparing covers", std::min(cached, n), n, SetLibrary, Stoppable::Covers, "Covers ready" };
        busy[0] = true;
    }
    if (thumbs.running() && !store.stopThumbs) {
        int made = 0, left = 0;
        thumbs.progress(made, left);
        if (left > 0) { jobs[1] = { "Overlay covers", made, made + left, SetPlayback, Stoppable::Thumbs, "Covers for the overlay" }; busy[1] = true; }
    }
    // Mood playlists: the library's moods, in the same worker (it measures loudness and Automix's analysis in the same decode);
    // first, or its busy worker would read as "Levelling" with every track measured
    int moodDone = 0, moodTotal = 0;
    moodProgress(moodDone, moodTotal);
    if (moodTotal > 0 && !store.stopLevels && moodDone < moodTotal && levels.loaded()) {
        jobs[2] = { "Moods", moodDone, moodTotal, SetPlayback, Stoppable::Levels, "Analysed" };
        busy[2] = true;
    } else if (store.levelMode && !store.stopLevels && levels.busy()) {   // short: the count shares the line
        jobs[2] = { "Levelling", std::min(int(levels.count()), n), n, SetPlayback, Stoppable::Levels, "Measured" };
        busy[2] = true;
    } else if (!store.levelMode) {   // levelling off: only Automix playlists' tracks are analysed, first in the worker's queue
        // (shown while any is waiting: the worker counted itself busy for the library only, so the card never came)
        int done = 0, total = 0;
        automixProgress(done, total);
        if (done < total) { jobs[2] = { "Automix", done, total, SetPlayback, Stoppable::Levels, "Analysed" }; busy[2] = true; }
    }
    // Files: copying, or deleting a folder (settingsCat −1: the card opens Files where it ends)
    if (fileWork.thread.joinable() && !fileWork.done) {
        jobs[3] = { fileWork.label, fileWork.count.load(), fileWork.total.load(), fileWork.cat, Stoppable::None, nullptr };
        busy[3] = true;
    }
    // An update downloading, checked and put in place (not the short check of the list): its card opens Settings → Updates
    if (updater.busy() && updater.phase() != Updater::Checking) {
        const Updater::Phase ph = updater.phase();
        const bool bytes = ph == Updater::Downloading || ph == Updater::Verifying;
        // Short: the count in megabytes takes room (the version is on Settings' row)
        const char* what = ph == Updater::Downloading ? "Downloading" : ph == Updater::Verifying ? "Checking it" : "Installing";
        jobs[4] = { what, int(bytes ? updater.done() / 1024 : updater.done()), int(bytes ? updater.total() / 1024 : updater.total()),
                    SetUpdates, Stoppable::None, "Check for updates", bytes };
        if (ph == Updater::BackingUp || ph == Updater::Installing) jobs[4].total = 0;   // quick: just busy
        busy[4] = true;
    }
    // Search by lyrics: every song's lyrics read once, on the first start or after many changes (the user, 2026-11-05: "why dont we
    // have a worker banner for this? like for leveling, cover"); not stopped (light work, as Reading tags); it opens Settings › Lyrics
    {
        // after the tags, as Preparing covers comes after Reading tags: the same files, one reader on the SD card at a time
        lyrIndex.hold(lyrHeld || (n > 0 && meta.taggedCount() < n));   // (lyrHeld: until counted, or Not now on its card)
        const LyricIndex::Progress lp = lyrIndex.progress();
        if (!store.stopLyrics && !lyrIndex.held() && lp.total > 0 && lp.read < lp.total) {
            jobs[5] = { lp.checking ? "Checking lyrics" : "Reading lyrics", lp.read, lp.total, 5 /* Settings › Lyrics */, Stoppable::Lyrics, "Lyrics read", false, ICON_QUOTE };
            busy[5] = true;
        }
    }
#ifdef GF_DEV
    if (Job st; selfTestJob(st)) { jobs[8] = st; busy[8] = true; }   // Debug › Testing: how far the self test is
#endif
    // The server's downloads (Subsonic, stage 4): this batch, one song at a time; Stop keeps the queue
    {
        int done = 0, total = 0;
        subsonic.dlProgress(done, total);
        if (!store.stopDownloads && total > 0 && done < total) {
            jobs[7] = { "Downloading", done, total, SetLibrary, Stoppable::Downloads, "Downloaded", false, ICON_DOWNLOAD };
            busy[7] = true;
        }
    }
    // tag and cover changes into the files: this run, a song at a time (Stop keeps the rest waiting, in Songate meanwhile)
    if (!store.stopTagWrites && tagTotal_ > 0 && tagDone_ < tagTotal_) {
        jobs[9] = { "Writing tags", tagDone_, tagTotal_, SetLibrary, Stoppable::TagWrites, "Written into files", false, ICON_TAG };
        busy[9] = true;
    }
#ifdef GF_PREVIEW
    // The PC preview has no background player, and its covers are made too fast to overlap: GF_JOBS=1 shows all
    // three at once (hostshim.cpp), so the stack and its menus can be looked at.
    if (getenv("GF_JOBS")) {
        const int t = int(time * 20);
        if (!busy[0] && !store.stopCovers) { jobs[0] = { "Preparing covers", std::min(1015 + t, 3074), 3074, SetLibrary, Stoppable::Covers, "Covers ready" }; busy[0] = true; }
        if (!busy[1] && !store.stopThumbs) { jobs[1] = { "Overlay covers", std::min(449 + t, 2547), 2547, SetPlayback, Stoppable::Thumbs, "Covers for the overlay" }; busy[1] = true; }
        if (!busy[2] && !store.stopLevels) { jobs[2] = { "Levelling", std::min(13 + t / 10, 3074), 3074, SetPlayback, Stoppable::Levels, "Measured" }; busy[2] = true; }
        if (!busy[5] && !store.stopLyrics) { jobs[5] = { "Reading lyrics", std::min(212 + t, 3074), 3074, 5, Stoppable::Lyrics, "Lyrics read", false, ICON_QUOTE }; busy[5] = true; }
    }
#endif
}

/**
 * The stack as layers: each job's share of the front card (the first one running in slot order has it; one fading
 * out hands its share to the next, so the card stays and only its contents change), the front card's own opacity
 * (all of them together), and how many cards peek out behind it, in fractions.
 */
App::JobStack App::jobStack() const {
    JobStack st;
    float rest = 1, layers = 0;
    for (int i = 0; i < JobSlots; i++) {
        const float s = jobCards[i].shown;
        st.weight[i] = s * rest;
        rest *= 1 - s;
        layers += s;
        st.running += s > 0.5f;
        if (st.front < 0 || st.weight[i] > st.weight[st.front]) st.front = i;
    }
    st.body = 1 - rest;
    st.behind = std::max(0.f, layers - st.body);
    return st;
}

float App::jobsHeight(bool collapsed) const {
    // The front card's height as the stack fades in, and a peek for each card behind it.
    const JobStack st = jobStack();
    return (collapsed ? 30 : 52) * st.body + (collapsed ? 4 : 6) * st.behind;
}

std::string App::jobCount(const Job& j) const {
    if (j.total <= 0) return {};
    if (!j.kb) return std::to_string(j.done) + " of " + std::to_string(j.total);
    char b[48];
    snprintf(b, sizeof b, "%.1f of %.1f MB", j.done / 1024.0, j.total / 1024.0);
    return b;
}

void App::showSetting(const Job& j) {
    if (j.settingsCat < 0) { openFiles(fileWork.dest); return; }   // Files' work: the folder it ends in
    settingsCat = j.settingsCat;
    settingsQuery.clear();
    settingsPhoneOpen_ = settingsPhoneDirect_ = true;   // upright: its page, and back returns from where it came
    navigate(View::Settings);
    settingsJump = j.row ? j.row : "";
    settingsJumpAt = time;
}

void App::applyJobStops() {
    meta.pauseCovers(store.stopCovers || coversHeld);
    thumbs.pause(store.stopThumbs);
    levels.pause(store.stopLevels);
    lyrIndex.pause(store.stopLyrics);
    subsonic.pauseDownloads(store.stopDownloads);
}

void App::setJobStopped(Stoppable which, bool stopped) {
    bool& flag = which == Stoppable::Covers ? store.stopCovers : which == Stoppable::Thumbs ? store.stopThumbs
               : which == Stoppable::Lyrics ? store.stopLyrics
               : which == Stoppable::Downloads ? store.stopDownloads : which == Stoppable::TagWrites ? store.stopTagWrites : store.stopLevels;
    if (which == Stoppable::None || flag == stopped) return;
    flag = stopped;
    if (which == Stoppable::Covers) coversHeld = false;   // your Stop or Resume answers the launch's hold too
    applyJobStops();
    persistSoon(0.3f);
    static const char* names[] = { "", "Covers", "Overlay covers", "Levelling", "Reading lyrics", "Mood tags", "Downloads", "Writing tags" };
    toast(std::string(names[int(which)]) + (stopped ? " stopped. Resume it in Settings" : " resumed"));
}

/**
 * The front card of the stack above Settings: the job, a count and a thin bar (a sliding one when there is no
 * count). Shown once a job has run for a second, so the quick ones at launch do not flash; it fades out when
 * the work is done. With others running behind it, pressing it lists them all (openJobList); alone, a job
 * that can wait (the covers, levelling) gets its menu (stop it, or its Settings row), the others go straight
 * to their Settings row. Collapsed: just the bar.
 */
void App::drawJobCard(const JobStack& st, float x, float bottom, float w, bool collapsed) {
    const float h = collapsed ? 22 : 44, y = bottom - h;
    const int slot = st.front;
    const Id id = uid("side.job");   // one id for the stack, whichever job is in front: the focus stays on it when the front one goes
    if (ui.item(id, { x, y, w, h }, 10)) {
        const Job& j = jobCards[slot].job;
        if (st.running > 1) openJobList();                          // every job running, each with its own menu
        else if (j.stop != Stoppable::None) openJobMenu(slot, false);   // Stop it, or its Settings row
        else showSetting(j);
    }
    const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
    g.roundRect(x, y, w, h, 10, pal.text, (hot ? 0.09f : 0.05f) * st.body);
    const float bx = x + 10, bw = w - 20, by = y + h - 10;
    g.roundRect(bx, by, bw, 3, 1.5f, pal.text, 0.12f * st.body);
    // The contents of each job with a share of the front: a job leaving fades out, then the next one fades in,
    // in the same card (no crossing lines: each shows only while it has most of the card).
    for (int i = 0; i < JobSlots; i++) {
        const float a = std::clamp((st.weight[i] - 0.5f) / 0.5f, 0.f, 1.f);
        if (a <= 0.01f) continue;
        const Job& j = jobCards[i].job;
        const float frac = j.total > 0 ? std::clamp(float(j.done) / float(j.total), 0.f, 1.f) : -1;
        if (frac >= 0) {
            g.roundRect(bx, by, std::max(3.f, bw * frac), 3, 1.5f, pal.accent, a);
        } else {   // busy without a count: a segment sliding along
            const float t = float(std::fmod(time, 1.6) / 1.6), seg = bw * 0.3f;
            const float sx = bx + (bw + seg) * t - seg, l = std::max(bx, sx), r = std::min(bx + bw, sx + seg);
            if (r > l) g.roundRect(l, by, r - l, 3, 1.5f, pal.accent, a);
        }
        if (collapsed) continue;
        const float right = x + w - 10;
        const Font lf{ css::Xs, Weight::SemiBold };
        const std::string count = jobCount(j);
        // The count in fixed-width digits, and the label's room taken from the widest the count can be: nothing moves as it rises.
        Job full = j;
        full.done = j.total;
        const std::string widest = jobCount(full);
        const int cw = widest.empty() ? 0 : g.measureNum(widest, { css::Xs });
        g.text(j.label, lf, x + 10, y + 8, pal.text2, a, Align::Left, int(right - x - 10 - cw - 8));
        if (!count.empty()) g.textNum(count, { css::Xs }, right, y + 8, pal.text3, a, Align::Right);
    }
}

/** The overlay is missing many covers: a small modal offering to make them now (they are made in the background). */
void App::drawCoverNotice() {
    const bool app = appCoverNotice == 1;   // the app's own covers come first, the overlay's after (never both at once)
    const bool lyr = !app && coverNotice != 1 && lyrNotice == 1;   // then search by lyrics
    if (!app && coverNotice != 1 && !lyr) return;
    const float a = std::clamp(float(time - coverNoticeAt) / 0.25f, 0.f, 1.f);
    ui.region(Region::Modal);
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.45f * a);
    const Font tf{ css::Lg, Weight::Bold }, sf{ css::Base };
    const float w = cardW(520), pad = 24, x = (css::W - w) / 2;
    char sub[240];
    if (lyr)
        snprintf(sub, sizeof sub, "%d of %d tracks' lyrics are not read yet, so search by lyrics cannot find those songs. Read now, "
                 "in the background (a few minutes for a large library).", lyrMissing, int(lib.tracks.size()));
    else if (app)
        snprintf(sub, sizeof sub, "%d of %d tracks have no small cover yet, so lists make each one as you scroll to it. Made now, "
                 "in the background, every list opens with its covers (a few minutes for a large library).", coverMissing, int(lib.tracks.size()));
    else
        snprintf(sub, sizeof sub, "%d of %d tracks have none yet, so the overlay shows no cover for them. They are made in the "
                 "background while Songate is open.", coverMissing, int(lib.tracks.size()));
    const std::vector<std::string> lines = g.wrap(sub, sf, int(w - pad * 2 - 52), 4);
    const float h = pad + 26 + 8 + lines.size() * 21.f + 20 + 34 + pad;
    const float y = (css::H - h) / 2 + (1 - a) * 10;
    g.roundRect(x, y + 10, w, h, 18, { 0, 0, 0 }, 0.3f * a);
    g.roundRect(x, y, w, h, 18, pal.surface2, a);
    g.roundStroke(x, y, w, h, 18, 1, pal.text, 0.1f * a);
    g.roundRect(x + pad, y + pad, 40, 40, 11, pal.accent, 0.16f * a);
    g.icon(lyr ? ICON_QUOTE : ICON_IMAGE, x + pad + 20, y + pad + 20, 20, pal.accentInk, a);
    const float tx = x + pad + 52;
    g.text(lyr ? "Lyrics for search" : app ? "Covers for your library" : "Covers for the overlay are missing", tf, tx, y + pad, pal.text, a, Align::Left, int(w - pad * 2 - 52));
    float ly = y + pad + 26 + 8;
    for (const auto& l : lines) { g.text(l, sf, tx, ly, pal.text2, a); ly += 21; }
    bool make = false, later = false;
    const float by = ly + 20;
    // one width for the first button on every such card, so Not now sits in the same place (the user, 2026-11-05: "The position of
    // those buttons are not consistent with the covers card buttons")
    const float firstW = 16 + 24 + g.measure("Make them now", { css::Sm, Weight::SemiBold }) + 16;
    const float bw = button(uid("notice.make"), ICON_REFRESH, lyr ? "Read them now" : "Make them now", tx, by, true, &make, false, false, DefaultFocus, firstW);
    button(uid("notice.later"), nullptr, "Not now", tx + bw + 8, by, false, &later);
    if (coverNoticeAt == time) ui.focus(uid("notice.make"));   // the first frame: focus moves onto it
    if (lyr) {
        if (make) {
            releaseLyrics();
            toast("Reading them in the background: the card above Settings shows how far it is");
        } else if (later) {
            toast("Settings › Lyrics › Lyrics read reads them when you want");
        }
        if (make || later) closeCoverNotice();
        return;
    }
    if (app) {
        if (make) {
            releaseCovers();
            toast("Making them in the background: the card above Settings shows how far it is");
        } else if (later) {
            toast("Settings › Library › Covers ready makes them when you want");
        }
        if (make || later) closeCoverNotice();
        return;
    }
    if (make) {
        setJobStopped(Stoppable::Thumbs, false);
        for (int t = 0; t < int(lib.tracks.size()); t++) thumbFor(t, false);
        toast("Making them in the background: the card above Settings shows how far it is");
        closeCoverNotice();
    }
    if (later) closeCoverNotice();
}

/** After the sleep timer: the console goes to sleep when the count reaches 0, unless you stay awake. */
void App::drawSleepPrompt() {
    if (sleepPromptAt <= 0) return;
    const float a = std::clamp(float(time - sleepPromptAt) / 0.25f, 0.f, 1.f);
    const int left = std::max(0, int(std::ceil(store.sleepConsole - (time - sleepPromptAt))));
    ui.region(Region::Modal);
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.45f * a);
    const Font tf{ css::Lg, Weight::Bold }, sf{ css::Base };
    const float w = cardW(520), pad = 24, x = (css::W - w) / 2;
    const std::vector<std::string> lines =
        g.wrap("The sleep timer has paused the music. " GF_CAP_THE_CONSOLE " goes to sleep by itself unless you stay awake.", sf, int(w - pad * 2 - 52), 3);
    const float h = pad + 26 + 8 + lines.size() * 21.f + 20 + 34 + pad;
    const float y = (css::H - h) / 2 + (1 - a) * 10;
    g.roundRect(x, y + 10, w, h, 18, { 0, 0, 0 }, 0.3f * a);
    g.roundRect(x, y, w, h, 18, pal.surface2, a);
    g.roundStroke(x, y, w, h, 18, 1, pal.text, 0.1f * a);
    g.roundRect(x + pad, y + pad, 40, 40, 11, pal.accent, 0.16f * a);
    g.icon(ICON_MOON, x + pad + 20, y + pad + 20, 20, pal.accentInk, a);
    const float tx = x + pad + 52;
    g.text("Going to sleep in " + std::to_string(left), tf, tx, y + pad, pal.text, a, Align::Left, int(w - pad * 2 - 52));
    float ly = y + pad + 26 + 8;
    for (const auto& l : lines) { g.text(l, sf, tx, ly, pal.text2, a); ly += 21; }
    bool sleepNow = false, stay = false;
    const float by = ly + 20;
    const float bw = button(uid("sleep.now"), ICON_MOON, "Sleep now", tx, by, true, &sleepNow, false, false, DefaultFocus);
    button(uid("sleep.stay"), nullptr, "Stay awake", tx + bw + 8, by, false, &stay);
    if (sleepPromptAt == time) ui.focus(uid("sleep.now"));
    if (sleepNow) { closeSleepPrompt(); plat::requestSleep(); }
    if (stay) closeSleepPrompt();
}

/** The covers cache's sweep, held at launch until the covers were counted (or asked about), goes ahead. */
void App::releaseCovers() {
    if (!coversHeld) return;
    coversHeld = false;
    applyJobStops();
}

/** Search by lyrics' reading, held at launch until counted (or asked about, if many are unread), goes ahead. */
void App::releaseLyrics() { lyrHeld = false; }

void App::closeCoverNotice() {
    // the app's question is answered first (B or Not now leaves its sweep held for this launch); then the overlay's
    if (appCoverNotice == 1) appCoverNotice = 2;
    else if (coverNotice == 1) coverNotice = 2;
    else lyrNotice = 2;
    if (noticeFocus_) ui.focus(noticeFocus_);
    else ui.focusDefault();
}

void App::drawToast() {
    if (toastT <= 0) return;
    const float a = std::min(1.f, toastT / 0.25f) * std::min(1.f, (3.2f - toastT) / 0.2f + 0.01f);
    const Font f{ css::Sm, Weight::SemiBold };
    const float w = std::min(float(g.measure(toastText, f) + 36), css::W - 24.f), h = 40;   // (never wider than the screen)
    float bottom = css::H - css::BarH - 20;
#ifdef GF_ANDROID
    // upright: over the bar of five and, off Now playing, the mini player (it sat on the mini player's title)
    // (Now playing: in the gap above its seek bar, clear of the cover, the transport and the badges)
    if (phonePortrait()) bottom = route.view == View::Now && phoneToastBottom_ > 0 ? phoneToastBottom_ : css::H - 68 - (64 + 16 + 12);
#endif
    const float x = (css::W - w) / 2, y = bottom - h + (1 - a) * 8;
    g.roundRect(x, y + 6, w, h, 12, { 0, 0, 0 }, 0.25f * a);
    g.roundRect(x, y, w, h, 12, pal.surface2, a);
    g.roundStroke(x, y, w, h, 12, 1, pal.text, 0.09f * a);
    g.textC(toastText, f, x + 18, y + h / 2, pal.text, a, Align::Left, int(w - 36));
}

void App::drawFocusRing() {
    if (!ui.showFocusRing()) return;
    const Id id = ui.focusedId();
    if (!id || !ui.hasItem(id)) return;
    SDL_FRect r = ui.rectOf(id);
    if (r.w <= 0) return;
    // the player bar's seek: its touch area reaches up to the play button; the ring hugs the waveform instead, clear of it (the user,
    // 2026-11-06: "the outline of the waveform and scrub line is clipping the pause/play button")
    if (id == uid("pb.seek")) { r.y += 6; r.h -= 12; }
    // :focus-visible { outline: 2px solid var(--accent-ink); outline-offset: 2px }
    const float rad = ui.radiusOf(id) + 3;
    // Behind what is around its list, as the item itself is (the user, 2026-11-04: "every single focus ring is overlaping the
    // UI"): cut at the list's edge where the item goes under it; where the item is all in view, the ring's 6 px outside it stay
    const SDL_FRect c = ui.clipOf(id);
    constexpr float out = 6;
    const float x0 = r.x >= c.x ? r.x - out : c.x, y0 = r.y >= c.y ? r.y - out : c.y;
    const float x1 = r.x + r.w <= c.x + c.w ? r.x + r.w + out : c.x + c.w, y1 = r.y + r.h <= c.y + c.h ? r.y + r.h + out : c.y + c.h;
    if (x1 <= x0 || y1 <= y0) return;   // all of it out of view
    g.clip(x0, y0, x1 - x0, y1 - y0);
    g.roundStroke(r.x - 4, r.y - 4, r.w + 8, r.h + 8, rad, 2, pal.accentInk);
    g.unclip();
}
