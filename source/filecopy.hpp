/*
 * Songate · filecopy.hpp
 * Connections' music files: a device with Music files on takes the songs it lacks from a paired device, each into the same folder
 * of the music folder as it has there, so the two libraries match (the user, 2026-11-01). Never replaces a file. A song counts as
 * held if the same song is here at all, in any format: a FLAC is not sent to a device with its AAC.
 *
 * Each file travels as <exact hash>.part beside where it will live, a piece at a time, each piece asked for: so a drop just
 * carries on from where the .part ends, music playing slows it (the asking is the brake), and a stop leaves nothing half-written
 * among your music. A file is checked against its hash, then renamed. Only names of files in the other device's library can be
 * asked for, by hash: never a path. copied.txt keeps the hashes of what came: so a file is never taken twice while the library
 * catches up, and a song you delete here does not come back.
 *
 * A file's name on the Switch must be one HOS can read: safeName() keeps only what the user's Switch library, proved on the console,
 * is made of (the user, 2026-10-31: ´ and ’ cost hours to find). Tags keep the real title.
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

class Connect;

namespace filecopy {

struct Item {
    std::string exact, song, path;
    int64_t size = 0;
    double duration = 0;
};
struct Offer {
    std::string exact, song, name;   // name: its path inside the music folder there, / between folders
    int64_t size = 0;
    double duration = 0;
};

/** A name this device can read: on the Switch letters, digits, spaces and ! # $ % & ' ( ) + , - . [ ], accents and lookalikes made
 *  plain; on the PC only what Windows forbids goes. Its extension stays; at most 122 bytes; "" if nothing is left of it. */
std::string safeName(const std::string& name, bool forSwitch);
/** A path inside the music folder there, as a folder inside it here: each folder a name this device can read; nothing that climbs
 *  out of it, at most 8 deep. "" when the file is at the top. */
std::string safeFolders(const std::string& path, bool forSwitch);
/** taken: free among these names, with " (2)" before the extension if not. Compares without case: the card does. */
std::string freeName(const std::string& name, const std::unordered_set<std::string>& takenLower);
/** Of theirs, what to copy here: supported formats, one file of each song, none already here (the same song and length within 2 s). */
std::vector<Offer> toCopy(const std::vector<Offer>& theirs, const std::vector<Item>& mine);
bool hashLike(const std::string& s);
/** Offers as text and back: names and hashes from another device are checked here. */
std::string packOffer(const Offer& o);
bool unpackOffer(const std::string& line, Offer& o);

}  // namespace filecopy

class Copier {
public:
    enum class State { Idle, Looking, Asking, Copying, Done, Failed };
    struct View {
        State state = State::Idle;
        std::string peer, current, error;
        int done = 0, total = 0, failed = 0, offered = 0;
        int64_t bytes = 0, totalBytes = 0, free = 0;
    };

    void start(const std::string& dir, Connect* connect);
    void stop();
    /** This device's songs, once a sync has them: what it can give, and what it need not take. complete: every one of them is in
     *  items, none still waiting for its identity. Only then may it take: a song not known yet would be copied again. */
    void library(std::vector<filecopy::Item> items, std::unordered_set<std::string> gone, bool complete);
    /** The music folder: where files are served from, and where they arrive. */
    /** Every frame: is it wanted, can it run (not in applet mode), and is music playing (then it goes slowly)? */
    void set(bool want, bool playing) { want_ = want; playing_ = playing; }
    void place(const std::string& musicFolder, const std::string& peerId, const std::string& peerName);
    void accept();
    void decline();
    /** Stops for now; a part-copied file stays, and goes on next time. */
    void pause();
    View view();
    /** Files arrived since the last call? Then the library is read again. */
    bool takeArrived();

private:
    void run();
    void serve(const std::vector<uint8_t>& m);
    void takeList(const std::string& text);
    void begin();
    void nextFile();
    void chunk(const std::vector<uint8_t>& m);
    void finishFile();
    void askChunk();
    void fail(const std::string& why);
    void endSession(const std::string& why);

    std::string dir_, dest_, peerId_, peerName_;
    Connect* connect_ = nullptr;
    std::thread thread_;   // joined in stop(), never detached
    std::mutex m_;
    std::condition_variable wake_;
    bool quit_ = false, answered_ = false, accepted_ = false, paused_ = false, arrived_ = false;
    std::atomic<bool> want_{ false }, playing_{ false };
    bool complete_ = false;
    std::vector<filecopy::Item> items_;
    std::unordered_set<std::string> gone_;   // songs deleted on some device: never fetched back
    View view_;

    // the worker's
    std::vector<filecopy::Offer> theirs_, plan_;
    size_t at_ = 0;
    FILE* in_ = nullptr;      // the file being read for the other device
    std::string inExact_;
    FILE* out_ = nullptr;     // the .part being written
    int64_t offset_ = 0;
    double askedAt_ = 0, nextAskAt_ = 0, triedAt_ = -1000, activeUntil_ = 0, servedAt_ = 0;
    bool waiting_ = false, sessionOn_ = false;
    std::unordered_set<std::string> badThisRun_, copied_;
    bool copiedRead_ = false;
    std::vector<std::string> met_;
};
