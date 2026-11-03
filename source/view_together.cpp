/*
 * Songate · view_together.cpp
 * Listening together, the session (together.hpp): the device that started it decides, and both act at one shared moment. While it
 * runs, the background player stands aside and Songate must stay open: leaving ends it for this device, and the other plays on. An
 * Automix join or a crossfade is planned once, by the starter, and carried out by both: it begins on a sample of the outgoing track,
 * which the two share. The queue of the device that was asked is put away and comes back.
 * Nothing about a session is saved. The card is in view_settings.cpp.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"
#include "words.hpp"

using namespace together;

namespace {
constexpr double kStartAhead = 0.5, kPauseAhead = 0.25, kGapBetween = 0.15, kNextBefore = 5, kAnswerSeconds = 45, kReadySeconds = 15;
constexpr int kClockEnough = 15;   // three seconds of pings
// Off the clock: the song is decoded from its start to an exact sample (a seek lands on a packet, and the other device did not seek), on
// a worker, so it rejoins this far ahead of where the other device is now
constexpr double kRejoinAhead = 8;
constexpr double kLostSeconds = 60, kResumeSeconds = 10;   // how long a dropped link is waited for; how long the other device has to answer
constexpr double kKeptTime = 0.02;                         // a device this close played on in time: nothing to do
// One song, two files: how long a Prepare waits for its song's shift (the starter's print, and this device's own) before it plays
// unshifted, as before
constexpr double kPrintWait = 4;
}  // namespace

bool App::togetherOn() const { return tog.stage == Tog::On; }
bool App::togetherGuest() const { return tog.stage != Tog::Off && tog.stage != Tog::Inviting && !tog.host; }
bool App::togetherLocked() const { return togetherOn() && !tog.host && tog.onlyHost; }

/** The starter: Who changes what plays (T7; the user, 2026-11-02: what this device cannot control is greyed there). */
void App::togetherSetOnlyHost(bool only) {
    if (!tog.host || tog.stage == Tog::Off) return;
    tog.onlyHost = only;
    togetherSendWho();
}

/** The starter: who changes what plays, and its order (shuffle, repeat), which decides what comes next on both (the user, 2026-11-03:
 *  the order changed on the Switch did not show on the PC). Sent when either changes, and when the session or a dropped link begins. */
void App::togetherSendWho() {
    tog.whoSent = int(store.order);
    Message m;
    m.kind = Kind::Who;
    m.flag = tog.onlyHost;
    m.song = int(store.order);
    togetherSend(m);
}

void App::togetherSend(const Message& m) { connect.send(pack(m)); }

together::Song App::togetherSong(int track) const {
    const Track& t = lib.tracks[size_t(track)];
    Song s;
    s.duration = t.duration;
    Identity::Known k;
    if (const_cast<Identity&>(identity).find(t.path, k)) { s.exact = k.exact; s.loose = k.loose; s.duration = k.duration > 0 ? k.duration : t.duration; }
    if (s.loose.empty()) s.loose = Identity::looseKey(t.artist, t.albumArtist, t.album, t.title);
    return s;
}

double App::togetherMine(int song, double theirs) const {
    const auto it = tog.shift.find(song);
    return std::max(0.0, theirs + (it != tog.shift.end() ? it->second : 0));
}

double App::togetherTheirs(int song, double mine) const {
    const auto it = tog.shift.find(song);
    return std::max(0.0, mine - (it != tog.shift.end() ? it->second : 0));
}

bool App::togetherShiftKnown(int song) const {
    return song < 0 || song >= int(tog.otherFile.size()) || !tog.otherFile[size_t(song)] || tog.shift.count(song) > 0;
}

/** The device that did not start it: a song it holds as another file, printed here and asked of the starter, once a session. */
void App::togetherAskPrint(int song) {
    if (tog.host || togetherShiftKnown(song) || tog.printing.count(song) || song >= int(tog.tracks.size()) || tog.tracks[size_t(song)] < 0) return;
    const double from = printFrom(std::min(tog.songs[size_t(song)].duration, lib.tracks[size_t(tog.tracks[size_t(song)])].duration));
    if (from < 0) { tog.shift[song] = 0; return; }   // too short to print: as before
    Tog::Printing& p = tog.printing[song];
    p.from = from;
    p.since = time;
    Message m;
    m.kind = Kind::Print;
    m.song = song;
    m.position = from;
    togetherSend(m);
    prints.want({ togPrintGen, song, false, lib.tracks[size_t(tog.tracks[size_t(song)])].path, from - kPrintReach, kPrintSeconds + 2 * kPrintReach });
}

/** Prints finished on the worker: the starter's go across; this device's meet the starter's, and the song has its shift. */
void App::togetherPrintsTick() {
    for (SongPrints::Done& d : prints.take()) {
        if (d.job.gen != togPrintGen || tog.stage == Tog::Off) continue;
        if (d.job.forThem) {
            Message m;
            m.kind = Kind::Print;
            m.flag = true;
            m.song = d.job.song;
            m.position = d.job.from;
            m.print = std::move(d.print);
            togetherSend(m);
            continue;
        }
        if (auto it = tog.printing.find(d.job.song); it != tog.printing.end()) {
            it->second.mine = std::move(d.print);
            it->second.mineIn = true;
        }
    }
    for (auto it = tog.printing.begin(); it != tog.printing.end();) {
        Tog::Printing& p = it->second;
        const bool late = time - p.since > 30;   // never answered (an older Songate there): as before
        if (!(p.theirsIn && p.mineIn) && !late) { ++it; continue; }
        Aligned a;
        if (!late && p.theirs.size() >= size_t(kPrintSteps) && p.mine.size() >= size_t(kPrintSteps + 2 * kPrintReachSteps)) {
            p.theirs.resize(kPrintSteps);
            p.mine.resize(size_t(kPrintSteps + 2 * kPrintReachSteps));
            a = align(p.theirs, p.mine);
        }
        tog.shift[it->first] = a.shift;
        char line[200];
        if (a.clear) snprintf(line, sizeof line, "Together: song %d is another file here, %+.2f ms from theirs (fit %.2f, next best %.2f)", it->first, a.shift * 1e3, a.score, a.next);
        else snprintf(line, sizeof line, "Together: song %d is another file here, no clear fit (%s, fit %.2f, next best %.2f): played unshifted", it->first, late ? "no print came" : !p.theirsIn || p.theirs.empty() || p.mine.empty() ? "a file would not read" : "unclear", a.score, a.next);
        logLine(line);
        it = tog.printing.erase(it);
    }
    if (tog.waiting && (togetherShiftKnown(tog.waitSong) || time - tog.waitSince > kPrintWait)) {
        tog.waiting = false;
        if (!togetherShiftKnown(tog.waitSong)) logLine("Together: song " + std::to_string(tog.waitSong) + "'s fit is late: it starts unshifted");
        togetherPrepareHere(tog.waitSong, togetherMine(tog.waitSong, tog.waitPosition), tog.waitNatural);
    }
}

/** The device that did not start it: a song to prepare, once its shift is known (a song held as the same bytes needs none). */
void App::togetherPrepareGuest(int song, double position, bool natural) {
    togetherAskPrint(song);
    // and the next song held here, ahead, so a join or the next track does not wait
    for (int n = song + 1; n < int(tog.tracks.size()); n++)
        if (tog.tracks[size_t(n)] >= 0) { togetherAskPrint(n); break; }
    if (togetherShiftKnown(song)) { tog.waiting = false; togetherPrepareHere(song, togetherMine(song, position), natural); return; }
    tog.waiting = true;
    tog.waitSong = song;
    tog.waitPosition = position;
    tog.waitNatural = natural;
    tog.waitSince = time;
}

/** Everyone's first step: the background player stands aside, and the sound starts following the clock. */
void App::togetherBegin(const std::string& peerId, const std::string& peerName, bool host) {
    tog = Tog{};
    togPrintGen++;
    tog.host = host;
    tog.peerId = peerId;
    tog.peerName = peerName;
    tog.since = time;
    if (player.remote()) {
        const bool had = now.track >= 0 && q.pos >= 0, playing = isPlaying();
        const double at = player.loaded() ? player.position() : 0;
        bg.stop();
        bg.disconnect();
        player.setRemote(nullptr);
        tog.hadBackground = true;
        if (had) {
            loadTrack(q.tracks[q.order[q.pos]], !playing);
            if (at > 1) player.seekNow(at);
        }
    }
    connect.clockWanted(true);
    player.syncEnable(true);
#ifdef GF_DEV
    player.syncChirps(togChirps);   // on by default in a dev build: a test without them measures nothing
#endif
    player.trap = [this](bool seek, double seconds) { return togetherOn() && togetherTrap(seek, seconds); };
}

void App::togetherEnd(const std::string& why) {
    if (tog.stage == Tog::Off) return;
    const bool guest = togetherGuest(), wasBg = tog.hadBackground;
    const double ownAt = tog.savedPosition;
    bool inQueue = true, restored = false;
    player.trap = nullptr;
    player.syncEnable(false);
    connect.clockWanted(false);
    if (guest && tog.queueTaken) {
        const int track = q.pos >= 0 && q.pos < int(q.order.size()) ? q.tracks[q.order[q.pos]] : -1;
        q = tog.savedQueue;
        restored = true;
        const auto it = std::find(q.tracks.begin(), q.tracks.end(), track);
        inQueue = false;
        if (it != q.tracks.end()) {
            const int at = int(it - q.tracks.begin());
            const auto ot = std::find(q.order.begin(), q.order.end(), at);
            if (ot != q.order.end()) { q.pos = int(ot - q.order.begin()); inQueue = true; }
        }
    }
    tog = Tog{};
    // The session's song is not in this queue: this device's own song back, on screen and at its place, paused, playing or not (the
    // user, 2026-11-02: "Back to my song, paused"). Before, the screen kept the session's last song while the background player played
    // this queue, and a song still playing played out first
    if (restored && !inQueue) {
        if (isPlaying()) player.stop();
        togetherOwnBack(ownAt, false);
    }
    // Back to the background player by the handover that stops this one first: reconnecting beside it left both able to sound, and the
    // app showing the idle one as paused (the user, 2026-11-02, airplane mode)
    if (wasBg && store.background) setBackground(true);
    if (!why.empty()) toast(why);
}

void App::togetherOwnBack(double at, bool play) {
    if (q.pos < 0 || q.pos >= int(q.order.size())) return;
    loadTrack(q.tracks[size_t(q.order[size_t(q.pos)])], !play);
    if (at > 1) player.seekNow(at);
}

void App::togetherStart(const std::string& peerId, const std::string& peerName) {
    if (tog.stage != Tog::Off) return;
    if (q.order.empty() || q.pos < 0) { toast("Play something first: the session starts from this queue"); return; }
    togetherBegin(peerId, peerName, true);
    tog.stage = Tog::Inviting;
    Message m;
    m.kind = Kind::Invite;
    togetherSend(m);
}

void App::togetherLeave() {
    Message m;
    m.kind = Kind::Leave;
    togetherSend(m);
    if (isPlaying()) player.stop();
    togetherEnd("You left the session");
}

/** The starter: the list as it is now goes across, then the other says which songs it holds. */
void App::togetherSendQueue() {
    tog.songs.clear();
    tog.songs.reserve(q.order.size());
    tog.tracks.clear();
    for (int o : q.order) { tog.songs.push_back(togetherSong(q.tracks[size_t(o)])); tog.tracks.push_back(q.tracks[size_t(o)]); }
    tog.signature = q.order.size() * 31 + q.tracks.size();
    for (int o : q.order) tog.signature = tog.signature * 1000003u + size_t(q.tracks[size_t(o)]);
    Message m;
    m.kind = Kind::Queue;
    m.songs = tog.songs;
    togetherSend(m);
    tog.have.clear();
}

/** The starter: this song, from here, on both. natural: the track playing plays out first. */
void App::togetherPrepare(int song, double position, bool natural) {
    tog.serial++;
    Message m;
    m.kind = Kind::Prepare;
    m.serial = tog.serial;
    m.song = song;
    m.position = position;
    m.flag = natural;
    togetherSend(m);
    togetherPrepareHere(song, position, natural);
}

/** A seek or a pause in a track's last seconds: the next song waits, and the track playing stays the one playing. */
void App::togetherCancelNext() {
    if (!tog.natural || tog.loaded) return;
    tog.natural = false;
    tog.nextSent = false;
    tog.song = tog.playingSong;
    tog.prepared = tog.loaded = tog.saidReady = true;
    tog.theyReady = false;
    if (tog.startAt == 0) tog.startAt = 1;
}

/** The starter, from ten seconds into a track: the join into the next one, if there is one. */
void App::togetherPlanJoin() {
    const double pos = player.position();
    if (pos < 10 || now.track < 0) return;
    const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[size_t(now.track)].duration;
    const double left = dur - pos;
    if (dur <= 0 || left <= 0.25) return;
    const int p = crossfadeNext(left);
    if (p < 0) return;   // looked at again next frame, as updateAutomix does
    const int song = firstHeld(tog.have, p);
    if (song < 0) return;
    automix::Plan pl;
    if (automixQueue()) {
        const Level* la = levels.find(lib.tracks[size_t(now.track)].path);
        const Level* lb = levels.find(lib.tracks[size_t(tog.tracks[size_t(song)])].path);
        pl = planJoin(la ? &la->mix : nullptr, dur, lb ? &lb->mix : nullptr, store.crossfade, nullptr);
        if (pl.off || dur <= pl.length * 2) { tog.planNone = true; return; }
    } else if (store.crossfade > 0 && dur > store.crossfade * 2) {
        pl.length = store.crossfade;
        pl.startOut = dur - store.crossfade;
        pl.join = automix::fromPreset(automix::Preset::Fade);
    } else {
        tog.planNone = true;
        return;
    }
    tog.serial++;
    Message m;
    m.kind = Kind::Join;
    m.serial = tog.serial;
    m.song = song;
    m.plan = pl;
    m.flag = automixQueue();
    togetherSend(m);
    togetherArmJoin(song, pl);
}

void App::togetherArmJoin(int song, const automix::Plan& plan) {
    tog.plan = plan;
    tog.joinAutomix = automixQueue();   // (the other device: set from the message just after)
    tog.joinShifted = false;
    tog.joinSong = song;
    tog.joinArmed = tog.nextSent = true;
    stretchBegun = mixPrepared = false;
}

/** Both devices, each by its own position: the two are together, so the same plan lands on the same sample. */
void App::togetherJoinStep() {
    const automix::Plan& pl = tog.plan;
    const double pos = player.position();
    const int track = tog.joinSong >= 0 && tog.joinSong < int(tog.tracks.size()) ? tog.tracks[size_t(tog.joinSong)] : -1;
    if (track < 0) { tog.joinArmed = false; return; }
    if (pl.speed != 1 && !stretchBegun && pos >= pl.stretchFrom) {
        stretchBegun = true;
        player.beginStretch(pl);
    }
    if (!mixPrepared && pos >= pl.startOut - 4) {
        mixPrepared = true;
        if (!tog.host && !tog.joinShifted) {
            tog.joinShifted = true;
            tog.plan.offset = togetherMine(tog.joinSong, pl.offset);
        }
        player.prepare(lib.tracks[size_t(track)].path, pl.offset);
    }
    if (pos < pl.startOut - 0.6 || player.preparing()) return;
    tog.joinArmed = false;
    tog.nextSent = false;
    tog.planNone = false;
    keepMixOld();
    const automix::Plan plan = pl;
    if (tog.host) q.pos = tog.joinSong;
    else {
        const auto it = std::find(q.tracks.begin(), q.tracks.end(), track);
        if (it != q.tracks.end()) q.pos = int(it - q.tracks.begin());
    }
    tog.song = tog.joinSong;
    tog.position = plan.offset;
    // Automix's label and animation for an Automix join only: the other device showed them for a crossfade too (the user, 2026-11-03:
    // "it appeared the automix animation on track change even tho the playlist on console is a normal one")
    if (tog.host ? automixQueue() : tog.joinAutomix) {
        mixLabel = "Automix";
        if (plan.beatmatched) mixLabel += " · " + std::to_string(int(std::lround(plan.bpm))) + " BPM";
        mixShownAt = mixFrom = time;
        mixUntil = time + plan.length + 0.6;
        mixShowLen = mixUntil - mixShownAt;
    }
    loadTrack(track, false, true, plan.length, &plan);
#ifdef GF_DEV
    logLine("Together: join, from " + std::to_string(int(plan.startOut)) + " s for " + std::to_string(int(plan.length)) + " s");
#endif
}

void App::togetherPrepareHere(int song, double position, bool natural) {
    tog.waiting = false;
    if (natural) tog.playingSong = tog.song;
    else tog.nextSent = tog.planNone = false;
    tog.joinArmed = false;
    tog.song = song;
    tog.position = position;
    tog.natural = natural;
    tog.prepared = tog.loaded = tog.theyReady = tog.rejoining = false;
    tog.saidReady = false;
    tog.startAt = 0;
    tog.readySince = time;
    const int track = song >= 0 && song < int(tog.tracks.size()) ? tog.tracks[size_t(song)] : -1;
    if (track < 0) { tog.prepared = true; tog.loaded = true; return; }   // not here: this device sits this one out
    player.prepareTimed(lib.tracks[size_t(track)].path, position);
}

void App::togetherLoadNow() {
    tog.loaded = true;
    const int track = tog.song >= 0 && tog.song < int(tog.tracks.size()) ? tog.tracks[size_t(tog.song)] : -1;
    if (track < 0) { player.stop(); return; }
    if (tog.host) q.pos = tog.song;
    else {
        const auto it = std::find(q.tracks.begin(), q.tracks.end(), track);
        if (it != q.tracks.end()) q.pos = int(it - q.tracks.begin());
    }
    loadTrack(track, true, true);
    if (std::fabs(player.position() - tog.position) > 0.01) player.seekNow(tog.position);
}

void App::togetherStartAt(double at) {
    tog.startAt = at;
    tog.paused = false;
    if (!tog.natural && !tog.loaded) togetherLoadNow();
    if (tog.loaded && tog.song >= 0 && tog.tracks[size_t(tog.song)] >= 0) player.syncStart(at, tog.position);
}

/** Off the clock, or back from a dropped link: silent until the moment `position` plays, reached exactly on a worker. */
void App::togetherRejoin(int song, double moment, double position) {
    player.syncHold();
    tog.joinArmed = tog.nextSent = false;
    tog.rejoinNew = song != tog.song || !tog.loaded;
    tog.song = song;
    tog.natural = false;
    tog.rejoining = true;
    tog.position = position;
    tog.rejoinAt = moment;
    player.prepareTimed(lib.tracks[size_t(tog.tracks[size_t(song)])].path, position, true);
}

/** The starter, the link back: where things stand, for the other device to compare with its own. */
void App::togetherResume() {
    togetherCancelNext();
    const bool stopped = tog.paused;
    tog.serial++;
    Message m;
    m.kind = Kind::Resume;
    m.serial = tog.serial;
    m.song = tog.song;
    m.flag = stopped;
    const double at = player.syncNow();
    m.at = at;
    m.position = stopped ? player.position() : std::max(0.0, player.syncPositionAt(at));
    togetherSend(m);
    tog.whoSent = -1;
    tog.awaitingResume = true;
    tog.resumedAt = time;
    // A join planned while the link was down went nowhere (the user, 2026-11-03: its plan left in the drop, and the other device
    // stopped at the song's end while this one joined): sent again, after the Resume that sets where the other device is
    if (tog.joinArmed) {
        Message j;
        j.kind = Kind::Join;
        j.serial = tog.serial;
        j.song = tog.joinSong;
        j.plan = tog.plan;
        j.flag = tog.joinAutomix;
        togetherSend(j);
    }
}

/** Pause or a seek, from this device's own controls. */
bool App::togetherTrap(bool seek, double seconds) {
    Message m;
    if (tog.linkLost && !tog.host) { toast("Waiting for " + tog.peerName); return true; }
    if (togetherLocked()) { toast("Only " + tog.peerName + " changes what plays in this session"); return true; }
    if (!tog.host) {
        m.kind = Kind::Ask;
        m.what = seek ? AskSeek : tog.paused ? AskPlay : AskPause;
        m.position = seek ? togetherTheirs(tog.song, seconds) : seconds;
        togetherSend(m);
        return true;
    }
    if (seek) togetherHostSeek(seconds);
    else if (tog.paused) togetherHostPlay();
    else togetherHostPause();
    return true;
}

void App::togetherHostSeek(double seconds) {
    tog.serial++;
    Message m;
    m.kind = Kind::Seek;
    m.serial = tog.serial;
    m.position = seconds;
    m.flag = !tog.paused;
    m.at = player.syncNow() + kStartAhead;
    togetherSend(m);
    togetherSeekHere(m.at, seconds, m.flag);
}

void App::togetherSeekHere(double at, double seconds, bool playing) {
    togetherCancelNext();
    stretchBegun = false;   // (a join still ahead begins its stretch again, from where the track now is)
    tog.rejoining = false;
    player.syncHold();
    player.seekNow(seconds);
    tog.position = seconds;
    tog.nextSent = false;
    if (playing) player.syncStart(at, seconds);
}

void App::togetherHostPause() {
    Message m;
    m.kind = Kind::Pause;
    m.serial = tog.serial;
    m.at = player.syncNow() + kPauseAhead;
    togetherSend(m);
    togetherCancelNext();
    tog.paused = true;
    player.syncPauseAt(m.at);
}

void App::togetherHostPlay() {
    tog.serial++;
    Message m;
    m.kind = Kind::Go;
    m.serial = tog.serial;
    m.flag = true;   // from the position given: a pause may have left the two a buffer apart
    m.song = tog.song;
    m.position = player.position();
    m.at = player.syncNow() + kStartAhead;
    togetherSend(m);
    togetherSeekHere(m.at, m.position, true);
    tog.paused = false;
}

/** Next, previous, or a track chosen: on the starter it goes to both; on the other device it is asked for. */
bool App::togetherPlayAt(int orderPos) {
    if (tog.host) {
        size_t sig = q.order.size() * 31 + q.tracks.size();
        for (int o : q.order) sig = sig * 1000003u + size_t(q.tracks[size_t(o)]);
        if (sig != tog.signature) {   // a new queue here: the other device hears of it first
            togetherSendQueue();
            tog.pendingSong = orderPos;
            return true;
        }
        const int song = firstHeld(tog.have, orderPos);
        if (song < 0) { toast("Neither of these songs is on both devices"); return true; }
        togetherPrepare(song, 0, false);
        return true;
    }
    if (tog.linkLost) { toast("Waiting for " + tog.peerName); return true; }
    if (togetherLocked()) { toast("Only " + tog.peerName + " changes what plays in this session"); return true; }
    Message m;
    m.kind = Kind::Ask;
    m.what = AskSong;
    m.song = -1;
    for (int i = 0; i < int(tog.tracks.size()); i++)
        if (tog.tracks[size_t(i)] == q.tracks[size_t(q.order[size_t(orderPos)])]) { m.song = i; break; }
    if (m.song >= 0) togetherSend(m);
    return true;
}

/** A track ended here. The starter has usually sent the next one by now. */
void App::togetherFinished() {
    if (tog.prepared && tog.natural && !tog.loaded) {
        togetherLoadNow();
        if (tog.startAt > 0) player.syncStart(tog.startAt, tog.position);
        return;
    }
    // The other device, its track over and nothing after it (a message lost in a drop): it asks where things stand, and rejoins from the
    // answer, rather than sitting silent while the starter plays on
    if (!tog.host && tog.stage == Tog::On && !tog.natural && !tog.joinArmed && !tog.linkLost && time - tog.askedWhereAt > 3) {
        tog.askedWhereAt = time;
        Message m;
        m.kind = Kind::Ask;
        m.what = AskWhere;
        togetherSend(m);
        logLine("Together: the track ended here with nothing after it: asking " + tog.peerName + " where things stand");
    }
}

void App::togetherHandle(const Message& m) {
    switch (m.kind) {
    case Kind::Invite:
        if (tog.stage != Tog::Off) return;
        {
            std::string name = "The other device";
            for (const Connect::Paired& p : connect.view().paired) if (p.linked) { name = p.name; tog.peerId = p.id; }
            const std::string id = tog.peerId;
            tog.stage = Tog::Off;
            confirm(ICON_LINK, "Devices", "Listen together", name, "Listen together with " + name + "?",
                    "Both play one queue, in time. Your own queue comes back when you leave, and nothing of the session is saved.", "Listen", [this, id, name] {
                togetherBegin(id, name, false);
                tog.stage = Tog::Joining;
                Message a;
                a.kind = Kind::Answer;
                a.flag = true;
                togetherSend(a);
            });
            sheet.askSafe = true;
            sheet.askYesIcon = ICON_LINK;
        }
        return;
    case Kind::Answer:
        if (tog.stage != Tog::Inviting || !tog.host) return;
        if (!m.flag) { togetherEnd(tog.peerName + " said no"); return; }
        tog.stage = Tog::Joining;
        togetherSendQueue();
        return;
    case Kind::Queue: {
        if (tog.host || tog.stage == Tog::Off) return;
        std::vector<Held> mine;
        mine.reserve(lib.tracks.size());
#ifdef GF_DEV
        const Uint64 mapFrom = SDL_GetPerformanceCounter();
#endif
        for (int i = 0; i < int(lib.tracks.size()); i++) mine.push_back({ togetherSong(i), i });
        tog.songs = m.songs;
        tog.tracks = match(m.songs, mine);
        tog.otherFile.assign(tog.tracks.size(), 0);
        for (size_t i = 0; i < tog.tracks.size(); i++)
            if (tog.tracks[i] >= 0 && (m.songs[i].exact.empty() || m.songs[i].exact != mine[size_t(tog.tracks[i])].song.exact)) tog.otherFile[i] = 1;
        tog.shift.clear();
        tog.printing.clear();
        tog.waiting = false;
#ifdef GF_DEV
        logLine("Together: " + std::to_string(m.songs.size()) + " songs matched in " + std::to_string(int(double(SDL_GetPerformanceCounter() - mapFrom) * 1000.0 / double(SDL_GetPerformanceFrequency()))) + " ms");
        if (getenv("GF_TOGETHER_HIDE")) for (size_t i = 1; i < tog.tracks.size(); i += 2) tog.tracks[i] = -1;   // every second song is "not here"
#endif
        if (!tog.queueTaken) {
            tog.savedQueue = q;
            tog.savedPosition = player.loaded() ? player.position() : 0;
            tog.queueTaken = true;
        }
        Queue t;
        t.name = "Listening together";
        for (int k : tog.tracks) if (k >= 0) { t.order.push_back(int(t.tracks.size())); t.tracks.push_back(k); }
        t.pos = -1;
        q = std::move(t);
        Message h;
        h.kind = Kind::Have;
        for (int k : tog.tracks) h.have.push_back(k >= 0);
        togetherSend(h);
        tog.stage = Tog::On;
        return;
    }
    case Kind::Have: {
        if (!tog.host || m.have.size() != tog.songs.size()) return;
        tog.have = m.have;
        const bool first = tog.stage != Tog::On;
        tog.stage = Tog::On;
        tog.missing = int(std::count(tog.have.begin(), tog.have.end(), uint8_t(0)));
        tog.whoSent = -1;
        if (first) {
            const int song = firstHeld(tog.have, q.pos);
            if (song < 0) { togetherEnd("None of these songs is on both devices"); return; }
            togetherPrepare(song, song == q.pos && player.loaded() ? player.position() : 0, false);
        } else if (tog.pendingSong >= 0) {
            const int song = firstHeld(tog.have, tog.pendingSong);
            tog.pendingSong = -1;
            if (song >= 0) togetherPrepare(song, 0, false);
        }
        return;
    }
    case Kind::Handoff: {
        if (tog.stage != Tog::Off) {
            Message no;
            no.kind = Kind::Taken;
            togetherSend(no);
            return;
        }
        std::string name = "The other device", id;
        for (const Connect::Paired& p : connect.view().paired) if (p.linked) { name = p.name; id = p.id; }
        if (store.handoffFrom.find(id) != std::string::npos) { handoffTake(m, name, time); return; }
        confirm(ICON_LINK, "Devices", "Handoff from " + name, std::to_string(m.songs.size()) + (m.songs.size() == 1 ? " song" : " songs"), "Take over from " + name + "?",
                "Its queue and its place in the song move here, and it pauses. This replaces your queue; you can undo that for two minutes. After this, " + name + " does not ask again.",
                "Take over", [this, m, name, id, at = time] {
            if (!id.empty()) { store.handoffFrom += id + " "; persistSoon(0.3f); }
            handoffTake(m, name, at);
        });
        sheet.askSafe = true;
        sheet.askYesIcon = ICON_CHECK;
        return;
    }
    case Kind::Taken:
        if (handoffPeer.empty()) return;
        if (m.flag) {
            if (isPlaying()) player.togglePause();
            toast("Handed off to " + handoffName);
        } else {
            toast(handoffName + " did not take it");
        }
        handoffPeer.clear();
        return;
    case Kind::Join: {
        if (tog.host || m.song >= int(tog.tracks.size())) return;
        tog.serial = m.serial;
        // the track playing's places, in this device's file; the next one's start is shifted when it is prepared (its print may be
        // on its way still)
        automix::Plan pl = m.plan;
        const auto sh = tog.shift.find(tog.song);
        const double out = sh != tog.shift.end() ? sh->second : 0;
        pl.startOut = std::max(0.0, pl.startOut + out);
        pl.stretchFrom = std::max(0.0, pl.stretchFrom + out);
        if (pl.loopFrom >= 0) pl.loopFrom = std::max(0.0, pl.loopFrom + out);
        if (pl.loopTo >= 0) pl.loopTo = std::max(0.0, pl.loopTo + out);
        togetherAskPrint(m.song);
        togetherArmJoin(m.song, pl);
        tog.joinAutomix = m.flag;
        return;
    }
    case Kind::Prepare:
        if (tog.host) return;
        tog.serial = m.serial;
        togetherPrepareGuest(m.song, m.position, m.flag);
        return;
    case Kind::Print:
        if (tog.stage == Tog::Off) return;
        if (!m.flag) {   // the starter: its own file's print, for the other device
            if (!tog.host || m.song >= int(tog.tracks.size()) || tog.tracks[size_t(m.song)] < 0 || m.position < 0 || m.position > 30) return;
            prints.want({ togPrintGen, m.song, true, lib.tracks[size_t(tog.tracks[size_t(m.song)])].path, m.position, kPrintSeconds });
            return;
        }
        if (auto it = tog.printing.find(m.song); !tog.host && it != tog.printing.end() && std::fabs(it->second.from - m.position) < 1e-3) {
            it->second.theirs = m.print;
            it->second.theirsIn = true;
        }
        return;
    case Kind::Ready:
        if (tog.host && m.serial == tog.serial) { tog.theyReady = true; tog.awaitingResume = false; }
        return;
    case Kind::Resume: {
        if (tog.host || tog.stage != Tog::On) return;
        tog.linkLost = false;
        tog.serial = m.serial;
        togetherCancelNext();
        Message r;
        r.kind = Kind::Ready;
        r.serial = m.serial;
        togetherSend(r);
        if (m.song < 0 || m.song >= int(tog.tracks.size()) || tog.tracks[size_t(m.song)] < 0) return;
        if (m.flag) {
            tog.paused = true;
            togetherRejoin(m.song, 0, togetherMine(m.song, m.position));
            return;
        }
        tog.paused = false;
        const double moment = player.syncNow() + kRejoinAhead, position = togetherMine(m.song, m.position + (moment - m.at));
        const bool kept = m.song == tog.song && player.syncRunning() && std::fabs(player.syncPositionAt(moment) - position) < kKeptTime && std::fabs(player.syncError()) < kKeptTime;
#ifdef GF_DEV
        logLine(std::string("Together: link back, ") + (kept ? "kept time" : "rejoining"));
#endif
        if (!kept) togetherRejoin(m.song, moment, position);
        return;
    }
    case Kind::Go:
        if (tog.host) return;
        tog.serial = m.serial;
        if (m.flag) { togetherSeekHere(m.at, togetherMine(tog.song, m.position), true); tog.paused = false; }
        else { tog.position = togetherMine(tog.song, m.position); togetherStartAt(m.at); }
        return;
    case Kind::Seek:
        if (tog.host) return;
        tog.serial = m.serial;
        togetherSeekHere(m.at, togetherMine(tog.song, m.position), m.flag);
        return;
    case Kind::Pause:
        if (tog.host) return;
        togetherCancelNext();
        tog.paused = true;
        player.syncPauseAt(m.at);
        return;
    case Kind::Ask:
        if (!tog.host || !togetherOn()) return;
        if (m.what == AskWhere) {   // (not a control: answered whoever decides)
            if (!tog.linkLost && !tog.awaitingResume) togetherResume();
            return;
        }
        if (tog.onlyHost) return;
        if (m.what == AskPause && !tog.paused) togetherHostPause();
        else if (m.what == AskPlay && tog.paused) togetherHostPlay();
        else if (m.what == AskSeek) togetherHostSeek(m.position);
        else if (m.what == AskSong && m.song >= 0 && m.song < int(tog.have.size())) togetherPrepare(firstHeld(tog.have, m.song), 0, false);
        else if (m.what == AskOrder && m.song >= 0 && m.song <= 3) {
            Order o = Order(m.song);
            if (o == Order::Shuffle && automixQueue()) o = Order::Default;   // an Automix playlist: no Shuffle (holdOrderForAutomix)
            setOrder(o);
            static const char* names[] = { "Default order", "Repeat playlist", "Repeat track", "Shuffle" };
            toast(std::string(names[int(store.order)]) + ", from " + tog.peerName);
            store.save();
        }
        return;
    case Kind::Leave:
        if (tog.stage != Tog::Off) togetherEnd(tog.peerName + " left the session");
        return;
    case Kind::Who:
        if (tog.host || tog.stage == Tog::Off) return;
        if (m.song >= 0 && m.song <= 3) tog.order = m.song;
        if (tog.onlyHost == m.flag) return;
        tog.onlyHost = m.flag;
        toast(m.flag ? tog.peerName + " now decides what plays" : "Both devices change what plays again");
        return;
    }
}

void App::extBegin(const std::string& from) {
    extOn = true;
    extFrom = from.empty() ? std::string("the other device") : from;
    extPos = 0;
    extPosAt = time;
    extClearAt = 0;
    now.info = TrackInfo{};
    now.info.codec = "AirPlay";   // as it arrives: Apple Lossless, 16-bit, 44.1 kHz (the chips say so)
    now.info.bitsPerSample = 16;
    now.info.sampleRate = 44100;
    now.lyrics.clear();
    TrackLoader::Result none;   // no cover until it sends one: the placeholder and the default colours
    none.gen = ++loadGen;
    applyCover(none);
}

void App::extInfo(const std::string& title, const std::string& artist, const std::string& album) {
    now.info.title = title;
    now.info.artist = artist;
    now.info.album = album;
}

void App::extCover(std::vector<uint8_t> image) {
    // iOS says "no cover" as one song gives way to the next, a moment before the next one's cover: the old cover stays a little,
    // and the next crossfades from it as any song's (the user, 2026-11-02: "shows a no cover split second"). A song that truly
    // has none is shown so once that time is up
    if (image.empty()) {
        if (extClearAt <= 0) extClearAt = time + 1.5;
        return;
    }
    extClearAt = 0;
    TrackLoader::Request r;   // decoded, shrunk and its palette made on the loader's worker, as a track's cover
    r.gen = ++loadGen;
    r.readInfo = false;
    r.image = std::move(image);
    r.opts = paletteOpts();
    loader.request(std::move(r));
}

void App::extProgress(double position, double duration) {
    extPos = position;
    extPosAt = time;
    now.info.duration = duration;
}

double App::shownPosition() const {
    if (!extOn) return player.position();
    const double p = extPos + (time - extPosAt);
    return now.info.duration > 0 ? std::clamp(p, 0.0, now.info.duration) : std::max(0.0, p);
}

void App::extTick() {
    if (extClearAt <= 0 || time < extClearAt) return;
    extClearAt = 0;
    TrackLoader::Result none;
    none.gen = ++loadGen;
    applyCover(none);
}

void App::extEnd() {
    extClearAt = 0;
    extOn = false;
    if (now.track < 0) {
        now.info = TrackInfo{};
        TrackLoader::Result none;
        none.gen = ++loadGen;
        applyCover(none);
        return;
    }
    // your own song again, its tags, lyrics and cover read as when it loads (the player is not touched)
    const Track& t = lib.tracks[now.track];
    TrackLoader::Request r;
    r.gen = ++loadGen;
    r.path = t.path;
    r.folder = t.folder;
    r.readInfo = true;
    if (auto it = store.covers.find(t.path); it != store.covers.end()) r.myCover = it->second;
    r.opts = paletteOpts();
    now.loading = true;
    loader.request(std::move(r));
}

void App::airplayQuestion(const std::string& name, std::function<void(int)> answer) {
    confirm(ICON_LINK, "AirPlay", name, "Wants to play on " GF_THIS_CONSOLE, "Let " + name + " play here?",
            "It is on this network but has not played here before. Always lets it play without asking; Settings › Devices can "
            "forget it.",
            "Allow once", [answer] { answer(1); });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
    sheet.askAlt = "Always";
    sheet.askAltYes = [answer] { answer(2); };
}

#ifdef GF_AIRPLAY
/** The remembered devices as AirPlay knows them: by name (older entries kept a DACP-ID, which iOS changes each time). */
std::vector<std::string> App::airplayIds() const {
    std::vector<std::string> ids;
    for (const auto& d : store.airplayDevices) ids.push_back(d.id.rfind("ip:", 0) == 0 ? d.id : "name:" + d.name);
    return ids;
}

/** An iPhone began or ended: this device's music stands aside, and the background player with it. */
void App::airplayTick() {
    airplay.want(store.airplayReceive && tog.stage == Tog::Off);
    if (store.airplayDevices.size() != airplayAllowedSent) {   // (a device is only added or forgotten here, one at a time)
        airplayAllowedSent = store.airplayDevices.size();
        airplay.allow(airplayIds());
    }
    const AirPlay::View v = airplay.view();
    // A new device asks to play (the user, 2026-11-02: "Ask for new devices"): the question, while its request waits
    if (v.asking && !airplayAsking) {
        airplayAsking = true;
        const std::string id = v.askId, name = v.askName;
        airplayQuestion(name, [this, id, name](int a) {
            if (a == 2) {
                std::erase_if(store.airplayDevices, [&](const Store::AirPlayDevice& d) { return d.name == name; });   // one entry a name
                store.airplayDevices.push_back({ id, name });
                store.saveAirplayDevices();
                airplayAllowedSent = store.airplayDevices.size();
                airplay.allow(airplayIds());
            }
            airplay.answer(a > 0);
        });
    } else if (airplayAsking && !v.asking) {
        airplayAsking = false;   // answered, or 20 s passed: the request is gone, and so is its question
        if (sheet.page == Sheet::Confirm && sheet.askKicker == "AirPlay") closeSheet();
    } else if (airplayAsking && !(sheet.page == Sheet::Confirm && sheet.askKicker == "AirPlay")) {
        airplay.answer(false);   // Cancel, or anything that closed it
    }
    if (v.streaming && !airplayOn) {
        airplayOn = true;
        if (player.remote()) {
            bg.stop();
            bg.disconnect();
            player.setRemote(nullptr);
            airplayHadBackground = true;
        } else if (isPlaying()) {
            player.togglePause();
        }
        player.streamBegin([this](int16_t* out, int frames) { return airplay.read(out, frames); });
        extBegin(v.peer);
        toast("Playing from " + (v.peer.empty() ? std::string("AirPlay") : v.peer));
    } else if (!v.streaming && airplayOn) {
        airplayOn = false;
        player.streamEnd();
        extEnd();
        toast("AirPlay ended");
        if (airplayHadBackground && store.background) {
            airplayHadBackground = false;
            setBackground(true);
        }
    }
    if (airplayOn) {
        // its song and cover on screen as they arrive (the cover moved out once, never copied each frame)
        if (v.title != now.info.title || v.artist != now.info.artist || v.album != now.info.album) extInfo(v.title, v.artist, v.album);
        if (v.progressGen != airplayProgressGen) {
            airplayProgressGen = v.progressGen;
            extProgress(v.position, v.duration);
        }
        if (v.coverGen != airplayCoverGen) {
            airplayCoverGen = v.coverGen;
            extCover(airplay.takeCover());
        }
        extTick();
        player.streamGain(v.gain);
        // Levelling as a track's (gainTo, app.cpp), from the stream's own measurement once 3 s of the song are in: toward your target,
        // never past its true peak. Off, or not measured yet: as it comes. A stream has no album, so every mode levels the song.
        float lv = 1;
        if (store.levelMode != 0 && v.measured >= 3) {
            double g = store.levelTarget - v.lufs;
            if (v.peak > 0) g = std::min(g, -1.0 - 20 * std::log10(v.peak));
            lv = float(std::pow(10, std::clamp(g, -24.0, 12.0) / 20));
        }
        player.streamLevel(lv);
#ifdef GF_DEV
        if (time - airplayLoggedAt >= 10) {
            airplayLoggedAt = time;
            char line[200];
            snprintf(line, sizeof line, "AirPlay: lost %d, repaired %d; this song measured %.0f s at %.1f LUFS, peak %.2f; level %+.1f dB, iPhone volume %.2f",
                     v.lost, v.repaired, v.measured, v.lufs, v.peak, 20 * std::log10(std::max(lv, 1e-6f)), double(v.gain));
            logLine(line);
        }
#endif
    }
}
#endif

void App::handoffTo(const std::string& peerId, const std::string& peerName) {
    if (tog.stage != Tog::Off || !handoffPeer.empty()) return;
    if (q.order.empty() || q.pos < 0) { toast("Play something first: this queue is what moves"); return; }
    Message m;
    m.kind = Kind::Handoff;
    if (queuePlaylist() >= 0) m.list = q.name;   // the playlist it came from (Automix, smart, Mood…): played as one there too
    m.song = q.pos;
    m.position = player.loaded() ? player.position() : 0;
    m.flag = isPlaying();
    m.songs.reserve(q.order.size());
    for (int o : q.order) m.songs.push_back(togetherSong(q.tracks[size_t(o)]));
    togetherSend(m);
    handoffPeer = peerId;
    handoffName = peerName;
    handoffSince = time;
    toast("Asking " + peerName + " to take over");
}

/** This queue, this track, from there: through the background player when it is the one playing here. */
void App::handoffPlay(int track, bool playing, double at) {
    if (player.remote()) {
        bgs.tracks.clear();
        if (bgSyncQueue(false)) {
            loadTrack(track, !playing, false);
            bg.playAt(q.pos, !playing, at);
            bgs.awaitUntil = time + 1.5;
            return;
        }
    }
    loadTrack(track, !playing);
    if (at > 1) player.seekNow(at);
}

void App::handoffTake(const Message& m, const std::string& name, double receivedAt) {
    Message answer;
    answer.kind = Kind::Taken;
    std::vector<Held> mine;
    mine.reserve(lib.tracks.size());
    for (int i = 0; i < int(lib.tracks.size()); i++) mine.push_back({ togetherSong(i), i });
    const std::vector<int> tracks = match(m.songs, mine);
    Queue t;
    t.name = "From " + name;
    // the playlist it came from, when this device has one of that name (Sync makes them the same): its queue is that playlist,
    // so an Automix one joins as Automix here too, and a smart or Mood one shows as itself
    for (const FolderPlaylist& p : store.playlists)
        if (!m.list.empty() && p.name == m.list) { t.name = m.list; break; }
    int pos = -1, missing = 0;
    for (int i = 0; i < int(tracks.size()); i++) {
        if (tracks[size_t(i)] < 0) { missing++; continue; }
        if (i >= m.song && pos < 0) pos = int(t.tracks.size());
        t.order.push_back(int(t.tracks.size()));
        t.tracks.push_back(tracks[size_t(i)]);
    }
    if (t.tracks.empty()) {
        togetherSend(answer);
        toast("None of those songs are here");
        return;
    }
    const bool same = m.song >= 0 && m.song < int(tracks.size()) && tracks[size_t(m.song)] >= 0;
    if (pos < 0) pos = 0;
    t.pos = pos;
    // Your own queue, from before the first of several: a second handoff saved the first one's in its place (the user, 2026-11-02)
    if (time >= handoffOldUntil) {
        handoffOld = q;
        handoffOldAt = player.loaded() ? player.position() : 0;
    }
    handoffOldUntil = time + 120;
    q = std::move(t);
    handoffPlay(q.tracks[size_t(q.order[size_t(q.pos)])], m.flag, same ? m.position + (m.flag ? time - receivedAt : 0) : 0);
    answer.flag = true;
    togetherSend(answer);
    persistSoon(0.5f);
    toast("Took over from " + name + (missing ? ", " + std::to_string(missing) + (missing == 1 ? " song is not here" : " songs are not here") : ""));
}

void App::handoffUndo() {
    if (time > handoffOldUntil) return;
    q = handoffOld;
    handoffOldUntil = 0;
    if (q.pos >= 0 && q.pos < int(q.order.size())) handoffPlay(q.tracks[size_t(q.order[size_t(q.pos)])], false, handoffOldAt);
    else player.stop();
    persistSoon(0.5f);
    toast("Your queue is back");
}

/** Every frame. */
void App::togetherTick() {
#ifdef GF_DEV
    // Dev, session 11 (the PC's 10 ms between launches): GF_SOLO_SYNC starts the library's first song through a session's own timed
    // start, this device's clock standing for the shared one and no output delay, with the test chirps; it prints that moment on
    // steady_clock (the performance counter), so tools/clicktest's microphone, stamped on the same counter, says how late each chirp
    // sounds. Nothing else: no device, no link
    if (static const bool solo = getenv("GF_SOLO_SYNC") != nullptr; solo && soloStage_ < 2 && !lib.tracks.empty() && time > 3) {
        if (soloStage_ == 0) {
            loadTrack(0, true);
            player.syncEnable(true);
            player.syncClock(0, 0, 0);
            player.syncDelay(getenv("GF_SOLO_DELAY") ? atof(getenv("GF_SOLO_DELAY")) : 0);   // (the control: a delay set must show)
            player.syncChirps(true);
            soloStage_ = 1;
            soloAt_ = time;
        } else if (time - soloAt_ > 2) {   // the callbacks' line has its points
            const double at = player.syncNow() + 1.5;
            player.syncStart(at, 0);
            printf("[solo] start at %.6f\n", at);
            fflush(stdout);
            soloStage_ = 2;
        }
    }
#endif
    for (const std::vector<uint8_t>& raw : connect.receiveTogether()) {
        Message m;
        if (unpack(raw, m)) togetherHandle(m);
    }
    if (time - devicesAt > 1) {
        devicesAt = time;
        const Connect::View cv = connect.view();
        devicesPaired = !cv.paired.empty();
        devicesLinked.clear();
        for (const Connect::Paired& p : cv.paired) if (p.linked) devicesLinked.push_back(p);
    }
    // not taken yet: this device simply goes on playing, so nothing is lost
    if (!handoffPeer.empty() && (time - handoffSince > 60 || !connect.linkedTo(handoffPeer))) {
        toast(handoffName + " did not answer: playing on here");
        handoffPeer.clear();
    }
    if (tog.stage == Tog::Off) return;
    if (!connect.linkedTo(tog.peerId)) {
        if (tog.stage != Tog::On || tog.startAt == 0) { togetherEnd("The link to " + tog.peerName + " ended: playing on here"); return; }
        if (!tog.linkLost) {
            tog.linkLost = true;
            tog.lostAt = time;
            tog.awaitingResume = false;
            toast("The link to " + tog.peerName + " dropped: playing on, waiting for it");
        } else if (time - tog.lostAt > kLostSeconds) { togetherEnd("The link to " + tog.peerName + " ended: playing on here"); return; }
    } else if (tog.linkLost && tog.host) {
        tog.linkLost = false;
        togetherResume();
    }
    if (tog.awaitingResume && time - tog.resumedAt > kResumeSeconds) { togetherEnd(tog.peerName + " left the session"); return; }
    const Connect::Clock c = connect.clock();
    if (tog.host) player.syncClock(0, 0, 0);
    else if (c.samples > 0) player.syncClock(c.at, c.offset, c.slope);
    player.syncDelay(store.togetherDelay[std::clamp(store.togetherOutput, 0, 2)] / 1000.0);
    if (tog.stage == Tog::Inviting && time - tog.since > kAnswerSeconds) { togetherEnd(tog.peerName + " did not answer"); return; }
    togetherPrintsTick();
    if (tog.host && tog.stage == Tog::On && !tog.linkLost && tog.whoSent != int(store.order)) togetherSendWho();
    if (tog.stage != Tog::On || tog.song < 0) return;

    if (tog.rejoining) {
        if (!player.preparing()) {
            tog.rejoining = false;
            if (tog.rejoinNew) {
                tog.prepared = tog.saidReady = true;
                togetherLoadNow();
                if (tog.startAt == 0) tog.startAt = tog.rejoinAt;
            } else {
                player.load(lib.tracks[size_t(tog.tracks[size_t(tog.song)])].path, true);
            }
            if (!tog.paused) player.syncStart(tog.rejoinAt, tog.position);
        }
        return;
    }
    if (player.takeLost() && tog.startAt > 0 && !tog.paused && !tog.natural && tog.tracks[size_t(tog.song)] >= 0) {
        const double moment = player.syncNow() + kRejoinAhead, position = player.syncPositionAt(moment);
        const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[size_t(now.track)].duration;
        if (dur <= 0 || position < dur - 1) {   // (near its end: the next song starts in time anyway)
            togetherRejoin(tog.song, moment, position);
#ifdef GF_DEV
            logLine("Together: off the clock, rejoining at " + std::to_string(int(position)) + " s");
#endif
            return;
        }
    }

    if (!tog.prepared && !player.preparing()) {
        tog.prepared = true;
        if (!tog.natural) togetherLoadNow();
    }
    if (tog.linkLost && tog.host) tog.theyReady = true;   // the song after this one goes ahead: the other device hears where things stand when it is back
    // ready: the file is open, and this device knows the time
    if (tog.prepared && !tog.saidReady && (tog.host || c.samples >= kClockEnough)) {
        tog.saidReady = true;
        if (!tog.host) {
            Message r;
            r.kind = Kind::Ready;
            r.serial = tog.serial;
            togetherSend(r);
        }
    }
    if (tog.host && tog.saidReady && tog.theyReady && tog.startAt == 0) {
        double at = player.syncNow() + kStartAhead;
        if (tog.natural) {
            const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[size_t(now.track)].duration;
            at = std::max(at, player.syncNow() + std::max(0.0, dur - player.position()) + kGapBetween);
        }
        Message g;
        g.kind = Kind::Go;
        g.serial = tog.serial;
        g.song = tog.song;
        g.position = tog.position;
        g.at = at;
        togetherSend(g);
        togetherStartAt(at);
    }
    if (tog.host && !tog.linkLost && tog.startAt == 0 && time - tog.readySince > kReadySeconds) { togetherEnd(tog.peerName + " could not keep up: playing on here"); return; }
    // the track playing plays out; if it runs past the moment the next one starts, it stops for it
    if (tog.natural && tog.startAt > 0 && !tog.loaded && player.syncNow() > tog.startAt - 0.08) togetherFinished();
    if (tog.joinArmed && !tog.paused) togetherJoinStep();
    if (tog.host && !tog.natural && !tog.nextSent && !tog.planNone && !tog.paused && tog.startAt > 0 && player.syncRunning()) togetherPlanJoin();
    // the starter looks ahead to the next song
    if (tog.host && !tog.natural && !tog.nextSent && !tog.paused && tog.startAt > 0 && player.syncRunning() && now.track >= 0) {
        const double dur = now.info.duration > 0 ? now.info.duration : lib.tracks[size_t(now.track)].duration;
        if (dur > 0 && dur - player.position() < kNextBefore) {
            tog.nextSent = true;
            int p = q.pos + 1;
            if (store.order == Order::RepeatOne) p = q.pos;
            if (p < int(q.order.size())) {
                const int song = firstHeld(tog.have, p);
                if (song >= p || store.order != Order::Default) togetherPrepare(song, 0, true);
            }
        }
    }
    if (tog.natural && tog.loaded && tog.startAt > 0 && player.syncNow() > tog.startAt + 0.2) { tog.natural = false; tog.nextSent = false; }
#ifdef GF_DEV
    {
        const Uint64 at = SDL_GetPerformanceCounter();
        if (tog.frameAt) tog.worstFrame = std::max(tog.worstFrame, double(at - tog.frameAt) * 1000.0 / double(SDL_GetPerformanceFrequency()));
        tog.frameAt = at;
    }
    if (time - tog.loggedAt > 5) {
        tog.loggedAt = time;
        const Player::Trace tr = player.takeTrace();
        char line[220];
        snprintf(line, sizeof line, "Together: stream error %.2f ms, round trip %.1f ms, offset %.1f ms, slope %.1f ppm, clock samples %d, freezes %u, worst frame %.1f ms, worst audio %.1f ms, late %u, missed %u", player.syncError() * 1e3, c.rtt * 1e3, c.offset * 1e3, c.slope * 1e6, c.samples, player.syncFreezes(), tog.worstFrame, tr.worstMs, tr.late, tr.lockMiss);
        tog.worstFrame = 0;
        logLine(line);
    }
#endif
}
