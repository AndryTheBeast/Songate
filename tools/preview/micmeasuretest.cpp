/* Songate · micmeasuretest.cpp: listening together's delay measuring (source/micmeasure.hpp), on recordings made up here, where the
 * answer is known: three devices whose sound is late by known amounts, one microphone with its own delay, noise, music under the chirps,
 * an echo off a wall, and a microphone whose blocks arrive unevenly. Prints each check, then ALL GOOD or SOME FAILED.
 *   clang++ -std=c++20 -DGF_PREVIEW -Itools/preview/out/include -Isource tools/preview/micmeasuretest.cpp source/micmeasure.cpp
 *           -Ltools/preview/deps/sdl/SDL2-2.28.5/lib/x64 -lSDL2 -o micmeasuretest.exe   (SDL2.dll beside it)
 */
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "micmeasure.hpp"

namespace plat {
std::string deviceName() { return "Test"; }   // (micmeasure.cpp's choice of microphone asks it; nothing here records)
}

static int passed = 0, failed = 0;
static void check(const char* what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? passed : failed)++;
}

struct Made {
    std::vector<int16_t> pcm;
    std::vector<std::pair<double, double>> stamps;
};

/** A recording from shared time `from`: each device's chirps late by lateMs (plus the microphone's own delay), at `level`. */
static Made record(double rate, double from, double t0, const std::vector<double>& lateMs, double micMs, double level, double noise, double music,
                   double echo, double jitterMs, unsigned seed, const std::vector<bool>& silent = {}) {
    const int slots = int(lateMs.size());
    const double end = micm::chirpTime(t0, slots, slots - 1, micm::kRounds - 1) + 1;
    const size_t n = size_t((end - from) * rate);
    std::vector<double> x(n, 0.0);
    std::mt19937 g(seed);
    std::normal_distribution<double> nd(0, noise);
    for (size_t i = 0; i < n; i++) {
        const double t = i / rate;
        x[i] = nd(g) + music * (std::sin(2 * 3.14159265 * 220 * t) * 0.6 + std::sin(2 * 3.14159265 * 2730 * t + 1) * 0.4);
    }
    for (int k = 0; k < slots; k++) {
        if (!silent.empty() && silent[size_t(k)]) continue;
        for (int r = 0; r < micm::kRounds; r++) {
            const double heard = micm::chirpTime(t0, slots, k, r) + (lateMs[size_t(k)] + micMs) / 1000;
            for (int e = 0; e < (echo > 0 ? 2 : 1); e++) {
                const double at = heard + e * 0.0137, amp = level * (e ? echo : 1);
                const long long s0 = (long long)std::floor((at - from) * rate);
                for (long long i = s0; i < s0 + (long long)(micm::kChirp * rate) + 2; i++) {
                    if (i < 0 || i >= (long long)n) continue;
                    x[size_t(i)] += amp * micm::chirpAt(i / rate - (at - from), k);
                }
            }
        }
    }
    Made m;
    m.pcm.resize(n);
    for (size_t i = 0; i < n; i++) m.pcm[i] = int16_t(std::lround(std::clamp(x[i], -1.0, 1.0) * 32767));
    std::uniform_real_distribution<double> jd(-jitterMs, jitterMs);
    for (size_t i = 512; i <= n; i += 512) m.stamps.push_back({ double(i), from + i / rate + micMs / 1000 * 0 + jd(g) / 1000 });
    return m;
}

static double corr(const micm::Result& r, int k) {   // against the mean of those heard, as the app sets each device
    double s = 0;
    int h = 0;
    for (size_t i = 0; i < r.heard.size(); i++) if (r.heard[i]) { s += r.lateMs[i]; h++; }
    return r.lateMs[size_t(k)] - s / h;
}

int main() {
    const double t0 = 100;
    {   // three devices, clean
        const std::vector<double> late = { 30, -12, 85 };
        const Made m = record(48000, 99.5, t0, late, 40, 0.2, 0.002, 0, 0, 0, 1);
        const micm::Result r = micm::analyze(m.pcm, m.stamps, 48000, t0, 3);
        check("clean: all three heard", r.heard[0] && r.heard[1] && r.heard[2] && r.error.empty());
        check("clean: each one's lateness within 0.5 ms (the microphone's own delay in all)", std::fabs(r.lateMs[0] - 70) < 0.5 && std::fabs(r.lateMs[1] - 28) < 0.5 && std::fabs(r.lateMs[2] - 125) < 0.5);
        check("clean: the corrections are the devices' own differences", std::fabs(corr(r, 0) - (30 - 34.333)) < 0.5 && std::fabs(corr(r, 2) - (85 - 34.333)) < 0.5);
    }
    {   // a real room: noise, music under it, an echo, a microphone whose blocks come unevenly, 44.1 kHz
        const std::vector<double> late = { 0, 210, -150 };
        const Made m = record(44100, 99.5, t0, late, 25, 0.05, 0.01, 0.05, 0.4, 1.5, 2);
        const micm::Result r = micm::analyze(m.pcm, m.stamps, 44100, t0, 3);
        check("room: all three heard", r.heard[0] && r.heard[1] && r.heard[2]);
        check("room: Bluetooth-sized delays found within 1 ms", std::fabs(corr(r, 1) - corr(r, 0) - 210) < 1 && std::fabs(corr(r, 2) - corr(r, 0) + 150) < 1);
    }
    {   // a phone listening (its microphone 120 ms late) and a Bluetooth speaker 250 ms late: still inside the window
        const std::vector<double> late = { 0, 250, 20 };
        const Made m = record(48000, 99.5, t0, late, 120, 0.1, 0.005, 0.03, 0.3, 1, 5);
        const micm::Result r = micm::analyze(m.pcm, m.stamps, 48000, t0, 3);
        check("phone and Bluetooth: all heard, 250 ms found within 1 ms", r.heard[0] && r.heard[1] && r.heard[2] && std::fabs(corr(r, 1) - corr(r, 0) - 250) < 1);
    }
    {   // the phone through a Bluetooth speaker, 520 ms behind the Switch, and loud (it is next to the microphone): found, and not taken
        // for the next device's (2026-11-05: it was, at 0.5 s apart)
        const std::vector<double> late = { 0, 520, 10 };
        const Made m = record(48000, 99.5, t0, late, 100, 0.08, 0.004, 0.02, 0.2, 1, 6);
        const micm::Result r = micm::analyze(m.pcm, m.stamps, 48000, t0, 3);
        check("Bluetooth 520 ms behind: all heard, found within 1 ms", r.heard[0] && r.heard[1] && r.heard[2] && std::fabs(corr(r, 1) - corr(r, 0) - 520) < 1 &&
                                                                     std::fabs(corr(r, 2) - corr(r, 0) - 10) < 1);
    }
    {   // one device not playing (its sound off): that one not heard, the others are
        const std::vector<double> late = { 10, 20, 30 };
        const Made m = record(48000, 99.5, t0, late, 30, 0.2, 0.005, 0.02, 0, 0, 3, { false, true, false });
        const micm::Result r = micm::analyze(m.pcm, m.stamps, 48000, t0, 3);
        check("one silent: it is not heard, the other two are", r.heard[0] && !r.heard[1] && r.heard[2]);
    }
    {   // nothing but noise: nothing heard, and said
        const std::vector<double> late = { 0, 0 };
        const Made m = record(48000, 99.5, t0, late, 30, 0.0, 0.05, 0.05, 0, 0, 4);
        const micm::Result r = micm::analyze(m.pcm, m.stamps, 48000, t0, 2);
        check("only noise: none heard, and an error to show", !r.heard[0] && !r.heard[1] && !r.error.empty());
    }
    {   // a recording too short to say anything
        micm::Result r = micm::analyze({}, {}, 48000, t0, 2);
        check("no recording: an error, nothing heard", !r.error.empty() && !r.heard[0]);
    }
    printf("%d passed, %d failed: %s\n", passed, failed, failed ? "SOME FAILED" : "ALL GOOD");
    return failed ? 1 : 0;
}
