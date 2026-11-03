/* Songate · togethertest.cpp: listening together's messages and matching, attacked alone.
 *   clang++ -std=c++20 -Isource tools/preview/togethertest.cpp source/together.cpp -o togethertest
 * (the same-length rule comes from identity.hpp, inline; no link to identity.cpp is needed.) */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "together.hpp"

using namespace together;
static int passed = 0, failed = 0;
static void check(const char* what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? passed : failed)++;
}
static std::vector<uint8_t> B(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

/* A song for the prints: notes and a kick, each a function of time, so one song can be made at any rate and from any moment; the
 * noise (a hi-hat, and a floor for the other encoding) is made a sample at a time and so differs between two files, as a codec's. */
struct Note { double at, freq, amp, decay; };
static std::vector<Note> song(unsigned seed, bool grid16) {
    std::vector<Note> n;
    double t = 0;
    while (t < 40) {
        seed = seed * 1103515245 + 12345;
        const double r = double((seed >> 8) & 0xffff) / 65536.0;
        if (grid16) t += 0.125;                    // sixteenths at 120 BPM, nothing else: the hardest case
        else t += 0.04 + 0.25 * r;                 // about six notes a second, at no grid
        seed = seed * 1103515245 + 12345;
        const double r2 = double((seed >> 8) & 0xffff) / 65536.0;
        n.push_back({ t, grid16 ? 880 : 110 * std::pow(2.0, int(r2 * 36) / 12.0), grid16 ? 0.4 : 0.15 + 0.35 * r2, grid16 ? 0.03 : 0.08 + 0.3 * r });
    }
    if (!grid16) for (double k = 0.25; k < 40; k += 0.5) n.push_back({ k, 55, 0.6, 0.12 });   // a kick, every beat
    return n;
}
/** `seconds` of the song at `rate`, stereo, from `from` in its own time; `late`: this file's music starts that much later. */
static std::vector<int16_t> render(const std::vector<Note>& notes, int rate, double from, double seconds, double late, double gain, double floorDb, unsigned noiseSeed, double lowPass = 0) {
    const size_t frames = size_t(seconds * rate);
    std::vector<double> mix(frames, 0);
    for (const Note& n : notes) {
        const double start = n.at + late;
        const double end = start + n.decay * 8;
        if (end < from || start > from + seconds) continue;
        const size_t a = size_t(std::max(0.0, std::ceil((start - from) * rate))), b = std::min(frames, size_t(std::max(0.0, (end - from) * rate)));
        for (size_t i = a; i < b; i++) {
            const double t = from + double(i) / rate - start;
            const double env = std::min(1.0, t / 0.003) * std::exp(-t / n.decay);
            mix[i] += n.amp * env * std::sin(2 * M_PI * n.freq * t);
        }
    }
    unsigned sd = noiseSeed;
    const double floorAmp = std::pow(10, floorDb / 20);
    std::vector<int16_t> out(frames * 2);
    const double k = lowPass > 0 ? 1 - std::exp(-2 * M_PI * lowPass / rate) : 1;   // a one-pole low-pass: a codec's lost highs
    double lp = 0;
    for (size_t i = 0; i < frames; i++) {
        sd = sd * 1103515245 + 12345;
        const double noise = (double((sd >> 8) & 0xffff) / 32768.0 - 1) * floorAmp;
        lp += k * (mix[i] - lp);
        const double v = std::clamp((lp * 0.5 + noise) * gain, -1.0, 1.0);
        out[2 * i] = out[2 * i + 1] = int16_t(std::lround(v * 32000));
    }
    return out;
}
/** Both devices' prints of one song from `T`: theirs at 44.1 kHz; mine at 48 kHz, quieter, duller, with a noise floor 40 dB down, its
 *  music `late` later. */
static Aligned twoFiles(const std::vector<Note>& a, const std::vector<Note>& b, double late) {
    const double T = 12;
    const std::vector<int16_t> t = render(a, 44100, T, kPrintSeconds, 0, 1, -200, 1);
    const std::vector<int16_t> m = render(b, 48000, T - kPrintReach, kPrintSeconds + 2 * kPrintReach, late, 0.7, -40, 7, 3000);
    std::vector<uint8_t> pt = loudness(t.data(), t.size() / 2, 44100, 2), pm = loudness(m.data(), m.size() / 2, 48000, 2);
    pt.resize(kPrintSteps);
    pm.resize(size_t(kPrintSteps + 2 * kPrintReachSteps));
    const Aligned r = align(pt, pm);
    printf("      late %+.1f ms: %s, shift %+.2f ms, fit %.2f, next best %.2f\n", late * 1e3, r.clear ? "clear" : "unclear", r.shift * 1e3, r.score, r.next);
    return r;
}

int main() {
    const std::string h1(32, 'a'), h2(32, 'b'), h3(32, 'c'), h4(32, 'd');
    Message m, u, x0;

    m.kind = Kind::Go; m.serial = 41; m.song = 7; m.at = 123456.789012; m.position = 61.5;
    check("a Go comes back the same", unpack(pack(m), u) && u.kind == Kind::Go && u.serial == 41 && u.song == 7 && u.at == 123456.789012 && u.position == 61.5);
    m = Message{}; m.kind = Kind::Ask; m.what = AskSeek; m.position = 12;
    check("an Ask comes back the same", unpack(pack(m), u) && u.kind == Kind::Ask && u.what == AskSeek && u.position == 12);
    m = Message{}; m.kind = Kind::Resume; m.serial = 9; m.song = 3; m.at = 5000.25; m.position = 77.5; m.flag = true;
    check("a Resume comes back the same", unpack(pack(m), u) && u.kind == Kind::Resume && u.serial == 9 && u.song == 3 && u.at == 5000.25 && u.position == 77.5 && u.flag);
    {
        automix::Plan pl;
        pl.startOut = 201.25; pl.offset = 12.5; pl.length = 8; pl.bpm = 128; pl.beats = 16; pl.speed = 1.02; pl.easeIn = 3; pl.stretchFrom = 190; pl.gate = 1; pl.gateBars = 2;
        pl.join.preset = automix::Preset::Blend; pl.join.volume = automix::Volume::Drop; pl.join.eq = automix::Eq::BassBridge; pl.join.effect = automix::Effect::Echo; pl.beatmatched = true; pl.drop = true; pl.keyDistance = 3;
        Message j; j.kind = Kind::Join; j.serial = 4; j.song = 2; j.plan = pl;
        const std::vector<uint8_t> packed = pack(j);
        const bool ok = unpack(packed, u) && u.kind == Kind::Join && u.song == 2;
        const automix::Plan& q = u.plan;
        check("a plan comes back the same", ok && q.startOut == 201.25 && q.offset == 12.5 && q.length == 8 && q.bpm == 128 && q.beats == 16 && q.speed == 1.02 && q.stretchFrom == 190 && q.gate == 1 && q.gateBars == 2 && q.join.preset == automix::Preset::Blend && q.join.volume == automix::Volume::Drop && q.join.eq == automix::Eq::BassBridge && q.join.effect == automix::Effect::Echo && q.beatmatched && q.drop && !q.off && q.keyDistance == 3 && q.model == pl.model && q.gateLevel == pl.gateLevel);
        std::string text(packed.begin(), packed.end());
        const size_t nl = text.find('\n');
        const std::string head = text.substr(0, nl + 1);
        auto bad = [&](int field, const char* value) {
            std::string body = text.substr(nl + 1), out;
            int at = 0;
            size_t a = 0;
            for (;;) {
                const size_t e = body.find(' ', a);
                out += (at == field ? std::string(value) : body.substr(a, e == std::string::npos ? e : e - a));
                if (e == std::string::npos) break;
                out += ' ';
                a = e + 1;
                at++;
            }
            Message r;
            return !unpack(B(head + out), r);
        };
        check("a plan with a speed of 50 is refused", bad(7, "50"));
        check("a plan with a negative start is refused", bad(0, "-3"));
        check("a plan with an unknown preset is refused", bad(31, "9"));
        check("a plan with an unknown effect is refused", bad(35, "2"));
        check("a plan with gate every 0 is refused", bad(25, "0"));
        check("a plan with a flag of 2 is refused", bad(36, "2"));
        check("a plan with a number that is no number is refused", bad(2, "x"));
        check("a plan a field short is refused", !unpack(B(text.substr(0, text.rfind(' '))), x0));
        j.flag = true;
        check("a join says it is Automix's", unpack(pack(j), u) && u.flag);
        j.flag = false;
        check("a crossfade's join says it is not", unpack(pack(j), u) && !u.flag);
        Message noSong; noSong.kind = Kind::Join; noSong.plan = pl;
        check("a plan with no song is refused", !unpack(pack(noSong), x0));
    }
    m = Message{}; m.kind = Kind::Answer; m.flag = true;
    check("a yes comes back the same", unpack(pack(m), u) && u.flag);
    m = Message{}; m.kind = Kind::Who; m.flag = true;
    check("Who: only the starter, comes back the same", unpack(pack(m), u) && u.kind == Kind::Who && u.flag);
    check("Who with a body is refused", !unpack(B("tW\t1\t-\t0\t0\t0\t0\nx"), x0));
    m = Message{}; m.kind = Kind::Who; m.song = 3;
    check("Who carries the starter's order", unpack(pack(m), u) && u.song == 3 && !u.flag);
    m = Message{}; m.kind = Kind::Ask; m.what = AskOrder; m.song = 2;
    check("an order asked for comes back the same", unpack(pack(m), u) && u.what == AskOrder && u.song == 2);
    m = Message{}; m.kind = Kind::Ask; m.what = AskWhere;
    check("where things stand, asked, comes back the same", unpack(pack(m), u) && u.kind == Kind::Ask && u.what == AskWhere);
    m = Message{}; m.kind = Kind::Queue; m.songs = { { h1, h2, 200.5 }, { "", h3, 100 }, { h4, "", 90 } };
    check("a queue comes back the same", unpack(pack(m), u) && u.songs.size() == 3 && u.songs[0].exact == h1 && u.songs[1].exact.empty() && u.songs[1].loose == h3 && u.songs[2].loose.empty() && u.songs[0].duration == 200.5);
    m = Message{}; m.kind = Kind::Queue;
    check("an empty queue is a queue", unpack(pack(m), u) && u.songs.empty());
    // Handoff carries the playlist the queue came from (2026-11-03: an Automix playlist arrived as a plain queue)
    m = Message{}; m.kind = Kind::Handoff; m.song = 1; m.position = 42.5; m.flag = true; m.list = "Night Drive";
    m.songs = { { h1, h2, 200.5 }, { "", h3, 100 } };
    check("a handoff carries its playlist", unpack(pack(m), u) && u.kind == Kind::Handoff && u.list == "Night Drive" && u.songs.size() == 2 && u.songs[0].exact == h1 && u.song == 1 && u.flag);
    m.list = "Caf\xc3\xa9 \xe2\x99\xa5\tline\nbreak @ 100%";
    check("any characters in a playlist's name", unpack(pack(m), u) && u.list == m.list && u.songs.size() == 2);
    m.list.clear();
    check("a handoff with no playlist (an album, older builds)", unpack(pack(m), u) && u.list.empty() && u.songs.size() == 2);
    m.list = "Mood: Happy"; m.songs.clear();
    check("a playlist's name and no songs", unpack(pack(m), u) && u.list == "Mood: Happy" && u.songs.empty());
    m.list = std::string(257, 'x');
    check("a name over 256 bytes is left out, the songs still go", unpack(pack(m), u) && u.list.empty());
    check("a name in a queue (not a handoff) is refused", !unpack(B("tQ\t0\t-\t0\t0\t0\t0\n@4142"), x0));
    check("a name of odd hex is refused", !unpack(B("tO\t0\t-\t0\t0\t0\t0\n@414"), x0));
    check("a name that is not hex is refused", !unpack(B("tO\t0\t-\t0\t0\t0\t0\n@zz"), x0));
    check("an empty name line is refused", !unpack(B("tO\t0\t-\t0\t0\t0\t0\n@"), x0));
    check("a name too long is refused", !unpack(B("tO\t0\t-\t0\t0\t0\t0\n@" + std::string(2 * 257, 'a')), x0));
    m = Message{}; m.kind = Kind::Handoff; m.song = 1; m.position = 42.5; m.flag = true; m.songs = { { h1, h2, 200 }, { "", h3, 100 } };
    check("a handoff comes back the same", unpack(pack(m), u) && u.kind == Kind::Handoff && u.song == 1 && u.position == 42.5 && u.flag && u.songs.size() == 2 && u.songs[1].loose == h3);
    m = Message{}; m.kind = Kind::Have; m.have = { 1, 0, 1 };
    check("Have comes back the same", unpack(pack(m), u) && u.have == std::vector<uint8_t>({ 1, 0, 1 }));
    m = Message{}; m.kind = Kind::Queue; m.songs.assign(3000, Song{ h1, h2, 200 });
    const std::vector<uint8_t> big = pack(m);
    check("3,000 songs fit a link message", big.size() < 1024 * 1024 && unpack(big, u) && u.songs.size() == 3000);

    Message x;
    check("not ours", !unpack(B("xG\t0\t-\t0\t0\t0\t0"), x));
    check("empty", !unpack({}, x));
    check("an unknown kind", !unpack(B("tX\t0\t-\t0\t0\t0\t0"), x));
    check("too few fields", !unpack(B("tG\t0\t-\t0\t0\t0"), x));
    check("too many fields", !unpack(B("tG\t0\t-\t0\t0\t0\t0\t0"), x));
    check("a flag of 2", !unpack(B("tG\t2\t-\t0\t0\t0\t0"), x));
    check("an unknown ask", !unpack(B("tC\t0\tq\t0\t0\t0\t0"), x));
    check("nan", !unpack(B("tG\t0\t-\t0\t0\tnan\t0"), x));
    check("infinity", !unpack(B("tG\t0\t-\t0\t0\tinf\t0"), x));
    check("a huge number", !unpack(B("tG\t0\t-\t0\t0\t1e308\t0"), x));
    check("a negative position", !unpack(B("tG\t0\t-\t0\t0\t0\t-5"), x));
    check("a song out of range", !unpack(B("tG\t0\t-\t0\t99999999\t0\t0"), x));
    check("a song that is no whole number", !unpack(B("tG\t0\t-\t0\t1.5\t0\t0"), x));
    check("a body on a message with none", !unpack(B("tG\t0\t-\t0\t0\t0\t0\nx"), x));
    check("a path instead of a hash", !unpack(B("tQ\t0\t-\t0\t0\t0\t0\n/Music/a.flac - 10"), x));
    check("a path in a handoff", !unpack(B("tO\t0\t-\t0\t0\t0\t0\n/Music/a.flac - 10"), x));
    check("climbing out in a hash", !unpack(B("tQ\t0\t-\t0\t0\t0\t0\n../../../../etc/passwd - 10"), x));
    check("a short hash", !unpack(B("tQ\t0\t-\t0\t0\t0\t0\nabc - 10"), x));
    check("a song line with too many parts", !unpack(B("tQ\t0\t-\t0\t0\t0\t0\n- - 10 x"), x));
    check("Have with other letters", !unpack(B("tH\t0\t-\t0\t0\t0\t0\n01x"), x));
    std::string many = "tH\t0\t-\t0\t0\t0\t0\n" + std::string(kMaxSongs + 1, '1');
    check("Have too long", !unpack(B(many), x));
    many = "tQ\t0\t-\t0\t0\t0\t0\n- - 1";
    for (size_t i = 0; i < kMaxSongs; i++) many += "\n- - 1";
    check("a queue too long", !unpack(B(many), x));
    check("over 4 MB", !unpack(std::vector<uint8_t>(5 * 1024 * 1024, 't'), x));
    int crashes = 0;
    {
        const std::vector<uint8_t> good = pack(m);
        unsigned seed = 12345;
        for (int i = 0; i < 20000; i++) {
            std::vector<uint8_t> v(good.begin(), good.begin() + (good.size() > 200 ? 200 : good.size()));
            for (int k = 0; k < 6; k++) { seed = seed * 1103515245 + 12345; v[seed % v.size()] = uint8_t(seed >> 16); }
            Message r;
            unpack(v, r);
        }
    }
    check("20,000 damaged messages, no crash", crashes == 0);

    std::vector<Held> mine = { { { h1, h2, 200 }, 10 }, { { "", h3, 100 }, 11 }, { { h4, h3, 300 }, 12 } };
    std::vector<int> got = match({ { h1, "", 1 } }, mine);
    check("the same bytes win, whatever the length says", got[0] == 10);
    got = match({ { h2, h3, 101.5 } }, mine);
    check("the same song within 2 s", got[0] == 11);
    got = match({ { h2, h3, 299 } }, mine);
    check("one key, two lengths: the right one", got[0] == 12);
    got = match({ { h2, h3, 150 } }, mine);
    check("the same song 50 s apart is not it", got[0] == -1);
    got = match({ { "", "", 100 } }, mine);
    check("no hashes match nothing", got[0] == -1);
    got = match({}, mine);
    check("an empty list", got.empty());

    check("firstHeld from here", firstHeld({ 0, 1, 1 }, 2) == 2);
    check("firstHeld goes round", firstHeld({ 1, 0, 0 }, 1) == 0);
    check("firstHeld with none", firstHeld({ 0, 0 }, 0) == -1);
    check("firstHeld of nothing", firstHeld({}, 0) == -1);

    // Two files of one song. Written down before the first run: each shift found within 1 ms of the one made, from -140 to +140 ms; a
    // shift past the reach, another song, silence: unclear, never a wrong number; sixteenths and nothing else: right or unclear
    {
        const std::vector<Note> one = song(42, false), other = song(4242, false), grid = song(1, true);
        for (double late : { 0.0, 0.037, -0.0123, 0.0915, -0.064, 0.140, -0.140 }) {
            const Aligned r = twoFiles(one, one, late);
            char what[96];
            snprintf(what, sizeof what, "one song, the other file %+.1f ms later: found within 1 ms", late * 1e3);
            check(what, r.clear && std::fabs(r.shift - late) < 0.001);
        }
        Aligned r = twoFiles(one, one, 0.2);
        check("200 ms apart, past the reach: unclear", !r.clear);
        r = twoFiles(one, other, 0);
        check("the control, another song: unclear", !r.clear);
        r = twoFiles(grid, grid, 0.02);
        check("sixteenths and nothing else: right or unclear", !r.clear || std::fabs(r.shift - 0.02) < 0.001);
        std::vector<uint8_t> quiet(kPrintSteps, 0), quietMine(size_t(kPrintSteps + 2 * kPrintReachSteps), 0);
        check("silence: unclear", !align(quiet, quietMine).clear);
        check("prints of the wrong length: unclear", !align(std::vector<uint8_t>(10, 100), quietMine).clear);
        check("a window for a 4-minute song, under 30 s", printFrom(240) >= kPrintReach && printFrom(240) + kPrintSeconds + kPrintReach < 30);
        check("a window for a 10-second song", printFrom(10) >= kPrintReach && printFrom(10) + kPrintSeconds + kPrintReach <= 10);
        check("no window for a 5-second song", printFrom(5) < 0);

        Message f; f.kind = Kind::Print; f.song = 3; f.position = 12.5;
        check("a print asked for comes back the same", unpack(pack(f), u) && u.kind == Kind::Print && !u.flag && u.song == 3 && u.position == 12.5 && u.print.empty());
        f.flag = true;
        for (int i = 0; i < kPrintSteps; i++) f.print.push_back(uint8_t(i * 7));
        check("a print comes back the same", unpack(pack(f), u) && u.flag && u.print == f.print);
        f.print.clear();
        check("an empty print (the file would not read) comes back", unpack(pack(f), u) && u.flag && u.print.empty());
        check("a print with no song is refused", !unpack(B("tF\t1\t-\t0\t-1\t0\t0\n00ff"), x0));
        check("a print with a letter that is no hex is refused", !unpack(B("tF\t1\t-\t0\t1\t0\t0\n0g"), x0));
        check("a print of an odd length is refused", !unpack(B("tF\t1\t-\t0\t1\t0\t0\n0ff"), x0));
        check("a question with a body is refused", !unpack(B("tF\t0\t-\t0\t1\t0\t0\n00ff"), x0));
        check("a print too long is refused", !unpack(B("tF\t1\t-\t0\t1\t0\t0\n" + std::string(2 * (kPrintSteps + 2 * kPrintReachSteps) + 2, 'a')), x0));
    }

    printf("%d passed, %d failed: %s\n", passed, failed, failed ? "SOME FAILED" : "ALL GOOD");
    return failed ? 1 : 0;
}
