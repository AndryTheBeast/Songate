/*
 * Songate · mood.hpp
 * Mood playlists: the library sorted into moods by how each track sounds (mixan::Mood, measured with Automix's analysis).
 * Our own, from features we measure (the PC's Gatefold uses a neural network, Essentia's, licensed non-commercial; its radio
 * uses features like these, vibe.js): Thayer's two axes, as most feature-based mood work does, each feature a rank within
 * the library (0..1) so the library's own spread decides, not one loud master:
 *   energy: busy (onsets), drums, bright, changing, loud, little quiet music, a faster beat
 *   positivity: major rather than minor, bright, a steady danceable beat
 * then five moods (the PC's): Happy, Party, Relaxed, Sad, Aggressive. Each mood's score is compared with the library's
 * (a z-score), so no mood takes every track; each track belongs to its best one, ordered by how strongly.
 * Tried on the user's library (3,074 songs) with tools/preview's mixtrain mood and a script before it went in.
 *
 * BETA 3 (the user, 2026-11-05: "Is there a way to analyse songs better so it more precise?", then "lets do 1 and 2"): four more
 * moods, Workout, Focus, Dark and Epic, from the same features (tried on 1,369 of the PC's songs first); and two more voices:
 *   your choices: a song you set to a mood is in it; one you said is not in a mood is never in it; and songs that sound like the
 *     ones you set move with them (the nearest of them by the features above, each pulling as close as it is);
 *   Last.fm's tags for a song ("chill", "gym", "cinematic"…), as a share for each mood, where Last.fm has any.
 */
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mixan.hpp"

namespace mood {

enum Mood { Happy, Party, Relaxed, Sad, Aggressive, Workout, Focus, Dark, Epic, Count };

/** Your choice for a song: its mood (-1 none), and the moods it is not (bit m: not mood m). */
struct Label {
    signed char is = -1;
    uint16_t nots = 0;
    bool any() const { return is >= 0 || nots; }
};

const char* name(int m);   // "Happy"
const char* desc(int m);   // a line for the New playlist sheet

/** The library's moods: built from every track's features (nullptr: not measured yet), on the main thread when they change
 *  (a few ms for thousands of tracks: ranks and sums, nothing read from the card). */
struct Index {
    std::vector<signed char> best;   // per track: its mood, -1 not known yet
    std::vector<float> score;        // … and how strongly (the mood's z-score)
    int known = 0;
    std::vector<signed char> sound;  // per track: its mood by its sound alone (as before your choices and Last.fm), -1 not known
    /** labels, tags: by track as `tracks`, or null. */
    void build(const std::vector<const mixan::Mood*>& tracks, const std::vector<Label>* labels = nullptr);
};

}  // namespace mood
