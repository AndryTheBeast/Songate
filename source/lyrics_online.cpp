/*
 * Songate · lyrics_online.cpp
 * Lyrics found online (BETA 3; lyricsnet.hpp for LRCLIB). A song that plays with no lyrics, Settings → Lyrics → Online on, is
 * looked up once a run. What comes back either plays at once, "found online, not saved", with Keep and Not these on the bar
 * (lyricsOnlineWay 0), or waits on a card with its first lines, Save and Not these (1): the user, 2026-11-04, "Keep both A and B
 * and make it toggable by the user". Kept: an .lrc beside the song, or in Songate's own folder (lyricsSaveTo; the folder too when
 * the music's cannot be written). Not these: LRCLIB's other records for the song, its length first.
 */
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "icons.inc"

void App::lyrOnlineReset() {
    const std::string path = now.track >= 0 ? lib.tracks[now.track].path : std::string();
    if (path != lyrOn.path) lyrEmbedAt_ = 0;   // another song: one waiting to have its lyrics written inside may be free now
    if (std::find(lyrTakingOut_.begin(), lyrTakingOut_.end(), path) != lyrTakingOut_.end() && !now.lyricsFromFile) {
        now.lyrics.clear();   // (still in the file until the worker takes them out)
        now.synced = false;
    }
    if (path == lyrOn.path && lyrOn.state == LyrOnline::Saved) return;   // (its lyrics read again after a save)
    if (path != lyrOn.path || !now.lyrics.empty()) lyrOn = LyrOnline{};
    else if (lyrOn.state == LyrOnline::Failed) lyrOn.state = LyrOnline::None;   // not reached the last time: asked again now it plays again, as its card says
}

void App::lyrMemoSet(const std::string& path, const lyrnet::Found* found, const std::string& why) {
    LyrMemo* m = nullptr;
    for (auto& e : lyrMemo_) if (e.path == path) m = &e;
    if (!m) { lyrMemo_.push_back({ path, false, {}, {} }); m = &lyrMemo_.back(); }
    m->have = found != nullptr;
    if (found) m->found = *found;
    m->why = found ? std::string() : why;
}

void App::lyrRemember() {
    const bool have = lyrOn.state == LyrOnline::Found || lyrOn.state == LyrOnline::Saving;
    lyrMemoSet(lyrOn.path, have ? &lyrOn.found : nullptr, lyrOn.why);
}

bool App::lyrWrote(const char* kind, const std::string& path) const {
    return std::find(lyrWritten_.begin(), lyrWritten_.end(), std::string(kind) + "\t" + path) != lyrWritten_.end();
}

void App::lyrOnlineShow(const lyrnet::Found& f) {
    parseLyrics(f.text());
    now.lyricsSource = "found online, not saved";
    now.lyricsFromFile = false;
    now.lyricsInSongate = false;
    lyrOn.shown = true;
}

void App::lyrOnlineTick() {
    // An answer: only the one for the song playing, to its latest question, counts
    if (!lyrWrittenAsked_) {   // what Songate wrote, once (for Remove)
        lyrWrittenAsked_ = true;
        lyrnet::Query w;
        w.kind = lyrnet::Ask::Written;
        lyrNet.ask(w);
    }
    // The songs waiting to have their lyrics written inside: one at a time, never the one playing or the next
    if (!lyrEmbedAsked_ && time >= lyrEmbedAt_) {
        lyrnet::Query e;
        e.kind = lyrnet::Ask::EmbedRun;
        if (now.track >= 0) e.busy.push_back(lib.tracks[now.track].path);
        for (int t : upNext(1)) e.busy.push_back(lib.tracks[t].path);
        lyrEmbedAsked_ = true;
        lyrNet.ask(e);
    }
    for (lyrnet::Answer a; lyrNet.take(a);) {
        if (!a.path.empty() && a.reached && a.kind != lyrnet::Ask::Lyrics && a.kind != lyrnet::Ask::List && a.kind != lyrnet::Ask::Written)
            lyrIndex.refresh(a.path);   // its lyrics changed: search by lyrics reads them again
        if (a.kind == lyrnet::Ask::Save || a.kind == lyrnet::Ask::Remove || a.kind == lyrnet::Ask::EmbedRun || a.kind == lyrnet::Ask::Unembed)
            lyrWritten_ = a.written;   // (the worker's written.tsv as it now is)
        if (a.kind == lyrnet::Ask::Embed) continue;
        if (a.kind == lyrnet::Ask::Written) { lyrWritten_ = a.written; continue; }
        if (a.kind == lyrnet::Ask::SyncSave || a.kind == lyrnet::Ask::SyncRemove) {   // from another device
            lyrWritten_ = a.written;
            if (a.kind == lyrnet::Ask::SyncRemove) {
                for (size_t i = 0; i < lyrMemo_.size(); i++) if (lyrMemo_[i].path == a.path) { lyrMemo_.erase(lyrMemo_.begin() + long(i)); break; }
                if (lyrWrote("tag", a.path) && std::find(lyrTakingOut_.begin(), lyrTakingOut_.end(), a.path) == lyrTakingOut_.end()) lyrTakingOut_.push_back(a.path);
                lyrEmbedAt_ = 0;
            }
            if (now.track >= 0 && lib.tracks[now.track].path == a.path && a.err != "own") { lyrOn = LyrOnline{}; reloadNowTrack(); }
            storage.at = -100;
            continue;
        }
        if (a.kind == lyrnet::Ask::Unembed) {
            if (!a.reached) toast(a.err);
            continue;
        }
        if (a.kind == lyrnet::Ask::EmbedRun) {
            lyrEmbedAsked_ = false;
            if (a.path.empty()) { lyrEmbedAt_ = time + 20; continue; }   // nothing to write now: looked at again in a while
            lyrEmbedAt_ = time + 1;
            const std::string name = a.path.substr(a.path.find_last_of('/') + 1);
            if (a.tookOut) {
                if (a.err != "The song is in use") {
                    const auto t = std::find(lyrTakingOut_.begin(), lyrTakingOut_.end(), a.path);
                    if (t != lyrTakingOut_.end()) lyrTakingOut_.erase(t);
                }
                if (a.reached) {
                    const auto w = std::find(lyrWritten_.begin(), lyrWritten_.end(), "tag\t" + a.path);
                    if (w != lyrWritten_.end()) lyrWritten_.erase(w);
                    toast("Lyrics taken out of “" + name + "”");
                } else if (a.err != "The song is in use") {
                    toast("Could not take the lyrics out of “" + name + "”: " + a.err);
                }
                continue;
            }
            if (a.reached) {
                if (!lyrWrote("tag", a.path)) lyrWritten_.push_back("tag\t" + a.path);
                toast("Lyrics written inside “" + name + "”");
            }
            continue;
        }
        if (a.kind == lyrnet::Ask::Remove) {   // gone: the song as it would be with none, not asked again this run
            if (!a.reached) { toast(a.err); continue; }
            const bool lrc = a.savedTo != lyrnet::savedPath(a.path);   // an .lrc beside the song, Songate's own
            if (lrc) {
                const auto w = std::find(lyrWritten_.begin(), lyrWritten_.end(), "lrc\t" + a.path);
                if (w != lyrWritten_.end()) lyrWritten_.erase(w);
            }
            storage.at = -100;
            lyrMemoSet(a.path, nullptr, "Removed from Songate's folder");
            if (!a.path.empty() && lyrHerePath() == a.path) {
                now.lyrics.clear();
                now.synced = false;
                now.lyricsFromFile = now.lyricsInSongate = false;
                lyrOn = LyrOnline{};
                lyrOn.path = a.path;
                lyrOn.state = LyrOnline::NotFound;
                lyrOn.why = "Removed from Songate's folder";
            }
            toast("Lyrics removed");
            continue;
        }
        const bool current = a.path == lyrOn.path && a.ticket == lyrOn.ticket;
        lyrnet::trace(std::string("app: an answer for ") + a.path + (current ? "" : " (not the song shown now)") + (a.reached ? "" : ", not reached: " + a.err) +
                      ", " + std::to_string(a.found.size()) + " found");
        if (a.kind == lyrnet::Ask::Lyrics && a.reached) {   // kept for its song, shown now or when it comes back
            const bool none = a.found.empty() || a.found[0].instrumental;
            lyrMemoSet(a.path, none ? nullptr : &a.found[0], !a.found.empty() && a.found[0].instrumental ? "LRCLIB has it as an instrumental" : "");
        }
        if (!current) continue;
        if (a.kind == lyrnet::Ask::Save) {
            if (!a.reached) { lyrOn.state = LyrOnline::Found; toast(a.err); continue; }
            lyrOn.state = LyrOnline::Saved;
            lyrOn.shown = false;
            const bool beside = a.savedTo != lyrnet::savedPath(a.path);
            if (beside && !lyrWrote("lrc", a.path)) lyrWritten_.push_back("lrc\t" + a.path);   // (as the worker's written.tsv: Remove offered)
            if (store.lyricsSaveTo == 2 && !beside) {   // Inside the song: for now in the folder (its bar says so, Remove there)
                now.lyricsSource = "Songate's folder";
                now.lyricsFromFile = true;
                now.lyricsInSongate = true;
                storage.at = -100;
                toast("Lyrics saved: written inside the song once it is not playing");
                continue;
            }
            now.lyricsSource = beside ? ".lrc file" : "Songate's folder";
            now.lyricsFromFile = true;
            now.lyricsInSongate = !beside;
            storage.at = -100;   // (Storage's sizes, measured again)
            toast(beside ? "Lyrics saved beside the song" : "Lyrics saved in Songate's folder");
            continue;
        }
        if (!a.reached) {   // no answer (no internet): asked again the next time the song plays, as the tab says
            lyrOn.state = LyrOnline::Failed;
            lyrOn.why = a.err;
            for (size_t i = 0; i < lyrMemo_.size(); i++) if (lyrMemo_[i].path == a.path) { lyrMemo_.erase(lyrMemo_.begin() + long(i)); break; }
            continue;
        }
        if (a.kind == lyrnet::Ask::List) {
            lyrOn.list = std::move(a.found);
            lyrOn.state = LyrOnline::Choosing;
            area("lyr.list").target = area("lyr.list").y = 0;
            continue;
        }
        if (a.found.empty() || a.found[0].instrumental) {
            lyrOn.state = LyrOnline::NotFound;
            lyrOn.why = !a.found.empty() ? "LRCLIB has it as an instrumental" : "";
            continue;
        }
        lyrOn.found = std::move(a.found[0]);
        lyrOn.state = LyrOnline::Found;
        if (store.lyricsOnlineWay == 0 && now.lyrics.empty()) lyrOnlineShow(lyrOn.found);
    }
    // A song with no lyrics, playing here, not asked about this run: asked now
    if (!store.lyricsOnline || now.track < 0 || now.loading || extOn || !now.lyrics.empty() || lyrOn.state != LyrOnline::None) return;
    const Track& t = lib.tracks[now.track];
    lyrOn.path = t.path;
    for (const auto& m : lyrMemo_)
        if (m.path == t.path) {   // answered before this run: what was found or picked, again (and not asked again)
            lyrnet::trace("app: " + t.path + " answered before this run: " + (m.have ? "found" : "none " + m.why));
            if (m.have) {
                lyrOn.found = m.found;
                lyrOn.state = LyrOnline::Found;
                if (store.lyricsOnlineWay == 0) lyrOnlineShow(lyrOn.found);
            } else {
                lyrOn.state = LyrOnline::NotFound;
                lyrOn.why = m.why;
            }
            return;
        }
    const std::string artist = !now.info.artist.empty() ? now.info.artist : t.artist;
    if (artist.empty()) {   // LRCLIB needs the artist: not asked, nothing remembered (its tags may still be on their way)
        static std::string told;
        if (told != t.path) { told = t.path; lyrnet::trace("app: " + t.path + " has no artist yet: not asked"); }
        lyrOn.path.clear();
        return;
    }
    lyrnet::trace("app: asking for " + t.path + " (" + artist + ")");
    lyrnet::Query q;
    q.kind = lyrnet::Ask::Lyrics;
    q.ticket = ++lyrOn.ticket;
    q.path = t.path;
    q.title = !now.info.title.empty() ? now.info.title : t.title;
    q.artist = artist;
    q.album = !now.info.album.empty() ? now.info.album : t.album;
    q.duration = now.info.duration > 0 ? now.info.duration : t.duration;
    lyrOn.state = LyrOnline::Looking;
    lyrNet.ask(q);
}

void App::lyrOnlineKeep() {
    if (lyrOn.state != LyrOnline::Found) return;
    if (!lyrOn.shown) lyrOnlineShow(lyrOn.found);   // (asked first: they show as they are saved)
    lyrnet::Query q;
    q.kind = lyrnet::Ask::Save;
    q.ticket = lyrOn.ticket;
    q.path = lyrOn.path;
    q.text = lyrOn.found.text();
    const bool server = lyrOn.path.rfind("server:", 0) == 0;   // a server track: kept in Songate's folder (no file of yours to put them in)
    q.beside = !server && store.lyricsSaveTo == 0;
    lyrOn.state = LyrOnline::Saving;
    lyrNet.ask(q);
    if (!server && store.lyricsSaveTo == 2) {   // Inside the song: in Songate's folder now; written inside once it is neither playing nor next
        lyrnet::Query e;
        e.kind = lyrnet::Ask::Embed;
        e.path = lyrOn.path;
        lyrNet.ask(e);
        lyrEmbedAt_ = 0;
    }
}

void App::lyrOnlineRemove() {
    const std::string path = lyrHerePath();   // (a server track's too: kept in Songate's folder)
    if (path.empty()) return;
    if (path.rfind("server:", 0) != 0) syncer.noteLyricsGone(path);   // other devices take theirs out too, at the next sync
    lyrnet::Query q;
    q.path = path;
    if (now.lyricsInSongate) {
        q.kind = lyrnet::Ask::Remove;
    } else if (now.lyricsFromFile && lyrWrote("lrc", path)) {   // the .lrc Songate made beside it
        q.kind = lyrnet::Ask::Remove;
        q.text = "lrc";
    } else if (!now.lyricsFromFile && lyrWrote("tag", path)) {   // what Songate wrote inside it: taken out once it is not playing
        q.kind = lyrnet::Ask::Unembed;
        if (std::find(lyrTakingOut_.begin(), lyrTakingOut_.end(), path) == lyrTakingOut_.end()) lyrTakingOut_.push_back(path);
        now.lyrics.clear();
        now.synced = false;
        lyrOn = LyrOnline{};
        lyrOn.path = path;
        lyrOn.state = LyrOnline::NotFound;
        lyrOn.why = "Taken out of the song";
        lyrMemoSet(path, nullptr, lyrOn.why);
        lyrEmbedAt_ = 0;
        toast("The lyrics are taken out of the song once it is not playing");
    } else {
        return;
    }
    lyrNet.ask(q);
}

void App::lyrOnlineNotThese() {
    const Track* t = now.track >= 0 && now.track < int(lib.tracks.size()) ? &lib.tracks[size_t(now.track)] : nullptr;   // (a server's song has none of yours)
    if (lyrOn.path.empty() || (!t && now.info.artist.empty())) return;
    lyrnet::Query q;
    q.kind = lyrnet::Ask::List;
    q.ticket = ++lyrOn.ticket;
    q.path = lyrOn.path;
    q.title = !now.info.title.empty() ? now.info.title : t ? t->title : std::string();
    q.artist = !now.info.artist.empty() ? now.info.artist : t ? t->artist : std::string();
    q.duration = now.info.duration > 0 ? now.info.duration : t ? t->duration : 0;
    lyrOn.state = LyrOnline::Listing;
    lyrNet.ask(q);
    lyrFocusNext_ = uid("lyr.pick", 0);   // (while it looks there is nothing to hold it: it waits for the list)
}

void App::lyrOnlinePick(int i) {
    if (i < 0) {   // None of these: nothing shown, nothing kept
        if (lyrOn.shown) { now.lyrics.clear(); now.synced = false; lyrOn.shown = false; }
        lyrOn.state = LyrOnline::NotFound;
        lyrOn.why = "None of LRCLIB's was right";
        lyrRemember();
        return;
    }
    if (i >= int(lyrOn.list.size())) return;
    lyrOn.found = lyrOn.list[size_t(i)];
    lyrOn.state = LyrOnline::Found;
    lyrRemember();
    lyrFocusNext_ = uid(store.lyricsOnlineWay == 0 ? "lyr.keep" : "lyr.save");
    if (store.lyricsOnlineWay == 0) lyrOnlineShow(lyrOn.found);
    else if (lyrOn.shown) { now.lyrics.clear(); now.synced = false; lyrOn.shown = false; }
}

bool App::lyrOnlineBar(float x, float y, float w) {
    // Kept in Songate's own folder: Remove (the user, 2026-11-04: "how do i delete the lrc when it is saved in songate folder")
    const std::string here = lyrHerePath();
    const bool ours = now.lyricsInSongate || (now.lyricsFromFile && lyrWrote("lrc", here)) || (!now.lyricsFromFile && lyrWrote("tag", here));
    if (ours && !now.lyrics.empty() && !lyrOn.shown) {   // (only what Songate wrote: your own lyrics never get a Remove)
        const char* where = now.lyricsInSongate ? " · Songate's folder" : now.lyricsFromFile ? " · .lrc file, saved by Songate" : " · Tags, written by Songate";
        g.textC(std::string(now.synced ? "Synced" : "Plain text") + where, { css::Xs }, x, y + 20, pal.text3, 1, Align::Left, int(w * 0.55f));
        const Font bf{ css::Sm, Weight::SemiBold };
        const float rw = 32 + 24 + g.measure("Remove", bf);
        bool c = false;
        button(uid("lyr.remove"), ICON_X, "Remove", x + w - rw, y + 3, false, &c);
        if (c) lyrOnlineRemove();
        return true;
    }
    // Play them: playing, Keep or Not these. Ask first's Try (the user, 2026-11-04: "the user needs to see if the lyrics are on
    // sync"): playing the same way, Save as on the card
    const bool listing = lyrOn.state == LyrOnline::Listing;
    if (!lyrOn.shown || (lyrOn.state != LyrOnline::Found && !listing) || lyrOn.path.empty() || lyrHerePath() != lyrOn.path) return false;
    const bool trying = store.lyricsOnlineWay == 1;
    const char* keep = trying ? "Save" : "Keep";
    g.textC(std::string(now.synced ? "Synced" : "Plain text") + (trying ? " · trying, not saved" : " · found online, not saved"), { css::Xs }, x, y + 20,
            pal.text3, 1, Align::Left, int(w * 0.5f));
    const Font bf{ css::Sm, Weight::SemiBold };
    const char* other = listing ? "Looking…" : "Not these";
    const float w2 = 32 + std::max(g.measure("Not these", bf), g.measure("Looking…", bf)), w1 = 32 + 24 + g.measure(keep, bf);
    bool c = false;
    button(uid("lyr.notthese"), nullptr, other, x + w - w2, y + 3, false, &c, false, false, 0, w2);
    if (c && !listing) { lyrOnlineNotThese(); return true; }
    button(uid("lyr.keep"), ICON_CHECK, keep, x + w - w2 - 8 - w1, y + 3, true, &c);
    if (lyrFocusNext_ && ui.hasItem(lyrFocusNext_)) { ui.focus(lyrFocusNext_); lyrFocusNext_ = 0; }
    if (c) lyrOnlineKeep();
    return true;
}

bool App::drawLyrOnline(float x, float y, float w, float h) {
    if (lyrOn.path.empty() || lyrHerePath() != lyrOn.path) return false;
    const Track* t = now.track >= 0 && now.track < int(lib.tracks.size()) ? &lib.tracks[size_t(now.track)] : nullptr;   // (a server's song has none of yours)
    const std::string song = !now.info.title.empty() ? now.info.title : t ? t->title : std::string();
    const bool stream = srvq_.on;   // no .lrc file to put anywhere for a server's song
    auto words = [&](const std::string& head, const std::string& line) {
        g.text(head, { css::Lg, Weight::SemiBold }, x, y + 28, pal.text2, 1, Align::Left, int(w));
        float ty = y + 56;
        for (const auto& l : g.wrap(line, { css::Base }, int(w), 3)) { g.text(l, { css::Base }, x, ty, pal.text3, 1, Align::Left, int(w)); ty += 22; }
    };
    switch (lyrOn.state) {
    case LyrOnline::Looking:
        words("Looking for lyrics online…", "This song has none here. LRCLIB is asked for its title, artist, album and length.");
        return true;
    case LyrOnline::NotFound:
        if (lyrOn.shown) return false;
        if (lyrOn.why == "Removed from Songate's folder") {
            words("No lyrics", "Removed. Found online again the next time Songate starts, or put an .lrc file beside it.");
            return true;
        }
        if (lyrOn.why == "Taken out of the song") {
            const bool waiting = std::find(lyrTakingOut_.begin(), lyrTakingOut_.end(), lyrOn.path) != lyrTakingOut_.end();
            words("No lyrics", waiting ? "Taken out of the song once it is not playing. Found online again the next time Songate starts."
                                       : "Taken out of the song. Found online again the next time Songate starts.");
            return true;
        }
        words("No lyrics", stream ? (lyrOn.why.empty() ? std::string("None found online either.") : lyrOn.why + ".")
                           : lyrOn.why.empty() ? "None found online either. An .lrc file with the song's name next to it, or a LYRICS tag, still works."
                                               : lyrOn.why + ". An .lrc file with the song's name next to it, or a LYRICS tag, still works.");
        return true;
    case LyrOnline::Failed:
        words("No lyrics", "Could not ask LRCLIB: " + lyrOn.why + ". It is asked again the next time the song plays.");
        return true;
    case LyrOnline::Choosing: {
        kicker("Other lyrics for " + song, x, y + 14, pal.text3);
        const float top = y + 30, vh = h - 30, rowH = 56;
        ScrollArea& sa = area("lyr.list");
        const int n = int(lyrOn.list.size());
        sa.view = { x - 8, top, w + 16, vh };
        sa.content = (n + 1) * rowH + 8;
        ui.pushArea(&sa);
        g.clip(x - 8, top, w + 16, vh);
        const double len = now.info.duration > 0 ? now.info.duration : t ? t->duration : 0;
        int picked = -2;
        for (int i = 0; i <= n; i++) {   // the records, then None of these
            const float ry = top + i * rowH - sa.y;
            const Id id = uid("lyr.pick", i);
            if (ui.item(id, { x - 8, ry, w + 16, rowH - 4 }, 8)) picked = i < n ? i : -1;
            if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(x - 8, ry, w + 16, rowH - 4, 8, pal.text, 0.065f);
            if (ry + rowH < top || ry > top + vh) continue;
            if (i == n) {
                g.textC("None of these", { css::Base, Weight::SemiBold }, x + 4, ry + 26, pal.text2);
                continue;
            }
            const lyrnet::Found& f = lyrOn.list[size_t(i)];
            const bool fits = len <= 0 || std::fabs(f.duration - len) <= 2;
            const std::string right = std::string(f.synced.empty() ? "Plain" : "Synced") + "  ·  " + fmtTime(f.duration);
            const int rw = g.measure(right, { css::Sm });
            g.text(f.title, { css::Base, Weight::SemiBold }, x + 4, ry + 7, pal.text, 1, Align::Left, int(w - rw - 24));
            g.text(f.artist + (f.album.empty() ? "" : " · " + f.album), { css::Sm }, x + 4, ry + 27, pal.text3, 1, Align::Left, int(w - rw - 24));
            g.textC(right, { css::Sm }, x + w - 4, ry + 26, fits && !f.synced.empty() ? pal.accentInk : pal.text3, 1, Align::Right);
        }
        g.unclip();
        ui.popArea();
        if (lyrFocusNext_ && ui.hasItem(lyrFocusNext_)) { ui.focus(lyrFocusNext_); lyrFocusNext_ = 0; }
        if (picked != -2) lyrOnlinePick(picked);
        return true;
    }
    case LyrOnline::Listing:
        if (lyrOn.shown) return false;   // the lyrics stay, Not these saying Looking… (lyrOnlineBar)
        if (lyrOn.found.text().empty()) {
            words("Looking for other lyrics…", "LRCLIB's records for " + song + ", its length first.");
            return true;
        }
        [[fallthrough]];   // the card stays, its Not these saying Looking…
    case LyrOnline::Found:
    case LyrOnline::Saving: {
        if (lyrOn.shown) return false;   // Play: the lyrics themselves, Keep and Not these on their bar
        // Ask first: a card with what was found
        const lyrnet::Found& f = lyrOn.found;
        // its height: as many of the first lines as fit (four at most), with the same gap below it as above (the user, 2026-11-04:
        // "clipping a bit at the bottom. The distance to the playing bar below should be the same distance that it is now from above")
        // (the tabs' line is 6 above y, and the area ends 11 above the player bar: 5 here makes both gaps 11, measured)
        constexpr float gap = 5, below = 0, fixedH = 18 + 20 + 24 + 24 + 12 + 34 + 18;
        const float room = h - gap - below;
        const size_t fit = size_t(std::clamp(int(std::floor((room - fixedH) / 22)), 0, 4));
        std::vector<std::string> first;
        for (size_t a = 0; a < f.text().size() && first.size() < fit;) {
            size_t b = f.text().find('\n', a);
            if (b == std::string::npos) b = f.text().size();
            std::string l = f.text().substr(a, b - a);
            a = b + 1;
            while (!l.empty() && l[0] == '[') { const size_t c = l.find(']'); if (c == std::string::npos) break; l.erase(0, c + 1); }
            while (!l.empty() && (l[0] == ' ' || l.back() == '\r')) { if (l[0] == ' ') l.erase(0, 1); else l.pop_back(); }
            if (!l.empty()) first.push_back(l);
        }
        const float cw = w, cy = y + gap;
        const float ch = std::max(room, fixedH + first.size() * 22.f);   // filling its room: the buttons at its foot
        g.roundRect(x, cy, cw, ch, 14, pal.text, 0.05f);
        g.roundStroke(x, cy, cw, ch, 14, 1, pal.text, 0.1f);
        float ty = cy + 18;
        kicker("Found online", x + 18, ty + 6, pal.accentInk);
        ty += 20;
        g.text("Lyrics for " + song, { css::Md, Weight::Bold }, x + 18, ty, pal.text, 1, Align::Left, int(cw - 36));
        ty += 24;
        const double len = now.info.duration > 0 ? now.info.duration : t ? t->duration : 0;
        const bool fits = len <= 0 || std::fabs(f.duration - len) <= 2;
        g.text(std::string(f.synced.empty() ? "Plain text" : "Synced") + " · LRCLIB · " + f.artist + " · " +
                   (fits ? "matches its length" : fmtTime(f.duration) + ", the song is " + fmtTime(len)),
               { css::Xs }, x + 18, ty, pal.text3, 1, Align::Left, int(cw - 36));
        ty += 24;
        for (const auto& l : first) { g.text(l, { css::Base }, x + 18, ty, pal.text2, 1, Align::Left, int(cw - 36)); ty += 22; }
        ty = cy + ch - 18 - 34;
        bool c = false;
        float bx = x + 18;
        const bool saving = lyrOn.state == LyrOnline::Saving || lyrOn.state == LyrOnline::Listing;
        const bool listing = lyrOn.state == LyrOnline::Listing;
        bx += button(uid("lyr.save"), saving && !listing ? nullptr : ICON_CHECK, saving && !listing ? "Saving…" : "Save", bx, ty, true, &c) + 8;
        if (c && !saving) lyrOnlineKeep();
        bx += button(uid("lyr.try"), ICON_PLAY, "Try", bx, ty, false, &c) + 8;   // played in time, not saved: Save or Not these on the bar
        if (c && !saving) { lyrOnlineShow(f); lyrFocusNext_ = uid("lyr.keep"); }   // (Try: the bar's Save)
        const Font nf{ css::Sm, Weight::SemiBold };
        button(uid("lyr.notthese"), nullptr, listing ? "Looking…" : "Not these", bx, ty, false, &c, false, false, 0,
               32 + std::max(g.measure("Not these", nf), g.measure("Looking…", nf)));
        if (lyrFocusNext_ && ui.hasItem(lyrFocusNext_)) { ui.focus(lyrFocusNext_); lyrFocusNext_ = 0; }
        if (c && !saving) lyrOnlineNotThese();
        g.textC(store.lyricsSaveTo == 0 ? "Saved as an .lrc beside the song" : store.lyricsSaveTo == 2 ? "Saved inside the song" : "Saved in Songate's folder",
                { css::Xs }, x + cw - 18, ty + 17, pal.text3, 1,
                Align::Right);
        return true;
    }
    default: return false;
    }
}
