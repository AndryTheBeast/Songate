/*
 * Songate · cfgmove.cpp
 * The settings folder from gatefold-nx to songate-nx and back (cfgmove.hpp). Plain files and renames, on the main thread at launch
 * and at exit only: nothing here runs while a frame is drawn.
 */
#include "cfgmove.hpp"

#ifndef GF_WINDOWED   // the Switch and its preview only: the PC's folder never moved
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <unistd.h>   // rmdir (the preview's comes with its stand-ins)
#endif
#include <vector>

#include "gfcfgdir.h"
#include "platform.hpp"
#include "updater.hpp"

namespace cfgmove {

namespace {

const std::string N = GFCFG_NEW, O = GFCFG_OLD, NB = GFCFG_NEW_BACKUPS, OB = GFCFG_OLD_BACKUPS;
constexpr const char* kJournal = "songate-move.txt";   // a move under way: in the folder, so it travels with the rename
constexpr const char* kMark = "songate-folder.txt";    // a move that finished, and a move back (back=): what the return reads
constexpr const char* kKept = "settings-songate.txt";  // settings.txt as it was before a move back: the keys older versions drop
constexpr const char* kSince = "2.0.0-beta.2";          // the first release on songate-nx

bool isDir(const std::string& p) { return gfcfgIsDir(p.c_str()) != 0; }
bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
long long mtime(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
}
std::string in(const std::string& dir, const std::string& name) { return dir + "/" + name; }

bool readFile(const std::string& p, std::string& out) {
    out.clear();
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
/** Written beside it and renamed over it: a power cut leaves the old file or the new one, never half of one. */
bool writeFile(const std::string& p, const std::string& data) {
    const std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return false; }
    remove(p.c_str());
    return rename(tmp.c_str(), p.c_str()) == 0;
}
std::vector<std::string> names(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) out.push_back(e->d_name);
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}
std::string stamp() {
    const time_t t = time(nullptr);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H.%M.%S", localtime(&t));   // as Updater names its backups
    return b;
}
std::string keyOf(const std::string& text, const std::string& key) {
    const std::string k = key + "=";
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        if (text.compare(a, k.size(), k) == 0) {
            std::string v = text.substr(a + k.size(), b - a - k.size());
            while (!v.empty() && v.back() == '\r') v.pop_back();
            return v;
        }
        a = b + 1;
    }
    return {};
}
void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
}

/** The files that name the folder or its backups by full path: chosen covers, backupDir, the updater's notes. */
void rewritePaths(const std::string& dir, const std::string& fromDir, const std::string& toDir, bool backupsToo) {
    for (const char* f : { "settings.txt", "covers.tsv", "updates/installed.txt", "updates/restore.txt", "updates/fresh.txt", "updates/pending.txt" }) {
        std::string t;
        if (!readFile(in(dir, f), t)) continue;
        std::string u = t;
        // the backups first: "…/gatefold-nx-backups" begins with "…/gatefold-nx"
        if (backupsToo) replaceAll(u, fromDir + "-backups", toDir + "-backups");
        replaceAll(u, fromDir + "/", toDir + "/");
        if (u != t) writeFile(in(dir, f), u);
    }
}

/** A backups folder renamed whole, or its backups moved in one by one when the other is there already (each a rename). */
bool moveBackups(const std::string& from, const std::string& to) {
    if (!isDir(from)) return true;
    if (!exists(to)) return rename(from.c_str(), to.c_str()) == 0;
    bool all = true;
    for (const std::string& n : names(from)) {
        std::string dst = in(to, n);
        for (int k = 2; exists(dst); k++) dst = in(to, n + " (" + std::to_string(k) + ")");
        if (rename(in(from, n).c_str(), dst.c_str()) != 0) all = false;
    }
    if (all) rmdir(from.c_str());
    return all;
}

/** A whole folder into the backups as a backup Settings › Updates lists ("Settings of …, set aside"). */
bool setAside(const std::string& dir, const std::string& backups, const std::string& from, std::string& log) {
    mkdir("sdmc:/config", 0777);
    mkdir(backups.c_str(), 0777);
    std::string dst = in(backups, stamp() + " " + (dir == O ? "gatefold-nx" : "songate-nx") + " set aside");
    for (int k = 2; exists(dst); k++) dst = in(backups, stamp() + " set aside " + std::to_string(k));
    if (rename(dir.c_str(), dst.c_str()) != 0) { log += " Could not set " + dir + " aside."; return false; }
    writeFile(in(dst, "gatefold-backup.txt"), "from=" + from + "\nto=set aside when both folders were there\ndate=" + stamp() + "\n");
    log += " " + dir + " kept as the backup " + dst + ".";
    return true;
}

/** settings.txt's keys that it lost while an older Songate wrote it (each save there writes only the keys that version knows). */
int restoreKeys(const std::string& dir) {
    std::string kept, now;
    if (!readFile(in(dir, kKept), kept)) return 0;
    readFile(in(dir, "settings.txt"), now);
    std::set<std::string> have;
    for (size_t a = 0; a < now.size();) {
        size_t b = now.find('\n', a);
        if (b == std::string::npos) b = now.size();
        const size_t eq = now.find('=', a);
        if (eq != std::string::npos && eq < b) have.insert(now.substr(a, eq - a));
        a = b + 1;
    }
    std::string add;
    int n = 0;
    for (size_t a = 0; a < kept.size();) {
        size_t b = kept.find('\n', a);
        if (b == std::string::npos) b = kept.size();
        const std::string line = kept.substr(a, b - a);
        const size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0 && !have.count(line.substr(0, eq))) { add += line + "\n"; n++; }   // the older version's own value stays
        a = b + 1;
    }
    if (!add.empty()) {
        if (!now.empty() && now.back() != '\n') now += "\n";
        if (!writeFile(in(dir, "settings.txt"), now + add)) return -1;
    }
    remove(in(dir, kKept).c_str());
    return n;
}

/** Everything after the rename, in songate-nx: each step can run again, so a cut here is finished at the next launch. */
void finish(Result& r, const std::string& version) {
    std::string mark;
    readFile(in(N, kMark), mark);
    r.returned = mark.find("back=") != std::string::npos;
    const bool backups = moveBackups(OB, NB);
    rewritePaths(N, O, N, backups);
    if (r.returned) {
        const int keys = restoreKeys(N);
        writeFile(in(N, "sync-addonly.txt"), "1\n");   // syncer.cpp: the first sync after it takes nothing away
        r.log += " Back from " + keyOf(mark, "back") + ": " + std::to_string(std::max(keys, 0)) + " settings it dropped put back; the next sync only adds.";
    }
    writeFile(in(N, kMark), "moved=gatefold-nx\nversion=" + version + "\ndate=" + stamp() + "\n");
    remove(in(N, kJournal).c_str());
    r.moved = true;
}

// What a BETA 1 background player or overlay, still running after the move, writes into a gatefold-nx it makes again
const std::set<std::string> kStray = { "bg-session.txt", "bg-session.tmp", "bg-config.txt", "bg-games.txt", "bg-places.txt", "bg-plays.txt",
                                       "bg-scrobbles.txt", "overlay.ini", "overlay-volume.txt", "overlay-playlist.txt", "overlay-source.txt" };

/** gatefold-nx holding strays only: each merged into songate-nx, then removed; true when the folder is gone. */
bool mergeStrays(std::string& log) {
    const std::vector<std::string> all = names(O);
    for (const std::string& n : all) if (!kStray.count(n) || isDir(in(O, n))) return false;
    int merged = 0;
    for (const std::string& n : all) {
        const std::string from = in(O, n), to = in(N, n);
        std::string a, b;
        if (!readFile(from, a)) return false;
        bool ok = true;
        if (n == "bg-session.tmp") {
            // half of a write: nothing to keep
        } else if (n == "bg-plays.txt" || n == "bg-scrobbles.txt") {
            // logs: the lines songate-nx's lacks are added (the app reads them and empties them)
            readFile(to, b);
            std::set<std::string> have;
            for (size_t p = 0; p < b.size();) {
                size_t e = b.find('\n', p);
                if (e == std::string::npos) e = b.size();
                have.insert(b.substr(p, e - p));
                p = e + 1;
            }
            std::string add;
            for (size_t p = 0; p < a.size();) {
                size_t e = a.find('\n', p);
                if (e == std::string::npos) e = a.size();
                const std::string l = a.substr(p, e - p);
                if (!l.empty() && !have.count(l)) add += l + "\n";
                p = e + 1;
            }
            if (!add.empty()) {
                if (!b.empty() && b.back() != '\n') b += "\n";
                ok = writeFile(to, b + add);
            }
        } else if (!exists(to) || mtime(from) > mtime(to)) {
            ok = writeFile(to, a);   // the newer one
        }
        if (!ok) { log += " Could not merge " + n + ": gatefold-nx left as it is."; return false; }
        merged++;
    }
    for (const std::string& n : all) remove(in(O, n).c_str());
    rmdir(O.c_str());
    log += " " + std::to_string(merged) + " files an older background player or overlay wrote after the move, merged in.";
    return !isDir(O);
}

std::string backupsOf(const std::string& dir) {
    std::string s;
    readFile(in(dir, "settings.txt"), s);
    const std::string chosen = keyOf(s, "backupDir");
    // never inside a settings folder (a backup would be moved with it)
    return chosen.empty() || chosen.rfind(N, 0) == 0 || chosen.rfind(O, 0) == 0 ? NB : chosen;
}

}  // namespace

bool usesOldFolder(const std::string& version) { return Updater::compare(version, kSince) < 0; }

Result atLaunch(const std::string& version) {
    Result r;
    r.dir = N;
    r.backups = NB;
    auto done = [&]() -> Result {
        plat::useConfigDir(r.dir.c_str(), r.backups.c_str());
        return r;
    };

    // ---- the set from gatefold-nx was chosen when both were there (settle): songate-nx's goes aside, gatefold-nx's comes in
    std::string settle;
    if (isDir(N) && isDir(O) && readFile(in(N, "updates/settle.txt"), settle) && keyOf(settle, "keep") == "old") {
        const bool ended = plat::bgEnd();
        if (!ended) { r.log = "Both folders: the background player could not be ended, so the choice waits for the next launch."; return done(); }
        remove(in(N, "updates/settle.txt").c_str());
        if (setAside(N, backupsOf(O), version, r.log) && rename(O.c_str(), N.c_str()) == 0) finish(r, version);
        plat::bgStart(ended);
        return done();
    }

    // ---- a move cut off: its journal says which side of the rename it stopped
    std::string j;
    if (readFile(in(N, kJournal), j)) {
        if (keyOf(j, "to") == "gatefold-nx") {
            // a move back that never reached its rename (the older version would have found no folder): undone, songate-nx again
            rewritePaths(N, O, N, true);
            moveBackups(OB, NB);
            remove(in(N, kJournal).c_str());
            r.log = "A move back to gatefold-nx was cut off before it happened: undone.";
        } else {
            const bool ended = plat::bgEnd();
            finish(r, version);
            plat::bgStart(ended);
            r.log = "A move to songate-nx cut off after its rename: finished." + r.log;
        }
    }

    if (!isDir(N) && isDir(O)) {
        // ---- the move: nothing may hold the folder, so the background player is ended (it is started again, from its new file)
        const bool ended = plat::bgEnd();
        if (!ended) {
            r.dir = O;
            r.backups = OB;
            r.legacy = true;
            r.log = "Not moved to songate-nx: the background player could not be ended. gatefold-nx for now.";
            return done();
        }
        writeFile(in(O, kJournal), "to=songate-nx\nversion=" + version + "\ndate=" + stamp() + "\n");
        if (rename(O.c_str(), N.c_str()) != 0) {
            remove(in(O, kJournal).c_str());
            plat::bgStart(ended);
            r.dir = O;
            r.backups = OB;
            r.legacy = true;
            r.log = "Not moved to songate-nx: the card refused the rename. gatefold-nx for now.";
            return done();
        }
        finish(r, version);
        plat::bgStart(ended);
        r.log = "Settings moved from gatefold-nx to songate-nx." + r.log;
        return done();
    }

    if (isDir(N) && isDir(O)) {
        // ---- both: strays from a BETA 1 part still running are merged; anything else is a question
        std::string log;
        if (!mergeStrays(log)) {
            r.both = true;
            log += " Both gatefold-nx and songate-nx hold settings: songate-nx's are used until you choose.";
        }
        r.log += log;
    }
    return done();
}

bool back(const std::string& to, std::string& log) {
    if (!isDir(N)) { log = "Going back to " + to + ": no songate-nx to move."; return true; }
    if (isDir(O) && !setAside(O, backupsOf(N), "an older version", log)) return false;   // never two: what was there is a backup now
    const bool ended = plat::bgEnd();   // the new background player writes into songate-nx until it is ended
    // everything settings.txt holds, for the return: an older version's saves keep only the keys it knows
    std::string s;
    if (readFile(in(N, "settings.txt"), s)) writeFile(in(N, kKept), s);
    std::string mark;
    readFile(in(N, kMark), mark);
    writeFile(in(N, kMark), mark + "back=" + to + "\nbackDate=" + stamp() + "\n");
    writeFile(in(N, kJournal), "to=gatefold-nx\nversion=" + to + "\ndate=" + stamp() + "\n");
    // the paths first, so that the older version reads them right from its first launch; the rename last
    const bool backups = !isDir(NB) || !exists(OB);
    rewritePaths(N, N, O, backups);
    if (backups) moveBackups(NB, OB);
    if (rename(N.c_str(), O.c_str()) != 0) {
        if (backups) moveBackups(OB, NB);
        rewritePaths(N, O, N, true);
        remove(in(N, kJournal).c_str());
        plat::bgStart(ended);
        log += " Could not move songate-nx back to gatefold-nx: " + to + " starts without its settings, which wait in songate-nx.";
        return false;
    }
    remove(in(O, kJournal).c_str());
    plat::bgStart(ended);
    log += " Settings moved back to gatefold-nx for " + to + ".";
    return true;
}

void settle(bool keepOld) {
    if (keepOld) {
        mkdir(in(N, "updates").c_str(), 0777);
        writeFile(in(N, "updates/settle.txt"), "keep=old\n");   // at the next launch, before anything reads the folder
        return;
    }
    std::string log;
    setAside(O, backupsOf(N), "an older version", log);
}

}  // namespace cfgmove

#endif
