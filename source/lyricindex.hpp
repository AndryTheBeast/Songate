/*
 * Songate · lyricindex.hpp
 * Search by lyrics (BETA 3; the user, 2026-11-03: "a user only remember a certain part of the song but not its name... search will
 * now be able to search inside lyric tag and .lrc files"; 2026-11-05: two groups, "Start of words", a card for where to play from).
 *
 * One worker, at a low priority, reads every song's lyrics once, as playback finds them (an .lrc beside the song, else its tags, else
 * Songate's own folder), and keeps each line with its time and a folded copy (lower case, accents off) to match against. What it read
 * is kept in lyrics-index.tsv. A start reads nothing it has (the user, 2026-11-05: "Instead of it reading everytime... only then read
 * those since none other music on the library changed"): only what the app tells it changed: a song new to the library, an .lrc
 * that came or went in the folder walk, lyrics Songate saved or removed, the playing song's lyrics when they differ, and "Read all
 * again" (Settings), which looks at every song's size and dates once. All of it on this worker, never the main thread. Searches run
 * on the same worker: every word of the search must start a word in one line; the first such line of each song is its hit.
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/** Lower case, accents off (é → e, ß → ss…), the rest as it was; offsets (if asked): for each byte of the result, the byte of the
 *  original it came from, so a match can be lit in the original text. */
std::string foldText(const std::string& s, std::vector<int>* offsets = nullptr);

class LyricIndex {
public:
    struct Hit {
        std::string path;
        std::string line;   // the line as written
        double at = -1;     // its time in seconds; -1: plain lyrics
    };
    struct Progress { int read = 0, total = 0; bool checking = false; };   // checking: "Read all again", read = looked at

    ~LyricIndex() { stop(); }
    void start(const std::string& cacheFile);
    void stop();
    /** The library's songs, after each scan (main thread). */
    void setLibrary(std::vector<std::string> paths);
    /** A song's lyrics changed (saved, removed): read again. */
    void refresh(const std::string& path);
    /** The .lrc files the folder walk found (any thread): a song whose .lrc came or went is read again. */
    void setSidecars(std::vector<std::string> lrcs);
    /** The playing song's lyrics as playback read them (main thread: only handed over): its entry made the same if it differs. */
    void offer(const std::string& path, const std::string& text);
    /** Every song's size and dates looked at once, and what changed read again ("Read all again"). */
    void checkAll();
    /** Asks; the answer comes with takeResults, with the same ticket. */
    void search(const std::string& query, uint64_t ticket);
    bool takeResults(uint64_t& ticket, std::vector<Hit>& hits);
    Progress progress() const {
        const bool c = checking_.load();
        return { c ? checked_.load() : read_.load(), total_.load(), c };
    }
    /** Stopped by hand (Settings, its card): no more songs read; searches, and lyrics Songate saves, still go on. */
    void pause(bool on);
    /** While the library's tags are being read (the same files): no songs read, as paused; Songate's own changes still go on. */
    void hold(bool on);
    bool held() const { return held_.load(); }

private:
    // A song's lines kept in two blocks, its words and their folded copy, a line each ('\n' between); a line is where it sits in
    // them. (Each line its own two strings: the 5 MB cache took ~700,000 allocations to load, ~9 s on the console, and every
    // frame's own allocations waited on the same lock: 30 fps at boot; the user, 2026-11-08)
    struct Line { float t; uint32_t a, n, fa, fn; };
    struct Song {
        std::string path, key, text, fold;
        std::vector<Line> lines;
        bool read = false, stale = false, check = false;
        std::string_view textOf(const Line& l) const { return std::string_view(text).substr(l.a, l.n); }
        std::string_view foldOf(const Line& l) const { return std::string_view(fold).substr(l.fa, l.fn); }
        void clearLines() { text.clear(); fold.clear(); lines.clear(); }
        void addLine(float t, const char* p, size_t n);   // (its words as written; no '\n' in them)
        void finish();                                    // the folded copy, once its lines are in
    };
    static void parse(const std::string& text, Song& into);
    void applySidecars(std::vector<std::string> lrcs);
    void run();
    void loadCache();
    void saveCache();
    void readSong(Song& s);
    std::string keyOf(const std::string& path) const;
    std::vector<std::string> saved_;   // the names in Songate's lyrics folder, listed once a pass (not a lookup per song)
    void listSaved();
    std::vector<Hit> find(const std::string& query) const;

    std::string cache_;
    std::thread th_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false, started_ = false, paused_ = false;
    std::vector<std::string> wantLib_;   // a new library, waiting for the worker
    bool haveLib_ = false;
    std::vector<std::string> refresh_;
    std::vector<std::string> wantLrc_, lrcs_;   // the walk's .lrc files: waiting; and as last known (sorted; kept in the cache)
    bool haveLrc_ = false, lrcKnown_ = false;
    std::vector<std::pair<std::string, std::string>> offers_;
    bool wantCheck_ = false;
    std::string query_;
    uint64_t queryTicket_ = 0, doneTicket_ = 0;
    bool haveQuery_ = false, haveDone_ = false;
    std::vector<Hit> done_;
    std::vector<Song> songs_;   // the worker's only
    std::atomic<int> read_{ 0 }, total_{ 0 }, checked_{ 0 };
    std::atomic<bool> checking_{ false }, held_{ false };
};
