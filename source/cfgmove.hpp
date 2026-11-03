/*
 * Songate · cfgmove.hpp
 * The Switch's settings folder: config/gatefold-nx up to 2.0.0 BETA 1, config/songate-nx from BETA 2 (the user, 2026-11-02: "a
 * mistake here loses people's settings, playlists, plays and likes"). The app alone moves it, at launch, before anything reads it,
 * with the background player ended and started again around it; the background player and the overlay only look where it is
 * (sysmodule/include/gfcfgdir.h). Before an older Songate runs ("Go back to"), it goes back the same way, everything that version
 * does not read kept beside it for the return. docs/next-session/session11-config-folder-plan.md has the reasons.
 *
 * One rename on the card, never a copy: instant, no space needed, the covers cache stays where it is. songate-move.txt in the folder
 * is the journal (a move cut off is finished at the next launch, whichever side of the rename it stopped), songate-folder.txt the
 * proof that one finished. Nothing is ever deleted but the few files a still-running BETA 1 background player or overlay wrote into
 * a gatefold-nx made again after the move, once each one is merged in.
 *
 * Not on the PC (its folder was Songate from the start).
 */
#pragma once
#include <string>

namespace cfgmove {

struct Result {
    std::string dir, backups;    // the folder to use this launch, and the default folder for backups
    bool moved = false;          // gatefold-nx became songate-nx, now or to finish one cut off
    bool returned = false;       // … after an older Songate had it (back()): its dropped settings put back, the next sync adds only
    bool both = false;           // both folders hold settings (an older version copied by hand ran meanwhile): Songate asks
    bool legacy = false;         // not moved: the background player could not be ended; gatefold-nx this time, the next launch tries again
    std::string log;             // what happened, for the app's log
};

/** At launch, before anything reads the folder (main), and before finishPending: also sets plat's folder. version: this Songate's. */
Result atLaunch(const std::string& version);

/** Whether a release of this version reads config/gatefold-nx: every one before 2.0.0 BETA 2 (1.0.0, 1.1.0, 2.0.0 BETA 1). */
bool usesOldFolder(const std::string& version);

/** songate-nx back to gatefold-nx before the older version `to` runs (as Songate closes, after its .nro went in). False when the
 *  folder could not be renamed: nothing changed then, and the older version starts as on a new card (its settings wait here). */
bool back(const std::string& to, std::string& log);

/** Result::both, answered: keepOld puts gatefold-nx's set in use (at the next launch: Songate restarts), otherwise it stays
 *  songate-nx's. The set not kept goes whole into the backups folder, restorable in Settings › Updates › Backups. */
void settle(bool keepOld);

}  // namespace cfgmove
