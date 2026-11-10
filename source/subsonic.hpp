/*
 * Songate · subsonic.hpp
 * Music from your own server (BETA 3, item 8): any server speaking the Subsonic API (Navidrome, Airsonic, Gonic, Ampache…). A worker
 * at cores::Net signs in and asks; nothing goes online before you sign in (Settings › Library › Subsonic server).
 *
 * The password is never kept. Subsonic's token sign-in sends md5(password + salt); a server takes any salt, so the token and its
 * salt are made once at sign-in and kept in subsonic-account.txt (the address, the user, the salt, the token), and the password is
 * wiped. That file is as lastfm-session.txt is: never in settings.txt, left out of backups, never logged, never in a report and
 * never sent to another device.
 */
#pragma once
#include <SDL2/SDL.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Subsonic {
public:
    enum State { SignedOut, SigningIn, SignedIn, Failed };
    struct View {
        State state = SignedOut;
        std::string server;          // its address, as typed (cleaned)
        std::string user;
        std::string kind, version;   // "navidrome", "0.64.2"; the API's version
        std::string error;           // why the last sign-in failed, in words
        int songs = -1;              // how many it holds (where it says: -1 not known)
        unsigned gen = 0;            // something changed
    };
    struct Artist { std::string id, name, cover; int albums = 0; };
    struct Album { std::string id, name, artist, artistId, cover; int year = 0, songs = 0; double duration = 0; };
    struct Song {
        std::string id, title, artist, artistId, album, albumId, cover, suffix, contentType, genre, parent;   // parent: its folder
        int track = 0, disc = 0, year = 0, bitRate = 0;
        double duration = 0;
    };

    void start(const std::string& dir);
    void stop();
    View view();
    /** Sign in: the password becomes the token and is wiped (the caller's copy too). Main thread. */
    void signIn(const std::string& server, const std::string& user, std::string& password);
    void signOut();

    // ---- browsing (stage 1b): asked, then read when gen moves on
    void askArtists();
    /** Every album on it (getAlbumList2, by artist, 500 a page): for its music in the library (stage 3). */
    void askAllAlbums();
    bool allAlbums(std::vector<Album>& out);
    void askArtist(const std::string& id);
    void askAlbum(const std::string& id);
    bool artists(std::vector<Artist>& out);
    bool artist(const std::string& id, std::vector<Album>& out);
    bool album(const std::string& id, Album& a, std::vector<Song>& songs);
    /** A cover's bytes (JPEG or PNG), asked for at `size` px; empty until it came. */
    void askCover(const std::string& id, int size);
    bool cover(const std::string& id, std::vector<uint8_t>& bytes);
    /** Covers asked for, decoded (RGBA32) on the worker: the caller owns them (a texture each, then SDL_FreeSurface). */
    void takeCovers(std::vector<std::pair<std::string, SDL_Surface*>>& out);
    /** Where a song streams from (with its token: never logged). */
    std::string streamUrl(const std::string& id);
    /** Stage 2: a song fetched to the card (server-cache/, the oldest dropped past 512 MB) to play from there; asked again it is found
     *  at once. fetched(): its file once it is there ("" while it comes; false when it could not be had). */
    /** now: the song to play now, fetched before everything else waiting (the user, 2026-11-07: "Sometimes it takes a bit to load":
     *  it waited behind the covers and lists of the pages browsed); else ahead of time (the next one), in its turn */
    void askSong(const std::string& id, const std::string& suffix, bool now = false);
    bool fetched(const std::string& id, std::string& path);

    // ---- the Server tab's other views (Genres, Folders, Tracks) and the library's genres mixed in
    struct Genre { std::string name; int songs = 0, albums = 0; };
    struct Entry { bool dir = false; std::string id, name, cover; Song song; };   // what a folder holds: a folder, or a song
    void askGenres();
    bool genres(std::vector<Genre>& out);
    void askGenreAlbums(const std::string& genre);   // getAlbumList2 byGenre
    bool genreAlbums(const std::string& genre, std::vector<Album>& out);
    void askFolders();                               // the top: getIndexes (every music folder's)
    bool folders(std::vector<Entry>& out);
    void askDir(const std::string& id);              // getMusicDirectory
    bool dir(const std::string& id, std::string& name, std::vector<Entry>& out);
    void askAllSongs();                              // search3 with no words, 500 a page
    bool allSongs(std::vector<Song>& out);

    // ---- stage 4: albums downloaded to keep (the user: "Stream, plus download"), to root/<Artist>/<Album>/<nn Title>.<ext>, root being
    // Songate's own folder or the music folder (Settings: Downloads). A queue behind browsing on the same worker, one song at a time;
    // its index (server-downloads.tsv) keeps what is wanted and what is here, so a stopped or unfinished album goes on at the next start.
    struct DlState { int state = 0, have = 0, total = 0; };   // state 0 none, 1 coming (wanted, not all here), 2 all here
    void downloadAlbum(const std::string& albumId, const std::string& root);
    void removeDownload(const std::string& albumId);           // its files removed (on the worker)
    void pauseDownloads(bool paused);                          // Stop / Resume: the queue kept
    DlState albumDownload(const std::string& albumId);
    bool downloaded(const std::string& songId, std::string& path);
    void dlProgress(int& done, int& total);                    // this run's batch (0, 0 when none)
    void dlCounts(int& songs, long long& bytes, int& albums);  // everything kept
    bool takeBatchDone(bool& toMusic);                         // a batch ended (toMusic: it wrote in the music folder)
    void wipeLocal(bool downloadsToo);                         // the songs kept to play again, and the downloads too: removed (worker)
    bool wiping();
    long long cacheBytes();
    unsigned dlGen();                                          // moves when downloads change

    // ---- stage 5: sync (the user: "Liked ↔ starred, Plays (scrobbles), Playlists")
    void scrobble(const std::string& songId, bool submission);   // false: now playing; true: played (its time now)
    void askStarred();                                            // getStarred2's songs
    bool starred(std::vector<Song>& out, unsigned& gen);          // a copy, and its generation (moves when stars change)
    bool isStarred(const std::string& songId);
    void star(const std::string& songId, bool on);                // here at once, on the server soon after
    std::unordered_set<std::string> likeLedger();                 // "artist\x1ftitle" liked on both at the last sync (server-likes.tsv)
    void setLikeLedger(std::unordered_set<std::string> keys);     // kept, and written on the worker
    struct Playlist { std::string id, name, cover; int songs = 0; double duration = 0; };
    void askPlaylists();
    bool playlists(std::vector<Playlist>& out, unsigned& gen);
    void askPlaylist(const std::string& id);
    bool playlist(const std::string& id, Playlist& pl, std::vector<Song>& songs);
    /** A playlist made (or, existingId given, its tracks replaced) on the server with these tracks; the list is asked again after. */
    void sendPlaylist(const std::string& name, const std::string& existingId, const std::vector<std::string>& songIds);
    /** Server playlists made and changed here (the user, 2026-11-06): new (with these tracks, maybe none), renamed, tracks added or
     *  removed (by their place), deleted. Each asks the list (and that playlist) again once the server answered. */
    void newPlaylist(const std::string& name, const std::vector<std::string>& songIds);
    void updatePlaylist(const std::string& id, const std::string& newName, const std::vector<std::string>& addIds, const std::vector<int>& removeAt);
    void deletePlaylist(const std::string& id);
    /** A track's lyrics: kept in Songate's folder (savedFile, from lyrnet::savedPath), else the server's (getLyricsBySongId: synced as
     *  LRC; else getLyrics, plain). lyrics(): true once answered; text "" none; source where they came from. */
    void askLyrics(const std::string& songId, const std::string& artist, const std::string& title, const std::string& savedFile);
    /** A song's file (fetched or downloaded) read as Songate reads yours: its format, sample rate, bits and bitrate (Now playing's chips). */
    struct FileInfo { std::string codec; int sampleRate = 0, bits = 0, kbps = 0; };
    void askFileInfo(const std::string& songId, const std::string& path);
    bool fileInfo(const std::string& songId, FileInfo& out);
    bool lyrics(const std::string& songId, std::string& text, std::string& source);

private:
    struct Task { enum Kind { DlAlbum, DlSong, DlRemove, DlWipe, Scrobble, Star, Starred, SaveLedger, Playlists, PlaylistOne, SendPlaylist, PlEdit, Lyrics, FileInfoTask, Ping, Artists, AllAlbums, Genres, GenreAlbums, Folders, Dir, AllSongs, Artist, Album, Cover, Song } kind; std::string id; int size = 0; std::string suffix, album, path;
                  bool now = false;      // (a Song: the one to play now, taken before anything else waiting)
                  int tries = 0; };      // (a list the server could not be reached for: asked again, later_)
    struct Later { Task task; std::chrono::steady_clock::time_point at; };
    std::vector<Later> later_;   // under m_: lists to ask again (no network at that moment), each when its wait is over
    unsigned sess_ = 0;          // under m_: bumps at every sign-in and sign-out; a task that began before one never fills the next
    bool unreached_ = false;     // the worker's: the task's last call never got to the server
    void clearSession();         // m_ held: every list and cover of the session forgotten
    std::vector<std::pair<std::string, std::string>> songs_;   // under m_: id → its file ("" could not be had)
    void prune();
    void run();
    bool call(const std::string& method, const std::string& extra, std::string& body, std::string& err);
    std::string base(const std::string& method, const std::string& extra);
    void loadAccount();
    void saveAccount();
    void note(const std::string& s);

    std::string dir_;
    std::thread thread_;
    std::mutex m_;
    std::condition_variable wake_;
    bool quit_ = false;
    std::deque<Task> tasks_;
    View view_;
    std::string salt_, token_;   // under m_; never logged
    std::vector<Artist> artists_;
    bool haveArtists_ = false;
    std::vector<Album> allAlbums_;
    bool haveAllAlbums_ = false;
    std::vector<Genre> genres_;
    bool haveGenres_ = false;
    std::vector<std::pair<std::string, std::vector<Album>>> genreAlbums_;
    std::vector<Entry> folders_;
    bool haveFolders_ = false;
    std::vector<std::pair<std::string, std::pair<std::string, std::vector<Entry>>>> dirs_;
    std::vector<Song> allSongs_;
    bool haveAllSongs_ = false;
    struct DlAlbumRec { std::string root, name, artist; int total = 0; };
    struct DlSongRec { std::string album; long long size = 0; std::string path; };
    std::unordered_map<std::string, DlAlbumRec> dlAlbums_;   // under m_
    std::unordered_map<std::string, DlSongRec> dlSongs_;
    bool dlPaused_ = false, dlBatchDone_ = false, dlBatchMusic_ = false, dlWroteMusic_ = false;
    int dlDone_ = 0, dlTotal_ = 0, wiping_ = 0;
    unsigned dlGen_ = 0;
    std::vector<Playlist> playlists_;
    unsigned plEdits_ = 0;
    bool havePlaylists_ = false;
    unsigned plGen_ = 0;
    std::vector<std::pair<std::string, std::pair<Playlist, std::vector<Song>>>> playlistSongs_;
    std::vector<std::pair<std::string, std::pair<std::string, std::string>>> lyrics_;   // id → (text, source)
    std::vector<std::pair<std::string, FileInfo>> fileInfos_;
    std::vector<Song> starred_;
    std::unordered_set<std::string> starredIds_, ledger_;
    bool haveStarred_ = false;
    unsigned starGen_ = 0;
    void loadDownloads();
    void saveDownloads();   // (not under m_)
    bool runnable() const;  // (under m_) a task the worker may take now
    void clearLists();   // (under m_) at sign-in and sign-out
    std::vector<std::pair<std::string, std::vector<Album>>> artistAlbums_;
    std::vector<std::pair<std::string, std::pair<Album, std::vector<Song>>>> albums_;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> covers_;
    std::vector<std::pair<std::string, SDL_Surface*>> decoded_;   // under m_: waiting for the app's thread
};
