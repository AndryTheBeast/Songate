/*
 * Gatefold NX · store.cpp
 */
#include "store.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

#include <deque>
#include <mutex>
#include <thread>

#include "cores.hpp"
#include "safefile.hpp"

namespace {

/**
 * Files are written by a thread of their own: a save touched five files on the SD card, tens of
 * milliseconds on the console, and it ran every 20 s while playing, a hitch in whatever was moving.
 * A file queued again before it was written is replaced, and a file whose contents did not change
 * since it was last written is skipped.
 */
struct Writer {
    struct Job { std::string path, data; bool atomic = false, erase = false; };   // erase: the file removed, not written
    std::mutex m;
    std::condition_variable wake, done;
    std::deque<Job> jobs;
    std::unordered_map<std::string, std::string> written;
    bool busy = false, started = false, quit = false;
    std::thread thread;   // kept, never detached: libnx has no pthread_detach (detach() throws and aborts)

    void put(const std::string& path, std::string data, bool atomic, bool erase = false) {
        std::lock_guard<std::mutex> g(m);
        const bool queued = std::any_of(jobs.begin(), jobs.end(), [&](const Job& j) { return j.path == path; });
        if (auto w = written.find(path); !erase && !queued && w != written.end() && w->second == data) return;
        if (queued) { for (Job& j : jobs) if (j.path == path) { j.data = std::move(data); j.erase = erase; } }
        else jobs.push_back({ path, std::move(data), atomic, erase });
        if (!started) {
            started = true;
            thread = std::thread([this] { cores::moveHere(cores::Files); run(); });
        }
        wake.notify_one();
    }
    void flush() {
        std::unique_lock<std::mutex> g(m);
        done.wait(g, [&] { return jobs.empty() && !busy; });
    }
    /** Writes what is queued and ends the thread. A thread left running when Gatefold exits keeps hbloader from
     *  unloading it: loading the next .nro (an update's Restart now) then aborted with 0xD401. */
    void stop() {
        {
            std::lock_guard<std::mutex> g(m);
            if (!started) return;
            quit = true;
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
        std::lock_guard<std::mutex> g(m);
        started = false;
        quit = false;
    }
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> g(m);
                busy = false;
                done.notify_all();
                wake.wait(g, [&] { return !jobs.empty() || quit; });
                if (jobs.empty()) return;   // quit, with everything written
                j = std::move(jobs.front());
                jobs.pop_front();
                busy = true;
            }
            if (j.erase) {   // (after any write of it queued before: they are in order)
                remove(j.path.c_str());
                std::lock_guard<std::mutex> g(m);
                written.erase(j.path);
                continue;
            }
            // Atomic ones are written next to the old file and swapped in (safefile.hpp), so a power cut at any moment
            // loses nothing: Store::load puts back whichever whole copy is on the card.
            // A write that did not go through whole (a full SD card) never replaces the old file, and is not remembered
            // as written, so the next save tries again.
            const std::string tmp = j.atomic ? j.path + ".tmp" : j.path;
            if (FILE* f = fopen(tmp.c_str(), "wb")) {
                bool ok = fwrite(j.data.data(), 1, j.data.size(), f) == j.data.size();
                ok = fclose(f) == 0 && ok;
                if (j.atomic) {
                    if (ok) ok = safefile::swapIn(tmp, j.path);
                    else remove(tmp.c_str());
                }
                if (ok) {
                    std::lock_guard<std::mutex> g(m);
                    written[j.path] = std::move(j.data);
                }
            }
        }
    }
};
Writer& writer() {
    static Writer* w = new Writer;   // never destroyed; its thread ends with Store::stopWriter (App::shutdown)
    return *w;
}

void appendf(std::string& s, const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t at = s.size();
        s.resize(at + n + 1);
        vsnprintf(&s[at], n + 1, fmt, ap2);
        s.resize(at + n);
    }
    va_end(ap2);
}

void makeDirs() {
    static bool made = false;   // the tag cache is opened in the folder straight after the first save
    if (made) return;
    mkdir(plat::configParent(), 0777);
    mkdir(Store::Dir, 0777);
    made = true;
}

std::vector<std::string> readLines(const std::string& file) {
    std::vector<std::string> out;
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return out;
    // Read whole and split: a fixed buffer cut any longer line (lyrics kept in edits.tsv) into two records.
    std::string data;
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, got);
    fclose(f);
    for (size_t a = 0; a < data.size();) {
        size_t b = data.find('\n', a);
        if (b == std::string::npos) b = data.size();
        size_t n = b - a;
        while (n && data[a + n - 1] == '\r') n--;
        if (n) out.emplace_back(data, a, n);
        a = b + 1;
    }
    return out;
}

}  // namespace

void Store::load() {
    safefile::recoverDir(Dir);   // a save cut by a power loss between its renames: the whole copy back in place
    bool filesTabKnown = false;   // a settings.txt from before Files: its hiddenTabs knows nothing of bit 6
    bool linesKnown = false;      // ... and one from before placed lines: Files had a line of its own above it
    bool lyricsKindKnown = false; // ... and one from before lyrics were shared: their switch starts on (the user, 2026-11-04)
    bool moodsKindKnown = false;  // ... and mood choices, the same (BETA 3)
    for (const auto& l : readLines(path("settings.txt"))) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "volume") volume = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "order") order = Order(std::clamp(atoi(v.c_str()), 0, 3));
        else if (k == "sidebarCollapsed") sidebarCollapsed = v == "1";
        else if (k == "motion") motion = v == "1";
        else if (k == "mixAnimNow") mixAnimNow = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "mixAnimCanvas") mixAnimCanvas = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "mixHint") mixHint = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "spectrum") showSpectrum = v == "1";
        else if (k == "canvasLayout") canvasLayout = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "canvasLyrics") canvasLyrics = v == "1";
        else if (k == "lyricTiming") lyricTiming = v == "1";
        else if (k == "lyricTimingFor") lyricTimingFor = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "lyricsOnline") lyricsOnline = v == "1";
        else if (k == "lyricsOnlineWay") lyricsOnlineWay = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "lyricsSaveTo") lyricsSaveTo = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "resume") resume = v == "1";
        else if (k == "background") background = v == "1";
#ifdef GF_WINDOWED
        else if (k == "window") { int m = 0; sscanf(v.c_str(), "%d %d %d %d %d", &winX, &winY, &winW, &winH, &m); winMax = m != 0; }
#endif
        else if (k == "viewPlaylist") viewPlaylist = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewFolders") viewFolders = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewTracks") viewTracks = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewGenre") viewGenre = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "libTab") libTab = std::clamp(atoi(v.c_str()), 0, 4);
        else if (k == "tracksSort") tracksSort = std::clamp(atoi(v.c_str()), 0, 5);
        else if (k == "tracksDesc") tracksDesc = v == "1";
        else if (k == "hiddenTabs") hiddenTabs = unsigned(atoi(v.c_str())) & 127u;
        else if (k == "theme") { theme = std::clamp(atoi(v.c_str()), 0, 4); if (theme == 2) theme = 1; }   // Grey is no longer offered: Dark
        else if (k == "brightness") brightness = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "saturation") saturation = std::clamp(float(atof(v.c_str())), 0.f, 1.f);   // was up to 150%, which changed nothing past 100%
        else if (k == "colourTransition") colourTransition = std::clamp(float(atof(v.c_str())), 0.f, 3.f);
        else if (k == "customBg") customBg = atoi(v.c_str());
        else if (k == "customAccent") customAccent = atoi(v.c_str());
        else if (k == "customAccent2") customAccent2 = atoi(v.c_str());
        else if (k == "customMode") customMode = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "canvasAutoMinimal") canvasAutoMinimal = v == "1";
        else if (k == "canvasFloat") canvasFloat = v == "1";
        else if (k == "canvasZoom") canvasZoom = v == "1";
        else if (k == "vizSensitivity") vizSensitivity = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "vizSmoothing") vizSmoothing = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "vizBands") vizBands = std::clamp(atoi(v.c_str()), 16, 96);
        else if (k == "vizPeaks") vizPeaks = v == "1";
        else if (k == "seekWave") seekWave = v == "1";
        else if (k == "canvasPulse") canvasPulse = v == "1";
        else if (k == "canvasBars") canvasBars = std::clamp(atoi(v.c_str()), 16, 128);
        else if (k == "canvasIdleAfter") canvasIdleAfter = std::clamp(float(atof(v.c_str())), 1.f, 30.f);
        else if (k == "canvasVizTime") canvasVizTime = std::clamp(float(atof(v.c_str())), 0.1f, 4.f);
        else if (k == "coverPulse") coverPulse = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "pointerSpeed") pointerSpeed = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "pointerAccel") pointerAccel = v == "1";
        else if (k == "frameTimes") frameTimes = v == "1";
        else if (k == "automixModel") automixModel = v;
        else if (k == "modelToasted") modelToasted = v;
        else if (k == "heldOrder") heldOrder = std::clamp(atoi(v.c_str()), -1, 3);
        else if (k == "keepAwake") keepAwake = v == "1";
        else if (k == "swipeKeep") swipeKeep = v == "1";
        else if (k == "levelMode") levelMode = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "levelTarget") levelTarget = std::clamp(atoi(v.c_str()), -23, -6);
        else if (k == "batteryPercent") batteryPercent = v == "1";
        else if (k == "batteryEstimate") batteryEstimate = v == "1";
        else if (k == "batteryWatts") batteryWatts = v == "1";
        else if (k == "batteryTemp") batteryTemp = v == "1";
        else if (k == "batteryVolts") batteryVolts = v == "1";
        else if (k == "batteryHealth") batteryHealth = v == "1";
        else if (k == "batteryPower") batteryPower = v == "1";
        else if (k == "batteryFahrenheit") batteryFahrenheit = v == "1";
        else if (k == "batteryOrder") {   // one digit each; what is missing goes at the end
            std::vector<int> o;
            for (char c : v)
                if (c >= '0' && c <= '6' && std::find(o.begin(), o.end(), c - '0') == o.end()) o.push_back(c - '0');
            for (int i = 0; i < 7; i++)
                if (std::find(o.begin(), o.end(), i) == o.end()) o.push_back(i);
            batteryOrder = o;
        }
        else if (k == "batteryCol") { for (size_t i = 0; i < size_t(BatteryStates) && i < v.size(); i++) batteryCol[i] = std::clamp(v[i] - 'a', 0, BatteryColours - 1); }
        else if (k == "batteryOutline") batteryOutline = std::clamp(atoi(v.c_str()), -1, BatteryColours - 1);
        else if (k == "batteryLook") { for (size_t i = 0; i < size_t(BatteryStates) && i < v.size(); i++) batteryLook[i] = std::clamp(v[i] - 'a', 0, BatteryLooks - 1); }
        else if (k == "batteryLowAt") batteryLowAt = std::clamp(atoi(v.c_str()), 5, 50);
        else if (k == "batteryAnimPace") batteryAnimPace = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "dimAfter") dimAfter = std::clamp(atoi(v.c_str()), 0, 60);
        else if (k == "sleepConsole") sleepConsole = std::clamp(atoi(v.c_str()), 0, 300);
        else if (k == "stopCovers") stopCovers = v == "1";
        else if (k == "stopThumbs") stopThumbs = v == "1";
        else if (k == "stopLevels") stopLevels = v == "1";
        else if (k == "stopLyrics") stopLyrics = v == "1";
        else if (k == "stopDownloads") stopDownloads = v == "1";
        else if (k == "stopTagWrites") stopTagWrites = v == "1";
        else if (k == "stopMixOrder") stopMixOrder = v == "1";
        else if (k == "pocketButton") pocketButton = v == "1";
        else if (k == "pocketHint") pocketHint = v == "1";
        else if (k == "pocketPads") pocketPads = v == "1";
        else if (k == "pocketKey") { pocketKey = std::clamp(atoi(v.c_str()), 0, 3); if (pocketKey == 2) pocketKey = 0; }   // (Either: out, the user 2026-11-08)
        else if (k == "pocketHow") pocketHow = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "pocketHold") pocketHold = std::clamp(atoi(v.c_str()), 1, 3);
        else if (k == "tagWrite") tagWrite = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "crossfade") crossfade = std::clamp(float(atof(v.c_str())), 0.f, 12.f);
        else if (k == "xfadeNotInAlbum") xfadeNotInAlbum = v == "1";
        else if (k == "eqOn") this->eq.on = uint8_t(v == "1");
        else if (k == "eqAdvanced") this->eq.advanced = uint8_t(v == "1");
        else if (k == "eqPreset") this->eq.preset = int8_t(std::clamp(atoi(v.c_str()), -1, GFEQ_PRESETS + GFEQ_USER_MAX - 1));
        else if (k == "eqPreamp") this->eq.preamp = float(atof(v.c_str()));
        else if (k == "eqBass") this->eq.bass = float(atof(v.c_str()));
        else if (k == "eqTreble") this->eq.treble = float(atof(v.c_str()));
        else if (k == "eqEdited") this->eq.edited = uint8_t(v == "1");
        else if (k == "eqBands") gfEqParseBands(v.c_str(), this->eq.bands);
        else if (k == "eqCustom") { gfEqParseBands(v.c_str(), this->eq.custom); this->eq.hasCustom = 1; }
        else if (k == "musicRoot") musicRoot = v;
        else if (k == "showHidden") showHidden = v == "1";
        else if (k == "viewPlaylists") viewPlaylists = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewGenres") viewGenres = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewArtists") viewArtists = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "viewAlbums") viewAlbums = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "letterRails") {   // two sets; one number (an earlier build) is both
            unsigned a = 31, b = 31;
            const int got = sscanf(v.c_str(), "%u,%u", &a, &b);
            if (got == 1) b = a;
            letterRails[0] = a & 31u; letterRails[1] = b & 31u;
        }
        else if (k == "tabOrder") {
            // ids that exist, once each; any missing (a tab newer than the file) go at the end in their own order
            std::vector<int> o;
            for (size_t i = 0; i < v.size();) {
                const int t = atoi(v.c_str() + i);
                if (((t >= 0 && t < TabCount) || isLine(t)) && std::find(o.begin(), o.end(), t) == o.end()) o.push_back(t);
                const size_t c = v.find(',', i);
                if (c == std::string::npos) break;
                i = c + 1;
            }
            for (int t = 0; t < TabCount; t++) if (std::find(o.begin(), o.end(), t) == o.end()) o.push_back(t);
            tabOrder = o;
        }
        else if (k == "filesTab") filesTabKnown = true;
        else if (k == "tabLines") linesKnown = true;
        else if (k == "filesDir") filesDir = v;
        else if (k == "importDir") importDir = v;
        else if (k == "exportDir") exportDir = v;
        else if (k == "coverDir") coverDir = v;
        else if (k == "backupDir") backupDir = v;
        else if (k == "updates") updates = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "updateAtStart") updateAtStart = v == "1";
        else if (k == "updateToasted") updateToasted = v;
        else if (k == "betas") betas = v == "1";
        else if (k == "reports") reports = v == "1";
        else if (k == "lastfm") lastfm = v == "1";
        else if (k == "openingSound") openingSound = v == "1";
        else if (k == "opening") opening = v == "1";
        else if (k == "edgeMode") edgeMode = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "edgeExtra") edgeExtra = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "edgeCorners") edgeCorners = v == "1";
        else if (k == "uiSize") uiSize = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "serverTab") serverTab = std::clamp(atoi(v.c_str()), 0, 4);
        else if (k == "libSource") libSource = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "plSource") plSource = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "serverDlPlace") serverDlPlace = std::clamp(atoi(v.c_str()), 0, 1);
        else if (k == "lastfmScrobble") lastfmScrobble = v == "1";
        else if (k == "lastfmNow") lastfmNow = v == "1";
        else if (k == "connections") connections = v == "1";
        else if (k == "connectAim") connectAim = v;
        else if (k == "syncKinds") syncKinds = unsigned(atoi(v.c_str())) & 111u;
        else if (k == "syncLyricsKnown") lyricsKindKnown = true;
        else if (k == "syncMoodsKnown") moodsKindKnown = true;
        else if (k == "syncHand") syncAuto = v != "1";
        else if (k == "syncResults") syncResults = v == "1";
        else if (k == "syncFiles") syncFiles = v == "1";
        else if (k == "togetherOutput") togetherOutput = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "handoffFrom") handoffFrom = v;
        else if (k == "airplayReceive") airplayReceive = v == "1";
        else if (k == "airplaySpeakers") airplaySpeakers = v == "1";
        else if (k == "airSpeakerVol") {   // (lines joined with '|' in the file)
            airSpeakerVol = v;
            for (char& c : airSpeakerVol) if (c == '|') c = '\n';
        }
        else if (k == "togetherDelay0") togetherDelay[0] = std::clamp(atoi(v.c_str()), -kTimingMax, kTimingMax);
        else if (k == "togetherDelay1") togetherDelay[1] = std::clamp(atoi(v.c_str()), -kTimingMax, kTimingMax);
        else if (k == "togetherDelay2") togetherDelay[2] = std::clamp(atoi(v.c_str()), -kTimingMax, kTimingMax);
        else if (k == "togetherMeasured") for (int i = 0; i < 3 && i < int(v.size()); i++) togetherMeasured[i] = v[size_t(i)] == '1';
        else if (k == "syncDeletes") syncDeletes = v == "1";
        else if (k == "syncDeleteFrom") syncDeleteFrom = strtoull(v.c_str(), nullptr, 10);
    }
    if (!filesTabKnown) hiddenTabs |= 64u;   // Files stays hidden until Settings → Appearance shows it
    if (!lyricsKindKnown) syncKinds |= 32u;   // syncdata::KindLyrics: on, as the others came
    if (!moodsKindKnown) syncKinds |= 64u;    // syncdata::KindMoods: the same (the user, 2026-11-05: "Yes, with a switch")
    if (!linesKnown && std::none_of(tabOrder.begin(), tabOrder.end(), isLine)) {   // the line Files had, now one of yours
        auto f = std::find(tabOrder.begin(), tabOrder.end(), int(TabFiles));
        tabOrder.insert(f, TabLine);
    }
    gfEqClamp(&eq);
    playlists.clear();
    for (const auto& l : readLines(path("playlists.txt"))) {
        const size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        playlists.push_back({ l.substr(0, tab), l.substr(tab + 1), {} });
    }
    // Icons chosen: "icon<TAB>name<TAB>path" (a file of their own, so playlists.txt stays as older versions read it)
    for (const auto& l : readLines(path("playlist-icons.tsv"))) {
        const size_t a = l.find('\t'), b = a == std::string::npos ? a : l.find('\t', a + 1);
        if (b == std::string::npos) continue;
        const std::string name = l.substr(a + 1, b - a - 1), where = l.substr(b + 1);
        for (auto& p : playlists) if (p.name == name && p.path == where) p.icon = l.substr(0, a);
    }
    // Your own playlists' tracks: "#name" then one path a line (lists.txt).
    FolderPlaylist* cur = nullptr;
    for (const auto& l : readLines(path("lists.txt"))) {
        if (!l.empty() && l[0] == '#') {
            cur = nullptr;
            for (auto& p : playlists) if (p.own() && p.name == l.substr(1)) cur = &p;
        } else if (cur && !l.empty()) {
            cur->tracks.push_back(l);
        }
    }
    for (const auto& l : readLines(path("automix-lists.tsv"))) {   // Automix playlists: name, manual, waiting, pin
        std::vector<std::string> f;
        for (size_t a = 0, b; a <= l.size(); a = b + 1) {
            b = l.find('\t', a);
            if (b == std::string::npos) b = l.size();
            f.push_back(l.substr(a, b - a));
        }
        if (f.size() < 4) continue;
        for (auto& p : playlists)
            if (p.own() && p.name == f[0]) {
                p.automix = true;
                p.manual = f[1] == "1";
                p.waiting = f[2] == "1";
                p.pin = f[3];
            }
    }
    liked = readLines(path("liked.txt"));
    eqUser.clear();
    for (const auto& l : readLines(path("eq-presets.txt"))) {
        GfEqUser u{};
        if (gfEqParseUser(l.c_str(), &u) && eqUser.size() < GFEQ_USER_MAX) eqUser.push_back(u);
    }
    if (eq.preset >= GFEQ_PRESETS + int(eqUser.size())) gfEqMakeCustom(&eq);   // a preset deleted meanwhile: what it set stays, as Custom
    edits.clear();
    for (const auto& l : readLines(path("edits.tsv"))) {
        // path \t field=value \t field=value …
        size_t a = l.find('\t');
        if (a == std::string::npos) continue;
        auto& e = edits[l.substr(0, a)];
        while (a != std::string::npos) {
            const size_t b = l.find('\t', a + 1);
            const std::string kv = l.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) e[kv.substr(0, eq)] = kv.substr(eq + 1);
            a = b;
        }
    }
    airplayDevices.clear();
    for (const auto& l : readLines(path("airplay-devices.tsv"))) {
        const size_t tab = l.find('\t');
        if (tab != std::string::npos && tab > 0) airplayDevices.push_back({ l.substr(0, tab), l.substr(tab + 1) });
    }
    covers.clear();
    for (const auto& l : readLines(path("covers.tsv"))) {
        const size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        std::string file = l.substr(tab + 1);
#ifndef GF_WINDOWED
        // a chosen cover kept in the folder under its old name (a backup from before BETA 2, restored): the folder as it is now
        constexpr const char* kOld = "sdmc:/config/gatefold-nx/";
        if (file.rfind(kOld, 0) == 0 && std::string(Dir).rfind("sdmc:/config/gatefold-nx", 0) != 0) file = std::string(Dir) + "/" + file.substr(strlen(kOld));
#endif
        covers[l.substr(0, tab)] = file;
    }
    lyricNudge.clear();
    for (const auto& l : readLines(path("lyric-offsets.tsv"))) {
        const size_t tab = l.find('\t');
        if (tab != std::string::npos) lyricNudge[l.substr(0, tab)] = std::clamp(float(atof(l.c_str() + tab + 1)), -10.f, 10.f);
    }
    joins.clear();
    for (const auto& l : readLines(path("mixes.tsv"))) {   // the key's three fields, then the join
        const size_t tab = l.rfind('\t');
        if (tab != std::string::npos && tab > 0) joins[l.substr(0, tab)] = l.substr(tab + 1);
    }
}

/** mixes.tsv: "playlist<TAB>path A<TAB>path B<TAB>join", Automix's joins edited on playlist pages. */
void Store::saveJoins() const {
    std::string out;
    for (const auto& [k, j] : joins) {
        out += k;
        out += '\t';
        out += j;
        out += '\n';
    }
    writeFile("mixes.tsv", std::move(out));
}

/** lyric-offsets.tsv: "path<TAB>seconds", the lyrics nudge per track (written by the writer thread). */
void Store::saveLyricNudges() const {
    std::string out;
    for (const auto& [p, s] : lyricNudge) appendf(out, "%s\t%.1f\n", p.c_str(), double(s));
    writeFile("lyric-offsets.tsv", std::move(out));
}

/** eq-presets.txt (GFEQ_USER): your saved equalizer presets, as gfEqParseUser reads them. */
void Store::saveEqPresets() const {
    std::string out;
    for (const auto& u : eqUser) {
        appendf(out, "%s\t%.1f\t%.1f\t%.1f\t", u.name, double(u.preamp), double(u.bass), double(u.treble));
        for (int b = 0; b < GFEQ_BANDS; b++) appendf(out, b ? " %.1f" : "%.1f", double(u.bands[b]));
        out += '\n';
    }
    writeFile("eq-presets.txt", std::move(out));
}

void Store::saveAirplayDevices() const {
    std::string t;
    for (const auto& d : airplayDevices) {
        std::string n = d.name;
        for (char& c : n) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        t += d.id + "\t" + n + "\n";
    }
    writeFile("airplay-devices.tsv", std::move(t));
}

/** edits.tsv and covers.tsv, through the writer thread: next to the old file and swapped in, off the frame. */
void Store::saveEdits() const {
    auto clean = [](std::string s) {
        for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    };
    std::string ed;
    for (const auto& [p, e] : edits) {
        if (e.empty()) continue;
        ed += p;
        for (const auto& [k, v] : e) { ed += '\t'; ed += k; ed += '='; ed += clean(v); }
        ed += '\n';
    }
    writeFile("edits.tsv", std::move(ed));
    std::string cv;
    for (const auto& [p, file] : covers) { cv += p; cv += '\t'; cv += file; cv += '\n'; }
    writeFile("covers.tsv", std::move(cv));
}

void Store::save() const {
    makeDirs();
    std::string f;
    appendf(f, "volume=%.3f\norder=%d\nsidebarCollapsed=%d\nmotion=%d\nspectrum=%d\ncanvasLayout=%d\ncanvasLyrics=%d\n", volume, int(order),
            sidebarCollapsed, motion, showSpectrum, canvasLayout, canvasLyrics);
    appendf(f, "vizSensitivity=%.3f\nvizSmoothing=%.3f\nvizBands=%d\nvizPeaks=%d\ncoverPulse=%.3f\nseekWave=%d\n", vizSensitivity, vizSmoothing, vizBands,
            vizPeaks, coverPulse, seekWave);
    appendf(f, "canvasPulse=%d\nhiddenTabs=%u\n", canvasPulse, hiddenTabs);
    appendf(f, "canvasBars=%d\ncanvasIdleAfter=%.2f\ncanvasVizTime=%.2f\n", canvasBars, double(canvasIdleAfter), double(canvasVizTime));
    appendf(f, "canvasFloat=%d\ncanvasZoom=%d\nresume=%d\ntheme=%d\ncanvasAutoMinimal=%d\n", canvasFloat, canvasZoom, resume, theme,
            canvasAutoMinimal);
    appendf(f, "background=%d\n", background);
#ifdef GF_WINDOWED
    appendf(f, "window=%d %d %d %d %d\n", winX, winY, winW, winH, int(winMax));
#endif
    appendf(f, "brightness=%d\nsaturation=%.2f\ncolourTransition=%.2f\ncustomBg=%d\ncustomAccent=%d\ncustomAccent2=%d\n", brightness, double(saturation),
            double(colourTransition), customBg, customAccent, customAccent2);
    appendf(f, "customMode=%d\n", customMode);
    appendf(f, "viewPlaylist=%d\nviewFolders=%d\nviewTracks=%d\nlibTab=%d\ntracksSort=%d\ntracksDesc=%d\n", viewPlaylist, viewFolders, viewTracks, libTab,
            tracksSort, tracksDesc);
    appendf(f, "pointerSpeed=%.2f\npointerAccel=%d\nframeTimes=%d\nviewGenre=%d\n", double(pointerSpeed), pointerAccel, frameTimes, viewGenre);
    appendf(f, "keepAwake=%d\ndimAfter=%d\nsleepConsole=%d\nswipeKeep=%d\n", keepAwake, dimAfter, sleepConsole, swipeKeep);
    if (!automixModel.empty()) appendf(f, "automixModel=%s\n", automixModel.c_str());
    if (!modelToasted.empty()) appendf(f, "modelToasted=%s\n", modelToasted.c_str());
    if (heldOrder >= 0) appendf(f, "heldOrder=%d\n", heldOrder);
    appendf(f, "mixAnimNow=%d\nmixAnimCanvas=%d\nmixHint=%d\n", mixAnimNow, mixAnimCanvas, mixHint);
    appendf(f, "batteryPercent=%d\nbatteryEstimate=%d\n", batteryPercent, batteryEstimate);
    appendf(f, "batteryWatts=%d\nbatteryTemp=%d\nbatteryVolts=%d\nbatteryHealth=%d\nbatteryFahrenheit=%d\nbatteryLowAt=%d\nbatteryAnimPace=%d\nbatteryPower=%d\n",
            batteryWatts, batteryTemp, batteryVolts, batteryHealth, batteryFahrenheit, batteryLowAt, batteryAnimPace, batteryPower);
    {   // (a letter each for the states' colours and animations: there are more than ten colours)
        std::string o, c, l;
        for (int i : batteryOrder) o += char('0' + i);
        for (int i = 0; i < BatteryStates; i++) { c += char('a' + batteryCol[i]); l += char('a' + batteryLook[i]); }
        appendf(f, "batteryOrder=%s\nbatteryCol=%s\nbatteryLook=%s\nbatteryOutline=%d\n", o.c_str(), c.c_str(), l.c_str(), batteryOutline);
    }
    appendf(f, "stopCovers=%d\nstopThumbs=%d\nstopLevels=%d\nstopLyrics=%d\nstopDownloads=%d\n", stopCovers, stopThumbs, stopLevels, stopLyrics,
            stopDownloads);
    appendf(f, "stopTagWrites=%d\ntagWrite=%d\nstopMixOrder=%d\n", stopTagWrites ? 1 : 0, tagWrite, stopMixOrder ? 1 : 0);
    appendf(f, "pocketButton=%d\npocketHint=%d\npocketKey=%d\npocketHow=%d\npocketHold=%d\n", pocketButton ? 1 : 0, pocketHint ? 1 : 0, pocketKey,
            pocketHow, pocketHold);
    appendf(f, "pocketPads=%d\n", pocketPads ? 1 : 0);
    appendf(f, "lyricTiming=%d\nlyricTimingFor=%d\n", lyricTiming, lyricTimingFor);
    appendf(f, "lyricsOnline=%d\nlyricsOnlineWay=%d\nlyricsSaveTo=%d\n", lyricsOnline, lyricsOnlineWay, lyricsSaveTo);
    appendf(f, "levelMode=%d\nlevelTarget=%d\n", levelMode, levelTarget);
    appendf(f, "crossfade=%.1f\nxfadeNotInAlbum=%d\n", double(crossfade), xfadeNotInAlbum);
    appendf(f, "eqEdited=%d\n", eq.edited);
    appendf(f, "eqOn=%d\neqAdvanced=%d\neqPreset=%d\neqPreamp=%.1f\neqBass=%.1f\neqTreble=%.1f\neqBands=", eq.on, eq.advanced, eq.preset,
            double(eq.preamp), double(eq.bass), double(eq.treble));
    for (int b = 0; b < GFEQ_BANDS; b++) appendf(f, b ? " %.1f" : "%.1f", double(eq.bands[b]));
    appendf(f, "\n");
    if (eq.hasCustom) {
        appendf(f, "eqCustom=");
        for (int b = 0; b < GFEQ_BANDS; b++) appendf(f, b ? " %.1f" : "%.1f", double(eq.custom[b]));
        appendf(f, "\n");
    }
    if (!musicRoot.empty()) appendf(f, "musicRoot=%s\n", musicRoot.c_str());
    appendf(f, "tabOrder=");
    for (size_t i = 0; i < tabOrder.size(); i++) appendf(f, i ? ",%d" : "%d", tabOrder[i]);
    appendf(f, "\ntabLines=1");   // knows placed lines (Store::load)
    appendf(f, "\n");
    appendf(f, "showHidden=%d\nfilesTab=1\nviewPlaylists=%d\n", showHidden, viewPlaylists);
    appendf(f, "viewGenres=%d\nviewArtists=%d\nviewAlbums=%d\nletterRails=%u,%u\n", viewGenres, viewArtists, viewAlbums, letterRails[0], letterRails[1]);   // filesTab: hiddenTabs already knows the Files tab (bit 6)
    for (const auto& [k, v] : { std::pair<const char*, const std::string*>{ "filesDir", &filesDir }, { "importDir", &importDir },
                                { "exportDir", &exportDir }, { "coverDir", &coverDir }, { "backupDir", &backupDir } })
        if (!v->empty()) appendf(f, "%s=%s\n", k, v->c_str());
    appendf(f, "updates=%d\nupdateAtStart=%d\nbetas=%d\nreports=%d\n", updates, updateAtStart, betas ? 1 : 0, reports ? 1 : 0);
    appendf(f, "lastfm=%d\nlastfmScrobble=%d\nlastfmNow=%d\nopeningSound=%d\nopening=%d\n", lastfm ? 1 : 0, lastfmScrobble ? 1 : 0, lastfmNow ? 1 : 0, openingSound ? 1 : 0,
            opening ? 1 : 0);
    appendf(f, "edgeMode=%d\nedgeExtra=%.3f\nedgeCorners=%d\nuiSize=%d\nserverTab=%d\nlibSource=%d\nserverDlPlace=%d\nplSource=%d\n", edgeMode, edgeExtra, edgeCorners ? 1 : 0, uiSize, serverTab, libSource, serverDlPlace,
            plSource);
    appendf(f, "handoffFrom=%s\nairplayReceive=%d\nairplaySpeakers=%d\n", handoffFrom.c_str(), airplayReceive ? 1 : 0, airplaySpeakers ? 1 : 0);
    {
        std::string v = airSpeakerVol;
        for (char& c : v) if (c == '\n' || c == '\r') c = '|';
        appendf(f, "airSpeakerVol=%s\n", v.c_str());
    }
    appendf(f, "togetherOutput=%d\ntogetherDelay0=%d\ntogetherDelay1=%d\ntogetherDelay2=%d\n", togetherOutput, togetherDelay[0], togetherDelay[1], togetherDelay[2]);
    appendf(f, "togetherMeasured=%d%d%d\n", int(togetherMeasured[0]), int(togetherMeasured[1]), int(togetherMeasured[2]));
    appendf(f, "syncLyricsKnown=1\nsyncMoodsKnown=1\n");
    appendf(f, "connections=%d\nconnectAim=%s\nsyncKinds=%u\nsyncHand=%d\nsyncResults=%d\nsyncFiles=%d\nsyncDeletes=%d\nsyncDeleteFrom=%llu\n", connections ? 1 : 0, connectAim.c_str(), syncKinds, syncAuto ? 0 : 1, syncResults ? 1 : 0, syncFiles ? 1 : 0, syncDeletes ? 1 : 0, (unsigned long long)syncDeleteFrom);
    if (!updateToasted.empty()) appendf(f, "updateToasted=%s\n", updateToasted.c_str());
    writer().put(path("settings.txt"), std::move(f), true);

    std::string pl;
    for (const auto& p : playlists) appendf(pl, "%s\t%s\n", p.name.c_str(), p.path.c_str());
    writer().put(path("playlists.txt"), std::move(pl), true);
    std::string icons;
    for (const auto& p : playlists) if (!p.icon.empty()) appendf(icons, "%s\t%s\t%s\n", p.icon.c_str(), p.name.c_str(), p.path.c_str());
    writer().put(path("playlist-icons.tsv"), std::move(icons), true);
    std::string own;
    for (const auto& p : playlists) {
        if (!p.own()) continue;
        appendf(own, "#%s\n", p.name.c_str());
        for (const auto& t : p.tracks) appendf(own, "%s\n", t.c_str());
    }
    writer().put(path("lists.txt"), std::move(own), true);
    // Automix playlists: "name<TAB>manual<TAB>waiting<TAB>pin" (a file of their own, so older versions read the rest)
    std::string am;
    for (const auto& p : playlists)
        if (p.own() && p.automix) appendf(am, "%s\t%d\t%d\t%s\n", p.name.c_str(), p.manual ? 1 : 0, p.waiting ? 1 : 0, p.pin.c_str());
    writer().put(path("automix-lists.tsv"), std::move(am), true);
    std::string lk;
    for (const auto& p : liked) appendf(lk, "%s\n", p.c_str());
    writer().put(path("liked.txt"), std::move(lk), true);
}

void Store::flush() { writer().flush(); }
void Store::stopWriter() { writer().stop(); }

void Store::writeFile(const char* file, std::string data) const {
    makeDirs();
    writer().put(path(file), std::move(data), true);
}

void Store::removeFile(const char* file) const { writer().put(path(file), std::string(), false, true); }

bool Store::loadSession(Session& s) const {
    s = Session{};
    for (const auto& l : readLines(path("session.txt"))) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "track") s.tracks.push_back(v);
        else if (k == "name") s.name = v;
        else if (k == "pos") s.pos = atoi(v.c_str());
        else if (k == "position") s.position = std::max(0.0, atof(v.c_str()));
        else if (k == "order") {
            const char* p = v.c_str();
            char* end = nullptr;
            for (long n = strtol(p, &end, 10); end != p; n = strtol(p, &end, 10)) { s.order.push_back(int(n)); p = end; }
        }
    }
    return !s.tracks.empty();
}

void Store::saveSession(const Session& s) const {
    makeDirs();
    std::string f;
    appendf(f, "name=%s\npos=%d\nposition=%.2f\norder=", s.name.c_str(), s.pos, s.position);
    for (size_t i = 0; i < s.order.size(); i++) appendf(f, i ? " %d" : "%d", s.order[i]);
    f += "\n";
    for (const auto& t : s.tracks) appendf(f, "track=%s\n", t.c_str());
    writer().put(path("session.txt"), std::move(f), true);
}

/** name, or "name 2", "name 3" …: playlists are known by name (lists.txt, the game actions' lists, the overlay). */
std::string Store::freePlaylistName(std::string name) const {
    if (name.empty()) name = "Playlist";
    const std::string base = name;
    for (int n = 2;; n++) {
        bool taken = name == "Liked";   // the background player and the overlay know Liked by that name
        for (const auto& p : playlists) taken |= p.name == name;
        if (!taken) return name;
        name = base + " " + std::to_string(n);
    }
}

int Store::newPlaylist(std::string name) {
    playlists.push_back({ freePlaylistName(std::move(name)), "", {} });
    return int(playlists.size()) - 1;
}

bool Store::isLiked(const std::string& p) const { return std::find(liked.begin(), liked.end(), p) != liked.end(); }

void Store::toggleLiked(const std::string& p) {
    auto it = std::find(liked.begin(), liked.end(), p);
    if (it == liked.end()) liked.push_back(p);
    else liked.erase(it);
}

int Store::playlistByPath(const std::string& p) const {
    for (int i = 0; i < int(playlists.size()); i++)
        if (strcasecmp(playlists[i].path.c_str(), p.c_str()) == 0) return i;
    return -1;
}
