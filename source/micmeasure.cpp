/*
 * Songate · micmeasure.cpp
 * See micmeasure.hpp.
 */
#include "micmeasure.hpp"

#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "cores.hpp"
#include "platform.hpp"

namespace micm {
namespace {

constexpr double kPi = 3.14159265358979323846;

double steadyNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/** The sweep's phase at x seconds: 1.5 kHz rising to 3.5 kHz over the chirp. */
double phaseAt(double x, bool down = false) {
    return down ? 2 * kPi * (3500 * x - 1000 * x * x / kChirp) : 2 * kPi * (1500 * x + 1000 * x * x / kChirp);
}
double hann(double x) { return 0.5 - 0.5 * std::cos(2 * kPi * x / kChirp); }

/** What the microphone gives, and when (each block's last sample, on the shared clock). Written by SDL's capture thread only, read after
 *  the device is closed. */
struct Tape {
    std::vector<int16_t> pcm;
    std::vector<std::pair<double, double>> stamps;   // samples so far, the shared time then
    std::function<double(double)> toShared;
};

void SDLCALL onAudio(void* self, Uint8* stream, int len) {
    auto* t = static_cast<Tape*>(self);
    const auto* s = reinterpret_cast<const int16_t*>(stream);
    const size_t n = size_t(len) / 2;
    if (t->pcm.size() + n > t->pcm.capacity()) return;   // (the room was made beforehand: nothing is allocated here)
    t->pcm.insert(t->pcm.end(), s, s + n);
    if (t->stamps.size() < t->stamps.capacity()) t->stamps.push_back({ double(t->pcm.size()), t->toShared(steadyNow()) });
}

/** The device's own microphone, never a Bluetooth one (the user, 2026-11-05: "make sure that when a speaker is connected the microphone
 *  used is the devices own and not the speaker's one"): a speaker's or a headset's microphone is somewhere else, and on Windows taking it
 *  turns the headset to its call mode. By name: a Bluetooth-looking one is never taken; a built-in-looking one, or (Android) the one named
 *  as the phone, comes first. "": none of its own (only Bluetooth ones). */
std::string ownMic(std::string& seen) {
    auto lower = [](std::string s) { for (char& c : s) c = char(tolower((unsigned char)c)); return s; };
    static const char* const away[] = { "hands-free", "handsfree", "headset", "bluetooth", "ag audio", "airpods", "buds", "earbuds", "headphone", "jbl", "bose", "sony wh", "sony wf", "beats", "speaker" };
    static const char* const own[] = { "microphone array", "internal", "built-in", "builtin", "realtek", "intel", "conexant", "cirrus", "microphone" };
    const std::string me = lower(plat::deviceName());
    std::string best;
    int bestScore = -1;
    const int n = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < n; i++) {
        const char* nm = SDL_GetAudioDeviceName(i, 1);
        if (!nm) continue;
        const std::string name = nm, l = lower(name);
        seen += (seen.empty() ? "" : "; ") + name;
        bool bluetooth = false;
        for (const char* a : away) if (l.find(a) != std::string::npos) bluetooth = true;
        if (bluetooth) continue;
        int score = 1;
        for (const char* o : own) if (l.find(o) != std::string::npos) score = std::max(score, 10);
        if (!me.empty() && l == me) score = 50;   // Android names the phone's own microphone after the phone
        if (score > bestScore) { bestScore = score; best = name; }
    }
    return best;
}

}  // namespace

double chirpAt(double x, int slot) { return x < 0 || x >= kChirp ? 0 : std::sin(phaseAt(x, slot % 2 == 1)) * hann(x); }

bool available() {
#ifdef __SWITCH__
    return false;   // no microphone
#else
    return SDL_GetNumAudioDevices(1) > 0;
#endif
}

Result analyze(const std::vector<int16_t>& pcm, const std::vector<std::pair<double, double>>& stamps, double rate, double t0, int slots,
               const std::atomic<bool>* quit) {
    Result r;
    r.done = true;
    r.lateMs.assign(size_t(slots), 0);
    r.heard.assign(size_t(slots), false);
    r.why.assign(size_t(slots), "");
    // sample number → shared time: the line through the blocks' stamps (the microphone's own delay is the same for every chirp)
    const auto& st = stamps;
    if (st.size() < 16 || pcm.size() < size_t(rate)) { r.error = "The microphone gave nothing"; return r; }
    double mi = 0, mt = 0;
    for (const auto& s : st) { mi += s.first; mt += s.second; }
    mi /= double(st.size());
    mt /= double(st.size());
    double sii = 0, sit = 0;
    for (const auto& s : st) { sii += (s.first - mi) * (s.first - mi); sit += (s.first - mi) * (s.second - mt); }
    double b = sii > 0 ? sit / sii : 1 / rate;
    if (!(b > 0.5 / rate && b < 2 / rate)) b = 1 / rate;   // (a fit gone wrong: the nominal rate)
    const double a = mt - b * mi;
    auto timeOf = [&](double i) { return a + b * i; };

    // the matched filter, as tools/clicktest/analyze.py: the chirp and its quarter-turn, so a peak does not depend on which cycle wins
    const int n = int(kChirp * rate);
    std::vector<float> tsUp(static_cast<size_t>(n)), tcUp(static_cast<size_t>(n)), tsDown(static_cast<size_t>(n)), tcDown(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        const double x = i / rate;
        tsUp[size_t(i)] = float(std::sin(phaseAt(x)) * hann(x));
        tcUp[size_t(i)] = float(std::cos(phaseAt(x)) * hann(x));
        tsDown[size_t(i)] = float(std::sin(phaseAt(x, true)) * hann(x));
        tcDown[size_t(i)] = float(std::cos(phaseAt(x, true)) * hann(x));
    }
    const std::vector<float>* ts = &tsUp;
    const std::vector<float>* tc = &tcUp;
    std::vector<float> x(pcm.size());
    for (size_t i = 0; i < x.size(); i++) x[i] = pcm[i] * (1.f / 32768.f);
    auto energy = [&](long long at) {
        double s = 0, c = 0;
        const float* p = x.data() + at;
        for (int i = 0; i < n; i++) { s += p[i] * (*ts)[size_t(i)]; c += p[i] * (*tc)[size_t(i)]; }
        return s * s + c * c;
    };
    bool any = false;
    for (int k = 0; k < slots && !(quit && *quit); k++) {
        std::vector<double> lates;
        std::string why;
        ts = k % 2 ? &tsDown : &tsUp;   // this slot's own sweep
        tc = k % 2 ? &tcDown : &tcUp;
        for (int rd = 0; rd < kRounds; rd++) {
            const double due = chirpTime(t0, slots, k, rd);
            const long long centre = (long long)std::llround((due - a) / b);
            const long long from = std::max(0LL, centre - (long long)(kReach * rate)), to = std::min((long long)x.size() - n - 1, centre + (long long)(kReach * rate));
            if (to - from < n) continue;
            // coarse every 4 samples, then the 8 round the best
            std::vector<double> coarse;
            long long best = from;
            double top = -1;
            for (long long i = from; i <= to; i += 4) {
                const double e = energy(i);
                coarse.push_back(e);
                if (e > top) { top = e; best = i; }
            }
            for (long long i = std::max(from, best - 4); i <= std::min(to, best + 4); i++) {
                const double e = energy(i);
                if (e > top) { top = e; best = i; }
            }
            std::nth_element(coarse.begin(), coarse.begin() + long(coarse.size() / 2), coarse.end());
            const double floor = std::max(1e-12, coarse[coarse.size() / 2]);
            const double ratio = std::sqrt(top / floor), lateMs = (timeOf(double(best)) - due) * 1000;
            char w[48];
            snprintf(w, sizeof w, "%s%.1fx at %+.0f ms", why.empty() ? "" : ", ", ratio, lateMs);
            why += w;
            if (ratio < 4) continue;   // not clear above the room
            lates.push_back(lateMs);
        }
        r.why[size_t(k)] = why;
        if (lates.size() < 2) continue;
        std::sort(lates.begin(), lates.end());
        // the middle one and one beside it agree (Bluetooth wobbles a little more than a cable; a knock or an echo moves one round only)
        const size_t mid = lates.size() / 2;
        const bool agree = (mid > 0 && lates[mid] - lates[mid - 1] <= 20) || (mid + 1 < lates.size() && lates[mid + 1] - lates[mid] <= 20);
        if (!agree) { r.why[size_t(k)] = "the rounds disagree: " + why; continue; }
        r.lateMs[size_t(k)] = lates[lates.size() / 2];
        r.heard[size_t(k)] = true;
        any = true;
    }
    if (!any) r.error = "No chirp was heard: is the microphone on, and the sound up?";
    return r;
}

void Measurer::start(std::function<double(double)> toShared, double t0, int slots) {
    stop();
    {
        std::lock_guard<std::mutex> g(m_);
        ready_ = false;
        result_ = Result{};
    }
    quit_ = false;
    busy_ = true;
    th_ = std::thread([this, toShared, t0, slots] { run(toShared, t0, slots); });
}

void Measurer::stop() {
    quit_ = true;
    if (th_.joinable()) th_.join();
    busy_ = false;
}

bool Measurer::take(Result& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!ready_) return false;
    ready_ = false;
    out = std::move(result_);
    return true;
}

void Measurer::run(std::function<double(double)> toShared, double t0, int slots) {
    cores::moveHere(cores::FileOps);
    Result r;
    r.lateMs.assign(size_t(slots), 0);
    r.heard.assign(size_t(slots), false);
    auto finish = [&] {
        r.done = true;
        std::lock_guard<std::mutex> g(m_);
        result_ = std::move(r);
        ready_ = true;
        busy_ = false;
    };
#ifdef GF_ANDROID
    if (!SDL_AndroidRequestPermission("android.permission.RECORD_AUDIO")) {   // (it waits for the answer: here, never on the main thread)
        r.error = "Songate may not use the microphone: allow it in Android's settings";
        finish();
        return;
    }
#endif
    const double end = chirpTime(t0, slots, slots - 1, kRounds - 1) + kChirp + 0.5;
    Tape tape;
    tape.toShared = toShared;
    tape.pcm.reserve(size_t(48000 * std::max(4.0, end - toShared(steadyNow()) + 2)));
    tape.stamps.reserve(4096);
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = onAudio;
    want.userdata = &tape;
    std::string seen;
    const std::string mic = SDL_GetNumAudioDevices(1) > 0 ? ownMic(seen) : std::string();
    if (SDL_GetNumAudioDevices(1) > 0 && mic.empty()) {   // only Bluetooth ones
        r.error = "This device's only microphone is a Bluetooth one (" + seen + "): measured there it would not be where you sit";
        finish();
        return;
    }
    r.mic = mic.empty() ? std::string("the system's own choice") : mic;
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(mic.empty() ? nullptr : mic.c_str(), 1, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!dev) {
        r.error = std::string("The microphone could not be opened: ") + SDL_GetError();
        finish();
        return;
    }
    if (have.freq != 48000) {   // (the room was made for 48 kHz)
        SDL_LockAudioDevice(dev);
        tape.pcm.reserve(size_t(have.freq * std::max(4.0, end - toShared(steadyNow()) + 2)));
        SDL_UnlockAudioDevice(dev);
    }
    SDL_PauseAudioDevice(dev, 0);
    while (!quit_ && toShared(steadyNow()) < end) SDL_Delay(20);
    SDL_CloseAudioDevice(dev);   // (waits for its thread: the tape is ours from here)
    if (quit_) { finish(); return; }

    const std::string used = r.mic;
    r = analyze(tape.pcm, tape.stamps, have.freq, t0, slots, &quit_);
    r.mic = used;
    finish();
}

}  // namespace micm
