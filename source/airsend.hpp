/*
 * Songate · airsend.hpp
 * This device plays to AirPlay speakers (BETA 3, item 5; the user, 2026-11-05: the Devices sheet's Speakers group, only the speaker plays,
 * several at once): an AirPlay 1 sender, the other side of airplay.hpp.
 *
 * A worker at cores::Net finds speakers (mDNS _raop._tcp; on Windows through its own DNS-SD, which holds port 5353) and pumps the sound:
 * Player::postMix hands over its finished mix (48 kHz, after the equalizer, the levelling and the volume), which becomes 44.1 kHz ALAC
 * (uncompressed frames: exact, and every receiver decodes them), AES-encrypted with a key wrapped by the AirPort key's public half, in
 * RTP over UDP. One clock for all of them: the same RTP times, sync packets every second and answers to their timing questions, so
 * several speakers play together, two seconds behind. Each chosen speaker has a small thread of its own for RTSP (connect, ANNOUNCE,
 * SETUP, RECORD, volume, FLUSH, TEARDOWN), so a slow one holds up nobody.
 *
 * Speakers that only speak AirPlay 2 (a HomePod) are listed but not offered.
 */
#pragma once
#ifdef GF_AIRPLAY
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AirSend {
public:
    struct Speaker {
        std::string id;              // the mDNS instance ("hwaddr@Name")
        std::string name, model;     // its name, its model (TXT am)
        std::string address;         // IPv4, dotted
        int port = 0;
        bool supported = true;       // AirPlay 1 spoken (not a HomePod's pairing-only AirPlay 2)
        bool chosen = false;         // the user picked it
        bool playing = false;        // its session is up and the sound goes there
        std::string state;           // "Connecting", "Playing", or why not
        float volume = 0.6f;         // 0..1, its own
    };

    AirSend();
    ~AirSend();
    void start();
    void stop();
    /** This device's name, as the speakers show who plays (iOS sends its own the same way: the receiver's "From …"). */
    void setName(const std::string& name);
    /** Look for speakers while this is on (the Devices sheet open, or a speaker chosen). Main thread, every frame: cheap. */
    void browse(bool on);
    std::vector<Speaker> speakers();
    /** Play there, or no longer. Main thread. */
    void choose(const std::string& id, bool on);
    /** None of them: the sound comes back here. */
    void chooseNone();
    void volume(const std::string& id, float v);
    /** Pause, a seek, a skip: what the speakers hold (two seconds of it) is dropped, so the change is heard at once. */
    void flush();
    /** The song now (its title, artist, album; its length and where it is), for speakers with a screen. */
    void metadata(const std::string& title, const std::string& artist, const std::string& album, double duration, double position);
    /** The song's cover (JPEG or PNG; empty: none). A speaker chosen later gets this and the details above at once. */
    void artwork(std::vector<uint8_t> image);
    /** A speaker is being played to (or set up): the sound is taken from the mix. */
    bool tapping() const { return tapping_.load(std::memory_order_relaxed); }
    /** At least one plays: this device stays quiet. */
    bool silenceHere() const { return silence_.load(std::memory_order_relaxed); }
    /** How far behind the speakers play, in seconds (what the lyrics and the seek bar wait for). */
    double delay() const { return 2.0; }
    /** Audio thread: the finished mix, interleaved stereo (16-bit or float) at `rate`. Lock-free, a copy only. */
    void feed(const void* samples, int frames, bool float32, int channels, int rate);
    /** Remembered volumes ("id\tvolume" lines), so a speaker comes back as loud as it was. */
    void setVolumes(const std::vector<std::pair<std::string, float>>& v);
    std::vector<std::pair<std::string, float>> volumes();
    /** What looking for speakers is doing (for the log, dev): whether it has port 5353, questions sent, answers heard, speakers known. */
    std::string status();
    /** Speakers that ended the stream themselves since last asked (their names): the caller pauses if none plays now. Main thread. */
    std::vector<std::string> takeDropped();

    struct Impl;

private:
    void run();
    std::unique_ptr<Impl> impl_;
    std::thread thread_;
    std::atomic<bool> tapping_{ false }, silence_{ false };
    // the mix from the audio thread to the worker: 48 kHz stereo 16-bit, single producer, single consumer
    static constexpr unsigned Ring = 1u << 18;   // frames: 5.4 s
    int16_t* ring_ = nullptr;
    std::atomic<unsigned> written_{ 0 }, read_{ 0 };
    std::atomic<int> rate_{ 48000 };
    friend struct Impl;
};
#endif
