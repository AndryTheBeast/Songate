/* Songate · moodtest.cpp: Mood playlists (source/mood.hpp) on a real library's measurements (a mood.tsv: path, then
 * mixan::Mood::pack), with your choices and Last.fm's tags made up here. Prints each check, then ALL GOOD or SOME FAILED.
 *   clang++ -std=c++20 -Isource tools/preview/moodtest.cpp source/mood.cpp source/mixan.cpp ... (see the build line in NEXT_SESSION)
 *   moodtest <mood.tsv>
 */
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "mood.hpp"

static int passed = 0, failed = 0;
static void check(const char* what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? passed : failed)++;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("moodtest <mood.tsv>\n"); return 2; }
    std::ifstream in(argv[1]);
    std::vector<std::string> paths;
    std::vector<mixan::Mood> ms;
    for (std::string l; std::getline(in, l);) {
        const size_t t = l.find('\t');
        if (t == std::string::npos) continue;
        mixan::Mood m;
        if (!mixan::Mood::unpack(l.substr(t + 1), m) || !m.valid) continue;
        paths.push_back(l.substr(0, t));
        ms.push_back(m);
    }
    const size_t n = ms.size();
    std::vector<const mixan::Mood*> per(n);
    for (size_t i = 0; i < n; i++) per[i] = &ms[i];
    printf("%zu tracks\n", n);

    mood::Index plain;
    plain.build(per);
    int counts[mood::Count] = {};
    for (size_t i = 0; i < n; i++) if (plain.best[i] >= 0) counts[int(plain.best[i])]++;
    bool spread = true;
    for (int c = 0; c < mood::Count; c++) {
        printf("  %-10s %4d  %4.1f%%\n", mood::name(c), counts[c], 100.0 * counts[c] / n);
        if (counts[c] < int(n) / 25 || counts[c] > int(n) / 4) spread = false;
    }
    check("nine moods, none under 4 % or over 25 % of the library", spread);
    bool same = true;
    for (size_t i = 0; i < n; i++) if (plain.best[i] != plain.sound[i]) same = false;
    check("with no choices and no tags, each song's mood is its sound's", same);

    // your choices: half of one folder's songs set to Focus; the other half should move towards it
    std::vector<size_t> lofi;
    for (size_t i = 0; i < n; i++) if (paths[i].find("/LO-FI/") != std::string::npos) lofi.push_back(i);
    if (lofi.size() >= 20) {
        std::vector<mood::Label> labels(n);
        for (size_t j = 0; j < lofi.size(); j += 2) labels[lofi[j]].is = mood::Focus;
        mood::Index learnt;
        learnt.build(per, &labels);
        bool own = true;
        for (size_t j = 0; j < lofi.size(); j += 2) if (learnt.best[lofi[j]] != mood::Focus) own = false;
        check("songs you set to Focus are in Focus", own);
        int before = 0, after = 0, others = 0;
        for (size_t j = 1; j < lofi.size(); j += 2) { before += plain.best[lofi[j]] == mood::Focus; after += learnt.best[lofi[j]] == mood::Focus; others++; }
        printf("  the folder's other half in Focus: %d of %d before, %d after\n", before, others, after);
        check("songs that sound like them follow (the other half of the folder: more in Focus than before)", after > before);
        int moved = 0;
        for (size_t i = 0; i < n; i++) if (learnt.best[i] != plain.best[i]) moved++;
        printf("  songs whose mood changed: %d of %zu\n", moved, n);
        check("and the rest of the library mostly stays (under a fifth moves)", moved < int(n) / 5);
        // "not": a song said not to be in its mood leaves it
        std::vector<mood::Label> nots(n);
        const size_t s = lofi[0];
        nots[s].nots = uint16_t(1u << int(plain.best[s]));
        mood::Index without;
        without.build(per, &nots);
        check("a song you said is not in its mood leaves it", without.best[s] != plain.best[s] && without.best[s] >= 0);
    } else {
        printf("  (no LO-FI folder with 20 songs: the choices are not tried)\n");
    }

    printf("%d passed, %d failed: %s\n", passed, failed, failed ? "SOME FAILED" : "ALL GOOD");
    return failed ? 1 : 0;
}
