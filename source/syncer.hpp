/*
 * Songate · syncer.hpp
 * One sync with a paired device, over Connect's encrypted link. The rules are in ledger.hpp and syncdata.hpp; this is the order of
 * things. A worker writes down what changed here and saves that at once, trades "have" with the other device, sends what it has
 * not heard and takes in what it sends. Only when all of it is here does anything change: the first time with this device, what
 * would happen is shown and the settings are backed up first; the main thread applies the changes; then the book is saved.
 *
 * Stopped at any point before that, the library is as it was and the next sync does it again. Files: ledger.tsv, sync-lists.tsv,
 * sync-seen.tsv, sync-peers.txt, all replaced whole, and sync-plays.tsv (Stats).
 */
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "syncdata.hpp"

class Connect;
class Copier;
class Identity;
class Levels;

class Syncer {
public:
    enum class State { Idle, Reading, Trading, Asking, Backing, Applying, Sharing, Done, Failed };
    struct View {
        State state = State::Idle;
        std::string peer, summary, error;   // summary: what the first sync would do, or what the last one did
        int sent = 0, received = 0;
        int measures = 0, covers = 0;       // taken in from the other device, this sync
        bool busy = false;                  // failed because the other device was syncing with a third: tried again soon
        bool shared = false;                // measurements and covers were traded, this sync
        uint64_t deleteFrom = ~uint64_t(0); // the clock the switch took effect from, once known
    };
    struct Job {
        syncdata::Snapshot now;
        unsigned kinds = syncdata::KindAll;
        bool share = true;        // measurements and covers this time: both devices must want to (G on the link)
        uint64_t deleteFrom = 0;  // deletions after this clock are obeyed; ~0: the switch was just turned on, from now
        std::string peerId, peerName, myId, myName;
        bool answering = false;   // the other device began this one
    };

    void start(const std::string& dir, Connect* connect, Identity* identity, Levels* levels, Copier* copier);
    void stop();
    /** A song deleted here, inside Songate: told to other devices at the next sync. Any thread. */
    void noteGone(const std::string& song);
    /** Lyrics Songate kept for this song (a path) removed here by hand: told to other devices at the next sync. Any thread. */
    void noteLyricsGone(const std::string& path);
    /** Runs one. False while one is under way. */
    bool begin(Job job);
    View view();
    /** A device that wants to sync (its id), each asked once; "" none. */
    std::string asked();
    /** The first sync's answer. */
    void accept();
    void decline();
    /** Backs up the settings, on the worker; false stops the sync. */
    std::function<bool()> backup;
    /** The changes, for the main thread, once; it calls applied() when they are in. */
    bool takeChanges(syncdata::Changes& out);
    void applied();

private:
    void run();
    void work(Job job);
    bool trade(const Job& job, const Ledger::Have& mine, Ledger::Have& theirs, std::vector<Ledger::Entry>& in);
    void share(const Job& job);
    void fail(const std::string& why);
    void load();
    void saveBook();
    void saveState();
    bool pump(double seconds);

    std::string dir_;
    Connect* connect_ = nullptr;
    Identity* identity_ = nullptr;
    Levels* levels_ = nullptr;
    Copier* copier_ = nullptr;
    std::thread thread_;   // joined in stop(), never detached
    std::mutex m_;
    std::condition_variable wake_;
    bool quit_ = false, have_ = false, answered_ = false, accepted_ = false, appliedDone_ = false, changesReady_ = false;
    std::vector<std::string> asked_;   // devices that asked, in order
    std::string peer_;                 // the worker's: the device of the sync under way
    std::map<std::string, std::vector<std::vector<uint8_t>>> early_;   // a device's H that came before its sync began here
    Job job_;
    View view_;
    syncdata::Changes changes_;

    Ledger book_;
    syncdata::ListIds ids_;
    syncdata::Seen seen_;
    std::vector<std::string> met_;   // devices a first sync was done with
    std::vector<std::vector<uint8_t>> inbox_;
    // What a step got but does not read, kept for the step it is for: the other device's G can arrive while this one still trades,
    // and an S (a new sync) while it shares. Dropped, they left one device sharing alone for 45 s and the other's next sync
    // unanswered for 240 s, the arrows turning (the user, 2026-11-02: "the sync got stuck on the console")
    std::vector<std::vector<uint8_t>> later_;
    std::vector<std::string> goneQueue_;   // to sync-gone.txt, on the worker
    std::vector<std::string> lyricsGoneQueue_;   // paths (noteLyricsGone)
};
