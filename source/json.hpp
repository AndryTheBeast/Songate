/*
 * Songate · json.hpp
 * A small JSON reader, just what the app's answers from the internet need (GitHub's releases, LRCLIB's lyrics): strings
 * with their escapes, numbers, true, false, null, arrays and objects, 64 levels deep at most. Moved here from updater.cpp.
 */
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace json {


struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json& operator[](const char* k) const {
        static const Json none;
        for (const auto& [key, v] : obj) if (key == k) return v;
        return none;
    }
    std::string s(const char* k) const { const Json& v = (*this)[k]; return v.kind == Str ? v.str : std::string(); }
    int64_t i(const char* k) const { const Json& v = (*this)[k]; return v.kind == Num ? int64_t(v.num) : 0; }
};

struct JsonReader {
    const char* p;
    const char* end;
    int depth = 0;
    bool fail = false;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    uint32_t hex4() {
        if (end - p < 4) { fail = true; return 0; }
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = *p++;
            v = v * 16 + uint32_t(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
        }
        return v;
    }
    std::string string() {
        std::string out;
        p++;   // the opening quote
        while (p < end && *p != '"') {
            if (*p != '\\') { out += *p++; continue; }
            if (++p >= end) break;
            const char e = *p++;
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t c = hex4();
                if (c >= 0xD800 && c < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {   // a surrogate pair
                    p += 2;
                    const uint32_t lo = hex4();
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, c);
                break;
            }
            default: out += e;
            }
        }
        if (p >= end) fail = true; else p++;
        return out;
    }
    Json value() {
        Json v;
        ws();
        if (p >= end || ++depth > 64) { fail = true; return v; }
        if (*p == '{') {
            v.kind = Json::Obj;
            p++;
            ws();
            if (p < end && *p == '}') p++;
            else
                while (!fail) {
                    ws();
                    if (p >= end || *p != '"') { fail = true; break; }
                    std::string k = string();
                    ws();
                    if (p >= end || *p != ':') { fail = true; break; }
                    p++;
                    v.obj.emplace_back(std::move(k), value());
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == '}') { p++; break; }
                    fail = true;
                }
        } else if (*p == '[') {
            v.kind = Json::Arr;
            p++;
            ws();
            if (p < end && *p == ']') p++;
            else
                while (!fail) {
                    v.arr.push_back(value());
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == ']') { p++; break; }
                    fail = true;
                }
        } else if (*p == '"') {
            v.kind = Json::Str;
            v.str = string();
        } else if (end - p >= 4 && !strncmp(p, "true", 4)) { v.kind = Json::Bool; v.b = true; p += 4; }
        else if (end - p >= 5 && !strncmp(p, "false", 5)) { v.kind = Json::Bool; p += 5; }
        else if (end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; }
        else {
            char* e = nullptr;
            const std::string num(p, size_t(std::min<ptrdiff_t>(end - p, 40)));
            v.num = strtod(num.c_str(), &e);
            if (e == num.c_str()) { fail = true; return v; }
            v.kind = Json::Num;
            p += e - num.c_str();
        }
        depth--;
        return v;
    }
};

inline bool parseJson(const std::string& text, Json& out) {
    JsonReader r{ text.data(), text.data() + text.size() };
    out = r.value();
    return !r.fail;
}

}  // namespace json
