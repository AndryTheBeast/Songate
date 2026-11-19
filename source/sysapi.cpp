/*
 * Songate · sysapi.cpp
 * SongateOS's system settings (sysapi.hpp): one worker makes every call (SysApi.java through platform_android.cpp), reads the
 * areas being shown again when the service says they changed, and keeps what it read for the screen. Dev builds on the emulator
 * (<config>/mock-r4.txt, the simulated R4) answer from a simulated service, so the pages and the tests run there too.
 */
#include "sysapi.hpp"

#ifdef GF_ANDROID
#endif

#ifdef GF_ANDROID
#include <cstring>
#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "json.hpp"
#include "platform.hpp"

namespace sysapi {
namespace {

using Extras = std::vector<std::pair<std::string, std::string>>;
struct Job { std::string method, arg; Extras extras; std::string area; };

std::mutex mx;
std::condition_variable cv;
bool started = false;
std::atomic<int> availV{ 0 };
std::set<std::string> caps, refused;
std::string versionV;
Wifi wifiV;
Bt btV;
Output outV;
Ota otaV;
Theme themeV;
Hold holdV;
Setup setupV;
Time timeV;
std::vector<Zone> zonesV;
bool zonesAsked = false;
Recovery recV;
Root rootV;
Gapps gappsV;
Codec codecV;
Earbuds budsV;
Display dispV;
Storage stoV;
Usb usbV;
Running runV;
About aboutV;
int preflightV = -1;   // −1 none waiting; 0 refused; 1 ok (taken once)
std::atomic<unsigned> genV{ 0 };
std::deque<Job> jobs;
std::set<std::string> watched, wanted;   // shown now; to read again
std::map<std::string, unsigned> seen;    // each area's change count as last read
std::string err;
bool errNew = false;
Uint32 lastScan = 0;

void wipe(std::string& s) {   // (a password's copy: overwritten, not only freed)
    volatile char* w = s.data();
    for (size_t i = 0; i < s.size(); i++) w[i] = 0;
    s.clear();
}

bool flag(const json::Json& j, const char* k) {
    const json::Json& v = j[k];
    return v.kind == json::Json::Bool ? v.b : v.kind == json::Json::Str ? v.str == "1" || v.str == "true" : v.kind == json::Json::Num && v.num != 0;
}
int num(const json::Json& j, const char* k, int fallback) {
    const json::Json& v = j[k];
    return v.kind == json::Json::Num ? int(v.num) : v.kind == json::Json::Str && !v.str.empty() ? atoi(v.str.c_str()) : fallback;
}

// ---- the calls: the service's, or (dev builds, the emulator) the simulated one
#ifdef GF_DEV
bool sim = false;
struct SimState {
    bool wifiOn = true, btOn = true, discovering = false;
    std::string cur = "net1", link = "connected", fail, outChoice = "auto", outActive = "bt:00:11:22:33:44:55";
    Uint32 linkAt = 0;
    std::string linkTo;
    std::set<std::string> saved{ "net1", "net2" };
    std::set<std::string> bonded{ "00:11:22:33:44:55", "66:77:88:99:AA:BB" }, conn{ "00:11:22:33:44:55" };
    std::string ota = "up_to_date";
    int64_t otaProgress = 0, otaChecked = 0;
    Uint32 otaAt = 0;
    bool otaAuto = true, otaBeta = false;
    std::string themeColour = "#fb695a";
    bool themeDark = true;
    // phase 2: the earbuds (the user's Liberty 4 NC), the codec, the screen, the card, USB
    std::string codec = "aac", mode = "noise_cancelling", anc = "adaptive", transparency = "full", transport = "plane";
    int ancLevel = 3, brightness = 62, timeout = 120;
    bool wind = false, usbIn = true, cardIn = true;
    std::string usbMode = "charge", cardState = "mounted", holdLevel = "volume";
    std::set<std::string> killed;   // (apps_kill: off apps_running until Songate starts again)
    bool holdSleep = true;
    std::string recKind = "twrp", recState = "idle", rootState = "idle";
    bool rootOn = false, rootKeep = true;
    std::string gappsOn;
    bool gappsKeep = true;
    Uint32 cardAt = 0;
    // the first-boot setup (service v52): not done while <config>/mock-setup.txt is there (the test makes it); the time zone
    bool setupDone = false;
    std::string zone = "Europe/London";
} S;
struct SimZone { const char* id; const char* name; const char* lng; int offset; };
const SimZone kZones[] = { { "Pacific/Honolulu", "Honolulu", "Hawaii-Aleutian Standard Time", -600 },
                           { "America/Los_Angeles", "Los Angeles", "Pacific Standard Time", -480 },
                           { "America/Denver", "Denver", "Mountain Standard Time", -420 },
                           { "America/Chicago", "Chicago", "Central Standard Time", -360 },
                           { "America/New_York", "New York", "Eastern Standard Time", -300 },
                           { "America/Sao_Paulo", "São Paulo", "Brasilia Standard Time", -180 },
                           { "Atlantic/Azores", "Azores", "Azores Standard Time", -60 },
                           { "Europe/Lisbon", "Lisbon", "Western European Standard Time", 0 },
                           { "Europe/London", "London", "Greenwich Mean Time", 0 },
                           { "Africa/Lagos", "Lagos", "West Africa Standard Time", 60 },
                           { "Europe/Berlin", "Berlin", "Central European Standard Time", 60 },
                           { "Europe/Madrid", "Madrid", "Central European Standard Time", 60 },
                           { "Europe/Paris", "Paris", "Central European Standard Time", 60 },
                           { "Europe/Athens", "Athens", "Eastern European Standard Time", 120 },
                           { "Europe/Istanbul", "Istanbul", "Turkey Time", 180 },
                           { "Asia/Dubai", "Dubai", "Gulf Standard Time", 240 },
                           { "Asia/Kolkata", "Kolkata", "India Standard Time", 330 },
                           { "Asia/Shanghai", "Shanghai", "China Standard Time", 480 },
                           { "Asia/Tokyo", "Tokyo", "Japan Standard Time", 540 },
                           { "Australia/Sydney", "Sydney", "Australian Eastern Daylight Time", 660 },
                           { "Pacific/Auckland", "Auckland", "New Zealand Daylight Time", 780 } };
struct SimNet { const char* key; const char* ssid; const char* sec; int level; };
const SimNet kNets[] = { { "net1", "TheBeast-5G", "sae", 4 }, { "net2", "TheBeast", "psk", 3 }, { "net3", "Vodafone-B2C1", "psk", 2 },
                         { "net4", "Café Central", "open", 2 }, { "net5", "MEO-WiFi", "psk", 1 }, { "net6", "eduroam", "eap", 1 } };
struct SimDev { const char* addr; const char* name; const char* kind; int battery; };
const SimDev kDevs[] = { { "00:11:22:33:44:55", "Soundcore Liberty 4 NC", "headphones", 80 }, { "66:77:88:99:AA:BB", "Moondrop Space Travel", "headphones", -1 },
                         { "CC:DD:EE:FF:00:11", "JBL Flip 6", "speaker", -1 }, { "22:33:44:55:66:77", "Galaxy Buds2", "headphones", -1 } };

std::string q(const std::string& s) { return "\"" + s + "\""; }
std::string simCall(const std::string& m, const std::string& arg, const Extras& ex) {
    auto x = [&](const char* k) { for (const auto& [a, b] : ex) if (a == k) return b; return std::string(); };
    const std::string ok = "{\"ok\":true}";
    // (the test: mock-late.txt, a service that starts late, as at the R4's first boot: no answer for its first 8 s, then setup_get
    // refused once more while it finishes starting)
    if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-late.txt").c_str(), "rb")) {
        fclose(f);
        static const Uint32 t0 = SDL_GetTicks();
        static bool setupOnce = false;
        if (SDL_GetTicks() - t0 < 8000) return "{\"ok\":false,\"error\":\"no_answer: the service did not answer\"}";
        if (m == "setup_get" && !setupOnce) { setupOnce = true; return "{\"ok\":false,\"error\":\"busy: starting\"}"; }
    }
    if (m == "sys_info")
        return "{\"ok\":true,\"api\":1,\"caps\":[\"wifi\",\"bt\",\"bt_codec\",\"earbuds\",\"display\",\"storage\",\"usb\",\"about\",\"hold\",\"recovery\",\"root\",\"output\",\"power\","
               "\"ota\",\"theme\",\"time\",\"setup\",\"gapps\",\"apps\",\"power_sleep\"],\"songateos\":\"0.3.0\",\"ready\":true}";
    // ---- phase 2 (the shapes of SONGATEOS_SYSTEM_API.md, service v41)
    const bool budsOn = S.btOn && S.conn.count("00:11:22:33:44:55") > 0;
    if (m == "bt_codec_get") {
        if (!budsOn) return "{\"ok\":false,\"error\":\"not_connected: No Bluetooth headphones connected\"}";
        return "{\"ok\":true,\"address\":\"00:11:22:33:44:55\",\"codec\":" + q(S.codec) + ",\"rate\":44100,\"bits\":16,\"available\":[\"aac\",\"sbc\"]}";
    }
    if (m == "bt_codec_set") {
        if (x("codec") != "aac" && x("codec") != "sbc") return "{\"ok\":false,\"error\":\"unsupported: These headphones don't support that codec\"}";
        S.codec = x("codec");
        plat::sysBump("bt");
        return ok;
    }
    if (m == "earbuds_get" || m == "earbuds_set") {
        if (!budsOn) return "{\"ok\":false,\"error\":\"not_connected: No Bluetooth headphones connected\"}";
        if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-galaxybuds.txt").c_str(), "rb")) {   // (the test: Galaxy Buds)
            fclose(f);
            static std::string gMode = "anc", gConv = "1", gAnc = "2", gAmb = "2", gTimeout = "10", gOne = "0";
            SDL_Delay(m == "earbuds_set" ? 500 : 300);
            if (m == "earbuds_set") {
                if (!x("mode").empty()) gMode = x("mode");
                if (!x("conversation").empty()) gConv = x("conversation");
                if (!x("anc_level").empty()) gAnc = x("anc_level");
                if (!x("ambient_level").empty()) gAmb = x("ambient_level");
                if (!x("conversation_timeout").empty()) gTimeout = x("conversation_timeout");
                if (!x("one_bud").empty()) gOne = x("one_bud");
            }
            return "{\"ok\":true,\"kind\":\"galaxybuds\",\"name\":\"Galaxy Buds4 Pro\",\"address\":\"00:11:22:33:44:77\",\"battery_left\":78,"
                   "\"battery_right\":74,\"battery_case\":-1,\"battery_case_last\":52,\"battery_case_last_age_s\":300,\"charging_case_last\":\"0\",\"charging_left\":\"0\","
                   "\"charging_right\":\"0\",\"charging_case\":\"0\",\"ear_left\":\"in\",\"ear_right\":\"in\",\"modes\":[\"off\",\"anc\",\"ambient\"],\"mode\":" + q(gMode) +
                   ",\"anc_level\":" + gAnc + ",\"anc_level_max\":2,\"ambient_level\":" + gAmb + ",\"ambient_level_max\":4,\"conversation\":" + q(gConv) +
                   ",\"conversation_timeout\":" + gTimeout + ",\"one_bud\":" + q(gOne) + "}";
        }
        if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-airpods.txt").c_str(), "rb")) {   // (the test: AirPods)
            fclose(f);
            static std::string apMode = "anc", apConv = "1", apLevel = "100";
            SDL_Delay(m == "earbuds_set" ? 900 : 300);
            if (m == "earbuds_set") {
                if (!x("mode").empty()) apMode = x("mode");
                if (!x("conversation").empty()) apConv = x("conversation");
                if (!x("adaptive_level").empty()) apLevel = x("adaptive_level");
            }
            return "{\"ok\":true,\"kind\":\"airpods\",\"name\":\"AirPods Pro\",\"address\":\"00:11:22:33:44:66\",\"battery_left\":72,"
                   "\"battery_right\":73,\"battery_case\":-1,\"battery_case_last\":64,\"battery_case_last_age_s\":600,\"charging_case_last\":\"0\",\"charging_left\":\"0\",\"charging_right\":\"0\",\"charging_case\":\"0\","
                   "\"modes\":[\"off\",\"anc\",\"transparency\",\"adaptive\"],\"mode\":" + q(apMode) + ",\"conversation\":" + q(apConv) + ",\"adaptive_level\":" + apLevel +
                   ",\"ear_primary\":\"in\",\"ear_secondary\":\"in\"}";
        }
        if (m == "earbuds_set") {
            SDL_Delay(900);   // (as the earbuds: about a second)
            if (!x("mode").empty()) S.mode = x("mode");
            if (!x("anc").empty()) S.anc = x("anc");
            if (!x("anc_level").empty()) S.ancLevel = atoi(x("anc_level").c_str());
            if (!x("transparency").empty()) S.transparency = x("transparency");
            if (!x("transport").empty()) S.transport = x("transport");
            if (!x("wind").empty()) S.wind = x("wind") == "1";
        } else SDL_Delay(900);
        return "{\"ok\":true,\"kind\":\"soundcore\",\"name\":\"Soundcore Liberty 4 NC\",\"address\":\"00:11:22:33:44:55\",\"firmware\":\"03.71\","
               "\"battery_left\":80,\"battery_right\":60,\"charging_left\":\"0\",\"charging_right\":\"0\","
               "\"modes\":[\"noise_cancelling\",\"transparency\",\"normal\"],\"mode\":" + q(S.mode) + ",\"anc\":" + q(S.anc) +
               ",\"anc_level\":" + std::to_string(S.ancLevel) + ",\"transport\":" + q(S.transport) + ",\"transparency\":" + q(S.transparency) +
               ",\"wind\":\"" + (S.wind ? "1" : "0") + "\"}";
    }
    if (m == "recovery_get")
        return "{\"ok\":true,\"installed\":" + q(S.recKind) + ",\"installed_ready\":\"1\",\"installed_version\":\"2026.11.16\",\"kinds\":[\"twrp\","
               "\"orangefox\"],\"twrp_version\":\"2026.11.20\",\"orangefox_version\":\"2026.11.16\",\"twrp_size\":36700160,\"orangefox_size\":41943040,"
               "\"state\":" + q(S.recState) + ",\"error\":\"\",\"progress\":0}";
    if (m == "recovery_check") return ok;
    if (m == "recovery_switch") { S.recKind = arg; S.recState = "done"; plat::sysBump("recovery"); return ok; }
    if (m == "root_get" || m == "root_set") {
        if (m == "root_set" && !x("keep").empty()) S.rootKeep = x("keep") == "1";
        return "{\"ok\":true,\"installed\":\"" + std::string(S.rootOn ? "1" : "0") + "\",\"version\":" + q(S.rootOn ? "31.1" : "") +
               ",\"latest\":\"31.1\",\"keep\":\"" + (S.rootKeep ? "1" : "0") + "\",\"state\":" + q(S.rootState) + ",\"error\":\"\"}";
    }
    if (m == "root_install" || m == "root_remove") { S.rootOn = m == "root_install"; S.rootState = "restart"; plat::sysBump("root"); return ok; }
    if (m == "root_check") return ok;
    if (m == "gapps_get") {
        return "{\"ok\":true,\"installed\":" + q(S.gappsOn) + ",\"installed_version\":" + q(S.gappsOn.empty() ? "" : "2026.11.10") +
               ",\"keep\":\"" + (S.gappsKeep ? "1" : "0") + "\",\"state\":\"idle\",\"progress\":0,\"error\":\"\",\"variants\":["
               "{\"id\":\"core\",\"name\":\"Core\",\"summary\":\"Play Store and Play Services\",\"size\":188743680,\"version\":\"2026.11.10\"},"
               "{\"id\":\"basic\",\"name\":\"Basic\",\"summary\":\"Core, plus Gmail, Maps and more\",\"size\":440401920,\"version\":\"2026.11.10\"}]}";
    }
    if (m == "gapps_set") { if (arg == "keep") S.gappsKeep = x("value") == "1"; return ok; }
    if (m == "gapps_install") { S.gappsOn = arg; plat::sysBump("gapps"); return ok; }
    if (m == "gapps_remove") { S.gappsOn.clear(); plat::sysBump("gapps"); return ok; }
    if (m == "gapps_check") return ok;
    if (m == "home_ready") return ok;   // (the boot animation ends: nothing to simulate)
    if (m == "setup_get" || m == "setup_done") {
        if (m == "setup_done") {
            if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-setup-fail.txt").c_str(), "rb")) {   // (the test: a refusal)
                fclose(f);
                return "{\"ok\":false,\"error\":\"busy: Try again in a moment\"}";
            }
            S.setupDone = true;
            remove((std::string(plat::configDir()) + "/mock-setup.txt").c_str());
            plat::sysBump("setup");
        }
        bool done = S.setupDone;
        if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-setup.txt").c_str(), "rb")) fclose(f);
        else done = true;
        return "{\"ok\":true,\"done\":\"" + std::string(done ? "1" : "0") + "\",\"android\":\"" + (done ? "1" : "0") + "\"}";
    }
    if (m == "time_get") {
        for (const SimZone& z : kZones)
            if (S.zone == z.id)
                return "{\"ok\":true,\"auto_time\":\"1\",\"auto_zone\":\"0\",\"zone\":" + q(z.id) + ",\"zone_name\":" + q(z.name) +
                       ",\"zone_long\":" + q(z.lng) + ",\"offset\":" + std::to_string(z.offset) + ",\"hour24\":\"1\",\"hour24_auto\":\"1\",\"now\":\"0\"}";
        return "{\"ok\":false,\"error\":\"bad_arg: no zone\"}";
    }
    if (m == "time_set") {
        if (arg == "zone") { S.zone = x("value"); plat::sysBump("time"); }
        return ok;
    }
    if (m == "zones_list") {
        std::string v;
        for (const SimZone& z : kZones) {
            if (!v.empty()) v += ",";
            v += "{\"id\":" + q(z.id) + ",\"name\":" + q(z.name) + ",\"long\":" + q(z.lng) + ",\"offset\":" + std::to_string(z.offset) + "}";
        }
        return "{\"ok\":true,\"zones\":[" + v + "]}";
    }
    if (m == "hold_get" || m == "hold_set") {   // (the switch itself: flipped from the test through mock-hold.txt)
        if (m == "hold_set") {
            if (!x("level").empty()) S.holdLevel = x("level");
            if (!x("sleep").empty()) S.holdSleep = x("sleep") == "1";
        }
        bool locked = false;
        if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-hold.txt").c_str(), "rb")) { locked = true; fclose(f); }
        return "{\"ok\":true,\"locked\":\"" + std::string(locked ? "1" : "0") + "\",\"level\":" + q(S.holdLevel) +
               ",\"levels\":[\"media\",\"volume\",\"all\"],\"sleep\":\"" + (S.holdSleep ? "1" : "0") + "\"}";
    }
    if (m == "display_get")
        return "{\"ok\":true,\"brightness\":" + std::to_string(S.brightness) + ",\"timeout\":" + std::to_string(S.timeout) +
               ",\"timeouts\":[15,30,60,120,300,600,1800,0]}";
    if (m == "display_set") {
        if (arg == "brightness") S.brightness = std::clamp(atoi(x("value").c_str()), 0, 100);
        else S.timeout = atoi(x("value").c_str());
        plat::sysBump("display");
        return ok;
    }
    if (m == "storage_get") {
        std::string v = "{\"id\":\"internal\",\"label\":\"Internal storage\",\"kind\":\"internal\",\"state\":\"mounted\",\"total\":23729694720,"
                        "\"free\":21045339750}";
        if (S.cardIn)
            v += ",{\"id\":\"public:179,65\",\"label\":\"TheBeast\",\"kind\":\"sd\",\"state\":" + q(S.cardState) + ",\"fs\":\"exfat\"" +
                 (S.cardState == "mounted" ? std::string(",\"total\":134217728000,\"free\":44667659878") : std::string()) + "}";
        return "{\"ok\":true,\"volumes\":[" + v + "]}";
    }
    if (m == "storage_eject" || m == "storage_mount") {
        if (arg == "internal") return "{\"ok\":false,\"error\":\"bad_arg: Only a microSD card or USB drive can do that\"}";
        S.cardState = m == "storage_eject" ? "ejecting" : "checking";
        S.cardAt = SDL_GetTicks();
        plat::sysBump("storage");
        return ok;
    }
    if (m == "apps_running") {
        static const char* const sim[] = { "com.android.chrome", "com.google.android.apps.maps", "com.google.android.youtube" };
        std::string out = "{\"ok\":true,\"running\":[";
        bool first = true;
        for (const char* p : sim) {
            if (S.killed.count(p)) continue;
            out += std::string(first ? "" : ",") + "{\"package\":\"" + p + "\",\"state\":\"" + (std::string(p).find("youtube") != std::string::npos ? "service" : "background") + "\"}";
            first = false;
        }
        return out + "]}";
    }
    if (m == "apps_kill") {
        if (arg == "io.github.andrythebeast.songate" || arg == "android") return "{\"ok\":false,\"error\":\"not_allowed: Songate can't stop that\"}";
        S.killed.insert(arg);
        plat::sysBump("apps");
        return ok;
    }
    if (m == "usb_get")
        return "{\"ok\":true,\"connected\":\"" + std::string(S.usbIn ? "1" : "0") + "\",\"mode\":" + q(S.usbMode) +
               ",\"modes\":[\"charge\",\"mtp\",\"ptp\",\"midi\"]}";
    if (m == "usb_set") {
        if (!S.usbIn) return "{\"ok\":false,\"error\":\"not_connected: Connect the R4 to a computer first\"}";
        S.usbMode = arg;
        plat::sysBump("usb");
        return ok;
    }
    if (m == "about")
        return "{\"ok\":true,\"device\":\"HiBy R4\",\"songateos\":\"0.3.0\",\"android\":\"14\",\"patch\":\"2026-11-01\",\"vendor_patch\":\"2021-09-05\","
               "\"build\":\"songateos_r4-user 14 AP2A.241105 release-keys\",\"kernel\":\"4.19.191-songateos\",\"recovery\":\"ok:TWRP\","
               "\"service\":\"41\",\"ram\":4294967296,\"battery\":43,\"battery_status\":\"charging\",\"battery_temp\":315}";
    if (m == "wifi_get") {
        std::string nets;
        for (const SimNet& n : kNets) {
            if (!S.wifiOn) break;
            if (!nets.empty()) nets += ",";
            nets += "{\"key\":" + q(n.key) + ",\"ssid\":" + q(n.ssid) + ",\"level\":" + std::to_string(n.level) + ",\"security\":" + q(n.sec) +
                    ",\"saved\":\"" + (S.saved.count(n.key) ? "1" : "0") + "\",\"connected\":\"" + (S.cur == n.key && S.link == "connected" ? "1" : "0") + "\"}";
        }
        std::string cur = "null";
        for (const SimNet& n : kNets)
            if (S.wifiOn && S.cur == n.key)
                cur = "{\"ssid\":" + q(n.ssid) + ",\"level\":" + std::to_string(n.level) + ",\"ip\":\"192.168.1.109\",\"security\":" + q(n.sec) + "}";
        return "{\"ok\":true,\"enabled\":" + std::string(S.wifiOn ? "true" : "false") + ",\"state\":" + q(S.wifiOn ? "on" : "off") +
               ",\"link\":" + q(S.wifiOn ? S.link : "none") + ",\"fail\":" + q(S.fail) + ",\"current\":" + cur + ",\"networks\":[" + nets + "],\"scanning\":false}";
    }
    if (m == "wifi_set_enabled") { S.wifiOn = x("value") == "1"; if (!S.wifiOn) S.cur.clear(); return ok; }
    if (m == "wifi_scan") return S.wifiOn ? ok : "{\"ok\":false,\"error\":\"off: Wi-Fi is off\"}";
    if (m == "wifi_connect" || m == "wifi_connect_saved") {
        std::string key = arg;
        if (m == "wifi_connect") for (const SimNet& n : kNets) if (x("ssid") == n.ssid) key = n.key;
        S.cur = key;
        S.link = "connecting";
        S.fail.clear();
        S.linkAt = SDL_GetTicks() + 1200;
        S.linkTo = m == "wifi_connect" && x("security") != "open" && x("password") == "wrongpass" ? "failed" : "connected";
        if (S.linkTo == "connected") S.saved.insert(key);
        return ok;
    }
    if (m == "wifi_forget") { S.saved.erase(arg); if (S.cur == arg) { S.cur.clear(); S.link = "none"; } return ok; }
    if (m == "bt_get") {
        std::string devs;
        for (const SimDev& d : kDevs) {
            if (!S.btOn) break;
            const bool b = S.bonded.count(d.addr) > 0;
            if (!b && !S.discovering) continue;
            if (!devs.empty()) devs += ",";
            devs += "{\"address\":" + q(d.addr) + ",\"name\":" + q(d.name) + ",\"kind\":" + q(d.kind) + ",\"bonded\":\"" + (b ? "1" : "0") +
                    "\",\"connected\":\"" + (S.conn.count(d.addr) ? "1" : "0") + "\"" +
                    (d.battery >= 0 && S.conn.count(d.addr) ? ",\"battery\":" + std::to_string(d.battery) : std::string()) + "}";
        }
        return "{\"ok\":true,\"enabled\":" + std::string(S.btOn ? "true" : "false") + ",\"state\":" + q(S.btOn ? "on" : "off") +
               ",\"name\":\"HiBy R4\",\"discovering\":" + (S.discovering ? "true" : "false") + ",\"devices\":[" + devs + "]}";
    }
    if (m == "bt_set_enabled") { S.btOn = x("value") == "1"; return ok; }
    if (m == "bt_discover") { S.discovering = arg == "start"; return ok; }
    if (m == "bt_pair") { S.bonded.insert(arg); return ok; }
    if (m == "bt_connect") { S.conn.insert(arg); return ok; }
    if (m == "bt_disconnect") { S.conn.erase(arg); return ok; }
    if (m == "bt_unpair") { S.bonded.erase(arg); S.conn.erase(arg); return ok; }
    if (m == "output_get") {
        if (FILE* up = fopen((std::string(plat::configDir()) + "/mock-unplugged.txt").c_str(), "rb")) {
            fclose(up);
            return "{\"ok\":true,\"outputs\":[],\"active\":\"\",\"choice\":" + q(S.outChoice) + ",\"connected\":\"0\"}";
        }
        const bool bt = S.btOn && S.conn.count("00:11:22:33:44:55");
        std::string active = S.outChoice != "auto" ? S.outChoice : bt ? "bt:00:11:22:33:44:55" : "wired";
        if (active.rfind("bt:", 0) == 0 && !bt) active = "wired";
        S.outActive = active;
        std::string outs = "{\"id\":\"wired\",\"kind\":\"balanced_44\",\"name\":\"Headphones 4.4 mm\",\"active\":\"" + std::string(active == "wired" ? "1" : "0") +
                           "\",\"bitperfect\":\"1\",\"dac_volume\":\"1\"}";
        if (bt) outs += ",{\"id\":\"bt:00:11:22:33:44:55\",\"kind\":\"bluetooth\",\"name\":\"Soundcore Liberty 4 NC\",\"codec\":\"aac\",\"active\":\"" +
                        std::string(active != "wired" ? "1" : "0") + "\",\"bitperfect\":\"0\",\"dac_volume\":\"0\"}";
        return "{\"ok\":true,\"outputs\":[" + outs + "],\"active\":" + q(active) + ",\"choice\":" + q(S.outChoice) + "}";
    }
    if (m == "output_set") { S.outChoice = arg; return ok; }
    // the updater: up to date until a check finds 0.3.1 (a beta); its download fills in 6 s, then it is ready
    if (m == "ota_status") {
        bool card = true;   // (mock-nocard.txt: no microSD card)
        if (FILE* nc = fopen((std::string(plat::configDir()) + "/mock-nocard.txt").c_str(), "rb")) { fclose(nc); card = false; }
        return "{\"ok\":true,\"state\":" + q(S.ota) + ",\"error\":\"\",\"current\":\"0.3.0\",\"latest\":" + q(S.ota == "up_to_date" || S.ota == "idle" ? "" : "0.3.1") +
               ",\"notes\":\"## Songate 2.1.0\\n- **Setup** for a new player, in a minute\\n- Key sounds: Soft, Tick, Pop and Clack\\n\\n## Fixes\\n"
               "- HiBy settings are saved again\\n- The status LED after a restart ([#12](https://example.invalid/12))\"" +
               ",\"size\":1288490188,\"progress\":" + std::to_string(S.otaProgress) + ",\"last_check\":" + std::to_string(S.otaChecked) +
               ",\"prerelease\":true,\"auto\":" + (S.otaAuto ? "true" : "false") + ",\"beta\":" + (S.otaBeta ? "true" : "false") +
               ",\"recovery\":\"ok:OrangeFox:hook1\",\"card\":" + (card ? "true" : "false") + "}";
    }
    if (m == "ota_check") { S.ota = "available"; S.otaChecked = int64_t(::time(nullptr)) * 1000; return ok; }
    if (m == "ota_download") { S.ota = "downloading"; S.otaAt = SDL_GetTicks(); return ok; }
    if (m == "ota_cancel") { S.ota = "available"; return ok; }
    if (m == "ota_preflight") return S.ota == "ready" ? ok : "{\"ok\":false,\"error\":\"failed: The update isn't downloaded\"}";
    if (m == "ota_install") return ok;   // (simulated: nothing restarts)
    if (m == "ota_set") { if (arg == "auto") S.otaAuto = x("value") == "1"; else S.otaBeta = x("value") == "1"; return ok; }
    // Android's colours (each change restarts screens on the R4; simulated: only the values)
    if (m == "theme_get") return "{\"ok\":true,\"colour\":" + q(S.themeColour) + ",\"default\":\"#fb695a\",\"dark\":\"" + (S.themeDark ? "1" : "0") + "\",\"exact\":\"1\"}";
    if (m == "theme_set") {
        if (!x("colour").empty()) S.themeColour = x("colour") == "default" ? "#fb695a" : x("colour");
        if (!x("dark").empty()) S.themeDark = x("dark") == "1";
        return ok;
    }
    if (m == "power" && arg == "sleep") { SDL_Log("SongateOS: power sleep (the stand-in: the screen would turn off)"); return ok; }
    if (m == "power") { if (arg == "restart") { S.rootState = "idle"; S.recState = "idle"; plat::sysBump("root"); } return ok; }   // (simulated: nothing restarts)
    return "{\"ok\":false,\"error\":\"unsupported: not in the simulated service\"}";
}
void simTick() {   // a connection's progress, a download's, as the service would report them
    if (S.cardAt && SDL_GetTicks() - S.cardAt > 2500) {   // (the card ejected, or used again)
        S.cardAt = 0;
        S.cardState = S.cardState == "ejecting" ? "unmounted" : "mounted";
        plat::sysBump("storage");
    }
    if (S.ota == "downloading") {
        const Uint32 d = SDL_GetTicks() - S.otaAt;
        S.otaProgress = std::min<int64_t>(1288490188, int64_t(1288490188.0 * d / 6000.0));
        if (d >= 6000) S.ota = "ready";
        static Uint32 last = 0;
        if (SDL_GetTicks() - last >= 1000 || S.ota == "ready") { last = SDL_GetTicks(); plat::sysBump("ota"); }   // (once a second, as the service)
    }
    if (!S.linkAt || SDL_GetTicks() < S.linkAt) return;
    S.linkAt = 0;
    S.link = S.linkTo;
    if (S.link == "failed") S.fail = "wrong_password";
    plat::sysBump("wifi");
}
#endif

std::string call(const std::string& m, const std::string& arg, const Extras& ex) {
#ifdef GF_DEV
    if (sim) return simCall(m, arg, ex);
#endif
    std::string lines;
    for (const auto& [k, v] : ex) lines += k + "=" + v + "\n";
    std::string out = plat::sysCall(m, arg, lines);
    wipe(lines);
    return out;
}

/** The answer; its refusal noted (no_permission: that area falls back to Android's page) and kept for the screen */
// (quiet: a read whose refusal is a state, not news: no headphones connected, the codec not known yet, earbuds it does not control)
bool answer(const std::string& text, json::Json& j, const std::string& area, bool quiet = false) {
    if (!json::parseJson(text, j)) { std::lock_guard<std::mutex> g(mx); err = "No answer from the system service"; errNew = true; return false; }
    if (flag(j, "ok") || j["ok"].kind == json::Json::Null) return true;
    const std::string e = j.s("error");
    const size_t colon = e.find(": ");
    const std::string code = colon == std::string::npos ? e : e.substr(0, colon);
    std::lock_guard<std::mutex> g(mx);
    if (code == "no_permission" && !area.empty()) refused.insert(area);
    if (quiet && code != "no_permission") return false;
    err = colon == std::string::npos ? e : e.substr(colon + 2);
    errNew = !err.empty();
    return false;
}

void readWifi() {
    json::Json j;
    if (!answer(call("wifi_get", "", {}), j, "wifi")) return;
    Wifi w;
    w.known = true;
    w.enabled = flag(j, "enabled");
    w.scanning = flag(j, "scanning");
    w.state = j.s("state");
    w.link = j.s("link");
    w.fail = j.s("fail");
    const json::Json& c = j["current"];
    if (c.kind == json::Json::Obj && !c.s("ssid").empty()) {
        w.hasCur = true;
        w.cur.ssid = c.s("ssid");
        w.cur.level = num(c, "level", -1);
        w.cur.security = c.s("security");
        w.cur.ip = c.s("ip");
        w.cur.connected = true;
    }
    for (const json::Json& n : j["networks"].arr) {
        Net x;
        x.key = n.s("key");
        x.ssid = n.s("ssid");
        x.security = n.s("security");
        x.level = num(n, "level", -1);
        x.saved = flag(n, "saved");
        x.connected = flag(n, "connected");
        if (x.connected || (w.hasCur && x.ssid == w.cur.ssid)) {   // (the one in use has a card of its own: its key kept for Forget)
            if (w.hasCur && x.ssid == w.cur.ssid) { w.cur.key = x.key; w.cur.saved = x.saved; }
            continue;
        }
        w.nets.push_back(x);
    }
    std::lock_guard<std::mutex> g(mx);
    wifiV = std::move(w);
    genV++;
}

void readBt() {
    json::Json j;
    if (!answer(call("bt_get", "", {}), j, "bt")) return;
    Bt b;
    b.known = true;
    b.enabled = flag(j, "enabled");
    b.discovering = flag(j, "discovering");
    b.name = j.s("name");
    for (const json::Json& d : j["devices"].arr) {
        BtDev x;
        x.address = d.s("address");
        x.name = d.s("name").empty() ? x.address : d.s("name");
        x.kind = d.s("kind");
        x.bonded = flag(d, "bonded");
        x.connected = flag(d, "connected");
        x.battery = num(d, "battery", -1);
        b.devs.push_back(x);
    }
    // paired first, connected first among them
    std::stable_sort(b.devs.begin(), b.devs.end(), [](const BtDev& a, const BtDev& c) {
        return a.bonded != c.bonded ? a.bonded : a.connected != c.connected ? a.connected : false;
    });
    std::lock_guard<std::mutex> g(mx);
    btV = std::move(b);
    genV++;
}

void readOutput() {
    json::Json j;
    if (!answer(call("output_get", "", {}), j, "output")) return;
    Output o;
    o.known = true;
    o.active = j.s("active");
    o.choice = j.s("choice").empty() ? "auto" : j.s("choice");
    o.connected = j["connected"].kind == json::Json::Null || flag(j, "connected");
    for (const json::Json& d : j["outputs"].arr) {
        Out x;
        x.id = d.s("id");
        x.kind = d.s("kind");
        x.name = d.s("name");
        x.codec = d.s("codec");
        x.active = flag(d, "active") || x.id == o.active;
        x.bitperfect = flag(d, "bitperfect");
        x.dacVolume = flag(d, "dac_volume");
        o.outs.push_back(x);
    }
    std::lock_guard<std::mutex> g(mx);
    outV = std::move(o);
    genV++;
}

void readOta() {
    json::Json j;
    if (!answer(call("ota_status", "", {}), j, "ota")) return;
    auto big = [&](const char* k) { const json::Json& v = j[k]; return v.kind == json::Json::Num ? (long long)v.num : 0LL; };
    Ota o;
    o.known = true;
    o.state = j.s("state");
    o.error = j.s("error");
    o.current = j.s("current");
    o.latest = j.s("latest");
    o.notes = j.s("notes");
    o.recovery = j.s("recovery");
    o.size = big("size");
    o.progress = big("progress");
    o.lastCheck = big("last_check");
    o.prerelease = flag(j, "prerelease");
    o.autoCheck = flag(j, "auto");
    o.beta = flag(j, "beta");
    o.card = j["card"].kind == json::Json::Null || flag(j, "card");
    std::lock_guard<std::mutex> g(mx);
    otaV = std::move(o);
    genV++;
}

void readTheme() {
    json::Json j;
    if (!answer(call("theme_get", "", {}), j, "theme")) return;
    Theme t;
    t.known = true;
    t.colour = j.s("colour");
    t.def = j.s("default");
    t.dark = flag(j, "dark");
    for (std::string* s : { &t.colour, &t.def }) for (char& c : *s) c = char(tolower(static_cast<unsigned char>(c)));
    std::lock_guard<std::mutex> g(mx);
    themeV = std::move(t);
    genV++;
}

std::string errCode(const json::Json& j) {
    const std::string e = j.s("error");
    const size_t colon = e.find(": ");
    return colon == std::string::npos ? e : e.substr(0, colon);
}

std::vector<std::string> strs(const json::Json& a) {
    std::vector<std::string> v;
    for (const json::Json& x : a.arr) if (x.kind == json::Json::Str) v.push_back(x.str);
    return v;
}

/** hold_get's answer, or hold_set's (the same fields) */
Hold holdFrom(const json::Json& j) {
    Hold h;
    h.known = true;
    h.locked = flag(j, "locked");
    h.sleep = flag(j, "sleep");
    h.level = j.s("level");
    h.levels = strs(j["levels"]);
    return h;
}

void readSetup() {
    json::Json j;
    const std::string text = call("setup_get", "", {});
    if (!answer(text, j, "setup", true)) { SDL_Log("SongateOS: setup_get not answered: %.160s", text.c_str()); return; }
    Setup s;
    s.known = true;
    s.done = flag(j, "done");
    s.android = flag(j, "android");
    std::lock_guard<std::mutex> g(mx);
    setupV = s;
    genV++;
}

void readTime() {
    json::Json j;
    if (!answer(call("time_get", "", {}), j, "time")) return;
    Time t;
    t.known = true;
    t.autoTime = flag(j, "auto_time");
    t.autoZone = flag(j, "auto_zone");
    t.zone = j.s("zone");
    t.zoneName = j.s("zone_name");
    t.zoneLong = j.s("zone_long");
    t.offset = num(j, "offset", 0);
    std::lock_guard<std::mutex> g(mx);
    timeV = std::move(t);
    genV++;
}

void readZones() {
    json::Json j;
    if (!answer(call("zones_list", "", {}), j, "time")) return;
    std::vector<Zone> v;
    for (const json::Json& z : j["zones"].arr) v.push_back({ z.s("id"), z.s("name"), z.s("long"), num(z, "offset", 0) });
    std::lock_guard<std::mutex> g(mx);
    zonesV = std::move(v);
    genV++;
}

void readRecovery() {
    json::Json j;
    if (!answer(call("recovery_get", "", {}), j, "recovery")) return;
    auto big = [&](const char* k) { const json::Json& v = j[k]; return v.kind == json::Json::Num ? (long long)v.num : 0LL; };
    Recovery r;
    r.known = true;
    r.installed = j.s("installed");
    r.ready = flag(j, "installed_ready");
    r.version = j.s("installed_version");
    r.state = j.s("state");
    r.error = j.s("error");
    r.twrpVersion = j.s("twrp_version");
    r.foxVersion = j.s("orangefox_version");
    r.twrpSize = big("twrp_size");
    r.foxSize = big("orangefox_size");
    r.progress = big("progress");
    std::lock_guard<std::mutex> g(mx);
    recV = std::move(r);
    genV++;
}

Root rootFrom(const json::Json& j) {
    Root r;
    r.known = true;
    r.installed = flag(j, "installed");
    r.keep = flag(j, "keep");
    r.version = j.s("version");
    r.latest = j.s("latest");
    r.state = j.s("state");
    r.error = j.s("error");
    return r;
}

void readRoot() {
    json::Json j;
    if (!answer(call("root_get", "", {}), j, "root")) return;
    std::lock_guard<std::mutex> g(mx);
    rootV = rootFrom(j);
    genV++;
}

Gapps gappsFrom(const json::Json& j) {
    auto big = [](const json::Json& o, const char* k) {
        const json::Json& v = o[k];
        return v.kind == json::Json::Num ? (long long)v.num : v.kind == json::Json::Str && !v.str.empty() ? atoll(v.str.c_str()) : 0LL;
    };
    Gapps r;
    r.known = true;
    r.installed = j.s("installed");
    r.installedVersion = j.s("installed_version");
    r.keep = flag(j, "keep");
    r.state = j.s("state");
    r.progress = num(j, "progress", 0);
    r.error = j.s("error");
    for (const json::Json& v : j["variants"].arr) r.variants.push_back({ v.s("id"), v.s("name"), v.s("summary"), v.s("version"), big(v, "size") });
    return r;
}

void readGapps() {
    json::Json j;
    if (!answer(call("gapps_get", "", {}), j, "gapps")) return;
    std::lock_guard<std::mutex> g(mx);
    gappsV = gappsFrom(j);
    genV++;
}


void readHold() {
    json::Json j;
    if (!answer(call("hold_get", "", {}), j, "hold")) return;
    std::lock_guard<std::mutex> g(mx);
    holdV = holdFrom(j);
    genV++;
}

void readCodec() {
    json::Json j;
    Codec c;
    if (answer(call("bt_codec_get", "", {}), j, "bt", true)) {
        c.known = true;
        c.address = j.s("address");
        c.codec = j.s("codec");
        c.ldac = j.s("ldac");
        c.rate = num(j, "rate", 0);
        c.bits = num(j, "bits", 0);
        c.available = strs(j["available"]);
    }
    std::lock_guard<std::mutex> g(mx);
    codecV = std::move(c);   // (refused, not connected, not known yet: none shown)
    genV++;
}

/** earbuds_get's answer, or earbuds_set's (the same fields, read back from the earbuds) */
Earbuds budsFrom(const json::Json& j) {
    Earbuds b;
    b.known = b.supported = true;
    b.kind = j.s("kind");
    b.name = j.s("name");
    b.address = j.s("address");
    b.mode = j.s("mode");
    b.anc = j.s("anc");
    b.transparency = j.s("transparency");
    b.transport = j.s("transport");
    b.ancLevel = num(j, "anc_level", 0);
    b.wind = flag(j, "wind");
    b.batteryL = num(j, "battery_left", -1);
    b.batteryR = num(j, "battery_right", -1);
    b.chargingL = flag(j, "charging_left");
    b.chargingR = flag(j, "charging_right");
    b.batteryCase = num(j, "battery_case", -1);
    b.batteryCaseLast = num(j, "battery_case_last", -1);
    b.chargingCase = flag(j, "charging_case");
    b.conversation = j.s("conversation");
    b.adaptiveLevel = num(j, "adaptive_level", -1);
    b.ancLevelMax = num(j, "anc_level_max", -1);
    b.ambientLevel = num(j, "ambient_level", -1);
    b.ambientLevelMax = num(j, "ambient_level_max", -1);
    b.conversationTimeout = num(j, "conversation_timeout", -1);
    b.oneBud = j.s("one_bud");
    b.modes = strs(j["modes"]);
    return b;
}

void readEarbuds() {
    json::Json j;
    Earbuds b;
    if (answer(call("earbuds_get", "", {}), j, "earbuds", true)) b = budsFrom(j);
    else b.known = errCode(j) == "unsupported";   // (not ones it controls: known, no card; not connected, busy: not known)
    std::lock_guard<std::mutex> g(mx);
    budsV = std::move(b);
    genV++;
}

void readDisplay() {
    json::Json j;
    if (!answer(call("display_get", "", {}), j, "display")) return;
    Display d;
    d.known = true;
    d.brightness = num(j, "brightness", 0);
    d.timeout = num(j, "timeout", -1);
    for (const json::Json& x : j["timeouts"].arr) if (x.kind == json::Json::Num) d.timeouts.push_back(int(x.num));
    std::lock_guard<std::mutex> g(mx);
    dispV = std::move(d);
    genV++;
}

void readStorage() {
    json::Json j;
    if (!answer(call("storage_get", "", {}), j, "storage")) return;
    auto big = [](const json::Json& o, const char* k) { const json::Json& v = o[k]; return v.kind == json::Json::Num ? (long long)v.num : 0LL; };
    Storage s;
    s.known = true;
    for (const json::Json& v : j["volumes"].arr) {
        Volume x;
        x.id = v.s("id");
        x.label = v.s("label");
        x.kind = v.s("kind");
        x.state = v.s("state");
        x.fs = v.s("fs");
        x.total = big(v, "total");
        x.free = big(v, "free");
        s.vols.push_back(x);
    }
    std::lock_guard<std::mutex> g(mx);
    stoV = std::move(s);
    genV++;
}

void readUsb() {
    json::Json j;
    if (!answer(call("usb_get", "", {}), j, "usb")) return;
    Usb u;
    u.known = true;
    u.connected = flag(j, "connected");
    u.mode = j.s("mode");
    u.modes = strs(j["modes"]);
    std::lock_guard<std::mutex> g(mx);
    usbV = std::move(u);
    genV++;
}

void readApps() {
    json::Json j;
    if (!answer(call("apps_running", "", {}), j, "apps")) return;
    Running r;
    r.known = true;
    for (const json::Json& a : j["running"].arr) {
        if (a.kind != json::Json::Obj) continue;
        RunningApp x{ a.s("package"), a.s("state") };
        if (!x.package.empty()) r.apps.push_back(std::move(x));
    }
    std::lock_guard<std::mutex> g(mx);
    runV = std::move(r);   // (never logged: which apps run is the user's own)
    genV++;
}

void readAbout() {
    json::Json j;
    if (!answer(call("about", "", {}), j, "about")) return;
    About a;
    a.known = true;
    a.device = j.s("device");
    a.songateos = j.s("songateos");
    a.android = j.s("android");
    a.patch = j.s("patch");
    a.vendorPatch = j.s("vendor_patch");
    a.build = j.s("build");
    a.kernel = j.s("kernel");
    a.recovery = j.s("recovery");
    a.service = j.s("service");
    a.batteryStatus = j.s("battery_status");
    const json::Json& r = j["ram"];
    a.ram = r.kind == json::Json::Num ? (long long)r.num : 0;
    a.battery = num(j, "battery", -1);
    a.batteryTemp = num(j, "battery_temp", 0);
    std::lock_guard<std::mutex> g(mx);
    aboutV = std::move(a);
    genV++;
}

void read(const std::string& area) {
    if (area == "hold") readHold();
    else if (area == "setup") readSetup();
    else if (area == "time") readTime();
    else if (area == "zones") readZones();
    else if (area == "recovery") readRecovery();
    else if (area == "root") readRoot();
    else if (area == "gapps") readGapps();
    else if (area == "codec") readCodec();
    else if (area == "earbuds") readEarbuds();
    else if (area == "display") readDisplay();
    else if (area == "storage") readStorage();
    else if (area == "usb") readUsb();
    else if (area == "apps") readApps();
    else if (area == "about") readAbout();
    else if (area == "theme") readTheme();
    else if (area == "wifi") readWifi();
    else if (area == "bt") readBt();
    else if (area == "output") readOutput();
    else if (area == "ota") readOta();
}

/** sys_info: true once settled (the service's answer, or a refusal: an older service has no system API, Android's pages then);
 *  false while it does not answer at all (not started yet: at a first boot Songate, the home app, comes up well before it) */
bool info() {
    json::Json j;
    const std::string text = call("sys_info", "", {});
    if (!json::parseJson(text, j) || !flag(j, "ok")) {
        const bool silent = text.find("no_answer") != std::string::npos || !json::parseJson(text, j);
        SDL_Log("SongateOS: sys_info %s: %.160s", silent ? "no answer yet" : "refused", text.c_str());
        if (silent) return false;
        availV = int(Avail::No);
        return true;
    }
    std::lock_guard<std::mutex> g(mx);
    caps.clear();
    for (const json::Json& c : j["caps"].arr) if (c.kind == json::Json::Str) caps.insert(c.str);
    versionV = j.s("songateos");
    availV = int(Avail::Yes);
    genV++;
    SDL_Log("SongateOS: sys_info answered, SongateOS %s, setup %s", versionV.c_str(), caps.count("setup") ? "offered" : "not offered");
    return true;
}

void worker() {
#ifdef GF_DEV
    if (FILE* f = fopen((std::string(plat::configDir()) + "/mock-r4.txt").c_str(), "rb")) { sim = true; fclose(f); }
#endif
    // asked again every 2 s while the service does not answer, 90 times at most (3 minutes): then Android's pages
    for (int tries = 1; !info(); tries++) {
        if (tries >= 90) { availV = int(Avail::No); SDL_Log("SongateOS: no answer after 3 minutes, Android's pages"); break; }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    for (;;) {
        Job job;
        std::vector<std::string> due;
        {
            std::unique_lock<std::mutex> g(mx);
            cv.wait_for(g, std::chrono::milliseconds(250), [] { return !jobs.empty() || !wanted.empty(); });
            if (!jobs.empty()) { job = std::move(jobs.front()); jobs.pop_front(); }
            // the areas shown that the service says changed (its notifyChange; counted by platform_android.cpp)
            for (const std::string& a : watched) {
                const unsigned n = plat::sysChanged(a.c_str());
                if (n != seen[a]) { seen[a] = n; wanted.insert(a); }
            }
            due.assign(wanted.begin(), wanted.end());
            wanted.clear();
        }
#ifdef GF_DEV
        if (sim) simTick();
#endif
        if (!job.method.empty()) {
            json::Json j;
            const std::string text = call(job.method, job.arg, job.extras);
            // home_ready (service v58): its answer only logged; an older service does not know it, nothing is shown for that
            if (job.method == "home_ready") SDL_Log("SongateOS: home_ready answered %s", text.c_str());
            if (job.method == "earbuds_set") {   // (logged without the earbuds' name or address: a name often holds its owner's)
                std::string shown = text;
                for (const char* key : { "\"name\":\"", "\"address\":\"" }) {
                    const size_t at = shown.find(key);
                    if (at == std::string::npos) continue;
                    const size_t from = at + strlen(key), to = shown.find('"', from);
                    if (to != std::string::npos) shown.replace(from, to - from, "…");
                }
                SDL_Log("SongateOS: earbuds_set answered %.400s", shown.c_str());
            }
            const bool okay = job.method == "home_ready" || answer(text, j, job.area);
            if (job.method == "ota_preflight") { std::lock_guard<std::mutex> g(mx); preflightV = okay ? 1 : 0; }
            for (auto& [k, v] : job.extras) if (k == "password") wipe(v);
            if (job.method == "root_set" && okay) {   // (its answer is root's state)
                std::lock_guard<std::mutex> g(mx);
                rootV = rootFrom(j);
                genV++;
            } else if (job.method == "hold_set" && okay) {   // (its answer is the switch's state: no second call)
                std::lock_guard<std::mutex> g(mx);
                holdV = holdFrom(j);
                genV++;
            } else if (job.method == "earbuds_set") {   // (its answer is the earbuds read back: no second call; refused: read again)
                if (okay) { std::lock_guard<std::mutex> g(mx); budsV = budsFrom(j); genV++; }
                else due.push_back("earbuds");
            } else if (!job.area.empty()) due.push_back(job.area);
        }
        // the codec read again with Bluetooth or the output (its changes come through …/system/bt), while it is shown
        {
            std::lock_guard<std::mutex> g(mx);
            if (watched.count("codec") && std::any_of(due.begin(), due.end(), [](const std::string& a) { return a == "bt" || a == "output"; }))
                due.push_back("codec");
        }
        std::sort(due.begin(), due.end());
        due.erase(std::unique(due.begin(), due.end()), due.end());
        for (const std::string& a : due) read(a);
    }
}

}  // namespace

void start() {
    std::lock_guard<std::mutex> g(mx);
    if (started) return;
    started = true;
    std::thread(worker).detach();
}

Avail avail() { return Avail(availV.load()); }

bool has(const char* cap) {
    std::lock_guard<std::mutex> g(mx);
    // (an area refused once falls back to Android's page: "bt" covers bt_codec, "ota"/"power" their own)
    std::string area = cap;
    if (area == "bt_codec") area = "bt";
    return availV.load() == int(Avail::Yes) && caps.count(cap) && !refused.count(area);
}

std::string version() { std::lock_guard<std::mutex> g(mx); return versionV; }
Wifi wifi() { std::lock_guard<std::mutex> g(mx); return wifiV; }
Bt bt() { std::lock_guard<std::mutex> g(mx); return btV; }
Output output() { std::lock_guard<std::mutex> g(mx); return outV; }
Ota ota() { std::lock_guard<std::mutex> g(mx); return otaV; }
Theme theme() { std::lock_guard<std::mutex> g(mx); return themeV; }
Hold hold() { std::lock_guard<std::mutex> g(mx); return holdV; }
Setup setup() { std::lock_guard<std::mutex> g(mx); return setupV; }
void setupForget() { std::lock_guard<std::mutex> g(mx); setupV = {}; }
void readAgain(const char* area) {
    {
        std::lock_guard<std::mutex> g(mx);
        wanted.insert(area);
    }
    cv.notify_one();
}
Time time() { std::lock_guard<std::mutex> g(mx); return timeV; }
std::vector<Zone> zones() { std::lock_guard<std::mutex> g(mx); return zonesV; }
void zonesWanted() {
    {
        std::lock_guard<std::mutex> g(mx);
        if (zonesAsked) return;
        zonesAsked = true;
        wanted.insert("zones");
    }
    cv.notify_one();
}
Recovery recovery() { std::lock_guard<std::mutex> g(mx); return recV; }
Root root() { std::lock_guard<std::mutex> g(mx); return rootV; }
Gapps gapps() { std::lock_guard<std::mutex> g(mx); return gappsV; }
Codec codec() { std::lock_guard<std::mutex> g(mx); return codecV; }
Earbuds earbuds() { std::lock_guard<std::mutex> g(mx); return budsV; }
Display display() { std::lock_guard<std::mutex> g(mx); return dispV; }
Storage storage() { std::lock_guard<std::mutex> g(mx); return stoV; }
Usb usb() { std::lock_guard<std::mutex> g(mx); return usbV; }
Running running() { std::lock_guard<std::mutex> g(mx); return runV; }
About about() { std::lock_guard<std::mutex> g(mx); return aboutV; }
unsigned gen() { return genV.load(); }

void preflight() {
    { std::lock_guard<std::mutex> g(mx); preflightV = -1; }
    act("ota_preflight", "", {}, "ota");
}

bool takePreflight(bool& ok) {
    std::lock_guard<std::mutex> g(mx);
    if (preflightV < 0) return false;
    ok = preflightV == 1;
    preflightV = -1;
    return true;
}

void watch(const char* area, bool on) {
    {
        std::lock_guard<std::mutex> g(mx);
        if (on == (watched.count(area) > 0)) return;
        if (on) { watched.insert(area); wanted.insert(area); }
        else watched.erase(area);
    }
    // (the codec's changes come through bt's notifications; the earbuds' only after Songate's own change, whose answer it reads;
    // About has none: each of these is read when it shows)
    const std::string a = area;
    const bool notified = a != "codec" && a != "earbuds" && a != "about" && a != "zones";
#ifdef GF_DEV
    if (!sim)
#endif
        if (notified) plat::sysWatch(area, on);
    cv.notify_one();
}

void earbudsSet(const Extras& extras) {
    {
        std::lock_guard<std::mutex> g(mx);
        for (const auto& [k, v] : extras) {   // (shown at once; the earbuds' own answer follows in a second)
            if (k == "mode") budsV.mode = v;
            else if (k == "anc") budsV.anc = v;
            else if (k == "anc_level") budsV.ancLevel = atoi(v.c_str());
            else if (k == "transparency") budsV.transparency = v;
            else if (k == "transport") budsV.transport = v;
            else if (k == "wind") budsV.wind = v == "1";
            else if (k == "conversation") budsV.conversation = v;   // (AirPods' Conversation awareness: it flipped at once as the rest)
            else if (k == "adaptive_level") budsV.adaptiveLevel = atoi(v.c_str());
            else if (k == "ambient_level") budsV.ambientLevel = atoi(v.c_str());           // (Galaxy Buds')
            else if (k == "conversation_timeout") budsV.conversationTimeout = atoi(v.c_str());
            else if (k == "one_bud") budsV.oneBud = v;
        }
        SDL_Log("SongateOS: earbuds_set %s", extras.empty() ? "" : (extras[0].first + "=" + extras[0].second).c_str());
        genV++;
    }
    act("earbuds_set", "", extras, "earbuds");
}

void scanIfDue() {
    const Uint32 now = SDL_GetTicks();
    if (lastScan && now - lastScan < 10000) return;
    lastScan = now;
    act("wifi_scan", "", {}, "wifi");
}

void act(const std::string& method, const std::string& arg, const Extras& extras, const char* area) {
    {
        std::lock_guard<std::mutex> g(mx);
        jobs.push_back({ method, arg, extras, area ? area : "" });
    }
    cv.notify_one();
}

bool takeError(std::string& out) {
    std::lock_guard<std::mutex> g(mx);
    if (!errNew) return false;
    errNew = false;
    out = err;
    return true;
}

}  // namespace sysapi

#else   // no SongateOS anywhere else

namespace sysapi {
void start() {}
Avail avail() { return Avail::No; }
bool has(const char*) { return false; }
std::string version() { return {}; }
Wifi wifi() { return {}; }
Bt bt() { return {}; }
Output output() { return {}; }
Ota ota() { return {}; }
Theme theme() { return {}; }
Hold hold() { return {}; }
Setup setup() { return {}; }
void setupForget() {}
void readAgain(const char*) {}
Time time() { return {}; }
std::vector<Zone> zones() { return {}; }
void zonesWanted() {}
Recovery recovery() { return {}; }
Root root() { return {}; }
Gapps gapps() { return {}; }
Codec codec() { return {}; }
Earbuds earbuds() { return {}; }
Display display() { return {}; }
Storage storage() { return {}; }
Usb usb() { return {}; }
Running running() { return {}; }
About about() { return {}; }
void earbudsSet(const std::vector<std::pair<std::string, std::string>>&) {}
void preflight() {}
bool takePreflight(bool&) { return false; }
unsigned gen() { return 0; }
void watch(const char*, bool) {}
void scanIfDue() {}
void act(const std::string&, const std::string&, const std::vector<std::pair<std::string, std::string>>&, const char*) {}
bool takeError(std::string&) { return false; }
}  // namespace sysapi

#endif
