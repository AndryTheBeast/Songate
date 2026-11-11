/*
 * Songate · platform.hpp
 * Everything that differs from one platform to another, and nothing else. source/ is the same code on every
 * platform: it never names a system library, it asks here. Each platform has one implementation of this file
 * (platform/switch/, platform/windows/, platform/android/); tools/preview/ has its own, for the PC test harness.
 */
#pragma once
#include <cstddef>
#ifdef GF_WINDOWED
struct SDL_Window;
#endif
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace plat {

// ---- the app's life
void init();   // before anything else
void quit();   // after SDL_Quit
/** False when the system wants the app to end. */
bool mainLoop();
/** Closing from outside waits (holdExit) until the settings and the session are written (releaseExit). */
void holdExit();
void releaseExit();
/** fn runs whenever the app stops being in front; exiting: it is about to end. */
using LeaveFn = void (*)(bool exiting, void* arg);
void watchLeave(LeaveFn fn, void* arg);
void unwatchLeave();
bool inFocus();
/** The running program, where an update's new one is swapped in. */
std::string selfPath(int argc, char** argv);
/** An update replaces the running program as the app closes (and before the next launch reads it). */
bool swapsSelf();
/** The next launch runs this program. False when the system cannot do that. */
bool relaunch(const std::string& program);
/** Folders and files left by older versions on this platform may exist, to tidy away. */
bool legacyData();

// ---- input: the Joy-Con's buttons are the app's own vocabulary
enum Button : uint64_t {
    BtnA = 1u << 0, BtnB = 1u << 1, BtnX = 1u << 2, BtnY = 1u << 3,
    BtnL = 1u << 6, BtnR = 1u << 7, BtnZL = 1u << 8, BtnZR = 1u << 9,
    BtnPlus = 1u << 10, BtnMinus = 1u << 11,
    BtnLeft = 1u << 12, BtnUp = 1u << 13, BtnRight = 1u << 14, BtnDown = 1u << 15,
};
struct Stick { int x, y; };   // -32767..32767, y up
void padInit();
void padRead();   // once a frame
uint64_t padDown();   // pressed this frame
uint64_t padHeld();
Stick stick(int which);   // 0 left, 1 right

// ---- typing
enum class TextResult { Done, Cancelled, Unavailable };
/** Asks for a line of text and waits for the answer. initial may be null. out holds UTF-8, at most size bytes with its end. */
TextResult askText(const char* header, const char* guide, const char* initial, char* out, size_t size);
/** Set around an askText for a password (Subsonic's): the system's password keyboard, or the app's card shows dots. */
inline std::atomic<bool>& textSecret() { static std::atomic<bool> on{ false }; return on; }
#ifdef GF_WINDOWED
/** No system keyboard to hand over to: askText goes to the app's own card (view_textcard.cpp). */
using TextFn = TextResult (*)(const char* header, const char* guide, const char* initial, char* out, size_t size);
void setTextCard(TextFn fn);
/** A tablet with no keyboard attached: the system's on-screen one, while the card is open. */
void showTouchKeyboard(bool on);
/** A button's name as drawn on a key cap, for what was used last: the keyboard's key, or a pad's label. */
std::string capName(const std::string& button);
/** The last thing pressed was on a pad. */
bool padWasLast();
/** The window as the system sees it: the tray's icon, the taskbar's buttons, the media overlay. */
void windowReady(SDL_Window* window);
enum class Cmd { None, PlayPause, Play, Pause, Next, Previous, Show, Quit, Like, CycleOrder, Seek };
/** What the system's controls asked for, oldest first; once a frame until None. */
Cmd nextCommand();
void trayIcon(bool on);
/** The window's title bar in the app's colours (0xRRGGBB). */
void titleColours(uint32_t background, uint32_t text);
/** What is playing, for the system's media controls and the taskbar. cover: RGBA, may be null; jpeg: the same as a file. */
void mediaInfo(const std::string& title, const std::string& artist, const std::string& album, const uint8_t* rgba, int w, int h,
               const std::vector<uint8_t>& jpeg);
void mediaState(bool hasTrack, bool playing);
/** Settings › Playback: swiped away while music plays, it keeps playing (on) or ends with it (off). */
void keepOnSwipe(bool on);
#ifdef GF_ANDROID
/** Where the song is and what the controls' two buttons of Songate's own show (Like, the order: Order's 0..3), for Android's music
 *  controls; sent when something changed or the position jumped. */
void mediaProgress(double position, double duration, bool liked, int order);
/** Cmd::Seek's place, in seconds. */
double commandSeek();
/** The Canvas opened from a phone held upright: the screen is turned sideways and kept there (on), then given back to the phone's own
 *  turning (off), which puts it upright again when that is how it is held. */
void holdLandscape(bool on);
/** Android's density: real pixels to a dp (a 160th of an inch as the device's maker and the user's Display size have it; 2.625 on
 *  a 1080-wide phone of 411 dp). The window's size in dp is what the layout's scale comes from (App::fitWindow). */
float screenDensity();
#endif
/** A file name, or a path ending in one, is a release's installer. */
bool isInstaller(const std::string& name);
/** Opens a web address in the default browser. */
void openUrl(const std::string& url);
/** The usual place for music, offered at the first launch. */
std::string musicFolder();
#endif

// ---- the machine
/** Music playing, or something else that keeps the screen from dimming and the machine from sleeping. */
void setMediaPlaying(bool on);
/** No dimming or sleeping by itself. */
void setKeepAwake(bool on);
void requestSleep();
/** The screen's own light down; saved keeps the level. False: not possible here, a veil is drawn instead. */
bool dimBacklight(float& saved);
void restoreBacklight(float saved);
/** The console's volume buttons, as they are now (the pocket lock's unlock: no Joy-Con needed). False where they cannot be read;
 *  err says why the first time. Read only: the system still changes the volume with them. */
bool volumeKeys(bool& up, bool& down, std::string* err = nullptr);
/** The screen's light off (the console stays on) or back on. False where Songate cannot. */
bool backlightOff(bool off);
/** HOME's presses (short and long) held back from the system; false where it cannot be (the Switch's applet mode, other devices). */
bool blockHome(bool on);
/** Capture's short presses taken by Songate (no screenshot) while on; false where it cannot be. capturePresses: how many since asked. */
bool takeCapture(bool on);
int capturePresses();
/** The controllers off the console (wireless Joy-Con, Pro Controller…) disconnected, so they sleep; a press wakes them. How many. */
int disconnectControllers();
/** The volume of the output playing now (speaker, headphones, Bluetooth…), in the system's steps; and set back. False where not. */
bool outputVolume(int& level);
bool setOutputVolume(int level);
/** False when there is no battery. */
bool batteryLevel(unsigned& percent);
/** percent with its decimals; charging: a charger is connected. */
bool batteryRaw(double& percent, bool& charging);
/** The battery and its charger, beyond the charge (BETA 3: the Switch's battery card). 0 where not known. */
struct BatteryInfo {
    int charger = 0;        // 0 none, 1 full power (USB-C PD), 2 low power, 3 not supported
    bool fast = false;      // fast charging on
    double volts = 0, tempC = 0, healthPct = 0;   // the battery's voltage, temperature, health (100: as new)
    double chargerVolts = 0, chargerAmps = 0;     // what the charger agreed to give (watts: the two multiplied)
    std::string raw;        // the power service's words as read (the Switch: for the log, once)
    // The battery's fuel gauge (the Switch's MAX17050, over I2C; the user, 2026-11-05: "make the battery time appear sooner" and "how
    // many watts the battery is drawing"): what it measures now. gauge false: none read
    bool gauge = false;
    double gaugeVolts = 0, gaugeAmps = 0;   // the cell's average voltage and current (+ into the battery, − out of it)
    double capMah = 0, fullMah = 0;         // the charge in it now and when full (0: not known)
    int tteMin = -1, ttfMin = -1;           // its own time to empty and to full, in minutes (−1: not known)
    // What a platform cannot read is left out, never shown as zero (the PC, 2026-11-11). missing: a bit for each of
    // Settings' battery items it has no reading for (2 charger watts, 3 temperature, 4 voltage, 5 health, 6 power)
    unsigned missing = 0;
    bool chargerKnown = true;   // false: only whether one is connected (charger: 1 or 0), not its kind nor what it gives
    bool wattHours = false;     // capMah and fullMah are in mWh (a PC's battery reports energy, not charge)
    int cycles = 0;             // charge cycles so far (0: not known)
};
/** False where there is none to read. */
bool batteryInfo(BatteryInfo& out);
void sysInfo(std::string& firmware, std::string& atmosphere, bool& docked);
/** When the machine last started, in time() seconds: a different one means it restarted. */
long long bootStamp();
/** Moves the calling thread to that core and priority, as cores.hpp lists them. */
void placeThread(int core, int priority);
/** The network library's start and end. */
bool netStart();
/** A network this device is on: its address and that network's broadcast address, each as sockets want them. */
struct Net { uint32_t address, broadcast; };
std::vector<Net> networks();
/** What this device is called, for other devices to show. */
std::string deviceName();
/** Free bytes where this path is, -1 if unknown. */
int64_t freeBytes(const std::string& path);
/** Started from an applet (the album): little memory, so nothing long-running. */
bool appletMode();
/** Random bytes fit for keys; false if the system gave none. */
bool randomBytes(uint8_t* out, size_t n);
void netStop();

// ---- the background player
int bgProtocol();
/** Ends it and starts it again. False, with why in err, when that failed. */
bool bgRestart(std::string& err);
/** Ends it for now: true means bgStart(true) is needed after. */
bool bgEnd();
void bgStart(bool ended);

// ---- files
/** The app's own read-only files (fonts). */
std::string assetPath(const char* relative);
struct FontData { void* data; size_t size; };
/** The system's fonts, as fallbacks for what Inter does not have. */
std::vector<FontData> systemFonts();
const char* configParent();   // the folder holding configDir
const char* configDir();
const char* backupDir();      // where backups go unless the user chose
/** The Switch's folder as cfgmove::atLaunch settled it (songate-nx, or gatefold-nx not moved yet), before anything reads it:
 *  configDir() and backupDir() hand out the same buffers, so Store::Dir follows. The PC's never changes. */
void useConfigDir(const char* dir, const char* backups);
const char* root();           // what browsers start from: the top of everything the app can see
const char* rootName();       // the same in words
bool isRoot(const std::string& path);
std::string parentDir(const std::string& path);
/** A name inside a folder. */
std::string join(const std::string& dir, const std::string& name);
/** The length of an absolute path's root (0 when it has none). */
size_t rootLen(const std::string& path);
/** "/a/b" from a playlist written elsewhere → the same place here. */
std::string fromRoot(const std::string& path);
/** Folders the system needs (Files asks more firmly before touching them). */
const std::vector<const char*>& systemFolders();
#ifdef GF_ANDROID
/** What is at root(): Android does not let an app list /storage, so its volumes (the phone's own, each card), as join() takes them. */
std::vector<std::string> rootFolders();
/** A volume's name for people ("SD card"), when path is one of the volumes; empty for any other folder. */
std::string volumeLabel(const std::string& path);
/** Songate may read the shared volumes: All files access (Android 11 and later), the storage permission (Android 10). */
bool storageAllowed();
/** Android's own page (or dialog) for it; the answer is seen with storageAllowed when Songate is back in front. */
void askStorage();
/** The screen's edges in window pixels: each camera cutout as a box (a hole in a corner or the middle, a notch, two of them) and
 *  the rounded corners' radius. The number changes whenever Android says they did (a turn, a fold). */
struct EdgeBox { float x, y, w, h; };
unsigned screenEdges(std::vector<EdgeBox>& boxes, float& cornerRadius, int& width, int& height);   // width, height: the window's they are for
#endif
/** Files and bytes below a folder, the system's own fast way. False: use stat(). */
bool measureFiles(const std::string& path, int& files, uint64_t& bytes, int cap);
/** … the same for Storage, only names keep accepts at its top. */
bool measureStorage(const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&));

}  // namespace plat
