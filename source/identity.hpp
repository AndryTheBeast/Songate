/*
 * Songate · identity.hpp
 * Connections' first stage: what makes a track the same one on two devices, whose paths have nothing in common. Two levels:
 *
 *   exact: the same bytes: the file's size and 64 KB from its start, middle and end, hashed (BLAKE2b, 128 bits). Measurements may
 *          be shared on this only.
 *   loose: the same song: normalised artist, album and title, and a length within 2 seconds. Liked, plays, edits and playlists
 *          may follow it, measurements never: a FLAC and an AAC of a song are this, not exact.
 *
 * identity.tsv keeps the exact hash by path, size and date, so a file is read once. Everything on disk stays by path, as it
 * always was; a peer will be told identities, never paths. A worker does the reading, at the lowest priority there is; nothing
 * here runs unless asked.
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class Identity {
public:
    struct Want { std::string path, artist, albumArtist, album, title; double duration = 0; bool edited = false; };   // edited: read its tags from the file
    struct Known { int64_t size = 0, date = 0; std::string exact, loose; double duration = 0; };
    /** How the latest run went. */
    struct Progress { int done = 0, total = 0, hashed = 0, failed = 0; double hashSeconds = 0, seconds = 0; bool running = false; };

    /** The text a loose match compares (lower case, no accents, no "feat.", no punctuation). */
    static std::string normalise(const std::string& s);
    /** Hex of the song's key, "" when the tags cannot identify it (no title, or no artist). */
    static std::string looseKey(const std::string& artist, const std::string& albumArtist, const std::string& album, const std::string& title);
    /** Hex of the file's exact hash, "" if it cannot be read. */
    static std::string exactHash(const std::string& path, int64_t size);
    /** Hex of a text's hash (a server's print: Listening together and Handoff compare it, never the address itself). */
    static std::string textHash(const std::string& s);
    /** Two tracks with the same looseKey: the same song? */
    static bool sameLength(double a, double b) { return a - b <= 2 && b - a <= 2; }

    void start(const std::string& dir);
    void stop();
    /** The library, as a list: read on the worker; a run under way is replaced. */
    void want(std::vector<Want> tracks);
    Progress progress();
    /** A run under way? Without the lock: asked every frame. */
    bool busy() const { return busy_; }
    /** identity.tsv is read and knows nearly all of `tracks`: a sync need not wait for the run that checks them. */
    bool ready(size_t tracks) const { return size_t(knownCount_) * 10 >= tracks * 9 && tracks > 0; }
    /** Settings › Storage's Clear: what was read is forgotten, here and in identity.tsv (on the worker). */
    void forget();
    bool find(const std::string& path, Known& out);

private:
    void run();
    void load();
    void save();

    std::string dir_;
    std::thread thread_;   // joined in stop(), never detached
    std::mutex m_;
    std::condition_variable wake_;
    bool quit_ = false, fresh_ = false, loaded_ = false, forget_ = false;
    std::atomic<bool> stopRun_{ false }, busy_{ false };
    std::atomic<size_t> knownCount_{ 0 };
    std::vector<Want> want_;
    std::unordered_map<std::string, Known> known_;
    Progress prog_;
};
