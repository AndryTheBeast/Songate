/* Connections' record book, tested alone (source/ledger.hpp): a sync run twice, stopped halfway, and after both sides changed.
 * Prints each check, then ALL GOOD or SOME FAILED. Build with source/ledger.cpp and source/monocypher.c. */
#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "ledger.hpp"

using namespace ledgerkey;

static int failed = 0, passed = 0;
static void check(const char* what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? passed : failed)++;
}

/** One whole sync; returns how many records changed anywhere. */
static size_t sync(Ledger& a, Ledger& b, const char* na = "Switch", const char* nb = "PC") {
    const auto ha = a.have(), hb = b.have();
    const auto ta = a.since(hb), tb = b.since(ha);
    const size_t x = b.merge(ta, ha, na, nb).size();
    const size_t y = a.merge(tb, hb, nb, na).size();
    return x + y;
}
static bool sameAs(const Ledger& a, const Ledger& b) {
    if (a.size() != b.size()) return false;
    for (const auto& kv : a.entries()) {
        const Ledger::Entry* e = b.find(kv.first);
        if (!e || e->value != kv.second.value) return false;
    }
    return true;
}
static std::string val(const Ledger& l, const std::string& k) {
    const Ledger::Entry* e = l.find(k);
    return e ? e->value : "(none)";
}
static int plays(const Ledger& l, const std::string& song, std::vector<std::string> devices) {
    int n = 0;
    for (const auto& d : devices) n += atoi(val(l, ledgerkey::plays(song, d)).c_str());
    return n;
}
static size_t lists(const Ledger& l) {
    size_t n = 0;
    for (const auto& kv : l.entries()) n += kv.first[0] == 'P';
    return n;
}

int main() {
    {   // Liked
        Ledger a("aaa"), b("bbb");
        a.put(liked("s1"), "1");
        b.put(liked("s2"), "1");
        sync(a, b);
        check("first sync: each gets the other's Liked", val(a, liked("s2")) == "1" && val(b, liked("s1")) == "1");
        check("both hold the same", sameAs(a, b));
        const std::string before = a.serialise();
        check("run twice: nothing changes", sync(a, b) == 0 && a.serialise() == before);
        a.put(liked("s1"), "");
        sync(a, b);
        check("an unlike reaches the other device", val(b, liked("s1")) == "");
        sync(a, b);
        check("and is not brought back by the next sync", val(a, liked("s1")) == "" && val(b, liked("s1")) == "");
        b.put(liked("s1"), "1");
        sync(a, b);
        check("liked again later: the later one wins", val(a, liked("s1")) == "1");
    }
    {   // plays
        Ledger a("aaa"), b("bbb");
        a.put(ledgerkey::plays("s", "aaa"), "5");
        b.put(ledgerkey::plays("s", "bbb"), "3");
        sync(a, b);
        check("plays: 5 and 3 show 8 on both", plays(a, "s", { "aaa", "bbb" }) == 8 && plays(b, "s", { "aaa", "bbb" }) == 8);
        for (int i = 0; i < 5; i++) sync(a, b);
        check("five more syncs: still 8, nothing doubled", plays(a, "s", { "aaa", "bbb" }) == 8 && plays(b, "s", { "aaa", "bbb" }) == 8);
        a.put(ledgerkey::plays("s", "aaa"), "6");
        b.put(ledgerkey::plays("s", "bbb"), "4");
        sync(a, b);
        check("both played it more: 6 and 4 show 10", plays(a, "s", { "aaa", "bbb" }) == 10 && plays(b, "s", { "aaa", "bbb" }) == 10);
    }
    {   // edits, both sides
        Ledger a("aaa"), b("bbb");
        a.put(edit("s", "title"), "From A");
        sync(a, b);
        a.put(edit("s", "title"), "A again");
        b.put(edit("s", "title"), "B's");
        sync(a, b);
        check("the same tag edited on both: one answer on both", val(a, edit("s", "title")) == val(b, edit("s", "title")));
        a.put(edit("s", "album"), "Album A");
        b.put(edit("s", "artist"), "Artist B");
        sync(a, b);
        check("different tags of one song: both kept", val(a, edit("s", "artist")) == "Artist B" && val(b, edit("s", "album")) == "Album A");
        a.put(edit("s", "album"), "");
        sync(a, b);
        check("an edit undone reaches the other device", val(b, edit("s", "album")) == "");
    }
    {   // a sync stopped halfway
        Ledger a("aaa"), b("bbb");
        for (int i = 0; i < 1000; i++) a.put(liked("s" + std::to_string(i)), "1");
        const auto ha = a.have(), hb = b.have();
        auto all = a.since(hb);
        std::vector<Ledger::Entry> half(all.begin(), all.begin() + 400);
        b.merge(half, ha, "A", "B");
        check("stopped at 400 of 1000: 400 held", b.size() == 400);
        const auto again = a.since(b.have());
        check("the next sync sends only the other 600", again.size() == 600);
        sync(a, b);
        check("and then both hold all 1000", b.size() == 1000 && sameAs(a, b));
    }
    {   // a sync stopped halfway, Ledger written and read back in the middle
        Ledger a("aaa"), b("bbb"), c("bbb");
        for (int i = 0; i < 100; i++) a.put(liked("s" + std::to_string(i)), "1");
        auto all = a.since(b.have());
        b.merge({ all.begin(), all.begin() + 30 }, a.have(), "A", "B");
        c.load(b.serialise());
        sync(a, c);
        check("restarted between: still all there, once", c.size() == 100 && sameAs(a, c));
    }
    {   // playlists
        Ledger a("aaa"), b("bbb");
        a.put(list("id1"), Ledger::packList("Road", { "x", "y" }));
        sync(a, b);
        std::string name;
        std::vector<std::string> songs;
        Ledger::unpackList(val(b, list("id1")), name, songs);
        check("a playlist arrives whole", name == "Road" && songs == std::vector<std::string>({ "x", "y" }));
        a.put(list("id1"), Ledger::packList("Road", { "x", "y", "z" }));
        sync(a, b);
        Ledger::unpackList(val(b, list("id1")), name, songs);
        check("changed on one side: the change arrives, no copy", songs.size() == 3 && lists(b) == 1);
        a.put(list("id1"), Ledger::packList("Road", { "x", "A" }));
        b.put(list("id1"), Ledger::packList("Road", { "x", "B" }));
        sync(a, b, "Switch", "PC");
        check("changed on both: both kept, on both devices", lists(a) == 2 && lists(b) == 2);
        check("and the same on both", sameAs(a, b));
        bool named = false;
        for (const auto& kv : a.entries()) {
            if (kv.first[0] != 'P') continue;
            Ledger::unpackList(kv.second.value, name, songs);
            named = named || name == "Road (PC)" || name == "Road (Switch)";
        }
        check("the older one named after its device", named);
        const size_t n = a.size();
        sync(a, b);
        sync(a, b);
        check("run twice after: no more copies", a.size() == n && b.size() == n && sameAs(a, b));
    }
    {   // the order things arrive in
        std::mt19937 rng(7);
        Ledger a("aaa"), b("bbb");
        std::vector<Ledger::Entry> es;
        Ledger w("www");
        for (int i = 0; i < 300; i++) w.put(liked("s" + std::to_string(rng() % 50)), i % 3 ? "1" : "");
        auto all = w.since({});
        a.merge(all, w.have(), "W", "A");
        std::shuffle(all.begin(), all.end(), rng);
        b.merge(all, w.have(), "W", "B");
        check("any order of arrival: the same result", sameAs(a, b));
    }
    {   // three devices
        Ledger a("aaa"), b("bbb"), c("ccc");
        a.put(liked("s"), "1");
        sync(a, b);
        sync(b, c);
        check("A's change reaches C through B", val(c, liked("s")) == "1");
        c.put(liked("s"), "");
        sync(c, a);
        sync(a, b);
        check("C's change reaches B through A", val(b, liked("s")) == "");
    }
    {   // files
        Ledger a("aaa");
        a.put(edit("s", "title"), "tab\there, line\nbreak, back\\slash, Ü ’ ´");
        a.put(liked("q"), "1");
        Ledger b("aaa");
        b.load(a.serialise());
        check("written and read back, odd characters whole", sameAs(a, b) && b.have() == a.have());
        b.put(liked("new"), "1");
        const Ledger::Entry* e = b.find(liked("new"));
        check("numbers carry on after a restart", e && e->seq == 3 && e->clock == 3);
        Ledger c("zzz");
        c.load("junk\ne\tnot\tenough\nclock\t5\n");
        check("a damaged file reads what it can", c.size() == 0);
    }
    printf("%d passed, %d failed: %s\n", passed, failed, failed ? "SOME FAILED" : "ALL GOOD");
    return failed ? 1 : 0;
}
