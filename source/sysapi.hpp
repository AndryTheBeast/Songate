/*
 * Songate · sysapi.hpp
 * SongateOS's system settings (session 21, phase 1; the SongateOS session's SONGATEOS_SYSTEM_API.md): the HiBy R4 service does the
 * work, Songate draws it. Wi-Fi and Bluetooth (Settings › Connectivity), the audio output (the volume pop-up's line and its list,
 * Sound & DAC's "Plays on"), power and updates (Settings › System). The R4 only; every other device, the Switch and the PC: nothing.
 * Every call is IPC, made on a worker of its own; the screen reads what was last read and never waits. While a page (or the
 * pop-up) shows an area, it is watched: the service's change notifications read it again, with no polling.
 */
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace sysapi {

enum class Avail { Unknown, No, Yes };   // Yes: the service answered sys_info (its caps say what it offers)

/** Asks the service what it offers (once, on the worker). The R4 only (App::init). */
void start();
Avail avail();
/** The service offers this (caps) and has not refused Songate it (no_permission): Songate's page; otherwise Android's. */
bool has(const char* cap);
/** SongateOS's version as the service says it ("0.0.0" on the GSI); "" unknown */
std::string version();

struct Net { std::string key, ssid, security, ip; int level = -1; bool saved = false, connected = false; };
struct Wifi {
    bool known = false, enabled = false, scanning = false, hasCur = false;
    std::string state, link, fail;   // state off/turning_on/on/turning_off; link none/connecting/…/connected/failed; fail after one
    Net cur;
    std::vector<Net> nets;           // every network but the one in use, strongest first
};
struct BtDev { std::string address, name, kind; bool bonded = false, connected = false; int battery = -1; };
struct Bt { bool known = false, enabled = false, discovering = false; std::string name; std::vector<BtDev> devs; };
struct Out { std::string id, kind, name, codec; bool active = false, bitperfect = false, dacVolume = false; };
// connected: something plugged in or connected (service v99's "connected"; an older service: true). The 4.4 mm is "wired44" from v99,
// listed only while plugged in; "wired" the 3.5 mm
struct Output { bool known = false, connected = true; std::vector<Out> outs; std::string active, choice; };

/** SongateOS's own updates (ota_status: the service's updater; its flags are real Booleans) */
struct Ota {
    bool known = false, prerelease = false, autoCheck = false, beta = false;
    bool card = true;   // a microSD card in (service v97): updates and Google apps are kept on it, none without one (an older service: true)
    std::string state, error, current, latest, notes, recovery;   // state idle/checking/up_to_date/available/downloading/verifying/ready/staging/error
    long long size = 0, progress = 0, lastCheck = 0;               // bytes, bytes, ms since 1970 (0: never)
};

/** Android's own colours (theme_get): its accent "#rrggbb" (def: SongateOS's default), dark */
struct Theme { bool known = false, dark = false; std::string colour, def; };

// ---- phase 2 (session 21; the user's picks, 2026-11-16)
/** The connected headphones' codec (bt_codec_get): Android's own choice for that device; rate and depth Android's */
struct Codec { bool known = false; std::string address, codec, ldac; int rate = 0, bits = 0; std::vector<std::string> available; };
/** Earbuds the service controls (earbuds_get; Soundcore today): each call ~1 s, read when shown, never polled.
 *  supported false: the connected headphones are not ones it controls (unsupported) */
struct Earbuds {
    bool known = false, supported = false, wind = false, chargingL = false, chargingR = false, chargingCase = false;
    std::string kind, name, address, mode, anc, transparency, transport;   // mode noise_cancelling/transparency/normal
    // AirPods (kind "airpods", service v66): mode off/anc/transparency/adaptive; conversation "1"/"0"/"" (not known); the case's
    // battery only while they are in it (-1 otherwise)
    std::string conversation;
    int adaptiveLevel = -1;   // AirPods' Adaptive strength 0..100 (service v70; -1 not known)
    int ancLevel = 0, batteryL = -1, batteryR = -1, batteryCase = -1;
    // the case's last level while it is not reporting (service v67): -1 never seen (its age is not shown: the user, 2026-11-19)
    int batteryCaseLast = -1;
    // Galaxy Buds (kind "galaxybuds", service v78): modes off/anc/ambient; ancLevel 0..ancLevelMax (2, the strongest, their own
    // default); ambientLevel 0..ambientLevelMax (4, the loudest), -1 not known; conversation (detection) "1"/"0"/"" and its
    // timeout 5/10/15 s (-1 not known); oneBud "1"/"0"/"": noise control with one bud in
    int ancLevelMax = -1, ambientLevel = -1, ambientLevelMax = -1, conversationTimeout = -1;
    std::string oneBud;
    std::vector<std::string> modes;
};
/** The screen (display_get): brightness 0..100 on Android's own slider, timeout in seconds (0: never), the ones offered */
struct Display { bool known = false; int brightness = 0, timeout = -1; std::vector<int> timeouts; };
struct Volume { std::string id, label, kind, state, fs; long long total = 0, free = 0; };   // kind internal/sd/usb
struct Storage { bool known = false; std::vector<Volume> vols; };
/** USB (usb_get): a computer connected, what the cable is for (charge/mtp/ptp/midi) */
struct Usb { bool known = false, connected = false; std::string mode; std::vector<std::string> modes; };
/** The apps running now (apps_running, service v92): each with a launcher entry, its most important process's state
 *  (foreground/visible/service/background); Songate, the service and Android left out */
struct RunningApp { std::string package, state; };
struct Running { bool known = false; std::vector<RunningApp> apps; };
/** The player (about): no serial or anything personal */
struct About {
    bool known = false;
    std::string device, songateos, android, patch, vendorPatch, build, kernel, recovery, service, batteryStatus;
    long long ram = 0;
    int battery = -1, batteryTemp = 0;   // battery 0..100; its temperature in tenths of a °C
};

Wifi wifi();
Bt bt();
Output output();
Ota ota();
Theme theme();
/** The hold switch on top of the player (hold_get, service v45): locked now, what it locks (media/volume/all), the screen with it */
struct Hold { bool known = false, locked = false, sleep = true; std::string level; std::vector<std::string> levels; };
Hold hold();
/** The recovery (recovery_get, service v46): what is installed (twrp/orangefox/other/none), ready for SongateOS updates, its version
 *  (when put by a switch); the packages found (version, size, by kind); what the switch is doing (state, error, bytes so far) */
struct Recovery {
    bool known = false, ready = false;
    std::string installed, version, state, error, twrpVersion, foxVersion;
    long long twrpSize = 0, foxSize = 0, progress = 0;
};
/** Root (root_get, service v47): Magisk installed and its version, the newest known, keep after updates, what it is doing */
struct Root { bool known = false, installed = false, keep = true; std::string version, latest, state, error; };
Recovery recovery();
Root root();
/** Google apps (gapps_get, service v53): which bundle is in ("" none, core, basic) and its version, keep after updates, what it is
 *  doing (idle/checking/downloading/ready/installing/error, progress 0-100), and the bundles offered */
struct GappsVariant { std::string id, name, summary, version; long long size = 0; };
struct Gapps {
    bool known = false, keep = true;
    std::string installed, installedVersion, state, error;
    int progress = 0;
    std::vector<GappsVariant> variants;
};
Gapps gapps();
/** The first-boot setup (setup_get, service v52): Songate's setup finished (done), Android's setup flags set (android) */
struct Setup { bool known = false, done = false, android = false; };
Setup setup();
/** setup's state forgotten until it is read again (after setup_done: its own answer, not the one before) */
void setupForget();
/** An area read again now (one whose first read found nothing to take: the setup's, at a first boot) */
void readAgain(const char* area);
/** Date and time (time_get, service v37): automatic time and zone, the zone (its id, city and long name), minutes from UTC */
struct Time { bool known = false, autoTime = false, autoZone = false; std::string zone, zoneName, zoneLong; int offset = 0; };
Time time();
/** One time zone of zones_list: its id, city, long name, minutes from UTC */
struct Zone { std::string id, name, longName; int offset = 0; };
/** Every zone (about 400, sorted by offset then name), read once when asked for (zonesWanted) */
std::vector<Zone> zones();
void zonesWanted();
Codec codec();
Earbuds earbuds();
Display display();
Storage storage();
Usb usb();
Running running();
About about();
/** An earbuds change shown at once (the call takes a second; its answer, read back from the earbuds, replaces it) */
void earbudsSet(const std::vector<std::pair<std::string, std::string>>& extras);
/** Install asked: the service's own checks first (ota_preflight). takePreflight gives its answer once: ok, or its reason shown */
void preflight();
bool takePreflight(bool& ok);
/** Changes each time something new was read */
unsigned gen();

/** An area ("wifi", "bt", "output") shown: watched (the service's notifications) and read now; hidden: let go */
void watch(const char* area, bool on);
/** Wi-Fi's page open: a scan when it opens, then at most one every 10 s (the API's rule) */
void scanIfDue();
/** A change asked for (wifi_connect, bt_pair, output_set, power…): sent on the worker, the area read again after it */
void act(const std::string& method, const std::string& arg, const std::vector<std::pair<std::string, std::string>>& extras = {},
         const char* area = nullptr);
/** The service's last refusal, its text for the user; taken once */
bool takeError(std::string& out);

}  // namespace sysapi
