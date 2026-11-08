/*
 * Songate · tagjobs.hpp
 * Tag and cover changes written into songs, one at a time, on a worker (tagwrite.hpp does the writing and its checks). The app
 * keeps the queue and hands the next song only when it is not the one playing (its file is held open); results come back with
 * what each file held for what changed, for "Restore the file's original tags" (the user, 2026-11-07).
 */
#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "tagwrite.hpp"

class TagJobs {
public:
    struct Job {
        std::string path;
        tagwrite::Changes changes;
        bool undo = false;          // the file's originals written back
    };
    struct Done {
        Job job;
        bool ok = false;
        std::string err;
        tagwrite::Before before;    // what the file held for what changed
    };
    ~TagJobs() { stop(); }
    void start();
    void stop();
    /** Nothing being written: the next may be handed over. */
    bool idle();
    void put(Job j);
    bool take(Done& d);

private:
    void run();
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> in_;
    std::deque<Done> out_;
    bool busy_ = false, quit_ = false, started_ = false;
};
