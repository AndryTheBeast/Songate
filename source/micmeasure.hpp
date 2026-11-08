/*
 * Songate · micmeasure.hpp
 * Listening together's delay, measured (BETA 3 item 2; the user, 2026-11-05: measured the first time a device meets an output, and
 * "Measure again"; the starter listens if it has a microphone, else the first device in the session that has one).
 *
 * Every device in the session plays a short chirp at moments agreed on the shared clock, each device in its own slot, a few rounds
 * (Player::tones). The device listening records them with its microphone for those seconds, on a worker: the recording stays in memory
 * and is dropped once measured, never saved or sent; only the numbers leave. A matched filter finds each chirp; how late each device's
 * chirps were heard against when they were due, less the others', is how far that device's sound is behind.
 *
 * The chirp: 20 ms, 1.5 to 3.5 kHz, a Hann window (as tools/clicktest's, which proved the method with a microphone in session 10).
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace micm {

constexpr double kChirp = 0.020;     // seconds
// Between one chirp and the next, and how far either way each is looked for: a phone through a Bluetooth speaker was heard 515 ms late
// (2026-11-05), past the 0.5 s looked for, and in the next device's place. Neighbouring slots sweep opposite ways, so one device's chirp
// is not taken for the next one's.
constexpr double kSpacing = 1.5, kReach = 0.7;
constexpr double kChirpLevel = 0.5;  // the chirp's peak
// Under the chirps, from 1 s before the first to the last, a 15 Hz tone: a Bluetooth speaker given only silence sleeps or gates it, and
// a 20 ms chirp after it is lost (the user, 2026-11-05: "when a bluetooth speaker is connected to the phone it does not play the
// measuring chirps"). Below what people hear and small speakers play, still sound to the speaker (2026-11-07: kept over the hiss it
// replaced and a 19 kHz tone, which Bluetooth codecs cut). With it, "playing" to the system meanwhile (App::chirpsDue)
constexpr double kWakeHz = 15, kWakeLevel = 0.06, kBedLead = 1.0;
constexpr int kRounds = 3;

/** The chirp's value at x seconds into it (0..kChirp), -1..1. Slot `slot`'s: even ones sweep up (1.5 to 3.5 kHz), odd ones down. */
double chirpAt(double x, int slot = 0);
/** When device `slot` of `slots` chirps in round r, from t0 (shared clock). */
inline double chirpTime(double t0, int slots, int slot, int r) { return t0 + (r * slots + slot) * kSpacing; }
/** This device can listen: it has a microphone (the Switch: none). Asking for the permission comes later, on the worker. */
bool available();

struct Result {
    bool done = false;
    std::string error;            // the microphone could not be opened, nothing was heard…
    std::vector<double> lateMs;   // by slot: how late its chirps were heard, against when they were due
    std::vector<bool> heard;      // by slot: its chirps were clear in at least two rounds, and agreed
    std::vector<std::string> why; // by slot, one not heard: how close it came, each round (for the log)
    std::string mic;              // the microphone it listened with (for the log)
};

/** The measuring itself, from a recording: its samples, and stamps of (samples so far, shared time). For the test too. */
Result analyze(const std::vector<int16_t>& pcm, const std::vector<std::pair<double, double>>& stamps, double rate, double t0, int slots,
               const std::atomic<bool>* quit = nullptr);

class Measurer {
public:
    ~Measurer() { stop(); }
    /** Records from the shared moment t0 - 0.3 s to the last chirp + 0.5 s and measures, on a worker. toShared: a steady_clock
     *  reading (seconds) as the shared clock (Player::sharedAt). */
    void start(std::function<double(double)> toShared, double t0, int slots);
    bool take(Result& out);   // once, when done
    bool busy() const { return busy_; }
    void stop();

private:
    void run(std::function<double(double)> toShared, double t0, int slots);
    std::thread th_;
    std::atomic<bool> busy_{ false }, quit_{ false };
    std::mutex m_;
    Result result_;
    bool ready_ = false;
};

}  // namespace micm
