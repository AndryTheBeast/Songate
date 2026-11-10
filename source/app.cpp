/*
 * Gatefold NX · app.cpp
 * Lifecycle, input mapping, play queue and track loading.
 */
#include "app.hpp"
#include "cfgmove.hpp"
#include "gfcfgdir.h"
#include "cores.hpp"
#include "icons.inc"
#include "m3u.hpp"
#include "plicons.inc"
#include "net.hpp"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <random>
#include <strings.h>
#include <sys/stat.h>

namespace {


bool iless(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; }

std::string lower(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

/** Fires on press, then repeats while held (D-pad and stick). */
struct Repeat {
    double held = 0, nextAt = 0;
    bool step(bool down, double dt) {
        if (!down) { held = 0; return false; }
        if (held == 0) { held = dt; nextAt = 0.38; return true; }
        held += dt;
        if (held >= nextAt) { nextAt += 0.075; return true; }
        return false;
    }
};
Repeat rUp, rDown, rLeft, rRight;

std::mt19937 rng{ 0x6a7e };   // seeded again in App::init: a fixed seed shuffled a list the same way at every launch

}  // namespace

// ============================================================ lifecycle

bool App::init() {
    launchAt_ = profAt_ = SDL_GetPerformanceCounter();
    plat::init();
    metadataInit();
#if defined(GF_WINDOWED) && defined(GF_DEV)
    // Tests (dev builds on a PC): GF_TAGTEST=<a list of songs, one a line>: each gets lyrics written inside it (tagwrite.hpp) and
    // what happened is printed; then Songate ends. Only for copies: it changes the files it is given.
    if (const char* list = getenv("GF_TAGTEST")) {
        if (FILE* f = fopen(list, "rb")) {
            char line[2048];
            while (fgets(line, sizeof line, f)) {
                std::string p = line;
                while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
                if (p.empty()) continue;
                const std::string lrc = "[00:01.00] Written by Songate's test\n[00:02.50] second line, ünïcødé ✓ 🎵\n[00:04.00] third line\n";
                std::string err;
                const bool out = getenv("GF_TAGTEST_REMOVE") != nullptr;   // taken out instead
                const bool ok = tagwrite::putLyrics(p, out ? std::string() : lrc, err);
                TrackInfo back;
                readTrackInfo(p, back, false, true);
                const bool right = out ? back.lyrics.empty() : back.lyrics.find("ünïcødé ✓ 🎵") != std::string::npos;
                fprintf(stderr, "TAGTEST %s | %s | lyrics back: %s\n", ok ? (out ? "taken out" : "written") : "NOT changed", p.c_str(),
                        ok ? (right ? (out ? "none" : "yes") : "NO") : err.c_str());
            }
            fclose(f);
        }
        return false;
    }
#endif
    startMark("sys");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) return false;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
    startMark("init");
#ifdef GF_WINDOWED
    store.load();   // the window goes where it was left
    {
        // The console's shape, as big as the screen comfortably holds at this screen's own scale
        SDL_Rect room{ 0, 0, 1280, 720 };
        SDL_GetDisplayUsableBounds(0, &room);
        float dpi = 96;
        SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr);
        const float k = std::min({ dpi / 96.f, room.w * 0.92f / 1280, room.h * 0.92f / 720 });
        // Where it was left, if a screen is still there
        bool kept = store.winW >= 640 && store.winH >= 360;
        if (kept) {
            const SDL_Rect at{ store.winX + 40, store.winY + 8, 80, 30 };
            kept = false;
            for (int d = 0; d < SDL_GetNumVideoDisplays(); d++) {
                SDL_Rect b{}, o{};
                if (SDL_GetDisplayBounds(d, &b) == 0 && SDL_IntersectRect(&at, &b, &o)) kept = true;
            }
        }
#ifdef GF_ANDROID
        const Uint32 flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;   // the whole screen, turning with the phone
        kept = false;
#else
        const Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI | (store.winMax ? SDL_WINDOW_MAXIMIZED : 0);
#endif
        window_ = kept ? SDL_CreateWindow("Songate", store.winX, store.winY, store.winW, store.winH, flags)
                       : SDL_CreateWindow("Songate", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, int(1280 * k), int(720 * k), flags);
        if (window_) SDL_SetWindowMinimumSize(window_, 640, 360);
    }
#else
    window_ = SDL_CreateWindow("Songate", 0, 0, css::W, css::H, 0);
#endif
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window_ || !renderer_ || !g.init(renderer_)) return false;
#ifdef GF_PREVIEW
    // the film's sharpness (GF_FILM_SCALE=1.5: 1920x1080 drawn as such, text and icons rasterized at that size, not upscaled)
    if (const char* fs = getenv("GF_FILM_SCALE")) {
        const float k = float(atof(fs));
        if (k > 1) {
            SDL_SetWindowSize(window_, int(css::W * k), int(css::H * k));
            SDL_RenderSetScale(renderer_, k, k);
            g.setDisplay(k, 60);
        }
    }
#endif
#ifdef GF_WINDOWED
    fitWindow();
    textCardOwner();
    plat::windowReady(window_);
#ifndef GF_ANDROID
    // (not on Android: there SDL announces a resize, a turn, from Android's own thread, and this would draw from it with no GL
    // context; the frame's own event loop lays it out, input())
    SDL_AddEventWatch(&App::resizeWatch, this);
#endif
    SDL_SetWindowHitTest(window_, &App::hitTest, this);
#endif
    // Text in a scroll area follows its scroll sub-pixel, so a glide does not end in one-pixel steps.
    ui.onArea = [this](const ScrollArea* a) { g.setShift(0, textShift - (a ? a->y : 0)); };
    startMark("sdl");

#ifndef GF_WINDOWED
    store.load();
#endif
    lyrnet::setFolder(store.path("lyrics"));   // lyrics found online, kept in Songate's own folder (before the loader reads any)
    rng.seed(uint32_t(SDL_GetPerformanceCounter()) ^ uint32_t(std::time(nullptr)));
    lyrIndex.start(store.path("lyrics-index.tsv"));   // search by lyrics: every song's lyrics, read once on a worker
    loadTagUndo();   // what files held before Songate wrote into them (Restore the file's original tags)
    moodLabelsLoad();   // your mood choices (a small file, read once here)
#ifndef GF_WINDOWED
    if (!folder.log.empty()) logLine("Settings folder: " + folder.log);
    if (folder.both) askBothFolders();
#endif
#if defined(GF_DEV) && !defined(GF_AIRPLAY) && !defined(GF_WINDOWED)
    // The preview has no AirPlay: GF_AIRPLAY_MOCK draws its question and its Settings card with two made-up devices (never saved)
    if (const char* who = getenv("GF_AIRPLAY_MOCK")) {
        store.airplayDevices = { { "0000000000000001", "My iPhone" }, { "0000000000000002", "Living room iPad" } };
        airplayQuestion(who, [](int) {});
    }
    // GF_TOG_MOCK: a session this device started, PC in time, the phone's link dropped, a deck linked and not in it (Settings > Devices)
    if (getenv("GF_TOG_MOCK")) {
        tog.stage = Tog::On;
        tog.host = true;
        tog.startAt = 1;
        tog.missing = 3;
        tog.peerId = "pc";
        tog.peerName = "TheBeast's PC";
        Tog::Member a, b;
        a.id = "pc"; a.name = "TheBeast's PC"; a.state = 2;
        b.id = "phone"; b.name = "Redmi Note"; b.state = 2; b.lost = true;
        tog.members = { a, b };
    }
    // … and GF_AIRPLAY_MOCK_NOW=<an image file> an iPhone's song playing, with that file as its cover
    if (const char* cover = getenv("GF_AIRPLAY_MOCK_NOW")) {
        extBegin("Guest's iPhone");
        extInfo("Midnight Sky", "The Very Long Name Orchestra, Its Choir, Its Friends and Everyone Else", "Plastic Hearts (The Long Edition, Remastered)");
        extProgress(74, 223);
        std::vector<uint8_t> bytes;
        if (FILE* f = fopen(cover, "rb")) {
            char buf[16384];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
            fclose(f);
        }
        extCover(std::move(bytes));
    }
#endif
    // After a factory reset: its old settings folder, moved aside at exit, is deleted in the background (BETA 1's too)
    if (struct stat st; stat((std::string(Store::Dir) + ".erased").c_str(), &st) == 0
#ifndef GF_WINDOWED
                        || stat(GFCFG_OLD ".erased", &st) == 0
#endif
    )
        eraseOld_ = std::thread([] {
            cores::moveHere(cores::FileOps);
            Updater::eraseLeftovers(Store::Dir);
        });
    coversHeld = !store.stopCovers;   // until the covers are counted (and asked about, if many are missing)
    lyrHeld = !store.stopLyrics;      // the lyrics too, after them
    applyJobStops();   // before the workers start: a stopped job never begins
    stats.start(Store::Dir);
    lastfm.start(Store::Dir);
    subsonic.start(Store::Dir);
#ifdef GF_DEV
    // tests: subsonic-test.txt in the settings folder (the address, the user, the password: a test server's) signs in at launch,
    // then is deleted; the preview has no keyboard to type them with
    if (FILE* f = fopen(store.path("subsonic-test.txt").c_str(), "rb")) {
        char b[3][256] = {};
        for (auto& l : b) {
            if (!fgets(l, sizeof l, f)) break;
            l[strcspn(l, "\r\n")] = 0;
        }
        fclose(f);
        remove(store.path("subsonic-test.txt").c_str());
        std::string pw = b[2];
        memset(b[2], 0, sizeof b[2]);
        if (*b[0] && *b[1] && !pw.empty()) subsonic.signIn(b[0], b[1], pw);
    }
#endif
    identity.start(Store::Dir);
    connect.aim(store.connectAim);
    (void)plat::deviceName();   // (asked here first, on the app's thread: Android keeps this answer for Connect's thread)
    connect.start(Store::Dir);
#ifdef GF_AIRPLAY
    airplay.start("");
    {   // the speakers' volumes as they were
        std::vector<std::pair<std::string, float>> vols;
        size_t p = 0;
        while (p < store.airSpeakerVol.size()) {
            const size_t e = store.airSpeakerVol.find('\n', p);
            const std::string line = store.airSpeakerVol.substr(p, e == std::string::npos ? std::string::npos : e - p);
            const size_t tab = line.find('\t');
            if (tab != std::string::npos) vols.push_back({ line.substr(0, tab), float(atof(line.c_str() + tab + 1)) });
            if (e == std::string::npos) break;
            p = e + 1;
        }
        airsend.setVolumes(vols);
    }
    airsend.setName(plat::deviceName());
    airsend.start();
    player.setAirSend(&airsend);
#endif
    copier.start(Store::Dir, &connect);
    syncer.start(Store::Dir, &connect, &identity, &levels, &copier);
    syncer.backup = [this] { return !updater.backupNow(backupRoot(), "before a sync").empty(); };
    levels.start(Store::Dir);
    sideW = store.sidebarCollapsed ? css::SideWCollapsed : css::SideW;
    sideP_ = store.sidebarCollapsed ? 0.f : 1.f;
    applyTheme();
    pal = target;
    if (store.opening) openingFirst();
    else openingDone_ = true;   // Settings › Playback › Startup: straight into the app, no bells either
    loader.start();
    prints.start();
    player.init();   // the UI still runs without audio, so a failure is not fatal
    player.setVolume(store.volume);
    player.setEq(store.eq);
    volumeShown = store.volume;
    startMark("audio");
    if (store.background) bgConnect();
    startMark("player");
    // Settings → Updates: the last check's list, read from the card (nothing goes online here unless allowed and asked for)
    updater.start(Store::Dir, selfPath);
    loadModelFiles();   // Automix's downloaded models (a few small files)
    if (store.updates && store.updateAtStart) checkUpdates(true);

    // Closing from the HOME menu ends the process without running shutdown(): hold the exit until the
    // settings and the session are written, and save whenever the app loses focus anyway.
    plat::holdExit();
    plat::watchLeave(&App::onLeave, this);

    plat::padInit();

    // First start: always ask (the user, 2026-10-27). The folder picker at the card's top, and over it, when the card has
    // a Music folder at its top, a welcome card offering it; Choose another, B or × leave the picker to choose with.
    std::string found;
#ifdef GF_ANDROID
    storageOk_ = plat::storageAllowed();   // a Music folder is seen without access, but nothing in it: offered once it is given
    if (store.musicRoot.empty() && storageOk_) found = plat::musicFolder();
#elif defined(GF_WINDOWED)
    if (store.musicRoot.empty()) found = plat::musicFolder();
#else
    if (store.musicRoot.empty())
        for (const auto& d : listDirs(plat::root()))
            if (strcasecmp(d.c_str(), "music") == 0) { found = plat::join(plat::root(), d); break; }   // keep the card's own spelling
#endif
    rescan();
    pcm_.resize(Spectrum::N);
#ifdef GF_DEV
    newUserTestAtLaunch();   // a New user self test: its fresh start (or the folder put back after one interrupted)
#endif
    if (store.musicRoot.empty() && subsonic.view().state != Subsonic::SignedOut) {
        navigate(View::Albums, -1, std::string(), false);   // the server's music alone (chosen on the first card)
    } else if (store.musicRoot.empty()) {
        tourAsk = true;   // a first launch: the tour is offered (the welcome card, or once a folder is chosen)
        navigate(View::PickFolder, -1, plat::root(), false);
        firstCard(found);   // this device's music or a server's first (the welcome card after it)
    }
#ifdef __SWITCH__
    // The console with its background player on (Settings › Playback): Songate opens on your own queue, as it always did, and Library's
    // Local | Server on Local with it; the server's queue is kept for the switch (the user, 2026-11-07: "One rule for the switch")
    const bool bgRule = store.background;
#else
    const bool bgRule = false;
#endif
    if (store.musicRoot.empty()) {}
    else if (player.remote() && bgAdoptQueue()) { if (store.resume) restoreServerQueue(false); }   // the background player's queue, playing or not, wins
    else if (store.resume) {
        restoreSession();
        restoreServerQueue(!bgRule);   // the server's queue too: back paused when it was the one playing, else kept for Library's switch
    }
    if (bgRule && !store.musicRoot.empty() && store.libSource != 0) { store.libSource = 0; persistSoon(1); }
#ifdef GF_ANDROID
    // Android's access to the music (storageOk_, above): asked for first, by a card (the user, 2026-11-03: "Card first"), on a first
    // start or when it was taken away since
    if (!storageOk_ && !store.musicRoot.empty()) accessCard();   // (a first start: the first card asks, then this)
#ifdef GF_DEV
    if (FILE* f = fopen(store.path("options.txt").c_str(), "r")) {   // dev views (app.hpp optShowTouch_)
        char word[32];
        while (fscanf(f, "%31s", word) == 1) {
            const std::string w = word;
            if (w == "touch") optShowTouch_ = 1;
        }
        fclose(f);
    }
    if (FILE* f = fopen(store.path("portrait-mock.txt").c_str(), "r")) {   // portrait mock-ups (view_portrait.cpp)
        if (fscanf(f, "%d", &portraitMock_) != 1) portraitMock_ = -1;
        // then, if given, a word of a song's file name to play (one with lyrics, for the lyrics page)
        char want[128] = {};
        if (fscanf(f, " %127[^\n]", want) == 1)
            for (int t = 0; t < int(lib.tracks.size()); t++)
                if (lib.tracks[t].path.find(want) != std::string::npos) { playTracks({ t }, 0, "Mock", false); break; }
        fclose(f);
        if (portraitMock_ >= 0 && now.track >= 0 && !isPlaying()) togglePlay();   // the music on, for what moves with it
    }
#endif
#endif
    startMark("queue");
    ui.focusDefault();
#ifdef GF_ANDROID
    ui.setPadMode(false);   // a touch screen: no focus ring (nor its hint) until a pad or a keyboard is used
#endif
    ready_ = true;
    return true;
}

void App::useMusicFolder(const std::string& path) {
    store.musicRoot = path;
    store.save();
    rescan();
    navigate(View::Library, -1, {}, false);
    toast(std::to_string(lib.tracks.size()) + " tracks found in " + path);
    if (tourAsk) {   // a first launch whose folder was chosen in the picker: the tour, offered now
        tourAsk = false;
        offerTour();
    }
}

void App::rescan(const std::vector<std::string>* given, Library* built) {
    // Track indexes change with the scan: remember the queue by path.
    const std::string current = now.track >= 0 ? lib.tracks[now.track].path : std::string();
    std::vector<std::string> queued;
    for (int t : q.tracks) queued.push_back(lib.tracks[t].path);
    // ... and the queues set aside (a session's, a handoff's Undo): they come back, or are saved, after the scan
    auto pathsOf = [&](const Queue& k) {
        std::vector<std::string> v;
        for (int t : k.tracks) v.push_back(t >= 0 && t < int(lib.tracks.size()) ? lib.tracks[size_t(t)].path : std::string());
        return v;
    };
    const std::vector<std::string> togQueued = pathsOf(tog.savedQueue), handQueued = pathsOf(handoffOld);

#ifdef GF_DEV
    Uint64 rsAt = SDL_GetPerformanceCounter();
    std::string rsLog = "Rescan:";
    auto rsMark = [&](const char* what) {
        const Uint64 at = SDL_GetPerformanceCounter();
        char one[48];
        snprintf(one, sizeof one, " %s %.1f", what, double(at - rsAt) * 1000.0 / double(SDL_GetPerformanceFrequency()));
        rsLog += one;
        rsAt = at;
    };
#else
    auto rsMark = [](const char*) {};
#endif
    meta.stop();
    rsMark("tags stopped");
    libEpoch++;
    if (!ready_) startMark("start");
    const std::vector<std::string> roots = Library::validRoots(store.musicRoot.empty() ? std::vector<std::string>{} : std::vector<std::string>{ store.musicRoot });
    std::vector<std::string> files;
    bool remembered = false;
    if (given) files = *given;
    else if (!ready_ && !roots.empty()) {
        // Launch: the list from last time, if it is for the same folder ("root=" first, then one path a line).
        if (FILE* f = fopen(store.path("library.tsv").c_str(), "rb")) {
            char line[2048];
            std::string root;
            if (fgets(line, sizeof line, f)) { root = line; while (!root.empty() && (root.back() == '\n' || root.back() == '\r')) root.pop_back(); }
            if (root == "root=" + roots[0])
                while (fgets(line, sizeof line, f)) {
                    std::string p = line;
                    while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
                    if (!p.empty()) files.push_back(std::move(p));
                }
            fclose(f);
            remembered = !files.empty();
        }
    }
    if (!given && !remembered) {
        std::vector<std::string> lrcs;
        files = Library::listFiles(roots, &lrcs);
        lyrIndex.setSidecars(std::move(lrcs));
    }
    std::sort(files.begin(), files.end());
    rsMark("list");
    if (built) {
        // the tags in memory go over: reading them from the card again was 85 ms of the frame, on the console
        for (Track& t : built->tracks) {
            auto it = pathIndex.find(t.path);
            if (it != pathIndex.end()) t = std::move(lib.tracks[size_t(it->second)]);
        }
        lib = std::move(*built);
    } else {
        lib.build(roots, files);
    }
    rsMark("build");
    if (!remembered && !roots.empty() && files != libFiles_) {
        std::string out = "root=" + roots[0] + "\n";
        for (const auto& p : files) out += p + "\n";
        store.writeFile("library.tsv", std::move(out));
    }
    libFiles_ = files;
    if (remembered) {
        // The card may have changed since: walked again on a worker core, compared in update().
        if (libCheck_.joinable()) libCheck_.join();
        libCheckDone_ = false;
        libCheck_ = std::thread([this, roots] {
            cores::moveHere(cores::Files);
            std::vector<std::string> lrcs;
            libChecked_ = Library::listFiles(roots, &lrcs);
            std::sort(libChecked_.begin(), libChecked_.end());
            lyrIndex.setSidecars(std::move(lrcs));   // (handed over on this thread: the index compares on its own)
            libCheckDone_ = true;
        });
    }
    if (!ready_) startMark(remembered ? "list" : "scan");
    store.save();   // creates the config folder for the tag cache
    meta.overlay = [this](Track& t) { applyEdits(t); };
    meta.start(&lib, store.path("tags.tsv"), built != nullptr);   // tags.tsv: meta.tsv from 0.2 cached failed M4A reads
    if (!ready_) startMark("tags");
    rsMark("tags started");
    for (const auto& [path, file] : store.covers)
        if (auto it = std::find_if(lib.tracks.begin(), lib.tracks.end(), [&](const Track& t) { return t.path == path; }); it != lib.tracks.end())
            meta.setCoverFile(int(it - lib.tracks.begin()), file);
    rsMark("covers");
    indexDirty = true;
    rebuildIndex();
    if (!ready_) startMark("index");
    rsMark("index");
#ifdef GF_DEV
    if (ready_) logLine(rsLog);
#endif

    auto find = [&](const std::string& p) { auto it = pathIndex.find(p); return it == pathIndex.end() ? -1 : it->second; };
    now.track = find(current);
    q.tracks.clear();
    for (const auto& p : queued) if (const int t = find(p); t >= 0) q.tracks.push_back(t);
    q.order.resize(q.tracks.size());
    for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
    q.pos = -1;
    for (int i = 0; i < int(q.tracks.size()); i++) if (q.tracks[i] == now.track) q.pos = i;
    auto remap = [&](Queue& k, const std::vector<std::string>& paths) {   // its order and place kept, what is gone dropped
        std::vector<int> keep, tracks, order;
        for (const auto& p : paths) {
            const int t = p.empty() ? -1 : find(p);
            keep.push_back(t < 0 ? -1 : int(tracks.size()));
            if (t >= 0) tracks.push_back(t);
        }
        const int cur = k.pos >= 0 && k.pos < int(k.order.size()) ? k.order[size_t(k.pos)] : -1;
        int pos = -1;
        for (int o : k.order) {
            if (o < 0 || o >= int(keep.size()) || keep[size_t(o)] < 0) continue;
            if (o == cur) pos = int(order.size());
            order.push_back(keep[size_t(o)]);
        }
        k.tracks = std::move(tracks);
        k.order = std::move(order);
        k.pos = pos;
    };
    remap(tog.savedQueue, togQueued);
    remap(handoffOld, handQueued);
    // What the background player was sent is remembered by track index, and indexes mean other files now (a rename in
    // Files kept the same indexes for new paths, so the player kept the old ones): it is sent again, by path
    bgs.tracks.clear();
    history.clear();
    // Every list starts at its top again. Reset in place, never freed: a rescan can come in the middle of a frame (the
    // welcome card's button, the folder picker's "Use this folder"), and Ui still holds this frame's lists; freeing them
    // here let the next frame write into freed memory, which crashed the first launch's tour (2026-10-28, a data abort in
    // malloc on the loader thread)
    for (auto& [key, a] : areas) a = ScrollArea{};
    {   // search by lyrics: the songs to read
        std::vector<std::string> paths;
        paths.reserve(lib.tracks.size());
        for (const Track& t : lib.tracks) paths.push_back(t.path);
        lyrIndex.setLibrary(std::move(paths));
    }
}

void App::shutdown() {
#ifdef GF_DEV
    // Dev builds: how long closing takes, step by step, in shutdown-trace.txt (the user, 2026-11-05: HOME crashed closing the
    // forwarder, 1 time in 20; Songate holds the exit until this ends)
    const Uint64 sdAt0 = SDL_GetPerformanceCounter();
    Uint64 sdAt = sdAt0;
    std::string sdLog = "shutdown:";
    auto sdMark = [&](const char* what) {
        const Uint64 at = SDL_GetPerformanceCounter();
        char one[48];
        snprintf(one, sizeof one, " %s %.0f", what, double(at - sdAt) * 1000.0 / double(SDL_GetPerformanceFrequency()));
        sdLog += one;
        sdAt = at;
    };
#else
    auto sdMark = [](const char*) {};
#endif
    lyrNet.stop();
    sdMark("lyrnet");
    lyrIndex.stop();   // (its question may wait on the network: ended first)
    tagJobs.stop();    // (a write under way finishes whole first)
    mixOrderEnd();     // (an order being worked out is dropped: asked again when its playlist next needs it)
    if (pocketLocked_) pocketLock(false);   // (never left dark)
    sdMark("lyrindex");
    if (tog.stage != Tog::Off) togetherLeave();
    if (syncRebuild.joinable()) syncRebuild.join();
#ifdef GF_WINDOWED
    if (window_) keepWindowPlace();
#endif
#if defined(GF_WINDOWED) && defined(GF_DEV)
    fprintf(stderr, "frames %ld at %d Hz: mean %.2f ms, worst %.1f ms, work worst %.1f ms, late %ld, starved %u\nlaunch: %s\n", statFrames_, hz_,
            statSum_ / std::max(1L, statFrames_) * 1000, statWorst_ * 1000, statWork_, statLate_, player.takeTrace().starved, startStats_.c_str());
#endif
    closeJoinEditor();   // its worker ends first
    updater.stop();   // a download stops; the files are only swapped when it finished
    setDimmed(false);
    sdMark("start");
    persistNow();
    Store::flush();
    saveLevels();
    sdMark("save");
    stats.stop();
    lastfm.stop();
    subsonic.stop();
    syncer.stop();
    copier.stop();
    connect.stop();
#ifdef GF_AIRPLAY
    player.streamEnd();
    airplay.stop();
    player.setAirSend(nullptr);   // (the audio thread lets go of it first)
    airsend.stop();
#endif
    sdMark("net");
    identity.stop();
    levels.stop();
    sdMark("levels");
    plat::unwatchLeave();
    plat::setMediaPlaying(false);
    if (awakeHeld) plat::setKeepAwake(false);   // the console's own idle times again
    wave.cancel();
    loader.stop();
    prints.stop();
    sdMark("loader");
    Updater::stopErasing = true;   // a reset's old folder: the rest next launch
    gStorageStop = true;           // Storage's sizes: measured again when the page opens
    if (libCheck_.joinable()) libCheck_.join();
    if (tracksView.thread.joinable()) tracksView.thread.join();
    if (fileWork.thread.joinable()) fileWork.thread.join();   // a copy finishes before Gatefold goes
    if (storage.thread.joinable()) storage.thread.join();     // Storage and reset's sizes
    if (eraseOld_.joinable()) eraseOld_.join();               // a factory reset's old folder (it carries on next launch)
    if (files.lister.joinable()) files.lister.join();         // Files' listing of a folder
    if (moodThread_.joinable()) moodThread_.join();           // Mood playlists' sorting (a few ms)
    if (chimeThread_.joinable()) chimeThread_.join();         // the opening's bells
    if (supportThread_.joinable()) supportThread_.join();     // Settings › Support's reads (the crash reports)
    if (reportThread_.joinable()) reportThread_.join();       // … a report being sent (curl gives up within its timeouts)
    if (logDirty_) {
        std::string t;
        for (const auto& l : logLines_) { t += l; t += '\n'; }
        store.writeFile("log.txt", std::move(t));
    }
    sdMark("joins");
    meta.stop();
    sdMark("tags");
    player.shutdown();
    sdMark("player");
    thumbs.stop();
    sdMark("thumbs");
    bg.disconnect();   // the background player carries on by itself
    Store::stopWriter();   // the last thread of ours: none may outlive Gatefold (hbloader cannot unload it then)
    sdMark("writer");
    for (SDL_Texture* t : { now.cover, now.prevCover, now.blur, now.prevBlur, now.frost, now.prevFrost, svTex, hueTex, wheelTex, lfmQr }) if (t) SDL_DestroyTexture(t);
    g.shutdown();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    IMG_Quit();
    SDL_Quit();
    plat::quit();
    net::end();
    // An update's files that wait (the app's own .nro above all: romfs no longer reads it), a fresh start or a restore;
    // then, after Restart now, the (new) Gatefold starts again.
    const std::string next = Updater::finishPending(Store::Dir, selfPath);
#ifdef GF_WINDOWED
    const std::string setup = updater.installerFile();
    const std::string again = !setup.empty() ? setup : next.empty() ? selfPath : next;
#else
    const std::string again = next.empty() ? selfPath : next;   // (going back to 1.x: its own .nro, where it is)
#endif
    if (factoryMask_) Updater::factoryReset(Store::Dir, factoryMask_);   // what was ticked (or everything), moved aside; it starts again
#ifdef GF_DEV
    if (freshStart_) Updater::swapFreshTest(Store::Dir, true);             // Debug › New user test (dev only): as new
    if (freshRestore_) Updater::swapFreshTest(Store::Dir, false);          // … and the folder put back after
#endif
#ifndef GF_WINDOWED
    if (!next.empty()) goBackIfDue();   // an older version's .nro went in: its folder is gatefold-nx, made ready before it starts
#endif
    sdMark("quit");
#ifdef GF_DEV
    {
        char total[48];
        snprintf(total, sizeof total, " = %.0f ms", double(SDL_GetPerformanceCounter() - sdAt0) * 1000.0 / double(SDL_GetPerformanceFrequency()));
        if (FILE* f = fopen((std::string(Store::Dir) + "/shutdown-trace.txt").c_str(), "ab")) { fprintf(f, "%s%s\n", sdLog.c_str(), total); fclose(f); }
    }
#endif
    if (relaunch) plat::relaunch(again);
    plat::releaseExit();
}

#ifndef GF_WINDOWED
namespace {
/** What a settings folder holds, in words: its playlists, liked songs and when it last changed (a few small files, read once). */
std::string folderWords(const std::string& dir) {
    int lists = 0, liked = 0;
    if (FILE* f = fopen((dir + "/playlists.txt").c_str(), "rb")) {
        char l[1024];
        while (fgets(l, sizeof l, f)) if (l[0] && l[0] != '\n' && l[0] != '\r') lists++;   // a name a line (lists.txt holds their songs)
        fclose(f);
    }
    if (FILE* f = fopen((dir + "/liked.txt").c_str(), "rb")) {
        char l[1024];
        while (fgets(l, sizeof l, f)) if (l[0] && l[0] != '\n' && l[0] != '\r') liked++;
        fclose(f);
    }
    std::string when = "never changed";
    struct stat st;
    if (stat((dir + "/settings.txt").c_str(), &st) == 0) {
        char b[32];
        strftime(b, sizeof b, "%d %b %Y, %H:%M", localtime(&st.st_mtime));
        when = std::string("changed ") + b;
    }
    return std::to_string(lists) + (lists == 1 ? " playlist, " : " playlists, ") + std::to_string(liked) + " liked, " + when;
}
}  // namespace

/** Both folders hold settings (an older Songate copied by hand made gatefold-nx again): which to keep. The other becomes a backup. */
void App::askBothFolders() {
    confirm(ICON_GEAR, "Settings", "Two sets of settings", "An older Songate made its own", "Which settings do you want to keep?",
            "This version's, in config/songate-nx: " + folderWords(GFCFG_NEW) + ". The older version's, in config/gatefold-nx: " +
                folderWords(GFCFG_OLD) + ". The other set is kept as a backup in Settings › Updates › Backups. Nothing is deleted.",
            "Keep this version's", [this] {
                cfgmove::settle(false);
                toast("The older version's settings are a backup now");
            });
    sheet.askSafe = true;
    sheet.askNoCancel = true;
    sheet.askYesIcon = ICON_CHECK;
    sheet.askAlt = "Keep the older version's";
    sheet.askAltYes = [this] {
        cfgmove::settle(true);   // at the next launch, before anything reads the folder
        relaunch = true;
        running = false;
    };
}

bool App::goBackIfDue() {
    const std::string to = Updater::takeBack(Store::Dir);
    if (to.empty()) return false;
    std::string log;
    cfgmove::back(Updater::pretty(to), log);   // (what it did is written in the folder's songate-folder.txt)
    return true;
}
#endif

void App::onLeave(bool exiting, void* self) {
    auto* app = static_cast<App*>(self);
    app->setDimmed(false);   // the backlight is the system's again outside Gatefold
    app->persistNow();
    app->saveLog();   // closed from the HOME menu, Songate never reaches its own end: the log is saved as it leaves (session 11's
                      // AirPlay tests lost all but their first 50 s)
#ifdef GF_ANDROID
    app->meta.saveTags();   // Android ends a backgrounded app without its own end: every tag would be read again at the next start
#endif
    if (exiting) Store::flush();
}

void App::persistSoon(float delay) {
    if (persistAt == 0 || time + delay < persistAt) persistAt = time + delay;
}

void App::saveStats() {
    statsSavedAt = time;
    if (stats.loaded() && stats.dirty()) {
        store.writeFile("stats.tsv", stats.serialise());
        store.writeFile("sync-plays.tsv", stats.serialiseOthers());
    }
}

void App::saveLevels() {
    if (levels.loaded() && levels.dirty()) {
        store.writeFile("loudness.tsv", levels.serialise());
        store.writeFile("mix.tsv", levels.serialiseMix());
        store.writeFile("mood.tsv", levels.serialiseMood());
    }
}

/** Writes the settings and the session (sessionOnly: just the session). The files are written on a thread. */
void App::persistNow(bool sessionOnly) {
    if (!sessionOnly) persistAt = 0;
    if (!ready_) return;
    sessionAt = time;
    store.volume = volumeShown;
    if (!sessionOnly) {
        // A colour being tried in the picker is not chosen yet: what it was is saved until Use
        const int live = pick.field ? *pick.field : 0;
        if (pick.field) *pick.field = pick.before;
        store.save();
        if (pick.field) *pick.field = live;
    }
    saveStats();
    if (lyricNudgeUnsaved_) { lyricNudgeUnsaved_ = false; store.saveLyricNudges(); }
    if (!store.resume) return;
    Session s;
    const Queue& kept = tog.queueTaken ? tog.savedQueue : q;   // a session's list is never saved
    s.name = kept.name;
    for (int t : kept.tracks) s.tracks.push_back(lib.tracks[t].path);
    s.order = kept.order;
    s.pos = kept.pos;
    // the position only when the player holds the queue's own track: saved in the moment between a track change and its load, it was
    // the track before's (the phone, 2026-11-06: True Colors restored at 4:49 of its 3:52)
    const bool same = kept.pos >= 0 && kept.pos < int(kept.order.size()) && now.track == kept.tracks[size_t(kept.order[size_t(kept.pos)])];
    s.position = player.loaded() && !tog.queueTaken && !now.loading && same && !srvq_.on ? player.position() : 0;
    store.saveSession(s);
    // the server's queue, beside it: to the card only when its list changed, or at the close (the position is RAM's until then);
    // written at every save it wore the card for nothing (the user, 2026-11-09: "writing the queue every single second is crazy")
    {
        static std::string lastList;
        static bool lastKnown = false;
        if (!lastKnown) {   // what the card holds from the last run: the launch's first save wrote the same list again
            lastKnown = true;
            std::string have;
            if (FILE* f = fopen(store.path("server-queue.tsv").c_str(), "rb")) {
                char buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) have.append(buf, n);
                fclose(f);
            }
            const size_t nl = have.find('\n');
            lastList = nl == std::string::npos ? have : have.substr(nl + 1);
        }
        const std::string sq = serverQueueText();
        const size_t nlAt = sq.find('\n');
        const std::string list = nlAt == std::string::npos ? sq : sq.substr(nlAt + 1);
        if (sq.empty()) { if (!lastList.empty()) { lastList.clear(); store.removeFile("server-queue.tsv"); } }
        else if (list != lastList || !running) { lastList = list; store.writeFile("server-queue.tsv", sq); }
    }
}

/** Puts back the queue of the last run, paused where it was. Paths that are gone are dropped. */
void App::restoreSession() {
    Session s;
    if (!store.loadSession(s)) return;
    std::vector<int> keep;   // old index → new index, −1 when the file is gone
    for (const auto& p : s.tracks) {
        auto it = pathIndex.find(p);
        keep.push_back(it == pathIndex.end() ? -1 : int(q.tracks.size()));
        if (it != pathIndex.end()) q.tracks.push_back(it->second);
    }
    if (q.tracks.empty()) return;
    q.name = s.name;
    const int cur = s.pos >= 0 && s.pos < int(s.order.size()) && s.order[s.pos] >= 0 && s.order[s.pos] < int(keep.size()) ? keep[s.order[s.pos]] : -1;
    q.order.clear();
    for (int o : s.order) if (o >= 0 && o < int(keep.size()) && keep[o] >= 0) q.order.push_back(keep[o]);
    if (q.order.size() != q.tracks.size()) {   // damaged or partial: the saved order is not a permutation any more
        q.order.resize(q.tracks.size());
        for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
    }
    q.pos = 0;
    for (int i = 0; i < int(q.order.size()); i++) if (q.order[i] == cur) q.pos = i;
    queueNormalize();   // an older queue may hold a "Play next" outside the list's order
    holdOrderForAutomix(true);   // an Automix playlist restored with Shuffle on: its own order
    if (cur < 0) return;   // the track that was playing is gone: keep the queue, start nothing
    loadTrack(q.tracks[q.order[q.pos]], true);
    const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[size_t(now.track)].duration;
    if (player.loaded() && s.position > 1 && (dur <= 0 || s.position < dur - 1)) player.seek(s.position);   // (never past its end)
    savedTrack = now.track;
}

void App::run() {
    lastTick_ = SDL_GetPerformanceCounter();
#if defined(GF_ANDROID) && defined(GF_DEV)
    std::rename(store.path("log.txt").c_str(), store.path("log-previous.txt").c_str());
    auto nowMs = [] { return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
    frameDoneMs_ = nowMs();
    watchdog_ = std::thread([this, nowMs] {
        int told = 0;   // seconds of this stall already written: 3, 10, 30
        while (!watchdogQuit_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const int64_t since = nowMs() - frameDoneMs_.load();
            if (since < 3000) { told = 0; continue; }
            const int secs = int(since / 1000);
            if ((told < 3 && secs >= 3) || (told < 10 && secs >= 10) || (told < 30 && secs >= 30)) {
                told = secs >= 30 ? 30 : secs >= 10 ? 10 : 3;
                if (FILE* f = fopen(store.path("stall.txt").c_str(), "a")) {
                    const time_t t = std::time(nullptr);
                    char b[16];
                    strftime(b, sizeof b, "%H:%M:%S", localtime(&t));
                    fprintf(f, "%s  a frame not finished for %d s, at: %s\n", b, secs, stage_.load());
                    fclose(f);
                }
            }
        }
    });
#endif
    while (running && plat::mainLoop()) frame();
#if defined(GF_ANDROID) && defined(GF_DEV)
    watchdogQuit_ = true;
    if (watchdog_.joinable()) watchdog_.join();
#endif
}

#ifdef GF_WINDOWED
/** The window, in app pixels and real ones: the shorter way it is 1280×720's scale, the other way there is more room. */
void App::fitWindow() {
    int pw = 1280, ph = 720;
    SDL_GetRendererOutputSize(renderer_, &pw, &ph);
    if (pw <= 0 || ph <= 0) return;
    // As big as on the whole screen (the console's 1280×720 filling it the short way), however small the window: it shows less, like
    // any desktop app, and only under kMinW × kMinH does everything shrink with it. In twentieths, so that dragging an edge does
    // not make all the text again for each pixel of a change
    SDL_Rect screen{ 0, 0, 1280, 720 };
    const int on = SDL_GetWindowDisplayIndex(window_);
    if (on >= 0) SDL_GetDisplayBounds(on, &screen);
    constexpr float kMinW = 1000, kMinH = 600;
#ifdef GF_ANDROID
    // Settings › Appearance › Layout › Size: a phone is held close, its screen small; the short side holds 720, 660 or 600 app
    // pixels (600: the least the layout is made for, kMinH)
    static const float kTall[3] = { 720, 660, 600 };
    const float tall = kTall[std::clamp(store.uiSize, 0, 2)];
    const float full = std::min(screen.w / (1280.f * tall / 720), screen.h / tall);
#else
    const float full = std::min(screen.w / 1280.f, screen.h / 720.f);
#endif
    float s = std::max(0.25f, std::floor(std::min({ full, pw / kMinW, ph / kMinH }) * 20) / 20);
#ifdef GF_ANDROID
    // portrait: a phone held upright, about 400 across as its own apps are (the user, 2026-11-04: "everything is so tiny... It's a
    // phone"); always 440, Standard: Size is sideways' only (the user, 2026-11-04)
    if (pw < ph) s = std::max(0.25f, std::floor(pw / 440.f * 20) / 20);
#endif
    css::W = int(std::ceil(pw / s - 0.01f));
    css::H = int(std::ceil(ph / s - 0.01f));
    SDL_RenderSetScale(renderer_, s, s);
    SDL_DisplayMode m{};
    const int display = SDL_GetWindowDisplayIndex(window_);
    hz_ = display >= 0 && SDL_GetDesktopDisplayMode(display, &m) == 0 && m.refresh_rate > 0 ? m.refresh_rate : 60;
    const bool sameSize = frameTex_[0] && s == scale_ && pw == framePw_ && ph == framePh_;
    g.setDisplay(s, hz_);
    scale_ = s;
    framePw_ = pw;
    framePh_ = ph;
    if (sameSize) return;
    // A little larger than needed: a window being dragged wider keeps them, instead of a new pair every pixel
    if (frameTex_[0] && pw <= frameTexW_ && ph <= frameTexH_) return;
    frameTexW_ = (pw + 255) / 256 * 256;
    frameTexH_ = (ph + 255) / 256 * 256;
    for (SDL_Texture*& t : frameTex_) {
        if (t) SDL_DestroyTexture(t);
        t = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, frameTexW_, frameTexH_);
        if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    }
}

/** Windows holds a window's owner while its edge is dragged: its messages come from inside SDL's pump, which does not
 *  return until the mouse is let go. So the frames go on from here, between frames by construction (App::frame). */
int App::resizeWatch(void* self, SDL_Event* e) {
    auto* app = static_cast<App*>(self);
    if (e->type != SDL_WINDOWEVENT || !app->pumping_ || app->resizing_) return 0;
    if (e->window.event != SDL_WINDOWEVENT_SIZE_CHANGED && e->window.event != SDL_WINDOWEVENT_EXPOSED && e->window.event != SDL_WINDOWEVENT_MOVED) return 0;
    app->resizing_ = true;
    app->fitWindow();
    app->frame();
    app->resizing_ = false;
    return 0;
}
#endif

#ifdef GF_WINDOWED
/** Between frames: going full screen resizes everything. The Canvas is always full screen; F11 and Alt+Enter, the whole app. */
void App::applyFullscreen() {
    fullCanvas_ = canvas;
#ifdef GF_ANDROID
    return;   // always the whole screen
#endif
    const bool want = fullApp_ || fullCanvas_;
    if (want == fullNow_) return;
    if (want) keepWindowPlace();
    fullNow_ = want;
    SDL_SetWindowFullscreen(window_, want ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    fitWindow();
}

/** The window has no frame of its own: its edges still resize it, and the app's title bar moves it (and snaps it, and a double click
 *  maximises it), but for what is pressed there. */
SDL_HitTestResult App::hitTest(SDL_Window* w, const SDL_Point* p, void* self) {
    auto* app = static_cast<App*>(self);
    if (app->fullNow_) return SDL_HITTEST_NORMAL;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_MAXIMIZED)) {
        constexpr int m = 7, c = 16;
        const bool l = p->x < m, r = p->x >= ww - m, t = p->y < m, b = p->y >= wh - m;
        const bool lc = p->x < c, rc = p->x >= ww - c, tc = p->y < c, bc = p->y >= wh - c;
        if ((t && lc) || (l && tc)) return SDL_HITTEST_RESIZE_TOPLEFT;
        if ((t && rc) || (r && tc)) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if ((b && lc) || (l && bc)) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if ((b && rc) || (r && bc)) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (t) return SDL_HITTEST_RESIZE_TOP;
        if (b) return SDL_HITTEST_RESIZE_BOTTOM;
        if (l) return SDL_HITTEST_RESIZE_LEFT;
        if (r) return SDL_HITTEST_RESIZE_RIGHT;
    }
    const float ax = p->x / app->scale_, ay = p->y / app->scale_;
    if (ay >= css::TbH || ax < kLightsW) return SDL_HITTEST_NORMAL;
    for (const SDL_FRect& k : app->titleKeep_) if (ax >= k.x && ax < k.x + k.w) return SDL_HITTEST_NORMAL;
    return SDL_HITTEST_DRAGGABLE;
}

/** × on the window: Keep playing leaves the app by the clock, as the console's background player keeps the music. */
void App::closeWindow() {
    if (!store.background) { running = false; return; }
    fullApp_ = false;
    if (canvas) openCanvas(false);
    applyFullscreen();
    keepWindowPlace();
    persistNow();
    plat::trayIcon(true);
}

void App::showWindow() {
    plat::trayIcon(false);
    SDL_RestoreWindow(window_);
    SDL_RaiseWindow(window_);
    lastTick_ = SDL_GetPerformanceCounter();
}

void App::keepWindowPlace() {
    const Uint32 fl = SDL_GetWindowFlags(window_);
    store.winMax = (fl & SDL_WINDOW_MAXIMIZED) != 0;
    if (fl & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) return;
    SDL_GetWindowPosition(window_, &store.winX, &store.winY);
    SDL_GetWindowSize(window_, &store.winW, &store.winH);
}

void App::systemCommands() {
    if (closePending_) { closePending_ = false; closeWindow(); }
#ifdef GF_ANDROID
    {   // Settings › Playback › Keep playing when swiped away, to Android's side: at launch and whenever it changes
        static int told = -1;
        if (told != int(store.swipeKeep)) { told = int(store.swipeKeep); plat::keepOnSwipe(store.swipeKeep); }
    }
#endif
#ifdef GF_DEV
    if (static const char* cmd = getenv("GF_ITEMS"); cmd) {   // tests: a toast shown, for the log
        static std::string seen;
        if (toastText != seen) { seen = toastText; if (!seen.empty()) fprintf(stderr, "toast: %s\n", seen.c_str()); }
    }
#endif
    for (plat::Cmd c = plat::nextCommand(); c != plat::Cmd::None; c = plat::nextCommand()) switch (c) {
        case plat::Cmd::PlayPause: togglePlay(); break;
        case plat::Cmd::Play: if (!isPlaying()) togglePlay(); break;
        case plat::Cmd::Pause: if (isPlaying()) togglePlay(); break;
        case plat::Cmd::Next: next(false); break;
        case plat::Cmd::Previous: prev(); break;
        case plat::Cmd::Show: showWindow(); break;
        case plat::Cmd::Quit: running = false; break;
        // Android's music controls: the player bar's heart and order button, and the seek bar
        case plat::Cmd::Like:
            if ((now.track >= 0 && !extOn) || srvq_.on) toggleCurrentLike();
            break;
        case plat::Cmd::CycleOrder: if (!controlsOff()) cycleOrder(); break;
#ifdef GF_ANDROID
        case plat::Cmd::Seek: if (!controlsOff()) seekBy(plat::commandSeek() - player.position()); break;
#endif
        default: break;
    }
    // The title bar follows the interface's colours as they fade from cover to cover, a few steps a second
    if (time - titleAt_ > 0.12) {
        titleAt_ = time;
        const auto pack = [](const Rgb& c) { return (Uint32(std::lround(std::clamp(c.r, 0.f, 1.f) * 255)) << 16) | (Uint32(std::lround(std::clamp(c.g, 0.f, 1.f) * 255)) << 8) | Uint32(std::lround(std::clamp(c.b, 0.f, 1.f) * 255)); };
        const Uint32 bg = pack(pal.bg), fg = pack(pal.text);
        if (bg != titleBg_ || fg != titleFg_) {
            titleBg_ = bg;
            titleFg_ = fg;
            plat::titleColours(bg, fg);
        }
    }
    const bool playing = isPlaying() || chirpsDue();   // (the chirps: "playing" to Android and Windows, which tell a Bluetooth speaker)
    const bool has = now.track >= 0;
    if (has != mediaHas_ || playing != mediaPlaying_) {
        mediaHas_ = has;
        mediaPlaying_ = playing;
        plat::mediaState(has, playing);
    }
#ifdef GF_ANDROID
    // The controls' seek bar, heart and order: told when one changed or the position jumped (Android runs the bar on by itself)
    {
        const double pos = has ? shownPosition() : 0, dur = has ? now.info.duration : 0;
        const bool liked = has && currentLiked();
        const int order = int(shownOrder());
        const double expected = mediaPos_ + (mediaPlaying_ ? time - mediaPosAt_ : 0);
        if (liked != mediaLiked_ || order != mediaOrder_ || dur != mediaDur_ || std::fabs(pos - expected) > 1.5 || mediaPushed_ != mediaPlaying_) {
            mediaLiked_ = liked;
            mediaOrder_ = order;
            mediaDur_ = dur;
            mediaPushed_ = mediaPlaying_;
            plat::mediaProgress(pos, dur, liked, order);
        }
        mediaPos_ = pos;
        mediaPosAt_ = time;
    }
#endif
}
#endif

bool App::chirpsDue() {
    const double last = player.tonesLast();
    return last > 0 && player.syncEnabled() && player.syncNow() < last + micm::kChirp + 0.5;
}

void App::frame() {
#ifdef GF_ANDROID
    stage_ = "size";
    {   // a turn: SDL's resize event can come before the surface has its new size; the size itself, each frame, is what counts
        int pw = 0, ph = 0;
        SDL_GetRendererOutputSize(renderer_, &pw, &ph);
        if (pw > 0 && ph > 0 && (pw != framePw_ || ph != framePh_)) {
            if (framePw_ > 0 && (pw > ph) != (framePw_ > framePh_)) {   // upright ⇄ sideways: the new layout fades in
                turnAt_ = time;
                if (canvas && pw < ph) openCanvas(false);   // turned upright: the Canvas closes (it is sideways' only)
            }
            fitWindow();
        }
    }
#endif
#ifdef GF_WINDOWED
    if (!resizing_) {
        stage_ = "events";
        pumping_ = true;
        SDL_PumpEvents();
        pumping_ = false;
    }
#endif
    const double freq = double(SDL_GetPerformanceFrequency());
    {
        const Uint64 nowTicks = SDL_GetPerformanceCounter();
        const double interval = double(nowTicks - lastTick_) / freq;
        lastTick_ = nowTicks;
        // Frames are presented on vsync, so each one is on screen for a whole number of refreshes. The measured
        // interval jitters a millisecond or two around that; stepping animations by the jitter made their
        // motion uneven, most visibly at the slow start and end of an eased one. So dt is whole refreshes (at most a tenth
        // of a second), of this screen: 60 a second on the console, 144 or whatever the monitor under the window does on a PC.
        dt = std::clamp(int(std::lround(interval * hz_)), 1, hz_ / 10) / float(hz_);
#ifdef GF_PREVIEW
        // the README's film (hostshim, GF_FILM): exactly 1/60 s a frame, however long the frame took to draw
        if (static const bool film = getenv("GF_FILM") != nullptr; film) dt = 1.f / 60;
#endif
        time += dt;

        profAt_ = nowTicks;
        std::copy(std::begin(profFrame_), std::end(profFrame_), std::begin(profPrev_));
        std::fill(std::begin(profFrame_), std::end(profFrame_), 0.0);
        ui.beginFrame(dt);
#ifdef GF_PREVIEW
        // the preview's look at the Speakers group (GF_SPEAKER_MOCK): the Devices sheet opens by itself once things have settled
        if (static bool opened = false; !opened && getenv("GF_SPEAKER_MOCK") && time > 12) { opened = true; openAirPlay(); }
#endif
        stage_ = "input";
        input();
        stage_ = "update";
        update();
        stage_ = "draw";
        profMark(0);
#ifdef GF_WINDOWED
        // Minimised or in the tray: the music goes on, nothing is drawn
        if (SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) {
            ui.endFrame();
            stage_ = "out of sight";   // (a frame all the same: the watchdog's)
            frameDoneMs_ = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#ifdef GF_ANDROID
            SDL_Delay(isPlaying() ? 16 : 100);   // out of sight and paused: the controls' presses ten times a second, the battery spared
#else
            SDL_Delay(16);
#endif
            return;
        }
        SDL_SetRenderTarget(renderer_, frameTex_[frameCur_]);
        SDL_RenderSetScale(renderer_, scale_, scale_);
#endif
        draw();
        drawOpening();
#ifdef GF_ANDROID
        // a turn: Android's own animation, then the new layout comes out of the background's colour, as Songate's pages change
        // (0.35 s, eased); Motion off: at once (the user, 2026-11-04, after trying the cover's glide: "go back to the old animation")
        if (store.motion && time - turnAt_ < 0.35) {
            const float t = float(time - turnAt_) / 0.35f, a = 1 - t * t * (3 - 2 * t);
            g.rect(0, 0, float(css::W), float(css::H), pal.bg, a);
        }
#endif
#ifdef GF_DEV
        if (optShowTouch_)   // (dev: every touch area outlined)
            ui.eachItem([&](const SDL_FRect& r) { g.roundStroke(r.x, r.y, r.w, r.h, 4, 1.5f, { 1, 0.3f, 0.35f }, 0.9f); });
#endif
        ui.endFrame();
#if defined(GF_WINDOWED) && defined(GF_DEV)
        // Tests press controls by name: every few frames, what is on screen and where
        if (static const char* items = getenv("GF_ITEMS"); items && profFrames_ % 10 == 0) {
            const std::string tmp = std::string(items) + ".new";
            if (FILE* out = fopen(tmp.c_str(), "w")) {
                char label[320];
                snprintf(label, sizeof label, "scale %.3f size %d %d pos %.2f playing %d vol %.3f track %s", double(scale_), css::W, css::H, player.loaded() ? player.position() : -1.0,
                         int(isPlaying()), double(store.volume), now.track >= 0 ? lib.tracks[now.track].title.c_str() : "-");
                ui.audit(label, out);
                for (const auto& t : g.said)   // TOP: over the page (a sheet, a card, a toast)
                    fprintf(out, "%s %.0f %.0f %.0f %.0f %d|%s\n", t.layer ? "TOP" : "TEXT", double(t.x), double(t.y), double(t.w), double(t.h), int(t.cut), t.text.c_str());
                fclose(out);
                rename(tmp.c_str(), items);
            }
        }
#endif
#ifdef GF_PREVIEW
        // The preview's D-pad audit: hostshim sets GF_AUDIT from the script's "audit <label>"
        if (const char* a = getenv("GF_AUDIT"); a && *a) {
            static FILE* out = fopen("dpad-audit.txt", "w");
            if (out) ui.audit(a, out);
            _putenv("GF_AUDIT=");
        }
#endif
        frameMs_[frameAt_] = float(interval * 1000);
        spanWork_[frameAt_] = workMs_[(frameAt_ + 119) % 120];
        spanPres_[frameAt_] = lastPres_;
        workMs_[frameAt_] = float(double(SDL_GetPerformanceCounter() - nowTicks) / freq * 1000);
#ifdef GF_DEV
        traceJoinFrame(frameMs_[frameAt_], workMs_[frameAt_]);
        selfTestTick(frameMs_[frameAt_], workMs_[frameAt_]);   // Debug › Testing (selftest.cpp)
        if (store.frameTimes) {
            frameLogWorst_ = std::max(frameLogWorst_, frameMs_[frameAt_]);
            frameLogSlow_ += frameMs_[frameAt_] > 33.3f;
            frameLogN_++;
            if (time - frameLogAt_ >= 5) {
                if (frameLogAt_ > 0) {
                    char line[160];
                    snprintf(line, sizeof line, "Frames: 5 s, %d frames, worst %.1f ms, over 33 ms %d; playing %d (here %d), in a session %d", frameLogN_,
                             double(frameLogWorst_), frameLogSlow_, int(isPlaying()), int(isPlaying() && !player.remote()), int(togetherOn()));
                    logLine(line);
                }
                frameLogAt_ = time;
                frameLogWorst_ = 0;
                frameLogSlow_ = frameLogN_ = 0;
            }
        }
#endif
        frameAt_ = (frameAt_ + 1) % 120;
        profAt_ = SDL_GetPerformanceCounter();
#ifdef GF_WINDOWED
        SDL_SetRenderTarget(renderer_, nullptr);
        const SDL_Rect whole{ 0, 0, framePw_, framePh_ };
        SDL_RenderCopy(renderer_, frameTex_[frameCur_], &whole, nullptr);
        frameCur_ ^= 1;
#endif
        const Uint64 presAt = SDL_GetPerformanceCounter();
        stage_ = "present";
        SDL_RenderPresent(renderer_);
        stage_ = "after present";
        frameDoneMs_ = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        lastPres_ = float(double(SDL_GetPerformanceCounter() - presAt) / freq * 1000);
        profMark(7);
        if (firstFrame_) { firstFrame_ = false; launchAt("frame1"); }
#ifdef GF_DEV
        g.said.clear();
        g.saidLayer = 0;
#endif
#if defined(GF_WINDOWED) && defined(GF_DEV)
        statFrames_++;
        statSum_ += interval;
        statWorst_ = std::max(statWorst_, interval);
        statWork_ = std::max(statWork_, double(workMs_[(frameAt_ + 119) % 120]));
        statLate_ += interval > 1.5 / hz_;
#endif
        profFrames_++;
        g.endFrame();
    }
}

// ============================================================ input

/** + (or a long touch, held): the menu of the row under focus. A long touch on something without one (a button)
 *  does nothing, without the toasts or Now playing's own track. */
void App::plusMenu(bool held) {
    if (route.view == View::Playlists && !canvas && serverPlaylistsShown()) {   // a server playlist: its menu
        if (hoverSrvPlShown_ >= 0 && hoverSrvPlShown_ < int(srvPlaylists_.size())) openServerPlaylistMenu(srvPlaylists_[size_t(hoverSrvPlShown_)]);
        else if (!held) toast("Select a playlist, then " GF_MENU_WORDS);
    } else if (route.view == View::Playlists && !canvas) {   // the Playlists page: the playlist's menu (Move, Icon, Rename)
        if (plHoverShown >= 0) openPlaylistMenu(plHoverShown);
        else if (!held) toast(plHoverShown == -1 ? "Liked stays first" : "Select a playlist, then " GF_MENU_WORDS);
    } else if (route.view == View::Files && !canvas) {   // Files: the menu of the row under focus
        if (!files.hover.empty()) openFileMenu(files.hover);
        else if (!held) toast("Select a file or folder, then " GF_MENU_WORDS);
    } else {
        if (hoverSrvShownOn_ && hoverTrackShown < 0) { openServerMenu(hoverSrvShown_, hoverSrvIdxShown_); return; }   // a server track's row under focus
        // Now playing (or the Canvas) with a server track playing: its menu, not the track of yours played before it (now.track keeps
        // that one while the server's queue plays: the user, 2026-11-06)
        if (hoverTrackShown < 0 && !held && (route.view == View::Now || canvas) && srvq_.on && srvq_.pos >= 0 && srvq_.pos < int(srvq_.songs.size())) {
            openServerMenu(srvq_.songs[size_t(srvq_.pos)]);
            return;
        }
        const int t = hoverTrackShown >= 0 ? hoverTrackShown : !held && (route.view == View::Now || canvas) && !extOn ? now.track : -1;
        if (t >= 0) openTrackMenu(t, hoverTrackShown >= 0 ? hoverQueueShown : -1, hoverUpNextShown);
        else if (!held) toast("Select a song, then " GF_MENU_WORDS);
    }
}

void App::input() {
    SDL_Event e;
    bool touched = false;
    bool mouseMoved = false, mouseClicked = false;
#ifdef GF_WINDOWED
    bool rightClick = false;
#endif
    float wheel = 0;
#ifdef GF_WINDOWED
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {   // pumped at the top of the frame
#else
    while (SDL_PollEvent(&e)) {
#endif
#ifdef GF_WINDOWED
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) closeWindow();
        if (e.type == SDL_FINGERDOWN) typingByTouch_ = true;
        if (e.type == SDL_KEYDOWN || (e.type == SDL_MOUSEBUTTONDOWN && e.button.which != SDL_TOUCH_MOUSEID)) typingByTouch_ = false;
        if (e.type == SDL_KEYDOWN && !e.key.repeat &&
            (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))) fullApp_ = !fullApp_;
        if (e.type == SDL_QUIT) running = false;
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)) fitWindow();
#endif
        // A mouse (a USB one on the console; the PC's in the preview and the live page): it moves the pointer, its wheel
        // scrolls the list under it, its left button presses there (the user, 2026-10-30: "the UI should scroll where the
        // mouse is, app wide"). The mouse events SDL makes up from touches are left to the finger events.
        if ((e.type == SDL_MOUSEMOTION && e.motion.which != SDL_TOUCH_MOUSEID) ||
            ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && e.button.which != SDL_TOUCH_MOUSEID)) {
            int ww = css::W, wh = css::H;
            SDL_GetWindowSize(window_, &ww, &wh);
            const float mx = (e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x) * float(css::W) / float(std::max(1, ww));
            const float my = (e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y) * float(css::H) / float(std::max(1, wh));
            pointer.x = std::clamp(mx, 0.f, float(css::W - 1));
            pointer.y = std::clamp(my, 0.f, float(css::H - 1));
            pointer.idle = 0;
            mouseMoved = true;
#ifdef GF_WINDOWED
            pointerIsMouse_ = true;
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) rightClick = true;
#endif
#ifdef GF_WINDOWED
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) lightDown_ = lightAt(pointer.x, pointer.y);
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && lightDown_ >= 0) {
                const int was = lightDown_;
                lightDown_ = -1;
                if (lightAt(pointer.x, pointer.y) == was) pressLight(was);
                continue;
            }
            if (lightDown_ >= 0) continue;
#endif
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) { ui.touchDown(pointer.x, pointer.y); mouseHeld_ = true; mouseClicked = true; }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && mouseHeld_) { ui.touchUp(pointer.x, pointer.y); mouseHeld_ = false; }
        }
        if (e.type == SDL_MOUSEWHEEL && e.wheel.which != SDL_TOUCH_MOUSEID) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
            wheel += e.wheel.preciseY;
#else
            wheel += float(e.wheel.y);
#endif
            pointer.idle = 0;
        }
        if (e.type == SDL_FINGERDOWN || e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN) openingSkip_ = true;
        if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION) touched = true;
        const float x = e.tfinger.x * css::W, y = e.tfinger.y * css::H;
#ifdef GF_WINDOWED
        if (e.type == SDL_FINGERDOWN && x < 12 && y < css::TbH) lightsHold_ = 4;   // a finger in the corner
        if (e.type == SDL_FINGERDOWN && lightAt(x, y) >= 0) { lightDown_ = lightAt(x, y); continue; }
        if (e.type == SDL_FINGERUP && lightDown_ >= 0) {
            const int was = lightDown_;
            lightDown_ = -1;
            if (lightAt(x, y) == was) pressLight(was);
            continue;
        }
#endif
        if (pocketLocked_) { if (e.type == SDL_FINGERDOWN) touched = true; continue; }   // (locked: no touch reaches anything)
        if (e.type == SDL_FINGERDOWN) ui.touchDown(x, y, true);
        else if (e.type == SDL_FINGERMOTION) ui.touchMove(x, y);
        else if (e.type == SDL_FINGERUP) ui.touchUp(x, y);
    }

    plat::padRead();
    if (pocketLocked_) { pocketTick(plat::padDown() != 0 || touched); return; }   // locked: only the unlock (pocketTick)
    if (pocketGuard_) pocketGuardTick();
    uint64_t down = plat::padDown();
    if (down) openingSkip_ = true;
#ifdef GF_WINDOWED
    if (down && plat::padWasLast()) typingByTouch_ = true;
    if (fitPending_) { fitPending_ = false; fitWindow(); }
    applyFullscreen();
    updateLights();
    if (rightClick) down |= plat::BtnPlus;
    systemCommands();
#endif
    uint64_t held = plat::padHeld();
    // Any input brings the Canvas controls back.
#ifdef GF_WINDOWED
    const bool woken = touched || mouseMoved || wheel != 0;   // the mouse counts too: the Canvas's controls and a dimmed screen come back
#else
    const bool woken = touched;
#endif
    canvasIdle = (down || woken) ? 0 : canvasIdle + dt;
    // ... and the screen out of its dimming. That first press only wakes it: it does nothing else.
    {
        const plat::Stick l0 = plat::stick(0), r0 = plat::stick(1);
        const bool any = down || held || woken || std::abs(l0.x) + std::abs(l0.y) > 9000 || std::abs(r0.x) + std::abs(r0.y) > 9000;
        idleFor = any ? 0 : idleFor + dt;
        static bool wakeHeld = false;   // ... nor while it stays held: it stepped the D-pad on its repeat
        if (any && dimmed) { setDimmed(false); down = 0; wakeHeld = held != 0; }
        if (wakeHeld) { if (held) held = 0; else wakeHeld = false; }
    }

    // + opens the track menu for the focused track (or the one playing); HOME closes the app, so + no longer exits.
    const bool notice = coverNotice == 1 || appCoverNotice == 1 || lyrNotice == 1 || sleepPromptAt > 0 || tourStep >= 0;   // … and the tour   // the overlay covers' modal (drawCoverNotice), the sleep card
    if ((down & plat::BtnPlus) && !notice) {
        if (sheet.page != Sheet::Closed) closeSheet();
        else plusMenu(false);
    }
    // A finger held on a row (the user, 2026-10-27): what + opens. Ui focuses the row; the rows say what they hold
    // while they are drawn, and the frame after that publishes it (the …Shown values), so it opens two frames on.
    if (ui.touchHeld()) plusAfterHold = 2;
    else if (plusAfterHold > 0 && --plusAfterHold == 0 && sheet.page == Sheet::Closed && !notice) plusMenu(true);
    const bool modal = sheet.page != Sheet::Closed || notice || je.open || ap.open;
    if (!modal && (down & plat::BtnMinus)) openCanvas(!canvas);
    // Left stick: a pointer for what the D-pad reaches badly (the player bar's right end, small
    // buttons). Slow near the centre, fast at the edge; A taps and drags under it like a finger.
    const plat::Stick ls = plat::stick(0);
    const float mag = std::hypot(float(ls.x), float(ls.y)) / 32767.f;
    bool pointerMoved = false, stickMoved = false;
    if (mag > 0.15f) {
        stickMoved = true;
        if (pointer.idle > Pointer::Hide) {
            // It appears where the focus is, so the eye does not have to look for it.
            const SDL_FRect r = ui.rectOf(ui.focusedId());
            pointer.x = r.w > 0 ? r.x + r.w / 2 : css::W / 2.f;
            pointer.y = r.w > 0 ? r.y + r.h / 2 : css::H / 2.f;
        }
        // Settings → Controls → Pointer speed scales the whole curve (0.5 is 0.6's 90 + 1250·k² px/s);
        // without acceleration the speed simply follows the stick.
        const float k = std::min(1.f, (mag - 0.15f) / 0.8f);
        const float scale = 0.35f + 1.3f * store.pointerSpeed;
        const float speed = (store.pointerAccel ? 90 + 1250 * k * k : 120 + 700 * k) * scale;   // px/s
        pointer.x = std::clamp(pointer.x + ls.x / 32767.f / mag * speed * dt, 0.f, float(css::W - 1));
        pointer.y = std::clamp(pointer.y - ls.y / 32767.f / mag * speed * dt, 0.f, float(css::H - 1));
#ifdef GF_WINDOWED
        pointerIsMouse_ = false;
#endif
        pointer.idle = 0;
        pointerMoved = true;
    } else if (mouseMoved) {
        pointerMoved = true;   // (the mouse moved it: as the stick does)
    } else {
        pointer.idle += dt;
    }
    // the wheel: the list under the pointer, a notch about two rows (the focused list when nothing scrolls there)
    if (wheel != 0 && grab.pos < 0 && !ui.scrollAreaAt(pointer.x, pointer.y, -wheel * 110)) ui.scrollFocusedArea(-wheel * 110);   // (a row held: the wheel moves it, below)
    if (wheel != 0) rehover_ = 0.5f;
    if (touched || (down & (plat::BtnUp | plat::BtnDown | plat::BtnLeft | plat::BtnRight)))
        pointer.idle = Pointer::Hide + 1;   // the D-pad or a finger took over
    const bool pointing = pointer.idle <= Pointer::Hide || pointer.down;
    if (pointing) canvasIdle = 0;
    if (pointerMoved) {
        if (pointer.down || mouseHeld_) ui.touchMove(pointer.x, pointer.y);
        else ui.point(ui.itemAt(pointer.x, pointer.y));
    } else if (rehover_ > 0) {
        // the list moved under it (the wheel, the right stick): the row now under the pointer lights, not the one it left
        rehover_ -= dt;
        if (pointer.idle <= Pointer::Hide && !pointer.down && !mouseHeld_) ui.point(ui.itemAt(pointer.x, pointer.y));
    }
    // What moves things by touch (the sidebar's handles) shows once a finger is on the screen; a button, a key, the stick or a click
    // bring Move back (the user, 2026-11-04: "only change to the sort mode by touch when touch is detected")
    if (touched) byTouch_ = true;
    if (down || stickMoved || mouseClicked) byTouch_ = false;
    if (touched) touchUi_ = true;   // (the back button: the stick alone does not take it away; a stick at rest still reports a little)
    if (down || mouseClicked) touchUi_ = false;
    // A picked-up queue row (the track menu's Move): A or B puts it down, as with a slider; a
    // finger, the stick's pointer, a click or leaving the page put it down too. Not the mouse moving: on the PC the
    // slightest move put it down as soon as it was picked up (the user, 2026-11-03); the wheel moves it instead
    if (grab.pos >= 0) {
        const View gv = grab.kind == Grab::Playlists || grab.kind == Grab::SrvPlaylists ? View::Playlists
                      : grab.kind == Grab::Tabs || grab.kind == Grab::Battery ? View::Settings
                      : grab.kind == Grab::List ? View::List : grab.kind == Grab::SrvList ? View::Server : grab.upNext ? View::Now : View::Queue;
        if ((down & (plat::BtnA | plat::BtnB)) || touched || stickMoved || mouseClicked || modal || canvas || route.view != gv ||
            (grab.kind == Grab::List && route.ref != grab.ref) || (grab.kind == Grab::SrvList && route.ref != 5) ||
            (grab.kind == Grab::SrvPlaylists && !serverPlaylistsShown())) {
            grabDrop();
            down &= ~uint64_t(plat::BtnA | plat::BtnB);
        } else if (wheel != 0) {
            static float notches = 0;   // (a touchpad's wheel comes in fractions: a step per whole notch)
            notches += wheel;
            while (notches >= 1) { grabStep(-grab.cols); notches -= 1; }
            while (notches <= -1) { grabStep(grab.cols); notches += 1; }
            wheel = 0;
        }
    }
    if (pointing && (down & plat::BtnA)) {
        ui.touchDown(pointer.x, pointer.y);
        ui.setPadMode(true);
        pointer.down = true;
    } else if (pointer.down && !(held & plat::BtnA)) {
        ui.touchUp(pointer.x, pointer.y);
        pointer.down = false;
    } else if (down & plat::BtnA) {
        ui.activate();
    }
    ui.padHeld(held & plat::BtnA);   // a Hold item (the equalizer buttons) acts when A is let go, or on a long press
    // B puts a picked-up slider down before it goes back.
    if (down & plat::BtnB)
        ui.anyEngaged() ? ui.disengage() : sleepPromptAt > 0 ? closeSleepPrompt() : tourStep >= 0 ? tourBack() : je.open ? closeJoinEditor() : ap.open ? closeAutomixPicker() : notice ? closeCoverNotice() : modal ? sheetBack() : back();
    if (down & plat::BtnX) togglePlay();
    if (!modal && (down & plat::BtnY)) openSearch();
    if (down & plat::BtnL) prev();
    if (down & plat::BtnR) next(false);
    if (down & plat::BtnZL) seekBy(-10);
    if (down & plat::BtnZR) seekBy(10);

    const bool up = held & plat::BtnUp;
    const bool dn = held & plat::BtnDown;
    const bool lf = held & plat::BtnLeft;
    const bool rt = held & plat::BtnRight;
    if (grab.pos >= 0) {
        if (rUp.step(up, dt)) grabStep(-grab.cols);
        if (rDown.step(dn, dt)) grabStep(grab.cols);
        if (rLeft.step(lf, dt) && grab.cols > 1) grabStep(-1);
        if (rRight.step(rt, dt) && grab.cols > 1) grabStep(1);
    } else {
        const bool square = ui.focusIsSlider2D();   // a colour square picked up with A: ↑/↓ move it too
        if (rUp.step(up, dt)) square ? ui.slideY(-1) : ui.navigate(0, -1);
        if (rDown.step(dn, dt)) square ? ui.slideY(1) : ui.navigate(0, 1);
    }
    const bool slider = ui.focusIsSlider();
    // Settings: → from the category list goes to the top of its options (spatially the title bar's search
    // field was nearer than the controls at the far right of the rows); B comes back (App::back).
    auto intoSettingsPane = [&] {
        if (route.view != View::Settings || modal) return false;
        const SDL_FRect f = ui.rectOf(ui.focusedId());
        if (f.w <= 0 || f.x < sideW + edge_.side || f.x >= settingsPaneX || f.y < css::TbH || f.y + f.h > css::H - css::BarH) return false;
        const Id first = ui.firstIn(&area("settings.pane"));
        if (!first) return false;
        ui.focus(first);
        ui.setPadMode(true);
        return true;
    };
    if (grab.pos < 0) {
        if (rLeft.step(lf, dt)) slider ? ui.slide(-1) : ui.navigate(-1, 0);
        if (rRight.step(rt, dt)) { if (slider) ui.slide(1); else if (!intoSettingsPane()) ui.navigate(1, 0); }
    }

    // Right stick: the list under the pointer while it shows (the user, 2026-10-30), else whatever list has focus.
    const plat::Stick rs = plat::stick(1);
    if (std::abs(rs.y) > 6000) {
        const float dy = -rs.y / 32767.f * 1400 * dt;
        if (pointer.idle <= Pointer::Hide && ui.scrollAreaAt(pointer.x, pointer.y, dy)) rehover_ = 0.5f;
        else ui.scrollFocusedArea(dy);
        ui.setPadMode(true);
        canvasIdle = 0;   // reading the Canvas lyrics is not idle
    }
}

// ============================================================ update

// The dev build's join trace: update's slowest step this frame (updMark)
#ifdef GF_DEV
#define UPD_MARK(step) updMark(step)
#else
#define UPD_MARK(step)
#endif

void App::update() {
#ifdef GF_DEV
    updAt_ = SDL_GetPerformanceCounter();
    updPrevMs_ = updSlowMs_;
    updPrevSlow_ = updSlow_;
    updPrevTotalMs_ = updTotalMs_;
    updSlowMs_ = 0;
    updSlow_ = "";
#endif
#ifdef GF_ANDROID
    // Waiting for Android's access to the music (accessCard, the music folder page): once a second, from Android's page and back
    if (!storageOk_ && time >= storageCheckAt_) {
        storageCheckAt_ = time + 1;
        if (plat::storageAllowed()) storageGiven();
    }
    // Hidden, Android has taken the GPU's surface away: a new song's cover waits to be uploaded until Songate is seen again, and only
    // the system's music controls get it now (from memory)
    if (coverWaiting_ && hiddenNow() && coverWait_.gen == loadGen) {   // (an older song's is dropped below as ever: a newer one comes)
        if (!coverToldHidden_) {
            coverToldHidden_ = true;
            plat::mediaInfo(now.info.title, now.info.artist, now.info.album,
                            coverWait_.mediaCover ? static_cast<const uint8_t*>(coverWait_.mediaCover->pixels) : nullptr,
                            coverWait_.mediaCover ? coverWait_.mediaCover->w : 0, coverWait_.mediaCover ? coverWait_.mediaCover->h : 0, coverWait_.mediaJpeg);
        }
    } else
#endif
    if (coverWaiting_) {
        coverWaiting_ = false;
#ifdef GF_ANDROID
        coverToldHidden_ = false;
#endif
        applyCover(coverWait_);
        coverWait_ = TrackLoader::Result{};
        UPD_MARK("applyCover");
    }
    else if (TrackLoader::Result r; loader.take(r)) { applyLoaded(r); UPD_MARK("applyLoaded"); }
    lyrOnlineTick();
    {   // Search: what the lyrics index found (only songs in the library now), and a song played from a line, sought once it is in
        uint64_t t = 0;
        std::vector<LyricIndex::Hit> hits;
        if (lyrIndex.takeResults(t, hits) && t == searchTicket_) {
            searchLyrics_.clear();
            for (auto& h : hits) if (pathIndex.count(h.path)) searchLyrics_.push_back(std::move(h));
        }
        if (lyrSeekAt_ >= 0) {
            if (time > lyrSeekUntil_) lyrSeekAt_ = -1;
            else if (now.track >= 0 && !now.loading && lib.tracks[now.track].path == lyrSeekPath_ && player.loaded()) {
                player.seek(std::max(0.0, lyrSeekAt_ - 1.0));   // a moment before the line
                lyrSeekAt_ = -1;
            }
        }
    }
    bgUpdate();
    updateTick();   // Settings → Updates: what the updater's worker finished
    UPD_MARK("bg+updates");
    player.update();
    UPD_MARK("player");
#ifdef GF_DEV
    if (time - traceAt >= 5 && !player.remote()) {   // stutters with Automix (the user, 2026-10-28): what the audio thread saw
        traceAt = time;
        const Player::Trace t = player.takeTrace();
        const unsigned st = t.starved - traceStarved;
        traceStarved = t.starved;
        if (t.late || t.lockMiss || st || t.worstMs > 30) {
            char line[512];
            snprintf(line, sizeof line, "%.0f s\tworst %.1f ms\tlate %u\tlock %u\tdry %u\t%s\t%s\n", time, double(t.worstMs), t.late, t.lockMiss, st,
                     time < mixUntil ? "join" : "-", now.track >= 0 ? lib.tracks[now.track].path.c_str() : "");
            audioTrace += line;
            if (audioTrace.size() > 64 * 1024) audioTrace.erase(0, audioTrace.find('\n', audioTrace.size() - 48 * 1024) + 1);
            store.writeFile("audio-trace.txt", audioTrace);
        }
    }
#endif
    serverTick();
    tagWritesTick();
    if (player.takeFinished()) {
        if (tog.stage != Tog::Off) togetherFinished();
        else {
            next(true);
        }
    }
    UPD_MARK("next");
    if (player.remote() && time - bgListsAt > 5) { bgListsAt = time; writeBgPlaylists(); }
    // Crossfade, in Gatefold's own player: over the last seconds of the track, into the next one. Never while a stream plays
    // (extOn: the server's, AirPlay's): the player's position is the stream's, and your own track, kept for the
    // switch back, is not what is playing (a stream's first song ended in a join into your queue, 2026-11-09)
    if (tog.stage == Tog::Off && !player.remote() && !extOn && store.crossfade > 0 && !automixQueue() && !xfadeChecked && isPlaying() && now.track >= 0) {
        const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[now.track].duration;
        const double left = dur - player.position();
        if (dur > 0 && left <= store.crossfade && left > 0.25) {
            xfadeChecked = true;
            // A track shorter than two fades plays whole: the fade takes at most half of it.
            const int p = dur > store.crossfade * 2 ? crossfadeNext(left) : -1;
            if (p >= 0) playAt(p, left);
        }
    }
    UPD_MARK("crossfade");
    // a join's show while the music stops or jumps: paused (the app's clock would run it on in silence), or sought (the
    // player drops the outgoing song): ended cleanly, on every screen (endJoinShow)
    if (time < mixUntil && (!isPlaying() || player.seeks() != mixSeeksSeen)) endJoinShow();
    mixSeeksSeen = player.seeks();
    // the outgoing song let go once the join has given way to the next one (its wipe lasts at least 0.8 s: mixWipe)
    if (mixOld.on && time >= mixFrom + std::max(0.8, mixUntil - mixFrom) + 0.05) mixOld = MixOld{};
    updateAutomix();
    UPD_MARK("updateAutomix");
    automixListsTick();
    mixOrderTick();
    moodTick();
    supportTick();
    UPD_MARK("automixLists");

    // Play statistics. Gatefold counts what it plays itself; while the background player plays, it
    // counts (bg-plays.txt) and Gatefold only takes its counts in.
    stats.poll();
    stats.setBackground(player.remote());
    if (stats.loaded() && statsEpoch != libEpoch && !lib.tracks.empty()) {
        statsEpoch = libEpoch;
        std::vector<std::string> paths;
        paths.reserve(lib.tracks.size());
        for (const Track& t : lib.tracks) paths.push_back(t.path);
        stats.wantDates(paths);
    }
    if (!player.remote() && isPlaying() && now.track >= 0 && !statCounted) {
        statHeard += dt;
        const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[now.track].duration;
        if (statHeard >= Stats::threshold(dur)) {
            statCounted = true;
            stats.countPlay(lib.tracks[now.track].path, int64_t(std::time(nullptr)));
        }
    }
    lastfmTick();
    connectTick();
    UPD_MARK("stats");
    if (stats.dirty() && time - statsSavedAt > 15) saveStats();
    UPD_MARK("saveStats");
    updateSleep();
    sampleBattery();
    updateLevels();
    updateFileWork();
    UPD_MARK("sleep+levels+files");

    // Gatefold crossfades the palette over --tx (1.1 s) with an ease-out curve.
    // During an Automix join, towards the incoming track's palette from wherever it is, arriving as the join ends (the user,
    // 2026-10-28: "the UI background colour should change gradually too"); from where it is, so a cover (and its palette)
    // that arrives a moment late never makes it jump (a blend from the outgoing palette did: "sometimes not gradual")
    if (time >= mixShownAt && time < mixUntil && mixUntil > mixFrom && store.colourTransition >= 0.05f) {
        pal = paletteMix(pal, target, float(std::clamp(dt / std::max(double(dt), mixUntil - time), 0.0, 1.0)));
    } else {
        pal = store.colourTransition < 0.05f ? target : paletteMix(pal, target, 1 - std::exp(-dt / (store.colourTransition / 3.6f)));
    }
    // the covers crossfade over 0.5 s; during an Automix join, over the join itself, eased at both ends (the user, 2026-10-28:
    // "from one to another smoothly with the automix timing"), so a long blend changes the cover slowly and a cut at once
    // (from the moment the join is decided: the next track, and its cover, load 0.6 s before it begins)
    if (time >= mixShownAt && time < mixUntil && mixUntil > mixFrom) {
        const float u = float(std::clamp((time - mixFrom) / (mixUntil - mixFrom), 0.0, 1.0));
        now.fade = std::max(now.fade, u * u * (3 - 2 * u));
    } else {
        now.fade = std::min(1.f, now.fade + dt / 0.5f);
    }
    {
        const float to = sidebarFolded() ? 0.f : 1.f;
        if (to == 1 && sideP_ == 0) sideOpenedAt_ = time;
        sideP_ = !store.motion ? to : to > sideP_ ? std::min(to, sideP_ + dt / 0.38f) : std::max(to, sideP_ - dt / 0.38f);
        const float e = sideP_ < 0.5f ? 4 * sideP_ * sideP_ * sideP_ : 1 - std::pow(-2 * sideP_ + 2, 3.f) / 2;   // cubic, in and out
        sideW = css::SideWCollapsed + (css::SideW - css::SideWCollapsed) * e;
    }
    toastT = std::max(0.f, toastT - dt);

    // Only a burst that actually arrived is analysed; between bursts the bars glide (spectrum.hpp).
    spec.params = { canvas ? store.canvasBars : store.vizBands, store.vizSensitivity, store.vizSmoothing };
    // (an outside source sounds too: AirPlay's stream feeds the same ring, the user, 2026-11-02: "not reacting to airplay")
    const bool sounding = isPlaying() || (extOn && !srvq_.on);
    if (sounding && player.latestPcm(pcm_.data(), Spectrum::N)) spec.feed(pcm_.data(), player.sampleRate());
    spec.step(dt, sounding);
    // The cover's bass pulse (Now playing, and the Canvas when asked for), eased so a kick swells rather than pops.
    // (still while Automix joins: the user, 2026-10-28)
    const float wantBeat = store.coverPulse > 0 ? std::min(0.03f, spec.beat * 0.07f) * store.coverPulse * (1 - automixFocus().quiet) : 0;
    coverBeat += (wantBeat - coverBeat) * (1 - std::exp(-dt / (wantBeat > coverBeat ? 0.05f : 0.14f)));

    // Saved as things change rather than on exit, which the HOME menu can skip: the volume once the
    // slider settles, the session on every track change, play/pause, and every 20 s while playing.
    if (std::abs(volumeShown - store.volume) > 0.0005f) { store.volume = volumeShown; persistSoon(0.8f); }
    if (now.track != savedTrack || isPlaying() != savedPlaying) {
        savedTrack = now.track;
        savedPlaying = isPlaying();
        persistSoon(0.5f);
    }
    UPD_MARK("palette+spectrum");
    // (the session was saved every 20 s while playing, for its position; close-only now: the user, 2026-11-09, the card over a position)
    if (persistAt > 0 && time >= persistAt) persistNow();
    UPD_MARK("persist");

    // The overlay's covers: when many are missing, say so once (drawCoverNotice). Looked at once the
    // library has settled a little after launch, and only with the background player connected.
    // It is a small modal: focus goes to it, B or Not now closes it. Not over the Canvas or the track menu.
    // First the app's own covers (the user, 2026-10-27): once every track's tags are in, counted; more than a tenth
    // missing (a first launch, a cleared cache) asks before the sweep runs, else it just goes on. The overlay's after.
    if (appCoverNotice == 0 && !selfTestBusy() && !lib.tracks.empty() && meta.taggedCount() >= int(lib.tracks.size()) && time > 3 && !canvas &&
        sheet.page == Sheet::Closed && sleepPromptAt <= 0 && !tourPending && tourStep < 0) {   // never over the tour: after it
        if (!coversHeld) appCoverNotice = 2;   // stopped by you (Settings): nothing to ask
        else if (std::min(meta.coversCached(), int(lib.tracks.size())) * 10 < int(lib.tracks.size()) * 9) {
            appCoverNotice = 1;
            coverMissing = int(lib.tracks.size()) - std::min(meta.coversCached(), int(lib.tracks.size()));
            coverNoticeAt = time;
            noticeFocus_ = ui.focusedId();
        } else {
            appCoverNotice = 2;
            releaseCovers();
        }
    }
    // The tour the welcome card asked for: at once, as soon as the folder's files are listed (seconds); the tags are read
    // during it (the user, 2026-10-28: reading a big library took minutes, and the app was in use by then), and the
    // covers question waits for it to end
    if (tourPending && tourStep < 0 && coverNotice != 1 && appCoverNotice != 1 && lyrNotice != 1 && sheet.page == Sheet::Closed && sleepPromptAt <= 0 &&
        !lib.tracks.empty()) {
        tourPending = false;
        startTour();
    }
    if (coverNotice == 0 && !selfTestBusy() && appCoverNotice == 2 && lyrNotice != 1 && !tourPending && tourStep < 0 && player.remote() && thumbs.running() && !store.stopThumbs && time > 6 && !lib.tracks.empty() && !canvas && sheet.page == Sheet::Closed) {
        coverMissing = 0;
        for (const auto& t : lib.tracks) coverMissing += !thumbs.has(t.path);
        coverNotice = coverMissing * 10 > int(lib.tracks.size()) ? 1 : 2;
        coverNoticeAt = time;
        if (coverNotice == 1) noticeFocus_ = ui.focusedId();
    }
    if (coverNotice == 1 && !player.remote()) closeCoverNotice();
    // Search by lyrics: once the tags are in and the covers asked about, counted; more than a tenth unread (a first launch, a
    // cleared cache) asks before reading, as the covers do, else it just goes on
    // last of the three (the user, 2026-11-05: "The lyrics card should be the last one to appear"): after the overlay's covers were
    // asked about, or once they cannot be (no background player, or stopped)
    const bool overlayAsked = coverNotice == 2 || (coverNotice == 0 && time > 6 && (!player.remote() || !thumbs.running() || store.stopThumbs));
    if (lyrNotice == 0 && !selfTestBusy() && appCoverNotice == 2 && overlayAsked && !tourPending && tourStep < 0 && !canvas && sheet.page == Sheet::Closed &&
        sleepPromptAt <= 0 && !lib.tracks.empty() && meta.taggedCount() >= int(lib.tracks.size())) {
        const LyricIndex::Progress lp = lyrIndex.progress();
        if (!lyrHeld) lyrNotice = 2;   // stopped by you (Settings): nothing to ask
        else if (lp.total == int(lib.tracks.size()) && !lp.checking) {   // (the index has the library)
            if (lp.read * 10 < lp.total * 9) {
                lyrNotice = 1;
                lyrMissing = lp.total - lp.read;
                coverNoticeAt = time;
                noticeFocus_ = ui.focusedId();
            } else {
                lyrNotice = 2;
                releaseLyrics();
            }
        }
    }

    // The sidebar's progress cards: each job that has run for a second shows, and fades out once it is done (or stopped).
    {
        Job jobs[JobSlots];
        bool busy[JobSlots] = {};
        backgroundJobs(jobs, busy);
        for (int i = 0; i < JobSlots; i++) {
            JobCard& c = jobCards[i];
            c.since = busy[i] ? c.since + dt : 0;
            if (busy[i]) c.job = jobs[i];
            const float want = busy[i] && c.since > 1 ? 1.f : 0.f;
            c.shown += (want - c.shown) * (1 - std::exp(-dt / 0.15f));
            if (c.shown < 0.005f && want == 0) c.shown = 0;
        }
    }

    // The launch check of the music folder: a rescan only if the card's files are not the remembered ones.
    if (libCheckDone_) {
        libCheckDone_ = false;
        libCheck_.join();
        if (libChecked_ != libFiles_) {
            const std::vector<std::string> files = std::move(libChecked_);
            rescan(&files);
            toast("Library updated: " + std::to_string(lib.tracks.size()) + " tracks");
        }
        libChecked_.clear();
    }

    UPD_MARK("notices+jobs+libcheck");
    if (meta.pump(renderer_)) indexDirty = true;
    UPD_MARK("meta.pump");
#ifdef GF_DEV
    updTotalMs_ = double(SDL_GetPerformanceCounter() - updAt_) * 1000.0 / double(SDL_GetPerformanceFrequency());
#endif
    if (indexDirty && time - indexAt > 1.5) rebuildIndex();
    UPD_MARK("rebuildIndex");
}

void App::rebuildIndex() {
    indexDirty = false;
    indexGen++;
    indexAt = time;
    pathIndex.clear();
    for (int i = 0; i < int(lib.tracks.size()); i++) pathIndex[lib.tracks[i].path] = i;

    // Albums come from tags alone (album + album artist, else the track's artist), whatever the folders look like.
    // Tracks without an album tag are not in any album; they stay reachable through Artists, Folders and search.
    lib.albums.clear();
    albumByKey.clear();
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        Track& t = lib.tracks[i];
        t.albumIdx = -1;
        if (t.album.empty()) continue;
        const std::string credit = t.albumArtist.empty() ? t.artist : t.albumArtist;
        const std::string key = lower(t.album) + "|" + lower(credit);
        auto it = albumByKey.find(key);
        if (it == albumByKey.end()) {
            it = albumByKey.emplace(key, int(lib.albums.size())).first;
            lib.albums.push_back({ key, t.album, credit, {}, {} });
        }
        t.albumIdx = it->second;
        Album& a = lib.albums[it->second];
        a.tracks.push_back(i);
        if (a.year.empty() && t.date.size() >= 4) a.year = t.date.substr(0, 4);
    }
    for (Album& a : lib.albums)
        std::stable_sort(a.tracks.begin(), a.tracks.end(), [&](int x, int y) {
            const Track &p = lib.tracks[x], &q2 = lib.tracks[y];
            if (p.trackNo != q2.trackNo) return (p.trackNo ? p.trackNo : 9999) < (q2.trackNo ? q2.trackNo : 9999);
            return iless(p.path, q2.path);
        });

    albumOrder.resize(lib.albums.size());
    for (int i = 0; i < int(albumOrder.size()); i++) albumOrder[i] = i;
    auto& A = lib.albums;
    std::stable_sort(albumOrder.begin(), albumOrder.end(), [&](int a, int b) {
        switch (librarySort) {
        case 1: return iless(A[a].title, A[b].title);
        case 2: return A[a].year != A[b].year ? A[a].year > A[b].year : iless(A[a].title, A[b].title);
        case 3: return iless(lib.tracks[A[a].tracks[0]].path, lib.tracks[A[b].tracks[0]].path);
        default:
            if (strcasecmp(A[a].artist.c_str(), A[b].artist.c_str()) != 0) return iless(A[a].artist, A[b].artist);
            return A[a].year != A[b].year ? A[a].year < A[b].year : iless(A[a].title, A[b].title);
        }
    });

    // Artists: everyone credited on a track, with their tracks and the albums credited to them.
    artists.clear();
    artistByKey.clear();
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        const std::string name = lib.tracks[i].artist.empty() ? "Unknown artist" : lib.tracks[i].artist;
        auto it = artistByKey.find(lower(name));
        if (it == artistByKey.end()) {
            it = artistByKey.emplace(lower(name), int(artists.size())).first;
            artists.push_back({ name, {}, {} });
        }
        artists[it->second].tracks.push_back(i);
    }
    for (int i = 0; i < int(A.size()); i++) {
        auto it = artistByKey.find(lower(A[i].artist));
        if (it != artistByKey.end()) artists[it->second].albums.push_back(i);
    }
    std::sort(artists.begin(), artists.end(), [](const Artist& a, const Artist& b) { return iless(a.name, b.name); });
    artistByKey.clear();
    for (int i = 0; i < int(artists.size()); i++) {
        artistByKey[lower(artists[i].name)] = i;
        auto& al = artists[i].albums;
        std::sort(al.begin(), al.end(), [&](int a, int b) { return A[a].year != A[b].year ? A[a].year < A[b].year : iless(A[a].title, A[b].title); });
    }

    // Genres: from the GENRE tag, several per track when separated by ";". An album is under every genre its tracks have.
    genres.clear();
    genreByKey.clear();
    std::vector<std::vector<int>> albumGenres(A.size());
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        const std::string& gs = lib.tracks[i].genre;
        size_t a = 0;
        while (a < gs.size()) {
            size_t b = gs.find(';', a);
            if (b == std::string::npos) b = gs.size();
            std::string name = gs.substr(a, b - a);
            a = b + 1;
            while (!name.empty() && name.front() == ' ') name.erase(0, 1);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) continue;
            auto it = genreByKey.find(lower(name));
            if (it == genreByKey.end()) {
                it = genreByKey.emplace(lower(name), int(genres.size())).first;
                genres.push_back({ name, {}, {} });
            }
            auto& gt = genres[it->second].tracks;
            if (gt.empty() || gt.back() != i) gt.push_back(i);
            const int al = lib.tracks[i].albumIdx;
            if (al >= 0 && std::find(albumGenres[al].begin(), albumGenres[al].end(), it->second) == albumGenres[al].end())
                albumGenres[al].push_back(it->second);
        }
    }
    for (int al = 0; al < int(A.size()); al++)
        for (int gi : albumGenres[al]) genres[gi].albums.push_back(al);
    std::sort(genres.begin(), genres.end(), [](const Artist& a, const Artist& b) { return iless(a.name, b.name); });
    genreByKey.clear();
    for (int i = 0; i < int(genres.size()); i++) {
        genreByKey[lower(genres[i].name)] = i;
        auto& al = genres[i].albums;
        std::sort(al.begin(), al.end(), [&](int a, int b) { return iless(A[a].artist, A[b].artist) || (!iless(A[b].artist, A[a].artist) && A[a].year < A[b].year); });
    }
}

int App::routeGenre() const {
    auto it = genreByKey.find(lower(route.query));
    return it == genreByKey.end() ? -1 : it->second;
}

/** Before a page that belongs to a sidebar tab (an album, an artist, a genre): its own tab when shown, else Library
 *  (which lights for it), else nowhere to show it from: a toast says which tab to show. */
bool App::tabFor(int tab, const char* page) {
    if (!(store.hiddenTabs & Store::tabBit(tab))) {
        if (tab == Store::TabAlbums) libFrom = View::Albums;
        if (tab == Store::TabFolders) folderFrom = View::Folders;
        return true;
    }
    if (!(store.hiddenTabs & Store::tabBit(Store::TabLibrary))) {
        if (tab == Store::TabAlbums) libFrom = View::Library;
        if (tab == Store::TabFolders) folderFrom = View::Library;
        return true;
    }
    toast(std::string("To see the ") + page + ", show " + Store::tabName(tab) + " or Library in Settings › Appearance › Sidebar");
    return false;
}

void App::openGenre(const std::string& name) {
    if (!name.empty() && tabFor(Store::TabGenres, "genre")) navigate(View::Genre, -1, name);
}

int App::routeAlbum() const {
    auto it = albumByKey.find(route.query);
    return it == albumByKey.end() ? -1 : it->second;
}

int App::routeArtist() const {
    auto it = artistByKey.find(lower(route.query));
    return it == artistByKey.end() ? -1 : it->second;
}

void App::openAlbum(int a) {
    if (a >= 0 && a < int(lib.albums.size())) navigate(View::Album, -1, lib.albums[a].key);
}

/** An album from outside its grids (Now playing, the track menu, Files): the tab it lights follows tabFor. */
void App::goToAlbum(int a) {
    if (a >= 0 && tabFor(Store::TabAlbums, "album")) openAlbum(a);
}

void App::openArtist(const std::string& name) {
    if (tabFor(Store::TabArtists, "artist")) navigate(View::Artist, -1, name.empty() ? "Unknown artist" : name);
}

// ============================================================ play queue

bool App::isPlaying() const { return player.loaded() && !player.paused(); }

int App::currentQueuePos() const { return q.pos >= 0 && q.pos < int(q.order.size()) ? q.order[q.pos] : -1; }

void App::playTracks(const std::vector<int>& tracks, int startPos, const std::string& name, bool shuffle) {
    if (togetherOn() && !tog.host) { toast("The queue is " + tog.peerName + "'s for now: change it there"); return; }
    if (tracks.empty()) return;
    grabDrop();   // a new queue: the row held belonged to the old one
    q.tracks = tracks;
    q.name = name;
    q.failed = 0;
    startPos = std::clamp(startPos, 0, int(tracks.size()) - 1);
    if (shuffle && !automixQueue()) store.order = Order::Shuffle;
    holdOrderForAutomix();
    q.order.resize(tracks.size());
    for (int i = 0; i < int(tracks.size()); i++) q.order[i] = i;
    if (store.order == Order::Shuffle) {
        std::shuffle(q.order.begin(), q.order.end(), rng);
        if (!shuffle) std::iter_swap(q.order.begin(), std::find(q.order.begin(), q.order.end(), startPos));
        playAt(0);
    } else {
        playAt(startPos);
    }
}

void App::playAt(int orderPos, double fade, const automix::Plan* mix) {
    if (q.order.empty()) return;
    if (togetherOn() && togetherPlayAt(std::clamp(orderPos, 0, int(q.order.size()) - 1))) return;
    q.pos = std::clamp(orderPos, 0, int(q.order.size()) - 1);
    loadTrack(q.tracks[q.order[q.pos]], false, true, fade, mix);
    // The background player skips an unplayable file itself and says so (errorGen, bgUpdate); its polled
    // state is a frame old here, and on a fresh install it still said "no queue", which skipped every track.
    if (player.remote()) {
        q.failed = 0;
    } else if (!player.loaded()) {
        // Unplayable file: say so, skip it, but never loop forever over a list of broken files.
        toast("Can't play “" + baseName(lib.tracks[q.tracks[q.order[q.pos]]].path) + "”");
        if (++q.failed < int(q.order.size())) next(false);
        else toast("None of these songs could be played");
    } else {
        q.failed = 0;
    }
}

void App::next(bool natural) {
    if (natural && libSwitchTo_ >= 0) { libSwitchNow(); return; }   // Library switched meanwhile: the other side's track, paused
    if (srvq_.on) {   // the server's queue (view_server.cpp; in a session serverPlayAt asks the starter)
        if (srvq_.pos + 1 < int(srvq_.songs.size())) serverPlayAt(srvq_.pos + 1);
        else if (natural) player.stop();
        return;
    }
    if (extOn) { if (!natural) toast("Skip on " + extFrom); return; }
    if (q.order.empty()) return;
    if (natural && store.order == Order::RepeatOne) return playAt(q.pos);
    if (natural && stopAfterCurrent) {
        stopAfterCurrent = false;
        player.stop();
        return;
    }
    int p = q.pos + 1;
    if (p >= int(q.order.size())) {
        if (store.order == Order::Default && natural) {
            player.stop();
            return;
        }
        if (store.order == Order::Shuffle) { grabDrop(); std::shuffle(q.order.begin(), q.order.end(), rng); }
        p = 0;
    }
    playAt(p);
}

/**
 * The rules of next(true), without doing anything: none at Repeat track, Stop after this track, the end of
 * the list, a reshuffle at the end, a sleep timer ending first, and (by default) between consecutive tracks
 * of one album, which keep their gapless join.
 */
#ifdef GF_DEV
/** The dev build's join trace: each Automix join's frame times, a line in audio-trace.txt when it ends. */
void App::traceJoinFrame(float frameMs, float workMs) {
    const bool joining = !player.remote() && now.track >= 0 && mixUntil > mixShownAt && time >= mixShownAt && time < mixUntil;
    if (joining) {
        joinFrames_++;
        joinSum_ += frameMs;
        joinWorst_ = std::max(joinWorst_, double(frameMs));
        joinWorkSum_ += workMs;
        joinWorkWorst_ = std::max(joinWorkWorst_, double(workMs));
        if (frameMs > 17.5f) joinLate_++;
        if (frameMs >= 25) joinDrops_++;
        if (workMs >= joinWorkWorst_) {   // what the worst frame did: when, its sections, update's slowest step
            char why[256];
            snprintf(why, sizeof why, "at %.1f s: upd %.1f (%s %.1f) bg %.1f view %.1f side %.1f bar %.1f rest %.1f warm %.1f", time - mixShownAt,
                     profFrame_[0], updSlow_, updSlowMs_, profFrame_[1], profFrame_[2], profFrame_[3], profFrame_[4], profFrame_[5], profFrame_[6]);
            joinWhy_ = why;
        }
        return;
    }
    if (joinFrames_ == 0) return;
    char line[512];
    snprintf(line, sizeof line, "%.0f s\tjoin %.1f s\t%d frames\tframe avg %.1f worst %.1f ms\twork avg %.1f worst %.1f ms\tover 17 ms: %d\tdropped (25 ms+): %d\tworst work %s\t%s\n", time,
             mixUntil - mixShownAt, joinFrames_, joinSum_ / joinFrames_, joinWorst_, joinWorkSum_ / joinFrames_, joinWorkWorst_, joinLate_,
             joinDrops_, joinWhy_.c_str(), now.track >= 0 ? lib.tracks[now.track].path.c_str() : "");
    audioTrace += line;
    store.writeFile("audio-trace.txt", audioTrace);
    joinFrames_ = joinLate_ = joinDrops_ = 0;
    joinSum_ = joinWorst_ = joinWorkSum_ = joinWorkWorst_ = 0;
}
#endif

void App::endJoinShow(double over) {
    mixAnnounced = false;   // (announced, not joined yet: no join now; a pause gets a new show when it plays on)
    if (time >= mixUntil || mixUntil - time <= over) return;   // over, or ending as soon on its own
    // the swipes and the lap go on from where they are, (time - mixFrom) / (mixUntil - mixFrom) unchanged now, and reach
    // the end with the hint, the frost and the glide (their 0.9 s eases out from mixUntil)
    const double p = mixUntil > mixFrom && time > mixFrom ? std::min((time - mixFrom) / (mixUntil - mixFrom), 0.999) : 0.0;
    mixUntil = time + over;
    mixFrom = mixUntil - over / (1 - p);
}

/**
 * Automix (Gatefold's own player): from ten seconds into a track, each frame, the join to the next one is
 * planned from both analyses (automix.hpp; the rules of crossfadeNext still decide whether there is one),
 * and started 0.6 s before its moment: the file is opened while this one plays, and the player starts it on
 * its exact sample. A track not analysed yet gets a plain fade.
 */
void App::updateAutomix() {
    if (tog.stage != Tog::Off || player.remote() || extOn || !automixQueue() || xfadeChecked || !isPlaying() || now.track < 0) return;   // (extOn: a stream plays, see the crossfade above)
    const Level* la = levels.find(lib.tracks[now.track].path);
    double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[now.track].duration;
    if (dur <= 0 && la && la->mix.seconds > 0) dur = la->mix.seconds;   // no length in its tags: the analysis measured it
    const double pos = player.position(), left = dur - pos;
    if (dur <= 0 || pos < 10 || left <= 0.25) return;
    const int p = crossfadeNext(left);
    if (p < 0) return;   // looked at again next frame: the queue, the order or Stop after may change
    const Level* lb = levels.find(lib.tracks[q.tracks[q.order[p]]].path);
    // Auto, always (the user, 2026-10-29: "all in on Auto" for 2.0; the join editor is out of reach). Joins edited before
    // stay in mixes.tsv, unused, for a later version that edits them again
    const bool edited = false;
    const automix::Plan pl = planJoin(la ? &la->mix : nullptr, dur, lb ? &lb->mix : nullptr, store.crossfade, nullptr);
    if (pl.off || dur <= pl.length * 2) { xfadeChecked = true; return; }   // Off, or a track shorter than two fades: it plays whole
    if (pl.speed != 1 && !stretchBegun && pos >= pl.stretchFrom) {   // towards the next track's tempo, a few beats before
        stretchBegun = true;
        player.beginStretch(pl);
    }
    if (!mixPrepared && pos >= pl.startOut - 4) {   // opened and moved to its offset on a worker, ahead of the join
        mixPrepared = true;
        player.prepare(lib.tracks[q.tracks[q.order[p]]].path, pl.offset);
    }
    // Shown 0.4 s before the next track is loaded (the stage's fade-out: automixFocus): this track's title and lyrics leave
    // before the next one's take their place (they changed as they began to fade: the user, 2026-10-29, the Canvas)
    if (pos < pl.startOut - 1.0) {
        if (mixAnnounced) endJoinShow();   // sought back: not yet
        return;
    }
    if (!mixAnnounced || time >= mixUntil) {   // (again after a long pause in between: it had run out)
        mixAnnounced = true;
        mixLabel = "Automix";
        if (edited && pl.join.preset != automix::Preset::Auto) mixLabel += std::string(" · ") + automix::name(pl.join.preset);
        if (pl.beatmatched) mixLabel += " · " + std::to_string(int(std::lround(pl.bpm))) + " BPM";
        mixUntil = time + std::max(0.0, pl.startOut - pos) + pl.length;
        mixShownAt = time;
        mixFrom = time + std::max(0.0, pl.startOut - pos);
        mixShowLen = mixUntil - mixShownAt;
    }
    if (pos < pl.startOut - 0.6) return;
    // the next track still being opened and moved to its offset (the position landed in the join's last seconds at once, a
    // seek: prepare, the stage and the join came in one frame): the join waits for the worker, a frame at a time, the
    // outgoing song playing on, rather than the frame waiting for it in load() (228-640 ms on the console's trace, the
    // join's first frame, 2026-10-29); it starts as soon as it can (late: at once)
    if (player.preparing()) return;
    xfadeChecked = true;
    mixAnnounced = false;   // (loadTrack: a load other than this join's ends the stage)
    keepMixOld();
#ifdef GF_PREVIEW
    SDL_Log("automix: at %.2f s, next from %.2f s, %.2f s%s, %s: %s, %s, %s, %s, loop %d", pl.startOut, pl.offset, pl.length,
            pl.beatmatched ? " on the beat" : "", automix::name(pl.join.preset), automix::name(pl.join.volume), automix::name(pl.join.eq),
            automix::name(pl.join.filter), automix::name(pl.join.effect), pl.join.loopBars);   // the PC preview checks it with this
    if (pl.speed != 1) SDL_Log("automix: this track at %+.2f %% from %.1f s, to its end", (pl.speed - 1) * 100, pl.stretchFrom);
#endif
    playAt(p, pl.length, &pl);
}

/** The outgoing song, kept through a join (MixOld: Simple and Immersive give its title, lyrics and the rest way to the next). */
void App::keepMixOld() {
    mixOld.on = true;
    mixOld.info = now.info;
    mixOld.lyrics = now.lyrics;
    mixOld.synced = now.synced;
    mixOld.lyricsSource = now.lyricsSource;
    mixOld.upNext = upNext(15);
    mixOld.wave = wave.ready() && wave.path() == lib.tracks[now.track].path ? wave.peaks() : std::vector<float>{};
    mixOld.lyricsAt = lyricsPos();
    mixOld.at = time;
    mixOld.track = now.track;
    mixOld.posAt = player.position();
    mixOld.coverIn = false;
    mixOld.palette = target;
}

/** A join's show announced, its times from the background player's (protocol 6): as updateAutomix's, 1 s before it. */
void App::announceBgJoin(const BgState& s) {
    mixAnnounced = true;
    mixLabel = "Automix";
    if (s.joinBpm > 0) mixLabel += " · " + std::to_string(int(std::lround(s.joinBpm))) + " BPM";
    mixShownAt = time;
    mixFrom = time - s.joinAt;
    mixUntil = mixFrom + s.joinLength;
    mixShowLen = mixUntil - mixShownAt;
}

int App::crossfadeNext(double left) const {
    if (q.order.empty() || store.order == Order::RepeatOne || stopAfterCurrent) return -1;
    const double sl = sleepLeft();
    if (sl > 0 && sl <= left + store.crossfade) return -1;
    const int p = q.pos + 1;
    if (p >= int(q.order.size())) return -1;   // the end, or a new round (and a reshuffle): as before
    // an album's own joins kept; not in an Automix playlist, made to be mixed (the user, 2026-10-28: one album's songs in one
    // were never joined)
    if (store.xfadeNotInAlbum && !automixQueue()) {
        const int a = lib.tracks[now.track].albumIdx, b = lib.tracks[q.tracks[q.order[p]]].albumIdx;
        if (a >= 0 && a == b) return -1;
    }
    return p;
}

bool App::bgShown() const {
#ifdef GF_PREVIEW
    static const bool fake = getenv("GF_BGCFG") != nullptr;   // the preview has no background player: its settings, to look at
    if (fake) return true;
#endif
    return player.remote() && bgs.cfgRead;
}

uint64_t App::listHash(const std::string& name) {
    uint64_t h = 0xcbf29ce484222325ULL;   // gfbgHash (FNV-1a 64), as the player hashes the "#name" lines
    for (unsigned char c : name) { h ^= c; h *= 0x100000001b3ULL; }
    return h;
}

std::string App::listName(uint64_t hash) const {
    if (!hash) return {};
    if (hash == listHash("Liked")) return "Liked";
    for (const auto& p : store.playlists) if (listHash(p.name) == hash) return p.name;
    return {};
}

/**
 * GFBG_PLAYLISTS: Liked, then every playlist in the sidebar (yours, folder ones, smart ones as they stand now) as
 * "#name" and its tracks' paths. The background player reads the one a game event asks for; the overlay the names.
 * Protocol 6, where the readers before it look past them: first "!kinds" and a letter per playlist (GFBG_KIND_*: Liked,
 * yours, a folder's, smart, mood, Automix: the player plays an Automix one in its order, joined), then a line per playlist
 * "!list <kind letter><TAB><icon key><TAB><tracks><TAB><name>" (the overlay's Playlists pages read only these, never the
 * rest); after the last playlist a lone "#" and a "!t path<TAB>title<TAB>artist" line per track listed, so the overlay
 * names a playlist's tracks as its folders' (title by artist) without opening a file.
 */
void App::writeBgPlaylists() {
    std::string out;
    out.reserve(64 * 1024);
    out += "!kinds L";
    for (const auto& pl : store.playlists)
        out += pl.own() ? (pl.automix ? 'A' : 'N') : pl.smart() ? (isMoodPlaylist(pl) ? 'M' : 'S') : 'F';
    out += '\n';
    // each playlist's kind, icon (as the sidebar shows it), number of tracks and name, the tracks gathered first
    std::vector<std::vector<int>> smartLists(store.playlists.size());
    for (size_t i = 0; i < store.playlists.size(); i++) {
        const FolderPlaylist& pl = store.playlists[i];
        if (!pl.own()) smartLists[i] = pl.smart() ? smartTracks(pl.path) : lib.tracksUnder(lib.folderByPath(pl.path));
    }
    for (int i = -1; i < int(store.playlists.size()); i++) {   // (-1: Liked)
        const char* svg = playlistIcon(i);
        const char* key = "list";
        for (const PlaylistIcon& k : kPlaylistIcons) if (k.svg == svg) { key = k.key; break; }
        const FolderPlaylist* pl = i >= 0 ? &store.playlists[size_t(i)] : nullptr;
        const char kind = !pl ? 'L' : pl->own() ? (pl->automix ? 'A' : 'N') : pl->smart() ? (isMoodPlaylist(*pl) ? 'M' : 'S') : 'F';
        const size_t n = !pl ? store.liked.size() : pl->own() ? pl->tracks.size() : smartLists[size_t(i)].size();
        out += "!list ";
        out += kind;
        out += '\t';
        out += key;
        out += '\t';
        out += std::to_string(n);
        out += '\t';
        out += pl ? pl->name : std::string("Liked");
        out += '\n';
    }
    std::vector<int> listed;   // the tracks named in the "!t" lines
    std::vector<char> seen(lib.tracks.size(), 0);
    auto add = [&](const std::string& path, int idx) {
        out += path;
        out += '\n';
        if (idx < 0) {
            const auto it = pathIndex.find(path);
            idx = it == pathIndex.end() ? -1 : it->second;
        }
        if (idx >= 0 && idx < int(seen.size()) && !seen[size_t(idx)]) { seen[size_t(idx)] = 1; listed.push_back(idx); }
    };
    out += "#Liked\n";
    for (const auto& p : store.liked) add(p, -1);
    for (const auto& pl : store.playlists) {
        out += '#';
        out += pl.name;
        out += '\n';
        if (pl.own()) {
            for (const auto& p : pl.tracks) add(p, -1);
        } else {
            for (int i : smartLists[size_t(&pl - store.playlists.data())]) add(lib.tracks[size_t(i)].path, i);
        }
    }
    out += "#\n";
    for (int i : listed) {
        const Track& t = lib.tracks[size_t(i)];
        out += "!t ";
        out += t.path;
        out += '\t';
        out += t.title;
        out += '\t';
        out += t.artist;
        out += '\n';
    }
    const uint64_t sig = listHash(out);
    if (sig == bgListsSig) return;
    bgListsSig = sig;
    store.writeFile("bg-playlists.txt", std::move(out));
}

void App::setEq(const GfbgEq& e) {
    store.eq = e;
    gfEqClamp(&store.eq);
    player.setEq(store.eq);
    eqDirty = true;
    persistSoon(1.0f);
}

void App::prev() {
    if (srvq_.on) {
        if (player.position() > 3 || srvq_.pos <= 0) player.seek(0);
        else serverPlayAt(srvq_.pos - 1);
        return;
    }
    if (extOn) { toast("Skip on " + extFrom); return; }
    if (q.order.empty()) return;
    if (player.position() > 3) { player.seek(0); return; }   // first restart, then go back
    playAt(q.pos > 0 ? q.pos - 1 : int(q.order.size()) - 1);
}

void App::togglePlay() {
    if (srvq_.on) {
        if (player.loaded()) player.togglePause();
        else if (srvq_.waitFor.empty() && srvq_.pos >= 0) serverPlayAt(srvq_.pos);   // (nothing open: its song again, from its start)
        return;
    }
    if (extOn) { toast("Pause or play on " + extFrom); return; }
    if (!player.loaded()) {
        if (q.pos >= 0) playAt(q.pos);
        else if (!lib.tracks.empty()) playTracks(listTracks(View::Library, -1, {}), 0, "Library");
        return;
    }
    player.togglePause();
}

void App::seekBy(double d) {
    if (!player.loaded()) return;
    double p = player.position() + d;
    if (now.info.duration > 0) p = std::min(p, now.info.duration - 1);
    player.seek(std::max(0.0, p));
}

// ------------------------------------------------------------ volume levelling

namespace {

/** gfbgHash (FNV-1a 64) of a path: gains.bin's key, as the background player computes it. */
uint64_t pathHash(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
    return h;
}

/** The gain to the target, limited so the true peak stays under −1 dBTP, in dB. */
double gainTo(double lufs, double peak, int target) {
    double g = target - lufs;
    if (peak > 0) g = std::min(g, -1.0 - 20 * std::log10(peak));
    return std::clamp(g, -24.0, 12.0);
}

}  // namespace

double App::levelDb(int track, int mode) const {
    if (mode < 0) mode = store.levelMode;
    if (mode == 0 || track < 0 || track >= int(lib.tracks.size())) return 0;
    const Level* l = levels.find(lib.tracks[track].path);
    if (!l || std::isnan(l->lufs)) return 0;
    // Auto: the album's gain, but the track's while shuffling (foobar2000's "by playback order").
    const bool album = mode == 2 || (mode == 3 && store.order != Order::Shuffle);
    const int a = lib.tracks[track].albumIdx;
    if (!album || a < 0) return gainTo(l->lufs, l->peak, store.levelTarget);
    // The album: the power mean of its measured tracks, weighted by their gated blocks; its loudest peak.
    double p = 0, n = 0, peak = 0;
    for (int t : lib.albums[a].tracks) {
        const Level* m = levels.find(lib.tracks[t].path);
        if (!m || std::isnan(m->lufs) || m->blocks <= 0) continue;
        p += m->blocks * std::pow(10, (m->lufs + 0.691) / 10);
        n += m->blocks;
        peak = std::max(peak, double(m->peak));
    }
    if (n <= 0) return gainTo(l->lufs, l->peak, store.levelTarget);
    return gainTo(-0.691 + 10 * std::log10(p / n), peak, store.levelTarget);
}

/** gains.bin for the background player (gfbg.h, GFBG_GAINS): every measured track's track and album gain. */
void App::writeGains() {
    gainsDirty = false;
    gainsWrittenAt = time;
    gainsHave.clear();
    gainsOut.clear();
    gainsBuilding = true;
    gainsNext = 0;
    gainsEpoch = libEpoch;
    while (gainsBuilding) gainsStep();
}

/** A little each frame: every track's gain at once was tens of milliseconds on the console. */
void App::gainsStep() {
    if (gainsEpoch != libEpoch) {   // rescanned meanwhile: from the top
        gainsEpoch = libEpoch;
        gainsNext = 0;
        gainsOut.clear();
        gainsHave.clear();
    }
    const Uint64 t0 = SDL_GetPerformanceCounter();
    const Uint64 limit = SDL_GetPerformanceFrequency() / 1000;
    while (gainsNext < lib.tracks.size() && SDL_GetPerformanceCounter() - t0 < limit) {
        const int t = int(gainsNext++);
        const Level* l = levels.find(lib.tracks[t].path);
        if (!l || std::isnan(l->lufs)) continue;
        gainsOut.push_back({ pathHash(lib.tracks[t].path), float(levelDb(t, 1)), float(levelDb(t, 2)) });
        gainsHave.insert(lib.tracks[t].path);
    }
    if (gainsNext < lib.tracks.size()) return;
    gainsBuilding = false;
    std::vector<GainEntry>& out = gainsOut;
    std::sort(out.begin(), out.end(), [](const GainEntry& a, const GainEntry& b) { return a.hash < b.hash; });
    // A new generation each time, also across restarts: the player reloads when it differs.
    gainsGen = std::max(gainsGen + 1, unsigned(std::time(nullptr)));
    const uint32_t header[4] = { 0x564C4647u, uint32_t(out.size()), gainsGen, 0 };   // GfbgGainsHeader, "GFLV"
    std::string data(reinterpret_cast<const char*>(header), sizeof header);
    data.append(reinterpret_cast<const char*>(out.data()), out.size() * sizeof(GainEntry));
    store.writeFile("gains.bin", std::move(data));
}

/** Every frame: what to measure next, the gains file, and the gain here. */
void App::updateLevels() {
    const bool changed = levels.poll();
    const std::vector<std::string> amWaiting = automixWaitingTracks(32);   // an Automix playlist's tracks not analysed yet
    // Mood playlists: every track's mood, so the whole library is analysed (in the same decode as the rest: levels.hpp)
    bool moods = false;
    for (const auto& p : store.playlists) if (isMoodPlaylist(p)) { moods = true; break; }
    levels.wantMood(moods);
    if (store.levelMode == 0 && !automixQueue() && amWaiting.empty() && !moods) {   // the worker measures for levelling and analyses Automix and Mood playlists
        if (!levelFirst.empty() || !levelAll.empty()) {
            levelFirst.clear();
            levelAll.clear();
            levelEpoch = ~0u;
            levels.want({}, {});
        }
    } else {
        // First the track playing and the next three, then the whole library in its order.
        std::vector<std::string> first;
        if (now.track >= 0) first.push_back(lib.tracks[now.track].path);
        for (int t : upNext(3)) first.push_back(lib.tracks[t].path);
        first.insert(first.end(), amWaiting.begin(), amWaiting.end());
        const bool libChanged = levelEpoch != libEpoch && !lib.tracks.empty();
        if (libChanged) {
            levelEpoch = libEpoch;
            levelAll.clear();
            levelAll.reserve(lib.tracks.size());
            for (const Track& t : lib.tracks) levelAll.push_back(t.path);
        }
        // levelling off: only what Automix needs (the queue's next tracks, Automix playlists' tracks), not the whole library
        // (it was decoded track after track while an Automix playlist played: the SD card and core 1 busy under the audio)
        const bool library = store.levelMode != 0 || moods;
        if (libChanged || first != levelFirst || library != levelAllSent) {
            levelFirst = first;
            levelAllSent = library;
            levels.want(first, library ? levelAll : std::vector<std::string>{});
        }
    }
    // The gains: new measurements or a new target. Soon when a track asked for first has its value now
    // (it is about to play, or plays), else at most every 20 s while the library is measured.
    if (changed || store.levelTarget != levelTargetUsed) { levelTargetUsed = store.levelTarget; gainsDirty = true; }
    if (gainsDirty && levels.loaded()) {
        bool soon = false;
        for (const auto& p : levelFirst)
            if (levels.find(p) && !gainsHave.count(p)) soon = true;
        if (!gainsBuilding && ((soon && time - gainsWrittenAt > 1) || time - gainsWrittenAt > 20)) {
            gainsBuilding = true;
            gainsNext = 0;
            gainsEpoch = libEpoch;
            gainsDirty = false;
            gainsWrittenAt = time;
            gainsHave.clear();
            gainsOut.clear();
            gainsOut.reserve(lib.tracks.size());
        }
    }
    if (gainsBuilding) gainsStep();
    // the files, a little each frame: a whole library at once held the console's frame for 167 ms
    if (levels.saving()) {
        std::string loud, mix, mood;
        if (levels.saveStep(1.0, loud, mix, mood)) {
            store.writeFile("loudness.tsv", std::move(loud));
            store.writeFile("mix.tsv", std::move(mix));
            store.writeFile("mood.tsv", std::move(mood));
        }
    } else if (levels.dirty() && time - levelsSavedAt > 30) {
        levelsSavedAt = time;
        levels.saveBegin();
    }
    if (player.remote()) {
        if (store.levelMode != levelModeSent) { bg.setLevelMode(store.levelMode); levelModeSent = store.levelMode; }
    } else {
        player.setLevel(float(std::pow(10, levelDb(now.track) / 20)));   // eased: a value measured mid-track comes in gently
    }
}

// ------------------------------------------------------------ the sleep timer

double App::sleepLeft() const {
    if (player.remote()) return bg.state().sleepLeft;
    return sleepUntil > 0 ? std::max(0.0, sleepUntil - time) : 0;
}

/** seconds 0: off. choice: what Settings' control shows (0 off, 1… the minutes in order). */
void App::setSleep(double seconds, int choice) {
    sleepChoice = seconds > 0 ? choice : 0;
    if (sleepFading) player.setVolume(volumeShown);   // turned off or moved during the fade: back up
    sleepFading = false;
    sleepUntil = seconds > 0 ? time + seconds : 0;
    if (player.remote()) {
        bg.setSleep(float(seconds));
        sleepUntil = 0;   // the background player keeps it: it carries on with Gatefold closed
    }
}

/** Every frame. Gatefold's own timer fades the volume over the last 10 s and pauses; the background player's is only watched. */
void App::updateSleep() {
    if (player.remote()) {
        const BgState& s = bg.state();
        // Seen as it happened when the timer was seen running a moment ago (the player reports 0 left for up
        // to a quarter second before it bumps sleepGen, so not "last frame"): an end that came while Gatefold
        // was in the background is not followed by the sleep card. A steady clock: App::time stops while suspended.
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (s.sleepLeft > 0) sleepSeenAt = wall;
        if (s.sleepGen != sleepGenSeen) { sleepGenSeen = s.sleepGen; sleepEnded(sleepSeenAt > 0 && wall - sleepSeenAt < 5); }
    } else if (sleepUntil > 0) {
        const double left = sleepUntil - time;
        if (left <= 0) {
            sleepUntil = 0;
            if (isPlaying()) player.togglePause();
            if (sleepFading) player.setVolume(volumeShown);
            sleepFading = false;
            sleepEnded(true);
        } else if (left <= 10 && isPlaying()) {
            sleepFading = true;
            player.setVolume(volumeShown * float(left / 10));
        }
    }
    // Settings → Playback → Keep the console awake: no dimming or auto-sleep while Gatefold is open, until
    // a sleep timer ends; then the console may sleep by itself again, until music plays once more.
    if (isPlaying()) sleepReleased = false;
    const bool want = (store.keepAwake && !sleepReleased) || pocketLocked_;   // (locked: awake, the screen off by Songate)
    if (want != awakeHeld) {
        awakeHeld = want;
        plat::setKeepAwake(want);
    }
    // "Media playing" keeps the screen from dimming and the console from sleeping: while music plays, and all the time
    // while Keep awake holds (Gatefold's own dimming, below, then decides). Set from here alone, once per change.
    if (const int media = (isPlaying() || want || chirpsDue()) ? 1 : 0; media != mediaState) {   // (the Switch: its own, the chirps too)
        mediaState = media;
        plat::setMediaPlaying(media != 0);
    }
    // ... and Dim the screen, after store.dimAfter minutes without input (only then: else the system dims).
    const bool dim = want && store.dimAfter > 0 && idleFor >= store.dimAfter * 60.0 && sleepPromptAt == 0;
    if (dim != (dimmed != 0)) setDimmed(dim);
    veilA += ((dimmed == 2 ? 0.78f : 0.f) - veilA) * (1 - std::exp(-dt / 0.35f));

    // The sleep card's count: at 0 the console goes to sleep.
    if (sleepPromptAt > 0 && time - sleepPromptAt >= store.sleepConsole) {
        closeSleepPrompt();
        plat::requestSleep();
    }
}

void App::sleepEnded(bool seen) {
    sleepChoice = 0;
    sleepReleased = true;
    // Then the console sleeps, after a card that lets you stay awake; only Gatefold in front can ask for that.
    if (seen && store.sleepConsole > 0 && plat::inFocus()) {
        if (sheet.page != Sheet::Closed) closeSheet();
        sleepPromptAt = time;
        idleFor = 0;
        if (dimmed) setDimmed(false);
        ui.setPadMode(true);
        return;
    }
    toast("Sleep timer: the music is paused");
}

void App::closeSleepPrompt() {
    sleepPromptAt = 0;
    ui.focusDefault();
}

/** Handheld: the backlight goes down, and back up to what it was; docked: a veil (veilA), as the TV is not ours. */
void App::setDimmed(bool on) {
    if (on && !dimmed) {
        dimmed = plat::dimBacklight(dimSaved) ? 1 : 2;
    } else if (!on && dimmed) {
        if (dimmed == 1) plat::restoreBacklight(dimSaved);
        dimmed = 0;
    }
}

/** Every 10 s: the charge, and from how fast it moved over the last 10 minutes, how long to full or to empty.
 *  The raw charge has decimals, so a minute of samples already gives a rate; it steadies as more come. */
void App::sampleBattery() {
    if (time - battery.lastAt < 10) return;
    battery.lastAt = time;
    double pct = 0;
    bool plugged = false;
    if (!plat::batteryRaw(pct, plugged)) return;
    battery.infoOk = plat::batteryInfo(battery.info);
    {   // the power service's words after the battery's age, once a run, while charging (the charger's layout: platform_switch.cpp)
        static bool logged = false;
        if (!logged && battery.infoOk && battery.info.charger && !battery.info.raw.empty()) {
            logged = true;
            logLine("Battery fields from 0x28: " + battery.info.raw);
        }
    }
    {
        unsigned shown = 0;
        battery.pct = plat::batteryLevel(shown) ? double(shown) : pct;
    }
    battery.rate = 0;
    const int charging = plugged;
    const long long wall = (long long)std::time(nullptr);
    // A charger in or out, or a gap (the app was suspended, its clock stopped): the old samples no longer tell the rate.
    if (charging != battery.charging || (battery.lastWall && wall - battery.lastWall > 120)) battery.samples.clear();
    battery.charging = charging;
    battery.lastWall = wall;
    auto& sm = battery.samples;
    sm.push_back({ time, pct });
    while (sm.size() > 2 && time - sm.front().first > 10 * 60) sm.erase(sm.begin());
    battery.est.clear();
    if (charging && pct >= 99.5) { battery.est = "charged"; return; }
    auto timeText = [](int mins) {
        char b[32];
        if (mins >= 60) snprintf(b, sizeof b, "%d h %02d", mins / 60, mins % 60);
        else snprintf(b, sizeof b, "%d min", std::max(1, mins));
        return std::string(b);
    };
    // the fuel gauge's own time (the Switch): from the first sample, no minute of watching first
    bool fromGauge = false;
    if (battery.infoOk && battery.info.gauge) {
        const int m = charging ? battery.info.ttfMin : battery.info.tteMin;
        if (m > 0 && m < 24 * 60) {
            battery.est = charging ? "full in " + timeText(m) : timeText(m);
            fromGauge = true;
        }
    }
    if (sm.size() < 6 || time - sm.front().first < 60) return;   // too soon to tell
    // The rate: a line fitted through the samples (per second).
    double mt = 0, mp = 0;
    for (const auto& [t, p] : sm) { mt += t; mp += p; }
    mt /= double(sm.size());
    mp /= double(sm.size());
    double num = 0, den = 0;
    for (const auto& [t, p] : sm) { num += (t - mt) * (p - mp); den += (t - mt) * (t - mt); }
    if (den <= 0) return;
    const double rate = num / den;
    battery.rate = rate * 60;
    if (charging ? rate <= 1e-5 : rate >= -1e-5) return;   // not moving the way it should: no guess
    const double secs = charging ? (100 - pct) / rate : pct / -rate;
    if (secs > 24 * 3600) return;
    const int mins = std::max(5, int(std::lround(secs / 60 / 5)) * 5);
    if (!fromGauge) battery.est = charging ? "full in " + timeText(mins) : timeText(mins);
}

/**
 * Automix playlists play in the order Gatefold worked out for their joins (the user, 2026-10-29: "shuffle should not exist
 * on Automix playlists"): queued with Shuffle on, Shuffle is set aside (store.heldOrder, kept over a restart) and the order
 * is the playlist's; the next queue that is not one brings Shuffle back. While one plays, Shuffle is not offered (the
 * player bar's button passes it by, Settings greys it, the page has no Shuffle button).
 */
void App::holdOrderForAutomix(bool rebuild) {
    // rebuild: a queue already in place (restored at launch, or the background player's) has its order made again (setOrder);
    // playTracks only sets the mode, before it makes the new queue's order
    if (automixQueue()) {
        if (store.order == Order::Shuffle) {
            store.heldOrder = int(Order::Shuffle);
            if (rebuild) setOrder(Order::Default);
            else store.order = Order::Default;
            persistSoon(0.5f);
        }
    } else if (store.heldOrder >= 0) {
        const Order back = Order(store.heldOrder);
        store.heldOrder = -1;
        if (rebuild) setOrder(back);
        else store.order = back;
        persistSoon(0.5f);
    }
}

void App::cycleOrder() {
    static const char* names[] = { "Default order", "Repeat playlist", "Repeat song", "Shuffle" };
    if (togetherOn() && !tog.host) {   // the starter's order decides what comes next on both: asked of it, shown when it says so
        if (togetherLocked() || tog.linkLost) return;
        together::Message m;
        m.kind = together::Kind::Ask;
        m.what = together::AskOrder;
        m.song = (tog.order + 1) % 4;
        togetherSend(m);
        toast(std::string(names[m.song]) + ": asked of " + tog.peerName);
        return;
    }
    Order next = Order((int(store.order) + 1) % 4);
    if (next == Order::Shuffle && automixQueue()) next = Order::Default;   // an Automix playlist: no Shuffle (holdOrderForAutomix)
    setOrder(next);   // (it saves)
    toast(names[int(store.order)]);
}

void App::setOrder(Order o) {
    const int cur = currentQueuePos();
    grabDrop();
    store.order = o;
    persistSoon(0.5f);
    if (q.order.empty()) return;
    for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
    if (o == Order::Shuffle) {
        std::shuffle(q.order.begin(), q.order.end(), rng);
        if (cur >= 0) std::iter_swap(q.order.begin(), std::find(q.order.begin(), q.order.end(), cur));
        q.pos = 0;
    } else {
        q.pos = std::max(0, cur);
    }
}

std::vector<int> App::upNext(int n) const {
    std::vector<int> out;
    if (q.order.empty() || q.pos < 0) return out;
    const bool wrap = store.order == Order::RepeatAll || store.order == Order::Shuffle;
    for (int i = 1; i <= n && i < int(q.order.size()); i++) {
        int p = q.pos + i;
        if (p >= int(q.order.size())) {
            if (!wrap) break;
            p %= int(q.order.size());
        }
        out.push_back(q.tracks[q.order[p]]);
    }
    return out;
}

// ------------------------------------------------------------ editing the queue

/** The queue as it will play: what the Playlist view shows, so it agrees with Up next. */
std::vector<int> App::queueInPlayOrder() const {
    std::vector<int> out;
    out.reserve(q.order.size());
    for (int o : q.order) out.push_back(q.tracks[o]);
    return out;
}

/** Without Shuffle the list is rewritten in play order and the order becomes 0, 1, 2…: setOrder
 *  resets the order to the list's, which would otherwise undo a move or a "Play next". */
void App::queueNormalize() {
    if (store.order == Order::Shuffle) return;
    q.tracks = queueInPlayOrder();
    for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
}

void App::queueEdited() {
    queueNormalize();
    persistSoon(0.5f);
    // bgSyncQueue sends it next frame with the new pos; until the player's state shows that, its
    // old pos must not be taken for a track change (bgUpdate).
    if (player.remote()) bgs.awaitUntil = std::max(bgs.awaitUntil, time + 1.0);
}

void App::queueAdd(int track, bool next) {
    if (q.order.empty()) { playTracks({ track }, 0, "Queue"); return; }
    q.tracks.push_back(track);
    const int at = next ? std::min(q.pos + 1, int(q.order.size())) : int(q.order.size());
    q.order.insert(q.order.begin() + at, int(q.tracks.size()) - 1);
    queueEdited();
}

void App::queueRemove(int pos) {
    if (pos < 0 || pos >= int(q.order.size()) || pos == q.pos) return;   // the track playing stays
    const int idx = q.order[pos];
    q.order.erase(q.order.begin() + pos);
    q.tracks.erase(q.tracks.begin() + idx);
    for (int& o : q.order) if (o > idx) o--;
    if (pos < q.pos) q.pos--;
    queueEdited();
}

/** Moves the row at play position from so that it ends up at to; the playing track keeps playing. */
void App::queueMove(int from, int to) {
    const int n = int(q.order.size());
    if (from < 0 || from >= n) return;
    to = std::clamp(to, 0, n - 1);
    if (from == to) return;
    const int v = q.order[from];
    q.order.erase(q.order.begin() + from);
    q.order.insert(q.order.begin() + to, v);
    if (q.pos == from) q.pos = to;
    else if (from < q.pos && to >= q.pos) q.pos--;
    else if (from > q.pos && to <= q.pos) q.pos++;
    queueEdited();
}

/** "Play next" on a row already in the queue moves it rather than adding a copy. */
void App::queuePlayNext(int pos) {
    if (pos == q.pos) return;
    queueMove(pos, pos < q.pos ? q.pos : q.pos + 1);
}

/** Clear empties the queue, the track playing too: the music stops, as with the overlay's Remove All (the user, 2026-10-27;
 *  1.1.0 kept the track playing). */
void App::queueClear() {
    grabDrop();
    q.tracks.clear();
    q.order.clear();
    q.pos = -1;
    q.name.clear();
    q.failed = 0;
    stopAfterCurrent = false;
    if (player.remote()) {
        // An empty queue stops the background player; sent here, as bgSyncQueue never sends an empty one
        BgQueue none;
        none.mode = int(store.order);
        bg.setQueue(none);
        bgs.tracks.clear();
        bgs.order.clear();
        bgs.name.clear();
        bgs.mode = none.mode;
    } else {
        player.stop();
    }
    forgetNowPlaying();
    persistSoon(0.5f);
}

void App::grabStep(int d) {
    if (grab.kind == Grab::SrvList) {   // a track of a server playlist: it swaps places with its neighbour (the order sent when put down)
        SrvPage& pg = srvPage_[0];
        if (pg.key != "P:" + srvGrabPl_) { grabDrop(); return; }
        const int to = std::clamp(grab.pos + d, 0, int(pg.songs.size()) - 1);
        if (to == grab.pos || grab.pos >= int(pg.songs.size())) return;
        std::swap(pg.songs[size_t(grab.pos)], pg.songs[size_t(to)]);
        grab.pos = to;
        ui.focus(uid("srv.plt.r", to));
        ui.setPadMode(true);
        return;
    }
    if (grab.kind == Grab::List) {   // a track of one of your own playlists: it swaps places with its neighbour
        if (grab.ref < 0 || grab.ref >= int(store.playlists.size()) || !store.playlists[size_t(grab.ref)].own()) { grabDrop(); return; }
        FolderPlaylist& p = store.playlists[size_t(grab.ref)];
        const std::vector<int> rows = playlistTracks(grab.ref);
        const int to = std::clamp(grab.pos + d, 0, int(rows.size()) - 1);
        if (to == grab.pos || grab.pos >= int(rows.size())) return;
        auto at = [&](int row) { return std::find(p.tracks.begin(), p.tracks.end(), lib.tracks[size_t(rows[size_t(row)])].path); };
        const auto a = at(grab.pos), b = at(to);
        if (a != p.tracks.end() && b != p.tracks.end()) std::iter_swap(a, b);
        if (p.automix) p.manual = true;   // moved by hand: it keeps this order until Order again
        grabMoved_ = true;   // saved when it is put down (grabDrop): held, the D-pad's repeat wrote the card 13 times a second
        grab.pos = to;
        ui.focus(uid("pl.r", to));
        ui.setPadMode(true);
        return;
    }
    if (grab.kind == Grab::Playlists) {   // the Playlists page: the playlist itself moves (its rows keep their ids)
        const int to = std::clamp(grab.pos + d, 0, int(store.playlists.size()) - 1);
        if (to != grab.pos) { movePlaylist(grab.pos, to); grab.pos = to; }
        ui.setPadMode(true);
        return;
    }
    if (grab.kind == Grab::SrvPlaylists) {   // … the server's: Songate's own order of them (moveServerPlaylist)
        const int to = std::clamp(grab.pos + d, 0, int(srvPlaylists_.size()) - 1);
        if (to != grab.pos) { moveServerPlaylist(grab.pos, to); grab.pos = to; }
        ui.setPadMode(true);
        return;
    }
    if (grab.kind == Grab::Battery) {     // Settings → Appearance → Battery: what shows beside it
        std::vector<int> vis = batteryItems();
        const int to = std::clamp(grab.pos + d, 0, int(vis.size()) - 1);
        if (to != grab.pos && grab.pos < int(vis.size())) {
            std::swap(vis[size_t(grab.pos)], vis[size_t(to)]);
            setBatteryItems(vis);
            grab.pos = to;
            persistSoon(0.3f);
        }
        ui.setPadMode(true);
        return;
    }
    if (grab.kind == Grab::Tabs) {        // Settings → Appearance → Sidebar: a tab, one row along the list as SHOWN (a tab with
        // no row is stepped over, so one press is one visible step: the user, 2026-11-09)
        const std::vector<int> shown = sidebarTabsShown();
        const auto at = std::find(shown.begin(), shown.end(), grab.pos);
        const int n = int(store.tabOrder.size());
        if (at == shown.end()) {   // (not listed: the plain step)
            const int to = std::clamp(grab.pos + d, 0, n - 1);
            if (to != grab.pos) { std::swap(store.tabOrder[size_t(grab.pos)], store.tabOrder[size_t(to)]); grab.pos = to; persistSoon(0.3f); }
        } else {
            const int k = int(at - shown.begin()), k2 = std::clamp(k + d, 0, int(shown.size()) - 1);
            if (k2 != k) {
                // the held tab leaves its place and lands just past the neighbour shown (after it going down, before it going up)
                const int from = grab.pos, target = shown[size_t(k2)];
                const int tab = store.tabOrder[size_t(from)];
                store.tabOrder.erase(store.tabOrder.begin() + from);
                int to = target - (from < target ? 1 : 0);
                if (d > 0) to += 1;
                to = std::clamp(to, 0, int(store.tabOrder.size()));
                store.tabOrder.insert(store.tabOrder.begin() + to, tab);
                grab.pos = to;
                persistSoon(0.3f);
            }
        }
        ui.setPadMode(true);
        return;
    }
    const int n = int(q.order.size());
    if (grab.pos < 0 || grab.pos >= n) { grabDrop(); return; }
    // In Up next the row stays after the track playing (what lies before it has been played),
    // and within the 15 rows Up next shows.
    const int lo = grab.upNext ? q.pos + 1 : 0, hi = grab.upNext ? std::min(n - 1, q.pos + 15) : n - 1;
    const int to = std::clamp(grab.pos + d, lo, hi);
    if (to == grab.pos) return;
    queueMove(grab.pos, to);
    grab.pos = to;
    ui.focus(grab.upNext ? uid("np.next", to - q.pos - 1) : uid("pl.r", to));
    ui.setPadMode(true);
}

void App::grabDrop() {
    if (grab.kind == Grab::SrvList && grab.pos >= 0) {   // a server playlist's new order, to the server
        const SrvPage& pg = srvPage_[0];
        if (pg.key == "P:" + srvGrabPl_) {
            std::vector<std::string> ids;
            for (const auto& so : pg.songs) ids.push_back(so.id);
            subsonic.sendPlaylist(pg.name, srvGrabPl_, ids);
        }
        srvGrabPl_.clear();
    }
    if (grabMoved_) { grabMoved_ = false; persistSoon(0.3f); }   // a playlist's song put down in its new place
    grab = Grab{};
}

// ============================================================ track loading

/**
 * Shows trackIdx as the one playing; start: also start it (false when the background player already did).
 * The file's tags, format, lyrics and cover are read by the loader (applyLoaded), so the frame of a track
 * change is not spent on the SD card. Until they arrive, a moment later, the previous track's text and
 * lyrics stay as they were (frozen where they were), and then everything changes at once: showing the
 * library's tags first made the lyrics read "No lyrics" and the Canvas text jump for a frame or two.
 */
void App::loadTrack(int trackIdx, bool paused, bool start, double fade, const automix::Plan* mix) {
    // a song chosen: Library's switch waiting for the song playing to end is not wanted any more (the user, 2026-11-07: a song of yours
    // chosen after it paused, and played from the last one's time)
    libSwitchTo_ = -1;
    if (srvq_.on) serverLeave();   // a song of the library: the server's queue ends
    const Track& t = lib.tracks[trackIdx];
    // A crossfade is set: a track changed by hand fades quickly instead of cutting (Player: only while one plays).
    if (fade <= 0 && start && !paused && (store.crossfade > 0 || automixQueue()) && !player.remote() && isPlaying()) fade = 0.3;
    xfadeChecked = false;
    stretchBegun = false;
    mixPrepared = false;
    // a load that is not the join (a skip, before or through one): its show ends cleanly (it stayed on screen: the user,
    // 2026-10-29), and nothing is left to give way from
    if (!mix) endJoinShow();
    if (!mix && mixOld.on) mixOld = MixOld{};
    lyricsHold = player.loaded() ? player.position() : 0;
    const bool shown = now.track >= 0 && !now.info.title.empty();
    now.track = trackIdx;
    if (!shown) {   // nothing to keep: what the library knows, until the file's own
        now.info = TrackInfo{};
        now.info.title = t.title;
        now.info.artist = t.artist;
        now.info.album = t.album;
        now.info.albumArtist = t.albumArtist;
        now.info.genre = t.genre;
        now.info.date = t.date;
        now.info.trackNo = t.trackNo;
        now.info.duration = t.duration;
        now.lyrics.clear();
        now.synced = false;
    }
    now.loading = true;
    statHeard = 0;   // a new play of it, also when Repeat track plays it again
    statCounted = false;
    lfmHeard = 0;
    lfmDone = lfmSaid = false;
    const float gain = float(std::pow(10, levelDb(trackIdx) / 20));
    TrackLoader::Request r;
    r.gen = ++loadGen;
    r.path = t.path;
    r.folder = t.folder;
    if (auto it = store.covers.find(t.path); it != store.covers.end()) r.myCover = it->second;
    r.opts = paletteOpts();
    loader.request(std::move(r));

    wave.request(t.path);
    meta.wantTags(trackIdx);
    // levelling from its first sample; set after load(), which keeps the outgoing track's gain for its fade
    if (!start) { player.setLevel(gain, true); return; }
    if (player.remote() && bgSyncQueue(false)) {
        player.setLevel(gain, true);
        bg.playAt(q.pos, paused, 0);
        bgs.awaitUntil = time + 1.5;
        return;
    }
    UPD_MARK("loadTrack");   // (the dev trace: the join's frame split, the player's load apart)
    player.load(t.path, paused, fade, mix);
    player.setLevel(gain, true);
    UPD_MARK("player.load");
}

// ============================================================ the background player

void App::bgConnect() {
    if (!bg.connect()) return;
    player.setRemote(&bg);
    thumbs.start(store.path("thumbs"));   // GFBG_THUMBS
    bg.poll(time);
    const BgState& s = bg.state();
    bgs = BgSync{};
    bgs.trackGen = s.trackGen;
    bgs.queueGen = s.queueGen;
    bgs.errorGen = s.errorGen;
    bgs.seenStopAfter = bgs.stopAfter = stopAfterCurrent = s.stopAfter;
    // The overlay may have changed the volume since Gatefold last ran: the player's is the one heard.
    bgs.volume = s.volume;
    volumeShown = store.volume = s.volume;
    sleepGenSeen = s.sleepGen;   // a timer that ran out before Gatefold opened is not news
    levelModeSent = -1;          // tell it the levelling mode again
    bgs.cfgRead = bg.config(bgs.cfg);
    // The equalizer: the player's copy is the one heard (the overlay may have changed it meanwhile).
    bgs.eqGen = s.eqGen;
    bgs.joinGen = s.joinGen;   // a join already under way is not shown from its middle
    if (GfbgEq e; bg.eq(e)) { store.eq = e; player.setEq(e); }
    eqDirty = false;
}

/** Takes the background player's queue as Gatefold's own (at launch, and after the overlay reshuffled it). */
bool App::bgAdoptQueue() {
    BgQueue bq;
    if (!bg.getQueue(bq)) return false;
    if (bq.paths.empty()) {
        // Emptied in the overlay (Remove All): Gatefold's queue goes too. An empty queue from anyone else (a player just
        // started with nothing) is not news: then Gatefold keeps, or at launch restores, its own. Before, Gatefold kept
        // its queue either way, and at its next launch restored it and sent it back to the player.
        if (bg.state().queueBy != BgClient::ByOverlay) return false;
        grabDrop();
        q.tracks.clear();
        q.order.clear();
        q.pos = -1;
        q.name.clear();
        q.failed = 0;
        bgs.tracks.clear();
        bgs.order.clear();
        bgs.name.clear();
        bgs.mode = int(store.order);
        forgetNowPlaying();
        persistSoon(0.5f);   // the session too: an empty one restores nothing
        return true;
    }
    std::vector<int> keep, tracks;   // its index → ours, −1 for files this library does not have
    for (const auto& p : bq.paths) {
        auto it = pathIndex.find(p);
        keep.push_back(it == pathIndex.end() ? -1 : int(tracks.size()));
        if (it != pathIndex.end()) tracks.push_back(it->second);
    }
    if (tracks.empty()) return false;
    grabDrop();
    q.tracks = tracks;
    q.name = bq.name;
    q.failed = 0;
    q.order.clear();
    for (int o : bq.order) if (o >= 0 && o < int(keep.size()) && keep[o] >= 0) q.order.push_back(keep[o]);
    if (q.order.size() != q.tracks.size()) {
        q.order.resize(q.tracks.size());
        for (int i = 0; i < int(q.order.size()); i++) q.order[i] = i;
    }
    const int cur = bq.pos >= 0 && bq.pos < int(bq.order.size()) && bq.order[bq.pos] >= 0 && bq.order[bq.pos] < int(keep.size())
                        ? keep[bq.order[bq.pos]] : -1;
    q.pos = 0;
    for (int i = 0; i < int(q.order.size()); i++) if (q.order[i] == cur) q.pos = i;
    queueNormalize();   // an older queue may hold a "Play next" outside the list's order
    store.order = Order(std::clamp(bq.mode, 0, 3));
    holdOrderForAutomix(true);   // (the background player's queue: Automix playlists not shuffled)
    // Remembered as sent, so it is not sent straight back; unless files were dropped, which it should hear about.
    bgs.tracks = q.tracks.size() == bq.paths.size() ? q.tracks : std::vector<int>{};
    bgs.order = q.order;
    bgs.mode = bq.mode;
    bgs.name = q.name;
    for (int t : q.tracks) thumbFor(t, false);
    const int t = cur >= 0 ? q.tracks[q.order[q.pos]] : -1;
    if (t >= 0 && t != now.track) loadTrack(t, true, false);
    savedTrack = now.track;
    return true;
}

/** Sends Gatefold's queue when it changed here: a new list, a reshuffle, an order change, a track queued. False when the
 *  background player could not take it (too big for its memory): Gatefold plays by itself from then on (reload: and
 *  carries on with the track playing, where it was). */
bool App::bgSyncQueue(bool reload) {
    // An empty queue here (no music folder yet, a library that lacks its files) never clears the player's.
    if (!player.remote() || q.tracks.empty()) return player.remote();
    if (q.tracks == bgs.tracks && q.order == bgs.order && int(store.order) == bgs.mode && q.name == bgs.name && automixQueue() == bgs.automix)
        return true;
    BgQueue bq;
    bq.name = q.name;
    bq.automix = automixQueue();   // joined by Automix there too (protocol 6), in its own order
    bq.paths.reserve(q.tracks.size());
    for (int t : q.tracks) bq.paths.push_back(lib.tracks[t].path);
    bq.order = q.order;
    bq.pos = q.pos;
    bq.mode = int(store.order);
    if (!bg.setQueue(bq)) {
        if (!bg.queueRefused()) return false;
        const bool had = now.track >= 0 && q.pos >= 0, playing = isPlaying();
        const double at = player.loaded() ? player.position() : 0;
        bg.stop();
        bg.disconnect();
        player.setRemote(nullptr);
        toast("This queue is too big for the background player: Songate plays it itself");
        if (reload && had) {
            loadTrack(q.tracks[q.order[q.pos]], !playing);
            if (at > 1) player.seek(at);
        }
        return false;
    }
    for (int t : q.tracks) thumbFor(t, false);
    bgs.tracks = q.tracks;
    bgs.order = q.order;
    bgs.mode = bq.mode;
    bgs.name = q.name;
    bgs.automix = bq.automix;
    return true;
}

/** Every frame while connected: what the player did on its own (next track, the overlay's buttons), and ours to it. */
void App::bgUpdate() {
    if (!player.remote()) return;
    if (!bg.poll(time)) {
        // as where the queue is too big for it: the song it held is loaded here, at its place
        const bool had = now.track >= 0 && q.pos >= 0 && q.pos < int(q.order.size()), playing = isPlaying();
        const double at = player.loaded() ? player.position() : 0;
        player.setRemote(nullptr);
        toast("The background player stopped, so Songate plays on its own");
        if (had) {
            loadTrack(q.tracks[q.order[q.pos]], !playing);
            if (at > 1) player.seek(at);
        }
        return;
    }
    if (!bgSyncQueue()) return;   // too big for it: Gatefold plays by itself now
    const BgState& s = bg.state();
    if (s.queueGen != bgs.queueGen) {
        bgs.queueGen = s.queueGen;
        if (s.queueBy != BgClient::ByGatefold) bgAdoptQueue();
    }
    if (s.trackGen != bgs.trackGen) { bgs.trackGen = s.trackGen; bgs.awaitUntil = 0; }
    // Automix's joins in the player (protocol 6), shown as Gatefold's own (updateAutomix): announced a second before, the
    // outgoing song kept as the next one loads, on the player's clock
    automix::Plan joined;
    bool join = false;
    if (automixQueue() && now.track >= 0) {
        if (s.joining == 2 && s.joinGen != bgs.joinGen) {
            if (!mixAnnounced || time >= mixUntil) announceBgJoin(s);
            mixAnnounced = false;
            keepMixOld();
            join = true;
            joined.length = s.joinLength;
            joined.bpm = s.joinBpm;
            joined.beatmatched = (s.joinFlags & 1) != 0;
            joined.drop = (s.joinFlags & 2) != 0;
        } else if (s.joining == 1 && s.joinAt >= -1.0 && isPlaying()) {
            if (!mixAnnounced || time >= mixUntil) announceBgJoin(s);
        } else if (mixAnnounced && s.joining != 2) {
            endJoinShow();   // sought back before it, or it is off now
        }
    }
    bgs.joinGen = s.joinGen;
    if (time >= bgs.awaitUntil && s.pos >= 0 && s.pos < int(q.order.size()) && s.pos != q.pos) {
        q.pos = s.pos;
        loadTrack(q.tracks[q.order[q.pos]], false, false, 0, join ? &joined : nullptr);
    }
    if (s.errorGen != bgs.errorGen) {
        bgs.errorGen = s.errorGen;
        toast("A song could not be played, so it was skipped");
    }
    // Stop after current: whichever side changed it last wins (the player clears it when it stops).
    if (s.stopAfter != bgs.seenStopAfter) { bgs.seenStopAfter = bgs.stopAfter = stopAfterCurrent = s.stopAfter; }
    else if (stopAfterCurrent != bgs.stopAfter) { bg.setStopAfter(stopAfterCurrent); bgs.stopAfter = stopAfterCurrent; }
    if (std::fabs(s.volume - bgs.volume) > 0.0005f) {
        bgs.volume = s.volume;
        if (std::fabs(s.volume - volumeShown) > 0.0005f) volumeShown = s.volume;   // the overlay moved it
    }
    // Automix's model, for an Automix playlist's joins in the player (protocol 6)
    if (const std::string file = mixFile() ? mixFile()->file : std::string(); int(mixModel()) != bgs.modelSent || file != bgs.modelFileSent) {
        bg.setAutomixModel(int(mixModel()), file);   // a downloaded model: the player reads the same file
        bgs.modelSent = int(mixModel());
        bgs.modelFileSent = file;
    }
    // Crossfade's settings, whenever they differ from what it was told.
    if (store.crossfade != bgs.xfadeSent || int(store.xfadeNotInAlbum) != bgs.xfadeFlagSent) {
        bg.setCrossfade(store.crossfade, store.xfadeNotInAlbum);
        bgs.xfadeSent = store.crossfade;
        bgs.xfadeFlagSent = store.xfadeNotInAlbum;
    }
    // The equalizer: ours when it changed here (a slider: at most every 0.1 s), else the player's when the overlay changed it.
    if (eqDirty && time - eqSentAt >= 0.1) {
        bg.setEq(store.eq);
        eqDirty = false;
        eqSentAt = time;
    } else if (!eqDirty && s.eqGen != bgs.eqGen) {
        bgs.eqGen = s.eqGen;
        if (GfbgEq e; bg.eq(e) && memcmp(&e, &store.eq, sizeof e) != 0) { store.eq = e; player.setEq(e); persistSoon(1.0f); }
    }
}

/** Queues the overlay's copy of a track's cover; now: ahead of the rest, and made again even if it exists. */
void App::thumbFor(int track, bool now) {
    const Track& t = lib.tracks[track];
    auto mine = store.covers.find(t.path);
    thumbs.want({ t.path, t.folder, mine == store.covers.end() ? std::string() : mine->second, now }, now);
}

void App::newPlaylist(int track) {
    std::string name = "Playlist " + std::to_string(store.playlists.size() + 1);
    if (!keyboard("New playlist", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    const int p = store.newPlaylist(name);
    if (track >= 0) addToPlaylist(p, track);
    store.save();
    if (track < 0) navigate(View::List, p);
    toast("Made the playlist “" + store.playlists[p].name + "”");
}

void App::addToPlaylist(int playlist, int track) {
    FolderPlaylist& p = store.playlists[playlist];
    const std::string& path = lib.tracks[track].path;
    if (std::find(p.tracks.begin(), p.tracks.end(), path) != p.tracks.end()) { toast("Already in “" + p.name + "”"); return; }
    p.tracks.push_back(path);
    store.save();
    if (p.automix && !p.manual) {   // an Automix playlist: ordered again with it
        orderAutomixList(playlist, false);
        toast("Added to “" + p.name + "”: ordering it for Automix");
        return;
    }
    toast("Added to “" + p.name + "”");
}

// ============================================================ playlist files (m3u.hpp)

/**
 * A playlist file's lines as library tracks, in its order. Each line is tried as written (sdmc:/…, "/…" from the
 * card's root, or relative to the file), then the same path in any case (FAT ignores it), then, for a path from
 * elsewhere (a PC's "C:/Music/…"), the library file of that name that shares the most folders from the end.
 */
std::vector<int> App::resolvePlaylistFile(const std::string& file, int& missing) {
    missing = 0;
    std::vector<M3uLine> lines;
    if (!readM3u(file, lines)) return {};
    const std::string dir = file.substr(0, file.find_last_of('/'));
    std::unordered_map<std::string, int> byLower;
    std::unordered_multimap<std::string, int> byName;
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        byLower.emplace(lower(lib.tracks[i].path), i);
        byName.emplace(lower(baseName(lib.tracks[i].path)), i);
    }
    auto parts = [](const std::string& p) {
        std::vector<std::string> v;
        size_t at = 0;
        while (at <= p.size()) {
            size_t s = p.find('/', at);
            if (s == std::string::npos) s = p.size();
            if (s > at) v.push_back(lower(p.substr(at, s - at)));
            at = s + 1;
        }
        return v;
    };
    std::vector<int> out;
    for (const M3uLine& l : lines) {
        const std::string& p = l.path;
        std::string cand;
        if (plat::rootLen(p)) cand = p;
        else if (!p.empty() && p[0] == '/') cand = plat::fromRoot(p);
        else if (!(p.size() > 1 && p[1] == ':')) cand = dir + "/" + p;   // a drive letter is someone else's card
        int t = -1;
        if (!cand.empty()) {
            cand = normalisePath(cand);
            if (auto it = pathIndex.find(cand); it != pathIndex.end()) t = it->second;
            else if (auto lt = byLower.find(lower(cand)); lt != byLower.end()) t = lt->second;
        }
        if (t < 0) {
            const std::vector<std::string> want = parts(p);
            const auto range = byName.equal_range(lower(baseName(p)));
            int best = 0;
            for (auto it = range.first; it != range.second; ++it) {
                const std::vector<std::string> have = parts(lib.tracks[it->second].path);
                int n = 0;
                while (n < int(want.size()) && n < int(have.size()) && want[want.size() - 1 - n] == have[have.size() - 1 - n]) n++;
                if (n > best) { best = n; t = it->second; }
            }
        }
        if (t >= 0) out.push_back(t);
        else missing++;
    }
    return out;
}

void App::openPlaylistFile(const std::string& file) {
    const Id back = sheet.page != Sheet::Closed ? sheet.returnFocus : ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::PlFile;
    sheet.dir = file;
    sheet.returnFocus = back;
    sheet.fileTracks = resolvePlaylistFile(file, sheet.fileMissing);
    playlistPickerAt = file.substr(0, file.find_last_of('/'));
    ui.focusDefault();
}

/** Starts where the last file was picked this session, else at the card's root. */
void App::openPlaylistPicker() {
    closeSheet();
    std::string at = playlistPickerAt;
    struct stat st;
    if (at.empty() || stat(at.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) at = store.importDir;   // Settings → Storage and reset
    if (at.empty() || stat(at.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) at = plat::root();
    navigate(View::PickPlaylist, -1, at);
}

void App::leavePlaylistPicker() {
    if (route.view == View::PickPlaylist || route.view == View::PickExport) route.query = plat::root();   // as if at the root: back() leaves
    back();
}

/** Starts in the folder last exported to this session, else <music folder>/Playlists if there is one, else the music folder. */
void App::openExportPicker(const std::string& name, const std::vector<int>& tracks) {
    if (tracks.empty()) { toast("Nothing to export: the playlist is empty"); return; }
    exportName = name.empty() ? "Playlist" : name;
    exportTracks = tracks;
    auto isDir = [](const std::string& p) {
        struct stat st;
        return !p.empty() && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    };
    std::string at = exportPickerAt;
    if (!isDir(at)) at = store.exportDir;   // Settings → Storage and reset
    if (!isDir(at) && !lib.roots.empty()) at = isDir(lib.roots[0] + "/Playlists") ? lib.roots[0] + "/Playlists" : lib.roots[0];
    if (!isDir(at)) at = plat::root();
    navigate(View::PickExport, -1, at);
}

/** <dir>/<name>.m3u8, the paths relative to it (writeM3u), so it works on a PC copy of the card too. */
void App::exportPlaylist(const std::string& dir) {
    std::string safe = exportName;
    for (char& c : safe) if (strchr("/\\:*?\"<>|", c)) c = ' ';
    const std::string file = plat::join(dir, safe + ".m3u8");
    auto write = [this, dir, file, safe] {
        std::vector<M3uTrack> out;
        out.reserve(exportTracks.size());
        for (int t : exportTracks) {
            if (t < 0 || t >= int(lib.tracks.size())) continue;   // the library was rebuilt meanwhile
            const Track& tr = lib.tracks[size_t(t)];
            out.push_back({ tr.path, tr.artist, tr.title, tr.duration });
        }
        if (!writeM3u(file, exportName, out)) { toast("Could not write " + file); return; }
        exportPickerAt = dir;
        pickerGen++;   // the next export opens here: its listing shows this file
        const size_t n = out.size();
        leavePlaylistPicker();
        toast("Exported " + std::to_string(n) + (n == 1 ? " track to " : " tracks to ") + safe + ".m3u8 in " +
              (plat::isRoot(dir) ? std::string(plat::rootName()) : baseName(dir)));
    };
    struct stat st;
    if (stat(file.c_str(), &st) != 0) { write(); return; }
    confirm(nullptr, "Export", safe + ".m3u8", "Already in " + dir, "Replace the playlist file that is there?",
            "The file there is overwritten with this playlist's songs.", "Replace", write);
}

void App::confirm(const char* icon, const std::string& kicker, const std::string& title, const std::string& sub, const std::string& question,
                  const std::string& text, const std::string& button, std::function<void()> yes) {
    const Id back = sheet.page != Sheet::Closed ? sheet.returnFocus : ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Confirm;
    sheet.returnFocus = back;
    sheet.askIcon = icon;
    sheet.askKicker = kicker;
    sheet.askTitle = title;
    sheet.askSub = sub;
    sheet.askQuestion = question;
    sheet.askText = text;
    sheet.askButton = button;
    sheet.askYes = std::move(yes);
    ui.focusDefault();
}

/** Settings → Playback → Background player: hands the track over, at the same place, either way. */
void App::setBackground(bool on) {
    store.background = on;
    persistSoon(0.3f);
    const bool had = now.track >= 0 && q.pos >= 0;
    const double at = player.loaded() ? player.position() : 0;
    const bool playing = isPlaying();
    if (on && !player.remote()) {
        player.stop();
        bgConnect();
        if (!player.remote()) { toast("The background player is not installed"); return; }
        bgs.tracks.clear();   // send ours
        if (had) {
            if (!bgSyncQueue(false)) {   // too big for it (it said so): Gatefold carries on by itself
                loadTrack(q.tracks[q.order[q.pos]], !playing);
                if (at > 1) player.seek(at);
                return;
            }
            bg.playAt(q.pos, !playing, at);
            bgs.awaitUntil = time + 1.5;
        }
        toast("Playing through the background player");
    } else if (!on && player.remote()) {
        bg.pause(time);
        bg.disconnect();
        player.setRemote(nullptr);
        if (had) {
            loadTrack(q.tracks[q.order[q.pos]], !playing);
            if (at > 1) player.seek(at);
        }
        toast("Playing in Songate only: music stops when you leave it");
    }
}

/** After your cover for the playing track is set or forgotten: its own cover (or the folder's) again. */
void App::loadTrackCoverAgain() {
    if (now.track < 0) return;
    const Track& t = lib.tracks[now.track];
    TrackLoader::Request r;
    r.gen = ++loadGen;
    r.path = t.path;
    r.folder = t.folder;
    r.readInfo = now.loading;   // the track's own load would be dropped for this one: do its part too
    if (auto it = store.covers.find(t.path); it != store.covers.end()) r.myCover = it->second;
    r.opts = paletteOpts();
    loader.request(std::move(r));
}

/** What the loader read for the track playing: its tags and lyrics, and the cover, uploaded here (textures belong to this thread). */
void App::applyLoaded(TrackLoader::Result& r) {
    if (r.gen != loadGen || (now.track < 0 && !extOn)) { r.release(); return; }   // a track that is no longer the one playing
    now.loading = false;
    static const Track none;   // an outside source playing: no track of the library's
    const Track& t = now.track >= 0 ? lib.tracks[size_t(now.track)] : none;
    if (r.readInfo) {
        TrackInfo& in = r.info;
        if (in.title.empty()) in.title = t.title;
        if (in.artist.empty()) in.artist = t.artist;
        if (in.album.empty()) in.album = t.album;
        if (in.duration <= 0) in.duration = t.duration;
        if (store.edits.count(t.path)) {
            // Your edits win over the file's tags here too (the library copy already has them).
            in.title = t.title; in.artist = t.artist; in.album = t.album;
            in.albumArtist = t.albumArtist; in.genre = t.genre; in.date = t.date; in.trackNo = t.trackNo;
        }
        now.info = std::move(in);
        now.lyricsSource = r.inSongate ? "Songate's folder" : r.sidecar ? ".lrc file" : "Tags";
        now.lyricsFromFile = r.sidecar;
        now.lyricsInSongate = r.inSongate;
        lyrIndex.offer(t.path, r.lyrics);   // search by lyrics: a song changed elsewhere is set right when it plays
        parseLyrics(r.lyrics);
        lyrOnlineReset();
        // The new texts render this frame; the cover is uploaded the next one (update), not both at once.
#ifdef GF_ANDROID
        coverWait_.release();   // one held while hidden, not uploaded: a newer song's replaces it
#endif
        coverWait_ = std::move(r);
        coverWaiting_ = true;
#ifdef GF_ANDROID
        coverToldHidden_ = false;   // a new song: the system's controls are told of it
#endif
        return;
    }
#ifdef GF_ANDROID
    if (hiddenNow()) {   // no GPU while hidden: it waits with the others (update)
        coverWait_.release();
        coverWait_ = std::move(r);
        coverWaiting_ = true;
        coverToldHidden_ = false;
        return;
    }
#endif
    applyCover(r);
}

void App::forgetNowPlaying() {
    loadGen++;   // what the loader still reads for the old track is dropped
    now.track = -1;
    now.info = TrackInfo{};
    now.lyrics.clear();
    now.synced = false;
    now.loading = false;
    // The cover fades out, as it fades between tracks
    if (now.prevCover) SDL_DestroyTexture(now.prevCover);
    if (now.prevBlur) SDL_DestroyTexture(now.prevBlur);
    if (now.prevFrost) SDL_DestroyTexture(now.prevFrost);
    now.prevCover = now.cover;
    now.prevBlur = now.blur;
    now.prevFrost = now.frost;
    now.cover = now.blur = now.frost = nullptr;
    now.fade = 0;
    savedTrack = -1;
}

/** The loaded cover: uploaded, crossfaded in, and the palette from it. */
void App::applyCover(TrackLoader::Result& r) {
    if (r.gen != loadGen || (now.track < 0 && !extOn)) { r.release(); return; }
#ifdef GF_WINDOWED
    plat::mediaInfo(now.info.title, now.info.artist, now.info.album, r.mediaCover ? static_cast<const uint8_t*>(r.mediaCover->pixels) : nullptr,
                    r.mediaCover ? r.mediaCover->w : 0, r.mediaCover ? r.mediaCover->h : 0, r.mediaJpeg);
#endif
#ifdef GF_AIRPLAY
    airCover_ = std::move(r.airCover);   // (AirPlay speakers: airsendTick sends it with the song's details)
    airCoverGen_++;
#endif
    if (mixOld.on) mixOld.coverIn = true;   // (a join's outgoing cover is now.prevCover from here: mixCover)
    // Keep the outgoing cover for the 500 ms crossfade.
    if (now.prevCover) SDL_DestroyTexture(now.prevCover);
    if (now.prevBlur) SDL_DestroyTexture(now.prevBlur);
    if (now.prevFrost) SDL_DestroyTexture(now.prevFrost);
    now.prevCover = now.cover;
    now.prevBlur = now.blur;
    now.prevFrost = now.frost;
    now.cover = now.blur = now.frost = nullptr;
    now.fade = 0;
    coverPx.clear();
    coverPal = store.brightness ? paletteFixed(FixedTheme::Light) : paletteDefault();
    if (firstCover_) { firstCover_ = false; launchAt("cover"); }
    if (r.cover) {
        now.cover = SDL_CreateTextureFromSurface(renderer_, r.cover);
        if (now.cover) { SDL_SetTextureScaleMode(now.cover, SDL_ScaleModeLinear); SDL_SetTextureBlendMode(now.cover, SDL_BLENDMODE_BLEND); }   // (blended: its rounded edge is drawn through alpha)
        // The overlay's copy, made again for the track shown: a cover set in the track menu changes it.
        if (player.remote() && now.track >= 0 && !extOn) thumbFor(now.track, true);
    }
    if (r.blur) {
        now.blur = SDL_CreateTextureFromSurface(renderer_, r.blur);
        if (now.blur) SDL_SetTextureScaleMode(now.blur, SDL_ScaleModeLinear);
    }
    if (r.frost) {
        now.frost = SDL_CreateTextureFromSurface(renderer_, r.frost);
        if (now.frost) SDL_SetTextureScaleMode(now.frost, SDL_ScaleModeLinear);
    }
    if (!r.px.empty()) {
        coverPal = r.pal;
        coverPx = std::move(r.px);
    }
    r.release();
    applyTheme();
}

/** [mm:ss.xx] LRC lines become timed lines; anything else is plain text. */
void App::parseLyrics(const std::string& text) {
    now.lyrics.clear();
    now.synced = false;
    now.lrcOffset = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<double> stamps;
        size_t p = 0;
        while (p < line.size() && line[p] == '[') {
            const size_t close = line.find(']', p);
            if (close == std::string::npos) break;
            int mm = 0;
            double ss = 0;
            if (sscanf(line.c_str() + p + 1, "%d:%lf", &mm, &ss) == 2) stamps.push_back(mm * 60 + ss);
            else if (!stamps.empty()) break;
            else if (!line.compare(p + 1, 7, "offset:")) now.lrcOffset = atof(line.c_str() + p + 8) / 1000;   // ms; + is sooner
            p = close + 1;
        }
        std::string body = line.substr(p);
        while (!body.empty() && body.front() == ' ') body.erase(0, 1);
        if (!stamps.empty()) {
            now.synced = true;
            for (double t : stamps) now.lyrics.push_back({ t, body });
        } else if (p == 0) {
            now.lyrics.push_back({ -1, body });
        }
        if (end == text.size()) break;
    }
    if (now.synced) {
        std::erase_if(now.lyrics, [](const LyricLine& l) { return l.t < 0; });
        // [offset:] as LRC defines it: a positive offset shows every line that much sooner
        if (now.lrcOffset != 0) for (LyricLine& l : now.lyrics) l.t = std::max(0.0, l.t - now.lrcOffset);
        std::stable_sort(now.lyrics.begin(), now.lyrics.end(), [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
    }
    while (!now.lyrics.empty() && now.lyrics.back().text.empty() && !now.synced) now.lyrics.pop_back();
}

float App::lyricNudge() const {
    if (now.track < 0) return 0;
    const auto it = store.lyricNudge.find(lib.tracks[now.track].path);
    return it == store.lyricNudge.end() ? 0 : it->second;
}

void App::nudgeLyrics(float by) {
    if (now.track < 0) return;
    const std::string& path = lib.tracks[now.track].path;
    const float v = by == 0 ? 0 : std::clamp(std::round((lyricNudge() + by) * 10) / 10, -10.f, 10.f);
    if (std::fabs(v) < 0.05f) store.lyricNudge.erase(path);
    else store.lyricNudge[path] = v;
    lyricNudgeUnsaved_ = true;   // written with the next save (persistNow), not at every 0.1 s step
    persistSoon(3.f);
}

void App::seekLyric(int line) {
    if (line >= 0 && line < int(now.lyrics.size()) && now.lyrics[line].t >= 0) player.seek(std::max(0.0, now.lyrics[line].t + lyricNudge()));
}

// ============================================================ navigation

ScrollArea& App::area(const char* key, int ref) { return areas[uid(key, ref)]; }

void App::navigate(View v, int ref, const std::string& query, bool push) {
    canvas = false;
    // A picker opened from elsewhere lists afresh: the card may have changed meanwhile (an export, Files)
    auto picker = [](View x) { return x == View::PickFolder || x == View::PickPlaylist || x == View::PickExport; };
    if (picker(v) && !picker(route.view)) pickerGen++;
    // Into the folder pages: from Library → Folders they stay Library's in the sidebar; moving between
    // folders keeps whichever it was.
    if (v == View::Folders && route.view != View::Folders) folderFrom = route.view == View::Library ? View::Library : View::Folders;
    if (push && !(route.view == v && route.ref == ref && route.query == query)) {
        route.focus = ui.focusedId();
        history.push_back(route);
        if (history.size() > 40) history.erase(history.begin());
    }
    route = { v, ref, query, 0 };
    if (v == View::Search) {
        runSearch();
        area("search").target = area("search").y = 0;
        searchLyrics_.clear();
        lyrIndex.search(query, ++searchTicket_);   // In the lyrics: the worker's answer fills it (App::update)
    }
    ui.focusDefault();
}

/** What route.query matches, as indices into the lists as they are now (searchSync keeps them so). */
void App::runSearch() {
    searchFor_ = route.query;
    searchGen_ = indexGen;
    searchLists_ = searchListsStamp();
    searchResults.clear();
    const std::vector<std::string> words = searchWords(route.query);
    for (int i = 0; i < int(lib.tracks.size()); i++) {
        const Track& t = lib.tracks[i];
        if (wordsIn(words, t.title + " " + t.artist + " " + t.album + " " + t.genre + " " + t.path)) searchResults.push_back(i);
    }
    // the other tabs (the user, 2026-11-09): your albums, artists, genres and playlists whose names have the words
    searchAlbums_.clear(); searchArtists_.clear(); searchGenres_.clear(); searchPlaylists_.clear();
    for (int i = 0; i < int(lib.albums.size()); i++) if (wordsIn(words, lib.albums[size_t(i)].title + " " + lib.albums[size_t(i)].artist)) searchAlbums_.push_back(i);
    for (int i = 0; i < int(artists.size()); i++) if (wordsIn(words, artists[size_t(i)].name)) searchArtists_.push_back(i);
    for (int i = 0; i < int(genres.size()); i++) if (wordsIn(words, genres[size_t(i)].name)) searchGenres_.push_back(i);
    if (wordsIn(words, "Liked")) searchPlaylists_.push_back(-1);
    for (int i = 0; i < int(store.playlists.size()); i++) if (wordsIn(words, store.playlists[size_t(i)].name)) searchPlaylists_.push_back(i);
}

/** The playlists' names in their order: a rename, a move or a delete changes what an index into them means. */
uint32_t App::searchListsStamp() const {
    uint32_t h = 2166136261u;
    for (const auto& p : store.playlists) {
        for (unsigned char c : p.name) h = (h ^ c) * 16777619u;
        h = (h ^ 0xffu) * 16777619u;
    }
    return h;
}

/** A Search page's lists hold indices: made again when the index was rebuilt (tags arriving, a rescan), a playlist
 *  came, went or moved, or the page was come Back to after another search. */
void App::searchSync() {
    if (route.view != View::Search) return;
    if (searchFor_ == route.query && searchGen_ == indexGen && searchLists_ == searchListsStamp()) return;
    const bool other = searchFor_ != route.query;
    runSearch();
    if (other) {
        searchLyrics_.clear();
        lyrIndex.search(route.query, ++searchTicket_);
    }
}

std::vector<std::string> App::searchWords(const std::string& query) {
    std::vector<std::string> words;
    std::string w;
    for (char c : lower(query) + " ") {
        if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += c;
    }
    return words;
}

bool App::wordsIn(const std::vector<std::string>& words, const std::string& hay) {
    const std::string h = lower(hay);
    return std::all_of(words.begin(), words.end(), [&](const std::string& x) { return h.find(x) != std::string::npos; });
}

PaletteOpts App::paletteOpts() const {
    PaletteOpts o;
    o.mode = store.brightness ? PaletteOpts::Light : PaletteOpts::Dark;
    o.intensity = store.saturation;
    return o;
}

/** Custom: your background and accents (theme.js customPalette); a colour left to "A" comes from the cover playing. */
Palette App::customPalette() const {
    PaletteOpts c = paletteOpts();
    auto rgb = [](int v) { return Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f }; };
    c.hasBg = store.customBg >= 0;
    c.hasAccent = store.customAccent >= 0;
    c.hasAccent2 = store.customAccent2 >= 0;
    if (c.hasBg) c.bg = rgb(store.customBg);
    c.accent = c.hasAccent ? rgb(store.customAccent) : coverPal.accent;
    c.hasAccent = true;   // theme.js: a custom theme always has its accent
    if (c.hasAccent2) c.accent2 = rgb(store.customAccent2);
    // Brightness: on a dark or a light background, your colours read as a cover's; As picked keeps your background
    // (with "A" for the background there is none to keep: the Brightness of From the cover applies)
    if (store.customMode == 0) c.mode = PaletteOpts::Dark;
    else if (store.customMode == 1) c.mode = PaletteOpts::Light;
    else if (c.hasBg) c.mode = PaletteOpts::Auto;
    return paletteCustom(c);
}

/** Settings → Appearance: From the cover (dark or light, any saturation), Dark, Light, or Custom. */
void App::applyTheme() {
    const PaletteOpts o = paletteOpts();
    paletteSetDefaults(o);   // the album pages' palettes, made on the metadata thread
    if (!coverPx.empty()) coverPal = paletteFromPixels(coverPx.data(), 64, 64, o);
    else coverPal = store.brightness ? paletteFixed(FixedTheme::Light) : paletteDefault();
    switch (store.theme) {
    case 1: target = paletteFixed(FixedTheme::Dark); break;
    case 2: target = paletteFixed(FixedTheme::Gray); break;
    case 3: target = paletteFixed(FixedTheme::Light); break;
    case 4: target = customPalette(); break;
    default: target = coverPal; break;
    }
}

int App::canvasLayoutShown() const {
    // A track's lyrics are still on their way: keep the layout, rather than going Minimal for a moment and back.
    if (now.loading && now.track >= 0) return cvTo;
    // Nothing to read: Minimal. So is the lyrics-first layout with lyrics switched off.
    if (now.lyrics.empty() && store.canvasAutoMinimal) return 2;
    if (store.canvasLayout == 1 && !store.canvasLyrics) return 2;
    return store.canvasLayout;
}

unsigned App::canvasLayoutsOff() const {
    if (now.lyrics.empty() && store.canvasAutoMinimal && now.track >= 0) return 0b011;   // Cover and Lyrics: Minimal until lyrics come back
    if (!store.canvasLyrics) return 0b010;                                                // Lyrics with lyrics switched off
    return 0;
}

void App::openCanvas(bool on) {
#ifdef GF_ANDROID
    if (on && css::H > css::W) return;   // no Canvas upright (the user, 2026-11-04): its key, the tour's step, nothing opens it there
#endif
    canvas = on;
    // Opening shows the right layout at once; only changes while it is open are animated.
    cvTo = canvasLayoutShown();
    for (int i = 0; i < 3; i++) cvFrom[i] = i == cvTo ? 1.f : 0.f;
    cvStart = -10;
    canvasIdle = 0;
    canvasCtl = 1;
    canvasViz = 0;
    if (on) ui.focus(uid("cv.play"));
    else ui.focusDefault();
}

void App::back() {
    if (canvas) { openCanvas(false); return; }
#ifdef GF_ANDROID
    // a phone upright: back closes its Up next, then its lyrics page, before it leaves the page (view_phone.cpp)
    if (phonePortrait() && phoneQueue_) { phoneQueue_ = false; return; }
    if (phonePortrait() && phoneLyrics_ && route.view == View::Now) { phoneLyrics_ = false; return; }
    if (phonePortrait() && route.view == View::Settings && (settingsPhoneOpen_ || !settingsQuery.empty()) && !settingsPhoneDirect_) {   // a page: to the list
        settingsPhoneOpen_ = false;
        settingsQuery.clear();
        return;
    }
    if (settingsPhoneDirect_) settingsPhoneOpen_ = settingsPhoneDirect_ = false;   // opened from elsewhere: back there
#endif
    // The playlist browsers (import, export): B goes to the folder above; at the SD card's root it leaves them.
    if (route.view == View::PickPlaylist || route.view == View::PickExport) {
        const std::string& p = route.query;
        if (!plat::isRoot(p)) {
            const std::string child = p;
            navigate(route.view, -1, plat::parentDir(child), false);
            ui.focus(uid("pk.dir", int(uid(child) & 0x7fffffff)));
            return;
        }
        while (!history.empty() && (history.back().view == View::PickPlaylist || history.back().view == View::PickExport)) history.pop_back();
    }
    // Files: B leaves Select first
    if (route.view == View::Files && files.selecting) {
        files.selecting = false;
        files.selected.clear();
        return;
    }
    // Files: B goes to the folder above (the one come from is focused); at the SD card's root it leaves as any page.
    if (route.view == View::Files && !plat::isRoot(route.query)) {
        const std::string p = route.query;
        files.focusAfter = p;
        navigate(View::Files, -1, plat::parentDir(p), false);
        return;
    }
    // Settings: from a category's options, B goes back to that category in the list; only from the list does it leave.
    if (const SDL_FRect f = ui.rectOf(ui.focusedId());
        route.view == View::Settings && f.x >= settingsPaneX && f.y >= css::TbH && f.y + f.h <= css::H - css::BarH) {
        ui.focus(settingsQuery.empty() ? uid("set.cat", settingsCat) : uid("set.find"));
        ui.setPadMode(true);
        return;
    }
    if (history.empty()) {
        if (route.view != View::Now) navigate(View::Now, -1, {}, false);
        return;
    }
    route = history.back();
    history.pop_back();
    if (route.focus) ui.focus(route.focus);
    else ui.focusDefault();
}

void App::openSearch() {
    char out[256] = {};
    const plat::TextResult r = plat::askText("Search", searchHint(), route.view == View::Search ? route.query.c_str() : nullptr, out, sizeof out);
    if (r == plat::TextResult::Unavailable) toast("The keyboard is not available in applet mode");
    if (r == plat::TextResult::Done && out[0]) navigate(View::Search, -1, out, route.view != View::Search);
}

void App::toast(const std::string& s) {
    toastText = s;
    toastT = 3.2f;
    logLine(s);
}

void App::logLine(const std::string& s) {
    // the app's log (Settings › Support sends its last lines with a report): what it told you, and when
    const time_t t = std::time(nullptr);
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", localtime(&t));
    logLines_.push_back(std::string(b) + "  " + s);
#ifdef GF_DEV
    if (logLines_.size() > 1000) logLines_.erase(logLines_.begin(), logLines_.begin() + 200);   // (dev: a whole test)
#else
    if (logLines_.size() > 200) logLines_.erase(logLines_.begin(), logLines_.begin() + 50);
#endif
    logDirty_ = true;
}

// ============================================================ helpers

std::vector<int> App::listTracks(View v, int ref, const std::string&) const {
    std::vector<int> out;
    switch (v) {
    case View::Queue: return queueInPlayOrder();   // as it will play, like Up next
    case View::Search: return searchResults;
    case View::Album:
        if (ref >= 0 && ref < int(lib.albums.size())) out = lib.albums[ref].tracks;
        return out;
    case View::Artist:
        if (ref >= 0 && ref < int(artists.size())) out = artists[ref].tracks;
        return out;
    case View::Genre:
        if (ref >= 0 && ref < int(genres.size())) out = genres[ref].tracks;
        return out;
    case View::Folders: return lib.tracksUnder(ref);
    case View::List:
        if (ref < 0) {
            for (const auto& p : store.liked) {
                auto it = pathIndex.find(p);
                if (it != pathIndex.end()) out.push_back(it->second);
            }
        } else if (ref < int(store.playlists.size())) {
            const FolderPlaylist& p = store.playlists[ref];
            if (p.own()) {
                // an Automix playlist being analysed: only its songs analysed so far, in their order (the rest join it one by one)
                const bool hide = p.automix && p.waiting && !p.manual;
                for (const auto& path : p.tracks)
                    if (auto it = pathIndex.find(path); it != pathIndex.end() && (!hide || automixReady(path))) out.push_back(it->second);
            } else if (p.smart()) {
                out = smartTracks(p.path);
            } else {
                out = lib.tracksUnder(lib.folderByPath(p.path));
            }
        }
        return out;
    default:
        // The whole music folder, in folder order.
        for (int i = 0; i < int(lib.tracks.size()); i++) out.push_back(i);
        return out;
    }
}

std::string App::fmtTime(double s) const {
    if (s <= 0 || !std::isfinite(s)) return "–:––";
    const int t = int(s);
    char buf[16];
    if (t >= 3600) snprintf(buf, sizeof buf, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(buf, sizeof buf, "%d:%02d", t / 60, t % 60);
    return buf;
}

std::string App::fmtTotal(const std::vector<int>& tracks) const {
    double s = 0;
    for (int t : tracks) s += lib.tracks[t].duration;
    const int m = int(s / 60 + 0.5);
    std::string n = std::to_string(tracks.size()) + (tracks.size() == 1 ? " track" : " tracks");
    if (m <= 0) return n;
    return n + " · " + (m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min" : std::to_string(m) + " min");
}

std::string App::trackSub(int t) const {
    const Track& tr = lib.tracks[t];
    if (tr.artist.empty()) return tr.album;
    if (tr.album.empty()) return tr.artist;
    return tr.artist + " · " + tr.album;
}


int main(int argc, char** argv) {
    // The running program, where an update's new one is swapped in
    const std::string self = plat::selfPath(argc, argv);
#ifndef GF_WINDOWED
    // The settings folder, before anything reads it: gatefold-nx moved to songate-nx, a move cut off finished (cfgmove.hpp)
    cfgmove::Result folder = cfgmove::atLaunch(Updater::thisVersion());
#endif
#if defined(GF_DEV) && !defined(GF_WINDOWED)
    // Tests of the move alone (the preview: nothing of the app's own saves mixed in): GF_CFGMOVE_ONLY stops here; GF_CFGMOVE_BACK=<version>
    // moves it back as an install of that version would, then stops
    if (const char* v = getenv("GF_CFGMOVE_BACK")) {
        std::string log;
        const bool ok = cfgmove::back(v, log);
        printf("[cfgmove] back %s:%s\n", ok ? "ok" : "FAILED", log.c_str());
        return 0;
    }
    if (getenv("GF_CFGMOVE_ONLY")) {
        printf("[cfgmove] dir=%s backups=%s moved=%d returned=%d both=%d legacy=%d:%s\n", folder.dir.c_str(), folder.backups.c_str(), int(folder.moved),
               int(folder.returned), int(folder.both), int(folder.legacy), folder.log.c_str());
        return 0;
    }
#endif
    if (plat::swapsSelf()) {
        // An update installed while Gatefold was closed some other way (the console restarted): its .nro is swapped in
        // before anything reads this one, and the new one starts instead.
        if (const std::string next = Updater::finishPending(Store::Dir, self); !next.empty()) {
#ifndef GF_WINDOWED
            const bool wentBack = App::goBackIfDue();   // an older version's: its folder is gatefold-nx
#endif
            if (plat::relaunch(next)) return 0;
#ifndef GF_WINDOWED
            if (wentBack) folder = cfgmove::atLaunch(Updater::thisVersion());   // it did not start: this one carries on, moved forward again
#endif
        }
#ifndef GF_WINDOWED
        Updater::cleanLegacy(Store::Dir, self);   // after the rebranding: 1.x's .nro and overlay, once (the first launch)
#endif
    }
    App app;
    app.selfPath = self;
#ifndef GF_WINDOWED
    app.folder = folder;
#endif
    if (app.init()) app.run();
    app.shutdown();
#ifdef GF_ANDROID
    // Android keeps the process after main returns and SDL would run main in it again, every static as this run left it: the next
    // start gets a process of its own (shutdown has ended every thread and written everything)
    exit(0);
#endif
    return 0;
}
