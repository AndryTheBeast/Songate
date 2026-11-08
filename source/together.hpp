/*
 * Songate · together.hpp
 * Listening together: the messages of a session and how a list of songs is matched, with no sockets and nothing of the app in them,
 * so that tools/preview/togethertest.cpp can attack them alone. The session itself is in view_together.cpp, the sound in player.cpp,
 * the clock in connect.cpp.
 *
 * A message begins with a t, then its kind. Only songs' hashes and lengths cross, times, and a song's loudness over 8 s when the two
 * hold it as different files: never a path, a key or a name of a file. Every field from the other device is checked here before
 * anything believes it.
 *
 * Who decides: the device that started the session. The other asks it to skip, pause or seek, and both act at one shared moment, on
 * the starter's clock. Songs are named by their place in the list that device sent.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "automix.hpp"

namespace together {

constexpr size_t kMaxSongs = 20000;

struct Song {
    std::string exact, loose;   // identity.hpp's two hashes; either may be ""
    double duration = 0;
    // a server's song (the user, 2026-11-07: Listening together and Handoff with server libraries, "Both"): the server's print
    // (Identity::textHash of its address: never the address, the account or a key) and the song's id there; both "" for your own
    std::string server, sid;
};
constexpr size_t kMaxSongId = 128;

enum class Kind : char {
    Invite = 'I',    // shall we?
    Answer = 'A',    // flag: yes
    Queue = 'Q',     // the songs
    Have = 'H',      // which of them this device holds: have, one byte each; song: 1 it has a microphone, 2 its output's delay was
                     // never measured
    Prepare = 'P',   // open this song at this position; flag: the track playing plays out first
    Ready = 'R',     // it is open
    Go = 'G',        // start it at this moment
    Seek = 'S',      // jump to this position at this moment; flag: playing
    Pause = 'Z',     // pause at this moment
    Ask = 'C',       // the other device asks: what is one of the Ask letters
    Join = 'J',      // Automix or a crossfade into this song: plan, the same numbers on both; flag: Automix (else a crossfade)
    Handoff = 'O',   // this device's queue, for the other to take over: song, where it is in the list; position; flag: playing
    Taken = 'K',     // flag: it did
    Resume = 'Y',    // the link is back: this song plays this position at this moment; flag: paused
    Leave = 'L',
    Print = 'F',     // a song held as another file (the same tags, other bytes): from the other device, the print from `position`
                     // of its own; flag: this is it (print, empty if its file would not read)
    Who = 'W',       // the starter: who changes what plays; flag: only the starter (else both); song: its order (Order, 0..3), which
                     // decides what comes next on both
    Members = 'M',   // the starter: the other devices in the session (BETA 3: three or more), their names in list, one a line
    Tones = 'T',     // the delay, measured (micmeasure.hpp): chirp in slot `song` of `serial` slots from the moment `at`; flag: you listen
    Delay = 'D'      // to the starter, from the device listening: "slot ms" a line ("slot -": not heard; "!words": it failed); from the
                     // starter: "ms", this device's correction
};
/** What the device that did not start the session may ask for. */
constexpr char AskPause = 'p', AskPlay = 'r', AskSeek = 's', AskSong = 'o', AskOrder = 'm';   // AskOrder: song is the order wanted
constexpr char AskWhere = 'w';   // a track ended here with nothing after it: where do things stand (answered as a link coming back)

struct Message {
    Kind kind = Kind::Leave;
    bool flag = false;
    char what = 0;
    int64_t serial = 0;    // the track change or jump it belongs to
    int song = -1;
    double at = 0, position = 0;
    std::vector<Song> songs;
    std::string list;      // Handoff: the playlist its queue came from, by name ("": none), so the other device plays it as one
    std::vector<uint8_t> have;
    std::vector<uint8_t> print;
    automix::Plan plan;
};

std::vector<uint8_t> pack(const Message& m);
/** False for anything that is not exactly what pack() makes. */
bool unpack(const std::vector<uint8_t>& bytes, Message& out);

struct Held {
    Song song;
    int track = -1;
};
/** For each of theirs, the track here: the same bytes first, then the same song within 2 s; -1 if it isn't here. */
std::vector<int> match(const std::vector<Song>& theirs, const std::vector<Held>& mine);
/** From `from` on, the first place with have set, going round once; -1 if none. */
int firstHeld(const std::vector<uint8_t>& have, int from);

/*
 * One song, two files (the user, 2026-11-02: "Align them"): a FLAC here and an M4A there begin their music a few milliseconds apart (an
 * encoder's delay, a rip's start), and the same position is not the same moment. Each device takes a print of its own file: its
 * loudness every 2 ms over 8 s, decoded from the start as the player decodes it, one byte a step (about 4 KB). The device that did not
 * start the session takes a little more on each side and finds where the other's fits, within 150 ms. Only for songs matched by
 * their tags and length and not by their bytes; a song held as the same bytes needs none.
 */
constexpr double kPrintStep = 0.002, kPrintSeconds = 8, kPrintReach = 0.15;
constexpr int kPrintSteps = 4000, kPrintReachSteps = 75;
/** Where the window starts in a song this long; < 0: too short for one. */
double printFrom(double duration);
/** Interleaved 16-bit sound at `rate` with `channels`, from the window's start: one byte a step, as long as the sound lasts. */
std::vector<uint8_t> loudness(const int16_t* pcm, size_t frames, int rate, int channels);
struct Aligned {
    bool clear = false;
    double shift = 0;        // a moment at `t` in their file is at `t + shift` in this one (s)
    double score = 0, next = 0;   // the best fit's correlation, and the best one more than 20 ms from it
};
/** theirs: kPrintSteps from the window's start; mine: kPrintReachSteps more on each side. */
Aligned align(const std::vector<uint8_t>& theirs, const std::vector<uint8_t>& mine);

}  // namespace together
