/* Songate · printpair.cpp: two files of one song, printed and fitted as listening together does (together.hpp), on the PC.
 *   printpair <theirs> <mine> [extra]
 * Both decoded from their start as the player decodes them (48 kHz, stereo). `extra` (s) starts mine's window that much earlier still:
 * the control, a shift made on purpose, which the fit must find on top of the files' own. Built by tools/preview/printpair.sh. */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ffdecoder.hpp"
#include "together.hpp"

using namespace together;

// ffdecoder.cpp's tag reader wants it (library.cpp); no tags are read here
std::string cleanGenre(const std::string& g) { return g; }

static bool take(const std::string& path, double from, double seconds, std::vector<uint8_t>& out, double& duration) {
    FfDecoder d;
    if (!d.open(path, 48000, 2)) return false;
    if (from > 0 && !d.skip(from, true)) return false;
    std::vector<int16_t> pcm(size_t(seconds * 48000 + 0.5) * 2);
    const int got = d.read(pcm.data(), int(pcm.size() / 2));
    out = loudness(pcm.data(), size_t(got), 48000, 2);
    (void)duration;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "printpair <theirs> <mine> <duration> [extra]\n"); return 2; }
    const double duration = atof(argv[3]), extra = argc > 4 ? atof(argv[4]) : 0;
    const double T = printFrom(duration);
    if (T < 0) { printf("too short\n"); return 1; }
    std::vector<uint8_t> a, b;
    double d0 = 0;
    if (!take(argv[1], T, kPrintSeconds, a, d0) || !take(argv[2], T - kPrintReach - extra, kPrintSeconds + 2 * kPrintReach, b, d0)) { printf("unreadable\n"); return 1; }
    if (a.size() < size_t(kPrintSteps) || b.size() < size_t(kPrintSteps + 2 * kPrintReachSteps)) { printf("short read\n"); return 1; }
    a.resize(kPrintSteps);
    b.resize(size_t(kPrintSteps + 2 * kPrintReachSteps));
    const Aligned r = align(a, b);
    printf("from %.2f s: %s, shift %+.2f ms, fit %.2f, next best %.2f\n", T, r.clear ? "clear" : "unclear", r.shift * 1e3, r.score, r.next);
    return 0;
}
