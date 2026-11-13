/*
 * Gatefold NX · loader.hpp
 * What a track change needs from the SD card and the CPU, done on a worker core: the tags and
 * format, the lyrics, the cover (your own, the embedded one or the folder's), decoded and shrunk,
 * its palette and its backdrop blur. The main thread only uploads the textures, so a track change
 * no longer stops the frame it happens in.
 */
#pragma once
#include <SDL2/SDL.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "metadata.hpp"
#include "palette.hpp"

/** A 64×64 RGBA sample of a cover blurred smooth: three passes of a box blur 2·radius+1 texels wide each way, edges held
 *  (radius 4 is the frost of an Automix join). Stretched with linear filtering it shows no grid. The caller frees it. */
SDL_Surface* coverFrost(const uint8_t* rgba, int pitch, int radius);

class TrackLoader {
public:
    struct Request {
        unsigned gen = 0;
        std::string path, folder;
        std::string myCover;    // your cover for it (Store::covers), else empty
        std::vector<uint8_t> image;   // a picture in memory instead (AirPlay's cover): no file is read, no tags
        bool readInfo = true;   // false: only the cover again (one set or forgotten in the track menu)
        PaletteOpts opts;
        int softRadius = 0;     // the backdrop blurred smooth, this far (coverFrost's radius); 0: not wanted (Background blur: Classic)
    };
    struct Result {
        unsigned gen = 0;
        bool readInfo = true;
        TrackInfo info;                  // without the cover bytes
        std::string lyrics;              // the .lrc file's, else the tag's
        bool sidecar = false;            // the lyrics came from an .lrc file
        bool inSongate = false;          // ... the one in Songate's own folder (found online: lyricsnet.hpp)
        SDL_Surface* cover = nullptr;    // RGBA32, at most 512 px: the GPU path has no mipmaps (Gfx::shrink)
#ifdef GF_WINDOWED
        SDL_Surface* mediaCover = nullptr;   // 256 px, for the taskbar and the system's media controls
        std::vector<uint8_t> mediaJpeg;
#endif
        std::vector<uint8_t> airCover;   // AirPlay speakers' cover (airsend.hpp): JPEG or PNG, 512 KB at most (GF_AIRPLAY)
        SDL_Surface* blur = nullptr;     // 12×12, stretched with linear filtering for the backdrop
        SDL_Surface* frost = nullptr;    // 64×64, blurred (three box passes): the cover frosted while Automix joins
        SDL_Surface* soft = nullptr;     // 64×64, blurred further (Request::softRadius): the backdrop, Background blur: Smooth
        std::vector<uint8_t> px;         // the 64×64 sample the palette came from (Brightness and Saturation rebuild it)
        Palette pal;
        void release();
    };

    void start();
    void stop();
    /** Replaces whatever is waiting: only the newest track matters. */
    void request(Request r);
    /** The newest finished result, once. The caller frees its surfaces (Result::release). */
    bool take(Result& out);

private:
    void run();
    static void work(const Request& r, Result& out);

    std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    bool quit_ = false, hasReq_ = false, hasDone_ = false;
    Request req_;
    Result done_;
};
