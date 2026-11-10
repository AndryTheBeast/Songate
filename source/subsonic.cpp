/*
 * Songate · subsonic.cpp
 * See subsonic.hpp. The Subsonic API as its documentation gives it (subsonic.org/pages/api.jsp, version 1.16.1), answers in JSON.
 */
#include "subsonic.hpp"

#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#if defined(_WIN32) && !defined(GF_PREVIEW)
#include <direct.h>
#elif !defined(_WIN32)
#include <unistd.h>
#endif

#include "cores.hpp"
#include "coverio.hpp"
#include "json.hpp"
#include "metadata.hpp"
#include "net.hpp"
#include "platform.hpp"

namespace {

// ---- MD5 (RFC 1321): the token is md5(password + salt), hex
struct Md5 {
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
            0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
            0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
            0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
            0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
        static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t t = d;
            d = c;
            c = b;
            const uint32_t x = a + f + K[i] + w[g];
            b = b + (x << R[i] | x >> (32 - R[i]));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    std::string hex(const std::string& msg) {
        std::vector<uint8_t> m(msg.begin(), msg.end());
        const uint64_t bits = uint64_t(m.size()) * 8;
        m.push_back(0x80);
        while (m.size() % 64 != 56) m.push_back(0);
        for (int i = 0; i < 8; i++) m.push_back(uint8_t(bits >> (8 * i)));
        for (size_t o = 0; o < m.size(); o += 64) block(&m[o]);
        std::fill(m.begin(), m.end(), 0);   // (it held the password)
        static const char* x = "0123456789abcdef";
        std::string out;
        for (uint32_t v : h)
            for (int i = 0; i < 4; i++) { const uint8_t byte = uint8_t(v >> (8 * i)); out += x[byte >> 4]; out += x[byte & 15]; }
        return out;
    }
};

void wipe(std::string& s) {
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; p && i < s.size(); i++) p[i] = 0;
    s.clear();
}

std::string urlEncode(const std::string& s) {
    static const char* x = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { o += '%'; o += x[c >> 4]; o += x[c & 15]; }
    }
    return o;
}

/** "nas.local:4533/" → "http://nas.local:4533": a scheme if none, no slash at its end. */
std::string cleanServer(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    if (!s.empty() && s.find("://") == std::string::npos) s = "http://" + s;
    return s;
}

/** The answer's subsonic-response, or why not, in words. */
bool answer(const std::string& body, json::Json& out, std::string& err) {
    json::Json all;
    if (!json::parseJson(body, all)) { err = "The server's answer could not be read"; return false; }
    out = all["subsonic-response"];
    if (out.kind != json::Json::Obj) { err = "Not a Subsonic server"; return false; }
    if (out.s("status") == "ok") return true;
    const int code = int(out["error"].i("code"));
    err = code == 40 || code == 41 ? "Wrong username or password"
        : code == 50 ? "This user may not do that"
        : code == 20 || code == 30 ? "This server's Subsonic version is too old or too new"
        : out["error"].s("message").empty() ? "The server said no" : out["error"].s("message");
    return false;
}

}  // namespace

// ------------------------------------------------------------------ the account

void Subsonic::loadAccount() {
    FILE* f = fopen((dir_ + "/subsonic-account.txt").c_str(), "rb");
    if (!f) return;
    std::string all;
    char b[512];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) all.append(b, n);
    fclose(f);
    std::vector<std::string> l;
    size_t p = 0;
    while (p <= all.size()) {
        const size_t e = all.find('\n', p);
        std::string line = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        l.push_back(line);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    wipe(all);
    if (l.size() >= 4 && !l[0].empty() && !l[1].empty() && !l[2].empty() && l[3].size() == 32) {
        std::lock_guard<std::mutex> g(m_);
        view_.server = l[0];
        view_.user = l[1];
        salt_ = l[2];
        token_ = l[3];
        view_.state = SigningIn;   // (checked again with a ping as the worker starts)
    }
    for (std::string& s : l) wipe(s);
}

void Subsonic::saveAccount() {
    std::string out;
    {
        std::lock_guard<std::mutex> g(m_);
        if (!token_.empty()) out = view_.server + "\n" + view_.user + "\n" + salt_ + "\n" + token_ + "\n";
    }
    const std::string p = dir_ + "/subsonic-account.txt";
    if (out.empty()) { remove(p.c_str()); return; }
    const std::string tmp = p + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        remove(p.c_str());
        rename(tmp.c_str(), p.c_str());
    }
    wipe(out);
}

// ------------------------------------------------------------------ the main thread's side

void Subsonic::start(const std::string& dir) {
    dir_ = dir;
    loadAccount();
    loadDownloads();
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = false;
        if (view_.state == SigningIn) tasks_.push_back({ Task::Ping });
    }
    thread_ = std::thread([this] { run(); });
}

void Subsonic::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    for (auto& d : decoded_) if (d.second) SDL_FreeSurface(d.second);
    decoded_.clear();
}

Subsonic::View Subsonic::view() {
    std::lock_guard<std::mutex> g(m_);
    return view_;
}

void Subsonic::signIn(const std::string& server, const std::string& user, std::string& password) {
    uint8_t r[8];
    plat::randomBytes(r, sizeof r);
    static const char* x = "0123456789abcdef";
    std::string salt;
    for (uint8_t c : r) { salt += x[c >> 4]; salt += x[c & 15]; }
    std::string t = Md5().hex(password + salt);
    wipe(password);
    {
        std::lock_guard<std::mutex> g(m_);
        view_ = View{};
        view_.server = cleanServer(server);
        view_.user = user;
        view_.state = SigningIn;
        view_.gen++;
        salt_ = salt;
        token_ = t;
        sess_++;
        clearSession();
        tasks_.clear();
        tasks_.push_back({ Task::Ping });
    }
    wipe(t);
    wake_.notify_all();
}

void Subsonic::signOut() {
    {
        std::lock_guard<std::mutex> g(m_);
        view_ = View{};
        view_.gen++;
        wipe(token_);
        wipe(salt_);
        tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [](const Task& t) { return t.kind != Task::DlWipe; }), tasks_.end());
        dlDone_ = dlTotal_ = 0;
        sess_++;
        clearSession();
    }
    saveAccount();   // (no token: the file goes)
}

void Subsonic::clearSession() {
    later_.clear();
    starred_.clear();
    starredIds_.clear();
    haveStarred_ = false;
    starGen_++;
    artists_.clear();
    haveArtists_ = false;
    allAlbums_.clear();
    haveAllAlbums_ = false;
    clearLists();
    artistAlbums_.clear();
    albums_.clear();
    covers_.clear();
}

void Subsonic::askArtists() { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::Artists }); wake_.notify_all(); }
void Subsonic::clearLists() {
    playlists_.clear();
    havePlaylists_ = false;
    plGen_++;
    playlistSongs_.clear();
    genres_.clear();
    haveGenres_ = false;
    genreAlbums_.clear();
    folders_.clear();
    haveFolders_ = false;
    dirs_.clear();
    allSongs_.clear();
    haveAllSongs_ = false;
}
void Subsonic::askGenres() { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::Genres }); wake_.notify_all(); }
void Subsonic::askGenreAlbums(const std::string& genre) { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::GenreAlbums, genre }); wake_.notify_all(); }
void Subsonic::askFolders() { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::Folders }); wake_.notify_all(); }
void Subsonic::askDir(const std::string& id) { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::Dir, id }); wake_.notify_all(); }
void Subsonic::askAllSongs() { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::AllSongs }); wake_.notify_all(); }
bool Subsonic::genres(std::vector<Genre>& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!haveGenres_) return false;
    out = genres_;
    return true;
}
bool Subsonic::genreAlbums(const std::string& genre, std::vector<Album>& out) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : genreAlbums_) if (k == genre) { out = v; return true; }
    return false;
}
bool Subsonic::folders(std::vector<Entry>& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!haveFolders_) return false;
    out = folders_;
    return true;
}
bool Subsonic::dir(const std::string& id, std::string& name, std::vector<Entry>& out) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : dirs_) if (k == id) { name = v.first; out = v.second; return true; }
    return false;
}
bool Subsonic::allSongs(std::vector<Song>& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!haveAllSongs_) return false;
    out = allSongs_;
    return true;
}

void Subsonic::askAllAlbums() { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::AllAlbums }); wake_.notify_all(); }
bool Subsonic::allAlbums(std::vector<Album>& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!haveAllAlbums_) return false;
    out = allAlbums_;
    return true;
}
void Subsonic::askArtist(const std::string& id) { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::Artist, id }); wake_.notify_all(); }
void Subsonic::askAlbum(const std::string& id) { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::Album, id }); wake_.notify_all(); }
void Subsonic::askCover(const std::string& id, int size) {
    std::lock_guard<std::mutex> g(m_);
    for (const Task& t : tasks_) if (t.kind == Task::Cover && t.id == id) return;
    tasks_.push_back({ Task::Cover, id, size });
    wake_.notify_all();
}

bool Subsonic::artists(std::vector<Artist>& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!haveArtists_) return false;
    out = artists_;
    return true;
}
bool Subsonic::artist(const std::string& id, std::vector<Album>& out) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : artistAlbums_) if (k == id) { out = v; return true; }
    return false;
}
bool Subsonic::album(const std::string& id, Album& a, std::vector<Song>& songs) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : albums_) if (k == id) { a = v.first; songs = v.second; return true; }
    return false;
}
bool Subsonic::cover(const std::string& id, std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : covers_) if (k == id) { bytes = v; return true; }
    return false;
}

void Subsonic::askSong(const std::string& id, const std::string& suffix, bool now) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : songs_) if (k == id && !v.empty()) return;
    for (Task& t : tasks_) if (t.kind == Task::Song && t.id == id) { t.now = t.now || now; wake_.notify_all(); return; }   // (asked ahead, now wanted)
    Task t{ Task::Song, id, 0, suffix };
    t.now = now;
    tasks_.push_back(t);
    wake_.notify_all();
}

bool Subsonic::fetched(const std::string& id, std::string& path) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : songs_) if (k == id) { path = v; return true; }
    return false;
}

namespace {
std::string lowerOf(std::string s) { for (char& c : s) c = char(tolower(uint8_t(c))); return s; }
Subsonic::Song songOf(const json::Json& s) {
    Subsonic::Song x;
    x.id = s.s("id");
    x.title = s.s("title");
    x.artist = s.s("artist");
    x.artistId = s.s("artistId");
    x.album = s.s("album");
    x.albumId = s.s("albumId");
    x.cover = s.s("coverArt");
    x.suffix = s.s("suffix");
    x.contentType = s.s("contentType");
    x.genre = s.s("genre");
    x.parent = s.s("parent");
    x.track = int(s.i("track"));
    x.disc = int(s.i("discNumber"));
    x.year = int(s.i("year"));
    x.bitRate = int(s.i("bitRate"));
    x.duration = double(s.i("duration"));
    return x;
}
/** A name HOS reads (plain ASCII; others as _), as short as a folder name should be. */
std::string safeName(const std::string& in) {
    std::string o;
    for (unsigned char ch : in) o += (isalnum(ch) || strchr(" -_.,()&'!+[]", ch)) && ch < 0x80 ? char(ch) : '_';
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    if (o.size() > 80) o.resize(80);
    return o.empty() ? std::string("Unknown") : o;
}
/** Every folder down to p made (the app's own mkdir: the preview maps it). */
void makeFolders(const std::string& p) {
    for (size_t i = 1; i <= p.size(); i++)
        if (i == p.size() || p[i] == '/') {
            const std::string part = p.substr(0, i);
            if (part.empty() || part.back() == ':') continue;
#if defined(_WIN32) && !defined(GF_PREVIEW)
            _mkdir(part.c_str());
#else
            mkdir(part.c_str(), 0777);
#endif
        }
}
/** A removed song's album folder, then its artist folder, removed where now empty (rmdir fails on a folder with anything in it). */
void dropEmpty(const std::string& file) {
    std::string d = file.substr(0, file.rfind('/'));
    for (int k = 0; k < 2 && !d.empty(); k++) {
#if defined(_WIN32) && !defined(GF_PREVIEW)
        if (_rmdir(d.c_str()) != 0) break;
#else
        if (rmdir(d.c_str()) != 0) break;
#endif
        d = d.substr(0, d.rfind('/'));
    }
}
struct Cached { long long at = 0, size = 0; std::string file; };
std::vector<Cached> readIndex(const std::string& p) {
    std::vector<Cached> v;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return v;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        Cached c;
        char name[900] = {};
        if (sscanf(line, "%lld\t%lld\t%899[^\r\n]", &c.at, &c.size, name) == 3) { c.file = name; v.push_back(c); }
    }
    fclose(f);
    return v;
}
void writeIndex(const std::string& p, const std::vector<Cached>& v) {
    const std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    for (const Cached& c : v) fprintf(f, "%lld\t%lld\t%s\n", c.at, c.size, c.file.c_str());
    fclose(f);
    remove(p.c_str());
    rename(tmp.c_str(), p.c_str());
}
void makeFolder(const std::string& p) {
#if defined(_WIN32) && !defined(GF_PREVIEW)
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0777);
#endif
}
}  // namespace

void Subsonic::scrobble(const std::string& songId, bool submission) {
    std::lock_guard<std::mutex> g(m_);
    tasks_.push_back({ Task::Scrobble, songId, submission ? 1 : 0, std::to_string((long long)std::time(nullptr) * 1000) });
    wake_.notify_all();
}
void Subsonic::askPlaylists() { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::Playlists }); wake_.notify_all(); }
bool Subsonic::playlists(std::vector<Playlist>& out, unsigned& gen) {
    std::lock_guard<std::mutex> g(m_);
    if (!havePlaylists_) return false;
    out = playlists_;
    gen = plGen_;
    return true;
}
void Subsonic::askPlaylist(const std::string& id) {
    std::lock_guard<std::mutex> g(m_);
    playlistSongs_.erase(std::remove_if(playlistSongs_.begin(), playlistSongs_.end(), [&](const auto& q) { return q.first == id; }), playlistSongs_.end());
    tasks_.push_back({ Task::PlaylistOne, id });
    wake_.notify_all();
}
bool Subsonic::playlist(const std::string& id, Playlist& pl, std::vector<Song>& songs) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : playlistSongs_) if (k == id) { pl = v.first; songs = v.second; return true; }
    return false;
}
void Subsonic::sendPlaylist(const std::string& name, const std::string& existingId, const std::vector<std::string>& songIds) {
    std::string ids;
    for (const auto& i : songIds) ids += (ids.empty() ? "" : ",") + i;
    std::lock_guard<std::mutex> g(m_);
    tasks_.push_back({ Task::SendPlaylist, existingId, 0, name, std::string(), ids });
    wake_.notify_all();
}
void Subsonic::askLyrics(const std::string& songId, const std::string& artist, const std::string& title, const std::string& savedFile) {
    std::lock_guard<std::mutex> g(m_);
    lyrics_.erase(std::remove_if(lyrics_.begin(), lyrics_.end(), [&](const auto& p) { return p.first == songId; }), lyrics_.end());
    tasks_.push_front({ Task::Lyrics, songId, 0, artist, title, savedFile });
    wake_.notify_all();
}
bool Subsonic::lyrics(const std::string& songId, std::string& text, std::string& source) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : lyrics_) if (k == songId) { text = v.first; source = v.second; return true; }
    return false;
}
void Subsonic::askFileInfo(const std::string& songId, const std::string& path) {
    std::lock_guard<std::mutex> g(m_);
    tasks_.push_front({ Task::FileInfoTask, songId, 0, std::string(), std::string(), path });
    wake_.notify_all();
}
bool Subsonic::fileInfo(const std::string& songId, FileInfo& out) {
    std::lock_guard<std::mutex> g(m_);
    for (const auto& [k, v] : fileInfos_) if (k == songId) { out = v; return true; }
    return false;
}
void Subsonic::newPlaylist(const std::string& name, const std::vector<std::string>& songIds) {
    std::string ids;
    for (const auto& i : songIds) ids += (ids.empty() ? "" : ",") + i;
    std::lock_guard<std::mutex> g(m_);
    tasks_.push_back({ Task::PlEdit, std::string(), 0, name, ids });
    wake_.notify_all();
}
void Subsonic::updatePlaylist(const std::string& id, const std::string& newName, const std::vector<std::string>& addIds, const std::vector<int>& removeAt) {
    std::string ids, at;
    for (const auto& i : addIds) ids += (ids.empty() ? "" : ",") + i;
    for (int i : removeAt) at += (at.empty() ? "" : ",") + std::to_string(i);
    std::lock_guard<std::mutex> g(m_);
    tasks_.push_back({ Task::PlEdit, id, 1, newName, ids, at });
    wake_.notify_all();
}
void Subsonic::deletePlaylist(const std::string& id) {
    std::lock_guard<std::mutex> g(m_);
    tasks_.push_back({ Task::PlEdit, id, 2 });
    wake_.notify_all();
}
void Subsonic::askStarred() { std::lock_guard<std::mutex> g(m_); tasks_.push_back({ Task::Starred }); wake_.notify_all(); }
bool Subsonic::starred(std::vector<Song>& out, unsigned& gen) {
    std::lock_guard<std::mutex> g(m_);
    if (!haveStarred_) return false;
    out = starred_;
    gen = starGen_;
    return true;
}
bool Subsonic::isStarred(const std::string& songId) { std::lock_guard<std::mutex> g(m_); return starredIds_.count(songId) > 0; }
void Subsonic::star(const std::string& songId, bool on) {
    std::lock_guard<std::mutex> g(m_);
    if (on == (starredIds_.count(songId) > 0)) return;
    if (on) {
        starredIds_.insert(songId);
        Song x;   // (the list's own copy comes with the next getStarred2; meanwhile its id)
        x.id = songId;
        bool found = false;
        for (const auto& a : allSongs_) if (a.id == songId) { x = a; found = true; break; }
        if (!found) for (const auto& [k, v] : albums_) for (const auto& a : v.second) if (a.id == songId) { x = a; found = true; }
        starred_.insert(starred_.begin(), x);
    } else {
        starredIds_.erase(songId);
        starred_.erase(std::remove_if(starred_.begin(), starred_.end(), [&](const Song& a) { return a.id == songId; }), starred_.end());
    }
    starGen_++;
    view_.gen++;
    tasks_.push_back({ Task::Star, songId, on ? 1 : 0 });
    wake_.notify_all();
}
std::unordered_set<std::string> Subsonic::likeLedger() { std::lock_guard<std::mutex> g(m_); return ledger_; }
void Subsonic::setLikeLedger(std::unordered_set<std::string> keys) {
    std::lock_guard<std::mutex> g(m_);
    ledger_ = std::move(keys);
    tasks_.push_back({ Task::SaveLedger });
    wake_.notify_all();
}

bool Subsonic::runnable() const {
    for (const Task& t : tasks_) if ((t.kind != Task::DlAlbum && t.kind != Task::DlSong) || !dlPaused_) return true;
    return false;
}
void Subsonic::downloadAlbum(const std::string& albumId, const std::string& root) {
    std::lock_guard<std::mutex> g(m_);
    DlAlbumRec& a = dlAlbums_[albumId];
    a.root = root;
    tasks_.push_back({ Task::DlAlbum, albumId, 0, std::string(), std::string(), root });
    dlGen_++;
    wake_.notify_all();
}
void Subsonic::removeDownload(const std::string& albumId) {
    std::lock_guard<std::mutex> g(m_);
    tasks_.push_front({ Task::DlRemove, albumId });
    wake_.notify_all();
}
void Subsonic::pauseDownloads(bool paused) {
    std::lock_guard<std::mutex> g(m_);
    dlPaused_ = paused;
    wake_.notify_all();
}
Subsonic::DlState Subsonic::albumDownload(const std::string& albumId) {
    std::lock_guard<std::mutex> g(m_);
    DlState d;
    auto it = dlAlbums_.find(albumId);
    if (it == dlAlbums_.end()) return d;
    for (const auto& [id, so] : dlSongs_) d.have += so.album == albumId;
    d.total = it->second.total;
    d.state = d.total > 0 && d.have >= d.total ? 2 : 1;
    return d;
}
bool Subsonic::downloaded(const std::string& songId, std::string& path) {
    std::lock_guard<std::mutex> g(m_);
    auto it = dlSongs_.find(songId);
    if (it == dlSongs_.end()) return false;
    path = it->second.path;
    return true;
}
void Subsonic::dlProgress(int& done, int& total) {
    std::lock_guard<std::mutex> g(m_);
    done = dlDone_;
    total = dlTotal_;
}
void Subsonic::dlCounts(int& songs, long long& bytes, int& albums) {
    std::lock_guard<std::mutex> g(m_);
    songs = int(dlSongs_.size());
    bytes = 0;
    for (const auto& [id, so] : dlSongs_) bytes += so.size;
    albums = int(dlAlbums_.size());
}
bool Subsonic::takeBatchDone(bool& toMusic) {
    std::lock_guard<std::mutex> g(m_);
    if (!dlBatchDone_) return false;
    dlBatchDone_ = false;
    toMusic = dlBatchMusic_;
    return true;
}
void Subsonic::wipeLocal(bool downloadsToo) {
    std::lock_guard<std::mutex> g(m_);
    wiping_++;
    tasks_.push_front({ Task::DlWipe, std::string(), downloadsToo ? 1 : 0 });
    wake_.notify_all();
}
bool Subsonic::wiping() { std::lock_guard<std::mutex> g(m_); return wiping_ > 0; }
unsigned Subsonic::dlGen() { std::lock_guard<std::mutex> g(m_); return dlGen_; }
long long Subsonic::cacheBytes() {   // (a small file, read where asked: Settings, the sign-out card)
    long long n = 0;
    for (const Cached& k : readIndex(dir_ + "/server-cache.tsv")) n += k.size;
    return n;
}

/** server-downloads.tsv: "A\tid\troot\ttotal\tname\tartist" for each album wanted, "S\tid\talbum\tsize\tpath" for each song here. */
void Subsonic::loadDownloads() {
    if (FILE* lf = fopen((dir_ + "/server-likes.tsv").c_str(), "rb")) {
        char k[1024];
        std::lock_guard<std::mutex> g(m_);
        while (fgets(k, sizeof k, lf)) {
            std::string key = k;
            while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) key.pop_back();
            if (!key.empty()) ledger_.insert(key);
        }
        fclose(lf);
    }
    FILE* f = fopen((dir_ + "/server-downloads.tsv").c_str(), "rb");
    if (!f) return;
    char line[2048];
    std::lock_guard<std::mutex> g(m_);
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> p;
        std::string cur;
        for (char* q = line; *q && *q != '\r' && *q != '\n'; q++) {
            if (*q == '\t') { p.push_back(cur); cur.clear(); }
            else cur += *q;
        }
        p.push_back(cur);
        if (p.size() >= 6 && p[0] == "A") dlAlbums_[p[1]] = { p[2], p[4], p[5], atoi(p[3].c_str()) };
        else if (p.size() >= 5 && p[0] == "S") dlSongs_[p[1]] = { p[2], atoll(p[3].c_str()), p[4] };
    }
    fclose(f);
}
void Subsonic::saveDownloads() {
    std::string out;
    {
        std::lock_guard<std::mutex> g(m_);
        for (const auto& [id, a] : dlAlbums_)
            out += "A\t" + id + "\t" + a.root + "\t" + std::to_string(a.total) + "\t" + a.name + "\t" + a.artist + "\n";
        for (const auto& [id, so] : dlSongs_) out += "S\t" + id + "\t" + so.album + "\t" + std::to_string(so.size) + "\t" + so.path + "\n";
    }
    const std::string p = dir_ + "/server-downloads.tsv", tmp = p + ".tmp";
    if (out.empty()) { remove(p.c_str()); return; }
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    remove(p.c_str());
    rename(tmp.c_str(), p.c_str());
}

void Subsonic::takeCovers(std::vector<std::pair<std::string, SDL_Surface*>>& out) {
    std::lock_guard<std::mutex> g(m_);
    for (auto& d : decoded_) out.push_back(d);
    decoded_.clear();
}

std::string Subsonic::base(const std::string& method, const std::string& extra) {
    std::lock_guard<std::mutex> g(m_);
    return view_.server + "/rest/" + method + "?u=" + urlEncode(view_.user) + "&t=" + token_ + "&s=" + salt_ + "&v=1.16.1&c=Songate&f=json" + extra;
}

void Subsonic::note(const std::string& s) { (void)s; }

bool Subsonic::call(const std::string& method, const std::string& extra, std::string& body, std::string& err) {
    std::string url = base(method, extra);
    const bool ok = net::get(url, body, err);
    wipe(url);   // (it held the token)
    if (!ok) { err = "Can't reach the server"; unreached_ = true; }
    return ok;
}

// ------------------------------------------------------------------ the worker

void Subsonic::run() {
    cores::moveHere(cores::Net);
    std::unique_lock<std::mutex> g(m_);
    // Lists asked while the server could not be reached wait in later_ and are asked again (2 s, 4 s … 30 s apart): dropped,
    // their pages said "Asking the server…" until the next sign-in
    auto isList = [](Task::Kind k) {
        return k == Task::Artists || k == Task::AllAlbums || k == Task::Genres || k == Task::GenreAlbums || k == Task::Folders || k == Task::Dir
            || k == Task::AllSongs || k == Task::Artist || k == Task::Album || k == Task::Starred || k == Task::Playlists || k == Task::PlaylistOne;
    };
    auto again = [&](const Task& t, unsigned sess) {   // m_ held
        if (!isList(t.kind) || sess != sess_ || token_.empty()) return;
        Task n = t;
        n.tries++;
        later_.push_back({ n, std::chrono::steady_clock::now() + std::chrono::seconds(std::min(30, 1 << std::min(n.tries, 5))) });
    };
    while (!quit_) {
        for (;;) {
            const auto nowAt = std::chrono::steady_clock::now();
            for (size_t i = 0; i < later_.size();) {
                if (later_[i].at <= nowAt) { tasks_.push_back(later_[i].task); later_.erase(later_.begin() + long(i)); }
                else i++;
            }
            if (quit_ || runnable()) break;
            if (later_.empty()) wake_.wait(g);
            else wake_.wait_for(g, std::chrono::milliseconds(500));
        }
        if (quit_) break;
        auto pick = std::find_if(tasks_.begin(), tasks_.end(), [](const Task& k) { return k.kind == Task::Song && k.now; });   // the song wanted now first
        if (pick == tasks_.end()) pick = std::find_if(tasks_.begin(), tasks_.end(), [](const Task& k) { return k.kind != Task::DlAlbum && k.kind != Task::DlSong; });
        if (pick == tasks_.end()) pick = tasks_.begin();   // (downloads: runnable() said they may go)
        const Task t = *pick;
        tasks_.erase(pick);
        const unsigned sess = sess_;
        g.unlock();
        unreached_ = false;
        std::string err, body;
        if (!net::begin(err)) {
            g.lock();   // (held from here, as at the loop's top)
            if (t.kind == Task::Ping) { if (sess == sess_) { view_.state = Failed; view_.error = "No network"; view_.gen++; } }
            else again(t, sess);
            continue;
        }
        json::Json r;
        if (t.kind == Task::Ping) {
            bool ok = call("ping", "", body, err) && answer(body, r, err);
            {
                std::lock_guard<std::mutex> v(m_);
                if (sess != sess_) ok = false;   // signed out, or into another server, meanwhile: that one's own answer counts
                else if (ok) {
                    view_.state = SignedIn;
                    view_.kind = r.s("type");
                    view_.version = r.s("serverVersion");
                    view_.error.clear();
                } else {
                    view_.state = Failed;
                    view_.error = err;
                }
                view_.gen++;
            }
            if (ok) {
                saveAccount();   // (only a sign-in that worked is kept)
                {   // downloads wanted and not all here: on again
                    std::lock_guard<std::mutex> v(m_);
                    for (const auto& [id, a] : dlAlbums_) {
                        int have = 0;
                        for (const auto& [sid, so] : dlSongs_) have += so.album == id;
                        if (a.total == 0 || have < a.total) tasks_.push_back({ Task::DlAlbum, id, 0, std::string(), std::string(), a.root });
                    }
                }
                std::string b2, e2;
                json::Json s;
                if (call("getScanStatus", "", b2, e2) && answer(b2, s, e2)) {   // (how many: where the server says)
                    std::lock_guard<std::mutex> v(m_);
                    if (s["scanStatus"]["count"].kind == json::Json::Num) view_.songs = int(s["scanStatus"].i("count"));
                    view_.gen++;
                }
            }
        } else if (t.kind == Task::Artists) {
            if (call("getArtists", "", body, err) && answer(body, r, err)) {
                std::vector<Artist> list;
                for (const json::Json& idx : r["artists"]["index"].arr)
                    for (const json::Json& a : idx["artist"].arr) list.push_back({ a.s("id"), a.s("name"), a.s("coverArt"), int(a.i("albumCount")) });
                std::lock_guard<std::mutex> v(m_);
                artists_ = std::move(list);
                haveArtists_ = true;
                view_.gen++;
            }
        } else if (t.kind == Task::Genres) {
            if (call("getGenres", "", body, err) && answer(body, r, err)) {
                std::vector<Genre> list;
                for (const json::Json& x : r["genres"]["genre"].arr) {
                    Genre gn{ x.s("value"), int(x.i("songCount")), int(x.i("albumCount")) };
                    if (!gn.name.empty() && gn.songs > 0) list.push_back(std::move(gn));
                }
                std::sort(list.begin(), list.end(), [](const Genre& a, const Genre& b) { return lowerOf(a.name) < lowerOf(b.name); });
                std::lock_guard<std::mutex> v(m_);
                genres_ = std::move(list);
                haveGenres_ = true;
                view_.gen++;
            }
        } else if (t.kind == Task::GenreAlbums) {
            std::vector<Album> list;
            bool ok = true;
            for (int offset = 0; offset < 20000; offset += 500) {
                if (!(call("getAlbumList2", "&type=byGenre&genre=" + urlEncode(t.id) + "&size=500&offset=" + std::to_string(offset), body, err) && answer(body, r, err))) { ok = false; break; }
                const auto& arr = r["albumList2"]["album"].arr;
                for (const json::Json& a : arr)
                    list.push_back({ a.s("id"), a.s("name"), a.s("artist"), a.s("artistId"), a.s("coverArt"), int(a.i("year")), int(a.i("songCount")), double(a.i("duration")) });
                if (arr.size() < 500) break;
            }
            if (ok) {
                std::lock_guard<std::mutex> v(m_);
                genreAlbums_.erase(std::remove_if(genreAlbums_.begin(), genreAlbums_.end(), [&](const auto& p) { return p.first == t.id; }), genreAlbums_.end());
                genreAlbums_.push_back({ t.id, std::move(list) });
                if (genreAlbums_.size() > 96) genreAlbums_.erase(genreAlbums_.begin());
                view_.gen++;
            }
        } else if (t.kind == Task::Folders) {
            if (call("getIndexes", "", body, err) && answer(body, r, err)) {
                std::vector<Entry> list;
                for (const json::Json& idx : r["indexes"]["index"].arr)
                    for (const json::Json& a : idx["artist"].arr) { Entry e; e.dir = true; e.id = a.s("id"); e.name = a.s("name"); e.cover = a.s("coverArt"); list.push_back(std::move(e)); }
                for (const json::Json& c : r["indexes"]["child"].arr) { Entry e; e.id = c.s("id"); e.name = c.s("title"); e.song = songOf(c); list.push_back(std::move(e)); }
                std::lock_guard<std::mutex> v(m_);
                folders_ = std::move(list);
                haveFolders_ = true;
                view_.gen++;
            }
        } else if (t.kind == Task::Dir) {
            if (call("getMusicDirectory", "&id=" + urlEncode(t.id), body, err) && answer(body, r, err)) {
                const json::Json& d = r["directory"];
                std::vector<Entry> list;
                for (const json::Json& c : d["child"].arr) {
                    Entry e;
                    e.dir = c["isDir"].kind == json::Json::Bool && c["isDir"].b;
                    e.id = c.s("id");
                    e.name = c.s("title");
                    e.cover = c.s("coverArt");
                    if (!e.dir) e.song = songOf(c);
                    list.push_back(std::move(e));
                }
                std::stable_sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.dir > b.dir; });   // folders first, as Folders shows
                std::lock_guard<std::mutex> v(m_);
                dirs_.erase(std::remove_if(dirs_.begin(), dirs_.end(), [&](const auto& p) { return p.first == t.id; }), dirs_.end());
                dirs_.push_back({ t.id, { d.s("name"), std::move(list) } });
                if (dirs_.size() > 64) dirs_.erase(dirs_.begin());
                view_.gen++;
            }
        } else if (t.kind == Task::AllSongs) {
            std::vector<Song> list;
            bool ok = true;
            for (int offset = 0; offset < 100000; offset += 500) {
                if (!(call("search3", "&query=&artistCount=0&albumCount=0&songCount=500&songOffset=" + std::to_string(offset), body, err) && answer(body, r, err))) { ok = false; break; }
                const auto& arr = r["searchResult3"]["song"].arr;
                for (const json::Json& s : arr) list.push_back(songOf(s));
                if (arr.size() < 500) break;
            }
            if (ok) {
                std::lock_guard<std::mutex> v(m_);
                allSongs_ = std::move(list);
                haveAllSongs_ = true;
                view_.gen++;
            }
        } else if (t.kind == Task::AllAlbums) {
            std::vector<Album> list;
            bool ok = true;
            for (int offset = 0; offset < 50000; offset += 500) {
                if (!(call("getAlbumList2", "&type=alphabeticalByArtist&size=500&offset=" + std::to_string(offset), body, err) && answer(body, r, err))) { ok = false; break; }
                const auto& arr = r["albumList2"]["album"].arr;
                for (const json::Json& a : arr)
                    list.push_back({ a.s("id"), a.s("name"), a.s("artist"), a.s("artistId"), a.s("coverArt"), int(a.i("year")), int(a.i("songCount")), double(a.i("duration")) });
                if (arr.size() < 500) break;
            }
            if (ok) {
                std::lock_guard<std::mutex> v(m_);
                allAlbums_ = std::move(list);
                haveAllAlbums_ = true;
                view_.gen++;
            }
        } else if (t.kind == Task::Artist) {
            if (call("getArtist", "&id=" + urlEncode(t.id), body, err) && answer(body, r, err)) {
                std::vector<Album> list;
                for (const json::Json& a : r["artist"]["album"].arr)
                    list.push_back({ a.s("id"), a.s("name"), a.s("artist"), a.s("artistId"), a.s("coverArt"), int(a.i("year")), int(a.i("songCount")), double(a.i("duration")) });
                std::lock_guard<std::mutex> v(m_);
                artistAlbums_.erase(std::remove_if(artistAlbums_.begin(), artistAlbums_.end(), [&](const auto& p) { return p.first == t.id; }), artistAlbums_.end());
                artistAlbums_.push_back({ t.id, std::move(list) });
                if (artistAlbums_.size() > 64) artistAlbums_.erase(artistAlbums_.begin());
                view_.gen++;
            }
        } else if (t.kind == Task::Album) {
            if (call("getAlbum", "&id=" + urlEncode(t.id), body, err) && answer(body, r, err)) {
                const json::Json& al = r["album"];
                Album a{ al.s("id"), al.s("name"), al.s("artist"), al.s("artistId"), al.s("coverArt"), int(al.i("year")), int(al.i("songCount")), double(al.i("duration")) };
                std::vector<Song> songs;
                for (const json::Json& s : al["song"].arr) {
                    Song x;
                    x.id = s.s("id");
                    x.title = s.s("title");
                    x.artist = s.s("artist");
                    x.album = s.s("album");
                    x.albumId = s.s("albumId");
                    x.cover = s.s("coverArt");
                    x.suffix = s.s("suffix");
                    x.contentType = s.s("contentType");
                    x.genre = s.s("genre");   // (the album page's genre button)
                    x.track = int(s.i("track"));
                    x.disc = int(s.i("discNumber"));
                    x.year = int(s.i("year"));
                    x.bitRate = int(s.i("bitRate"));
                    x.duration = double(s.i("duration"));
                    songs.push_back(std::move(x));
                }
                std::lock_guard<std::mutex> v(m_);
                albums_.erase(std::remove_if(albums_.begin(), albums_.end(), [&](const auto& p) { return p.first == t.id; }), albums_.end());
                albums_.push_back({ t.id, { a, std::move(songs) } });
                if (albums_.size() > 64) albums_.erase(albums_.begin());
                view_.gen++;
            }
        } else if (t.kind == Task::Scrobble) {   // a play (or now playing) on the server; lost when it cannot be sent
            call("scrobble", "&id=" + urlEncode(t.id) + (t.size ? "&submission=true&time=" + t.suffix : std::string("&submission=false")), body, err);
        } else if (t.kind == Task::Star) {
            call(t.size ? "star" : "unstar", "&id=" + urlEncode(t.id), body, err);
        } else if (t.kind == Task::Starred) {
            if (call("getStarred2", "", body, err) && answer(body, r, err)) {
                std::vector<Song> list;
                for (const json::Json& so : r["starred2"]["song"].arr) list.push_back(songOf(so));
                std::lock_guard<std::mutex> v(m_);
                starred_ = std::move(list);
                starredIds_.clear();
                for (const auto& x : starred_) starredIds_.insert(x.id);
                haveStarred_ = true;
                starGen_++;
                view_.gen++;
            }
        } else if (t.kind == Task::Playlists) {
            if (call("getPlaylists", "", body, err) && answer(body, r, err)) {
                std::vector<Playlist> list;
                for (const json::Json& p : r["playlists"]["playlist"].arr)
                    list.push_back({ p.s("id"), p.s("name"), p.s("coverArt"), int(p.i("songCount")), double(p.i("duration")) });
                std::lock_guard<std::mutex> v(m_);
                playlists_ = std::move(list);
                havePlaylists_ = true;
                plGen_++;
                view_.gen++;
            }
        } else if (t.kind == Task::PlaylistOne) {
            if (call("getPlaylist", "&id=" + urlEncode(t.id), body, err) && answer(body, r, err)) {
                const json::Json& p = r["playlist"];
                Playlist pl{ p.s("id"), p.s("name"), p.s("coverArt"), int(p.i("songCount")), double(p.i("duration")) };
                std::vector<Song> songs;
                for (const json::Json& so : p["entry"].arr) songs.push_back(songOf(so));
                std::lock_guard<std::mutex> v(m_);
                playlistSongs_.erase(std::remove_if(playlistSongs_.begin(), playlistSongs_.end(), [&](const auto& q) { return q.first == t.id; }), playlistSongs_.end());
                playlistSongs_.push_back({ t.id, { pl, std::move(songs) } });
                if (playlistSongs_.size() > 32) playlistSongs_.erase(playlistSongs_.begin());
                view_.gen++;
            }
        } else if (t.kind == Task::SendPlaylist) {   // createPlaylist: new, or (playlistId) its tracks replaced; songIds in t.path
            std::string extra = t.id.empty() ? "&name=" + urlEncode(t.suffix) : "&playlistId=" + urlEncode(t.id);
            size_t a = 0;
            while (a < t.path.size()) {
                size_t b = t.path.find(',', a);
                if (b == std::string::npos) b = t.path.size();
                if (b > a) extra += "&songId=" + urlEncode(t.path.substr(a, b - a));
                a = b + 1;
            }
            call("createPlaylist", extra, body, err);
            {
                std::lock_guard<std::mutex> v(m_);
                playlistSongs_.erase(std::remove_if(playlistSongs_.begin(), playlistSongs_.end(), [&](const auto& q) { return q.first == t.id; }), playlistSongs_.end());
                tasks_.push_back({ Task::Playlists });
            }
        } else if (t.kind == Task::Lyrics) {   // id; artist in suffix, title in album; Songate's own file in path
            std::string text, source;
            if (FILE* f = fopen(t.path.c_str(), "rb")) {   // kept before ("Keep" on lyrics found online)
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
                fclose(f);
                if (!text.empty()) source = "Songate's folder";
            }
            if (text.empty() && call("getLyricsBySongId", "&id=" + urlEncode(t.id), body, err) && answer(body, r, err)) {
                for (const json::Json& sl : r["lyricsList"]["structuredLyrics"].arr) {
                    const bool synced = sl["synced"].kind == json::Json::Bool && sl["synced"].b;
                    std::string out;
                    for (const json::Json& ln : sl["line"].arr) {
                        if (synced) {
                            const long long ms = (long long)ln.i("start");
                            char ts[24];
                            snprintf(ts, sizeof ts, "[%02lld:%02lld.%02lld]", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 10);
                            out += ts;
                        }
                        out += ln.s("value") + "\n";
                    }
                    if (!out.empty() && (text.empty() || synced)) { text = out; source = "the server"; }
                    if (synced && !text.empty()) break;
                }
            }
            if (text.empty() && call("getLyrics", "&artist=" + urlEncode(t.suffix) + "&title=" + urlEncode(t.album), body, err) && answer(body, r, err)) {
                text = r["lyrics"].s("value");
                if (!text.empty()) source = "the server";
            }
            std::lock_guard<std::mutex> v(m_);
            lyrics_.erase(std::remove_if(lyrics_.begin(), lyrics_.end(), [&](const auto& p) { return p.first == t.id; }), lyrics_.end());
            lyrics_.push_back({ t.id, { text, source } });
            if (lyrics_.size() > 32) lyrics_.erase(lyrics_.begin());
            view_.gen++;
        } else if (t.kind == Task::FileInfoTask) {
            TrackInfo in;
            FileInfo fi;
            if (readTrackInfo(t.path, in, false, false)) fi = { in.codec, in.sampleRate, in.bitsPerSample, in.bitrateKbps };
            std::lock_guard<std::mutex> v(m_);
            fileInfos_.erase(std::remove_if(fileInfos_.begin(), fileInfos_.end(), [&](const auto& p) { return p.first == t.id; }), fileInfos_.end());
            fileInfos_.push_back({ t.id, fi });
            if (fileInfos_.size() > 32) fileInfos_.erase(fileInfos_.begin());
        } else if (t.kind == Task::PlEdit) {   // size: 0 new, 1 change, 2 delete; id the playlist; suffix a name; album ids to add; path places to remove
            auto each = [](const std::string& csv, const std::function<void(const std::string&)>& f) {
                size_t a = 0;
                while (a < csv.size()) {
                    size_t b = csv.find(',', a);
                    if (b == std::string::npos) b = csv.size();
                    if (b > a) f(csv.substr(a, b - a));
                    a = b + 1;
                }
            };
            std::string extra;
            const char* method = t.size == 0 ? "createPlaylist" : t.size == 1 ? "updatePlaylist" : "deletePlaylist";
            if (t.size == 0) {
                extra = "&name=" + urlEncode(t.suffix);
                each(t.album, [&](const std::string& s) { extra += "&songId=" + urlEncode(s); });
            } else if (t.size == 1) {
                extra = "&playlistId=" + urlEncode(t.id);
                if (!t.suffix.empty()) extra += "&name=" + urlEncode(t.suffix);
                each(t.album, [&](const std::string& s) { extra += "&songIdToAdd=" + urlEncode(s); });
                each(t.path, [&](const std::string& s) { extra += "&songIndexToRemove=" + s; });
            } else {
                extra = "&id=" + urlEncode(t.id);
            }
            const bool ok = call(method, extra, body, err) && answer(body, r, err);
            std::lock_guard<std::mutex> v(m_);
            if (!ok) note("Playlist " + std::string(method) + " failed: " + err);
            playlistSongs_.erase(std::remove_if(playlistSongs_.begin(), playlistSongs_.end(), [&](const auto& q) { return q.first == t.id; }), playlistSongs_.end());
            tasks_.push_back({ Task::Playlists });
            view_.gen++;
        } else if (t.kind == Task::SaveLedger) {
            std::string out;
            {
                std::lock_guard<std::mutex> v(m_);
                for (const auto& k : ledger_) out += k + "\n";
            }
            const std::string p = dir_ + "/server-likes.tsv";
            if (out.empty()) remove(p.c_str());
            else if (FILE* f = fopen((p + ".tmp").c_str(), "wb")) {
                fwrite(out.data(), 1, out.size(), f);
                fclose(f);
                remove(p.c_str());
                rename((p + ".tmp").c_str(), p.c_str());
            }
        } else if (t.kind == Task::DlAlbum) {   // an album to keep: its songs, each one not here yet queued
            if (call("getAlbum", "&id=" + urlEncode(t.id), body, err) && answer(body, r, err)) {
                const json::Json& al = r["album"];
                std::lock_guard<std::mutex> v(m_);
                DlAlbumRec& a = dlAlbums_[t.id];
                if (a.root.empty()) a.root = t.path;
                a.name = al.s("name");
                a.artist = al.s("artist");
                a.total = int(al["song"].arr.size());
                int queued = 0;
                for (const json::Json& so : al["song"].arr) {
                    const Song x = songOf(so);
                    if (dlSongs_.count(x.id)) continue;
                    if (std::any_of(tasks_.begin(), tasks_.end(), [&](const Task& k) { return k.kind == Task::DlSong && k.id == x.id; })) continue;
                    std::string ext = lowerOf(x.suffix);
                    if (ext != "mp3" && ext != "m4a" && ext != "flac") ext = "mp3";
                    char num[16];
                    if (x.disc > 1) snprintf(num, sizeof num, "%d-%02d ", x.disc, x.track);
                    else snprintf(num, sizeof num, "%02d ", x.track);
                    const std::string path = a.root + "/" + safeName(a.artist.empty() ? x.artist : a.artist) + "/" + safeName(a.name) + "/" +
                                             (x.track > 0 ? std::string(num) : std::string()) + safeName(x.title) + "." + ext;
                    tasks_.push_back({ Task::DlSong, x.id, 0, x.suffix, t.id, path });
                    queued++;
                }
                dlTotal_ += queued;
                dlGen_++;
                view_.gen++;
            }
            saveDownloads();
        } else if (t.kind == Task::DlSong) {   // one song kept: its own file (MP3, M4A, FLAC), else the server's MP3 at 320 kbps
#ifdef GF_DEV
            if (const char* slow = getenv("GF_DL_SLOW")) SDL_Delay(Uint32(atoi(slow)));   // (tests: the card seen)
#endif
            std::string ext = lowerOf(t.suffix);
            const bool raw = ext == "mp3" || ext == "m4a" || ext == "flac";
            makeFolders(t.path.substr(0, t.path.rfind('/')));
            struct stat st{};
            bool ok = stat(t.path.c_str(), &st) == 0 && st.st_size > 0;
            if (!ok) {
                std::string url = base("stream", "&id=" + urlEncode(t.id) + (raw ? "&format=raw" : "&format=mp3&maxBitRate=320"));
                ok = net::download(url, t.path, err, nullptr);
                wipe(url);
                if (!ok) remove(t.path.c_str());
                else if (stat(t.path.c_str(), &st) != 0) st.st_size = 0;
            }
            {
                std::lock_guard<std::mutex> v(m_);
                dlDone_++;
                if (ok && dlAlbums_.count(t.album)) {
                    dlSongs_[t.id] = { t.album, (long long)st.st_size, t.path };
                    if (t.path.compare(0, dir_.size(), dir_) != 0) dlWroteMusic_ = true;
                }
                if (!std::any_of(tasks_.begin(), tasks_.end(), [](const Task& k) { return k.kind == Task::DlSong || k.kind == Task::DlAlbum; })) {
                    dlBatchDone_ = true;   // the batch ended: the card goes; a music folder written gets read again (App)
                    dlBatchMusic_ = dlWroteMusic_;
                    dlWroteMusic_ = false;
                    dlDone_ = dlTotal_ = 0;
                }
                dlGen_++;
                view_.gen++;
            }
            saveDownloads();
        } else if (t.kind == Task::DlRemove) {   // an album no longer kept: its files go, and its folders where empty
            std::vector<std::string> paths;
            {
                std::lock_guard<std::mutex> v(m_);
                dlAlbums_.erase(t.id);
                for (auto it = dlSongs_.begin(); it != dlSongs_.end();) {
                    if (it->second.album == t.id) { paths.push_back(it->second.path); it = dlSongs_.erase(it); }
                    else ++it;
                }
                const size_t before = tasks_.size();
                tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [&](const Task& k) { return k.kind == Task::DlSong && k.album == t.id; }), tasks_.end());
                dlTotal_ -= int(before - tasks_.size());
                if (dlTotal_ <= dlDone_) dlDone_ = dlTotal_ = 0;
                dlGen_++;
                view_.gen++;
            }
            for (const auto& p : paths) { remove(p.c_str()); dropEmpty(p); }
            saveDownloads();
        } else if (t.kind == Task::DlWipe) {   // the songs kept to play again, and (t.size) every download: removed
            std::vector<std::string> paths;
            {
                std::lock_guard<std::mutex> v(m_);
                if (t.size) {
                    for (const auto& [id, so] : dlSongs_) paths.push_back(so.path);
                    dlSongs_.clear();
                    dlAlbums_.clear();
                    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [](const Task& k) { return k.kind == Task::DlSong || k.kind == Task::DlAlbum; }), tasks_.end());
                    dlDone_ = dlTotal_ = 0;
                }
                songs_.clear();
            }
            for (const auto& p : paths) { remove(p.c_str()); dropEmpty(p); }
            for (const Cached& k : readIndex(dir_ + "/server-cache.tsv")) remove((dir_ + "/server-cache/" + k.file).c_str());
            remove((dir_ + "/server-cache.tsv").c_str());
            if (t.size) {
                remove((dir_ + "/server-downloads.tsv").c_str());
                remove((dir_ + "/server-likes.tsv").c_str());
                std::lock_guard<std::mutex> v(m_);
                ledger_.clear();
            }
            // their folders too, where now empty: nothing of the server left behind (the music folder's Songate downloads, where used)
            std::vector<std::string> roots = { dir_ + "/server-cache", dir_ + "/server-downloads" };
            for (const auto& p : paths) {
                const size_t a = p.find("/Songate downloads/");
                if (a != std::string::npos) { roots.push_back(p.substr(0, a + 18)); break; }
            }
            for (const auto& d : roots) {
#if defined(_WIN32) && !defined(GF_PREVIEW)
                _rmdir(d.c_str());
#else
                rmdir(d.c_str());
#endif
            }
            std::lock_guard<std::mutex> v(m_);
            wiping_--;
            dlGen_++;
            view_.gen++;
        } else if (t.kind == Task::Song) {
            // its own file where Songate plays the kind (MP3, M4A, FLAC); anything else the server makes MP3 at 320 kbps
            std::string ext = t.suffix;
            for (char& c : ext) c = char(tolower(uint8_t(c)));
            const bool raw = ext == "mp3" || ext == "m4a" || ext == "flac";
            if (!raw) ext = "mp3";
            std::string safe;
            for (char c : t.id) safe += isalnum(uint8_t(c)) || c == '-' || c == '_' ? c : '_';
            makeFolder(dir_ + "/server-cache");
            const std::string file = safe + "." + ext, path = dir_ + "/server-cache/" + file;
            struct stat st{};
            bool ok = stat(path.c_str(), &st) == 0 && st.st_size > 0;
            if (!ok) {
                std::string url = base("stream", "&id=" + urlEncode(t.id) + (raw ? "&format=raw" : "&format=mp3&maxBitRate=320"));
                ok = net::download(url, path, err, nullptr);
                wipe(url);
                if (ok && stat(path.c_str(), &st) != 0) st.st_size = 0;
            }
            if (ok) {   // the index: this song now; the oldest beyond 512 MB removed
                const std::string ip = dir_ + "/server-cache.tsv";
                std::vector<Cached> idx = readIndex(ip);
                idx.erase(std::remove_if(idx.begin(), idx.end(), [&](const Cached& c) { return c.file == file; }), idx.end());
                idx.push_back({ (long long)std::time(nullptr), (long long)st.st_size, file });
                std::sort(idx.begin(), idx.end(), [](const Cached& x, const Cached& y) { return x.at < y.at; });
                long long total = 0;
                for (const Cached& c : idx) total += c.size;
                std::vector<std::string> gone;
                while (total > 512LL * 1024 * 1024 && idx.size() > 3) {   // (the newest three stay: this one, the one playing, the one before)
                    remove((dir_ + "/server-cache/" + idx.front().file).c_str());
                    gone.push_back(dir_ + "/server-cache/" + idx.front().file);
                    total -= idx.front().size;
                    idx.erase(idx.begin());
                }
                writeIndex(ip, idx);
                std::lock_guard<std::mutex> v(m_);
                for (const std::string& gp : gone)
                    songs_.erase(std::remove_if(songs_.begin(), songs_.end(), [&](const auto& p) { return p.second == gp; }), songs_.end());
            }
            {
                std::lock_guard<std::mutex> v(m_);
                songs_.erase(std::remove_if(songs_.begin(), songs_.end(), [&](const auto& p) { return p.first == t.id; }), songs_.end());
                songs_.push_back({ t.id, ok ? path : std::string() });
                if (songs_.size() > 512) songs_.erase(songs_.begin());
                view_.gen++;
            }
        } else if (t.kind == Task::Cover) {
            std::string url = base("getCoverArt", "&id=" + urlEncode(t.id) + "&size=" + std::to_string(t.size));
            const bool ok = net::get(url, body, err) && body.size() > 8 && (uint8_t(body[0]) == 0xFF || uint8_t(body[0]) == 0x89);
            wipe(url);
            SDL_Surface* rgba = nullptr;   // decoded here: the app's thread only makes it a texture
            if (ok) {
                if (SDL_Surface* src = loadImage(body.data(), body.size(), "server cover")) {
                    rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
                    SDL_FreeSurface(src);
                }
            }
            std::lock_guard<std::mutex> v(m_);
            decoded_.push_back({ t.id, rgba });   // (nullptr: none, so it is not asked for again)
            covers_.push_back({ t.id, ok ? std::vector<uint8_t>(body.begin(), body.end()) : std::vector<uint8_t>() });
            if (covers_.size() > 256) covers_.erase(covers_.begin());
            view_.gen++;
        }
        g.lock();
        if (sess != sess_) {   // the session ended while this ran: what it brought belongs to no one
            const std::vector<Later> keep = std::move(later_);   // (the new session's own, asked meanwhile)
            clearSession();
            later_ = keep;
            view_.gen++;
        } else if (unreached_) {
            again(t, sess);
        }
    }
}
