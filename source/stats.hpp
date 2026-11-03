/*
 * Gatefold NX · stats.hpp
 * Play statistics, as foo_playcount keeps them for Gatefold on the PC: how often each track was
 * played, when last, and when it came to the library. Kept by path in stats.tsv; the music files
 * are never touched.
 *
 * A worker reads the file at launch, looks up file dates for tracks seen for the first time, and
 * takes in the plays the background player counted (bg-plays.txt). The main thread merges what it
 * found in poll(), so nothing here waits on the SD card.
 */
#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct PlayStat {
    int plays = 0;
    int64_t last = 0;    // when last played (seconds since 1970), 0 never
    int64_t added = 0;   // when the file came to the library: its date on the card, the first time it was seen
};

class Stats {
public:
    /** A play is counted once this much of a track was heard: foo_playcount's 60 s, or nearly all of a shorter one. */
    static double threshold(double duration) { return duration > 0 ? std::min(60.0, duration * 0.9) : 60.0; }

    void start(const std::string& dir);
    void stop();

    const PlayStat* find(const std::string& path) const;
    void countPlay(const std::string& path, int64_t when);
    /** The library's files: those without a date added get their file's date (on the worker). */
    void wantDates(const std::vector<std::string>& paths);
    /** While the background player plays, its bg-plays.txt is taken in every few seconds. */
    void setBackground(bool on);
    void forget(const std::string& path);
    /** Played here, without what other devices played (sync-plays.tsv): Connections. plays above is the two added up. */
    PlayStat mine(const std::string& path) const;
    int others(const std::string& path) const;
    /** From Connections: its plays on other devices, the last played and date added everywhere. */
    void adopt(const std::string& path, int others, int64_t last, int64_t added);
    /** sync-plays.tsv as it should be now. */
    std::string serialiseOthers() const;

    /** Main thread, every frame: merges the worker's results. True when anything changed. */
    bool poll();
    unsigned gen() const { return gen_; }
    bool dirty() const { return dirty_; }
    bool loaded() const { return loaded_; }   // stats.tsv has been read: saving before that would lose it
    /** stats.tsv as it should be now; clears dirty. */
    std::string serialise();

private:
    struct Found {
        bool loaded = false;
        std::unordered_map<std::string, PlayStat> file;            // stats.tsv as read at launch
        std::unordered_map<std::string, int> others;               // sync-plays.tsv
        std::vector<std::pair<std::string, int64_t>> dates, plays;  // path → file date / time played
    };
    void run();

    std::unordered_map<std::string, PlayStat> map_;
    std::unordered_map<std::string, int> others_;   // already added into map_'s plays; stats.tsv never holds them
    unsigned gen_ = 0;
    bool dirty_ = false, loaded_ = false;
    std::string dir_;

    std::thread thread_;   // joined in stop(), never detached
    std::mutex m_;
    std::condition_variable wake_;
    bool quit_ = false, background_ = false;
    std::vector<std::string> wantDates_;   // waiting for the worker
    Found found_;                          // the worker's, until poll() takes it
};
