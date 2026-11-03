/*
 * Songate · ledger.cpp
 * See ledger.hpp.
 */
#include "ledger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include "monocypher.h"
}

namespace {

bool newer(const Ledger::Entry& a, const Ledger::Entry& b) {
    return a.clock != b.clock ? a.clock > b.clock : a.origin > b.origin;
}

std::string hashHex(const std::string& s) {
    uint8_t h[16];
    crypto_blake2b(h, sizeof h, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    static const char* const digits = "0123456789abcdef";
    std::string o;
    for (uint8_t b : h) { o += digits[b >> 4]; o += digits[b & 15]; }
    return o;
}

std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

std::string unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 == s.size()) { o += s[i]; continue; }
        const char c = s[++i];
        o += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return o;
}

}  // namespace

const Ledger::Entry* Ledger::find(const std::string& key) const {
    const auto it = all_.find(key);
    return it == all_.end() ? nullptr : &it->second;
}

void Ledger::note(const Entry& e) {
    clock_ = std::max(clock_, e.clock);
    uint64_t& h = have_[e.origin];
    h = std::max(h, e.seq);
}

void Ledger::put(const std::string& key, const std::string& value) {
    auto it = all_.find(key);
    if (it != all_.end() && it->second.value == value) return;
    Entry e;
    e.key = key;
    e.value = value;
    e.origin = me_;
    e.clock = clock_ + 1;
    e.seq = have_[me_] + 1;
    note(e);
    all_[key] = std::move(e);
}

std::vector<Ledger::Entry> Ledger::since(const Have& theirs) const {
    std::vector<Entry> out;
    for (const auto& kv : all_) {
        const auto it = theirs.find(kv.second.origin);
        if (it == theirs.end() || kv.second.seq > it->second) out.push_back(kv.second);
    }
    // in each device's order: a sync stopped halfway has taken in a clean beginning, so "have" stays true
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.origin != b.origin ? a.origin < b.origin : a.seq < b.seq; });
    return out;
}

std::vector<std::string> Ledger::merge(const std::vector<Entry>& incoming, const Have& theirs, const std::string& deviceName, const std::string& myName) {
    std::vector<std::string> changed;
    const auto heardOfMe = theirs.find(me_);
    const uint64_t saw = heardOfMe == theirs.end() ? 0 : heardOfMe->second;
    for (const Entry& e : incoming) {
        if (e.origin.empty() || e.key.empty()) continue;
        auto it = all_.find(e.key);
        if (it == all_.end()) {
            note(e);
            all_[e.key] = e;
            changed.push_back(e.key);
            continue;
        }
        Entry& mine = it->second;
        const bool same = mine.value == e.value;
        // a playlist changed here since the other device last heard, and there too: neither is the newer one, so both are kept
        if (e.key[0] == 'P' && !same && mine.origin == me_ && mine.seq > saw && e.origin != me_) {
            if (e.value.empty() || mine.value.empty()) {
                // deleted there and changed here, or the other way: the one that is still a playlist stays, on both
                note(e);
                if (mine.value.empty()) { mine = e; changed.push_back(e.key); }
                continue;
            }
            const bool theyWin = newer(e, mine);
            const Entry loser = theyWin ? mine : e;
            std::string name;
            std::vector<std::string> songs;
            unpackList(loser.value, name, songs);
            const std::string who = theyWin ? myName : deviceName;
            const std::string keep = ledgerkey::list(hashHex(e.key + "\n" + std::to_string(loser.clock) + "\n" + loser.origin));
            if (!all_.count(keep)) {
                Entry copy;
                copy.key = keep;
                copy.value = packList(name + " (" + who + ")", songs);
                copy.origin = me_;
                copy.clock = loser.clock;
                copy.seq = have_[me_] + 1;
                note(copy);
                all_[keep] = copy;
                changed.push_back(keep);
            }
            if (theyWin) {
                note(e);
                Entry& w = all_[e.key];
                w = e;
                changed.push_back(e.key);
            } else {
                note(e);
            }
            continue;
        }
        note(e);
        if (newer(e, mine)) {
            mine = e;
            if (!same) changed.push_back(e.key);
        }
    }
    return changed;
}

std::string Ledger::serialise() const {
    std::vector<const Entry*> list;
    list.reserve(all_.size());
    for (const auto& kv : all_) list.push_back(&kv.second);
    std::sort(list.begin(), list.end(), [](const Entry* a, const Entry* b) { return a->key < b->key; });
    std::string o = "clock\t" + std::to_string(clock_) + "\n";
    for (const auto& h : have_) o += "have\t" + h.first + "\t" + std::to_string(h.second) + "\n";
    for (const Entry* e : list) o += packEntry(*e);
    return o;
}

void Ledger::load(const std::string& text) {
    all_.clear();
    have_.clear();
    clock_ = 0;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        const std::string l = text.substr(a, b - a);
        a = b + 1;
        std::vector<std::string> c;
        size_t p = 0;
        for (;;) {
            const size_t t = l.find('\t', p);
            c.push_back(l.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) break;
            p = t + 1;
        }
        if (c[0] == "clock" && c.size() == 2) clock_ = std::max<uint64_t>(clock_, strtoull(c[1].c_str(), nullptr, 10));
        else if (c[0] == "have" && c.size() == 3) have_[c[1]] = strtoull(c[2].c_str(), nullptr, 10);
        else if (c[0] == "e") {
            Entry e;
            if (!unpackEntry(l, e)) continue;
            clock_ = std::max(clock_, e.clock);
            all_[e.key] = std::move(e);
        }
    }
}

std::string Ledger::packEntry(const Entry& e) {
    return "e\t" + std::to_string(e.clock) + "\t" + std::to_string(e.seq) + "\t" + e.origin + "\t" + escape(e.key) + "\t" + escape(e.value) + "\n";
}

bool Ledger::unpackEntry(const std::string& line, Entry& e) {
    std::vector<std::string> c;
    size_t p = 0;
    for (;;) {
        const size_t t = line.find('\t', p);
        c.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
        if (t == std::string::npos) break;
        p = t + 1;
    }
    if (c.size() != 6 || c[0] != "e" || c[3].empty() || c[4].empty()) return false;
    e.clock = strtoull(c[1].c_str(), nullptr, 10);
    e.seq = strtoull(c[2].c_str(), nullptr, 10);
    e.origin = c[3];
    e.key = unescape(c[4]);
    e.value = unescape(c[5]);
    return true;
}

std::string Ledger::packList(const std::string& name, const std::vector<std::string>& songs) {
    std::string v = name;
    for (const std::string& s : songs) { v += ledgerkey::kSep; v += s; }
    return v;
}

void Ledger::unpackList(const std::string& value, std::string& name, std::vector<std::string>& songs) {
    songs.clear();
    size_t a = value.find(ledgerkey::kSep);
    name = value.substr(0, a);
    while (a != std::string::npos) {
        const size_t b = value.find(ledgerkey::kSep, a + 1);
        songs.push_back(value.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
        a = b;
    }
}
