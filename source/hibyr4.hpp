/*
 * Songate · hibyr4.hpp
 * The HiBy R4 on the user's own ROM (Android only, and that device only): the one place that answers "is this the R4", and the
 * settings its service keeps (gain, filter, balance, the outputs, the volume's rules, the status LED, the ROM's equalizer).
 * Everything goes through a worker thread (each call is IPC); the screen reads what was last read back, never waits.
 * On every other device, and on the Switch and the PC, avail() is No and nothing else does anything.
 */
#pragma once
#include <string>

#include "gfeqcfg.h"

namespace r4 {

enum class Avail { No, NoPermission, Yes };

/** Looks for the service (once, on the worker); later calls do nothing. */
void start();
/** What start found: No until it has answered. */
Avail avail();
inline bool on() { return avail() == Avail::Yes; }

/** A setting or status key as last read back (or as just set: shown at once, read back about 0.5 s later); "" unknown. */
std::string get(const char* key);
int getInt(const char* key, int fallback);
/** Changes each time new values arrive. */
unsigned gen();

/** A setting changed by the user; a refusal comes back as error(). */
void set(const char* key, const std::string& value);
/** Songate's equalizer drives the ROM's (one equalizer on the R4): on, the ten bands as Custom, its own preamp off (auto). */
void eqSet(const GfbgEq& e);
/** The ROM's equalizer's Automatic preamp (its eq value's field 1; the same switch as Settings › HiBy audio › Equalizer):
 *  on, it lowers its input by the largest band boost so a boost never clips. Songate's own preamp is a stage of its own. */
bool autoPreamp();
void setAutoPreamp(bool on);
/** The LED's custom colour shown at once while dragging (at most 25 a second), nothing saved: set() led_color on release. */
void ledLive(int rgb, int bright, bool breath);
/** Everything read again (the page opened, Songate back in front). */
void refresh();
/** The last refusal, and when (SDL ticks, ms); taken once by the caller. */
bool takeError(std::string& out);

/** 3.5 mm line out is on (the ROM holds the music volume at full: Songate leaves it alone). */
bool lineOut();

}  // namespace r4
