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
 *   stats <file>      frames since the last "stats": how many, the mean and the worst time between two, the worst work
 * tools/preview/devresize.py and devsizes.py speak it.
 */
#if defined(GF_WINDOWED) && defined(GF_DEV) && !defined(GF_ANDROID)
#include "app.hpp"

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
        else if (s.rfind("search ", 0) == 0) navigate(View::Search, -1, s.substr(7), route.view != View::Search);
        else if (s.rfind("stats ", 0) == 0) {
            if (FILE* out = fopen(s.substr(6).c_str(), "w")) {
                fprintf(out, "frames %ld mean %.3f worst %.3f work %.3f hz %d scale %.4f size %d %d real %d %d\n", devN_, devN_ ? devSum_ / devN_ * 1000 : 0.0,
                        devWorst_ * 1000, devWork_, hz_, double(scale_), css::W, css::H, framePw_, framePh_);
                fclose(out);
            }
            devN_ = 0;
            devSum_ = devWorst_ = devWork_ = 0;
        }
    }
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
