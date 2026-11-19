/*
 * Gatefold NX · view_sheet.cpp
 * The track menu: + on any track (or ⋯ on Now playing) opens a sheet over the
 * view with what can be done to that track — queue it, like it, edit its tags,
 * give it a cover from the SD card, jump to its album, artist, genre or folder,
 * see its details, or delete the file.
 *
 * Edits and covers are kept by Gatefold (sdmc:/config/songate-nx/edits.tsv,
 * covers.tsv, covers/) and layered over the file's own tags everywhere in the
 * app: the console's FFmpeg can read but not write files, and the music stays
 * untouched — Gatefold 1.4's default for covers ("only in Gatefold") as well.
 */
#include "words.hpp"
#include <SDL2/SDL_image.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <strings.h>

#include "app.hpp"
#include "keyclick.hpp"
#include "coverio.hpp"
#include "icons.inc"

namespace {

const std::pair<const char*, const char*> kFields[] = { { "title", "Title" },       { "artist", "Artist" }, { "album", "Album" },
                                                        { "albumArtist", "Album artist" }, { "genre", "Genre" }, { "date", "Year" },
                                                        { "trackNo", "Track number" } };

std::string fieldOf(const Track& t, const std::string& k) {
    if (k == "title") return t.title;
    if (k == "artist") return t.artist;
    if (k == "album") return t.album;
    if (k == "albumArtist") return t.albumArtist;
    if (k == "genre") return t.genre;
    if (k == "date") return t.date;
    if (k == "trackNo") return t.trackNo ? std::to_string(t.trackNo) : std::string();
    return {};
}

std::string joinPath(const std::string& dir, const std::string& name) { return plat::join(dir, name); }

bool isImage(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string e = name.substr(dot + 1);
    return !strcasecmp(e.c_str(), "jpg") || !strcasecmp(e.c_str(), "jpeg") || !strcasecmp(e.c_str(), "png") || !strcasecmp(e.c_str(), "webp") ||
           !strcasecmp(e.c_str(), "bmp");
}

std::vector<uint8_t> readAll(const std::string& path) {
    std::vector<uint8_t> out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
        fclose(f);
    }
    return out;
}

std::string sizeText(double bytes) {
    char b[32];
    if (bytes >= 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", bytes / (1024 * 1024));
    else snprintf(b, sizeof b, "%.0f KB", bytes / 1024);
    return b;
}

}  // namespace

// ------------------------------------------------------------------ edits

void App::applyEdits(Track& t) {
    auto it = store.edits.find(t.path);
    if (it == store.edits.end()) return;
    for (const auto& [k, v] : it->second) {
        if (k == "title") { t.title = v; t.fromName &= ~1; }
        else if (k == "artist") { t.artist = v; t.fromName &= ~2; }
        else if (k == "album") { t.album = v; t.fromName &= ~4; }
        else if (k == "albumArtist") t.albumArtist = v;
        else if (k == "genre") t.genre = v;
        else if (k == "date") t.date = v;
        else if (k == "trackNo") t.trackNo = atoi(v.c_str());
    }
}

bool App::keyboard(const char* header, const std::string& initial, std::string& out) {
    char buf[512] = {};
    const plat::TextResult r = plat::askText(header, header, initial.c_str(), buf, sizeof buf);
    if (r == plat::TextResult::Unavailable) toast("The keyboard is not available in applet mode");
    if (r != plat::TextResult::Done) return false;   // cancelled: keep what was there
    out = buf;
    return true;
}

// ------------------------------------------------------------------ Search's keyboard

/* Search's own keyboard (the user chose "B", 2026-11-15: "a search card with an in app keyboard (switch and Android)"): a card at
 * the foot with the field and the keys; what is typed searches at once, the results showing above it. The console's and the
 * phone's alike; the PC keeps its own typing card. Touch, the D-pad and A; B takes a letter back (nothing typed: closes), Y closes,
 * + searches, X is a space. A tap above the card closes it, the results then there to touch. */
namespace {
const char* const KbLetters[4] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
const char* const KbSymbols[4] = { "1234567890", "-/:;()&@'\"", ".,?!#%+=*", "_$~<>[]" };
}  // namespace

void App::openSearchKb() {
    skb_ = {};
    skb_.open = true;
    skb_.text = route.view == View::Search ? route.query : "";
    skb_.pushed = route.view == View::Search;
    skb_.cur = skb_.text.size();
    skb_.at = time;
    skb_.focusWas = ui.focusedId();
    ui.focus(uid("skb.k", 10));   // q (the PC's pill puts the focus back: no keys there)
}

/** Songate's card for every text: the phone and the console always; the PC when a pad or a finger was the last input, its own typing
 *  (the pill, the typing card) with a keyboard and a mouse (the user, 2026-11-18) */
bool App::kbCard() const {
#if defined(GF_WINDOWED) && !defined(GF_ANDROID)
    return typingByTouch_;
#else
    return true;
#endif
}

bool App::openTextKb(const std::string& initial, std::function<void(const std::string&)> set, const char* field) {
    if (!kbCard()) return false;
    openSearchKb();
    skb_.text = initial;
    skb_.cur = initial.size();
    skb_.set = std::move(set);
    skb_.field = field;
    return true;
}

void App::typingMark(const char* field, float fx, float fy, float fw, float fh, float rad, float textX, const std::string& text, const Font& f, float maxW) {
    if (!skb_.open || strcmp(skb_.field, field) != 0) return;
    g.roundStroke(fx, fy, fw, fh, rad, 1.5f, pal.accent, 0.8f);
    const std::string before = text == skb_.text ? text.substr(0, std::min(skb_.cur, text.size())) : text;   // (the caret at the cursor)
    const float cx = textX + (before.empty() ? 0.f : std::min(g.measureF(before, f), maxW)) + 1;
    if (std::fmod(time - skb_.at, 1.06) < 0.62) g.rect(cx, fy + fh / 2 - std::min(11.f, fh / 2 - 6), 1.5f, std::min(22.f, fh - 12), pal.text);
}

void App::closeSearchKb() {
    if (!skb_.open) return;
    skb_.open = false;
#if defined(GF_WINDOWED) && !defined(GF_ANDROID)
    if (plat::textTyping().load()) { plat::textTyping() = false; SDL_StopTextInput(); }   // (the PC's pill: the keys are buttons again)
#endif
    std::string was;
    std::function<void(const std::string&)> live;
    if (skb_.kind != KbKind::Search) { was.swap(skb_.was); live = std::move(skb_.set); }
    secretWipe(skb_.text);
    skb_.done = nullptr;
    ui.focusDefault();
    if (live) live(was);   // a name or a number cancelled: what was there comes back
}

/** The card's text gone for good: a password's letters overwritten, not only freed */
void App::secretWipe(std::string& s) {
    if (!skb_.secret) return;
    volatile char* w = s.data();
    for (size_t i = 0; i < s.size(); i++) w[i] = 0;
    s.clear();
}

/** The blue key (or +): a search closes; a name or a number is kept. */
void App::finishSearchKb() {
    if (!skb_.open) return;
    if (skb_.kind == KbKind::Search) { closeSearchKb(); return; }
    skb_.open = false;
    ui.focusDefault();
    auto done = std::move(skb_.done);
    std::string t = skb_.text;
    secretWipe(skb_.text);
    skb_.set = nullptr;
    if (done) done(t);
    if (skb_.secret) { volatile char* w = t.data(); for (size_t i = 0; i < t.size(); i++) w[i] = 0; }
}

bool App::askKb(KbKind kind, const char* title, const std::string& initial, std::function<void(const std::string&)> done,
                std::function<void(const std::string&)> live, bool secret, bool symbols) {
    if (!kbCard()) return false;
    if (skb_.open) closeSearchKb();
    openSearchKb();
    skb_.kind = kind;
    skb_.title = title;
    skb_.text = skb_.was = initial;
    skb_.cur = initial.size();
    skb_.secret = secret;
    skb_.done = std::move(done);
    skb_.set = std::move(live);
    skb_.field = "name";
    const bool number = kind == KbKind::Number || kind == KbKind::Code || kind == KbKind::Minutes;
    skb_.fresh = number && !initial.empty();   // a number shows selected: the first key replaces it
    // upright a number gets the pad (the user chose E2); sideways the whole-width card on its symbols page (digits and "-")
    skb_.symbols = symbols || (number && css::W > css::H);
    if (number && css::W <= css::H) ui.focus(uid("skb.k", 4));   // the pad's 5
    return true;
}

void App::askName(const char* title, const std::string& initial, std::function<void(const std::string&)> done, bool secret, bool symbols) {
    if (askKb(KbKind::Text, title, initial, done, nullptr, secret, symbols)) return;
    std::string out;
    plat::textSecret() = secret;
    const bool ok = keyboard(title, initial, out);
    plat::textSecret() = false;
    if (ok && done) done(out);
    if (secret) { volatile char* w = out.data(); for (size_t i = 0; i < out.size(); i++) w[i] = 0; }
}

/** The text changed: searched at once, the Search page under the card (Back from it: the page the search began on). */
void App::searchKbSet(const std::string& t, size_t cur) {
    skb_.text = t;
    skb_.cur = std::min(cur, t.size());
    if (skb_.kind != KbKind::Search) { if (skb_.set) skb_.set(t); return; }   // a name; a number set as it is typed
    if (skb_.set) { skb_.set(t); return; }   // another search field: it takes the words as they come
    const Id f = ui.focusedId();
    if (t.empty()) { if (route.view == View::Search) navigate(View::Search, -1, std::string(), false); }
    else {
        navigate(View::Search, -1, t, !skb_.pushed && route.view != View::Search);
        skb_.pushed = true;
    }
    ui.focus(f);   // (navigate puts the focus on the page's first item: it stays on the key)
}

void App::searchKbType(const char* s) {
    std::string in = s;
    const size_t at = std::min(skb_.cur, skb_.text.size());
    if (skb_.kind == KbKind::Number || skb_.kind == KbKind::Code || skb_.kind == KbKind::Minutes) {   // digits only (sideways the symbols page has the rest too)
        const bool digit = in.size() == 1 && in[0] >= '0' && in[0] <= '9';
        if (!digit && !(skb_.kind == KbKind::Number && in == "-")) return;
        if (skb_.fresh) { skb_.fresh = false; skb_.text.clear(); skb_.cur = 0; }
        const size_t c = std::min(skb_.cur, skb_.text.size());
        if (skb_.kind == KbKind::Code) { if (skb_.text.size() < 6) searchKbSet(skb_.text.substr(0, c) + in + skb_.text.substr(c), c + 1); return; }
        // minutes (the sleep timer's Custom): up to three digits, never starting with 0 (the user, 2026-11-20: it took one number only)
        if (skb_.kind == KbKind::Minutes) {
            if (skb_.text.size() < 3 && !(c == 0 && in == "0")) searchKbSet(skb_.text.substr(0, c) + in + skb_.text.substr(c), c + 1);
            return;
        }
        // a dB value: "-" only first, nothing past ±12
        const bool lone0 = skb_.text == "0";   // (a lone 0 is replaced)
        const std::string had = lone0 ? std::string() : skb_.text;
        const size_t hc = lone0 ? 0 : c;
        const std::string t = had.substr(0, hc) + in + had.substr(hc);
        if (in == "-" ? (hc != 0 || had.find('-') != std::string::npos) : std::abs(atoi(t.c_str())) > 12 || t == "-0" || (hc == 0 && !had.empty() && had[0] == '-')) return;
        searchKbSet(t, hc + 1);
        return;
    }
    if (skb_.shift || skb_.caps) for (char& c : in) c = char(toupper(static_cast<unsigned char>(c)));
    skb_.shift = false;   // (one letter; caps lock stays)
    if (skb_.text.size() + in.size() < (skb_.kind == KbKind::Long ? 2000u : 200u))
        searchKbSet(skb_.text.substr(0, at) + in + skb_.text.substr(at), at + in.size());
}

void App::searchKbMove(int dir) {
    skb_.fresh = false;
    const std::string& t = skb_.text;
    size_t c = std::min(skb_.cur, t.size());
    if (dir < 0 && c > 0) {
        c--;
        while (c > 0 && (static_cast<unsigned char>(t[c]) & 0xC0) == 0x80) c--;
    } else if (dir > 0 && c < t.size()) {
        c++;
        while (c < t.size() && (static_cast<unsigned char>(t[c]) & 0xC0) == 0x80) c++;
    }
    skb_.cur = c;
    skb_.at = time - 0.7;   // (the caret shown at once where it went, not mid-blink)
}

void App::keyClickPlay(bool overMusic) {
    const int kind = std::clamp(store.keySoundKind, 0, 3);
#ifdef GF_ANDROID
    plat::keyTap(true, 0, kind, overMusic);
#else
    (void)overMusic;
    if (keyClickKind_ != kind) {   // made once a sound (keyclick.hpp), at 48 kHz in stereo, at half its strength: as loud as the phone's
        const std::vector<float> m = keyClickWave(kind, 48000);
        keyClick_.assign(m.size() * 2, 0.f);
        for (size_t i = 0; i < m.size(); i++) keyClick_[i * 2] = keyClick_[i * 2 + 1] = m[i] * 0.5f;
        keyClickKind_ = kind;
    }
    player.playOnce(&keyClick_);
#endif
}

void App::keyFeedback() {
    if (r4Dev_) return;   // (the HiBy R4: no motor, and no click: the user)
    // never over music (the user, 2026-11-18: While music plays taken out again, "the keyboard will never play sound when music is
    // playing")
    const bool click = store.keySound && !isPlaying() && !player.streaming();
#ifdef GF_ANDROID
    const int vibe = store.keyVibe ? std::clamp(store.keyVibeLevel, 1, 3) : 0;
    if (click || vibe) plat::keyTap(click, vibe, std::clamp(store.keySoundKind, 0, 3), false);
#else
    if (click) keyClickPlay(false);
    if (store.keyVibe) plat::padRumble(std::clamp(store.keyVibeLevel, 1, 3));
#endif
}

void App::searchKbShift() {
    if (skb_.caps) skb_.caps = false;
    else if (skb_.shift) { skb_.shift = false; skb_.caps = true; }
    else skb_.shift = true;
}

#if defined(GF_WINDOWED) && !defined(GF_ANDROID)
/** The PC's keys while Search's pill takes the words: ⌫ a letter back, Ctrl+⌫ all of it, Ctrl+V pasted; Enter, Esc and ↓ end the
 *  typing (↓: the focus on the first result) */
void App::pillKey(int sym, int mod) {
    const bool ctrl = mod & KMOD_CTRL;
    if (sym == SDLK_BACKSPACE) { if (ctrl) searchKbSet(std::string()); else searchKbBack(); }
    else if (ctrl && sym == SDLK_v) {
        if (char* c = SDL_GetClipboardText()) {
            std::string t = c;
            SDL_free(c);
            for (char& ch : t) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            searchKbType(t.c_str());
        }
    } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_ESCAPE || sym == SDLK_DOWN) closeSearchKb();
}
#endif

void App::searchKbBack() {
    skb_.fresh = false;
    std::string t = skb_.text;
    const size_t c = std::min(skb_.cur, t.size());
    if (c == 0) return;
    size_t i = c - 1;
    while (i > 0 && (static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) i--;   // a whole letter, the one before the cursor
    t.erase(i, c - i);
    searchKbSet(t, i);
}

void App::drawSearchKb() {
    if (!skb_.open) return;
    if (plat::textTyping().load()) return;   // the PC: typed into Search's pill, no card
    ui.region(Region::Modal);
    const float a = std::clamp(float(time - skb_.at) / 0.18f, 0.f, 1.f), lift = (1 - a) * 24;
    const float W = float(css::W), H = float(css::H);
    // the card: the whole width, upright and sideways (the user, 2026-11-15: "on landscape stretch it to full")
    const float cw = W, cx = 0;
    const float side = 10, gap = 6, kw = (cw - side * 2 - gap * 9) / 10, kh = W < 600 ? 46.f : 44.f;
    // the keys alone: what is typed shows in the page's own search bar (the user, 2026-11-15: "it should only show the keyboard
    // and the words appear on the upper search"); the page is not dimmed (the card's top corners showed it undimmed)
    // (a rounded card: the user chose "A" over a flat panel, 2026-11-15)
    // a name or a number has no field on the page: the card shows its own, under the title (session 21)
    const bool named = skb_.kind != KbKind::Search;
    const bool number = skb_.kind == KbKind::Number || skb_.kind == KbKind::Code || skb_.kind == KbKind::Minutes;
    // (sentences, the support report's: three lines that wrap, the last ones in sight as they are typed)
    const bool longText = skb_.kind == KbKind::Long;
    const float lineH = 22, boxH = longText ? 44 + 2 * lineH : 44;
    const float fieldH = named ? 14 + 18 + 8 + boxH : 0;
    const bool pad = number && W <= H;   // the number pad upright (the user chose E2); sideways the whole-width card
    // a pad's buttons shown (the user chose C, 2026-11-18): each on its key's corner, and a line at the foot for the cursor, which has
    // no key; only while a pad was the last input (a finger, a mouse: none)
    const bool hints = ui.showFocusRing() && !pad;
    const bool words = skb_.kind == KbKind::Search || skb_.kind == KbKind::Text || skb_.kind == KbKind::Long;
    const float stripH = hints ? 24.f : 0.f;
    const float cardH = fieldH + 12 + (pad ? kh * 4 + gap * 3 : kh * 5 + gap * 4) + stripH + 16;
    const float y0 = H - cardH + lift;
    // (touch only: the pad never lands there, nor its focus by default)
    if (ui.item(uid("skb.away"), { 0, 0, W, H - cardH }, 0, NoDefault | NoSideEntry | NoUpDownEntry)) closeSearchKb();   // a tap above: closed, what was found there to touch
    g.roundRect(cx, y0, cw, cardH + 30, 22, pal.surface2, a);
    g.roundStroke(cx, y0, cw, cardH + 30, 22, 1, pal.text, 0.1f * a);
    // (the page's bar follows the text: its × may have emptied it)
    if (!skb_.set && !named && route.view == View::Search && route.query != skb_.text) { skb_.text = route.query; skb_.cur = skb_.text.size(); }
    float ky = y0 + 12;
    const Font kf{ css::Md }, bf{ css::Base, Weight::SemiBold };
    const char* blue = named ? "Done" : "Search";   // (the user chose N1: "Done" for every name and number)
    if (named) {
        const float fx = cx + side + 4, fw = cw - side * 2 - 8;
        g.textC(skb_.title, { css::Sm, Weight::SemiBold }, fx + 4, ky + 2 + 9, pal.text2, a, Align::Left, int(fw - 8));
        const float fy = ky + 2 + 18 + 8;
        g.roundRect(fx, fy, fw, boxH, 12, pal.text, 0.06f * a);
        g.roundStroke(fx, fy, fw, boxH, 12, 1.5f, pal.accent, 0.8f * a);
        std::string shown;
        const size_t tc = std::min(skb_.cur, skb_.text.size());
        size_t sc = tc;   // the cursor in what is shown
        if (skb_.secret) {
            sc = 0;
            for (size_t i = 0; i < skb_.text.size(); i++) if ((static_cast<unsigned char>(skb_.text[i]) & 0xC0) != 0x80) { shown += "\xE2\x80\xA2"; if (i < tc) sc += 3; }
        }
        else if (skb_.kind == KbKind::Number && !skb_.text.empty() && skb_.text[0] == '-') { shown = "\xE2\x88\x92" + skb_.text.substr(1); if (tc > 0) sc += 2; }   // (a real minus)
        else shown = skb_.text;
        const Font ff{ css::Md };
        const float tx = fx + 14, maxW = fw - 28 - (skb_.kind == KbKind::Number || skb_.kind == KbKind::Minutes ? 30 : 0);
        const bool blink = !skb_.fresh && std::fmod(time - skb_.at, 1.06) < 0.62;
        if (longText) {   // wrapped, the last three lines in sight, the caret after the last word
            std::vector<std::string> lines = shown.empty() ? std::vector<std::string>{ std::string() } : g.wrap(shown, ff, int(maxW), 1000);
            if (lines.empty()) lines.push_back(std::string());
            // the caret: where the words up to the cursor end, wrapped alike; its line kept in sight
            const std::string pre = shown.substr(0, sc);
            std::vector<std::string> pl = pre.empty() ? std::vector<std::string>{ std::string() } : g.wrap(pre, ff, int(maxW), 1000);
            if (pl.empty()) pl.push_back(std::string());
            const size_t cl = std::min(pl.size() - 1, lines.size() - 1);
            size_t from = lines.size() > 3 ? lines.size() - 3 : 0;
            if (cl < from) from = cl;
            for (size_t k = from; k < lines.size() && k < from + 3; k++)
                g.textC(lines[k], ff, tx, fy + 11 + float(k - from) * lineH + lineH / 2, pal.text, a, Align::Left, int(maxW));
            const float lx = tx + (pl.back().empty() ? 0.f : g.measureF(pl.back(), ff)) + (!pre.empty() && pre.back() == ' ' ? g.spaceWidth(ff) : 0);
            if (blink) g.rect(lx + 1, fy + 11 + float(cl - from) * lineH, 1.5f, lineH, pal.text, a);
        } else {
            // longer than the field: the cursor in sight (where the typing is): the end, or from where L moved it
            std::string pre = shown.substr(0, sc);
            if (!skb_.fresh && g.measureF(shown, ff) > maxW) {
                size_t cut = 0;
                auto step = [&] { cut++; while (cut < shown.size() && (static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80) cut++; };
                while (cut < shown.size() && g.measureF("\xE2\x80\xA6" + shown.substr(cut), ff) > maxW) step();
                if (sc < cut) {   // the cursor before that: the words from as far back as it lets
                    cut = 0;
                    if (g.measureF(pre, ff) > maxW)
                        while (cut < sc && g.measureF("\xE2\x80\xA6" + shown.substr(cut, sc - cut), ff) > maxW) step();
                }
                if (cut > 0) { pre = "\xE2\x80\xA6" + shown.substr(cut, sc - cut); shown = "\xE2\x80\xA6" + shown.substr(cut); }
            }
            float tw = shown.empty() ? 0.f : std::min(g.measureF(shown, ff), maxW);
            if (skb_.fresh && !shown.empty()) g.roundRect(tx - 4, fy + 9, tw + 8, 26, 6, pal.accent, 0.35f * a);   // selected: a key replaces it
            g.textC(shown, ff, tx, fy + 22, pal.text, a, Align::Left, int(maxW));
            const float cw2 = skb_.fresh ? tw : pre.empty() ? 0.f : std::min(g.measureF(pre, ff), maxW);
            if (blink) g.rect(tx + cw2 + 1, fy + 11, 1.5f, 22, pal.text, a);
        }
        if (skb_.kind == KbKind::Number || skb_.kind == KbKind::Minutes)   // (the unit: the equalizer's dB, the sleep timer's min)
            g.textC(skb_.kind == KbKind::Number ? "dB" : "min", { css::Sm }, fx + fw - 14, fy + 22, pal.text2, a, Align::Right);
        ky += fieldH;
    }
    if (pad) {   // the number pad: 1–9; − and 0 (the code: 0 alone, the row's width); ⌫ and the blue key, each two rows tall, at the right
        const float pw = (cw - side * 2 - gap * 3) / 4;
        auto padKey = [&](int idx, int col, int row, int rows, int cols, const char* label, const char* icon, bool accent) {
            const float x = cx + side + col * (pw + gap), y = ky + row * (kh + gap);
            const float w = pw * cols + gap * (cols - 1), h = kh * rows + gap * (rows - 1);
            const Id id = uid("skb.k", idx);
            const bool hit = ui.item(id, { x - gap / 2, y - gap / 2, w + gap, h + gap }, 12, Key);   // (no dead gaps between keys)
            if (hit) keyFeedback();
            ringAt_[id] = { x, y, w, h };
            const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
            if (accent) g.roundRect(x, y, w, h, 12, pal.accent, a);
            else g.roundRect(x, y, w, h, 12, pal.text, (hot ? 0.16f : 0.08f) * a);
            const Rgb ink = accent ? pal.onAccent : pal.text;
            if (icon) g.icon(icon, x + w / 2, y + h / 2, 18, ink, a);
            else if (!label[0]) g.rect(x + w / 2 - 6, y + h / 2 - 1, 12, 2, ink, a);   // the minus: a bar the digits' width (the font's sat low and short)
            else g.textC(label, accent ? bf : Font{ css::Lg }, x + w / 2, y + h / 2, ink, a, Align::Center);
            return hit;
        };
        for (int d = 1; d <= 9; d++) {
            char ch[2] = { char('0' + d), 0 };
            if (padKey(d - 1, (d - 1) % 3, (d - 1) / 3, 1, 1, ch, nullptr, false)) searchKbType(ch);
        }
        if (skb_.kind == KbKind::Number) {
            if (padKey(9, 0, 3, 1, 1, "", nullptr, false)) searchKbType("-");
            if (padKey(10, 1, 3, 1, 2, "0", nullptr, false)) searchKbType("0");
        } else if (padKey(10, 0, 3, 1, 3, "0", nullptr, false)) searchKbType("0");
        if (padKey(101, 3, 0, 2, 1, nullptr, ICON_BACK, false)) searchKbBack();
        if (padKey(104, 3, 2, 2, 1, blue, nullptr, true)) finishSearchKb();
        return;
    }
    const float keysTop = ky;
    const Font hf{ css::Xs, Weight::SemiBold };
    // a button's name in a small outlined pill (the PC: its pad's own names, LB, LS, Menu)
    auto badge = [&](const char* b, float rx, float cy, const Rgb& ink, float alpha) {
#if defined(GF_WINDOWED) && !defined(GF_ANDROID)
        const std::string name = plat::capName(b);
#else
        const std::string name = b;
#endif
        // (on whole pixels and an even width: the circle and its letter on one grid, the letter centred, not half a pixel off)
        rx = std::round(rx);
        cy = std::round(cy);
        // the console: HOS's own button glyphs (its extension font; the codes Ultrahand's table gives), as its keyboard shows them
        // (the user, 2026-11-18: "Can't we use HOS icons?")
        static const std::pair<const char*, const char*> hos[] = { { "Y", "\uE0E3" }, { "B", "\uE0E1" }, { "L", "\uE0E4" }, { "R", "\uE0E5" },
                                                                    { "L3", "\uE08A" }, { "R3", "\uE08B" }, { "+", "\uE0B5" } };
#if !defined(GF_WINDOWED)
        for (const auto& [k, glyph] : hos)
            if (name == k && g.provides(glyph)) {
                const Font gf{ 20 };
                const float gw = g.measureF(glyph, gf);
                g.textInkC(glyph, gf, rx - gw / 2, cy, ink, std::min(1.f, alpha * 1.4f) * a, Align::Center);
                return gw;
            }
#else
        (void)hos;
#endif
        const float bw = 2 * std::ceil(std::max(16.f, g.measureF(name, hf) + 9) / 2);
        g.roundStroke(rx - bw, cy - 8, bw, 16, 8, 1, ink, alpha * a);
        g.textInkC(name, hf, rx - bw / 2, cy, ink, alpha * a, Align::Center);   // (centred in its circle by the letters as drawn)
        return bw;
    };
    auto key = [&](int idx, float x, float y, float w, const char* label, const char* icon, bool accent, bool lit = false, const char* btn = nullptr) {
        const Id id = uid("skb.k", idx);
        // its touch area reaches halfway to its neighbours, a row's end key to the card's edge and the top row to the card's top:
        // a fast thumb between keys still types (the user, 2026-11-18)
        float hx0 = x - gap / 2, hx1 = x + w + gap / 2, hy0 = y - gap / 2;
        if (x - (cx + side) < kw) hx0 = cx;
        if ((cx + cw - side) - (x + w) < kw) hx1 = cx + cw;
        if (y == keysTop) hy0 = y0;
        const bool hit = ui.item(id, { hx0, hy0, hx1 - hx0, y + kh + gap / 2 - hy0 }, 12, Key);
        if (hit) keyFeedback();
        ringAt_[id] = { x, y, w, kh };   // (its ring round the key drawn, clear of the card's edge)
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (accent) g.roundRect(x, y, w, kh, 12, hot ? Rgb{ pal.accent.r * 0.88f + pal.text.r * 0.12f, pal.accent.g * 0.88f + pal.text.g * 0.12f, pal.accent.b * 0.88f + pal.text.b * 0.12f } : pal.accent, a);
        else if (lit) { g.roundRect(x, y, w, kh, 12, pal.accent, 0.2f * a); g.roundStroke(x, y, w, kh, 12, 1, pal.accent, 0.5f * a); }
        else g.roundRect(x, y, w, kh, 12, pal.text, (hot ? 0.16f : 0.08f) * a);
        const Rgb ink = accent ? pal.onAccent : lit ? pal.accentInk : pal.text;
        if (icon && label) {   // an icon and its word side by side, centred (the shift key); too narrow for both: the icon
            const Font lf = accent ? bf : kf;
            const float lw = g.measureF(label, lf), both = 18 + 6 + lw;
            if (both <= w - 12) {
                g.icon(icon, x + (w - both) / 2 + 9, y + kh / 2, 18, ink, a);
                g.textC(label, lf, x + (w - both) / 2 + 24, y + kh / 2, ink, a);
            } else g.icon(icon, x + w / 2, y + kh / 2, 18, ink, a);
        } else if (icon) g.icon(icon, x + w / 2, y + kh / 2, 18, ink, a);
        else g.textC(label, accent ? bf : kf, x + w / 2, y + kh / 2, ink, a, Align::Center);
        if (btn && hints) badge(btn, x + w - 5, y + 13, ink, 0.6f);
        return hit;
    };
    const char* const* rows = skb_.symbols ? KbSymbols : KbLetters;
    int idx = 0;
    for (int r = 0; r < 4; r++) {
        const int n = int(strlen(rows[r]));
        float x = cx + side + (10 - n) * (kw + gap) / 2;
        if (r == 3) {   // shift at the left, delete at the right
            if (key(100, cx + side, ky, kw * 1.5f, "Shift", skb_.caps ? ICON_CAPSLOCK : ICON_SHIFT, skb_.caps, skb_.shift, words ? "L3" : nullptr))
                searchKbShift();   // (caps lock: filled, the user's pick)
            x = cx + side + kw * 1.5f + gap + (7 - n) * (kw + gap) / 2;
        }
        for (int i = 0; i < n; i++, idx++) {
            char ch[2] = { rows[r][i], 0 };
            if ((skb_.shift || skb_.caps) && !skb_.symbols) ch[0] = char(toupper(static_cast<unsigned char>(ch[0])));
            if (key(idx, x + i * (kw + gap), ky, kw, ch, nullptr, false)) { ch[0] = rows[r][i]; searchKbType(ch); }
        }
        if (r == 3) {
            const float dx = cx + side + kw * 1.5f + gap + 7 * (kw + gap);
            if (key(101, dx, ky, cx + cw - side - dx, nullptr, ICON_BACK, false, false, "B")) searchKbBack();
        }
        ky += kh + gap;
    }
    const float bw1 = kw * 1.5f, bw3 = kw * 2.6f;
    if (key(102, cx + side, ky, bw1, "?#!", nullptr, false, skb_.symbols)) skb_.symbols = !skb_.symbols;
    if (key(103, cx + side + bw1 + gap, ky, cw - side * 2 - bw1 - bw3 - gap * 2, "space", nullptr, false, false, words ? "Y" : nullptr)) searchKbType(" ");
    if (key(104, cx + cw - side - bw3, ky, bw3, blue, nullptr, true, false, "+")) finishSearchKb();
    if (hints) {   // the cursor's buttons, at the foot on the right
        const float sy = ky + kh + 4 + stripH / 2;
        const char* what = "Move the cursor";
        float rx = cx + cw - side - 4;
        g.textC(what, { css::Xs }, rx, sy, pal.text2, a, Align::Right);
        rx -= g.measureF(what, { css::Xs }) + 8;
        rx -= badge("R", rx, sy, pal.text2, 0.9f) + 4;
        badge("L", rx, sy, pal.text2, 0.9f);
    }
}

void App::editTag(const char* field, const char* label) {
    askName(label, sheet.draft[field], [this, f = std::string(field)](const std::string& t) { sheet.draft[f] = t; });
}

void App::saveTagEdits() {
    Track& t = lib.tracks[sheet.track];
    auto& e = store.edits[t.path];
    for (const auto& [k, v] : sheet.draft)
        if (v != fieldOf(t, k)) e[k] = v;
    if (e.empty()) store.edits.erase(t.path);
    store.saveEdits();
    applyEdits(t);
    rebuildIndex();
    if (now.track == sheet.track) {
        now.info.title = t.title; now.info.artist = t.artist; now.info.album = t.album;
        now.info.albumArtist = t.albumArtist; now.info.genre = t.genre; now.info.date = t.date; now.info.trackNo = t.trackNo;
    }
    toast("Tags saved");
    // into the file too: every change of Songate's to this song (kept in Songate as well: it stays the same on your other devices)
    if (auto it = store.edits.find(t.path); it != store.edits.end() && !it->second.empty()) {
        TagJobs::Job j;
        j.path = t.path;
        for (const auto& [k, v] : it->second) j.changes.text[k] = v;
        tagWriteAsk({ j }, "“" + t.title + "”");
    }
}

void App::undoTagEdits(int track) {
    store.edits.erase(lib.tracks[track].path);
    store.saveEdits();
    meta.retag(track);   // the file's own tags come back as it is read again
    toast("Back to the tags in the file");
}

// ------------------------------------------------------------------ covers

void App::listCoverDir(const std::string& dir) {
    sheet.dir = dir;
    sheet.entries.clear();
    sheet.memo.clear();
    for (const auto& d : listDirs(dir)) sheet.entries.push_back(d + "/");
    std::vector<std::string> images;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.' && isImage(e->d_name)) images.push_back(e->d_name);
        closedir(d);
    }
    std::sort(images.begin(), images.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    sheet.entries.insert(sheet.entries.end(), images.begin(), images.end());
    area("sheet", int(Sheet::Cover)).target = area("sheet", int(Sheet::Cover)).y = 0;
    ui.focusDefault();
}

void App::openTagEditor(int t) {
    if (t < 0 || t >= int(lib.tracks.size())) return;
    sheet.draft.clear();
    for (const auto& [k, l] : kFields) sheet.draft[k] = fieldOf(lib.tracks[size_t(t)], k);
    sheet.page = Sheet::Tags;
    ui.focusDefault();
}

void App::setTrackCover(const std::string& image, const std::vector<int>* forTracks) {
    const std::vector<uint8_t> bytes = readAll(image);
    if (bytes.empty()) { toast("Couldn't read that image"); return; }
    // Copied into Gatefold's folder, so the cover stays when the image is moved; named by its content.
    uint32_t h = 2166136261u;
    for (uint8_t b : bytes) h = (h ^ b) * 16777619u;
    char name[32];
    snprintf(name, sizeof name, "%08x", h);
    const std::string dir = store.path("covers");
    mkdir(dir.c_str(), 0777);
    const std::string dest = dir + "/" + name + image.substr(image.find_last_of('.'));
    if (FILE* f = fopen(dest.c_str(), "wb")) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }

    const Track& t = lib.tracks[sheet.track];
    std::vector<int> targets{ sheet.track };
    if (sheet.wholeAlbum && t.albumIdx >= 0) targets = lib.albums[t.albumIdx].tracks;
    if (forTracks && !forTracks->empty()) targets = *forTracks;
    for (int i : targets) {
        store.covers[lib.tracks[i].path] = dest;
        meta.setCoverFile(i, dest);
    }
    store.saveEdits();
    if (std::find(targets.begin(), targets.end(), now.track) != targets.end()) loadTrackCoverAgain();   // now yours (store.covers)
    toast(targets.size() > 1 ? "Cover set for the whole album" : "Cover set");
    // into the files too (a JPEG or a PNG: what every reader takes)
    const bool jpegOrPng = bytes.size() > 8 && ((bytes[0] == 0xFF && bytes[1] == 0xD8) || (bytes[0] == 0x89 && bytes[1] == 'P'));
    if (!jpegOrPng) return;
    std::vector<TagJobs::Job> jobs;
    for (int i : targets) {
        TagJobs::Job j;
        j.path = lib.tracks[i].path;
        j.changes.cover = true;
        j.changes.coverBytes = bytes;
        jobs.push_back(std::move(j));
    }
    tagWriteAsk(std::move(jobs), targets.size() > 1 ? "the album's cover" : "the cover");
}

// ------------------------------------------------------------------ tags and covers into the files

void App::tagWriteAsk(std::vector<TagJobs::Job> jobs, const std::string& what) {
    if (jobs.empty() || store.tagWrite == 1) return;   // Songate only
    if (store.tagWrite == 2) { tagWriteQueue(std::move(jobs)); return; }
    const size_t n = jobs.size();
    confirm(ICON_TAG, "Tags", "Where should " + what + " go?", n > 1 ? std::to_string(n) + " songs" : std::string("This song"),
            "Into the file too?",
            "Songate has it now. In the file too, every app and device reading it sees the change; Songate keeps what the file held, so "
            "\"Restore the file's original tags\" puts it back. Settings › Library can decide for you.",
            "Into the file", [this, jobs] { tagWriteQueue(jobs); });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
    sheet.askAlt = "Songate only";
    sheet.askAltYes = [] {};
}

void App::tagWriteQueue(std::vector<TagJobs::Job> jobs) {
    if (tagPending_.empty() && tagJobs.idle()) tagDone_ = tagTotal_ = 0;   // (a new run of writes)
    for (auto& j : jobs) {
        // the same song waiting already: this one replaces it (the latest change wins, a write each)
        for (auto it = tagPending_.begin(); it != tagPending_.end(); ++it)
            if (it->path == j.path && it->undo == j.undo) {
                for (const auto& [k, v] : it->changes.text) if (!j.changes.text.count(k)) j.changes.text[k] = v;
                if (!j.changes.cover && it->changes.cover) { j.changes.cover = true; j.changes.coverBytes = it->changes.coverBytes; }
                tagPending_.erase(it);
                tagTotal_--;
                break;
            }
        tagPending_.push_back(std::move(j));
        tagTotal_++;
    }
}

/** Every frame: the next song handed over when nothing is being written and it is not the one playing; what came back applied. */
void App::tagWritesTick() {
#ifdef GF_PREVIEW
    // (the preview: GF_TAGWRITE_TEST=<cover image>: the first song's title and cover into its file, then its originals back; in the log)
    if (static int step = 0; getenv("GF_TAGWRITE_TEST") && !lib.tracks.empty() && time > 4 && step < 3) {
        if (step == 0) {
            TagJobs::Job j;
            j.path = lib.tracks[0].path;
            j.changes.text["title"] = "Written by Songate";
            j.changes.cover = true;
            j.changes.coverBytes = readAll(getenv("GF_TAGWRITE_TEST"));
            logLine("Tag test: writing into " + j.path);
            tagWriteQueue({ j });
            step = 1;
        } else if (step == 1 && tagPending_.empty() && tagJobs.idle() && tagUndo_.count(lib.tracks[0].path)) {
            const auto& u = tagUndo_[lib.tracks[0].path];
            logLine("Tag test: written; kept to undo: title \"" + u.text.at("title") + "\", cover " + (u.coverFile.empty() ? std::string("none") : u.coverFile));
            restoreFileTags(0);
            step = 2;
        } else if (step == 2 && tagPending_.empty() && tagJobs.idle() && !tagUndo_.count(lib.tracks[0].path)) {
            logLine("Tag test: restored; undo records left " + std::to_string(tagUndo_.size()));
            step = 3;
        }
    }
#endif
    if (!tagPending_.empty() && !store.stopTagWrites && tagJobs.idle()) {
        const std::string playing = playingFile();
        for (auto it = tagPending_.begin(); it != tagPending_.end(); ++it) {
            if (it->path == playing) continue;   // (held open: written once it is not playing)
            tagJobs.start();
            tagJobs.put(std::move(*it));
            tagPending_.erase(it);
            break;
        }
    }
    TagJobs::Done d;
    while (tagJobs.take(d)) {
        tagDone_++;
        const auto ti = pathIndex.find(d.job.path);
        const std::string title = ti != pathIndex.end() ? lib.tracks[size_t(ti->second)].title : baseName(d.job.path);
        if (!d.ok) {
            logLine("Tags not written into " + d.job.path + ": " + d.err);
            toast("Couldn't write into “" + title + "”: " + d.err + ". Kept in Songate");
            continue;
        }
        if (!d.job.undo) {   // what the file held, the first write's (a later write does not move the original)
            TagUndo& u = tagUndo_[d.job.path];
            for (const auto& [k, v] : d.before.text) if (!u.text.count(k)) u.text[k] = v;
            if (d.before.cover && !u.cover) {
                u.cover = true;
                if (!d.before.coverBytes.empty()) {
                    uint32_t h = 2166136261u;
                    for (uint8_t b : d.before.coverBytes) h = (h ^ b) * 16777619u;
                    char name[40];
                    snprintf(name, sizeof name, "%08x.%s", h, d.before.coverBytes[0] == 0x89 ? "png" : "jpg");
                    const std::string dir = store.path("tag-undo");
                    mkdir(dir.c_str(), 0777);
                    u.coverFile = dir + "/" + name;
                    store.writeFile((std::string("tag-undo/") + name).c_str(), std::string(d.before.coverBytes.begin(), d.before.coverBytes.end()));
                }
            }
        } else {   // the originals are back: Songate shows the file as it is (its own changes to these go too)
            if (auto u = tagUndo_.find(d.job.path); u != tagUndo_.end()) {
                if (d.job.changes.cover && !u->second.coverFile.empty()) store.removeFile(u->second.coverFile.substr(store.path("").size()).c_str());
                tagUndo_.erase(u);
            }
            if (auto e = store.edits.find(d.job.path); e != store.edits.end()) {
                for (const auto& [k, v] : d.job.changes.text) { (void)v; e->second.erase(k); }
                if (e->second.empty()) store.edits.erase(e);
            }
            if (d.job.changes.cover) store.covers.erase(d.job.path);
            store.saveEdits();
        }
        saveTagUndo();
        if (ti != pathIndex.end()) {   // read again: the file's tags, its cover
            meta.retag(ti->second);
            if (d.job.changes.cover) {
                auto c = store.covers.find(d.job.path);
                meta.setCoverFile(ti->second, c != store.covers.end() ? c->second : std::string());
            }
            if (now.track == ti->second && d.job.changes.cover) loadTrackCoverAgain();
        }
        if (tagPending_.empty() && tagJobs.idle())
            toast(d.job.undo ? "The file's original tags are back in “" + title + "”"
                             : tagTotal_ > 1 ? "Written into " + std::to_string(tagTotal_) + " files" : "Written into “" + title + "”");
    }
}

void App::restoreFileTags(int track) {
    const std::string& path = lib.tracks[size_t(track)].path;
    auto u = tagUndo_.find(path);
    if (u == tagUndo_.end()) return;
    TagJobs::Job j;
    j.path = path;
    j.undo = true;
    j.changes.text = u->second.text;
    if (u->second.cover) {
        j.changes.cover = true;
        if (!u->second.coverFile.empty()) j.changes.coverBytes = readAll(u->second.coverFile);
    }
    tagWriteQueue({ j });
    toast(now.track == track ? "Its original tags go back once it is not playing" : "Putting the file's original tags back");
}

void App::loadTagUndo() {
    tagUndo_.clear();
    FILE* f = fopen(store.path("tag-undo.tsv").c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[8192];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    auto unescape = [](const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\\' && i + 1 < s.size()) { const char c = s[++i]; o += c == 'n' ? '\n' : c == 't' ? '\t' : c; }
            else o += s[i];
        }
        return o;
    };
    for (size_t a = 0; a < text.size();) {   // path TAB field TAB value (a cover: field "#cover", value its file, "" none)
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        const std::string l = text.substr(a, b - a);
        a = b + 1;
        const size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? std::string::npos : l.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        TagUndo& u = tagUndo_[unescape(l.substr(0, t1))];
        const std::string field = l.substr(t1 + 1, t2 - t1 - 1), value = unescape(l.substr(t2 + 1));
        if (field == "#cover") { u.cover = true; u.coverFile = value; }
        else u.text[field] = value;
    }
}

void App::saveTagUndo() {
    auto escape = [](const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\t') o += "\\t"; else if (c != '\r') o += c; }
        return o;
    };
    std::string out;
    for (const auto& [path, u] : tagUndo_) {
        for (const auto& [k, v] : u.text) out += escape(path) + "\t" + k + "\t" + escape(v) + "\n";
        if (u.cover) out += escape(path) + "\t#cover\t" + escape(u.coverFile) + "\n";
    }
    if (out.empty()) store.removeFile("tag-undo.tsv");
    else store.writeFile("tag-undo.tsv", std::move(out));
}

long long App::tagUndoBytes() const {
    // (Settings shows it on every frame of every page: the card is asked only when the kept covers changed)
    static size_t forCount = ~size_t(0);
    static uint32_t forNames = 0;
    static long long kept = 0;
    uint32_t h = 2166136261u;
    for (const auto& [p, u] : tagUndo_) for (unsigned char c : u.coverFile) h = (h ^ c) * 16777619u;
    if (forCount == tagUndo_.size() && forNames == h) return kept;
    forCount = tagUndo_.size();
    forNames = h;
    long long n = 0;
    struct stat st;
    for (const auto& [p, u] : tagUndo_) if (!u.coverFile.empty() && stat(u.coverFile.c_str(), &st) == 0) n += st.st_size;
    return kept = n;
}

// ------------------------------------------------------------------ delete

bool App::deleteSong(int track) {
    const Track t = lib.tracks[track];
    if (now.track == track) {
        if (q.order.size() > 1) next(false);   // move on first: the file is open while it plays
        else player.stop();
    }
    if (now.track == track) player.stop();
    if (remove(t.path.c_str()) != 0) return false;
    std::erase(store.liked, t.path);
    stats.forget(t.path);
    store.edits.erase(t.path);
    store.covers.erase(t.path);
    return true;
}

void App::deleteTrack(int track) {
    const Track t = lib.tracks[track];
    if (!deleteSong(track)) {
        toast("Couldn't delete “" + baseName(t.path) + "”");
        return;
    }
    syncNoteDeleted(t.path);   // only once it really is gone
    store.saveEdits();
    store.save();
    closeSheet();
    rescan();
    toast("Deleted “" + t.title + "”");
}

// ------------------------------------------------------------------ the sheet

void App::openTrackMenu(int track, int queuePos, bool upNext) {
    if (track < 0 || track >= int(lib.tracks.size())) return;
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Menu;
    sheet.track = track;
    sheet.queuePos = queuePos;
    sheet.upNext = upNext && queuePos >= 0;
    sheet.returnFocus = ui.focusedId();
    area("sheet", int(Sheet::Menu)).target = area("sheet", int(Sheet::Menu)).y = 0;
    ui.focusDefault();
}

void App::closeSheet() {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet.preview = nullptr;
    sheet.page = Sheet::Closed;
    ui.setModal(false);
    if (sheet.returnFocus) ui.focus(sheet.returnFocus);
}

void App::sheetBack() {
    if (sheet.page == Sheet::Colour) {   // B puts down a square or bar first (Ui); then it is Cancel
        closeColourPicker(false);
        return;
    }
    if (sheet.page == Sheet::Job && sheet.fromList) {   // back to the list, on the job it came from
        int row = 0;
        for (int i = 0; i < sheet.recipe && i < JobSlots; i++) row += jobCards[i].shown > 0.01f;
        openJobList();
        sheet.queuePos = row;   // drawSmartSheet focuses it
        return;
    }
    if (sheet.page == Sheet::Menu || sheet.page == Sheet::NewList || sheet.page == Sheet::Sleep || sheet.page == Sheet::Devices || sheet.page == Sheet::AirPlay || sheet.page == Sheet::Job || sheet.page == Sheet::Jobs ||
        sheet.page == Sheet::PlFile || sheet.page == Sheet::Confirm || sheet.page == Sheet::EqPreset ||
        sheet.page == Sheet::ActList || sheet.page == Sheet::ListActions || sheet.page == Sheet::PlMenu || sheet.page == Sheet::FileMenu ||
        sheet.page == Sheet::Factory || sheet.page == Sheet::Report || sheet.page == Sheet::Crashes || sheet.page == Sheet::Battery || sheet.page == Sheet::SrvMenu || sheet.page == Sheet::SrvPlMenu) {
        closeSheet();
        return;
    }
    if (sheet.page == Sheet::AppMenu) { closeSheet(); return; }
    if (sheet.page == Sheet::AppIcon) {   // the symbols → the app's menu
        sheet.page = Sheet::AppMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::SrvPlIcon) {   // the icons → the server playlist's menu
        sheet.page = Sheet::SrvPlMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::SrvLists) {   // Add to a playlist… → the server track's menu
        sheet.page = Sheet::SrvMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::FileInfo || sheet.page == Sheet::FileLink) {   // Files: back to the file's menu
        sheet.page = Sheet::FileMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::PlIcon && !sheet.fromList) {   // the icons → the playlist's menu (opened from the page's icon button: closes)
        sheet.page = Sheet::PlMenu;
        ui.focusDefault();
        return;
    }
    if (sheet.page == Sheet::Smart || sheet.page == Sheet::SmartForm) {   // the recipe → the recipes → the choice
        sheet.page = sheet.page == Sheet::SmartForm ? Sheet::Smart : Sheet::NewList;
        ui.focusDefault();
        return;
    }
    sheet.page = Sheet::Menu;   // every page goes back to the menu ("Up one folder" climbs in the picker)
    ui.focusDefault();
}

void App::drawSheet() {
    if (sheet.page == Sheet::Closed) return;
    if (sheet.page >= Sheet::NewList) { drawSmartSheet(); return; }   // no track: view_smart.cpp
    if (sheet.track < 0 || sheet.track >= int(lib.tracks.size())) { closeSheet(); return; }
    ui.region(Region::Modal);
    const int t = sheet.track;
    const Track& tr = lib.tracks[t];
    const Sheet::Page page = sheet.page;

    // backdrop and card
    g.rect(0, 0, css::W, css::H, { 0, 0, 0 }, 0.55f);
    const float w = cardW(600), x = (css::W - w) / 2;
    const bool footer = page == Sheet::Tags || page == Sheet::Delete;
    const float headH = 24 + 64 + 18, footH = footer ? 64 : 0, maxBody = css::H - 60 - headH - footH - 16;

    // ---- the rows of this page, laid out first so the card can be sized to them
    struct Item {
        const char* icon;
        std::string label, detail;
        std::function<void()> act;
        bool danger = false;
        float h = 46;
        bool text = false;
    };
    std::vector<Item> items;
    const bool liked = store.isLiked(tr.path);
    // Opened on a row of the queue (the Playlist view, Up next): still that row, unless the queue changed meanwhile.
    const int qp = sheet.queuePos >= 0 && sheet.queuePos < int(q.order.size()) && q.tracks[q.order[sheet.queuePos]] == t ? sheet.queuePos : -1;
    if (page == Sheet::Menu && qp >= 0) {
        if (qp != q.pos)
            items.push_back({ ICON_NEXT, "Play next", "", [this, qp] {
                                  queuePlayNext(qp);
                                  toast("Plays next");
                                  closeSheet();
                              } });
        // In Up next, a row wrapped round from the start (Repeat) cannot move: it lies before the track playing.
        if (q.order.size() > 1 && (!sheet.upNext || qp > q.pos))
            items.push_back({ ICON_SORT_DOWN, "Move", "", [this, qp, upNext = sheet.upNext] {
                                  closeSheet();
                                  if (sortBegin(Grab::Queue, qp, upNext)) return;   // by touch: the list's sort mode (handles to drag)
                                  grab = Grab{ Grab::Queue, qp, upNext, 1 };
                                  ui.focus(upNext ? uid("np.next", qp - q.pos - 1) : uid("pl.r", qp));
                                  ui.setPadMode(true);
                                  toast(GF_MOVE_WORDS);
                              } });
        if (qp != q.pos)
            items.push_back({ ICON_X, "Remove from the playlist", "", [this, qp] {
                                  queueRemove(qp);
                                  toast("Removed from the playlist");
                                  closeSheet();
                              } });
    } else if (page == Sheet::Menu) {
        items.push_back({ ICON_NEXT, "Play next", "", [this, t] {
                              queueAdd(t, true);
                              toast("Plays next");
                              closeSheet();
                          } });
        items.push_back({ ICON_QUEUE, "Add to the end of the playlist", "", [this, t] {
                              queueAdd(t, false);
                              toast("Added to the playlist");
                              closeSheet();
                          } });
    }
    if (page == Sheet::Menu) {
        items.push_back({ ICON_LIST, "Add to a playlist…", "", [this] { sheet.page = Sheet::Lists; ui.focusDefault(); } });
        // On one of your own playlists, the track can leave it.
        if (route.view == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && store.playlists[route.ref].own()) {
            auto& pl = store.playlists[route.ref];
            // Move: picked up as the queue's rows are (an Automix playlist then keeps this order until Order again)
            {
                const std::vector<int> rows = playlistTracks(route.ref);
                const int row = int(std::find(rows.begin(), rows.end(), t) - rows.begin());
                if (row < int(rows.size()) && rows.size() > 1)
                    items.push_back({ ICON_SORT_DOWN, "Move", pl.automix ? "Your order stays" : "", [this, row, ref = route.ref] {
                                          closeSheet();
                                          if (sortBegin(Grab::List, row, false, ref)) return;
                                          grab = Grab{ Grab::List, row, false, 1, ref };
                                          ui.focus(uid("pl.r", row));
                                          ui.setPadMode(true);
                                          toast(GF_MOVE_WORDS);
                                      } });
            }
            if (pl.automix) {   // an Automix playlist: this track first (pinned), or Gatefold's choice again
                const bool pinned = pl.pin == tr.path;
                items.push_back({ ICON_LINK, pinned ? "Let Songate choose the first song" : "Play this first", "", [this, ref = route.ref, pinned, path = tr.path] {
                                      store.playlists[ref].pin = pinned ? "" : path;
                                      orderAutomixList(ref, false);
                                      toast(pinned ? "Songate chooses the first song again" : "Plays first; the rest ordered after it");
                                      closeSheet();
                                  } });
            }
            if (std::find(pl.tracks.begin(), pl.tracks.end(), tr.path) != pl.tracks.end())
                items.push_back({ ICON_X, "Remove from “" + pl.name + "”", "", [this, ref = route.ref, path = tr.path] {
                                      auto& tl = store.playlists[ref].tracks;
                                      if (auto it = std::find(tl.begin(), tl.end(), path); it != tl.end()) tl.erase(it);
                                      store.save();
                                      toast("Removed from “" + store.playlists[ref].name + "”");
                                      closeSheet();
                                  } });
        }
        items.push_back({ liked ? ICON_HEART_F : ICON_HEART, liked ? "Remove from Liked" : "Add to Liked", "", [this, liked, path = tr.path] {
                              store.toggleLiked(path);
                              store.save();
                              toast(liked ? "Removed from Liked" : "Added to Liked");
                          } });
        {   // its mood (BETA 3: yours to correct; the user, 2026-11-05: "Any song's menu")
            const int m = size_t(t) < moodIdx_.best.size() ? moodIdx_.best[size_t(t)] : -1;
            const auto lb = moodLabels_.find(tr.path);
            const bool yours = lb != moodLabels_.end() && lb->second.is >= 0;
            // one row; its page holds the moods, Not this one, and back to its sound (the user, 2026-11-05: "A, one Mood row")
            items.push_back({ moodIcon(m), "Mood", m < 0 ? std::string("Not measured yet") : std::string(mood::name(m)) + (yours ? " · yours" : ""),
                              [this] { sheet.page = Sheet::Moods; ui.focusDefault(); } });
        }
        items.push_back({ ICON_TAG, "Edit tags…", store.edits.count(tr.path) ? "Edited" : "", [this, t] { openTagEditor(t); } });
        items.push_back({ ICON_IMAGE, "Change the cover…", store.covers.count(tr.path) ? "Yours" : "", [this, t] {
                              sheet.page = Sheet::Cover;
                              sheet.wholeAlbum = lib.tracks[t].albumIdx >= 0;
                              // Settings → Storage and reset may name a folder of covers; else the track's own
                              struct stat st;
                              const bool own = !store.coverDir.empty() && stat(store.coverDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
                              listCoverDir(own ? store.coverDir : lib.tracks[t].folder);
                          } });
        if (store.covers.count(tr.path))
            items.push_back({ ICON_REFRESH, "Use the original cover", "", [this, t] {
                                  // for every track of the album that shares this cover
                                  const std::string mine = store.covers[lib.tracks[t].path];
                                  std::vector<int> group{ t };
                                  if (lib.tracks[t].albumIdx >= 0) group = lib.albums[lib.tracks[t].albumIdx].tracks;
                                  for (int i : group)
                                      if (auto it = store.covers.find(lib.tracks[i].path); it != store.covers.end() && it->second == mine) {
                                          store.covers.erase(it);
                                          meta.setCoverFile(i, "");
                                      }
                                  store.saveEdits();
                                  if (now.track >= 0 && std::find(group.begin(), group.end(), now.track) != group.end()) loadTrackCoverAgain();
                                  toast("Original cover back");
                              } });
        if (store.edits.count(tr.path)) items.push_back({ ICON_REFRESH, "Undo my tag edits", "", [this, t] { undoTagEdits(t); } });
        if (tagUndo_.count(tr.path))   // written into the file: what it held, back in it
            items.push_back({ ICON_REFRESH, "Restore the file's original tags", "", [this, t] { restoreFileTags(t); closeSheet(); } });
        if (tr.albumIdx >= 0)
            items.push_back({ ICON_GRID, "Go to the album", lib.albums[tr.albumIdx].title, [this, a = tr.albumIdx] { closeSheet(); goToAlbum(a); } });
        items.push_back({ ICON_MIC, "Go to the artist", tr.artist, [this, a = tr.artist] { closeSheet(); openArtist(a); } });
        if (!tr.genre.empty()) {
            const std::string g1 = tr.genre.substr(0, tr.genre.find(';'));
            items.push_back({ ICON_TAG, "Go to the genre", g1, [this, g1] { closeSheet(); openGenre(g1); } });
        }
        items.push_back({ ICON_FOLDER, "Show in Folders", "", [this, f = tr.folder] { closeSheet(); if (tabFor(Store::TabFolders, "folder")) navigate(View::Folders, lib.folderByPath(f)); } });
        items.push_back({ ICON_INFO, "Details", "", [this, t] {
                              sheet.info = TrackInfo{};
                              readTrackInfo(lib.tracks[t].path, sheet.info, true, true);
                              sheet.page = Sheet::Info;
                              ui.focusDefault();
                          } });
        items.push_back({ ICON_X, "Delete from " GF_THE_CARD "…", "", [this] { sheet.page = Sheet::Delete; ui.focusDefault(); }, true });
    } else if (page == Sheet::Lists) {
        // Your own playlists (folder playlists follow their folder, so tracks cannot be added to them).
        items.push_back({ ICON_PLUS, "New playlist…", "", [this, t] { closeSheet(); newPlaylist(t); } });
        for (int i = 0; i < int(store.playlists.size()); i++) {
            const FolderPlaylist& p = store.playlists[i];
            if (!p.own()) continue;
            const bool in = std::find(p.tracks.begin(), p.tracks.end(), tr.path) != p.tracks.end();
            items.push_back({ ICON_LIST, p.name, in ? "Already in it" : std::to_string(p.tracks.size()) + " tracks", [this, i, t] {
                                  addToPlaylist(i, t);
                                  closeSheet();
                              } });
        }
    } else if (page == Sheet::Moods) {
        const int m = size_t(t) < moodIdx_.best.size() ? moodIdx_.best[size_t(t)] : -1;
        const int bySound = size_t(t) < moodIdx_.sound.size() ? moodIdx_.sound[size_t(t)] : -1;
        mood::Label own = moodLabels_.count(tr.path) ? moodLabels_[tr.path] : mood::Label{};
        for (int c = 0; c < mood::Count; c++) {
            items.push_back({ moodIcon(c), mood::name(c), c == m ? std::string("Now") : (own.nots & (1u << c)) ? std::string("You said not") : std::string(mood::desc(c)),
                              [this, t, c, own] {
                                  mood::Label l = own;
                                  l.is = static_cast<signed char>(c);
                                  l.nots = uint16_t(l.nots & ~(1u << c));
                                  setMoodLabel(t, l);
                                  toast(std::string("In ") + mood::name(c) + " from now on: songs like it follow");
                                  closeSheet();
                              } });
        }
        if (own.any())
            items.push_back({ ICON_REFRESH, "As its sound says", bySound >= 0 ? std::string(mood::name(bySound)) : std::string(), [this, t] {
                                  setMoodLabel(t, mood::Label{});
                                  toast("Your choice for it cleared");
                                  closeSheet();
                              } });
    } else if (page == Sheet::Tags) {
        for (const auto& [k, l] : kFields)
            items.push_back({ nullptr, l, sheet.draft[k], [this, k = k, l = l] { editTag(k, l); } });
    } else if (page == Sheet::Cover) {
        if (!plat::isRoot(sheet.dir))
            items.push_back({ ICON_BACK, "Up one folder", "", [this] { listCoverDir(plat::parentDir(sheet.dir)); } });
        for (const auto& e : sheet.entries) {
            if (e.back() == '/') {
                const std::string name = e.substr(0, e.size() - 1);
                items.push_back({ ICON_FOLDER, name, "", [this, name] { listCoverDir(joinPath(sheet.dir, name)); } });
            } else {
                const std::string full = joinPath(sheet.dir, e);
                auto m = sheet.memo.find(full);
                if (m == sheet.memo.end()) {
                    struct stat st {};
                    m = sheet.memo.emplace(full, stat(full.c_str(), &st) == 0 ? sizeText(double(st.st_size)) : "").first;
                }
                const std::string size = m->second;
                items.push_back({ ICON_IMAGE, e, size, [this, full] { setTrackCover(full); if (sheet.page != Sheet::Confirm) { sheet.page = Sheet::Menu; ui.focusDefault(); } } });
            }
        }
        if (sheet.entries.empty()) items.push_back({ nullptr, "No images or folders here", "", nullptr, false, 46, true });
    } else if (page == Sheet::Info) {
        const TrackInfo& in = sheet.info;
        auto info = [&](const char* label, const std::string& v) {
            if (v.empty()) return;
            // a value longer than its room (the file's path, on a phone upright) takes the lines it needs, three at most
            // (a narrow card only: the console's and the PC's keep their one line, as they were)
            const int lines = w < 520 ? int(g.wrap(v, { css::Base }, int(w - 48 - 120), 3).size()) : 1;
            items.push_back({ nullptr, label, v, [] {}, false, 40 + std::max(0, lines - 1) * 20.f, true });
        };
        info("Title", tr.title);
        info("Artist", tr.artist);
        info("Album", tr.album);
        info("Album artist", tr.albumArtist);
        info("Genre", tr.genre);
        info("Year", tr.date);
        info("Song", tr.trackNo ? std::to_string(tr.trackNo) : "");
        std::string fmt = in.codec;
        if (in.sampleRate) fmt += " · " + std::to_string(in.sampleRate / 1000) + "." + std::to_string(in.sampleRate % 1000 / 100) + " kHz";
        if (in.bitsPerSample && in.codec != "MP3") fmt += " · " + std::to_string(in.bitsPerSample) + "-bit";
        if (in.bitrateKbps) fmt += " · " + std::to_string(in.bitrateKbps) + " kbps";
        info("Format", fmt);
        info("Length", fmtTime(tr.duration > 0 ? tr.duration : in.duration));
        if (!sheet.memo.count("size")) {   // once, not every frame
            struct stat st {};
            sheet.memo["size"] = stat(tr.path.c_str(), &st) == 0 ? sizeText(double(st.st_size)) : "";
            sheet.memo["lrc"] = readSidecarLyrics(tr.path).empty() ? "" : "1";
        }
        info("Size", sheet.memo["size"]);
        info("Cover", store.covers.count(tr.path) ? "Yours, kept by Songate" : !in.cover.empty() ? "In the file" : "From the folder, or none");
        info("Lyrics", !sheet.memo["lrc"].empty() ? ".lrc file" : !in.lyrics.empty() ? "In the tags" : "None");
        if (const auto it = store.lyricNudge.find(tr.path); it != store.lyricNudge.end()) {   // the timing control (Now playing, the Canvas)
            char b[48];
            snprintf(b, sizeof b, "%.1f s %s, kept by Songate", double(std::fabs(it->second)), it->second > 0 ? "later" : "earlier");
            info("Lyrics timing", b);
        }
        info("Your edits", store.edits.count(tr.path) ? "Yes, kept by Songate; the file is unchanged" : "");
        // Volume levelling (levels.hpp)
        if (const Level* lv = levels.find(tr.path); lv && !std::isnan(lv->lufs)) {
            char b[64];
            snprintf(b, sizeof b, "%.1f LUFS, peak %.1f dBTP", double(lv->lufs), 20 * std::log10(std::max(1e-6, double(lv->peak))));
            std::string v = b;
            if (store.levelMode) {
                snprintf(b, sizeof b, " · %+.1f dB here", levelDb(t));
                v += b;
            }
            info("Loudness", v);
        } else {
            info("Loudness", lv ? "Could not be measured" : "Not measured yet");
        }
        // Automix's analysis (mixan.hpp), made with the loudness
        if (const Level* lv = levels.find(tr.path); lv && lv->mix.seconds != 0) {
            char b[64];
            if (lv->mix.bpm > 0 && lv->mix.confidence >= 0.5f) snprintf(b, sizeof b, "%.0f", double(lv->mix.bpm));
            else if (lv->mix.bpm > 0) snprintf(b, sizeof b, "About %.0f (no steady beat for Automix)", double(lv->mix.bpm));
            else snprintf(b, sizeof b, "No steady beat");
            info("BPM", b);
            if (lv->mix.key >= 0)   // the key, and its place on the Camelot wheel (Automix v2)
                info("Key", (lv->mix.keySure >= 0.5f ? "" : "Perhaps ") + mixan::keyName(lv->mix.key) + " (" + mixan::camelot(lv->mix.key) + ")");
        }
        // Play statistics (stats.hpp)
        auto when = [](int64_t t) {
            char buf[32];
            const std::time_t tt = std::time_t(t);
            std::strftime(buf, sizeof buf, "%d %b %Y", std::localtime(&tt));
            return std::string(buf);
        };
        const PlayStat* ps = stats.find(tr.path);
        info("Plays", ps && ps->plays ? std::to_string(ps->plays) : "Not played yet");
        if (ps && ps->last) info("Last played", when(ps->last));
        if (ps && ps->added) info("Added", when(ps->added));
        info("File", tr.path);
    }

    // ---- card
    float bodyH = 8;
    for (const auto& it : items) bodyH += it.h;
    if (page == Sheet::Cover) bodyH += 22 + (tr.albumIdx >= 0 ? 44 : 0);   // the folder line, and Use for where there is an album (below)
    if (page == Sheet::Delete) bodyH = 110;
    const std::vector<std::string> tagsNote = page == Sheet::Tags
        ? g.wrap("Kept by Songate and shown everywhere in the app. The file itself is not changed.", { css::Sm }, int(w - 48), 3) : std::vector<std::string>{};
    if (page == Sheet::Tags) bodyH += 34 + (std::max<size_t>(1, tagsNote.size()) - 1) * 18.f;   // (two lines on a phone upright: it was cut)
    bodyH = std::min(bodyH + 8, maxBody);
    const float h = headH + bodyH + footH + 16, y = cardY(h);
    g.glow(x + w / 2, y + h / 2 + 30, w * 0.75f, { 0, 0, 0 }, 0.5f);
    g.roundRect(x, y, w, h, 20, pal.surface2);
    g.roundRect(x, y, w, h, 20, pal.text, 0.03f);
    g.roundStroke(x, y, w, h, 20, 1, pal.text, 0.12f);

    // ---- header: cover (or the image being considered), kicker, title, artist · album, close
    const float hx = x + 24, hy = y + 24;
    if (page == Sheet::Cover && sheet.preview) g.image(sheet.preview, hx, hy, 64, 64, 10);
    else coverBox(t, hx, hy, 64, 10);
    g.roundStroke(hx, hy, 64, 64, 10, 1, pal.text, 0.09f);
    static const char* kickers[] = { "", "Song", "Edit tags", "Choose a cover", "Delete", "Details", "Add to a playlist", "Mood" };
    kicker(kickers[int(page)], hx + 64 + 16, hy + 8, pal.accentInk);
    const int textW = int(w - 64 - 16 - 24 - 24 - 40);
    // the mood page: Not <its mood>, on the close button's line, to its left (the user, 2026-11-05: no cross in it, and clear of the
    // close button's ring and its tip)
    const int notMood = page == Sheet::Moods && size_t(t) < moodIdx_.best.size() ? moodIdx_.best[size_t(t)] : -1;
    const std::string notLabel = notMood >= 0 ? std::string("Not ") + mood::name(notMood) : std::string();
    const float notW = notMood >= 0 ? 16 + g.measure(notLabel, { css::Sm, Weight::SemiBold }) + 16 : 0;
    const int titleW = notMood >= 0 ? int(textW + 40 - notW - 28 - 16) : textW;
    g.text(tr.title, { css::Lg, Weight::Bold }, hx + 64 + 16, hy + 18, pal.text, 1, Align::Left, titleW);
    g.text(trackSub(t), { css::Sm }, hx + 64 + 16, hy + 44, pal.text3, 1, Align::Left, textW);
    if (notMood >= 0) {
        bool no = false;
        button(uid("sh.notmood"), nullptr, notLabel, x + w - 24 - 28 - 12 - notW, hy + 14 - 17, false, &no);
        if (no) {
            mood::Label l = moodLabels_.count(tr.path) ? moodLabels_[tr.path] : mood::Label{};
            if (l.is == notMood) l.is = -1;
            l.nots = uint16_t(l.nots | (1u << notMood));
            setMoodLabel(t, l);
            toast(std::string("Not ") + mood::name(notMood) + " from now on: Songate picks the next best");
            closeSheet();
            return;
        }
    }
    hint(uid("sh.close"), "Close");
    if (iconButton(uid("sh.close"), ICON_X, x + w - 24 - 14, hy + 14)) { closeSheet(); return; }
    g.rect(x + 24, y + headH - 1, w - 48, 1, pal.text, 0.09f);

    // ---- body
    const float by = y + headH;
    ScrollArea& sa = area("sheet", int(page));
    sa.view = { x, by, w, bodyH };
    ui.pushArea(&sa);
    g.clip(x, by, w, bodyH);
    float ry = by + 8 - sa.y;
    if (page == Sheet::Cover) {
        // where the browser is, and which tracks get the cover
        g.textC(sheet.dir, { css::Xs }, x + 24, ry + 10, pal.text3, 1, Align::Left, int(w - 48));
        ry += 22;
        if (tr.albumIdx >= 0) {
            int pick = -1;
            segmented(uid("sh.scope"), x + w - 24, ry + 14, { "This song", "The whole album" }, sheet.wholeAlbum ? 1 : 0, &pick);
            g.textC("Use for", { css::Base, Weight::SemiBold }, x + 24, ry + 14, pal.text2);
            if (pick >= 0) sheet.wholeAlbum = pick == 1;
            ry += 44;   // the control is 36 tall round ry + 14: 30 put the first row's focus ring against it
        }
    }
    if (page == Sheet::Delete) {
        g.text("Delete this song from " GF_THE_CARD "?", { css::Lg, Weight::Bold }, x + 24, ry + 8, pal.text, 1, Align::Left, int(w - 48));
        float ty = ry + 40;
        for (const auto& l : g.wrap("The file is removed for good, and it leaves the library, the playlist and Liked. " + tr.path, { css::Sm },
                                    int(w - 48), 3)) {
            g.text(l, { css::Sm }, x + 24, ty, pal.text3);
            ty += 19;
        }
    }
    // Focus starts on the first row, or in the picker on the first image (most folders hold just the one cover).
    size_t focusRow = 0;
    if (page == Sheet::Cover)
        for (size_t i = 0; i < items.size(); i++)
            if (items[i].icon == ICON_IMAGE) { focusRow = i; break; }
    if (page == Sheet::Moods)   // its mood now (the rows are the moods in order)
        if (const int m = size_t(t) < moodIdx_.best.size() ? moodIdx_.best[size_t(t)] : -1; m >= 0 && size_t(m) < items.size()) focusRow = size_t(m);
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i];
        const Id id = uid("sh.row", int64_t(i) + 1000 * int(page));
        const bool hit = it.act && ui.item(id, { x + 12, ry, w - 24, it.h }, 10, i == focusRow ? DefaultFocus : 0);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (hot && !it.text) g.roundRect(x + 12, ry, w - 24, it.h, 10, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
        const Rgb fg = it.danger ? dangerInk() : pal.text;
        float lx = x + 24;
        if (it.icon) { g.icon(it.icon, lx + 9, ry + it.h / 2, 18, it.danger ? fg : pal.text2); lx += 18 + 14; }
        if (page == Sheet::Info) {
            // label column, value wrapped beside it
            g.textC(it.label, { css::Sm }, lx, ry + 20, pal.text3);
            if (it.h <= 40) g.textC(it.detail, { css::Base }, lx + 120, ry + 20, pal.text, 1, Align::Left, int(w - 48 - 120));
            else {
                float ty = ry + 20;
                for (const auto& l : g.wrap(it.detail, { css::Base }, int(w - 48 - 120), 3)) { g.textC(l, { css::Base }, lx + 120, ty, pal.text); ty += 20; }
            }
        } else if (page == Sheet::Tags) {
            g.textC(it.label, { css::Base, Weight::SemiBold }, lx, ry + it.h / 2, pal.text2);
            const std::string v = it.detail.empty() ? "—" : it.detail;
            g.textC(v, { css::Base }, x + w - 24, ry + it.h / 2, it.detail.empty() ? pal.text3 : pal.text, 1, Align::Right, int(w - 48 - 150));
        } else {
            const int dw = it.detail.empty() ? 0 : std::min(200, g.measure(it.detail, { css::Sm }));
            g.textC(it.label, { css::Base, Weight::SemiBold }, lx, ry + it.h / 2, it.text ? pal.text3 : fg, 1, Align::Left,
                    int(x + w - 24 - lx - dw - 16));
            if (dw) g.textC(it.detail, { css::Sm }, x + w - 24, ry + it.h / 2, pal.text3, 1, Align::Right, 200);
        }
        if (hit && it.act) it.act();
        if (page == Sheet::Cover && ui.focused(id) && it.icon == ICON_IMAGE) {
            // the header shows the image under focus, so you see it before choosing
            const std::string full = joinPath(sheet.dir, it.label);
            if (sheet.previewPath != full) {
                sheet.previewPath = full;
                if (sheet.preview) SDL_DestroyTexture(sheet.preview);
                sheet.preview = nullptr;
                const std::vector<uint8_t> bytes = readAll(full);
                if (SDL_Surface* s = loadImage(bytes)) {
                    if (SDL_Surface* small = Gfx::shrink(s, 128)) {
                        sheet.preview = SDL_CreateTextureFromSurface(renderer_, small);
                        if (sheet.preview) { SDL_SetTextureScaleMode(sheet.preview, SDL_ScaleModeLinear); SDL_SetTextureBlendMode(sheet.preview, SDL_BLENDMODE_BLEND); }
                        SDL_FreeSurface(small);
                    }
                    SDL_FreeSurface(s);
                }
            }
        }
        if (sheet.page != page) break;   // the action moved to another page (or closed): its rows are gone
        ry += it.h;
    }
    if (page == Sheet::Tags && sheet.page == page) {
        float ty = ry + 10;
        for (const auto& l : tagsNote) { g.text(l, { css::Sm }, x + 24, ty, pal.text3); ty += 18; }
        ry += 34 + (std::max<size_t>(1, tagsNote.size()) - 1) * 18.f;
    }
    sa.content = ry + sa.y - by + 8;
    g.unclip();
    ui.popArea();
    if (sheet.page != page) return;

    // ---- footer buttons
    if (footer) {
        const float fy = y + h - 16 - 48;
        g.rect(x + 24, fy - 8, w - 48, 1, pal.text, 0.09f);
        bool c = false;
        const Font bf{ css::Sm, Weight::SemiBold };
        if (page == Sheet::Tags) {
            const float sw = g.measure("Save", bf) + 32 + 24;
            const float cw = g.measure("Cancel", bf) + 32;
            button(uid("sh.save"), ICON_CHECK, "Save", x + w - 24 - sw, fy + 7, true, &c);
            if (c) { saveTagEdits(); if (sheet.page != Sheet::Confirm) { sheet.page = Sheet::Menu; ui.focusDefault(); } return; }
            button(uid("sh.cancel"), nullptr, "Cancel", x + w - 24 - sw - 8 - cw, fy + 7, false, &c);
            if (c) { sheet.page = Sheet::Menu; ui.focusDefault(); return; }
        } else {
            // Cancel is where focus lands: a stray A press must not delete anything
            const float dw = g.measure("Delete", bf) + 32 + 24;
            const float cw = g.measure("Cancel", bf) + 32;
            button(uid("sh.del"), ICON_X, "Delete", x + w - 24 - dw, fy + 7, false, &c, false, true);
            if (c) { deleteTrack(t); return; }
            button(uid("sh.keep"), nullptr, "Cancel", x + w - 24 - dw - 8 - cw, fy + 7, false, &c, false, false, DefaultFocus);
            if (c) { sheet.page = Sheet::Menu; ui.focusDefault(); return; }
        }
    }
}
