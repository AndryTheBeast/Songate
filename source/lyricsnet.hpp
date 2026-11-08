/*
 * Songate · lyricsnet.hpp
 * Lyrics found online (BETA 3; the user, 2026-11-03: "search for lyrics when a certain music does not have any. And save the
 * lyrics"): LRCLIB (lrclib.net), a free and open lyrics database with no key. Only a song's title, artist, album and length are
 * sent. One worker thread asks, one question at a time, never on the main thread: a song's lyrics (/api/get, then /api/search
 * for a record within 2 s of its length), or every record for it (Not these), and it writes the lyrics kept, beside the song
 * or in Songate's own folder. LRCLIB's rules (lrclib.net/docs): the app named in the User-Agent (net.cpp), a pause between
 * questions, and its Retry-After honoured when it says too many.
 */
#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lyrnet {

/** One of LRCLIB's records. */
struct Found {
    int64_t id = 0;
    std::string title, artist, album;
    double duration = 0;
    bool instrumental = false;
    std::string synced, plain;   // LRC with times, and the words alone; either may be empty
    const std::string& text() const { return synced.empty() ? plain : synced; }   // synced, else plain (the user's choice)
};

// Embed: this song's lyrics (in Songate's folder) to be written inside it, onto the waiting list; EmbedRun: the next one written
// that is not busy (playing or next), if any
// Written: what Songate wrote into the music folder (written.tsv), for Remove; Unembed: lyrics Songate wrote inside a song, to be
// taken out when it is neither playing nor next (onto the waiting list)
// SyncSave: lyrics kept on another device, saved here as Settings say (never over the song's own); SyncRemove: removed there
enum class Ask { Lyrics, List, Save, Remove, Embed, EmbedRun, Written, Unembed, SyncSave, SyncRemove };

struct Query {
    Ask kind = Ask::Lyrics;
    uint64_t ticket = 0;
    std::string path, title, artist, album;   // the song
    double duration = 0;
    std::string text;                         // Save: the lyrics; Remove: "lrc" for an .lrc Songate made beside the song
    bool beside = true;                       // Save: beside the song first, else straight to Songate's folder
    std::vector<std::string> busy;            // EmbedRun: songs not to touch now (the one playing, the next)
    int saveTo = 0;                           // SyncSave: Settings' Save them (0 beside, 1 Songate's folder, 2 inside)
};

struct Answer {
    Ask kind = Ask::Lyrics;
    uint64_t ticket = 0;
    std::string path;
    bool reached = false;       // LRCLIB answered (or, for Save, the file was written)
    std::string err;            // why not, in words
    std::vector<Found> found;   // Lyrics: the best, or none; List: every record, the likeliest first
    std::string savedTo;        // Save: the file written; Remove: the file removed
    std::vector<std::string> written;   // Written: "tag\tpath" and "lrc\tpath", what Songate wrote
    bool tookOut = false;       // EmbedRun: lyrics taken out (Unembed), not written
};

/** Where Songate's own folder keeps lyrics (set once at start, before the loader reads any): config/…/lyrics. */
void setFolder(const std::string& dir);
/** A song's lyrics in Songate's own folder: the file's name from a hash of the song's path. */
std::string savedPath(const std::string& songPath);
/** That file's lyrics, or empty. Any thread. */
std::string readSaved(const std::string& songPath);
/** Dev builds: a line in lyrics-trace.txt, beside Songate's lyrics folder (the app's decisions too). Any thread. */
void trace(const std::string& line);

class Client {
public:
    ~Client() { stop(); }
    /** Asked from the main thread; a Lyrics or List question replaces one still waiting (only the newest song matters). */
    void ask(const Query& q);
    /** An answer, if one came (main thread). */
    bool take(Answer& out);
    /** Ends the worker (before the app exits). */
    void stop();

private:
    void run();
    Answer answer(const Query& q);
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false, started_ = false;
    std::vector<Query> waiting_;
    std::vector<Answer> done_;
    std::vector<std::string> inside_;   // the waiting list (inside.txt in Songate's lyrics folder), the worker's only: a path to write
                                        // into, or "del\t" and a path to take Songate's lyrics out of
    std::vector<std::string> written_;  // what Songate wrote (written.tsv): "tag\tpath", "lrc\tpath"
    bool writtenRead_ = false;
    void writtenLoad();
    void writtenSet(const std::string& kind, const std::string& path, bool on);
    bool insideRead_ = false;
    void insideLoad();
    void insideSave();
};

}  // namespace lyrnet
