/*
 * Songate · hibyr4.cpp
 * The HiBy R4's settings service (hibyr4.hpp), on a worker of its own: the screen asks for changes and reads what was last read
 * back; the worker makes the calls (IPC, through SongateActivity: platform_android.cpp) and reads everything again about half a
 * second after each change, so what Settings shows is what the player really applied (the ROM's API asks for it).
 */
#include "hibyr4.hpp"

#ifdef GF_ANDROID
#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "gfeqcfg.h"
#include "platform.hpp"

namespace r4 {
namespace {

std::mutex mx;
std::condition_variable cv;
bool started = false;
std::atomic<int> availV{ 0 };   // Avail
std::map<std::string, std::string> vals;
std::atomic<unsigned> genV{ 0 };
std::deque<std::pair<std::string, std::string>> sets;   // (a key changed again before it was sent: the newer value replaces it)
// The ROM's equalizer, "on,autoPreamp,preamp,preset,g0…g9": Songate owns on, the preset (Custom) and the bands; the Automatic
// preamp switch when the user touches it; the manual preamp (field 2) never: it goes back as the ROM had it
struct EqPart { bool have = false; int on = 0; int bands[GFEQ_BANDS] = {}; } eqPart;
int autoWant = -1;       // the switch, as last set in Songate (−1: as the ROM has it)
bool eqDirty = false;
std::string eqSent;
std::vector<int> eqFields(const std::string& s) {   // the 14 numbers, or the ROM's default when unknown
    std::vector<int> f;
    size_t p = 0;
    while (p <= s.size() && f.size() < 14) {
        size_t e = s.find(',', p);
        if (e == std::string::npos) e = s.size();
        f.push_back(atoi(s.substr(p, e - p).c_str()));
        p = e + 1;
    }
    if (f.size() != 14) f = { 1, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return f;
}
struct Led { int rgb = 0, bright = 0; bool breath = false; } ledWant;
bool ledPending = false;
Uint32 ledAt = 0;
bool refreshWanted = true;
Uint32 readBackAt = 0;   // 0: nothing to read back
std::string err;
bool errNew = false;

void parse(const std::string& all) {
    std::map<std::string, std::string> v;
    size_t p = 0;
    while (p < all.size()) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos) e = all.size();
        const size_t eq = all.find('=', p);
        if (eq != std::string::npos && eq < e) v[all.substr(p, eq - p)] = all.substr(eq + 1, e - eq - 1);
        p = e + 1;
    }
    if (v.empty()) return;
    std::lock_guard<std::mutex> g(mx);
    // (a change asked for and not sent yet keeps its own value on screen)
    for (const auto& s : sets) v[s.first] = s.second;
    vals = std::move(v);
    genV++;
}

#ifdef GF_DEV
// Dev builds: an R4 simulated, for the emulator's tests and mock-ups (<config>/mock-r4.txt present: "key=value" lines over the
// ROM's defaults). It answers as the service does; nothing leaves the app.
bool sim = false;
std::map<std::string, std::string> simVals;
std::string simGet() {
    std::string out;
    for (const auto& [k, v] : simVals) out += k + "=" + v + "\n";
    return out;
}
bool simStart() {
    FILE* f = fopen((std::string(plat::configDir()) + "/mock-r4.txt").c_str(), "rb");
    if (!f) return false;
    simVals = { { "ready", "1" }, { "gain", "low" }, { "filter", "fast_rolloff" }, { "balance", "0" }, { "out35", "headphone" },
                { "out44", "bal_po" }, { "lo_gain", "high" }, { "remote", "1" }, { "vol_steps", "25" }, { "vol_restore", "last" },
                { "vol_boot", "20" }, { "led_mode", "custom" }, { "led_color", "00a0ff" }, { "led_bright", "60" }, { "led_effect", "solid" },
                { "led_charge", "1" }, { "led_screen", "0" }, { "led_back", "screen" }, { "led_back_charge", "0" }, { "led_back_notif", "0" },
                { "led_back_beat", "0" }, { "led_notif", "0" }, { "led_notif_color", "4fc3ff" }, { "led_lowbat", "0" },
                { "led_lowbat_level", "15" }, { "led_quality", "0" }, { "led_q_cd", "ff9030" }, { "led_q_hires", "ff3c30" },
                { "led_q_ultra", "ff3c8c" }, { "led_beat", "0" }, { "st_output", "Headphone" }, { "st_rate", "96000" }, { "out_rate", "96000" }, { "st_out_rate", "96000" }, { "btn_wake", "0" }, { "st_quality", "none" }, { "st_led", "custom" }, { "eq", "1,1,0,8,0,0,0,0,0,0,0,0,0,0" } };
    char line[256];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq != std::string::npos) simVals[l.substr(0, eq)] = l.substr(eq + 1);
    }
    fclose(f);
    return true;
}
#endif

// The service's calls (or, simulated, the map's)
std::string svcSet(const std::string& k, const std::string& v) {
#ifdef GF_DEV
    if (sim) { std::lock_guard<std::mutex> g(mx); simVals[k] = v; return {}; }
#endif
    return plat::hibySet(k, v);
}
bool svcEq(const std::string& e) {
#ifdef GF_DEV
    if (sim) { std::lock_guard<std::mutex> g(mx); simVals["eq"] = e; return true; }
#endif
    return plat::hibyEqSet(e);
}
void svcLed(int rgb, int bright, bool breath) {
#ifdef GF_DEV
    if (sim) return;
#endif
    plat::hibyLedLive(rgb, bright, breath);
}
std::string svcGet() {
#ifdef GF_DEV
    if (sim) { std::lock_guard<std::mutex> g(mx); return simGet(); }
#endif
    return plat::hibyGet();
}

void work() {
#ifdef GF_DEV
    sim = simStart();
    const int st = sim ? 2 : plat::hibyState();
    if (sim) SDL_Log("HiBy R4: SIMULATED (mock-r4.txt)");
#else
    const int st = plat::hibyState();
#endif
    availV = st == 2 ? int(Avail::Yes) : st == 1 ? int(Avail::NoPermission) : int(Avail::No);
    genV++;
    SDL_Log("HiBy R4: %s", st == 2 ? "the settings service is there" : st == 1 ? "the service is there, Songate lacks its permission" : "not this device");
    if (st != 2) return;
    unsigned resumesSeen = plat::resumes();
    for (;;) {
        std::vector<std::pair<std::string, std::string>> doSets;
        bool doEq = false;
        EqPart part;
        int autoNow = -1;
        bool doLed = false, doGet = false;
        Led led;
        {
            std::unique_lock<std::mutex> lk(mx);
            cv.wait_for(lk, std::chrono::milliseconds(40));
            const Uint32 now = SDL_GetTicks();
            if (plat::resumes() != resumesSeen) {
                resumesSeen = plat::resumes();
                refreshWanted = true;
            }
            doSets.assign(sets.begin(), sets.end());
            sets.clear();
            if (eqDirty) {
                doEq = true;
                eqDirty = false;
                part = eqPart;
                autoNow = autoWant;
            }
            if (ledPending && now - ledAt >= 40) {
                doLed = true;
                led = ledWant;
                ledPending = false;
                ledAt = now;
            }
            if (refreshWanted || (readBackAt && SDL_TICKS_PASSED(now, readBackAt))) {
                doGet = true;
                refreshWanted = false;
                readBackAt = 0;
            }
        }
        bool changed = false;
        for (const auto& s : doSets) {
            const std::string e = svcSet(s.first, s.second);
            changed = true;
            if (!e.empty()) {
                SDL_Log("HiBy R4: set %s %s refused: %s", s.first.c_str(), s.second.c_str(), e.c_str());
                std::lock_guard<std::mutex> g(mx);
                err = e;
                errNew = true;
            }
        }
        if (doEq) {
            // the ROM's own fields first (read now if not known yet), then only what Songate changed
            bool known;
            {
                std::lock_guard<std::mutex> g(mx);
                known = vals.count("eq") > 0;
            }
            if (!known) parse(svcGet());
            std::vector<int> f;
            {
                std::lock_guard<std::mutex> g(mx);
                f = eqFields(vals.count("eq") ? vals["eq"] : std::string());
            }
            if (part.have) {
                f[0] = part.on;
                f[3] = 8;   // Custom: the bands are Songate's
                for (int i = 0; i < GFEQ_BANDS; i++) f[size_t(4 + i)] = part.bands[i];
            }
            if (autoNow >= 0) f[1] = autoNow;
            std::string s;
            for (size_t i = 0; i < f.size(); i++) s += (i ? "," : "") + std::to_string(f[i]);
            if (s != eqSent) {
                if (!svcEq(s)) SDL_Log("HiBy R4: eq_set %s not accepted", s.c_str());
                changed = true;
            }
            std::lock_guard<std::mutex> g(mx);
            eqSent = s;
            vals["eq"] = s;   // (shown at once; the read-back confirms it)
            if (autoWant == autoNow) autoWant = -1;
            genV++;
        }
        if (doLed) svcLed(led.rgb, led.bright, led.breath);
        if (changed) {
            std::lock_guard<std::mutex> g(mx);
            readBackAt = SDL_GetTicks() + 500;
            if (!readBackAt) readBackAt = 1;
        }
        if (doGet) parse(svcGet());
    }
}

}  // namespace

void start() {
    std::lock_guard<std::mutex> g(mx);
    if (started) return;
    started = true;
    std::thread(work).detach();
}

Avail avail() { return Avail(availV.load()); }

std::string get(const char* key) {
    std::lock_guard<std::mutex> g(mx);
    const auto it = vals.find(key);
    return it == vals.end() ? std::string() : it->second;
}

int getInt(const char* key, int fallback) {
    const std::string v = get(key);
    if (v.empty()) return fallback;
    char* end = nullptr;
    const long n = strtol(v.c_str(), &end, 10);
    return end && end != v.c_str() ? int(n) : fallback;
}

unsigned gen() { return genV.load(); }

void set(const char* key, const std::string& value) {
    if (!on()) return;
    {
        std::lock_guard<std::mutex> g(mx);
        vals[key] = value;
        auto it = std::find_if(sets.begin(), sets.end(), [key](const auto& s) { return s.first == key; });
        if (it != sets.end()) it->second = value;
        else sets.emplace_back(key, value);
        genV++;
    }
    cv.notify_one();
}

void eqSet(const GfbgEq& e) {
    if (!on()) return;
    // on and the ten bands in whole dB; the worker puts them into the ROM's own value (its preamp fields kept)
    EqPart p;
    p.have = true;
    p.on = e.on ? 1 : 0;
    for (int i = 0; i < GFEQ_BANDS; i++) p.bands[i] = std::clamp(int(std::lround(e.bands[i])), -12, 12);
    {
        std::lock_guard<std::mutex> g(mx);
        if (eqPart.have && eqPart.on == p.on && std::equal(p.bands, p.bands + GFEQ_BANDS, eqPart.bands)) return;
        eqPart = p;
        eqDirty = true;
    }
    cv.notify_one();
}

bool autoPreamp() {
    std::lock_guard<std::mutex> g(mx);
    if (autoWant >= 0) return autoWant != 0;
    return eqFields(vals.count("eq") ? vals["eq"] : std::string())[1] != 0;
}

void setAutoPreamp(bool on) {
    if (!r4::on()) return;
    {
        std::lock_guard<std::mutex> g(mx);
        autoWant = on ? 1 : 0;
        eqDirty = true;
        genV++;
    }
    cv.notify_one();
}

void ledLive(int rgb, int bright, bool breath) {
    if (!on()) return;
    {
        std::lock_guard<std::mutex> g(mx);
        ledWant = { rgb & 0xffffff, std::clamp(bright, 0, 100), breath };
        ledPending = true;
    }
    cv.notify_one();
}

void refresh() {
    if (!on()) return;
    {
        std::lock_guard<std::mutex> g(mx);
        refreshWanted = true;
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

bool lineOut() {
    if (!on()) return false;
    const std::string st = get("st_output");
    return (st == "Headphone" && get("out35") == "lineout") || st == "BalanceLo";
}

}  // namespace r4

#else  // the Switch, the PC: there is no R4

namespace r4 {
void start() {}
Avail avail() { return Avail::No; }
std::string get(const char*) { return {}; }
int getInt(const char*, int fallback) { return fallback; }
unsigned gen() { return 0; }
void set(const char*, const std::string&) {}
void eqSet(const GfbgEq&) {}
bool autoPreamp() { return false; }
void setAutoPreamp(bool) {}
void ledLive(int, int, bool) {}
void refresh() {}
bool takeError(std::string&) { return false; }
bool lineOut() { return false; }
}  // namespace r4

#endif
