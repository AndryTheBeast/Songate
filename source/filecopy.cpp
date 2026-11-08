/*
 * Songate · filecopy.cpp
 * See filecopy.hpp. Messages on the link, after an f: A what have you got; L some of it; E that was all; G the piece of this file
 * from here on; D a piece; N it cannot be had.
 */
#include "filecopy.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unordered_map>

#include "connect.hpp"
#include "cores.hpp"
#include "identity.hpp"
#include "platform.hpp"

namespace filecopy {
namespace {

constexpr size_t kNameMost = 122;

uint32_t nextUtf8(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    const int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (!more) return c < 0x80 ? c : 0xFFFD;
    uint32_t v = c & (0x3F >> more);
    for (int k = 0; k < more && i < s.size(); k++) v = v << 6 | ((unsigned char)s[i++] & 63);
    return v;
}

/** The plain letters for U+00C0 to U+017F, keeping capitals. */
const char* plainLatin(uint32_t c) {
    static const char* const k[] = {
        "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I","D","N","O","O","O","O","O","","O","U","U","U","U","Y","Th","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","o","","o","u","u","u","u","y","th","y",
        "A","a","A","a","A","a","C","c","C","c","C","c","C","c","D","d","D","d","E","e","E","e","E","e","E","e","E","e","G","g","G","g",
        "G","g","G","g","H","h","H","h","I","i","I","i","I","i","I","i","I","i","IJ","ij","J","j","K","k","k","L","l","L","l","L",
        "l","L","l","L","l","N","n","N","n","N","n","n","N","n","O","o","O","o","O","o","OE","oe","R","r","R","r","R","r",
        "S","s","S","s","S","s","S","s","T","t","T","t","T","t","U","u","U","u","U","u","U","u","U","u","U","u","W","w","Y","y","Y","Z","z","Z","z","Z","z","s" };
    return c >= 0xC0 && c <= 0x17F ? k[c - 0xC0] : nullptr;
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

std::string extOf(const std::string& name) {
    const size_t d = name.find_last_of('.');
    return d == std::string::npos ? std::string() : lower(name.substr(d));
}

}  // namespace

bool hashLike(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

/** A name without its extension, made readable here. */
static std::string cleanName(const std::string& stem, bool forSwitch) {
    std::string out;
    if (forSwitch) {
        for (size_t i = 0; i < stem.size();) {
            const uint32_t c = nextUtf8(stem, i);
            if (c == 0xB4 || c == '`' || c == 0x2018 || c == 0x2019 || c == 0x2B9 || c == 0x2BC || c == 0x2032) { out += '\''; continue; }
            if (c == 0x2013 || c == 0x2014 || c == 0x2212) { out += '-'; continue; }
            if (c == '_' || c == 0xA0 || c == 0x3000) { out += ' '; continue; }
            if (const char* p = plainLatin(c)) { out += p; continue; }
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { out += char(c); continue; }
            if (c < 0x80 && strchr(" !#$%&'()+,-.[]", int(c))) out += char(c);
            // anything else: dropped
        }
    } else {
        for (char c : stem) if (!strchr("<>:\"/\\|?*", c) && (unsigned char)c >= 0x20) out += c;
    }
    // no doubled spaces, none at either end, and no dot at the end: Windows would drop it
    std::string tidy;
    for (char c : out) if (!(c == ' ' && (tidy.empty() || tidy.back() == ' '))) tidy += c;
    while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '.')) tidy.pop_back();
    return tidy;
}

std::string safeName(const std::string& name, bool forSwitch) {
    std::string base = name;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    const std::string ext = base.substr(dot);
    std::string tidy = cleanName(base.substr(0, dot), forSwitch);
    if (tidy.empty()) return {};
    const size_t most = kNameMost - ext.size();
    if (tidy.size() > most) {
        tidy.resize(most);
        while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '.' || ((unsigned char)tidy.back() & 0xC0) == 0x80)) tidy.pop_back();
    }
    return tidy + ext;
}

std::string safeFolders(const std::string& path, bool forSwitch) {
    std::string out;
    int depth = 0;
    size_t a = 0;
    for (;;) {
        const size_t b = path.find_first_of("/\\", a);
        if (b == std::string::npos) break;   // what is left is the file's own name
        const std::string part = path.substr(a, b - a);
        a = b + 1;
        if (part.empty() || part == "." || part == "..") continue;
        std::string c = cleanName(part, forSwitch);
        if (c.size() > kNameMost) c.resize(kNameMost);
        while (!c.empty() && (c.back() == ' ' || c.back() == '.')) c.pop_back();
        if (c.empty() || c == "." || c == "..") continue;
        if (++depth > 8) break;
        out += (out.empty() ? "" : "/") + c;
    }
    return out;
}

std::string freeName(const std::string& name, const std::unordered_set<std::string>& takenLower) {
    if (!takenLower.count(lower(name))) return name;
    const size_t dot = name.find_last_of('.');
    const std::string stem = name.substr(0, dot), ext = dot == std::string::npos ? std::string() : name.substr(dot);
    for (int n = 2;; n++) {
        const std::string tail = " (" + std::to_string(n) + ")";
        std::string s = stem;
        if (s.size() + tail.size() + ext.size() > kNameMost) s.resize(kNameMost - tail.size() - ext.size());
        const std::string c = s + tail + ext;
        if (!takenLower.count(lower(c))) return c;
    }
}

std::vector<Offer> toCopy(const std::vector<Offer>& theirs, const std::vector<Item>& mine) {
    std::unordered_map<std::string, std::vector<double>> here;
    std::unordered_set<std::string> exact;
    for (const Item& i : mine) {
        if (!i.song.empty()) here[i.song].push_back(i.duration);
        exact.insert(i.exact);
    }
    std::vector<Offer> out;
    for (const Offer& o : theirs) {
        const std::string ext = extOf(o.name);
        if (ext != ".mp3" && ext != ".m4a" && ext != ".flac") continue;
        if (o.song.empty() || exact.count(o.exact)) continue;   // no tags: nothing says what it is
        auto it = here.find(o.song);
        const bool held = it != here.end() && std::any_of(it->second.begin(), it->second.end(), [&](double d) { return d - o.duration <= 2 && o.duration - d <= 2; });
        if (held) continue;
        here[o.song].push_back(o.duration);   // and one file of it from there
        out.push_back(o);
    }
    return out;
}

std::string packOffer(const Offer& o) {
    char n[64];
    snprintf(n, sizeof n, "\t%lld\t%.3f\t", (long long)o.size, o.duration);
    return o.exact + "\t" + o.song + n + o.name + "\n";
}

bool unpackOffer(const std::string& line, Offer& o) {
    const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1), c = b == std::string::npos ? b : line.find('\t', b + 1),
                 d = c == std::string::npos ? c : line.find('\t', c + 1);
    if (d == std::string::npos) return false;
    o.exact = line.substr(0, a);
    o.song = line.substr(a + 1, b - a - 1);
    o.size = atoll(line.c_str() + b + 1);
    o.duration = atof(line.c_str() + c + 1);
    o.name = line.substr(d + 1);
    while (!o.name.empty() && (o.name.back() == '\n' || o.name.back() == '\r')) o.name.pop_back();
    const int64_t kMost = int64_t(2) * 1024 * 1024 * 1024;
    return hashLike(o.exact) && (o.song.empty() || hashLike(o.song)) && o.size > 0 && o.size < kMost && !o.name.empty() && o.name.size() < 512;
}

}  // namespace filecopy

using namespace filecopy;

namespace {

constexpr size_t kPiece = 128 * 1024;
constexpr int64_t kKeepFree = int64_t(512) * 1024 * 1024;   // the card or disk is never filled
constexpr double kPlayingRate = 1024 * 1024;                // bytes a second while music plays
constexpr double kAnswerWithin = 20;

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#ifdef GF_WINDOWED
constexpr bool kForSwitch = false;
#else
constexpr bool kForSwitch = true;
#endif

/** The folder here for a file of that path there. */
std::string placeOf(const std::string& root, const std::string& name) {
    const std::string f = safeFolders(name, kForSwitch);
    return f.empty() ? root : root + "/" + f;
}

void send(Connect* c, const std::string& to, char kind, const std::string& text) {
    std::vector<uint8_t> m;
    m.reserve(text.size() + 2);
    m.push_back('f');
    m.push_back(uint8_t(kind));
    m.insert(m.end(), text.begin(), text.end());
    c->send(to, std::move(m));
}

void makeDirs(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
    mkdir(path.c_str(), 0777);
}

}  // namespace

void Copier::start(const std::string& dir, Connect* connect) {
    dir_ = dir;
    connect_ = connect;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void Copier::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (in_) fclose(in_);
    if (out_) fclose(out_);
    in_ = out_ = nullptr;
}

void Copier::library(std::vector<Item> items, std::unordered_set<std::string> gone, bool complete) {
    std::lock_guard<std::mutex> g(m_);
    items_ = std::move(items);
    gone_ = std::move(gone);
    complete_ = complete;
    wake_.notify_all();
}

void Copier::place(const std::string& folder, const std::string& peerId, const std::string& peerName) {
    std::lock_guard<std::mutex> g(m_);
    dest_ = folder;
    peerId_ = peerId;
    peerName_ = peerName;
}

void Copier::accept() {
    std::lock_guard<std::mutex> g(m_);
    answered_ = accepted_ = true;
    wake_.notify_all();
}

void Copier::decline() {
    std::lock_guard<std::mutex> g(m_);
    if (answered_) return;
    answered_ = true;
    accepted_ = false;
    wake_.notify_all();
}

void Copier::pause() {
    std::lock_guard<std::mutex> g(m_);
    paused_ = true;
    wake_.notify_all();
}

Copier::View Copier::view() {
    std::lock_guard<std::mutex> g(m_);
    return view_;
}

bool Copier::takeArrived() {
    std::lock_guard<std::mutex> g(m_);
    const bool a = arrived_;
    arrived_ = false;
    return a;
}

void Copier::fail(const std::string& why) {
    std::lock_guard<std::mutex> g(m_);
    view_.state = State::Failed;
    view_.error = why;
}

void Copier::endSession(const std::string& why) {
    if (out_) { fclose(out_); out_ = nullptr; }
    sessionOn_ = false;
    waiting_ = false;
    triedAt_ = seconds();
    if (!why.empty()) fail(why);
}

/** The other device's asking. Only what is in this library, by hash: there is no way to name anything else. */
void Copier::serve(const std::vector<uint8_t>& m, const std::string& from) {
    const char kind = char(m[1]);
    if (kind == 'A') {
        std::vector<Item> items;
        {
            std::lock_guard<std::mutex> g(m_);
            items = items_;
        }
        std::string chunk;
        for (const Item& i : items) {
            Offer o;
            o.exact = i.exact;
            o.song = i.song;
            o.size = i.size;
            o.duration = i.duration;
            std::string root;
            {
                std::lock_guard<std::mutex> g(m_);
                root = dest_;
            }
            // its place inside the music folder, for the other device to match
            if (!root.empty() && i.path.compare(0, root.size() + 1, root + "/") == 0) o.name = i.path.substr(root.size() + 1);
            else o.name = i.path.substr(i.path.find_last_of('/') + 1);
            chunk += packOffer(o);
            if (chunk.size() > 32 * 1024) { send(connect_, from, 'L', chunk); chunk.clear(); }
        }
        if (!chunk.empty()) send(connect_, from, 'L', chunk);
        send(connect_, from, 'E', "");
    } else if (kind == 'G') {
        const std::string text(m.begin() + 2, m.end());
        const size_t sp = text.find(' ');
        if (sp != 32) return;
        const std::string exact = text.substr(0, sp);
        const int64_t off = atoll(text.c_str() + sp + 1);
        std::string path;
        {
            std::lock_guard<std::mutex> g(m_);
            for (const Item& i : items_) if (i.exact == exact) { path = i.path; break; }
        }
        if (path.empty() || off < 0) { send(connect_, from, 'N', exact); return; }
        if (inExact_ != exact) {
            if (in_) fclose(in_);
            in_ = fopen(path.c_str(), "rb");
            inExact_ = in_ ? exact : std::string();
        }
        if (!in_ || fseek(in_, long(off), SEEK_SET) != 0) { send(connect_, from, 'N', exact); return; }
        std::vector<uint8_t> out(2 + 32 + 16 + kPiece);
        out[0] = 'f';
        out[1] = 'D';
        memcpy(out.data() + 2, exact.data(), 32);
        char o[20];
        snprintf(o, sizeof o, "%016llx", (unsigned long long)off);
        memcpy(out.data() + 34, o, 16);
        const size_t n = fread(out.data() + 50, 1, kPiece, in_);
        out.resize(50 + n);
        connect_->send(from, std::move(out));
        servedAt_ = seconds();
        if (n < kPiece || feof(in_)) {   // all of it: let go at once. Windows cannot delete a file held open
            fclose(in_);
            in_ = nullptr;
            inExact_.clear();
        }
    }
}

void Copier::takeList(const std::string& text) {
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        Offer o;
        if (b > a && unpackOffer(text.substr(a, b - a), o)) theirs_.push_back(std::move(o));
        a = b + 1;
    }
}

/** The list is in: what to copy, whether it fits, whether it may. */
void Copier::begin() {
    std::vector<Item> mine;
    std::unordered_set<std::string> gone;
    std::string dest, peerId, peerName;
    {
        std::lock_guard<std::mutex> g(m_);
        mine = items_;
        gone = gone_;
        dest = dest_;
        peerId = peerId_;
        peerName = peerName_;
    }
    if (!copiedRead_) {
        copiedRead_ = true;
        if (FILE* f = fopen((dir_ + "/copied.txt").c_str(), "rb")) {
            char line[64];
            while (fgets(line, sizeof line, f)) {
                std::string h = line;
                while (!h.empty() && (h.back() == '\n' || h.back() == '\r')) h.pop_back();
                if (hashLike(h)) copied_.insert(h);
            }
            fclose(f);
        }
    }
    {
        std::lock_guard<std::mutex> g(m_);
        view_.offered = int(theirs_.size());
    }
    plan_ = toCopy(theirs_, mine);
    plan_.erase(std::remove_if(plan_.begin(), plan_.end(), [&](const Offer& o) { return copied_.count(o.exact) != 0 || gone.count(o.song) != 0; }), plan_.end());
    theirs_.clear();
    theirs_.shrink_to_fit();
    plan_.erase(std::remove_if(plan_.begin(), plan_.end(), [&](const Offer& o) { return badThisRun_.count(o.exact) != 0; }), plan_.end());
#ifdef GF_DEV
    if (const char* lim = getenv("GF_COPY_LIMIT")) if (plan_.size() > size_t(atoi(lim))) plan_.resize(size_t(atoi(lim)));
#endif
    if (plan_.empty()) { endSession(""); std::lock_guard<std::mutex> g(m_); view_.state = State::Idle; return; }
    int64_t total = 0;
    for (const Offer& o : plan_) total += o.size;
    const int64_t free = plat::freeBytes(dest);
    {
        std::lock_guard<std::mutex> g(m_);
        view_.total = int(plan_.size());
        view_.totalBytes = total;
        view_.free = free;
        view_.done = view_.failed = 0;
        view_.bytes = 0;
    }
    if (free >= 0 && total + kKeepFree > free) {
        // as much as fits, whole files, with room left over
        int64_t room = free - kKeepFree, sum = 0;
        std::vector<Offer> fits;
        for (const Offer& o : plan_) if (sum + o.size <= room) { sum += o.size; fits.push_back(o); }
        if (fits.empty()) { endSession("Not enough free space to copy anything"); return; }
        plan_ = std::move(fits);
        std::lock_guard<std::mutex> g(m_);
        view_.total = int(plan_.size());
        view_.totalBytes = sum;
    }
    {
        std::vector<std::string> met;
        if (FILE* f = fopen((dir_ + "/copy-peers.txt").c_str(), "rb")) {
            char b[4096];
            const size_t n = fread(b, 1, sizeof b, f);
            fclose(f);
            std::string t(b, n);
            size_t a = 0;
            while (a < t.size()) {
                size_t e = t.find('\n', a);
                if (e == std::string::npos) e = t.size();
                if (e > a) met.push_back(t.substr(a, e - a));
                a = e + 1;
            }
        }
        met_ = std::move(met);
    }
    if (std::find(met_.begin(), met_.end(), peerId) == met_.end()) {
        std::unique_lock<std::mutex> g(m_);
        view_.state = State::Asking;
        view_.peer = peerName;
        answered_ = accepted_ = false;
        wake_.wait(g, [&] { return quit_ || answered_; });
        if (quit_) return;
        if (!accepted_) {
            view_.state = State::Idle;
            g.unlock();
            endSession("");
            triedAt_ = seconds() + 3600;   // not asked again for a while
            return;
        }
        g.unlock();
        met_.push_back(peerId);
        std::string t;
        for (const auto& m : met_) t += m + "\n";
        if (FILE* f = fopen((dir_ + "/copy-peers.txt").c_str(), "wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); }
    }
    {
        std::lock_guard<std::mutex> g(m_);
        view_.state = State::Copying;
        view_.peer = peerName;
    }
    at_ = 0;
    nextFile();
}

void Copier::nextFile() {
    if (out_) { fclose(out_); out_ = nullptr; }
    if (at_ >= plan_.size()) {
        endSession("");
        std::lock_guard<std::mutex> g(m_);
        view_.state = State::Done;
        view_.current.clear();
        return;
    }
    std::string dest;
    {
        std::lock_guard<std::mutex> g(m_);
        dest = dest_;
        view_.current = plan_[at_].name;
    }
    const Offer& o = plan_[at_];
    const std::string folder = placeOf(dest, o.name);
    makeDirs(folder);
    const std::string part = folder + "/" + o.exact + ".part";
    struct stat st;
    offset_ = stat(part.c_str(), &st) == 0 ? int64_t(st.st_size) : 0;   // carries on where a drop left it
    if (offset_ > o.size) { remove(part.c_str()); offset_ = 0; }
    out_ = fopen(part.c_str(), offset_ ? "r+b" : "wb");
    if (!out_) { endSession("Could not write to the folder for received music"); return; }
    fseek(out_, long(offset_), SEEK_SET);
    setvbuf(out_, nullptr, _IOFBF, 256 * 1024);
    if (offset_ >= o.size) { finishFile(); return; }
    askChunk();
}

void Copier::askChunk() {
    connect_->hurry();
    send(connect_, sessionPeer_, 'G', plan_[at_].exact + " " + std::to_string((long long)offset_));
    waiting_ = true;
    askedAt_ = seconds();
}

void Copier::chunk(const std::vector<uint8_t>& m) {
    if (m.size() < 50 || !waiting_ || !out_) return;
    const Offer& o = plan_[at_];
    if (memcmp(m.data() + 2, o.exact.data(), 32) != 0) return;
    char hex[17] = {};
    memcpy(hex, m.data() + 34, 16);
    const int64_t off = int64_t(strtoull(hex, nullptr, 16));
    if (off != offset_) return;
    const size_t n = m.size() - 50;
    if (!n || offset_ + int64_t(n) > o.size) { badThisRun_.insert(o.exact); at_++; nextFile(); return; }
    if (fwrite(m.data() + 50, 1, n, out_) != n) { endSession("Could not write to the folder for received music"); return; }
    offset_ += int64_t(n);
    waiting_ = false;
    {
        std::lock_guard<std::mutex> g(m_);
        view_.bytes += int64_t(n);
    }
    if (offset_ >= o.size) { finishFile(); return; }
    // the brake: music playing, the next piece waits its turn
    nextAskAt_ = playing_ ? seconds() + double(n) / kPlayingRate : 0;
}

/** Whole: checked against its hash, then given its name. Until then it is no file of the library's. */
void Copier::finishFile() {
    fclose(out_);
    out_ = nullptr;
    std::string dest;
    {
        std::lock_guard<std::mutex> g(m_);
        dest = dest_;
    }
    const Offer& o = plan_[at_];
    const std::string folder = placeOf(dest, o.name);
    const std::string part = folder + "/" + o.exact + ".part";
    bool ok = Identity::exactHash(part, o.size) == o.exact;
    if (ok) {
        std::string name = safeName(o.name, kForSwitch);
        if (name.empty()) name = "Song " + o.exact.substr(0, 8) + o.name.substr(o.name.find_last_of('.'));
        // what is in the folder already: no file is ever replaced
        std::unordered_set<std::string> taken;
        if (DIR* d = opendir(folder.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                for (char& c : n) if (c >= 'A' && c <= 'Z') c = char(c + 32);
                taken.insert(n);
            }
            closedir(d);
        }
        name = freeName(name, taken);
        ok = rename(part.c_str(), (folder + "/" + name).c_str()) == 0;
        if (ok) {
            copied_.insert(o.exact);
            if (FILE* f = fopen((dir_ + "/copied.txt").c_str(), "ab")) { fprintf(f, "%s\n", o.exact.c_str()); fclose(f); }
        }
    } else {
        remove(part.c_str());
    }
    {
        std::lock_guard<std::mutex> g(m_);
        if (ok) { view_.done++; arrived_ = true; }
        else view_.failed++;
    }
    if (!ok) badThisRun_.insert(o.exact);
    at_++;
    nextFile();
}

void Copier::run() {
    cores::moveHere(cores::Loud);   // only in time nothing else on the core wants
    for (;;) {
        std::string peerId;
        bool listIn = false, pausedNow = false;
        {
            std::unique_lock<std::mutex> g(m_);
            if (quit_) break;
            wake_.wait_for(g, std::chrono::milliseconds(sessionOn_ || seconds() < activeUntil_ ? 2 : 100));   // serving too: its answers are the speed
            if (quit_) break;
            peerId = peerId_;
            pausedNow = paused_;
            paused_ = false;
        }
        // a copy under way stays with its device (the app may point the next one elsewhere meanwhile)
        const std::string from = sessionOn_ ? sessionPeer_ : peerId;
        const bool linked = !from.empty() && connect_->linkedTo(from);
        for (auto& fm : connect_->receiveFiles()) {
            std::vector<uint8_t>& m = fm.msg;
            if (m.size() < 2) continue;
            activeUntil_ = seconds() + 2;
            const char kind = char(m[1]);
            if (kind == 'A' || kind == 'G') { serve(m, fm.peer); continue; }   // any device may take songs from here
            if (fm.peer != sessionPeer_) continue;   // the rest is the copy's, from its own device only
            if (kind == 'L' && sessionOn_) takeList(std::string(m.begin() + 2, m.end()));
            else if (kind == 'E' && sessionOn_) listIn = true;
            else if (kind == 'D') chunk(m);
            else if (kind == 'N' && waiting_ && sessionOn_) {
                badThisRun_.insert(plan_[at_].exact);
                {
                    std::lock_guard<std::mutex> g(m_);
                    view_.failed++;
                }
                at_++;
                waiting_ = false;
                nextFile();
            }
        }
        if (listIn) { waiting_ = false; begin(); }
        if (in_ && seconds() - servedAt_ > 3) {   // the other device stopped asking partway: not held open for it
            fclose(in_);
            in_ = nullptr;
            inExact_.clear();
        }
        if (!sessionOn_) {
            std::lock_guard<std::mutex> g(m_);
            if (want_ && linked && complete_ && !dest_.empty() && seconds() - triedAt_ > 60 && !pausedNow) {   // no songs yet is a library too
                sessionOn_ = true;
                theirs_.clear();
                view_ = View{};
                view_.state = State::Looking;
                view_.peer = peerName_;
                sessionPeer_ = peerId;
                send(connect_, sessionPeer_, 'A', "");
                waiting_ = true;
                askedAt_ = seconds();
            }
            continue;
        }
        if (pausedNow || !want_) { endSession(""); std::lock_guard<std::mutex> g(m_); view_.state = State::Idle; continue; }
        if (!linked) { endSession(""); std::lock_guard<std::mutex> g(m_); view_.state = State::Idle; view_.current.clear(); continue; }   // the .part stays: next time
        if (waiting_) {
            if (seconds() - askedAt_ > kAnswerWithin) endSession("The other device stopped answering");
            continue;
        }
        if (seconds() >= nextAskAt_ && at_ < plan_.size() && out_) askChunk();
    }
    if (out_) { fclose(out_); out_ = nullptr; }
}
