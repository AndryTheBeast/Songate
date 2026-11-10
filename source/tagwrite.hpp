/*
 * Songate · tagwrite.hpp
 * Changes written inside a song: first its lyrics (BETA 3; the user, 2026-11-04: "Could we add the lyric tag save to the online
 * lyrics feature", then "Songate's own writer" and "Replaced once verified"); then every tag Songate edits and the cover (the user,
 * 2026-11-07: "When editing tag info or cover the user should get prompted if he wants to save the change in songate only or write
 * the changes to the tag ... there's always an option to go back and reverse changes"). It touches as little as it can: only the
 * entries for what changes are taken out and put in; every other tag, picture and the audio stay byte for byte. What the file held
 * for them is handed back (Before), so an undo writes it back.
 *
 *   FLAC: Vorbis comments (TITLE, ARTIST, ALBUM, ALBUMARTIST, GENRE, DATE, TRACKNUMBER, LYRICS) in the VORBIS_COMMENT block (made if
 *         there is none); the cover a PICTURE block (front cover); the PADDING block gives up the room they take.
 *   MP3:  ID3v2.3 or 2.4 frames (TIT2, TPE1, TALB, TPE2, TCON, TDRC or TYER, TRCK, USLT); the cover an APIC (front cover); a 2.4 tag
 *         made if there is none; in the tag's padding when it fits.
 *   M4A:  moov/udta/meta/ilst items (©nam, ©ART, ©alb, aART, ©gen, ©day, trkn, ©lyr; covr), made if missing; a 'free' atom gives
 *         up the room when there is one, else every chunk offset (stco, co64) moves by what moov grew, when moov is before the audio.
 *
 * A new copy is written beside the song, read back (what changed, what did not, its cover, its length) and its audio compared
 * packet by packet with the song's; only then does it take the song's place. Anything wrong, and the song is left as it was. Not on
 * the main thread, never for the song that is playing (it is held open).
 */
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tagwrite {

/** What to change. Text fields by Songate's names: "title", "artist", "album", "albumArtist", "genre", "date", "trackNo",
 *  "lyrics"; a value "" takes the field out. The cover only when `cover` is set: its image (JPEG or PNG), or none to take it out. */
struct Changes {
    std::map<std::string, std::string> text;
    bool cover = false;
    std::vector<uint8_t> coverBytes;
};
/** What the file held for what changed ("" where it had none; the cover's image, or none), for an undo. */
struct Before {
    std::map<std::string, std::string> text;
    bool cover = false;
    std::vector<uint8_t> coverBytes;
};

/** The changes into the song itself: true when written and checked; false and why (the song unchanged). before: what it held. */
bool putChanges(const std::string& song, const Changes& changes, Before* before, std::string& err);

/** A scan met ".songate-old.<name>" or ".songate-new.<name>" in dir: a write cut by a power loss between its renames.
 *  The song is put back (the checked copy if it is there, else the song as it was) and the leftovers go.
 *  Returns the song's path when it had been missing and is back, else empty. Waits for a write under way. */
std::string settleLeftover(const std::string& dir, const std::string& leftover);

/** The lyrics alone (putChanges with "lyrics"). */
bool putLyrics(const std::string& song, const std::string& lyrics, std::string& err);

/** For tests: only the new file, beside nothing (no check, no swap). */
bool buildWithChanges(const std::string& song, const std::string& out, const Changes& changes, Before* before, std::string& err);
bool buildWithLyrics(const std::string& song, const std::string& out, const std::string& lyrics, std::string& err);

/** For tests: the two files' audio, packet by packet, the same. */
bool sameAudio(const std::string& a, const std::string& b, std::string& err);

}  // namespace tagwrite
