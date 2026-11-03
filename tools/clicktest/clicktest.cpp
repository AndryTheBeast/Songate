/*
 * Songate · tools/clicktest/clicktest.cpp
 * Dev only, never in the app: the small test for session 10. Two jobs, on the Switch and on the PC.
 *
 * Sync: two devices play a stream of chirps, one a second, each its own sweep, and one microphone hears both (analyze.py).
 * The stream is a stand-in for a song: it is continuous and starts at an agreed moment on a clock shared by round-trips. The PC
 * pings, so Windows' firewall asks nothing; the server's clock is the shared one. Free: nothing corrects it, so the true drift
 * shows. Then corrected: each device trims its playback rate, at most 500 ppm, to follow the shared clock.
 *
 * Discovery: on the console, can a homebrew app join mDNS, be seen by an iPhone as an AirPlay receiver, and be sent audio? It asks for
 * no encryption, so no key of Apple's is involved, answers iOS through to RECORD, and plays nothing: what arrives is logged
 * (sdmc:/switch/clicktest/airplay.txt) and the audio packets kept (rtp.bin), for tools/clicktest/rtpcheck.py to decode on the PC.
 *
 *   clicktest pc <server ip> [free seconds] [corrected seconds]    records mic.wav and pings.csv here
 *   clicktest listen <seconds>                                     only records mic.wav: the app is playing the chirps itself
 *   clicktest solo <seconds> [buffer] [shift]                       the PC alone: its chirps against its callbacks (solo.py)
 *   clicktest server                                               the PC standing in for the console, to prove the test itself
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>
#include <cstdarg>
#include <sys/stat.h>
#include <fcntl.h>
#include <algorithm>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include "airport_key.inc"   // git-ignored; this test only, never Songate
using Sock = int;
static void closeSock(Sock s) { close(s); }
static double nowUs() { return armTicksToNs(armGetSystemTick()) / 1000.0; }
static void nonblock(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
using Sock = SOCKET;
static void closeSock(Sock s) { closesocket(s); }
static double nowUs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1e6 / double(f.QuadPart);
}
static void nonblock(Sock s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
#endif

static const int Rate = 48000, Buffer = 2048, Port = 47811;
static const int Period = Rate, ChirpLen = 960;   // one chirp a second, 20 ms long
static float chirp[ChirpLen];

static void makeChirp(bool up) {
    double phase = 0;
    for (int i = 0; i < ChirpLen; i++) {
        double x = double(i) / (ChirpLen - 1), hz = up ? 1500 + 2000 * x : 7000 - 3000 * x;   // bands apart: an up and a down sweep in one band answer each other's filter
        phase += 2 * M_PI * hz / Rate;
        chirp[i] = float(0.4 * sin(phase) * (0.5 - 0.5 * cos(2 * M_PI * x)));
    }
}

/** A least-squares line through (frame, time): one callback's wake-up is late or early, their line is not. */
struct Fit {
    static const int N = 1024;
    double f[N], t[N], slope = 1e6 / Rate, lastF = 0, lastT = 0;
    int n = 0, at = 0;
    void add(double frame, double us) {
        f[at] = frame; t[at] = us; at = (at + 1) % N; if (n < N) n++;
        double mf = 0, mt = 0;
        for (int i = 0; i < n; i++) { mf += f[i] - frame; mt += t[i] - us; }
        mf /= n; mt /= n;
        double sff = 0, sft = 0;
        for (int i = 0; i < n; i++) { double a = f[i] - frame - mf; sff += a * a; sft += a * (t[i] - us - mt); }
        if (n >= 128 && sff > 0) slope = sft / sff;
        lastF = frame; lastT = us + mt - slope * mf;   // the line's height at this frame
    }
    double time(double frame) const { return lastT + (frame - lastF) * slope; }
};

// shared between the main loop and the audio callback
static std::mutex offsetLock;
static double offsetAt = 0, offsetA = 0, offsetSlope = 0;   // server clock - this clock, at offsetAt, and its slope; 0 on the server
static std::atomic<double> startAt{ 0 };                    // the stream's first sample, on the server's clock
static std::atomic<int> corrected{ 0 };
static std::atomic<float> errorNow{ 0 }, trimNow{ 0 };
static std::atomic<unsigned> gaps{ 0 };

static Fit fit;
static double frames = 0, pos = 0, trimValue = 0, integral = 0, lastCall = 0;
static bool running = false;

static void audio(void*, Uint8* bytes, int len) {
    int16_t* out = reinterpret_cast<int16_t*>(bytes);
    int n = len / 4;
    double now = nowUs();
    if (lastCall && now - lastCall > 3e6 * n / Rate) gaps++;
    lastCall = now;
    fit.add(frames, now);

    static double oAt = 0, oA = 0, oS = 0;
    if (offsetLock.try_lock()) { oAt = offsetAt; oA = offsetA; oS = offsetSlope; offsetLock.unlock(); }
    double local = fit.time(frames), shared = local + oA + (local - oAt) * oS, begin = startAt.load();

    if (begin > 0) {
        double want = (shared - begin) * Rate / 1e6;
        if (!running) { pos = want; running = true; }
        double err = (want - pos) / Rate;
        errorNow = float(err);
        if (corrected.load()) {
            integral += err * n / Rate;
            if (integral > 0.05) integral = 0.05;
            if (integral < -0.05) integral = -0.05;
            trimValue = err / 8 + integral / 480;
            if (trimValue > 500e-6) trimValue = 500e-6;
            if (trimValue < -500e-6) trimValue = -500e-6;
        } else trimValue = 0;
        trimNow = float(trimValue);
    }
    for (int i = 0; i < n; i++) {
        float v = 0;
        if (running) {
            if (pos >= 0) {
                double p = fmod(pos, Period);
                if (p < ChirpLen - 1) { int k = int(p); float w = float(p - k); v = chirp[k] * (1 - w) + chirp[k + 1] * w; }
            }
            pos += 1 + trimValue;
        }
        out[i * 2] = out[i * 2 + 1] = int16_t(v * 32767);
    }
    frames += n;
}

static SDL_AudioDeviceID openOut() {
    SDL_AudioSpec want{}, got{};
    want.freq = Rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = Buffer; want.callback = audio;
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (d) SDL_PauseAudioDevice(d, 0);
    return d;
}

struct Ping { uint32_t magic, seq; double t1, t2, t3, start; int32_t mode, run; };
static const uint32_t Magic = 0x53474354;

#ifdef __SWITCH__
static FILE* logFile = nullptr;
static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    va_list a; va_start(a, fmt); char b[640]; vsnprintf(b, sizeof b, fmt, a); va_end(a);
    printf("%s\n", b); consoleUpdate(nullptr);
    if (logFile) { fprintf(logFile, "%.3f %s\n", nowUs() / 1e6, b); fflush(logFile); }
}
#else
#define say(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#endif

// ---- the server: the shared clock, answers pings, plays

static bool leave() {
#ifdef __SWITCH__
    static PadState pad;
    static bool once = false;
    if (!once) { padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad); once = true; }
    padUpdate(&pad);
    return !appletMainLoop() || (padGetButtonsDown(&pad) & HidNpadButton_Plus);
#else
    return false;
#endif
}

static int server() {
    makeChirp(false);
    Sock s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me{};
    me.sin_family = AF_INET; me.sin_port = htons(Port); me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) { say("bind failed"); return 1; }
    SDL_AudioDeviceID dev = openOut();
    if (!dev) { say("audio failed: %s", SDL_GetError()); return 1; }
    say("server up, waiting for the PC. Plus ends it.");
    int run = 0;
    double nextSay = 0;
    while (!leave()) {
        pollfd p{};
#ifdef __SWITCH__
        p.fd = s; p.events = POLLIN;
        int r = poll(&p, 1, 20);
#else
        p.fd = s; p.events = POLLRDNORM;
        int r = WSAPoll(&p, 1, 20);
#endif
        if (r > 0) {
            Ping m;
            sockaddr_in from{};
            socklen_t len = sizeof from;
            int got = int(recvfrom(s, reinterpret_cast<char*>(&m), sizeof m, 0, reinterpret_cast<sockaddr*>(&from), &len));
            double t2 = nowUs();
            if (got == int(sizeof m) && m.magic == Magic) {
                if (m.run != run) {   // a new run: the device opened again, as a new session of listening would
                    run = m.run;
                    SDL_CloseAudioDevice(dev);
                    startAt = 0; corrected = 0; running = false; frames = pos = trimValue = integral = lastCall = 0; fit.n = fit.at = 0; fit.slope = 1e6 / Rate;
                    dev = openOut();
                    say("run %d", run);
                }
                if (m.start > 0) startAt = m.start;
                corrected = m.mode;
                m.t2 = t2; m.t3 = nowUs();
                m.mode = 0; m.start = t2;
                sendto(s, reinterpret_cast<const char*>(&m), sizeof m, 0, reinterpret_cast<sockaddr*>(&from), len);
            }
        }
        double t = nowUs();
        if (t > nextSay) {
            nextSay = t + 5e6;
            say("err %.2f ms trim %.0f ppm mode %d gaps %u rate %.2f", errorNow.load() * 1e3, trimNow.load() * 1e6, corrected.load(), gaps.load(), 1e6 / fit.slope);
        }
    }
    SDL_CloseAudioDevice(dev);
    closeSock(s);
    return 0;
}

// ---- the PC: pings, plays, records

#ifndef __SWITCH__
static std::vector<int16_t> mic;
static std::atomic<size_t> micAt{ 0 };
static std::atomic<bool> recording{ false };
static std::atomic<bool> micQuit{ false };
static std::atomic<unsigned> micBreaks{ 0 };
static std::vector<std::pair<size_t, double>> micStamps;   // (sample index, its time in us on the performance counter): the solo test
/** The microphone raw: Windows' voice processing cancels what this PC plays and moves the stream, so SDL's capture cannot be used. */
static void micThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient2* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (FAILED(hr)) { say("mic open failed %08lx", hr); return; }
    AudioClientProperties props{};
    props.cbSize = sizeof props;
    props.eCategory = AudioCategory_Media;
    props.Options = AUDCLNT_STREAMOPTIONS_RAW;
    hr = client->SetClientProperties(&props);
    say("mic raw mode: %s (%08lx)", SUCCEEDED(hr) ? "yes" : "NO", hr);
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; f.nChannels = 1; f.nSamplesPerSec = Rate; f.wBitsPerSample = 32;
    f.nBlockAlign = 4; f.nAvgBytesPerSec = Rate * 4;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000, 0, &f, nullptr);
    if (FAILED(hr)) { say("mic init failed %08lx", hr); return; }
    client->GetService(IID_PPV_ARGS(&cap));
    client->Start();
    while (!micQuit) {
        UINT32 packet = 0;
        cap->GetNextPacketSize(&packet);
        if (!packet) { Sleep(3); continue; }
        BYTE* data; UINT32 n; DWORD flags;
        UINT64 qpc = 0;
        if (FAILED(cap->GetBuffer(&data, &n, &flags, nullptr, &qpc))) break;
        if (recording && micStamps.size() < micStamps.capacity()) micStamps.push_back({ micAt.load(), double(qpc) / 10.0 });
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) micBreaks++;
        if (recording) {
            const float* in = reinterpret_cast<const float*>(data);
            size_t at = micAt;
            for (UINT32 i = 0; i < n && at < mic.size(); i++) {
                float v = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : in[i] * 32767;
                mic[at++] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            micAt = at;
        }
        cap->ReleaseBuffer(n);
    }
    client->Stop();
    cap->Release(); client->Release(); dev->Release(); en->Release();
    CoUninitialize();
}
static void writeWav(const char* path, const int16_t* d, size_t n) {
    FILE* f = fopen(path, "wb");
    uint32_t bytes = uint32_t(n * 2), v;
    uint16_t h;
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); h = 1; fwrite(&h, 2, 1, f); h = 1; fwrite(&h, 2, 1, f);
    v = Rate; fwrite(&v, 4, 1, f); v = Rate * 2; fwrite(&v, 4, 1, f); h = 2; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f); fwrite(d, 2, n, f);
    fclose(f);
}

struct Sample { double t, rtt, off; };

static int pcSide(const char* ip, double freeS, double fixedS, int run) {
    makeChirp(true);
    Sock s = socket(AF_INET, SOCK_DGRAM, 0);
    nonblock(s);
    sockaddr_in to{};
    to.sin_family = AF_INET; to.sin_port = htons(Port); inet_pton(AF_INET, ip, &to.sin_addr);

    double seconds = 16 + freeS + fixedS;
    mic.assign(size_t(seconds * Rate), 0);
    std::thread micWorker(micThread);
    SDL_AudioDeviceID dev = openOut();
    if (!dev) { say("audio failed: %s", SDL_GetError()); return 1; }

    FILE* csv = fopen("pings.csv", "w");
    fprintf(csv, "t,rtt_ms,offset_ms\n");
    std::vector<Sample> all;
    uint32_t seq = 0, answers = 0;
    double begin = 0, firstAnswer = 0, nextPing = 0, nextSay = 0, ending = 0;
    bool started = false;
    int mode = 0;
    while (true) {
        double t = nowUs();
        if (t >= nextPing) {
            nextPing = t + 200e3;
            Ping m{ Magic, ++seq, nowUs(), 0, 0, begin, mode, run };
            sendto(s, reinterpret_cast<const char*>(&m), sizeof m, 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        }
        Ping m;
        int got2;
        while ((got2 = recv(s, reinterpret_cast<char*>(&m), sizeof m, 0)) == int(sizeof m)) {
            double t4 = nowUs();
            if (m.magic != Magic) continue;
            double rtt = (t4 - m.t1) - (m.t3 - m.t2), off = ((m.t2 - m.t1) + (m.t3 - t4)) / 2;
            all.push_back({ t4, rtt, off });
            answers++;
            if (!firstAnswer) { firstAnswer = t4; say("first answer: rtt %.2f ms", rtt / 1e3); }
            fprintf(csv, "%.0f,%.3f,%.3f\n", t4, rtt / 1e3, off / 1e3);
            // the last 30 s: the quick round-trips only, a line through them
            double oldest = t4 - 30e6, least = 1e12;
            size_t from = all.size();
            while (from > 0 && all[from - 1].t >= oldest) { from--; least = fmin(least, all[from].rtt); }
            double n = 0, mt = 0, mo = 0;
            for (size_t i = from; i < all.size(); i++) if (all[i].rtt <= least + 1500) { mt += all[i].t - t4; mo += all[i].off; n++; }
            mt /= n; mo /= n;
            double stt = 0, sto = 0;
            for (size_t i = from; i < all.size(); i++) if (all[i].rtt <= least + 1500) { double a = all[i].t - t4 - mt; stt += a * a; sto += a * (all[i].off - mo); }
            double slope = (n >= 20 && stt > 0) ? sto / stt : 0;
            std::lock_guard<std::mutex> l(offsetLock);
            offsetAt = t4 + mt; offsetA = mo; offsetSlope = slope;
        }
        t = nowUs();
        if (firstAnswer && !started && t - firstAnswer > 8e6) {
            std::unique_lock<std::mutex> l(offsetLock);
            double serverNow = t + offsetA + (t - offsetAt) * offsetSlope;
            l.unlock();
            begin = serverNow + 4e6;
            startAt = begin;
            started = true;
            recording = true;
            ending = t + (4 + freeS + fixedS) * 1e6;
            say("start agreed: 4 s from now, after %u answers", answers);
        }
        if (started && mode == 0 && t > ending - fixedS * 1e6) { mode = 1; corrected = 1; say("corrected from here"); }
        if (t > nextSay) {
            nextSay = t + 5e6;
            float rtt = all.empty() ? 0 : float(all.back().rtt / 1e3);
            say("err %.2f ms trim %.0f ppm mode %d gaps %u rate %.2f rtt %.1f slope %.2f ppm", errorNow.load() * 1e3, trimNow.load() * 1e6, mode, gaps.load(), 1e6 / fit.slope, rtt, offsetSlope * 1e6);
        }
        if (started && t > ending) break;
        Sleep(1);
    }
    fclose(csv);
    SDL_CloseAudioDevice(dev);
    micQuit = true;
    micWorker.join();
    writeWav("mic.wav", mic.data(), micAt);
    say("done: %.1f s recorded, %u mic breaks, %u answers of %u", double(micAt) / Rate, micBreaks.load(), answers, seq);
    closeSock(s);
    return 0;
}

/** Only the microphone, for the app itself playing chirps (Settings > Devices, dev). */
/**
 * The PC alone (session 11: is the 10 ms step between launches Windows' engine period?): a chirp at each whole second of the stream,
 * opened as Songate opens its output (SDL, 48 kHz, its buffer; the driver by SDL_AUDIODRIVER); each callback's frame and time kept,
 * and the microphone with Windows' own time of each packet. solo.py then finds each chirp and says how late it sounded against the
 * callbacks' line. Writes cb.csv, micstamps.csv and mic.wav here.
 */
static std::vector<std::pair<double, double>> soloCb;   // (frames before this callback, its time in us)
static int soloShift = 0;   // the control: each chirp this many samples later, so the reading must grow by exactly that
static void soloAudio(void*, Uint8* bytes, int len) {
    int16_t* out = reinterpret_cast<int16_t*>(bytes);
    const int n = len / 4;
    if (soloCb.size() < soloCb.capacity()) soloCb.push_back({ frames, nowUs() });
    for (int i = 0; i < n; i++) {
        const double p = fmod(frames + i - soloShift + Period, Period);
        const float v = p < ChirpLen ? chirp[int(p)] : 0.f;
        out[i * 2] = out[i * 2 + 1] = int16_t(v * 32767);
    }
    frames += n;
}

static int solo(double seconds, int buffer, int shift) {
    soloShift = shift;
    makeChirp(true);
    mic.assign(size_t((seconds + 2) * Rate), 0);
    micStamps.reserve(200000);
    soloCb.reserve(200000);
    std::thread micWorker(micThread);
    Sleep(500);
    recording = true;
    SDL_AudioSpec want{}, got{};
    want.freq = Rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = Uint16(buffer); want.callback = soloAudio;
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (!d) { say("no output: %s", SDL_GetError()); micQuit = true; micWorker.join(); return 1; }
    say("output: driver %s, %d Hz, buffer %d frames", SDL_GetCurrentAudioDriver(), got.freq, int(got.samples));
    SDL_PauseAudioDevice(d, 0);
    Sleep(DWORD(seconds * 1000));
    SDL_CloseAudioDevice(d);
    Sleep(400);
    recording = false;
    micQuit = true;
    micWorker.join();
    writeWav("mic.wav", mic.data(), micAt);
    if (FILE* f = fopen("cb.csv", "w")) { for (auto& c : soloCb) fprintf(f, "%.0f,%.3f\n", c.first, c.second); fclose(f); }
    if (FILE* f = fopen("micstamps.csv", "w")) { for (auto& m : micStamps) fprintf(f, "%zu,%.3f\n", m.first, m.second); fclose(f); }
    say("done: %zu callbacks, %.1f s of microphone, %zu stamps, %u breaks", soloCb.size(), double(micAt) / Rate, micStamps.size(), micBreaks.load());
    return 0;
}

static int listenOnly(double seconds) {
    mic.assign(size_t(seconds * Rate), 0);
    micStamps.reserve(200000);   // Windows' time of each packet too (session 11: Songate's own timed start, against the same counter)
    std::thread micWorker(micThread);
    recording = true;
    Sleep(DWORD(seconds * 1000));
    micQuit = true;
    micWorker.join();
    writeWav("mic.wav", mic.data(), micAt);
    if (FILE* f = fopen("micstamps.csv", "w")) { for (auto& m : micStamps) fprintf(f, "%zu,%.3f\n", m.first, m.second); fclose(f); }
    say("done: %.1f s recorded, %u mic breaks", double(micAt) / Rate, micBreaks.load());
    return 0;
}
#endif

// ---- discovery: mDNS on the console, as an AirPlay 1 receiver would be

#ifdef __SWITCH__
static void name(std::vector<uint8_t>& o, const char* labels) {   // labels separated by \1, so a dot may be in one
    const char* p = labels;
    while (*p) {
        const char* e = strchr(p, 1);
        size_t n = e ? size_t(e - p) : strlen(p);
        o.push_back(uint8_t(n));
        o.insert(o.end(), p, p + n);
        p += n; if (*p) p++;
    }
    o.push_back(0);
}
static void put16(std::vector<uint8_t>& o, unsigned v) { o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
static void put32(std::vector<uint8_t>& o, unsigned v) { put16(o, v >> 16); put16(o, v & 0xffff); }
static void record(std::vector<uint8_t>& o, const char* owner, unsigned type, unsigned cls, unsigned ttl, const std::vector<uint8_t>& data) {
    name(o, owner); put16(o, type); put16(o, cls); put32(o, ttl); put16(o, unsigned(data.size())); o.insert(o.end(), data.begin(), data.end());
}
/** RSA with the AirPort key: the answer to iOS's challenge, and the session's AES key out of its wrapping. Nothing of either is logged. */
struct Airport {
    mbedtls_pk_context pk;
    mbedtls_rsa_context* rsa = nullptr;
    mbedtls_aes_context aes;
    uint8_t iv[16] = {};
    bool haveAes = false;
    static int rng(void*, unsigned char* o, size_t n) { randomGet(o, n); return 0; }
    bool open() {
        mbedtls_pk_init(&pk);
        mbedtls_aes_init(&aes);
        if (mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(kAirportKey), sizeof kAirportKey, nullptr, 0) != 0) return false;
        rsa = mbedtls_pk_rsa(pk);
        return mbedtls_rsa_complete(rsa) == 0;
    }
    static std::vector<uint8_t> unbase64(std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '=' || s.back() == '\r')) s.pop_back();
        while (s.size() % 4) s += '=';
        std::vector<uint8_t> out(s.size());
        size_t n = 0;
        if (mbedtls_base64_decode(out.data(), out.size(), &n, reinterpret_cast<const unsigned char*>(s.data()), s.size()) != 0) n = 0;
        out.resize(n);
        return out;
    }
    std::string answer(const std::string& challenge, uint32_t ip, const uint8_t* hw) {
        std::vector<uint8_t> c = unbase64(challenge);
        if (c.size() != 16) return "";
        uint8_t data[32] = {};
        memcpy(data, c.data(), 16);
        memcpy(data + 16, &ip, 4);
        memcpy(data + 20, hw, 6);
        const size_t size = mbedtls_rsa_get_len(rsa);
        std::vector<uint8_t> em(size, 0xFF), sig(size);
        em[0] = 0; em[1] = 1;
        em[size - 33] = 0;
        memcpy(em.data() + size - 32, data, 32);
        if (mbedtls_rsa_private(rsa, rng, nullptr, em.data(), sig.data()) != 0) return "";
        std::string out(size * 2, 0);
        size_t n = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out.size(), &n, sig.data(), size) != 0) return "";
        out.resize(n);
        while (!out.empty() && out.back() == '=') out.pop_back();
        return out;
    }
    bool unwrap(const std::string& wrapped, const std::string& ivText) {
        std::vector<uint8_t> w = unbase64(wrapped), i = unbase64(ivText);
        if (w.size() != mbedtls_rsa_get_len(rsa) || i.size() != 16) return false;
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
        uint8_t key[32];
        size_t n = 0;
        const int rc = mbedtls_rsa_rsaes_oaep_decrypt(rsa, rng, nullptr, MBEDTLS_RSA_PRIVATE, nullptr, 0, &n, w.data(), key, sizeof key);
        if (rc != 0 || n != 16) return false;
        mbedtls_aes_setkey_dec(&aes, key, 128);
        memcpy(iv, i.data(), 16);
        haveAes = true;
        return true;
    }
    void decrypt(uint8_t* p, size_t n) {
        const size_t whole = n & ~size_t(15);
        if (!haveAes || !whole) return;
        uint8_t v[16];
        memcpy(v, iv, 16);
        mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, whole, v, p, p);
    }
};

static const char* RaopService = "_raop\1_tcp\1local";
// which receiver it says it is: 0 offers encryption as any AirPlay 1 receiver does (nothing is decrypted: what iOS then asks for is the
// answer); 1 no encryption, in the older words
static int variant = 0;
static std::string instanceName() { return variant == 0 ? "AABBCC112235@Songate Test 3\1_raop\1_tcp\1local" : "AABBCC112236@Songate Test 4\1_raop\1_tcp\1local"; }
static const char* Host = "songate-test\1local";
static const int RtspPort = 5005;

static std::vector<uint8_t> announcement(uint32_t ip) {
    std::vector<uint8_t> o, d;
    put16(o, 0); put16(o, 0x8400); put16(o, 0); put16(o, 4); put16(o, 0); put16(o, 0);
    const std::string inst = instanceName();
    const char* Instance = inst.c_str();
    name(d, Instance);
    record(o, RaopService, 12, 1, 4500, d);
    d.clear(); put16(d, 0); put16(d, 0); put16(d, RtspPort); name(d, Host);
    record(o, Instance, 33, 0x8001, 120, d);
    d.clear();
    static const std::vector<const char*> offers = { "txtvers=1", "ch=2", "cn=0,1", "et=0,1", "ek=1", "sr=44100", "ss=16", "sv=false", "da=true", "pw=false", "md=0,2", "tp=TCP,UDP", "vn=65537", "vs=105.1", "sf=0x4", "am=SongateTest" };
    static const std::vector<const char*> plain = { "txtvers=1", "ch=2", "cn=0,1", "et=0", "sv=false", "da=true", "sr=44100", "ss=16", "pw=false", "vn=3", "tp=UDP", "sm=false", "ek=0" };
    for (const char* t : variant == 0 ? offers : plain) {
        d.push_back(uint8_t(strlen(t))); d.insert(d.end(), t, t + strlen(t));
    }
    record(o, Instance, 16, 0x8001, 4500, d);
    d.assign(reinterpret_cast<uint8_t*>(&ip), reinterpret_cast<uint8_t*>(&ip) + 4);
    record(o, Host, 1, 0x8001, 120, d);
    return o;
}
static std::vector<uint8_t> question() {
    std::vector<uint8_t> o;
    put16(o, 0); put16(o, 0); put16(o, 1); put16(o, 0); put16(o, 0); put16(o, 0);
    name(o, RaopService); put16(o, 12); put16(o, 1);
    return o;
}
static bool holds(const uint8_t* d, size_t n, const char* s) {
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(d + i, s, k)) return true;
    return false;
}

static int discovery() {
    uint32_t ip = uint32_t(gethostid());
    in_addr a{}; a.s_addr = ip;
    say("this console: %s", inet_ntoa(a));
    Sock s = socket(AF_INET, SOCK_DGRAM, 0);
    int on = 1, rc;
    rc = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on); say("SO_REUSEADDR %d", rc);
    rc = setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on); say("SO_REUSEPORT %d", rc);
    sockaddr_in me{};
    me.sin_family = AF_INET; me.sin_port = htons(5353); me.sin_addr.s_addr = htonl(INADDR_ANY);
    rc = bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me); say("bind 5353: %d", rc);
    ip_mreq g{};
    g.imr_multiaddr.s_addr = inet_addr("224.0.0.251"); g.imr_interface.s_addr = ip;
    rc = setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &g, sizeof g); say("IP_ADD_MEMBERSHIP %d", rc);
    in_addr mi{}; mi.s_addr = ip;
    rc = setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &mi, sizeof mi); say("IP_MULTICAST_IF %d", rc);
    u_char ttl = 255;
    rc = setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl); say("IP_MULTICAST_TTL %d", rc);
    nonblock(s);

    Sock l = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in tcp{};
    tcp.sin_family = AF_INET; tcp.sin_port = htons(RtspPort); tcp.sin_addr.s_addr = htonl(INADDR_ANY);
    rc = bind(l, reinterpret_cast<sockaddr*>(&tcp), sizeof tcp); say("bind tcp %d: %d", RtspPort, rc);
    rc = listen(l, 4); say("listen %d", rc);
    nonblock(l);

    sockaddr_in group{};
    group.sin_family = AF_INET; group.sin_port = htons(5353); group.sin_addr.s_addr = inet_addr("224.0.0.251");
    std::vector<uint8_t> ann = announcement(ip), ask = question();
    double next = 0, seenRaop = 0;
    int heard = 0, asked = 0, sent = 0, others = 0, conns = 0;
    static Airport airport;
    say("key opened: %s", airport.open() ? "yes" : "NO");
    const uint8_t hw[6] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, variant == 0 ? uint8_t(0x35) : uint8_t(0x36) };
    Sock rtsp = -1, media[3];
    std::string rtspIn;
    int udpCount[3] = {}, peerTiming = 0;
    uint32_t peerAddr = 0;
    long long audioBytes = 0;
    size_t dumped = 0;
    double nextCount = 0;
    for (int i = 0; i < 3; i++) {
        media[i] = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in m{};
        m.sin_family = AF_INET; m.sin_port = htons(uint16_t(6000 + i)); m.sin_addr.s_addr = htonl(INADDR_ANY);
        rc = bind(media[i], reinterpret_cast<sockaddr*>(&m), sizeof m); say("bind udp %d: %d", 6000 + i, rc);
        nonblock(media[i]);
    }
    FILE* dump = fopen("sdmc:/switch/clicktest/rtp.bin", "wb");
    say("On the iPhone: play music, AirPlay, pick Songate Test %d. Plus ends it.", 3 + variant);
    while (!leave()) {
        double t = nowUs();
        if (t > next) {
            next = t + (sent < 6 ? 1e6 : 5e6);
            int r = int(sendto(s, ann.data(), ann.size(), 0, reinterpret_cast<sockaddr*>(&group), sizeof group));
            if (sent < 3) say("announce sent: %d bytes", r);
            sendto(s, ask.data(), ask.size(), 0, reinterpret_cast<sockaddr*>(&group), sizeof group);
            sent++;
            if (sent % 6 == 0) say("packets heard %d, asked about AirPlay %d, AirPlay answers from others %d, connections %d", heard, asked, others, conns);
        }
        uint8_t d[1500];
        sockaddr_in from{};
        socklen_t len = sizeof from;
        int n;
        while ((n = int(recvfrom(s, d, sizeof d, 0, reinterpret_cast<sockaddr*>(&from), &len))) > 0) {
            if (from.sin_addr.s_addr == ip) continue;
            heard++;
            if (heard <= 5) say("heard %d bytes from %s", n, inet_ntoa(from.sin_addr));
            bool answer = n > 3 && (d[2] & 0x80), raop = holds(d, size_t(n), "_raop");
            if (raop && answer) { if (others++ < 3) say("an AirPlay device answered: %s", inet_ntoa(from.sin_addr)); seenRaop = t; }
            if (raop && !answer) {
                if (asked++ < 8) say("asked about AirPlay by %s", inet_ntoa(from.sin_addr));
                sendto(s, ann.data(), ann.size(), 0, reinterpret_cast<sockaddr*>(&group), sizeof group);
            }
        }
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        Sock c = accept(l, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (c >= 0) {
            conns++;
            say("CONNECTION from %s", inet_ntoa(peer.sin_addr));
            if (rtsp >= 0) closeSock(rtsp);
            nonblock(c);
            rtsp = c;
            rtspIn.clear();
        }
        if (rtsp >= 0) {
            char buf[4096];
            int got = int(recv(rtsp, buf, sizeof buf, 0));
            if (got > 0) rtspIn.append(buf, size_t(got));
            else if (got == 0) { say("iOS closed the connection"); closeSock(rtsp); rtsp = -1; }
            for (;;) {
                const size_t end = rtspIn.find("\r\n\r\n");
                if (end == std::string::npos) break;
                const std::string head = rtspIn.substr(0, end + 2);
                size_t bodyLen = 0, cl = head.find("Content-Length:");
                if (cl != std::string::npos) bodyLen = size_t(atoi(head.c_str() + cl + 15));
                if (rtspIn.size() < end + 4 + bodyLen) break;
                const std::string body = rtspIn.substr(end + 4, bodyLen);
                rtspIn.erase(0, end + 4 + bodyLen);
                const std::string method = head.substr(0, head.find(' '));
                std::string shown = head, seen = body;
                for (char& ch : shown) if (ch == '\r' || ch == '\n') ch = '|';
                for (char& ch : seen) if (ch == '\r' || ch == '\n') ch = '|'; else if (ch < 32 || ch > 126) ch = '.';
                for (const char* secret : { "a=rsaaeskey:", "a=aesiv:" }) {
                    const size_t s = seen.find(secret);
                    if (s != std::string::npos) { const size_t from = s + strlen(secret); seen.replace(from, seen.find('|', from) - from, "<hidden>"); }
                }
                { const size_t s = shown.find("Apple-Challenge:"); if (s != std::string::npos) shown.replace(s + 16, shown.find('|', s) - s - 16, " <hidden>"); }
                say("RTSP %.420s", shown.c_str());
                if (!body.empty()) say("BODY %d bytes: %.500s", int(body.size()), seen.c_str());
                size_t q = head.find("CSeq:");
                std::string cseq = q == std::string::npos ? "1" : head.substr(q + 5, head.find('\r', q) - q - 5);
                std::string reply = "RTSP/1.0 200 OK\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n";
                {
                    const size_t ch = head.find("Apple-Challenge:");
                    if (ch != std::string::npos) {
                        const std::string answered = airport.answer(head.substr(ch + 17, head.find('\r', ch) - ch - 17), ip, hw);
                        say("challenge answered: %s", answered.empty() ? "NO" : "yes");
                        if (!answered.empty()) reply += "Apple-Response: " + answered + "\r\n";
                    }
                }
                if (method == "ANNOUNCE") {
                    auto field = [&](const char* k) {
                        const size_t s = body.find(k);
                        if (s == std::string::npos) return std::string();
                        const size_t from = s + strlen(k);
                        return body.substr(from, body.find_first_of("\r\n", from) - from);
                    };
                    const std::string wrapped = field("a=rsaaeskey:");
                    say("audio key %s", wrapped.empty() ? "not sent: unencrypted" : airport.unwrap(wrapped, field("a=aesiv:")) ? "unwrapped" : "FAILED to unwrap");
                }
                if (method == "OPTIONS") reply += "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n";
                else if (method == "SETUP") {
                    size_t tp = head.find("timing_port=");
                    if (tp != std::string::npos) peerTiming = atoi(head.c_str() + tp + 12);
                    peerAddr = peer.sin_addr.s_addr;
                    reply += "Session: 1\r\nAudio-Jack-Status: connected\r\nTransport: RTP/AVP/UDP;unicast;mode=record;server_port=6000;control_port=6001;timing_port=6002\r\n";
                } else if (method == "RECORD") reply += "Audio-Latency: 11025\r\nSession: 1\r\n";
                else if (method != "ANNOUNCE" && method != "FLUSH" && method != "SET_PARAMETER" && method != "GET_PARAMETER" && method != "TEARDOWN") reply = "RTSP/1.0 404 Not Found\r\nServer: AirTunes/105.1\r\nCSeq:" + cseq + "\r\n";
                reply += "\r\n";
                send(rtsp, reply.data(), reply.size(), 0);
            }
        }
        for (int i = 0; i < 3; i++) {
            uint8_t pk[2048];
            sockaddr_in src{};
            socklen_t sl = sizeof src;
            int n2;
            while ((n2 = int(recvfrom(media[i], pk, sizeof pk, 0, reinterpret_cast<sockaddr*>(&src), &sl))) > 0) {
                if (udpCount[i]++ < 4) {
                    char hex[100];
                    int at = 0;
                    for (int k = 0; k < std::min(n2, 28); k++) at += snprintf(hex + at, sizeof hex - size_t(at), "%02x", pk[k]);
                    say("UDP %d: %d bytes, type %02x: %s", 6000 + i, n2, pk[1] & 0x7f, hex);
                }
                if (i == 0) {
                    audioBytes += n2;
                    if (n2 > 12) airport.decrypt(pk + 12, size_t(n2 - 12));
                    if (dump && dumped < 16 * 1024 * 1024) {
                        const uint16_t len = uint16_t(n2);
                        fwrite(&len, 2, 1, dump);
                        fwrite(pk, 1, size_t(n2), dump);
                        dumped += size_t(n2) + 2;
                    }
                }
                if (i == 2 && (pk[1] & 0x7f) == 0x52 && n2 >= 32) {   // a timing request: answered as a receiver's clock would
                    uint8_t out[32];
                    memcpy(out, pk, 8);
                    out[1] = 0xD3;
                    memcpy(out + 8, pk + 24, 8);
                    const uint64_t ms = uint64_t(nowUs());
                    for (int k = 0; k < 8; k++) out[16 + k] = out[24 + k] = uint8_t(ms >> (56 - 8 * k));
                    sendto(media[2], out, sizeof out, 0, reinterpret_cast<sockaddr*>(&src), sl);
                }
            }
        }
        if (nowUs() > nextCount) {
            nextCount = nowUs() + 5e6;
            if (udpCount[0] + udpCount[1] + udpCount[2]) say("packets: audio %d (%lld bytes), control %d, timing %d", udpCount[0], audioBytes, udpCount[1], udpCount[2]);
            if (dump) fflush(dump);
        }
        svcSleepThread(3'000'000);
    }
    if (dump) fclose(dump);
    say("end: heard %d, asked %d, others %d, connections %d, last AirPlay answer %.0f s ago", heard, asked, others, conns, seenRaop ? (nowUs() - seenRaop) / 1e6 : -1);
    closeSock(l);
    closeSock(s);
    return 0;
}

int main(int, char**) {
    consoleInit(nullptr);
    socketInitializeDefault();
    mkdir("sdmc:/switch/clicktest", 0777);
    SDL_Init(SDL_INIT_AUDIO);
    printf("clicktest (dev)\n\n  A  sync test (the PC starts it)\n  X  AirPlay: Songate Test 3 (offers encryption)\n  Y  AirPlay: Songate Test 4 (none, older words)\n  +  leave\n");
    consoleUpdate(nullptr);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    int r = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_A) { logFile = fopen("sdmc:/switch/clicktest/sync.txt", "w"); say("sync test"); r = server(); break; }
        if (down & (HidNpadButton_X | HidNpadButton_Y)) {
            variant = (down & HidNpadButton_X) ? 0 : 1;
            logFile = fopen(variant == 0 ? "sdmc:/switch/clicktest/airplay3.txt" : "sdmc:/switch/clicktest/airplay4.txt", "w");
            say("discovery test, variant %d", variant);
            r = discovery();
            break;
        }
        if (down & HidNpadButton_Plus) break;
        svcSleepThread(16'000'000);
    }
    if (logFile) fclose(logFile);
    SDL_Quit();
    socketExit();
    consoleExit(nullptr);
    return r;
}
#else
int main(int argc, char** argv) {
    SDL_SetMainReady();
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    SDL_Init(SDL_INIT_AUDIO);
    timeBeginPeriod(1);
    int r = 1;
    if (argc >= 3 && !strcmp(argv[1], "pc")) r = pcSide(argv[2], argc > 3 ? atof(argv[3]) : 90, argc > 4 ? atof(argv[4]) : 180, argc > 5 ? atoi(argv[5]) : 0);
    else if (argc >= 2 && !strcmp(argv[1], "server")) r = server();
    else if (argc >= 3 && !strcmp(argv[1], "listen")) r = listenOnly(atof(argv[2]));
    else if (argc >= 3 && !strcmp(argv[1], "solo")) r = solo(atof(argv[2]), argc > 3 ? atoi(argv[3]) : Buffer, argc > 4 ? atoi(argv[4]) : 0);
    else printf("clicktest pc <server ip> [free s] [corrected s] | clicktest server\n");
    SDL_Quit();
    return r;
}
#endif
