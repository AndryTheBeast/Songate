/*
 * Songate · chime.hpp
 * The opening's sound (the user, 2026-10-31: "more magical and with a little sound"): five soft bells rising through an E major
 * chord as the star crosses the name, made here, so that no sound file travels with Songate. Stereo floats at 48 kHz, about a
 * fifth of full scale at most; run on a worker: it takes a moment on the console.
 */
#pragma once
#include <cmath>
#include <vector>

namespace chime {

constexpr int Rate = 48000;
constexpr float Seconds = 1.9f;

inline std::vector<float> make() {
    const int frames = int(Rate * Seconds);
    std::vector<float> out(size_t(frames) * 2, 0.f);
    const struct { float at, hz, pan, gain; } notes[] = {   // when (s), pitch, -1 left .. 1 right, loudness
        { 0.00f, 659.26f, -0.5f, 0.75f }, { 0.13f, 987.77f, -0.2f, 0.7f }, { 0.26f, 1318.5f, 0.1f, 0.62f }, { 0.39f, 1661.2f, 0.35f, 0.5f }, { 0.52f, 1975.5f, 0.55f, 0.42f } };
    const struct { float mul, gain, decay; } partials[] = { { 1, 1, 2.6f }, { 2.0f, 0.32f, 4.4f }, { 3.01f, 0.14f, 7.5f }, { 4.17f, 0.06f, 12 } };   // a bell's, the high ones gone first
    constexpr float tau = 6.283185307f, attack = 0.004f;
    for (const auto& n : notes) {
        const float left = std::sqrt(0.5f * (1 - n.pan)), right = std::sqrt(0.5f * (1 + n.pan));
        for (const auto& p : partials) {
            const float step = tau * n.hz * p.mul / Rate, fall = std::exp(-p.decay / Rate);
            float env = n.gain * p.gain, ph = 0;
            for (int i = int(n.at * Rate); i < frames && env > 0.0001f; i++) {
                const float t = float(i) / Rate - n.at;
                const float s = std::sin(ph) * env * (t < attack ? t / attack : 1);
                out[size_t(i) * 2] += s * left;
                out[size_t(i) * 2 + 1] += s * right;
                ph += step;
                if (ph > tau) ph -= tau;
                env *= fall;
            }
        }
    }
    // A short echo each way, for air; then to the level asked for, and to nothing at the very end
    const int d1 = int(Rate * 0.113f), d2 = int(Rate * 0.171f);
    for (int i = frames - 1; i >= 0; i--) {
        if (i >= d1) out[size_t(i) * 2] += out[size_t(i - d1) * 2 + 1] * 0.22f;
        if (i >= d2) out[size_t(i) * 2 + 1] += out[size_t(i - d2) * 2] * 0.22f;
    }
    float peak = 0.0001f;
    for (float s : out) peak = std::fmax(peak, std::fabs(s));
    const float k = 0.2f / peak;
    const int tail = int(Rate * 0.25f);
    for (int i = 0; i < frames; i++) {
        const float e = i > frames - tail ? float(frames - i) / tail : 1;
        out[size_t(i) * 2] *= k * e;
        out[size_t(i) * 2 + 1] *= k * e;
    }
    return out;
}

}  // namespace chime
