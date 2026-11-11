/*
 * Gatefold NX · player.hpp
 * Playback when the background player is not there. Every format goes through FFmpeg (fed through
 * Mix_HookMusic), so crossfade and the equalizer work the same for all of them and the same as in the
 * background player; SDL_mixer's own decoders are only a fallback for a file FFmpeg cannot open.
 * The position is counted from the frames actually delivered.
 *
 * Crossfade: load() with a fade keeps the track playing now as an outgoing voice that fades out while
 * the new one fades in (equal power); the position, the end and the spectrum follow the new one.
 * Automix (automix.hpp) adds to that a start at an exact sample of the outgoing track, the incoming one
 * started where its music (or first beat) begins, and a bass swap: each voice split at 150 Hz, the
 * incoming bass held out until the swap point, then handed over quickly, so two kick drums never pile up.
 * During any fade the outgoing track keeps its own levelling gain.
 */
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "automix.hpp"
#include "bgclient.hpp"
#include "ffdecoder.hpp"
#include "gfeq.hpp"

struct _Mix_Music;

class AirSend;   // (airsend.hpp: AirPlay speakers this device plays to)

class Player {
public:

    bool init();
    void shutdown();

    /**
     * Starts playback, or waits paused at 0. fade > 0 with a track playing: crossfades into it over
     * that many seconds (the track playing fades out meanwhile); otherwise it cuts, as before.
     */
    bool load(const std::string& path, bool paused = false, double fade = 0, const automix::Plan* mix = nullptr);
    void stop();
    void togglePause();
    void seek(double seconds);
    /**
     * Listening together. On: the audio callback's times are fitted to a line, which says when any sample of this device's stream
     * plays on this device's clock (tools/clicktest proved it on both platforms). Shared time is the other device's clock where that
     * one started the session, this device's own otherwise.
     *   syncClock: the offset from this clock to shared time, at `at`, and its slope. Each frame.
     *   syncDelay: this device's output delay, seconds: what it writes that much ahead of the shared moment.
     *   syncStart: a track loaded paused at `position` starts at that shared moment, on its exact sample.
     *   syncPauseAt: it pauses at that shared moment.
     * While it plays, one frame more or one fewer is taken into a buffer when the stream falls off the shared clock: at most 488 ppm
     * at 2048 frames, 0.85 cents, for one buffer. Seeks and pauses are the session's: see trap.
     */
    void syncEnable(bool on);
    bool syncEnabled() const { return syncOn_; }
    void syncClock(double at, double offset, double slope);
    void syncDelay(double seconds) { syncDelay_ = seconds; }
    void syncStart(double sharedAt, double position);
    void syncPauseAt(double sharedAt) { syncPause_ = sharedAt; }
    /** Silent now, waiting for a syncStart. */
    void syncHold();
    /** On this device, in shared time: now (from the fit), and the stream's error against the shared clock, seconds. */
    double syncNow() const;
    float syncError() const { return syncErr_; }
    /** Started, or waiting to? */
    bool syncRunning() const { return syncRun_ || syncWait_; }
    /** The next load() may take what prepare() made, with no Automix plan. */
    void prepareTimed(const std::string& path, double offset, bool exact = false);
    /** Its stream fell off the shared clock by more than a trim mends. Once. */
    bool takeLost() { return syncLost_.exchange(false); }
    /** The shared moment this position of the running start plays at, and that position at a moment. */
    double syncMomentOf(double position) const { return syncWantT_.load() + (position - syncWantP_.load()); }
    double syncPositionAt(double moment) const { return syncWantP_.load() + (moment - syncWantT_.load()); }
    /** Freezes noticed: dev log. */
    unsigned syncFreezes() const { return syncFroze_; }
    /** Pressing pause or seeking, while a session runs: the session decides what happens, on both. Returns true when it took it over. */
    std::function<bool(bool seek, double seconds)> trap;
    /** Plays and seeks on behalf of a session, not asked of the trap. */
    void seekNow(double seconds);
#ifdef GF_DEV
    /** Dev: a quiet chirp each whole second of the song, for one microphone to time real music by (tools/clicktest/analyze.py). */
    void syncChirps(bool on) { syncChirps_ = on; }
#endif
    /** Listening together's delay, measured (micmeasure.hpp): a chirp at each of these moments on the shared clock, as this device's sound
     *  is timed (its Timing included), over whatever plays. Replaces the ones before; {} none. */
    void tones(std::vector<double> sharedStarts, int slot = 0) {
        std::lock_guard<std::mutex> g(toneLock_);
        tones_ = std::move(sharedStarts);
        toneSlot_ = slot;
    }
    /** The last of those chirps' start on the shared clock (0: none). */
    double tonesLast() {
        std::lock_guard<std::mutex> g(toneLock_);
        double last = 0;
        for (double t : tones_) last = t > last ? t : last;
        return last;
    }
    /** A steady_clock reading (seconds) on the shared clock: the microphone's stamps. Any thread. */
    double sharedAt(double steady) const { return steady + syncTimeNow_.load() - syncTimeAt_.load(); }
    /**
     * Sound from elsewhere (AirPlay): read gives 48 kHz stereo on the audio thread, and plays over nothing else: the caller stops the
     * music first. Through levelling (streamLevel, from the stream's own measurement), the equalizer, then the volume set times gain.
     */
    void streamBegin(std::function<int(int16_t*, int)> read);
    void streamEnd();
    void streamGain(float g) { streamGain_ = g; }
    void streamLevel(float g) { streamLevel_ = g; }   // linear; eased over a few seconds in the hook
    bool streaming() const { return streamOn_; }
    /** How many seeks so far (the UI thread's): a join's show ends on one (App::endJoinShow). */
    unsigned seeks() const { return seeks_; }
    /** The main thread, once a frame: closes an outgoing voice whose fade has ended (not on the audio thread). */
    void update();
    /** Automix (v3): the track playing starts towards the next one's tempo (the plan's speed), a few beats before the join. */
    void beginStretch(const automix::Plan& p);
    /**
     * Automix: the next track opened and moved to the plan's offset on a worker (cores::Loader), a few seconds before its
     * join; load() takes it from there. Opening and decoding up to the offset (10-30 s of audio for a drop join) on the
     * main thread made the first frames of the join's animation stutter (the user, 2026-10-28).
     */
    void prepare(const std::string& path, double offset);
    /** prepare()'s worker still opening the next track: load() would wait for it on the main thread (Automix waits a frame). */
    bool preparing() const { return prepRunning_.load(); }
    /**
     * The dev build's audio trace (Settings, Debug: written to audio-trace.txt): since the last call, the longest audio
     * callback (ms), callbacks that took longer than their buffer lasts, buffers lost to the lock (silence), and reads
     * that found the read-ahead dry (ffStarved, counted since the start).
     */
    struct Trace { float worstMs = 0; unsigned late = 0, lockMiss = 0, starved = 0; };
    Trace takeTrace();

    bool loaded() const;
    bool paused() const;
    double position() const;
    int sampleRate() const { return remote() ? 48000 : rate_; }

    void setRemote(BgClient* bg) { bg_ = bg; }
    bool remote() const { return bg_ && bg_->connected(); }

    /** 0..1, applied to music playback. */
    void setVolume(float v);
    /** Plays stereo floats at 48 kHz once over whatever else sounds, at the volume set (the opening's bells); `sound` must outlive it. */
    void playOnce(const std::vector<float>* sound);
    bool oncePlaying() const { return oncePlaying_; }
    void stopOnce() { if (oncePlaying_) onceStop_ = true; }
    /** Volume levelling (levels.hpp): a linear gain on everything played here, eased over ~0.4 s, or at once. */
    void setLevel(float gain, bool now = false) { levelTarget_ = gain; if (now) level_ = gain; }
    /** The equalizer for what is played here (the background player has its own, GfbgCmd_SetEq). */
    void setEq(const GfbgEq& e);

    /** Newest n mono samples of what was mixed; false when nothing new since the last call. */
    bool latestPcm(float* out, int n);

    /**
     * The join editor's Listen (view_join.cpp): a join rendered in memory, 16-bit stereo at sampleRate(), played here
     * over everything else (the caller pauses the music first; the background player's too). stopPreview ends it.
     */
    void preview(std::vector<int16_t>&& pcm);
    void stopPreview();
    bool previewing() const { return pvOn_ && pvAt_.load() < pvLen_.load(); }
    double previewPos() const { return double(pvAt_.load()) / rate_; }

    /** True once after a track has played to its end. */
    bool takeFinished() { return finished_.exchange(false); }
    /** AirPlay speakers (airsend.hpp): the finished mix goes to them, and this device stays quiet while one plays. nullptr: none. */
    void setAirSend(AirSend* a) { air_.store(a, std::memory_order_release); }

private:
    std::atomic<AirSend*> air_{ nullptr };
    static void postMix(void* self, unsigned char* stream, int len);
    static void postMixInner(void* self, unsigned char* stream, int len);
    static void musicFinished();
    static void ffHook(void* self, unsigned char* stream, int len);
    void feedRing(const float* stereo, int frames);
    void syncTick(int frames, double now);

    // listening together (the audio thread's, but for what is atomic or under syncLock_)
    struct SyncFit {
        static constexpr int N = 1024;
        double f[N], t[N], slope = 1.0 / 48000, lastF = 0, lastT = 0;
        int n = 0, at = 0;
        void clear() { n = at = 0; slope = 1.0 / 48000; }
        void add(double frame, double when);
        double time(double frame) const { return lastT + (frame - lastF) * slope; }
    };
    SyncFit syncFit_;
    std::mutex syncLock_;
    double syncAt_ = 0, syncOffset_ = 0, syncSlope_ = 0;   // syncLock_
    double syncOffsetIn_[3] = {};                          // the callback's copy
    double syncFrames_ = 0, syncTrim_ = 0, syncSum_ = 0, syncOwed_ = 0, syncBase_ = 0;
    double syncT_ = 0, syncP_ = 0;                         // playing: this shared moment is this position
    std::atomic<double> syncDelay_{ 0 }, syncWantT_{ 0 }, syncWantP_{ 0 }, syncPause_{ 0 }, syncTimeNow_{ 0 }, syncTimeAt_{ 0 };
    std::mutex toneLock_;
    std::vector<double> tones_;   // tones(): taken by the audio thread with try_lock
    double bedPhase_ = 0;          // the wake tone's phase under them (the audio thread's)
    int toneSlot_ = 0;            // whose sweep (micmeasure.hpp: up or down)
    std::atomic<bool> syncOn_{ false }, syncWait_{ false }, syncRun_{ false }, syncReset_{ false }, syncChirps_{ false }, prepTimed_{ false };
    std::atomic<bool> syncLost_{ false }, prepExact_{ false };
    std::atomic<unsigned> syncFroze_{ 0 };
    int syncBad_ = 0;
    std::atomic<bool> syncRebase_{ false };
    double syncRebaseP_ = 0;
    std::atomic<float> syncErr_{ 0 };
    std::function<int(int16_t*, int)> stream_;
    std::atomic<bool> streamOn_{ false };
    std::atomic<float> streamGain_{ 1.f }, streamLevel_{ 1.f };
    float streamLv_ = 1.f;   // the audio thread's eased copy
    float streamMono_[8192 * 2] = {};
    int16_t syncBuf_[(4096 + 2) * 2] = {};
    int16_t syncLast_[2] = {};

    BgClient* bg_ = nullptr;
    unsigned seeks_ = 0;
    _Mix_Music* music_ = nullptr;
    int rate_ = 48000;
    int frameBytes_ = 4;
    std::atomic<long long> frames_{ 0 };
    std::atomic<bool> finished_{ false };
    static Player* instance_;

    // FFmpeg path: the track playing (cur_) and, during a crossfade, the one fading out (out_)
    std::unique_ptr<FfDecoder> cur_, out_, done_;   // done_: an outgoing voice that ended, closed by update()
    // prepare(): the worker and what it made (read only after joining it)
    std::thread prepThread_;
    std::unique_ptr<FfDecoder> prepDec_;
    std::string prepPath_;
    double prepOffset_ = -1, prepOffsetUsed_ = 0;
    void prepJoin() { if (prepThread_.joinable()) prepThread_.join(); }
    std::atomic<bool> prepRunning_{ false };   // prepare()'s worker has not finished
    std::atomic<bool> doneSet_{ false };   // done_ holds one: update() takes the lock only then (the callback only tries it)
    std::atomic<unsigned> worstUs_{ 0 }, late_{ 0 }, lockMiss_{ 0 };   // the trace
    std::mutex ffLock_;   // the audio callback decodes; load, seek and stop come from the UI thread
    std::atomic<bool> ffActive_{ false }, ffPaused_{ false }, ffEnded_{ false };
    long long fadeLen_ = 0, fadeAt_ = 0;   // frames: the crossfade's length and how far it is (ffLock_)
    long long delay_ = 0;                  // frames of the outgoing track still to play before the incoming one starts (Automix)
    float outLevel_ = 1;                   // the outgoing track's levelling gain, kept through the fade
    bool outEcho_ = false;   // the join echoes the outgoing track: its fade outlasts its end
    std::vector<int16_t> pv_;              // the preview (ffLock_)
    std::atomic<bool> pvOn_{ false };
    std::atomic<size_t> pvAt_{ 0 }, pvLen_{ 0 };   // frames
    automix::Fader fader_;                 // the fade's gains, EQ swaps, filters and echo (shared with tools/preview/mixtest)
    automix::OutLoop outLoop_;             // the outgoing voice, its last bars looped when the join asks
    automix::Stretcher stretch_;           // the track playing, at the next one's tempo before and through a join (Automix v3)
    automix::Stretcher stretchOut_;        // … handed over with it when it becomes the outgoing voice (its buffered audio must not be skipped)
    float outF_[4096 * 2] = {};            // the outgoing voice, as floats at its own level
    float mixIn_[4096 * 2] = {};
    int16_t pcmIn_[4096 * 2] = {};

    // The equalizer (gfeq.hpp), run in the hook. Settings are handed over through eqWant_ (eqLock_), which
    // the hook takes up when it can: a slider dragged never makes the hook wait, nor skip a buffer.
    GfEq eq_{ 48000 };
    GfbgEq eqWant_ = gfEqDefault();
    std::mutex eqLock_;
    std::atomic<bool> eqNew_{ false };

    static constexpr int Ring = 8192;
    float ring_[Ring] = {};
    std::atomic<unsigned> written_{ 0 };
    unsigned lastRead_ = 0;
    int channels_ = 2;
    bool float32_ = false;
    const std::vector<float>* once_ = nullptr;
    size_t onceAt_ = 0;
    std::atomic<bool> oncePlaying_{ false }, onceStop_{ false };   // (the audio thread ends it, so `sound` is never freed under it)
    std::atomic<float> volume_{ 1.f }, vol_{ 1.f };   // vol_: volume_ eased in the hook, so a slider drag never crackles
    std::atomic<float> level_{ 1.f }, levelTarget_{ 1.f };
};
