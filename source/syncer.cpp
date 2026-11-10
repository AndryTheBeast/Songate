/*
 * Songate · syncer.cpp
 * See syncer.hpp. Messages on the link, by their first byte: S let us sync, H what I have heard, I some records, E that was all,
 * X I stopped, W I am syncing with another device (try again soon). Each is text: Ledger's own lines, so nothing here is a second
 * format. With several devices linked, each sync is with one of them; one that asks meanwhile is told W.
 *
 * Then measurements and covers, which are results and not records, and heavy: only now and then, not with every like. G would you
 * like to, 1 or 0: both must. Q what I have no measurements for, and my versions; R those I
 * have that you asked for; A the covers I have, and my size; F one cover; Z nothing more from me. Each is kept as it arrives, so
 * a sync stopped here has lost nothing, and the next one sends what is still missing.
 */
#include "syncer.hpp"
#include "lyricsnet.hpp"
#include "metadata.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>

#include "connect.hpp"
#include "cores.hpp"
#include "safefile.hpp"
#include "filecopy.hpp"
#include "identity.hpp"
#include "levels.hpp"
#include "metacache.hpp"
#include "mixan.hpp"
#include "store.hpp"

namespace {

constexpr size_t kChunk = 32 * 1024;
constexpr double kWaitPeer = 240;   // the other device may be reading its library for the first time
constexpr double kWaitData = 45;

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string readAll(const std::string& p) {
    std::string out;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return out;
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    fclose(f);
    return out;
}

bool writeAll(const std::string& p, const std::string& data) {
    const std::string tmp = p + ".tmp";   // next to it and renamed over it: a power cut never loses the old one
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return false; }
    return safefile::swapIn(tmp, p);
}

template <typename F>
void eachLine(const std::string& t, F f) {
    size_t a = 0;
    while (a < t.size()) {
        size_t b = t.find('\n', a);
        if (b == std::string::npos) b = t.size();
        if (b > a) f(t.substr(a, b - a));
        a = b + 1;
    }
}

std::vector<uint8_t> message(char kind, const std::string& text) {
    std::vector<uint8_t> m;
    m.reserve(text.size() + 1);
    m.push_back(uint8_t(kind));
    m.insert(m.end(), text.begin(), text.end());
    return m;
}

std::string haveText(const Ledger::Have& h) {
    std::string t;
    for (const auto& kv : h) t += kv.first + "\t" + std::to_string(kv.second) + "\n";
    return t;
}

Ledger::Have haveOf(const std::string& t) {
    Ledger::Have h;
    eachLine(t, [&](const std::string& l) {
        const size_t tab = l.find('\t');
        if (tab != std::string::npos) h[l.substr(0, tab)] = strtoull(l.c_str() + tab + 1, nullptr, 10);
    });
    return h;
}

}  // namespace

void Syncer::start(const std::string& dir, Connect* connect, Identity* identity, Levels* levels, Copier* copier) {
    levels_ = levels;
    copier_ = copier;
    dir_ = dir;
    connect_ = connect;
    identity_ = identity;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void Syncer::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Syncer::noteGone(const std::string& song) {
    std::lock_guard<std::mutex> g(m_);
    goneQueue_.push_back(song);
    wake_.notify_all();
}

void Syncer::noteLyricsGone(const std::string& path) {
    std::lock_guard<std::mutex> g(m_);
    lyricsGoneQueue_.push_back(path);
}

bool Syncer::begin(Job job) {
    std::lock_guard<std::mutex> g(m_);
    if (have_) return false;
    job_ = std::move(job);
    have_ = true;
    view_ = View{};
    view_.state = State::Reading;
    view_.peer = job_.peerName;
    wake_.notify_all();
    return true;
}

Syncer::View Syncer::view() {
    std::lock_guard<std::mutex> g(m_);
    return view_;
}

std::string Syncer::asked() {
    std::lock_guard<std::mutex> g(m_);
    if (asked_.empty()) return {};
    const std::string a = asked_.front();
    asked_.erase(asked_.begin());
    return a;
}

void Syncer::accept() {
    std::lock_guard<std::mutex> g(m_);
    answered_ = accepted_ = true;
    wake_.notify_all();
}

void Syncer::decline() {
    std::lock_guard<std::mutex> g(m_);
    if (answered_) return;
    answered_ = true;
    accepted_ = false;
    wake_.notify_all();
}

bool Syncer::takeChanges(syncdata::Changes& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!changesReady_) return false;
    changesReady_ = false;
    out = std::move(changes_);
    return true;
}

void Syncer::applied() {
    std::lock_guard<std::mutex> g(m_);
    appliedDone_ = true;
    wake_.notify_all();
}

void Syncer::fail(const std::string& why) {
    std::lock_guard<std::mutex> g(m_);
    view_.state = State::Failed;
    view_.error = why;
}

bool Syncer::met(const std::string& peerId) {
    std::lock_guard<std::mutex> g(m_);
    return std::find(met_.begin(), met_.end(), peerId) != met_.end();
}

void Syncer::load() {
    safefile::recoverDir(dir_);   // a write cut between its renames
    book_.load(readAll(dir_ + "/ledger.tsv"));
    ids_.clear();
    eachLine(readAll(dir_ + "/sync-lists.tsv"), [&](const std::string& l) {
        const size_t tab = l.find('\t');
        if (tab != std::string::npos) ids_[l.substr(0, tab)] = l.substr(tab + 1);
    });
    seen_ = {};
    eachLine(readAll(dir_ + "/sync-seen.tsv"), [&](const std::string& l) {
        if (l.size() > 2 && l[0] == 'k') seen_.keys.insert(l.substr(2));
        else if (l.size() > 2 && l[0] == 's') seen_.songs.insert(l.substr(2));
    });
    met_.clear();
    eachLine(readAll(dir_ + "/sync-peers.txt"), [&](const std::string& l) { met_.push_back(l); });
}

void Syncer::saveBook() {
    writeAll(dir_ + "/ledger.tsv", book_.serialise());
    std::string t;
    for (const auto& kv : ids_) t += kv.first + "\t" + kv.second + "\n";
    writeAll(dir_ + "/sync-lists.tsv", t);
}

void Syncer::saveState() {
    std::string t;
    for (const auto& k : seen_.keys) t += "k " + k + "\n";
    for (const auto& s : seen_.songs) t += "s " + s + "\n";
    writeAll(dir_ + "/sync-seen.tsv", t);
    std::string p;
    for (const auto& m : met_) p += m + "\n";
    writeAll(dir_ + "/sync-peers.txt", p);
}

/** Takes in what the link has brought, up to `wait` seconds. False once it is time to stop. */
bool Syncer::pump(double wait) {
    const double until = seconds() + wait;
    for (;;) {
        auto got = connect_->receive(peer_);
        for (auto& m : got) inbox_.push_back(std::move(m));
        // another device asking meanwhile: told to try again soon (its other messages are for that sync, which it ends)
        for (auto& f : connect_->receiveAll(peer_))
            if (!f.msg.empty() && char(f.msg[0]) == 'S') connect_->send(f.peer, message('W', ""));
        if (!inbox_.empty()) return true;
        {
            std::unique_lock<std::mutex> g(m_);
            if (quit_) return false;
            wake_.wait_for(g, std::chrono::milliseconds(50));
        }
        if (seconds() > until) return false;
    }
}

bool Syncer::trade(const Job& job, const Ledger::Have& mine, Ledger::Have& theirs, std::vector<Ledger::Entry>& in) {
    if (!job.answering) connect_->send(job.peerId, message('S', ""));
    connect_->send(job.peerId, message('H', haveText(mine)));
    bool gotHave = false, gotEnd = false, sentAll = false;
    std::vector<Ledger::Entry> out;
    size_t at = 0;
    std::string chunk;
    double lastHeard = seconds();
    while (!gotEnd || !sentAll) {
        if (!connect_->linkedTo(job.peerId)) { fail("The link to " + job.peerName + " was lost: nothing changed"); return false; }
        if (gotHave && !sentAll && connect_->outboxBytes(job.peerId) < 256 * 1024) {
            // one chunk at a time, while the other way keeps flowing
            chunk.clear();
            while (at < out.size() && chunk.size() < kChunk) {
                chunk += Ledger::packEntry(out[at++]);
            }
            if (!chunk.empty()) {
                connect_->send(job.peerId, message('I', chunk));
                std::lock_guard<std::mutex> g(m_);
                view_.sent = int(at);
            }
            if (at >= out.size()) {
                connect_->send(job.peerId, message('E', std::to_string(out.size())));
                sentAll = true;
            }
        }
        if (!pump(gotHave && !sentAll ? 0.02 : 0.2)) {
            {
                std::lock_guard<std::mutex> g(m_);
                if (quit_) return false;
            }
            if (seconds() - lastHeard > (gotHave ? kWaitData : kWaitPeer)) { fail(job.peerName + " stopped answering: nothing changed"); return false; }
            continue;
        }
        std::vector<std::vector<uint8_t>> msgs;
        msgs.swap(inbox_);
        lastHeard = seconds();
        for (const auto& m : msgs) {
            if (m.empty()) continue;
            const std::string text(m.begin() + 1, m.end());
            switch (char(m[0])) {
            case 'H':
                theirs = haveOf(text);
                gotHave = true;
                out = book_.since(theirs);
                break;
            case 'I':
                eachLine(text, [&](const std::string& l) {
                    Ledger::Entry e;
                    if (Ledger::unpackEntry(l, e)) in.push_back(std::move(e));
                });
                {
                    std::lock_guard<std::mutex> g(m_);
                    view_.received = int(in.size());
                }
                break;
            case 'E':
                if (strtoull(text.c_str(), nullptr, 10) != in.size()) { fail("Some of it did not arrive: nothing changed"); return false; }
                gotEnd = true;
                break;
            case 'X':
                fail(job.peerName + " stopped the sync: nothing changed");
                return false;
            case 'W': {
                fail(job.peerName + " is syncing with another device");
                std::lock_guard<std::mutex> g(m_);
                view_.busy = true;
                return false;
            }
            default:
                later_.push_back(m);   // their G (they finished first), or anything for a later step
                break;
            }
        }
    }
    return true;
}

void Syncer::work(Job job) {
    peer_ = job.peerId;
    {   // what this device said before the sync began here (its H): for this sync
        std::lock_guard<std::mutex> g(m_);
        inbox_.clear();
        auto it = early_.find(job.peerId);
        if (it != early_.end()) { inbox_ = std::move(it->second); early_.erase(it); }
    }
    // the songs' ids here, off the main thread
    for (syncdata::Song& s : job.now.songs) {
        Identity::Known k;
        if (identity_->find(s.path, k)) { s.id = std::move(k.loose); s.exact = std::move(k.exact); s.size = k.size; }
    }
    // the lyrics Songate kept here, read here (not on the main thread); and those removed by hand since the last sync, by song
    for (const std::string& k : job.now.lyricsKept) {
        const size_t tab = k.find('\t');
        if (tab == std::string::npos) continue;
        const std::string kind = k.substr(0, tab), path = k.substr(tab + 1);
        std::string text;
        if (kind == "dir") text = lyrnet::readSaved(path);
        else if (kind == "lrc") text = readSidecarLyrics(path);
        else if (kind == "tag") { TrackInfo t; readTrackInfo(path, t, false, true); text = t.lyrics; }
        text = syncdata::lyricsText(text);
        if (!text.empty()) job.now.lyrics[path] = text;
    }
    {
        std::vector<std::string> gone;
        {
            std::lock_guard<std::mutex> g(m_);
            gone.swap(lyricsGoneQueue_);
        }
        for (const std::string& p : gone)
            for (const syncdata::Song& s : job.now.songs)
                if (s.path == p && !s.id.empty()) { job.now.lyricsGone.push_back(s.id); break; }
    }
    load();
    book_.setDevice(job.myId);
    // what was deleted here: kept in a file till it is in the book, so that closing Songate loses none of it
    {
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> g(m_);
            pending.swap(goneQueue_);
        }
        std::string have = readAll(dir_ + "/sync-gone.txt");
        for (const std::string& s : pending) have += s + "\n";
        eachLine(have, [&](const std::string& l) { job.now.gone.push_back(l); });
    }
    // sync-addonly.txt: an older Songate ran on this folder since the last sync (cfgmove.cpp, coming back from one). It keeps no
    // book, so what is missing here may be its reset, not your choice: this sync takes nothing away, and the file goes once it is in
    const bool addOnly = !readAll(dir_ + "/sync-addonly.txt").empty();
    syncdata::scan(book_, job.now, ids_, seen_, job.kinds, addOnly);
    if (job.deleteFrom == ~uint64_t(0)) job.deleteFrom = book_.clock();
    job.now.deleteFrom = job.deleteFrom;
    saveBook();   // true whatever happens next, and the numbers must never be used twice
    remove((dir_ + "/sync-gone.txt").c_str());
    {
        std::lock_guard<std::mutex> g(m_);
        view_.deleteFrom = job.deleteFrom;
    }
    {
        std::vector<filecopy::Item> items;
        items.reserve(job.now.songs.size());
        for (const syncdata::Song& s : job.now.songs) if (!s.exact.empty()) items.push_back({ s.exact, s.id, s.path, s.size, s.duration });
        const bool complete = items.size() == job.now.songs.size() || !identity_->busy();
        std::unordered_set<std::string> gone;
        for (const auto& kv : book_.entries()) if (kv.first[0] == 'X' && kv.second.value == "1") gone.insert(kv.first.substr(1));
        copier_->library(std::move(items), std::move(gone), complete);
    }
    {
        std::lock_guard<std::mutex> g(m_);
        view_.state = State::Trading;
    }
    Ledger::Have theirs;
    std::vector<Ledger::Entry> in;
    if (!trade(job, book_.have(), theirs, in)) { connect_->send(job.peerId, message('X', "")); return; }

    Ledger merged = book_;
    merged.merge(in, theirs, job.peerName, job.myName);
    syncdata::ListIds ids = ids_;
    syncdata::Seen seen = seen_;
    syncdata::Changes changes = syncdata::plan(merged, job.now, ids, seen, job.kinds);

    const bool first = std::find(met_.begin(), met_.end(), job.peerId) == met_.end();
    if (first) {
        {
            std::unique_lock<std::mutex> g(m_);
            answered_ = accepted_ = false;
            view_.state = State::Asking;
            view_.summary = changes.summary();
            connect_->send(job.peerId, message('Q', ""));   // "asking here": the other device says it waits for this one, and waits longer
            wake_.wait(g, [&] { return quit_ || answered_; });
            if (quit_) return;
            if (!accepted_) {
                view_.state = State::Failed;
                view_.error = "Not synced: nothing changed";
                connect_->send(job.peerId, message('G', "0"));   // (it need not wait for this one's share)
                return;
            }
            view_.state = State::Backing;
        }
        if (backup && !backup()) { fail("The settings could not be backed up: nothing changed"); return; }
    }
    const std::string said = changes.summary();
    {
        std::unique_lock<std::mutex> g(m_);
        view_.state = State::Applying;
        changes_ = std::move(changes);
        changesReady_ = true;
        appliedDone_ = false;
        wake_.wait(g, [&] { return quit_ || appliedDone_; });
        if (quit_ && !appliedDone_) return;   // left unsaved: the next sync does it again
    }
    Store::flush();   // on the card before the book says it is done; here, so that the main thread never waits for it
    book_ = std::move(merged);
    ids_ = std::move(ids);
    seen_ = std::move(seen);
    if (first) { std::lock_guard<std::mutex> g(m_); met_.push_back(job.peerId); }
    saveBook();
    saveState();
    if (addOnly) remove((dir_ + "/sync-addonly.txt").c_str());
    // both, or neither: one sharing alone would wait for an answer that never comes
    connect_->send(job.peerId, message('G', job.share ? "1" : "0"));
    bool theyWill = false, heard = false;
    double until = seconds() + 10;
    // their answer, wherever it waits: kept by the trade if it came early, else as it arrives; the rest kept for later
    auto takeG = [&](std::vector<std::vector<uint8_t>>& from) {
        std::vector<std::vector<uint8_t>> got;
        got.swap(from);
        for (auto& m : got) {
            if (!heard && !m.empty() && char(m[0]) == 'G') { heard = true; theyWill = m.size() > 1 && m[1] == '1'; }
            else if (!m.empty() && char(m[0]) == 'Q') {   // its user is being asked about its first sync: two minutes for the answer
                until = seconds() + 120;
                std::lock_guard<std::mutex> g(m_);
                view_.peerAsks = true;
            }
            else later_.push_back(std::move(m));
        }
    };
    takeG(later_);
    while (!heard && seconds() < until && connect_->linkedTo(job.peerId)) {
        {
            std::lock_guard<std::mutex> g(m_);
            if (quit_) break;
        }
        if (!pump(0.2)) continue;
        takeG(inbox_);
    }
    {
        std::lock_guard<std::mutex> g(m_);
        view_.peerAsks = false;
    }
    if (job.share && theyWill) {
        {
            std::lock_guard<std::mutex> g(m_);
            view_.state = State::Sharing;
            view_.shared = true;
        }
        share(job);
    }
    std::lock_guard<std::mutex> g(m_);
    if (view_.state != State::Failed) view_.state = State::Done;
    view_.summary = said;
}

/** What the disk holds, by path, as written: read here, never by the main thread. */
static void measuredOnDisk(const std::string& dir, std::unordered_map<std::string, std::string>& loud, std::unordered_map<std::string, std::string>& mix,
                           std::unordered_map<std::string, std::string>& mood) {
    auto read = [&](const char* file, std::unordered_map<std::string, std::string>& to) {
        eachLine(readAll(dir + "/" + file), [&](const std::string& l) {
            const size_t t = l.find('\t');
            if (t != std::string::npos && t) to[l.substr(0, t)] = l.substr(t + 1);
        });
    };
    read("loudness.tsv", loud);
    read("mix.tsv", mix);
    read("mood.tsv", mood);
}

/** Measurements, both ways, for exactly the same files of the same versions; covers, only down to a device that keeps them smaller. */
void Syncer::share(const Job& job) {
    std::unordered_map<std::string, std::string> loud, mix, mood;
    measuredOnDisk(dir_, loud, mix, mood);
    std::unordered_map<std::string, std::vector<std::string>> byExact;   // a file held twice here is measured twice otherwise
    std::string lacking = std::to_string(mixan::Version) + " " + std::to_string(mixan::Mood::Version) + "\n";
    std::unordered_set<std::string> asked;
    for (const syncdata::Song& s : job.now.songs) {
        if (s.exact.empty()) continue;
        byExact[s.exact].push_back(s.path);
        if (mix.count(s.path) && loud.count(s.path)) continue;
        if (asked.insert(s.exact).second) lacking += s.exact + "\n";
    }
    connect_->send(job.peerId, message('Q', lacking));

    const std::string artDir = dir_ + "/art";
    std::string art = std::to_string(MetaCache::ThumbSize) + "\n";
    std::vector<std::string> mine;
    if (DIR* d = opendir(artDir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".jpg") == 0) { mine.push_back(n); art += n + "\n"; }
        }
        closedir(d);
    }
    connect_->send(job.peerId, message('A', art));

    std::vector<std::string> sendExact, sendArt;
    int sendSize = 0;
    size_t exactAt = 0, artAt = 0;
    bool gotAsk = false, gotArt = false, theyEnded = false, ended = false;
    double lastHeard = seconds();
    int measures = 0, covers = 0;
    while (!theyEnded || !ended) {
        if (!connect_->linkedTo(job.peerId)) return;   // kept what came; the next sync sends the rest
        {
            std::lock_guard<std::mutex> g(m_);
            if (quit_) return;
        }
        if (connect_->outboxBytes(job.peerId) < 256 * 1024) {
            std::string chunk;
            while (exactAt < sendExact.size() && chunk.size() < kChunk) {
                const std::string& x = sendExact[exactAt++];
                const auto it = byExact.find(x);
                if (it == byExact.end()) continue;
                for (const std::string& p : it->second) {
                    const auto m = mix.find(p);
                    const auto l = loud.find(p);
                    if (m == mix.end() || l == loud.end()) continue;
                    const auto o = mood.find(p);
                    chunk += x + "\n" + l->second + "\n" + m->second + "\n" + (o == mood.end() ? std::string() : o->second) + "\n";
                    break;
                }
            }
            if (!chunk.empty()) connect_->send(job.peerId, message('R', chunk));
            else if (artAt < sendArt.size()) {
                const std::string& n = sendArt[artAt++];
                const std::string raw = readAll(artDir + "/" + n);
                std::vector<uint8_t> small = MetaCache::coverAtSize(std::vector<uint8_t>(raw.begin(), raw.end()), sendSize);
                if (!small.empty()) {
                    std::vector<uint8_t> m;
                    m.push_back(uint8_t('F'));
                    m.insert(m.end(), n.begin(), n.end());
                    m.push_back(uint8_t('\n'));
                    m.insert(m.end(), small.begin(), small.end());
                    connect_->send(job.peerId, std::move(m));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));   // music comes first
            } else if (gotAsk && gotArt && !ended) {
                connect_->send(job.peerId, message('Z', ""));
                ended = true;
            }
        }
        const bool busy = exactAt < sendExact.size() || artAt < sendArt.size();
        if (busy) lastHeard = seconds();   // sending is not silence
        if (inbox_.empty() && !pump(busy ? 0.0 : 0.2)) {
            if (seconds() - lastHeard > kWaitData) return;
            continue;
        }
        std::vector<std::vector<uint8_t>> msgs;
        msgs.swap(inbox_);
        lastHeard = seconds();
        bool newSync = false;
        for (const auto& m : msgs) {
            if (m.empty()) continue;
            const char kind = char(m[0]);
            if (kind == 'F') {
                const auto nl = std::find(m.begin() + 1, m.end(), uint8_t('\n'));
                if (nl == m.end()) continue;
                const std::string name(m.begin() + 1, nl);
                // only a name of ours: a hash and .jpg, never a path
                bool safe = name.size() > 4 && name.size() <= 32 && name.compare(name.size() - 4, 4, ".jpg") == 0;
                for (char c : name.substr(0, name.size() - 4)) safe = safe && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
                const size_t bytes = size_t(m.end() - nl) - 1;
                if (!safe || bytes < 4 || bytes > 4 * 1024 * 1024) continue;
                const std::string path = artDir + "/" + name;
                struct stat st;
                if (stat(path.c_str(), &st) == 0) continue;   // ours stands
                mkdir(artDir.c_str(), 0777);
                if (writeAll(path, std::string(nl + 1, m.end()))) {
                    covers++;
                    std::lock_guard<std::mutex> g(m_);
                    view_.covers = covers;
                }
                continue;
            }
            const std::string text(m.begin() + 1, m.end());
            if (kind == 'Q') {
                gotAsk = true;
                const size_t nl = text.find('\n');
                const std::string want = std::to_string(mixan::Version) + " " + std::to_string(mixan::Mood::Version);
                if (nl == std::string::npos || text.substr(0, nl) != want) continue;   // another version's results are no use to it
                std::string one;
                eachLine(text.substr(nl + 1), [&](const std::string& l) { sendExact.push_back(l); });
            } else if (kind == 'A') {
                gotArt = true;
                std::unordered_set<std::string> theirs;
                bool head = true;
                eachLine(text, [&](const std::string& l) {
                    if (head) { sendSize = atoi(l.c_str()); head = false; }
                    else theirs.insert(l);
                });
                // down only: a small cover would make the other device's blurred, and it can make its own
                if (sendSize > 0 && sendSize <= MetaCache::ThumbSize)
                    for (const std::string& n : mine) if (!theirs.count(n)) sendArt.push_back(n);
            } else if (kind == 'R') {
                std::vector<std::string> lines;
                size_t a = 0;
                while (a < text.size()) {
                    size_t b = text.find('\n', a);
                    if (b == std::string::npos) b = text.size();
                    lines.push_back(text.substr(a, b - a));
                    a = b + 1;
                }
                for (size_t i = 0; i + 3 < lines.size(); i += 4) {
                    const auto it = byExact.find(lines[i]);
                    if (it == byExact.end()) continue;
                    for (const std::string& p : it->second)
                        if (levels_->adopt(p, lines[i + 1], lines[i + 2], lines[i + 3])) measures++;
                }
                std::lock_guard<std::mutex> g(m_);
                view_.measures = measures;
            } else if (kind == 'Z') {
                theyEnded = true;
            } else if (kind != 'F') {
                // not the sharing's: kept for the step it is for. An S is their next sync: they are not sharing this time
                later_.push_back(m);
                if (kind == 'S') newSync = true;
            }
        }
        if (newSync) return;
    }
}

void Syncer::run() {
    cores::moveHere(cores::FileOps);
    std::unique_lock<std::mutex> g(m_);
    while (!quit_) {
        if (have_) {
            Job job = std::move(job_);
            g.unlock();
            work(std::move(job));
            g.lock();
            have_ = false;
            {
                std::vector<std::vector<uint8_t>> left = std::move(later_);
                later_.clear();
                for (auto& m : inbox_) left.push_back(std::move(m));
                inbox_.clear();
                for (auto& m : left) {
                    if (!m.empty() && char(m[0]) == 'S') asked_.push_back(peer_);   // they began another while this one ended: answered
                    else if (!m.empty() && char(m[0]) == 'H') early_[peer_].push_back(std::move(m));   // its first words, for when it starts
                }
            }
            continue;
        }
        wake_.wait_for(g, std::chrono::milliseconds(200));
        if (quit_ || have_) continue;
        std::vector<std::string> gone;
        gone.swap(goneQueue_);
        g.unlock();
        if (!gone.empty()) {
            if (FILE* f = fopen((dir_ + "/sync-gone.txt").c_str(), "ab")) {
                for (const std::string& s : gone) fprintf(f, "%s\n", s.c_str());
                fclose(f);
            }
        }
        // idle: only "let us sync" matters, from any device
        auto got = connect_->receiveAll();
        g.lock();
        for (auto& f : got) {
            if (f.msg.empty()) continue;
            if (char(f.msg[0]) == 'S') { if (std::find(asked_.begin(), asked_.end(), f.peer) == asked_.end()) asked_.push_back(f.peer); }
            else if (char(f.msg[0]) == 'H') early_[f.peer].push_back(std::move(f.msg));   // ours is not read yet: kept for when it starts
        }
    }
}
