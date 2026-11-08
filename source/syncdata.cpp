/*
 * Songate · syncdata.cpp
 * See syncdata.hpp.
 */
#include "syncdata.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace syncdata {
namespace {

using ledgerkey::kSep;

struct Group { std::vector<const Song*> copies; };
using Groups = std::unordered_map<std::string, Group>;

/** Songs by id. Copies over 2 s apart are not the same song: only those near the length on record count, or the first's. */
Groups groups(Ledger* writeTo, const Ledger& ledger, const Snapshot& now) {
    Groups g;
    for (const Song& s : now.songs) if (!s.id.empty()) g[s.id].copies.push_back(&s);
    for (auto& kv : g) {
        auto& c = kv.second.copies;
        if (c.size() < 2) continue;
        double lo = c[0]->duration, hi = lo;
        for (const Song* s : c) { lo = std::min(lo, s->duration); hi = std::max(hi, s->duration); }
        const Ledger::Entry* d = ledger.find("D" + kv.first);
        if (hi - lo <= 2 && !d) continue;
        if (!d && writeTo) { char t[32]; snprintf(t, sizeof t, "%.1f", c[0]->duration); writeTo->put("D" + kv.first, t); d = writeTo->find("D" + kv.first); }
        const double at = d ? atof(d->value.c_str()) : c[0]->duration;
        c.erase(std::remove_if(c.begin(), c.end(), [&](const Song* s) { return s->duration - at > 2 || at - s->duration > 2; }), c.end());
    }
    return g;
}

std::string playsText(const Plays& p) { return std::to_string(p.plays) + " " + std::to_string((long long)p.last) + " " + std::to_string((long long)p.added); }

Plays playsOf(const std::string& v) {
    Plays p;
    long long l = 0, a = 0;
    if (sscanf(v.c_str(), "%d %lld %lld", &p.plays, &l, &a) >= 1) { p.last = l; p.added = a; }
    return p;
}

/** A list's name in the book: what kind of list it is, then the name, which a copy named after its device may add to. */
constexpr char kTrait = '\x02';

std::string nameWithTraits(const List& l, const std::unordered_map<std::string, std::string>& bySong) {
    if (!l.automix && l.icon.empty() && l.folder.empty()) return l.name;
    std::string pin;
    if (!l.pin.empty()) { auto it = bySong.find(l.pin); if (it != bySong.end()) pin = it->second; }
    // a folder playlist: an f among the kinds, and its folder before the name
    return std::string(1, kTrait) + (l.automix ? "a" : "-") + (l.manual ? "m" : "-") + (l.folder.empty() ? "" : "f") + kTrait + l.icon + kTrait + pin +
           kTrait + (l.folder.empty() ? std::string() : l.folder + kTrait) + l.name;
}

/** Fills the list's kind and returns its name; pinSong: the first track, as a song. */
std::string traitsOf(const std::string& text, List& l, std::string& pinSong) {
    l.automix = l.manual = false;
    l.icon.clear();
    l.folder.clear();
    pinSong.clear();
    if (text.empty() || text[0] != kTrait) return text;
    const size_t a = text.find(kTrait, 1), b = a == std::string::npos ? a : text.find(kTrait, a + 1), c = b == std::string::npos ? b : text.find(kTrait, b + 1);
    if (c == std::string::npos) return text;
    l.automix = text.size() > 1 && text[1] == 'a';
    l.manual = text.size() > 2 && text[2] == 'm';
    l.icon = text.substr(a + 1, b - a - 1);
    pinSong = text.substr(b + 1, c - b - 1);
    if (text.substr(1, a - 1).find('f') != std::string::npos) {   // a folder playlist: its folder, then the name
        const size_t d = text.find(kTrait, c + 1);
        if (d != std::string::npos) {
            l.folder = text.substr(c + 1, d - c - 1);
            return text.substr(d + 1);
        }
    }
    return text.substr(c + 1);
}

std::string freeId(const std::string& name, const std::string& device, const ListIds& ids) {
    // a name is not unique for ever, so the device and a count go in too; a hash keeps it short and its characters plain
    uint32_t h = 2166136261u;
    for (unsigned char c : name + "|" + device) h = (h ^ c) * 16777619u;
    for (uint32_t n = 0;; n++) {
        char t[32];
        snprintf(t, sizeof t, "%08x%04x", h, n);
        if (!ids.count(t)) return t;
    }
}

std::unordered_map<std::string, std::string> songByPath(const Snapshot& now) {
    std::unordered_map<std::string, std::string> m;
    m.reserve(now.songs.size());
    for (const Song& s : now.songs) if (!s.id.empty()) m[s.path] = s.id;
    return m;
}

std::vector<std::string> idsOf(const List& l, const std::unordered_map<std::string, std::string>& bySong) {
    std::vector<std::string> v;
    v.reserve(l.paths.size());
    for (const std::string& p : l.paths) { auto it = bySong.find(p); if (it != bySong.end()) v.push_back(it->second); }
    return v;
}

/** The list's songs that are here: a list keeps songs of the other device, which must not vanish for not being here. */
std::vector<std::string> present(const std::vector<std::string>& ids, const Groups& g) {
    std::vector<std::string> v;
    for (const std::string& s : ids) if (g.count(s)) v.push_back(s);
    return v;
}

/** The same among songs that were here at the last sync: one that came since cannot have been taken out of a list here. */
std::vector<std::string> presentThen(const std::vector<std::string>& ids, const Groups& g, const Seen& seen) {
    std::vector<std::string> v;
    for (const std::string& s : ids) if (g.count(s) && seen.songs.count(s)) v.push_back(s);
    return v;
}

}  // namespace

std::string lyricsText(std::string s) {
    std::string t;
    for (char c : s) if (c != '\r') t += c;
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
    return t;
}

std::string Changes::summary() const {
    std::string s;
    auto add = [&](size_t n, const char* one, const char* many) {
        if (!n) return;
        if (!s.empty()) s += ", ";
        s += std::to_string(n) + " " + (n == 1 ? one : many);
    };
    add(likedAdd.size(), "song liked", "songs liked");
    add(likedRemove.size(), "song unliked", "songs unliked");
    add(listsNew.size(), "new playlist", "new playlists");
    add(listsSet.size() + listsRename.size(), "playlist changed", "playlists changed");
    add(listsDelete.size(), "playlist deleted", "playlists deleted");
    add(filesDelete.size(), "song deleted", "songs deleted");
    add(lyricsSet.size(), "song's lyrics", "songs' lyrics");
    add(moodsSet.size(), "song's mood", "songs' moods");
    add(edits.size(), "tag edit", "tag edits");
    add(plays.size(), "play count updated", "play counts updated");
    if (!listOrder.empty()) add(1, "playlist order changed", "");
    return s.empty() ? std::string("Nothing changes here") : s;
}

void scan(Ledger& ledger, const Snapshot& now, ListIds& ids, const Seen& seen, unsigned kinds, bool addOnly) {
    const std::string me = ledger.device();
    const Groups g = groups(&ledger, ledger, now);
    std::set<std::string> liked(now.liked.begin(), now.liked.end());

    // what the book holds, by song: looked at once
    std::unordered_map<std::string, std::vector<std::pair<std::string, const Ledger::Entry*>>> editsOf;
    for (const auto& kv : ledger.entries()) {
        const std::string& k = kv.first;
        const size_t sep = k.find(kSep);
        if (sep == std::string::npos) continue;
        const std::string song = k.substr(1, sep - 1), rest = k.substr(sep + 1);
        if (k[0] == 'E') editsOf[song].push_back({ rest, &kv.second });
    }

    // Deleted here, inside Songate: the only way a song counts as deleted. One that merely is not here (a card out, a folder
    // moved) proves nothing, and would empty the other device. Written whatever the switches: the other device's says if it follows
    for (const std::string& song : now.gone) {
        auto it = g.find(song);
        if (it == g.end() || it->second.copies.empty()) ledger.put("X" + song, "1");
    }
    // deleted once, and here again by someone's choice: no longer deleted
    for (const auto& kv : g) {
        const Ledger::Entry* x = ledger.find("X" + kv.first);
        if (x && x->value == "1" && !kv.second.copies.empty() && !seen.songs.count(kv.first)) ledger.put("X" + kv.first, "");
    }

    if (kinds & KindLyrics)
        for (const std::string& song : now.lyricsGone) {
            auto it = g.find(song);
            bool still = false;
            if (it != g.end()) for (const Song* s : it->second.copies) still = still || now.lyrics.count(s->path);
            if (!still) ledger.put(ledgerkey::lyrics(song), "");
        }

    for (const auto& kv : g) {
        const std::string& song = kv.first;
        const auto& copies = kv.second.copies;
        if (copies.empty()) continue;
        if (kinds & KindLyrics) {
            for (const Song* s : copies) {
                auto l = now.lyrics.find(s->path);
                if (l != now.lyrics.end() && !l->second.empty()) { ledger.put(ledgerkey::lyrics(song), l->second); break; }
            }
        }
        if (kinds & KindMoods) {   // as Liked: put while chosen here; cleared only if it was here before
            std::string v;
            for (const Song* s : copies) {
                auto it = now.moods.find(s->path);
                if (it != now.moods.end() && !it->second.empty()) { v = it->second; break; }
            }
            const std::string key = ledgerkey::mood(song);
            const Ledger::Entry* e = ledger.find(key);
            if (!v.empty() && (!e || e->value != v)) ledger.put(key, v);
            else if (v.empty() && e && !e->value.empty() && seen.keys.count(key) && !addOnly) ledger.put(key, "");
        }
        if (kinds & KindLiked) {
            bool on = false;
            for (const Song* s : copies) on = on || liked.count(s->path);
            const std::string key = ledgerkey::liked(song);
            const Ledger::Entry* e = ledger.find(key);
            if (on && (!e || e->value != "1")) ledger.put(key, "1");
            else if (!on && e && e->value == "1" && seen.keys.count(key) && !addOnly) ledger.put(key, "");
        }
        if (kinds & KindEdits) {
            std::map<std::string, std::string> fields;
            for (const Song* s : copies) {
                auto it = now.edits.find(s->path);
                if (it != now.edits.end()) for (const auto& f : it->second) fields.emplace(f.first, f.second);
            }
            for (const auto& f : fields) ledger.put(ledgerkey::edit(song, f.first), f.second);
            auto was = editsOf.find(song);
            if (was != editsOf.end())
                for (const auto& r : was->second)
                    if (!r.second->value.empty() && !fields.count(r.first) && seen.keys.count(ledgerkey::edit(song, r.first)) && !addOnly)
                        ledger.put(ledgerkey::edit(song, r.first), "");
        }
        if (kinds & KindPlays) {
            Plays t;
            for (const Song* s : copies) {
                auto it = now.plays.find(s->path);
                if (it == now.plays.end()) continue;
                t.plays += it->second.plays;
                t.last = std::max(t.last, it->second.last);
                if (it->second.added) t.added = t.added ? std::min(t.added, it->second.added) : it->second.added;
            }
            const std::string key = ledgerkey::plays(song, me);
            if (t.plays || ledger.find(key)) ledger.put(key, playsText(t));
        }
    }

    if (kinds & KindLists) {
        const auto bySong = songByPath(now);
        std::set<std::string> names;
        for (const List& l : now.lists) names.insert(l.name);
        for (const List& l : now.lists) {
            const std::vector<std::string> mine = idsOf(l, bySong);
            std::string id;
            for (const auto& kv : ids) if (kv.second == l.name) id = kv.first;
            if (id.empty()) {
                // renamed here? an id whose name is gone, holding these same songs (a pin or icon may differ)
                for (auto& kv : ids) {
                    if (kv.second.empty() || names.count(kv.second)) continue;
                    const Ledger::Entry* e = ledger.find(ledgerkey::list(kv.first));
                    if (!e) continue;
                    std::string n;
                    std::vector<std::string> songs;
                    Ledger::unpackList(e->value, n, songs);
                    List was;
                    std::string wasPin;
                    traitsOf(n, was, wasPin);
                    if (was.folder != l.folder) continue;
                    if (presentThen(songs, g, seen) == mine) { id = kv.first; kv.second = l.name; break; }
                }
            }
            if (id.empty()) { id = freeId(l.name, me, ids); ids[id] = l.name; }
            const std::string key = ledgerkey::list(id);
            const Ledger::Entry* e = ledger.find(key);
            if (e) {
                std::string n;
                std::vector<std::string> songs;
                Ledger::unpackList(e->value, n, songs);
                if (n == nameWithTraits(l, bySong) && presentThen(songs, g, seen) == mine) continue;   // nothing changed here
                // keep the songs only the other device has, where they stood at the end
                std::vector<std::string> keep = mine;
                for (const std::string& s : songs)
                    if (!g.count(s) || !seen.songs.count(s) || (addOnly && std::find(keep.begin(), keep.end(), s) == keep.end())) keep.push_back(s);
                ledger.put(key, Ledger::packList(nameWithTraits(l, bySong), keep));
            } else {
                ledger.put(key, Ledger::packList(nameWithTraits(l, bySong), mine));
            }
        }
        // deleted here: deleted everywhere (the user, 2026-10-31). The book says so, as it does for an unlike
        for (auto& kv : ids) {
            if (!kv.second.empty() && !names.count(kv.second)) kv.second.clear();
            if (!kv.second.empty()) continue;
            if (addOnly) continue;   // not here, not deleted: plan() makes it again from the book
            const Ledger::Entry* e = ledger.find(ledgerkey::list(kv.first));
            if (e && !e->value.empty()) ledger.put(ledgerkey::list(kv.first), "");
        }

        // their order, as ids; those not here keep a place at the end, for the devices that have them
        std::vector<std::string> order;
        for (const List& l : now.lists)
            for (const auto& kv : ids) if (kv.second == l.name) { order.push_back(kv.first); break; }
        std::string unused;
        std::vector<std::string> was;
        if (const Ledger::Entry* e = ledger.find("O")) Ledger::unpackList(e->value, unused, was);
        std::vector<std::string> wasHere;
        for (const std::string& id : was) {
            auto it = ids.find(id);
            if (it != ids.end() && !it->second.empty() && std::find(order.begin(), order.end(), id) != order.end()) wasHere.push_back(id);
        }
        if (wasHere != order) {
            std::vector<std::string> keep = order;
            for (const std::string& id : was) if (std::find(keep.begin(), keep.end(), id) == keep.end()) keep.push_back(id);
            ledger.put("O", Ledger::packList("", keep));
        }
    }
}

Changes plan(const Ledger& after, const Snapshot& now, ListIds& ids, Seen& seen, unsigned kinds) {
    Changes out;
    const std::string me = after.device();
    const Groups g = groups(nullptr, after, now);
    std::set<std::string> liked(now.liked.begin(), now.liked.end());
    std::unordered_map<std::string, Plays> best;   // by song: others' plays added up, the latest time, the earliest date

    for (const auto& kv : after.entries()) {
        const std::string& k = kv.first;
        const std::string& v = kv.second.value;
        if (k[0] == 'L' && (kinds & KindLiked)) {
            auto it = g.find(k.substr(1));
            if (it == g.end() || it->second.copies.empty()) continue;
            bool any = false;
            for (const Song* s : it->second.copies) any = any || liked.count(s->path);
            if (v == "1" && !any) out.likedAdd.push_back(it->second.copies[0]->path);
            else if (v.empty() && any) for (const Song* s : it->second.copies) if (liked.count(s->path)) out.likedRemove.push_back(s->path);
        } else if (k[0] == 'E' && (kinds & KindEdits)) {
            const size_t sep = k.find(kSep);
            if (sep == std::string::npos) continue;
            auto it = g.find(k.substr(1, sep - 1));
            if (it == g.end()) continue;
            const std::string field = k.substr(sep + 1);
            for (const Song* s : it->second.copies) {
                auto e = now.edits.find(s->path);
                const std::string have = e == now.edits.end() || !e->second.count(field) ? std::string() : e->second.at(field);
                if (have != v) out.edits.push_back({ s->path, field, v });
            }
        } else if (k[0] == 'Y' && (kinds & KindLyrics)) {
            auto it = g.find(k.substr(1));
            if (it == g.end()) continue;
            for (const Song* s : it->second.copies) {
                const std::string ownKey = "own\t" + s->path;
                if (std::find(now.lyricsKept.begin(), now.lyricsKept.end(), ownKey) != now.lyricsKept.end()) continue;   // its own: never touched
                auto have = now.lyrics.find(s->path);
                if (!v.empty()) { if (have == now.lyrics.end() || have->second != v) out.lyricsSet.push_back({ s->path, v }); }
                else if (have != now.lyrics.end()) out.lyricsRemove.push_back(s->path);
            }
        } else if (k[0] == 'M' && (kinds & KindMoods)) {
            auto it = g.find(k.substr(1));
            if (it == g.end()) continue;
            for (const Song* s : it->second.copies) {
                auto have = now.moods.find(s->path);
                const std::string h = have == now.moods.end() ? std::string() : have->second;
                if (h != v) out.moodsSet.push_back({ s->path, v });
            }
        } else if (k[0] == 'X' && (kinds & KindDeletes)) {
            if (v != "1" || kv.second.clock <= now.deleteFrom) continue;
            auto it = g.find(k.substr(1));
            if (it == g.end() || !seen.songs.count(k.substr(1))) continue;   // a copy just arrived is no copy to delete
            for (const Song* s : it->second.copies) out.filesDelete.push_back(s->path);
        } else if (k[0] == 'C' && (kinds & KindPlays)) {
            const size_t sep = k.find(kSep);
            if (sep == std::string::npos) continue;
            const std::string song = k.substr(1, sep - 1);
            const Plays p = playsOf(v);
            Plays& b = best[song];
            if (k.substr(sep + 1) != me) b.plays += p.plays;
            b.last = std::max(b.last, p.last);
            if (p.added) b.added = b.added ? std::min(b.added, p.added) : p.added;
        }
    }

    if (kinds & KindPlays) {
        for (const auto& kv : best) {
            auto it = g.find(kv.first);
            if (it == g.end() || it->second.copies.empty()) continue;
            const std::string& path = it->second.copies[0]->path;
            auto have = now.plays.find(path);
            const Plays was = have == now.plays.end() ? Plays{} : have->second;
            auto oth = now.others.find(path);
            const int wasOthers = oth == now.others.end() ? 0 : oth->second;
            Plays p;
            p.plays = kv.second.plays;
            p.last = std::max(was.last, kv.second.last);
            p.added = !kv.second.added ? was.added : was.added ? std::min(was.added, kv.second.added) : kv.second.added;
            if (p.plays != wasOthers || p.last != was.last || p.added != was.added) out.plays.push_back({ path, p });
        }
    }

    if (kinds & KindLists) {
        std::set<std::string> taken;
        const auto bySong = songByPath(now);
        for (const List& l : now.lists) taken.insert(l.name);
        for (const auto& kv : after.entries()) {
            if (kv.first[0] != 'P') continue;
            const std::string id = kv.first.substr(1);
            if (kv.second.value.empty()) {   // deleted on another device
                auto gone = ids.find(id);
                if (gone != ids.end() && !gone->second.empty() && taken.count(gone->second)) {
                    out.listsDelete.push_back(gone->second);
                    taken.erase(gone->second);
                }
                ids[id] = "";
                continue;
            }
            std::string text;
            std::vector<std::string> songs;
            Ledger::unpackList(kv.second.value, text, songs);
            List incoming;
            std::string pinSong;
            const std::string name = traitsOf(text, incoming, pinSong);
            if (!pinSong.empty()) { auto it = g.find(pinSong); if (it != g.end() && !it->second.copies.empty()) incoming.pin = it->second.copies[0]->path; }
            std::vector<std::string> paths;
            for (const std::string& s : songs) {
                auto it = g.find(s);
                if (it != g.end() && !it->second.copies.empty()) paths.push_back(it->second.copies[0]->path);
            }
            const auto known = ids.find(id);
            if (known == ids.end() || known->second.empty()) {
                // new here, or deleted here while it was changed there. A list of that name already, not by this id: yours stays, this one beside it
                std::string use = name;
                for (int n = 2; taken.count(use); n++) use = name + " " + std::to_string(n);
                taken.insert(use);
                ids[id] = use;
                incoming.name = use;
                incoming.paths = paths;
                out.listsNew.push_back(std::move(incoming));
                continue;
            }
            const List* mine = nullptr;
            for (const List& l : now.lists) if (l.name == known->second) mine = &l;
            if (!mine) continue;
            if (mine->name != name && !taken.count(name)) {
                out.listsRename.push_back({ mine->name, name });
                taken.insert(name);
                known->second = name;
            }
            const std::vector<std::string> here = idsOf(*mine, bySong);
            const bool kind = mine->automix != incoming.automix || mine->manual != incoming.manual || mine->icon != incoming.icon || mine->pin != incoming.pin ||
                              mine->folder != incoming.folder;
            if (present(songs, g) != here || kind) {
                incoming.name = known->second;
                incoming.paths = paths;
                out.listsSet.push_back(std::move(incoming));
            }
        }
        // their order, by the names they will have once the rest is applied
        if (const Ledger::Entry* e = after.find("O")) {
            std::string unused;
            std::vector<std::string> order;
            Ledger::unpackList(e->value, unused, order);
            std::vector<std::string> will;
            for (const List& l : now.lists) will.push_back(l.name);
            for (const auto& r : out.listsRename) std::replace(will.begin(), will.end(), r.from, r.to);
            for (const auto& n : out.listsDelete) std::erase(will, n);
            for (const List& l : out.listsNew) will.push_back(l.name);
            std::vector<std::string> want;
            for (const std::string& id : order) {
                auto it = ids.find(id);
                if (it != ids.end() && !it->second.empty() && std::find(will.begin(), will.end(), it->second) != will.end()) want.push_back(it->second);
            }
            // those the record does not name stay after the ones it does
            std::vector<std::string> rest;
            for (const std::string& n : will) if (std::find(want.begin(), want.end(), n) == want.end()) rest.push_back(n);
            want.insert(want.end(), rest.begin(), rest.end());
            if (want != will) out.listOrder = std::move(want);
        }
    }

    seen.keys.clear();
    seen.songs.clear();
    for (const auto& kv : g) if (!kv.second.copies.empty()) seen.songs.insert(kv.first);
    for (const auto& kv : after.entries()) {
        const std::string& k = kv.first;
        if ((k[0] != 'L' && k[0] != 'E' && k[0] != 'M') || kv.second.value.empty()) continue;
        const size_t sep = k.find(kSep);
        if (seen.songs.count(k[0] != 'E' ? k.substr(1) : k.substr(1, sep == std::string::npos ? sep : sep - 1))) seen.keys.insert(k);
    }
    return out;
}

}  // namespace syncdata
