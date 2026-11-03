/*
 * Songate · songprint.cpp
 * See songprint.hpp.
 */
#include "songprint.hpp"

#include "cores.hpp"
#include "ffdecoder.hpp"
#include "together.hpp"

void SongPrints::start() {
    if (thread_.joinable()) return;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void SongPrints::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    jobs_.clear();
    done_.clear();
}

void SongPrints::want(Job j) {
    {
        std::lock_guard<std::mutex> g(m_);
        jobs_.push_back(std::move(j));
    }
    cv_.notify_one();
}

std::vector<SongPrints::Done> SongPrints::take() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<Done> out;
    out.swap(done_);
    return out;
}

void SongPrints::run() {
    // with the file writer: below the audio, its read-ahead and the track loader, which a song starting needs first
    cores::moveHere(cores::Files);
    constexpr int kRate = 48000, kChannels = 2;   // as the player decodes
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> g(m_);
            cv_.wait(g, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            j = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Done d;
        d.job = j;
        FfDecoder dec;
        if (j.from >= 0 && j.seconds > 0 && dec.open(j.path, kRate, kChannels) && (j.from <= 0 || dec.skip(j.from, true))) {
            std::vector<int16_t> pcm(size_t(j.seconds * kRate + 0.5) * kChannels);
            const int got = dec.read(pcm.data(), int(pcm.size() / kChannels));
            if (size_t(got) * kChannels == pcm.size()) d.print = together::loudness(pcm.data(), size_t(got), kRate, kChannels);
        }
        dec.close();
        std::lock_guard<std::mutex> g(m_);
        done_.push_back(std::move(d));
    }
}
