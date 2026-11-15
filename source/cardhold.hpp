/*
 * Songate · cardhold.hpp
 * The music's card, held while Android is still mounting it (Android only; elsewhere nothing is ever held). At a boot vold ends
 * every app with a file open on a card it is mounting: Songate, the home app, was ended four times and lost its queue (the user's
 * Hiby R4, 2026-11-15). While held, the library and the queue are shown from Songate's own copies, and every reader of a song
 * file under the held folder asks here first: a worker waits until the card is ready, the screen's thread is told not now.
 */
#pragma once
#include <string>

namespace cardhold {

/** Held under this folder (App::rootAway_); "" lets go. */
void set(const std::string& root);
bool held();
/** The screen's thread, so it is never made to wait. */
void markMain();
/** Before a file is opened: true to go on. Under the held folder, a worker waits until it is let go; the screen's thread gets false. */
bool pass(const std::string& path);
/** The path is under the held folder now (asks, never waits). */
bool under(const std::string& path);
/** Songate is closing: whatever waits is told no, so its worker ends. */
void giveUp();
inline bool pass(const char* path) { return pass(std::string(path ? path : "")); }

}  // namespace cardhold
