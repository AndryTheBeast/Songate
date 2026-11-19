/*
 * Gatefold NX · store.hpp
 * Everything that survives a restart, as small text files in sdmc:/config/songate-nx/.
 */
#pragma once
#include <map>
#include <string>
#include "platform.hpp"
#include <unordered_map>
#include <vector>

#include "gfeqcfg.h"

/** A playlist in the sidebar: a folder kept in sync with the library, your own list of tracks, or a smart one. */
struct FolderPlaylist {
    std::string name;
    std::string path;                  // a folder playlist: every track below this folder; empty for your own;
                                       // "smart:<recipe>:<setting>" for a smart one (view_smart.cpp)
    std::vector<std::string> tracks;   // your own: track paths, in your order
    std::string icon;                  // its icon's key (plicons.inc), empty for its kind's own (playlist-icons.tsv)
    // An Automix playlist (one of your own; automix-lists.tsv): Gatefold orders it for the best joins (automix::order),
    // again whenever tracks are added, until it is moved by hand (manual) and "Order again" is pressed; pin: the track
    // that stays first (its path; empty: Gatefold chooses); waiting: ordered before all of it was analysed
    bool automix = false, manual = false, waiting = false;
    std::string pin;
    bool own() const { return path.empty(); }
    bool smart() const { return path.rfind("smart:", 0) == 0; }
    bool folder() const { return !own() && !smart(); }
};

enum class Order { Default, RepeatAll, RepeatOne, Shuffle };

/** "Resume where I left off": the play queue by path, so it survives a rescan. */
struct Session {
    std::string name;
    std::vector<std::string> tracks;
    std::vector<int> order;
    int pos = -1;          // index into order
    double position = 0;   // seconds into the current track
};

struct Store {
    static inline const char* const Dir = plat::configDir();

    std::string musicRoot;   // the folder the library is read from; empty until chosen or found
    std::vector<FolderPlaylist> playlists;
    std::vector<std::string> liked;   // track paths
    float volume = 0.5f;   // first-run defaults: the user's choices (2026-10-28, NEXT_SESSION #18)
    Order order = Order::RepeatAll;
    bool sidebarCollapsed = false;
    bool motion = true;
    bool keySound = true;   // Settings › Controls › Keyboard: a click as each key is pressed
    int keySoundKind = 0;   // … which: 0 Soft, 1 Tick, 2 Pop, 3 Clack (keyclick.hpp; the user, 2026-11-18)
    bool keyVibe = true;    // … and its buzz,
    int keyVibeLevel = 1;   // … how strong: 1 light, 2 medium, 3 strong (the user, 2026-11-18)
    // Settings › Appearance › Automix (the user, 2026-10-29): how a join shows. Now playing: 0 Full (the interface steps
    // aside, the cover glides to the screen's middle), 1 Immersive (the cover to the page's middle, the bars stay), 2 Simple
    // (all in place: the cover frosts, the texts wipe from one song to the next). The Canvas: 0 Full, 1 Simple
    int mixAnimNow = 0, mixAnimCanvas = 0;
    int mixHint = 0;   // the Automix hint: 0 Automatic (each animation's own place), 1 on the cover, 2 above it
    bool showSpectrum = true;
    int canvasLayout = 0;     // 0 cover, 1 lyrics, 2 minimal
    bool canvasLyrics = true;
    bool lyricTiming = false;  // Settings → Lyrics: the timing control (Earlier · 0.0 s · Later) on Now playing and the Canvas
    int lyricTimingFor = 2;    // ... for lyrics from 0 the tags, 1 .lrc files, 2 both
    // Settings → Lyrics → Online (BETA 3): a song with none looks on LRCLIB; what is found plays at once (0) or is asked about
    // first (1); kept beside the song (0) or in Songate's own folder (1)
    bool lyricsOnline = false;
    int lyricsOnlineWay = 0;
    int lyricsSaveTo = 0;          // 0 beside the song, 1 Songate's folder, 2 inside the song (by way of the folder)
    bool resume = true;
    bool background = true;   // play through the background player (sysmodule) when it is installed
#ifdef GF_WINDOWED
    int winX = 0, winY = 0, winW = 0, winH = 0;   // where the window was left
    bool winMax = false;
#endif
    // The list views (ListMode) of the playlists, Folders and Library → Tracks, and the Library's tab
    int viewPlaylist = 1, viewFolders = 2, viewTracks = 1, viewGenre = 1;   // viewGenre: a genre's or an artist's Tracks tab
    int viewPlaylists = 2;     // the Playlists page: tiles
    int viewGenres = 2;        // the Genres page: tiles, rows with covers, the plain list (as Folders')
    int viewArtists = 2;       // the Artists page: the same three
    int viewAlbums = 2;        // the Albums pages and search's Albums: the same three (the user, 2026-11-09)
    unsigned letterRails[2] = { 31, 31 };      // the #-to-Z rail per list, for each library (Local, Server): 1 Albums, 2 Artists,
                                                // 4 Genres, 8 Folders, 16 Tracks (32 the Playlists page and 64 a playlist's page were dropped: your order)
    int libTab = 1;            // 0 Albums, 1 Tracks, 2 Folders, 3 Artists, 4 Genres (in Library too: the sidebar's may be hidden)
    int tracksSort = 0;        // by artist, title, album, year, genre, length
    bool tracksDesc = false;
    int theme = 0;                  // 0 from the cover, 1 Dark, 3 Light (theme.js FIXED), 4 Custom; 2 was Grey, read as Dark
    int brightness = 0;             // From the cover: 0 dark, 1 light (palette-core.js opts.mode)
    // Background blur (2.0.1): how the cover is blurred behind every page and the Canvas. 0 Classic: 12x12 points of it stretched,
    // its patches showing (as it always was; the user likes it, "but some people might not"); 1 Smooth, as smooth as bgSmooth says
    int bgBlur = 1;   // (Smooth, Light by default: the user, 2026-11-18)
    // Screen rotation (2.0.1, Android only): 0 Auto, with the device; 1 Portrait; 2 Landscape (plat::screenRotation)
    int screenTurn = 0;
    // Settings' folded cards (session 21): each page's open card by its title ("" all folded; a page not here: its first card)
    std::map<std::string, std::string> foldOpen;
    // Show Apps (2.0.1, Android only): the device's other apps as a sixth button of the upright bottom bar, while Songate is the
    // home screen (Settings › Home screen)
    bool appsTab = false;
    // The Apps page's own settings (session 21, its gear): Order (0 your own, 1 A to Z, 2 most used), Tile size (0 small, 1 medium,
    // 2 large), Icons (0 Songate's, 1 their own), Names under the tiles, Find an app's field, Only the apps you installed
    // System colours (SongateOS, Appearance): Android's accent set from Songate's own (as it was when chosen), not a colour of
    // your own; the service keeps the colour itself
    bool sysThemeSongate = false;
    int appsOrder = 0, appsSize = 1, appsIcons = 0;
    bool appsNames = true, appsFind = true, appsMine = false;
    // Settings › Playback › Other apps (Android): what another app plays shows in Songate. On by default on SongateOS (the user,
    // 2026-11-19), off elsewhere: otherAppsSaved says whether settings.txt had it ("otherMusic"; the older "otherApps" key, saved
    // while it was off for everyone, is left unread)
    bool otherApps = false, otherAppsSaved = false;
    // Settings › Playback › Other apps › Spectrum for other apps (the user chose B, asked in place, 2026-11-19): the bars follow
    // another app's music (Android's audio recording permission); otherSpecAsked: Now playing's Allow / Not now answered
    bool otherSpectrum = false, otherSpecAsked = false;
    // Sidebar opacity (2.0.1): how solid the sidebar's ground is when it opens over the page, 0.5 to 1 (solid, as it was). Only a
    // screen too narrow for the sidebar beside the page opens it so (a small player held sideways, a narrow window)
    float sideOpacity = 1.f;
    int bgSmooth = 0;               // Smooth's strength: 0 Light, 1 Medium, 2 Strong
    float saturation = 1;           // opts.intensity, 0..1 (higher changes nothing)
    float colourTransition = 1.1f;  // seconds the colours take to follow a new cover (Gatefold's --tx)
    // Custom: your background and accents as 0xRRGGBB, −1 when left to the cover (theme.js customTheme)
    int customBg = 0x1a1720, customAccent = 0xff5a6a, customAccent2 = -1;
    int customMode = 2;   // Custom's brightness: 0 dark, 1 light (your colours as a cover's), 2 the background as picked
    bool canvasAutoMinimal = true;  // Minimal while a track has no lyrics
    bool canvasFloat = true;   // the cover's slow float
    bool canvasZoom = true;    // its Ken Burns zoom
    // Settings → Visualizer (defaults are Gatefold's, except a calmer cover pulse)
    float vizSensitivity = 0.5f;
    float vizSmoothing = 0.f;
    int vizBands = 56;
    bool vizPeaks = true;
    // Android upright, Settings › Visualizer › Seek bar › Seek bar animation: how the bars come back after scrubbing. 0 Quick (half a
    // second, all together), 1 Smooth (longer and gentler: the default, the user's choice), 2 Wave (out from the song's place)
    int seekAnim = 1;
    // Android upright, Settings › Storage › Files › Files in Library (the user, 2026-11-16): the Files row on the Library page, between
    // Folders and Liked; its own switch (it was the sidebar's Files tab's, whose card goes when the screen is held upright). -1:
    // not saved yet: as the sidebar's Files tab is
    int libFiles = -1;
    // Android, Settings › Playback › Volume (2.1.0; the volume is Android's own music volume): what the volume keys show (0 Songate's
    // own pop-up, 1 Android's panel), where Songate's shows (0 Top, 1 Side, 2 Bottom), and the volume as 0 steps or 1 percent
    int volPop = 0, volWhere = 1, volNum = 0;
    float volStay = 1.5f;   // ... and how long Songate's stays after the last press, in seconds (the user, 2026-11-17)
    // Android upright, Settings › Lyrics › Under the cover: 0 off, 1 the line being sung in the buttons' place, 2 with the next one
    int lyrUnder = 1;
    // The HiBy R4 only, Appearance › Status LED › Colour "A": the light follows Songate's accent, set again with each cover
    // (the ROM keeps the colour itself; this is Songate's own part of it). Saved only while it is on: no other device keeps a line
    bool r4LedAccent = false;
    // The HiBy R4, Sound & DAC › Output › Bit-perfect output (on by default; saved only when off: no other device keeps a line)
    bool bitPerfect = true;
    // The HiBy R4, Sound & DAC › Output › Each song at its own rate (SongateOS's out_rate; on by default, saved only when off)
    bool matchRate = true;
    bool seekWave = true;         // Settings › Visualizer › Seek bar › Waveform: the song's loudness as bars; off, a plain line
    float coverPulse = 1.f;     // 1 = Gatefold's breathing
    bool canvasPulse = true;    // the pulse on the Canvas cover too
    int canvasBars = 112;           // the Canvas's idle spectrum (Now playing has vizBands)
    float canvasIdleAfter = 3.5f;   // seconds without input before the Canvas controls fade
    float canvasVizTime = 0.9f;     // seconds the progress line takes to grow into the spectrum
    // Settings → Controls: the left stick's pointer. Speed 0..1 (0.5 is 0.6's pace); acceleration: slow near the centre, fast at the edge
    float pointerSpeed = 0.2f;   // Settings shows it doubled: 40%
    bool pointerAccel = true;
    bool frameTimes = false;        // Settings → About: the frame-time graph
    int heldOrder = -1;             // Shuffle, set aside while an Automix playlist plays (App::holdOrderForAutomix); -1 none
    std::string automixModel;       // the Automix model by its name ("Automix V1.2.0"): Settings › Updates › Automix models (a
                                    // downloaded one, or the built-in); Settings › Debug's built-in ones in dev builds; empty: the newest built-in
    std::string modelToasted;       // the downloadable model announced at launch (once each)
    bool swipeKeep = true;          // Android: swiped away from the recent apps while music plays, the music goes on (off: it ends)
    bool keepAwake = true;          // while Gatefold is open: no dimming and no auto-sleep (until a sleep timer ends)
    int dimAfter = 0;               // … but dim the screen after this many minutes without input; 0 off
    bool batteryPercent = true;     // the title bar: the battery's percentage
    bool batteryEstimate = true;    // ... and how long to full or to empty
    // the battery (BETA 3; the user, 2026-11-05: "Let the user customise almost everything about the battery"): what else shows beside
    // it, its colour, and each of its animations
    bool batteryWatts = false, batteryTemp = false, batteryVolts = false, batteryHealth = false, batteryFahrenheit = false;
    bool batteryPower = false;      // what the battery gives or takes now, in watts (the fuel gauge: the Switch)
    // what shows beside it, in your order (the user, 2026-11-05: "sortable from settings like the side bar is"): 0 percentage, 1 time,
    // 2 charger watts, 3 temperature, 4 voltage, 5 health, 6 power
    std::vector<int> batteryOrder{ 0, 1, 2, 3, 4, 5, 6 };
    // the Canvas's own (the user, 2026-11-11: "battery information and icon in canvas too", "a setting to switch between canvas
    // info and home info"): what shows beside it there (the same seven, by their numbers), in which order, and whether it leaves
    // with the controls when the Canvas goes idle ("remove the battery on the canvas setting and replace with Fade when Canvas
    // goes idle"; off: it stays)
    bool cvBatteryFade = false;
    bool cvBatteryShow[7]{ true, false, false, false, false, false, false };
    std::vector<int> cvBatteryOrder{ 0, 1, 2, 3, 4, 5, 6 };
    // each state's colour and animation ("choose colour for each state, charging animation (by how fast is charging), discharging
    // animation, animation when its fully charged"; "consistent with the UI and magical too"; "a gradual gradient from full/empty";
    // "the accent color from the cover or theme pallete"). States: 0 on battery, 1 low, 2 charging slowly, 3 charging,
    // 4 charging fast, 5 charged. Colours: 0 as the text, 1 the accent, 2 the palette's gradient (the seek bar's), 3 by charge (red at
    // empty, yellow, green at full, gradually), 4 green, 5 yellow, 6 orange, 7 red, 8 blue, 9 purple, 10 pink.
    // Animations: 0 none, 1 sweep, 2 pulse, 3 glow, 4 fill up, 5 blink, 6 sparkle, 7 shimmer.
    static constexpr int BatteryStates = 6, BatteryColours = 11, BatteryLooks = 8;
    int batteryCol[BatteryStates]{ 0, 7, 0, 0, 0, 0 };
    int batteryLook[BatteryStates]{ 0, 2, 1, 1, 1, 3 };
    int batteryOutline = -1;        // its edge (the user, 2026-11-05: "edit the color of ... the little outline"): -1 as each state's
                                    // fill, else one of the colours above for every state
    int batteryLowAt = 15;          // low at or under this (10, 15, 20, 30)
    int batteryAnimPace = 1;        // 0 calm, 1 normal, 2 lively
    int sleepConsole = 30;   // seconds of the "going to sleep" card after the sleep timer ends; 0: do not sleep the console
    // Background work stopped by hand (the progress stack's menu, Settings) until resumed: the cover cache, the overlay's covers, measuring loudness
    bool stopCovers = false, stopThumbs = false, stopLevels = false, stopLyrics = false, stopDownloads = false;
    bool stopTagWrites = false;   // writing tag and cover changes into the files, stopped until resumed
    // the pocket lock (the Switch; Settings › Controls): its button on Now playing; the unlock: 0 Volume −, 1 Volume +, 3 Capture (twice); 2 either is only the fallback when Capture is refused,
    // held (pocketHold seconds) or pressed twice; a hint lit on a press while locked
    bool pocketButton = true, pocketHint = true;
    bool pocketPads = false;   // the controllers off the console turned off at the lock
    int pocketKey = 0, pocketHow = 0, pocketHold = 2;
    int tagWrite = 0;             // a tag or cover change (Settings › Library): 0 asks each time, 1 Songate only, 2 into the file too
    int levelMode = 0;              // volume levelling: 0 off (the default: the user chooses), 1 track, 2 album, 3 auto (GfbgLevel)
    int levelTarget = -14;          // ... to this loudness, LUFS
    // Settings → Playback → Crossfade: seconds (0 off), and whether consecutive tracks of one album keep their join
    float crossfade = 0;
    bool xfadeNotInAlbum = true;
    // Settings › Playback › Play when plugged in (the HiBy R4 only; the user, 2026-11-20): headphones into the 3.5 or 4.4 mm
    // start the music where it is, every time. Off unless chosen
    bool playOnPlug = false;
    // Settings → Equalizer (gfeqcfg.h); with the background player connected, the player's own copy wins (the overlay edits it too)
    GfbgEq eq = gfEqDefault();
    struct AirPlayDevice { std::string id, name; };
    std::vector<AirPlayDevice> airplayDevices;   // the devices that play here over AirPlay without asking (airplay-devices.tsv)
    void saveAirplayDevices() const;
    std::vector<GfEqUser> eqUser;   // your saved presets (GFEQ_USER, eq-presets.txt): eq.preset GFEQ_PRESETS + k is eqUser[k]

    // Your changes, kept by Gatefold with the music files untouched (the console's FFmpeg cannot write files):
    std::unordered_map<std::string, std::map<std::string, std::string>> edits;   // track path → tag field → value
    std::unordered_map<std::string, std::string> covers;                         // track path → image in Dir/covers
    std::unordered_map<std::string, float> lyricNudge;                           // track path → seconds its synced lyrics move (+ later)
    // Automix's joins edited on a playlist page: "playlist<TAB>path A<TAB>path B" → automix::Join::pack() (mixes.tsv)
    std::map<std::string, std::string> joins;
    static std::string joinKey(const std::string& playlist, const std::string& a, const std::string& b) { return playlist + '\t' + a + '\t' + b; }
    // Sidebar tabs hidden in Settings → Appearance: bit 0 Playlist, 1 Artists, 2 Genres, 3 Library, 4 Folders, 5 Albums, 6 Files
    // (Files is hidden until shown: a settings.txt from before it existed gets bit 6 too, Store::load)
    unsigned hiddenTabs = 64;
    // The sidebar's tabs in your order (Settings → Appearance → Sidebar); Settings stays at the bottom
    enum Tab { TabNow, TabQueue, TabAlbums, TabArtists, TabGenres, TabFolders, TabLibrary, TabFiles, TabServer, TabApps, TabCount };   // TabServer: a Subsonic server's (shown while signed in)
    // Lines between the tabs, placed as you like (Settings → Appearance → Sidebar: Add a line, Move, Remove): ids TabLine + k
    static constexpr int TabLine = 100, MaxLines = 4;
    static bool isLine(int tab) { return tab >= TabLine && tab < TabLine + MaxLines; }
    // TabApps is Android's alone (the device's apps, while Songate is its home screen): no other build keeps it in its order
#ifdef GF_ANDROID
    static constexpr bool HasAppsTab = true;
#else
    static constexpr bool HasAppsTab = false;
#endif
    static bool tabExists(int tab) { return tab >= 0 && tab < TabCount && (tab != TabApps || HasAppsTab); }
    static unsigned tabBit(int tab) {   // its bit in hiddenTabs (0.4's numbering); Now playing and lines have none
        static const unsigned bits[TabCount] = { 0, 1, 32, 2, 4, 16, 8, 64, 128, 256 };
        return tab >= 0 && tab < TabCount ? bits[tab] : 0;
    }
    static const char* tabName(int tab) {
        static const char* names[TabCount] = { "Now playing", "Playlist", "Albums", "Artists", "Genres", "Folders", "Library", "Files", "Server", "Apps" };
        return tab >= 0 && tab < TabCount ? names[tab] : "Line";
    }
    // (a line above Files by default: it had one of its own before lines could be placed)
#ifdef GF_ANDROID
    std::vector<int> tabOrder = { TabNow, TabQueue, TabAlbums, TabArtists, TabGenres, TabFolders, TabLibrary, TabServer, TabApps, TabLine, TabFiles };
#else
    std::vector<int> tabOrder = { TabNow, TabQueue, TabAlbums, TabArtists, TabGenres, TabFolders, TabLibrary, TabServer, TabLine, TabFiles };
#endif
    bool showHidden = false;   // Files: names starting with a dot
    // Settings → Storage and reset → Folders: where each browser starts (empty: its own default)
    std::string filesDir, importDir, exportDir, coverDir, backupDir;
    // Settings → Updates: 0 off (nothing goes online; the default until you allow it), 1 allowed; a check at launch;
    // the newer version already announced at launch (once per version)
    int updates = 0;
    bool updateAtStart = false;
    bool betas = false;   // Beta versions: listed (apart) and offered; asked first (App::allowBetas)
    bool reports = false; // Settings › Support › Reports: off, nothing can be sent; on, asked first (App::allowReports)
    std::string updateToasted;
    // Settings › Connections: off, no socket is open; on, asked first (App::allowConnections). connectAim: a typed address
    bool connections = false;
    std::string connectAim;
    unsigned syncKinds = 111;       // syncdata::Kind: what is shared, each with its own switch (32: lyrics, 64: mood choices, BETA 3)
    bool syncResults = true;        // measurements and covers, for exactly the same files
    bool syncFiles = false;         // take the songs this device lacks: off until you turn it on
    int togetherOutput = 0;         // listening together: 0 speakers, 1 headphones, 2 Bluetooth
    bool airplayReceive = false;    // an iPhone may play here: off until you turn it on
    bool airplaySpeakers = true;    // this device may play on AirPlay speakers: the title bar's list looks for them
    std::string airSpeakerVol;      // AirPlay speakers this device played to, and how loud: "id\tvolume" a line (airsend.hpp)
    std::string handoffFrom;        // devices whose handoffs are taken without asking, by their ids
    static constexpr int kTimingMax = 1000;   // ms either way: a Bluetooth speaker was 415 ms behind (2026-11-05), past the ±300 before
    bool togetherMeasured[3] = {};  // each output's delay measured with the chirps once (micmeasure.hpp): not measured again by itself
#ifdef __SWITCH__
    int togetherDelay[3] = { 39, 0, 0 };   // ms this device plays ahead; 39: the console's speakers against the PC's, measured (session 10)
#else
    int togetherDelay[3] = { 0, 0, 0 };
#endif
    bool syncDeletes = false;       // a song deleted on another device is deleted here too: off until you turn it on, and asked
    uint64_t syncDeleteFrom = 0;    // … only those deleted after that (~0: turned on just now, the next sync sets it)
    bool syncAuto = true;           // on link and after any change (the user, 2026-10-31); false only by hand
    bool fresh = false;         // load() found no settings.txt: a new user (the HiBy R4's own defaults then: App::init)
    bool opening = true;        // Settings › Playback › Startup: the star and the name as Songate opens (view_opening.cpp)
    bool openingSound = true;   // … and its bells (only with the opening on)
    // Settings › Appearance › Screen edges (Android): the camera's cutouts and the screen's rounded corners (App::updateEdges)
    int edgeMode = 0;           // 0 Step aside, 1 Keep the side clear, 2 Black strip, 3 Use the whole screen
    float edgeExtra = 0;        // 0..1: up to 24 px more room around a cutout
    bool edgeCorners = true;    // keep clear of the rounded corners too
    int serverDlPlace = 0;      // Settings › Library › Subsonic server › Downloads: 0 Songate's folder, 1 the music folder (Songate downloads)
    int plSource = 0;           // the Playlists page and the sidebar's list: 0 yours, 1 the server's (shown only while signed in)
    int libSource = 0;          // Library's Local | Server (the user, 2026-11-06: "two options, Local or Server")
    int serverTab = 0;          // the Server tab's view: 0 Albums, 1 Artists, 2 Genres, 3 Folders, 4 Tracks
    int uiSize = 1;             // Settings › Appearance › Layout › Size (Android), sideways: 0 Standard (720 tall), 1 Medium (660, the default), 2 Largest (600)
    int uiSizeUp = 0;           // … and upright's own (the user, 2026-11-15: "each layout has it's own layout size"): 0 Standard (the default), 1 Medium, 2 Largest

    void load();
    /** Queued for a background thread (store.cpp); flush() waits until everything is on the card. */
    void save() const;
    static void flush();
    /** At exit, after everything is saved: the writer's thread ends (a later write starts it again). */
    static void stopWriter();
    /** Any file of ours, written on the same thread (and atomically: next to it, then renamed over it). */
    void writeFile(const char* file, std::string data) const;
    void removeFile(const char* file) const;   // on the writer thread, after what is queued for it
    void saveEdits() const;
    void saveLyricNudges() const;
    void saveJoins() const;
    void saveEqPresets() const;
    bool loadSession(Session& s) const;
    void saveSession(const Session& s) const;
    std::string path(const char* file) const { return std::string(Dir) + "/" + file; }

    bool isLiked(const std::string& p) const;
    void toggleLiked(const std::string& p);
    int playlistByPath(const std::string& p) const;
    /** A new playlist of your own, its name made unique; returns its index. */
    std::string freePlaylistName(std::string name) const;
    int newPlaylist(std::string name);
};
