/* Songate · tagwritetest.cpp: tags and covers written into songs (source/tagwrite.hpp), on copies of real ones, MP3, M4A and FLAC
 * (the user, 2026-11-07: "test tag edit in all three formats"). For each song given: every field and the cover changed and read back;
 * one field alone, the rest as they were; the cover taken out; then everything put back from what each write handed back, and the
 * song read back as it was at first, its audio the same packet by packet throughout. Prints each check, then ALL GOOD or SOME FAILED.
 *   built by tools/preview/build-tagwritetest.sh; run on copies only (it writes into the files it is given):
 *   tagwritetest.exe cover.jpg cover.png song.mp3 song.m4a song.flac
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../source/metadata.hpp"
#include "../../source/tagwrite.hpp"

static int passed = 0, failed = 0;
static void check(const std::string& what, bool ok, const std::string& why = "") {
    printf("%s %s%s\n", ok ? "ok  " : "FAIL", what.c_str(), ok || why.empty() ? "" : (" (" + why + ")").c_str());
    (ok ? passed : failed)++;
}

static std::vector<uint8_t> readAll(const char* p) {
    std::vector<uint8_t> b;
    FILE* f = fopen(p, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    b.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    fclose(f);
    return b;
}
static bool copyFile(const std::string& a, const std::string& b) {
    const std::vector<uint8_t> d = readAll(a.c_str());
    FILE* f = fopen(b.c_str(), "wb");
    if (!f || d.empty()) { if (f) fclose(f); return false; }
    const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    return fclose(f) == 0 && ok;
}
static std::string field(const TrackInfo& t, const std::string& f) {
    if (f == "title") return t.title;
    if (f == "artist") return t.artist;
    if (f == "album") return t.album;
    if (f == "albumArtist") return t.albumArtist;
    if (f == "genre") return t.genre;
    if (f == "date") return t.date;
    if (f == "trackNo") return t.trackNo ? std::to_string(t.trackNo) : "";
    return t.lyrics;
}
static const char* kFields[] = { "title", "artist", "album", "albumArtist", "genre", "date", "trackNo", "lyrics" };

int main(int argc, char** argv) {
    if (argc < 4) { printf("tagwritetest cover.jpg cover.png song...\n"); return 2; }
    metadataInit();
    const std::vector<uint8_t> jpg = readAll(argv[1]), png = readAll(argv[2]);
    check("the test covers read", !jpg.empty() && !png.empty());
    for (int a = 3; a < argc; a++) {
        const std::string song = argv[a], orig = song + ".orig";
        const std::string name = song.substr(song.find_last_of("/\\") + 1);
        if (!copyFile(song, orig)) { check(name + ": a copy to compare with", false); continue; }
        TrackInfo at0;
        readTrackInfo(song, at0, true, true);
        std::string err;

        // 1. every field and the cover (a JPEG), with accents and a long lyric
        tagwrite::Changes all;
        all.text = { { "title", "Título Ñ — test" }, { "artist", "Artïst Ünö" }, { "album", "Albüm 1" }, { "albumArtist", "Album Artist" },
                     { "genre", "Synthwave" }, { "date", "2021" }, { "trackNo", "7" }, { "lyrics", "[00:01.00]First line\n[00:05.50]Second líne" } };
        all.cover = true;
        all.coverBytes = jpg;
        tagwrite::Before b1;
        const bool w1 = tagwrite::putChanges(song, all, &b1, err);
        check(name + ": every field and the cover written", w1, err);
        TrackInfo t1;
        readTrackInfo(song, t1, true, true);
        for (const char* f : kFields) {
            std::string want = all.text[f], got = field(t1, f);
            while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
            check(name + ": " + f + " reads back", got == want, "got \"" + got + "\"");
        }
        check(name + ": the cover reads back", t1.cover.size() == jpg.size(), std::to_string(t1.cover.size()) + " bytes");
        for (const char* f : kFields) check(name + ": " + f + " as it was, handed back", b1.text[f] == field(at0, f) || (std::string(f) == "lyrics"), "\"" + b1.text[f] + "\" / \"" + field(at0, f) + "\"");
        check(name + ": the old cover handed back", b1.cover && b1.coverBytes.size() == at0.cover.size(), std::to_string(b1.coverBytes.size()) + " / " + std::to_string(at0.cover.size()));
        err.clear();
        check(name + ": its audio the same after", tagwrite::sameAudio(orig, song, err), err);

        // 2. one field alone (the title), the cover a PNG: the rest as written before
        tagwrite::Changes one;
        one.text = { { "title", "Only the title" } };
        one.cover = true;
        one.coverBytes = png;
        tagwrite::Before b2;
        err.clear();
        check(name + ": one field and a PNG cover written", tagwrite::putChanges(song, one, &b2, err), err);
        TrackInfo t2;
        readTrackInfo(song, t2, true, true);
        check(name + ": the title changed", t2.title == "Only the title", t2.title);
        check(name + ": the artist as it was", t2.artist == "Artïst Ünö", t2.artist);
        check(name + ": the track number as it was", t2.trackNo == 7, std::to_string(t2.trackNo));
        check(name + ": the PNG cover reads back", t2.cover.size() == png.size(), std::to_string(t2.cover.size()));
        check(name + ": the title before handed back", b2.text["title"] == "Título Ñ — test", b2.text["title"]);

        // 3. the cover taken out, the genre emptied
        tagwrite::Changes out;
        out.text = { { "genre", "" } };
        out.cover = true;
        tagwrite::Before b3;
        err.clear();
        check(name + ": the cover and genre taken out", tagwrite::putChanges(song, out, &b3, err), err);
        TrackInfo t3;
        readTrackInfo(song, t3, true, false);
        check(name + ": no genre", t3.genre.empty(), t3.genre);
        check(name + ": no cover (or none of the front)", t3.cover.size() != png.size(), std::to_string(t3.cover.size()));

        // 4. everything back as it was at first: each write's Before, newest first
        bool back = true;
        for (tagwrite::Before* b : { &b3, &b2, &b1 }) {
            tagwrite::Changes undo;
            undo.text = b->text;
            undo.cover = b->cover;
            undo.coverBytes = b->coverBytes;
            err.clear();
            if (!tagwrite::putChanges(song, undo, nullptr, err)) { back = false; check(name + ": an undo written", false, err); }
        }
        check(name + ": every undo written", back);
        TrackInfo t4;
        readTrackInfo(song, t4, true, true);
        for (const char* f : kFields) {
            std::string a0 = field(at0, f), a4 = field(t4, f);
            while (!a0.empty() && (a0.back() == '\n' || a0.back() == '\r')) a0.pop_back();
            while (!a4.empty() && (a4.back() == '\n' || a4.back() == '\r')) a4.pop_back();
            check(name + ": " + f + " back as at first", a0 == a4, "\"" + a4 + "\" / \"" + a0 + "\"");
        }
        check(name + ": the cover back as at first", t4.cover == at0.cover, std::to_string(t4.cover.size()) + " / " + std::to_string(at0.cover.size()));
        err.clear();
        check(name + ": its audio the same at the end", tagwrite::sameAudio(orig, song, err), err);
        remove(orig.c_str());
    }
    printf("%d passed, %d failed: %s\n", passed, failed, failed ? "SOME FAILED" : "ALL GOOD");
    return failed ? 1 : 0;
}
