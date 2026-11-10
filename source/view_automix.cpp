/*
 * Gatefold NX · view_automix.cpp
 * Automix playlists (the user's idea, 2026-10-28): + › Automix playlist opens a picker (Tracks, Albums, Artists, Genres,
 * Folders, a tick on each row; an album, artist, genre or folder ticks all of its tracks); Done names it, and Gatefold
 * orders it for the best joins (automix::order, from each track's analysis). It is ordered again whenever tracks are
 * added, and once the tracks not analysed yet are (they are analysed first meanwhile), until it is moved by hand; its page
 * then offers Order again. A track can be pinned to play first. Over every page, as the join editor.
 */
#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"

/** Orders an Automix playlist (store.playlists[ref]) for the best joins; tracks gone from the library stay at its end. */
automix::Model App::mixModel() const {
#ifdef GF_DEV
    automix::Model m;
    if (automix::modelFromName(store.automixModel, m)) return m;   // Settings › Debug: models compared on the console
#endif
    return automix::Latest;   // a release: the newest (a choice saved by a dev build is ignored)
}

namespace {
// Automix V1.2.0, bundled (the user, 2026-11-03: "Bundle automix 1.2 in"): byte for byte the file songate-automix released as
// v1.2.0 (models/automix-v1.2.0.model, 377 bytes, sha256 2e2a625f…6535), under the name a download of it has, so the two are one
constexpr char kBundledFile[] = "automix-v1.2.0.model";
constexpr char kBundledModel[] = R"MODEL(songate-automix-model 1
name Automix V1.2.0
app 2.0.0-beta
mixan 4
order 1.0.0
gateOn drop,structure
dropBars 8
bridgeFrac 0.25
exitBars 1
tempoBridgeMax 8
tempoCutLead 2
cutFade 0.03
cutEcho false
fadeScale 1
inLevel 0.8
lpFrom 1000
outPow 5
bridgeOutPow 2
noSkip true
noCut true
gate 1
gateLevel 1
gateDuck 0.3
gateEvery 1
clashBeats 16
clashBars 2
blendFrom 8
beatMaxSec 32
)MODEL";
}  // namespace

const char* App::bundledModelFile() { return kBundledFile; }

/** The models this Songate plans with from a file: each .model file in config/songate-nx/models (a few small files: at launch and
 *  after a download). The bundled one is written there first when it is missing (a first start, a factory reset), so the
 *  background player, which reads the same folder, has it too. */
void App::loadModelFiles() {
    modelFiles.clear();
    const std::string dir = store.path("models");
    const std::string bundled = dir + "/" + kBundledFile;
    struct stat st;
    if (stat(bundled.c_str(), &st) != 0) {
        // every folder on the way: on a first start the settings folder itself is not made yet (the user, 2026-11-03: a fresh
        // PC install came without V1.2.0)
        for (size_t i = 1; i < dir.size(); i++)
            if (dir[i] == '/' || dir[i] == '\\') mkdir(dir.substr(0, i).c_str(), 0777);
        mkdir(dir.c_str(), 0777);
        if (FILE* out = fopen(bundled.c_str(), "wb")) {
            fwrite(kBundledModel, 1, sizeof kBundledModel - 1, out);
            fclose(out);
        }
    }
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (const dirent* ent = readdir(d)) {
        const std::string f = ent->d_name;
        if (f.size() < 7 || f[0] == '.' || f.compare(f.size() - 6, 6, ".model") != 0) continue;
        std::string text;
        if (FILE* in = fopen((dir + "/" + f).c_str(), "rb")) {
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof buf, in)) > 0 && text.size() < 65536;) text.append(buf, n);
            fclose(in);
        }
        ModelEntry e;
        if (!e.m.parse(text)) continue;
        e.file = f;
        modelFiles.push_back(std::move(e));
    }
    closedir(d);
    std::sort(modelFiles.begin(), modelFiles.end(), [](const ModelEntry& a, const ModelEntry& b) { return a.m.name < b.m.name; });
}

const App::ModelEntry* App::mixFile() const {
    // none chosen: the bundled one; a built-in model's name (Go back to Automix V1.1.0): no file, the built-in one
    if (store.automixModel.empty()) {
        for (const ModelEntry& e : modelFiles) if (e.file == kBundledFile) return &e;
        return nullptr;
    }
    for (const ModelEntry& e : modelFiles) if (e.m.name == store.automixModel) return &e;
    return nullptr;
}

std::string App::mixName() const { return mixFile() ? mixFile()->m.name : automix::name(mixModel()); }

automix::Plan App::planJoin(const mixan::Info* a, double dur, const mixan::Info* b, double fallback, const automix::Join* want) const {
    if (const ModelEntry* f = mixFile()) return automix::plan(a, dur, b, fallback, want, f->m);
    return automix::plan(a, dur, b, fallback, want, mixModel());
}

void App::orderAutomixList(int ref, bool announce) {
    if (ref < 0 || ref >= int(store.playlists.size())) return;
    FolderPlaylist& p = store.playlists[size_t(ref)];
    if (!p.own() || !p.automix) return;
    std::vector<std::string> here, gone;
    std::vector<mixan::Info> info;
    bool waiting = false;
    for (const auto& path : p.tracks) {
        if (!pathIndex.count(path)) gone.push_back(path);
        else here.push_back(path);
    }
    // by path first: the same tracks give the same order, whatever order they stood in (the user, 2026-10-28: "every time I
    // click on the reorganize button it gives a completely different sequence": ties went by where the tracks stood)
    std::sort(here.begin(), here.end());
    // only the analysed ones are ordered (and shown); the others wait after them, and each joins the order where it fits
    // best once analysed (automixListsTick): the mix builds up song by song (the user, 2026-10-28)
    std::vector<std::string> later;
    {
        std::vector<std::string> ready;
        for (const auto& path : here) (automixReady(path) ? ready : later).push_back(path);
        here.swap(ready);
    }
    waiting = !later.empty();
    (void)waiting;
    for (const auto& path : here) info.push_back(levels.find(path)->mix);
    int first = -1;
    for (size_t i = 0; i < here.size(); i++) if (!p.pin.empty() && here[i] == p.pin) first = int(i);
    // the order itself on a worker (mixOrderTick puts it in): one at a time; another asked meanwhile waits its turn
    const bool sameRunning = mixOrderOn_ && mixOrderJob_.name == p.name && mixOrderJob_.before == p.tracks;
    if (sameRunning) { mixOrderJob_.announce = mixOrderJob_.announce || announce; return; }
    if (mixOrderOn_ || store.stopMixOrder) {
        for (auto& w : mixOrderWaiting_) if (w.first == p.name) { w.second = w.second || announce; return; }
        mixOrderWaiting_.push_back({ p.name, announce });
        if (announce && store.stopMixOrder) toast("Ordering is stopped: Resume it in Settings › Playback");
        return;
    }
    mixOrderJob_ = MixOrderJob{};
    mixOrderJob_.name = p.name;
    mixOrderJob_.before = p.tracks;
    mixOrderJob_.here = std::move(here);
    mixOrderJob_.later = std::move(later);
    mixOrderJob_.gone = std::move(gone);
    mixOrderJob_.info = std::move(info);
    mixOrderJob_.first = first;
    mixOrderJob_.model = mixModel();
    mixOrderJob_.announce = announce;
    mixOrderWatch_.done = 0;
    mixOrderWatch_.total = 0;
    mixOrderWatch_.stop = false;
    mixOrderDone_ = false;
    mixOrderOn_ = true;
    if (mixOrderThread_.joinable()) mixOrderThread_.join();
    mixOrderThread_ = std::thread([this] {
        cores::moveHere(cores::Files);
        std::vector<const mixan::Info*> ptr;
        for (const auto& i : mixOrderJob_.info) ptr.push_back(&i);
        mixOrderJob_.result = automix::order(ptr, mixOrderJob_.first, mixOrderJob_.model, &mixOrderWatch_);
        mixOrderDone_ = true;
    });
}

/** Every frame: the order a worker found goes into its playlist; then the next one waiting is asked. */
void App::mixOrderTick() {
    if (mixOrderOn_ && mixOrderDone_) {
        if (mixOrderThread_.joinable()) mixOrderThread_.join();
        mixOrderOn_ = false;
        MixOrderJob j = std::move(mixOrderJob_);
        mixOrderJob_ = MixOrderJob{};
        int ref = -1;
        for (int i = 0; i < int(store.playlists.size()); i++) if (store.playlists[size_t(i)].name == j.name) ref = i;
        if (ref >= 0 && store.playlists[size_t(ref)].own() && store.playlists[size_t(ref)].automix) {
            FolderPlaylist& p = store.playlists[size_t(ref)];
            const bool whole = j.result.size() == j.here.size();   // (empty for a list with songs: it was stopped)
            if (!whole || p.tracks != j.before) {
                // stopped, or songs came or went meanwhile: asked again (at once, or when ordering is resumed)
                mixOrderWaiting_.insert(mixOrderWaiting_.begin(), { j.name, j.announce });
            } else {
                std::vector<std::string> out;
                for (int i : j.result) out.push_back(j.here[size_t(i)]);
                out.insert(out.end(), j.later.begin(), j.later.end());
                out.insert(out.end(), j.gone.begin(), j.gone.end());
                p.tracks = out;
                p.waiting = !j.later.empty();
                p.manual = false;
                persistSoon(0.3f);
                if (j.announce) toast(p.waiting ? "Ordered “" + p.name + "”; again once all of its songs are analysed" : "Ordered “" + p.name + "” for Automix");
            }
        }
    }
    if (!mixOrderOn_ && !store.stopMixOrder && !mixOrderWaiting_.empty()) {
        const auto next = mixOrderWaiting_.front();
        mixOrderWaiting_.erase(mixOrderWaiting_.begin());
        for (int i = 0; i < int(store.playlists.size()); i++)
            if (store.playlists[size_t(i)].name == next.first) { orderAutomixList(i, next.second); break; }
    }
}

void App::mixOrderEnd() {
    mixOrderWatch_.stop = true;
    if (mixOrderThread_.joinable()) mixOrderThread_.join();
    mixOrderOn_ = false;
}

bool App::automixReady(const std::string& path) const {
    const Level* l = levels.find(path);
    return l && l->mix.seconds != 0;
}

/**
 * Every second: in an Automix playlist still being analysed, each track the worker has finished leaves the waiting ones
 * (after the ordered ones, not shown) and goes where it joins best between two of them (the gain of putting it there:
 * the two new joins less the one it breaks; never before a pinned first track); once all are in, the whole playlist is
 * ordered once more. An insertion is a row of scores, cheap on the console; ordering again each time would not be.
 */
void App::automixListsTick() {
    if (time - automixTickAt < 1) return;
    automixTickAt = time;
    for (int i = 0; i < int(store.playlists.size()); i++) {
        FolderPlaylist& p = store.playlists[size_t(i)];
        if (!p.automix || !p.waiting || p.manual) continue;
        // the ordered ones (ready, in front), then the waiting ones
        size_t head = 0;
        while (head < p.tracks.size() && pathIndex.count(p.tracks[head]) && automixReady(p.tracks[head])) head++;
        bool all = true;
        for (size_t k = head; k < p.tracks.size();) {
            const std::string path = p.tracks[k];
            if (!pathIndex.count(path)) { k++; continue; }
            if (!automixReady(path)) { all = false; k++; continue; }
            const mixan::Info& x = levels.find(path)->mix;
            auto info = [&](size_t j) { return &levels.find(p.tracks[j])->mix; };
            const size_t from = !p.pin.empty() && head > 0 && p.tracks[0] == p.pin ? 1 : 0;
            size_t at = head;   // at the end, unless somewhere between is better
            const automix::Model mm = mixModel();
            double best = head > 0 ? automix::joinScore(info(head - 1), &x, mm) : 0;
            for (size_t j = from; j < head; j++) {   // before p.tracks[j]
                const double g = (j > 0 ? automix::joinScore(info(j - 1), &x, mm) : 0) + automix::joinScore(&x, info(j), mm) -
                                 (j > 0 ? automix::joinScore(info(j - 1), info(j), mm) : 0);
                if (g > best) { best = g; at = j; }
            }
            p.tracks.erase(p.tracks.begin() + long(k));
            p.tracks.insert(p.tracks.begin() + long(at), path);
            head++;
            k++;
        }
        if (all) {
            orderAutomixList(i, false);
            toast("“" + p.name + "” is ordered for Automix: all of its songs are analysed");
        }
        // (songs placed as they are analysed are saved with the next save or at the close, not every 5 s meanwhile: the order
        // is made again from the analyses if Songate stops before)
    }
}

/** The tracks of Automix playlists not analysed yet (up to `max`): the levelling worker takes them first. */
std::vector<std::string> App::automixWaitingTracks(size_t max) const {
    std::vector<std::string> out;
    for (const auto& p : store.playlists) {
        if (!p.own() || !p.automix) continue;
        for (const auto& path : p.tracks) {
            if (out.size() >= max) return out;
            const Level* l = levels.find(path);
            if (pathIndex.count(path) && (!l || l->mix.seconds == 0)) out.push_back(path);
        }
    }
    return out;
}

/** The queue came from an Automix playlist (q.name is the playlist's): only then does Automix join its tracks. */
bool App::automixQueue() const {
    if (q.name.empty()) return false;
    for (const auto& p : store.playlists)
        if (p.own() && p.automix && p.name == q.name) return true;
    return false;
}

/** The playlist the queue came from, for the queue page's kicker and icon: own playlists first (an Automix one
 *  and a folder of the same name: the one that plays Automix), then smart and folder ones. -1: none. */
int App::queuePlaylist() const {
    if (q.name.empty()) return -1;
    int other = -1;
    for (size_t i = 0; i < store.playlists.size(); ++i) {
        const FolderPlaylist& p = store.playlists[i];
        if (p.name != q.name) continue;
        if (p.own()) return int(i);
        if (other < 0) other = int(i);
    }
    return other;
}

void App::automixProgress(int& done, int& total) const {
    done = total = 0;
    for (const auto& p : store.playlists) {
        if (!p.own() || !p.automix) continue;
        for (const auto& path : p.tracks) {
            if (!pathIndex.count(path)) continue;
            const Level* l = levels.find(path);
            total++;
            if (l && l->mix.seconds != 0) done++;
        }
    }
}

void App::openAutomixPicker() {
    closeSheet();
    ap = AutomixPick{};
    ap.open = true;
    ap.sel.assign(lib.tracks.size(), 0);
}

void App::closeAutomixPicker() { ap.open = false; ap.sel.clear(); ap.sel.shrink_to_fit(); }

void App::drawAutomixPicker() {
    if (!ap.open) return;
    if (ap.sel.size() != lib.tracks.size()) ap.sel.assign(lib.tracks.size(), 0);   // the library changed under it
    ui.region(Region::Modal);
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.6f);
    // the PC's and the console's 1080 by 676; on a smaller screen, the screen less a margin (a phone upright: narrow, see below)
    const float W = std::min(1080.f, css::W - 24.f), H = std::min(676.f, css::H - 40.f), X = (css::W - W) / 2, Y = (css::H - H) / 2;
    const bool narrow = W < 700;
    const float pad = narrow ? 18 : 24, ix = X + pad, iw = W - pad * 2;
    g.roundRect(X, Y, W, H, 18, pal.surface);
    g.roundStroke(X, Y, W, H, 18, 1, pal.text, 0.08f);

    // ---- header
    g.roundRect(ix, Y + pad, 40, 40, 10, pal.accent, 0.14f);
    g.icon(ICON_SPARKLE, ix + 20, Y + pad + 20, 20, pal.accentInk);   // the Automix playlists' icon
    kicker("New Automix playlist", ix + 54, Y + pad + 7, pal.accentInk);
    // narrow: the line under the tile, wrapped, and Done on the row of the count (it has no room by the close)
    const std::vector<std::string> titleLines = narrow ? g.wrap("Pick the songs; Songate orders them for the best joins", { css::Lg, Weight::Bold }, int(iw), 2)
                                                       : std::vector<std::string>{ "Pick the songs; Songate orders them for the best joins" };
    if (narrow) {
        for (size_t k = 0; k < titleLines.size(); k++)
            g.text(titleLines[k], { css::Lg, Weight::Bold }, ix, Y + pad + 52 + k * 26, pal.text, 1, Align::Left, int(iw));
    } else {
        g.text(titleLines[0], { css::Lg, Weight::Bold }, ix + 54, Y + pad + 16, pal.text, 1, Align::Left, int(iw - 54 - 260));
    }
    const float headExtra = narrow ? 52 + titleLines.size() * 26 + 6 : 0;   // what the narrow header adds above the count's row
    hint(uid("ap.x"), "Close");
    if (iconButton(uid("ap.x"), ICON_X, X + W - pad - 17, Y + pad + 20)) { closeAutomixPicker(); return; }
    int count = 0;
    for (char s : ap.sel) count += s;
    {
        const Font bf{ css::Sm, Weight::SemiBold };
        const float dw = 16 + 24 + g.measure("Done", bf) + 16;
        bool done = false;
        if (narrow) button(uid("ap.done"), ICON_CHECK, "Done", X + W - pad - dw, Y + pad + headExtra, count > 0, &done);
        else button(uid("ap.done"), ICON_CHECK, "Done", X + W - pad - 34 - 12 - dw, Y + pad + 3, count > 0, &done);
        if (done) {
            if (!count) { toast("Pick a few songs first"); return; }
            std::string name = "Automix";
            if (!keyboard("Name the Automix playlist", name, name)) return;
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) name = "Automix";
            name = store.freePlaylistName(name);
            FolderPlaylist p{ name, "", {} };
            for (size_t i = 0; i < ap.sel.size(); i++) if (ap.sel[i]) p.tracks.push_back(lib.tracks[i].path);
            p.automix = true;
            store.playlists.push_back(p);
            const int ref = int(store.playlists.size()) - 1;
            closeAutomixPicker();
            orderAutomixList(ref, true);
            navigate(View::List, ref);
            return;
        }
    }

    // ---- the tabs, and how many are ticked
    // (narrow: the count by Done, the tabs on a row of their own under it)
    const float ty = narrow ? Y + pad + headExtra + 46 : Y + pad + 62;
    {
        int picked = -1;
        segmented(uid("ap.tab"), ix + iw, ty + 18, { "Songs", "Albums", "Artists", "Genres", "Folders" }, ap.tab, &picked, 0, narrow ? iw : 0, narrow ? 2 : 0);   // (narrow: the column's width, as the text)
        if (picked >= 0 && picked != ap.tab) ap.tab = picked;
        char b[64];
        snprintf(b, sizeof b, "%d song%s ticked", count, count == 1 ? "" : "s");
        g.textC(b, { css::Md, Weight::SemiBold }, ix, narrow ? Y + pad + headExtra + 17 : ty + 18, count ? pal.accentInk : pal.text2, 1);
    }

    // ---- the rows: only those on screen are drawn (a library of thousands)
    struct RowRef { const std::vector<int>* tracks; int one; std::string title, sub; };
    auto rowCount = [&]() -> int {
        switch (ap.tab) {
        case 0: return int(lib.tracks.size());
        case 1: return int(lib.albums.size());
        case 2: return int(artists.size());
        case 3: return int(genres.size());
        default: return std::max(0, int(lib.folders.size()) - 1);   // every folder but the music folder itself
        }
    };
    auto rowAt = [&](int i) -> RowRef {
        switch (ap.tab) {
        case 0: { const Track& t = lib.tracks[size_t(i)]; return { nullptr, i, t.title, t.artist + (t.album.empty() ? "" : " · " + t.album) }; }
        case 1: { const Album& a = lib.albums[size_t(i)]; return { &a.tracks, -1, a.title, a.artist }; }
        case 2: return { &artists[size_t(i)].tracks, -1, artists[size_t(i)].name, "" };
        case 3: return { &genres[size_t(i)].tracks, -1, genres[size_t(i)].name, "" };
        default: { const FolderNode& f = lib.folders[size_t(i) + 1]; return { nullptr, -2 - (i + 1), f.name, f.path }; }
        }
    };
    const float lx = ix, ly = ty + 46, lw = iw, lh = Y + H - pad - ly, RH = 48;
    ScrollArea& sa = area("ap.list", ap.tab);
    sa.view = { lx, ly, lw, lh };
    const int n = rowCount();
    sa.content = n * RH;
    ui.pushArea(&sa);
    g.clip(lx, ly, lw, lh);
    const int i0 = std::max(0, int(sa.y / RH) - 1), i1 = std::min(n, int((sa.y + lh) / RH) + 2);
    for (int i = i0; i < i1; i++) {
        const RowRef r = rowAt(i);
        const float y = ly - sa.y + i * RH;
        // its tracks: one, a list, or a folder's range
        std::vector<int> folderTracks;
        const std::vector<int>* list = r.tracks;
        if (r.one <= -2) { folderTracks = lib.tracksUnder(-2 - r.one); list = &folderTracks; }
        int on = 0, all = 0;
        if (r.one >= 0) { on = ap.sel[size_t(r.one)]; all = 1; }
        else for (int t : *list) { on += ap.sel[size_t(t)]; all++; }
        const Id id = uid("ap.r", ap.tab * 1000000 + i);
        if (ui.item(id, { lx, y, lw, RH - 4 }, 10)) {
            const char v = on == all ? 0 : 1;   // all ticked: untick them all; else tick them all
            if (r.one >= 0) ap.sel[size_t(r.one)] = v;
            else for (int t : *list) ap.sel[size_t(t)] = v;
        }
        const bool hot = ui.focused(id) && ui.showFocusRing();
        if (hot || ui.pressed(id)) g.roundRect(lx, y, lw, RH - 4, 10, pal.text, 0.06f);
        // the tick: full, part (a dash), or empty
        const float bx = lx + 14, by = y + (RH - 4) / 2 - 10;
        if (on && on == all) {
            g.roundRect(bx, by, 20, 20, 5, pal.accent);
            g.icon(ICON_CHECK, bx + 10, by + 10, 14, pal.onAccent);
        } else {
            g.roundStroke(bx, by, 20, 20, 5, 1.5f, pal.text, 0.35f);
            if (on) g.roundRect(bx + 5, by + 9, 10, 2.5f, 1, pal.accent);
        }
        g.text(r.title.empty() ? "Unknown" : r.title, { css::Base, Weight::SemiBold }, lx + 48, y + 6, pal.text, 1, Align::Left, int(lw - 48 - (narrow ? 72 : 140)));
        const std::string sub = r.one >= 0 ? r.sub : (r.sub.empty() ? "" : r.sub + " · ") + std::to_string(all) + (all == 1 ? " track" : " tracks");
        g.text(sub, { css::Sm }, lx + 48, y + 26, pal.text3, 1, Align::Left, int(lw - 48 - (narrow ? 72 : 140)));
        if (on && r.one < 0) {
            char b[32];
            snprintf(b, sizeof b, "%d of %d", on, all);
            g.textC(b, { css::Sm, Weight::SemiBold }, lx + lw - 16, y + (RH - 4) / 2, pal.accentInk, 1, Align::Right);
        }
    }
    if (!n) g.textC("Nothing here yet", { css::Md, Weight::SemiBold }, lx + lw / 2, ly + 60, pal.text2, 1, Align::Center);
    g.unclip();
    ui.popArea();
}
