/*
 * Songate · selftest.cpp
 * Debug › Testing (dev builds only; the user, 2026-11-06: "create new options on debug settings for testing ... a script that goes through
 * the entire app, step by step, button by button, logging frames and what the app its doing"). Three walks:
 *   Basic: every page once, playing, the Now playing tabs, every Settings category;
 *   Advanced: Basic, then every list view and sort, the sheets, Canvas, search, Files, the first albums and artists, the server's views;
 *   Full: Advanced, then every album and artist page, and on the main pages each control focused in turn (focus only: nothing is
 *         pressed that changes your things).
 * Each step: what it did, its frames (count, worst, over 25 ms, the app's own work), its controls, and what looks wrong: a control the
 * step wanted and did not find, controls lying over each other, words cut short (…), words off the screen. The log gets a line a step and
 * a summary; selftest-<level>.txt in the settings folder gets it all. The card above Settings shows how far it is.
 */
#ifdef GF_DEV
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include <sys/stat.h>

#include "app.hpp"
#include "icons.inc"
#include "updater.hpp"
#include "words.hpp"

struct App::SelfTest {
    struct Step {
        std::string label;
        std::function<void(App&)> act;   // done once, when the step starts
        double wait = 0.9;               // seconds before it is measured and the next one starts
        std::string press;               // a control pressed (as A would), by its key and index
        int pressIdx = 0;
        bool walk = false;               // Full: each control on the page focused in turn
        std::function<bool(App&)> until; // waits for it (at most maxWait s); each runs every frame meanwhile
        double maxWait = 0;
        std::function<void(App&)> each;
    };
    int level = 0;
    std::vector<Step> steps;
    size_t at = 0;
    bool started = false;
    double stepAt = 0;
    // this step
    int frames = 0, slow = 0;
    float worst = 0, worstWork = 0, sumWork = 0;
    std::vector<Id> walkIds;
    size_t walkAt = 0;
    int walkFrame = 0;
    bool walkReady = false;
    bool untilDone = false;   // a waiting step's wait is over (said once)
    // all of it
    int totalFrames = 0, totalSlow = 0, problems = 0;
    float totalWorst = 0;
    std::vector<std::pair<float, std::string>> slowest;   // worst frame, step
    std::vector<std::string> lines;
    double began = 0;
    // put back after
    View view = View::Now;
    int libTab = 1, viewTracks = 0, tracksSort = 0, viewFolders = 0, libSource = 0, serverTab = 0, settingsCat = 0;
    bool tracksDesc = false;
};

bool App::selfTestRunning() const { return selfTest_ != nullptr; }

void App::selfTestStart(int level) {
    if (selfTest_) selfTestStop();
    SelfTest* t = new SelfTest;
    t->level = level;
    t->view = route.view;
    t->libTab = store.libTab;
    t->viewTracks = store.viewTracks;
    t->tracksSort = store.tracksSort;
    t->tracksDesc = store.tracksDesc;
    t->viewFolders = store.viewFolders;
    t->libSource = store.libSource;
    t->serverTab = store.serverTab;
    t->settingsCat = settingsCat;
    auto& S = t->steps;
    auto go = [&](const std::string& label, std::function<void(App&)> act, double wait = 0.9) { S.push_back({ label, std::move(act), wait }); };
    auto press = [&](const std::string& label, const char* key, int idx = 0, double wait = 0.8) {
        S.push_back({ label, nullptr, wait, key, idx });
    };
    auto walk = [&](const std::string& label, std::function<void(App&)> act) { S.push_back({ label + " (each control)", std::move(act), 0.6, {}, 0, true }); };
    auto until = [&](const std::string& label, std::function<bool(App&)> cond, double maxWait, std::function<void(App&)> each = nullptr) {
        SelfTest::Step st{ label, nullptr, 0.3 };
        st.until = std::move(cond);
        st.maxWait = maxWait;
        st.each = std::move(each);
        S.push_back(std::move(st));
    };
    // the first start's question cards (covers, the overlay's, lyrics), answered as a new user would: Make them / Read them now
    auto answerCards = [](App& a) {
        if (a.coverNotice == 1 || a.appCoverNotice == 1 || a.lyrNotice == 1) {
            const Id id = uid("notice.make");
            if (a.ui.hasItem(id)) { a.ui.focus(id); a.ui.activate(); }
        }
    };
    const int nt = int(lib.tracks.size());
    (void)nt;

    // ---- New user: as newly installed (the folder set aside at the launch before), everything a first start meets, then all
    if (level == 3) {
        until("First start: the first card", [](App& a) { return a.sheet.page != Sheet::Closed; }, 30);
        press("First card › On " GF_THIS_CARD, "sp.yes", 0, 1.5);
        press("The welcome card › Just use it", "sp.alt", 0, 1.5);
        go("The music folder (if no card offered one)", [](App& a) {
            if (a.sheet.page != Sheet::Closed) a.closeSheet();
            if (a.store.musicRoot.empty()) a.useMusicFolder(plat::join(plat::root(), "Music"));
        }, 1.5);
        until("Reading the music: every track's tags", [](App& a) { return !a.lib.tracks.empty() && a.meta.taggedCount() >= int(a.lib.tracks.size()); }, 1200, answerCards);
        until("The first start's questions, answered", [](App& a) { return a.appCoverNotice == 2 && a.lyrNotice == 2 && a.coverNotice != 1; }, 120, answerCards);
        go("The tour: Complete", [](App& a) {
            if (a.sheet.page != Sheet::Closed) a.closeSheet();
            a.startTour();
            a.tourLevel = 2;
            a.tourGo(1);
        }, 1.0);
        until("The tour: every step", [](App& a) { return a.tourStep < 0; }, 400, [](App& a) {
            static double last = 0;
            if (a.tourStep < 0 || a.time - last < 1.3) return;
            last = a.time;
            if (a.tourStep + 1 >= a.tourCount()) a.endTour();
            else a.tourGo(a.tourStep + 1);
        });
        // playing and the queue
        go("Play the first album", [](App& a) {
            if (a.albumOrder.empty()) return;
            const Album& al = a.lib.albums[size_t(a.albumOrder[0])];
            a.playTracks(al.tracks, 0, al.title);
            a.navigate(View::Now);
        }, 2.0);
        go("Like it", [](App& a) { a.toggleCurrentLike(); });
        go("Liked shows it", [](App& a) { a.navigate(View::List, -1); });
        go("Unlike it", [](App& a) { a.toggleCurrentLike(); });
        go("Add the second album to the queue", [](App& a) {
            if (a.albumOrder.size() < 2) return;
            for (int t2 : a.lib.albums[size_t(a.albumOrder[1])].tracks) a.queueAdd(t2, false);
            a.navigate(View::Queue);
        });
        go("Play next: the last track", [](App& a) { if (!a.lib.tracks.empty()) a.queueAdd(int(a.lib.tracks.size()) - 1, true); });
        for (int k = 0; k < 4; k++) go("Order: the next one", [](App& a) { a.cycleOrder(); }, 0.6);
        go("Next track", [](App& a) { a.next(false); }, 1.5);
        go("Seek 30 s on", [](App& a) { a.seekBy(30); }, 1.0);
        go("Seek 30 s back", [](App& a) { a.seekBy(-30); }, 1.0);
        go("Sleep timer: 1 minute", [](App& a) { a.setSleep(60.0, 9); });
        go("Sleep timer off", [](App& a) { a.setSleep(0, 0); });
        // a playlist of its own: made, added to, each view, deleted (all inside the test's folder)
        go("A playlist of my own", [](App& a) {
            std::vector<std::string> paths;
            for (int i = 0; i < std::min<int>(3, int(a.lib.tracks.size())); i++) paths.push_back(a.lib.tracks[size_t(i)].path);
            a.store.playlists.push_back({ a.store.freePlaylistName("Self test"), std::string(), paths });
            a.navigate(View::List, int(a.store.playlists.size()) - 1);
        });
        go("Add a track to it", [](App& a) { if (!a.store.playlists.empty() && a.lib.tracks.size() > 3) a.addToPlaylist(int(a.store.playlists.size()) - 1, 3); });
        for (int m : { 2, 0, 1, 3 })
            go("Its page, view " + std::to_string(m), [m](App& a) { a.store.viewPlaylist = m; }, 0.6);
        go("Play it", [](App& a) {
            const int r = int(a.store.playlists.size()) - 1;
            if (r >= 0) a.playTracks(a.listTracks(View::List, r, {}), 0, a.store.playlists[size_t(r)].name);
        }, 1.5);
        go("Delete it", [](App& a) { if (!a.store.playlists.empty()) a.erasePlaylist(int(a.store.playlists.size()) - 1); a.navigate(View::Playlists); });
        // the sheets and cards a new user opens
        go("The track's menu", [](App& a) { if (a.now.track >= 0) a.openTrackMenu(a.now.track); });
        go("The track's menu closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("The equalizer", [](App& a) { a.openEqualizer(); });
        go("Battery", [](App& a) { a.openBattery(); });
        go("Battery closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("Light", [](App& a) { a.store.brightness = 1; a.applyTheme(); a.navigate(View::Now); }, 1.2);
        go("Dark again", [](App& a) { a.store.brightness = 0; a.applyTheme(); }, 1.2);
        go("Lyrics", [](App& a) { a.navigate(View::Now); a.npTab = 1; }, 1.0);
        go("Up next", [](App& a) { a.npTab = 0; }, 0.6);
    }

    static const char* const catNames[] = { "Appearance", "Playback", "Equalizer", "Library", "Storage", "Lyrics", "Canvas", "Visualizer", "Controls",
                                            "Devices", "Last.fm", "Reset", "Updates", "Support" };

    // ---- Basic: every page once
    go("Now playing", [](App& a) { a.navigate(View::Now); });
    press("Now playing › Up next", "np.tab", 0);
    press("Now playing › Lyrics", "np.tab", 1);
    press("Now playing › Palette", "np.tab", 2);
    press("Now playing › Up next again", "np.tab", 0);
    go("Play or pause", [](App& a) { a.togglePlay(); }, 1.2);
    go("Play or pause back", [](App& a) { a.togglePlay(); }, 0.8);
    go("Next track", [](App& a) { a.next(false); }, 1.2);
    go("Previous track", [](App& a) { a.prev(); }, 1.2);
    go("Playlist (the queue)", [](App& a) { a.navigate(View::Queue); });
    go("Albums", [](App& a) { a.navigate(View::Albums); });
    go("An album", [](App& a) { if (!a.albumOrder.empty()) a.openAlbum(a.albumOrder[0]); });
    go("Artists", [](App& a) { a.navigate(View::Artists); });
    go("An artist", [](App& a) { if (!a.artists.empty()) a.openArtist(a.artists[0].name); });
    go("Genres", [](App& a) { a.navigate(View::Genres); });
    go("A genre", [](App& a) { if (!a.genres.empty()) a.openGenre(a.genres[0].name); });
    go("Folders", [](App& a) { a.navigate(View::Folders, a.lib.rootFolders.empty() ? -1 : a.lib.rootFolders[0]); });
    for (int k = 0; k < 3; k++)
        go(std::string("Library › ") + (k == 0 ? "Albums" : k == 1 ? "Tracks" : "Folders"), [k](App& a) { a.store.libTab = k; a.store.libSource = 0; a.navigate(View::Library); });
    for (int k = 3; k < 5; k++)
        go(std::string("Library › ") + (k == 3 ? "Artists" : "Genres"), [k](App& a) { a.store.libTab = k; a.store.libSource = 0; a.navigate(View::Library); });
    go("Liked", [](App& a) { a.navigate(View::List, -1); });
    go("Playlists", [](App& a) { a.navigate(View::Playlists); });
    for (int c : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 })
        go(std::string("Settings › ") + catNames[c], [c](App& a) { a.settingsCat = c; a.navigate(View::Settings); }, 0.7);

    // ---- Advanced: the views, sorts, sheets, Canvas, search, Files, the server
    if (level >= 1) {
        for (int m : { 2, 0, 1, 3 })
            go("Library › Tracks, view " + std::to_string(m), [m](App& a) { a.store.libTab = 1; a.store.viewTracks = m; a.navigate(View::Library); }, 0.7);
        for (int so = 0; so < 6; so++)
            go("Library › Tracks, sort " + std::to_string(so), [so](App& a) { a.store.libTab = 1; a.store.tracksSort = so; a.navigate(View::Library); }, 0.7);
        go("Library › Tracks, the other way", [](App& a) { a.store.tracksDesc = !a.store.tracksDesc; });
        for (int m : { 2, 1, 3 })
            go("Library › Folders, view " + std::to_string(m), [m](App& a) { a.store.libTab = 2; a.store.viewFolders = m; a.navigate(View::Library); }, 0.7);
        go("Devices", [](App& a) { a.openDevices(); });
        go("Devices closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("AirPlay", [](App& a) { a.openAirPlay(); });
        go("AirPlay closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("Sleep timer", [](App& a) { a.openSleep(); });
        go("Sleep timer closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("Equalizer presets", [](App& a) { a.openEqPresets(); });
        go("Equalizer presets closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("Battery", [](App& a) { a.openBattery(); });
        go("Battery closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("The track's menu", [](App& a) { if (a.now.track >= 0) a.openTrackMenu(a.now.track); });
        go("The track's menu closed", [](App& a) { a.closeSheet(); }, 0.5);
        // BETA 3 (the user, 2026-11-08: "Update the debug test with the new features"): opened and closed, nothing saved
        go("The track's mood (its menu's Mood row)", [](App& a) {
            if (a.now.track < 0) return;
            a.openTrackMenu(a.now.track);
            a.sheet.page = Sheet::Moods;
            a.ui.focusDefault();
        });
        go("The track's mood closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("Edit tags (nothing saved)", [](App& a) {
            if (a.now.track < 0) return;
            a.openTrackMenu(a.now.track);
            a.openTagEditor(a.now.track);
        });
        go("Edit tags closed", [](App& a) { a.closeSheet(); }, 0.5);
        go("Search, the lyrics too", [](App& a) { a.navigate(View::Search, -1, "love"); }, 2.0);
#ifdef __SWITCH__
        if (pocketAvailable()) {   // the screen dark for a moment; the controllers never turned off by a test
            go("Pocket lock", [](App& a) {
                a.navigate(View::Now);
                const bool pads = a.store.pocketPads;
                a.store.pocketPads = false;
                a.pocketLock(true);
                a.store.pocketPads = pads;
            }, 1.5);
            go("Pocket lock off", [](App& a) { a.pocketLock(false); }, 0.8);
        }
#endif
        go("Factory reset's card", [](App& a) { a.openFactory(); });
        go("Factory reset's card closed", [](App& a) { a.closeSheet(); }, 0.5);
        if (subsonic.view().state == Subsonic::SignedIn) {
            go("The server's sign-out card", [](App& a) { a.serverSignOutCard(); });
            go("The server's sign-out card closed", [](App& a) { a.closeSheet(); }, 0.5);
        }
        go("Canvas", [](App& a) { a.openCanvas(true); }, 1.5);
        go("Canvas closed", [](App& a) { a.openCanvas(false); }, 0.8);
        go("Search", [](App& a) { a.navigate(View::Search, -1, "a"); }, 1.2);
        go("Files", [](App& a) { a.openFiles(a.filesStart()); }, 1.0);
        for (int i = 1; i < 6; i++)
            go("Album " + std::to_string(i + 1), [i](App& a) { if (i < int(a.albumOrder.size())) a.openAlbum(a.albumOrder[size_t(i)]); }, 0.7);
        for (int i = 1; i < 6; i++)
            go("Artist " + std::to_string(i + 1), [i](App& a) { if (i < int(a.artists.size())) a.openArtist(a.artists[size_t(i)].name); }, 0.7);
        if (subsonic.view().state == Subsonic::SignedIn) {   // the server's views and a page of each
            static const char* const tabs[] = { "Albums", "Artists", "Genres", "Folders", "Tracks" };
            go("Playlists, the server's", [](App& a) { a.store.libSource = 1; a.navigate(View::Playlists); }, 1.5);
            go("Playlists, yours", [](App& a) { a.store.libSource = 0; a.navigate(View::Playlists); }, 0.8);
            for (int k = 0; k < 5; k++)
                go(std::string("Library › Server › ") + tabs[k], [k](App& a) { a.store.libSource = 1; a.store.serverTab = k; a.navigate(View::Library); }, 1.5);
            go("Server album", [](App& a) { if (!a.serverAlbums_.empty()) a.openServer(2, a.serverAlbums_[0].id, a.serverAlbums_[0].name); }, 1.5);
            go("Server artist", [](App& a) { if (a.srvl_.hA && !a.srvl_.artists.empty()) a.openServer(1, a.srvl_.artists[0].id, a.srvl_.artists[0].name); }, 1.5);
            go("Server genre", [](App& a) { if (a.srvl_.hG && !a.srvl_.genres.empty()) a.openServer(3, a.srvl_.genres[0].name, a.srvl_.genres[0].name); }, 1.5);
            go("Server folder", [](App& a) {
                if (!a.srvl_.hF) return;
                for (const auto& e : a.srvl_.folders) if (e.dir) { a.openServer(4, e.id, e.name); break; }
            }, 1.5);
            go("Server playlist", [](App& a) { if (!a.srvPlaylists_.empty()) a.openServer(5, a.srvPlaylists_[0].id, a.srvPlaylists_[0].name); }, 1.5);
        }
    }

    // ---- Full: every album and artist page; each control of the main pages focused in turn
    if (level >= 2) {
        const int na = std::min<int>(int(albumOrder.size()), 400), nr = std::min<int>(int(artists.size()), 400);
        for (int i = 0; i < na; i++) go("Every album: " + std::to_string(i + 1) + " of " + std::to_string(na), [i](App& a) {
                if (i < int(a.albumOrder.size())) a.openAlbum(a.albumOrder[size_t(i)]);
            }, 0.35);
        for (int i = 0; i < nr; i++) go("Every artist: " + std::to_string(i + 1) + " of " + std::to_string(nr), [i](App& a) {
                if (i < int(a.artists.size())) a.openArtist(a.artists[size_t(i)].name);
            }, 0.35);
        walk("Now playing", [](App& a) { a.navigate(View::Now); });
        walk("Library › Tracks", [](App& a) { a.store.libTab = 1; a.store.libSource = 0; a.navigate(View::Library); });
        walk("Albums", [](App& a) { a.navigate(View::Albums); });
        walk("Liked", [](App& a) { a.navigate(View::List, -1); });
        for (int c : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 })
            walk(std::string("Settings › ") + catNames[c], [c](App& a) { a.settingsCat = c; a.navigate(View::Settings); });
        if (subsonic.view().state == Subsonic::SignedIn)
            for (int k = 0; k < 5; k++) walk(std::string("Library › Server › ") + (k == 0 ? "Albums" : k == 1 ? "Artists" : k == 2 ? "Genres" : k == 3 ? "Folders" : "Tracks"), [k](App& a) { a.store.libSource = 1; a.store.serverTab = k; a.navigate(View::Library); });
    }
    go("Back to Now playing", [](App& a) { a.navigate(View::Now); }, 0.6);

    selfTest_ = t;
    g.sayOn = true;   // the words drawn, for the cut and off-screen checks
    static const char* const names[] = { "Basic", "Advanced", "Full", "New user" };
    selfTestFresh_ = level == 3;
    t->began = time;
    t->lines.push_back(std::string("Self test, ") + names[level] + ": " + std::to_string(S.size()) + " steps");
    logLine(t->lines.back());
}

void App::selfTestStop() {
    SelfTest* t = selfTest_;
    if (!t) return;
    selfTest_ = nullptr;
#ifndef GF_WINDOWED
    g.sayOn = false;
#endif
    // the settings it changed, back as they were
    store.libTab = t->libTab;
    store.viewTracks = t->viewTracks;
    store.tracksSort = t->tracksSort;
    store.tracksDesc = t->tracksDesc;
    store.viewFolders = t->viewFolders;
    store.libSource = t->libSource;
    store.serverTab = t->serverTab;
    settingsCat = t->settingsCat;
    for (bool& on : selfTestOn_) on = false;
    static const char* const names[] = { "Basic", "Advanced", "Full", "New user" };
    // the summary: the slowest steps first
    std::sort(t->slowest.begin(), t->slowest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    char head[240];
    snprintf(head, sizeof head, "Self test, %s: %d of %d steps in %.0f s, %d frames, worst %.1f ms, %d over 25 ms; %d to look at", names[t->level], int(t->at),
             int(t->steps.size()), time - t->began, t->totalFrames, double(t->totalWorst), t->totalSlow, t->problems);
    logLine(head);
    t->lines.push_back(head);
    t->lines.push_back("Slowest steps:");
    for (size_t i = 0; i < std::min<size_t>(10, t->slowest.size()); i++) {
        char l[200];
        snprintf(l, sizeof l, "  %.1f ms  %s", double(t->slowest[i].first), t->slowest[i].second.c_str());
        t->lines.push_back(l);
    }
    const std::string file = std::string("selftest-") + (t->level == 0 ? "basic" : t->level == 1 ? "advanced" : t->level == 2 ? "full" : "new-user") + ".txt";
    const std::string path = store.path(file.c_str());
    if (FILE* f = fopen(path.c_str(), "w")) {   // (once, at the end: dev builds only)
        for (const auto& l : t->lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
    }
    toast(std::string("Self test done: ") + std::to_string(t->problems) + " to look at, in " + path);
    if (t->level == 3) {   // New user: the results into your folder too, then Songate closes, puts your folder back and starts again
        if (FILE* f = fopen((std::string(Store::Dir) + ".before-test/" + file).c_str(), "w")) {
            for (const auto& l : t->lines) fprintf(f, "%s\n", l.c_str());
            fclose(f);
        }
        selfTestFresh_ = false;
        freshRestore_ = true;
        relaunch = true;
        running = false;
    }
    delete t;
}

/** Each frame, after it was drawn: the step's frames counted; a step started, measured and ended in turn. */
void App::selfTestTick(float frameMs, float workMs) {
    static const char* fromEnv = getenv("GF_SELFTEST");   // (tests: a level started 4 s after launch; GF_SELFTEST_QUIT closes after)
    static bool envStarted = false;
    if (fromEnv && !envStarted && time > 4) {
        envStarted = true;
        const int lv = std::clamp(atoi(fromEnv), 0, 3);
        if (lv == 3) { if (!selfTest_) newUserTestGo(); }   // (a launch already in the fresh folder starts it by itself)
        else selfTestOn_[lv] = true;
    }
    if (fromEnv && envStarted && !selfTest_ && getenv("GF_SELFTEST_QUIT") && time > 6) running = false;
    for (int k = 0; k < 4; k++) {   // the toggles: one on starts it, off stops it
        if (selfTestOn_[k] && (!selfTest_ || selfTest_->level != k)) { for (int j = 0; j < 4; j++) if (j != k) selfTestOn_[j] = false; selfTestStart(k); break; }
    }
    SelfTest* t = selfTest_;
    if (!t) return;
    if (!selfTestOn_[t->level]) { selfTestStop(); return; }
    if (t->at >= t->steps.size()) { selfTestStop(); return; }
    SelfTest::Step& st = t->steps[t->at];
    if (!t->started) {   // the step begins
        t->started = true;
        t->stepAt = time;
        t->frames = t->slow = 0;
        t->worst = t->worstWork = t->sumWork = 0;
        t->walkIds.clear();
        t->walkAt = 0;
        t->walkReady = false;
        t->untilDone = false;
        // a sheet or card left open (a step before, a question the app asked): out of the way first, unless this step closes it
        if (sheet.page != Sheet::Closed && st.act && st.label.find(" closed") == std::string::npos) closeSheet();
        if (st.act) st.act(*this);
        if (!st.press.empty()) {
            const Id id = uid(st.press, st.pressIdx);
            if (ui.hasItem(id)) { ui.focus(id); ui.activate(); }
            else {
                t->problems++;
                t->lines.push_back("  ! " + st.label + ": its control " + st.press + "#" + std::to_string(st.pressIdx) + " is not on the page");
            }
        }
        return;
    }
    t->frames++;
    t->worst = std::max(t->worst, frameMs);
    t->worstWork = std::max(t->worstWork, workMs);
    t->sumWork += workMs;
    t->slow += frameMs > 25.f;
    if (st.walk) {   // Full: after it settles, each control focused in turn, two frames apiece
        if (!t->walkReady && time - t->stepAt > 0.5) {
            t->walkReady = true;
            ui.eachItemId([&](Id id, const SDL_FRect&) { t->walkIds.push_back(id); });
        }
        if (t->walkReady && t->walkAt < t->walkIds.size()) {
            if (++t->walkFrame % 2 == 0) { ui.focus(t->walkIds[t->walkAt]); t->walkAt++; }
            return;
        }
    }
    if (st.until && !t->untilDone) {
        if (st.each) st.each(*this);
        const bool done = st.until(*this);
        if (!done && time - t->stepAt < st.maxWait) return;
        t->untilDone = true;
        if (!done) { t->problems++; t->lines.push_back("  ! waited " + std::to_string(int(st.maxWait)) + " s: it did not happen"); }
        else {
            char took[64];
            snprintf(took, sizeof took, "  · took %.1f s", time - t->stepAt);
            t->lines.push_back(took);
        }
        t->stepAt = time;   // (its wait, then measured, from here)
    }
    if (time - t->stepAt < st.wait) return;
    if (t->level == 3 && (coverNotice == 1 || appCoverNotice == 1 || lyrNotice == 1)) {   // New user: answered as a new user would
        const Id id = uid("notice.make");
        if (ui.hasItem(id)) { ui.focus(id); ui.activate(); }
        t->stepAt = time;
        return;
    }
    if (coverNotice == 1 || appCoverNotice == 1 || lyrNotice == 1) {   // (one already up when the test began)
        if (t->lines.empty() || t->lines.back().find("a question card") == std::string::npos) t->lines.push_back("  · a question card is up (covers or lyrics): answer it; the test waits");
        t->stepAt = time;
        return;
    }

    // measured: what is on the screen now
    std::vector<std::pair<Id, SDL_FRect>> items;
    ui.eachItemId([&](Id id, const SDL_FRect& r) { items.push_back({ id, r }); });
    int overlaps = 0;
    std::string overlapAt;
    for (size_t i = 0; i < items.size(); i++)
        for (size_t j = i + 1; j < items.size(); j++) {
            const SDL_FRect& a = items[i].second;
            const SDL_FRect& b = items[j].second;
            const float ix = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x), iy = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
            if (ix <= 2 || iy <= 2) continue;
            const float inter = ix * iy, small = std::min(a.w * a.h, b.w * b.h);
            const bool inside = (a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h) ||
                                (b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h);
            if (!inside && small > 0 && inter > small * 0.3f) {
                if (!overlaps) {
                    char w[96];
                    snprintf(w, sizeof w, " (%.0f,%.0f %.0fx%.0f / %.0f,%.0f %.0fx%.0f)", double(a.x), double(a.y), double(a.w), double(a.h), double(b.x), double(b.y),
                             double(b.w), double(b.h));
                    overlapAt = w;
                }
                overlaps++;
            }
        }
    int cut = 0, off = 0;
    std::string cutWords, offWords;
    for (const auto& s : g.said) {
        if (s.cut) { cut++; if (cutWords.size() < 160) cutWords += (cutWords.empty() ? "" : ", ") + std::string("“") + s.text + "”"; }
        if (s.x < -1 || s.y < -1 || s.x + s.w > float(css::W) + 1 || s.y + s.h > float(css::H) + 1) {
            off++;
            if (offWords.size() < 120) offWords += (offWords.empty() ? "" : ", ") + std::string("“") + s.text + "”";
        }
    }
    char line[400];
    snprintf(line, sizeof line, "%3d %-44s %4d frames, worst %5.1f ms (work %4.1f, mean %4.1f), %d over 25 ms, %3d controls%s%s%s", int(t->at + 1), st.label.c_str(),
             t->frames, double(t->worst), double(t->worstWork), t->frames ? double(t->sumWork / float(t->frames)) : 0.0, t->slow, int(items.size()),
             st.walk ? (", " + std::to_string(t->walkIds.size()) + " focused").c_str() : "", cut ? (", " + std::to_string(cut) + " cut").c_str() : "",
             overlaps ? (", " + std::to_string(overlaps) + " overlapping").c_str() : "");
    t->lines.push_back(line);
    logLine(std::string("[selftest] ") + line);
    if (overlaps) { t->problems++; t->lines.push_back("  ! controls over each other" + overlapAt); }
    if (off) { t->problems++; t->lines.push_back("  ! words off the screen: " + offWords); }
    if (cut) t->lines.push_back("  · cut short: " + cutWords);   // (often meant: long titles; listed to look at)
    if (t->worst > 50.f) { t->problems++; t->lines.push_back("  ! a frame took " + std::to_string(int(t->worst)) + " ms"); }
    t->totalFrames += t->frames;
    t->totalSlow += t->slow;
    t->totalWorst = std::max(t->totalWorst, t->worst);
    t->slowest.push_back({ t->worst, st.label });
    t->at++;
    t->started = false;
}

/** New user's card: what it does, and Start. */
void App::newUserTestAsk() {
    confirm(ICON_BOLT, "Self test", "New user test", "Songate as if newly installed", "Start it?",
            "Songate closes, sets your settings folder aside as it is (beside it, as .before-test: settings, playlists, Liked, covers, "
            "caches, sign-ins) and starts as new: the first card, the music folder, the questions, the tour, then every page, view and feature, "
            "playing, a playlist of its own made and deleted, as Full does. Your music files are not changed. At the end it closes, puts your "
            "folder back and starts again; the results are in selftest-new-user.txt.",
            "Start", [this] { newUserTestGo(); });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_BOLT;
}

void App::newUserTestGo() {
    struct stat st;
    if (stat((std::string(Store::Dir) + ".before-test").c_str(), &st) == 0) {   // never over a folder set aside before: yours could be in it
        toast("A settings folder from an earlier test is still set aside (.before-test): look at it first");
        return;
    }
    logLine("Self test, New user: closing, the settings folder set aside");
    freshStart_ = true;
    relaunch = true;
    running = false;
}

/** At launch: the folder set aside means a New user test; a marker in this one means it ran here before and was cut short, so the
 *  folder is put back at once (Songate closes and starts again with it). */
void App::newUserTestAtLaunch() {
    struct stat st;
    if (stat((std::string(Store::Dir) + ".before-test").c_str(), &st) != 0) return;
    const std::string marker = store.path("new-user-test.txt");
    if (stat(marker.c_str(), &st) == 0) {
        logLine("Self test, New user: cut short before; your settings folder is put back");
        freshRestore_ = true;
        relaunch = true;
        running = false;
        return;
    }
    if (FILE* f = fopen(marker.c_str(), "w")) { fputs("A New user self test runs in this folder\n", f); fclose(f); }
    selfTestOn_[3] = true;
}

/** The card above Settings while it runs: its steps. */
bool App::selfTestJob(Job& j) {
    if (!selfTest_) return false;
    static const char* const names[] = { "Self test · Basic", "Self test · Advanced", "Self test · Full", "Self test · New user" };
    j = { names[selfTest_->level], int(selfTest_->at), int(selfTest_->steps.size()), 14 /* Debug */, Stoppable::None, nullptr, false, ICON_BOLT };
    return true;
}
#endif
