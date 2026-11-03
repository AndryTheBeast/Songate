/*
 * Songate · syncdata.hpp
 * Between the app's data and the record book (ledger.hpp), with neither sockets nor the app in it (tools/preview/synctest.cpp).
 *
 * scan() writes down what changed here since the last sync, by song. plan() works out what the records mean for this library:
 * what to add, change or take away, as a list the app applies on its own thread. The record book is saved only after that is
 * applied: stopped between them, the next scan finds the change here and repeats nothing; the other way round it would read a
 * like not yet applied as an unlike.
 *
 * A song is its identity (identity.hpp), from the file's own tags: an edit does not make it another song. Several copies of a song
 * here are one song: liked if any is, its edits on all of them, its plays added up, and what others played shown on the first.
 *
 * Plays are kept apart: stats.tsv stays what was played here, and what other devices played is a number of its own beside it
 * (sync-plays.tsv), shown added together. So a sync stopped at any moment can count nothing twice: nothing is ever subtracted.
 */
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ledger.hpp"

namespace syncdata {

enum Kind : unsigned { KindLiked = 1, KindLists = 2, KindEdits = 4, KindPlays = 8, KindAll = 15, KindDeletes = 16 };

struct Song { std::string path, id, exact; double duration = 0; int64_t size = 0; };   // exact: the same bytes (identity.hpp)
struct Plays { int plays = 0; int64_t last = 0, added = 0; };
/** One of your own playlists, and what kind it is: an Automix playlist stays one on the other device, with its icon and first track. */
struct List {
    std::string name;
    std::vector<std::string> paths;
    bool automix = false, manual = false;
    std::string pin, icon;   // pin: a path
    // A folder playlist (the user, 2026-11-02: "Yes, as a folder"): its folder within the music folder ("A/B"), and no songs:
    // each device fills it from its own copy of that folder, so two that differ a little do not correct each other for ever
    std::string folder;
};

/** The library as it is now, copied on the main thread. */
struct Snapshot {
    std::vector<Song> songs;
    std::vector<std::string> liked;                                                  // paths
    std::unordered_map<std::string, std::map<std::string, std::string>> edits;       // path → field → value
    std::vector<List> lists;                                                         // your own playlists
    std::unordered_map<std::string, Plays> plays;                                    // path → played here, and when
    std::unordered_map<std::string, int> others;                                     // path → played on other devices
    std::vector<std::string> gone;                                                   // songs deleted here, inside Songate, since the last sync
    uint64_t deleteFrom = 0;                                                         // only deletions after this clock are obeyed
};

/** What the app is to do. */
struct Changes {
    std::vector<std::string> likedAdd, likedRemove;
    struct Edit { std::string path, field, value; };   // value "": the file's own tag again
    std::vector<Edit> edits;
    std::vector<List> listsNew, listsSet;               // listsSet: by name
    struct Rename { std::string from, to; };
    std::vector<Rename> listsRename;
    std::vector<std::string> listsDelete;               // by name
    std::vector<std::pair<std::string, Plays>> plays;   // path → its plays on other devices, last played and date added
    std::vector<std::string> listOrder;                 // your playlists' names, in the sidebar's order; empty: leave it
    std::vector<std::string> filesDelete;               // paths: deleted on another device, and this one says to follow
    bool empty() const { return filesDelete.empty() && likedAdd.empty() && likedRemove.empty() && edits.empty() && listsNew.empty() && listsSet.empty() && listsRename.empty() && listsDelete.empty() && plays.empty() && listOrder.empty(); }
    /** Words for the first sync's preview. */
    std::string summary() const;
};

/** Playlist id → name, kept in sync-lists.tsv: a name can change, the id does not. An id whose name is "" is deleted here. */
using ListIds = std::map<std::string, std::string>;

/**
 * What this device held when it last finished a sync (sync-seen.tsv). A record says a song is liked and it is not liked here: that
 * is an unlike only if it was liked here before. A song that has only now come to this library had simply not got it yet.
 */
struct Seen {
    std::set<std::string> keys;    // the Liked and edit records in force here
    std::set<std::string> songs;   // the songs here
};

/** addOnly: the first sync after an older Songate ran on this folder (it keeps no book, and a reset there looks like a hundred
 *  unlikes here): nothing missing here counts as taken away. No unlike, no edit undone, no song out of a list, no list deleted;
 *  what is missing comes back from the other device instead. */
void scan(Ledger& ledger, const Snapshot& now, ListIds& ids, const Seen& seen, unsigned kinds, bool addOnly = false);
/** after: the record book with what came in merged. seen becomes what will be true once the changes are applied. */
Changes plan(const Ledger& after, const Snapshot& now, ListIds& ids, Seen& seen, unsigned kinds);

}  // namespace syncdata
