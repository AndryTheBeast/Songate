/*
 * Songate · net.cpp
 * See net.hpp. Console: libcurl (portlibs, built on libnx's ssl service: the console's own TLS and trusted
 * certificates, so no CA bundle travels with Songate). Preview: GF_NET=<folder>.
 */
#include "words.hpp"
#include "net.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>

#ifndef GF_VERSION
#define GF_VERSION "2.0.0 BETA 2"
#endif

namespace net {
namespace {

/** Written next to the file and renamed over it once whole. */
bool finish(const std::string& tmp, const std::string& file) {
    remove(file.c_str());
    return rename(tmp.c_str(), file.c_str()) == 0;
}

}  // namespace
}  // namespace net

#ifdef GF_CURL
#include <curl/curl.h>

#include "platform.hpp"

namespace net {
namespace {

std::mutex gate;
bool started = false;

struct Sink {
    std::string* str = nullptr;
    FILE* file = nullptr;
    Progress* p = nullptr;
    std::string* date = nullptr;   // the Date header, when wanted
    int* retry = nullptr;          // the Retry-After header's seconds, when wanted
};

size_t onData(char* data, size_t size, size_t n, void* user) {
    Sink* s = static_cast<Sink*>(user);
    const size_t len = size * n;
    if (s->str) s->str->append(data, len);
    if (s->file && fwrite(data, 1, len, s->file) != len) return 0;   // the card is full: curl stops with a write error
    return len;
}

size_t onHeader(char* data, size_t size, size_t n, void* user) {
    Sink* s = static_cast<Sink*>(user);
    const size_t len = size * n;
    static const char kRetry[] = "retry-after:";
    bool isRetry = s->retry && len > 12;
    for (int i = 0; isRetry && i < 12; i++) isRetry = char(data[i] | (data[i] >= 'A' && data[i] <= 'Z' ? 32 : 0)) == kRetry[i];
    if (isRetry) *s->retry = std::max(0, atoi(std::string(data + 12, len - 12).c_str()));
    if (s->date && len > 5 && (data[0] | 32) == 'd' && (data[1] | 32) == 'a' && (data[2] | 32) == 't' && (data[3] | 32) == 'e' && data[4] == ':') {
        s->date->assign(data + 5, len - 5);
        while (!s->date->empty() && (s->date->back() == '\r' || s->date->back() == '\n' || s->date->back() == ' ')) s->date->pop_back();
    }
    return len;
}

int onProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    Progress* p = static_cast<Progress*>(user);
    if (!p) return 0;
    if (total > 0) p->total = int64_t(total);
    p->done = int64_t(now);
    return p->cancel ? 1 : 0;
}

/** What went wrong, in words the Settings row can show. */
std::string explain(CURLcode rc, long status) {
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT: return "No internet connection";
    case CURLE_OPERATION_TIMEDOUT: return "The connection timed out";
    case CURLE_ABORTED_BY_CALLBACK: return "Stopped";
    case CURLE_WRITE_ERROR: return "Could not write to " GF_THE_CARD " (is it full?)";
    case CURLE_PEER_FAILED_VERIFICATION: return "The server's certificate could not be checked";
    case CURLE_HTTP_RETURNED_ERROR:
        if (status == 401) return "GitHub refused this build's key for reports";
        if (status == 403 || status == 429) return "GitHub says to wait a while (too many checks this hour)";
        if (status == 404) return "Not found on GitHub";
        return "GitHub answered " + std::to_string(status);
    default: return std::string("Network error: ") + curl_easy_strerror(rc);
    }
}

bool run(const std::string& url, Sink& sink, std::string& err, const std::string* json = nullptr, const char* secret = nullptr,
         const std::string* form = nullptr, long* statusOut = nullptr, bool keepErrors = false) {
    std::string e;
    if (!begin(e)) { err = e; return false; }
    CURL* c = curl_easy_init();
    if (!c) { err = "The network library did not start"; return false; }
    curl_slist* headers = nullptr;
    if (url.find("api.github.com") != std::string::npos) headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    if (json) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, json->c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(json->size()));
    }
    if (form) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, form->c_str());   // (application/x-www-form-urlencoded by itself)
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(form->size()));
    }
    if (secret) headers = curl_slist_append(headers, secret);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    static const std::string agent = [] {   // "Songate/2.0.0-BETA (…)": a product token has no spaces; the homepage, as LRCLIB asks
        std::string s = "Songate/" GF_VERSION;
        for (char& ch : s) if (ch == ' ') ch = '-';
        return s + " (https://github.com/AndryTheBeast/Songate)";
    }();
    curl_easy_setopt(c, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   // release assets redirect to GitHub's file host
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    if (!form && !keepErrors) curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);   // (a form's errors are in the body)
    if (sink.date || sink.retry) {
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &sink);
    }
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);   // under 64 bytes a second for 30 s: give up
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, sink.p);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 128L * 1024);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (statusOut) *statusOut = status;
    curl_easy_cleanup(c);
    for (curl_slist* h = headers; h; h = h->next)   // (the secret's copy: wiped before it is freed)
        if (h->data) { volatile char* d = h->data; while (*d) *d++ = 0; }
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) { err = explain(rc, status); return false; }
    return true;
}

}  // namespace

bool begin(std::string& err) {
    std::lock_guard<std::mutex> g(gate);
    if (started) return true;
    if (!plat::netStart()) { err = GF_CAP_THE_CONSOLE "'s network did not start"; return false; }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        plat::netStop();
        err = "The network library did not start";
        return false;
    }
    started = true;
    return true;
}

void end() {
    std::lock_guard<std::mutex> g(gate);
    if (!started) return;
    curl_global_cleanup();
    plat::netStop();
    started = false;
}

bool get(const std::string& url, std::string& out, std::string& err, Progress* p) {
    out.clear();
    Sink s;
    s.str = &out;
    s.p = p;
    return run(url, s, err);
}

bool post(const std::string& url, const std::string& json, char* secret, std::string& out, std::string& err) {
    out.clear();
    Sink s;
    s.str = &out;
    const bool ok = run(url, s, err, &json, secret);
    if (secret) { volatile char* d = secret; while (*d) *d++ = 0; }
    return ok;
}

bool form(const std::string& url, const std::string& body, std::string& out, long& status, std::string& date, std::string& err, Progress* p) {
    out.clear();
    date.clear();
    status = 0;
    Sink s;
    s.str = &out;
    s.p = p;
    s.date = &date;
    return run(url, s, err, nullptr, nullptr, &body, &status);
}

bool lookup(const std::string& url, std::string& out, long& status, int& retryAfter, std::string& err) {
    out.clear();
    status = 0;
    retryAfter = 0;
    Sink s;
    s.str = &out;
    s.retry = &retryAfter;
    return run(url, s, err, nullptr, nullptr, nullptr, &status, true);
}

bool download(const std::string& url, const std::string& file, std::string& err, Progress* p) {
    const std::string tmp = file + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = "Could not write to " GF_THE_CARD; return false; }
    setvbuf(f, nullptr, _IOFBF, 256 * 1024);
    Sink s;
    s.file = f;
    s.p = p;
    const bool ok = run(url, s, err);
    if (fclose(f) != 0 && ok) { err = "Could not write to " GF_THE_CARD " (is it full?)"; remove(tmp.c_str()); return false; }
    if (!ok) { remove(tmp.c_str()); return false; }
    if (!finish(tmp, file)) { err = "Could not write to " GF_THE_CARD; return false; }
    return true;
}

}  // namespace net

#else  // the PC preview: GF_NET=<folder> stands in for GitHub

#include <SDL2/SDL.h>
#include <windows.h>
#include <wininet.h>

namespace net {
namespace {

/** api.github.com/repos/…/releases → <folder>/releases.json; …/download/<tag>/<name> → <folder>/<tag>/<name>. */
bool local(const std::string& url, std::string& path, std::string& err) {
    const char* root = getenv("GF_NET");
    if (!root || !*root) { err = "No internet connection"; return false; }
    if (url.find("api.github.com") != std::string::npos) { path = std::string(root) + "/releases.json"; return true; }
    const size_t b = url.find_last_of('/');
    const size_t a = b == std::string::npos || b == 0 ? std::string::npos : url.find_last_of('/', b - 1);
    if (a == std::string::npos) { err = "Not found on GitHub"; return false; }
    path = std::string(root) + "/" + url.substr(a + 1);
    return true;
}

}  // namespace

/** The preview: LRCLIB's answers from <GF_NET>/lrclib/get.json and search.json, 404 when a file is not there; GF_NET_429
 *  answers the first lookup "too many" (Retry-After 2). */
bool lookup(const std::string& url, std::string& out, long& status, int& retryAfter, std::string& err) {
    out.clear();
    status = 0;
    retryAfter = 0;
    const char* root = getenv("GF_NET");
    if (!root || !*root) { err = "No internet connection"; return false; }
    static bool told429 = false;
    if (getenv("GF_NET_429") && !told429) { told429 = true; status = 429; retryAfter = 2; return true; }
    if (const char* d = getenv("GF_NET_DELAY")) SDL_Delay(Uint32(atoi(d)));   // a slow answer (ms): the song changed meanwhile
    const char* kind = url.find("/api/search") != std::string::npos ? "search.json" : "get.json";
    FILE* f = fopen((std::string(root) + "/lrclib/" + kind).c_str(), "rb");
    if (!f) { status = 404; out = "{\"code\":404,\"name\":\"TrackNotFound\"}"; return true; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    status = 200;
    // (the asked URL, for tests: what was sent)
    if (FILE* log = fopen((std::string(root) + "/lrclib/asked.txt").c_str(), "ab")) { fprintf(log, "%s\n", url.c_str()); fclose(log); }
    return true;
}

bool begin(std::string& err) {
    const char* root = getenv("GF_NET");
    if (!root || !*root) { err = "No internet connection"; return false; }
    return true;
}

void end() {}

/** The preview: a report is written to <GF_NET>/reports/report-<n>.json and answered as GitHub would, with its number. */
bool post(const std::string& url, const std::string& json, char* secret, std::string& out, std::string& err) {
    const char* root = getenv("GF_NET");
    if (secret) { volatile char* d = secret; while (*d) *d++ = 0; }
    if (!root || !*root) { err = "No internet connection"; return false; }
    const std::string dir = std::string(root) + "/reports";
    mkdir(dir.c_str(), 0777);   // (the shim's)
    int n = 1;
    for (;; n++) {
        FILE* t = fopen((dir + "/report-" + std::to_string(n) + ".json").c_str(), "rb");
        if (!t) break;
        fclose(t);
    }
    FILE* f = fopen((dir + "/report-" + std::to_string(n) + ".json").c_str(), "wb");
    if (!f) { err = "Could not write the report"; return false; }
    fwrite(json.data(), 1, json.size(), f);
    fclose(f);
    (void)url;
    out = "{\"number\": " + std::to_string(n) + ", \"html_url\": \"https://github.com/AndryTheBeast/songate-reports/issues/" + std::to_string(n) + "\"}";
    SDL_Delay(400);   // a little of the wait a real request has
    return true;
}

/** The preview: <GF_NET>/lastfm/<method>.xml answers (none: a plain ok); each request is added to <GF_NET>/lastfm/sent.txt. */
bool form(const std::string& url, const std::string& body, std::string& out, long& status, std::string& date, std::string& err, Progress* p) {
    (void)url;
    (void)p;
    const char* root = getenv("GF_NET");
    if (!root || !*root) { err = "No internet connection"; return false; }
    const std::string dir = std::string(root) + "/lastfm";
    mkdir(dir.c_str(), 0777);   // (the shim's)
    if (FILE* f = fopen((dir + "/sent.txt").c_str(), "ab")) {
        fwrite(body.data(), 1, body.size(), f);
        fputc('\n', f);
        fclose(f);
    }
    std::string method;
    const size_t m = body.find("method=");
    if (m != std::string::npos) method = body.substr(m + 7, body.find('&', m) == std::string::npos ? std::string::npos : body.find('&', m) - m - 7);
    out = "<lfm status=\"ok\"></lfm>";
    if (FILE* f = fopen((dir + "/" + method + ".xml").c_str(), "rb")) {
        out.clear();
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        fclose(f);
    }
    status = 200;
    date.clear();
    SDL_Delay(100);
    return true;
}

/** A server on this PC itself (a test Subsonic server: the user's Navidrome, 2026-11-05) is asked for real, through WinINet. */
bool live(const std::string& url, std::string& out, std::string& err) {
    HINTERNET in = InternetOpenA("Songate preview", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!in) { err = "No internet connection"; return false; }
    HINTERNET h = InternetOpenUrlA(in, url.c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!h) { InternetCloseHandle(in); err = "Can't reach the server"; return false; }
    char buf[16384];
    DWORD n = 0;
    out.clear();
    while (InternetReadFile(h, buf, sizeof buf, &n) && n > 0) out.append(buf, n);
    InternetCloseHandle(h);
    InternetCloseHandle(in);
    return true;
}

bool get(const std::string& url, std::string& out, std::string& err, Progress* p) {
    if (url.rfind("http://127.0.0.1", 0) == 0 || url.rfind("http://localhost", 0) == 0) return live(url, out, err);
    std::string path;
    if (!local(url, path, err)) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "Not found on GitHub"; return false; }
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    if (p) p->done = p->total = int64_t(out.size());
    SDL_Delay(150);   // a little of the wait a real request has
    return true;
}

bool download(const std::string& url, const std::string& file, std::string& err, Progress* p) {
    if (url.rfind("http://127.0.0.1", 0) == 0 || url.rfind("http://localhost", 0) == 0) {   // (a server on this PC: for real)
        std::string body;
        if (!live(url, body, err)) return false;
        const std::string tmp = file + ".part";
        FILE* out = fopen(tmp.c_str(), "wb");
        if (!out) { err = "Could not write to " GF_THE_CARD; return false; }
        fwrite(body.data(), 1, body.size(), out);
        fclose(out);
        if (p) p->done = p->total = int64_t(body.size());
        if (!finish(tmp, file)) { err = "Could not write to " GF_THE_CARD; return false; }
        return true;
    }
    std::string path;
    if (!local(url, path, err)) return false;
    FILE* in = fopen(path.c_str(), "rb");
    if (!in) { err = "Not found on GitHub"; return false; }
    fseek(in, 0, SEEK_END);
    if (p) { p->total = int64_t(ftell(in)); p->done = 0; }
    fseek(in, 0, SEEK_SET);
    const std::string tmp = file + ".part";
    FILE* out = fopen(tmp.c_str(), "wb");
    if (!out) { fclose(in); err = "Could not write to " GF_THE_CARD; return false; }
    char buf[32768];
    size_t n;
    bool ok = true;
    // Slowed to about 2 MB a second, so the progress card and Stop can be seen.
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (p && p->cancel) { err = "Stopped"; ok = false; break; }
        fwrite(buf, 1, n, out);
        if (p) p->done += int64_t(n);
        SDL_Delay(15);
    }
    fclose(in);
    fclose(out);
    if (!ok) { remove(tmp.c_str()); return false; }
    if (!finish(tmp, file)) { err = "Could not write to " GF_THE_CARD; return false; }
    return true;
}

}  // namespace net
#endif
