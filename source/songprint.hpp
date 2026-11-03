/*
 * Songate · songprint.hpp
 * Listening together's prints of a song (together.hpp: one song, two files), taken on a worker: the file decoded from its start as the
 * player decodes it, up to the window, then its loudness. A few tenths of a second each, on the main thread never.
 */
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SongPrints {
public:
    struct Job {
        unsigned gen = 0;        // the session's: a print from one before is dropped
        int song = -1;
        bool forThem = false;    // to send to the other device (else this device's own, to fit theirs into)
        std::string path;
        double from = 0, seconds = 0;
    };
    struct Done {
        Job job;
        std::vector<uint8_t> print;   // empty: the file would not read, or ended first
    };

    void start();
    void stop();
    void want(Job j);
    /** Finished prints, oldest first. */
    std::vector<Done> take();

private:
    void run();

    std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    bool quit_ = false;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
};
