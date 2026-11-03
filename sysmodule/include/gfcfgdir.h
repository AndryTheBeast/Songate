/*
 * Songate · gfcfgdir.h
 * Where the settings folder is, for all three parts (plain C, header only). It was config/gatefold-nx up to 2.0.0 BETA 1 and is
 * config/songate-nx from BETA 2 (the user, 2026-11-02). Only the app moves it (cfgmove.cpp: at launch, with the background
 * player ended; back again before an older version runs); the background player and the overlay only look where it is:
 *
 *   songate-nx there                → songate-nx (moved, or a new install)
 *   only gatefold-nx there          → gatefold-nx (not moved yet: the app does it at its next launch)
 *   neither                         → songate-nx
 *
 * The background player looks once as it starts (the app starts it again after a move); the overlay each time it reads or
 * writes, since it stays loaded while hidden.
 */
#pragma once
#include <sys/stat.h>

#define GFCFG_NEW "sdmc:/config/songate-nx"
#define GFCFG_OLD "sdmc:/config/gatefold-nx"
#define GFCFG_NEW_BACKUPS "sdmc:/config/songate-nx-backups"
#define GFCFG_OLD_BACKUPS "sdmc:/config/gatefold-nx-backups"

static inline int gfcfgIsDir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/** The settings folder as "sdmc:/config/…", no slash at the end. */
static inline const char* gfcfgPick(void) {
    if (gfcfgIsDir(GFCFG_NEW)) return GFCFG_NEW;
    if (gfcfgIsDir(GFCFG_OLD)) return GFCFG_OLD;
    return GFCFG_NEW;
}

/** The same without "sdmc:" ("/config/…"): for the overlay's own file calls, which take paths from the card's root. */
static inline const char* gfcfgPickRoot(void) { return gfcfgPick() + 5; }
