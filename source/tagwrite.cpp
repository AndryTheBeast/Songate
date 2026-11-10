/*
 * Songate · tagwrite.cpp
 * See tagwrite.hpp.
 */
#include "tagwrite.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <vector>

#include "metadata.hpp"
#include "safefile.hpp"

#ifdef GF_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

namespace tagwrite {
namespace {

using Bytes = std::vector<uint8_t>;

std::string lowerExt(const std::string& p) {
    const size_t dot = p.find_last_of('.'), slash = p.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string e = p.substr(dot + 1);
    for (char& c : e) c = char(tolower((unsigned char)c));
    return e;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint32_t syncsafe(const uint8_t* p) { return uint32_t(p[0] & 127) << 21 | uint32_t(p[1] & 127) << 14 | uint32_t(p[2] & 127) << 7 | (p[3] & 127); }
void putBe32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
void putBe64(Bytes& b, uint64_t v) { putBe32(b, uint32_t(v >> 32)); putBe32(b, uint32_t(v)); }
void putLe32(Bytes& b, uint32_t v) { for (int s = 0; s <= 24; s += 8) b.push_back(uint8_t(v >> s)); }
void putSyncsafe(Bytes& b, uint32_t v) { for (int s = 21; s >= 0; s -= 7) b.push_back(uint8_t((v >> s) & 127)); }
void setBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void setBe64(uint8_t* p, uint64_t v) { setBe32(p, uint32_t(v >> 32)); setBe32(p + 4, uint32_t(v)); }
void add(Bytes& b, const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
void add(Bytes& b, const std::string& s) { add(b, s.data(), s.size()); }

long long fileSize(FILE* f) {
    fseek(f, 0, SEEK_END);
    const long long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    return n;
}

bool readAt(FILE* f, long long at, size_t n, Bytes& out) {
    out.resize(n);
    if (fseek(f, long(at), SEEK_SET) != 0) return false;
    return n == 0 || fread(out.data(), 1, n, f) == n;
}

/** The rest of the song, from `from` to its end, after what was written. */
bool copyRest(FILE* in, long long from, FILE* out) {
    if (fseek(in, long(from), SEEK_SET) != 0) return false;
    static thread_local Bytes buf(256 * 1024);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
        if (fwrite(buf.data(), 1, n, out) != n) return false;
    return !ferror(in);
}

/** UTF-8 → UTF-16LE (ID3v2.3's text). */
Bytes utf16le(const std::string& s) {
    Bytes o;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n > 1) { c &= (0xFF >> (n + 1)); for (int k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | ((unsigned char)s[i + k] & 63); }
        i += size_t(n);
        if (c >= 0x10000) {
            c -= 0x10000;
            const uint16_t hi = uint16_t(0xD800 + (c >> 10)), lo = uint16_t(0xDC00 + (c & 1023));
            o.push_back(uint8_t(hi)); o.push_back(uint8_t(hi >> 8)); o.push_back(uint8_t(lo)); o.push_back(uint8_t(lo >> 8));
        } else {
            o.push_back(uint8_t(c)); o.push_back(uint8_t(c >> 8));
        }
    }
    return o;
}

/** UTF-16 (little or big endian) → UTF-8. */
std::string fromUtf16(const uint8_t* p, size_t n, bool be) {
    std::string o;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = be ? (uint32_t(p[i]) << 8 | p[i + 1]) : (uint32_t(p[i + 1]) << 8 | p[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            const uint32_t lo = be ? (uint32_t(p[i + 2]) << 8 | p[i + 3]) : (uint32_t(p[i + 3]) << 8 | p[i + 2]);
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        if (c == 0) break;
        if (c < 0x80) o += char(c);
        else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
        else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    }
    return o;
}

std::string fromLatin1(const uint8_t* p, size_t n) {
    std::string o;
    for (size_t i = 0; i < n && p[i]; i++) {
        if (p[i] < 0x80) o += char(p[i]);
        else { o += char(0xC0 | (p[i] >> 6)); o += char(0x80 | (p[i] & 63)); }
    }
    return o;
}

/** ID3 text in its encoding (0 Latin-1, 1 UTF-16 with a BOM, 2 UTF-16BE, 3 UTF-8), to its first NUL. */
std::string id3Text(int enc, const uint8_t* p, size_t n) {
    if (enc == 0) return fromLatin1(p, n);
    if (enc == 3) { std::string s((const char*)p, n); return s.substr(0, s.find('\0')); }
    bool be = enc == 2;
    if (enc == 1 && n >= 2) {
        if (p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; n -= 2; }
        else if (p[0] == 0xFF && p[1] == 0xFE) { p += 2; n -= 2; }
    }
    return fromUtf16(p, n, be);
}

/** Past an ID3 string ended by its encoding's NUL (one byte, or two aligned): where what follows begins. */
size_t id3Skip(int enc, const uint8_t* p, size_t n, size_t from) {
    if (enc == 0 || enc == 3) {
        for (size_t i = from; i < n; i++) if (p[i] == 0) return i + 1;
        return n;
    }
    for (size_t i = from; i + 1 < n; i += 2) if (p[i] == 0 && p[i + 1] == 0) return i + 2;
    return n;
}

bool isPng(const std::vector<uint8_t>& b) { return b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G'; }
const char* imageMime(const std::vector<uint8_t>& b) { return isPng(b) ? "image/png" : "image/jpeg"; }

/** A JPEG's or a PNG's width and height (0 when they cannot be found: FLAC allows it). */
void imageSize(const std::vector<uint8_t>& b, uint32_t& w, uint32_t& h) {
    w = h = 0;
    if (isPng(b) && b.size() >= 24) { w = be32(&b[16]); h = be32(&b[20]); return; }
    for (size_t p = 2; p + 9 < b.size();) {
        if (b[p] != 0xFF) { p++; continue; }
        const uint8_t m = b[p + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }
        const size_t len = size_t(b[p + 2]) << 8 | b[p + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            h = uint32_t(b[p + 5]) << 8 | b[p + 6];
            w = uint32_t(b[p + 7]) << 8 | b[p + 8];
            return;
        }
        p += 2 + len;
    }
}

const std::vector<std::string>& textFields() {
    static const std::vector<std::string> f = { "title", "artist", "album", "albumArtist", "genre", "date", "trackNo", "lyrics" };
    return f;
}

/** Notes what the file held for a field, the first entry found (an empty Before entry is made for every field that changes). */
void noteBefore(Before* b, const std::string& field, const std::string& value) {
    if (!b) return;
    auto it = b->text.find(field);
    if (it == b->text.end() || it->second.empty()) b->text[field] = value;
}

// ------------------------------------------------------------------ FLAC

/** The Vorbis comment names of a field: the first is the one written; all are taken out. */
const std::vector<std::string>& flacKeys(const std::string& f) {
    static const std::map<std::string, std::vector<std::string>> k = {
        { "title", { "TITLE" } }, { "artist", { "ARTIST" } }, { "album", { "ALBUM" } },
        { "albumArtist", { "ALBUMARTIST", "ALBUM ARTIST", "ALBUM_ARTIST" } }, { "genre", { "GENRE" } }, { "date", { "DATE", "YEAR" } },
        { "trackNo", { "TRACKNUMBER" } }, { "lyrics", { "LYRICS", "UNSYNCEDLYRICS" } } };
    static const std::vector<std::string> none;
    auto it = k.find(f);
    return it == k.end() ? none : it->second;
}

bool buildFlac(FILE* in, FILE* out, const Changes& ch, Before* before, std::string& err) {
    Bytes h;
    long long at = 0;
    if (!readAt(in, 0, 10, h)) { err = "Too short to be a FLAC"; return false; }
    Bytes prefix;
    if (!memcmp(h.data(), "ID3", 3)) {   // an ID3v2 tag before "fLaC": kept as it is
        at = 10 + syncsafe(&h[6]) + ((h[5] & 0x10) ? 10 : 0);
        if (!readAt(in, 0, size_t(at), prefix)) { err = "Its ID3 tag could not be read"; return false; }
    }
    Bytes magic;
    if (!readAt(in, at, 4, magic) || memcmp(magic.data(), "fLaC", 4)) { err = "Not a FLAC"; return false; }
    at += 4;
    struct Block { int type; Bytes data; };
    std::vector<Block> blocks;
    for (bool last = false; !last;) {
        Bytes bh;
        if (!readAt(in, at, 4, bh)) { err = "Its metadata ends early"; return false; }
        last = bh[0] & 0x80;
        const int type = bh[0] & 0x7F;
        const uint32_t len = uint32_t(bh[1]) << 16 | uint32_t(bh[2]) << 8 | bh[3];
        Block b{ type, {} };
        if (!readAt(in, at + 4, len, b.data)) { err = "Its metadata ends early"; return false; }
        at += 4 + len;
        blocks.push_back(std::move(b));
        if (blocks.size() > 1000) { err = "Its metadata is damaged"; return false; }
    }
    const long long audio = at;
    long long oldMeta = 0;
    for (const auto& b : blocks) oldMeta += 4 + b.data.size();
    for (const auto& [f, v] : ch.text) { (void)v; noteBefore(before, f, ""); }
    // the comments: those of what changes out (what they held noted), the new ones in
    auto fieldOfKey = [&](const std::string& key) -> std::string {
        for (const auto& [f, v] : ch.text) { (void)v; for (const auto& k : flacKeys(f)) if (k == key) return f; }
        return {};
    };
    auto it = std::find_if(blocks.begin(), blocks.end(), [](const Block& b) { return b.type == 4; });
    std::string vendor = "Songate";
    std::vector<std::string> comments;
    if (it != blocks.end()) {
        const Bytes& d = it->data;
        size_t p = 0;
        auto need = [&](size_t n) { return p + n <= d.size(); };
        if (!need(4)) { err = "Its comments are damaged"; return false; }
        const uint32_t vl = le32(&d[p]); p += 4;
        if (!need(vl)) { err = "Its comments are damaged"; return false; }
        vendor.assign((const char*)&d[p], vl); p += vl;
        if (!need(4)) { err = "Its comments are damaged"; return false; }
        const uint32_t n = le32(&d[p]); p += 4;
        for (uint32_t i = 0; i < n; i++) {
            if (!need(4)) { err = "Its comments are damaged"; return false; }
            const uint32_t cl = le32(&d[p]); p += 4;
            if (!need(cl)) { err = "Its comments are damaged"; return false; }
            std::string c((const char*)&d[p], cl); p += cl;
            const size_t eq = c.find('=');
            std::string key = c.substr(0, eq);
            for (char& k : key) k = char(toupper((unsigned char)k));
            const std::string f = fieldOfKey(key);
            if (f.empty()) { comments.push_back(std::move(c)); continue; }
            noteBefore(before, f, eq == std::string::npos ? std::string() : c.substr(eq + 1));
        }
    }
    for (const std::string& f : textFields()) {   // (in Songate's order: the same file written twice comes out the same)
        auto c = ch.text.find(f);
        if (c != ch.text.end() && !c->second.empty() && !flacKeys(f).empty()) comments.push_back(flacKeys(f)[0] + "=" + c->second);
    }
    Bytes vc;
    putLe32(vc, uint32_t(vendor.size())); add(vc, vendor);
    putLe32(vc, uint32_t(comments.size()));
    for (const auto& c : comments) { putLe32(vc, uint32_t(c.size())); add(vc, c); }
    if (vc.size() >= (1u << 24)) { err = "The tags are too long for a FLAC comment"; return false; }
    if (it != blocks.end()) it->data = std::move(vc);
    else blocks.insert(blocks.begin() + 1, Block{ 4, std::move(vc) });   // after STREAMINFO, which is always first
    // the cover: the front cover's PICTURE (else the first) out, what it held noted, the new one in its place
    if (ch.cover) {
        if (before) { before->cover = true; before->coverBytes.clear(); }
        int pick = -1;
        for (int i = 0; i < int(blocks.size()) && pick < 0; i++) if (blocks[size_t(i)].type == 6 && blocks[size_t(i)].data.size() >= 4 && be32(blocks[size_t(i)].data.data()) == 3) pick = i;
        for (int i = 0; i < int(blocks.size()) && pick < 0; i++) if (blocks[size_t(i)].type == 6) pick = i;
        size_t where = 2;
        if (pick >= 0) {
            const Bytes& d = blocks[size_t(pick)].data;
            size_t p = 4;
            auto need = [&](size_t n) { return p + n <= d.size(); };
            if (!need(4)) { err = "Its picture is damaged"; return false; }
            const uint32_t ml = be32(&d[p]); p += 4;
            if (!need(ml + 4)) { err = "Its picture is damaged"; return false; }
            p += ml;
            const uint32_t dl = be32(&d[p]); p += 4;
            if (!need(dl + 20)) { err = "Its picture is damaged"; return false; }
            p += dl + 16;
            const uint32_t il = be32(&d[p]); p += 4;
            if (!need(il)) { err = "Its picture is damaged"; return false; }
            if (before) before->coverBytes.assign(d.begin() + long(p), d.begin() + long(p + il));
            blocks.erase(blocks.begin() + pick);
            where = size_t(pick);
        }
        if (!ch.coverBytes.empty()) {
            Bytes pic;
            uint32_t w, hh;
            imageSize(ch.coverBytes, w, hh);
            const std::string mime = imageMime(ch.coverBytes);
            putBe32(pic, 3);   // the front cover
            putBe32(pic, uint32_t(mime.size())); add(pic, mime);
            putBe32(pic, 0);   // no description
            putBe32(pic, w); putBe32(pic, hh); putBe32(pic, 24); putBe32(pic, 0);
            putBe32(pic, uint32_t(ch.coverBytes.size())); add(pic, ch.coverBytes.data(), ch.coverBytes.size());
            if (pic.size() >= (1u << 24)) { err = "The cover is too large for a FLAC (16 MB)"; return false; }
            blocks.insert(blocks.begin() + long(std::min(std::max<size_t>(where, 1), blocks.size())), Block{ 6, std::move(pic) });
        }
    }
    // the padding gives up what the changes took, so the audio starts where it did (when there is enough)
    long long newMeta = 0;
    for (const auto& b : blocks) newMeta += 4 + b.data.size();
    auto pad = std::find_if(blocks.begin(), blocks.end(), [](const Block& b) { return b.type == 1; });
    if (pad != blocks.end()) {
        const long long room = (long long)pad->data.size() - (newMeta - oldMeta);
        if (room >= 0) pad->data.assign(size_t(room), 0);
    }
    if (!prefix.empty() && fwrite(prefix.data(), 1, prefix.size(), out) != prefix.size()) return false;
    if (fwrite("fLaC", 1, 4, out) != 4) return false;
    for (size_t i = 0; i < blocks.size(); i++) {
        const uint32_t len = uint32_t(blocks[i].data.size());
        const uint8_t bh[4] = { uint8_t((i + 1 == blocks.size() ? 0x80 : 0) | blocks[i].type), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len) };
        if (fwrite(bh, 1, 4, out) != 4) return false;
        if (len && fwrite(blocks[i].data.data(), 1, len, out) != len) return false;
    }
    return copyRest(in, audio, out);
}

// ------------------------------------------------------------------ MP3 (ID3v2.3 / 2.4)

/** The field an ID3 frame holds ("" none Songate changes). */
std::string id3Field(const char* id) {
    static const std::pair<const char*, const char*> m[] = {
        { "TIT2", "title" }, { "TPE1", "artist" }, { "TALB", "album" }, { "TPE2", "albumArtist" }, { "TCON", "genre" },
        { "TDRC", "date" }, { "TYER", "date" }, { "TRCK", "trackNo" }, { "USLT", "lyrics" } };
    for (const auto& [i, f] : m) if (!memcmp(id, i, 4)) return f;
    return {};
}

bool buildMp3(FILE* in, FILE* out, const Changes& ch, Before* before, std::string& err) {
    Bytes h;
    const bool has = readAt(in, 0, 10, h) && !memcmp(h.data(), "ID3", 3);
    int major = 4;
    Bytes frames;
    long long audio = 0;
    uint32_t oldBody = 0;
    for (const auto& [f, v] : ch.text) { (void)v; noteBefore(before, f, ""); }
    if (ch.cover && before) { before->cover = true; before->coverBytes.clear(); }
    if (has) {
        major = h[3];
        if (major != 3 && major != 4) { err = "Its ID3 tag is version 2." + std::to_string(major) + ", not written"; return false; }
        const uint8_t flags = h[5];
        if (flags & 0x80) { err = "Its ID3 tag is unsynchronised, not written"; return false; }
        if (flags & 0x40) { err = "Its ID3 tag has an extended header, not written"; return false; }
        if (flags & 0x10) { err = "Its ID3 tag has a footer, not written"; return false; }
        oldBody = syncsafe(&h[6]);
        audio = 10 + oldBody;
        Bytes body;
        if (!readAt(in, 10, oldBody, body)) { err = "Its ID3 tag ends early"; return false; }
        // the cover to replace: the front cover's APIC, else the first
        long long coverAt = -1, firstApic = -1;
        for (size_t p = 0; p + 10 <= body.size();) {
            if (body[p] == 0) break;
            const uint32_t fl = major == 4 ? syncsafe(&body[p + 4]) : be32(&body[p + 4]);
            if (p + 10 + fl > body.size()) { err = "Its ID3 tag is damaged"; return false; }
            if (!memcmp(&body[p], "APIC", 4) && fl > 2) {
                if (firstApic < 0) firstApic = (long long)p;
                const int enc = body[p + 10];
                const size_t mimeEnd = id3Skip(0, &body[p + 10], fl, 1);
                if (coverAt < 0 && mimeEnd < fl && body[p + 10 + mimeEnd] == 3) coverAt = (long long)p;
                (void)enc;
            }
            p += 10 + fl;
        }
        if (coverAt < 0) coverAt = firstApic;
        for (size_t p = 0; p + 10 <= body.size();) {
            if (body[p] == 0) break;   // the padding
            const uint32_t fl = major == 4 ? syncsafe(&body[p + 4]) : be32(&body[p + 4]);
            const char* id = (const char*)&body[p];
            const std::string f = id3Field(id);
            const bool textOut = !f.empty() && ch.text.count(f), coverOut = ch.cover && (long long)p == coverAt;
            if (!textOut && !coverOut) { add(frames, &body[p], 10 + fl); p += 10 + fl; continue; }   // as it was
            if (body[p + 9] != 0) { err = "Its tag stores " + std::string(id, 4) + " compressed or encrypted, not written"; return false; }
            const uint8_t* d = &body[p + 10];
            if (textOut && fl >= 1) {
                const int enc = d[0];
                if (!memcmp(id, "USLT", 4)) {   // encoding, language, a description, the lyrics
                    if (fl >= 4) { const size_t t = id3Skip(enc, d, fl, 4); noteBefore(before, f, id3Text(enc, d + t, fl - t)); }
                } else {
                    noteBefore(before, f, id3Text(enc, d + 1, fl - 1));
                }
            }
            if (coverOut && before && fl > 2) {   // encoding, its type's name, the picture's kind, a description, the image
                const int enc = d[0];
                size_t q = id3Skip(0, d, fl, 1);
                q = id3Skip(enc, d, fl, q + 1);
                if (q <= fl) before->coverBytes.assign(d + q, d + fl);
            }
            p += 10 + fl;
        }
    }
    bool adds = false;
    for (const auto& [f, v] : ch.text) { (void)f; adds = adds || !v.empty(); }
    adds = adds || (ch.cover && !ch.coverBytes.empty());
    if (!has && !adds) return copyRest(in, 0, out);   // nothing to take out, nothing to put in
    auto frame = [&](const char* id, const Bytes& data) {
        add(frames, id, 4);
        if (major == 4) putSyncsafe(frames, uint32_t(data.size())); else putBe32(frames, uint32_t(data.size()));
        frames.push_back(0); frames.push_back(0);
        add(frames, data.data(), data.size());
    };
    auto textData = [&](const std::string& s) {   // 2.4 in UTF-8; 2.3 in UTF-16 with its BOM
        Bytes d;
        if (major == 4) { d.push_back(3); add(d, s); }
        else { d.push_back(1); const uint8_t bom[2] = { 0xFF, 0xFE }; add(d, bom, 2); const Bytes t = utf16le(s); add(d, t.data(), t.size()); }
        return d;
    };
    for (const std::string& f : textFields()) {
        auto c = ch.text.find(f);
        if (c == ch.text.end() || c->second.empty()) continue;
        const std::string& v = c->second;
        if (f == "lyrics") {   // encoding, language, an empty description, the lyrics
            Bytes d;
            if (major == 4) { d.push_back(3); add(d, "eng", 3); d.push_back(0); add(d, v); }
            else {
                d.push_back(1); add(d, "eng", 3);
                const uint8_t bomEnd[4] = { 0xFF, 0xFE, 0, 0 }; add(d, bomEnd, 4);
                const uint8_t bom[2] = { 0xFF, 0xFE }; add(d, bom, 2);
                const Bytes t = utf16le(v); add(d, t.data(), t.size());
            }
            frame("USLT", d);
            continue;
        }
        const char* id = f == "title" ? "TIT2" : f == "artist" ? "TPE1" : f == "album" ? "TALB" : f == "albumArtist" ? "TPE2"
                       : f == "genre" ? "TCON" : f == "trackNo" ? "TRCK" : major == 4 ? "TDRC" : "TYER";
        frame(id, textData(f == "date" && major == 3 ? v.substr(0, 4) : v));   // (2.3's TYER: the year only)
    }
    if (ch.cover && !ch.coverBytes.empty()) {   // Latin-1, its type, the front cover, no description, the image
        Bytes d;
        d.push_back(0);
        add(d, std::string(imageMime(ch.coverBytes)));
        d.push_back(0);
        d.push_back(3);
        d.push_back(0);
        add(d, ch.coverBytes.data(), ch.coverBytes.size());
        frame("APIC", d);
    }
    // in the old padding when it fits (the audio where it was), else with 2 KB of room for next time
    const uint32_t body = frames.size() <= oldBody ? oldBody : uint32_t(frames.size() + 2048);
    if (body >= (1u << 28)) { err = "The tags are too long for an ID3 tag"; return false; }
    Bytes head;
    add(head, "ID3", 3);
    head.push_back(uint8_t(major)); head.push_back(0); head.push_back(0);
    putSyncsafe(head, body);
    frames.resize(body, 0);
    if (fwrite(head.data(), 1, head.size(), out) != head.size()) return false;
    if (fwrite(frames.data(), 1, frames.size(), out) != frames.size()) return false;
    return copyRest(in, audio, out);
}

// ------------------------------------------------------------------ M4A

struct Atom {
    std::string type;
    Bytes body;                 // what follows the header (a container's children, raw)
    std::vector<Atom> kids;     // parsed, for the containers walked into
    bool parsed = false;
};

/** The atoms in [p, end) of b. False when they do not add up. */
bool parseAtoms(const Bytes& b, size_t p, size_t end, std::vector<Atom>& out) {
    while (p < end) {
        if (end - p < 8) return false;
        uint64_t size = be32(&b[p]);
        size_t hdr = 8;
        if (size == 1) { if (end - p < 16) return false; size = be64(&b[p + 8]); hdr = 16; }
        else if (size == 0) size = end - p;
        if (size < hdr || size > end - p) return false;
        Atom a;
        a.type.assign((const char*)&b[p + 4], 4);
        a.body.assign(b.begin() + long(p + hdr), b.begin() + long(p + size));
        out.push_back(std::move(a));
        p += size_t(size);
    }
    return true;
}

bool walk(Atom& a, size_t skip = 0) {   // its children parsed (skip: a full box's version and flags, 'meta')
    if (a.parsed) return true;
    if (a.body.size() < skip) return false;
    a.parsed = parseAtoms(a.body, skip, a.body.size(), a.kids);
    return a.parsed;
}

Bytes serialise(const Atom& a, size_t skip = 0) {
    Bytes inner;
    if (a.parsed) {
        add(inner, a.body.data(), skip);   // a full box's version and flags
        for (const auto& k : a.kids) {
            const bool full = k.type == "meta";
            const Bytes kb = serialise(k, full ? 4 : 0);
            add(inner, kb.data(), kb.size());
        }
    } else {
        inner = a.body;
    }
    Bytes out;
    if (inner.size() + 8 <= 0xFFFFFFFFull) putBe32(out, uint32_t(inner.size() + 8));
    else { putBe32(out, 1); }
    add(out, a.type);
    if (inner.size() + 8 > 0xFFFFFFFFull) putBe64(out, inner.size() + 16);
    add(out, inner.data(), inner.size());
    return out;
}

Atom* child(Atom& a, const char* type) {
    for (auto& k : a.kids) if (k.type == type) return &k;
    return nullptr;
}

/** Every chunk offset in moov's tracks moved by delta (the audio after moov moved). */
bool shiftOffsets(Atom& moov, long long delta, std::string& err) {
    for (auto& trak : moov.kids) {
        if (trak.type != "trak") continue;
        Atom* mdia = walk(trak) ? child(trak, "mdia") : nullptr;
        Atom* minf = mdia && walk(*mdia) ? child(*mdia, "minf") : nullptr;
        Atom* stbl = minf && walk(*minf) ? child(*minf, "stbl") : nullptr;
        if (!stbl || !walk(*stbl)) continue;
        for (auto& t : stbl->kids) {
            const bool co64 = t.type == "co64";
            if (t.type != "stco" && !co64) continue;
            if (t.body.size() < 8) { err = "Its chunk table is damaged"; return false; }
            const uint32_t n = be32(&t.body[4]);
            const size_t w = co64 ? 8 : 4;
            if (t.body.size() < 8 + size_t(n) * w) { err = "Its chunk table is damaged"; return false; }
            for (uint32_t i = 0; i < n; i++) {
                uint8_t* e = &t.body[8 + size_t(i) * w];
                const long long v = co64 ? (long long)be64(e) : (long long)be32(e);
                const long long nv = v + delta;
                if (nv < 0 || (!co64 && nv > 0xFFFFFFFFll)) { err = "Its audio would move too far for its chunk table"; return false; }
                if (co64) setBe64(e, uint64_t(nv)); else setBe32(e, uint32_t(nv));
            }
        }
    }
    return true;
}

/** An ilst item's name for a field ("" none). */
std::string m4aItem(const std::string& f) {
    if (f == "title") return "\xA9nam";
    if (f == "artist") return "\xA9" "ART";
    if (f == "album") return "\xA9" "alb";
    if (f == "albumArtist") return "aART";
    if (f == "genre") return "\xA9gen";
    if (f == "date") return "\xA9" "day";
    if (f == "trackNo") return "trkn";
    if (f == "lyrics") return "\xA9lyr";
    return {};
}

bool buildM4a(FILE* in, FILE* out, const Changes& ch, Before* before, std::string& err) {
    const long long total = fileSize(in);
    struct Top { std::string type; long long at, size; };
    std::vector<Top> tops;
    for (long long p = 0; p < total;) {
        Bytes h;
        if (!readAt(in, p, 8, h)) { err = "Its atoms end early"; return false; }
        long long size = be32(h.data());
        if (size == 1) { Bytes x; if (!readAt(in, p + 8, 8, x)) { err = "Its atoms end early"; return false; } size = (long long)be64(x.data()); }
        else if (size == 0) size = total - p;
        if (size < 8 || p + size > total) { err = "Its atoms do not add up"; return false; }
        tops.push_back({ std::string((const char*)&h[4], 4), p, size });
        p += size;
    }
    auto find = [&](const char* t) { for (size_t i = 0; i < tops.size(); i++) if (tops[i].type == t) return int(i); return -1; };
    if (find("moof") >= 0) { err = "A fragmented M4A, not written"; return false; }
    const int mi = find("moov"), di = find("mdat");
    if (mi < 0 || di < 0) { err = "Not an M4A Songate can write"; return false; }
    if (tops[size_t(mi)].size > 64ll * 1024 * 1024) { err = "Its index is too large"; return false; }
    Bytes raw;
    if (!readAt(in, tops[size_t(mi)].at, size_t(tops[size_t(mi)].size), raw)) { err = "Its index could not be read"; return false; }
    std::vector<Atom> one;
    if (!parseAtoms(raw, 0, raw.size(), one) || one.size() != 1) { err = "Its index is damaged"; return false; }
    Atom moov = std::move(one[0]);
    if (!walk(moov)) { err = "Its index is damaged"; return false; }
    // moov/udta/meta/ilst, made where missing
    Atom* udta = child(moov, "udta");
    if (!udta) { Atom a; a.type = "udta"; a.parsed = true; moov.kids.push_back(a); udta = &moov.kids.back(); }
    if (!walk(*udta)) { err = "Its tags are damaged"; return false; }
    Atom* meta = child(*udta, "meta");
    if (!meta) {
        Atom a; a.type = "meta"; a.parsed = true; a.body.assign(4, 0);
        Atom hd; hd.type = "hdlr";
        const uint8_t hdlr[25] = { 0, 0, 0, 0, 0, 0, 0, 0, 'm', 'd', 'i', 'r', 'a', 'p', 'p', 'l', 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        add(hd.body, hdlr, sizeof hdlr);
        a.kids.push_back(hd);
        udta->kids.push_back(a);
        meta = &udta->kids.back();
    }
    if (!walk(*meta, 4)) { err = "Its tags are damaged"; return false; }
    Atom* ilst = child(*meta, "ilst");
    if (!ilst) { Atom a; a.type = "ilst"; a.parsed = true; meta->kids.push_back(a); ilst = &meta->kids.back(); }
    if (!walk(*ilst)) { err = "Its tags are damaged"; return false; }
    for (const auto& [f, v] : ch.text) { (void)v; noteBefore(before, f, ""); }
    if (ch.cover && before) { before->cover = true; before->coverBytes.clear(); }
    // an item's first 'data': its kind (1 text, 0 numbers, 13 JPEG, 14 PNG) and what follows its locale
    auto dataOf = [](Atom& item, uint32_t& kind, Bytes& payload) {
        if (!walk(item)) return false;
        Atom* d = child(item, "data");
        if (!d || d->body.size() < 8) return false;
        kind = be32(d->body.data()) & 0xFFFFFF;
        payload.assign(d->body.begin() + 8, d->body.end());
        return true;
    };
    // what changes out, what it held noted
    std::vector<Atom> kept;
    bool coverTaken = false;
    for (auto& k : ilst->kids) {
        std::string field;
        for (const auto& [f, v] : ch.text) { (void)v; if (m4aItem(f) == k.type || (f == "genre" && k.type == "gnre")) field = f; }
        const bool coverOut = ch.cover && k.type == "covr" && !coverTaken;
        if (field.empty() && !coverOut) { kept.push_back(std::move(k)); continue; }
        uint32_t kind = 0;
        Bytes p;
        if (dataOf(k, kind, p)) {
            if (coverOut) { coverTaken = true; if (before) before->coverBytes = p; }
            else if (k.type == "trkn") { if (p.size() >= 4) noteBefore(before, field, std::to_string(uint32_t(p[2]) << 8 | p[3])); }
            else if (k.type == "gnre") { if (p.size() >= 2) noteBefore(before, field, cleanGenre(std::to_string(int(uint32_t(p[0]) << 8 | p[1]) - 1))); }
            else noteBefore(before, field, std::string(p.begin(), p.end()));
        }
    }
    ilst->kids = std::move(kept);
    // the new items
    auto item = [&](const std::string& type, uint32_t kind, const Bytes& payload) {
        Atom it; it.type = type; it.parsed = true;
        Atom d; d.type = "data";
        putBe32(d.body, kind);
        putBe32(d.body, 0);   // no locale
        add(d.body, payload.data(), payload.size());
        it.kids.push_back(d);
        ilst->kids.push_back(it);
    };
    for (const std::string& f : textFields()) {
        auto c = ch.text.find(f);
        if (c == ch.text.end() || c->second.empty()) continue;
        if (f == "trackNo") {
            const int n = std::clamp(atoi(c->second.c_str()), 0, 65535);
            const Bytes p = { 0, 0, uint8_t(n >> 8), uint8_t(n), 0, 0, 0, 0 };
            item("trkn", 0, p);
        } else {
            Bytes p;
            add(p, c->second);
            item(m4aItem(f), 1, p);
        }
    }
    if (ch.cover && !ch.coverBytes.empty()) item("covr", isPng(ch.coverBytes) ? 14 : 13, ch.coverBytes);
    // what moov grew by; a 'free' in meta, or right after moov, gives it up
    long long delta = (long long)serialise(moov).size() - tops[size_t(mi)].size;
    if (delta != 0)
        for (auto& k : meta->kids)
            if ((k.type == "free" || k.type == "skip") && !k.parsed && (long long)k.body.size() >= delta) {
                k.body.resize(size_t((long long)k.body.size() - delta));
                delta = 0;
                break;
            }
    int freeAfter = -1;
    if (delta > 0 && mi + 1 < int(tops.size()) && (tops[size_t(mi) + 1].type == "free" || tops[size_t(mi) + 1].type == "skip") &&
        tops[size_t(mi) + 1].size - 8 >= delta)
        freeAfter = mi + 1;
    const bool audioAfter = tops[size_t(di)].at > tops[size_t(mi)].at;
    if (delta != 0 && freeAfter < 0 && audioAfter && !shiftOffsets(moov, delta, err)) return false;
    const Bytes nm = serialise(moov);
    // the file: what was before moov, the new moov, (a smaller free), the rest as it was
    for (size_t i = 0; i < tops.size(); i++) {
        if (int(i) == mi) {
            if (fwrite(nm.data(), 1, nm.size(), out) != nm.size()) return false;
            continue;
        }
        if (int(i) == freeAfter) {
            Bytes f;
            putBe32(f, uint32_t(tops[i].size - delta));
            add(f, tops[i].type);
            f.resize(size_t(tops[i].size - delta), 0);
            if (fwrite(f.data(), 1, f.size(), out) != f.size()) return false;
            continue;
        }
        if (fseek(in, long(tops[i].at), SEEK_SET) != 0) return false;
        static thread_local Bytes buf(256 * 1024);
        for (long long left = tops[i].size; left > 0;) {
            const size_t n = size_t(std::min<long long>(left, (long long)buf.size()));
            if (fread(buf.data(), 1, n, in) != n || fwrite(buf.data(), 1, n, out) != n) return false;
            left -= (long long)n;
        }
    }
    return true;
}

std::string norm(std::string s) {   // (what a reader gives back may lose a final newline or turn \r\n into \n; spaces at the ends)
    std::string t;   // (no std::remove: the PC's shim makes remove a macro for files)
    for (char c : s) if (c != '\r') t += c;
    s.swap(t);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && s[a] == ' ') a++;
    return s.substr(a);
}

/** A field as readTrackInfo gives it back. */
std::string infoField(const TrackInfo& t, const std::string& f) {
    if (f == "title") return t.title;
    if (f == "artist") return t.artist;
    if (f == "album") return t.album;
    if (f == "albumArtist") return t.albumArtist;
    if (f == "genre") return t.genre;
    if (f == "date") return t.date;
    if (f == "trackNo") return t.trackNo ? std::to_string(t.trackNo) : std::string();
    if (f == "lyrics") return t.lyrics;
    return {};
}

/** The two the same, as a reader shows them: a date by its year (ID3v2.3 keeps the year only), a track number as a number. */
bool sameField(const std::string& f, const std::string& a, const std::string& b) {
    const std::string x = norm(a), y = norm(b);
    if (f == "date") return x.substr(0, 4) == y.substr(0, 4);
    if (f == "trackNo") return atoi(x.c_str()) == atoi(y.c_str());
    if (f == "genre") return cleanGenre(x) == cleanGenre(y);
    return x == y;
}

}  // namespace

bool buildWithChanges(const std::string& song, const std::string& outPath, const Changes& ch, Before* before, std::string& err) {
    const std::string ext = lowerExt(song);
    if (ext != "flac" && ext != "mp3" && ext != "m4a") { err = "Only MP3, M4A and FLAC are written"; return false; }
    for (const auto& [f, v] : ch.text) { (void)v; if (std::find(textFields().begin(), textFields().end(), f) == textFields().end()) { err = "Not a field Songate writes: " + f; return false; } }
    FILE* in = fopen(song.c_str(), "rb");
    if (!in) { err = "The song could not be read"; return false; }
    FILE* out = fopen(outPath.c_str(), "wb");
    if (!out) { fclose(in); err = "Could not write beside the song"; return false; }
    std::string why;
    if (before) *before = Before{};
    bool ok = ext == "flac" ? buildFlac(in, out, ch, before, why) : ext == "mp3" ? buildMp3(in, out, ch, before, why) : buildM4a(in, out, ch, before, why);
    fclose(in);
    if (fclose(out) != 0 && ok) { ok = false; why = "Could not write beside the song (is the card full?)"; }
    if (!ok) { err = why.empty() ? "Could not write beside the song (is the card full?)" : why; remove(outPath.c_str()); }
    return ok;
}

bool buildWithLyrics(const std::string& song, const std::string& outPath, const std::string& lyrics, std::string& err) {
    Changes c;
    c.text["lyrics"] = lyrics;
    return buildWithChanges(song, outPath, c, nullptr, err);
}

bool sameAudio(const std::string& a, const std::string& b, std::string& err) {
#ifdef GF_FFMPEG
    AVFormatContext* fa = nullptr;
    AVFormatContext* fb = nullptr;
    const std::string ua = "file:" + a, ub = "file:" + b;
    bool ok = false;
    if (avformat_open_input(&fa, ua.c_str(), nullptr, nullptr) == 0 && avformat_open_input(&fb, ub.c_str(), nullptr, nullptr) == 0 &&
        avformat_find_stream_info(fa, nullptr) >= 0 && avformat_find_stream_info(fb, nullptr) >= 0) {
        const int sa = av_find_best_stream(fa, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0), sb = av_find_best_stream(fb, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        AVPacket* pa = av_packet_alloc();
        AVPacket* pb = av_packet_alloc();
        auto next = [](AVFormatContext* f, int s, AVPacket* p) {
            for (;;) {
                av_packet_unref(p);
                if (av_read_frame(f, p) < 0) return false;
                if (p->stream_index == s) return true;
            }
        };
        long packets = 0;
        if (sa >= 0 && sb >= 0) {
            ok = true;
            for (;;) {
                const bool ga = next(fa, sa, pa), gb = next(fb, sb, pb);
                if (ga != gb) { ok = false; err = "The copy's audio ends at another place"; break; }
                if (!ga) break;
                packets++;
                if (pa->size != pb->size || memcmp(pa->data, pb->data, size_t(pa->size)) != 0) {
                    ok = false;
                    err = "The copy's audio differs at packet " + std::to_string(packets);
                    break;
                }
            }
            if (ok && packets == 0) { ok = false; err = "No audio in the song"; }
        } else {
            err = "No audio stream";
        }
        av_packet_free(&pa);
        av_packet_free(&pb);
    } else {
        err = "The copy could not be opened";
    }
    if (fa) avformat_close_input(&fa);
    if (fb) avformat_close_input(&fb);
    return ok;
#else
    (void)a; (void)b;
    err = "This build cannot check audio";
    return false;
#endif
}

namespace {
const char kNew[] = ".songate-new.", kOld[] = ".songate-old.";
std::mutex& swapLock() {   // a write and a scan's settleLeftover never touch the same leftovers at once
    static std::mutex m;
    return m;
}
}  // namespace

std::string settleLeftover(const std::string& dir, const std::string& leftover) {
    std::string name;
    if (leftover.rfind(kOld, 0) == 0) name = leftover.substr(sizeof kOld - 1);
    else if (leftover.rfind(kNew, 0) == 0) name = leftover.substr(sizeof kNew - 1);
    if (name.empty()) return std::string();
    std::lock_guard<std::mutex> g(swapLock());
    const std::string song = dir + "/" + name, tmp = dir + "/" + kNew + name, old = dir + "/" + kOld + name;
    if (!safefile::exists(old)) {   // no swap had begun: an unchecked copy at most
        remove(tmp.c_str());
        return std::string();
    }
    if (safefile::exists(song)) {   // the swap went through, only the tidying did not
        remove(old.c_str());
        return std::string();
    }
    // the song stepped aside and the copy, checked before that, never took its place
    if (rename(tmp.c_str(), song.c_str()) == 0) remove(old.c_str());
    else if (rename(old.c_str(), song.c_str()) != 0) return std::string();
    return song;
}

bool putChanges(const std::string& song, const Changes& ch, Before* before, std::string& err) {
    std::lock_guard<std::mutex> g(swapLock());
    // beside the song, its extension kept (readTrackInfo goes by it), a dot first so a scan passes them by: .songate-new.Song.m4a
    const size_t slash = song.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : song.substr(0, slash + 1), name = song.substr(slash + 1);
    const std::string tmp = dir + kNew + name, old = dir + kOld + name;
    Before held;
    if (!buildWithChanges(song, tmp, ch, &held, err)) return false;
    // read back: every field as written where it changed and as it was where it did not, the cover, the length, the audio
    const bool lyr = ch.text.count("lyrics") > 0;
    TrackInfo was, now;
    readTrackInfo(song, was, true, lyr);
    readTrackInfo(tmp, now, true, lyr);
    std::string why;
    for (const std::string& f : textFields()) {
        if (f == "lyrics" && !lyr) continue;
        auto c = ch.text.find(f);
        const std::string want = c != ch.text.end() ? c->second : infoField(was, f);
        if (!sameField(f, infoField(now, f), want)) { why = f == "lyrics" ? "The lyrics did not read back" : "Its tags did not read back as written (" + f + ")"; break; }
    }
    if (why.empty()) {
        if (ch.cover && !ch.coverBytes.empty()) { if (now.cover.size() != ch.coverBytes.size()) why = "Its cover did not read back as written"; }
        else if (!ch.cover && now.cover.size() != was.cover.size()) why = "Its cover did not read back the same";
    }
    if (why.empty() && (now.duration - was.duration > 0.05 || was.duration - now.duration > 0.05)) why = "Its length did not read back the same";
    if (why.empty()) sameAudio(song, tmp, why);
    if (!why.empty()) { remove(tmp.c_str()); err = why; return false; }
    // the swap: the song aside, the copy in its place, the song gone (or back, if the copy could not move)
    remove(old.c_str());
    if (rename(song.c_str(), old.c_str()) != 0) { remove(tmp.c_str()); err = "The song is in use"; return false; }
    if (rename(tmp.c_str(), song.c_str()) != 0) {
        rename(old.c_str(), song.c_str());
        remove(tmp.c_str());
        err = "The copy could not take the song's place";
        return false;
    }
    remove(old.c_str());
    if (before) *before = std::move(held);
    return true;
}

bool putLyrics(const std::string& song, const std::string& lyrics, std::string& err) {
    Changes c;
    c.text["lyrics"] = lyrics;
    return putChanges(song, c, nullptr, err);
}

}  // namespace tagwrite
