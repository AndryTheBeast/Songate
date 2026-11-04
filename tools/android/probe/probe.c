/*
 * Songate · tools/android/probe/probe.c (dev only, never in the APK)
 * Opens files with the Android build's own FFmpeg and libFLAC, outside the app, to tell "cannot read" from "cannot decode".
 *   probe <file>...   prints, for each: FFmpeg's open, its streams, the first frames decoded; libFLAC's tags
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <FLAC/metadata.h>

static void ff(const char* path) {
    char url[4096], err[256];
    snprintf(url, sizeof url, "file:%s", path);
    AVFormatContext* fmt = NULL;
    int r = avformat_open_input(&fmt, url, NULL, NULL);
    if (r != 0) { av_strerror(r, err, sizeof err); printf("  ffmpeg open: %s\n", err); return; }
    r = avformat_find_stream_info(fmt, NULL);
    if (r < 0) { av_strerror(r, err, sizeof err); printf("  ffmpeg stream info: %s\n", err); }
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        printf("  stream %u: %s %s\n", i, av_get_media_type_string(fmt->streams[i]->codecpar->codec_type), avcodec_get_name(fmt->streams[i]->codecpar->codec_id));
    const AVCodec* codec = NULL;
    const int s = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (s < 0 || !codec) { av_strerror(s, err, sizeof err); printf("  best audio stream: %s\n", err); avformat_close_input(&fmt); return; }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[s]->codecpar);
    r = avcodec_open2(dec, codec, NULL);
    if (r < 0) { av_strerror(r, err, sizeof err); printf("  decoder open: %s\n", err); }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    int frames = 0;
    while (frames < 20 && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == s && avcodec_send_packet(dec, pkt) >= 0)
            while (avcodec_receive_frame(dec, fr) >= 0) frames++;
        av_packet_unref(pkt);
    }
    printf("  decoded %d frames, %d Hz, %d channels\n", frames, dec->sample_rate, dec->ch_layout.nb_channels);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        printf("%s\n", argv[i]);
        FILE* f = fopen(argv[i], "rb");
        printf("  fopen: %s\n", f ? "ok" : strerror(errno));
        if (f) fclose(f);
        ff(argv[i]);
        FLAC__StreamMetadata* tags = NULL;
        if (strstr(argv[i], ".flac")) printf("  libFLAC tags: %s\n", FLAC__metadata_get_tags(argv[i], &tags) ? "ok" : "failed");
        if (tags) FLAC__metadata_object_delete(tags);
    }
    return 0;
}
