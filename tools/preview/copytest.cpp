/* The decisions of copying music (source/filecopy.hpp), alone: names HOS can read, what to copy, what a stranger's list may hold.
 * Prints each check, then ALL GOOD or SOME FAILED. Build with -DCOPYTEST_ONLY, see tools/preview/copytest.cpp's callers. */
#include <cstdio>
#include <string>
#include <vector>

#include "filecopy.hpp"

using namespace filecopy;

static int failed = 0, passed = 0;
static void check(const char* what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? passed : failed)++;
}
static void same(const char* what, const std::string& got, const std::string& want) {
    if (got != want) printf("     got [%s]\n", got.c_str());
    check(what, got == want);
}
static const std::string H1 = "00000000000000000000000000000001", H2 = "00000000000000000000000000000002", H3 = "00000000000000000000000000000003";

int main() {
    same("the acute accent that cost hours: an apostrophe", safeName("Don\xC2\xB4t Stop.m4a", true), "Don't Stop.m4a");
    same("the curly one too", safeName("I\xE2\x80\x99m Not Okay.m4a", true), "I'm Not Okay.m4a");
    same("the grave too", safeName("Don`t.mp3", true), "Don't.mp3");
    same("U with dots", safeName("Where Are \xC3\x9C Now.flac", true), "Where Are U Now.flac");
    same("O with a stroke", safeName("Cold Water (feat. Justin Bieber & M\xC3\x98).flac", true), "Cold Water (feat. Justin Bieber & MO).flac");
    same("Spanish", safeName("Ni\xC3\xB1o - Canci\xC3\xB3n.mp3", true), "Nino - Cancion.mp3");
    same("German", safeName("Stra\xC3\x9F" "e.mp3", true), "Strasse.mp3");
    same("what the library already uses stays", safeName("Dj's #1 Mix! (100%) [Live] $5 + more, part.2.m4a", true), "Dj's #1 Mix! (100%) [Live] $5 + more, part.2.m4a");
    same("other alphabets are dropped", safeName("\xE6\x9D\xB1\xE4\xBA\xAC Tokyo.mp3", true), "Tokyo.mp3");
    same("nothing left: no name", safeName("\xE6\x9D\xB1\xE4\xBA\xAC.mp3", true), "");
    same("a long dash", safeName("A \xE2\x80\x93 B.mp3", true), "A - B.mp3");
    same("forbidden characters go", safeName("a:b*c?d\"e<f>g|h.mp3", true), "abcdefgh.mp3");
    same("doubled and end spaces go", safeName("  A   B  .mp3", true), "A B.mp3");
    same("a dot before the extension", safeName("Mr..mp3", true), "Mr.mp3");
    same("folders never come with it", safeName("../../config/settings.m4a", true), "settings.m4a");
    same("Windows paths neither", safeName("C:\\Windows\\evil.m4a", true), "evil.m4a");
    const std::string longName(300, 'a');
    const std::string cut = safeName(longName + ".flac", true);
    check("cut to 122 bytes, its extension kept", cut.size() == 122 && cut.substr(cut.size() - 5) == ".flac");
    same("the PC keeps accents", safeName("Where Are \xC3\x9C Now.flac", false), "Where Are \xC3\x9C Now.flac");
    same("the PC drops what Windows forbids", safeName("a:b?.mp3", false), "ab.mp3");
    same("no extension: no name", safeName("noextension", true), "");

    same("the top of the music folder", safeFolders("Song.m4a", true), "");
    same("a folder", safeFolders("TheBeast's 19/Song.m4a", true), "TheBeast's 19");
    same("folders inside folders", safeFolders("A/B/C/Song.m4a", true), "A/B/C");
    same("a folder's lookalikes and accents too", safeFolders("Caf\xC3\xA9\xC2\xB4s/Song.m4a", true), "Cafe's");
    same("climbing out is dropped", safeFolders("../../config/Song.m4a", true), "config");
    same("all of it climbing: the top", safeFolders("../../Song.m4a", true), "");
    same("backslashes count as folders", safeFolders("..\\..\\x\\Song.m4a", true), "x");
    same("a name of nothing but dots is dropped", safeFolders("...../a/Song.m4a", true), "a");
    same("forbidden characters go", safeFolders("a:b/Song.m4a", true), "ab");
    same("a folder of another alphabet is dropped", safeFolders("\xE6\x9D\xB1\xE4\xBA\xAC/Song.m4a", true), "");
    same("the PC keeps accents", safeFolders("Caf\xC3\xA9/Song.m4a", false), "Caf\xC3\xA9");
    same("the PC still refuses climbing", safeFolders("../Song.m4a", false), "");
    check("no deeper than 8", safeFolders("1/2/3/4/5/6/7/8/9/10/Song.m4a", true) == "1/2/3/4/5/6/7/8");
    same("a free name stays", freeName("A.mp3", { "b.mp3" }), "A.mp3");
    same("taken: numbered", freeName("A.mp3", { "a.mp3" }), "A (2).mp3");
    same("taken twice", freeName("A.mp3", { "a.mp3", "a (2).mp3" }), "A (3).mp3");

    std::vector<Item> mine = { { H1, "song1", "a", 1, 200 } };
    auto offer = [](const std::string& e, const std::string& s, const char* n, double d) { Offer o; o.exact = e; o.song = s; o.name = n; o.size = 100; o.duration = d; return o; };
    check("a song not here: copied", toCopy({ offer(H2, "song2", "b.m4a", 200) }, mine).size() == 1);
    check("the same file: not copied", toCopy({ offer(H1, "song1", "x.m4a", 200) }, mine).empty());
    check("the same song, another format: not copied", toCopy({ offer(H2, "song1", "b.flac", 200.5) }, mine).empty());
    check("the same tags, 43 s longer: another song, copied", toCopy({ offer(H2, "song1", "b.flac", 243) }, mine).size() == 1);
    check("no tags: not copied, nothing says what it is", toCopy({ offer(H2, "", "b.m4a", 200) }, mine).empty());
    check("unsupported formats: not copied", toCopy({ offer(H2, "s", "b.wav", 200), offer(H3, "t", "c.exe", 200) }, mine).empty());
    check("one file of each song", toCopy({ offer(H2, "s", "a.m4a", 200), offer(H3, "s", "b.flac", 200) }, mine).size() == 1);

    Offer o;
    check("a good offer reads back", unpackOffer(packOffer(offer(H2, H3, "b.m4a", 200)), o) && o.exact == H2 && o.name == "b.m4a" && o.size == 100);
    check("a false hash is refused", !unpackOffer("../../x\t" + H3 + "\t100\t1\tb.m4a", o));
    check("a size of nothing is refused", !unpackOffer(H1 + "\t" + H2 + "\t0\t1\tb.m4a", o));
    check("a huge size is refused", !unpackOffer(H1 + "\t" + H2 + "\t99999999999\t1\tb.m4a", o));
    check("a cut line is refused", !unpackOffer(H1 + "\t" + H2, o));

    printf("%d passed, %d failed: %s\n", passed, failed, failed ? "SOME FAILED" : "ALL GOOD");
    return failed ? 1 : 0;
}
