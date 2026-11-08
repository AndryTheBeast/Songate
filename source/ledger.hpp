/*
 * Songate · ledger.hpp
 * What Connections knows about Liked, playlists, tag edits and play counts: one record book, the same rules for all four, with no
 * sockets and no files of the app in it, so that it can be tested alone (tools/preview/ledgertest.cpp).
 *
 * Each record is a key and a value, with a Lamport clock (a counter, never the time of day: clocks disagree between devices) and
 * the device that wrote it. The higher clock wins, the device id breaking a tie, so every device reaches the same answer whatever
 * order things arrive in, and hearing the same record twice changes nothing.
 *
 *   Liked:     L<song>                    "1", or "" once unliked: the record is why an unlike is not undone by the other device
 *   Tag edits: E<song><US><field>         the value, or "" once undone
 *   Plays:     C<song><US><device>        that device's count, last played and date added. Only that device writes it, so
 *                                         they add up, and a sync run twice counts nothing twice
 *   Deleted:   X<song>                    "1" once deleted inside Songate, "" if it came back. Obeyed only where that device says so
 *   Their order: O                        the ids of your playlists, as the sidebar shows them
 *   Playlists: P<id>                      its name and songs. Changed on both sides since they last met, both are kept:
 *                                         the older one under a new id, named after the device it came from.
 *                                         "" once deleted; deleted on one side and changed on the other, the change stays
 *
 * Each device numbers the records it writes (seq). Two devices trade what they have heard ("have") and send only what the other
 * has not, in that order; "have" moves only over what was really taken in, so a sync stopped halfway just carries on next time.
 */
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Ledger {
public:
    struct Entry {
        std::string key, value, origin;   // origin: the device that wrote it
        uint64_t clock = 0, seq = 0;
    };
    using Have = std::map<std::string, uint64_t>;   // device → the last of its records heard

    explicit Ledger(std::string device = {}) : me_(std::move(device)) {}

    const std::string& device() const { return me_; }
    void setDevice(std::string device) { me_ = std::move(device); }
    const Entry* find(const std::string& key) const;
    /** This device's own change; nothing happens when the value is already that. */
    void put(const std::string& key, const std::string& value);

    Have have() const { return have_; }
    /** What the other device has not heard, oldest first for each device. */
    std::vector<Entry> since(const Have& theirs) const;
    /**
     * Takes in records from `from`, whose Have was `theirs` when this sync began. Returns the keys that changed here.
     * deviceName: what `from` is called, for a playlist kept from both sides. A record already held is ignored.
     */
    std::vector<std::string> merge(const std::vector<Entry>& incoming, const Have& theirs, const std::string& deviceName, const std::string& myName);

    const std::unordered_map<std::string, Entry>& entries() const { return all_; }
    size_t size() const { return all_.size(); }
    uint64_t clock() const { return clock_; }

    /** Text for ledger.tsv; load() refuses nothing it can read, and skips what it cannot. */
    std::string serialise() const;
    void load(const std::string& text);

    /** A playlist's value: its name, then its songs. */
    static std::string packList(const std::string& name, const std::vector<std::string>& songs);
    static void unpackList(const std::string& value, std::string& name, std::vector<std::string>& songs);
    /** One record as a line, for the link and the file alike. */
    static std::string packEntry(const Entry& e);
    static bool unpackEntry(const std::string& line, Entry& e);

private:
    void note(const Entry& e);

    std::string me_;
    std::unordered_map<std::string, Entry> all_;
    Have have_;
    uint64_t clock_ = 0;
};

namespace ledgerkey {
constexpr char kSep = '\x1f';
inline std::string liked(const std::string& song) { return "L" + song; }
inline std::string edit(const std::string& song, const std::string& field) { return "E" + song + kSep + field; }
inline std::string plays(const std::string& song, const std::string& device) { return "C" + song + kSep + device; }
inline std::string list(const std::string& id) { return "P" + id; }
// Lyrics: Y<song>, the lyrics Songate kept for it (found online and saved), or "" once removed (BETA 3; the user, 2026-11-04: "Yes,
// add lyrics to sync"). A Songate before BETA 3 keeps and passes on these records without using them.
inline std::string lyrics(const std::string& song) { return "Y" + song; }
// Moods: M<song>, your choice for its mood, "is,nots" (mood::Label: its mood or -1, and a bit for each mood it is not), or "" once
// cleared (BETA 3; the user, 2026-11-05: corrections travel, "Yes, with a switch"). An older Songate keeps and passes them on.
inline std::string mood(const std::string& song) { return "M" + song; }
}  // namespace ledgerkey
