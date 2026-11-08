/*
 * Songate · lyricsnet.cpp
 * See lyricsnet.hpp.
 */
#include "lyricsnet.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

#include "cores.hpp"
#include "json.hpp"
#include "net.hpp"
#include "tagwrite.hpp"
#include "metadata.hpp"

namespace lyrnet {
namespace {

std::string gFolder;   // setFolder: config/…/lyrics

constexpr const char* kBase = "https://lrclib.net/api/";
constexpr double kGap = 0.35;   // seconds between questions (LRCLIB asks for 200-500 ms)

std::string encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

Found record(const json::Json& j) {
    Found f;
    f.id = j.i("id");
    f.title = j.s("trackName");
    f.artist = j.s("artistName");
    f.album = j.s("albumName");
    f.duration = j["duration"].kind == json::Json::Num ? j["duration"].num : 0;
    f.instrumental = j["instrumental"].kind == json::Json::Bool && j["instrumental"].b;
    f.synced = j.s("syncedLyrics");
    f.plain = j.s("plainLyrics");
    return f;
}

bool usable(const Found& f) { return f.instrumental || !f.text().empty(); }

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

uint64_t fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

}  // namespace

/** Dev builds: what each question asked and got, in lyrics-trace.txt beside Songate's lyrics folder (at most 400 lines a run). */
void trace(const std::string& line) {
#ifdef GF_DEV
    static int n = 0;
    if (gFolder.empty() || ++n > 400) return;
    const size_t slash = gFolder.find_last_of('/');
    if (FILE* f = fopen((gFolder.substr(0, slash == std::string::npos ? 0 : slash) + "/lyrics-trace.txt").c_str(), "ab")) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }
#else
    (void)line;
#endif
}

namespace {

bool writeWhole(const std::string& file, const std::string& text) {
    const std::string tmp = file + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return false; }
    remove(file.c_str());   // (Windows renames over nothing; only Songate's own copy is ever replaced: the caller checks)
    if (rename(tmp.c_str(), file.c_str()) != 0) { remove(tmp.c_str()); return false; }
    return true;
}

}  // namespace

void setFolder(const std::string& dir) { gFolder = dir; }

std::string savedPath(const std::string& songPath) {
    char name[24];
    snprintf(name, sizeof name, "%016llx.lrc", (unsigned long long)fnv(songPath));
    return gFolder + "/" + name;
}

std::string readSaved(const std::string& songPath) {
    if (gFolder.empty()) return {};
    FILE* f = fopen(savedPath(songPath).c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < 256 * 1024) s.append(buf, n);
    fclose(f);
    return s;
}

void Client::ask(const Query& q) {
    {
        std::lock_guard<std::mutex> g(m_);
        auto local = [](const Query& w) { return w.kind != Ask::Lyrics && w.kind != Ask::List; };   // nothing sent: never dropped
        if (!local(q))   // a newer song's question: the older one waiting is no longer wanted
            waiting_.erase(std::remove_if(waiting_.begin(), waiting_.end(), [&](const Query& w) { return !local(w); }), waiting_.end());
        waiting_.push_back(q);
        if (!started_) {
            started_ = true;
            quit_ = false;
            th_ = std::thread([this] { cores::moveHere(cores::Net); run(); });
        }
    }
    cv_.notify_one();
}

bool Client::take(Answer& out) {
    std::lock_guard<std::mutex> g(m_);
    if (done_.empty()) return false;
    out = std::move(done_.front());
    done_.erase(done_.begin());
    return true;
}

void Client::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> g(m_);
    started_ = false;
    waiting_.clear();
}

void Client::run() {
    double nextAt = 0;   // not before this: LRCLIB's pause, or its Retry-After
    for (;;) {
        Query q;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return quit_ || !waiting_.empty(); });
            if (quit_) return;
            // a question to the internet waits its turn; a Save does not (nothing is sent)
            const Ask k0 = waiting_.front().kind;
            const bool saving = k0 == Ask::Save || k0 == Ask::Remove || k0 == Ask::Embed || k0 == Ask::EmbedRun;   // nothing sent: no wait
            if (!saving && now() < nextAt) {
                cv_.wait_for(l, std::chrono::duration<double>(nextAt - now()), [&] { return quit_; });
                if (quit_) return;
                continue;   // (it may have been replaced meanwhile)
            }
            q = waiting_.front();
            waiting_.erase(waiting_.begin());
        }
        Answer a = answer(q);
        if (q.kind == Ask::Save || q.kind == Ask::Remove || q.kind == Ask::EmbedRun || q.kind == Ask::Unembed) {
            writtenLoad();
            a.written = written_;   // what Songate keeps, as it now is (the app's copy)
        }
        if (q.kind == Ask::Lyrics || q.kind == Ask::List) {
            nextAt = now() + kGap;
            if (a.err == "429") {   // too many: wait as long as it says (with up to 30% more), then ask again unless a newer question came
                const double wait = std::max(2.0, double(std::atoi(a.savedTo.c_str())));
                nextAt = now() + wait * (1 + 0.3 * (std::rand() % 100) / 100.0);
                std::lock_guard<std::mutex> g(m_);
                if (std::none_of(waiting_.begin(), waiting_.end(), [](const Query& w) { return w.kind == Ask::Lyrics || w.kind == Ask::List; }))
                    waiting_.insert(waiting_.begin(), q);
                continue;
            }
        }
        std::lock_guard<std::mutex> g(m_);
        if (quit_) return;
        done_.push_back(std::move(a));
    }
}

void Client::insideLoad() {
    if (insideRead_) return;
    insideRead_ = true;
    if (FILE* f = fopen((gFolder + "/inside.txt").c_str(), "rb")) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            std::string p = line;
            while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
            if (!p.empty() && std::find(inside_.begin(), inside_.end(), p) == inside_.end()) inside_.push_back(p);
        }
        fclose(f);
    }
}

void Client::writtenLoad() {
    if (writtenRead_) return;
    writtenRead_ = true;
    if (FILE* f = fopen((gFolder + "/written.tsv").c_str(), "rb")) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.size() > 4 && l[3] == '\t' && std::find(written_.begin(), written_.end(), l) == written_.end()) written_.push_back(l);
        }
        fclose(f);
    }
}

void Client::writtenSet(const std::string& kind, const std::string& path, bool on) {
    writtenLoad();
    const std::string l = kind + "\t" + path;
    const auto it = std::find(written_.begin(), written_.end(), l);
    if (on == (it != written_.end())) return;
    if (on) written_.push_back(l); else written_.erase(it);
    const std::string file = gFolder + "/written.tsv";
    if (written_.empty()) { remove(file.c_str()); return; }
    mkdir(gFolder.c_str(), 0777);
    std::string t;
    for (const auto& w : written_) t += w + "\n";
    writeWhole(file, t);
}

void Client::insideSave() {
    const std::string file = gFolder + "/inside.txt";
    if (inside_.empty()) { remove(file.c_str()); return; }
    mkdir(gFolder.c_str(), 0777);
    std::string t;
    for (const auto& p : inside_) t += p + "\n";
    writeWhole(file, t);
}

Answer Client::answer(const Query& q) {
    Answer a;
    a.kind = q.kind;
    a.ticket = q.ticket;
    a.path = q.path;
    if (q.kind == Ask::SyncSave || q.kind == Ask::SyncRemove) {
        writtenLoad();
        insideLoad();
        auto kept = [&](const char* k) { return std::find(written_.begin(), written_.end(), std::string(k) + "\t" + q.path) != written_.end(); };
        const size_t dot = q.path.find_last_of('.'), slash = q.path.find_last_of('/');
        const std::string lrc = (dot == std::string::npos || (slash != std::string::npos && dot < slash) ? q.path : q.path.substr(0, dot)) + ".lrc";
        if (q.kind == Ask::SyncRemove) {   // only what Songate kept here
            if (kept("dir")) { remove(savedPath(q.path).c_str()); writtenSet("dir", q.path, false); }
            if (kept("lrc")) { remove(lrc.c_str()); writtenSet("lrc", q.path, false); }
            if (kept("tag")) {
                const std::string l = "del\t" + q.path;
                if (std::find(inside_.begin(), inside_.end(), l) == inside_.end()) { inside_.push_back(l); insideSave(); }
            }
            trace("sync: removed " + q.path);
            a.reached = true;
            a.written = written_;
            return a;
        }
        // the song's own lyrics (not Songate's) stay, and the song is noted, so it is not looked at again each sync
        const std::string side = readSidecarLyrics(q.path);
        TrackInfo t;
        readTrackInfo(q.path, t, false, true);
        if ((!side.empty() && !kept("lrc")) || (!t.lyrics.empty() && !kept("tag"))) {
            writtenSet("own", q.path, true);
            trace("sync: " + q.path + " has lyrics of its own, left");
            a.err = "own";
            a.written = written_;
            return a;
        }
        bool ok = false;
        if (q.saveTo == 0 && (kept("lrc") || side.empty())) {   // beside: over Songate's own .lrc, or where there is none
            ok = writeWhole(lrc, q.text);
            if (ok) { writtenSet("lrc", q.path, true); a.savedTo = lrc; }
        }
        if (!ok) {   // Songate's folder (and, for Inside, written in when the song is free)
            mkdir(gFolder.c_str(), 0777);
            ok = writeWhole(savedPath(q.path), q.text);
            if (ok) {
                writtenSet("dir", q.path, true);
                a.savedTo = savedPath(q.path);
                if (q.saveTo == 2 && std::find(inside_.begin(), inside_.end(), q.path) == inside_.end()) { inside_.push_back(q.path); insideSave(); }
            }
        }
        trace("sync: " + q.path + (ok ? " saved to " + a.savedTo : " not saved"));
        a.reached = ok;
        if (!ok) a.err = "Could not save the lyrics";
        a.written = written_;
        return a;
    }
    if (q.kind == Ask::Written) {
        writtenLoad();
        a.written = written_;
        a.reached = true;
        return a;
    }
    if (q.kind == Ask::Unembed) {   // onto the waiting list, to be taken out (only what Songate wrote)
        insideLoad();
        writtenLoad();
        if (std::find(written_.begin(), written_.end(), "tag\t" + q.path) == written_.end()) { a.err = "Songate did not write these lyrics"; return a; }
        const std::string l = "del\t" + q.path;
        for (size_t i = 0; i < inside_.size(); i++) if (inside_[i] == q.path) { inside_.erase(inside_.begin() + long(i)); break; }
        if (std::find(inside_.begin(), inside_.end(), l) == inside_.end()) inside_.push_back(l);
        insideSave();
        trace("inside: to take out of " + q.path);
        a.reached = true;
        return a;
    }
    if (q.kind == Ask::Embed) {   // onto the waiting list (written when the song is no longer playing or next)
        insideLoad();
        if (std::find(inside_.begin(), inside_.end(), q.path) == inside_.end()) { inside_.push_back(q.path); insideSave(); }
        trace("inside: waiting " + q.path);
        a.reached = true;
        return a;
    }
    if (q.kind == Ask::EmbedRun) {   // the first waiting song not busy: written inside it (tagwrite.hpp), Songate's copy then removed
        insideLoad();
        a.path.clear();
        for (size_t i = 0; i < inside_.size(); i++) {
            const bool out = inside_[i].compare(0, 4, "del\t") == 0;
            const std::string p = out ? inside_[i].substr(4) : inside_[i];
            if (std::find(q.busy.begin(), q.busy.end(), p) != q.busy.end()) continue;
            const std::string entry = inside_[i];
            inside_.erase(inside_.begin() + long(i));
            a.path = p;
            if (out) {   // Songate's lyrics taken out of the song (tagwrite: empty lyrics), checked to read back with none
                std::string err;
                a.tookOut = true;
                if (tagwrite::putLyrics(p, std::string(), err)) {
                    a.reached = true;
                    writtenSet("tag", p, false);
                    trace("inside: taken out of " + p);
                } else {
                    a.err = err;
                    trace("inside: " + p + " not taken out: " + err);
                    if (err == "The song is in use") inside_.push_back(entry);
                }
                insideSave();
                return a;
            }
            const std::string text = readSaved(p);
            if (text.empty()) { a.err = "Its lyrics were no longer in Songate's folder"; trace("inside: " + p + ": " + a.err); insideSave(); return a; }
            std::string err;
            if (tagwrite::putLyrics(p, text, err)) {
                remove(savedPath(p).c_str());
                writtenSet("dir", p, false);
                writtenSet("tag", p, true);
                a.reached = true;
                a.savedTo = p;
                trace("inside: written into " + p);
            } else {
                a.err = err;
                trace("inside: " + p + " not written: " + err);
                if (err == "The song is in use") inside_.push_back(p);   // tried again later; anything else: its lyrics stay in Songate's folder
            }
            insideSave();
            return a;
        }
        return a;   // (none to write now)
    }
    if (q.kind == Ask::Remove && q.text == "lrc") {   // an .lrc beside the song: only one Songate made (written.tsv)
        writtenLoad();
        if (std::find(written_.begin(), written_.end(), "lrc\t" + q.path) == written_.end()) { a.err = "Songate did not make this .lrc"; return a; }
        const size_t dot = q.path.find_last_of('.'), slash = q.path.find_last_of('/');
        a.savedTo = (dot == std::string::npos || (slash != std::string::npos && dot < slash) ? q.path : q.path.substr(0, dot)) + ".lrc";
        a.reached = remove(a.savedTo.c_str()) == 0;
        if (a.reached) writtenSet("lrc", q.path, false); else a.err = "Could not remove the .lrc";
        trace("removed " + a.savedTo + (a.reached ? "" : " (failed)"));
        return a;
    }
    if (q.kind == Ask::Remove) {   // only Songate's own copy: never a file in the music folder
        insideLoad();
        for (size_t i = 0; i < inside_.size(); i++) if (inside_[i] == q.path) { inside_.erase(inside_.begin() + long(i)); insideSave(); break; }
        a.savedTo = savedPath(q.path);
        a.reached = remove(a.savedTo.c_str()) == 0;
        if (a.reached) writtenSet("dir", q.path, false);
        if (!a.reached) a.err = "Could not remove the lyrics";
        trace("removed " + a.savedTo + (a.reached ? "" : " (failed)"));
        return a;
    }
    if (q.kind == Ask::Save) {
        const size_t dot = q.path.find_last_of('.'), slash = q.path.find_last_of('/');
        const std::string beside = (dot == std::string::npos || (slash != std::string::npos && dot < slash) ? q.path : q.path.substr(0, dot)) + ".lrc";
        struct stat st;
        if (q.beside && stat(beside.c_str(), &st) != 0 && writeWhole(beside, q.text)) {   // never over an .lrc already there
            writtenSet("lrc", q.path, true);
            a.reached = true;
            a.savedTo = beside;
            return a;
        }
        mkdir(gFolder.c_str(), 0777);
        if (!gFolder.empty() && writeWhole(savedPath(q.path), q.text)) {
            writtenSet("dir", q.path, true);
            a.reached = true;
            a.savedTo = savedPath(q.path);
            return a;
        }
        a.err = "Could not write the lyrics";
        return a;
    }
    std::string body, err;
    long status = 0;
    int retry = 0;
    auto lookup = [&](const std::string& url) {
        const bool ok = net::lookup(url, body, status, retry, err);
        trace("asked " + url + "  -> " + (ok ? "status " + std::to_string(status) + ", " + std::to_string(body.size()) + " bytes" : "failed: " + err));
        if (!ok) { a.err = err; return false; }
        if (status == 429) { a.err = "429"; a.savedTo = std::to_string(retry); return false; }
        if (status != 200 && status != 404) { a.err = "LRCLIB answered " + std::to_string(status); return false; }
        return true;
    };
    const std::string who = "artist_name=" + encode(q.artist) + "&track_name=" + encode(q.title);
    // Synced, else plain (the user's choice): the exact record with words alone is kept back while the search looks for a synced
    // one of the same length (the user, 2026-11-04: "it found them and used the plain text one", a synced one there too)
    Found plainOnly;
    bool havePlain = false;
    if (q.kind == Ask::Lyrics) {
        std::string url = std::string(kBase) + "get?" + who;
        if (!q.album.empty()) url += "&album_name=" + encode(q.album);
        if (q.duration > 0) url += "&duration=" + std::to_string(int(std::lround(q.duration)));
        if (!lookup(url)) return a;
        a.reached = true;
        json::Json j;
        const bool parsed = status == 200 && json::parseJson(body, j) && j.kind == json::Json::Obj;
        if (status == 200) trace(std::string("  get: ") + (parsed ? "a record" : "NOT READ: " + body.substr(0, 160)));
        if (parsed) {
            Found f = record(j);
            trace("  get: " + f.artist + " | " + f.title + " | " + std::to_string(f.duration) + (f.synced.empty() ? " | no synced" : " | synced") +
                  (f.plain.empty() ? " | no plain" : " | plain"));
            if (f.instrumental || !f.synced.empty()) { a.found.push_back(std::move(f)); return a; }
            if (!f.plain.empty()) { plainOnly = std::move(f); havePlain = true; }
        }
    }
    auto settle = [&]() -> Answer& {   // the search gave nothing better: the plain words, if the exact record had them
        if (havePlain) { a.err.clear(); a.reached = true; a.found.assign(1, std::move(plainOnly)); }
        return a;
    };
    // the search: every record for the title and artist (Not these), or the best of them within 2 s of the song's length
    if (!lookup(std::string(kBase) + "search?" + who)) return settle();
    a.reached = true;
    json::Json list;
    if (status != 200 || !json::parseJson(body, list) || list.kind != json::Json::Arr) {
        trace("  search: NOT READ (status " + std::to_string(status) + "): " + body.substr(0, 160));
        return settle();
    }
    std::vector<Found> all;
    for (const auto& j : list.arr) if (j.kind == json::Json::Obj) { Found f = record(j); if (usable(f)) all.push_back(std::move(f)); }
    auto off = [&](const Found& f) { return q.duration > 0 && f.duration > 0 ? std::fabs(f.duration - q.duration) : 0.0; };
    std::stable_sort(all.begin(), all.end(), [&](const Found& x, const Found& y) {
        const bool xa = off(x) <= 2, ya = off(y) <= 2;   // its length first, then synced ones, then the closest
        if (xa != ya) return xa;
        if (x.synced.empty() != y.synced.empty()) return !x.synced.empty();
        return off(x) < off(y);
    });
    trace("  search: " + std::to_string(list.arr.size()) + " records, " + std::to_string(all.size()) + " with words; the song is " + std::to_string(q.duration) + " s");
    for (const auto& f : all) trace("    " + f.artist + " | " + f.title + " | " + std::to_string(f.duration) + " | off " + std::to_string(off(f)) + (f.synced.empty() ? " | plain" : " | synced"));
    if (q.kind == Ask::List) {
        if (all.size() > 20) all.resize(20);
        a.found = std::move(all);
    } else if (!all.empty() && off(all[0]) <= 2 && (!all[0].synced.empty() || !havePlain)) {
        a.found.push_back(std::move(all[0]));   // (a synced one of its length; else plain words only when the exact record had none)
    } else {
        settle();
        trace(a.found.empty() ? "  -> none" : "  -> the exact record's plain words");
        return a;
    }
    trace("  -> " + (a.found.empty() ? std::string("none") : a.found[0].artist + (a.found[0].synced.empty() ? ", plain" : ", synced")));
    return a;
}

}  // namespace lyrnet
