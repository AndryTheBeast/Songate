/*
 * Songate · devctl.cpp
 * Dev builds on a PC only (never in a release, never on the console or Android): a test drives the window's layout by a file.
 * GF_CTL=<file>: each frame the file is read, done and deleted. One command a line:
 *   size <w> <h>      the window is laid out as if it had this many real pixels (0 0: as it is); the frame is drawn at that size
 *   screen <w> <h>    … on a screen of this many pixels (0 0: the real one)
 *   dpi <n>           … at this many dots an inch (96 is 100 %; 0: the real one)
 *   shot <file.bmp>   this frame, as drawn, saved
 *   tap <x> <y>       a mouse click at app pixels
 *   move <x> <y>      the mouse there
 *   down <x> <y>      its left button pressed there, held until "up" (a drag: down, move…, up)
 *   up <x> <y>        … and let go there
 *   wheel <x> <y> <n> the wheel, n notches
 *   pad <mask>        buttons pressed for a frame (platform.hpp's bits)
 *   search <words>    the Search page for these words (the text card holds the frame, so a test cannot type into it)
 *   stats <file>      frames since the last "stats": how many, the mean and the worst time between two, the worst work; and the
 *                     C heap's used bytes and blocks, walked (leak tests)
 * tools/preview/devresize.py and devsizes.py speak it.
 */
#if defined(GF_WINDOWED) && defined(GF_DEV) && !defined(GF_ANDROID)
#include "app.hpp"

#include <malloc.h>
#include <map>

/** The C heap's used bytes and blocks, walked (exact, a few ms): what the app itself holds, apart from the graphics driver's. */
static void devHeap(size_t& bytes, size_t& blocks) {
    bytes = blocks = 0;
    _HEAPINFO h;
    h._pentry = nullptr;
    while (_heapwalk(&h) == _HEAPOK)
        if (h._useflag == _USEDENTRY) { bytes += h._size; blocks++; }
}

void App::devControl() {
    static const char* path = getenv("GF_CTL");
    if (!path) return;
    // a click's release, a few frames after its press
    if (devTapUp_ > 0 && --devTapUp_ == 0 && mouseHeld_) {
        if (sortTapAt(pointer.x, pointer.y)) devSortTap_ = true;
        ui.touchUp(pointer.x, pointer.y); mouseHeld_ = false;
    }
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char line[1024];
    std::vector<std::string> lines;
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) lines.push_back(s);
    }
    fclose(f);
    if (remove(path) != 0) return;   // (still being written: the next frame)
    for (const std::string& s : lines) {
        float a = 0, b = 0, c = 0;
        if (sscanf(s.c_str(), "size %f %f", &a, &b) == 2) { devPw_ = int(a); devPh_ = int(b); fitWindow(); }
        else if (sscanf(s.c_str(), "screen %f %f", &a, &b) == 2) { devScreenW_ = int(a); devScreenH_ = int(b); fitWindow(); }
        else if (sscanf(s.c_str(), "dpi %f", &a) == 1) { devDpi_ = a; fitWindow(); }
        else if (s.rfind("shot ", 0) == 0) devShot_ = s.substr(5);
        else if (sscanf(s.c_str(), "tap %f %f", &a, &b) == 2 || sscanf(s.c_str(), "move %f %f", &a, &b) == 2) {
            pointer.x = std::clamp(a, 0.f, float(css::W - 1));
            pointer.y = std::clamp(b, 0.f, float(css::H - 1));
            pointer.idle = 0;
            pointerIsMouse_ = true;
            devMoved_ = true;
            if (s[0] == 't') { ui.touchDown(pointer.x, pointer.y); mouseHeld_ = true; devTapUp_ = 4; fingerDownX_ = pointer.x; fingerDownY_ = pointer.y; }
        } else if (sscanf(s.c_str(), "down %f %f", &a, &b) == 2 || sscanf(s.c_str(), "up %f %f", &a, &b) == 2) {
            pointer.x = std::clamp(a, 0.f, float(css::W - 1));
            pointer.y = std::clamp(b, 0.f, float(css::H - 1));
            pointer.idle = 0;
            pointerIsMouse_ = true;
            if (s[0] == 'd') {
                ui.touchDown(pointer.x, pointer.y); mouseHeld_ = true; devClicked_ = true; devTapUp_ = 0;
                fingerDownX_ = pointer.x; fingerDownY_ = pointer.y;
            } else if (mouseHeld_) {
                if (sortTapAt(pointer.x, pointer.y)) devSortTap_ = true;
                ui.touchUp(pointer.x, pointer.y); mouseHeld_ = false;
            }
        } else if (sscanf(s.c_str(), "wheel %f %f %f", &a, &b, &c) == 3) {
            pointer.x = a; pointer.y = b; pointer.idle = 0;
            devWheel_ += c;
        } else if (sscanf(s.c_str(), "pad %f", &a) == 1) devPad_ |= uint64_t(a);
        else if (sscanf(s.c_str(), "album %f", &a) == 1) {   // (session 23: the nth album of the Albums page opened, its name in the log)
            const int i = int(a);
            if (i >= 0 && i < int(albumOrder.size())) {
                logLine("devctl: album " + std::to_string(i) + ": " + lib.albums[size_t(albumOrder[size_t(i)])].title);
                openAlbum(albumOrder[size_t(i)]);
            }
        } else if (s.rfind("heapsizes ", 0) == 0) {   // (leak hunts: the C heap's used blocks counted by size, "size count" a line)
            std::map<size_t, size_t> bySize;
            _HEAPINFO h;
            h._pentry = nullptr;
            while (_heapwalk(&h) == _HEAPOK)
                if (h._useflag == _USEDENTRY) bySize[h._size]++;
            if (FILE* out = fopen(s.substr(10).c_str(), "w")) {
                for (const auto& kv : bySize) fprintf(out, "%zu %zu\n", kv.first, kv.second);
                fclose(out);
            }
        } else if (s.rfind("hitches ", 0) == 0) {   // every frame over 18 ms since the last "hitches": its parts (as devconsole.cpp's)
            if (FILE* out = fopen(s.substr(8).c_str(), "w")) {
                for (const DevHitch& h : devHitches_) {
                    fprintf(out, "%.2f %.1f work %.1f upd %.1f (%s %.1f) bg %.1f view %.1f side %.1f bar %.1f rest %.1f warm %.1f pres %.1f view# %d sheet %d canvas %d\n",
                            h.t, h.ms, h.work, h.prof[0], h.step, h.stepMs, h.prof[1], h.prof[2], h.prof[3], h.prof[4], h.prof[5], h.prof[6], h.prof[7],
                            h.view, h.sheet, int(h.canvas));
                }
                fclose(out);
            }
            devHitches_.clear();
        }
        else if (s.rfind("search ", 0) == 0) navigate(View::Search, -1, s.substr(7), route.view != View::Search);
        else if (s.rfind("stats ", 0) == 0) {
            if (FILE* out = fopen(s.substr(6).c_str(), "w")) {
                size_t heapBytes = 0, heapBlocks = 0;
                devHeap(heapBytes, heapBlocks);   // (leak tests: after the rest, so the older readers of this line still find their words)
                fprintf(out, "frames %ld mean %.3f worst %.3f work %.3f hz %d scale %.4f size %d %d real %d %d heap %zu %zu srvcovers %zu\n", devN_, devN_ ? devSum_ / devN_ * 1000 : 0.0,
                        devWorst_ * 1000, devWork_, hz_, double(scale_), css::W, css::H, framePw_, framePh_, heapBytes, heapBlocks, serverCoverUsed_.size());
                fclose(out);
            }
            devN_ = 0;
            devSum_ = devWorst_ = devWork_ = 0;
        }
    }
}

/*
 * The console, emulated (dev only): GF_EMU="<cpu> <gpu>" stretches each frame as the Switch's slower Cortex-A57s and Tegra X1 would.
 * The app is pinned to 3 cores (an application's share of the console's 4); the frame's CPU work (input, update, the draw calls) is
 * timed, then the GPU's (the draw flushed and one pixel read back, which waits for it), and the frame is held until
 * cpu * its CPU time + gpu * its GPU time have passed. The two factors are fitted from the same self-test walk on the console and here
 * (docs/next-session/session23). Background threads are not slowed: the emulation says nothing about them.
 */
extern "C" __declspec(dllimport) int __stdcall SetProcessAffinityMask(void*, unsigned long long);
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();

void App::devEmuStretch(Uint64 frameStart) {
    static float kc = -1, kg = 1;
    if (kc < 0) {
        kc = 0;
        if (const char* e = getenv("GF_EMU")) {
            if (sscanf(e, "%f %f", &kc, &kg) < 1) kc = 0;
            if (kc > 0) SetProcessAffinityMask(GetCurrentProcess(), 0x7);   // 3 cores
        }
    }
    if (kc <= 0) return;
    const double freq = double(SDL_GetPerformanceFrequency());
    const Uint64 cpuEnd = SDL_GetPerformanceCounter();
    Uint64 gpuEnd = cpuEnd;
    if (kg > 1) {   // (a GPU factor of 1 or less: the GPU left alone; its read-back stalls the pipeline)
        SDL_RenderFlush(renderer_);
        Uint32 px = 0;
        const SDL_Rect one{ 0, 0, 1, 1 };
        SDL_RenderReadPixels(renderer_, &one, SDL_PIXELFORMAT_ARGB8888, &px, 4);   // (waits for the GPU)
        gpuEnd = SDL_GetPerformanceCounter();
    }
    const double cpu = double(cpuEnd - frameStart) / freq, gpu = double(gpuEnd - cpuEnd) / freq;
    const double want = cpu * kc + gpu * kg;
    while (double(SDL_GetPerformanceCounter() - frameStart) / freq < want) {}
}

/** A frame over 18 ms, kept with its parts (profMark) and update's slowest step: written on "hitches", never per frame. */
void App::devHitch(double interval, float workMs) {
    if (!getenv("GF_CTL") || interval * 1000 <= 18.0 || devHitches_.size() >= 4000) return;
    DevHitch h{};
    h.t = float(time); h.ms = float(interval * 1000); h.work = workMs;
    for (int i = 0; i < 8; i++) h.prof[i] = float(profFrame_[i]);
    h.step = updSlow_ ? updSlow_ : ""; h.stepMs = float(updSlowMs_);
    h.view = int(route.view); h.sheet = int(sheet.page); h.canvas = canvas;
    devHitches_.push_back(h);
}

/** The frame as drawn (its texture is still the target), to a file. */
void App::devShot() {
    if (devShot_.empty()) return;
    if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, framePw_, framePh_, 32, SDL_PIXELFORMAT_ARGB8888)) {
        const SDL_Rect whole{ 0, 0, framePw_, framePh_ };
        if (SDL_RenderReadPixels(renderer_, &whole, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) {
            const std::string tmp = devShot_ + ".new";
            SDL_SaveBMP(s, tmp.c_str());
            remove(devShot_.c_str());
            rename(tmp.c_str(), devShot_.c_str());
        }
        SDL_FreeSurface(s);
    }
    devShot_.clear();
}
#endif
