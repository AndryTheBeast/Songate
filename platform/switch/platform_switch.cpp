/*
 * Songate · platform_switch.cpp
 * The platform on the Nintendo Switch: libnx.
 */
#include "platform.hpp"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include "gfbg.h"

namespace plat {
namespace { I2cSession gaugeSession{}; bool gaugeOpen = false; }   // the battery's fuel gauge (batteryInfo)

static_assert(u64(BtnA) == u64(HidNpadButton_A) && u64(BtnB) == u64(HidNpadButton_B) && u64(BtnX) == u64(HidNpadButton_X) && u64(BtnY) == u64(HidNpadButton_Y));
static_assert(u64(BtnL) == u64(HidNpadButton_L) && u64(BtnR) == u64(HidNpadButton_R) && u64(BtnZL) == u64(HidNpadButton_ZL) && u64(BtnZR) == u64(HidNpadButton_ZR));
static_assert(u64(BtnPlus) == u64(HidNpadButton_Plus) && u64(BtnMinus) == u64(HidNpadButton_Minus));
static_assert(u64(BtnLeft) == u64(HidNpadButton_Left) && u64(BtnUp) == u64(HidNpadButton_Up) && u64(BtnRight) == u64(HidNpadButton_Right) && u64(BtnDown) == u64(HidNpadButton_Down));

namespace {

constexpr u64 kBgProgram = 0x4200000047464E58ull;

PadState pad;
AppletHookCookie cookie{};
LeaveFn leaveFn = nullptr;
void* leaveArg = nullptr;

void onApplet(AppletHookType type, void*) {
    if (leaveFn && (type == AppletHookType_OnExitRequest || (type == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus)))
        leaveFn(type == AppletHookType_OnExitRequest, leaveArg);
}

/** The console's file system read directly: up to 64 entries with their sizes a request, where stat() asked once a file
 *  (a folder of thousands held the app for many seconds: Details and Delete measure before they show). */
void measureFs(FsFileSystem* fs, const std::string& path, int& files, uint64_t& bytes, int cap) {
    FsDir d;
    char p[FS_MAX_PATH];
    snprintf(p, sizeof p, "%s", path.c_str());
    if (R_FAILED(fsFsOpenDirectory(fs, p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) return;
    static thread_local std::vector<FsDirectoryEntry> buf(64);
    std::vector<std::string> dirs;
    for (;;) {
        s64 n = 0;
        if (files >= cap || R_FAILED(fsDirRead(&d, &n, buf.size(), buf.data())) || n <= 0) break;
        for (s64 i = 0; i < n; i++) {
            const FsDirectoryEntry& e = buf[size_t(i)];
            if (e.type == FsDirEntryType_Dir) dirs.push_back(e.name);
            else { bytes += uint64_t(e.file_size); files++; }
        }
    }
    fsDirClose(&d);
    for (const auto& n : dirs) {
        if (files >= cap) break;
        measureFs(fs, (path == "/" ? "/" : path + "/") + n, files, bytes, cap);
    }
}

/** The same for Storage: stat() asked once per file (thousands of requests for the covers cache, each one queued with
 *  everything else). */
void measureUse(FsFileSystem* fs, const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&), bool top) {
    FsDir d;
    char p[FS_MAX_PATH];
    snprintf(p, sizeof p, "%s", path.c_str());
    if (R_FAILED(fsFsOpenDirectory(fs, p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) {
        FsFile f;   // a file, not a folder
        s64 size = 0;
        if (R_SUCCEEDED(fsFsOpenFile(fs, p, FsOpenMode_Read, &f))) {
            if (R_SUCCEEDED(fsFileGetSize(&f, &size))) { bytes += size; files++; }
            fsFileClose(&f);
        }
        return;
    }
    static thread_local std::vector<FsDirectoryEntry> buf(64);
    std::vector<std::string> dirs;
    for (;;) {
        s64 n = 0;
        if (R_FAILED(fsDirRead(&d, &n, buf.size(), buf.data())) || n <= 0) break;
        for (s64 i = 0; i < n; i++) {
            const FsDirectoryEntry& e = buf[size_t(i)];
            if (top && keep && !keep(e.name)) continue;
            if (e.type == FsDirEntryType_Dir) dirs.push_back(e.name);
            else { bytes += e.file_size; files++; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // a breath between batches for the rest of the app
    }
    fsDirClose(&d);
    for (const auto& n : dirs) measureUse(fs, (path == "/" ? "/" : path + "/") + n, bytes, files, keep, false);
}

}  // namespace

// ---- the app's life

void init() {
    romfsInit();
    plInitialize(PlServiceType_User);
    psmInitialize();
    if (R_SUCCEEDED(i2cInitialize())) gaugeOpen = R_SUCCEEDED(i2cOpenSession(&gaugeSession, I2cDevice_Max17050));
    // (the backlight service, lbl, is opened only around each call: see LblCall. Held for the whole run it took one of the few
    // sessions lbl allows, and Atmosphère's fatal, needing one to show an error screen, panicked instead: 2026-11-09)
}

void quit() {
    if (gaugeOpen) i2csessionClose(&gaugeSession);
    gaugeOpen = false;
    i2cExit();
    psmExit();
    plExit();
    romfsExit();
}

bool mainLoop() { return appletMainLoop(); }
void holdExit() { appletLockExit(); }
void releaseExit() { appletUnlockExit(); }

void watchLeave(LeaveFn fn, void* arg) {
    leaveFn = fn;
    leaveArg = arg;
    appletHook(&cookie, onApplet, nullptr);
}
void unwatchLeave() { appletUnhook(&cookie); }

bool inFocus() { return appletGetFocusState() == AppletFocusState_InFocus; }

std::string selfPath(int argc, char** argv) {
    // hbloader gives the .nro's path
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0 && strstr(argv[0], ".nro")) return argv[0];
    return "sdmc:/switch/Songate/Songate.nro";
}

bool swapsSelf() { return true; }

bool relaunch(const std::string& program) {
    if (!envHasNextLoad()) return false;
    envSetNextLoad(program.c_str(), program.c_str());
    return true;
}

bool legacyData() { return true; }

// ---- input

void padInit() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
}
void padRead() { padUpdate(&pad); }
uint64_t padDown() { return padGetButtonsDown(&pad); }
uint64_t padHeld() { return padGetButtons(&pad); }
Stick stick(int which) {
    const HidAnalogStickState s = padGetStickPos(&pad, which);
    return { s.x, s.y };
}

// ---- typing

TextResult askText(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return TextResult::Unavailable;
    if (textSecret().load()) swkbdConfigMakePresetPassword(&kbd);   // (a password: hidden as typed)
    else swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetHeaderText(&kbd, header);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    const Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) ? TextResult::Done : TextResult::Cancelled;
}

// ---- the machine

void setMediaPlaying(bool on) { appletSetMediaPlaybackState(on); }

void setKeepAwake(bool on) {
    // (1.1 called appletSetAutoSleepTimeAndDimmingTimeEnabled, which only an overlay applet may: from an application it did
    // nothing, so the console dimmed by itself whenever the music was paused.) An application's own ways: the idle
    // time stretched as far as it goes, and auto-sleep off.
    appletSetIdleTimeDetectionExtension(on ? AppletIdleTimeDetectionExtension_ExtendedUnsafe : AppletIdleTimeDetectionExtension_None);
    appletSetAutoSleepDisabled(on);
}

void requestSleep() { appletRequestToSleep(); }

namespace {
/** The backlight service for one call: lbl allows few sessions (the overlays take theirs), and a session kept for the whole run
 *  starved Atmosphère's fatal of one (its error screen turns the backlight on first), so it panicked: 2021-0003 for "lbl" in
 *  its report, 2026-11-09. As the sysmodule's aud:a: open, call, close. */
struct LblCall {
    bool ok;
    LblCall() : ok(R_SUCCEEDED(lblInitialize())) {}
    ~LblCall() { if (ok) lblExit(); }
};
}  // namespace

/** Handheld only: docked, the TV is not ours. */
bool dimBacklight(float& saved) {
    if (appletGetOperationMode() != AppletOperationMode_Handheld) return false;
    LblCall lbl;
    if (!lbl.ok || R_FAILED(lblGetCurrentBrightnessSetting(&saved)) || R_FAILED(lblSetCurrentBrightnessSetting(std::min(saved, 0.04f)))) return false;
    lblApplyCurrentBrightnessSettingToBacklight();
    return true;
}

void restoreBacklight(float saved) {
    LblCall lbl;
    if (!lbl.ok) return;
    lblSetCurrentBrightnessSetting(saved);
    lblApplyCurrentBrightnessSettingToBacklight();
}

namespace {
GpioPadSession volUp_{}, volDown_{};
int volState_ = 0;   // 0 not tried, 1 open, -1 could not
std::string volErr_;
}

bool volumeKeys(bool& up, bool& down, std::string* err) {
    up = down = false;
    if (volState_ == 0) {
        char e[96];
        Result rc = gpioInitialize();
        if (R_FAILED(rc)) { snprintf(e, sizeof e, "gpio: 0x%x", rc); volErr_ = e; volState_ = -1; }
        else if (R_FAILED(rc = gpioOpenSession(&volUp_, GpioPadName_ButtonVolUp))) { snprintf(e, sizeof e, "volume up pad: 0x%x", rc); volErr_ = e; volState_ = -1; }
        else if (R_FAILED(rc = gpioOpenSession(&volDown_, GpioPadName_ButtonVolDown))) { snprintf(e, sizeof e, "volume down pad: 0x%x", rc); volErr_ = e; volState_ = -1; }
        else volState_ = 1;
    }
    if (volState_ < 0) { if (err) *err = volErr_; return false; }
    GpioValue a = GpioValue_High, b = GpioValue_High;
    if (R_FAILED(gpioPadGetValue(&volUp_, &a)) || R_FAILED(gpioPadGetValue(&volDown_, &b))) { if (err) *err = "a pad would not read"; return false; }
    up = a == GpioValue_Low;   // (pressed pulls the line low)
    down = b == GpioValue_Low;
    return true;
}

bool backlightOff(bool off) {
    LblCall lbl;
    return lbl.ok && R_SUCCEEDED(off ? lblSwitchBacklightOff(0) : lblSwitchBacklightOn(0));
}
int disconnectControllers() {   // (attached Joy-Con are the Handheld pad: never touched)
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        const HidNpadIdType id = HidNpadIdType(HidNpadIdType_No1 + i);
        if (hidGetNpadStyleSet(id) != 0 && R_SUCCEEDED(hidDisconnectNpad(id))) ++n;
    }
    return n;
}
namespace {
AppletHookCookie captureCookie;
int capturePresses_ = 0;
void onCapture(AppletHookType hook, void*) { if (hook == AppletHookType_OnCaptureButtonShortPressed) ++capturePresses_; }
}
bool takeCapture(bool on) {   // (an application only, 3.0.0+; a long press still records, the system's)
    static bool taken = false;
    if (on == taken) return true;
    if (on) {
        if (R_FAILED(appletSetRequiresCaptureButtonShortPressedMessage(true))) return false;
        appletHook(&captureCookie, onCapture, nullptr);
    } else {
        appletSetRequiresCaptureButtonShortPressedMessage(false);
        appletUnhook(&captureCookie);
    }
    capturePresses_ = 0;
    taken = on;
    return true;
}
int capturePresses() { const int n = capturePresses_; capturePresses_ = 0; return n; }
bool blockHome(bool on) {   // (an application only: run from the album, HOME stays the system's)
    static bool blocked = false;
    if (on == blocked) return true;
    const AppletType t = appletGetAppletType();
    if (t != AppletType_Application && t != AppletType_SystemApplication) return false;
    // (held back, HOME's presses reach no app: no message, no HID state; tried on HOS 23, 2026-11-08)
    if (R_FAILED(on ? appletBeginBlockingHomeButtonShortAndLongPressed(0) : appletEndBlockingHomeButtonShortAndLongPressed())) return false;
    blocked = on;
    return true;
}

namespace { int audState_ = 0; }   // 0 not tried, 1 open, -1 could not
bool outputVolume(int& level) {
    if (audState_ == 0) audState_ = R_SUCCEEDED(audctlInitialize()) ? 1 : -1;
    AudioTarget t = AudioTarget_Invalid;
    s32 v = 0;
    if (audState_ < 0 || R_FAILED(audctlGetActiveOutputTarget(&t)) || R_FAILED(audctlGetTargetVolume(&v, t))) return false;
    level = int(v);
    return true;
}
bool setOutputVolume(int level) {
    AudioTarget t = AudioTarget_Invalid;
    if (audState_ <= 0 || R_FAILED(audctlGetActiveOutputTarget(&t))) return false;
    return R_SUCCEEDED(audctlSetTargetVolume(t, s32(level)));
}

bool batteryLevel(unsigned& percent) {
    u32 p = 0;
    if (R_FAILED(psmGetBatteryChargePercentage(&p))) return false;
    percent = p;
    return true;
}

bool batteryRaw(double& percent, bool& charging) {
    PsmChargerType ct = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetRawBatteryChargePercentage(&percent)) || R_FAILED(psmGetChargerType(&ct))) return false;
    charging = ct != PsmChargerType_Unconnected;
    return true;
}

namespace {
// The battery's fuel gauge, a MAX17050 on I2C, as Status Monitor and hekate read it: a 5 mΩ sense resistor and a current gain of 2,
// so current is 0.15625 mA a step, charge 0.5 mAh, voltage 78.125 µV, time 5.625 s
bool readGauge(u8 reg, u16& out) {
    if (!gaugeOpen) return false;
    struct { u8 send, sendLength, sendData, receive, receiveLength; } cmd = {
        u8(0 | (I2cTransactionOption_Start << 6)), 1, reg, u8(1 | (I2cTransactionOption_All << 6)), 2 };
    u16 v = 0;
    if (R_FAILED(i2csessionExecuteCommandList(&gaugeSession, &v, sizeof v, &cmd, sizeof cmd))) return false;
    out = v;
    return true;
}
void gaugeInfo(BatteryInfo& o) {
    u16 cur = 0, vcell = 0, rep = 0, full = 0, tte = 0, ttf = 0;
    if (!readGauge(0x0B, cur) || !readGauge(0x19, vcell)) return;   // AvgCurrent, AvgVCELL
    const double amps = int16_t(cur) * 0.15625 / 1000.0, volts = vcell * 78.125e-6;
    if (volts < 3.0 || volts > 4.6 || std::fabs(amps) > 8) return;   // not what a Switch's battery reads: shown as unknown
    o.gauge = true;
    o.gaugeVolts = volts;
    o.gaugeAmps = amps;
    if (readGauge(0x05, rep) && readGauge(0x10, full)) {   // RepCap, FullCAP
        const double r = rep * 0.5, f = full * 0.5;
        if (f >= 1500 && f <= 6000 && r <= f * 1.05) { o.capMah = r; o.fullMah = f; }
    }
    // Charge cycles (the user, 2026-11-11). The chip's own counter (Cycles, 0x17, hundredths of a cycle) is only what the system
    // has not counted yet: over two cycles the system adds it to its own total, kept with the fuel gauge's learned parameters
    // (set:sys, PtmFuelGaugeParameter.cycles_actual, hundredths too), and starts the chip's again. The two together are the
    // battery's count (the console read 1.01 from the chip alone, against some 340). The system's part changes every two cycles:
    // asked at the first reading and every ten minutes after. Not known, or what no battery could have: left out
    u16 cyc = 0;
    if (readGauge(0x17, cyc) && cyc != 0xFFFF) {
        static u32 counted = 0;
        static bool haveCounted = false;
        static int until = 0;
        if (until-- <= 0) {
            until = 60;
            haveCounted = false;
            if (R_SUCCEEDED(setsysInitialize())) {
                SetSysPtmFuelGaugeParameter fg{};
                if (R_SUCCEEDED(setsysGetPtmFuelGaugeParameter(&fg))) { counted = fg.cycles_actual; haveCounted = true; }
                setsysExit();
            }
        }
        char t[48];
        snprintf(t, sizeof t, " cycles=%u counted=%d", unsigned(cyc), haveCounted ? int(counted) : -1);   // (as read, for the log)
        o.raw += t;
        if (haveCounted) {
            const int n = int((double(counted) + cyc) / 100.0 + 0.5);
            if (n > 0 && n < 5000) o.cycles = n;
        }
    }
    if (readGauge(0x11, tte) && tte != 0xFFFF && tte > 0) o.tteMin = int(tte * 5.625 / 60);   // TTE
    if (readGauge(0x20, ttf) && ttf != 0xFFFF && ttf > 0) o.ttfMin = int(ttf * 5.625 / 60);   // TTF
}
}  // namespace

bool batteryInfo(BatteryInfo& o) {
    PsmChargerType ct = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetChargerType(&ct))) return false;
    o = BatteryInfo{};
    o.charger = int(ct);
    if (hosversionAtLeast(17, 0, 0)) {   // the fields' layout from 17.0.0 (libnx: PsmBatteryChargeInfoFields)
        PsmBatteryChargeInfoFields f{};
        if (R_SUCCEEDED(psmGetBatteryChargeInfoFields(&f))) {
            o.fast = f.fast_battery_charging;
            o.volts = f.battery_charge_milli_voltage / 1000.0;
            o.tempC = f.temperature_celcius / 1000.0;
            o.healthPct = f.battery_age_percentage / 1000.0;
            // The charger's agreed voltage and current: libnx's places (0x30, 0x34) read "0 V, 15.0 A" on HOS 22 and 23 (the user's
            // console, 2026-11-05), the voltage a word later. So: the first word from 0x2C on that is a USB-C voltage (4.5 to 21 V)
            // with a current after it (0.05 to 6 A); fast charging is the byte after them.
            u32 w[sizeof f / 4];
            memcpy(w, &f, sizeof w);
            for (size_t i = 0x2C / 4; i + 1 < 0x48 / 4; i++) {
                if (w[i] >= 4500 && w[i] <= 21000 && w[i + 1] >= 50 && w[i + 1] <= 6000) {
                    o.chargerVolts = w[i] / 1000.0;
                    o.chargerAmps = w[i + 1] / 1000.0;
                    o.fast = reinterpret_cast<const u8*>(w)[(i + 2) * 4] != 0;
                    break;
                }
            }
            char hex[16];
            for (size_t i = 0x28 / 4; i < 0x44 / 4; i++) { snprintf(hex, sizeof hex, "%s%X", o.raw.empty() ? "" : " ", unsigned(w[i])); o.raw += hex; }
            gaugeInfo(o);
        }
    } else {
        double age = 0;
        if (R_SUCCEEDED(psmGetBatteryAgePercentage(&age))) o.healthPct = age;
    }
    return true;
}

void sysInfo(std::string& firmware, std::string& atmosphere, bool& docked) {
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) firmware = fw.display_version;
        setsysExit();
    }
    if (R_SUCCEEDED(splInitialize())) {
        u64 v = 0;
        if (R_SUCCEEDED(splGetConfig(SplConfigItem(65000), &v)))   // Exosphère's API version: Atmosphère's own
            atmosphere = std::to_string((v >> 56) & 0xFF) + "." + std::to_string((v >> 48) & 0xFF) + "." + std::to_string((v >> 40) & 0xFF);
        splExit();
    }
    docked = appletGetOperationMode() == AppletOperationMode_Console;
}

long long bootStamp() { return (long long)time(nullptr) - (long long)(armTicksToNs(armGetSystemTick()) / 1000000000ull); }

void placeThread(int core, int priority) {
    svcSetThreadPriority(CUR_THREAD_HANDLE, u32(priority));
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
}

bool netStart() { return R_SUCCEEDED(socketInitializeDefault()); }
void netStop() { socketExit(); }

std::vector<Net> networks() {
    std::vector<Net> out;
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) return out;
    u32 ip = 0, mask = 0, gate = 0, dns1 = 0, dns2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gate, &dns1, &dns2)) && ip) out.push_back({ ip, ip | ~mask });
    nifmExit();
    return out;
}

std::string deviceName() {
    SetSysDeviceNickName n{};
    if (R_SUCCEEDED(setsysInitialize())) {
        const Result rc = setsysGetDeviceNickname(&n);
        setsysExit();
        if (R_SUCCEEDED(rc) && n.nickname[0]) return n.nickname;
    }
    return "Nintendo Switch";
}

int64_t freeBytes(const std::string&) {
    s64 n = 0;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    return fs && R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &n)) ? int64_t(n) : -1;
}

bool appletMode() {
    const AppletType t = appletGetAppletType();
    return t != AppletType_Application && t != AppletType_SystemApplication;
}

bool randomBytes(uint8_t* out, size_t n) {
    randomGet(out, n);
    return true;
}

// ---- the background player

int bgProtocol() { return GFBG_PROTOCOL; }

bool bgRestart(std::string& err) {
    if (R_FAILED(pmshellInitialize())) { err = "The console did not allow it: restart the console instead"; return false; }
    pmshellTerminateProgram(kBgProgram);   // fails harmlessly when it is not running
    svcSleepThread(300'000'000ull);
    NcmProgramLocation loc{};
    loc.program_id = kBgProgram;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    const Result rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_FAILED(rc)) { err = "It did not start again: restart the console"; return false; }
    return true;
}

namespace {
bool bgWasRunning = false;   // at the last bgEnd: one that was off (Settings, or not installed) is not started by bgStart
}

bool bgEnd() {
    const bool pm = R_SUCCEEDED(pmshellInitialize());
    if (pm) {
        bgWasRunning = gfbgRunning();
        pmshellTerminateProgram(kBgProgram);   // fails harmlessly when it is not running
        svcSleepThread(300'000'000ull);
    }
    return pm;
}

void bgStart(bool ended) {
    if (!ended) return;
    if (bgWasRunning) {
        NcmProgramLocation loc{};
        loc.program_id = kBgProgram;
        loc.storageID = NcmStorageId_None;
        u64 pid = 0;
        pmshellLaunchProgram(0, &loc, &pid);   // not installed: fails harmlessly
    }
    bgWasRunning = false;
    pmshellExit();
}

// ---- files

std::string assetPath(const char* relative) { return std::string("romfs:/") + relative; }

std::vector<FontData> systemFonts() {
    std::vector<FontData> out;
    for (PlSharedFontType t : { PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified,
                                PlSharedFontType_ChineseTraditional, PlSharedFontType_KO }) {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, t))) out.push_back({ fd.address, fd.size });
    }
    return out;
}

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

bool measureFiles(const std::string& path, int& files, uint64_t& bytes, int cap) {
    if (path.rfind("sdmc:/", 0) != 0) return false;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return false;
    measureFs(fs, path.substr(5), files, bytes, cap);
    return true;
}

bool measureStorage(const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&)) {
    if (path.rfind("sdmc:/", 0) != 0) return false;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return false;
    measureUse(fs, path.substr(5), bytes, files, keep, true);
    return true;
}

}  // namespace plat
