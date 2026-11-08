/* Gatefold NX preview · platform_preview.cpp
 * The platform for the PC test harness: it looks like the console (sdmc:/ paths, Joy-Con buttons) through the stand-ins
 * of switch.h and hostshim.cpp. The real PC version is platform/windows/. */
#include "../../source/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "include/switch.h"

namespace plat {

namespace {
PadState pad;
}

void init() {}
void quit() {}
bool mainLoop() { return appletMainLoop(); }
void holdExit() {}
void releaseExit() {}
void watchLeave(LeaveFn, void*) {}
void unwatchLeave() {}
bool inFocus() { return true; }
std::string selfPath(int, char**) { return "sdmc:/switch/Songate/Songate.nro"; }
bool swapsSelf() { return false; }
bool relaunch(const std::string& program) {
    printf("[updater] Songate starts again: %s\n", program.c_str());
    return true;
}
bool legacyData() { return false; }

void padInit() { padInitializeDefault(&pad); }
void padRead() { padUpdate(&pad); }
uint64_t padDown() { return padGetButtonsDown(&pad); }
uint64_t padHeld() { return padGetButtons(&pad); }
Stick stick(int which) {
    const HidAnalogStickState s = padGetStickPos(&pad, u32(which));
    return { s.x, s.y };
}

TextResult askText(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    SwkbdConfig kbd;
    swkbdCreate(&kbd, 0);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetHeaderText(&kbd, header);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    return R_SUCCEEDED(swkbdShow(&kbd, out, size)) ? TextResult::Done : TextResult::Cancelled;
}

void setMediaPlaying(bool on) { appletSetMediaPlaybackState(on); }
void setKeepAwake(bool on) {
    appletSetIdleTimeDetectionExtension(on ? AppletIdleTimeDetectionExtension_ExtendedUnsafe : AppletIdleTimeDetectionExtension_None);
    appletSetAutoSleepDisabled(on);
}
void requestSleep() { appletRequestToSleep(); }
bool dimBacklight(float& saved) {
    if (appletGetOperationMode() != AppletOperationMode_Handheld) return false;
    lblGetCurrentBrightnessSetting(&saved);
    lblSetCurrentBrightnessSetting(saved < 0.04f ? saved : 0.04f);
    return true;
}
void restoreBacklight(float saved) { lblSetCurrentBrightnessSetting(saved); }
bool volumeKeys(bool& up, bool& down, std::string* err) { up = down = false; if (err) *err = "the Switch only"; return false; }
bool backlightOff(bool) { return false; }
bool blockHome(bool) { return false; }
bool takeCapture(bool) { return false; }
int capturePresses() { return 0; }
int disconnectControllers() { return 0; }
bool outputVolume(int&) { return false; }
bool setOutputVolume(int) { return false; }
bool batteryLevel(unsigned& percent) {
    u32 p = 0;
    psmGetBatteryChargePercentage(&p);
    if (getenv("GF_BATTERY_MOCK")) {   // (as the raw charge, which hostshim gives for each state)
        double raw = 0;
        psmGetRawBatteryChargePercentage(&raw);
        p = u32(raw);
    }
    percent = p;
    return true;
}
bool batteryRaw(double& percent, bool& charging) {
    PsmChargerType ct = PsmChargerType_Unconnected;
    psmGetRawBatteryChargePercentage(&percent);
    psmGetChargerType(&ct);
    charging = ct != PsmChargerType_Unconnected;
    return true;
}
// GF_BATTERY_MOCK: low, slow, normal, fast or full: the battery card and the animations in each state (hostshim's psm stand-ins too)
bool batteryInfo(BatteryInfo& o) {
    const char* m = getenv("GF_BATTERY_MOCK");
    o = BatteryInfo{};
    o.volts = 3.92;
    o.tempC = 31.4;
    o.healthPct = 92.6;
    if (!m) return true;
    const std::string k = m;
    if (k == "slow") { o.charger = 2; o.chargerVolts = 5; o.chargerAmps = 0.5; o.volts = 3.88; }
    else if (k == "normal") { o.charger = 1; o.chargerVolts = 15; o.chargerAmps = 1.2; o.volts = 4.01; o.tempC = 33.8; }
    else if (k == "fast" || k == "full") { o.charger = 1; o.fast = true; o.chargerVolts = 15; o.chargerAmps = 2.6; o.volts = k == "full" ? 4.35 : 4.08; o.tempC = 36.1; }
    else if (k == "low") { o.volts = 3.55; o.tempC = 29.0; }
    // the fuel gauge: what each state would read
    o.gauge = true;
    o.gaugeVolts = o.volts;
    o.gaugeAmps = k == "low" ? -1.35 : k == "slow" ? 0.42 : k == "normal" ? 1.25 : k == "fast" ? 2.3 : k == "full" ? 0.03 : -1.6;
    const double pct = k == "low" ? 0.124 : k == "full" ? 1 : k == "slow" ? 0.412 : k == "normal" ? 0.587 : k == "fast" ? 0.631 : 0.76;
    o.fullMah = 3990;
    o.capMah = 3990 * pct;
    if (o.gaugeAmps < 0) o.tteMin = int(o.capMah / (-o.gaugeAmps * 1000) * 60);
    else if (k != "full") o.ttfMin = int((o.fullMah - o.capMah) / (o.gaugeAmps * 1000) * 60 * 1.15);
    return true;
}

void sysInfo(std::string& firmware, std::string& atmosphere, bool&) {
    firmware = "PC preview";
    atmosphere = "-";
}
long long bootStamp() {
    static const long long at = (long long)time(nullptr);   // the run is the "boot"
    return at;
}
void placeThread(int, int) {}
bool netStart() { return true; }
void netStop() {}
std::vector<Net> networks() { return {}; }
std::string deviceName() { return "Preview"; }
int64_t freeBytes(const std::string&) { return -1; }
bool appletMode() { return false; }
bool randomBytes(uint8_t* out, size_t n) { for (size_t i = 0; i < n; i++) out[i] = uint8_t(rand()); return true; }

int bgProtocol() { return 6; }   // gfbg.h's GFBG_PROTOCOL
bool bgRestart(std::string&) {
    printf("[updater] the background player restarts\n");
    return true;
}
// There is no background player here: ending it "works" (cfgmove may then move the folder), unless GF_BGEND_FAIL says no
bool bgEnd() {
    const bool ok = !getenv("GF_BGEND_FAIL");
    printf("[platform] the background player ends%s\n", ok ? "" : ": refused (GF_BGEND_FAIL)");
    return ok;
}
void bgStart(bool ended) { if (ended) printf("[platform] the background player starts again\n"); }

std::string assetPath(const char* relative) { return std::string("romfs:/") + relative; }
std::vector<FontData> systemFonts() { return {}; }
const char* configParent() { return "sdmc:/config"; }
// Settled at launch by cfgmove::atLaunch (the names in gfcfgdir.h); Store::Dir points at this buffer from before main
char gCfgDir[64] = "sdmc:/config/songate-nx", gCfgBackups[64] = "sdmc:/config/songate-nx-backups";
const char* configDir() { return gCfgDir; }
const char* backupDir() { return gCfgBackups; }
void useConfigDir(const char* dir, const char* backups) {
    snprintf(gCfgDir, sizeof gCfgDir, "%s", dir);
    snprintf(gCfgBackups, sizeof gCfgBackups, "%s", backups);
}
const char* root() { return "sdmc:/"; }
const char* rootName() { return "the SD card"; }
bool isRoot(const std::string& path) { return path.empty() || path == "sdmc:/" || path == "sdmc:"; }
std::string parentDir(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos || s <= 5 ? std::string("sdmc:/") : path.substr(0, s);
}
std::string join(const std::string& dir, const std::string& name) { return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name; }
size_t rootLen(const std::string& path) { return path.compare(0, 6, "sdmc:/") == 0 ? 6 : 0; }
std::string fromRoot(const std::string& path) { return "sdmc:" + path; }
const std::vector<const char*>& systemFolders() {
    static const std::vector<const char*> v = { "sdmc:/atmosphere", "sdmc:/bootloader", "sdmc:/switch", "sdmc:/Nintendo", "sdmc:/config", "sdmc:/emuMMC" };
    return v;
}
bool measureFiles(const std::string&, int&, uint64_t&, int) { return false; }
bool measureStorage(const std::string&, int64_t&, int&, bool (*)(const std::string&)) { return false; }

}  // namespace plat
