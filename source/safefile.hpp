/*
 * Songate · safefile.hpp
 *
 * A file replaced without a moment where neither the old nor the new one is on the card.
 * The card's file system cannot rename over a file that exists, so the old one steps aside as
 * "<file>.prev" first; a power cut between the two renames leaves that name behind, and
 * recover() (called before the file is read) puts the newer whole copy back in place.
 */
#pragma once

#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace safefile {

inline bool exists(const std::string& p) {
    struct stat s;
    return stat(p.c_str(), &s) == 0;
}

/** `tmp`, written whole and closed, takes `path`'s place. False (and the old file back, tmp gone) when it could not. */
inline bool swapIn(const std::string& tmp, const std::string& path) {
    const std::string prev = path + ".prev";
    remove(prev.c_str());
    const bool had = rename(path.c_str(), prev.c_str()) == 0;
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        if (had) rename(prev.c_str(), path.c_str());
        remove(tmp.c_str());
        return false;
    }
    if (had) remove(prev.c_str());
    return true;
}

/** After a cut swap: the new copy if it is there (it was whole before the old one stepped aside), else the old one. */
inline void recover(const std::string& path, const std::string& tmp = std::string()) {
    const std::string prev = path + ".prev";
    if (!exists(prev)) return;
    if (exists(path)) { remove(prev.c_str()); return; }
    const std::string t = tmp.empty() ? path + ".tmp" : tmp;
    if (rename(t.c_str(), path.c_str()) == 0) remove(prev.c_str());
    else rename(prev.c_str(), path.c_str());
}

/** recover() for every "<file>.prev" in a folder (not below it). */
inline void recoverDir(const std::string& dir) {
    std::vector<std::string> found;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".prev") == 0) found.push_back(n.substr(0, n.size() - 5));
        }
        closedir(d);
    }
    for (const std::string& n : found) recover(dir + "/" + n);
}

}  // namespace safefile
