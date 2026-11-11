/*
 * Gatefold NX · app.hpp
 * Application state and the views. Layout values are Gatefold's CSS (app.css)
 * in CSS pixels, which map 1:1 onto the Switch's 1280×720 screen.
 */
#pragma once
#include <SDL2/SDL.h>
#include "platform.hpp"

#include <functional>
#include <map>
#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern std::atomic<bool> gStorageStop;   // view_storage.cpp: set as Songate closes

#include "automix.hpp"
#include "bgclient.hpp"
#include "gfx.hpp"
#include "connect.hpp"
#include "filecopy.hpp"
#include "together.hpp"
#include "micmeasure.hpp"
#include "tagjobs.hpp"
#include "airplay.hpp"
#include "airsend.hpp"
#include "syncer.hpp"
#include "identity.hpp"
#include "lastfm.hpp"
#include "subsonic.hpp"
#include "levels.hpp"
#include "mood.hpp"
#include "report.hpp"
#include "library.hpp"
#include "loader.hpp"
#include "lyricsnet.hpp"
#include "lyricindex.hpp"
#include "tagwrite.hpp"
#include "songprint.hpp"
#include "metacache.hpp"
#include "metadata.hpp"
#include "palette.hpp"
#include "player.hpp"
#include "spectrum.hpp"
#include "stats.hpp"
#include "store.hpp"
#include "thumbs.hpp"
#include "ui.hpp"
#include "cfgmove.hpp"
#include "updater.hpp"
#include "waveform.hpp"
#include "words.hpp"

namespace css {
#ifdef GF_WINDOWED
inline int W = 1280, H = 720;   // follows the window: the shorter way 1280×720, the other way more room (App::fitWindow)
#else
constexpr int W = 1280, H = 720;
#endif
#ifdef GF_ANDROID
// The phone's own layout (view_phone.cpp): a window held upright and under kPhoneDp wide. A tablet or an open foldable held
// upright keeps the console's layout, its sidebar folded when the width is short (App::fitWindow decides)
inline bool Phone = false;
#else
constexpr bool Phone = false;
#endif
constexpr float TbH = 46, BarH = 92, SideW = 236, SideWCollapsed = 68;
// --fs-* scale, one pixel up from the desktop values for a handheld screen
constexpr int Xs = 12, Sm = 13, Base = 15, Md = 16, Lg = 19, Xl = 25, Xxl = 34, Hero = 52;
}  // namespace css

struct TourStep;   // tour.cpp

enum class View { Now, Queue, Artists, Artist, Genres, Genre, Library, Album, Folders, List, Search, Settings, PickFolder, Albums, PickPlaylist, PickExport,
                  Files, Playlists, Server };

/** Gatefold 1.4.2's four list views (.pl-modes): by album, with covers, as tiles, as a plain list. */
enum class ListMode { Albums = 0, Covers = 1, Grid = 2, List = 3 };

struct LyricLine { double t; std::string text; };

/** An artist or a genre: its name, the albums under it and every track, albums or not. */
struct Artist {
    std::string name;
    std::vector<int> albums;
    std::vector<int> tracks;
};

/** Files: one entry of a folder (view_files.cpp). */
struct FileEntry { std::string name; bool dir = false; uint64_t size = 0; int64_t mtime = 0; };

/** A row of the sheets without a track (view_smart.cpp's card): an action, a heading, a choice with a check, … */
struct SheetRow {
    enum Kind { Action, Group, Setting, Count, Preview, Name, Choice, Grid, Text, Toggle, Speaker } kind;   // Text: wrapped lines (sub), an icon before them; Toggle: label, sub, a switch (on)
    // Speaker (BETA 3, AirPlay sending): a place to play on: its name, its state, a check (on) that act turns; on, its volume below (vol
    // 0..1, setVol), unless vol < 0 (this device: the player bar's). disabled: shown, greyed, not chosen (an AirPlay 2 only speaker)
    const char* icon = nullptr;
    std::string label, sub, detail;
    std::function<void()> act;
    float h = 58;
    bool on = false;
    // Setting: its own options (Support's form) instead of a smart recipe's: the labels, the one chosen, and what a pick does
    std::vector<std::string> opts;
    int cur = 0;
    std::function<void(int)> pick;
    float vol = -1;
    std::function<void(float)> setVol;
    bool disabled = false;
};

/** The track menu (+ on a track): its page and what it is working on. */
struct Sheet {
    // NewList, Smart and SmartForm (view_smart.cpp) belong to no track: + in the sidebar opens them.
    enum Page { Closed, Menu, Tags, Cover, Delete, Info, Lists, Moods, NewList, Smart, SmartForm, Sleep, Devices, AirPlay, Job, Jobs, Colour, PlFile, Confirm, EqPreset, ActList, ListActions,
                PlMenu, PlIcon,     // PlMenu / PlIcon: a playlist page's ⋯ and its icons (recipe: the playlist, name: its name)
                FileMenu, FileInfo, FileLink,     // Files: a file's menu, its details, the track an .lrc goes with (dir: the file)
                Release,                          // Settings → Updates: a release's changelog and Install (recipe: its index)
                UpdateOffer,                      // the update banner pressed: Update, Later, the top of what is new (recipe, name: as Release)
                Model,                            // … an Automix model: Download, Use, Go back (name: the model's)
                Factory,                          // Settings → Reset → Factory reset: what to erase (App::factoryPick)
                Report, Crashes,                  // Settings › Support: the report's form, Atmosphère's crash reports (view_support.cpp)
                Battery,                          // the title bar's battery pressed: everything about it (App::openBattery)
                SrvMenu,                          // a server track's … (App::openServerMenu): its song in App::srvMenuSong_
                SrvLists,                         // … its Add to a playlist… : the server's playlists
                SrvPlMenu,                        // a server playlist's menu (+ on the Playlists page): App::srvMenuPl_
                SrvPlIcon };                      // … its icon (server-playlist-icons.tsv)
    Page page = Closed;
    int track = -1;
    int queuePos = -1;      // opened on a queue row: its position in play order; Jobs: the row to focus
    bool upNext = false;    // … in Now playing's Up next
    Id returnFocus = 0;
    std::map<std::string, std::string> draft;   // Tags: the fields as they are being edited
    std::string dir;                            // Cover: the folder being browsed; PlFile: the playlist file
    std::vector<std::string> entries;           // its sub-folders ("/" at the end) and images
    bool wholeAlbum = true;
    std::string previewPath;                    // Cover: the image shown in the header
    SDL_Texture* preview = nullptr;
    TrackInfo info;                             // Info
    int recipe = -1;                            // SmartForm: the recipe (view_smart.cpp), its setting, the name; Job: the card's slot
    std::string arg, name;
    bool nameTouched = false;
    bool fromList = false;                      // Job: opened from the Jobs list, where B goes back to
    std::vector<int> fileTracks;                // PlFile: its lines found in the library
    int fileMissing = 0;                        // ... and those that were not
    // Confirm (App::confirm): a question with Cancel (focused) and a red button, as the track menu's Delete
    std::string askKicker, askTitle, askSub, askQuestion, askText, askButton;
    const char* askIcon = nullptr;              // nullptr: the list icon
    std::function<void()> askYes;
    // … a safe question (askSafe: the button in the accent, not red), and a second choice beside it (askAlt)
    bool askSafe = false;
    bool askSync = false;   // the first sync's question (syncTick: another card over it brings it back after)
    bool askNoCancel = false;                   // no Cancel: the first launch's music folder card (askAlt is the other way)
    const char* askYesIcon = nullptr;
    std::string askAlt;
    std::function<void()> askAltYes;
    std::string askAlt2;                        // a third choice, left of askAlt (the welcome card)
    std::function<void()> askAlt2Yes;
    std::string askWarn;                        // a warning under the text, apart from it: bold capitals
    // What a page shows of files on the SD card (sizes, whether .lrc lyrics exist, a file's kind): read once, not every
    // frame (the sheet's rows are built each frame, and each read waits on the SD card)
    std::unordered_map<std::string, std::string> memo;
    // Report (Settings › Support): the form as it is filled
    int repKind = 0, repArea = -1, repOften = 0;   // a bug or an idea; the part of the app (App::kReportAreas); always, sometimes, once
    std::string repTitle, repWhat, repSteps;
    bool repCrashes = true, repLog = true;
};

class App {
public:
    bool init();
    void run();
    void shutdown();

private:
    // ---------------- systems
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    Gfx g;
    Ui ui;
    Player player;
    BgClient bg;     // the background player (sysmodule), when it runs: it then does the playback
    ThumbWorker thumbs;   // covers for its overlay
    TrackLoader loader;   // a track's tags, lyrics and cover, read and decoded on a worker core
    SongPrints prints;    // listening together: prints of a song held as two files (together.hpp)
    unsigned loadGen = 0;
    Library lib;
    MetaCache meta;
    Waveform wave;
    Spectrum spec;
    Store store;

    // ---------------- palette
    Palette pal = paletteDefault();
    Palette target = paletteDefault();
    Palette coverPal = paletteDefault();   // the playing cover's own colours, whatever the theme
    std::vector<uint8_t> coverPx;          // its 64×64 sample, so Brightness and Saturation can rebuild coverPal
    PaletteOpts paletteOpts() const;
    Palette customPalette() const;

    // ---------------- now playing
    struct Now {
        int track = -1;
        TrackInfo info;
        SDL_Texture* cover = nullptr;
        SDL_Texture* prevCover = nullptr;
        SDL_Texture* blur = nullptr;
        SDL_Texture* prevBlur = nullptr;
        SDL_Texture* frost = nullptr;      // the cover blurred smooth (TrackLoader's frost): Automix's join
        SDL_Texture* prevFrost = nullptr;
        float fade = 1;
        std::vector<LyricLine> lyrics;
        bool synced = false;
        std::string lyricsSource;
        double lrcOffset = 0;   // the .lrc's own [offset:], in seconds (already taken off the lines' times)
        bool lyricsFromFile = false;   // from an .lrc file beside the track, rather than its tags
        bool lyricsInSongate = false;  // ... from Songate's own folder (found online, kept there): Remove on their bar
        bool loading = false;   // its tags, lyrics and cover are on their way (TrackLoader)
    } now;
    double lyricsHold = 0;      // while loading: where the previous lyrics stay
    /** Where the lyrics are: the music's position, less the track's nudge (lyrics moved later show later). */
    double lyricsPos() const { return (now.loading ? lyricsHold : player.position()) - lyricNudge(); }
    // The lyrics nudge: per track, in 0.1 s steps, kept by Gatefold (Store::lyricNudge); + later, − earlier.
    float lyricNudge() const;
    void nudgeLyrics(float by);   // 0: back in time with the file
    void seekLyric(int line);     // a tapped line: the music goes to where that line now shows
    /** The timing control shows: synced lyrics, the setting on, and from a source it is wanted for (Settings → Lyrics). */
    bool lyricTimingShown() const {
        return store.lyricTiming && now.synced && !now.lyrics.empty() &&
               (store.lyricTimingFor == 2 || (store.lyricTimingFor == 1) == now.lyricsFromFile);
    }
    /** "Earlier · 0.3 s · Later": the nudge's control (the Lyrics tab, the Canvas). Returns its width. */
    float lyricSyncControl(const char* key, float x, float cy, float h, const Font& f, float alpha, bool live);

    // ---------------- play queue (Gatefold's "active playlist")
    struct Queue {
        std::string name;
        std::vector<int> tracks;
        std::vector<int> order;   // positions into tracks, in play order
        int pos = -1;             // index into order
        int failed = 0;
    } q;
    bool stopAfterCurrent = false;

    // ---------------- volume levelling (levels.hpp): measured here, applied here and by the background player
    Levels levels;
    std::vector<std::string> levelFirst, levelAll;   // what was last asked to be measured first, and the library
    unsigned levelEpoch = ~0u, levelGenUsed = ~0u;
    bool levelAllSent = false;   // the library was handed to the worker (levelling on), not only what Automix needs
    int levelModeSent = -1, levelTargetUsed = 0;
    // Crossfade (Gatefold's own player; the background player does its own): this track's was looked at
    bool xfadeChecked = false;
    // Automix (automix.hpp): while the join runs, the chip on Now playing says so, until mixUntil
    double mixUntil = 0;
    double mixShownAt = 0, mixFrom = 0;   // the chip's start (its animation), and where the join itself begins
    double mixShowLen = 0;                // the show's length as announced (endJoinShow shortens mixUntil, not this)
    bool mixAnnounced = false;            // shown, the next track not loaded yet (0.4 s: updateAutomix)
    /** A join's show cut short (a skip, a pause, a seek, Stop: the user, 2026-10-29, "they must end cleanly"): every
     *  animation carried on from where it is to its end over `over` seconds (the hint's own 0.9 s ease), the swipes too. */
    void endJoinShow(double over = 0.9);
    unsigned mixSeeksSeen = 0;            // Player::seeks() last frame (a seek through a join's show: endJoinShow)
    void holdOrderForAutomix(bool rebuild = false);        // Shuffle set aside while an Automix playlist is queued, back after (app.cpp)
    automix::Model mixModel() const;      // Settings › Debug's Automix model in dev builds; a release plays automix::Latest
    // Automix's models over the air (the user, 2026-10-30): the ones downloaded (the settings folder's models/*.model), read at
    // launch and after a download; the one chosen in Settings › Updates › Automix models plans the joins (planJoin)
    struct ModelEntry { std::string file; automix::ModelFile m; };
    std::vector<ModelEntry> modelFiles;
    void loadModelFiles();
    static const char* bundledModelFile();   // Automix V1.2.0's file, bundled (view_automix.cpp)
    const ModelEntry* mixFile() const;    // the downloaded model chosen, or nullptr (a built-in one)
    std::string mixName() const;          // the model playing, by its name
    automix::Plan planJoin(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const automix::Join* want) const;
    struct ModelRow { std::string name; bool builtin = false, downloaded = false; int offer = -1; };
    std::vector<ModelRow> modelList() const;   // Settings › Updates › Automix models: built in, downloaded, listed
    void openModel(const std::string& name);   // its sheet (Sheet::Model)
    void useModel(const std::string& name);
    void downloadModel(int offer);
    void modelSheetRows(std::vector<SheetRow>& rows);
    void modelSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    /** Automix's hold on Now playing during a join: `move` the cover's glide to the middle (0..1), `ui` how much of the rest
     *  shows (1: all), `quiet` how far the cover's pulse rests (1: still). As the Canvas's layout morph. */
    struct Focus { float move = 0, ui = 1, quiet = 0; };
    // mode: Settings › Appearance › Automix: 0 Full, 1 Immersive (the same curve; what it moves and fades is the caller's),
    // 2 Simple (nothing moves or fades: the pulse's rest only)
    Focus automixFocus(int mode = 0) const;
    // the Canvas's join animation in use: a phone upright has Full only (the user, 2026-11-04: "the automix full animation
    // should be the only one default and available for portrait"), else Settings › Appearance › Automix
    int mixCanvasMode() const {
#ifdef GF_ANDROID
        if (css::Phone) return 0;
#endif
        return store.mixAnimCanvas;
    }
    // The outgoing song through a join (Simple and Immersive: the user, 2026-10-29): what it showed, kept when the next one
    // loads, so its title, lyrics, Up next and waveform can give way to the next one's as the join goes (mixWipe)
    struct MixOld {
        bool on = false;
        TrackInfo info;
        std::vector<LyricLine> lyrics;
        bool synced = false;
        std::string lyricsSource;
        std::vector<int> upNext;     // its Up next (the next one first)
        std::vector<float> wave;     // its seek bar's waveform (Waveform::peaks), empty if it had none yet
        double lyricsAt = 0, at = 0; // where its lyrics were, and when (they go on with the clock)
        int track = -1;              // its track (its genre chip)
        double posAt = 0;            // where it was playing at `at` (its time on the Canvas)
        bool coverIn = false;        // the next one's cover is in (applyCover): the outgoing one's is now.prevCover, else now.cover
        Palette palette;             // its palette (the Palette tab's swatches and roles)
    } mixOld;
    /** A cover (or its blur, its frost) of either song through a join: the outgoing one's, or the incoming one's. */
    SDL_Texture* mixCover(bool old, SDL_Texture* cur, SDL_Texture* prev) const { return old && mixOld.coverIn ? prev : cur; }
    /** The Automix hint above the cover rather than on it (Settings › Appearance › Automix › The hint; Automatic: each
     *  animation's own place, `simpleAbove` for the ones that set it above) */
    // Settings › Appearance › Automix › The hint (store.mixHint) is Now playing's Simple's only (the user, 2026-10-29: "Simple
    // mode will only let the users change the hint position on Now playing"), but its Lyrics tab's (always the lyrics'
    // place: hintInLyrics): 0 Automatic (above the cover), 1 on the cover, 2 above it. Every other animation, the Canvas's included, has its
    // own place: on the cover in Full and Immersive; the Canvas's Simple in the lyrics' place, else above the cover.
    bool nowHintAbove() const { return store.mixAnimNow == 2 && store.mixHint != 1; }
    // A page swiped (Simple: the whole of Now playing, or the whole Canvas but its controls; the player bar in Immersive and
    // Simple; the user, 2026-10-29): its images too (wipedImage: the covers, their frost, the Canvas's background). Every
    // section swipes across its own width, all on the join's one clock (mixWipe): they start together and end together,
    // each at its own speed ("each section separate, but all start at the same time and end at the same time", the user)
    struct PageWipe { bool on = false; } pageWipe_;
    // … but sections drawn as one (Now playing's info column, from the title to Up next or the lyrics: "this one should be
    // the only one not separated", the user, 2026-10-29): one wipe across x0..x1 for all of them, its light once (groupMagic)
    struct WipeGroup { bool on = false; float x0 = 0, x1 = 0; } wipeGroup_;
    void groupMagic(float y0, float y1) {
        const float p = mixOld.on ? mixWipe() : 1.f;
        if (!wipeGroup_.on || p <= 0 || p >= 1) return;
        float soft;
        magicEdge(wipeEdge(wipeGroup_.x0, wipeGroup_.x1, p, soft), wipeGroup_.x0, wipeGroup_.x1, y0, y1, p);
    }
    /** A section's wipe across x0..x1, p of the way: its edge, and `soft`, how wide the change across it is (the letters
     *  fade over three quarters of it each side of a 10 px gap, blurring over all of it); before it starts and after it
     *  ends nothing of it is on the section */
    static float wipeEdge(float x0, float x1, float p, float& soft) {
        soft = std::max(48.f, (x1 - x0) * 0.3f);
        const float reach = soft * 0.75f + 5;
        return x0 - reach + p * (x1 - x0 + 2 * reach);
    }
    /** An image of either song through a wipe across x0..x1 (the cover, its frost, the Canvas's background), on the join's
     *  clock as every section: draw(true) the outgoing one's, draw(false) the incoming one's over it from the left. `under`:
     *  the outgoing one only right of the edge (where the incoming one does not cover it: translucent, or no cover); `magic`:
     *  its edge's light down y0..y1. Without a page swiped, the incoming one's. */
    template <class F> void wipedImage(float x0, float x1, float y0, float y1, bool magic, bool under, F&& draw) {
        const float p = mixOld.on && pageWipe_.on ? mixWipe() : 1.f;
        if (p >= 1) { draw(false); return; }
        if (p <= 0) { draw(true); return; }
        float soft;
        const float edge = wipeEdge(x0, x1, p, soft);   // as wiped's
        if (under) g.setWipe(edge, soft, false);
        draw(true);
        g.setWipe(edge, soft, true);
        draw(false);
        g.clearWipe();
        if (magic) magicEdge(edge, x0, x1, y0, y1, p);   // its light inside the image only (it bled past the cover: the user, 2026-10-29)
    }
    /** How far the join has gone over to the next song, eased: 0 the outgoing song's, 1 the incoming one's (at least 0.8 s). */
    float mixWipe() const;
    /** Draws a block twice through a wipe from left to right (Gfx::setWipe): draw(true) the outgoing song's, draw(false) the
     *  next one's; once (the next one's) when no join shows. x0..x1: the block's width, y0..y1 its height (the magic's). */
    template <class F> void wiped(float x0, float x1, float y0, float y1, F&& draw) {
        const float p = mixOld.on ? mixWipe() : 1.f;
        if (p >= 1) { draw(false); return; }
        if (p <= 0) { draw(true); return; }
        if (wipeGroup_.on) { x0 = wipeGroup_.x0; x1 = wipeGroup_.x1; }   // one of a group's sections: the group's wipe
        float soft;
        const float edge = wipeEdge(x0, x1, p, soft), gap = 10;
        // shapes, images and icons (a chip, a row's cover): the outgoing one's into the next one's across the edge, per pixel
        // (by their middles, each pair of chips changed at once and the two songs' overlapped: "looks strange", the user)
        g.setShapesOnly(true);
        g.setWipe(edge, soft, false);
        draw(true);
        g.setWipe(edge, soft, true);
        draw(false);
        g.setShapesOnly(false);
        // the two songs' letters never on top of each other (they blurred into each other, the user said, 2026-10-29): the
        // outgoing one's gone just right of the edge, the next one's from just left of it, each over 3/4 of the soft width
        // (smoother, the user, 2026-10-29: over half, the letters turned to blur too abruptly, a title most);
        // between them a blur: both songs' letters again, faint, spread sideways, round the edge only, under its light
        ui.setQuiet(true);
        g.setTextNudge(0, 0, true);
        const float half = soft * 0.75f;
        g.setWipe(edge + gap / 2 + half / 2, half, false);
        draw(true);
        g.setWipe(edge - gap / 2 - half / 2, half, true);
        draw(false);
        g.setTextNudge(0, 0, false);
        // the blur: each song's letters from their blurred copies (Gfx::blurTexture, made once), round the edge only (copies
        // of the sharp letters, nudged round, showed as copies however many: "make the blur cleaner", the user, 2026-10-29)
        const float f0 = g.fade();
        g.setFade(f0 * 0.65f);
        g.setWipeBump(edge, soft);
        g.setTextBlur(true);
        draw(true);
        draw(false);
        g.setTextBlur(false);
        g.setFade(f0);
        ui.setQuiet(false);
        g.clearWipe();
        if (!wipeGroup_.on) magicEdge(edge, x0, x1, y0, y1, p);   // (a group's: once, groupMagic)
    }
    /** In a Simple join, lyrics on screen give the hint their place (the user, 2026-10-29: swiped, the two songs' lines
     *  never read right, however they met): they fade out as the hint comes in, the hint shows large where they were, and
     *  the next song's lyrics fade back in as it leaves (automixCoverEase: the hint's 0.9 s). The Canvas always; Now
     *  playing's Lyrics tab whatever its hint's setting (the user, 2026-10-29: set Above, it stayed on top of the cover). */
    bool hintInLyrics(bool canvas) const { return canvas ? store.mixAnimCanvas == 1 : store.mixAnimNow == 2; }
    /** … the hint there: centred in the lyrics' box, k its size (1: the chip's own). */
    void lyricsHint(float x, float y, float w, float h, float k) {
        const float e = automixCoverEase();
        if (e <= 0.002f) return;
        const float cw = 36 * k + float(g.measure(mixLabel, { int(std::round(css::Xs * k)), Weight::SemiBold }));
        automixChip(mixLabel, std::round(x + (w - cw) / 2), std::round(y + h * 0.42f), k, e, false, true);
    }
    /** The wipe's edge as something magical happening (the user, 2026-10-29): a soft column of light in the accent's ink
     *  travelling with it and sparkles rising round it, swelling mid-way and gone at both ends (p: how far the wipe has gone).
     *  Only inside its section, x0..x1 by y0..y1: it fades as the edge comes in and goes out, and is clipped there (the edge
     *  starts and ends outside the section: the info column's light ran over the cover, the user, 2026-10-29). */
    void magicEdge(float edge, float x0, float x1, float y0, float y1, float p);
    void drawMagic(float edge, float y0, float y1, float s);   // (its light and sparkles, s their strength)
    void drawNowCover(float x, float y, float art);
    SDL_FRect glideCover_{};   // where drawNow wants the cover while it glides: App::draw draws it last
    bool glideOn_ = false;
    std::vector<SDL_FPoint> lapPts_;      // the chip's progress lap (kept: nothing allocated per frame)
    bool mixPrepared = false;   // the next track opened ahead on the player's worker (Player::prepare)
    bool stretchBegun = false;   // this track started towards the next one's tempo (Automix v3: Player::beginStretch)
    // the dev build's audio trace (Player::takeTrace): audio-trace.txt in the config folder, every 5 s something went wrong
    double traceAt = 0;
    // ... and each Automix join's frames (the user's console videos, 2026-10-28: the animation dropped frames there): its
    // frames, average and worst frame time and work, frames over 17 ms; a line when the join ends
    int joinFrames_ = 0, joinLate_ = 0;
    double joinSum_ = 0, joinWorst_ = 0, joinWorkSum_ = 0, joinWorkWorst_ = 0;
    void traceJoinFrame(float frameMs, float workMs);
    // ... and what the worst one did (the console's first trace, 2026-10-29: one frame of 228 ms of work in a join): refreshes
    // really missed (a frame of 25 ms or more), when in the join the worst work came, its sections, update's slowest step
    int joinDrops_ = 0;
    std::string joinWhy_;
    double profFrame_[8] = {};   // this frame's sections (profMark)
    double profPrev_[8] = {};    // the last whole frame's
    const char* updSlow_ = "";
    double updSlowMs_ = 0;
    const char* updPrevSlow_ = "";       // the last whole frame's
    double updPrevMs_ = 0;
    double updTotalMs_ = 0, updPrevTotalMs_ = 0;   // update() as a whole
    Uint64 updAt_ = 0;
    void updMark(const char* step) {
        const Uint64 t = SDL_GetPerformanceCounter();
        const double ms = double(t - updAt_) / double(SDL_GetPerformanceFrequency()) * 1000;
        if (ms > updSlowMs_) { updSlowMs_ = ms; updSlow_ = step; }
        updAt_ = t;
    }
    unsigned traceStarved = 0;
    std::string audioTrace;
    std::string mixLabel;
    // The join editor (view_join.cpp): a playlist's join between two of its tracks, as a streaming service's transition editor
    struct JoinEdit {
        bool open = false;
        std::string playlist;              // its name: the joins' key in mixes.tsv (Store::joinKey)
        int a = -1, b = -1;                // the two tracks (library indices)
        automix::Join join, kept;          // as edited; as kept (Auto when none)
        mixan::Info infoA, infoB;          // their analyses (empty: not analysed yet)
        double durA = 0;
        // Decoded on a worker when it opens: the first track's last 45 s and the second's first 40 s, 16-bit stereo at
        // the player's rate, and their loudness every 20 ms for the drawing; then, for Listen, the join rendered
        std::thread worker;                // one job at a time; joined before the next and when it closes
        std::atomic<bool> cancel{ false };
        std::atomic<int> loaded{ 0 };      // 0 reading, 1 ready, 2 could not
        std::atomic<int> rendered{ 0 };    // 0 none, 1 rendering, 2 ready (pcm), then handed to the player
        std::vector<int16_t> pcmA, pcmB, pcm;
        double aFrom = 0;                  // the first track's time at pcmA's start (s)
        std::vector<float> envA, envB;     // 0..1 every 20 ms
        double listenFrom = 0;             // the first track's time at the preview's start (s)
        bool listening = false;
        int rate = 48000;
        // a touch drag on the picture: the second track (when it comes in) or the join's end (its length); the time
        // across the picture is held still while it lasts, and the join as it was when it began
        double viewT0 = 0, viewT1 = 0;     // the first track's time across the picture: kept while it is open (widened when
                                           // the join moves out of it), so a dragged track stays where it was let go
        int drag = 0;                      // 0 none, 1 the second track, 2 the join's end
        float dragX = 0;
        double dragT0 = 0, dragT1 = 0;
        automix::Join dragFrom;
    } je;
    // Automix playlists (view_automix.cpp): the picker, and their order
    struct AutomixPick {
        bool open = false;
        int tab = 0;               // Tracks, Albums, Artists, Genres, Folders
        std::vector<char> sel;     // per library track: ticked
    } ap;
    double automixTickAt = 0;
    bool grabMoved_ = false;          // a playlist's song was moved while held: saved at grabDrop
    bool lyricNudgeUnsaved_ = false;  // the lyrics' timing was nudged: written with the next save
    bool automixReady(const std::string& path) const;   // analysed for Automix (its mix.tsv row)
    void openAutomixPicker();
    void closeAutomixPicker();
    void drawAutomixPicker();
    void orderAutomixList(int ref, bool announce);
    void automixListsTick();
    // An Automix playlist's order, worked out on a worker (it took seconds of a frozen screen for a long playlist on the console):
    // one at a time, the rest waiting by name; what it finds is put in when it is done, if the playlist is still as it was
    struct MixOrderJob {
        std::string name;                               // the playlist, found again by its name
        std::vector<std::string> before;                // its songs when asked: changed meanwhile, it is asked again
        std::vector<std::string> here, later, gone;     // analysed (to order), waiting for their analysis, not in the library
        std::vector<mixan::Info> info;                  // the analysed ones' analyses, copied for the worker
        int first = -1;
        automix::Model model = automix::Latest;
        bool announce = false;
        std::vector<int> result;                        // empty: stopped
    };
    std::thread mixOrderThread_;
    std::atomic<bool> mixOrderDone_{ false };
    bool mixOrderOn_ = false;
    MixOrderJob mixOrderJob_;
    automix::OrderWatch mixOrderWatch_;
    std::vector<std::pair<std::string, bool>> mixOrderWaiting_;   // playlists to order next (name, announce)
    void mixOrderTick();
    void mixOrderEnd();   // at the close: the worker ended
    std::vector<std::string> automixWaitingTracks(size_t max) const;
    // Automix is only Automix playlists' (the user, 2026-10-28): the queue came from one of them
    bool automixQueue() const;
    int queuePlaylist() const;   // the playlist the queue came from (by its name), or -1
    void automixProgress(int& done, int& total) const;   // their tracks analysed, of all of them
    void openJoinEditor(const std::string& playlist, int a, int b);
    void closeJoinEditor();
    void drawJoinEditor();
    automix::Plan joinPlan() const;   // the editor's join as it would play
    // The equalizer: a change to send to the background player (at most every 0.1 s while a slider moves)
    bool eqDirty = false;
    double eqSentAt = 0;
    double levelsSavedAt = 0, gainsWrittenAt = -100;
    bool gainsDirty = false;
    unsigned gainsGen = 0;
    std::unordered_set<std::string> gainsHave;   // the paths in gains.bin as last written
    /** The gain for a track in dB, for a mode (GfbgLevel; −1: the one set) and the target (0: not measured, or off). */
    double levelDb(int track, int mode = -1) const;
    void updateLevels();
    void writeGains();

    // ---------------- the sleep timer: the background player keeps it when it plays, else Gatefold (sleepUntil)
    double sleepUntil = 0;        // App::time it runs out, 0 off (Gatefold's own playback)
    bool sleepFading = false;
    unsigned sleepGenSeen = 0;    // the background player's sleepGen last seen
    int sleepChoice = 0;          // the choice last made (Settings' segmented control), 0 off
    bool sleepReleased = false;   // the timer ran out: the console may dim and sleep again until music plays
    bool awakeHeld = false;       // keepAwake as applied (the idle-time extension and auto-sleep off)
    int mediaState = -1;          // appletSetMediaPlaybackState as last set: playing, or kept awake
    double sleepLeft() const;     // seconds, 0 off
    void setSleep(double seconds, int choice);
    void updateSleep();
    /** seen: it ran out while Gatefold watched, so the card that puts the console to sleep may follow. */
    void sleepEnded(bool seen);
    double sleepSeenAt = 0;       // steady-clock seconds the background player's timer was last seen running
    // The card after the timer: the console goes to sleep (appletRequestToSleep) unless you stay awake.
    double sleepPromptAt = 0;     // App::time it appeared, 0 none
    void drawSleepPrompt();
    void closeSleepPrompt();

    // ---------------- Keep the console awake → Dim the screen: after store.dimAfter minutes without input
    double idleFor = 0;           // seconds without input
    int dimmed = 0;               // 0 no, 1 the backlight lowered (handheld), 2 a veil (docked: the TV is not ours)
    float dimSaved = 1;           // the brightness to put back
    float veilA = 0;              // the docked veil, eased
    void setDimmed(bool on);

    // ---------------- the battery: charging, and how long to full or to empty
    struct Battery {
        std::vector<std::pair<double, double>> samples;   // App::time, raw percentage
        double lastAt = -100;
        long long lastWall = 0;   // std::time of the last sample: a gap means the app was suspended
        int charging = -1;
        std::string est;          // "2 h 10", "full in 45 min", "" while unknown
        double pct = 0;           // the latest, as HOS shows it (the raw one, finer, only makes the rate)
        double rate = 0;          // % a minute (+ charging), 0 unknown
        plat::BatteryInfo info;   // the charger and the battery (plat::batteryInfo), every 10 s
        bool infoOk = false;
        /** 0 not charging, 1 slow (a low-power charger), 2 normal, 3 fast. */
        int speed() const { return charging != 1 ? 0 : !infoOk ? 2 : info.fast ? 3 : info.charger == 1 ? 2 : 1; }
        bool full() const { return charging == 1 && pct >= 99.5; }
    } battery;
    void openBattery();   // the battery's card (the title bar's battery pressed)
    // The battery drawn (shell.cpp): the title bar's, and each state's in Settings. s: its scale (1: 24 x 12); state: Store's battery
    // states (on battery, low, charging slowly, charging, charging fast, charged)
    void drawBatteryIcon(float bx, float cy, float s, double pct, int state, float alpha = 1);
    // the battery, what is beside it, its press: left edge. side: 0 the title bar's (Home), 1 the Canvas's own items and order
    float drawBatteryCluster(float rx, float cy, unsigned pct, float limit, int side = 0);
    int batFor_ = 0;            // Settings › Appearance › Battery › Set it for: 0 Home, 1 Canvas (the side its rows edit)
    double batPhase_[6] = {};   // each state's animation, 0..1 and round again: advanced by the frame's time over its period, so a
    double batPhaseAt_ = -1;   // change of pace or charger speeds it up or slows it down without a jump
    // (every platform has them; only Android's dev build watches) Dev, Android: a frame that does not finish (the user's recording, 2026-11-05: after a turn the app stopped drawing, its last
    // landscape picture shown sideways, then black for 15 s until it was swiped away): where in the frame it stopped, written at once
    // to stall.txt by a watchdog; and the run before's log kept as log-previous.txt (a frozen run's log was lost on its restart)
    std::atomic<const char*> stage_{ "start" };
    std::atomic<int64_t> frameDoneMs_{ 0 };
    std::atomic<bool> watchdogQuit_{ false };
    std::thread watchdog_;
    float batteryRoom_ = 0;     // the title bar's room for what shows beside the battery, last frame (Settings greys what would not fit)
    int batteryState(double pct) const;
    Rgb batteryInk(int colour, double pct) const;
    // what may show beside it, in your order (the charger's and the battery's details only where the power service tells them);
    // put back in your order. side: 0 Home, 1 Canvas; −1 the side Settings is on (batFor_)
    std::vector<int> batteryItems(int side = -1) const;
    void setBatteryItems(const std::vector<int>& vis, int side = -1);
    bool* batteryShown(int item, int side = -1);
    // A row of a list put in order by touch (Settings: the sidebar's tabs, the battery's details): its handle, held and dragged
    void dragSort(std::vector<int>& order, int base, const char* key, int i, float rx, float cy, const std::function<void()>& after,
                  const std::vector<int>* rows = nullptr);
    void sampleBattery();

    // ---------------- play statistics (stats.hpp): Gatefold counts what it plays itself; the background player its own
    Stats stats;
    double statHeard = 0;       // seconds of the current track heard here
    bool statCounted = false;   // … and already counted
    double statsSavedAt = 0;
    unsigned statsEpoch = ~0u;  // the scan whose files were asked for their dates (libEpoch)
    void saveStats();

    // ---------------- the background player: what Gatefold sent it last, and what it has seen of it
    struct BgSync {
        std::vector<int> tracks, order;
        std::string name;
        int pos = -2, mode = -1;
        bool stopAfter = false, seenStopAfter = false;
        unsigned trackGen = 0, queueGen = 0, errorGen = 0;
        float volume = -1;
        double awaitUntil = 0;   // a track Gatefold asked for is on its way: the state may still show the old one
        BgConfig cfg;
        bool cfgRead = false;
        unsigned eqGen = 0;
        float xfadeSent = -1;
        int xfadeFlagSent = -1;
        int modelSent = -1;        // Automix's model, as told to the player (protocol 6)
        std::string modelFileSent = "\x01";   // … a downloaded one's file ("": the built-in modelSent)
        bool automix = false;      // the queue it has is an Automix playlist
        unsigned joinGen = 0;      // its last Automix join, shown
        int coverCount = 0;        // library tracks with an overlay cover (Settings), counted once a second
        double coverCountAt = -10;
    } bgs;

    // ---------------- navigation
    struct Route { View view; int ref; std::string query; Id focus = 0; };
    Route route{ View::Now, -1, {}, 0 };
    std::vector<Route> history;
    std::unordered_map<uint64_t, ScrollArea> areas;
    int npTab = 0;
    Id listJump = 0;   // "Current track": the row to focus once the list has scrolled to it
    View libFrom = View::Library;   // which tab an album page was opened from, for the sidebar's highlight
    std::string playlistPickerAt;      // + → Import a playlist file…: the folder of the last file picked (this session)
    std::string exportPickerAt;        // Export: the folder last exported to (this session)
    std::string exportName;            // Export: the list being exported, while its folder is chosen (View::PickExport)
    std::vector<int> exportTracks;
    View folderFrom = View::Folders;   // … and a folder page: Library when opened from its Folders tab (App::navigate)
    int librarySort = 0;   // 0 artist, 1 title, 2 year, 3 folder
    bool canvas = false;
    float canvasIdle = 0;    // seconds without input; the Canvas controls fade away
    float canvasCtl = 1;     // their opacity
    float canvasViz = 0;     // 0 = seek hairline, 1 = the idle spectrum it turns into
    // the Canvas layout switch: weights it started from, the layout it is heading to, and when it began
    float cvFrom[3] = { 1, 0, 0 };
    int cvTo = 0;
    double cvStart = -10;

    // ---------------- derived indexes
    std::vector<int> albumOrder;
    std::vector<Artist> artists;
    std::vector<int> searchResults;
    // Search's filter tabs (the user, 2026-11-09): what matched among your albums, artists, genres and playlists (-1 Liked), made
    // with searchResults; the tab chosen is kept between searches (0 Songs, 1 Albums, 2 Artists, 3 Genres, 4 Playlists, 5 Lyrics)
    std::vector<int> searchAlbums_, searchArtists_, searchGenres_, searchPlaylists_;
    int searchTab_ = 0;
    std::string searchFor_;          // what the lists above were made for: the query, the index and the playlists then
    unsigned searchGen_ = ~0u;
    uint32_t searchLists_ = 0;
    void runSearch();
    void searchSync();
    uint32_t searchListsStamp() const;
    float drawSearchTab(float x, float top, float w, float clipY, float clipH);   // a tab's body but Songs', in the page's scroll area; its height (view_grid.cpp)
    void drawServerSearch(float x, float y, float w, float h);                     // the search page with the server chosen (view_server.cpp)
    static std::vector<std::string> searchWords(const std::string& query);        // the words typed, lower-cased
    static bool wordsIn(const std::vector<std::string>& words, const std::string& hay);   // every word somewhere in hay (lower-cased)
    // Search by lyrics (BETA 3, lyricindex.hpp): the songs whose lyrics have the words, their line; asked when a search opens
    LyricIndex lyrIndex;
    /** The search field's words: lyrics too once every song's lyrics are read (the user, 2026-11-07); before, as they were. */
    const char* searchHint() const {
        // the tabs' own names, in their order (the user, 2026-11-11: "Songs, Albums, Artists or Genres", and Lyrics once all are read)
        const LyricIndex::Progress p = lyrIndex.progress();
        return !p.checking && p.total > 0 && p.read >= p.total ? "Songs, Albums, Artists, Genres or Lyrics" : "Songs, Albums, Artists or Genres";
    }
    std::vector<LyricIndex::Hit> searchLyrics_;
    uint64_t searchTicket_ = 0;
    std::string lyrSeekPath_;             // a song played from a lyrics hit's line: where to go once it is loaded
    double lyrSeekAt_ = -1, lyrSeekUntil_ = 0;
    float drawLyricHits(float x, float top, float w, float clipY, float clipH);   // Search's In the lyrics group (what is in view drawn); its height
    void playLyricHit(size_t i);
    std::unordered_map<std::string, int> pathIndex;
    std::vector<Artist> genres;
    std::unordered_map<std::string, int> albumByKey, artistByKey, genreByKey;   // album and artist pages are routed by key: indexes move as tags arrive
    bool indexDirty = true;
    double indexAt = 0;

    // ---------------- the left stick's pointer
    struct Pointer {
        static constexpr float Hide = 4;   // seconds still before it fades away
        float x = 640, y = 360;
        float idle = 99;                   // seconds since it last moved
        bool down = false;                 // A held: pressing under it
    } pointer;
    bool mouseHeld_ = false;               // a mouse's left button held: a finger at the pointer (App::input)
    float rehover_ = 0;                    // a list scrolled under the still pointer: what it is over, again, while it glides

    // ---------------- misc
    std::string toastText;
    float toastT = 0;
    double time = 0;
    float dt = 0;
    bool byTouch_ = false;                  // the last input was a finger, not a button, a key, the stick or a click (App::frame)
    std::vector<Id> cardRowIds_;            // this frame's card rows (their outline is drawn inside the row: App::drawFocusRing)
    static constexpr float kCardRowInset = 4;
    bool byMouse_ = false;                  // the last input was a mouse's click, not a finger, a button, a key or the stick
    /** What is put in order is dragged by its handle (a finger or a mouse, the user 2026-11-11: "for mouse sorting use the touch way
     *  too"); else Move picks it up for the D-pad. */
    bool sortByDrag() const { return byTouch_ || byMouse_; }
    bool chirpsDue();   // (declared here, for every build: it sat among the dev build's and a release did not compile)
    bool sortTapAt(float x, float y) const;  // a list in sort mode: a press let go here was a tap that is not on a handle
    bool devClicked_ = false, devSortTap_ = false;   // (devctl.cpp: a test's mouse button)
    int tabFloat_ = -1;                     // Settings → Sidebar: the tab row held by touch or settling, drawn over the others
    bool running = true;
    float sideW = css::SideW;
    // Settings › Appearance › Screen edges (Android): how far each piece keeps from the camera's cutouts and the rounded corners,
    // in app pixels, eased as the phone turns (App::updateEdges). tb: the title bar's two ends; side: the sidebar's column (the page
    // starts that much later); bar: the player bar's two ends; mainR: the page's right edge; strip: Black strip's band on each side.
    struct Edges { float tbL = 0, tbR = 0, side = 0, barL = 0, barR = 0, mainR = 0, stripL = 0, stripR = 0, searchL = 0, searchR = 0; };
    Edges edge_, edgeWant_;
    std::vector<SDL_FRect> edgeBoxes_;   // the cutouts and corners as the app sees them, for Show where it is
    unsigned edgeGen_ = ~0u;
    int edgeModeWas_ = -1;
    float edgeExtraWas_ = -1, edgeScaleWas_ = 0;
    bool edgeCornersWas_ = false;
    double edgeShowUntil_ = 0;
    void updateEdges();
    void drawEdges();
    // The sidebar opening and closing (the user, 2026-10-30: "a nice animation"): sideP_ runs 0 (icons only) to 1 (open) in
    // 0.38 s, eased in and out; sideW follows it, and the labels, counts and the wordmark fade and slide in with it
    float sideP_ = 1;
    double sideOpenedAt_ = -10;   // when it last began to open: the wordmark's shimmer plays as it lands
    /** How open the sidebar is, 0..1, as drawn (from sideW) */
    float sideOpen() const { return std::clamp((sideW - css::SideWCollapsed) / (css::SideW - css::SideWCollapsed), 0.f, 1.f); }
    int marqueeTrack = -2;   // the player bar's long title and artist scroll from marqueeAt, again for each track
    double marqueeAt = 0;
    /** A line too long for its room scrolls in a loop instead of ending in "…" (the player bar's, and Now playing's artist and
     *  album: the user, 2026-11-02): it rests 3 s at its start, then 30 px/s, the cut edge faded; still with Motion off. */
    void marqueeLine(const std::string& s, Font f, float x, float y, float w, const Rgb& c);
    /** Now playing's title in a column w wide: too long for one line, a bracketed part at its end (or what follows " - ") is left
     *  out (the user, 2026-11-07: "make it disappear", the album line below mostly says it); then one line at Hero, or two at 42,
     *  cut with … past that, as always. */
    struct TitleFit {
        std::vector<std::string> lines;
        Font font{ css::Hero, Weight::ExtraBold };
        float lh = 54, h = 54;     // a line's height; the whole block's
    };
    const TitleFit& fitTitle(const std::string& text, float w);
    /** A big title in f, w wide: the whole of it if it fits one line, else without a bracketed part at its end (or what follows " - ");
     *  Now playing's and the Canvas's (the user, 2026-11-07: "can you apply it to canvas too?"). Kept, not measured every frame. */
    std::string titleTrim(const std::string& text, Font f, float w);
    float volumeShown = 1;
    float coverBeat = 0;     // the cover's eased bass pulse
    double persistAt = 0;    // when the debounced save is due (0: nothing pending)
    double sessionAt = 0;    // last time the session (queue, position) was written
    int savedTrack = -1;
    bool savedPlaying = false;
    bool ready_ = false;     // init finished: before that there is nothing worth saving over the files
    std::vector<float> pcm_;
    // Settings → About → Frame times: the last 120 frames, interval and the work before the present
    float frameMs_[120] = {}, workMs_[120] = {};
    float spanWork_[120] = {}, spanPres_[120] = {}, lastPres_ = 0;   // what each interval held: the frame before's work and present
    // dev, with Frame times on: every 5 s in the log, the slowest frame and those over 33 ms, the same in a session or not (session 11:
    // the "Second" lines stop 70 s after launch, and a session's own line counts only its time)
    double frameLogAt_ = 0;
    float frameLogWorst_ = 0;
    int frameLogSlow_ = 0, frameLogN_ = 0;
    int frameAt_ = 0;
    std::string frameStats_, profStats_;
    std::string startStats_;   // how long launch took, by step (Frame times): what the console spends before the first frame
    Uint64 launchAt_ = 0;      // when init began: "frame1" and "cover" are counted from it
    bool firstFrame_ = true, firstCover_ = true;
    void launchAt(const char* what) {
        char b[32];
        snprintf(b, sizeof b, "  %s @%.0f", what, double(SDL_GetPerformanceCounter() - launchAt_) / double(SDL_GetPerformanceFrequency()) * 1000);
        startStats_ += b;
    }
    void startMark(const char* step) {
        const Uint64 t = SDL_GetPerformanceCounter();
        char b[40];
        snprintf(b, sizeof b, "%s%s %.0f", startStats_.empty() ? "launch: " : "  ", step, double(t - profAt_) / double(SDL_GetPerformanceFrequency()) * 1000);
        startStats_ += b;
        profAt_ = t;
    }
    // ... and where the work goes: input+update, background, view, sidebar+title bar, player bar, the rest, the Canvas warm-up, present
    double profAcc_[8] = {};
    int profFrames_ = 0;
    Uint64 profAt_ = 0;
    void profMark(int section) {
        const Uint64 t = SDL_GetPerformanceCounter();
        const double ms = double(t - profAt_) / double(SDL_GetPerformanceFrequency()) * 1000;
        profAcc_[section] += ms;
        profFrame_[section] += ms;
        profAt_ = t;
    }
    double frameStatsAt_ = 0;
    unsigned warmTick_ = 0;
    // Once a launch: many tracks have no cover for the overlay yet. 0 not looked yet, 1 showing, 2 done
    // The app's own covers (the covers cache): asked about first when many are missing, the sweep held until then
    // (coversHeld; this launch only, never saved). 0 not looked at yet, 1 showing, 2 answered or not needed.
    int appCoverNotice = 0;
    bool coversHeld = false;
    void releaseCovers();
    // The same for search by lyrics (the user, 2026-11-05: "Can we create a card for lyris reading too like we have for covers?"):
    // after the covers' questions, more than a tenth unread asks; Not now holds the reading for this launch
    bool cnCodeFocus_ = false;   // Settings › Devices: Show a code pressed, the page goes to the code when it shows
    int lyrNotice = 0, lyrMissing = 0;
    bool lyrHeld = false;
    void releaseLyrics();
    int coverNotice = 0;
    int coverMissing = 0;
    double coverNoticeAt = 0;
    Id noticeFocus_ = 0;   // where focus was before it, to go back to
    void closeCoverNotice();
    float textShift = 0;          // the Canvas's own moving offset, under any scroll (Gfx::setShift)
    std::vector<SDL_FRect> settingsRows;   // each row of Settings' pane as last drawn (the tour lights whole rows)
    float settingsPaneX = 1e9f;   // where Settings' right pane begins: B there goes back to the categories

    // ================= app.cpp: lifecycle, input, playback
    void input();
    void update();
    void rebuildIndex();
    /** Reads the music folder again; files: a walk already made (the launch check's), else it walks now. */
    void rescan(const std::vector<std::string>* files = nullptr, Library* built = nullptr);   // built: already made from files, on a worker
    // The music folder's file list, remembered (library.tsv): launch builds from it instead of walking the
    // card, then a walk on another core checks it, and a rescan follows only when something changed.
    std::vector<std::string> libFiles_;    // the list the library was built from, sorted
    std::thread libCheck_;
    std::vector<std::string> libChecked_;
    std::atomic<bool> libCheckDone_{ false };

    void playTracks(const std::vector<int>& tracks, int startPos, const std::string& name, bool shuffle = false);
    /** fade: crossfade into it over that many seconds (Gatefold's own player), 0 cuts or, when a crossfade is set, 0.3 s. */
    void playAt(int orderPos, double fade = 0, const automix::Plan* mix = nullptr);
    void loadTrack(int trackIdx, bool paused = false, bool start = true, double fade = 0, const automix::Plan* mix = nullptr);
    /** Every frame: Automix's join at the end of this track, planned from both analyses and started on time. */
    void updateAutomix();
    void keepMixOld();                          // the outgoing song kept through a join (mixOld)
    void announceBgJoin(const BgState& s);      // the background player's join announced (protocol 6)
    /** The order position a crossfade at the end of this track goes to, or −1 when this join takes none. */
    int crossfadeNext(double left) const;
    /** Settings → Equalizer: kept, applied here, and sent to the background player. */
    void setEq(const GfbgEq& e);
    void bgConnect();
    void bgUpdate();
    bool bgSyncQueue(bool reload = true);
    void forgetNowPlaying();   // nothing plays any more (the overlay emptied the queue): Now playing as at a first launch
    bool bgAdoptQueue();
    void setBackground(bool on);
    /** Asks a name with the keyboard and makes a playlist of your own; track ≥ 0 goes into it. */
    void newPlaylist(int track);
    void addToPlaylist(int playlist, int track);
    void thumbFor(int track, bool now);
    void applyTheme();
    void persistSoon(float delay);
    void persistNow(bool sessionOnly = false);
    void restoreSession();
    static void onLeave(bool exiting, void* self);
    void next(bool natural);
    void prev();
    void togglePlay();
    void seekBy(double d);
    void setOrder(Order o);
    std::vector<int> upNext(int n) const;
    int currentQueuePos() const;
    // Editing the queue, by position in play order (q.order). Without Shuffle the list itself is
    // kept in play order (queueNormalize), so turning Shuffle off and on never loses an edit.
    std::vector<int> queueInPlayOrder() const;
    void queueNormalize();
    void queueEdited();
    void queueAdd(int track, bool next);
    void queueRemove(int pos);
    void queueMove(int from, int to);
    void queuePlayNext(int pos);
    void queueClear();
    // A queue row picked up with the track menu's Move: the D-pad moves it, A or B puts it down.
    // Also the Playlists page's playlists (+ → Move) and the sidebar's tabs (Settings → Appearance → Move): same keys.
    // List: a track of one of your own playlists (route.ref), by its row on the page
    // (SrvPlaylists: the server's Playlists page, the playlist itself moved, as yours)
    struct Grab { enum Kind { Queue, Playlists, Tabs, List, Battery, SrvList, SrvPlaylists } kind = Queue; int pos = -1; bool upNext = false; int cols = 1; int ref = -1; } grab;
    void grabStep(int d);
    void grabDrop();
    // The same lists put in order BY TOUCH (the user, 2026-11-11: "a Dpad way to do it and a touch way like we have on setting -
    // appearance - sidebar sorting"): the menu's Move after a finger puts the list in a sort mode instead of picking one row up.
    // Every row then shows the sidebar sorting's handle in the place of its length (tiles are dragged whole, any way); held, a row
    // follows the finger at once, floats with its shadow, and the rows it passes slide into the place left. Done, a tap elsewhere,
    // back, a button or leaving the page end it. Nothing is written while a finger holds a row: saved when it lifts (sortSave).
    // The engine is beside Settings' own (view_settings.cpp: sortRun and its friends).
    struct TouchSort {
        bool on = false;
        Grab::Kind kind = Grab::Queue;
        bool upNext = false;        // Queue: Up next's rows (the first 15 after the song playing), not the Playlist page's
        int ref = -1;               // List: which playlist
        std::string srvPl;          // SrvList: the server playlist's id
        int held = -1;              // the item under the finger, by its place now
        int settle = -1;            // the one let go, still on its way to its place: drawn over the others until there
        Id heldId = 0;              // the handle (or tile) the finger took: the item keeps it wherever it goes
        float offX = 0, offY = 0;   // the finger's place inside the held item
        float hx = 0, hy = 0;       // where the held item is drawn (the screen's pixels)
        std::vector<SDL_FPoint> slide;   // each item's offset from its place, easing back to 0
        bool moved = false;         // the order changed since it was last saved
        bool sendSrv = false;       // … a server playlist's: sent when the finger lifts
        bool ran = false;           // its list was drawn this frame (not: the page was left, the mode ends)
        int endIn = 0;              // a tap elsewhere: ends after the frame that swallows the tap (App::input)
        int show = -1;              // the row Move was chosen on: its list scrolls to it once
    } tsort_;
    bool sorting(Grab::Kind k, bool upNext = false) const { return tsort_.on && tsort_.kind == k && tsort_.upNext == upNext; }
    /** Move chosen in a menu: by touch the list's sort mode, else the row picked up for the D-pad (App::grab). True: by touch. */
    bool sortBegin(Grab::Kind kind, int at, bool upNext = false, int ref = -1, const std::string& srvPl = {});
    void sortEnd();
    void sortSave();
    void sortMove(int from, int to);
    /** Each frame before the list's items are drawn: n items, those in [lo, hi] can move; slot(i): where item i belongs on the
     *  screen this frame; grid: tiles, dragged any way (else rows, up and down). */
    void sortRun(int n, int lo, int hi, ScrollArea& sa, const std::function<SDL_FRect(int)>& slot, bool grid);
    SDL_FPoint sortAt(int i, const SDL_FRect& slot) const;    // where item i is drawn: its slot, sliding, or under the finger
    bool sortFloats(int i) const { return tsort_.on && (i == tsort_.held || i == tsort_.settle); }
    /** What takes the finger for item i (a handle's box, or a whole tile), at: where the item is drawn now. */
    void sortGrip(const char* key, int i, const SDL_FRect& touch, const SDL_FRect& at);
    /** The sidebar sorting's handle for row i, its glyph's right edge at rx. */
    void sortHandle(const char* key, int i, float rx, float cy, const SDL_FRect& at);
    void dragGlyph(float boxX, float cy, bool hot);          // Settings' and these: one drawing
    void sortFloatBox(float x, float y, float w, float h, float rad);   // lifted off the page: Settings' shadow and ground
    void drawSortDone();
    SDL_FRect sortPill_{};       // where Done is (the toast keeps clear of it)
    bool rowBare_ = false;       // trackRow, coverRow, serverRow: drawn without the length (a handle takes its place)
    float fingerDownX_ = 0, fingerDownY_ = 0;   // where the last finger came down (a tap, or a drag: App::input)
    bool grabSave_ = false;      // a playlist moved on the Playlists page while held: saved at grabDrop, not at each step
    void applyLoaded(TrackLoader::Result& r);
    void applyCover(TrackLoader::Result& r);
    TrackLoader::Result coverWait_;   // a loaded cover kept for the frame after its tags (one thing at a time)
    bool coverWaiting_ = false;
    void parseLyrics(const std::string& text);
    // Lyrics found online (BETA 3, lyrics_online.cpp; Settings → Lyrics → Online): a song with none asks LRCLIB; what comes plays
    // at once with Keep and Not these (lyricsOnlineWay 0), or is shown on a card first, Save or Not these (1)
    lyrnet::Client lyrNet;
    struct LyrOnline {
        enum State { None, Looking, Found, Saving, Saved, NotFound, Listing, Choosing, Failed } state = None;
        std::string path;               // the song it is about
        uint64_t ticket = 0;            // its question (an answer for another is dropped)
        lyrnet::Found found;            // shown (Play) or offered (Ask)
        std::vector<lyrnet::Found> list;   // Not these: LRCLIB's records for the song
        std::string why;                // Failed: why
        bool shown = false;             // its lyrics are in now.lyrics, not yet kept
    } lyrOn;
    // This run, each song asked about: what was found or picked (have), or none and why. A song that comes back shows it again
    // (the user, 2026-11-04: picked a synced one with Not these, skipped, came back: "it does not prompt the lyrics anymore")
    struct LyrMemo { std::string path; bool have = false; lyrnet::Found found; std::string why; };
    std::vector<LyrMemo> lyrMemo_;
    bool lyrEmbedAsked_ = false;          // an EmbedRun is with the worker
    std::vector<std::string> lyrWritten_;   // what Songate wrote: "tag\tpath", "lrc\tpath" (the worker's written.tsv, asked once)
    bool lyrWrittenAsked_ = false;
    std::vector<std::string> lyrTakingOut_; // songs whose Songate lyrics are to be taken out: shown without them meanwhile
    bool lyrWrote(const char* kind, const std::string& path) const;
    double lyrEmbedAt_ = 0;               // not asked again before this (nothing waiting)
    Id lyrFocusNext_ = 0;                 // where the focus goes once it is drawn: the list's first record, Keep or Save
    void lyrRemember();                   // lyrOn's found, or none, for its song
    void lyrMemoSet(const std::string& path, const lyrnet::Found* found, const std::string& why);   // a song's answer, whenever it comes
    void lyrOnlineReset();                // a new song's lyrics arrived (applyLoaded)
    void lyrOnlineTick();                 // asks for a song with none; takes the answers (update)
    void lyrOnlineKeep();                 // Keep / Save
    void lyrOnlineNotThese();
    void lyrOnlinePick(int i);
    void lyrOnlineShow(const lyrnet::Found& f);   // into now.lyrics, not kept
    void lyrOnlineRemove();               // the song's lyrics in Songate's folder: removed (asked again next run)
    bool drawLyrOnline(float x, float y, float w, float h);   // the Lyrics tab's states; true when it drew in the lyrics' place
    bool lyrOnlineBar(float x, float y, float w);             // Play: Keep and Not these on the bar; true when drawn

    void navigate(View v, int ref = -1, const std::string& query = {}, bool push = true);
    unsigned pickerGen = 0;   // the folder pickers list a folder again when it bumps (opened anew, a file written there)
    void back();
    void openSearch();
    void openCanvas(bool on);
    void toast(const std::string& s);
    ScrollArea& area(const char* key, int ref = 0);

    std::vector<int> listTracks(View v, int ref, const std::string& query) const;
    std::string trackSub(int t) const;
    std::string fmtTime(double s) const;
    std::string fmtTotal(const std::vector<int>& tracks) const;
    bool isPlaying() const;

    // ================= shell.cpp: background, title bar, sidebar, player bar
    void draw();
    void drawBackground();
    void drawTitlebar();
    void drawSyncMark(float ix, float iy);   // the two arrows while devices sync (the title bar's; a phone upright's top line)
    /** By touch there is no B: a back button comes in while the last input was a finger and there is somewhere to go back to
     *  (the user, 2026-11-08). backShown(): 0..1, eased, for the places that make room for it. */
    float backShown();
    /** store.tabOrder's indices as the sidebar can show them (Server, which is in Library now, left out). Settings lists them
     *  so, and Move steps along this list. */
    std::vector<int> sidebarTabsShown() const;
    bool backOn_ = false;        // the touch back button is wanted
    float backAnim_ = 0;         // ... and how far it is shown, eased towards that each frame (never restarted: no jumps)
    double backTick_ = 0;
    bool touchUi_ = false;       // the last input was a finger, for the back button: a button or a click clears it, the stick's drift does not
    void drawSidebar();
    void drawPlayerbar();
    void drawToast();
    void drawFocusRing();
    void drawPointer();
    void drawFrameTimes();
    void drawCoverNotice();
    // The sidebar's progress cards, stacked above Settings: one per kind of work running in the background at once.
    // Slot 0: the music folder check, then tags, then the cover cache (one after another); 1: the overlay's covers; 2: levelling.
    enum class Stoppable { None, Covers, Thumbs, Levels, Lyrics, Downloads, TagWrites };
    struct Job {
        std::string label;
        int done = 0, total = 0;   // total 0: just busy
        int settingsCat = 3;   // SetLibrary
        Stoppable stop = Stoppable::None;
        const char* row = nullptr;   // its row in that Settings category (showSetting)
        bool kb = false;             // done and total are kilobytes: shown as "4.1 of 12.9 MB" (a download)
        const char* icon = nullptr;  // its icon in the list of jobs, where its kind does not say (Reading lyrics: the quote)
    };
    // 3: Files' copying, moving and deleting (fileWork); 4: an update (updater); 5: reading lyrics for search (lyrIndex)
    static constexpr int JobSlots = 10;   // 7: the server's downloads; 8: the self test (dev); 9: tags written into files
    struct JobCard { Job job; float shown = 0, since = 0; };
    JobCard jobCards[JobSlots];
    /** What runs in each slot now; false where nothing does (or it was stopped). */
    void backgroundJobs(Job (&jobs)[JobSlots], bool (&busy)[JobSlots]);
    struct JobStack {
        float weight[JobSlots] = {};   // each job's share of the front card's contents
        float body = 0;                // the front card's opacity (the stack's presence)
        float behind = 0;              // cards peeking out behind it, in fractions
        int front = 0;                 // the job with the largest share: where A on the card goes
        int running = 0;               // jobs mostly shown: more than one opens the list
    };
    JobStack jobStack() const;
    std::string jobCount(const Job& j) const;   // "120 of 3074", "4.1 of 12.9 MB"; "" without a total
    float jobsHeight(bool collapsed) const;
    void drawJobCard(const JobStack& st, float x, float bottom, float w, bool collapsed);
    /** Stops a job until it is resumed (the card's ×, Settings), kept in settings.txt; or resumes it. */
    void setJobStopped(Stoppable which, bool stopped);
    void applyJobStops();
    void openJobMenu(int slot, bool fromList);   // a job that can be stopped: Stop, or Open in Settings (view_smart.cpp)
    void openJobList();                          // A on the stack with more than one job: all of them
    // Playlist files (m3u.hpp): .m3u / .m3u8 in the music folder, played or imported; any playlist exported as one.
    std::vector<int> resolvePlaylistFile(const std::string& file, int& missing);
    void openPlaylistFile(const std::string& file);   // its card: Play, Shuffle, Import as a playlist
    void openPlaylistPicker();                         // + → Import a playlist file…: the SD card's folders (drawPicker)
    void leavePlaylistPicker();                        // back to the page it was opened from (either picker)
    void openExportPicker(const std::string& name, const std::vector<int>& tracks);   // Export: the folder to write it in
    void exportPlaylist(const std::string& dir);       // … "Export here": <dir>/<name>.m3u8, asking before replacing one
    /** A question in the sheet's card, as the track menu's Delete: Cancel is focused, `button` (red) runs yes. */
    void confirm(const char* icon, const std::string& kicker, const std::string& title, const std::string& sub, const std::string& question,
                 const std::string& text, const std::string& button, std::function<void()> yes);
    /** Settings at a job's row: the category, scrolled to it, focused and lit for a moment. */
    void showSetting(const Job& j);
    std::string settingsJump;   // the row label showSetting is after, and when it asked (drawSettings)
    double settingsJumpAt = -10;
    void drawSeek(float x, float y, float w, float h, Id id);

    // ================= widgets.cpp: Gatefold's components
    bool iconButton(Id id, const char* icon, float cx, float cy, bool on = false, float size = 34, float iconSize = 18, uint8_t flags = 0);
    float button(Id id, const char* icon, const std::string& label, float x, float y, bool primary = false, bool* clicked = nullptr,
                 bool toggled = false, bool danger = false, uint8_t flags = 0, float minW = 0);   // minW: at least this wide, its content centred
    float chip(const std::string& label, float x, float cy, bool accent);
    float automixChip(const std::string& label, float x, float cy, float k, float alpha, bool lap = true, bool glass = false);   // animated, while Automix joins
    /** Now playing's cover frosted over during a join (its beat-scaled box), the hint inside, centred on (hx, hy) and sized
     *  by `rest`: the cover at rest, so the hint holds still while the cover breathes. */
    void drawAutomixCover(float x, float y, float s, float hx, float hy, float rest, float rad = 16, bool hint = true, float lapW = 2.5f);
    float automixCoverEase() const;   // the frost's strength now (0.9 s in and out, as the glide): for a hint drawn beside it
    /** A part `p` (0..1) of a rounded box's outline, from the middle of its top edge, clockwise (Automix's progress lap). */
    void lapPath(float x, float y, float w, float h, float r, float p, std::vector<SDL_FPoint>& pts);
    void kicker(const std::string& s, float x, float cy, const Rgb& c, float spacing = 0.12f);
    void automixKicker(const std::string& s, float x, float cy);
    // Hints (the user, 2026-10-29): an icon-only button's name, in a small pill by it once it has been focused (the D-pad,
    // or the pointer over it) for half a second. Registered as the button is drawn; drawn last (widgets.cpp)
    void hint(Id id, const std::string& text) { hints_.push_back({ id, text }); }
    void drawHint();
    std::vector<std::pair<Id, std::string>> hints_;
    Id hintId_ = 0;
    double hintSince_ = 0;
    float hintA_ = 0;
    std::string hintText_;
    SDL_FRect hintAt_{};   // "Automix playlist", with a little magic (widgets.cpp)
    void eqGlyph(float x, float cy, const Rgb& c);
    void eqGlyph(float x, float cy, const Rgb& c, bool moving, float alpha = 1, float scale = 1);   // moving: Now playing's, an outside source (AirPlay) too
    void lyricDots(float x, float cy, float d, const Rgb& c, float alpha, bool current);
    void coverBox(int track, float x, float y, float size, float rad, float alpha = 1);
    bool trackRow(Id id, int track, int number, float x, float y, float w, float h, bool showAlbum);
    void listHead(float x, float y, float w, bool showAlbum);
    float viewHead(const std::string& kick, const std::string& title, const std::string& sub, float x, float y, float w);
    /** Red for what deletes: Gatefold's .btn.danger, oklch(0.72 0.17 25), and on a light palette its darker
     *  oklch(0.48 0.17 25) (app.css's light error colour): the light one was too pale to read on white. */
    Rgb dangerInk() const { return pal.dark ? Rgb{ 0.93f, 0.45f, 0.42f } : Rgb{ 0.66f, 0.13f, 0.15f }; }
    void slider(Id id, float x, float cy, float w, float value, float* out);
    /** Upright, value 0..1 from the bottom, filled from the middle (the equalizer's bands); ←/→ while picked up go to left/right. */
    void vslider(Id id, float cx, float top, float h, float value, float* out, Id left, Id right);
    void openEqPresets();
    /** Your saved equalizer presets: the sound now under a name (an existing name is replaced), renamed, deleted. */
    void saveEqPreset(int into = -1);   // into: your preset k updated with the sound now, else a new one (named)
    void renameEqPreset(int k);
    void deleteEqPreset(int k);
    std::string eqPresetName(int preset) const;   // the preset button's text: a preset's name, or Custom
    // ---- game actions: a playlist (GfbgAct_Playlist); event 0 opens, 1 HOME, 2 closes, 3 back to the game (GfbgConfig.lists)
    static uint64_t listHash(const std::string& name);   // gfbgHash of a playlist's name
    std::string listName(uint64_t hash) const;           // "" when no playlist has it any more
    void openActionList(int event);                       // Choose: the playlist for that event
    void openListActions(const std::string& name);        // a playlist page's Games: the events that play it
    /** The playlists for the background player (GFBG_PLAYLISTS), rewritten when they changed (every few seconds). */
    void writeBgPlaylists();
    /** The background player is connected and its settings read (the preview pretends so with GF_BGCFG=1). */
    bool bgShown() const;
    double bgListsAt = -100;
    uint64_t bgListsSig = 0;
    void openEqualizer();   // a long press on the equalizer buttons (player bar, Canvas): Settings → Equalizer
    void toggleEq();        // ... a short press: on or off
    bool toggle(Id id, float x, float cy, bool on, float alpha = 1);   // alpha: greyed (it still takes the focus and a press)

    // ================= view_now.cpp
    void drawNow(float x, float y, float w, float h);
    void drawSpectrum(float x, float y, float w, float h, float played = -1);   // played 0..1: the bars before it bright, the rest dim
    void drawUpNext(float x, float y, float w, float h);
    void drawLyrics(float x, float y, float w, float h);
    // Now playing's lyrics from a source: the song playing (live: the timing control, taps, the scroll by hand), or the
    // outgoing one in a Simple join (at: where its lines are)
    void drawLyricsOf(float x, float y, float w, float h, const std::vector<LyricLine>& lyrics, bool synced, const std::string& source,
                      double at, ScrollArea& sa, bool live);
    void drawPaletteTab(float x, float y, float w, float h);
    void drawPaletteOf(float x, float y, float w, float h, const Palette& target, bool live);   // (a join's outgoing colours: not live)
    void drawCanvas();
    void warmCanvas();
    void drawCanvasLyrics(float x, float y, float w, float h, int size, int layout, float alpha);
    void drawCanvasLyricsOf(float x, float y, float w, float h, int size, int layout, float alpha, const std::vector<LyricLine>& lyrics,
                            bool synced, double at, ScrollArea& sa, bool live);   // (a source: the song playing, or the outgoing one)
    int canvasLayoutShown() const;
    unsigned canvasLayoutsOff() const;   // bit i: Canvas layout i cannot show right now

    // ================= view_lists.cpp
    void drawTrackList(float x, float y, float w, float h);
    bool plViewsOwnLine_ = false;   // upright: a list page's views had no room on its actions' line (last frame): a line of their own
    void drawAlbum(float x, float y, float w, float h);
    void drawFolders(float x, float y, float w, float h);
    float drawSubfolders(const char* idp, const FolderNode& node, float x, float cy, float w, float y, float h, ListMode mode);
    void addSubfoldersAsPlaylists(int folder);   // asks first
    void drawPicker(float x, float y, float w, float h);
    void confirmRemoveFolderPlaylist(int ref, int count);   // Delete on a folder playlist: asks first (App::confirm)
    void useMusicFolder(const std::string& path);
    void welcomeCard(const std::string& found);   // the first launch, over the folder picker: use the card's Music folder?
    /** A first start (the user, 2026-11-06: "ask the user if he wants a local library or a server library"): where the music is,
     *  this device's folder (then the welcome card as before) or a server (its address, user and password, then its music In the library). */
    void firstCard(const std::string& found);
    void firstLocal(const std::string& found);
    void firstServer(const std::string& found);
    bool firstServer_ = false;   // signing in from the first card: its result shown (serverTick)
    std::string firstFound_;
    void plusMenu(bool held);                     // + on the focused row; held: a long touch (quiet when there is no menu)
    // The guided tour (tour.cpp): offered on the first launch's welcome card, and in Settings › About
    void startTour();
    void offerTour();     // a first launch that chose its folder in the picker: asks
    void tourGo(int step);
    void tourBack();      // B: the chapter list closes, else the step before
    void endTour();       // back to the page it started on
    void drawTour();
#ifdef GF_DEV
    void tourCheck();     // GF_TOURCHECK=<0 Basic, 1 Advanced, 2 Complete>: walks that tour by itself and logs what each step lit
#endif
    int tourCount() const;
    const std::vector<TourStep>& tourSteps() const { return tourStepsAt(tourLevel); }   // the chosen tour's
    static const std::vector<TourStep>& tourStepsAt(int level);   // 0 Basic, 1 Advanced, 2 Complete
    int tourStep = -1;                  // the step shown; −1: no tour
    int tourLevel = 0;                  // the tour chosen on its first card
    bool tourChapters = false, tourFocus = false, tourLit = false;
    bool tourPending = false;           // the welcome card asked for it: it starts once the music is read and the covers asked
    bool tourAsk = false;               // a first launch that chose another folder: asked once the folder is in use
    SDL_FRect tourRect{};               // the part lit, gliding to the step's
    SDL_FRect tourWant{}, tourCard{};   // where the light is heading (kept a moment while a page draws its part); the card, gliding
    double tourAt = 0;                  // when the step (or the chapter list) came: its words fade in
    float tourDim = 0, tourMiss = 0;    // the dimming, coming up; how long the part has not been found
    bool tourCardSet = false;
    Route tourFrom{ View::Now, -1, {}, 0 };
    int plusAfterHold = 0;                        // frames until a long touch opens it
    int routeAlbum() const;
    int routeArtist() const;
    void openAlbum(int a);
    void goToAlbum(int a);                       // from outside the grids: lights Albums, or Library when Albums is hidden
    bool tabFor(int tab, const char* page);      // false: its tab and Library are both hidden (a toast says so)
    void openArtist(const std::string& name);
    int routeGenre() const;
    void openGenre(const std::string& name);

    // ================= view_grid.cpp
    void drawLibrary(float x, float y, float w, float h);
    void drawArtists(float x, float y, float w, float h, bool genre = false, bool inLibrary = false);   // inLibrary: Library's switch in its head
    void drawArtist(float x, float y, float w, float h, bool genre = false);
    // A genre's page shows one of Albums, Artists or Tracks at a time (kept while the app runs); its artists are
    // worked out once per genre and index: (artist, tracks of theirs in the genre), by name
    int genreTab = 0;    // 0 Albums, 1 Artists, 2 Tracks
    int genreSort = 0;   // a genre's Albums tab, yours and the server's: 0 by artist, 1 by title, 2 by year (the user, 2026-11-09)
    /** The Albums page's sort select, right-aligned at rx (cy its middle): a button that cycles; its width. */
    float sortButton(const char* key, const std::string& label, float rx, float cy, bool* clicked);
    int artistTab = 0;   // an artist's page: 0 Albums, 2 Tracks (the same numbers, no Artists)
    std::vector<std::pair<int, int>> genreArtists_;
    int genreArtistsFor_ = -1;
    unsigned genreArtistsGen_ = ~0u;
    const std::vector<std::pair<int, int>>& genreArtists(int genre);
    /** quad: the four covers of these tracks' first albums (a genre), as Folders shows a folder. */
    bool tile(Id id, int coverTrack, float x, float y, float w, bool round, const std::string& title, const std::string& sub,
              const std::vector<int>* quad = nullptr, uint8_t flags = 0);

    // ================= view_tracks.cpp: the four list views, shared by every list of tracks
    struct ListEntry {
        enum Kind { ColumnHead, GroupHead, Row, TileRow } kind;
        int index;    // the track's place in the list (the first of a tile row)
        float y, h;   // from the top of the list
    };
    // joinGap: room after each row but the last (a playlist page's Mix: the joins between its tracks)
    std::vector<ListEntry> listLayout(const std::vector<int>& tracks, float w, ListMode mode, float joinGap = 0) const;
    float listRowY(const std::vector<ListEntry>& layout, int index) const;
    float drawTrackBody(const char* key, const std::vector<int>& tracks, float x, float top, float w, float clipY, float clipH, ListMode mode,
                        bool extended, const std::function<void(int)>& play, float joinGap = 0);
    int bodyFocus = -1;   // drawTrackBody: the index of the row or tile under focus, −1 when none
    int bodyGrab = -1;    // drawTrackBody: the index drawn picked up (the queue's Move), −1 when none
    int bodyCols = 1;     // drawTrackBody: tiles in a row last drawn (1 in the row views)
    ScrollArea* bodySortArea = nullptr;   // drawTrackBody, drawServerBody: set, the list is in sort mode (by touch) and this is its scroll area
    float viewSwitch(const char* key, float rx, float cy, int& mode, unsigned hidden = 0);   // hidden: bit i, view i left out
    /** The #-to-Z rail down a list's right edge (railX: its left edge; top, h: the list's view): the letters present, the
     *  section showing lit; a tap or A glides there; a finger slid along it scrubs, the letter under it in a bubble (the user,
     *  2026-11-08: the small letters were poor to tap). rowY(row): that row's y in the area's content. */
    void letterRail(const char* key, float railX, float top, float h, const std::vector<std::pair<char, int>>& letters, ScrollArea& sa,
                    const std::function<float(int)>& rowY);
    /** A list's letters (#, A-Z) with the first row of each, in rail order; an unsorted list gets each letter's first row. */
    static std::vector<std::pair<char, int>> lettersOf(const std::vector<std::string>& names);
    /** A rail's letters, kept per key and made again only when `stamp` changes: gathered every frame, a page's names were copied
     *  and walked sixty times a second. names: asked only then. */
    static const std::vector<std::pair<char, int>>& railLettersFor(const char* key, const std::string& stamp, const std::function<std::vector<std::string>()>& names);
    static constexpr float kRailW = 26;   // what a list leaves the rail at its right
    /** The rail switches of the library showing (Local, Server: Settings › Appearance › Letter rail). */
    int railSet() const { return store.libSource == 1 ? 1 : 0; }
    bool railOn(unsigned bit) const { return (store.letterRails[railSet()] & bit) != 0; }
    struct RailBubble { Id zone = 0; char letter = 0; float y = 0; double at = -10; } railBubble_;
    void coverQuad(const std::vector<int>& tracks, float x, float y, float size, float rad, float alpha = 1);
    void listColumns(float x, float y, float w, ListMode mode, bool extended);
    bool coverRow(Id id, int track, int number, float x, float y, float w, float h, bool extended);
    bool trackTile(Id id, int track, float x, float y, float w, float cover);
    void groupHead(const char* key, int index, int track, float x, float y, float w);
    // Library → Tracks: every song, sorted and filtered (cached: sorting thousands each frame is not free)
    struct TracksView {
        int sort = -1;
        bool desc = false;
        std::string filter;
        unsigned gen = ~0u;
        std::vector<int> list;
        std::vector<std::pair<char, int>> letters;   // the letter index: first character, first row with it
        unsigned epoch = 0;                          // the scan its indexes belong to (libEpoch)
        std::shared_ptr<struct TracksJob> job;       // a sort running on a worker core
        std::thread thread;                          // its thread
    } tracksView;
    unsigned libEpoch = 0;   // bumps with every rescan: track indexes from before mean other tracks
    unsigned indexGen = 0;   // bumps with every rebuildIndex
    const std::vector<int>& sortedTracks();
    void drawTracksTab(float x, float y, float w, float h, float top);
    void drawFoldersTab(float x, float y, float w, float h, float top);
    int looseTracks();   // songs without an album tag: counted once for each index (the Albums pages' line asked every frame)
    void drawAlbumGrid(float x, float y, float w, float h, float top, bool toolsRow = false);   // toolsRow: Library's row under its tabs
    /** One of your albums as a row (the Albums pages' and search's rows-with-covers and list views): cover, title, artist and count, a chevron. */
    bool albumRow(Id id, int album, float x, float y, float w, float rowH, bool list, uint8_t flags = 0);
    void drawAlbums(float x, float y, float w, float h);
    bool folderTile(Id id, int folder, float x, float y, float w, float cover);
    std::string tracksFilter;   // Library → Tracks: what the filter keeps

    // ================= view_playlists.cpp: every playlist on a page, and their order
    void drawPlaylists(float x, float y, float w, float h, bool inLibrary = false);
    std::vector<int> playlistTracks(int ref) const;   // −1 Liked
    void movePlaylist(int from, int to, bool save = true);   // the sidebar follows; open pages of playlists move with them
    void erasePlaylist(int ref);                      // Delete: pages of it leave the history, later ones keep theirs
    int plHover = -2, plHoverShown = -2;              // the page's playlist under focus (−1 Liked, −2 none): + opens its menu

    // ================= view_files.cpp: Files, a browser of the whole SD card
    struct FilesState {
        std::string listed;            // the folder listed, with or without hidden files, and when (gen bumps to list again)
        bool listedHidden = false;
        unsigned gen = 0, listedGen = ~0u;
        std::vector<FileEntry> entries;
        // listed on a worker (the self test on the console: the card's top took 190-240 ms on the main thread), taken when done
        std::thread lister;
        std::atomic<bool> listDone{ false };
        bool listing = false, listOk = true, listHidden = false;
        std::string listPath;
        unsigned listGen = 0;
        std::vector<FileEntry> listOut;
        std::vector<std::string> clip; // Copy / Move: what Paste here puts in the folder shown (one, or a selection)
        bool clipMove = false;
        std::string hover, hoverShown; // the row under focus: + opens its menu
        std::string focusAfter;        // a row to focus once listed (the folder come up from, a file just made)
        // Select: A ticks rows (Select all / none), then Copy, Move or Delete takes them all; only in one folder at a time
        bool selecting = false;
        std::vector<std::string> selected;
        std::string selectIn;
    } files;
    bool fileSelected(const std::string& p) const;
    void fileSelect(const std::string& p, bool on);
    void fileDeleteMany(const std::vector<std::string>& paths);
    // One piece of work at a time on a worker (copying, deleting a folder): the progress stack shows it (slot 3).
    struct FileWork {
        std::thread thread;
        std::atomic<bool> done{ false };
        std::atomic<int> count{ 0 }, total{ 0 };
        std::string label, dest, result, failed;   // the card's label, the folder it ends in, the toast, what went wrong
        bool library = false;                      // it changed the music folder: a rescan follows
        std::function<void()> after;               // on the main thread once it is done (Storage's Clear)
        int cat = -1;                              // the Settings category its card opens (−1: Files, where it ends)
    } fileWork;
    void drawFiles(float x, float y, float w, float h);
    std::string filesStart() const;                // where Files opens (Settings → Storage and reset)
    void openFiles(const std::string& dir);
    void openFileMenu(const std::string& path);
    /** The rows and header of the Files pages of the sheet (FileMenu, FileInfo, FileLink). */
    void fileSheetRows(std::vector<SheetRow>& rows);
    void fileSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    void filePaste();
    void fileRename(const std::string& path);
    void fileDelete(const std::string& path);
    void fileNewFolder(const std::string& dir);
    bool startFileWork(const std::string& label, const std::string& dest, bool library, std::function<void(FileWork&)> work,
                       std::function<void()> after = nullptr);   // false: another is running (a toast says so)
    void updateFileWork();
    void linkLyrics(const std::string& file, int track);
    /** A rename or move in the music folder: Liked, playlists, your edits, covers and lyric timings follow the paths. */
    void remapPaths(const std::string& from, const std::string& to);
    bool inMusicFolder(const std::string& path) const;
    void reloadNowTrack();                          // tags, lyrics and cover read again (an .lrc just linked)
    void setCoverFor(const std::string& image, const std::vector<int>& tracks);

    // ================= view_settings.cpp
    void drawSettings(float x, float y, float w, float h);

    // ================= view_updates.cpp: Settings → Updates (updater.hpp): releases, changelogs, install, roll back, backups
public:
    std::string selfPath;   // the running program (plat::selfPath)
#ifndef GF_WINDOWED
    cfgmove::Result folder;   // what main found of the settings folder (cfgmove::atLaunch): logged, and asked about when both are there
    /** An older version's .nro went in (Updater::takeBack): the folder back to gatefold-nx before it starts. True when one was due. */
    static bool goBackIfDue();
    void askBothFolders();
#endif
    void frame();
    // A card or sheet as wide as it wants, but never wider than the screen (a phone upright); upright, the sheets rise from its foot
    float cardW(float want) const { return std::min(want, float(css::W) - 16); }
    float cardY(float h) const { return phoneSheets() ? std::max(8.f, float(css::H) - h - 8) : (css::H - h) / 2; }
    static constexpr float kNarrowList = 600;   // a list narrower than this (a phone upright) shows its rows without columns
    /** Search's filter tabs on a narrow page: a phone upright draws that control itself, over the list (view_phone.cpp), and the
     *  list under it is the tab's, as on a wide page; any other narrow page has none (Songs, then In the lyrics, on one page). */
    bool phoneSearchTabs() const { return phoneUpright(); }
    /** Search's filter tabs on a wide page that is not wide enough for them beside Play, Shuffle and New search (a folded-open
     *  sidebar in a short window, a small phone sideways, a tablet upright): on a line of their own under the head, from the
     *  cover's side. They ran under the buttons there. */
    bool searchTabsOwnLine(float w, const std::vector<std::string>& names) {
        if (w < kNarrowList) return false;
        const Font sf{ css::Sm, Weight::SemiBold };
        // the cover and its margins; Play, Shuffle and New search at its smallest (round); the control; the views and the right margin
        float need = 28 + 120 + 22 + (34 + 8) - 8 + 6 + 12 + (6 + 38.f * 4) + 28;
        for (const auto& t : names) need += g.measure(t, sf) + 26;
        for (const char* b : { "Play", "Shuffle" }) need += 16 + 24 + g.measure(b, sf) + 16 + 8;
        return need > w;
    }
    static constexpr float kSearchTabsLineH = 46;
    /** Where the background work's card is, for the toasts that point at it: above Settings in the sidebar; a phone upright has it
     *  in Settings, at the head of its list. */
    const char* workCardWords() const { return phoneUpright() ? "the card in Settings shows how far it is" : "the card above Settings shows how far it is"; }
    /** A phone held upright (Android only: no title bar and no sidebar there, so what they carry has a place on the pages). */
    bool phoneUpright() const { return css::Phone; }
    /** Sheets rise from the foot: the phone's own layout, and any other window taller than wide that is no tablet's (the PC's). */
    bool phoneSheets() const {
#ifdef GF_ANDROID
        return css::Phone;
#else
        return css::H > css::W;
#endif
    }
    int optShowTouch_ = 0;   // dev builds: every touch area outlined ("touch" in options.txt in the settings folder)
    float settingsCtlMax_ = 0;   // Settings: the width a stacked row's control may take (segmented shrinks to it)
    float setSliderW() const { return settingsCtlMax_ > 0 ? settingsCtlMax_ : 180.f; }   // a Settings slider: 180, or its line's width
    bool settingsPhoneDirect_ = false;   // narrow Settings: that page was opened from elsewhere (back returns there, not to the list)
    bool settingsPhoneOpen_ = false;   // narrow Settings: a category's page shown (else the list of categories)
#ifdef GF_WINDOWED
    static constexpr int kNarrowW = 1170;   // under this the pages need the sidebar's room: it folds by itself, and comes back
    // … on a PC it comes back a little later than it went (kNarrowBack wider), so an edge dragged to and fro about that width does
    // not fold and open it each time. Android has none: its width changes with a turn only, and the same width must give the same
    // layout whichever way it was reached
#ifdef GF_ANDROID
    static constexpr int kNarrowBack = 0;
#else
    static constexpr int kNarrowBack = 40;
#endif
    bool narrow_ = false;      // the window is narrow, the sidebar folded by itself (App::fitWindow); your own choice
                               // (store.sidebarCollapsed, Settings › Appearance) is kept as it is, for when there is room again
    // Narrow, the title bar's button opens the sidebar over the page, which stays as it is under it (there is no room to push it
    // aside): until something in it is chosen, the page is pressed, or back. peekK_: how much of its open width lies over the
    // page, 1 while narrow; when the window gets its room back with it open, the page slides out from under it (1 to 0)
    bool sidePeek_ = false;
    float peekK_ = 0;
    Route peekRoute_{};
    bool sidebarFolded() const { return narrow_ ? !sidePeek_ : store.sidebarCollapsed; }
    /** Where the page starts: the sidebar's width, but for what of it lies over the page */
    float pageSideW() const { return sideW - peekK_ * (sideW - css::SideWCollapsed); }
    float rasterScale_ = 0;      // the scale the words and icons are made at: scale_ once the window's edge has come to rest
    double scaleMovedAt_ = -10;  // the scale last changed (a drag under the least size changes it every frame)
    float minFor_ = 0;           // the screen scale the window's least size was last set for
#else
    bool sidebarFolded() const { return store.sidebarCollapsed; }
    float pageSideW() const { return sideW; }
#endif
    Uint64 lastTick_ = 0;
    int hz_ = 60;   // the screen's refreshes a second
#ifdef GF_WINDOWED
    void fitWindow();
    float scale_ = 1;                       // real pixels to an app pixel
    SDL_Texture* frameTex_[2] = {};         // frames are drawn here, then shown: the last whole one is what the text card sits on
    int frameCur_ = 0;
    plat::TextResult textCard(const char* header, const char* guide, const char* initial, char* out, size_t size);
    void textCardOwner();
    static plat::TextResult textCardHook(const char* header, const char* guide, const char* initial, char* out, size_t size);
    bool pointerIsMouse_ = false;           // the system's own arrow shows: the app's round pointer is for the stick
    void closeWindow();
    void showWindow();
    void keepWindowPlace();
    void systemCommands();
    bool mediaHas_ = false, mediaPlaying_ = false;
#ifdef GF_ANDROID
    // what Android's music controls were last told (systemCommands)
    bool mediaLiked_ = false, mediaPushed_ = false;
    int mediaOrder_ = -1;
    double mediaDur_ = -1, mediaPos_ = 0, mediaPosAt_ = 0;
    bool coverToldHidden_ = false;   // a cover held while hidden (no GPU then) was already sent to the system's controls
    // Android's access to the music (plat::storageAllowed): asked for by a card (accessCard) or the music folder page; looked at
    // once a second until it is given, then the music is offered as on any first start
    bool storageOk_ = true;
    double storageCheckAt_ = 0;
    void accessCard();
    void storageGiven();
    // A phone held upright (view_phone.cpp): its own layout of the same pages. Up next and the lyrics page are Now playing's own,
    // opened and closed with their animation (the P values, 0..1); the seek bar is the visualizer until touched (phoneSeekP_)
    bool phonePortrait() const { return css::Phone; }
    void drawPhone();
    bool phoneQueue_ = false, phoneLyrics_ = false;
    float phoneQueueP_ = 0, phoneLyricsP_ = 0, phoneSeekP_ = 1;
    double phoneSeekAt_ = -10;   // the seek bar last touched
    double turnAt_ = -10;        // the phone last turned (upright ⇄ sideways): App::frame fades the new layout in
    // The scale rule (App::fitWindow): under kPhoneDp across, a window held upright is a phone's (its own layout); that layout
    // is kPhoneMinW..kPhoneMaxW app pixels across
    static constexpr float kPhoneDp = 600, kPhoneMinW = 400, kPhoneMaxW = 560;
    float density_ = 2.625f;     // Android's pixels to a dp, as the scale was last made from
    double densityAt_ = 0;
    float phoneToastBottom_ = 0;  // Now playing upright: where a toast sits (its foot: the gap above the seek bar)
    float phoneJoinMove_ = -1;   // Now playing upright: an Automix join's glide (0..1) this frame; -1 no join
    SDL_FRect phoneCoverRest_{};  // … and where its cover rests
    // The Canvas from upright (the user, 2026-11-11: "Canvas is landscape only. Portrait can have a button to activate canvas but it
    // switches to landscape"): 1 the turn asked for (plat::holdLandscape), the Canvas opens once the screen is sideways; 2 open, the
    // screen held sideways until it closes, then given back to the phone's own turning (upright again when held so)
    int canvasTurn_ = 0;
    double canvasTurnAt_ = 0;
    void openCanvasTurning();
    void endCanvasTurn();
    // What the sidebar carries above Settings, for a phone upright (no sidebar): the work's cards and a newer version's banner at
    // the head of Settings' list (phoneWorkCard: its height there, 0 with nothing to show), and a mark on the bar's Settings
    float phoneWorkCard(float x, float y, float w);
    int phoneNowTab_ = 0;         // Now playing's second page upright: 0 Lyrics, 1 Palette (sideways' tabs but Up next, a sheet here)
    float phoneFoot() const;      // upright: where the page ends over the bar (and the mini player), for what floats there
    bool hiddenNow() const { return SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN); }
#endif
    double titleAt_ = 0;
    Uint32 titleBg_ = 0xFFFFFFFF, titleFg_ = 0xFFFFFFFF;
    bool pumping_ = false, resizing_ = false;
    static constexpr float kLightsW = 72;   // the title bar's room for the three lights
    int lightAt(float px, float py) const;
    void pressLight(int i);
    void drawWindowLights();
    void updateLights();
    float lightsShown() const;
    float lightsP_ = 0, lightsHold_ = 0;
    float lightsNear_ = 0;                  // how long the mouse has been near the lights (updateLights)
    bool cameFromInside_ = false;
    int lightDown_ = -1;
    bool dragNoVsync_ = false;              // an edge is held: frames are not queued behind the screen's refresh (App::resizeWatch)
    SDL_FRect titleKeep_[6] = {};           // what is pressed in the title bar, not dragged: the rest of it moves the window
    static SDL_HitTestResult hitTest(SDL_Window* w, const SDL_Point* p, void* self);
    bool typingByTouch_ = false;            // the last input was a finger or a pad: the text card brings the on-screen keyboard
    bool closePending_ = false;             // × during the text card: done when it has closed
    bool fitPending_ = false;               // the window changed under the text card
    bool fullApp_ = false, fullCanvas_ = false, fullNow_ = false;   // full screen: asked for (F11), by the Canvas, as it is
    void applyFullscreen();
    int framePw_ = 0, framePh_ = 0, frameTexW_ = 0, frameTexH_ = 0;
    static int resizeWatch(void* self, SDL_Event* e);
#ifdef GF_DEV
    long statFrames_ = 0, statLate_ = 0;    // what a test run prints at the end
    double statSum_ = 0, statWorst_ = 0, statWork_ = 0;
#ifndef GF_ANDROID
    // A test's hand on the window (devctl.cpp, GF_CTL): a size, a screen and a scale made up; a frame saved; a click; frame times
    void devControl();
    void devShot();
    int devPw_ = 0, devPh_ = 0, devScreenW_ = 0, devScreenH_ = 0, devTapUp_ = 0;
    float devDpi_ = 0, devWheel_ = 0;
    bool devMoved_ = false;
    uint64_t devPad_ = 0;
    std::string devShot_;
    long devN_ = 0;
    double devSum_ = 0, devWorst_ = 0, devWork_ = 0;
#endif
#endif
#endif
private:
    Updater updater;
    bool relaunch = false;              // Gatefold closes and starts again (the new .nro, swapped in at shutdown)
#ifdef GF_DEV
    bool freshStart_ = false, freshRestore_ = false;   // the New user self test (dev only): at shutdown, the folder set aside / put back
#endif
    bool updateQuiet = false;           // the check at launch: nothing said unless a newer version is out
    // A newer version, offered (the user, 2026-11-11): a banner where the background work shows its progress, above Settings, in
    // front of that work's cards; pressed, its sheet (Sheet::UpdateOffer): Update, Later, and the top of what is new. Either
    // answer puts the banner away until Songate next starts, and the work's cards are back in their place; the sheet closed
    // without one leaves it. Update opens Settings › Updates at that version. Only with Check when Songate starts on; betas count
    // only with Beta versions on (Updater::newer: a release is above its own betas)
    bool updLater_ = false;             // answered this run
    ScrollArea* pinnedScroll_ = nullptr;   // a sheet whose first rows stay while the rest scrolls (drawSheet): the rest's area, and
    double pinnedScrollAt_ = -10;          // when it was last drawn: the right stick scrolls it while it is up
    float updBanner_ = 0;               // 0..1: the banner in the work cards' place
    int updateOffer() const;            // the release to offer, or -1
    void drawUpdateBanner(float x, float bottom, float w, bool collapsed, int rel);
    void openUpdateOffer(int rel);
    void answerUpdateOffer(int rel, bool update);
    void updateOfferRows(std::vector<SheetRow>& rows);
    void updateOfferHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    std::vector<UpdBackup> backupsShown;
    double backupsAt = -100;
    std::string backupRoot() const;     // store.backupDir, else sdmc:/config/songate-nx-backups
    // ---- Settings → Storage and reset: where each browser starts (the music-folder picker chooses it), what Gatefold keeps
    int folderFor = -1;                 // 0 Files, 1 import, 2 export, 3 covers, 4 backups; −1 the picker is the music folder's
    std::string& folderPickField();
    const char* folderPickName() const;
    void chooseFolder(int which);
    struct StorageUse { int64_t bytes = 0; int files = 0; };
    struct StorageScan {
        std::thread thread;
        std::atomic<bool> done{ false };
        StorageUse use[9];              // the covers cache, overlay covers, loudness, tag cache, backups, downloads, lyrics found online,
                                        // the server's tracks kept to play again, the server's downloads in Songate's folder
        StorageUse found[9];            // the worker's, taken over when it is done
        bool seen = false;
        double at = -100;
    } storage;
    std::string storageValue(int which);   // "4.2 MB", "Empty", "…" while first measured
    void scanStorage();                 // on a worker; Settings shows the sizes when it is done
    void clearStorage(int which);       // asks first
    const std::vector<UpdBackup>& updateBackups();   // read again every few seconds while shown
    void allowUpdates();                // the card that asks before anything goes online
    void allowBetas();                  // Settings › Updates › Beta versions: asked first, with the warning
    // Settings › Support (view_support.cpp, report.hpp): the report's form, sent on a worker; Atmosphère's crash reports and the
    // console's versions, read on a worker when Support first opens; the app's own log (every toast, with its time)
    void openReport(int kind);          // 0 a bug, 1 an idea
    void allowReports();                // Support's switch: asked first, saying what a report sends and where (as Updates)
    void reportRows(std::vector<SheetRow>& rows);
    void reportHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    void reportFooter(float x, float w, float fy);
    void crashRows(std::vector<SheetRow>& rows);
    void supportScan();                 // (starts the worker once; again after a report)
    void supportTick();                 // every frame: takes the workers' results in
    Identity identity;                  // Connections: what makes a track the same on two devices (identity.hpp)
    Connect connect;                    // Connections: finding other devices, pairing, the link (connect.hpp)
    void allowConnections();            // the switch: says what it means first
    void connectTick();                 // every frame: on or off, and what the worker has to say
    void connectEnterCode(const std::string& peerId);
    void connectAddress();
    double connectAt = 0;
    Syncer syncer;                      // one sync with a paired device (syncer.hpp)
    Copier copier;                      // the songs this device lacks (filecopy.hpp)
    // listening together (together.hpp, view_together.cpp)
    struct Tog {
        enum Stage { Off, Inviting, Joining, On } stage = Off;
        bool host = false, hadBackground = false, queueTaken = false;
        std::string peerId, peerName;
        std::vector<together::Song> songs;
        std::vector<int> tracks;              // each song's track here, -1 not here, kTogServer the server's copy (srv)
        // server songs (the user, 2026-11-07, "Both": your own copy if you have it, else the server's): each song's copy on the server this
        // device is signed in to (an id "" none). The starter whose queue is the server's: all of them
        std::vector<Subsonic::Song> srv;
        int fetchSong = -1;                   // a song to open, waiting for its server file: its position, and whether it rejoins
        double fetchPos = 0;
        bool fetchRejoin = false;
        bool hadServer = false;               // the other device: the server's queue was playing here when the session began
        bool queueWait = false;               // the other device: the queue held until the server's song list is here (20 s at most)
        double queueWaitSince = 0;
        together::Message heldQueue;
        std::vector<uint8_t> have;            // the starter: which songs the other device holds
        int song = -1, pendingSong = -1, missing = 0;
        int64_t serial = 0;
        size_t signature = 0;                 // of the queue sent
        double position = 0, startAt = 0, since = 0, readySince = 0, loggedAt = 0, rejoinAt = 0;
        bool rejoining = false;
        int playingSong = -1;                 // while the next song waits for this one to end
        automix::Plan plan;                   // the join under way: the same on both
        bool joinArmed = false, planNone = false;
        bool joinAutomix = false;             // the join under way is Automix's (else a crossfade): only then its label and animation
        int joinSong = -1;
        bool linkLost = false, rejoinNew = false;   // a listener: the link to the starter dropped: each plays on, on the shared clock
        double lostAt = 0, resumedAt = 0;
        double worstFrame = 0;                // dev log
        uint64_t frameAt = 0;
        bool natural = false, prepared = false, loaded = false, saidReady = false, theyReady = false, nextSent = false, paused = false;
        Queue savedQueue;
        double savedPosition = 0;            // where this device was in it when the session took it
        // One song, two files (together.hpp): the device that did not start it, for a song it holds as another file, the shift
        // between the two, which every position from the starter goes through; 0 when the fit was unclear
        std::vector<uint8_t> otherFile;       // 1: matched by its tags and length, not its bytes
        std::unordered_map<int, double> shift;
        struct Printing { double from = 0, since = 0; std::vector<uint8_t> theirs, mine; bool theirsIn = false, mineIn = false; };
        std::unordered_map<int, Printing> printing;
        bool waiting = false, waitNatural = false;   // a Prepare held until its song's shift is known
        int waitSong = -1;
        double waitPosition = 0, waitSince = 0;
        bool joinShifted = false;             // the join under way: its next song's own shift added
        bool onlyHost = false;                // Who changes what plays (T7): only the starter; set there, told to the other
        int order = 0;                        // the other device: the starter's order (Order), shown in its bar
        int whoSent = -1;                     // the starter: the order last told (-1: tell it again)
        double askedWhereAt = -100;           // the other device: when it last asked where things stand
        // Three or more (BETA 3, the user, 2026-11-05: added from the session; a song only if every device holds it; one dropping
        // leaves the rest playing): the starter keeps each device it asked in; a listener talks only to the starter
        struct Member {
            std::string id, name;
            int state = 0;                    // 0 asked, 1 joining (the queue sent to it), 2 in
            double since = 0;
            std::vector<uint8_t> have;        // which songs of the queue it holds
            int64_t readySerial = -1;         // the last Prepare or Resume it said Ready to
            bool mic = false, needs = false;  // it can listen (a microphone); its output's delay was never measured
            bool lost = false, awaiting = false, resumeDue = false;   // its link dropped; a Resume sent, answer awaited; one to send
            double lostAt = 0, resumedAt = 0;
        };
        std::vector<Member> members;          // the starter's
        std::vector<std::string> others;      // a listener: the other devices in it, as the starter says
        // a listener just in: where things stand, held until its clock knows the starter's (the user, 2026-11-05: "the third device
        // ... is connected but is not playing together": a Resume acted on with no clock yet aimed at the wrong moment)
        bool resumeHeld = false;
        together::Message heldResume;
        // the delay, measured (micmeasure.hpp): the starter runs it; the device listening records
        struct Measure {
            int stage = 0;                    // 0 none, 1 under way
            double endAt = 0, giveUpAt = 0;
            std::vector<std::string> slots;   // the starter's: each slot's device ("" itself)
            std::string listener;             // the starter's: who listens ("" itself)
            bool listening = false;           // this device records
            bool first = false, resume = false;   // the session's first song waits for it; the music plays again after it
            int firstSong = -1;
            double firstPos = 0;
        } measure;
    } tog;
    micm::Measurer measurer;
    /** The starter: chirps on every device, one listening (first: before the session's first song). False: no device can listen. */
    bool togetherMeasure(bool first, int song = -1, double position = 0);
    void togetherMeasured(const micm::Result& r);   // the starter: the corrections, to each device
    void togetherMeasureEnd();
    void togetherApplyDelay(double correctionMs);    // this device's Timing, for its output
    std::string togetherListener() const;            // the starter: who would listen ("" none, "-" itself)
    void togetherApplyResume(const together::Message& m);
    Tog::Member* togMember(const std::string& id);
    /** The starter: one more device asked into the session (Settings, the Devices sheet). */
    void togetherInvite(const std::string& id, const std::string& name);
    /** The starter: a device out of the session; tell: it is told to leave. The last one out ends the session here. */
    void togetherDrop(const std::string& id, const std::string& why, bool tell);
    void togetherSendMembers();
    bool togetherHaveAll();               // every device in has said which songs it holds: tog.have, those all hold
    void togetherResumeTo(Tog::Member& m);
    bool togetherMembersTick();           // false: the session ended
    /** The devices in the session besides this one, in words: "PC", "PC and Phone", "PC, Phone and Deck". */
    std::string togetherNames() const;
    static const char* moodIcon(int m);   // a mood's own icon (its Mood playlist's)
    static std::string togetherStateOf(const Tog::Member& mb);   // a device in the session, in words for its row
    unsigned togPrintGen = 0;           // a session's prints: one from before is dropped
    bool togetherOn() const;
    bool togetherGuest() const;
    /** The other device, while the starter alone changes what plays: its Previous, Play, Next, seek and order are off. */
    bool togetherLocked() const;
    /** The bar's and the Canvas's playing controls are someone else's: AirPlay's iPhone, or the starter of a session. */
    bool controlsOff() const { return (extOn && !srvq_.on) || togetherLocked(); }
    void togetherSetOnlyHost(bool only);
    void togetherSendWho();
    /** The order the bar shows: in a session, the other device shows the starter's, which decides what comes next on both. */
    Order shownOrder() const { return togetherOn() && !tog.host ? Order(tog.order) : store.order; }
    /** The bar's order button: the next order here, or, on the other device of a session, asked of the starter. */
    void cycleOrder();
    void togetherSend(const together::Message& m);   // to the session's device, else the one being handed to, else the one being answered
    void togetherSendTo(const std::string& peer, const together::Message& m);
    std::string togFrom_;   // the device whose message is being handled (togetherHandle)
    together::Song togetherSong(int track) const;
    void togetherBegin(const std::string& peerId, const std::string& peerName, bool host);
    void togetherEnd(const std::string& why);
    void togetherStart(const std::string& peerId, const std::string& peerName);
    void togetherLeave();
    void togetherSendQueue();
    void togetherPrepare(int song, double position, bool natural);
    void togetherCancelNext();
    void togetherPlanJoin();
    void togetherArmJoin(int song, const automix::Plan& plan);
    void togetherJoinStep();
    void togetherRejoin(int song, double moment, double position);
    /** The device that did not start it: a position in the starter's file of `song` as one in this device's, and back. */
    double togetherMine(int song, double theirs) const;
    double togetherTheirs(int song, double mine) const;
    bool togetherShiftKnown(int song) const;
    void togetherAskPrint(int song);
    void togetherPrintsTick();
    void togetherPrepareGuest(int song, double position, bool natural);
    bool togChirps = true;              // dev: Settings > Devices
    int soloStage_ = 0;                 // dev, GF_SOLO_SYNC: 0 not yet, 1 loaded, 2 started
    double soloAt_ = 0;
    /** After a session: this device's own song from its queue back in the player and on screen, at its place (the background
     *  player takes it from there). */
    void togetherOwnBack(double at, bool play);
    void togetherPrepareHere(int song, double position, bool natural);
    void togetherLoadNow();
    void togetherStartAt(double at);
    bool togetherTrap(bool seek, double seconds);
    void togetherHostSeek(double seconds);
    void togetherSeekHere(double at, double seconds, bool playing);
    void togetherHostPause();
    void togetherHostPlay();
    bool togetherPlayAt(int orderPos);
    void togetherFinished();
    void togetherHandle(const together::Message& m);
    void togetherTick();                // every frame
    /** A device this console does not know wants to play over AirPlay: Allow once, Always, or Cancel. answer: 0 no, 1 once, 2 always
     *  (no AirPlay in it: the preview draws it for a mock-up). */
    void airplayQuestion(const std::string& name, std::function<void(int)> answer);
#ifdef GF_AIRPLAY
    AirPlay airplay;                    // an iPhone plays here (airplay.hpp)
    AirSend airsend;                    // … and this device plays to AirPlay speakers (airsend.hpp; the Devices sheet's Speakers)
    void airsendTick();
    int airLastTrack_ = -2;
    bool airsendOn_ = false, airsendHadBackground_ = false;   // speakers chosen; the background player handed back after them
    std::vector<uint8_t> airCover_;   // the song's cover for the speakers (TrackLoader::Result::airCover)
    unsigned airCoverGen_ = 0;
    std::string airSentKey_, airWaitKey_;   // the details sent; the ones waiting for the speakers to reach the song
    double airWaitUntil_ = 0;
    bool airLastPlaying_ = false;
    double airLastPos_ = 0, airLastAt_ = 0, airMetaAt_ = -100;
    bool airplayOn = false, airplayHadBackground = false;
#endif
    // An outside source on screen (AirPlay): its song, artist, album and cover in now.info and now.cover, as a track's, and no
    // library song under them (no like, actions or Up next); your own song comes back when it ends
    bool extOn = false;
    std::string marqueeTitle;   // the bar's marquee starts again for a new title (an outside source keeps no track)
    std::string extFrom;
    void extBegin(const std::string& from);
    void extInfo(const std::string& title, const std::string& artist, const std::string& album);
    void extCover(std::vector<uint8_t> image);
    double extClearAt = 0;   // "no cover" said: cleared at this time unless a cover comes first (iOS says none between songs)
    void extTick();          // every frame while an outside source plays
    void extEnd();
    double extPos = 0, extPosAt = 0;   // where the outside song was, and when that was said (it runs on from there)
    void extProgress(double position, double duration);
    /** Where the song shown is: the player's, or an outside source's as it last said, counted on. */
    double shownPosition() const;
#ifdef GF_AIRPLAY
    std::vector<std::string> airplayIds() const;
    unsigned airplayCoverGen = 0;   // the cover last taken (AirPlay::View::coverGen)
    unsigned airplayProgressGen = 0;
    double airplayLoggedAt = 0;          // dev: the stream's numbers in the log every 10 s
    bool airplayAsking = false;          // the question about a new device is up
    size_t airplayAllowedSent = size_t(-1);   // the remembered devices AirPlay was told of (their count, and they change only here)
    void airplayTick();                 // every frame
#endif
    // handoff (view_together.cpp): this device's queue and place move to the other, which takes over
    void handoffTo(const std::string& peerId, const std::string& peerName);
    void handoffTake(const together::Message& m, const std::string& from, const std::string& name, double receivedAt, bool waited = false);
    void handoffPlay(int track, bool playing, double at);
    void handoffUndo();
    std::string handoffPeer, handoffName;     // sent, and waiting for its answer
    double handoffSince = 0;
    Queue handoffOld;                         // the queue this device had: Undo, for a while
    double handoffOldAt = 0, handoffOldUntil = 0;
    std::vector<Connect::Paired> devicesLinked;   // kept once a second: asked for by the player bar every frame
    double devicesAt = -10;
    bool devicesPaired = false;
    bool syncBusy = false;                // a sync or a copy under way: the arrows by the name (Connections' tick, twice a second)
    float syncIconA = 0;                  // … faded in and out
    bool copyShown = false;
    Copier::State copyWas = Copier::State::Idle;
    double copyStartedAt = 0, copyGapMs = 0;   // how fast, and the longest frame while it ran
    void syncNow(const std::string& peerId, bool answering);   // copies the library a little each frame, then begins
    void syncBuildStep();
    struct SyncBuild {
        bool on = false, wantIdentity = false;
        size_t next = 0;
        unsigned epoch = 0;
        std::vector<Identity::Want> wants;
        Syncer::Job job;
    };
    SyncBuild syncBuild;
    unsigned syncIdentityEpoch = ~0u;    // the library the identities were read from
    size_t syncIdentityEdits = 0;
    double syncWorstMs = 0;              // the longest a sync held a frame
    void saveLevels();                  // all at once: when Songate closes
    struct GainEntry { uint64_t hash; float track, album; };   // GfbgGain
    std::vector<GainEntry> gainsOut;
    bool gainsBuilding = false;         // gains.bin, a little each frame
    size_t gainsNext = 0;
    unsigned gainsEpoch = 0;
    void gainsStep();
#ifdef GF_DEV
    Uint64 syncFrameAt = 0;
    double syncGapMs = 0, syncStepMs = 0;   // the longest frame during a sync; update's slowest step then
    std::string syncStep;
    double syncPhaseGap[10] = {}, syncPhaseUpdate[10] = {};   // by phase: the longest frame, and how much of it was update()
    double syncIdleGap = 0;              // the longest frame with no sync running, once settled: what a sync is measured against
    double syncWorstProf[8] = {};        // the longest frame's sections
    double syncAfterUntil = 0, syncAfterGap = 0, syncAfterAt = 0, syncAfterProf[8] = {};   // the 20 seconds after a sync: its longest frame, and when
    int syncAfterSlow = 0;               // … and how many frames took over 33 ms
    std::string syncAfterStep;
    int tlSec = -1, tlSlow = 0;          // the first 70 seconds, second by second
    double tlWorst = 0, tlPresent = 0, tlUpdate = 0, tlPage = 0, tlBars = 0, tlOver = 0;   // (the boot log: the worst frame's parts)
    const char* tlStep = "";
    std::string syncWorstStep;           // and update's slowest step in it
    int syncWorstPhase = 0;
#endif
    void syncApply(const syncdata::Changes& c);
    void syncDeleteStep();               // every frame while a sync has songs to delete
    void rebuildBehind(bool walk);       // the library without stopping a frame: after a sync's deletions, or songs that arrived
    std::vector<std::string> syncDeleting, syncDeleted;   // still to go; gone
    bool syncDeleteOn = false, syncRebuilding = false;    // deleting; the library without them being built, on a worker
    std::thread syncRebuild;
    std::atomic<bool> syncRebuilt{ false };
    Library syncNext;
    std::vector<std::string> syncNextFiles;
    unsigned syncRebuildEpoch = 0;
    std::string syncPeer, syncPeerName;  // the sync waiting for the identities
    bool syncWaiting = false, syncAnswering = false, syncShown = false;
    bool syncAskCovered_ = false;   // another card took the first sync's question's place: asked again when that one closes
    std::vector<std::string> syncLinked;   // who was linked at the last look: a new link syncs, if automatic
    uint64_t syncStamp() const;         // changes whenever what is shared changes
    std::string folderInMusic(const std::string& path) const;   // a folder playlist's folder within the music folder, or ""
    /** The playlists Connections carries: your own and folder ones (smart ones are made from each library). */
    static bool syncedList(const FolderPlaylist& p) { return p.own() || p.folder(); }
    // Each device's last sync (BETA 3, several devices): what came from one is still news to the others, so it goes on to them
    struct PeerSync {
        uint64_t sent = 0;     // the stamp sent to it
        unsigned plays = 0;    // stats.gen() then
        double at = -100;      // when
    };
    std::map<std::string, PeerSync> syncFor_;
    uint64_t syncLast = 0;               // the latest stamp looked at
    double syncChangedAt = 0, syncStampAt = 0;
    std::vector<std::string> syncAskedLater;   // devices that asked while this one was busy or still starting
    void syncAllDue() { for (auto& kv : syncFor_) kv.second.sent = 0; }   // a change here: every device hears of it
    std::string copyFrom_;               // the device songs are copied from now; each linked one in turn
    double copyFromAt_ = -100;
    bool syncWatchIdentity = false;      // identities being checked behind a sync that did not wait: if one changed, sync again
    double syncSharedAt = -1000;         // measurements and covers: they are heavy, so now and then, never during startup
    double syncShareTriedAt = -1000;     // … and if the other device would not, not asked again at once
    bool syncShareDue() const { return store.syncResults && time > 30 && time - syncSharedAt > 600 && time - syncShareTriedAt > 300; }
    bool syncSettled() const;            // nothing automatic while Songate is still starting up
    void identityMeasure();             // every track of the library, on its worker
    Identity::Want identityWant(const Track& t) const;
#ifdef GF_DEV
    bool identityAsked = false;
#endif
    // Settings › Last.fm (lastfm.hpp)
    Lastfm lastfm;
    Subsonic subsonic;                  // music from your own server (subsonic.hpp)
    void subsonicSignIn();              // Settings › Library › Subsonic server: the address, the user, the password, asked in turn
    // the Server tab (view_server.cpp): level 0 its artists, 1 an artist's albums, 2 an album's songs; route.query "id\x1fname"
    void openServer(int level, const std::string& id, const std::string& name);
    void drawServer(float x, float y, float w, float h, bool inLibrary = false);   // inLibrary: Library › Server (its top, the source switch)
    float librarySource(float rx, float cy, float hgt = 36);   // Local | Server, right-aligned at rx (while signed in); its width
    float librarySourceW();   // its width (0 when not shown)
    /** The sidebar's Albums, Artists, Genres, Folders and Library show the server's (Library on Server, its music Apart: the user, 2026-11-06) */
    bool serverSidebar() { return store.libSource == 1 && subsonic.view().state == Subsonic::SignedIn; }
    void openServerSection(int tab) { store.serverTab = tab; navigate(View::Server, 6, std::to_string(tab)); }   // one view, as the sidebar's own pages
    // Playlists follow Library's Local | Server (the user, 2026-11-06: "make the library local/server toggle global for playlist too");
    // with its music Mixed in, the server's playlists come after yours
    // Library's switch, its music Apart: what plays finishes (or is paused), then the other side's last track comes back, paused where
    // it was (the user, 2026-11-06); each side's queue is kept
    int libSwitchTo_ = -1;            // a switch waiting: 0 yours, 1 the server's
    int libSwitchTrack_ = -1, libSwitchPos_ = -1;
    double srvSavedAt_ = 0, localSavedAt_ = 0;   // where each was in its track
    bool srvLoadPaused_ = false;      // the server's track comes back paused, at srvSeek_
    double srvSeek_ = 0;
    void libSourceChanged();          // Library's switch pressed
    void libSourceFollow();           // … and the page open goes to the same page of the other library (the sidebar's follow it)
    void libSwitchTick();             // each frame: the song ended, or paused: the other side comes back
    void libSwitchNow();
    bool serverPlaylistsShown() { return serverSidebar(); }
    void drawServerPlaylists(float x, float y, float w, float h, float cy, float oy, ListMode mode,
                             const std::vector<Subsonic::Playlist>* list = nullptr);   // the Playlists page's body: the server's, or a list given (search's)
    /** Library's views switch (Albums, Artists, Genres, Folders, Tracks: the server's order) and Local | Server beside it, at a head's top
     *  y; sideways only (upright, Library's page has them as rows). store.libTab keeps its numbers: 0 Albums, 1 Tracks, 2 Folders, 3 Artists,
     *  4 Genres. */
    float libraryTabsRow(float x, float w, float y);   // its width (0: not shown)
    bool serverTile(Id id, const std::string& cover, float x, float y, float w, bool round, const std::string& title, const std::string& sub, uint8_t flags = 0,
                    const char* none = nullptr);
    SDL_Texture* serverCover(const std::string& id, int size = 300);
    /** A server album as a row, as albumRow. */
    bool serverAlbumRow(Id id, const Subsonic::Album& al, float x, float y, float w, float rowH, bool list, uint8_t flags = 0);
    void serverCoverPump();
    /** Stage 3: the server's albums for the library's Albums (In the library); asked once, empty until they came. */
    const std::vector<Subsonic::Album>& serverAlbums();
    std::vector<Subsonic::Album> serverAlbums_;
    unsigned serverAlbumsGen_ = ~0u;
    // the Server tab's views (Albums, Artists, Genres, Folders, Tracks) and the library's artists and genres mixed in: each list
    // copied from the worker once it came (not again each time the worker's view moves on, as covers come), cleared at sign-out
    int srvSort_ = 0;   // the server's Albums: 0 by artist, 1 by title, 2 by year
    struct SrvLists {
        bool hA = false, hG = false, hS = false, hF = false;
        std::vector<Subsonic::Artist> artists;
        std::vector<Subsonic::Genre> genres;
        std::vector<Subsonic::Song> songs;
        std::vector<Subsonic::Entry> folders;
        unsigned gen = 0;   // bumps when one of them came
    } srvl_;
    const std::vector<Subsonic::Artist>* serverArtistList();
    const std::vector<Subsonic::Genre>* serverGenreList();
    const std::vector<Subsonic::Song>* serverSongList();
    const std::vector<Subsonic::Entry>* serverFolderList();
    struct SrvPage { std::string key; bool have = false; std::string name, cover; std::vector<Subsonic::Album> albums; std::vector<Subsonic::Entry> entries;
                     std::vector<Subsonic::Song> songs; };
    SrvPage srvPage_[2];   // [0] the page shown (an artist's, a genre's albums, a folder), [1] a library page's twin on the server
    const SrvPage& serverPage(char kind, const std::string& id, int slot = 0);   // 'R' an artist's albums, 'G' a genre's, 'D' a folder, 'P' a playlist
    std::vector<Subsonic::Playlist> srvPlaylists_;
    // the Playlists page's order for the server's playlists, Songate's own (Subsonic has none): ids, one a line, in server-playlist-order.tsv
    std::vector<std::string> srvPlOrder_;
    bool srvPlOrderRead_ = false;
    void applyServerPlaylistOrder();
    void moveServerPlaylist(int from, int to, bool save = true);
    void saveServerPlaylistOrder();
    unsigned srvPlaylistsGen_ = ~0u;
    const std::vector<Subsonic::Playlist>* serverPlaylistList();
    void sendPlaylistToServer(const std::string& name, const std::vector<int>& tracks);
    std::string srvSendName_;            // a send waiting for the server's lists (serverTick sends it once they came)
    std::vector<int> srvSendTracks_;   // a playlist of yours, to the server (matched tracks)
    void savePlaylistHere(const std::string& name, const std::vector<Subsonic::Song>& songs);   // a server playlist, as one of yours
    std::unordered_map<std::string, std::vector<std::string>> srvGenreCovers_;
    const std::vector<std::string>& serverGenreCovers(const std::string& genre);
    void serverQuad(const std::vector<std::string>& covers, float x, float y, float size, float rad);
    void serverBadge(float tx, float ty, float tileW, bool down = false);   // down: downloaded (the arrow)
    std::string serverDlRoot() const;   // where downloads go: Songate's folder, or the music folder's Songate downloads
    void serverSignOutCard();           // Sign out: what to keep (the user, 2026-11-06)
    void serverSignedOut(bool removeAll);
    bool srvRescanAfterWipe_ = false;
    // stage 5: the heart on a server track stars it; its play is told to the server (half of it, or 4 minutes, as Last.fm's)
    bool currentLiked();
    void toggleCurrentLike();
    std::string srvPlayFor_;
    double srvPlayHeard_ = 0;
    bool srvPlaySaid_ = false, srvPlayDone_ = false;
    std::vector<Subsonic::Song> srvStarred_;   // the server's starred tracks (the Liked page's group), copied when they change
    unsigned srvStarredGen_ = ~0u;
    size_t srvLikeSig_ = 0;                    // what the last like sync saw (Liked, the stars, the lists)
    double srvLikeAt_ = 0;
    void serverLikesSync();
    bool serverGenreTile(Id id, const Subsonic::Genre& gn, float tx, float ty, float tileW, uint8_t flags = 0);
    void serverForget();   // signed out: every copy, texture and question forgotten
    void drawServerQueue(float x, float y, float w, float h);   // the Playlist page while the server's tracks play (the user: "its not added to the temporary playlist")
    Subsonic::Song srvMenuSong_;   // the server track whose menu is open
    // the server track under focus (the D-pad, the stick's pointer, a finger held): + and a long touch open its menu, as yours
    Subsonic::Song hoverSrv_, hoverSrvShown_;
    bool hoverSrvOn_ = false, hoverSrvShownOn_ = false;
    int hoverSrvIdx_ = -1, hoverSrvIdxShown_ = -1;
    void hoverServer(Id id, const Subsonic::Song& s, int idx = -1) { if (ui.focused(id)) { hoverSrv_ = s; hoverSrvOn_ = true; hoverSrvIdx_ = idx; } }

    int srvMenuIdx_ = -1;          // … its place on the server playlist's page it was opened from, else −1
    void openServerMenu(const Subsonic::Song& s, int idx = -1);
    // server playlists made and changed here, as yours are (the user, 2026-11-06: "Make it consistent")
    Subsonic::Playlist srvMenuPl_;
    int hoverSrvPl_ = -1, hoverSrvPlShown_ = -1;   // the Playlists page's server playlist under focus (+ opens its menu)
    std::string srvOpenNew_;                       // a playlist just made from New playlist: opened once it is listed
    std::string srvGrabPl_;                        // Move on a server playlist's page: its id (its order sent when put down)
    void serverNewPlaylist(const std::vector<std::string>& songIds);
    void serverRenamePlaylist(const std::string& id, const std::string& name);
    void serverDeletePlaylist(const std::string& id, const std::string& name, int songs);
    void serverPlaylistChanged(const std::string& id);
    // server playlists' icons, chosen here as yours (the user, 2026-11-06): kept on this device by the playlist's id; none: the cloud
    std::unordered_map<std::string, std::string> srvPlIcons_;
    bool srvPlIconsRead_ = false;
    const char* serverPlaylistIcon(const std::string& id);
    void setServerPlaylistIcon(const std::string& id, const std::string& key);
    void openServerPlaylistMenu(const Subsonic::Playlist& p, bool icons = false);
    // genres as playlists, as Folders' (the user, 2026-11-06: "lets do the exact same thing but for genres"): yours follow the genre
    // (a smart Genre playlist, "smart:genre:<name>"); the server's are made with the genre's tracks as they are now
    void addGenresAsPlaylists();
    void genrePlaylistButton(const std::string& genre, float& bx, float by);
    void addServerGenresAsPlaylists();
    void serverGenrePlaylistButton(const std::string& genre, float& bx, float by);
    std::vector<std::string> serverGenreSongIds(const std::string& genre);
    void serverQueueAdd(const Subsonic::Song& s, bool next);   // Play next / Add to the end (the server's queue; none playing: it plays)
    // a server track's lyrics: Songate's folder or the server's, else (online lyrics on) LRCLIB's as for a song of yours
    std::string srvLyrFor_;   // the track asked for, until answered
    std::string srvInfoFor_;  // a server track whose file's format was asked (Now playing's chips)
    std::string srvFile_;     // the server track playing: its file (fetched or downloaded), for its waveform
    std::string playingFile() const { return srvq_.on ? srvFile_ : now.track >= 0 ? lib.tracks[size_t(now.track)].path : std::string(); }
    std::string lyrHerePath() const;   // the song shown now, for the lyrics: a server track's "server:<id>", else your track's path
    // the four list views (view_tracks.cpp's: tiles, by album, with covers, the plain list) for the server's tracks; order: indexes into all
    std::vector<ListEntry> serverLayout(const std::vector<Subsonic::Song>& all, const std::vector<int>& order, float w, ListMode mode) const;
    float drawServerBody(const char* key, const std::vector<Subsonic::Song>& all, const std::vector<int>& order, float x, float top, float w, float clipY,
                         float clipH, ListMode mode, bool extended, const std::function<void(int)>& play);
    bool serverRow(Id id, const Subsonic::Song& s, int number, float x, float y, float w, float h, bool covers, bool extended);
    bool serverTrackTile(Id id, const Subsonic::Song& s, float x, float y, float w, float cover);
    void serverGroupHead(const char* key, int index, const std::vector<Subsonic::Song>& all, const std::vector<int>& order, float x, float y, float w);
    float serverFolders(const char* key, const std::vector<const Subsonic::Entry*>& dirs, float x, float cy, float w, float y, float h, ListMode mode);
    bool serverPlaying(const Subsonic::Song& s) const { return srvq_.on && srvq_.pos >= 0 && srvq_.songs[size_t(srvq_.pos)].id == s.id; }
    // the Server tab's Tracks: sorted and filtered as Library › Tracks (store.tracksSort, tracksDesc, viewTracks), its own filter
    struct SrvTracksView { unsigned gen = ~0u; int sort = -1; bool desc = false; std::string filter; std::vector<int> list; std::vector<std::pair<char, int>> letters; } srvTracks_;
    std::string srvFilter_;
    // The grids' orders, made again only when the index changed (they once also held the server's among yours: that "Mixed in"
    // went with the title bar's switch, the user, 2026-11-09): (your artist or genre, -1), (your album, -1).
    const std::vector<std::pair<int, int>>& artistMix(bool genre);
    std::vector<std::pair<int, int>> artistMix_[2];
    unsigned artistMixIndex_[2] = { ~0u, ~0u };
    const std::vector<std::pair<int, int>>& albumMix();
    std::vector<std::pair<int, int>> albumMix_;
    unsigned albumMixIndex_ = ~0u;
    int albumMixSort_ = -1;
    size_t albumMixLocal_ = ~size_t(0);
    std::unordered_map<std::string, SDL_Texture*> serverCovers_;
    std::vector<std::pair<std::string, SDL_Surface*>> serverReady_;
    std::unordered_set<std::string> serverAsked_;   // what was asked of the server once (cleared at sign-out)
    // Stage 2: server songs play in a queue of their own (Now playing shows them as an outside source, with working controls);
    // playing anything of the library ends it. Each song is fetched to the card first, the next one ahead
    struct ServerQueue {
        bool on = false;
        std::vector<Subsonic::Song> songs;
        int pos = -1;
        std::string name;
        std::string waitFor, coverFor;   // a song being fetched; a cover being fetched
        bool hadBackground = false;      // the console's background player handed back after
        bool session = false;            // a listening together session's server songs: not kept for Library's switch
    } srvq_;
    void serverShow(int i);              // Now playing for srvq_.songs[i]: its tags, cover, lyrics, the next one fetched ahead (no player)
    ServerQueue srvSaved_;   // the server's queue as it was left (its songs, its place, its name): Library's switch brings it back
    /** The server signed in to, as a print (Identity::textHash of its address); "" signed out. */
    std::string serverPrint();
    /** A server song as Listening together and Handoff send it: its loose key from its tags, its length, the server's print, its id. */
    together::Song serverSongAcross(const Subsonic::Song& s);
    // Handoff with server songs: the queue this device had, when it was the server's (Undo brings it back); a handoff waiting for the
    // server's song list (to find its songs there)
    ServerQueue handoffOldSrv_;
    bool handoffOldWasSrv_ = false, srvResumeAfter_ = false;   // (srvResumeAfter_: a server track loaded paused at srvSeek_, then played)
    struct HandoffWait { bool on = false; together::Message m; std::string from, name; double receivedAt = 0, since = 0; } handoffWait_;
    void playServer(const std::vector<Subsonic::Song>& songs, int start, const std::string& name);
    // the server's queue, remembered for the next launch as yours is (the user, 2026-11-07): server-queue.tsv, written with the session
    std::string serverQueueText();
    void restoreServerQueue(bool mayPlay = true);   // mayPlay false: kept for Library's switch even when it was the one playing
    void serverPlayAt(int i);
    // listening together with server songs (view_together.cpp)
    static constexpr int kTogServer = -2;
    bool togHere(int song) const;        // this device plays it (its own file or the server's)
    std::string togPath(int song);       // its file here; "" while its server copy is being fetched (asked first)
    double togDuration(int song) const;
    int togIndexNow() const;             // the starter: the session's song playing (its queue's place)
    void togSetPos(int song);            // the queue's place on this song
    void togLoad(int song, bool paused, bool start = true, double fade = 0, const automix::Plan* mix = nullptr);
    size_t togetherQueueSig() const;     // the starter's queue as it is (the one sent, or changed since)
    void togetherTakeQueue(const together::Message& m, bool waited);
    bool togetherPlayServer(int i);      // a server song of the queue chosen: the session's, as togetherPlayAt
    void serverTick();
    void serverLeave();
    void allowLastfm();                 // the switch: says what is sent and where first
    void lastfmTick();                  // every frame: what was heard, what plays, the background player's scrobbles
    bool lastfmSong(const Track& t, double duration, int64_t when, Lastfm::Song& out) const;   // false: not enough tags
    void lastfmHeard(const std::string& path, int64_t when);
    SDL_Texture* lastfmCode(int& side);  // the sign-in page's QR code: made once, dropped by lastfmTick
    double lfmHeard = 0;                // seconds of the current track heard here
    int64_t lfmBegan = 0;               // when they began
    bool lfmDone = false, lfmSaid = false;   // scrobbled; announced
    double lfmBgAt = 0;
    std::vector<std::pair<std::string, int64_t>> lfmBgWait;   // the background player's, until the library knows them
    SDL_Texture* lfmQr = nullptr;
    int lfmQrSide = 0;
    bool lfmShowUrl = false;
#ifdef GF_WINDOWED
    std::string lfmOpened;              // the sign-in page the browser was sent to
#endif
    void logLine(const std::string& s); // the app's log, without a toast
#ifdef GF_DEV
    // Debug › Testing (selftest.cpp): Basic, Advanced and Full, each a scripted walk of the app logging frames and what looks wrong
    struct SelfTest;
    SelfTest* selfTest_ = nullptr;
    bool selfTestOn_[4] = {};    // Basic, Advanced, Full, New user
    /** Listening together's chirps due from this device (from its wake sound before the first to after the last): "playing" to the
     *  system meanwhile, which Bluetooth passes to the speaker (the user, 2026-11-07: a Marshall Kilburn II plays nothing, Android's own
     *  sounds neither, while the phone says paused; "for all devices"). */
    // (bool chirpsDue() is declared by sortByDrag, for every build)
    bool selfTestFresh_ = false; // the New user test running: the first start's question cards come, and it answers them
    void newUserTestAsk();       // its card: what it does, Start
    void newUserTestGo();        // Start: Songate closes, sets the folder aside, starts as new
    void newUserTestAtLaunch();  // a launch with the folder aside: the test (or, interrupted before, the folder put back)
    void selfTestStart(int level);
    void selfTestStop();
    void selfTestTick(float frameMs, float workMs);
    bool selfTestRunning() const;
    bool selfTestJob(Job& j);
#endif
    /** A self test running (dev): the first start's questions (covers, the overlay's, lyrics) wait, so it never answers one. */
    bool selfTestBusy() const {
#ifdef GF_DEV
        return selfTest_ != nullptr && !selfTestFresh_;
#else
        return false;
#endif
    }
    void saveLog();                     // log.txt, by the writer thread: every 30 s with news, and as Songate leaves for HOME
    // The opening (view_opening.cpp)
    void openingScene(double t, float alpha);
    void openingFirst();
    void drawOpening();
    double openingAt_ = -1;
    bool openingDone_ = false, openingSkip_ = false;
    std::vector<float> chime_;          // the bells, until they have rung
    std::thread chimeThread_;           // joined in drawOpening or at exit
    std::atomic<bool> chimeReady_{ false };
    std::string reportBody() const;     // the form and the app's part, Markdown (the worker adds the console's and the crashes)
    std::thread supportThread_, reportThread_;
    std::atomic<bool> supportReady_{ false }, reportDone_{ false };
    bool supportAsked_ = false;
    report::System supportSys_;
    std::vector<report::Crash> supportCrashes_;
    bool reportOk_ = false;
    int reportNumber_ = 0;
    std::string reportErr_;
    double reportSentAt_ = -1e9;        // one report each five minutes
    std::vector<std::string> logLines_; // the app's log: at most 200 lines, saved to log.txt now and then (supportTick)
    bool logDirty_ = false;
    double logSavedAt_ = 0;
    void checkUpdates(bool quiet);
    void openRelease(int i);            // its changelog and Install / Go back (Sheet::Release)
    void installRelease(int i);         // asks first, saying all that will happen
    void restoreBackup(const UpdBackup& b);
    void resetSettings();    // Settings → Reset: asks first; every setting as it came, your music and lists stay
    void defaultSettings();  // ... without asking (Reset settings, and a factory reset's Settings part)
    // Settings → Reset → Factory reset: a sheet of what Gatefold keeps, each with a switch (factoryPick), then Erase what
    // is ticked, or Erase everything (asks twice). It is done when Gatefold closes (factoryMask_), which then starts again.
    enum FactoryPart { FacSettings, FacMusic, FacPlaylists, FacLiked, FacEdits, FacLyrics, FacLyricsOnline, FacEqPresets, FacStats, FacQueue, FacLastfm, FacConnections, FacCaches, FacParts };
    void openFactory();
    void factoryRows(std::vector<SheetRow>& rows);
    void factoryErase(unsigned mask);   // asks (twice for everything), then closes Gatefold
    unsigned factoryPick = 0, factoryMask_ = 0;
    std::thread eraseOld_;   // the next launch: a factory reset's old folder deleted in the background
    void finishUpdate();                // Restart now: the background player, then Gatefold
    void updateTick();                  // every frame: what the worker finished
    void updateSheetRows(std::vector<SheetRow>& rows);
    void updateSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub);
    const char* changelogIcon(const std::string& name) const;
    std::string updateStatus() const;   // the worker's progress in words ("Downloading 1.2.0 · 4.1 of 12.9 MB")

    // ================= view_sheet.cpp: the track menu
    Sheet sheet;
    int hoverTrack = -1, hoverTrackShown = -1;   // the track row under focus this frame / last frame
    int hoverQueue = -1, hoverQueueShown = -1;   // its position in the queue, on the Playlist view and Up next
    bool hoverUpNext = false, hoverUpNextShown = false;
    /** queuePos ≥ 0: opened on a row of the queue, which adds Remove, Move and Play next for that row. */
    void openTrackMenu(int track, int queuePos = -1, bool upNext = false);
    void openTagEditor(int track);   // the track menu's Edit tags…: its fields as they are now (nothing saved until Save)
    void closeSheet();
    void sheetBack();

    // ================= view_smart.cpp: smart playlists
    void openNewList();
    void openSleep();
    void openDevices();   // the player bar's devices button: what can be done with other devices, from anywhere
    void openAirPlay();   // the title bar's AirPlay button: the speakers this device plays to, and receiving
    // Settings → Appearance → Custom: the colour picker (view_smart.cpp). The field changes live, so the
    // interface shows the colour as it is picked; Cancel (and B) put back what it was.
    struct ColourPick {
        int* field = nullptr;   // store.customBg / customAccent / customAccent2 (0xRRGGBB, −1 the cover's)
        int before = 0;
        float h = 0, s = 0, v = 1;   // hue 0..360, saturation and value 0..1
        const char* what = "";
    } pick;
    SDL_Texture* svTex = nullptr;    // the square for pick.h (made again when the hue moves)
    SDL_Texture* hueTex = nullptr;   // the hue bar, made once
    float svTexHue = -1;
    SDL_Texture* hueTexture();       // made on first use (the picker's bar)
    SDL_Texture* wheelTex = nullptr;
    SDL_Texture* wheelTexture();     // a colour wheel with soft edges: the picker's swatch in Settings
    void openColourPicker(int* field, const char* what, const Rgb& start);
    void drawColourSheet();
    void setPick(float h, float s, float v);
    void closeColourPicker(bool keep);   // Use, or Cancel: the field as it was
    void drawSmartSheet();
    /** The tracks of a "smart:…" spec, worked out again when the library or the statistics change. */
    const std::vector<int>& smartTracks(const std::string& spec) const;
    const char* smartIcon(const std::string& spec) const;
    /** A sidebar playlist's icon: the one chosen (plicons.inc), else its kind's (folder, the smart recipe's, a list); −1 Liked. */
    const char* playlistIcon(int ref) const;
    const char* playlistKind(int ref) const;   // "Automix playlist", "Smart playlist"…: the page's and the menu's kicker
    static bool isMoodPlaylist(const FolderPlaylist& p) { return p.path.rfind("smart:mood:", 0) == 0; }   // (a smart recipe of its own kind)
    void openPlaylistMenu(int ref);   // + on the Playlists page: Move, Icon, Rename
    void openPlaylistIcons(int ref);  // the playlist page's icon button: the icon grid at once
    void renamePlaylist(int ref);
    void renameJoins(const std::string& from, const std::string& to);
    std::string smartSummary(const std::string& spec) const;
    struct SmartCache { unsigned statsGen = ~0u, indexGen = ~0u, moodGen = ~0u; int64_t day = -1; std::vector<int> tracks; };
    // Mood playlists (mood.hpp): the library's moods, built from the analysis when it changes (moodTick); their progress
    mood::Index moodIdx_;
    // … sorted on a worker (cores::Files: 2.5 ms on the PC for 3,074 tracks, several times that on the console: never in a
    // frame), the features copied to it first; taken in when it is done (moodTick), joined at shutdown
    std::thread moodThread_;
    std::atomic<bool> moodReady_{ false };
    mood::Index moodNext_;
    std::vector<mixan::Mood> moodIn_;
    unsigned moodGen_ = 0, moodLevelsGen_ = ~0u, moodLibEpoch_ = ~0u;
    double moodBuiltAt_ = -100;
    int moodDone_ = 0, moodTotal_ = 0;
    int moodMeasured_ = 0;   // songs analysed for mood, whether a Mood playlist exists or not (moodDone_ counts for the job card)
    // your mood choices (mood::Label), by path, in mood-labels.tsv: they go to the sort with the features (moodTick); a change sorts
    // again (moodInputsGen_). (Last.fm's tags as moods taken out, 2026-11-08: 249 of 2,901 songs had tags, mostly genres; the user:
    // "it makes things worse and complicated")
    std::unordered_map<std::string, mood::Label> moodLabels_;
    unsigned moodInputsGen_ = 0, moodInputsBuilt_ = ~0u;
    std::vector<mood::Label> moodLabelsIn_;   // (the worker's copy, by track)
    void moodLabelsLoad();
    void moodLabelsSave();
    /** Your choice for a track: its mood (-1 none) and the moods it is not; nothing: cleared. Saved, synced, sorted again. */
    void setMoodLabel(int track, mood::Label l);
    static std::string moodLabelText(const mood::Label& l);
    static bool moodLabelParse(const std::string& s, mood::Label& out);
    void moodTick();
    void moodProgress(int& done, int& total) const { done = moodDone_; total = moodTotal_; }   // (total 0: no Mood playlist)
    mutable std::unordered_map<std::string, SmartCache> smartCache_;
    void drawSheet();
    void applyEdits(Track& t);
    void editTag(const char* field, const char* label);
    void saveTagEdits();
    void undoTagEdits(int track);
    // Tag and cover changes into the files too (the user, 2026-11-07: asked each time, Settings › Library decides otherwise; "there's
    // always an option to go back"): a queue handed to the worker a song at a time, never the one playing; what each file held for
    // what changed kept (tag-undo.tsv, its covers in tag-undo/), the first write's, until "Restore the file's original tags"
    TagJobs tagJobs;
    std::deque<TagJobs::Job> tagPending_;
    int tagDone_ = 0, tagTotal_ = 0;          // this run of writes (the job card)
    struct TagUndo { std::map<std::string, std::string> text; bool cover = false; std::string coverFile; };   // coverFile "" none before
    std::unordered_map<std::string, TagUndo> tagUndo_;
    void loadTagUndo();
    void saveTagUndo();
    long long tagUndoBytes() const;
    void tagWriteAsk(std::vector<TagJobs::Job> jobs, const std::string& what);   // asked, or as Settings says
    void tagWriteQueue(std::vector<TagJobs::Job> jobs);
    void tagWritesTick();
    void restoreFileTags(int track);
    // The pocket lock (the Switch; a Reddit user, 2026-11-07: "the switch in his pocket ... skips songs by itself"): the screen off, the
    // console on and playing, every button and touch ignored but the unlock on the volume buttons (no Joy-Con needed); HOME, or Songate
    // leaving the front, brings the screen back
    bool pocketLocked_ = false, pocketLightOff_ = false, pocketKeyWas_ = false;
    double pocketHintUntil_ = 0, pocketKeyAt_ = -1, pocketFirstAt_ = -10, pocketLitAt_ = 0;
    double pocketPressAt_ = -1;   // the unlock key's press under way: since when, and the volume when it began (kept for the unlock)
    int pocketVol0_ = -1;
    int pocketVolIdle_ = -1;   // the volume read while the unlock key is up (the system steps it the moment the key goes down)
    int pocketKeyNow_ = 0;   // the unlock key this lock: store.pocketKey, or Either when Capture could not be taken
    bool pocketGuard_ = false;   // unlocked by the volume key still held: its volume kept until it is let go (pocketGuardTick)
    void pocketGuardTick();
    bool pocketAvailable();
    void pocketLock(bool on);
    void pocketTick(bool pressed);
    void drawPocket();
    std::string pocketHowWords() const;
    void pocketPill(float cx, float cy);
    void setTrackCover(const std::string& image, const std::vector<int>* forTracks = nullptr);   // forTracks: these (Files), else the sheet's
    void deleteTrack(int track);
    bool deleteSong(int track);          // the file and what is kept about it, without a word or a rescan
    void syncNoteDeleted(const std::string& path);   // for other devices that follow deletions
    void allowDeletes();                 // the switch: says what it means first
    void listCoverDir(const std::string& dir);
    bool keyboard(const char* header, const std::string& initial, std::string& out);
    void loadTrackCoverAgain();
    void openFindSetting();
    /** maxW: narrower than its natural width, the room round each choice shrinks to fit (down to 12; a phone upright) */
    bool segGrid(float words, size_t n, float maxW) const;
    float segmented(Id id, float rx, float cy, const std::vector<std::string>& opts, int cur, int* picked, unsigned disabled = 0, float maxW = 0,
                    int fill = 0, float hgt = 36);   // fill: exactly maxW wide (a choice bar on a line of its own), 1 on two rows when cramped, 2 never (tabs); hgt: the title bar's is 32, as search
    int settingsCat = 0;
    bool settingsFocusFirst = false;   // a category was chosen: its first setting takes the focus next frame
    int settingsCatSeen_ = -1;   // the category last drawn: opening one may read the card once (drawSettings)
    /** Settings' categories that other pages open by number (the progress cards, the equalizer button). */
    enum SettingsCat { SetAppearance = 0, SetPlayback = 1, SetEqualizer = 2, SetLibrary = 3, SetStorage = 4, SetDevices = 9, SetLastfm = 10, SetReset = 11, SetUpdates = 12, SetSupport = 13 };   // (Reset above Updates: the user, 2026-10-30)
    std::string settingsQuery;
};
