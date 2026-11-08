/*
 * Songate · tagjobs.cpp
 * See tagjobs.hpp.
 */
#include "tagjobs.hpp"

#include "cores.hpp"

void TagJobs::start() {
    std::lock_guard<std::mutex> g(m_);
    if (started_) return;
    started_ = true;
    quit_ = false;
    th_ = std::thread([this] { cores::moveHere(cores::Files); run(); });
}

void TagJobs::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        if (!started_) return;
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();   // (a write under way finishes first: it swaps whole or not at all)
    std::lock_guard<std::mutex> g(m_);
    started_ = false;
}

bool TagJobs::idle() {
    std::lock_guard<std::mutex> g(m_);
    return !busy_ && in_.empty();
}

void TagJobs::put(Job j) {
    {
        std::lock_guard<std::mutex> g(m_);
        in_.push_back(std::move(j));
    }
    cv_.notify_all();
}

bool TagJobs::take(Done& d) {
    std::lock_guard<std::mutex> g(m_);
    if (out_.empty()) return false;
    d = std::move(out_.front());
    out_.pop_front();
    return true;
}

void TagJobs::run() {
    std::unique_lock<std::mutex> g(m_);
    while (!quit_) {
        cv_.wait(g, [&] { return quit_ || !in_.empty(); });
        if (quit_) break;
        Done d;
        d.job = std::move(in_.front());
        in_.pop_front();
        busy_ = true;
        g.unlock();
        d.ok = tagwrite::putChanges(d.job.path, d.job.changes, &d.before, d.err);
        g.lock();
        busy_ = false;
        out_.push_back(std::move(d));
    }
}
