/*
 * Songate · lrcwords.hpp
 * Word-synced lyrics (the user, 2026-11-18: "Apple music synced per word lyrics" in the tags): an LRC line's words, each with its
 * own start, as the enhanced LRC writes them after the line's [mm:ss.xx]:
 *     [00:08.839]<00:08.839>I <00:09.029>can't <00:09.499>stop <00:10.019>this <00:10.559>feeling
 * A time inside a word (syllables: <00:01.0>hoo<00:01.4>ked) makes a piece joined to the one before (glue: no space between).
 * Read by App::parseLyrics (the line's words) and the lyrics index (the line's words alone, without their times).
 */
#pragma once

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

struct LyricWord { double t; std::string text; bool glue = false; };

/** A line's body: its words with their starts (none when the line has no <mm:ss.xx>), and its text without the times (the words,
 *  one space between them, glued pieces together). Text before the first time is a word of its own, at -1 (the line's start). */
inline void lrcWords(const std::string& body, std::vector<LyricWord>& words, std::string& plain) {
    words.clear();
    plain.clear();
    if (body.find('<') == std::string::npos) { plain = body; return; }
    double at = -1;
    bool glue = false;
    std::string cur;
    auto put = [&] {
        const size_t a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t");
        if (a != std::string::npos) {
            const std::string w = cur.substr(a, b - a + 1);
            const bool g = glue && !words.empty() && a == 0;   // (no space before it in the line)
            words.push_back({ at, w, g });
            if (!plain.empty() && !g) plain += ' ';
            plain += w;
        }
    };
    bool timed = false;
    for (size_t i = 0; i < body.size();) {
        if (body[i] == '<') {
            const size_t close = body.find('>', i);
            int mm = 0;
            double ss = 0;
            if (close != std::string::npos && sscanf(body.c_str() + i + 1, "%d:%lf", &mm, &ss) == 2) {
                put();
                glue = !cur.empty() && !std::isspace(static_cast<unsigned char>(cur.back()));
                cur.clear();
                at = mm * 60 + ss;
                timed = true;
                i = close + 1;
                continue;
            }
        }
        cur += body[i++];
    }
    put();
    if (!timed) { words.clear(); plain = body; }
}
