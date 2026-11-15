/*
 * Songate · cardhold.cpp (cardhold.hpp)
 */
#include "cardhold.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace cardhold {
namespace {
std::atomic<bool> heldV{ false };
std::atomic<bool> givenUp{ false };   // Songate closing: nothing waits any more (a worker's join would never end)
std::mutex mx;
std::string rootV;
std::thread::id mainId;
}  // namespace

void set(const std::string& root) {
#ifdef GF_ANDROID
    std::lock_guard<std::mutex> g(mx);
    rootV = root;
    heldV = !root.empty();
#else
    (void)root;
#endif
}

bool held() { return heldV.load(); }

void markMain() { mainId = std::this_thread::get_id(); }

bool pass(const std::string& path) {
    if (!heldV.load()) return true;
    {
        std::lock_guard<std::mutex> g(mx);
        if (rootV.empty() || path.compare(0, rootV.size(), rootV) != 0) return true;   // (another folder: the phone's own storage)
    }
    if (std::this_thread::get_id() == mainId) return false;
    while (heldV.load() && !givenUp.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return !heldV.load();
}

void giveUp() { givenUp = true; }

bool under(const std::string& path) {
    if (!heldV.load()) return false;
    std::lock_guard<std::mutex> g(mx);
    return !rootV.empty() && path.compare(0, rootV.size(), rootV) == 0;
}

}  // namespace cardhold
