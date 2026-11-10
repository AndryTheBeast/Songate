/*
 * Songate · together.cpp
 * See together.hpp.
 */
#include "together.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "identity.hpp"

namespace together {

namespace {
constexpr size_t kMaxListName = 256;   // a Handoff's playlist name, in bytes
constexpr size_t kMaxMembers = 1024;   // Members' names, in bytes

bool hexText(const std::string& s) {
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return !s.empty();
}

bool hexOr0(const std::string& s) {
    if (s == "-") return true;
    if (s.size() < 16 || s.size() > 64) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool number(const std::string& s, double& out) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    char* end = nullptr;
    out = strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(out);
}

std::vector<std::string> split(const std::string& s, char by) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t e = s.find(by, a);
        if (e == std::string::npos) { out.push_back(s.substr(a)); return out; }
        out.push_back(s.substr(a, e - a));
        a = e + 1;
    }
}

/** A plan as text and back. These numbers steer the audio thread, so what comes from the other device is held to what a planner can make. */
void planText(const automix::Plan& p, std::string& out) {
    char b[64];
    auto d = [&](double v) { snprintf(b, sizeof b, "%.6f ", v); out += b; };
    auto i = [&](int v) { snprintf(b, sizeof b, "%d ", v); out += b; };
    for (double v : { p.startOut, p.offset, p.length, p.loopFrom, p.loopTo, p.echoAt, p.echoDelay, p.speed, p.easeIn, p.stretchFrom, p.bassCut, p.cross, p.lpFrom, p.cutFade,
                      double(p.bpm), double(p.inLevel), double(p.outPow), double(p.gateLevel), double(p.gateDuck), double(p.gateLen), double(p.join.seconds), double(p.join.shiftSec) }) d(v);
    for (int v : { p.beats, p.keyDistance, p.gate, p.gateEvery, p.gateBars, int(p.model), p.join.loopBars, p.join.beats, p.join.shiftBeats, int(p.join.preset), int(p.join.volume),
                   int(p.join.eq), int(p.join.filter), int(p.join.effect), p.beatmatched ? 1 : 0, p.beatmatchable ? 1 : 0, p.off ? 1 : 0, p.onBars ? 1 : 0, p.structured ? 1 : 0,
                   p.drop ? 1 : 0, p.cut ? 1 : 0, p.keyClash ? 1 : 0, p.join.beatmatch ? 1 : 0 }) i(v);
    out.pop_back();
}

bool planFrom(const std::string& s, automix::Plan& p) {
    const std::vector<std::string> f = split(s, ' ');
    if (f.size() != 22 + 23) return false;
    double n[45];
    for (size_t i = 0; i < 45; i++) if (!number(f[i], n[i]) || std::fabs(n[i]) > 1e6) return false;
    for (size_t i = 22; i < 45; i++) if (n[i] != std::floor(n[i])) return false;
    for (size_t i = 36; i < 45; i++) if (n[i] != 0 && n[i] != 1) return false;
    auto within = [](double v, double lo, double hi) { return v >= lo && v <= hi; };
    if (!within(n[0], 0, 86400) || !within(n[1], 0, 86400) || !within(n[2], 0, 600) || !within(n[3], -1, 86400) || !within(n[4], -1, 86400) || !within(n[5], 0, 1) || !within(n[6], 0, 10) ||
        !within(n[7], 0.5, 2) || !within(n[8], 0, 600) || !within(n[9], 0, 86400) || !within(n[10], 0, 1) || !within(n[11], 0, 1) || !within(n[12], 0, 24000) || !within(n[13], 0, 10) ||
        !within(n[14], 0, 1000) || !within(n[15], 0, 4) || !within(n[16], 0, 16) || !within(n[17], 0, 4) || !within(n[18], 0, 4) || !within(n[19], 0, 64) || !within(n[20], 0, 600) || !within(n[21], -600, 600))
        return false;
    if (!within(n[22], 0, 4096) || !within(n[23], -1, 12) || !within(n[24], 0, 2) || !within(n[25], 1, 64) || !within(n[26], 0, 64) || !within(n[27], 0, 255) || !within(n[28], 0, 2) ||
        !within(n[29], 0, 4096) || !within(n[30], -4096, 4096) || !within(n[31], 0, 7) || !within(n[32], 0, 4) || !within(n[33], 0, 5) || !within(n[34], 0, 3) || !within(n[35], 0, 1))
        return false;
    p = automix::Plan{};
    double* ds[] = { &p.startOut, &p.offset, &p.length, &p.loopFrom, &p.loopTo, &p.echoAt, &p.echoDelay, &p.speed, &p.easeIn, &p.stretchFrom, &p.bassCut, &p.cross, &p.lpFrom, &p.cutFade };
    for (int i = 0; i < 14; i++) *ds[i] = n[i];
    float* fs[] = { &p.bpm, &p.inLevel, &p.outPow, &p.gateLevel, &p.gateDuck, &p.gateLen, &p.join.seconds, &p.join.shiftSec };
    for (int i = 0; i < 8; i++) *fs[i] = float(n[14 + i]);
    p.beats = int(n[22]); p.keyDistance = int(n[23]); p.gate = int(n[24]); p.gateEvery = int(n[25]); p.gateBars = int(n[26]);
    p.model = automix::Model(int(n[27]));
    p.join.loopBars = int(n[28]); p.join.beats = int(n[29]); p.join.shiftBeats = int(n[30]);
    p.join.preset = automix::Preset(int(n[31])); p.join.volume = automix::Volume(int(n[32])); p.join.eq = automix::Eq(int(n[33]));
    p.join.filter = automix::Filter(int(n[34])); p.join.effect = automix::Effect(int(n[35]));
    p.beatmatched = n[36] != 0; p.beatmatchable = n[37] != 0; p.off = n[38] != 0; p.onBars = n[39] != 0; p.structured = n[40] != 0;
    p.drop = n[41] != 0; p.cut = n[42] != 0; p.keyClash = n[43] != 0; p.join.beatmatch = n[44] != 0;
    return true;
}

}  // namespace

std::vector<uint8_t> pack(const Message& m) {
    char head[120];
    snprintf(head, sizeof head, "t%c\t%d\t%c\t%lld\t%d\t%.6f\t%.6f", char(m.kind), m.flag ? 1 : 0, m.what ? m.what : '-', (long long)m.serial, m.song, m.at, m.position);
    std::string out = head;
    if ((m.kind == Kind::Handoff && !m.list.empty() && m.list.size() <= kMaxListName) ||
        ((m.kind == Kind::Members || m.kind == Kind::Delay) && !m.list.empty() && m.list.size() <= kMaxMembers)) {   // a first line "@<the name in hex>"
        static const char hex[] = "0123456789abcdef";
        out += "\n@";
        for (unsigned char c : m.list) { out += hex[c >> 4]; out += hex[c & 15]; }
    }
    if (m.kind == Kind::Queue || m.kind == Kind::Handoff)
        for (const Song& s : m.songs) {
            char d[32];
            snprintf(d, sizeof d, "%.3f", s.duration);
            out += "\n" + (s.exact.empty() ? std::string("-") : s.exact) + " " + (s.loose.empty() ? std::string("-") : s.loose) + " " + d;
            if (!s.server.empty() && !s.sid.empty() && s.sid.size() <= kMaxSongId) {   // a server's song: its print and its id (in hex)
                static const char hx[] = "0123456789abcdef";
                out += " " + s.server + " ";
                for (unsigned char c : s.sid) { out += hx[c >> 4]; out += hx[c & 15]; }
            }
        }
    if (m.kind == Kind::Join) {
        out += "\n";
        planText(m.plan, out);
    }
    if (m.kind == Kind::Have) {
        out += "\n";
        for (uint8_t h : m.have) out += h ? '1' : '0';
    }
    if (m.kind == Kind::Print && m.flag && !m.print.empty()) {
        static const char hex[] = "0123456789abcdef";
        out += "\n";
        for (uint8_t v : m.print) { out += hex[v >> 4]; out += hex[v & 15]; }
    }
    return std::vector<uint8_t>(out.begin(), out.end());
}

bool unpack(const std::vector<uint8_t>& bytes, Message& out) {
    if (bytes.size() < 2 || bytes.size() > 4 * 1024 * 1024 || bytes[0] != 't') return false;
    out = Message{};
    const std::string text(bytes.begin(), bytes.end());
    const size_t nl = text.find('\n');
    const std::vector<std::string> f = split(text.substr(0, nl), '\t');
    if (f.size() != 7 || f[0].size() != 2 || f[1].size() != 1 || f[2].size() != 1) return false;
    switch (f[0][1]) {
    case 'I': case 'A': case 'Q': case 'H': case 'P': case 'R': case 'G': case 'S': case 'Z': case 'C': case 'Y': case 'J': case 'O': case 'K': case 'L': case 'F': case 'W': case 'M': case 'T': case 'D': break;
    default: return false;
    }
    out.kind = Kind(f[0][1]);
    if (f[1] != "0" && f[1] != "1") return false;
    out.flag = f[1] == "1";
    if (f[2] != "-") {
        const char w = f[2][0];
        if (w != AskPause && w != AskPlay && w != AskSeek && w != AskSong && w != AskOrder && w != AskWhere) return false;
        out.what = w;
    }
    double serial, song;
    if (!number(f[3], serial) || !number(f[4], song) || !number(f[5], out.at) || !number(f[6], out.position)) return false;
    if (serial < 0 || serial > 9e15 || song < -1 || song >= double(kMaxSongs) || song != std::floor(song) || out.at < 0 || out.position < 0) return false;
    out.serial = int64_t(serial);
    out.song = int(song);
    const bool list = out.kind == Kind::Queue || out.kind == Kind::Handoff;
    if (out.kind == Kind::Print) {
        if (out.song < 0) return false;
        if (nl == std::string::npos) return true;
        const std::string body = text.substr(nl + 1);
        if (!out.flag || body.empty() || body.size() % 2 || body.size() > 2 * size_t(kPrintSteps + 2 * kPrintReachSteps)) return false;
        out.print.reserve(body.size() / 2);
        for (size_t i = 0; i < body.size(); i += 2) {
            int v = 0;
            for (char c : { body[i], body[i + 1] }) {
                if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
                else return false;
            }
            out.print.push_back(uint8_t(v));
        }
        return true;
    }
    if (out.kind == Kind::Members || out.kind == Kind::Delay) {   // "@<the text in hex>", or nothing
        if (nl == std::string::npos) return true;
        const std::string h = text.substr(nl + 1);
        if (h.size() < 3 || h[0] != '@' || h.size() % 2 == 0 || (h.size() - 1) / 2 > kMaxMembers) return false;
        for (size_t i = 1; i < h.size(); i += 2) {
            int v = 0;
            for (char c : { h[i], h[i + 1] }) {
                if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
                else return false;
            }
            out.list += char(v);
        }
        return true;
    }
    const bool bodied = list || out.kind == Kind::Have || out.kind == Kind::Join;
    if (!bodied) return nl == std::string::npos;
    if (nl == std::string::npos) return list;   // an empty list
    const std::string body = text.substr(nl + 1);
    if (out.kind == Kind::Join) return out.song >= 0 && planFrom(body, out.plan);
    if (out.kind == Kind::Have) {
        if (body.size() > kMaxSongs) return false;
        out.have.reserve(body.size());
        for (char c : body) {
            if (c != '0' && c != '1') return false;
            out.have.push_back(c == '1');
        }
        return true;
    }
    std::vector<std::string> lines = split(body, '\n');
    if (out.kind == Kind::Handoff && !lines.empty() && !lines[0].empty() && lines[0][0] == '@') {   // the playlist's name
        const std::string& h = lines[0];
        if (h.size() < 3 || h.size() % 2 == 0 || (h.size() - 1) / 2 > kMaxListName) return false;
        for (size_t i = 1; i < h.size(); i += 2) {
            int v = 0;
            for (char c : { h[i], h[i + 1] }) {
                if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
                else return false;
            }
            out.list += char(v);
        }
        lines.erase(lines.begin());
        if (lines.size() == 1 && lines[0].empty()) lines.clear();   // a name and no songs
    }
    if (lines.size() > kMaxSongs) return false;
    out.songs.reserve(lines.size());
    for (const std::string& l : lines) {
        const std::vector<std::string> p = split(l, ' ');
        Song s;
        if ((p.size() != 3 && p.size() != 5) || !hexOr0(p[0]) || !hexOr0(p[1]) || !number(p[2], s.duration) || s.duration < 0) return false;
        if (p[0] != "-") s.exact = p[0];
        if (p[1] != "-") s.loose = p[1];
        if (p.size() == 5) {   // a server's song
            if (p[3] == "-" || !hexOr0(p[3]) || p[3].size() > 64 || p[4].empty() || p[4].size() % 2 || p[4].size() > kMaxSongId * 2 || !hexText(p[4])) return false;   // (its id in hex, of any length: a Subsonic server's ids are short numbers)
            s.server = p[3];
            for (size_t k = 0; k < p[4].size(); k += 2) s.sid += char(std::stoi(p[4].substr(k, 2), nullptr, 16));
        }
        out.songs.push_back(std::move(s));
    }
    return true;
}

std::vector<int> match(const std::vector<Song>& theirs, const std::vector<Held>& mine) {
    std::unordered_map<std::string, int> exact;
    std::unordered_multimap<std::string, int> loose;
    exact.reserve(mine.size());
    loose.reserve(mine.size());
    for (int i = 0; i < int(mine.size()); i++) {
        if (!mine[size_t(i)].song.exact.empty()) exact.emplace(mine[size_t(i)].song.exact, i);
        if (!mine[size_t(i)].song.loose.empty()) loose.emplace(mine[size_t(i)].song.loose, i);
    }
    std::vector<int> out(theirs.size(), -1);
    for (size_t i = 0; i < theirs.size(); i++) {
        const Song& s = theirs[i];
        if (!s.exact.empty())
            if (auto it = exact.find(s.exact); it != exact.end()) { out[i] = mine[size_t(it->second)].track; continue; }
        if (s.loose.empty()) continue;
        auto range = loose.equal_range(s.loose);
        for (auto it = range.first; it != range.second; ++it)
            if (Identity::sameLength(mine[size_t(it->second)].song.duration, s.duration)) { out[i] = mine[size_t(it->second)].track; break; }
    }
    return out;
}

double printFrom(double duration) {
    // early in the song (one starting quietly has music by then), and under 30 s, where a skip decodes from the start
    if (duration < kPrintSeconds + 2 * kPrintReach + 1) return -1;
    return std::clamp(duration * 0.3, kPrintReach, std::max(kPrintReach, std::min(20.0, duration - kPrintSeconds - kPrintReach - 0.5)));
}

std::vector<uint8_t> loudness(const int16_t* pcm, size_t frames, int rate, int channels) {
    std::vector<uint8_t> out;
    if (rate <= 0 || channels <= 0) return out;
    // Each step's energy, of the sound with its lows taken out (a high-pass at 200 Hz): a bass note's wave is longer than a step, and
    // its own ripple, not its loudness, filled a step's energy, landing differently in two files half a step apart
    std::vector<double> step;
    const double hp = std::exp(-2 * 3.14159265358979 * 200.0 / rate);
    double last = 0, high = 0;
    for (size_t k = 0;; k++) {
        // each step's own frames, by time (88.2 a step at 44.1 kHz): the steps keep to the same clock at any rate
        const size_t a = size_t(std::llround(double(k) * kPrintStep * rate)), b = size_t(std::llround(double(k + 1) * kPrintStep * rate));
        if (b > frames || b <= a) break;
        double sum = 0;
        for (size_t i = a; i < b; i++) {
            double v = 0;
            for (int c = 0; c < channels; c++) v += pcm[i * size_t(channels) + size_t(c)];
            v /= channels * 32768.0;
            high = hp * (high + v - last);
            last = v;
            sum += high * high;
        }
        step.push_back(sum / double(b - a));
    }
    // over 8 ms, moved a step at a time; dB, 0.4 dB a unit from -90 dB (0) to +12 (255), silence 0
    static const double w[4] = { 1, 2, 2, 1 };
    for (size_t k = 0; k < step.size(); k++) {
        double e = 0, ws = 0;
        for (int j = -1; j <= 2; j++)
            if (const long i = long(k) + j; i >= 0 && i < long(step.size())) { e += w[j + 1] * step[size_t(i)]; ws += w[j + 1]; }
        e /= ws;
        const double db = e > 1e-12 ? 10 * std::log10(e) : -120;
        out.push_back(uint8_t(std::clamp(std::lround((db + 90) * 2.5), 0L, 255L)));
    }
    return out;
}

namespace {
/** Where the loudness rises, step to step: the onsets, which two encodings of one song share. Spread over 4 ms on each side: a drum's
 *  attack is shorter than a step, and two files half a step apart put it in different steps, so unspread they would hardly fit. */
std::vector<double> onsets(const std::vector<uint8_t>& e) {
    std::vector<double> d(e.empty() ? 0 : e.size() - 1), out(d.size(), 0);
    for (size_t i = 0; i + 1 < e.size(); i++) d[i] = std::max(0, int(e[i + 1]) - int(e[i]));
    static const double w[5] = { 1, 2, 3, 2, 1 };
    for (size_t i = 0; i < d.size(); i++)
        for (int k = -2; k <= 2; k++)
            if (const long j = long(i) + k; j >= 0 && j < long(d.size())) out[i] += w[k + 2] * d[size_t(j)];
    return out;
}
}  // namespace

Aligned align(const std::vector<uint8_t>& theirs, const std::vector<uint8_t>& mine) {
    Aligned a;
    const int R = kPrintReachSteps;
    if (theirs.size() != size_t(kPrintSteps) || mine.size() != size_t(kPrintSteps + 2 * R)) return a;
    // music in both: a quarter of the window at least above -60 dB
    auto heard = [](const std::vector<uint8_t>& e) { return int(std::count_if(e.begin(), e.end(), [](uint8_t v) { return v > 75; })); };
    if (heard(theirs) < kPrintSteps / 4 || heard(mine) < kPrintSteps / 4) return a;
    const std::vector<double> x = onsets(theirs), y = onsets(mine);
    const int n = int(x.size());
    double mx = 0, sx = 0;
    for (double v : x) mx += v;
    mx /= n;
    for (double v : x) sx += (v - mx) * (v - mx);
    if (sx <= 0) return a;
    std::vector<double> r(size_t(2 * R + 1), -1);
    for (int at = 0; at <= 2 * R; at++) {
        double my = 0, sy = 0, sxy = 0;
        for (int i = 0; i < n; i++) my += y[size_t(at + i)];
        my /= n;
        for (int i = 0; i < n; i++) {
            const double v = y[size_t(at + i)] - my;
            sy += v * v;
            sxy += (x[size_t(i)] - mx) * v;
        }
        if (sy > 0) r[size_t(at)] = sxy / std::sqrt(sx * sy);
    }
    int best = 0;
    for (int i = 1; i <= 2 * R; i++) if (r[size_t(i)] > r[size_t(best)]) best = i;
    a.score = r[size_t(best)];
    a.next = -1;
    for (int i = 0; i <= 2 * R; i++) if (std::abs(i - best) > 10) a.next = std::max(a.next, r[size_t(i)]);
    // between steps: the parabola through the best and its neighbours
    double frac = 0;
    if (best > 0 && best < 2 * R) {
        const double l = r[size_t(best - 1)], c = r[size_t(best)], h = r[size_t(best + 1)], den = l - 2 * c + h;
        if (den < 0) frac = std::clamp(0.5 * (l - h) / den, -0.5, 0.5);
    }
    // clear: a strong fit, well above any other, and not at the edge of the reach
    a.clear = a.score >= 0.5 && a.next < a.score * 0.75 && best > 0 && best < 2 * R;
    a.shift = a.clear ? (best - R + frac) * kPrintStep : 0;
    return a;
}

int firstHeld(const std::vector<uint8_t>& have, int from) {
    const int n = int(have.size());
    if (n == 0) return -1;
    if (from < 0) from = 0;
    for (int i = 0; i < n; i++) {
        const int at = (from + i) % n;
        if (have[size_t(at)]) return at;
    }
    return -1;
}

}  // namespace together
