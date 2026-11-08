/*
 * Songate · tracksort.hpp
 * Library › Tracks' sort and filter (view_library.cpp), shared with the Server tab's Tracks (view_server.cpp) so both sort, filter
 * and letter their lists the same way.
 */
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace tracksort {

/** What a sort needs of a track, copied on the main thread: the metadata thread keeps writing the library's own. */
struct SortRow {
    int idx;
    std::string title, artist, album, genre, date;   // artist: the album artist when there is one
    int trackNo;
    double duration;
};

/** Filters rows by the sorted field (the title when by length), sorts them (0 artist, 1 title, 2 album, 3 year, 4 genre, 5 length),
 *  and gives their idx in order, with the letter index (none for year and length). */
void sortRows(std::vector<SortRow>& rows, int s, bool desc, const std::string& filter, std::vector<int>& list,
              std::vector<std::pair<char, int>>& letters);

}  // namespace tracksort
