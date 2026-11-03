/* Songate · seekvsskip.cpp: one file, one moment, reached two ways as the player reaches it: FfDecoder::seek (a normal start and every
 * seek) and FfDecoder::skip(exact) (a rejoin, decoded from the start, sample by sample). Prints how far apart the two are, in samples
 * and ms, by cross-correlating 2 s of each. 0 means a rejoin lands where a start does.
 *   seekvsskip <file> <seconds> [<seconds> ...]
 * Built by tools/preview/build-seekvsskip.sh. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ffdecoder.hpp"

// ffdecoder.cpp's tag reader wants it (library.cpp); no tags are read here
std::string cleanGenre(const std::string& g) { return g; }

static std::vector<float> take(const std::string& path, double at, bool exact, double seconds) {
    FfDecoder d;
    std::vector<float> out;
    if (!d.open(path, 48000, 2)) return out;
    if (exact) { if (!d.skip(at, true)) return out; }
    else if (!d.seek(at)) return out;
    std::vector<int16_t> pcm(size_t(seconds * 48000) * 2);
    const int got = d.read(pcm.data(), int(pcm.size() / 2));
    for (int i = 0; i < got; i++) out.push_back((pcm[size_t(2 * i)] + pcm[size_t(2 * i + 1)]) / 65536.f);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "seekvsskip <file> <seconds>...\n"); return 2; }
    for (int k = 2; k < argc; k++) {
        const double at = atof(argv[k]);
        // SVS_SHIFT=<s>: the exact way aimed that much later: the control, whose answer is known (-shift x 48000 samples)
        const double shift = getenv("SVS_SHIFT") ? atof(getenv("SVS_SHIFT")) : 0;
        const std::vector<float> a = take(argv[1], at, false, 2.5), b = take(argv[1], at + shift, true, 2.5);
        if (a.size() < 96000 || b.size() < 96000) { printf("%7.2f s: unreadable\n", at); continue; }
        // b (exact) against a (seek) within +-100 ms: the lag where they agree
        const int R = 4800, N = 96000 - 2 * R;
        int best = 0;
        double bestR = -2;
        for (int L = -R; L <= R; L++) {
            double sab = 0, saa = 0, sbb = 0;
            for (int i = R; i < R + N; i += 2) {
                const double x = a[size_t(i)], y = b[size_t(i + L)];
                sab += x * y; saa += x * x; sbb += y * y;
            }
            const double r = saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0;
            if (r > bestR) { bestR = r; best = L; }
        }
        printf("%7.2f s: the exact way is %+d samples (%+.2f ms) from the seek's: the same sound sits that much later in it (fit %.3f)\n", at, best, best / 48.0, bestR);
    }
    return 0;
}
