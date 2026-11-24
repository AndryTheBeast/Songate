/*
 * Songate · devconsole.cpp
 * The console's dev remote control (dev builds on the Switch only, never a release): the PC's GF_CTL hand (devctl.cpp), driven over FTP.
 * Once a second the app looks for devctl.txt in its settings folder (sdmc:/config/songate-nx); found, it is read, deleted and its lines
 * queued. One command a line:
 *   tap <x> <y>               a finger down and, 4 frames later, up (app pixels, 1280x720)
 *   down|move|up <x> <y>      a finger by hand
 *   drag <x1> <y1> <x2> <y2> <frames>   a finger moved from one place to the other over that many frames, then up
 *   pad <mask>                buttons pressed for one frame (platform.hpp's bits)
 *   hold <mask> <frames>      ... held that many frames (the D-pad's repeat, a long press)
 *   wait <frames>             the lines after it wait that many frames
 *   shot <file.bmp>           the frame as drawn, saved (the frame that saves it is left out of the stats)
 *   items <file>              what is on screen and where, as the PC's GF_ITEMS writes it (pcdrive.py's Screen reads it)
 *   stats <file>              frames since the last stats: count, mean, worst, over 16.7 and 33.3 ms, worst work; the heap
 *   frames <file> <n>         the next n frames' times (interval and work, ms), one a line (charts, the emulation's fit)
 *   selftest <0|1|2>          Settings > Debug > Self test, Basic, Advanced or Full; "selftest stop" stops it
 *   log <words>               a line in log.txt
 *   done <file>               written when the queue gets here: the PC knows the lines before it are done
 *   hitches <file>            every frame over 18 ms since the last "hitches" (kept in RAM, at most 4000): when, how long, its work,
 *                             its parts (profMark), the page, the Canvas, the sheet, the keyboard card, update's slowest step, playing
 * Paths without "sdmc:" are in the settings folder. The PC writes devctl.tmp and renames it, so a file found is whole.
 */
#if defined(__SWITCH__) && defined(GF_DEV)
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <malloc.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "app.hpp"

extern bool uiNamesOn;   // (ui.cpp)

struct App::DevCon {
    struct Cmd { long at; std::string line; };
    std::deque<Cmd> q;
    long frame = 0;
    double readAt = 0;
    int dragLeft = 0, dragN = 0;   // a drag in progress
    float dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
    uint64_t holdMask = 0;         // a hold in progress
    int holdLeft = 0;
    long upAt = -1;                // a tap's release
    float upX = 0, upY = 0;
    std::string shot, items;
    int itemsIn = 0;   // (frames until the items are written: the names are made while the frames are drawn)
    int skipStat = 0;
    long n = 0, over16 = 0, over33 = 0;   // stats since the last "stats"
    double sum = 0, worst = 0, worstWork = 0;
    struct Hitch { float t, ms, work, prof[8]; int view, sheet; bool canvas, kb, playing; const char* step; };
    std::vector<Hitch> hitches;     // (RAM only: written on "hitches", never per frame)
    FILE* series = nullptr;        // a frame series
    int seriesLeft = 0;
};

static std::string devPath(const std::string& p) {
    if (p.rfind("sdmc:", 0) == 0) return p;
    return std::string(Store::Dir) + "/" + p;
}

static void devFinger(Uint32 type, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.timestamp = SDL_GetTicks();
    e.tfinger.touchId = 0;
    e.tfinger.fingerId = 7;   // (a finger of its own)
    e.tfinger.x = std::clamp(x, 0.f, float(css::W - 1)) / float(css::W);
    e.tfinger.y = std::clamp(y, 0.f, float(css::H - 1)) / float(css::H);
    e.tfinger.pressure = 1;
    SDL_PushEvent(&e);
}

/*
 * The port (5055): FTP is Sphaira's and stops while Songate runs, so the dev build listens itself. One request a connection:
 *   CTL <n>\n<n bytes of lines>   the lines queued as devctl.txt's would be; answers "OK\n"
 *   GET <name>\n                  a file of the settings folder (or "sdmc:..."): "<size>\n<bytes>", or "-1\n"
 *   DEL <name>\n                  it deleted: "OK\n"
 * Only in dev builds; the thread waits in accept and never touches the app's state (the lines go through a lock).
 */
static std::mutex devInboxLock;
static std::vector<std::string> devInbox;

static bool devSendAll(int s, const char* p, size_t n) {
    while (n > 0) { const ssize_t k = send(s, p, n, 0); if (k <= 0) return false; p += k; n -= size_t(k); }
    return true;
}

static void devServe(int c) {
    timeval tv{ 1, 500000 };   // (short: the close waits for this thread, inside the 4 s watchdog)
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string head;
    char ch;
    while (head.size() < 600 && recv(c, &ch, 1, 0) == 1 && ch != '\n') head += ch;
    if (head.rfind("CTL ", 0) == 0) {
        size_t n = size_t(std::max(0L, atol(head.c_str() + 4)));
        std::string body;
        char buf[2048];
        while (body.size() < n) { const ssize_t k = recv(c, buf, std::min(sizeof buf, n - body.size()), 0); if (k <= 0) break; body.append(buf, size_t(k)); }
        std::vector<std::string> lines;
        size_t at = 0;
        while (at < body.size()) {
            size_t e = body.find('\n', at);
            if (e == std::string::npos) e = body.size();
            std::string s = body.substr(at, e - at);
            while (!s.empty() && s.back() == '\r') s.pop_back();
            if (!s.empty()) lines.push_back(s);
            at = e + 1;
        }
        { std::lock_guard<std::mutex> l(devInboxLock); devInbox.insert(devInbox.end(), lines.begin(), lines.end()); }
        devSendAll(c, "OK\n", 3);
    } else if (head.rfind("GET ", 0) == 0) {
        std::string data;
        bool ok = false;
        if (FILE* f = fopen(devPath(head.substr(4)).c_str(), "rb")) {
            char buf[16384];
            size_t k;
            while ((k = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, k);
            fclose(f);
            ok = true;
        }
        const std::string h = ok ? std::to_string(data.size()) + "\n" : std::string("-1\n");
        devSendAll(c, h.data(), h.size());
        if (ok) devSendAll(c, data.data(), data.size());
    } else if (head.rfind("PUT ", 0) == 0) {   // "PUT <n> <path>": n bytes to the file (a new .nro as <it>.gfnew; "pending" swaps it in)
        char path[512] = {};
        long n = -1;
        if (sscanf(head.c_str() + 4, "%ld %511[^\n]", &n, path) == 2 && n >= 0) {
            const std::string to = devPath(path), tmp = to + ".part";
            bool ok = false;
            if (FILE* f = fopen(tmp.c_str(), "wb")) {
                char buf[16384];
                long got = 0;
                while (got < n) {
                    const ssize_t k = recv(c, buf, size_t(std::min<long>(sizeof buf, n - got)), 0);
                    if (k <= 0) break;
                    fwrite(buf, 1, size_t(k), f);
                    got += k;
                }
                ok = fclose(f) == 0 && got == n;
            }
            if (ok) { remove(to.c_str()); ok = rename(tmp.c_str(), to.c_str()) == 0; }
            else remove(tmp.c_str());
            devSendAll(c, ok ? "OK\n" : "FAIL\n", ok ? 3 : 5);
        } else devSendAll(c, "FAIL\n", 5);
    } else if (head.rfind("DEL ", 0) == 0) {
        remove(devPath(head.substr(4)).c_str());
        devSendAll(c, "OK\n", 3);
    }
    close(c);
}

static std::atomic<int> devListenSock{ -1 };
static std::atomic<bool> devStopping{ false };
static Thread devListenThread;
static bool devListenOn = false;

static void devListenMain(void*) {
    {
        const int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return;
        devListenSock = s;
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(5055);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 4) != 0) { close(s); return; }
        for (;;) {
            const int c = accept(s, nullptr, nullptr);
            if (devStopping) { if (c >= 0) close(c); break; }
            if (c < 0) { svcSleepThread(200'000'000); continue; }
            devServe(c);
        }
    }
}

/** The port's thread, made with libnx's own call: a std::thread that cannot be made ends the program (no exceptions: terminate,
 *  abort, exit; the console's crash of 2026-11-24 11:46, which also pulled the card from under the tag reader). Refused, the file
 *  (devctl.txt) still works, and the log says so. */
static bool devListen() {
    const Result rc = threadCreate(&devListenThread, devListenMain, nullptr, nullptr, 0x10000, 0x2C, -2);
    if (R_FAILED(rc)) return false;
    if (R_FAILED(threadStart(&devListenThread))) { threadClose(&devListenThread); return false; }
    devListenOn = true;
    return true;
}

/** At the close, first of all: the port's thread ended (its socket shut, which wakes accept) and waited for, so nothing of it is
 *  still in a socket call when Songate takes the sockets and the card away (the console's crash closing, 2026-11-24 12:04). */
void App::devConsoleEnd() {
    if (!devListenOn) return;
    devStopping = true;
    const int s = devListenSock.exchange(-1);
    if (s >= 0) { ::shutdown(s, SHUT_RDWR); ::close(s); }
    threadWaitForExit(&devListenThread);
    threadClose(&devListenThread);
    devListenOn = false;
}

void App::devConsole(uint64_t& down, uint64_t& held) {
    if (!devCon_) {
        devCon_ = new DevCon;
        if (!devListen()) logLine("devctl: no port 5055 (its thread could not be made): devctl.txt only");
    }
    DevCon& d = *devCon_;
    d.frame++;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> l(devInboxLock);
        lines.swap(devInbox);
    }
    // the file only when the port could not be had: its once-a-second look at the card cost the console's main thread up to 50 ms
    // (session 23's first console run: "input 50.7" on the album steps)
    if (!devListenOn && time - d.readAt >= 1.0) {
        d.readAt = time;
        const std::string path = devPath("devctl.txt");
        if (FILE* f = fopen(path.c_str(), "rb")) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                std::string s = line;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) lines.push_back(s);
            }
            fclose(f);
            remove(path.c_str());
        }
    }
    {
        long at = d.frame;
        for (const std::string& s : lines) {
            int w = 0;
            if (sscanf(s.c_str(), "wait %d", &w) == 1) { at += std::max(0, w); continue; }
            d.q.push_back({ at, s });
        }
    }
    // a hand already moving (its events reach the next frame's poll)
    if (d.upAt >= 0 && d.frame >= d.upAt) { devFinger(SDL_FINGERUP, d.upX, d.upY); d.upAt = -1; }
    if (d.dragLeft > 0) {
        const float t = 1.f - float(d.dragLeft - 1) / float(std::max(1, d.dragN));
        devFinger(SDL_FINGERMOTION, d.dx0 + (d.dx1 - d.dx0) * t, d.dy0 + (d.dy1 - d.dy0) * t);
        if (--d.dragLeft == 0) devFinger(SDL_FINGERUP, d.dx1, d.dy1);
    }
    if (d.holdLeft > 0) { held |= d.holdMask; d.holdLeft--; }
    while (!d.q.empty() && d.q.front().at <= d.frame) {
        const std::string s = d.q.front().line;
        d.q.pop_front();
        float a = 0, b = 0, c = 0, e4 = 0, f5 = 0;
        unsigned long long m = 0;
        char file[512];
        if (sscanf(s.c_str(), "tap %f %f", &a, &b) == 2) {
            devFinger(SDL_FINGERDOWN, a, b);
            d.upAt = d.frame + 4; d.upX = a; d.upY = b;
        } else if (sscanf(s.c_str(), "down %f %f", &a, &b) == 2) devFinger(SDL_FINGERDOWN, a, b);
        else if (sscanf(s.c_str(), "move %f %f", &a, &b) == 2) devFinger(SDL_FINGERMOTION, a, b);
        else if (sscanf(s.c_str(), "up %f %f", &a, &b) == 2) devFinger(SDL_FINGERUP, a, b);
        else if (sscanf(s.c_str(), "drag %f %f %f %f %f", &a, &b, &c, &e4, &f5) == 5) {
            devFinger(SDL_FINGERDOWN, a, b);
            d.dx0 = a; d.dy0 = b; d.dx1 = c; d.dy1 = e4;
            d.dragN = d.dragLeft = std::max(1, int(f5));
        } else if (sscanf(s.c_str(), "pad %llu", &m) == 1) { down |= m; held |= m; ui.setPadMode(true); }
        else if (sscanf(s.c_str(), "hold %llu %f", &m, &a) == 2) {
            down |= m; held |= m; ui.setPadMode(true);
            d.holdMask = m; d.holdLeft = std::max(0, int(a) - 1);
        } else if (s.rfind("shot ", 0) == 0) d.shot = devPath(s.substr(5));
        else if (s.rfind("items ", 0) == 0) { d.items = devPath(s.substr(6)); d.itemsIn = 3; uiNamesOn = true; }
        else if (s.rfind("stats ", 0) == 0) {
            if (FILE* out = fopen(devPath(s.substr(6)).c_str(), "w")) {
                const struct mallinfo mi = mallinfo();
                fprintf(out, "frames %ld mean %.3f worst %.3f over16 %ld over33 %ld work %.3f heap %d arena %d time %.1f\n", d.n,
                        d.n ? d.sum / d.n : 0.0, d.worst, d.over16, d.over33, d.worstWork, int(mi.uordblks), int(mi.arena), time);
                fclose(out);
            }
            d.n = d.over16 = d.over33 = 0;
            d.sum = d.worst = d.worstWork = 0;
        } else if (sscanf(s.c_str(), "frames %511s %f", file, &a) == 2) {
            if (d.series) fclose(d.series);
            d.series = fopen(devPath(file).c_str(), "w");
            d.seriesLeft = int(a);
        } else if (s == "selftest stop") { for (bool& on : selfTestOn_) on = false; }
        else if (sscanf(s.c_str(), "selftest %f", &a) == 1) {
            // (as Settings' toggle does: the tick starts the one switched on, and stops a test whose switch is off)
            const int lv = std::clamp(int(a), 0, 2);
            for (int k = 0; k < 4; k++) selfTestOn_[k] = k == lv;
        }
        else if (s.rfind("log ", 0) == 0) logLine("devctl: " + s.substr(4));
        else if (s.rfind("pending ", 0) == 0) {   // a file sent as <it>.gfnew swapped in at the close, as an update's (Updater::finishPending)
            const std::string list = std::string(Store::Dir) + "/updates/pending.txt";
            mkdir((std::string(Store::Dir) + "/updates").c_str(), 0777);
            if (FILE* f = fopen(list.c_str(), "ab")) { fprintf(f, "%s\n", s.substr(8).c_str()); fclose(f); }
            logLine("devctl: pending " + s.substr(8));
        } else if (s == "relaunch") {   // Songate closes and starts again (a new .nro swapped in first)
            logLine("devctl: relaunch");
            relaunch = true;
            running = false;
        }
        else if (s.rfind("hitches ", 0) == 0) {
            if (FILE* out = fopen(devPath(s.substr(8)).c_str(), "w")) {
                for (const DevCon::Hitch& h : d.hitches) {
                    fprintf(out, "%.2f %.1f work %.1f prof", double(h.t), double(h.ms), double(h.work));
                    for (float p : h.prof) fprintf(out, " %.1f", double(p));
                    fprintf(out, " view %d sheet %d canvas %d kb %d playing %d step %s\n", h.view, h.sheet, int(h.canvas), int(h.kb), int(h.playing), h.step);
                }
                fclose(out);
            }
            d.hitches.clear();
            d.skipStat = 2;
        }
        else if (s.rfind("done ", 0) == 0) {
            if (FILE* out = fopen(devPath(s.substr(5)).c_str(), "w")) { fprintf(out, "done %ld %.2f\n", d.frame, time); fclose(out); }
        }
    }
}

void App::devConsoleFrame(double interval, float workMs) {
    if (!devCon_) return;
    DevCon& d = *devCon_;
    const double ms = interval * 1000;
    if (d.skipStat > 0) d.skipStat--;   // (the frames after a shot or an items file: the card's write, not the app's)
    else {
        d.n++;
        d.sum += ms;
        d.worst = std::max(d.worst, ms);
        d.worstWork = std::max(d.worstWork, double(workMs));
        d.over16 += ms > 18.0;   // (a frame a little over 16.7 ms is the vsync's jitter, not a missed refresh)
        d.over33 += ms > 33.3;
        if (ms > 18.0 && d.hitches.size() < 4000) {
            DevCon::Hitch h{};
            h.t = float(time); h.ms = float(ms); h.work = workMs;
            for (int i = 0; i < 8; i++) h.prof[i] = float(profFrame_[i]);
            h.view = int(route.view); h.sheet = int(sheet.page); h.canvas = canvas; h.kb = skb_.open; h.playing = isPlaying();
            h.step = updSlow_ ? updSlow_ : "";
            d.hitches.push_back(h);
        }
    }
    if (d.series) {
        fprintf(d.series, "%.3f %.3f\n", ms, double(workMs));
        if (--d.seriesLeft <= 0) { fclose(d.series); d.series = nullptr; }
    }
    if (!d.items.empty() && --d.itemsIn <= 0) {
        const std::string tmp = d.items + ".new";
        if (FILE* out = fopen(tmp.c_str(), "w")) {
            char label[320];
            snprintf(label, sizeof label, "scale %.3f size %d %d pos %.2f playing %d vol %.3f track %s", 1.0, css::W, css::H,
                     player.loaded() ? player.position() : -1.0, int(isPlaying()), double(store.volume),
                     now.track >= 0 ? lib.tracks[size_t(now.track)].title.c_str() : "-");
            ui.audit(label, out);
            for (const auto& t : g.said)
                fprintf(out, "%s %.0f %.0f %.0f %.0f %d|%s\n", t.layer ? "TOP" : "TEXT", double(t.x), double(t.y), double(t.w), double(t.h),
                        int(t.cut), t.text.c_str());
            fclose(out);
            remove(d.items.c_str());
            rename(tmp.c_str(), d.items.c_str());
        }
        d.items.clear();
        uiNamesOn = false;
        d.skipStat = 2;
    }
    if (!d.shot.empty()) {
        int w = 1280, h = 720;
        SDL_GetRendererOutputSize(renderer_, &w, &h);
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24)) {
            if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGB24, s->pixels, s->pitch) == 0) {
                const std::string tmp = d.shot + ".new";
                SDL_SaveBMP(s, tmp.c_str());
                remove(d.shot.c_str());
                rename(tmp.c_str(), d.shot.c_str());
            }
            SDL_FreeSurface(s);
        }
        d.shot.clear();
        d.skipStat = 2;
    }
}
#endif
