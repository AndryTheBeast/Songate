/*
 * Gatefold NX background player · dec_flac.cpp
 * FLAC through dr_flac, which streams and seeks on its own. Pictures are not loaded into memory
 * (DR_FLAC_NO_PICTURE_METADATA_MALLOC): only where the front cover sits is kept, for the overlay.
 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_PICTURE_METADATA_MALLOC
#define DR_FLAC_NO_WCHAR
#include "dr_flac.h"

#include <cstdlib>
#include <strings.h>

#include "dec_common.hpp"

namespace {

class FlacDecoder : public Decoder {
public:
    ~FlacDecoder() override {
        if (f_) drflac_close(f_);
        if (fp_) fclose(fp_);
        free(tmp_);
    }

    bool open(const char* path) {
        // A FLAC behind an ID3v2 tag (some rippers write one): dr_flac passes over the tag and then seeks to the seek table and
        // the first frame as if there were none, so the file did not open (bgtest, 2026-11-12). It is given the file from
        // where the FLAC begins instead, through a reader of its own; every other FLAC opens as before.
        base_ = id3Length(path);
        if (base_ > 0) {
            fp_ = fopen(path, "rb");
            if (!fp_ || fseek(fp_, base_, SEEK_SET) != 0) return false;
            f_ = drflac_open_with_metadata(&FlacDecoder::ioRead, &FlacDecoder::ioSeek, &FlacDecoder::ioTell, &FlacDecoder::meta, this, nullptr);
        } else {
            f_ = drflac_open_file_with_metadata(path, &FlacDecoder::meta, this, nullptr);
        }
        if (!f_ || f_->channels == 0 || f_->sampleRate == 0) return false;
        codec = 3;
        rate = int(f_->sampleRate);
        channels = f_->channels;
        duration = double(f_->totalPCMFrameCount) / rate;
        if (FILE* fp = fopen(path, "rb")) {
            fseek(fp, 0, SEEK_END);
            if (duration > 0) bitrate = int(double(ftell(fp)) * 8 / duration / 1000);
            fclose(fp);
        }
        if (channels != 2) tmp_ = static_cast<int16_t*>(malloc(size_t(Chunk) * channels * 2));
        return channels == 2 || tmp_;
    }

    int read(int16_t* out, int maxFrames) override {
        if (channels == 2) return int(drflac_read_pcm_frames_s16(f_, drflac_uint64(maxFrames), out));
        int done = 0;
        while (done < maxFrames) {
            const int want = std::min(Chunk, maxFrames - done);
            const int got = int(drflac_read_pcm_frames_s16(f_, drflac_uint64(want), tmp_));
            if (got <= 0) break;
            toStereo(tmp_, got, channels, out + done * 2);
            done += got;
        }
        return done;
    }

    bool seek(double seconds) override { return drflac_seek_to_pcm_frame(f_, drflac_uint64(seconds * rate)); }

private:
    static constexpr int Chunk = 1024;
    drflac* f_ = nullptr;
    int16_t* tmp_ = nullptr;
    int pictureType_ = -1;
    FILE* fp_ = nullptr;   // the file itself, only for a FLAC behind an ID3 tag
    long base_ = 0;        // where the FLAC begins in it (the tags' length)

    /** The length of the ID3v2 tags at the front of the file (0: none, the usual). */
    static long id3Length(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return 0;
        long at = 0;
        for (int n = 0; n < 4; n++) {   // (more than one is allowed; four is plenty)
            uint8_t h[10];
            if (fseek(f, at, SEEK_SET) != 0 || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) break;
            at += 10 + long((h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F)) + ((h[5] & 0x10) ? 10 : 0);
        }
        fclose(f);
        return at;
    }
    static size_t ioRead(void* self, void* out, size_t n) { return fread(out, 1, n, static_cast<FlacDecoder*>(self)->fp_); }
    static drflac_bool32 ioSeek(void* self, int offset, drflac_seek_origin origin) {
        auto* d = static_cast<FlacDecoder*>(self);
        if (origin == DRFLAC_SEEK_SET) return fseek(d->fp_, d->base_ + offset, SEEK_SET) == 0;
        return fseek(d->fp_, offset, origin == DRFLAC_SEEK_CUR ? SEEK_CUR : SEEK_END) == 0;
    }
    static drflac_bool32 ioTell(void* self, drflac_int64* at) {
        auto* d = static_cast<FlacDecoder*>(self);
        const long p = ftell(d->fp_);
        if (p < 0) return DRFLAC_FALSE;
        *at = drflac_int64(p - d->base_);
        return DRFLAC_TRUE;
    }

    static void meta(void* self, drflac_metadata* m) {
        auto* d = static_cast<FlacDecoder*>(self);
        if (m->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
            drflac_vorbis_comment_iterator it;
            drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
            drflac_uint32 len;
            while (const char* c = drflac_next_vorbis_comment(&it, &len)) {
                const char* eq = static_cast<const char*>(memchr(c, '=', len));
                if (!eq) continue;
                const size_t k = size_t(eq - c);
                const std::string v(eq + 1, len - k - 1);
                if (k == 5 && !strncasecmp(c, "TITLE", 5) && d->tags.title.empty()) d->tags.title = v;
                else if (k == 6 && !strncasecmp(c, "ARTIST", 6) && d->tags.artist.empty()) d->tags.artist = v;
                else if (k == 5 && !strncasecmp(c, "ALBUM", 5) && d->tags.album.empty()) d->tags.album = v;
            }
        } else if (m->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
            // The front cover (type 3) wins over whatever came first.
            const int type = int(m->data.picture.type);
            if (d->pictureType_ == 3 || (d->pictureType_ >= 0 && type != 3)) return;
            const char* mime = m->data.picture.mime;
            const uint32_t ml = m->data.picture.mimeLength;
            const bool png = mime && ml >= 9 && !strncasecmp(mime, "image/png", 9);
            d->pictureType_ = type;
            d->tags.coverKind = png ? 2 : 1;
            d->tags.coverOffset = m->data.picture.pictureDataOffset + uint64_t(d->base_);   // (in the file, for the overlay)
            d->tags.coverSize = m->data.picture.pictureDataSize;
        }
    }
};

}  // namespace

std::unique_ptr<Decoder> openFlac(const char* path) {
    auto d = std::make_unique<FlacDecoder>();
    if (!d->open(path)) return nullptr;
    return d;
}
