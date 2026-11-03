/*
 * Songate · airplay.hpp
 * An iPhone plays to this device: an AirPlay 1 receiver. Off until allowed, and it holds no socket then.
 *
 * A worker at cores::Net answers mDNS, so iOS lists this device; answers iOS's RTSP, signing its challenge with the AirPort key
 * (airport_key.inc: iOS sends nothing without it, proved with tools/clicktest), and takes the session's AES key out of its wrapping;
 * then takes in RTP packets of ALAC over UDP, asks again for the ones lost, decrypts and decodes them, and leaves 48 kHz stereo in a ring
 * that Player::stream reads on the audio thread. Everything from the network is checked: anyone on this network can reach these ports
 * while the switch is on.
 *
 * On the console and the PC (GF_AIRPLAY; the PC builds Mbed TLS 2.28.10, the console's, in tools/windows/build-deps.sh, and is
 * announced through Windows' own mDNS responder, which holds port 5353 there).
 */
#pragma once
#ifdef GF_AIRPLAY
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AirPlay {
public:
    struct View {
        bool streaming = false;
        std::string peer, title, artist, album, error;
        float gain = 1;
        int lost = 0, repaired = 0;
        // levelling: this song's loudness so far (BS.1770, as Songate measures its tracks), measured from its start or a skip
        double lufs = 0, peak = 0, measured = 0;   // measured: seconds of it; lufs and peak mean nothing before some
        // A device this one does not know wants to play (the user, 2026-11-02: "Ask for new devices"): its request waits up to
        // 20 s for answer(); unanswered, it is refused
        bool asking = false;
        std::string askId, askName;
        unsigned coverGen = 0;   // a new cover (or none) arrived: takeCover() has it
        double position = 0, duration = 0;   // seconds, as the iPhone last said (at a new song or a seek)
        unsigned progressGen = 0;
    };
    void start(const std::string& deviceName);
    void stop();
    /** The switch: sockets open and close with it. Every frame, cheap when nothing changes. */
    void want(bool on);
    View view();
    /** The devices that play without asking (their ids: "name:" and the name iOS gives the device). Main thread, when the list changes. */
    void allow(std::vector<std::string> ids);
    /** The answer to View::asking: let it play, or not (it then hears no). Remembering it is the caller's (allow). */
    void answer(bool yes);
    /** The last cover iOS sent (JPEG or PNG; empty: none), moved out: the view stays cheap to copy each frame. */
    std::vector<uint8_t> takeCover();
    /** Audio thread: up to `frames` of 48 kHz stereo; silence is the caller's for the rest. */
    int read(int16_t* out, int frames);

private:
    struct Impl;
    void run();
    void session();

    std::string name_;
    std::thread thread_;   // joined in stop(), never detached
    std::mutex m_;
    std::condition_variable wake_;
    bool quit_ = false, wanted_ = false;
    std::vector<std::string> allowed_;   // under m_
    int answer_ = -1;                    // under m_: -1 waiting, 0 no, 1 yes
    std::condition_variable answered_;
    std::vector<uint8_t> cover_;   // under m_
    View view_;
    static constexpr unsigned Ring = 1u << 18;   // frames: 5.4 s
    int16_t* ring_ = nullptr;
    std::atomic<unsigned> written_{ 0 }, read_{ 0 };
    std::atomic<bool> primed_{ false }, clear_{ false };
    double lastRead_ = 0;   // the audio thread's: when it last asked, to see that Songate was away (HOME) meanwhile
    friend struct Impl;
};
#endif
