/* Two made-up libraries through source/syncdata.hpp: what a sync really does to Liked, edits, playlists and plays.
 * Prints each check, then ALL GOOD or SOME FAILED. Build with source/syncdata.cpp, source/ledger.cpp and source/monocypher.c. */
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "syncdata.hpp"

using namespace syncdata;

static int failed = 0, passed = 0;
static void check(const char* what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? passed : failed)++;
}

/** One device: its library, its book, and how it applies changes as the app does. */
struct Device {
    Snapshot now;
    Ledger book;
    ListIds ids;
    Seen seen;
    std::string name;
    unsigned kinds = KindAll;
    bool addOnly = false;   // the first sync after an older Songate ran here
    Device(const char* id, const char* n) : book(id), name(n) {}
    void song(const char* path, const char* sid, double d = 200) { now.songs.push_back({ path, sid, "", d }); }
    bool isLiked(const std::string& p) const { return std::find(now.liked.begin(), now.liked.end(), p) != now.liked.end(); }
    const List* list(const std::string& n) const { for (const List& l : now.lists) if (l.name == n) return &l; return nullptr; }
    int shown(const std::string& p) const {
        auto a = now.plays.find(p);
        auto b = now.others.find(p);
        return (a == now.plays.end() ? 0 : a->second.plays) + (b == now.others.end() ? 0 : b->second);
    }
    void apply(const Changes& c) {
        for (const auto& p : c.likedAdd) now.liked.push_back(p);
        for (const auto& p : c.likedRemove) now.liked.erase(std::remove(now.liked.begin(), now.liked.end(), p), now.liked.end());
        for (const auto& e : c.edits) {
            if (e.value.empty()) { now.edits[e.path].erase(e.field); if (now.edits[e.path].empty()) now.edits.erase(e.path); }
            else now.edits[e.path][e.field] = e.value;
        }
        for (const auto& n : c.listsDelete) now.lists.erase(std::remove_if(now.lists.begin(), now.lists.end(), [&](const List& l) { return l.name == n; }), now.lists.end());
        for (const auto& l : c.listsNew) now.lists.push_back(l);
        for (const auto& r : c.listsRename) for (List& m : now.lists) if (m.name == r.from) m.name = r.to;
        for (const auto& l : c.listsSet) for (List& m : now.lists) if (m.name == l.name) m = l;
        if (!c.listOrder.empty()) {
            std::vector<List> sorted;
            for (const auto& n : c.listOrder) for (const List& m : now.lists) if (m.name == n) sorted.push_back(m);
            now.lists = sorted;
        }
        for (const auto& p : c.filesDelete) {
            now.songs.erase(std::remove_if(now.songs.begin(), now.songs.end(), [&](const Song& s) { return s.path == p; }), now.songs.end());
            now.liked.erase(std::remove(now.liked.begin(), now.liked.end(), p), now.liked.end());
        }
        for (const auto& p : c.plays) {
            now.others[p.first] = p.second.plays;
            now.plays[p.first].last = p.second.last;
            now.plays[p.first].added = p.second.added;
        }
    }
};

/** A whole sync, as the app runs it; how many changes it made on the two libraries. */
static size_t sync(Device& a, Device& b) {
    scan(a.book, a.now, a.ids, a.seen, a.kinds, a.addOnly);
    scan(b.book, b.now, b.ids, b.seen, b.kinds, b.addOnly);
    const auto ha = a.book.have(), hb = b.book.have();
    const auto ta = a.book.since(hb), tb = b.book.since(ha);
    a.book.merge(tb, hb, b.name, a.name);
    b.book.merge(ta, ha, a.name, b.name);
    const Changes ca = plan(a.book, a.now, a.ids, a.seen, a.kinds), cb = plan(b.book, b.now, b.ids, b.seen, b.kinds);
    const size_t n = ca.likedAdd.size() + ca.likedRemove.size() + ca.edits.size() + ca.listsNew.size() + ca.listsSet.size() + ca.listsRename.size() + ca.listsDelete.size() + ca.plays.size() + ca.listOrder.size() +
                     cb.likedAdd.size() + cb.likedRemove.size() + cb.edits.size() + cb.listsNew.size() + cb.listsSet.size() + cb.listsRename.size() + cb.listsDelete.size() + cb.plays.size() + cb.listOrder.size();
    a.apply(ca);
    b.apply(cb);
    a.now.gone.clear();
    b.now.gone.clear();
    return n + ca.filesDelete.size() + cb.filesDelete.size();
}

static void lib(Device& d, const char* root) {
    for (const char* s : { "s1", "s2", "s3", "s4" }) {
        const std::string p = std::string(root) + s;
        static std::vector<std::string> keep;
        keep.push_back(p);
        d.now.songs.push_back({ p, s, "", 200 });
    }
}

int main() {
    const std::string A = "sdmc:/Music/", B = "D:/Music/";
    {
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        a.now.liked = { A + "s1" };
        b.now.liked = { B + "s2" };
        sync(a, b);
        check("first sync: Liked of both on both, by song, not by path", a.isLiked(A + "s2") && b.isLiked(B + "s1") && a.isLiked(A + "s1") && b.isLiked(B + "s2"));
        check("run twice: nothing to do", sync(a, b) == 0 && sync(a, b) == 0);
        a.now.liked.erase(std::remove(a.now.liked.begin(), a.now.liked.end(), A + "s2"), a.now.liked.end());
        sync(a, b);
        check("unliked on one: gone from the other", !b.isLiked(B + "s2"));
        sync(a, b);
        check("and it stays gone", !a.isLiked(A + "s2") && !b.isLiked(B + "s2"));
    }
    {   // plays
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        a.now.plays[A + "s1"] = { 5, 1000, 500 };
        b.now.plays[B + "s1"] = { 3, 2000, 400 };
        sync(a, b);
        check("plays 5 and 3 show 8 on both", a.shown(A + "s1") == 8 && b.shown(B + "s1") == 8);
        check("last played: the latest", a.now.plays[A + "s1"].last == 2000 && b.now.plays[B + "s1"].last == 2000);
        check("date added: the earliest", a.now.plays[A + "s1"].added == 400 && b.now.plays[B + "s1"].added == 400);
        for (int i = 0; i < 5; i++) sync(a, b);
        check("five more syncs: still 8", a.shown(A + "s1") == 8 && b.shown(B + "s1") == 8);
        a.now.plays[A + "s1"].plays++;
        b.now.plays[B + "s1"].plays++;
        sync(a, b);
        check("played once more on each: 10 on both", a.shown(A + "s1") == 10 && b.shown(B + "s1") == 10);
        // stopped at the worst moment: applied here, the book never saved
        a.now.plays[A + "s2"] = { 4, 3000, 0 };
        scan(a.book, a.now, a.ids, a.seen, a.kinds);
        scan(b.book, b.now, b.ids, b.seen, b.kinds);
        Ledger saved = b.book;
        const Seen seenThen = b.seen;
        const auto ha = a.book.have(), hb = b.book.have();
        b.book.merge(a.book.since(hb), ha, a.name, b.name);
        b.apply(plan(b.book, b.now, b.ids, b.seen, b.kinds));
        b.book = saved;   // the app stopped here
        b.seen = seenThen;
        sync(a, b);
        sync(a, b);
        check("stopped between applying and saving: nothing counted twice", a.shown(A + "s2") == 4 && b.shown(B + "s2") == 4);
    }
    {   // edits, and a song keeps its id
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        a.now.edits[A + "s1"]["title"] = "Better Title";
        sync(a, b);
        check("a tag edit reaches the same song", b.now.edits[B + "s1"]["title"] == "Better Title");
        check("run twice: nothing to do", sync(a, b) == 0);
        b.now.edits[B + "s1"]["artist"] = "Better Artist";
        sync(a, b);
        check("another tag of it, from the other side: both kept", a.now.edits[A + "s1"]["artist"] == "Better Artist" && a.now.edits[A + "s1"]["title"] == "Better Title");
        a.now.edits.erase(A + "s1");
        sync(a, b);
        check("undone on one: the file's own tags on both", !b.now.edits.count(B + "s1") && !a.now.edits.count(A + "s1"));
        a.now.edits[A + "s2"]["title"] = "From A";
        b.now.edits[B + "s2"]["title"] = "From B";
        sync(a, b);
        check("the same tag on both: one answer on both", a.now.edits[A + "s2"]["title"] == b.now.edits[B + "s2"]["title"]);
    }
    {   // playlists
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        b.song("D:/Music/only", "onlyPC");
        a.now.lists.push_back({ "Road", { A + "s1", A + "s2" } });
        sync(a, b);
        check("a playlist arrives, with this device's paths", b.list("Road") && b.list("Road")->paths == std::vector<std::string>({ B + "s1", B + "s2" }));
        check("run twice: nothing to do", sync(a, b) == 0);
        b.list("Road");
        for (List& l : b.now.lists) l.paths.push_back("D:/Music/only");
        sync(a, b);
        check("a song only the PC has: the Switch shows what it can", a.list("Road")->paths.size() == 2);
        for (List& l : a.now.lists) l.paths.push_back(A + "s3");
        sync(a, b);
        check("changed on the Switch: the PC's own song is still in it", b.list("Road")->paths.size() == 4);
        for (List& l : a.now.lists) l.paths = { A + "s1" };
        for (List& l : b.now.lists) l.paths = { B + "s4" };
        sync(a, b);
        sync(a, b);
        check("changed on both: two playlists on both", a.now.lists.size() == 2 && b.now.lists.size() == 2);
        check("nothing lost: each holds both", a.list("Road (PC)") || a.list("Road (Switch)"));
        for (List& l : a.now.lists) if (l.name == "Road") l.name = "Highway";
        sync(a, b);
        check("renamed on one: renamed on the other, no copy", b.list("Highway") && !b.list("Road") && b.now.lists.size() == 2);
        b.now.lists.erase(std::remove_if(b.now.lists.begin(), b.now.lists.end(), [](const List& l) { return l.name == "Highway"; }), b.now.lists.end());
        sync(a, b);
        sync(a, b);
        check("deleted on the PC: gone from both", !b.list("Highway") && !a.list("Highway"));
        check("run twice: it does not come back", sync(a, b) == 0 && !a.list("Highway") && !b.list("Highway"));
        Device e("eee", "Switch"), f("fff", "PC");
        lib(e, A.c_str());
        lib(f, B.c_str());
        e.now.lists.push_back({ "Both", { A + "s1" } });
        sync(e, f);
        for (List& l : e.now.lists) l.paths.push_back(A + "s2");
        f.now.lists.clear();
        sync(e, f);
        sync(e, f);
        check("deleted on one, changed on the other: the change stays, on both", e.list("Both") && f.list("Both") && f.list("Both")->paths.size() == 2);
        check("run twice: settled", sync(e, f) == 0);
        Device h("ggg", "Switch"), i("hhh", "PC");
        lib(h, A.c_str());
        lib(i, B.c_str());
        h.now.lists.push_back({ "Again", { A + "s1" } });
        sync(h, i);
        h.now.lists.clear();
        sync(h, i);
        h.now.lists.push_back({ "Again", { A + "s3" } });
        sync(h, i);
        check("deleted, then made again by that name: a new one on both", i.list("Again") && i.list("Again")->paths == std::vector<std::string>({ B + "s3" }));
        Device c("ccc", "Other"), d("ddd", "PC");
        lib(c, A.c_str());
        lib(d, B.c_str());
        c.now.lists.push_back({ "Gym", { A + "s1" } });
        d.now.lists.push_back({ "Gym", { B + "s2" } });
        sync(c, d);
        check("the same name made on both: neither replaced", c.now.lists.size() == 2 && d.now.lists.size() == 2);
    }
    {   // the sidebar's order
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        auto names = [](const Device& d) { std::string s; for (const List& l : d.now.lists) s += l.name + " "; return s; };
        for (const char* n : { "One", "Two", "Three" }) a.now.lists.push_back({ n, { A + "s1" } });
        sync(a, b);
        check("playlists arrive in their order", names(b) == "One Two Three ");
        std::swap(a.now.lists[0], a.now.lists[2]);
        sync(a, b);
        check("moved on one: moved on the other", names(b) == "Three Two One ");
        check("run twice: nothing to do", sync(a, b) == 0);
        b.now.lists.push_back({ "Four", { B + "s2" } });
        sync(a, b);
        check("a new one takes its place on both", names(a) == names(b) && a.now.lists.size() == 4);
        std::swap(b.now.lists[0], b.now.lists[1]);
        sync(a, b);
        sync(a, b);
        check("moved on the other: both the same", names(a) == names(b));
        b.now.lists.erase(b.now.lists.begin());
        sync(a, b);
        sync(a, b);
        check("one deleted on the PC: gone from both, the rest in order", a.now.lists.size() == 3 && b.now.lists.size() == 3 && names(a) == names(b));
        std::swap(a.now.lists[0], a.now.lists[2]);
        std::swap(b.now.lists[0], b.now.lists[1]);
        sync(a, b);
        check("moved on both at once: no copies, one answer", a.now.lists.size() == 3 && b.now.lists.size() == 3 && names(a) == names(b));
        check("then nothing more to do", sync(a, b) == 0);
    }
    {   // what kind of playlist it is
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        List mix;
        mix.name = "Night";
        mix.paths = { A + "s1", A + "s2", A + "s3" };
        mix.automix = true;
        mix.pin = A + "s2";
        mix.icon = "moon";
        a.now.lists.push_back(mix);
        sync(a, b);
        const List* got = b.list("Night");
        check("an Automix playlist stays one, icon and first track too", got && got->automix && !got->manual && got->icon == "moon" && got->pin == B + "s2");
        check("in the same order", got && got->paths == std::vector<std::string>({ B + "s1", B + "s2", B + "s3" }));
        check("run twice: nothing to do", sync(a, b) == 0);
        for (List& l : b.now.lists) l.manual = true;
        sync(a, b);
        check("moved by hand on one: so on the other", a.list("Night")->manual);
        for (List& l : a.now.lists) l.paths = { A + "s1" };
        for (List& l : b.now.lists) l.paths = { B + "s3" };
        sync(a, b);
        sync(a, b);
        check("changed on both: both kept, both still Automix", a.now.lists.size() == 2 && (a.now.lists[1].automix && a.now.lists[0].automix));
        const List* copy = a.list("Night (PC)") ? a.list("Night (PC)") : a.list("Night (Switch)");
        check("named after its device, its kind intact", copy && copy->automix && copy->icon == "moon");
        Device c("ccc", "Switch"), d("ddd", "PC");
        lib(c, A.c_str());
        lib(d, B.c_str());
        c.now.lists.push_back({ "Plain", { A + "s1" } });
        sync(c, d);
        for (List& l : c.now.lists) l.automix = true;   // the book from before knew only the songs
        sync(c, d);
        check("made Automix later: the other follows", d.list("Plain") && d.list("Plain")->automix);
    }
    {   // deleting
        auto has = [](const Device& d, const std::string& p) { return std::any_of(d.now.songs.begin(), d.now.songs.end(), [&](const Song& s) { return s.path == p; }); };
        auto del = [](Device& d, const std::string& p, const char* id) {
            d.now.songs.erase(std::remove_if(d.now.songs.begin(), d.now.songs.end(), [&](const Song& s) { return s.path == p; }), d.now.songs.end());
            d.now.gone.push_back(id);
        };
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        sync(a, b);
        del(a, A + "s1", "s1");
        sync(a, b);
        check("deleted on one, the switch off: the other keeps it", has(b, B + "s1"));
        b.kinds = KindAll | KindDeletes;
        b.now.deleteFrom = b.book.have().empty() ? 0 : 1000000;   // the switch just turned on: nothing before now
        sync(a, b);
        check("turned on later: what was deleted before stays", has(b, B + "s1"));
        b.now.deleteFrom = 0;
        lib(a, "x");   // (a library of other paths)
        Device c("ccc", "Switch"), d("ddd", "PC");
        lib(c, A.c_str());
        lib(d, B.c_str());
        d.kinds = KindAll | KindDeletes;
        sync(c, d);
        del(c, A + "s2", "s2");
        sync(c, d);
        check("the switch on: the other deletes it too", !has(d, B + "s2") && has(d, B + "s1"));
        check("run twice: nothing more", sync(c, d) == 0);
        Device e("eee", "Switch"), f("fff", "PC");
        lib(e, A.c_str());
        lib(f, B.c_str());
        f.kinds = KindAll | KindDeletes;
        sync(e, f);
        e.now.songs.clear();   // the card taken out, or a folder moved: every song missing at once
        sync(e, f);
        sync(e, f);
        check("every song missing here deletes nothing there", f.now.songs.size() == 4);
        Device all1("all", "Switch"), all2("al2", "PC");
        lib(all1, A.c_str());
        lib(all2, B.c_str());
        all2.kinds = KindAll | KindDeletes;
        sync(all1, all2);
        for (const char* id : { "s1", "s2", "s3", "s4" }) del(all1, A + id, id);
        sync(all1, all2);
        check("every song deleted by hand: the last one goes too", all2.now.songs.empty());
        check("run twice: nothing more", sync(all1, all2) == 0);
        Device fresh("new", "New PC"), full("ful", "Switch");
        lib(full, A.c_str());
        full.kinds = fresh.kinds = KindAll | KindDeletes;
        sync(fresh, full);
        check("a device with no music yet takes nothing from the other", full.now.songs.size() == 4);
        check("and it settles", sync(fresh, full) == 0 && full.now.songs.size() == 4);
        Device g("ggg", "Switch"), h("hhh", "PC");
        lib(g, A.c_str());
        lib(h, B.c_str());
        h.kinds = KindAll | KindDeletes;
        sync(g, h);
        del(g, A + "s3", "s3");
        sync(g, h);
        g.song("back", "s3");   // copied back later, by choice
        h.song("back2", "s3");
        sync(g, h);
        sync(g, h);
        check("put back later: stays, on both", has(g, "back") && has(h, "back2"));
        Device i("iii", "Switch"), j("jjj", "PC");
        lib(i, A.c_str());
        lib(j, B.c_str());
        i.song("dup1", "same");
        j.song("dupA", "same");
        j.song("dupB", "same");
        j.kinds = KindAll | KindDeletes;
        sync(i, j);
        del(i, "dup1", "same");
        sync(i, j);
        check("every copy of it goes", !has(j, "dupA") && !has(j, "dupB"));
        Device k("kkk", "Switch"), l("lll", "PC");
        k.song("two1", "both");
        k.song("two2", "both");
        l.song("t1", "both");
        l.kinds = KindAll | KindDeletes;
        sync(k, l);
        del(k, "two1", "both");
        sync(k, l);
        check("one of two copies deleted: the song is still here, so nothing goes there", has(l, "t1"));
        Device m("mmm", "Switch"), n("nnn", "PC");
        lib(m, A.c_str());
        lib(n, B.c_str());
        n.kinds = KindAll | KindDeletes;
        n.now.liked = { B + "s4" };
        sync(m, n);
        del(m, A + "s4", "s4");
        sync(m, n);
        check("its Liked goes with it", n.now.liked.empty());
    }
    {   // odd libraries
        Device a("aaa", "Switch"), b("bbb", "PC");
        a.song("a/1", "same");
        a.song("a/2", "same");
        b.song("b/1", "same");
        a.now.liked = { "a/1" };
        a.now.plays["a/1"] = { 2, 10, 0 };
        a.now.plays["a/2"] = { 3, 20, 0 };
        sync(a, b);
        check("two copies here, one there: liked, and their plays add up", b.isLiked("b/1") && b.shown("b/1") == 5);
        sync(a, b);
        check("the second copy is not liked as well", !a.isLiked("a/2"));
        Device c("ccc", "Switch"), d("ddd", "PC");
        c.song("c/none", "");
        d.song("d/none", "");
        c.now.liked = { "c/none" };
        sync(c, d);
        check("a file with no tags stays out of it", d.now.liked.empty() && d.book.size() == 0);
        Device e("eee", "Switch"), f("fff", "PC");
        e.song("e/short", "dup", 200);
        e.song("e/long", "dup", 243);
        f.song("f/short", "dup", 200);
        f.song("f/long", "dup", 243);
        e.now.liked = { "e/short" };
        sync(e, f);
        check("the same tags 43 s apart: only the right length is liked", f.isLiked("f/short") && !f.isLiked("f/long"));
        Device g("ggg", "Switch"), h("hhh", "PC");
        lib(g, A.c_str());
        lib(h, B.c_str());
        g.kinds = KindLiked;
        h.kinds = KindLiked;
        g.now.liked = { A + "s1" };
        g.now.plays[A + "s1"] = { 9, 1, 1 };
        sync(g, h);
        check("a kind turned off is not sent or applied", h.isLiked(B + "s1") && h.shown(B + "s1") == 0);
        Device i("iii", "Switch"), j("jjj", "PC");
        lib(i, A.c_str());
        i.now.liked = { A + "s1" };
        sync(i, j);
        check("a song the other device does not have: waits, nothing breaks", j.now.liked.empty());
        lib(j, B.c_str());
        sync(i, j);
        check("and arrives once it does", j.isLiked(B + "s1"));
        check("and it did not come back as an unlike", i.isLiked(A + "s1"));
        Device k("kkk", "Switch"), l("lll", "PC");
        lib(k, A.c_str());
        l.song("x", "s1");
        k.now.edits[A + "s2"]["title"] = "Edited";
        k.now.lists.push_back({ "Mix", { A + "s1", A + "s2" } });
        sync(k, l);
        l.song("y", "s2");   // new music on the PC, after the first sync
        sync(k, l);
        sync(k, l);
        check("a song new to a device takes its edit, the edit stays", l.now.edits["y"]["title"] == "Edited" && k.now.edits[A + "s2"]["title"] == "Edited");
        check("and takes its place in the list, the list stays whole", l.list("Mix")->paths.size() == 2 && k.list("Mix")->paths.size() == 2);
    }
    {   // back from an older Songate (2026-11-02): it reset Liked and lost a list and an edit, and kept no book of it
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        a.now.liked = { A + "s1", A + "s2" };
        a.now.edits[A + "s3"]["title"] = "Mine";
        a.now.lists.push_back({ "Road", { A + "s1", A + "s2", A + "s3" } });
        a.now.lists.push_back({ "Gym", { A + "s4" } });
        sync(a, b);
        sync(a, b);
        check("before: both have it all", b.isLiked(B + "s1") && b.isLiked(B + "s2") && b.list("Road") && b.list("Gym") && b.now.edits[B + "s3"]["title"] == "Mine");
        a.now.liked.clear();                                   // the older version's reset
        a.now.edits.clear();
        a.now.lists.erase(a.now.lists.begin() + 1);            // Gym gone
        a.now.lists[0].paths.pop_back();                       // a song out of Road
        a.now.liked.push_back(A + "s4");                       // and one real like made in the older version
        a.addOnly = true;
        sync(a, b);
        a.addOnly = false;
        check("add only: the PC lost nothing", b.isLiked(B + "s1") && b.isLiked(B + "s2") && b.list("Gym") && b.list("Road")->paths.size() == 3 &&
                                                  b.now.edits[B + "s3"]["title"] == "Mine");
        check("add only: what the older version made still goes over", b.isLiked(B + "s4"));
        check("add only: what was missing here came back", a.isLiked(A + "s1") && a.isLiked(A + "s2") && a.list("Gym") && a.now.edits[A + "s3"]["title"] == "Mine");
        check("and the list has its song again", a.list("Road") && a.list("Road")->paths.size() == 3);
        check("then: nothing left to do", sync(a, b) == 0);
        a.now.liked.erase(std::remove(a.now.liked.begin(), a.now.liked.end(), A + "s1"), a.now.liked.end());
        sync(a, b);
        check("and an unlike after it is an unlike again", !b.isLiked(B + "s1"));
    }
    {   // the control: the same without add only, and the PC loses what the older version's reset took (why it exists)
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        a.now.liked = { A + "s1", A + "s2" };
        a.now.lists.push_back({ "Gym", { A + "s4" } });
        sync(a, b);
        sync(a, b);
        a.now.liked.clear();
        a.now.lists.clear();
        sync(a, b);
        check("control, without add only: the PC loses the likes and the list", !b.isLiked(B + "s1") && !b.isLiked(B + "s2") && !b.list("Gym"));
    }
    {   // folder playlists travel as their folder (2026-11-02, the user: "Yes, as a folder"), never as songs
        Device a("aaa", "Switch"), b("bbb", "PC");
        lib(a, A.c_str());
        lib(b, B.c_str());
        List f;
        f.name = "TheBeast's 19";
        f.folder = "TheBeast's 19";
        f.icon = "disc";
        a.now.lists.push_back(f);
        a.now.lists.push_back({ "Mine", { A + "s1" } });
        sync(a, b);
        const List* got = b.list("TheBeast's 19");
        check("a folder playlist arrives as one, of the same folder", got && got->folder == "TheBeast's 19" && got->paths.empty() && got->icon == "disc");
        check("and your own beside it as before", b.list("Mine") && b.list("Mine")->folder.empty() && b.list("Mine")->paths.size() == 1);
        check("then nothing to do, either way", sync(a, b) == 0 && sync(b, a) == 0);
        for (List& l : a.now.lists) if (l.name == "TheBeast's 19") l.name = "Beast";
        sync(a, b);
        check("renamed: renamed there, still the folder", b.list("Beast") && b.list("Beast")->folder == "TheBeast's 19" && !b.list("TheBeast's 19"));
        a.now.lists.erase(std::remove_if(a.now.lists.begin(), a.now.lists.end(), [](const List& l) { return l.name == "Beast"; }), a.now.lists.end());
        sync(a, b);
        check("deleted here: deleted there", !b.list("Beast") && b.list("Mine"));
        // an own playlist emptied and renamed beside a new folder one: no mix-up
        for (List& l : a.now.lists) if (l.name == "Mine") l.paths.clear();
        sync(a, b);
        for (List& l : a.now.lists) if (l.name == "Mine") l.name = "Mine 2";
        List g;
        g.name = "Gym";
        g.folder = "Gym";
        a.now.lists.push_back(g);
        sync(a, b);
        check("an emptied list renamed stays yours, the folder one is new", b.list("Mine 2") && b.list("Mine 2")->folder.empty() && b.list("Gym") &&
                                                                      b.list("Gym")->folder == "Gym");
    }
    printf("%d passed, %d failed: %s\n", passed, failed, failed ? "SOME FAILED" : "ALL GOOD");
    return failed ? 1 : 0;
}
