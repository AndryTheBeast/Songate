/*
 * Gatefold NX · view_sheet.cpp
 * The track menu: + on any track (or ⋯ on Now playing) opens a sheet over the
 * view with what can be done to that track — queue it, like it, edit its tags,
 * give it a cover from the SD card, jump to its album, artist, genre or folder,
 * see its details, or delete the file.
 *
 * Edits and covers are kept by Gatefold (sdmc:/config/songate-nx/edits.tsv,
 * covers.tsv, covers/) and layered over the file's own tags everywhere in the
 * app: the console's FFmpeg can read but not write files, and the music stays
 * untouched — Gatefold 1.4's default for covers ("only in Gatefold") as well.
 */
#include "words.hpp"
#include <SDL2/SDL_image.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <strings.h>

#include "app.hpp"
#include "coverio.hpp"
#include "icons.inc"

namespace {

const std::pair<const char*, const char*> kFields[] = { { "title", "Title" },       { "artist", "Artist" }, { "album", "Album" },
                                                        { "albumArtist", "Album artist" }, { "genre", "Genre" }, { "date", "Year" },
                                                        { "trackNo", "Track number" } };

std::string fieldOf(const Track& t, const std::string& k) {
    if (k == "title") return t.title;
    if (k == "artist") return t.artist;
    if (k == "album") return t.album;
    if (k == "albumArtist") return t.albumArtist;
    if (k == "genre") return t.genre;
    if (k == "date") return t.date;
    if (k == "trackNo") return t.trackNo ? std::to_string(t.trackNo) : std::string();
    return {};
}

std::string joinPath(const std::string& dir, const std::string& name) { return plat::join(dir, name); }

bool isImage(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string e = name.substr(dot + 1);
    return !strcasecmp(e.c_str(), "jpg") || !strcasecmp(e.c_str(), "jpeg") || !strcasecmp(e.c_str(), "png") || !strcasecmp(e.c_str(), "webp") ||
           !strcasecmp(e.c_str(), "bmp");
}

std::vector<uint8_t> readAll(const std::string& path) {
    std::vector<uint8_t> out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
        fclose(f);
    }
    return out;
}

std::string sizeText(double bytes) {
    char b[32];
    if (bytes >= 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", bytes / (1024 * 1024));
    else snprintf(b, sizeof b, "%.0f KB", bytes / 1024);
    return b;
}

}  // namespace

// ------------------------------------------------------------------ edits

void App::applyEdits(Track& t) {
    auto it = store.edits.find(t.path);
    if (it == store.edits.end()) return;
    for (const auto& [k, v] : it->second) {
        if (k == "title") { t.title = v; t.fromName &= ~1; }
        else if (k == "artist") { t.artist = v; t.fromName &= ~2; }
        else if (k == "album") { t.album = v; t.fromName &= ~4; }
        else if (k == "albumArtist") t.albumArtist = v;
        else if (k == "genre") t.genre = v;
        else if (k == "date") t.date = v;
        else if (k == "trackNo") t.trackNo = atoi(v.c_str());
    }
}

bool App::keyboard(const char* header, const std::string& initial, std::string& out) {
    char buf[512] = {};
    const plat::TextResult r = plat::askText(header, header, initial.c_str(), buf, sizeof buf);
    if (r == plat::TextResult::Unavailable) toast("The keyboard is not available in applet mode");
    if (r != plat::TextResult::Done) return false;   // cancelled: keep what was there
    out = buf;
    return true;
}

void App::editTag(const char* field, const char* label) {
    std::string v;
    if (keyboard(label, sheet.draft[field], v)) sheet.draft[field] = v;
}

void App::saveTagEdits() {
    Track& t = lib.tracks[sheet.track];
    auto& e = store.edits[t.path];
    for (const auto& [k, v] : sheet.draft)
        if (v != fieldOf(t, k)) e[k] = v;
    if (e.empty()) store.edits.erase(t.path);
    store.saveEdits();
    applyEdits(t);
    rebuildIndex();
    if (now.track == sheet.track) {
        now.info.title = t.title; now.info.artist = t.artist; now.info.album = t.album;
        now.info.albumArtist = t.albumArtist; now.info.genre = t.genre; now.info.date = t.date; now.info.trackNo = t.trackNo;
    }
    toast("Tags saved");
    // into the file too: every change of Songate's to this song (kept in Songate as well: it stays the same on your other devices)
    if (auto it = store.edits.find(t.path); it != store.edits.end() && !it->second.empty()) {
        TagJobs::Job j;
        j.path = t.path;
        for (const auto& [k, v] : it->second) j.changes.text[k] = v;
        tagWriteAsk({ j }, "“" + t.title + "”");
    }
}

void App::undoTagEdits(int track) {
    store.edits.erase(lib.tracks[track].path);
    store.saveEdits();
    meta.retag(track);   // the file's own tags come back as it is read again
    toast("Back to the tags in the file");
}

// ------------------------------------------------------------------ covers

void App::listCoverDir(const std::string& dir) {
    sheet.dir = dir;
    sheet.entries.clear();
    sheet.memo.clear();
    for (const auto& d : listDirs(dir)) sheet.entries.push_back(d + "/");
    std::vector<std::string> images;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.' && isImage(e->d_name)) images.push_back(e->d_name);
        closedir(d);
    }
    std::sort(images.begin(), images.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    sheet.entries.insert(sheet.entries.end(), images.begin(), images.end());
    area("sheet", int(Sheet::Cover)).target = area("sheet", int(Sheet::Cover)).y = 0;
    ui.focusDefault();
}

void App::openTagEditor(int t) {
    if (t < 0 || t >= int(lib.tracks.size())) return;
    sheet.draft.clear();
    for (const auto& [k, l] : kFields) sheet.draft[k] = fieldOf(lib.tracks[size_t(t)], k);
    sheet.page = Sheet::Tags;
    ui.focusDefault();
}

void App::setTrackCover(const std::string& image, const std::vector<int>* forTracks) {
    const std::vector<uint8_t> bytes = readAll(image);
    if (bytes.empty()) { toast("Couldn't read that image"); return; }
    // Copied into Gatefold's folder, so the cover stays when the image is moved; named by its content.
    uint32_t h = 2166136261u;
    for (uint8_t b : bytes) h = (h ^ b) * 16777619u;
    char name[32];
    snprintf(name, sizeof name, "%08x", h);
    const std::string dir = store.path("covers");
    mkdir(dir.c_str(), 0777);
    const std::string dest = dir + "/" + name + image.substr(image.find_last_of('.'));
    if (FILE* f = fopen(dest.c_str(), "wb")) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }

    const Track& t = lib.tracks[sheet.track];
    std::vector<int> targets{ sheet.track };
    if (sheet.wholeAlbum && t.albumIdx >= 0) targets = lib.albums[t.albumIdx].tracks;
    if (forTracks && !forTracks->empty()) targets = *forTracks;
    for (int i : targets) {
        store.covers[lib.tracks[i].path] = dest;
        meta.setCoverFile(i, dest);
    }
    store.saveEdits();
    if (std::find(targets.begin(), targets.end(), now.track) != targets.end()) loadTrackCoverAgain();   // now yours (store.covers)
    toast(targets.size() > 1 ? "Cover set for the whole album" : "Cover set");
    // into the files too (a JPEG or a PNG: what every reader takes)
    const bool jpegOrPng = bytes.size() > 8 && ((bytes[0] == 0xFF && bytes[1] == 0xD8) || (bytes[0] == 0x89 && bytes[1] == 'P'));
    if (!jpegOrPng) return;
    std::vector<TagJobs::Job> jobs;
    for (int i : targets) {
        TagJobs::Job j;
        j.path = lib.tracks[i].path;
        j.changes.cover = true;
        j.changes.coverBytes = bytes;
        jobs.push_back(std::move(j));
    }
    tagWriteAsk(std::move(jobs), targets.size() > 1 ? "the album's cover" : "the cover");
}

// ------------------------------------------------------------------ tags and covers into the files

void App::tagWriteAsk(std::vector<TagJobs::Job> jobs, const std::string& what) {
    if (jobs.empty() || store.tagWrite == 1) return;   // Songate only
    if (store.tagWrite == 2) { tagWriteQueue(std::move(jobs)); return; }
    const size_t n = jobs.size();
    confirm(ICON_TAG, "Tags", "Where should " + what + " go?", n > 1 ? std::to_string(n) + " songs" : std::string("This song"),
            "Into the file too?",
            "Songate has it now. In the file too, every app and device reading it sees the change; Songate keeps what the file held, so "
            "\"Restore the file's original tags\" puts it back. Settings › Library can decide for you.",
            "Into the file", [this, jobs] { tagWriteQueue(jobs); });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
    sheet.askAlt = "Songate only";
    sheet.askAltYes = [] {};
}

void App::tagWriteQueue(std::vector<TagJobs::Job> jobs) {
    if (tagPending_.empty() && tagJobs.idle()) tagDone_ = tagTotal_ = 0;   // (a new run of writes)
    for (auto& j : jobs) {
        // the same song waiting already: this one replaces it (the latest change wins, a write each)
        for (auto it = tagPending_.begin(); it != tagPending_.end(); ++it)
            if (it->path == j.path && it->undo == j.undo) {
                for (const auto& [k, v] : it->changes.text) if (!j.changes.text.count(k)) j.changes.text[k] = v;
                if (!j.changes.cover && it->changes.cover) { j.changes.cover = true; j.changes.coverBytes = it->changes.coverBytes; }
                tagPending_.erase(it);
                tagTotal_--;
                break;
            }
        tagPending_.push_back(std::move(j));
        tagTotal_++;
    }
}

/** Every frame: the next song handed over when nothing is being written and it is not the one playing; what came back applied. */
void App::tagWritesTick() {
#ifdef GF_PREVIEW
    // (the preview: GF_TAGWRITE_TEST=<cover image>: the first song's title and cover into its file, then its originals back; in the log)
    if (static int step = 0; getenv("GF_TAGWRITE_TEST") && !lib.tracks.empty() && time > 4 && step < 3) {
        if (step == 0) {
            TagJobs::Job j;
            j.path = lib.tracks[0].path;
            j.changes.text["title"] = "Written by Songate";
            j.changes.cover = true;
            j.changes.coverBytes = readAll(getenv("GF_TAGWRITE_TEST"));
            logLine("Tag test: writing into " + j.path);
            tagWriteQueue({ j });
            step = 1;
        } else if (step == 1 && tagPending_.empty() && tagJobs.idle() && tagUndo_.count(lib.tracks[0].path)) {
            const auto& u = tagUndo_[lib.tracks[0].path];
            logLine("Tag test: written; kept to undo: title \"" + u.text.at("title") + "\", cover " + (u.coverFile.empty() ? std::string("none") : u.coverFile));
            restoreFileTags(0);
            step = 2;
        } else if (step == 2 && tagPending_.empty() && tagJobs.idle() && !tagUndo_.count(lib.tracks[0].path)) {
            logLine("Tag test: restored; undo records left " + std::to_string(tagUndo_.size()));
            step = 3;
        }
    }
#endif
    if (!tagPending_.empty() && !store.stopTagWrites && tagJobs.idle()) {
        const std::string playing = playingFile();
        for (auto it = tagPending_.begin(); it != tagPending_.end(); ++it) {
            if (it->path == playing) continue;   // (held open: written once it is not playing)
            tagJobs.start();
            tagJobs.put(std::move(*it));
            tagPending_.erase(it);
            break;
        }
    }
    TagJobs::Done d;
    while (tagJobs.take(d)) {
        tagDone_++;
        const auto ti = pathIndex.find(d.job.path);
        const std::string title = ti != pathIndex.end() ? lib.tracks[size_t(ti->second)].title : baseName(d.job.path);
        if (!d.ok) {
            logLine("Tags not written into " + d.job.path + ": " + d.err);
            toast("Couldn't write into “" + title + "”: " + d.err + ". Kept in Songate");
            continue;
        }
        if (!d.job.undo) {   // what the file held, the first write's (a later write does not move the original)
            TagUndo& u = tagUndo_[d.job.path];
            for (const auto& [k, v] : d.before.text) if (!u.text.count(k)) u.text[k] = v;
            if (d.before.cover && !u.cover) {
                u.cover = true;
                if (!d.before.coverBytes.empty()) {
                    uint32_t h = 2166136261u;
                    for (uint8_t b : d.before.coverBytes) h = (h ^ b) * 16777619u;
                    char name[40];
                    snprintf(name, sizeof name, "%08x.%s", h, d.before.coverBytes[0] == 0x89 ? "png" : "jpg");
                    const std::string dir = store.path("tag-undo");
                    mkdir(dir.c_str(), 0777);
                    u.coverFile = dir + "/" + name;
                    store.writeFile((std::string("tag-undo/") + name).c_str(), std::string(d.before.coverBytes.begin(), d.before.coverBytes.end()));
                }
            }
        } else {   // the originals are back: Songate shows the file as it is (its own changes to these go too)
            if (auto u = tagUndo_.find(d.job.path); u != tagUndo_.end()) {
                if (d.job.changes.cover && !u->second.coverFile.empty()) store.removeFile(u->second.coverFile.substr(store.path("").size()).c_str());
                tagUndo_.erase(u);
            }
            if (auto e = store.edits.find(d.job.path); e != store.edits.end()) {
                for (const auto& [k, v] : d.job.changes.text) { (void)v; e->second.erase(k); }
                if (e->second.empty()) store.edits.erase(e);
            }
            if (d.job.changes.cover) store.covers.erase(d.job.path);
            store.saveEdits();
        }
        saveTagUndo();
        if (ti != pathIndex.end()) {   // read again: the file's tags, its cover
            meta.retag(ti->second);
            if (d.job.changes.cover) {
                auto c = store.covers.find(d.job.path);
                meta.setCoverFile(ti->second, c != store.covers.end() ? c->second : std::string());
            }
            if (now.track == ti->second && d.job.changes.cover) loadTrackCoverAgain();
        }
        if (tagPending_.empty() && tagJobs.idle())
            toast(d.job.undo ? "The file's original tags are back in “" + title + "”"
                             : tagTotal_ > 1 ? "Written into " + std::to_string(tagTotal_) + " files" : "Written into “" + title + "”");
    }
}

void App::restoreFileTags(int track) {
    const std::string& path = lib.tracks[size_t(track)].path;
    auto u = tagUndo_.find(path);
    if (u == tagUndo_.end()) return;
    TagJobs::Job j;
    j.path = path;
    j.undo = true;
    j.changes.text = u->second.text;
    if (u->second.cover) {
        j.changes.cover = true;
        if (!u->second.coverFile.empty()) j.changes.coverBytes = readAll(u->second.coverFile);
    }
    tagWriteQueue({ j });
    toast(now.track == track ? "Its original tags go back once it is not playing" : "Putting the file's original tags back");
}

void App::loadTagUndo() {
    tagUndo_.clear();
    FILE* f = fopen(store.path("tag-undo.tsv").c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[8192];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    auto unescape = [](const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\\' && i + 1 < s.size()) { const char c = s[++i]; o += c == 'n' ? '\n' : c == 't' ? '\t' : c; }
            else o += s[i];
        }
        return o;
    };
    for (size_t a = 0; a < text.size();) {   // path TAB field TAB value (a cover: field "#cover", value its file, "" none)
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        const std::string l = text.substr(a, b - a);
        a = b + 1;
        const size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? std::string::npos : l.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        TagUndo& u = tagUndo_[unescape(l.substr(0, t1))];
        const std::string field = l.substr(t1 + 1, t2 - t1 - 1), value = unescape(l.substr(t2 + 1));
        if (field == "#cover") { u.cover = true; u.coverFile = value; }
        else u.text[field] = value;
    }
}

void App::saveTagUndo() {
    auto escape = [](const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\t') o += "\\t"; else if (c != '\r') o += c; }
        return o;
    };
    std::string out;
    for (const auto& [path, u] : tagUndo_) {
        for (const auto& [k, v] : u.text) out += escape(path) + "\t" + k + "\t" + escape(v) + "\n";
        if (u.cover) out += escape(path) + "\t#cover\t" + escape(u.coverFile) + "\n";
    }
    if (out.empty()) store.removeFile("tag-undo.tsv");
    else store.writeFile("tag-undo.tsv", std::move(out));
}

long long App::tagUndoBytes() const {
    long long n = 0;
    struct stat st;
    for (const auto& [p, u] : tagUndo_) if (!u.coverFile.empty() && stat(u.coverFile.c_str(), &st) == 0) n += st.st_size;
    return n;
}

// ------------------------------------------------------------------ delete

bool App::deleteSong(int track) {
    const Track t = lib.tracks[track];
    if (now.track == track) {
        if (q.order.size() > 1) next(false);   // move on first: the file is open while it plays
        else player.stop();
    }
    if (now.track == track) player.stop();
    if (remove(t.path.c_str()) != 0) return false;
    std::erase(store.liked, t.path);
    stats.forget(t.path);
    store.edits.erase(t.path);
    store.covers.erase(t.path);
    return true;
}

void App::deleteTrack(int track) {
    const Track t = lib.tracks[track];
    if (!deleteSong(track)) {
        toast("Couldn't delete “" + baseName(t.path) + "”");
        return;
    }
    syncNoteDeleted(t.path);   // only once it really is gone
    store.saveEdits();
    store.save();
    closeSheet();
    rescan();
    toast("Deleted “" + t.title + "”");
}

// ------------------------------------------------------------------ the sheet

void App::openTrackMenu(int track, int queuePos, bool upNext) {
    if (track < 0 || track >= int(lib.tracks.size())) return;
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Menu;
    sheet.track = track;
    sheet.queuePos = queuePos;
    sheet.upNext = upNext && queuePos >= 0;
    sheet.returnFocus = ui.focusedId();
    area("sheet", int(Sheet::Menu)).target = area("sheet", int(Sheet::Menu)).y = 0;
    ui.focusDefault();
}

void App::closeSheet() {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet.preview = nullptr;
    sheet.page = Sheet::Closed;
    ui.setModal(false);
    if (sheet.returnFocus) ui.focus(sheet.returnFocus);
}

void App::sheetBack() {
    if (sheet.page == Sheet::Colour) {   // B puts down a square or bar first (Ui); then it is Cancel
        closeColourPicker(false);
        return;
    }
    if (sheet.page == Sheet::Job && sheet.fromList) {   // back to the list, on the job it came from
        int row = 0;
        for (int i = 0; i < sheet.recipe && i < JobSlots; i++) row += jobCards[i].shown > 0.01f;
        openJobList();
        sheet.queuePos = row;   // drawSmartSheet focuses it
        return;
    }
    if (sheet.page == Sheet::Menu || sheet.page == Sheet::NewList || sheet.page == Sheet::Sleep || sheet.page == Sheet::Devices || sheet.page == Sheet::AirPlay || sheet.page == Sheet::Job || sheet.page == Sheet::Jobs ||
        sheet.page == Sheet::PlFile || sheet.page == Sheet::Confirm || sheet.page == Sheet::EqPreset ||
        sheet.page == Sheet::ActList || sheet.page == Sheet::ListActions || sheet.page == Sheet::PlMenu || sheet.page == Sheet::FileMenu ||
        sheet.page == Sheet::Factory || sheet.page == Sheet::Report || sheet.page == Sheet::Crashes || sheet.page == Sheet::Battery || sheet.page == Sheet::SrvMenu || sheet.page == Sheet::SrvPlMenu) {
        closeSheet();
        return;
    }
    if (sheet.page == Sheet::SrvPlIcon) {   // the icons → the server playlist's menu
        sheet.page = Sheet::SrvPlMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::SrvLists) {   // Add to a playlist… → the server track's menu
        sheet.page = Sheet::SrvMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::FileInfo || sheet.page == Sheet::FileLink) {   // Files: back to the file's menu
        sheet.page = Sheet::FileMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::PlIcon && !sheet.fromList) {   // the icons → the playlist's menu (opened from the page's icon button: closes)
        sheet.page = Sheet::PlMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::Smart || sheet.page == Sheet::SmartForm) {   // the recipe → the recipes → the choice
        sheet.page = sheet.page == Sheet::SmartForm ? Sheet::Smart : Sheet::NewList;
        ui.focusDefault();
        return;
    }
    sheet.page = Sheet::Menu;   // every page goes back to the menu ("Up one folder" climbs in the picker)
    ui.focusDefault();
}

void App::drawSheet() {
    if (sheet.page == Sheet::Closed) return;
    if (sheet.page >= Sheet::NewList) { drawSmartSheet(); return; }   // no track: view_smart.cpp
    if (sheet.track < 0 || sheet.track >= int(lib.tracks.size())) { closeSheet(); return; }
    ui.region(Region::Modal);
    const int t = sheet.track;
    const Track& tr = lib.tracks[t];
    const Sheet::Page page = sheet.page;

    // backdrop and card
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.55f);
    const float w = cardW(600), x = (css::W - w) / 2;
    const bool footer = page == Sheet::Tags || page == Sheet::Delete;
    const float headH = 24 + 64 + 18, footH = footer ? 64 : 0, maxBody = css::H - 60 - headH - footH - 16;

    // ---- the rows of this page, laid out first so the card can be sized to them
    struct Item {
        const char* icon;
        std::string label, detail;
        std::function<void()> act;
        bool danger = false;
        float h = 46;
        bool text = false;
    };
    std::vector<Item> items;
    const bool liked = store.isLiked(tr.path);
    // Opened on a row of the queue (the Playlist view, Up next): still that row, unless the queue changed meanwhile.
    const int qp = sheet.queuePos >= 0 && sheet.queuePos < int(q.order.size()) && q.tracks[q.order[sheet.queuePos]] == t ? sheet.queuePos : -1;
    if (page == Sheet::Menu && qp >= 0) {
        if (qp != q.pos)
            items.push_back({ ICON_NEXT, "Play next", "", [this, qp] {
                                  queuePlayNext(qp);
                                  toast("Plays next");
                                  closeSheet();
                              } });
        // In Up next, a row wrapped round from the start (Repeat) cannot move: it lies before the track playing.
        if (q.order.size() > 1 && (!sheet.upNext || qp > q.pos))
            items.push_back({ ICON_SORT_DOWN, "Move", "", [this, qp, upNext = sheet.upNext] {
                                  closeSheet();
                                  grab = Grab{ Grab::Queue, qp, upNext, 1 };
                                  ui.focus(upNext ? uid("np.next", qp - q.pos - 1) : uid("pl.r", qp));
                                  ui.setPadMode(true);
                                  toast(GF_MOVE_WORDS);
                              } });
        if (qp != q.pos)
            items.push_back({ ICON_X, "Remove from the playlist", "", [this, qp] {
                                  queueRemove(qp);
                                  toast("Removed from the playlist");
                                  closeSheet();
                              } });
    } else if (page == Sheet::Menu) {
        items.push_back({ ICON_NEXT, "Play next", "", [this, t] {
                              queueAdd(t, true);
                              toast("Plays next");
                              closeSheet();
                          } });
        items.push_back({ ICON_QUEUE, "Add to the end of the playlist", "", [this, t] {
                              queueAdd(t, false);
                              toast("Added to the playlist");
                              closeSheet();
                          } });
    }
    if (page == Sheet::Menu) {
        items.push_back({ ICON_LIST, "Add to a playlist…", "", [this] { sheet.page = Sheet::Lists; ui.focusDefault(); } });
        // On one of your own playlists, the track can leave it.
        if (route.view == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && store.playlists[route.ref].own()) {
            auto& pl = store.playlists[route.ref];
            // Move: picked up as the queue's rows are (an Automix playlist then keeps this order until Order again)
            {
                const std::vector<int> rows = playlistTracks(route.ref);
                const int row = int(std::find(rows.begin(), rows.end(), t) - rows.begin());
                if (row < int(rows.size()) && rows.size() > 1)
                    items.push_back({ ICON_SORT_DOWN, "Move", pl.automix ? "Your order stays" : "", [this, row, ref = route.ref] {
                                          closeSheet();
                                          grab = Grab{ Grab::List, row, false, 1, ref };
                                          ui.focus(uid("pl.r", row));
                                          ui.setPadMode(true);
                                          toast(GF_MOVE_WORDS);
                                      } });
            }
            if (pl.automix) {   // an Automix playlist: this track first (pinned), or Gatefold's choice again
                const bool pinned = pl.pin == tr.path;
                items.push_back({ ICON_LINK, pinned ? "Let Songate choose the first track" : "Play this first", "", [this, ref = route.ref, pinned, path = tr.path] {
                                      store.playlists[ref].pin = pinned ? "" : path;
                                      orderAutomixList(ref, false);
                                      toast(pinned ? "Songate chooses the first track again" : "Plays first; the rest ordered after it");
                                      closeSheet();
                                  } });
            }
            if (std::find(pl.tracks.begin(), pl.tracks.end(), tr.path) != pl.tracks.end())
                items.push_back({ ICON_X, "Remove from “" + pl.name + "”", "", [this, ref = route.ref, path = tr.path] {
                                      auto& tl = store.playlists[ref].tracks;
                                      if (auto it = std::find(tl.begin(), tl.end(), path); it != tl.end()) tl.erase(it);
                                      store.save();
                                      toast("Removed from “" + store.playlists[ref].name + "”");
                                      closeSheet();
                                  } });
        }
        items.push_back({ liked ? ICON_HEART_F : ICON_HEART, liked ? "Remove from Liked" : "Add to Liked", "", [this, liked, path = tr.path] {
                              store.toggleLiked(path);
                              store.save();
                              toast(liked ? "Removed from Liked" : "Added to Liked");
                          } });
        {   // its mood (BETA 3: yours to correct; the user, 2026-11-05: "Any song's menu")
            const int m = size_t(t) < moodIdx_.best.size() ? moodIdx_.best[size_t(t)] : -1;
            const auto lb = moodLabels_.find(tr.path);
            const bool yours = lb != moodLabels_.end() && lb->second.is >= 0;
            // one row; its page holds the moods, Not this one, and back to its sound (the user, 2026-11-05: "A, one Mood row")
            items.push_back({ moodIcon(m), "Mood", m < 0 ? std::string("Not measured yet") : std::string(mood::name(m)) + (yours ? " · yours" : ""),
                              [this] { sheet.page = Sheet::Moods; ui.focusDefault(); } });
        }
        items.push_back({ ICON_TAG, "Edit tags…", store.edits.count(tr.path) ? "Edited" : "", [this, t] { openTagEditor(t); } });
        items.push_back({ ICON_IMAGE, "Change the cover…", store.covers.count(tr.path) ? "Yours" : "", [this, t] {
                              sheet.page = Sheet::Cover;
                              sheet.wholeAlbum = lib.tracks[t].albumIdx >= 0;
                              // Settings → Storage and reset may name a folder of covers; else the track's own
                              struct stat st;
                              const bool own = !store.coverDir.empty() && stat(store.coverDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
                              listCoverDir(own ? store.coverDir : lib.tracks[t].folder);
                          } });
        if (store.covers.count(tr.path))
            items.push_back({ ICON_REFRESH, "Use the original cover", "", [this, t] {
                                  // for every track of the album that shares this cover
                                  const std::string mine = store.covers[lib.tracks[t].path];
                                  std::vector<int> group{ t };
                                  if (lib.tracks[t].albumIdx >= 0) group = lib.albums[lib.tracks[t].albumIdx].tracks;
                                  for (int i : group)
                                      if (auto it = store.covers.find(lib.tracks[i].path); it != store.covers.end() && it->second == mine) {
                                          store.covers.erase(it);
                                          meta.setCoverFile(i, "");
                                      }
                                  store.saveEdits();
                                  if (now.track >= 0 && std::find(group.begin(), group.end(), now.track) != group.end()) loadTrackCoverAgain();
                                  toast("Original cover back");
                              } });
        if (store.edits.count(tr.path)) items.push_back({ ICON_REFRESH, "Undo my tag edits", "", [this, t] { undoTagEdits(t); } });
        if (tagUndo_.count(tr.path))   // written into the file: what it held, back in it
            items.push_back({ ICON_REFRESH, "Restore the file's original tags", "", [this, t] { restoreFileTags(t); closeSheet(); } });
        if (tr.albumIdx >= 0)
            items.push_back({ ICON_GRID, "Go to the album", lib.albums[tr.albumIdx].title, [this, a = tr.albumIdx] { closeSheet(); goToAlbum(a); } });
        items.push_back({ ICON_MIC, "Go to the artist", tr.artist, [this, a = tr.artist] { closeSheet(); openArtist(a); } });
        if (!tr.genre.empty()) {
            const std::string g1 = tr.genre.substr(0, tr.genre.find(';'));
            items.push_back({ ICON_TAG, "Go to the genre", g1, [this, g1] { closeSheet(); openGenre(g1); } });
        }
        items.push_back({ ICON_FOLDER, "Show in Folders", "", [this, f = tr.folder] { closeSheet(); if (tabFor(Store::TabFolders, "folder")) navigate(View::Folders, lib.folderByPath(f)); } });
        items.push_back({ ICON_INFO, "Details", "", [this, t] {
                              sheet.info = TrackInfo{};
                              readTrackInfo(lib.tracks[t].path, sheet.info, true, true);
                              sheet.page = Sheet::Info;
                              ui.focusDefault();
                          } });
        items.push_back({ ICON_X, "Delete from " GF_THE_CARD "…", "", [this] { sheet.page = Sheet::Delete; ui.focusDefault(); }, true });
    } else if (page == Sheet::Lists) {
        // Your own playlists (folder playlists follow their folder, so tracks cannot be added to them).
        items.push_back({ ICON_PLUS, "New playlist…", "", [this, t] { closeSheet(); newPlaylist(t); } });
        for (int i = 0; i < int(store.playlists.size()); i++) {
            const FolderPlaylist& p = store.playlists[i];
            if (!p.own()) continue;
            const bool in = std::find(p.tracks.begin(), p.tracks.end(), tr.path) != p.tracks.end();
            items.push_back({ ICON_LIST, p.name, in ? "Already in it" : std::to_string(p.tracks.size()) + " tracks", [this, i, t] {
                                  addToPlaylist(i, t);
                                  closeSheet();
                              } });
        }
    } else if (page == Sheet::Moods) {
        const int m = size_t(t) < moodIdx_.best.size() ? moodIdx_.best[size_t(t)] : -1;
        const int bySound = size_t(t) < moodIdx_.sound.size() ? moodIdx_.sound[size_t(t)] : -1;
        mood::Label own = moodLabels_.count(tr.path) ? moodLabels_[tr.path] : mood::Label{};
        for (int c = 0; c < mood::Count; c++) {
            items.push_back({ moodIcon(c), mood::name(c), c == m ? std::string("Now") : (own.nots & (1u << c)) ? std::string("You said not") : std::string(mood::desc(c)),
                              [this, t, c, own] {
                                  mood::Label l = own;
                                  l.is = static_cast<signed char>(c);
                                  l.nots = uint16_t(l.nots & ~(1u << c));
                                  setMoodLabel(t, l);
                                  toast(std::string("In ") + mood::name(c) + " from now on: songs like it follow");
                                  closeSheet();
                              } });
        }
        if (own.any())
            items.push_back({ ICON_REFRESH, "As its sound says", bySound >= 0 ? std::string(mood::name(bySound)) : std::string(), [this, t] {
                                  setMoodLabel(t, mood::Label{});
                                  toast("Your choice for it cleared");
                                  closeSheet();
                              } });
    } else if (page == Sheet::Tags) {
        for (const auto& [k, l] : kFields)
            items.push_back({ nullptr, l, sheet.draft[k], [this, k = k, l = l] { editTag(k, l); } });
    } else if (page == Sheet::Cover) {
        if (!plat::isRoot(sheet.dir))
            items.push_back({ ICON_BACK, "Up one folder", "", [this] { listCoverDir(plat::parentDir(sheet.dir)); } });
        for (const auto& e : sheet.entries) {
            if (e.back() == '/') {
                const std::string name = e.substr(0, e.size() - 1);
                items.push_back({ ICON_FOLDER, name, "", [this, name] { listCoverDir(joinPath(sheet.dir, name)); } });
            } else {
                const std::string full = joinPath(sheet.dir, e);
                auto m = sheet.memo.find(full);
                if (m == sheet.memo.end()) {
                    struct stat st {};
                    m = sheet.memo.emplace(full, stat(full.c_str(), &st) == 0 ? sizeText(double(st.st_size)) : "").first;
                }
                const std::string size = m->second;
                items.push_back({ ICON_IMAGE, e, size, [this, full] { setTrackCover(full); if (sheet.page != Sheet::Confirm) { sheet.page = Sheet::Menu; ui.focusDefault(); } } });
            }
        }
        if (sheet.entries.empty()) items.push_back({ nullptr, "No images or folders here", "", nullptr, false, 46, true });
    } else if (page == Sheet::Info) {
        const TrackInfo& in = sheet.info;
        auto info = [&](const char* label, const std::string& v) {
            if (!v.empty()) items.push_back({ nullptr, label, v, [] {}, false, 40, true });
        };
        info("Title", tr.title);
        info("Artist", tr.artist);
        info("Album", tr.album);
        info("Album artist", tr.albumArtist);
        info("Genre", tr.genre);
        info("Year", tr.date);
        info("Track", tr.trackNo ? std::to_string(tr.trackNo) : "");
        std::string fmt = in.codec;
        if (in.sampleRate) fmt += " · " + std::to_string(in.sampleRate / 1000) + "." + std::to_string(in.sampleRate % 1000 / 100) + " kHz";
        if (in.bitsPerSample && in.codec != "MP3") fmt += " · " + std::to_string(in.bitsPerSample) + "-bit";
        if (in.bitrateKbps) fmt += " · " + std::to_string(in.bitrateKbps) + " kbps";
        info("Format", fmt);
        info("Length", fmtTime(tr.duration > 0 ? tr.duration : in.duration));
        if (!sheet.memo.count("size")) {   // once, not every frame
            struct stat st {};
            sheet.memo["size"] = stat(tr.path.c_str(), &st) == 0 ? sizeText(double(st.st_size)) : "";
            sheet.memo["lrc"] = readSidecarLyrics(tr.path).empty() ? "" : "1";
        }
        info("Size", sheet.memo["size"]);
        info("Cover", store.covers.count(tr.path) ? "Yours, kept by Songate" : !in.cover.empty() ? "In the file" : "From the folder, or none");
        info("Lyrics", !sheet.memo["lrc"].empty() ? ".lrc file" : !in.lyrics.empty() ? "In the tags" : "None");
        if (const auto it = store.lyricNudge.find(tr.path); it != store.lyricNudge.end()) {   // the timing control (Now playing, the Canvas)
            char b[48];
            snprintf(b, sizeof b, "%.1f s %s, kept by Songate", double(std::fabs(it->second)), it->second > 0 ? "later" : "earlier");
            info("Lyrics timing", b);
        }
        info("Your edits", store.edits.count(tr.path) ? "Yes, kept by Songate; the file is unchanged" : "");
        // Volume levelling (levels.hpp)
        if (const Level* lv = levels.find(tr.path); lv && !std::isnan(lv->lufs)) {
            char b[64];
            snprintf(b, sizeof b, "%.1f LUFS, peak %.1f dBTP", double(lv->lufs), 20 * std::log10(std::max(1e-6, double(lv->peak))));
            std::string v = b;
            if (store.levelMode) {
                snprintf(b, sizeof b, " · %+.1f dB here", levelDb(t));
                v += b;
            }
            info("Loudness", v);
        } else {
            info("Loudness", lv ? "Could not be measured" : "Not measured yet");
        }
        // Automix's analysis (mixan.hpp), made with the loudness
        if (const Level* lv = levels.find(tr.path); lv && lv->mix.seconds != 0) {
            char b[64];
            if (lv->mix.bpm > 0 && lv->mix.confidence >= 0.5f) snprintf(b, sizeof b, "%.0f", double(lv->mix.bpm));
            else if (lv->mix.bpm > 0) snprintf(b, sizeof b, "About %.0f (no steady beat for Automix)", double(lv->mix.bpm));
            else snprintf(b, sizeof b, "No steady beat");
            info("BPM", b);
            if (lv->mix.key >= 0)   // the key, and its place on the Camelot wheel (Automix v2)
                info("Key", (lv->mix.keySure >= 0.5f ? "" : "Perhaps ") + mixan::keyName(lv->mix.key) + " (" + mixan::camelot(lv->mix.key) + ")");
        }
        // Play statistics (stats.hpp)
        auto when = [](int64_t t) {
            char buf[32];
            const std::time_t tt = std::time_t(t);
            std::strftime(buf, sizeof buf, "%d %b %Y", std::localtime(&tt));
            return std::string(buf);
        };
        const PlayStat* ps = stats.find(tr.path);
        info("Plays", ps && ps->plays ? std::to_string(ps->plays) : "Not played yet");
        if (ps && ps->last) info("Last played", when(ps->last));
        if (ps && ps->added) info("Added", when(ps->added));
        info("File", tr.path);
    }

    // ---- card
    float bodyH = 8;
    for (const auto& it : items) bodyH += it.h;
    if (page == Sheet::Cover && tr.albumIdx >= 0) bodyH += 22 + 44;   // the folder line and Use for (below)
    if (page == Sheet::Delete) bodyH = 110;
    if (page == Sheet::Tags) bodyH += 34;
    bodyH = std::min(bodyH + 8, maxBody);
    const float h = headH + bodyH + footH + 16, y = cardY(h);
    g.glow(x + w / 2, y + h / 2 + 30, w * 0.75f, { 0, 0, 0 }, 0.5f);
    g.roundRect(x, y, w, h, 20, pal.surface2);
    g.roundRect(x, y, w, h, 20, pal.text, 0.03f);
    g.roundStroke(x, y, w, h, 20, 1, pal.text, 0.12f);

    // ---- header: cover (or the image being considered), kicker, title, artist · album, close
    const float hx = x + 24, hy = y + 24;
    if (page == Sheet::Cover && sheet.preview) g.image(sheet.preview, hx, hy, 64, 64, 10);
    else coverBox(t, hx, hy, 64, 10);
    g.roundStroke(hx, hy, 64, 64, 10, 1, pal.text, 0.09f);
    static const char* kickers[] = { "", "Track", "Edit tags", "Choose a cover", "Delete", "Details", "Add to a playlist", "Mood" };
    kicker(kickers[int(page)], hx + 64 + 16, hy + 8, pal.accentInk);
    const int textW = int(w - 64 - 16 - 24 - 24 - 40);
    // the mood page: Not <its mood>, on the close button's line, to its left (the user, 2026-11-05: no cross in it, and clear of the
    // close button's ring and its tip)
    const int notMood = page == Sheet::Moods && size_t(t) < moodIdx_.best.size() ? moodIdx_.best[size_t(t)] : -1;
    const std::string notLabel = notMood >= 0 ? std::string("Not ") + mood::name(notMood) : std::string();
    const float notW = notMood >= 0 ? 16 + g.measure(notLabel, { css::Sm, Weight::SemiBold }) + 16 : 0;
    const int titleW = notMood >= 0 ? int(textW + 40 - notW - 28 - 16) : textW;
    g.text(tr.title, { css::Lg, Weight::Bold }, hx + 64 + 16, hy + 18, pal.text, 1, Align::Left, titleW);
    g.text(trackSub(t), { css::Sm }, hx + 64 + 16, hy + 44, pal.text3, 1, Align::Left, textW);
    if (notMood >= 0) {
        bool no = false;
        button(uid("sh.notmood"), nullptr, notLabel, x + w - 24 - 28 - 12 - notW, hy + 14 - 17, false, &no);
        if (no) {
            mood::Label l = moodLabels_.count(tr.path) ? moodLabels_[tr.path] : mood::Label{};
            if (l.is == notMood) l.is = -1;
            l.nots = uint16_t(l.nots | (1u << notMood));
            setMoodLabel(t, l);
            toast(std::string("Not ") + mood::name(notMood) + " from now on: Songate picks the next best");
            closeSheet();
            return;
        }
    }
    hint(uid("sh.close"), "Close");
    if (iconButton(uid("sh.close"), ICON_X, x + w - 24 - 14, hy + 14)) { closeSheet(); return; }
    g.rect(x + 24, y + headH - 1, w - 48, 1, pal.text, 0.09f);

    // ---- body
    const float by = y + headH;
    ScrollArea& sa = area("sheet", int(page));
    sa.view = { x, by, w, bodyH };
    ui.pushArea(&sa);
    g.clip(x, by, w, bodyH);
    float ry = by + 8 - sa.y;
    if (page == Sheet::Cover) {
        // where the browser is, and which tracks get the cover
        g.textC(sheet.dir, { css::Xs }, x + 24, ry + 10, pal.text3, 1, Align::Left, int(w - 48));
        ry += 22;
        if (tr.albumIdx >= 0) {
            int pick = -1;
            segmented(uid("sh.scope"), x + w - 24, ry + 14, { "This track", "The whole album" }, sheet.wholeAlbum ? 1 : 0, &pick);
            g.textC("Use for", { css::Base, Weight::SemiBold }, x + 24, ry + 14, pal.text2);
            if (pick >= 0) sheet.wholeAlbum = pick == 1;
            ry += 44;   // the control is 36 tall round ry + 14: 30 put the first row's focus ring against it
        }
    }
    if (page == Sheet::Delete) {
        g.text("Delete this track from " GF_THE_CARD "?", { css::Lg, Weight::Bold }, x + 24, ry + 8, pal.text, 1, Align::Left, int(w - 48));
        float ty = ry + 40;
        for (const auto& l : g.wrap("The file is removed for good, and it leaves the library, the playlist and Liked. " + tr.path, { css::Sm },
                                    int(w - 48), 3)) {
            g.text(l, { css::Sm }, x + 24, ty, pal.text3);
            ty += 19;
        }
    }
    // Focus starts on the first row, or in the picker on the first image (most folders hold just the one cover).
    size_t focusRow = 0;
    if (page == Sheet::Cover)
        for (size_t i = 0; i < items.size(); i++)
            if (items[i].icon == ICON_IMAGE) { focusRow = i; break; }
    if (page == Sheet::Moods)   // its mood now (the rows are the moods in order)
        if (const int m = size_t(t) < moodIdx_.best.size() ? moodIdx_.best[size_t(t)] : -1; m >= 0 && size_t(m) < items.size()) focusRow = size_t(m);
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i];
        const Id id = uid("sh.row", int64_t(i) + 1000 * int(page));
        const bool hit = it.act && ui.item(id, { x + 12, ry, w - 24, it.h }, 10, i == focusRow ? DefaultFocus : 0);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (hot && !it.text) g.roundRect(x + 12, ry, w - 24, it.h, 10, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
        const Rgb fg = it.danger ? dangerInk() : pal.text;
        float lx = x + 24;
        if (it.icon) { g.icon(it.icon, lx + 9, ry + it.h / 2, 18, it.danger ? fg : pal.text2); lx += 18 + 14; }
        if (page == Sheet::Info) {
            // label column, value wrapped beside it
            g.textC(it.label, { css::Sm }, lx, ry + 20, pal.text3);
            g.textC(it.detail, { css::Base }, lx + 120, ry + 20, pal.text, 1, Align::Left, int(w - 48 - 120));
        } else if (page == Sheet::Tags) {
            g.textC(it.label, { css::Base, Weight::SemiBold }, lx, ry + it.h / 2, pal.text2);
            const std::string v = it.detail.empty() ? "—" : it.detail;
            g.textC(v, { css::Base }, x + w - 24, ry + it.h / 2, it.detail.empty() ? pal.text3 : pal.text, 1, Align::Right, int(w - 48 - 150));
        } else {
            const int dw = it.detail.empty() ? 0 : std::min(200, g.measure(it.detail, { css::Sm }));
            g.textC(it.label, { css::Base, Weight::SemiBold }, lx, ry + it.h / 2, it.text ? pal.text3 : fg, 1, Align::Left,
                    int(x + w - 24 - lx - dw - 16));
            if (dw) g.textC(it.detail, { css::Sm }, x + w - 24, ry + it.h / 2, pal.text3, 1, Align::Right, 200);
        }
        if (hit && it.act) it.act();
        if (page == Sheet::Cover && ui.focused(id) && it.icon == ICON_IMAGE) {
            // the header shows the image under focus, so you see it before choosing
            const std::string full = joinPath(sheet.dir, it.label);
            if (sheet.previewPath != full) {
                sheet.previewPath = full;
                if (sheet.preview) SDL_DestroyTexture(sheet.preview);
                sheet.preview = nullptr;
                const std::vector<uint8_t> bytes = readAll(full);
                if (SDL_Surface* s = loadImage(bytes)) {
                    if (SDL_Surface* small = Gfx::shrink(s, 128)) {
                        sheet.preview = SDL_CreateTextureFromSurface(renderer_, small);
                        if (sheet.preview) SDL_SetTextureScaleMode(sheet.preview, SDL_ScaleModeLinear);
                        SDL_FreeSurface(small);
                    }
                    SDL_FreeSurface(s);
                }
            }
        }
        if (sheet.page != page) break;   // the action moved to another page (or closed): its rows are gone
        ry += it.h;
    }
    if (page == Sheet::Tags && sheet.page == page) {
        g.text("Kept by Songate and shown everywhere in the app. The file itself is not changed.", { css::Sm }, x + 24, ry + 10, pal.text3, 1,
               Align::Left, int(w - 48));
        ry += 34;
    }
    sa.content = ry + sa.y - by + 8;
    g.unclip();
    ui.popArea();
    if (sheet.page != page) return;

    // ---- footer buttons
    if (footer) {
        const float fy = y + h - 16 - 48;
        g.rect(x + 24, fy - 8, w - 48, 1, pal.text, 0.09f);
        bool c = false;
        const Font bf{ css::Sm, Weight::SemiBold };
        if (page == Sheet::Tags) {
            const float sw = g.measure("Save", bf) + 32 + 24;
            const float cw = g.measure("Cancel", bf) + 32;
            button(uid("sh.save"), ICON_CHECK, "Save", x + w - 24 - sw, fy + 7, true, &c);
            if (c) { saveTagEdits(); if (sheet.page != Sheet::Confirm) { sheet.page = Sheet::Menu; ui.focusDefault(); } return; }
            button(uid("sh.cancel"), nullptr, "Cancel", x + w - 24 - sw - 8 - cw, fy + 7, false, &c);
            if (c) { sheet.page = Sheet::Menu; ui.focusDefault(); return; }
        } else {
            // Cancel is where focus lands: a stray A press must not delete anything
            const float dw = g.measure("Delete", bf) + 32 + 24;
            const float cw = g.measure("Cancel", bf) + 32;
            button(uid("sh.del"), ICON_X, "Delete", x + w - 24 - dw, fy + 7, false, &c, false, true);
            if (c) { deleteTrack(t); return; }
            button(uid("sh.keep"), nullptr, "Cancel", x + w - 24 - dw - 8 - cw, fy + 7, false, &c, false, false, DefaultFocus);
            if (c) { sheet.page = Sheet::Menu; ui.focusDefault(); return; }
        }
    }
}
