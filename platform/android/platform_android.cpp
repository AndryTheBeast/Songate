/*
 * Songate · platform_android.cpp
 * The platform on Android: SDL and the NDK, and SongateActivity (android/) for what only Java can do. Music lives on the
 * shared volumes under /storage (the phone's own, an SD card), the settings in the app's own storage; the app's own
 * vocabulary is still the Joy-Con's buttons (platform.hpp): back is B, a keyboard and a pad read as on the PC.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "platform.hpp"

namespace plat {

namespace {

LeaveFn leaveFn = nullptr;
void* leaveArg = nullptr;
SDL_GameController* padDev = nullptr;
uint64_t heldNow = 0, heldBefore = 0;
bool padLast = false;   // the last thing pressed was on a pad: key caps show its labels
std::atomic<bool> backHeld{ false };  // Android's back, as B
std::atomic<bool> backPressed{ false };   // … pressed since a frame last read it: a press and its release can come in one frame (both set on Android's thread)

/** A static method of SongateActivity returning a String, or "" when it is not there. */
std::string javaString(const char* method) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return {};
    jclass cls = env->FindClass("io/github/andrythebeast/songate/SongateActivity");
    if (!cls) { env->ExceptionClear(); return {}; }
    jmethodID m = env->GetStaticMethodID(cls, method, "()Ljava/lang/String;");
    std::string out;
    if (m) {
        if (auto s = static_cast<jstring>(env->CallStaticObjectMethod(cls, m))) {
            if (const char* c = env->GetStringUTFChars(s, nullptr)) {
                out = c;
                env->ReleaseStringUTFChars(s, c);
            }
            env->DeleteLocalRef(s);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    return out;
}

std::string property(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(name, v);
    return v;
}

bool isDirectory(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/** The shared volumes, as Android lists them (an app may not list /storage itself): the phone's own (/storage/emulated/0)
 *  first, then each card (/storage/XXXX-XXXX). */
std::vector<std::string> volumes() {
    std::vector<std::string> out;
    const std::string list = javaString("volumes");   // one a line
    for (size_t at = 0; at < list.size();) {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        const std::string v = list.substr(at, end - at);
        if (!v.empty() && isDirectory(v)) out.push_back(v);
        at = end + 1;
    }
    if (out.empty() && isDirectory("/storage/emulated/0")) out.push_back("/storage/emulated/0");
    return out;
}

// Leaving: Android says so on its own UI thread (SongateActivity.onPause, before SDL hears of it), while a frame may be running
// on the app's. The app's own thread does the work (mainLoop, at the top of the next frame) while Android's waits for it, a second
// at most: after that, the app may be ended without another word.
std::mutex leaveMx;
std::condition_variable leaveCv;
bool leavePending = false;

// A turn crashed or froze the app (the user, 2026-11-05: "sometimes the app starts flickering and then crashed when switching from
// landscape to portrait"; the crash: SIGSEGV in SDL_SetRenderTarget → FlushRenderCommands → GLES2 SetCopyState, a queued copy of a
// texture with no GL data). Android tells SDL the new size on its own UI thread (onNativeResize, Android_SendResize), and SDL's
// renderer watches window events right there: on a size change it sets the render target back and queues a new viewport, flushing
// the draw queue, while the app's thread is drawing into it (the frame is drawn into a texture, so a target is nearly always set).
// The two threads tore the queue: a crash, a freeze, the flicker. So a size change from any other thread is held here, before any
// watcher sees it, and sent again from the app's thread between frames (mainLoop), where SDL's renderer updates itself safely.
SDL_threadID appThread = 0;
SDL_Window* appWindow = nullptr;   // (windowReady)
std::atomic<bool> resizeHeld{ false };

int resizeFilter(void*, SDL_Event* e) {
    if (e->type == SDL_WINDOWEVENT && (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_RESIZED ||
                                       e->window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) &&
        appThread && SDL_ThreadID() != appThread) {
        resizeHeld = true;
        return 0;   // dropped: no watcher runs on this thread, nothing is queued
    }
    return 1;
}

int backWatch(void*, SDL_Event* e) {
    if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) && e->key.keysym.scancode == SDL_SCANCODE_AC_BACK)
        { backHeld = e->type == SDL_KEYDOWN; if (backHeld) backPressed = true; }
    return 0;
}

void openPad() {
    if (padDev) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (padDev = SDL_GameControllerOpen(i))) return;
}

}  // namespace

// ---- the app's life

void init() {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");   // upright: its own layout (view_phone.cpp)
#ifdef GF_DEV
    // portrait mock-ups (view_portrait.cpp): the screen upright while portrait-mock.txt is in the settings folder
    if (FILE* f = fopen((std::string(SDL_AndroidGetInternalStoragePath()) + "/Songate/portrait-mock.txt").c_str(), "r")) {
        fclose(f);
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait");
    }
    // test scripts (tools/android/devtest.py): with test-items in the settings folder, what is on screen is written to items.txt,
    // as GF_ITEMS does on the PC (App::frame), and the toasts to stderr
    {
        const std::string dir = std::string(SDL_AndroidGetInternalStoragePath()) + "/Songate/";
        if (FILE* f = fopen((dir + "test-items").c_str(), "r")) {
            fclose(f);
            setenv("GF_ITEMS", (dir + "items.txt").c_str(), 1);
        }
    }
#endif
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");                   // back reaches the app, as B
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");   // fingers are read as fingers (App::input)
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    // Out of sight the music goes on: SDL neither stops the app's thread nor its sound; nothing is drawn meanwhile (App::frame,
    // minimised), and SongateService keeps the app alive with its music controls
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO, "0");
    SDL_AddEventWatch(backWatch, nullptr);
    appThread = SDL_ThreadID();   // (init runs on the app's own thread, SDL_main's)
    SDL_SetEventFilter(resizeFilter, nullptr);
}

void quit() {
    if (padDev) SDL_GameControllerClose(padDev);
    padDev = nullptr;
}

/** Between frames, on the app's thread: a leave Android announced (leaveWatch) is done here, the settings and the caches written. */
bool mainLoop() {
    // a size change Android announced on its own thread (resizeFilter): now, on the app's, before the frame
    if (resizeHeld.exchange(false)) {
        if (SDL_Window* w = appWindow) {
            SDL_Event e{};
            e.type = SDL_WINDOWEVENT;
            e.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
            e.window.windowID = SDL_GetWindowID(w);
            SDL_GetWindowSize(w, &e.window.data1, &e.window.data2);
            SDL_PushEvent(&e);   // SDL's renderer resets its viewport from here; App::input lays the app out (fitWindow)
        } else {
            resizeHeld = true;   // no window to speak of yet: the next frame
        }
    }
    bool pending;
    LeaveFn fn;
    void* arg;
    { std::lock_guard<std::mutex> g(leaveMx); pending = leavePending; fn = leaveFn; arg = leaveArg; }
    if (pending) {
        if (fn) fn(false, arg);
        { std::lock_guard<std::mutex> g(leaveMx); leavePending = false; }
        leaveCv.notify_all();
    }
    return true;
}
void holdExit() {}
void releaseExit() {}

void watchLeave(LeaveFn fn, void* arg) {
    std::lock_guard<std::mutex> g(leaveMx);
    leaveFn = fn;
    leaveArg = arg;
}
void unwatchLeave() {
    std::lock_guard<std::mutex> g(leaveMx);
    leaveFn = nullptr;
    leavePending = false;
    leaveCv.notify_all();
}

bool inFocus() { return SDL_GetKeyboardFocus() != nullptr; }

std::string selfPath(int, char**) { return {}; }
bool swapsSelf() { return false; }   // Android's installer replaces the app
bool relaunch(const std::string&) { return false; }
bool legacyData() { return false; }

/** SongateAND-<version>.apk. */
bool isInstaller(const std::string& name) {
    const size_t cut = name.find_last_of('/');
    const std::string n = cut == std::string::npos ? name : name.substr(cut + 1);
    return n.rfind("SongateAND-", 0) == 0 && n.size() > 15 && n.compare(n.size() - 4, 4, ".apk") == 0;
}

// ---- input

void padInit() {
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    openPad();
}

void padRead() {
    heldBefore = heldNow;
    uint64_t b = 0;
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    const SDL_Keymod mod = SDL_GetModState();
    if (backHeld || backPressed) b |= BtnB;
    backPressed = false;
    if (mod & KMOD_CTRL) {
        if (k[SDL_SCANCODE_LEFT]) b |= BtnL;
        if (k[SDL_SCANCODE_RIGHT]) b |= BtnR;
        if (k[SDL_SCANCODE_F]) b |= BtnY;
    } else if (mod & KMOD_SHIFT) {
        if (k[SDL_SCANCODE_LEFT]) b |= BtnZL;
        if (k[SDL_SCANCODE_RIGHT]) b |= BtnZR;
    } else if (!SDL_IsTextInputActive()) {   // a hardware keyboard, as the PC's; not while typing into the text card
        if (k[SDL_SCANCODE_LEFT]) b |= BtnLeft;
        if (k[SDL_SCANCODE_RIGHT]) b |= BtnRight;
        if (k[SDL_SCANCODE_UP]) b |= BtnUp;
        if (k[SDL_SCANCODE_DOWN]) b |= BtnDown;
        if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) b |= BtnA;
        if (k[SDL_SCANCODE_ESCAPE]) b |= BtnB;
        if (k[SDL_SCANCODE_SPACE]) b |= BtnX;
        if (k[SDL_SCANCODE_APPLICATION]) b |= BtnPlus;
        if (k[SDL_SCANCODE_C]) b |= BtnMinus;
    }
    const uint64_t fromKeys = b;
    if (!padDev) openPad();
    if (padDev && !SDL_GameControllerGetAttached(padDev)) {
        SDL_GameControllerClose(padDev);
        padDev = nullptr;
    }
    if (padDev) {
        const std::pair<SDL_GameControllerButton, Button> map[] = {
            { SDL_CONTROLLER_BUTTON_A, BtnA },           { SDL_CONTROLLER_BUTTON_B, BtnB },           { SDL_CONTROLLER_BUTTON_X, BtnX },
            { SDL_CONTROLLER_BUTTON_Y, BtnY },           { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BtnL }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BtnR },
            { SDL_CONTROLLER_BUTTON_START, BtnPlus },    { SDL_CONTROLLER_BUTTON_BACK, BtnMinus },    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, BtnLeft },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, BtnUp },    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BtnRight }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, BtnDown },
        };
        for (const auto& [from, to] : map) if (SDL_GameControllerGetButton(padDev, from)) b |= to;
        if (SDL_GameControllerGetAxis(padDev, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) b |= BtnZL;
        if (SDL_GameControllerGetAxis(padDev, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) b |= BtnZR;
    }
    if (fromKeys) padLast = false;
    else if (b) padLast = true;
    heldNow = b;
}

uint64_t padDown() { return heldNow & ~heldBefore; }
uint64_t padHeld() { return heldNow; }

Stick stick(int which) {
    if (!padDev) return { 0, 0 };
    const int x = SDL_GameControllerGetAxis(padDev, which ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX);
    const int y = SDL_GameControllerGetAxis(padDev, which ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY);
    return { std::clamp(x, -32767, 32767), std::clamp(-y, -32767, 32767) };
}

// ---- typing

namespace {
TextFn textCard = nullptr;
}  // namespace

void setTextCard(TextFn fn) { textCard = fn; }
bool padWasLast() { return padLast; }

std::string capName(const std::string& b) {
    static const std::pair<const char*, const char*> keys[] = { { "A", "Enter" }, { "B", "Back" }, { "X", "Space" }, { "Y", "Ctrl F" }, { "L", "Ctrl ←" },
                                                                { "R", "Ctrl →" }, { "ZL", "Shift ←" }, { "ZR", "Shift →" }, { "+", "Menu" }, { "-", "C" } };
    static const std::pair<const char*, const char*> pad[] = { { "L", "LB" }, { "R", "RB" }, { "ZL", "LT" }, { "ZR", "RT" }, { "+", "Menu" }, { "-", "View" } };
    if (padLast) {
        for (const auto& [from, to] : pad) if (b == from) return to;
        return b;
    }
    if (b.rfind("DPAD", 0) == 0) return "Arrows";
    if (b == "BTN-UP") return "↑";
    if (b == "BTN-DOWN") return "↓";
    if (b == "BTN-LEFT") return "←";
    if (b == "BTN-RIGHT") return "→";
    for (const auto& [from, to] : keys) if (b == from) return to;
    return b;
}

TextResult askText(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    return textCard ? textCard(header, guide, initial, out, size) : TextResult::Unavailable;
}

void showTouchKeyboard(bool) {}   // SDL_StartTextInput (the text card) brings Android's own keyboard up

void windowReady(SDL_Window* w) { appWindow = w; }   // (mainLoop sends a held size change for it)
void trayIcon(bool) {}
void titleColours(uint32_t, uint32_t) {}

// ---- Android's music controls (SongateService: the notification, the lock screen, headsets): what plays goes there, their presses
// come back as commands, oldest first

namespace {
std::mutex cmdMx;
std::deque<std::pair<Cmd, double>> cmds;
double seekTo = 0;

/** SongateService, held: the app's thread finds it once (a thread Java started: the app's classes are seen). */
jclass service(JNIEnv* env) {
    static jclass cls = nullptr;
    if (!cls && env) {
        if (jclass local = env->FindClass("io/github/andrythebeast/songate/SongateService")) {
            cls = static_cast<jclass>(env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
        } else {
            env->ExceptionClear();
        }
    }
    return cls;
}

/** Text as UTF-8 bytes: NewStringUTF wants Java's own kind of UTF-8, which a song's emoji is not. */
jbyteArray bytes(JNIEnv* env, const std::string& s) {
    jbyteArray a = env->NewByteArray(jsize(s.size()));
    if (a && !s.empty()) env->SetByteArrayRegion(a, 0, jsize(s.size()), reinterpret_cast<const jbyte*>(s.data()));
    return a;
}

void done(JNIEnv* env) {
    if (env->ExceptionCheck()) env->ExceptionClear();
}
}  // namespace

Cmd nextCommand() {
    std::lock_guard<std::mutex> g(cmdMx);
    if (cmds.empty()) return Cmd::None;
    const auto [c, arg] = cmds.front();
    cmds.pop_front();
    if (c == Cmd::Seek) seekTo = arg;
    return c;
}

double commandSeek() { return seekTo; }

/** The cover as its RGBA pixels (up to 1024 px, TrackLoader), as Android's bitmaps hold them: nothing lost to a JPEG. */
void mediaInfo(const std::string& title, const std::string& artist, const std::string& album, const uint8_t* rgba, int w, int h,
               const std::vector<uint8_t>&) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = service(env);
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "mediaInfo", "([B[B[B[BII)V")) {
        jbyteArray t = bytes(env, title), ar = bytes(env, artist), al = bytes(env, album);
        const jsize n = rgba && w > 0 && h > 0 ? jsize(size_t(w) * size_t(h) * 4) : 0;
        jbyteArray cover = env->NewByteArray(n);
        if (cover && n > 0) env->SetByteArrayRegion(cover, 0, n, reinterpret_cast<const jbyte*>(rgba));
        env->CallStaticVoidMethod(cls, m, t, ar, al, cover, jint(n > 0 ? w : 0), jint(n > 0 ? h : 0));
        for (jbyteArray a : { t, ar, al, cover }) if (a) env->DeleteLocalRef(a);
    }
    done(env);
}

void keepOnSwipe(bool on) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = service(env);
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "setKeepOnSwipe", "(Z)V")) env->CallStaticVoidMethod(cls, m, jboolean(on));
    done(env);
}

void mediaState(bool hasTrack, bool playing) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = service(env);
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "mediaState", "(ZZ)V")) env->CallStaticVoidMethod(cls, m, jboolean(hasTrack), jboolean(playing));
    done(env);
}

void mediaProgress(double position, double duration, bool liked, int order) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = service(env);
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "mediaProgress", "(JJZI)V"))
        env->CallStaticVoidMethod(cls, m, jlong(position * 1000), jlong(duration * 1000), jboolean(liked), jint(order));
    done(env);
}
void openUrl(const std::string& url) { SDL_OpenURL(url.c_str()); }

/** A card's Music first (where the user keeps it), then the phone's own. */
std::string musicFolder() {
    std::vector<std::string> v = volumes();
    std::rotate(v.begin(), v.begin() + (v.empty() ? 0 : 1), v.end());
    for (const std::string& vol : v)
        if (DIR* d = opendir(vol.c_str())) {
            std::string found;
            while (dirent* e = readdir(d)) if (!strcasecmp(e->d_name, "music")) { found = vol + "/" + e->d_name; break; }
            closedir(d);
            if (!found.empty() && isDirectory(found)) return found;
        }
    return {};
}

// ---- the machine

void setMediaPlaying(bool) {}   // the media session's service keeps it playing (the next milestone)
void setKeepAwake(bool on) { on ? SDL_DisableScreenSaver() : SDL_EnableScreenSaver(); }   // the window's keep-screen-on
void requestSleep() {}          // an app cannot put a phone to sleep
bool dimBacklight(float&) { return false; }   // a veil for now
void restoreBacklight(float) {}
bool volumeKeys(bool& up, bool& down, std::string* err) { up = down = false; if (err) *err = "the Switch only"; return false; }
bool backlightOff(bool) { return false; }
bool blockHome(bool) { return false; }
bool takeCapture(bool) { return false; }
int capturePresses() { return 0; }
int disconnectControllers() { return 0; }
bool outputVolume(int&) { return false; }
bool setOutputVolume(int) { return false; }

bool batteryRaw(double& percent, bool& charging) {
    int pct = -1;
    const SDL_PowerState s = SDL_GetPowerInfo(nullptr, &pct);
    if (s == SDL_POWERSTATE_UNKNOWN || s == SDL_POWERSTATE_NO_BATTERY || pct < 0) return false;
    percent = pct;
    charging = s == SDL_POWERSTATE_CHARGING || s == SDL_POWERSTATE_CHARGED;
    return true;
}

bool batteryInfo(BatteryInfo&) { return false; }   // (the Switch's for now: BETA 3)

bool batteryLevel(unsigned& percent) {
    double p = 0;
    bool c = false;
    if (!batteryRaw(p, c)) return false;
    percent = unsigned(p);
    return true;
}

void sysInfo(std::string& firmware, std::string& atmosphere, bool& docked) {
    firmware = "Android " + property("ro.build.version.release") + " (API " + property("ro.build.version.sdk") + ")";
    atmosphere.clear();
    docked = false;
}

long long bootStamp() {
    timespec t{};
    clock_gettime(CLOCK_BOOTTIME, &t);
    return (long long)time(nullptr) - (long long)t.tv_sec;
}

/** Android places threads itself: only the order of importance is kept, as on the PC. */
void placeThread(int, int priority) {
    int nice = 0;
    if (priority <= 0x2B) nice = -16;        // audio and its read-ahead (the audio priority an app may take)
    else if (priority >= 0x30) nice = 10;    // covers, levelling, downloads
    setpriority(PRIO_PROCESS, gettid(), nice);
}

bool netStart() { return true; }
void netStop() {}

std::vector<Net> networks() {
    std::vector<Net> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        const uint32_t ip = reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr;
        const uint32_t mask = a->ifa_netmask ? reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr : 0xffffffffu;
        out.push_back({ ip, ip | ~mask });
    }
    freeifaddrs(list);
    return out;
}

bool storageAllowed() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return false;
    jclass cls = env->FindClass("io/github/andrythebeast/songate/SongateActivity");
    if (!cls) { env->ExceptionClear(); return false; }
    jboolean ok = JNI_FALSE;
    if (jmethodID m = env->GetStaticMethodID(cls, "hasAllFiles", "()Z")) ok = env->CallStaticBooleanMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = JNI_FALSE; }
    env->DeleteLocalRef(cls);
    return ok;
}

void askStorage() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jclass cls = env->FindClass("io/github/andrythebeast/songate/SongateActivity");
    if (!cls) { env->ExceptionClear(); return; }
    if (jmethodID m = env->GetStaticMethodID(cls, "askAllFiles", "()V")) env->CallStaticVoidMethod(cls, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
}

std::string deviceName() {
    // (the user, 2026-11-05: sync showed "M2101K9G" while AirPlay said "Mi 11 Lite": Connect asked from its own thread, where
    // SongateActivity cannot be found, and got the model; App asks first, on its thread, and that answer is kept)
    static std::mutex mx;
    static std::string kept;
    std::lock_guard<std::mutex> g(mx);
    if (!kept.empty()) return kept;
    std::string n = javaString("deviceName");
    if (!n.empty()) { kept = n; return n; }
    n = property("ro.product.model");
    return n.empty() ? std::string("Android") : n;
}

int64_t freeBytes(const std::string& path) {
    struct statvfs s;
    if (statvfs(path.c_str(), &s) != 0) return -1;
    return int64_t(s.f_bavail) * int64_t(s.f_frsize);
}

bool appletMode() { return false; }

bool randomBytes(uint8_t* out, size_t n) {
    while (n > 0) {
        const ssize_t got = getrandom(out, n, 0);
        if (got <= 0) return false;
        out += got;
        n -= size_t(got);
    }
    return true;
}

// ---- the background player: the Switch's

int bgProtocol() { return 0; }
bool bgRestart(std::string& err) { err = "There is no background player on Android"; return false; }
bool bgEnd() { return false; }
void bgStart(bool) {}

// ---- files

namespace {
/** The app's own storage (Context.getFilesDir), worked out without Java: static settings ask for it as the library loads,
 *  before SDL's side of Java is there. /data/user/<user>/<package>/files; the process is named after the package. */
const std::string& filesDir() {
    static const std::string p = [] {
        char name[256] = {};
        if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
            fread(name, 1, sizeof name - 1, f);
            fclose(f);
        }
        std::string pkg = name;
        if (const size_t colon = pkg.find(':'); colon != std::string::npos) pkg.resize(colon);   // a ":service" process
        return "/data/user/" + std::to_string(getuid() / 100000) + "/" + pkg + "/files";
    }();
    return p;
}
}  // namespace

/** SongateActivity copies the APK's romfs into the app's own storage as a version starts the first time: fopen reads files. */
std::string assetPath(const char* relative) { return filesDir() + "/romfs/" + relative; }

std::vector<FontData> systemFonts() {
    // Mapped, not read: the CJK one is tens of megabytes, and FreeType touches only the glyphs it uses
    std::vector<FontData> out;
    for (const char* name : { "Roboto-Regular.ttf", "NotoSansSymbols-Regular-Subsetted.ttf", "NotoSansCJK-Regular.ttc", "NotoSansArabic-Regular.ttf",
                              "NotoSansHebrew-Regular.ttf", "NotoSansThai-Regular.ttf", "NotoSansDevanagari-Regular.otf" }) {
        const std::string p = std::string("/system/fonts/") + name;
        const int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st;
        void* at = fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
        close(fd);
        if (at != MAP_FAILED) out.push_back({ at, size_t(st.st_size) });
    }
    return out;
}

const char* configParent() {
    return filesDir().c_str();
}
const char* configDir() {
    static const std::string p = std::string(configParent()) + "/Songate";
    return p.c_str();
}
void useConfigDir(const char*, const char*) {}
const char* backupDir() {
    static const std::string p = std::string(configParent()) + "/Songate-backups";
    return p.c_str();
}

const char* root() { return "/storage"; }
const char* rootName() { return "This device"; }
bool isRoot(const std::string& path) { return path.empty() || path == "/storage" || path == "/storage/" || path == "/"; }

/** "/storage/emulated/0/" or "/storage/XXXX-XXXX/": a volume's top, which a playlist written elsewhere leaves out. */
size_t rootLen(const std::string& path) {
    if (path.compare(0, 20, "/storage/emulated/0/") == 0) return 20;
    if (path.compare(0, 9, "/storage/") != 0) return 0;
    const size_t s = path.find('/', 9);
    return s == std::string::npos ? 0 : s + 1;
}

std::string parentDir(const std::string& path) {
    if (path == "/storage/emulated/0") return "/storage";   // the phone's own volume sits one level deeper than a card
    const size_t s = path.find_last_of('/');
    return s == std::string::npos || s <= 8 ? std::string("/storage") : path.substr(0, s);
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir == "/storage" && name == "emulated") return "/storage/emulated/0";
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

/** "/a/b" from a playlist written elsewhere: the first volume that has it. */
std::string fromRoot(const std::string& path) {
    const std::vector<std::string> v = volumes();
    for (const std::string& vol : v) {
        struct stat st;
        if (stat((vol + path).c_str(), &st) == 0) return vol + path;
    }
    return (v.empty() ? std::string("/storage/emulated/0") : v.front()) + path;
}

const std::vector<const char*>& systemFolders() {
    static const std::vector<const char*> v = { "/storage/emulated/0/Android" };
    return v;
}

namespace {
std::mutex edgeMx;
std::vector<EdgeBox> edgeBoxes;
float edgeRadius = 0;
int edgeW = 0, edgeH = 0;   // the window size Android measured them for
std::atomic<unsigned> edgeGen{ 0 };
}  // namespace

unsigned screenEdges(std::vector<EdgeBox>& boxes, float& cornerRadius, int& width, int& height) {
    std::lock_guard<std::mutex> g(edgeMx);
    boxes = edgeBoxes;
    cornerRadius = edgeRadius;
    width = edgeW;
    height = edgeH;
    return edgeGen.load();
}

std::vector<std::string> rootFolders() {
    std::vector<std::string> out;
    for (const std::string& v : volumes()) out.push_back(v == "/storage/emulated/0" ? std::string("emulated") : v.substr(9));   // join() takes "emulated" to /storage/emulated/0
    return out;
}

// Volume names in Songate's words, the same on every phone (the user chose them, 2026-11-04: "A and 2"): Internal storage,
// SD card, SD card 2 (a USB drive is one of these too: Android words them by maker, which the user did not choose).
std::string volumeLabel(const std::string& path) {
    static std::vector<std::string> paths, names;
    static Uint32 at = 0;
    if (paths.empty() || SDL_GetTicks() - at > 3000) {   // cards come and go; asking Java every frame is not needed
        at = SDL_GetTicks();
        paths = volumes();
        names.assign(paths.size(), std::string());
        const std::string list = javaString("volumesRemovable");   // "1" or "0" a line, in volumes()'s order
        int cards = 0;
        for (size_t i = 0; i < paths.size(); i++) {
            const bool removable = 2 * i < list.size() ? list[2 * i] == '1' : i > 0;
            names[i] = !removable ? std::string("Internal storage") : ++cards == 1 ? std::string("SD card") : "SD card " + std::to_string(cards);
        }
    }
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (p == "/storage/emulated") p = "/storage/emulated/0";
    for (size_t i = 0; i < paths.size(); i++)
        if (paths[i] == p) return names[i];
    return {};
}

bool measureFiles(const std::string&, int&, uint64_t&, int) { return false; }
bool measureStorage(const std::string&, int64_t&, int&, bool (*)(const std::string&)) { return false; }

}  // namespace plat

/** SongateService, on Android's UI thread: a press on the music controls (the codes are Cmd's), arg the place for a seek. */
extern "C" JNIEXPORT void JNICALL Java_io_github_andrythebeast_songate_SongateService_nativeCommand(JNIEnv*, jclass, jint code, jdouble arg) {
    if (code <= int(plat::Cmd::None) || code > int(plat::Cmd::Seek)) return;
    std::lock_guard<std::mutex> g(plat::cmdMx);
    plat::cmds.emplace_back(plat::Cmd(code), double(arg));
}

/** SongateActivity.onPause, on Android's UI thread: the app's thread does its leave (plat::mainLoop) while this waits, a second at most. */
extern "C" JNIEXPORT void JNICALL Java_io_github_andrythebeast_songate_SongateActivity_nativeLeave(JNIEnv*, jclass) {
    std::unique_lock<std::mutex> g(plat::leaveMx);
    if (!plat::leaveFn) return;   // not started yet, or ending
    plat::leavePending = true;
    plat::leaveCv.wait_for(g, std::chrono::seconds(1), [] { return !plat::leavePending; });
}

/** SongateActivity.reportEdges, on Android's UI thread each time the window is laid out. */
extern "C" JNIEXPORT void JNICALL Java_io_github_andrythebeast_songate_SongateActivity_nativeEdges(JNIEnv* env, jclass, jfloatArray boxes, jfloat radius, jint width, jint height) {
    std::vector<plat::EdgeBox> v;
    const jsize n = boxes ? env->GetArrayLength(boxes) : 0;
    if (n >= 4) {
        std::vector<jfloat> f(static_cast<size_t>(n));
        env->GetFloatArrayRegion(boxes, 0, n, f.data());
        for (jsize i = 0; i + 3 < n; i += 4) v.push_back({ f[size_t(i)], f[size_t(i + 1)], f[size_t(i + 2)], f[size_t(i + 3)] });
    }
    std::lock_guard<std::mutex> g(plat::edgeMx);
    plat::edgeBoxes = std::move(v);
    plat::edgeRadius = radius;
    plat::edgeW = width;
    plat::edgeH = height;
    plat::edgeGen++;
}
