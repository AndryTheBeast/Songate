/*
 * Songate · view_connect.cpp
 * Settings › Devices' side of the app (the worker is connect.cpp): the prompt before any socket opens, typing a code, and what
 * the worker has to say. The page itself is in view_settings.cpp.
 */
#include "words.hpp"
#include <algorithm>
#include <set>
#ifndef _WIN32
#include <unistd.h>   // rmdir
#endif

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"

void App::allowConnections() {
    confirm(ICON_LINK, "Devices", "Look for other devices?", "On this network only",
            "Allow Songate to look for other devices?",
            "While it is on and Songate is open, Songate says hello on this network, with " GF_YOUR_CONSOLE "'s name and a random number. "
            "Nothing of your library leaves it, and no other device can read or change anything until you pair it with a code."
#ifdef GF_WINDOWED
            " Windows will then ask whether Songate may use the network. Allow makes it easiest. If you cancel that, Songate can still "
            "call the other device, as long as that one can be reached."
#endif
            ,
            "Allow", [this] {
                store.connections = true;
                persistSoon(0.3f);
                toast("Devices on");
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
}

/** Opening takes all the card and the cores can give: tags, covers, the library. A sync waits its turn, it is in no hurry.
 *  A music folder with nothing in it is a library like any other (its last song was deleted, or the device is new); one that is
 *  not there at all, a card out or a drive unplugged, says nothing about what it holds. */
bool App::syncSettled() const {
    return time > 5 && !lib.roots.empty() && meta.taggedCount() >= int(lib.tracks.size());
}

void App::allowDeletes() {
    confirm(ICON_LINK, "Devices", "Follow deletions", "From now on, on this device", "Delete songs here when they are deleted elsewhere?",
            "From now on, a song deleted inside Songate on a paired device is deleted from " GF_THE_CARD " here too, with every copy of it. Songs deleted before now stay. "
            "A song that merely went missing there, such as a card taken out, is never deleted here.",
            "Follow deletions", [this] {
                store.syncDeletes = true;
                store.syncDeleteFrom = ~uint64_t(0);
                persistSoon(0.3f);
                toast("This device now follows deletions");
            });
    sheet.askWarn = "A DELETED SONG CANNOT BE BROUGHT BACK";
}

/** Told only for what is really deleted, inside Songate: a file that is simply missing proves nothing. */
void App::syncNoteDeleted(const std::string& path) {
    if (!store.connections) return;
    Identity::Known k;
    if (identity.find(path, k) && !k.loose.empty()) syncer.noteGone(k.loose);
    syncSent = 0;   // a change like any other: tells the others within seconds
    syncChangedAt = time;
}

void App::syncNow(const std::string& peerId, bool answering) {
    if (syncWaiting) return;
    if (lib.roots.empty()) { toast("The music folder isn't here: nothing can sync"); return; }
    if (meta.taggedCount() < int(lib.tracks.size())) { toast("Songate is still reading your library: sync in a moment"); return; }
    const Connect::View v = connect.view();
    syncPeer = peerId;
    syncPeerName.clear();
    for (const Connect::Paired& p : v.paired) if (p.id == peerId) syncPeerName = p.name;
    syncAnswering = answering;
    syncWaiting = true;
    syncWorstMs = 0;
    syncBuild = SyncBuild{};
    syncBuild.on = true;
    syncBuild.epoch = libEpoch;
    // the identities are read again only if the library or the edits changed: the rest is known, and the card left alone
    syncBuild.wantIdentity = libEpoch != syncIdentityEpoch || store.edits.size() != syncIdentityEdits;
    syncBuild.job.now.songs.reserve(lib.tracks.size());
}

/** The library is large and this is the main thread: a little each frame, never more than a millisecond's worth. */
void App::syncBuildStep() {
    const Uint64 t0 = SDL_GetPerformanceCounter();
    const Uint64 limit = SDL_GetPerformanceFrequency() / 1000;
    SyncBuild& b = syncBuild;
    if (b.epoch != libEpoch) {   // rescanned meanwhile: from the top
        const bool want = true;
        b = SyncBuild{};
        b.on = true;
        b.epoch = libEpoch;
        b.wantIdentity = want;
        b.job.now.songs.reserve(lib.tracks.size());
    }
    while (b.next < lib.tracks.size() && SDL_GetPerformanceCounter() - t0 < limit) {
        for (int n = 0; n < 16 && b.next < lib.tracks.size(); n++, b.next++) {
            const Track& t = lib.tracks[b.next];
            syncdata::Song s;
            s.path = t.path;
            s.duration = t.duration;
            b.job.now.songs.push_back(std::move(s));
            const PlayStat m = stats.mine(t.path);
            if (m.plays || m.last || m.added) b.job.now.plays[t.path] = { m.plays, m.last, m.added };
            if (const int o = stats.others(t.path)) b.job.now.others[t.path] = o;
            if (b.wantIdentity) b.wants.push_back(identityWant(t));
        }
    }
    if (b.next >= lib.tracks.size()) {
        b.on = false;
        if (b.wantIdentity) {
            identity.want(std::move(b.wants));
            syncIdentityEpoch = libEpoch;
            syncIdentityEdits = store.edits.size();
            syncWatchIdentity = true;
        }
    }
    syncWorstMs = std::max(syncWorstMs, double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency()));
}

/** A number that changes when Liked, a playlist or an edit does (plays: stats.gen()). Once a second, only while linked. */
/** A folder playlist's folder within the music folder ("A/B"), or empty when it is not inside it. */
std::string App::folderInMusic(const std::string& path) const {
    std::string root = store.musicRoot, p = path;
    std::replace(root.begin(), root.end(), '\\', '/');
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!root.empty() && root.back() == '/') root.pop_back();
    if (root.empty() || p.size() <= root.size() + 1 || p.compare(0, root.size(), root) != 0 || p[root.size()] != '/') return {};
    std::string rel = p.substr(root.size() + 1);
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    return rel;
}

uint64_t App::syncStamp() const {
    uint64_t h = 14695981039346656037ull;
    auto add = [&](const std::string& s) { for (unsigned char c : s) h = (h ^ c) * 1099511628211ull; h = (h ^ 0xFF) * 1099511628211ull; };
    for (const auto& p : store.liked) add(p);
    for (const auto& p : store.playlists) {
        if (p.smart()) continue;
        add(p.name);
        add(p.path);   // a folder playlist's folder (empty for your own)
        add(p.icon);
        add(p.pin);
        h = (h ^ (p.automix ? 1u : 0u) ^ (p.manual ? 2u : 0u)) * 1099511628211ull;
        for (const auto& t : p.tracks) add(t);
    }
    for (const auto& e : store.edits) {
        uint64_t one = 0;   // an unordered map: added up, so its order does not matter
        uint64_t keep = h;
        h = 14695981039346656037ull;
        add(e.first);
        for (const auto& f : e.second) { add(f.first); add(f.second); }
        one = h;
        h = keep + one;
    }
    return h;
}

void App::syncApply(const syncdata::Changes& c) {
    if (c.empty()) return;   // nothing to save: the card is left alone
    for (const std::string& p : c.likedAdd) if (std::find(store.liked.begin(), store.liked.end(), p) == store.liked.end()) store.liked.push_back(p);
    for (const std::string& p : c.likedRemove) std::erase(store.liked, p);
    std::vector<int> retag;
    for (const auto& e : c.edits) {
        auto ix = pathIndex.find(e.path);
        if (e.value.empty()) {
            auto it = store.edits.find(e.path);
            if (it != store.edits.end()) {
                it->second.erase(e.field);
                if (it->second.empty()) store.edits.erase(it);
            }
            if (ix != pathIndex.end()) retag.push_back(ix->second);   // the file's own tag comes back as it is read again
        } else {
            store.edits[e.path][e.field] = e.value;
            if (ix != pathIndex.end()) applyEdits(lib.tracks[size_t(ix->second)]);
        }
    }
    auto kindOf = [](FolderPlaylist& p, const syncdata::List& l) {
        p.automix = l.automix;
        p.manual = l.manual;
        p.pin = l.pin;
        p.icon = l.icon;
    };
    // through the same doors as by hand: pages open on a playlist, and the history, are by index
    for (const std::string& n : c.listsDelete) {
        for (int i = 0; i < int(store.playlists.size()); i++)
            if (syncedList(store.playlists[size_t(i)]) && store.playlists[size_t(i)].name == n) { erasePlaylist(i); break; }
    }
    for (const auto& l : c.listsNew) {
        // a folder playlist: the same folder in this device's music folder (empty until it is there)
        store.playlists.push_back({ l.name, l.folder.empty() ? std::string() : plat::join(store.musicRoot, l.folder), l.folder.empty() ? l.paths : std::vector<std::string>{} });
        kindOf(store.playlists.back(), l);
    }
    for (const auto& r : c.listsRename) {
        for (auto& p : store.playlists) if (syncedList(p) && p.name == r.from) p.name = r.to;
        renameJoins(r.from, r.to);
    }
    for (const auto& l : c.listsSet) {
        for (auto& p : store.playlists) {
            if (!syncedList(p) || p.name != l.name) continue;
            if (!l.folder.empty()) p.path = plat::join(store.musicRoot, l.folder);   // (its songs come from the folder)
            else if (p.own()) p.tracks = l.paths;
            kindOf(p, l);
        }
    }
    if (!c.listOrder.empty()) {
        // your own and folder playlists take these places among themselves; smart ones stay where they are
        for (size_t k = 0; k < c.listOrder.size(); k++) {
            std::vector<int> slots;
            int from = -1;
            for (int i = 0; i < int(store.playlists.size()); i++) {
                if (!syncedList(store.playlists[size_t(i)])) continue;
                slots.push_back(i);
                if (store.playlists[size_t(i)].name == c.listOrder[k]) from = i;
            }
            if (from >= 0 && k < slots.size() && from != slots[k]) movePlaylist(from, slots[k]);
        }
    }
    for (const auto& p : c.plays) stats.adopt(p.first, p.second.plays, p.second.last, p.second.added);
    if (!c.edits.empty()) store.saveEdits();
    store.save();
    saveStats();
    for (int t : retag) meta.retag(t);
    if (!c.edits.empty() && c.filesDelete.empty()) rebuildIndex();   // only tags change the index: a playlist by hand does not rebuild it either (54 ms on the console)
}

/** The library made again on a worker, for syncDeleteStep to take over in one frame. walk: the card decides what is in it (songs
 *  arrived), and those new tracks get their tags as any new file does; otherwise syncNextFiles does. */
void App::rebuildBehind(bool walk) {
    syncRebuilding = true;
    syncRebuilt = false;
    syncRebuildEpoch = libEpoch;
    syncRebuild = std::thread([this, walk, roots = lib.roots] {
        cores::moveHere(cores::Files);
        if (walk) {
            syncNextFiles = Library::listFiles(roots);
            std::sort(syncNextFiles.begin(), syncNextFiles.end());
        }
        syncNext = Library{};
        syncNext.build(roots, syncNextFiles);
        syncRebuilt = true;
    });
}

/** The songs a sync deletes, one a frame; then what is kept about them, saved; then the library without them, built on a worker
 *  and taken over in one frame. What went is known, so the card is not walked again, and the tags are in memory. */
void App::syncDeleteStep() {
    if (syncRebuilding) {
        if (!syncRebuilt) return;
        syncRebuild.join();
        syncRebuilding = false;
        if (syncRebuildEpoch == libEpoch) rescan(&syncNextFiles, &syncNext);
        else rescan();   // something else rescanned meanwhile: the list is old
        syncNextFiles.clear();
        if (!syncDeleteOn) return;   // songs that arrived: no sync waits for this
        syncDeleteOn = false;
        syncSent = syncLast = syncStamp();
        syncPlaysSent = stats.gen();
        syncer.applied();
        return;
    }
    if (syncDeleting.empty()) {
        const std::set<std::string> went(syncDeleted.begin(), syncDeleted.end());
        syncDeleted.clear();
        syncNextFiles = libFiles_;
        std::erase_if(syncNextFiles, [&](const std::string& f) { return went.count(f) != 0; });
        store.saveEdits();
        store.save();
        saveStats();
        rebuildBehind(false);
        return;
    }
    const std::string p = std::move(syncDeleting.back());
    syncDeleting.pop_back();
    auto ix = pathIndex.find(p);
    if (ix != pathIndex.end() && deleteSong(ix->second)) {
        syncDeleted.push_back(p);
        // the libraries match: a folder that held only this song goes with it (rmdir refuses one that holds anything else)
        const std::string root = store.musicRoot + "/";
        for (std::string d = plat::parentDir(p); d.size() > root.size() && d.compare(0, root.size(), root) == 0 && rmdir(d.c_str()) == 0; d = plat::parentDir(d)) {}
    }
}

void App::connectTick() {
    connect.want(store.connections);
    togetherTick();
#ifdef GF_AIRPLAY
    airplayTick();
#endif
#ifdef GF_DEV
    // while a sync runs: the longest real frame, and update's slowest step, in the log
    {
        const Uint64 at = SDL_GetPerformanceCounter();
        const double gap = syncFrameAt ? double(at - syncFrameAt) * 1000.0 / double(SDL_GetPerformanceFrequency()) : 0;
        syncFrameAt = at;
        const Syncer::State st = syncer.view().state;
        if (copier.view().state == Copier::State::Copying && gap > copyGapMs) copyGapMs = gap;
        // the first 70 seconds, one line a second: how the frames went, and what else was at work
        if (time < 70) {
            if (gap > 33) tlSlow++;
            if (gap > tlWorst) { tlWorst = gap; tlPresent = profPrev_[7]; tlUpdate = profPrev_[0]; }
            if (int(time) != tlSec) {
                if (tlSlow > 0) {
                    const Identity::Progress ip = identity.progress();
                    int made = 0, left = 0;
                    thumbs.progress(made, left);
                    char line[200];
                    snprintf(line, sizeof line, "Second %d: %d slow, worst %.0f (present %.0f, update %.0f); sync %d, identity %d/%d hashed %d, thumbs left %d, levels %d, gains %d, tags %d/%d, playing %d, opening %d",
                             int(time), tlSlow, tlWorst, tlPresent, tlUpdate, int(syncer.view().state), ip.done, ip.total, ip.hashed, left, int(levels.busy()), int(gainsBuilding), meta.taggedCount(), int(lib.tracks.size()), int(isPlaying()), int(!openingDone_));
                    logLine(line);
                }
                tlSec = int(time);
                tlSlow = 0;
                tlWorst = tlPresent = tlUpdate = 0;
            }
        }
        const bool syncing = syncWaiting || syncBuild.on || st == Syncer::State::Reading || st == Syncer::State::Trading || st == Syncer::State::Applying || st == Syncer::State::Sharing;
        if (!syncing && gap > syncIdleGap && syncSettled()) syncIdleGap = gap;
        if (!syncing && syncAfterUntil > 0) {
            if (gap > 33) syncAfterSlow++;
            if (gap > syncAfterGap) {
                syncAfterGap = gap;
                syncAfterAt = time - (syncAfterUntil - 20);
                std::copy(std::begin(profPrev_), std::end(profPrev_), std::begin(syncAfterProf));
                syncAfterStep = updPrevSlow_ ? updPrevSlow_ : "";
            }
            if (time >= syncAfterUntil) {
                char line[220];
                snprintf(line, sizeof line, "Connections: 20 s after the sync: %d frames over 33 ms; longest %.0f ms, %.1f s after: update %.1f (%s), drawing %.1f %.1f %.1f %.1f %.1f %.1f, present %.1f",
                         syncAfterSlow, syncAfterGap, syncAfterAt, syncAfterProf[0], syncAfterStep.c_str(), syncAfterProf[1], syncAfterProf[2], syncAfterProf[3], syncAfterProf[4], syncAfterProf[5], syncAfterProf[6], syncAfterProf[7]);
                logLine(line);
                syncAfterUntil = 0;
            }
        }
        if (gap > 0 && syncing) {
            const int ph = syncWaiting || syncBuild.on ? 0 : int(st);
            if (gap > syncGapMs) {
                syncGapMs = gap;
                std::copy(std::begin(profPrev_), std::end(profPrev_), std::begin(syncWorstProf));
                syncWorstStep = updPrevSlow_ ? updPrevSlow_ : "";
                syncWorstPhase = ph;
            }
            if (gap > syncPhaseGap[ph]) { syncPhaseGap[ph] = gap; syncPhaseUpdate[ph] = updPrevTotalMs_; }
            if (updPrevMs_ > syncStepMs) { syncStepMs = updPrevMs_; syncStep = updPrevSlow_ ? updPrevSlow_ : ""; }
        }
    }
#endif
    if (syncBuild.on) syncBuildStep();
    else if (syncWaiting && (!identity.busy() || identity.ready(lib.tracks.size()))) {   // what is known is enough: the check goes on behind it
        const Uint64 t0 = SDL_GetPerformanceCounter();
        syncWaiting = false;
        Syncer::Job job = std::move(syncBuild.job);
        job.kinds = store.syncKinds;
        if (store.syncDeletes) job.kinds |= syncdata::KindDeletes;
        job.deleteFrom = store.syncDeleteFrom;
        job.share = syncAnswering ? store.syncResults && time > 30 : syncShareDue();   // asked, it agrees once it has settled
        if (job.share && !syncAnswering) syncShareTriedAt = time;
        job.peerId = syncPeer;
        job.peerName = syncPeerName;
        job.answering = syncAnswering;
        const Connect::View v = connect.view();
        job.myId = v.id;
        job.myName = v.name;
        job.now.liked = store.liked;
        job.now.edits = store.edits;
        syncSent = syncLast = syncStamp();
        syncPlaysSent = stats.gen();
        syncAt = time;
        for (const auto& p : store.playlists) {
            if (p.smart()) continue;
            syncdata::List l;
            if (p.folder()) {
                l.folder = folderInMusic(p.path);
                if (l.folder.empty()) continue;   // a folder outside the music folder: no other device could find it
            }
            l.name = p.name;
            l.paths = p.tracks;
            l.automix = p.automix;
            l.manual = p.manual;
            l.pin = p.pin;
            l.icon = p.icon;
            job.now.lists.push_back(std::move(l));
        }
        syncShown = false;
        if (!syncer.begin(std::move(job))) toast("A sync is already under way");
        syncWorstMs = std::max(syncWorstMs, double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency()));
    }
    if (syncWatchIdentity && !identity.busy()) {
        syncWatchIdentity = false;
        if (identity.progress().hashed > 0) syncSent = 0;   // files changed: another sync, once this one is over
    }
    syncdata::Changes changes;
    if (syncer.takeChanges(changes)) {
        const Uint64 t0 = SDL_GetPerformanceCounter();
        syncApply(changes);
        syncSent = syncLast = syncStamp();   // what came in is no change of ours to send back
        syncPlaysSent = stats.gen();
        syncDeleting = std::move(changes.filesDelete);
        syncDeleteOn = !syncDeleting.empty();
        if (!syncDeleteOn) syncer.applied();
        syncWorstMs = std::max(syncWorstMs, double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency()));
    } else if (syncDeleteOn || syncRebuilding) {
        const Uint64 t0 = SDL_GetPerformanceCounter();
        syncDeleteStep();
        syncWorstMs = std::max(syncWorstMs, double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency()));
    }
    if (time < connectAt) return;
    connectAt = time + 0.5;
    for (const std::string& l : connect.takeLog()) logLine(l);
    for (const std::string& n : connect.takeNews()) toast(n);
    syncBusy = false;
    if (!store.connections) return;
    const Syncer::View sv = syncer.view();
    // The arrows by the name: reading, trading, applying, sharing, and the files a sync deletes or copies (not while it asks)
    syncBusy = syncBuild.on || syncDeleteOn || (sv.state != Syncer::State::Idle && sv.state != Syncer::State::Asking && sv.state != Syncer::State::Done &&
                                                sv.state != Syncer::State::Failed);
    if (sv.state == Syncer::State::Asking && !syncShown) {
        syncShown = true;
        confirm(ICON_LINK, "Devices", "The first sync with " + sv.peer, sv.summary, "Sync with " + sv.peer + "?",
                "This is what will change here: " + sv.summary + ". Nothing is deleted from your music, and your settings are backed up first. "
                "After this, syncing with this device does not ask again.",
                "Sync", [this] { syncer.accept(); });
        sheet.askSafe = true;
        sheet.askYesIcon = ICON_CHECK;
    } else if (sv.state == Syncer::State::Asking && sheet.page != Sheet::Confirm) {
        syncer.decline();   // Cancel on the prompt, or anything that closed it
    } else if (sv.state == Syncer::State::Done && !syncShown) {
        syncShown = true;
        if (sv.shared) syncSharedAt = time;   // only if it really happened
        if (store.syncDeleteFrom == ~uint64_t(0) && sv.deleteFrom != ~uint64_t(0)) { store.syncDeleteFrom = sv.deleteFrom; persistSoon(0.3f); }
        const bool nothing = sv.summary == "Nothing changes here" && !sv.measures && !sv.covers;
        std::string said = sv.summary;
        if (sv.measures) said = (sv.summary == "Nothing changes here" ? std::string() : said + ", ") + std::to_string(sv.measures) + (sv.measures == 1 ? " track's measurements" : " tracks' measurements");
        if (sv.covers) said = (said == "Nothing changes here" ? std::string() : said + ", ") + std::to_string(sv.covers) + (sv.covers == 1 ? " cover" : " covers");
        if (!nothing || !store.syncAuto) toast(nothing ? "Up to date with " + sv.peer : "Synced with " + sv.peer + ": " + said);
        char worst[160];
#ifdef GF_DEV
        snprintf(worst, sizeof worst, " (held %.1f ms; longest frame %.1f ms; slowest step %.1f ms, %s)", syncWorstMs, syncGapMs, syncStepMs, syncStep.c_str());
        {
            static const char* const names[10] = { "before", "reading", "trading", "asking", "backing", "applying", "sharing", "done", "failed", "" };
            char one[64];
            snprintf(one, sizeof one, "Connections: longest frame with no sync: %.0f ms;", syncIdleGap);
            std::string by = one;
            syncIdleGap = 0;
            for (int i = 0; i < 10; i++) {
                if (syncPhaseGap[i] <= 0) continue;
                snprintf(one, sizeof one, " %s %.0f ms (update %.0f)", names[i], syncPhaseGap[i], syncPhaseUpdate[i]);
                by += one;
                syncPhaseGap[i] = syncPhaseUpdate[i] = 0;
            }
            logLine(by);
            char split[200];
            double sum = 0;
            for (double v : syncWorstProf) sum += v;
            snprintf(split, sizeof split, "Connections: the longest frame, %.0f ms in phase %d: update %.1f (%s), drawing %.1f %.1f %.1f %.1f %.1f %.1f, present %.1f; unaccounted %.1f",
                     syncGapMs, syncWorstPhase, syncWorstProf[0], syncWorstStep.c_str(), syncWorstProf[1], syncWorstProf[2], syncWorstProf[3], syncWorstProf[4], syncWorstProf[5], syncWorstProf[6],
                     syncWorstProf[7], syncGapMs - sum);
            logLine(split);
        }
        syncGapMs = syncStepMs = 0;
        syncAfterUntil = time + 20;
        syncAfterGap = syncAfterAt = 0;
        syncAfterSlow = 0;
#else
        snprintf(worst, sizeof worst, " (the longest frame it held: %.1f ms)", syncWorstMs);
#endif
        logLine("Connections: synced: " + said + worst);
    } else if (sv.state == Syncer::State::Failed && !syncShown) {
        syncShown = true;
        if (!store.syncAuto) toast(sv.error);
        logLine("Connections: " + sv.error);
        syncSent = 0;   // what was not sent is still to send
    }
    // the other device began one, or (automatically) a device has just linked
    const Connect::View v = connect.view();
    std::string linked;
    for (const Connect::Paired& p : v.paired) if (p.linked) linked = p.id;
    copier.set(store.syncFiles && !store.musicRoot.empty() && !plat::appletMode(), isPlaying());
    {
        std::string to, toName;
        for (const Connect::Paired& p : v.paired) if (p.linked) { to = p.id; toName = p.name; }
        copier.place(store.musicRoot, to, toName);
    }
    const Copier::View cv = copier.view();
    if (cv.state == Copier::State::Copying) syncBusy = true;
#ifdef GF_DEV
    if (cv.state != copyWas) {
        char line[160];
        snprintf(line, sizeof line, "Connections: copier now %d: %d offered, %d to copy, free %lld MB", int(cv.state), cv.offered, cv.total, (long long)(cv.free / 1048576));
        logLine(line);
    }
#endif
    if (cv.state != copyWas) {
        char gb[64];
        snprintf(gb, sizeof gb, "%.1f GB", double(cv.totalBytes) / (1024.0 * 1024.0 * 1024.0));
        if (cv.state == Copier::State::Asking) {
            confirm(ICON_LINK, "Devices", "Copy music from " + cv.peer, std::to_string(cv.total) + (cv.total == 1 ? " song, " : " songs, ") + gb, "Copy these songs here?",
                    "Each goes into the same folder of your music folder as it has there, so the two libraries match. Nothing is replaced. Music playing slows the copy. After this, new songs from this device arrive by themselves.",
                    "Copy", [this] { copier.accept(); });
            sheet.askSafe = true;
            sheet.askYesIcon = ICON_CHECK;
        } else if (cv.state == Copier::State::Failed) {
            toast(cv.error);
            logLine("Connections: " + cv.error);
        } else if (cv.state == Copier::State::Copying) {
            copyStartedAt = time;
            copyGapMs = 0;
        } else if (cv.state == Copier::State::Done) {
            {
                char rate[120];
                const double took = std::max(0.1, time - copyStartedAt);
                snprintf(rate, sizeof rate, "Connections: %.0f MB in %.0f s, %.1f MB a second; longest frame %.0f ms", double(cv.bytes) / 1048576.0, took, double(cv.bytes) / 1048576.0 / took, copyGapMs);
                logLine(rate);
            }
            toast(std::to_string(cv.done) + (cv.done == 1 ? " song copied from " : " songs copied from ") + cv.peer + (cv.failed ? " (" + std::to_string(cv.failed) + " failed its check)" : ""));
            logLine("Connections: copied " + std::to_string(cv.done) + " songs, " + std::to_string(cv.failed) + " failed");
        }
        copyWas = cv.state;
    } else if (cv.state == Copier::State::Asking && sheet.page != Sheet::Confirm) {
        copier.decline();
    }
    if (cv.state != Copier::State::Copying && !syncDeleteOn && !syncRebuilding && copier.takeArrived()) rebuildBehind(true);   // the new songs join the library when the copy has ended
    const bool busy = syncWaiting || syncBuild.on || sv.state == Syncer::State::Reading || sv.state == Syncer::State::Trading || sv.state == Syncer::State::Asking ||
                      sv.state == Syncer::State::Backing || sv.state == Syncer::State::Applying || sv.state == Syncer::State::Sharing;
    if (syncer.asked()) syncAskedLater = true;
    if (syncAskedLater && !linked.empty() && syncSettled() && !busy) {
        syncAskedLater = false;
        syncNow(linked, true);
    } else if (store.syncAuto && !linked.empty() && !busy && syncSettled()) {
        if (linked != syncLinked || (syncShareDue() && time - syncAt >= 30)) {
            syncNow(linked, false);
        } else if (time - syncStampAt >= 1) {
            // a change: a few seconds' calm first, so that a run of them is one sync
            syncStampAt = time;
            const uint64_t s = syncStamp();
            if (s != syncLast) { syncLast = s; syncChangedAt = time; }
            const double gap = 15;
            if (s != syncSent && time - syncChangedAt >= 4 && time - syncAt >= gap) syncNow(linked, false);
            else if (stats.gen() != syncPlaysSent && time - syncAt >= 180) syncNow(linked, false);   // plays can wait: there is one every few minutes
        }
    }
    if (syncSettled() || linked.empty()) syncLinked = linked;   // a link made while starting up still counts as new
}

void App::connectEnterCode(const std::string& peerId) {
    std::string t;
    if (!keyboard("The 6-digit code shown on the other device", "", t)) return;
    std::string digits;
    for (char c : t) if (c >= '0' && c <= '9') digits += c;
    if (digits.size() != 6) { toast("The code has 6 digits"); return; }
    connect.enterCode(peerId, digits);
}

void App::connectAddress() {
    std::string t;
    if (!keyboard("The other device's address, such as 192.168.1.70", store.connectAim, t)) return;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    store.connectAim = t;
    connect.aim(t);
    persistSoon(0.3f);
    toast(t.empty() ? "Address cleared" : "Saying hello to " + t + " too");
}
