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
#include <map>
#include <mutex>
#include <thread>
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

/** SongateActivity's class, found once on the app's own thread (plat::init) and kept: a thread the app starts itself (Files'
 *  lister, since BETA 3) has only the system's class loader, where FindClass does not see the app's classes. That lister then
 *  got no volumes at all and Files showed Internal storage alone, the SD card gone (found by devtest.py, 2026-11-11). */
jclass activityClass(JNIEnv* env) {
    static jclass cls = nullptr;
    if (!cls && env) {
        if (jclass local = env->FindClass("io/github/andrythebeast/songate/SongateActivity")) {
            cls = static_cast<jclass>(env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
        } else {
            env->ExceptionClear();
        }
    }
    return cls;
}

/** A static method of SongateActivity returning a String, or "" when it is not there. */
std::string javaString(const char* method) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return {};
    jclass cls = activityClass(env);
    if (!cls) return {};
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


}  // namespace

// ---- the app's life

void init() {
    activityClass(static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv()));   // found here, on the app's own thread, for every thread after
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");   // upright: its own layout (view_phone.cpp)
#ifdef GF_DEV
    // test scripts (tools/android/devtest.py): with test-items in the settings folder, what is on screen is written to items.txt,
    // as GF_ITEMS does on the PC (App::frame), and the toasts to stderr
    {
        const std::string dir = std::string(SDL_AndroidGetInternalStoragePath()) + "/Songate/";
        if (FILE* f = fopen((dir + "test-items").c_str(), "r")) {
            fclose(f);
            setenv("GF_ITEMS", (dir + "items.txt").c_str(), 1);
        }
        // test-nofold: Settings shows every card open, as on the Switch and the PC (scripts older than the folds find their rows;
        // tools/android/devfold.py tests the folds themselves without it)
        if (FILE* f = fopen((dir + "test-nofold").c_str(), "r")) { fclose(f); setenv("GF_NOFOLD", "1", 1); }
        // test-nonotice: the first start's questions (covers, lyrics) never come (a script's taps are not answers to them)
        if (FILE* f = fopen((dir + "test-nonotice").c_str(), "r")) { fclose(f); setenv("GF_NONOTICE", "1", 1); }
        // the preview's aids for what cannot be made to happen on demand: test-update (a version, as GF_FAKEUPDATE: a release that
        // is not out, so the banner, its card and its row can be looked at) and test-jobs (as GF_JOBS: the work's cards)
        if (FILE* f = fopen((dir + "test-update").c_str(), "r")) {
            char v[64] = {};
            if (fgets(v, sizeof v, f)) { v[strcspn(v, "\r\n")] = 0; if (*v) setenv("GF_FAKEUPDATE", v, 1); }
            fclose(f);
        }
        if (FILE* f = fopen((dir + "test-jobs").c_str(), "r")) {
            fclose(f);
            setenv("GF_JOBS", "1", 1);
        }
        // test-tour: the tour's check (GF_TOURCHECK, tour.cpp) at the level written in it (0, 1 or 2): every step walked, logged
        if (FILE* f = fopen((dir + "test-tour").c_str(), "r")) {
            char v[8] = {};
            if (fgets(v, sizeof v, f)) { v[strcspn(v, "\r\n")] = 0; if (*v) setenv("GF_TOURCHECK", v, 1); }
            fclose(f);
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

// (the system ends the app itself: nothing to cut short here)
bool exitNow() { return false; }

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
bool legacyData() { return false; }

/** SongateAND-<version>.apk. */
bool isInstaller(const std::string& name) {
    const size_t cut = name.find_last_of('/');
    const std::string n = cut == std::string::npos ? name : name.substr(cut + 1);
    return n.rfind("SongateAND-", 0) == 0 && n.size() > 15 && n.compare(n.size() - 4, 4, ".apk") == 0;
}

/** An update's APK: Android's own installer opens on it (SongateActivity.installApk) while Songate stays as it is; the
 *  installer replaces the app when the user agrees. Nothing else is started from here. */
bool relaunch(const std::string& program) {
    if (!isInstaller(program)) return false;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return false;
    jclass cls = activityClass(env);
    if (!cls) return false;
    bool ok = false;
    if (jmethodID m = env->GetStaticMethodID(cls, "installApk", "(Ljava/lang/String;)Z")) {
        jstring path = env->NewStringUTF(program.c_str());
        ok = env->CallStaticBooleanMethod(cls, m, path) == JNI_TRUE;
        env->DeleteLocalRef(path);
    }
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
    return ok;
}

// ---- input

// Android is touch only (the user, 2026-11-18: "remove any type of input (controller, keyboard) except touch"): Android's Back is
// the one button read here; the volume keys and the media buttons (a headphone's or Bluetooth remote, the HiBy R4's own) are
// Android's and SongateActivity's, as before. A keyboard's keys and a game pad do nothing in Songate
void padInit() {}

void padRead() {
    heldBefore = heldNow;
    uint64_t b = 0;
    if (backHeld || backPressed) b |= BtnB;
    backPressed = false;
    padLast = false;
    heldNow = b;
}

uint64_t padDown() { return heldNow & ~heldBefore; }
uint64_t padHeld() { return heldNow; }

Stick stick(int) { return { 0, 0 }; }   // (touch only: no pad)

// ---- typing

namespace {
TextFn textCard = nullptr;
}  // namespace

void setTextCard(TextFn fn) { textCard = fn; }
bool padWasLast() { return padLast; }
Names buttonNames() { return padLast ? Names::Pad : Names::None; }   // (touch only: a pad's names after a pad)
void keyPressed() {}

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
float screenDensity() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return 2.625f;
    jclass cls = activityClass(env);
    float d = 0;
    if (cls)
        if (jmethodID m = env->GetStaticMethodID(cls, "density", "()F")) d = env->CallStaticFloatMethod(cls, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return d > 0.3f ? d : 2.625f;
}

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
        jsize n = rgba && w > 0 && h > 0 ? jsize(size_t(w) * size_t(h) * 4) : 0;
        jbyteArray cover = env->NewByteArray(n);
        if (env->ExceptionCheck()) {   // no memory for its 4 MB: told without the cover (no call into Java with an exception pending)
            env->ExceptionClear();
            if (cover) env->DeleteLocalRef(cover);
            n = 0;
            cover = env->NewByteArray(0);   // (empty, as a song with no cover)
            if (env->ExceptionCheck()) { env->ExceptionClear(); cover = nullptr; }
        }
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

void screenRotation(int kind) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jclass cls = activityClass(env);
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "screenRotation", "(I)V")) env->CallStaticVoidMethod(cls, m, jint(kind));
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void padRumble(int) {}   // (a phone: keyTap's tick)

void keySounds(const std::vector<std::vector<int16_t>>& sounds, int rate) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jclass cls = activityClass(env);
    if (!cls) return;
    jmethodID m = env->GetStaticMethodID(cls, "keyWave", "(I[SI)V");
    if (!m) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }
    for (size_t k = 0; k < sounds.size(); k++) {
        jshortArray a = env->NewShortArray(jsize(sounds[k].size()));
        if (!a) break;
        env->SetShortArrayRegion(a, 0, jsize(sounds[k].size()), sounds[k].data());
        env->CallStaticVoidMethod(cls, m, jint(k), a, jint(rate));
        env->DeleteLocalRef(a);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

void keyTap(bool click, int vibrate, int kind, bool overMusic) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jclass cls = activityClass(env);
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "keyTap", "(ZIIZ)V")) env->CallStaticVoidMethod(cls, m, jboolean(click), jint(vibrate), jint(kind), jboolean(overMusic));
    if (env->ExceptionCheck()) env->ExceptionClear();
}

bool homeHeld() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? activityClass(env) : nullptr;
    if (!cls) return false;
    bool held = false;
    if (jmethodID m = env->GetStaticMethodID(cls, "homeHeld", "()Z")) held = env->CallStaticBooleanMethod(cls, m) == JNI_TRUE;
    if (env->ExceptionCheck()) env->ExceptionClear();
    return held;
}

void homeSet(bool on) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? activityClass(env) : nullptr;
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "homeSet", "(Z)V")) env->CallStaticVoidMethod(cls, m, jboolean(on));
    if (env->ExceptionCheck()) env->ExceptionClear();
}

std::vector<std::pair<std::string, std::string>> apps() {
    std::vector<std::pair<std::string, std::string>> out;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? activityClass(env) : nullptr;
    if (!cls) return out;
    jmethodID m = env->GetStaticMethodID(cls, "apps", "()[Ljava/lang/String;");
    auto arr = m ? static_cast<jobjectArray>(env->CallStaticObjectMethod(cls, m)) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return out; }
    if (!arr) return out;
    auto str = [&](jsize i) {
        std::string v;
        if (auto s = static_cast<jstring>(env->GetObjectArrayElement(arr, i))) {
            if (const char* c = env->GetStringUTFChars(s, nullptr)) { v = c; env->ReleaseStringUTFChars(s, c); }
            env->DeleteLocalRef(s);
        }
        return v;
    };
    const jsize n = env->GetArrayLength(arr);
    for (jsize i = 0; i + 1 < n; i += 2) out.emplace_back(str(i), str(i + 1));
    env->DeleteLocalRef(arr);
    return out;
}

std::vector<std::string> systemApps() {
    std::vector<std::string> out;
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? activityClass(env) : nullptr;
    if (!cls) return out;
    jmethodID m = env->GetStaticMethodID(cls, "systemApps", "()[Ljava/lang/String;");
    auto arr = m ? static_cast<jobjectArray>(env->CallStaticObjectMethod(cls, m)) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); return out; }
    if (!arr) return out;
    const jsize n = env->GetArrayLength(arr);
    for (jsize i = 0; i < n; i++)
        if (auto s = static_cast<jstring>(env->GetObjectArrayElement(arr, i))) {
            if (const char* c = env->GetStringUTFChars(s, nullptr)) { out.emplace_back(c); env->ReleaseStringUTFChars(s, c); }
            env->DeleteLocalRef(s);
        }
    env->DeleteLocalRef(arr);
    return out;
}

bool appIcon(const std::string& app, int size, std::vector<uint32_t>& argb) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? activityClass(env) : nullptr;
    if (!cls) return false;
    jmethodID m = env->GetStaticMethodID(cls, "appIcon", "(Ljava/lang/String;I)[I");
    if (!m) { env->ExceptionClear(); return false; }
    jstring name = env->NewStringUTF(app.c_str());
    auto arr = static_cast<jintArray>(env->CallStaticObjectMethod(cls, m, name, jint(size)));
    env->DeleteLocalRef(name);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!arr) return false;
    const bool ok = env->GetArrayLength(arr) == size * size;
    if (ok) {
        argb.resize(size_t(size) * size_t(size));
        env->GetIntArrayRegion(arr, 0, size * size, reinterpret_cast<jint*>(argb.data()));
    }
    env->DeleteLocalRef(arr);
    return ok;
}

void appPage(const std::string& app, bool uninstall) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? activityClass(env) : nullptr;
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "appPage", "(Ljava/lang/String;Z)V")) {
        jstring name = env->NewStringUTF(app.c_str());
        env->CallStaticVoidMethod(cls, m, name, jboolean(uninstall));
        env->DeleteLocalRef(name);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void openApp(const std::string& app) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jclass cls = env ? activityClass(env) : nullptr;
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "openApp", "(Ljava/lang/String;)V")) {
        jstring name = env->NewStringUTF(app.c_str());
        env->CallStaticVoidMethod(cls, m, name);
        env->DeleteLocalRef(name);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void holdLandscape(bool on) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jclass cls = activityClass(env);
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, "holdLandscape", "(Z)V")) env->CallStaticVoidMethod(cls, m, jboolean(on));
    if (env->ExceptionCheck()) env->ExceptionClear();
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

// The battery beyond its charge, from BatteryManager (SongateActivity.batteryInfo): what the phone does not say is marked missing
// and left out (platform.hpp). No fuel gauge's time to empty: the current is the moment's, and a time made from it jumps about, so
// the app's own (from ten minutes of the charge) stays; to full is Android's own estimate.
bool batteryInfo(BatteryInfo& o) {
    const std::string s = javaString("batteryInfo");
    if (s.empty()) return false;
    o = BatteryInfo{};
    o.chargerKnown = false;
    double amps = 0, left = 0, full = 0;
    bool haveAmps = false;
    size_t at = 0;
    while (at < s.size()) {
        const size_t end = s.find('\n', at), eq = s.find('=', at);
        if (end == std::string::npos) break;
        if (eq != std::string::npos && eq < end) {
            const std::string k = s.substr(at, eq - at);
            const double v = atof(s.substr(eq + 1, end - eq - 1).c_str());
            if (k == "volts") o.volts = v;
            else if (k == "temp") o.tempC = v;
            else if (k == "plugged") o.charger = v != 0 ? 1 : 0;
            else if (k == "amps") { amps = v; haveAmps = true; }
            else if (k == "left") left = v;
            else if (k == "full") full = v;
            else if (k == "health") o.healthPct = v;
            else if (k == "ttf") o.ttfMin = int(v);
            else if (k == "cycles") o.cycles = int(v);
        }
        at = end + 1;
    }
    o.missing |= 1u << 2;   // the charger's watts: Android does not say
    if (o.tempC == 0) o.missing |= 1u << 3;
    if (o.volts <= 0) o.missing |= 1u << 4;
    if (o.healthPct <= 0) o.missing |= 1u << 5;
    if (haveAmps && o.volts > 0) {
        o.gauge = true;
        o.gaugeVolts = o.volts;
        o.gaugeAmps = amps;
        if (left > 0 && full > 0) { o.capMah = left; o.fullMah = full; }
    } else o.missing |= 1u << 6;
    return true;
}

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

// ---- the music volume (SongateActivity: its keys, its watch on every change) and the HiBy R4's settings service

namespace {
std::atomic<int> volIndex{ -1 }, volMax{ 15 };
std::atomic<unsigned> volGen{ 0 }, volKeys{ 0 }, resumeCount{ 0 };

/** A static method of SongateActivity, with what the caller does with it; the exception (if any) cleared. */
template <class F> void callStatic(const char* name, const char* sig, F use) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) return;
    jclass cls = activityClass(env);
    if (!cls) return;
    if (jmethodID m = env->GetStaticMethodID(cls, name, sig)) use(env, cls, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

std::string fromJava(JNIEnv* env, jstring s) {
    std::string out;
    if (!s) return out;
    if (const char* c = env->GetStringUTFChars(s, nullptr)) {
        out = c;
        env->ReleaseStringUTFChars(s, c);
    }
    env->DeleteLocalRef(s);
    return out;
}
}  // namespace

MediaVolume mediaVolume() {
    if (volIndex.load() < 0) {   // not reported yet (the app's start): asked once
        callStatic("volumeNow", "()I", [](JNIEnv* env, jclass cls, jmethodID m) {
            const int v = env->CallStaticIntMethod(cls, m);
            if (v >= 0) {
                volMax = std::max(1, v >> 16);
                volIndex = v & 0xffff;
                volGen++;
            }
        });
    }
    MediaVolume o;
    o.index = volIndex.load();
    o.max = volMax.load();
    o.gen = volGen.load();
    o.keys = volKeys.load();
    return o;
}

void mediaVolumeSet(int index) {
    index = std::clamp(index, 0, volMax.load());
    if (index == volIndex.load()) return;
    volIndex = index;   // at once, so the slider does not jump back before Android's word comes
    volGen++;
    callStatic("volumeSet", "(I)V", [index](JNIEnv* env, jclass cls, jmethodID m) { env->CallStaticVoidMethod(cls, m, jint(index)); });
}

void volumeKeysOwn(bool own) {
    callStatic("volumeKeysOwn", "(Z)V", [own](JNIEnv* env, jclass cls, jmethodID m) { env->CallStaticVoidMethod(cls, m, jboolean(own)); });
}

unsigned resumes() { return resumeCount.load(); }

std::atomic<unsigned> otherGenV{ 0 };
unsigned otherGen() { return otherGenV.load(); }

namespace {
jmethodID otherMethod(JNIEnv*& env, jclass& cls, const char* name, const char* sig) {
    env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    cls = env ? activityClass(env) : nullptr;
    if (!cls) return nullptr;
    jmethodID m = env->GetStaticMethodID(cls, name, sig);
    if (!m) env->ExceptionClear();
    return m;
}
}  // namespace

bool otherAllowed() {
    JNIEnv* env;
    jclass cls;
    jmethodID m = otherMethod(env, cls, "otherAllowed", "()Z");
    if (!m) return false;
    const bool r = env->CallStaticBooleanMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return r;
}

void otherAccess() {
    JNIEnv* env;
    jclass cls;
    if (jmethodID m = otherMethod(env, cls, "otherAccess", "()V")) { env->CallStaticVoidMethod(cls, m); if (env->ExceptionCheck()) env->ExceptionClear(); }
}

bool otherWatch(bool on) {
    JNIEnv* env;
    jclass cls;
    jmethodID m = otherMethod(env, cls, "otherWatch", "(Z)Z");
    if (!m) return false;
    const bool done = env->CallStaticBooleanMethod(cls, m, jboolean(on));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return done;
}

std::vector<PlaySearchApp> playSearchApps() {
    JNIEnv* env;
    jclass cls;
    std::vector<PlaySearchApp> out;
    jmethodID m = otherMethod(env, cls, "playSearchApps", "()Ljava/lang/String;");
    if (!m) return out;
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(cls, m));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return out; }
    const std::string text = s ? fromJava(env, s) : std::string();
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t tab = line.find('\t'), tab2 = tab == std::string::npos ? tab : line.find('\t', tab + 1);
        if (tab == std::string::npos || tab == 0) continue;
        PlaySearchApp a;
        a.component = line.substr(0, tab);
        a.name = line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
        out.push_back(std::move(a));
    }
    return out;
}

void playSearch(const std::string& component, const std::string& query) {
    JNIEnv* env;
    jclass cls;
    jmethodID m = otherMethod(env, cls, "playSearch", "(Ljava/lang/String;[B)V");
    if (!m) return;
    jstring c = env->NewStringUTF(component.c_str());
    jbyteArray q = bytes(env, query);
    env->CallStaticVoidMethod(cls, m, c, q);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(c);
    if (q) env->DeleteLocalRef(q);
}

std::string otherState() {
    JNIEnv* env;
    jclass cls;
    jmethodID m = otherMethod(env, cls, "otherState", "()Ljava/lang/String;");
    if (!m) return {};
    auto s = static_cast<jstring>(env->CallStaticObjectMethod(cls, m));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return {}; }
    std::string out = s ? fromJava(env, s) : std::string();
    // (fromJava lets the string go)
    return out;
}

bool recordAllowed() {
    JNIEnv* env;
    jclass cls;
    jmethodID m = otherMethod(env, cls, "recordAllowed", "()Z");
    if (!m) return false;
    const bool r = env->CallStaticBooleanMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    return r;
}

void askRecord() {
    JNIEnv* env;
    jclass cls;
    if (jmethodID m = otherMethod(env, cls, "askRecord", "()V")) { env->CallStaticVoidMethod(cls, m); if (env->ExceptionCheck()) env->ExceptionClear(); }
}

int recordAnswer() {
    JNIEnv* env;
    jclass cls;
    jmethodID m = otherMethod(env, cls, "recordAnswer", "()I");
    if (!m) return 0;
    const int r = env->CallStaticIntMethod(cls, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return r;
}

bool otherViz(std::vector<uint8_t>& wave, int& rateHz) {
    JNIEnv* env;
    jclass cls;
    jmethodID m = otherMethod(env, cls, "otherViz", "([I)[B");
    if (!m) return false;
    jintArray r = env->NewIntArray(1);
    if (!r) return false;
    auto a = static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, m, r));
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(r); return false; }
    bool ok = false;
    if (a) {
        wave.resize(size_t(env->GetArrayLength(a)));
        env->GetByteArrayRegion(a, 0, jsize(wave.size()), reinterpret_cast<jbyte*>(wave.data()));
        jint hz = 0;
        env->GetIntArrayRegion(r, 0, 1, &hz);
        rateHz = hz;
        env->DeleteLocalRef(a);
        ok = !wave.empty() && hz > 0;
    }
    env->DeleteLocalRef(r);
    return ok;
}

void otherVizStop() {
    JNIEnv* env;
    jclass cls;
    if (jmethodID m = otherMethod(env, cls, "otherVizStop", "()V")) { env->CallStaticVoidMethod(cls, m); if (env->ExceptionCheck()) env->ExceptionClear(); }
}

std::vector<uint8_t> otherArt() {
    JNIEnv* env;
    jclass cls;
    std::vector<uint8_t> out;
    jmethodID m = otherMethod(env, cls, "otherArt", "()[B");
    if (!m) return out;
    auto a = static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, m));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return out; }
    if (!a) return out;
    out.resize(size_t(env->GetArrayLength(a)));
    env->GetByteArrayRegion(a, 0, jsize(out.size()), reinterpret_cast<jbyte*>(out.data()));
    env->DeleteLocalRef(a);
    return out;
}

bool otherIcon(int size, std::vector<uint32_t>& argb) {
    JNIEnv* env;
    jclass cls;
    jmethodID m = otherMethod(env, cls, "otherIcon", "(I)[I");
    if (!m) return false;
    auto arr = static_cast<jintArray>(env->CallStaticObjectMethod(cls, m, jint(size)));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!arr) return false;
    const bool ok = env->GetArrayLength(arr) == size * size;
    if (ok) {
        argb.resize(size_t(size) * size_t(size));
        env->GetIntArrayRegion(arr, 0, size * size, reinterpret_cast<jint*>(argb.data()));
    }
    env->DeleteLocalRef(arr);
    return ok;
}

void otherControl(int cmd, double arg) {
    JNIEnv* env;
    jclass cls;
    if (jmethodID m = otherMethod(env, cls, "otherControl", "(IJ)V")) {
        env->CallStaticVoidMethod(cls, m, jint(cmd), jlong(arg));
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
}

int hibyState() {
    int st = 0;
    callStatic("hibyState", "()I", [&st](JNIEnv* env, jclass cls, jmethodID m) { st = env->CallStaticIntMethod(cls, m); });
    return st;
}

// Asked of Android once a second on a thread of its own: the call goes to its audio service, which waits while the HiBy R4's
// restarts for a new output rate (on the app's thread that held the whole screen 4-5 s: SongateOS, 2026-11-16). The first
// call starts it; the answer is the last one in.
static std::atomic<bool> wiredNow{ false };
bool wiredOut() {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                bool on = false;
                callStatic("wiredOut", "()Z", [&](JNIEnv* env, jclass cls, jmethodID m) { on = env->CallStaticBooleanMethod(cls, m) == JNI_TRUE; });
                wiredNow = on;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }).detach();
    });
    return wiredNow;
}

bool volumeReady(const std::string& path) {
    bool ok = true;
    callStatic("volumeReady", "(Ljava/lang/String;)Z", [&](JNIEnv* env, jclass cls, jmethodID m) {
        jstring s = env->NewStringUTF(path.c_str());
        ok = env->CallStaticBooleanMethod(cls, m, s) == JNI_TRUE;
        env->DeleteLocalRef(s);
    });
    return ok;
}

std::atomic<bool> bpLost{ false }, bpConfirmed{ false };

int bitPerfectBegin(int rate, int bits) {
    bpConfirmed = false;   // (until Android's listener says it holds: only then does the badge sparkle)
    int code = 0;
    callStatic("bitPerfectBegin", "(II)I", [&](JNIEnv* env, jclass cls, jmethodID m) { code = env->CallStaticIntMethod(cls, m, jint(rate), jint(bits)); });
    if (code) bpLost = false;
    return code;
}

bool bitPerfectConfirmed() { return bpConfirmed.load(); }

void bitPerfectEnd() {
    callStatic("bitPerfectEnd", "()V", [](JNIEnv* env, jclass cls, jmethodID m) { env->CallStaticVoidMethod(cls, m); });
}

bool bitPerfectLost() { return bpLost.load(); }

bool bitPerfectRouted() {
    bool on = false;
    callStatic("bitPerfectRouted", "()Z", [&on](JNIEnv* env, jclass cls, jmethodID m) { on = env->CallStaticBooleanMethod(cls, m) == JNI_TRUE; });
    return on;
}

bool hibyDevice() {
    static int known = -1;   // asked once
    if (known < 0) {
        known = 0;
        callStatic("hibyDevice", "()Z", [](JNIEnv* env, jclass cls, jmethodID m) { known = env->CallStaticBooleanMethod(cls, m) == JNI_TRUE ? 1 : 0; });
    }
    return known == 1;
}

std::string hibyGet() {
    std::string out;
    callStatic("hibyGet", "()Ljava/lang/String;", [&out](JNIEnv* env, jclass cls, jmethodID m) {
        out = fromJava(env, static_cast<jstring>(env->CallStaticObjectMethod(cls, m)));
    });
    return out;
}

std::string hibySet(const std::string& key, const std::string& value) {
    std::string err = "No answer from the HiBy service";
    callStatic("hibySet", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [&](JNIEnv* env, jclass cls, jmethodID m) {
        jstring k = env->NewStringUTF(key.c_str()), v = env->NewStringUTF(value.c_str());
        err = fromJava(env, static_cast<jstring>(env->CallStaticObjectMethod(cls, m, k, v)));
        env->DeleteLocalRef(k);
        env->DeleteLocalRef(v);
    });
    return err;
}

std::string sysCall(const std::string& method, const std::string& arg, const std::string& extras) {
    std::string out = "{\"ok\":false,\"error\":\"no_answer: no Java\"}";
    callStatic("sysCall", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [&](JNIEnv* env, jclass cls, jmethodID m) {
        jstring a = env->NewStringUTF(method.c_str()), b = env->NewStringUTF(arg.c_str()), c = env->NewStringUTF(extras.c_str());
        out = fromJava(env, static_cast<jstring>(env->CallStaticObjectMethod(cls, m, a, b, c)));
        env->DeleteLocalRef(a);
        env->DeleteLocalRef(b);
        env->DeleteLocalRef(c);   // (a password's copy: Java's own string, gone with the call)
    });
    return out;
}

void sysWatch(const char* area, bool on) {
    callStatic("sysWatch", "(Ljava/lang/String;Z)V", [&](JNIEnv* env, jclass cls, jmethodID m) {
        jstring a = env->NewStringUTF(area);
        env->CallStaticVoidMethod(cls, m, a, jboolean(on));
        env->DeleteLocalRef(a);
    });
}

void sysOpen(const char* which) {
    callStatic("sysOpen", "(Ljava/lang/String;)V", [&](JNIEnv* env, jclass cls, jmethodID m) {
        jstring a = env->NewStringUTF(which);
        env->CallStaticVoidMethod(cls, m, a);
        env->DeleteLocalRef(a);
    });
}

namespace {
std::mutex sysChangedMx;
std::map<std::string, unsigned> sysChangedGen;
}
unsigned sysChanged(const char* area) {
    std::lock_guard<std::mutex> g(sysChangedMx);
    return sysChangedGen[area];
}
void sysBump(const std::string& area) {
    std::lock_guard<std::mutex> g(sysChangedMx);
    sysChangedGen[area]++;
}

bool hibyEqSet(const std::string& eq) {
    bool ok = false;
    callStatic("hibyEqSet", "(Ljava/lang/String;)Z", [&](JNIEnv* env, jclass cls, jmethodID m) {
        jstring s = env->NewStringUTF(eq.c_str());
        ok = env->CallStaticBooleanMethod(cls, m, s) == JNI_TRUE;
        env->DeleteLocalRef(s);
    });
    return ok;
}

bool hibyLedLive(int rgb, int bright, bool breath) {
    bool ok = false;
    callStatic("hibyLedLive", "(IIZ)Z", [&](JNIEnv* env, jclass cls, jmethodID m) {
        ok = env->CallStaticBooleanMethod(cls, m, jint(rgb), jint(bright), jboolean(breath)) == JNI_TRUE;
    });
    return ok;
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

/** SongateActivity, on Android's UI thread: the music volume changed (fromKey: a volume key Songate took itself). */
extern "C" JNIEXPORT void JNICALL Java_io_github_andrythebeast_songate_SongateActivity_nativeVolume(JNIEnv*, jclass, jint index, jint max, jboolean fromKey) {
    plat::volMax = std::max(1, int(max));
    plat::volIndex = std::clamp(int(index), 0, int(max));
    plat::volGen++;
    if (fromKey) plat::volKeys++;
}

/** SysApi.java's ContentObserver, on Android's main thread: the service says an area changed (Wi-Fi, Bluetooth, the output…). */
extern "C" JNIEXPORT void JNICALL Java_io_github_andrythebeast_songate_SysApi_nativeSysChanged(JNIEnv* env, jclass, jstring area) {
    plat::sysBump(plat::fromJava(env, area));
}

/** BitPerfect.java's listener, on Android's main thread: Android says whether Songate's bit-perfect claim holds. */
extern "C" JNIEXPORT void JNICALL Java_io_github_andrythebeast_songate_BitPerfect_nativeBitPerfect(JNIEnv*, jclass, jboolean on) {
    if (!on) plat::bpLost = true;
    plat::bpConfirmed = bool(on);
}

/** OtherApps.java, on Android's main thread: what another app plays changed (its session, song or state). */
extern "C" JNIEXPORT void JNICALL Java_io_github_andrythebeast_songate_OtherApps_nativeOtherChanged(JNIEnv*, jclass) {
    plat::otherGenV++;
}

/** SongateActivity.onResume, on Android's UI thread. */
extern "C" JNIEXPORT void JNICALL Java_io_github_andrythebeast_songate_SongateActivity_nativeResumed(JNIEnv*, jclass) {
    plat::resumeCount++;
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
