/*
 * Songate · lyricindex.cpp
 * See lyricindex.hpp.
 */
#include "lyricindex.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <dirent.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unordered_map>

#include "cores.hpp"
#include "lyricsnet.hpp"
#include "metadata.hpp"

namespace {

/** Latin-1 Supplement's letters (U+00C0..U+00FF) without their accents, lower case; "" for none. */
const char* kLatin1[64] = { "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
                            "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "ss",
                            "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
                            "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "y" };

bool wordChar(unsigned char c) { return isalnum(c) || c >= 0x80 || c == '\''; }

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) o += c == '\t' || c == '\n' || c == '\r' ? ' ' : c;
    return o;
}

}  // namespace

void LyricIndex::Song::addLine(float t, const char* p, size_t n) {
    if (!lines.empty()) text += '\n';
    lines.push_back({ t, uint32_t(text.size()), uint32_t(n), 0, 0 });
    text.append(p, n);
}

void LyricIndex::Song::finish() {
    fold = foldText(text);   // ('\n' folds to itself: the lines stay where they were, one a line)
    size_t at = 0;
    for (Line& l : lines) {
        size_t e = fold.find('\n', at);
        if (e == std::string::npos) e = fold.size();
        l.fa = uint32_t(at);
        l.fn = uint32_t(e - at);
        at = e + 1;
    }
}

std::string foldText(const std::string& s, std::vector<int>* offsets) {
    std::string o;
    o.reserve(s.size());
    if (offsets) offsets->clear();
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < s.size()) {   // é, ü, ñ, ç…: their letter
            const unsigned char d = (unsigned char)s[i + 1];
            const int k = int(d) - 0x80;
            if (k >= 0 && k < 64 && kLatin1[k]) {
                for (const char* p = kLatin1[k]; *p; p++) { o += *p; if (offsets) offsets->push_back(int(i)); }
                i += 2;
                continue;
            }
        }
        if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 && ((unsigned char)s[i + 2] == 0x98 || (unsigned char)s[i + 2] == 0x99)) {
            o += '\'';   // ‘ ’ as '
            if (offsets) offsets->push_back(int(i));
            i += 3;
            continue;
        }
        o += char(tolower(c));
        if (offsets) offsets->push_back(int(i));
        i++;
    }
    return o;
}

void LyricIndex::start(const std::string& cacheFile) {
    std::lock_guard<std::mutex> g(m_);
    if (started_) return;
    cache_ = cacheFile;
    started_ = true;
    quit_ = false;
    th_ = std::thread([this] { cores::moveHere(cores::Loud); run(); });
}

void LyricIndex::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> g(m_);
    started_ = false;
}

void LyricIndex::pause(bool on) {
    {
        std::lock_guard<std::mutex> g(m_);
        paused_ = on;
    }
    cv_.notify_all();
}

void LyricIndex::hold(bool on) {
    if (held_.exchange(on) == on) return;
    { std::lock_guard<std::mutex> g(m_); }
    cv_.notify_all();
}

void LyricIndex::setLibrary(std::vector<std::string> paths) {
    {
        std::lock_guard<std::mutex> g(m_);
        wantLib_ = std::move(paths);
        haveLib_ = true;
    }
    cv_.notify_all();
}

void LyricIndex::refresh(const std::string& path) {
    {
        std::lock_guard<std::mutex> g(m_);
        refresh_.push_back(path);
    }
    cv_.notify_all();
}

void LyricIndex::setSidecars(std::vector<std::string> lrcs) {
    {
        std::lock_guard<std::mutex> g(m_);
        wantLrc_ = std::move(lrcs);
        haveLrc_ = true;
    }
    cv_.notify_all();
}

void LyricIndex::offer(const std::string& path, const std::string& text) {
    {
        std::lock_guard<std::mutex> g(m_);
        if (offers_.size() > 8) offers_.erase(offers_.begin());   // (skipping through songs: the last ones count)
        offers_.emplace_back(path, text);
    }
    cv_.notify_all();
}

void LyricIndex::checkAll() {
    {
        std::lock_guard<std::mutex> g(m_);
        wantCheck_ = true;
    }
    cv_.notify_all();
}

void LyricIndex::search(const std::string& query, uint64_t ticket) {
    {
        std::lock_guard<std::mutex> g(m_);
        query_ = query;
        queryTicket_ = ticket;
        haveQuery_ = true;
    }
    cv_.notify_all();
}

bool LyricIndex::takeResults(uint64_t& ticket, std::vector<Hit>& hits) {
    std::lock_guard<std::mutex> g(m_);
    if (!haveDone_) return false;
    haveDone_ = false;
    ticket = doneTicket_;
    hits = std::move(done_);
    return true;
}

std::string LyricIndex::keyOf(const std::string& path) const {
    // the song, its .lrc and Songate's copy: their sizes and dates (any of them changed: read again)
    std::string k;
    auto add = [&](const std::string& p) {
        struct stat st;
        if (stat(p.c_str(), &st) == 0) k += std::to_string((long long)st.st_size) + ":" + std::to_string((long long)st.st_mtime);
        k += ";";
    };
    add(path);
    const size_t dot = path.find_last_of('.'), slash = path.find_last_of('/');
    add((dot == std::string::npos || (slash != std::string::npos && dot < slash) ? path : path.substr(0, dot)) + ".lrc");
    const std::string sp = lyrnet::savedPath(path);   // Songate's copy: there or not, from the folder's listing (saved_)
    k += std::binary_search(saved_.begin(), saved_.end(), sp.substr(sp.find_last_of('/') + 1)) ? "s" : "-";
    return k;
}

void LyricIndex::listSaved() {
    saved_.clear();
    const std::string any = lyrnet::savedPath("x"), dir = any.substr(0, any.find_last_of('/'));
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) saved_.push_back(e->d_name);
        closedir(d);
    }
    std::sort(saved_.begin(), saved_.end());
}

void LyricIndex::readSong(Song& s) {
    std::string text = readSidecarLyrics(s.path);   // as playback finds them: the .lrc, the tags, Songate's folder
    if (text.empty()) {
        TrackInfo t;
        readTrackInfo(s.path, t, false, true);
        text = t.lyrics;
    }
    if (text.empty()) text = lyrnet::readSaved(s.path);
    lyrnet::trace("index: read " + s.path);
    parse(text, s);
    s.read = true;
    s.stale = false;
}

void LyricIndex::parse(const std::string& text, Song& into) {
    into.clearLines();
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string l = text.substr(a, b - a);
        a = b + 1;
        float t = -1;
        bool meta = false;
        while (!l.empty() && l[0] == '[') {   // [mm:ss.xx] its time; [ar:…] and the like: not words of the song
            const size_t c = l.find(']');
            if (c == std::string::npos) break;
            int mm = 0;
            double ss = 0;
            if (sscanf(l.c_str() + 1, "%d:%lf", &mm, &ss) == 2) { if (t < 0) t = float(mm * 60 + ss); }
            else meta = true;
            l.erase(0, c + 1);
        }
        while (!l.empty() && (l[0] == ' ' || l[0] == '\t')) l.erase(0, 1);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.empty() || (meta && t < 0)) continue;
        into.addLine(t, l.data(), l.size());
        if (a > text.size()) break;
    }
    into.finish();
}

void LyricIndex::applySidecars(std::vector<std::string> lrcs) {
    std::sort(lrcs.begin(), lrcs.end());
    if (lrcKnown_) {   // the ones that came or went: their songs (the same name, any extension) read again
        std::vector<std::string> changed;
        std::set_symmetric_difference(lrcs.begin(), lrcs.end(), lrcs_.begin(), lrcs_.end(), std::back_inserter(changed));
        if (!changed.empty()) {
            std::unordered_map<std::string, size_t> byStem;
            for (size_t i = 0; i < songs_.size(); i++) {
                const std::string& p = songs_[i].path;
                const size_t dot = p.find_last_of('.'), slash = p.find_last_of('/');
                byStem[dot == std::string::npos || (slash != std::string::npos && dot < slash) ? p : p.substr(0, dot)] = i;
            }
            for (const std::string& l : changed) {
                lyrnet::trace("index: .lrc came or went: " + l);
                auto it = byStem.find(l.substr(0, l.size() - 4));
                if (it != byStem.end() && songs_[it->second].read) { songs_[it->second].stale = true; read_--; }
            }
        }
    }
    lrcs_ = std::move(lrcs);
    lrcKnown_ = true;
}

void LyricIndex::loadCache() {
    FILE* f = fopen(cache_.c_str(), "rb");
    if (!f) return;
    std::string all;   // the whole file at once, its lines read where they lie (no string per line)
    {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0) {
            all.resize(size_t(size));
            all.resize(fread(&all[0], 1, all.size(), f));
        }
        fclose(f);
    }
    std::unordered_map<std::string_view, size_t> at;
    at.reserve(songs_.size());
    for (size_t i = 0; i < songs_.size(); i++) at[songs_[i].path] = i;
    Song* cur = nullptr;
    int songsDone = 0;
    auto close = [&] {
        if (!cur) return;
        cur->finish();
        cur = nullptr;
        if (++songsDone % 64 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // (room for everyone else)
    };
    const std::string_view v(all);
    for (size_t a = 0; a < v.size();) {
        size_t e = v.find('\n', a);
        if (e == std::string_view::npos) e = v.size();
        std::string_view l = v.substr(a, e - a);
        a = e + 1;
        while (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        if (l.size() > 2 && l[0] == 'S' && l[1] == '\t') {   // S <TAB> path <TAB> key
            close();
            const size_t t2 = l.find('\t', 2);
            if (t2 == std::string_view::npos) continue;
            auto it = at.find(l.substr(2, t2 - 2));
            if (it == at.end()) continue;
            cur = &songs_[it->second];
            cur->key = std::string(l.substr(t2 + 1));
            cur->clearLines();
            cur->read = true;
        } else if (l == "K") {
            close();
            lrcKnown_ = true;
        } else if (l.size() > 2 && l[0] == 'R' && l[1] == '\t') {   // R <TAB> an .lrc file the walk found
            close();
            lrcs_.emplace_back(l.substr(2));
            lrcKnown_ = true;
        } else if (cur && l.size() > 2 && l[0] == 'L' && l[1] == '\t') {   // L <TAB> time <TAB> text
            const size_t t2 = l.find('\t', 2);
            if (t2 == std::string_view::npos) continue;
            const float t = strtof(l.data() + 2, nullptr);   // (stops at the tab)
            cur->addLine(t, l.data() + t2 + 1, l.size() - t2 - 1);
        }
    }
    close();
    std::sort(lrcs_.begin(), lrcs_.end());
}

void LyricIndex::saveCache() {
    const std::string tmp = cache_ + ".new";
    std::string out;   // made in memory, written at once (a line at a time is slow on the SD card)
    char t[24];
    for (const Song& s : songs_) {
        if (!s.read || s.stale) continue;   // (one still to read again: read at the next start, not kept as it was)
        out += "S\t"; out += s.path; out += '\t'; out += s.key; out += '\n';
        for (const Line& l : s.lines) {
            snprintf(t, sizeof t, "%.2f", double(l.t));
            out += "L\t"; out += t; out += '\t'; out += escape(std::string(s.textOf(l))); out += '\n';
        }
    }
    if (lrcKnown_) {   // K: the walk's list is known (even when it holds no .lrc at all)
        out += "K\n";
        for (const std::string& l : lrcs_) { out += "R\t"; out += l; out += '\n'; }
    }
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return; }
    remove(cache_.c_str());
    rename(tmp.c_str(), cache_.c_str());
}

std::vector<LyricIndex::Hit> LyricIndex::find(const std::string& query) const {
    std::vector<std::string> words;
    std::string w;
    for (char c : foldText(query) + " ") {
        if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += c;
    }
    std::vector<Hit> hits;
    if (words.empty()) return hits;
    // a word of the search starts a word in the line ("cry" finds "crying", not "scrying")
    auto startsWord = [](std::string_view hay, const std::string& x) {
        for (size_t p = hay.find(x); p != std::string_view::npos; p = hay.find(x, p + 1))
            if (p == 0 || !wordChar((unsigned char)hay[p - 1])) return true;
        return false;
    };
    for (const Song& s : songs_)
        for (const Line& l : s.lines)
            if (std::all_of(words.begin(), words.end(), [&](const std::string& x) { return startsWord(s.foldOf(l), x); })) {
                hits.push_back({ s.path, std::string(s.textOf(l)), double(l.t) });
                break;
            }
    return hits;
}

void LyricIndex::run() {
    // Only what changed is read (see lyricindex.hpp), slowly, on this worker: the SD card is every thread's (the user, 2026-11-05:
    // "the app is LAGGING SO MUCH after a few seconds of boot"). What the cache holds is there at once; work found at launch waits
    // till Songate has settled; a song read leaves room after it.
    using clock = std::chrono::steady_clock;
    const auto began = clock::now();
#ifdef GF_DEV
    const auto kSettle = std::chrono::seconds(getenv("GF_LYRSETTLE") ? atoi(getenv("GF_LYRSETTLE")) : 20);   // (tests: GF_LYRSETTLE=0)
#else
    constexpr auto kSettle = std::chrono::seconds(20);
#endif
    constexpr auto kLookGap = std::chrono::milliseconds(6), kReadGap = std::chrono::milliseconds(25);
    size_t next = 0;      // where the look for work goes on from
    int sinceSave = 0;
    bool dirty = false, listed = false;   // listed: Songate's lyrics folder listed for this run of work
    bool mustKeep = false;   // Songate's own changes (saved, removed, the playing song) not yet in the cache: kept even at exit
    auto needs = [](const Song& s) { return !s.read || s.stale || s.check; };
    auto pending = [&] {
        for (size_t i = next; i < songs_.size(); i++) if (needs(songs_[i])) { next = i; return true; }
        next = songs_.size();
        return false;
    };
    for (;;) {
        std::unique_lock<std::mutex> lk(m_);
        // a search first: it is someone waiting
        if (haveQuery_) {
            const std::string q = query_;
            const uint64_t t = queryTicket_;
            haveQuery_ = false;
            lk.unlock();
            std::vector<Hit> hits = find(q);
            lk.lock();
            done_ = std::move(hits);
            doneTicket_ = t;
            haveDone_ = true;
            continue;
        }
        if (haveLib_) {   // a new library: the songs kept that are still in it, the new ones to read, the cache for the rest
            std::vector<std::string> paths = std::move(wantLib_);
            haveLib_ = false;
            lk.unlock();
            const bool first = songs_.empty();
            std::unordered_map<std::string, Song> old;
            for (Song& s : songs_) old[s.path] = std::move(s);
            songs_.clear();
            songs_.reserve(paths.size());
            for (const std::string& p : paths) {
                auto it = old.find(p);
                if (it != old.end()) songs_.push_back(std::move(it->second));
                else { Song s; s.path = p; songs_.push_back(std::move(s)); }
            }
            if (first) loadCache();
            next = 0;
            total_ = int(songs_.size());
            int r = 0;
            for (const Song& s : songs_) r += s.read && !s.stale;
            read_ = r;
            continue;
        }
        if (haveLrc_ && !songs_.empty()) {   // (after the library: the songs are there to match)
            std::vector<std::string> l = std::move(wantLrc_);
            haveLrc_ = false;
            lk.unlock();
            const bool was = lrcKnown_;
            applySidecars(std::move(l));
            if (!was) dirty = true;   // the first list: kept
            next = 0;
            continue;
        }
        if (wantCheck_) {
            wantCheck_ = false;
            lk.unlock();
            lyrnet::trace("index: read all again");
            for (Song& s : songs_) s.check = true;
            checked_ = 0;
            checking_ = !songs_.empty();
            listed = false;
            next = 0;
            continue;
        }
        if (!refresh_.empty() || !offers_.empty()) {
            std::vector<std::string> paths;
            paths.swap(refresh_);
            std::vector<std::pair<std::string, std::string>> offers;
            offers.swap(offers_);
            lk.unlock();
            if (!paths.empty()) listSaved();   // (a copy just saved or removed there)
            mustKeep = true;
            for (const std::string& p : paths)
                for (Song& s : songs_)
                    if (s.path == p) {
                        const bool counted = s.read && !s.stale;
                        s.key = keyOf(p);
                        readSong(s);
                        if (!counted) read_++;
                        dirty = true;
                    }
            for (auto& [p, text] : offers)   // the playing song: as playback read it (an .lrc or a tag changed elsewhere)
                for (Song& s : songs_)
                    if (s.path == p && s.read && !s.stale) {
                        Song fresh;
                        parse(text, fresh);
                        const bool same = fresh.text == s.text && fresh.lines.size() == s.lines.size() &&
                                          std::equal(fresh.lines.begin(), fresh.lines.end(), s.lines.begin(), [](const Line& x, const Line& y) {
                                              return std::abs(x.t - y.t) < 0.01f;
                                          });
                        if (!same) {
                            lyrnet::trace("index: the playing song differs: " + p);
                            if (!listed) { listSaved(); listed = true; }
                            s.text = std::move(fresh.text);
                            s.fold = std::move(fresh.fold);
                            s.lines = std::move(fresh.lines);
                            s.key = keyOf(p);
                            dirty = true;
                        }
                    }
            continue;
        }
        if (quit_) break;
        // the next song with work: new, its .lrc came or went, or "Read all again"
        if (!paused_ && !held_ && pending()) {
            if (clock::now() - began < kSettle) {
                cv_.wait_until(lk, began + kSettle, [&] { return quit_ || haveQuery_ || haveLib_ || haveLrc_ || wantCheck_ || !refresh_.empty() || !offers_.empty(); });
                continue;
            }
            cv_.wait_for(lk, kLookGap, [&] { return quit_ || haveQuery_; });   // (a search goes first; quitting does not wait)
            if (quit_ || haveQuery_) continue;
            lk.unlock();
            if (!listed) { listSaved(); listed = true; }   // Songate's lyrics folder, once for this run of work
            Song& s = songs_[next++];
            const bool counted = s.read && !s.stale;
            const std::string k = keyOf(s.path);
            const bool checking = s.check;
            s.check = false;
            if (!counted || s.key != k) {
                s.key = k;
                readSong(s);
                if (!counted) read_++;
                dirty = true;
                std::this_thread::sleep_for(kReadGap);   // (room for the player and the covers between songs read)
                if (++sinceSave >= 300) { saveCache(); sinceSave = 0; dirty = false; mustKeep = false; }
            }
            if (checking) checked_++;
            continue;
        }
        // nothing to do: what changed kept, the check over
        if (dirty) { lk.unlock(); saveCache(); dirty = false; mustKeep = false; sinceSave = 0; continue; }
        checking_ = false;
        listed = false;
        cv_.wait(lk, [&] { return quit_ || haveQuery_ || haveLib_ || (haveLrc_ && !songs_.empty()) || wantCheck_ || !refresh_.empty() || !offers_.empty() || (!paused_ && !held_ && pending()); });
        if (quit_) break;
    }
    // At exit only Songate's own changes are worth the wait: songs read since the last save (at most 300) are read again next time;
    // the whole index written as Songate closed took about 1 s on the console (shutdown-trace.txt, 2026-11-05)
    if (dirty && mustKeep) saveCache();
}
