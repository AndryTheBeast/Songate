/* Gatefold NX preview · hostshim.cpp
 * libnx stand-ins, path mapping, keyboard input and the screenshot script.
 *
 * Environment:
 *   GF_ROOT    folder holding sdmc/ (the fake SD card)          default: current folder
 *   GF_ROMFS   the project's romfs/ folder                      default: ../../romfs
 *   GF_SEARCH  text the "keyboard" returns when it opens; "a|b" answers a, then b
 *   GF_SCRIPT  lines of "<frame> <action> [args]":
 *                press A|B|X|Y|L|R|ZL|ZR|UP|DOWN|LEFT|RIGHT|PLUS|MINUS
 *                hold <button> <frames>   tap <x> <y>   shot <file.bmp>   quit
 *                touch <x> <y> … release <x> <y>   (a finger held down: a long touch); move <x> <y> drags it
 *                wheel <x> <y> <notches>   (a mouse moved there, its wheel turned: + away from you, scrolls up)
 *                mouse <x> <y>   (the mouse moved there, nothing pressed)   click <x> <y>   (moved there, its left button pressed and let go)
 *                audit <label>   (the D-pad audit of that frame, appended to dpad-audit.txt: App, Ui::audit)
 *                stick <x> <y> <frames>   (the left stick, −1..1, y up)
 *   GF_JOBS    show the three background jobs (app covers, overlay covers, levelling) as running (shell.cpp)
 *   GF_BGCFG   show the background player's settings (Settings → Playback, a playlist's Games) as if it were connected
 *   GF_STREAM  <host:port> of tools/preview/live/server.py: frames, sound and input over the network (streamshim.cpp)
 *   GF_PROTECT an sdmc:/ folder nothing may write, rename or delete in (the live environment: the user's own music)
 *   GF_SHOW    show the window (keyboard: arrows, Enter=A, Backspace=B, Space=X,
 *              F=Y, Q/E=L/R, 1/2=ZL/ZR, Tab=Minus, Esc=Plus)
 */
// windows.h first: SDL_endian.h and clang's intrinsics disagree the other way round.
#include <windows.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>
#include <direct.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "include/dirent.h"
#include "include/switch.h"

void hostFilmEnd();   // film mode (below): the file finished at quit

// streamshim.cpp: the live test environment
bool streamStart();
void streamFrame(SDL_Renderer* r, long frame);
void streamPace();
void streamAudio(const Uint8* data, int len);
uint64_t streamButtons();
void streamStick(float& x, float& y);
bool streamQuit();
bool streamKeyboard(const char* header, const char* guide, const char* initial, char* out, size_t size);

namespace {

std::string env(const char* k, const char* d) {
    const char* v = getenv(k);
    return v ? v : d;
}

std::string mapPath(const char* p) {
    std::string s = p ? p : "";
    if (s.rfind("file:", 0) == 0) s.erase(0, 5);   // FFmpeg's file protocol prefix
    if (s.rfind("sdmc:/", 0) == 0) return env("GF_ROOT", ".") + "/sdmc/" + s.substr(6);
    if (s.rfind("romfs:/", 0) == 0) return env("GF_ROMFS", "../../romfs") + "/" + s.substr(7);
    return s;
}

// GF_PROTECT: a write, rename, removal or new folder in that folder fails (EACCES), as a read-only card would
bool isProtected(const char* p) {
    const char* g = getenv("GF_PROTECT");
    if (!g || !*g || !p) return false;
    std::string s = p, w = g;
    if (s.rfind("file:", 0) == 0) s.erase(0, 5);
    for (auto& c : s) if (c == '\\') c = '/';
    if (s.size() < w.size() || _strnicmp(s.c_str(), w.c_str(), w.size()) != 0) return false;
    if (s.size() > w.size() && s[w.size()] != '/') return false;
    errno = EACCES;
    fprintf(stderr, "protected: refused %s\n", p);
    return true;
}

struct Step {
    std::string action;
    std::vector<std::string> args;
};
std::multimap<long, Step> script;
bool scriptLoaded = false;
long frame = 0;
float backlight = 0.6f;
bool quitting = false;
std::map<std::string, long> holdUntil;
float stickX = 0, stickY = 0;   // the left stick, −1..1
long stickUntil = 0;

const std::map<std::string, u64> kButtons = {
    { "A", HidNpadButton_A },       { "B", HidNpadButton_B },         { "X", HidNpadButton_X },       { "Y", HidNpadButton_Y },
    { "L", HidNpadButton_L },       { "R", HidNpadButton_R },         { "ZL", HidNpadButton_ZL },     { "ZR", HidNpadButton_ZR },
    { "UP", HidNpadButton_Up },     { "DOWN", HidNpadButton_Down },   { "LEFT", HidNpadButton_Left }, { "RIGHT", HidNpadButton_Right },
    { "PLUS", HidNpadButton_Plus }, { "MINUS", HidNpadButton_Minus },
};

void loadScript() {
    scriptLoaded = true;
    const std::string path = env("GF_SCRIPT", "");
    if (path.empty()) return;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        long f;
        Step s;
        if (!(ls >> f >> s.action)) continue;
        std::string a;
        while (ls >> a) s.args.push_back(a);
        script.emplace(f, s);
    }
}

void pushFinger(Uint32 type, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.x = x / 1280.f;
    e.tfinger.y = y / 720.f;
    e.tfinger.fingerId = 1;
    SDL_PushEvent(&e);
}

}  // namespace

// ---------------------------------------------------------------- files

const char* hostPath(const char* p) {
    thread_local std::string s;
    s = mapPath(p);
    return s.c_str();
}

FILE* hostFopen(const char* p, const char* m) {
    if (m && (strchr(m, 'w') || strchr(m, 'a') || strchr(m, '+')) && isProtected(p)) return nullptr;
    return fopen(mapPath(p).c_str(), m);
}
int hostStat(const char* p, struct stat* s) { return stat(mapPath(p).c_str(), s); }
int hostMkdir(const char* p, int) { return isProtected(p) ? -1 : _mkdir(mapPath(p).c_str()); }
int hostRemove(const char* p) { return isProtected(p) ? -1 : remove(mapPath(p).c_str()); }
int hostRename(const char* a, const char* b) { return isProtected(a) || isProtected(b) ? -1 : rename(mapPath(a).c_str(), mapPath(b).c_str()); }
int hostRmdir(const char* p) { return isProtected(p) ? -1 : _rmdir(mapPath(p).c_str()); }
TTF_Font* hostOpenFont(const char* p, int size) { return TTF_OpenFont(mapPath(p).c_str(), size); }
Mix_Music* hostLoadMUS(const char* p) { return Mix_LoadMUS(mapPath(p).c_str()); }

struct DIR {
    HANDLE h;
    WIN32_FIND_DATAA fd;
    bool first;
    dirent ent;
};
DIR* opendir(const char* path) {
    DIR* d = new DIR{};
    d->h = FindFirstFileA((mapPath(path) + "/*").c_str(), &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { delete d; return nullptr; }
    d->first = true;
    return d;
}
dirent* readdir(DIR* d) {
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return nullptr;
    d->first = false;
    strncpy(d->ent.d_name, d->fd.cFileName, sizeof d->ent.d_name - 1);
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->ent;
}
int closedir(DIR* d) {
    FindClose(d->h);
    delete d;
    return 0;
}

// ---------------------------------------------------------------- input

void padConfigureInput(u32, u32) {}
void padInitializeDefault(PadState* p) { *p = PadState{}; }

void padUpdate(PadState* p) {
    if (!scriptLoaded) loadScript();
    p->old = p->buttons;
    u64 b = 0;
    if (getenv("GF_SHOW")) {
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        const std::pair<SDL_Scancode, u64> keys[] = {
            { SDL_SCANCODE_UP, HidNpadButton_Up },       { SDL_SCANCODE_DOWN, HidNpadButton_Down }, { SDL_SCANCODE_LEFT, HidNpadButton_Left },
            { SDL_SCANCODE_RIGHT, HidNpadButton_Right }, { SDL_SCANCODE_RETURN, HidNpadButton_A },  { SDL_SCANCODE_BACKSPACE, HidNpadButton_B },
            { SDL_SCANCODE_SPACE, HidNpadButton_X },     { SDL_SCANCODE_F, HidNpadButton_Y },       { SDL_SCANCODE_Q, HidNpadButton_L },
            { SDL_SCANCODE_E, HidNpadButton_R },         { SDL_SCANCODE_1, HidNpadButton_ZL },      { SDL_SCANCODE_2, HidNpadButton_ZR },
            { SDL_SCANCODE_TAB, HidNpadButton_Minus },   { SDL_SCANCODE_ESCAPE, HidNpadButton_Plus },
        };
        for (auto& [sc, bit] : keys) if (k[sc]) b |= bit;
    }
    if (streamStart()) b |= streamButtons();
    std::vector<Step> later;
    auto range = script.equal_range(frame);
    for (auto it = range.first; it != range.second; ++it) {
        const Step& s = it->second;
        if (s.action == "press" && !s.args.empty() && kButtons.count(s.args[0])) b |= kButtons.at(s.args[0]);
        else if (s.action == "hold" && s.args.size() >= 2) holdUntil[s.args[0]] = frame + atol(s.args[1].c_str());
        else if (s.action == "tap" && s.args.size() >= 2) {
            pushFinger(SDL_FINGERDOWN, float(atof(s.args[0].c_str())), float(atof(s.args[1].c_str())));
            later.push_back({ "release", s.args });
        } else if (s.action == "audit" && !s.args.empty()) {   // the D-pad audit of the next frame drawn (App, dpad-audit.txt)
            std::string label;
            for (const auto& a : s.args) label += (label.empty() ? "" : " ") + a;
            _putenv(("GF_AUDIT=" + label).c_str());
        } else if (s.action == "touch" && s.args.size() >= 2) {   // a finger down that stays (release x y lifts it)
            pushFinger(SDL_FINGERDOWN, float(atof(s.args[0].c_str())), float(atof(s.args[1].c_str())));
        } else if (s.action == "move" && s.args.size() >= 2) {   // the finger held down moves there (a drag)
            pushFinger(SDL_FINGERMOTION, float(atof(s.args[0].c_str())), float(atof(s.args[1].c_str())));
        } else if (s.action == "release" && s.args.size() >= 2) {
            pushFinger(SDL_FINGERUP, float(atof(s.args[0].c_str())), float(atof(s.args[1].c_str())));
        } else if ((s.action == "mouse" || s.action == "click") && s.args.size() >= 2) {   // the PC's mouse: moved, or a left click there
            SDL_Event e{};
            e.type = SDL_MOUSEMOTION;
            e.motion.x = atoi(s.args[0].c_str());
            e.motion.y = atoi(s.args[1].c_str());
            SDL_PushEvent(&e);
            if (s.action == "click") {
                for (Uint32 t : { Uint32(SDL_MOUSEBUTTONDOWN), Uint32(SDL_MOUSEBUTTONUP) }) {
                    SDL_Event b{};
                    b.type = t;
                    b.button.button = SDL_BUTTON_LEFT;
                    b.button.x = e.motion.x;
                    b.button.y = e.motion.y;
                    SDL_PushEvent(&b);
                }
            }
        } else if (s.action == "wheel" && s.args.size() >= 3) {   // a mouse there, its wheel turned n notches (+ away from you)
            SDL_Event e{};
            e.type = SDL_MOUSEMOTION;
            e.motion.x = atoi(s.args[0].c_str());
            e.motion.y = atoi(s.args[1].c_str());
            SDL_PushEvent(&e);
            SDL_Event w{};
            w.type = SDL_MOUSEWHEEL;
            w.wheel.preciseY = float(atof(s.args[2].c_str()));
            w.wheel.y = int(std::lround(w.wheel.preciseY));
            SDL_PushEvent(&w);
        } else if (s.action == "stick" && s.args.size() >= 3) {
            stickX = float(atof(s.args[0].c_str()));
            stickY = float(atof(s.args[1].c_str()));
            stickUntil = frame + atol(s.args[2].c_str());
        } else if (s.action == "quit") {
            quitting = true;
            hostFilmEnd();   // (the film's last frame is in: ffmpeg finishes the file)
        }
    }
    for (auto& s : later) script.emplace(frame + 2, s);
    for (auto& [name, until] : holdUntil) if (frame < until && kButtons.count(name)) b |= kButtons.at(name);
    p->buttons = b;
}

HidAnalogStickState padGetStickPos(const PadState*, u32 i) {
    if (i != 0) return { 0, 0 };
    float x = frame < stickUntil ? stickX : 0, y = frame < stickUntil ? stickY : 0;
    if (getenv("GF_SHOW")) {   // I/J/K/L: the left stick
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_J]) x = -1;
        if (k[SDL_SCANCODE_L]) x = 1;
        if (k[SDL_SCANCODE_I]) y = 1;
        if (k[SDL_SCANCODE_K]) y = -1;
    }
    streamStick(x, y);
    return { s32(x * 32767), s32(y * 32767) };
}
bool appletMainLoop() { return !quitting && !streamQuit(); }

Result swkbdShow(SwkbdConfig* c, char* out, size_t size) {
    // "a|b|c": the first time the keyboard opens it answers a, then b, then c (and c again after that)
    static int asked = 0;
    const std::string all = env("GF_SEARCH", "");
    if (all.empty() && streamStart())   // the live page asks the user, as the console's keyboard would
        return streamKeyboard(c->header, c->guide, c->initial, out, size) ? 0 : 1;
    if (all.empty()) return 1;
    std::string q = all;
    size_t at = 0;
    for (int i = 0; i < asked; i++) {
        const size_t bar = all.find('|', at);
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    q = all.substr(at, all.find('|', at) == std::string::npos ? std::string::npos : all.find('|', at) - at);
    asked++;
    snprintf(out, size, "%s", q.c_str());
    return 0;
}

// ---------------------------------------------------------------- film mode
// GF_FILM=<out>: the README's film, rendered offline (session 11; the user: "Really smooth animations and no stutters"). The app's
// clock steps exactly 1/60 s a frame however long a frame takes (App::frame), SDL's own audio device is paused, and each frame here
// pulls 1/60 s of sound through Songate's own music hook and post-mix into <out>.s16 (48 kHz stereo), and pipes the frame's pixels
// to ffmpeg (on the PATH, with libx264) as <out>.mp4. Picture and sound cannot drift: both are counted in frames.
namespace {
void (*appHook)(void*, Uint8*, int) = nullptr;
void* appHookArg = nullptr;
void (*appPostMix)(void*, Uint8*, int) = nullptr;
void* appPostArg = nullptr;
FILE* filmVideo = nullptr;
FILE* filmAudio = nullptr;
bool filmTried = false;
std::vector<uint8_t> filmPixels;

const char* filmOut() { const char* f = getenv("GF_FILM"); return f && *f ? f : nullptr; }

void filmOpen(int w, int h) {
    filmTried = true;
    const std::string out = filmOut();
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -framerate 60 -i - -c:v libx264 -preset slow "
                              "-crf 10 -pix_fmt yuv420p \"%s.mp4\"", w, h, out.c_str());
    filmVideo = _popen(cmd, "wb");
    filmAudio = ::fopen((out + ".s16").c_str(), "wb");
    printf("[film] %dx%d at 60 frames a second into %s.mp4 and %s.s16\n", w, h, out.c_str(), out.c_str());
    fflush(stdout);
}

/** One frame's sound: 800 frames at 48 kHz, as SDL_mixer's callback makes it (the music hook, then the post-mix). */
void filmSound() {
    static int16_t buf[800 * 2];
    memset(buf, 0, sizeof buf);
    SDL_LockAudio();
    if (appHook) appHook(appHookArg, reinterpret_cast<Uint8*>(buf), int(sizeof buf));
    if (appPostMix) appPostMix(appPostArg, reinterpret_cast<Uint8*>(buf), int(sizeof buf));
    SDL_UnlockAudio();
    if (filmAudio) fwrite(buf, 1, sizeof buf, filmAudio);
}

void filmFrame(SDL_Renderer* r) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(r, &w, &h);
    if (!filmTried) filmOpen(w, h);
    filmSound();
    if (!filmVideo) return;
    filmPixels.resize(size_t(w) * size_t(h) * 4);
    if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_ARGB8888, filmPixels.data(), w * 4) == 0) fwrite(filmPixels.data(), 1, filmPixels.size(), filmVideo);
}
}  // namespace

void hostFilmEnd() {
    if (filmVideo) { _pclose(filmVideo); filmVideo = nullptr; }
    if (filmAudio) { fclose(filmAudio); filmAudio = nullptr; }
}

// ---------------------------------------------------------------- frames

void hostPresent(SDL_Renderer* r) {
    if (filmOut()) filmFrame(r);
    auto range = script.equal_range(frame);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.action != "shot" || it->second.args.empty()) continue;
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(r, &w, &h);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s && SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, it->second.args[0].c_str());
        if (s) SDL_FreeSurface(s);
    }
    if (streamStart()) streamFrame(r, frame);
    SDL_RenderPresent(r);
    streamPace();
    frame++;
}

// ---- sound: the app's post-mix, then a copy of the final mix for the stream (the PC itself stays silent then)
namespace {
void postMixTap(void*, Uint8* s, int len) {
    if (appPostMix) appPostMix(appPostArg, s, len);
    if (streamStart()) {
        streamAudio(s, len);
        memset(s, 0, size_t(len));
    }
}
}  // namespace
void hostSetPostMix(void (*f)(void*, Uint8*, int), void* arg) {
    SDL_LockAudio();
    appPostMix = f;
    appPostArg = arg;
    SDL_UnlockAudio();
    if (!filmOut()) Mix_SetPostMix(postMixTap, nullptr);   // (in film mode only the film pulls it: filmSound)
}
void hostHookMusic(void (*f)(void*, Uint8*, int), void* arg) {
    SDL_LockAudio();
    appHook = f;
    appHookArg = arg;
    SDL_UnlockAudio();
    // In film mode SDL_mixer never has it: its own thread pulled the music too, and the film's sound ran 2.3 times fast (its first
    // proof, session 11); the film pulls it a frame at a time (filmSound)
    if (!filmOut()) Mix_HookMusic(f, arg);
}

// ---- battery, dock, sleep and backlight (switch.h)
Result psmGetChargerType(PsmChargerType* out) {
    *out = getenv("GF_CHARGING") ? PsmChargerType_EnoughPower : PsmChargerType_Unconnected;
    if (const char* m = getenv("GF_BATTERY_MOCK")) {
        const std::string k = m;
        *out = k == "slow" ? PsmChargerType_LowPower : k == "normal" || k == "fast" || k == "full" ? PsmChargerType_EnoughPower : PsmChargerType_Unconnected;
    }
    return 0;
}
Result psmGetRawBatteryChargePercentage(double* out) {
    if (const char* m = getenv("GF_BATTERY_MOCK")) {
        const std::string k = m;
        *out = k == "low" ? 12.4 : k == "full" ? 100 : k == "slow" ? 41.2 : k == "normal" ? 58.7 : k == "fast" ? 63.1 : 76;
        return 0;
    }
    const double minutes = frame / 60.0 / 60.0;
    const double v = getenv("GF_CHARGING") ? 40 + minutes : 76 - minutes * 0.5;   // windows.h's min/max macros: no std::min here
    *out = v > 100 ? 100 : v < 1 ? 1 : v;
    return 0;
}
AppletOperationMode appletGetOperationMode() { return getenv("GF_DOCKED") ? AppletOperationMode_Console : AppletOperationMode_Handheld; }
Result appletRequestToSleep() {
    printf("[frame %ld] the console goes to sleep\n", frame);
    fflush(stdout);
    return 0;
}
Result lblGetCurrentBrightnessSetting(float* out) { *out = backlight; return 0; }
Result lblSetCurrentBrightnessSetting(float b) {
    backlight = b;
    printf("[frame %ld] backlight %.2f\n", frame, double(b));
    fflush(stdout);
    return 0;
}
