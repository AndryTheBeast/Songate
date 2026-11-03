/*
 * Songate · lastfm.hpp
 * Last.fm: off until Settings › Last.fm allows it, and nothing goes online before that. A worker at cores::Net signs in (the
 * desktop flow), says what is playing (never retried) and sends the queue of scrobbles, oldest first, up to 50 at a time.
 *
 * scrobbles.tsv keeps the queue through restarts. lastfm-session.txt keeps the account: it is left out of backups, never
 * logged, never in a report and never sent to another device. bg-scrobbles.txt is what the background player heard long
 * enough for Last.fm's rule (its own: its plays count at 60 s, Stats::threshold); App::lastfmTick matches those to the library.
 *
 * lastfm_key.inc (git-ignored; tools/lastfm-key.py makes it from lastfm.key) holds the API key and secret, scrambled. Without
 * it a build simply cannot sign in.
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "net.hpp"

class Lastfm {
public:
    struct Song {
        std::string artist, title, album, albumArtist;
        int trackNo = 0;
        int duration = 0;   // whole seconds
        int64_t when = 0;   // when it began (seconds since 1970)
    };
    enum class Sign { None, Asking, Waiting, Failed };
    /** What Settings shows. */
    struct View {
        bool signedIn = false, lapsed = false;   // lapsed: Last.fm no longer takes this sign-in; the queue stays
        std::string user, url, error, status;    // url: the page to allow Songate on; status: the last send, in words
        Sign sign = Sign::None;
        int waiting = 0;
        int64_t expires = 0;                     // when the page stops working
    };

    /** Was this build given a key (lastfm_key.inc)? */
    static bool available();
    /** A scrobble counts: over 30 seconds, heard for half of it or 4 minutes. */
    static double threshold(double duration) { return duration > 30 ? (duration * 0.5 < 240 ? duration * 0.5 : 240) : -1; }

    void start(const std::string& dir);
    void stop();

    /** Every frame: what Settings allows, and whether the background player plays. */
    void set(bool on, bool scrobbling, bool nowPlaying, bool background) {
        on_ = on;
        scrobbling_ = scrobbling;
        nowOn_ = nowPlaying;
        background_ = background;
    }
    /** True while Songate keeps scrobbles: allowed, and an account (or one that lapsed) to send them to. */
    bool keeping() const { return on_ && scrobbling_ && (signedIn_ || lapsed_); }
    bool announcing() const { return on_ && nowOn_ && signedIn_; }
    bool waitingForSignIn() const { return waitingSignIn_; }

    void signIn();
    void cancelSignIn();
    void signOut();
    void nowPlaying(Song s);
    void scrobble(Song s);
    View view();
    /** What the background player heard (path, when it began; 0 when it had no clock), since the last call. */
    std::vector<std::pair<std::string, int64_t>> takeBackground();
    /** Lines for the app's log; no account, key or track in them. */
    std::vector<std::string> takeLog();

private:
    struct Answer {
        bool reached = false;   // Last.fm answered in its own format
        int code = 0;           // its error, 0 none
        std::string body, err;
    };
    using Params = std::map<std::string, std::string>;

    void run();
    Answer call(Params p, bool sign);
    void loadFiles();
    void saveQueue();
    void saveSession();
    void readBackground();
    void signStep();
    bool sendNow(const Song& s);
    void sendBatch();
    bool clockKnown();
    void note(const std::string& s);
    void fail(const std::string& why);
    void trim();

    std::string dir_;
    std::thread thread_;   // joined in stop(), never detached
    std::mutex m_;
    std::condition_variable wake_;
    net::Progress prog_;   // only its cancel: stop() ends a request under way
    bool quit_ = false;

    std::atomic<bool> on_{ false }, scrobbling_{ false }, nowOn_{ false }, background_{ false };
    std::atomic<bool> signedIn_{ false }, lapsed_{ false }, waitingSignIn_{ false };

    // the account (under m_; the key leaves here only in a signed request)
    std::string user_, key_;
    bool signOut_ = false;
    Sign sign_ = Sign::None;
    std::string token_, url_, signErr_;
    int64_t signUntil_ = 0;
    double pollAt_ = 0;

    std::deque<Song> queue_;
    bool queueDirty_ = false;
    bool hasNow_ = false;
    Song now_;
    std::string lastArtist_, lastTitle_;   // the last one queued, and when: the same play heard on both sides of a handover
    int64_t lastWhen_ = 0;
    int lastDuration_ = 0;

    std::string status_;
    double holdUntil_ = 0;    // after a failure (steady seconds), each time longer
    double backoff_ = 0;
    int64_t dayHold_ = 0;     // Last.fm's daily limit: not before this time (seconds since 1970)
    int64_t clockOff_ = 0;    // Last.fm's time minus ours, on the latest answer
    bool clockSeen_ = false;
    double bgAt_ = 0;
    bool bgFirst_ = true;
    std::vector<std::pair<std::string, int64_t>> bgFound_;
    std::vector<std::string> log_;
};
