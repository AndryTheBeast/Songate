/*
 * Songate · keyclick.hpp
 * The keyboard card's key sounds (Settings › Controls › Keyboard › Sound; the user chose them, 2026-11-18): made here, once, the same
 * on every build (the console and the PC play them through the player, a phone through Android's sound pool). Mono, peaks near 0.6.
 *   0 Soft   a short tick over a soft body (the first one)
 *   1 Tick   crisp and high, very short
 *   2 Pop    a round pop, its pitch falling, as a bubble
 *   3 Clack  a mechanical keyboard's key: a dull click, a low thump, a little rattle after
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

inline std::vector<float> keyClickWave(int kind, int rate) {
    constexpr double Pi = 3.14159265358979;
    auto frames = [rate](double ms) { return std::max(1, int(rate * ms / 1000)); };
    uint32_t seed = 0x5eed1234u;   // (the same noise each time: the same sound)
    auto noise = [&seed] { seed = seed * 1664525u + 1013904223u; return double(int32_t(seed) >> 8) / 8388608.0; };
    std::vector<float> v;
    switch (kind) {
    case 1: {   // Tick
        v.resize(size_t(frames(16)));
        for (size_t i = 0; i < v.size(); i++) {
            const double t = double(i) / rate, a = std::min(1.0, t / 0.0003);
            v[i] = float(a * (0.7 * std::sin(2 * Pi * 4200 * t) * std::exp(-t / 0.0011) + 0.3 * noise() * std::exp(-t / 0.0006)) * 0.75);
        }
        break;
    }
    case 2: {   // Pop
        v.resize(size_t(frames(45)));
        double phase = 0;
        for (size_t i = 0; i < v.size(); i++) {
            const double t = double(i) / rate, f = 320 + 700 * std::exp(-t / 0.006), a = std::min(1.0, t / 0.0015);
            phase += 2 * Pi * f / rate;
            v[i] = float(a * std::sin(phase) * std::exp(-t / 0.009) * 0.75);
        }
        break;
    }
    case 3: {   // Clack
        v.resize(size_t(frames(60)));
        double lp = 0;
        for (size_t i = 0; i < v.size(); i++) {
            const double t = double(i) / rate, a = std::min(1.0, t / 0.0002);
            lp += (noise() - lp) * 0.35;   // (the noise made dull: a plastic click, not a hiss)
            const double click = lp * std::exp(-t / 0.0035);
            const double thump = std::sin(2 * Pi * 170 * t) * std::exp(-t / 0.010);
            const double rattle = t > 0.022 ? lp * std::exp(-(t - 0.022) / 0.003) : 0;
            v[i] = float(a * (0.6 * click + 0.45 * thump + 0.3 * rattle) * 0.8);
        }
        break;
    }
    default: {   // Soft
        v.resize(size_t(frames(28)));
        for (size_t i = 0; i < v.size(); i++) {
            const double t = double(i) / rate, a = std::min(1.0, t / 0.0006);
            v[i] = float(a * (0.55 * std::sin(2 * Pi * 2400 * t) * std::exp(-t / 0.0025) + 0.45 * std::sin(2 * Pi * 900 * t) * std::exp(-t / 0.006)) * 0.6);
        }
        break;
    }
    }
    for (float& x : v) x = std::clamp(x, -1.f, 1.f);
    return v;
}
