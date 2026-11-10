/*
 * Gatefold NX · view_storage.cpp
 * Settings → Storage and reset (its rows are in view_settings.cpp): where each browser opens (Files, importing and
 * exporting playlists, the cover picker, the updater's backups), chosen with the music-folder picker; and what
 * Gatefold keeps on the SD card, measured on a worker, each with a Clear button that asks first.
 */
#include "words.hpp"
#include <dirent.h>
#include <sys/stat.h>

#include <chrono>
#include <thread>

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"

std::atomic<bool> gStorageStop{ false };   // Songate closing: the sizes stop where they are (app.hpp)

namespace {

std::string joinP(const std::string& a, const std::string& b) { return plat::join(a, b); }
bool isDirP(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        closedir(d);
    }
    return out;
}

/** Files and bytes below p (p itself when a file); only names `keep` accepts at its top. */
struct Use { int64_t bytes = 0; int files = 0; };

void measure(const std::string& p, Use& u, bool (*keep)(const std::string&) = nullptr) {
    if (gStorageStop) return;   // Songate closing: the sizes are measured again when the page opens
    if (plat::measureStorage(p, u.bytes, u.files, keep)) return;
    // The SD card serves one request at a time: a walk of thousands of files without pauses held up everything Gatefold
    // reads (covers, tags, the settings) and the app stalled. A short pause every 24 files lets the rest through.
    static thread_local int walked = 0;
    if (++walked % 24 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { u.bytes += int64_t(st.st_size); u.files++; return; }
    for (const auto& n : entries(p)) if (!keep || keep(n)) measure(joinP(p, n), u);
}

bool removeAll(const std::string& p) {
    if (!isDirP(p)) return remove(p.c_str()) == 0;
    bool ok = true;
    for (const auto& n : entries(p)) ok = removeAll(joinP(p, n)) && ok;
    return rmdir(p.c_str()) == 0 && ok;
}

bool isDownload(const std::string& n) {   // updates/: the zips (and a cut-off .part), not the release list
    return (n.size() > 4 && n.compare(n.size() - 4, 4, ".zip") == 0) || (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0);
}

std::string sizeText(int64_t b) {
    char s[32];
    if (b < 1024 * 1024) snprintf(s, sizeof s, "%d KB", int((b + 1023) / 1024));
    else snprintf(s, sizeof s, "%.1f MB", double(b) / (1024.0 * 1024.0));
    return s;
}

}  // namespace

std::string& App::folderPickField() {
    switch (folderFor) {
    case 0: return store.filesDir;
    case 1: return store.importDir;
    case 2: return store.exportDir;
    case 3: return store.coverDir;
    default: return store.backupDir;
    }
}

const char* App::folderPickName() const {
    static const char* names[5] = { "Files opens in", "Playlist imports open in", "Playlist exports open in", "Cover picker opens in",
                                    "Backups go to" };
    return names[std::clamp(folderFor, 0, 4)];
}

void App::chooseFolder(int which) {
    folderFor = which;
    const std::string cur = folderPickField();
    navigate(View::PickFolder, -1, isDirP(cur) ? cur : std::string(which == 4 ? plat::configParent() : plat::root()));
}

void App::scanStorage() {
    if (storage.thread.joinable()) {
        if (!storage.done) return;   // still measuring
        storage.thread.join();
    }
    storage.at = time;
    storage.done = false;
    const std::string dir = Store::Dir, backups = backupRoot();
    storage.thread = std::thread([this, dir, backups] {
        cores::moveHere(cores::FileOps);
        Use u[9];
        measure(dir + "/art", u[0]);
        measure(dir + "/thumbs", u[1]);
        measure(dir + "/loudness.tsv", u[2]);
        measure(dir + "/mix.tsv", u[2]);
        measure(dir + "/mood.tsv", u[2]);
        measure(dir + "/gains.bin", u[2]);
        measure(dir + "/tags.tsv", u[3]);
        measure(dir + "/identity.tsv", u[3]);
        measure(dir + "/lyrics-index.tsv", u[3]);   // search by lyrics: made again from the songs
        measure(dir + "/lastfm-tags.tsv", u[3]);    // (BETA 3's Last.fm mood tags, taken out: a leftover, cleared here)
        for (const auto& n : entries(backups))   // only Gatefold's own backups, whatever else that folder holds
            if (struct stat st; stat(joinP(joinP(backups, n), "gatefold-backup.txt").c_str(), &st) == 0) measure(joinP(backups, n), u[4]);
        measure(dir + "/updates", u[5], isDownload);
        measure(dir + "/lyrics", u[6]);
        measure(dir + "/server-cache", u[7]);       // a Subsonic server's tracks kept to play again
        measure(dir + "/server-downloads", u[8]);   // its downloads, where kept in Songate's folder
        for (int i = 0; i < 9; i++) storage.found[i] = { u[i].bytes, u[i].files };
        storage.done = true;
    });
}

std::string App::storageValue(int which) {
    if (settingsCat == SetStorage && storage.at < -50) scanStorage();   // once each time the page opens, and after a Clear
    if (storage.done && storage.thread.joinable()) {
        storage.thread.join();
        for (int i = 0; i < 9; i++) storage.use[i] = storage.found[i];
        storage.seen = true;
    }
    if (!storage.seen) return "…";
    const StorageUse& u = storage.use[which];
    return u.files == 0 ? std::string("Empty") : sizeText(u.bytes);
}

void App::clearStorage(int which) {
    static const char* titles[9] = { "Covers cache", "Covers for the overlay", "Loudness measurements", "Tag cache", "Backups", "Update downloads",
                                     "Lyrics found online", "Songs kept from the server", "Server downloads" };
    static const char* texts[9] = {
        "The small copies of every cover that lists show. Songate makes them again in the background, a few minutes for a large library.",
        "The covers the overlay shows. Made again for each queue sent to the background player, or all of them from Settings › Playback.",
        "Every song's measured loudness and Automix's analysis. With volume levelling or Automix on, the library is measured again in the background; until then songs play unlevelled and join with a plain fade.",
        "The tags read from your files. Every file is read again now, as Library's \"Read all tags again\" does; your own tag edits stay.",
        "Every settings backup made before an install or a restore. Going back to them is no longer possible; your settings now stay.",
        "Release zips kept from an install that did not finish. The release list stays.",
        "Lyrics found online and saved in Songate's folder. Those saved beside the songs stay. Each song may be found online again the next time it plays (the one playing now, after Songate starts again).",
        "The server's songs played lately, kept so they start at once the next time. They are fetched from the server again when they play.",
        "Every album downloaded from the server into Songate's folder. They play from the server again, and can be downloaded again from their pages.",
    };
    confirm(which == 4 ? ICON_HISTORY : which == 5 || which == 8 ? ICON_DOWNLOAD : which == 6 ? ICON_QUOTE : ICON_FOLDER, "Storage", titles[which],
            storage.seen ? sizeText(storage.use[which].bytes) + " " GF_ON_CARD : std::string(GF_CAP_ON_CARD), "Clear it?", texts[which], "Clear",
            [this, which] {
                const std::string dir = Store::Dir;
                switch (which) {
                // Thousands of files: deleted on the worker (the progress stack shows it), not on the main thread, which
                // held the whole app until they were gone
                case 0:
                    if (fileWork.thread.joinable()) { toast("Wait until “" + fileWork.label + "” has finished"); return; }
                    meta.stop();
                    meta.clearCovers();
                    startFileWork("Clearing the covers cache", dir, true, [art = dir + "/art"](FileWork& w) {
                        w.total = 0;
                        removeAll(art);
                        w.result = "Covers cache cleared";
                    }, [this] { storage.at = -100; });   // library: a rescan follows, which makes them again
                    fileWork.cat = SetStorage;   // its card opens Storage
                    return;
                case 1: {
                    if (fileWork.thread.joinable()) { toast("Wait until “" + fileWork.label + "” has finished"); return; }
                    const bool was = thumbs.running();
                    thumbs.stop();
                    startFileWork("Clearing the overlay's covers", dir, false, [th = dir + "/thumbs"](FileWork& w) {
                        w.total = 0;
                        removeAll(th);
                        w.result = "Covers for the overlay cleared";
                    }, [this, was] {
                        if (was) thumbs.start(store.path("thumbs"));
                        storage.at = -100;
                    });
                    fileWork.cat = SetStorage;
                    return;
                }
                case 2:
                    levels.forget();
                    store.writeFile("loudness.tsv", levels.serialise());
                    store.writeFile("mix.tsv", levels.serialiseMix());
                    store.writeFile("mood.tsv", levels.serialiseMood());
                    levelEpoch = ~0u;
                    levelFirst.clear();
                    gainsDirty = true;
                    writeGains();
                    break;
                case 3:
                    remove((dir + "/lyrics-index.tsv").c_str());   // (read again; what is in memory is written back as it goes)
                    remove((dir + "/lastfm-tags.tsv").c_str());    // (BETA 3's Last.fm mood tags, taken out: a leftover)
                    identity.forget();
                    meta.stop();
                    meta.clearDiskCache();
                    rescan();
                    break;
                case 4:
                    if (!startFileWork("Clearing the backups", backupRoot(), false, [root = backupRoot()](FileWork& w) {
                        w.total = 0;
                        for (const auto& n : entries(root))
                            if (struct stat st; stat(joinP(joinP(root, n), "gatefold-backup.txt").c_str(), &st) == 0) removeAll(joinP(root, n));
                        w.result = "Backups cleared";
                    }, [this] { backupsAt = -100; storage.at = -100; }))
                        return;
                    fileWork.cat = SetStorage;
                    return;
                case 5:
                    for (const auto& n : entries(dir + "/updates")) if (isDownload(n)) remove(joinP(dir + "/updates", n).c_str());
                    break;
                case 7:   // the server's tracks kept to play again: removed on its worker
                    subsonic.wipeLocal(false);
                    break;
                case 8:   // the server's downloads (and the tracks kept): removed on its worker; a music folder's stay (they are your tracks now)
                    subsonic.wipeLocal(true);
                    break;
                case 6:   // lyrics found online, kept in Songate's folder: the song playing shows none, and all may be looked up again
                    if (!startFileWork("Removing the lyrics found online", dir, false, [ly = dir + "/lyrics"](FileWork& w) {
                            w.total = 0;
                            removeAll(ly);
                            w.result = "Lyrics found online removed";
                        }, [this] {
                            storage.at = -100;
                            lyrMemo_.clear();   // other songs: looked up again when they play
                            if (now.lyricsInSongate && now.track >= 0) {   // the song playing: as Remove leaves it, not looked up again this run
                                const std::string path = lib.tracks[now.track].path;
                                now.lyrics.clear();
                                now.synced = false;
                                now.lyricsFromFile = now.lyricsInSongate = false;
                                lyrOn = LyrOnline{};
                                lyrOn.path = path;
                                lyrOn.state = LyrOnline::NotFound;
                                lyrOn.why = "Removed from Songate's folder";
                                lyrMemoSet(path, nullptr, lyrOn.why);
                            }
                        }))
                        return;
                    fileWork.cat = SetStorage;
                    return;
                }
                storage.at = -100;   // measured again
                toast(std::string(titles[which]) + " cleared");
            });
}
