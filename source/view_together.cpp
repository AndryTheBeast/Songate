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
#include <unordered_map>

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

void App::togetherSend(const Message& m) {
    if (tog.host && tog.stage != Tog::Off) {   // the starter: to every device in the session
        for (const Tog::Member& mb : tog.members) if (mb.state == 2) togetherSendTo(mb.id, m);
        return;
    }
    togetherSendTo(tog.stage != Tog::Off ? tog.peerId : !handoffPeer.empty() ? handoffPeer : togFrom_, m);
}

std::string App::togetherStateOf(const Tog::Member& mb) {
    return mb.state == 0 ? "Asked to join" : mb.state == 1 ? "Joining" : mb.lost ? "Link dropped: playing on, waiting for it"
         : mb.resumeDue || mb.awaiting ? "Getting in time" : "In time";
}

App::Tog::Member* App::togMember(const std::string& id) {
    for (Tog::Member& mb : tog.members) if (mb.id == id) return &mb;
    return nullptr;
}

std::string App::togetherNames() const {
    std::vector<std::string> n;
    if (tog.host) {
        for (const Tog::Member& mb : tog.members) n.push_back(mb.name);
    } else {
        n.push_back(tog.peerName);
        for (const std::string& o : tog.others) n.push_back(o);
    }
    std::string s;
    for (size_t i = 0; i < n.size(); i++) s += (i == 0 ? "" : i + 1 == n.size() ? " and " : ", ") + n[i];
    return s;
}

void App::togetherInvite(const std::string& id, const std::string& name) {
    if (!tog.host || tog.stage == Tog::Off || togMember(id) || id.empty()) return;
    Tog::Member mb;
    mb.id = id;
    mb.name = name;
    mb.since = time;
    tog.members.push_back(mb);
    Message m;
    m.kind = Kind::Invite;
    togetherSendTo(id, m);
    logLine("Together: asked " + name + " in");
}

void App::togetherDrop(const std::string& id, const std::string& why, bool tell) {
    logLine("Together: one out (" + why + ")" + (tell ? ", told to leave" : ""));
    if (tell) {
        Message l;
        l.kind = Kind::Leave;
        togetherSendTo(id, l);
    }
    std::erase_if(tog.members, [&](const Tog::Member& mb) { return mb.id == id; });
    if (tog.members.empty()) { togetherEnd(why + (tog.stage == Tog::On ? ": playing on here" : "")); return; }
    toast(why);
    if (tog.stage == Tog::On) {
        togetherHaveAll();   // (the songs every device holds may be more now)
        togetherSendMembers();
    }
}

/** The starter: each device in hears who else is (by name), so its card can say. */
void App::togetherSendMembers() {
    for (const Tog::Member& to : tog.members) {
        if (to.state != 2) continue;
        Message m;
        m.kind = Kind::Members;
        for (const Tog::Member& mb : tog.members)
            if (mb.state == 2 && mb.id != to.id) m.list += (m.list.empty() ? "" : "\n") + mb.name;
        togetherSendTo(to.id, m);
    }
}

bool App::togetherHaveAll() {
    std::vector<uint8_t> all(tog.songs.size(), 1);
    bool any = false;
    for (const Tog::Member& mb : tog.members) {
        if (mb.state != 2) continue;
        if (mb.have.size() != tog.songs.size()) {
            if (mb.lost) continue;   // (it hears the queue again when it is back)
            return false;
        }
        any = true;
        for (size_t i = 0; i < all.size(); i++) all[i] &= mb.have[i];
    }
    if (!any) return false;
    tog.have = std::move(all);
    tog.missing = int(std::count(tog.have.begin(), tog.have.end(), uint8_t(0)));
    return true;
}

void App::togetherSendTo(const std::string& peer, const Message& m) {
    if (!peer.empty()) connect.send(peer, pack(m));
}

together::Song App::togetherSong(int track) const {
    const Track& t = lib.tracks[size_t(track)];
    Song s;
    s.duration = t.duration;
    Identity::Known k;
    if (const_cast<Identity&>(identity).find(t.path, k)) { s.exact = k.exact; s.loose = k.loose; s.duration = k.duration > 0 ? k.duration : t.duration; }
    if (s.loose.empty()) s.loose = Identity::looseKey(t.artist, t.albumArtist, t.album, t.title);
    return s;
}

bool App::togHere(int song) const {
    if (song < 0 || song >= int(tog.tracks.size())) return false;
    const int t = tog.tracks[size_t(song)];
    return t >= 0 || (t == kTogServer && song < int(tog.srv.size()) && !tog.srv[size_t(song)].id.empty());
}

std::string App::togPath(int song) {
    if (song < 0 || song >= int(tog.tracks.size())) return {};
    const int t = tog.tracks[size_t(song)];
    if (t >= 0) return lib.tracks[size_t(t)].path;
    if (t != kTogServer || song >= int(tog.srv.size())) return {};
    const Subsonic::Song& s = tog.srv[size_t(song)];
    std::string path;
    if (subsonic.downloaded(s.id, path) || (subsonic.fetched(s.id, path) && !path.empty())) return path;
    subsonic.askSong(s.id, s.suffix, true);   // (wanted now: before what else waits)
    return {};
}

double App::togDuration(int song) const {
    if (song < 0 || song >= int(tog.tracks.size())) return 0;
    const int t = tog.tracks[size_t(song)];
    if (t >= 0) return lib.tracks[size_t(t)].duration;
    return song < int(tog.srv.size()) ? tog.srv[size_t(song)].duration : 0;
}

int App::togIndexNow() const { return srvq_.on && !srvq_.session ? srvq_.pos : q.pos; }

void App::togSetPos(int song) {
    if (song < 0 || song >= int(tog.tracks.size())) return;
    const int t = tog.tracks[size_t(song)];
    if (t == kTogServer) {
        if (tog.host) { srvq_.pos = song; return; }
        for (int i = 0; i < int(srvq_.songs.size()); i++) if (srvq_.songs[size_t(i)].id == tog.srv[size_t(song)].id) { srvq_.pos = i; return; }
        return;
    }
    if (tog.host) { q.pos = song; return; }
    const auto it = std::find(q.tracks.begin(), q.tracks.end(), t);
    if (it != q.tracks.end()) q.pos = int(it - q.tracks.begin());
}

/** A session's song into the player: your own as loadTrack; a server one as the server's queue shows it (its file fetched before). */
void App::togLoad(int song, bool paused, bool start, double fade, const automix::Plan* mix) {
    if (song < 0 || song >= int(tog.tracks.size())) return;
    const int t = tog.tracks[size_t(song)];
    if (t >= 0) { loadTrack(t, paused, start, fade, mix); return; }
    if (t != kTogServer) return;
    if (!srvq_.on || !extOn) {   // (shown as the server's queue: the other device's session songs, or the starter's queue just chosen)
        srvq_.on = true;
        std::string kind = subsonic.view().kind.empty() ? std::string("your server") : subsonic.view().kind;
        kind[0] = char(toupper(uint8_t(kind[0])));
        extBegin(kind);
    }
    togSetPos(song);
    if (!mix) endJoinShow();
    if (!mix && mixOld.on) mixOld = MixOld{};
    xfadeChecked = stretchBegun = mixPrepared = false;
    serverShow(srvq_.pos);
    if (!start) return;
    const std::string path = togPath(song);
    if (path.empty()) return;   // (asked: the session waits for it before saying Ready)
    player.load(path, paused, fade, mix);
    srvFile_ = path;
    wave.request(path);
    subsonic.askFileInfo(tog.srv[size_t(song)].id, path);
    srvInfoFor_ = tog.srv[size_t(song)].id;
    srvq_.waitFor.clear();
    for (int n = song + 1; n < int(tog.tracks.size()); n++)   // the next server song of the session, ahead
        if (tog.tracks[size_t(n)] == kTogServer) { subsonic.askSong(tog.srv[size_t(n)].id, tog.srv[size_t(n)].suffix); break; }
}

size_t App::togetherQueueSig() const {
    if (srvq_.on && !srvq_.session) {
        size_t sig = srvq_.songs.size() * 31 + 7;
        for (const Subsonic::Song& s : srvq_.songs) sig = sig * 1000003u + std::hash<std::string>()(s.id);
        return sig;
    }
    size_t sig = q.order.size() * 31 + q.tracks.size();
    for (int o : q.order) sig = sig * 1000003u + size_t(q.tracks[size_t(o)]);
    return sig;
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
    if (tog.host || togetherShiftKnown(song) || tog.printing.count(song) || !togHere(song)) return;
    const std::string path = togPath(song);
    if (path.empty()) return;   // (a server copy still coming: asked again when it is prepared)
    const double from = printFrom(std::min(tog.songs[size_t(song)].duration, togDuration(song)));
    if (from < 0) { tog.shift[song] = 0; return; }   // too short to print: as before
    Tog::Printing& p = tog.printing[song];
    p.from = from;
    p.since = time;
    Message m;
    m.kind = Kind::Print;
    m.song = song;
    m.position = from;
    togetherSend(m);
    prints.want({ togPrintGen, song, false, path, from - kPrintReach, kPrintSeconds + 2 * kPrintReach });
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
        if (togHere(n)) { togetherAskPrint(n); break; }
    if (!togetherShiftKnown(song) && tog.tracks[size_t(song)] == kTogServer && togPath(song).empty()) tog.shift[song] = 0;   // (its file not here yet: unshifted)
    if (togetherShiftKnown(song)) { tog.waiting = false; togetherPrepareHere(song, togetherMine(song, position), natural); return; }
    tog.waiting = true;
    tog.waitSong = song;
    tog.waitPosition = position;
    tog.waitNatural = natural;
    tog.waitSince = time;
}

/** Everyone's first step: the background player stands aside, and the sound starts following the clock. */
void App::togetherBegin(const std::string& peerId, const std::string& peerName, bool host) {
    const bool hadServer = srvq_.on && !srvq_.session;
    tog = Tog{};
    togPrintGen++;
    tog.host = host;
    tog.hadServer = hadServer;
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
    connect.clockWanted({ peerId });
    player.syncEnable(true);
#ifdef GF_DEV
    player.syncChirps(togChirps);   // on by default in a dev build: a test without them measures nothing
#endif
    player.trap = [this](bool seek, double seconds) { return togetherOn() && togetherTrap(seek, seconds); };
}

void App::togetherEnd(const std::string& why) {
    if (tog.stage == Tog::Off) return;
    logLine("Together: the session ended here: " + why);
    const bool guest = togetherGuest(), wasBg = tog.hadBackground;
    const double ownAt = tog.savedPosition;
    bool inQueue = true, restored = false;
    player.trap = nullptr;
    player.syncEnable(false);
    player.tones({});
#ifdef GF_DEV
    // the test chirps off with the session (the user, 2026-11-07: after leaving, a server song, played by this player and not the
    // background one, chirped every second)
    player.syncChirps(false);
#endif
    measurer.stop();
    connect.clockWanted({});
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
    const bool hadServer = guest && tog.hadServer;
    if (srvq_.session) {   // the session's server songs: gone with it (one playing: your own song back, as below)
        if (srvq_.on) inQueue = false;
        srvq_ = ServerQueue{};   // (its background player: tog.hadBackground's, below)
        extOn = false;
        extClearAt = 0;
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
    if (hadServer && !srvSaved_.songs.empty()) {   // the server's queue was playing here before: back, paused where it was
        if (isPlaying()) player.stop();
        srvLoadPaused_ = true;
        srvSeek_ = srvSavedAt_;
        playServer(srvSaved_.songs, std::max(0, srvSaved_.pos), srvSaved_.name);
        srvq_.hadBackground = wasBg;   // (the background player gets the music back after this queue, as before the session)
    } else if (wasBg && store.background) setBackground(true);
    if (!why.empty()) toast(why);
}

void App::togetherOwnBack(double at, bool play) {
    if (q.pos < 0 || q.pos >= int(q.order.size())) return;
    loadTrack(q.tracks[size_t(q.order[size_t(q.pos)])], !play);
    if (at > 1) player.seekNow(at);
}

void App::togetherStart(const std::string& peerId, const std::string& peerName) {
    if (tog.stage != Tog::Off) return;
    const bool srvQueue = srvq_.on && !srvq_.songs.empty() && srvq_.pos >= 0;
    if (!srvQueue && (q.order.empty() || q.pos < 0)) { toast("Play something first: the session starts from this queue"); return; }
    togetherBegin(peerId, peerName, true);
    tog.stage = Tog::Inviting;
    togetherInvite(peerId, peerName);
}

void App::togetherLeave() {
    Message m;
    m.kind = Kind::Leave;
    if (tog.host) for (const Tog::Member& mb : tog.members) togetherSendTo(mb.id, m);   // the starter leaving ends it for everyone
    else togetherSend(m);
    if (tog.host) {   // the starter: its song stays, paused where it was (the user, 2026-11-07: it stopped, and Play did nothing after)
        const bool playing = isPlaying();
        togetherEnd("You left the session");
        if (playing && player.loaded() && isPlaying()) player.togglePause();
        return;
    }
    if (isPlaying()) player.stop();
    togetherEnd("You left the session");
}

/** The starter: the list as it is now goes across, then the other says which songs it holds. */
void App::togetherSendQueue() {
    tog.songs.clear();
    tog.tracks.clear();
    tog.srv.clear();
    if (srvq_.on && !srvq_.session) {   // the server's queue: its songs as the server names them (the others: their own copies, or the server's)
        for (const Subsonic::Song& s : srvq_.songs) { tog.songs.push_back(serverSongAcross(s)); tog.tracks.push_back(kTogServer); tog.srv.push_back(s); }
    } else {
        tog.songs.reserve(q.order.size());
        for (int o : q.order) { tog.songs.push_back(togetherSong(q.tracks[size_t(o)])); tog.tracks.push_back(q.tracks[size_t(o)]); }
        tog.srv.assign(tog.songs.size(), Subsonic::Song{});
    }
    tog.signature = togetherQueueSig();
    Message m;
    m.kind = Kind::Queue;
    m.songs = tog.songs;
    for (Tog::Member& mb : tog.members) {   // every device in or joining; each says which songs it holds
        if (mb.state < 1) continue;
        mb.have.clear();
        togetherSendTo(mb.id, m);
    }
    tog.have.clear();
}

/** The starter: this song, from here, on both. natural: the track playing plays out first. */
void App::togetherPrepare(int song, double position, bool natural) {
    if (song < 0 || song >= int(tog.tracks.size())) return;   // nothing every device holds: what plays stays, the session with it
    tog.serial++;
#ifdef GF_DEV
    logLine("Together: prepare song " + std::to_string(song) + " at " + std::to_string(int(position)) + " s" + (natural ? ", after this one" : ""));
#endif
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
    const bool srvQueue = srvq_.on && !srvq_.session;
    if (pos < 10 || (now.track < 0 && !srvQueue)) return;
    const double dur = now.info.duration > 0 ? now.info.duration : srvQueue ? 0 : lib.tracks[size_t(now.track)].duration;
    const double left = dur - pos;
    if (dur <= 0 || left <= 0.25) return;
    const int p = srvQueue ? (srvq_.pos + 1 < int(srvq_.songs.size()) ? srvq_.pos + 1 : -1) : crossfadeNext(left);
    if (p < 0) { if (srvQueue) tog.planNone = true; return; }   // looked at again next frame, as updateAutomix does
    const int song = firstHeld(tog.have, p);
    if (song < 0) return;
    automix::Plan pl;
    if (!srvQueue && automixQueue()) {   // (no Automix for a server's songs: the user, 2026-11-06)
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
    m.flag = !srvQueue && automixQueue();
    togetherSend(m);
    togetherArmJoin(song, pl);
}

void App::togetherArmJoin(int song, const automix::Plan& plan) {
    tog.plan = plan;
    tog.joinAutomix = !(srvq_.on && !srvq_.session) && automixQueue();   // (the other device: set from the message just after)
    tog.joinShifted = false;
    tog.joinSong = song;
    tog.joinArmed = tog.nextSent = true;
    stretchBegun = mixPrepared = false;
}

/** Both devices, each by its own position: the two are together, so the same plan lands on the same sample. */
void App::togetherJoinStep() {
    const automix::Plan& pl = tog.plan;
    const double pos = player.position();
    if (!togHere(tog.joinSong)) { tog.joinArmed = false; return; }
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
        const std::string path = togPath(tog.joinSong);
        if (path.empty()) { mixPrepared = false; return; }   // (its server file still coming: asked again next frame)
        player.prepare(path, pl.offset);
    }
    if (pos < pl.startOut - 0.6 || player.preparing()) return;
    tog.joinArmed = false;
    tog.nextSent = false;
    tog.planNone = false;
    keepMixOld();
    const automix::Plan plan = pl;
    togSetPos(tog.joinSong);
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
    togLoad(tog.song, false, true, plan.length, &plan);
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
    tog.fetchSong = -1;
    if (!togHere(song)) { tog.prepared = true; tog.loaded = true; return; }   // not here: this device sits this one out
    const std::string path = togPath(song);
    if (path.empty()) { tog.fetchSong = song; tog.fetchPos = position; tog.fetchRejoin = false; return; }   // (its server file first: togetherTick)
    player.prepareTimed(path, position);
}

void App::togetherLoadNow() {
    tog.loaded = true;
    if (!togHere(tog.song)) { player.stop(); return; }
    togSetPos(tog.song);
    togLoad(tog.song, true, true);
    if (std::fabs(player.position() - tog.position) > 0.01) player.seekNow(tog.position);
}

void App::togetherStartAt(double at) {
    tog.startAt = at;
    tog.paused = false;
    if (!tog.natural && !tog.loaded) togetherLoadNow();
    if (tog.loaded && togHere(tog.song)) player.syncStart(at, tog.position);
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
    tog.fetchSong = -1;
    const std::string path = togPath(song);
    if (path.empty()) { tog.fetchSong = song; tog.fetchPos = position; tog.fetchRejoin = true; return; }   // (its server file first)
    player.prepareTimed(path, position, true);
}

/** The starter, a link back or a device just in: where things stand, for that device to compare with its own. Sent once no next song
 *  is half way (togetherMembersTick), so the others' Prepare and Go are left as they are. */
void App::togetherResumeTo(Tog::Member& mb) {
    const bool stopped = tog.paused;
    Message m;
    m.kind = Kind::Resume;
    m.serial = tog.serial;
    m.song = tog.song;
    m.flag = stopped;
    const double at = player.syncNow();
    m.at = at;
    m.position = stopped ? player.position() : std::max(0.0, player.syncPositionAt(at));
    togetherSendTo(mb.id, m);
    logLine("Together: told " + mb.name + " where things stand: song " + std::to_string(m.song) + " at " + std::to_string(int(m.position)) + " s" + (stopped ? ", paused" : ""));
    tog.whoSent = -1;
    mb.awaiting = true;
    mb.resumeDue = false;
    mb.resumedAt = time;
    // A join planned while the link was down went nowhere (the user, 2026-11-03: its plan left in the drop, and the other device
    // stopped at the song's end while this one joined): sent again, after the Resume that sets where the other device is
    if (tog.joinArmed) {
        Message j;
        j.kind = Kind::Join;
        j.serial = tog.serial;
        j.song = tog.joinSong;
        j.plan = tog.plan;
        j.flag = tog.joinAutomix;
        togetherSendTo(mb.id, j);
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
/** A server song of the queue chosen (Up next, next, previous): the session's, as togetherPlayAt does for yours. */
bool App::togetherPlayServer(int i) {
    if (!togetherOn()) return tog.stage != Tog::Off;   // (a session starting: nothing yet)
    if (i < 0 || i >= int(srvq_.songs.size())) return true;
    if (tog.host && !srvq_.session) {
        if (togetherQueueSig() != tog.signature) { togetherSendQueue(); tog.pendingSong = i; return true; }
        const int song = firstHeld(tog.have, i);
        if (song < 0) { toast("None of these songs is on every device"); return true; }
        togetherPrepare(song, 0, false);
        return true;
    }
    if (tog.linkLost) { toast("Waiting for " + tog.peerName); return true; }
    if (togetherLocked()) { toast("Only " + tog.peerName + " changes what plays in this session"); return true; }
    Message m;
    m.kind = Kind::Ask;
    m.what = AskSong;
    m.song = -1;
    for (int k = 0; k < int(tog.srv.size()); k++)
        if (tog.tracks[size_t(k)] == kTogServer && tog.srv[size_t(k)].id == srvq_.songs[size_t(i)].id) { m.song = k; break; }
    if (m.song >= 0) togetherSend(m);
    return true;
}

bool App::togetherPlayAt(int orderPos) {
    if (tog.host) {
        if (srvq_.on && !srvq_.session) {   // a queue of yours chosen while the server's was the session's: yours now
            serverLeave();
            tog.hadBackground = tog.hadBackground || srvq_.hadBackground;   // (its background player: back when the session ends)
            srvq_.hadBackground = false;
        }
        const size_t sig = togetherQueueSig();
        if (sig != tog.signature) {   // a new queue here: the other device hears of it first
            togetherSendQueue();
            tog.pendingSong = orderPos;
            return true;
        }
        const int song = firstHeld(tog.have, orderPos);
        if (song < 0) { toast("None of these songs is on every device"); return true; }
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
            const std::string id = togFrom_;
            for (const Connect::Paired& p : connect.view().paired) if (p.id == id) name = p.name;
            tog.stage = Tog::Off;
            confirm(ICON_LINK, "Devices", "Listen together", name, "Listen together with " + name + "?",
                    "One queue on every device in the session, in time. Your own queue comes back when you leave, and nothing of the session is saved.", "Listen", [this, id, name] {
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
    case Kind::Answer: {
        Tog::Member* mb = tog.host ? togMember(togFrom_) : nullptr;
        if (!mb || mb->state != 0) return;
        if (!m.flag) { togetherDrop(mb->id, mb->name + " said no", false); return; }
        logLine("Together: " + mb->name + " said yes");
        mb->state = 1;
        if (tog.stage == Tog::Inviting) tog.stage = Tog::Joining;
        if (tog.songs.empty()) {
            togetherSendQueue();
        } else {   // one more in a session under way: the queue as it is, to it alone
            Message qm;
            qm.kind = Kind::Queue;
            qm.songs = tog.songs;
            mb->have.clear();
            togetherSendTo(mb->id, qm);
        }
        return;
    }
    case Kind::Queue: {
        if (tog.host || tog.stage == Tog::Off) return;
        togetherTakeQueue(m, false);
        return;
    }
    case Kind::Have: {
        Tog::Member* mb = tog.host ? togMember(togFrom_) : nullptr;
        if (!mb || mb->state < 1 || m.have.size() != tog.songs.size()) return;
        const bool late = mb->state == 1 && tog.stage == Tog::On;   // joined, or back, in a session under way
        mb->have = m.have;
        mb->state = 2;
        mb->mic = m.song > 0 && (m.song & 1);
        mb->needs = m.song > 0 && (m.song & 2);
        logLine("Together: " + mb->name + " holds " + std::to_string(std::count(m.have.begin(), m.have.end(), uint8_t(1))) + " of " +
                std::to_string(m.have.size()) + " songs" + (mb->mic ? ", has a microphone" : "") + (mb->needs ? ", its output never measured" : "") +
                (late ? ", joining a session under way" : ""));
        if (late) mb->resumeDue = true;   // where things stand, once no next song is half way
        if (!togetherHaveAll()) return;   // others still to say
        const bool first = tog.stage != Tog::On;
        tog.stage = Tog::On;
        tog.whoSent = -1;
        togetherSendMembers();
        if (first) {
            const int song = firstHeld(tog.have, togIndexNow());
            if (song < 0) { togetherEnd("None of these songs is on every device"); return; }
            const double pos = song == togIndexNow() && player.loaded() ? player.position() : 0;
            // an output never measured, here or there: the chirps first (the user, 2026-11-05: the first time, then Measure again)
            bool needs = !store.togetherMeasured[std::clamp(store.togetherOutput, 0, 2)];
            for (const Tog::Member& o : tog.members) needs = needs || (o.state == 2 && o.needs);
            if (needs && togetherMeasure(true, song, pos)) return;
            togetherPrepare(song, pos, false);
        } else if (late && mb->needs && tog.measure.stage == 0 && togetherListener() != "") {
            togetherMeasure(false);   // one more device, its output never measured: the chirps now (the music pauses for them)
        } else if (tog.pendingSong >= 0) {
            const int song = firstHeld(tog.have, tog.pendingSong);   // (the place in the queue just sent: yours or the server's)
            tog.pendingSong = -1;
            if (song >= 0) togetherPrepare(song, 0, false);
        }
        return;
    }
    case Kind::Handoff: {
        if (tog.stage != Tog::Off) {
            Message no;
            no.kind = Kind::Taken;
            togetherSendTo(togFrom_, no);   // (to the one that asked: a session's starter would tell every device in it)
            return;
        }
        std::string name = "The other device";
        const std::string id = togFrom_;
        for (const Connect::Paired& p : connect.view().paired) if (p.id == id) name = p.name;
        if (store.handoffFrom.find(id) != std::string::npos) { handoffTake(m, id, name, time); return; }
        confirm(ICON_LINK, "Devices", "Handoff from " + name, std::to_string(m.songs.size()) + (m.songs.size() == 1 ? " song" : " songs"), "Take over from " + name + "?",
                "Its queue and its place in the song move here, and it pauses. This replaces your queue; you can undo that for two minutes. After this, " + name + " does not ask again.",
                "Take over", [this, m, name, id, at = time] {
            if (!id.empty()) { store.handoffFrom += id + " "; persistSoon(0.3f); }
            handoffTake(m, id, name, at);
        });
        sheet.askSafe = true;
        sheet.askYesIcon = ICON_CHECK;
        return;
    }
    case Kind::Taken:
        if (handoffPeer.empty() || togFrom_ != handoffPeer) return;   // only the device asked answers
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
            if (!tog.host || !togHere(m.song) || m.position < 0 || m.position > 30) return;
            const std::string path = togPath(m.song);
            if (path.empty()) {   // (its server file still coming: an empty print, the other plays it unshifted)
                Message e;
                e.kind = Kind::Print;
                e.flag = true;
                e.song = m.song;
                e.position = m.position;
                togetherSendTo(togFrom_, e);
                return;
            }
            prints.want({ togPrintGen, m.song, true, path, m.position, kPrintSeconds });
            return;
        }
        if (auto it = tog.printing.find(m.song); !tog.host && it != tog.printing.end() && std::fabs(it->second.from - m.position) < 1e-3) {
            it->second.theirs = m.print;
            it->second.theirsIn = true;
        }
        return;
    case Kind::Ready:
        if (Tog::Member* mb = tog.host ? togMember(togFrom_) : nullptr; mb && m.serial == tog.serial) {
            mb->readySerial = m.serial;
            mb->awaiting = false;
        }
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
        if (!togHere(m.song)) return;   // (the server's copy counts as here)
        if (connect.clock().samples < kClockEnough) {   // just in: its clock first (the tick acts on it then)
            tog.heldResume = m;
            tog.resumeHeld = true;
            return;
        }
        togetherApplyResume(m);
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
            if (Tog::Member* mb = togMember(togFrom_); mb && mb->state == 2 && !mb->lost && !mb->awaiting) mb->resumeDue = true;
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
            static const char* names[] = { "Default order", "Repeat playlist", "Repeat song", "Shuffle" };
            std::string from = tog.peerName;
            if (const Tog::Member* mb = togMember(togFrom_)) from = mb->name;
            toast(std::string(names[int(store.order)]) + ", from " + from);   // (setOrder saves it)
        }
        return;
    case Kind::Leave:
        if (tog.host) {   // one device out; the rest play on
            if (const Tog::Member* mb = togMember(togFrom_)) togetherDrop(mb->id, mb->name + " left the session", false);
            return;
        }
        if (tog.stage != Tog::Off) togetherEnd(tog.peerName + " left the session");
        return;
    case Kind::Tones: {   // a listener: chirp in its slot; perhaps listen
        if (tog.host || tog.stage == Tog::Off || m.serial < 1 || m.serial > 16 || m.song < 0 || m.song >= m.serial) return;
        std::vector<double> at;
        for (int r = 0; r < micm::kRounds; r++) at.push_back(micm::chirpTime(m.at, int(m.serial), m.song, r));
        player.tones(at, m.song);
        logLine("Together: chirps in slot " + std::to_string(m.song) + " of " + std::to_string(m.serial) + (m.flag ? ", this device listening" : ""));
        tog.measure = Tog::Measure{};
        tog.measure.stage = 1;
        tog.measure.endAt = micm::chirpTime(m.at, int(m.serial), int(m.serial) - 1, micm::kRounds - 1) + 1;
        if (m.flag) {
            tog.measure.listening = true;
            measurer.start([this](double steady) { return player.sharedAt(steady); }, m.at, int(m.serial));
        }
        return;
    }
    case Kind::Delay: {
        if (tog.stage == Tog::Off) return;
        if (!tog.host) {   // from the starter: this device's correction
            char* end = nullptr;
            const double ms = std::strtod(m.list.c_str(), &end);
            if (end != m.list.c_str() && std::isfinite(ms) && std::fabs(ms) < 1000) togetherApplyDelay(ms);
            return;
        }
        // the starter: what the listener heard, by slot
        if (tog.measure.stage != 1 || togFrom_ != tog.measure.listener) return;
        micm::Result r;
        r.done = true;
        r.lateMs.assign(tog.measure.slots.size(), 0);
        r.heard.assign(tog.measure.slots.size(), false);
        if (!m.list.empty() && m.list[0] == '!') r.error = m.list.substr(1);
        size_t a = 0;
        while (a < m.list.size() && r.error.empty()) {
            size_t b = m.list.find('\n', a);
            if (b == std::string::npos) b = m.list.size();
            const std::string l = m.list.substr(a, b - a);
            a = b + 1;
            int k = -1;
            double ms = 0;
            if (sscanf(l.c_str(), "%d %lf", &k, &ms) == 2 && k >= 0 && k < int(r.lateMs.size()) && std::isfinite(ms)) {
                r.lateMs[size_t(k)] = ms;
                r.heard[size_t(k)] = true;
            }
        }
        togetherMeasured(r);
        return;
    }
    case Kind::Members: {   // a listener: who else is in it
        if (tog.host || tog.stage == Tog::Off) return;
        tog.others.clear();
        size_t a = 0;
        while (a < m.list.size()) {
            size_t b = m.list.find('\n', a);
            if (b == std::string::npos) b = m.list.size();
            if (b > a && tog.others.size() < 16) tog.others.push_back(m.list.substr(a, b - a));
            a = b + 1;
        }
        return;
    }
    case Kind::Who:
        if (tog.host || tog.stage == Tog::Off) return;
        if (m.song >= 0 && m.song <= 3) tog.order = m.song;
        if (tog.onlyHost == m.flag) return;
        tog.onlyHost = m.flag;
        toast(m.flag ? tog.peerName + " now decides what plays" : "Every device changes what plays again");
        return;
    }
}

/** The starter, each frame: every device's link, answer and pace. One out leaves the rest playing; the last one out ends it. */
/** The other device: the starter's queue. Each song: your own copy (its bytes, else its tags and length); else, signed in to a server,
 *  that server's copy (by its id on the same server, or found there by its tags and length: "Both", the user, 2026-11-07). The server's
 *  song list may be asked for first: the queue waits for it, 20 s at most (togetherTick). */
void App::togetherTakeQueue(const Message& m, bool waited) {
    std::vector<Held> mine;
    mine.reserve(lib.tracks.size());
#ifdef GF_DEV
    const Uint64 mapFrom = SDL_GetPerformanceCounter();
#endif
    for (int i = 0; i < int(lib.tracks.size()); i++) mine.push_back({ togetherSong(i), i });
    std::vector<int> tracks = match(m.songs, mine);
    std::vector<Subsonic::Song> srv(m.songs.size());
    const std::string print = serverPrint();
    if (!print.empty() && std::find(tracks.begin(), tracks.end(), -1) != tracks.end()) {
        const std::vector<Subsonic::Song>* list = serverSongList();
        if (!list && !waited) {   // the server's song list first (asked now)
            tog.queueWait = true;
            tog.queueWaitSince = time;
            tog.heldQueue = m;
            return;
        }
        if (list) {
            std::unordered_map<std::string, size_t> byId;
            std::unordered_multimap<std::string, size_t> byLoose;
            for (size_t k = 0; k < list->size(); k++) byId.emplace((*list)[k].id, k);
            for (size_t k = 0; k < list->size(); k++) {
                const Subsonic::Song& so = (*list)[k];
                const std::string key = Identity::looseKey(so.artist, std::string(), so.album, so.title);
                if (!key.empty()) byLoose.emplace(key, k);
            }
            for (size_t i = 0; i < tracks.size(); i++) {
                if (tracks[i] >= 0) continue;
                const Song& s = m.songs[i];
                const Subsonic::Song* found = nullptr;
                if (s.server == print && !s.sid.empty()) { if (auto it = byId.find(s.sid); it != byId.end()) found = &(*list)[it->second]; }
                if (!found && !s.loose.empty()) {
                    auto r = byLoose.equal_range(s.loose);
                    for (auto it = r.first; it != r.second && !found; ++it)
                        if (Identity::sameLength((*list)[it->second].duration, s.duration)) found = &(*list)[it->second];
                }
                if (found) { tracks[i] = kTogServer; srv[i] = *found; }
            }
        }
    }
    tog.queueWait = false;
    tog.songs = m.songs;
    tog.tracks = tracks;
    tog.srv = std::move(srv);
    tog.otherFile.assign(tog.tracks.size(), 0);
    for (size_t i = 0; i < tog.tracks.size(); i++) {
        if (tog.tracks[i] >= 0 && (m.songs[i].exact.empty() || m.songs[i].exact != mine[size_t(tog.tracks[i])].song.exact)) tog.otherFile[i] = 1;
        // a server's copy: the same file when it is the same song on the same server; else found by its tags (another file)
        if (tog.tracks[i] == kTogServer && !(m.songs[i].server == print && m.songs[i].sid == tog.srv[i].id)) tog.otherFile[i] = 1;
    }
    tog.shift.clear();
    tog.printing.clear();
    tog.waiting = false;
#ifdef GF_DEV
    logLine("Together: " + std::to_string(m.songs.size()) + " songs matched in " + std::to_string(int(double(SDL_GetPerformanceCounter() - mapFrom) * 1000.0 / double(SDL_GetPerformanceFrequency()))) + " ms, " +
            std::to_string(std::count(tog.tracks.begin(), tog.tracks.end(), int(kTogServer))) + " from the server");
    if (getenv("GF_TOGETHER_HIDE")) for (size_t i = 1; i < tog.tracks.size(); i += 2) tog.tracks[i] = -1;   // every second song is "not here"
#endif
    if (!tog.queueTaken) {
        tog.savedQueue = q;
        tog.savedPosition = player.loaded() ? player.position() : 0;
        tog.queueTaken = true;
    }
    if (srvq_.on && !srvq_.session) serverLeave();   // the server's queue playing here: kept for after (tog.hadServer)
    Queue t;
    t.name = "Listening together";
    for (int k : tog.tracks) if (k >= 0) { t.order.push_back(int(t.tracks.size())); t.tracks.push_back(k); }
    t.pos = -1;
    q = std::move(t);
    // the session's server songs, in its order: shown as the server's queue while one of them plays
    ServerQueue sq;
    sq.session = true;
    sq.name = "Listening together";
    // (the background player it took from: given back when the session ends, never during it: serverTick hands it back as soon as
    // the server's queue is off)
    tog.hadBackground = tog.hadBackground || srvq_.hadBackground;
    sq.hadBackground = false;
    for (size_t i = 0; i < tog.tracks.size(); i++) if (tog.tracks[i] == kTogServer) sq.songs.push_back(tog.srv[i]);
    srvq_ = std::move(sq);
    Message h;
    h.kind = Kind::Have;
    for (size_t i = 0; i < tog.tracks.size(); i++) h.have.push_back(togHere(int(i)));
    h.song = (micm::available() ? 1 : 0) | (store.togetherMeasured[std::clamp(store.togetherOutput, 0, 2)] ? 0 : 2);
    togetherSend(h);
    tog.stage = Tog::On;
}

bool App::togetherMembersTick() {
    std::vector<std::string> ids;
    for (const Tog::Member& mb : tog.members) ids.push_back(mb.id);
    for (const std::string& id : ids) {
        Tog::Member* mb = togMember(id);
        if (!mb) continue;
        const bool linked = connect.linkedTo(id);
        if (mb->state == 0) {   // asked
            if (!linked) togetherDrop(id, "The link to " + mb->name + " ended", false);
            else if (time - mb->since > kAnswerSeconds) togetherDrop(id, mb->name + " did not answer", false);
        } else if (!linked) {
            if (mb->state != 2) togetherDrop(id, "The link to " + mb->name + " ended", false);
            else if (!mb->lost) {
                mb->lost = true;
                mb->lostAt = time;
                mb->awaiting = mb->resumeDue = false;
                toast("The link to " + mb->name + " dropped: playing on, waiting for it");
                logLine("Together: the link to " + mb->name + " dropped");
            } else if (time - mb->lostAt > kLostSeconds) {
                togetherDrop(id, "The link to " + mb->name + " ended", false);
            }
        } else {
            if (mb->lost) {   // back: the queue again if it changed meanwhile, else where things stand
                mb->lost = false;
                logLine("Together: the link to " + mb->name + " is back after " + std::to_string(int(time - mb->lostAt)) + " s" +
                        (mb->have.size() != tog.songs.size() ? ": the queue changed, sent again" : ""));
                if (mb->have.size() != tog.songs.size()) {
                    mb->state = 1;
                    Message qm;
                    qm.kind = Kind::Queue;
                    qm.songs = tog.songs;
                    togetherSendTo(id, qm);
                } else {
                    mb->resumeDue = true;
                }
            }
            if (mb->awaiting && time - mb->resumedAt > kResumeSeconds) togetherDrop(id, mb->name + " left the session", true);
            else if (mb->resumeDue && !tog.natural && tog.song >= 0 && (tog.startAt > 0 || tog.paused)) togetherResumeTo(*mb);
            // (only while a song is being prepared: during the chirps none is, and the clock ran from before them; the user, 2026-11-05:
            // "it is always exiting the session between the switch and android")
            else if (mb->state == 2 && !mb->resumeDue && !mb->awaiting && tog.stage == Tog::On && tog.startAt == 0 && tog.measure.stage == 0 && tog.song >= 0 &&
                     mb->readySerial != tog.serial && time - tog.readySince > kReadySeconds)
                togetherDrop(id, mb->name + " could not keep up", true);
        }
        if (tog.stage == Tog::Off) return false;
    }
    // the clock: pings with every device asked in (each listener measures its own against this one)
    std::vector<std::string> clockIds;
    for (const Tog::Member& mb : tog.members) if (mb.state >= 1) clockIds.push_back(mb.id);
    if (!clockIds.empty()) connect.clockWanted(clockIds);
    return true;
}

/** A listener: where the starter is, from its Resume (its song held here), acted on once this device knows the shared clock. */
void App::togetherApplyResume(const Message& m) {
    tog.resumeHeld = false;
    if (m.flag) {
        tog.paused = true;
        togetherRejoin(m.song, 0, togetherMine(m.song, m.position));
        return;
    }
    tog.paused = false;
    const double moment = player.syncNow() + kRejoinAhead, position = togetherMine(m.song, m.position + (moment - m.at));
    const bool kept = m.song == tog.song && player.syncRunning() && std::fabs(player.syncPositionAt(moment) - position) < kKeptTime && std::fabs(player.syncError()) < kKeptTime;
#ifdef GF_DEV
    logLine(std::string("Together: link back or joined, ") + (kept ? "kept time" : "rejoining"));
#endif
    if (!kept) togetherRejoin(m.song, moment, position);
}

std::string App::togetherListener() const {
    if (!tog.host) return "";
    if (micm::available()) return "-";
    for (const Tog::Member& mb : tog.members) if (mb.state == 2 && !mb.lost && mb.mic) return mb.id;
    return "";
}

bool App::togetherMeasure(bool first, int song, double position) {
    if (!tog.host || tog.measure.stage != 0) return false;
    const std::string who = togetherListener();
    if (who.empty()) {
        if (!first) toast("No device in this session has a microphone: set Timing by ear");
        return false;
    }
    Tog::Measure ms;
    ms.stage = 1;
    ms.listener = who == "-" ? "" : who;
    ms.first = first;
    ms.firstSong = song;
    ms.firstPos = position;
    ms.slots.push_back("");
    for (const Tog::Member& mb : tog.members) if (mb.state == 2 && !mb.lost) ms.slots.push_back(mb.id);
    // the music stops for the chirps: a session's, or (before its first song) this device's own
    if (!first && togetherOn() && !tog.paused && tog.startAt > 0) { togetherHostPause(); ms.resume = true; }
    if (first && isPlaying()) player.togglePause();
    const double t0 = player.syncNow() + 3.5;   // (a device just in needs about 3 s to know the shared clock)
    const int n = int(ms.slots.size());
    ms.endAt = micm::chirpTime(t0, n, n - 1, micm::kRounds - 1) + 0.6;
    ms.giveUpAt = ms.endAt + 8;
    for (int k = 1; k < n; k++) {
        Message m;
        m.kind = Kind::Tones;
        m.at = t0;
        m.serial = n;
        m.song = k;
        m.flag = ms.slots[size_t(k)] == ms.listener;
        togetherSendTo(ms.slots[size_t(k)], m);
    }
    std::vector<double> at;
    for (int r = 0; r < micm::kRounds; r++) at.push_back(micm::chirpTime(t0, n, 0, r));
    player.tones(at);
    ms.listening = ms.listener.empty();
    if (ms.listening) measurer.start([this](double steady) { return player.sharedAt(steady); }, t0, n);
    tog.measure = ms;
    toast("Measuring the delay: each device chirps in turn");
    logLine("Together: measuring the delay, " + std::to_string(n) + " devices, " + (ms.listener.empty() ? std::string("this device") : togMember(ms.listener) ? togMember(ms.listener)->name : std::string("?")) +
            " listening" + (first ? ", before the first song" : "") + (ms.resume ? ", the music paused for it" : ""));
    return true;
}

/** The starter: each device's lateness against the others' mean is its correction; one not heard keeps its Timing. */
void App::togetherMeasured(const micm::Result& r) {
    std::vector<std::string> missed;
    double sum = 0;
    int heard = 0;
    for (size_t k = 0; k < r.heard.size() && k < tog.measure.slots.size(); k++) if (r.heard[k]) { sum += r.lateMs[k]; heard++; }
    auto nameOf = [&](const std::string& id) {
        if (id.empty()) return std::string("This device");
        const Tog::Member* mb = togMember(id);
        return mb ? mb->name : std::string("A device");
    };
    for (size_t k = 0; k < tog.measure.slots.size(); k++) {
        char line[160];
        snprintf(line, sizeof line, "Together: slot %d (%s): %s", int(k), nameOf(tog.measure.slots[k]).c_str(),
                 k < r.heard.size() && r.heard[k] ? (std::to_string(int(std::lround(r.lateMs[k]))) + " ms late").c_str() : "not heard");
        logLine(line);
    }
    if (heard >= 2) {
        const double mean = sum / heard;
        for (size_t k = 0; k < tog.measure.slots.size(); k++) {
            const std::string& id = tog.measure.slots[k];
            if (k >= r.heard.size() || !r.heard[k]) { missed.push_back(nameOf(id)); continue; }
            const double corr = r.lateMs[k] - mean;   // heard late: plays further ahead
            if (id.empty()) { togetherApplyDelay(corr); continue; }
            Message m;
            m.kind = Kind::Delay;
            char t[32];
            snprintf(t, sizeof t, "%.1f", corr);
            m.list = t;
            togetherSendTo(id, m);
        }
        std::string said = "Delay measured";
        for (size_t i = 0; i < missed.size(); i++) said += (i == 0 ? ": not heard, so as it was: " : ", ") + missed[i];
        toast(said);
        logLine("Together: delay measured, " + std::to_string(heard) + " devices heard" + (missed.empty() ? "" : ", " + std::to_string(missed.size()) + " not"));
    } else {
        toast(r.error.empty() ? std::string("The delay could not be measured: too few chirps were heard. Timing is as it was") : "The delay could not be measured: " + r.error);
        logLine("Together: delay not measured: " + (r.error.empty() ? std::string("too few heard") : r.error));
    }
    togetherMeasureEnd();
}

void App::togetherMeasureEnd() {
    const Tog::Measure ms = tog.measure;
    tog.measure = Tog::Measure{};
    player.tones({});
    if (ms.first) {   // the session's first song, now
        if (ms.firstSong >= 0) togetherPrepare(ms.firstSong, ms.firstPos, false);
    } else if (ms.resume && togetherOn() && tog.paused) {
        togetherHostPlay();
    }
}

void App::togetherApplyDelay(double correctionMs) {
    const int out = std::clamp(store.togetherOutput, 0, 2);
    const int was = store.togetherDelay[out];
    store.togetherDelay[out] = std::clamp(int(std::lround(store.togetherDelay[out] + correctionMs)), -Store::kTimingMax, Store::kTimingMax);
    store.togetherMeasured[out] = true;
    persistSoon(0.3f);
    logLine("Together: Timing " + std::to_string(was) + " -> " + std::to_string(store.togetherDelay[out]) + " ms (output " + std::to_string(out) + ", by measuring)");
    if (!tog.host) toast("Timing set by measuring: " + std::string(store.togetherDelay[out] >= 0 ? "+" : "") + std::to_string(store.togetherDelay[out]) + " ms");
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
    // The cover it shows stays until the source sends the new one, which then crossfades in as a song of yours (the user, 2026-11-09:
    // the server's tracks flashed "no cover" and lost the fade); none within a moment, blank (extTick)
    ++loadGen;   // (a cover still loading for the song before is dropped)
    extClearAt = time + 1.5;
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
#ifdef GF_AIRPLAY
    // AirPlay speakers play two seconds behind: the lyrics and the seek bar show what is heard there
    if (!extOn && airsend.silenceHere()) return std::max(0.0, player.position() - airsend.delay());
#endif
    if (!extOn || srvq_.on) return player.position();   // (the server's queue: Songate plays it, its own position)
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
/** AirPlay speakers this device plays to: look for them while the Devices sheet is open; a pause or a seek drops what they hold, so
 *  it is heard at once (a song's change is not: Automix's joins and gapless play reach them whole, two seconds later); the song's
 *  details for speakers with a screen. */
void App::airsendTick() {
    airsend.browse(store.airplaySpeakers && (sheet.page == Sheet::Devices || sheet.page == Sheet::AirPlay));   // (Settings › Devices › Play on AirPlay speakers)
#ifdef GF_DEV
    if (sheet.page == Sheet::Devices || sheet.page == Sheet::AirPlay) {   // (dev: the search, in the log, as it changes; at most every 3 s)
        static double at = 0;
        static std::string was;
        if (time - at > 3) {
            at = time;
            // (written when what it found or holds changes, not each question and answer: they are counted, and said with it)
            const std::string st = airsend.status();
            const size_t semi = st.find(';'), last = st.rfind(';');
            const std::string gist = st.substr(0, semi) + (last != std::string::npos ? st.substr(last) : "");
            if (gist != was) { was = gist; logLine("AirPlay speakers: " + st); }
        }
    }
#endif
    // The console's background player plays with its own sound, which Songate's mix never sees (the user, 2026-11-05: "it still
    // plays on the switch even after choosing the phone"): while speakers are chosen Songate plays it itself, at the same place, as
    // listening together and AirPlay received do; and hands it back once none is
    if (airsend.tapping() && !airsendOn_) {
        airsendOn_ = true;
        if (player.remote()) {
            const bool had = now.track >= 0 && q.pos >= 0, playing = isPlaying();
            const double at = player.loaded() ? player.position() : 0;
            bg.stop();
            bg.disconnect();
            player.setRemote(nullptr);
            airsendHadBackground_ = true;
            if (had) {
                loadTrack(q.tracks[q.order[q.pos]], !playing);
                if (at > 1) player.seekNow(at);
            }
        }
    } else if (!airsend.tapping() && airsendOn_) {
        airsendOn_ = false;
        if (airsendHadBackground_ && store.background) {
            airsendHadBackground_ = false;
            setBackground(true);
        }
    }
    {   // a speaker ended it there: as an iPhone does, the music pauses here when no speaker is left (it does not burst out of this one)
        const std::vector<std::string> gone = airsend.takeDropped();
        if (!gone.empty()) {
            bool left = false;
            for (const AirSend::Speaker& k : airsend.speakers()) left = left || k.chosen;
            if (!left && isPlaying()) togglePlay();
            toast(gone.front() + (gone.size() > 1 ? " and others" : "") + " stopped playing" + (!left ? ": paused here" : ""));
        }
    }
    if (!airsend.tapping()) { airLastTrack_ = -2; airSentKey_.clear(); airWaitKey_.clear(); return; }
    const bool playing = isPlaying();
    const double pos = player.position();
    const int track = now.track;
    if (airLastTrack_ != -2 && track == airLastTrack_) {
        const bool paused = airLastPlaying_ && !playing;
        const bool seeked = playing && airLastPlaying_ && std::fabs((pos - airLastPos_) - (time - airLastAt_)) > 0.75;
        if (paused || seeked) airsend.flush();
    }
    // The song's details and cover, once they are read (the user, 2026-11-05: "the name of the music is one behind": they went at the
    // track's change, before its tags came), and as the speakers reach it, two seconds later; its progress every 15 s
    const std::string key = now.info.title + "\x1f" + now.info.artist + "\x1f" + now.info.album + "\x1f" + std::to_string(airCoverGen_);
    if (key != airSentKey_) {
        if (key != airWaitKey_) { airWaitKey_ = key; airWaitUntil_ = time + (airSentKey_.empty() ? 0.0 : airsend.delay()); }
        if (time >= airWaitUntil_) {
            airsend.metadata(now.info.title, now.info.artist, now.info.album, now.info.duration, shownPosition());
            airsend.artwork(airCover_);
            airSentKey_ = key;
            airMetaAt_ = time;
        }
    } else if (time - airMetaAt_ > 15) {
        airsend.metadata(now.info.title, now.info.artist, now.info.album, now.info.duration, shownPosition());
        airMetaAt_ = time;
    }
    airLastTrack_ = track;
    airLastPlaying_ = playing;
    airLastPos_ = pos;
    airLastAt_ = time;
}

void App::airplayTick() {
    airsendTick();
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
        if (srvq_.on) serverLeave();   // (an iPhone plays here now)
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
            snprintf(line, sizeof line, "AirPlay: lost %d, repaired %d; this song measured %.0f s at %.1f LUFS, peak %.2f; level %+.1f dB, the sender's volume %.2f",
                     v.lost, v.repaired, v.measured, v.lufs, v.peak, 20 * std::log10(std::max(lv, 1e-6f)), double(v.gain));
            logLine(line);
        }
#endif
    }
}
#endif

void App::handoffTo(const std::string& peerId, const std::string& peerName) {
    if (tog.stage != Tog::Off || !handoffPeer.empty()) return;
    Message m;
    m.kind = Kind::Handoff;
    if (srvq_.on) {   // the server's queue: its songs as the server names them (the other device plays them from there, or its own)
        if (srvq_.songs.empty() || srvq_.pos < 0) { toast("Play something first: this queue is what moves"); return; }
        m.song = srvq_.pos;
        for (const Subsonic::Song& s : srvq_.songs) m.songs.push_back(serverSongAcross(s));
    } else {
        if (q.order.empty() || q.pos < 0) { toast("Play something first: this queue is what moves"); return; }
        if (queuePlaylist() >= 0) m.list = q.name;   // the playlist it came from (Automix, smart, Mood…): played as one there too
        m.song = q.pos;
        m.songs.reserve(q.order.size());
        for (int o : q.order) m.songs.push_back(togetherSong(q.tracks[size_t(o)]));
    }
    m.position = player.loaded() ? player.position() : 0;
    m.flag = isPlaying();
    togetherSendTo(peerId, m);
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

void App::handoffTake(const Message& m, const std::string& from, const std::string& name, double receivedAt, bool waited) {
    Message answer;
    answer.kind = Kind::Taken;
    std::vector<Held> mine;
    mine.reserve(lib.tracks.size());
    for (int i = 0; i < int(lib.tracks.size()); i++) mine.push_back({ togetherSong(i), i });
    const std::vector<int> tracks = match(m.songs, mine);
    // "Both" (the user, 2026-11-07): your own copy of every song, your own queue; else, signed in to a server, the server's songs (by
    // their id on the same server, or found there by their tags and length), played as its queue; else the songs held here, as before
    const bool allOwn = std::find(tracks.begin(), tracks.end(), -1) == tracks.end();
    const std::string print = serverPrint();
    handoffWait_.on = false;
    if (!allOwn && !print.empty()) {
        const std::vector<Subsonic::Song>* list = serverSongList();
        if (!list && !waited) {   // its song list first (asked now): taken when it is here (togetherTick)
            handoffWait_ = { true, m, from, name, receivedAt, time };
            return;
        }
        if (list) {
            std::unordered_map<std::string, size_t> byId;
            std::unordered_multimap<std::string, size_t> byLoose;
            for (size_t k = 0; k < list->size(); k++) byId.emplace((*list)[k].id, k);
            bool anyLoose = false;
            for (const Song& s : m.songs) anyLoose = anyLoose || (s.server != print && !s.loose.empty());
            if (anyLoose)
                for (size_t k = 0; k < list->size(); k++) {
                    const Subsonic::Song& so = (*list)[k];
                    const std::string key = Identity::looseKey(so.artist, std::string(), so.album, so.title);
                    if (!key.empty()) byLoose.emplace(key, k);
                }
            std::vector<Subsonic::Song> songs;
            int pos = -1, own = 0;
            bool same = false;   // the song playing there is here (else the next one, from its start)
            for (int t : tracks) own += t >= 0;
            for (int i = 0; i < int(m.songs.size()); i++) {
                const Song& s = m.songs[size_t(i)];
                const Subsonic::Song* found = nullptr;
                if (s.server == print && !s.sid.empty()) { if (auto it = byId.find(s.sid); it != byId.end()) found = &(*list)[it->second]; }
                if (!found && !s.loose.empty()) {
                    auto r = byLoose.equal_range(s.loose);
                    for (auto it = r.first; it != r.second && !found; ++it)
                        if (Identity::sameLength((*list)[it->second].duration, s.duration)) found = &(*list)[it->second];
                }
                if (!found) continue;
                if (i == m.song) same = true;
                if (i >= m.song && pos < 0) pos = int(songs.size());
                songs.push_back(*found);
            }
            if (!songs.empty() && int(songs.size()) >= own) {
                if (time >= handoffOldUntil) {
                    handoffOldWasSrv_ = srvq_.on;
                    handoffOldSrv_ = srvq_;
                    handoffOld = q;
                    handoffOldAt = player.loaded() ? player.position() : 0;
                }
                handoffOldUntil = time + 120;
                if (srvq_.on) serverLeave();
                const double at = same ? m.position + (m.flag ? time - receivedAt : 0) : 0;
                srvLoadPaused_ = true;   // loaded at its place, then played (as it was there)
                srvSeek_ = at;
                srvResumeAfter_ = m.flag;
                playServer(songs, pos < 0 ? 0 : pos, "From " + name);
                answer.flag = true;
                togetherSendTo(from, answer);
                const int missing = int(m.songs.size()) - int(songs.size());
                toast("Took over from " + name + (missing ? ", " + std::to_string(missing) + (missing == 1 ? " song is not here" : " songs are not here") : ""));
                return;
            }
        }
    }
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
        togetherSendTo(from, answer);
        toast("None of those songs are here");
        return;
    }
    const bool same = m.song >= 0 && m.song < int(tracks.size()) && tracks[size_t(m.song)] >= 0;
    if (pos < 0) pos = 0;
    t.pos = pos;
    // Your own queue, from before the first of several: a second handoff saved the first one's in its place (the user, 2026-11-02)
    if (time >= handoffOldUntil) {
        handoffOldWasSrv_ = srvq_.on;
        handoffOldSrv_ = srvq_;
        handoffOld = q;
        handoffOldAt = player.loaded() ? player.position() : 0;
    }
    handoffOldUntil = time + 120;
    if (srvq_.on) serverLeave();   // (the server's queue playing here: yours takes over)
    q = std::move(t);
    handoffPlay(q.tracks[size_t(q.order[size_t(q.pos)])], m.flag, same ? m.position + (m.flag ? time - receivedAt : 0) : 0);
    answer.flag = true;
    togetherSendTo(from, answer);
    persistSoon(0.5f);
    toast("Took over from " + name + (missing ? ", " + std::to_string(missing) + (missing == 1 ? " song is not here" : " songs are not here") : ""));
}

void App::handoffUndo() {
    if (time > handoffOldUntil) return;
    handoffOldUntil = 0;
    if (srvq_.on) serverLeave();
    if (handoffOldWasSrv_ && !handoffOldSrv_.songs.empty()) {   // the server's queue it had: back at its place, paused
        handoffOldWasSrv_ = false;
        srvLoadPaused_ = true;
        srvSeek_ = handoffOldAt;
        playServer(handoffOldSrv_.songs, std::max(0, handoffOldSrv_.pos), handoffOldSrv_.name);
        persistSoon(0.5f);
        toast("Your queue is back");
        return;
    }
    q = handoffOld;
    if (q.pos >= 0 && q.pos < int(q.order.size())) handoffPlay(q.tracks[size_t(q.order[size_t(q.pos)])], false, handoffOldAt);
    else player.stop();
    persistSoon(0.5f);
    toast("Your queue is back");
}

/** Every frame. */
void App::togetherTick() {
    if (handoffWait_.on && (serverSongList() || time - handoffWait_.since > 20)) {   // a handoff waiting for the server's song list
        const HandoffWait w = handoffWait_;
        handoffWait_.on = false;
        handoffTake(w.m, w.from, w.name, w.receivedAt, true);   // (no list in time: its own copies only)
    }
#if defined(GF_DEV) && !defined(GF_WINDOWED) && !defined(GF_AIRPLAY)
    if (getenv("GF_TOG_MOCK")) return;   // a drawn session, nothing behind it
#endif
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
    for (const Connect::From& f : connect.receiveTogether()) {
        Message m;
        if (!unpack(f.msg, m)) continue;
        togFrom_ = f.peer;
        // a session is with one device: another inviting, or handing off, is told no; the rest of it is not for this session
        // (the starter: any device it asked in; a listener: the starter only. The starter's peerId is just its first device: checked
        // against it, the third device's yes was dropped, the user, 2026-11-05: "connected but is not playing together")
        if (tog.stage != Tog::Off && (tog.host ? !togMember(f.peer) : f.peer != tog.peerId)) {
            if (m.kind == Kind::Invite || m.kind == Kind::Handoff) {
                Message no;
                no.kind = m.kind == Kind::Invite ? Kind::Answer : Kind::Taken;
                no.flag = false;
                togetherSendTo(f.peer, no);
            }
            continue;
        }
        togetherHandle(m);
    }
    togFrom_.clear();
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
    if (tog.host) {
        if (!togetherMembersTick()) return;
    } else if (!connect.linkedTo(tog.peerId)) {
        if (tog.stage != Tog::On || tog.startAt == 0) { togetherEnd("The link to " + tog.peerName + " ended: playing on here"); return; }
        if (!tog.linkLost) {
            tog.linkLost = true;
            tog.lostAt = time;
            toast("The link to " + tog.peerName + " dropped: playing on, waiting for it");
            logLine("Together: the link to " + tog.peerName + " (the starter) dropped");
        } else if (time - tog.lostAt > kLostSeconds) { togetherEnd("The link to " + tog.peerName + " ended: playing on here"); return; }
    }
    const Connect::Clock c = connect.clock();
    if (tog.host) player.syncClock(0, 0, 0);
    else if (c.samples > 0) player.syncClock(c.at, c.offset, c.slope);
    if (!tog.host && tog.resumeHeld && c.samples >= kClockEnough) togetherApplyResume(tog.heldResume);   // (the clock is set just above)
    player.syncDelay(store.togetherDelay[std::clamp(store.togetherOutput, 0, 2)] / 1000.0);
    if (tog.measure.stage == 1) {
        micm::Result mr;
        if (tog.measure.listening && measurer.take(mr)) {   // this device listened
            logLine("Together: listened with " + (mr.mic.empty() ? std::string("no microphone") : mr.mic) + (mr.error.empty() ? "" : ": " + mr.error));
            for (size_t k = 0; k < mr.why.size(); k++)
                if (k < mr.heard.size() && !mr.heard[k]) logLine("Together: slot " + std::to_string(k) + " not heard: " + (mr.why[k].empty() ? std::string("no recording there") : mr.why[k]));
            if (tog.host) {
                togetherMeasured(mr);
            } else {
                Message d;
                d.kind = Kind::Delay;
                if (!mr.error.empty() && std::find(mr.heard.begin(), mr.heard.end(), true) == mr.heard.end()) d.list = "!" + mr.error;
                else
                    for (size_t k = 0; k < mr.heard.size(); k++) {
                        if (!mr.heard[k]) continue;
                        char t[48];
                        snprintf(t, sizeof t, "%d %.2f\n", int(k), mr.lateMs[k]);
                        d.list += t;
                    }
                togetherSend(d);
                logLine("Together: heard, sent to the starter: " + (d.list.empty() ? std::string("nothing") : d.list.substr(0, 120)));
                tog.measure = Tog::Measure{};
            }
        } else if (!tog.host && !tog.measure.listening && player.syncNow() > tog.measure.endAt) {
            tog.measure = Tog::Measure{};   // its chirps are over
        } else if (tog.host && player.syncNow() > tog.measure.giveUpAt) {
            toast("The delay could not be measured: the device listening sent nothing. Timing is as it was");
            togetherMeasureEnd();
        }
    }
    togetherPrintsTick();
    if (tog.host && tog.stage == Tog::On && !tog.linkLost && tog.whoSent != int(store.order)) togetherSendWho();
    if (tog.queueWait && (serverSongList() || time - tog.queueWaitSince > 20)) {   // the queue held for the server's song list
        const Message held = tog.heldQueue;
        togetherTakeQueue(held, true);
    }
    if (tog.stage != Tog::On || tog.song < 0) return;
    if (tog.fetchSong >= 0) {   // its server file first, then opened as it would have been
        if (tog.fetchSong != tog.song) tog.fetchSong = -1;
        else {
            const std::string path = togPath(tog.fetchSong);
            if (path.empty()) return;   // (Ready is said only once it is open)
            tog.fetchSong = -1;
            player.prepareTimed(path, tog.fetchPos, tog.fetchRejoin);
            if (!tog.host && !tog.fetchRejoin) togetherAskPrint(tog.song);
            return;
        }
    }

    if (tog.rejoining) {
        if (!player.preparing()) {
            tog.rejoining = false;
            if (tog.rejoinNew) {
                tog.prepared = tog.saidReady = true;
                togetherLoadNow();
                if (tog.startAt == 0) tog.startAt = tog.rejoinAt;
            } else {
                player.load(togPath(tog.song), true);
            }
            if (!tog.paused) player.syncStart(tog.rejoinAt, tog.position);
        }
        return;
    }
    if (player.takeLost() && tog.startAt > 0 && !tog.paused && !tog.natural && togHere(tog.song)) {
        const double moment = player.syncNow() + kRejoinAhead, position = player.syncPositionAt(moment);
        const double dur = now.info.duration > 0 ? now.info.duration : togDuration(tog.song);
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
    if (tog.host) {   // every device in is ready; one whose link dropped, or that is just joining, hears where things stand after
        bool all = true;
        for (const Tog::Member& mb : tog.members)
            if (mb.state == 2 && !mb.lost && !mb.resumeDue && !mb.awaiting && mb.readySerial != tog.serial) all = false;
        tog.theyReady = all;
    }
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
            const double dur = now.info.duration > 0 ? now.info.duration : togDuration(tog.playingSong);
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
    // the track playing plays out; if it runs past the moment the next one starts, it stops for it
    if (tog.natural && tog.startAt > 0 && !tog.loaded && player.syncNow() > tog.startAt - 0.08) togetherFinished();
    if (tog.joinArmed && !tog.paused) togetherJoinStep();
    if (tog.host && !tog.natural && !tog.nextSent && !tog.planNone && !tog.paused && tog.startAt > 0 && player.syncRunning()) togetherPlanJoin();
    // the starter looks ahead to the next song
    const bool srvQueue = srvq_.on && !srvq_.session;   // the starter's queue is the server's (no Repeat or Shuffle there)
    if (tog.host && !tog.natural && !tog.nextSent && !tog.paused && tog.startAt > 0 && player.syncRunning() && (now.track >= 0 || srvQueue)) {
        const double dur = now.info.duration > 0 ? now.info.duration : togDuration(tog.song);
        if (dur > 0 && dur - player.position() < kNextBefore) {
            tog.nextSent = true;
            int p = togIndexNow() + 1;
            if (!srvQueue && store.order == Order::RepeatOne) p = q.pos;
            if (p < (srvQueue ? int(srvq_.songs.size()) : int(q.order.size()))) {
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
