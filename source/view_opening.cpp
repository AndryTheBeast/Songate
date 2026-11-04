/*
 * Songate · view_opening.cpp
 * The opening (the user, 2026-10-31: the tour film's first seconds on the black screen before the app, "without the below text",
 * then "more magical and with a little sound"). Nothing can be drawn while the system loads Songate, so it begins with the first
 * thing that can: one frame of a star, waiting, while the app starts behind it. Once the app runs the star crosses, the name
 * unfurling behind it in a trail of dust, with five bells rising (chime.hpp); then it fades into the app, which is already
 * working underneath. It holds nothing up, and any press goes straight to the fade. The bells never play over music.
 */
#include "words.hpp"
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "chime.hpp"
#include "cores.hpp"

namespace {

constexpr double kCross = 0.8, kFadeAt = 1.6, kFade = 0.5;   // the star's crossing; the whole takes kFadeAt + kFade
constexpr float kBig = 5;                                    // the name, at five times the toolbar's
constexpr int kDust = 56;

Rgb mixed(const Rgb& p, const Rgb& q, float t) { return { p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t }; }
float eased(double t) { t = std::clamp(t, 0.0, 1.0); return float(t * t * (3 - 2 * t)); }
/** The same every time: 0..1 from a number. */
float noise(int i) {
    unsigned x = unsigned(i) * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return float(x % 10000) / 10000.f;
}

}  // namespace

void App::openingScene(double t, float alpha) {
    // five times the toolbar's, smaller where the screen is narrower (a phone upright: the name ran off both sides)
    const float big = std::min(kBig, css::W * 0.84f / std::max(1.f, float(g.wordmarkW())));
    const float w = g.wordmarkW() * big, h = g.wordmarkH() * big, cx = css::W / 2.f, cy = css::H / 2.f, x = cx - w / 2;
    const float run = eased(t / kCross);                                  // how far the star has come, 0..1 across the name
    const float swell = std::sin(float(std::clamp(t / (kCross * 1.5), 0.0, 1.0)) * 3.14159f);   // the light, up as it crosses and back
    const Rgb white{ 1, 1, 1 }, light = mixed(pal.accent, white, 0.55f), warm = mixed(pal.accent2, white, 0.5f);
    g.rect(0, 0, css::W, css::H, pal.bg, alpha);
    const float shine = t >= kCross * 0.75 && t < kCross + 0.75 ? float(-0.25 + 1.5 * (t - kCross * 0.75) / (0.75 + kCross * 0.25)) : -1;
    g.wordmark(x, cy, mixed(pal.accent, pal.text, 0.12f), mixed(pal.accent2, pal.text, 0.2f), mixed(pal.accent2, pal.text, 0.78f), pal.glow,
               (0.3f + 0.35f * swell) * eased((run - 0.75f) / 0.25f), shine, alpha, big, run);   // (its blur only behind letters that are there)
    // The dust: each mote lights as the star passes its place, drifts up and twinkles out
    for (int i = 0; i < kDust; i++) {
        const float u = noise(i), born = float(kCross) * std::acos(1 - 2 * std::clamp(u, 0.f, 1.f)) / 3.14159f, life = 0.8f + 0.7f * noise(i + 100);
        const float k = (float(t) - born) / life;
        if (k <= 0 || k >= 1) continue;
        const float px = x + w * (u * 1.06f - 0.03f) + (noise(i + 200) - 0.5f) * 20 * k;
        const float py = cy + (noise(i + 300) - 0.5f) * h * 1.5f - (22 + 46 * noise(i + 400)) * k;
        const float twinkle = 0.55f + 0.45f * std::sin(float(t) * (9 + 7 * noise(i + 500)) + 6 * u);
        const float a = std::sin(std::sqrt(k) * 3.14159f) * twinkle;
        g.sparkle(px, py, (6 + 11 * noise(i + 600)) * (1 - 0.4f * k), noise(i + 700) < 0.5f ? light : warm, a * alpha);
    }
    // The star: waits at the start, crosses, and goes out at the end of the name
    const float sx = x + w * run, sy = cy - h * 0.18f * std::sin(run * 3.14159f);
    const float left = t < kCross ? 1 : 1 - eased((t - kCross) / 0.35);
    if (left > 0) {
        const float size = (46 + 26 * std::sin(float(t) * 13)) * (0.6f + 0.4f * left) + 16 * swell;
        g.glow(sx, sy, 26, light, 0.3f * left * alpha);
        g.sparkle(sx, sy, size, white, left * alpha);
    }
    // Two last twinkles, as in the film
    const struct { double at; float x, y, size; } stars[] = { { 0.92, x + w - 8, cy - h * 0.42f, 36 }, { 1.1, x + w * 0.64f, cy - h * 0.6f, 24 } };
    for (const auto& s : stars) {
        const double k = (t - s.at) / 0.6;
        if (k > 0 && k < 1) {
            const float a = std::sin(float(k) * 3.14159f);
            g.sparkle(s.x, s.y, s.size * a, white, a * alpha);
        }
    }
}

/** One frame, as soon as the settings say which colours: the app then gets on with starting behind it. */
void App::openingFirst() {
#ifdef GF_WINDOWED
    SDL_RenderSetScale(renderer_, scale_, scale_);
#endif
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    openingScene(0, 1);
    SDL_RenderPresent(renderer_);
    // Music already playing when Songate opens, from the background player (the user, 2026-11-02: "it must not play any
    // chime"): the bells are not even made. Asked here, after the first frame, whether or not Songate uses the background
    // player: it may have been started from the overlay. drawOpening still stops them if music starts while they ring.
    if (store.openingSound && BgClient::mayBePlaying()) logLine("Opening: no bells, the background player is playing");
    else if (store.openingSound)
        chimeThread_ = std::thread([this] {
            cores::moveHere(cores::Files);
            chime_ = chime::make();
            chimeReady_ = true;
        });
}

void App::drawOpening() {
    const bool music = isPlaying() || (player.remote() && bg.state().status == BgState::Playing);
    if (music) player.stopOnce();   // never over music: also when it starts while the bells ring
    if (openingDone_) {
        if (!chime_.empty() && !player.oncePlaying()) std::vector<float>().swap(chime_);   // it has rung
        return;
    }
    if (openingAt_ < 0) openingAt_ = time;
    if (chimeReady_ && chimeThread_.joinable()) {
        chimeThread_.join();
        // In time with the star, or not at all; and never over music, here or from the background player
        if (time - openingAt_ < 0.25 && !music) player.playOnce(&chime_);
    }
    if (openingSkip_ && time - openingAt_ < kFadeAt) openingAt_ = time - kFadeAt;
    openingSkip_ = false;
    const double t = time - openingAt_;
    const float alpha = t < kFadeAt ? 1 : 1 - eased((t - kFadeAt) / kFade);
    if (alpha <= 0) { openingDone_ = true; return; }
    openingScene(t, alpha);
}
