/*
 * Gatefold NX · view_settings.cpp
 * #view-settings as Gatefold 1.4 lays it out: a "GATEFOLD / Settings" header, a
 * "Find a setting" field and the categories down the left; on the right the
 * chosen category's header (icon tile + title) over rounded cards of rows.
 * Rows put the label (and a hint, wrapped, never cut) on the left and the
 * control on the right: segmented choices, toggles, sliders, buttons.
 */
#include "words.hpp"
#include <algorithm>
#include <set>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>

#include "app.hpp"
#include "icons.inc"
#include "hibyr4.hpp"
#include "sysapi.hpp"
#include "plicons.inc"
#include "appicons.inc"

#ifndef GF_VERSION
#define GF_VERSION "2.1.1"
#endif

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

/** A key for a row's control made of a name and a number (the system pages' networks and devices), kept for good: the rows hold
 *  it as a pointer, frame after frame */
#ifdef GF_ANDROID
const char* uidKey(const char* base, int i) {
    static std::set<std::string> keys;
    return keys.insert(std::string(base) + "#" + std::to_string(i)).first->c_str();
}
#endif

struct Row {
    std::string label, value, hint;   // value: shown after the label, like Gatefold's "Saturation 100%"
    float ctlW = 0;                   // the control's width, so the text knows how much room it has
    std::function<void(float rx, float cy)> control;
    float minH = 56;
    // a full-width block instead of label + control (the theme picker, the About header)
    float blockH = 0;
    std::function<void(float x, float y, float w)> block;
    Id focus = 0;   // its control, for showSetting (buttons; other rows are only scrolled to and lit)
    bool lifted = false;   // picked up with Move (a sidebar tab): drawn as the queue's held row
    int dragKey = -1;      // a sidebar tab's row: its height noted, drawn where it slides to (tabSlide), held over the others
    float words = 0;       // a choice bar: its choices' words, measured (whether it takes two rows: App::segGrid)
    size_t nOpts = 0;
    bool fill = false;     // a choice bar: upright (narrow) it takes a line of its own, the row's whole width (sliders too: ctlW 180)
    bool dim = false;      // greyed: the label in the hint's colour (a row with nothing to switch just now)
    int rounds = 0;        // a colour row: its rounds (30 each); upright, more than a line holds wrap onto more lines (roundAt)
    bool roundsLead = false;   // ... its first round (A) as tall as the lines too, on the left
};

/** A colour row's rounds (n: its colours, then the wheel last). On one line as always, right-aligned. Upright (maxW, the row's own
 *  line) when they do not fit: the wheel goes to the right, as tall as the lines, and the colours share the lines evenly in columns
 *  to its left (the user, 2026-11-14: "a color picker circle with the diameter of the two rows"). With lead, the first round (A,
 *  the cover's own) is as tall too, on the left (the user, 2026-11-15: "can we also make this one big"). */
struct RoundsLayout { int lines = 1, per = 0; float wheelD = 26; bool lead = false; };
RoundsLayout roundsLayout(int n, float maxW, bool lead = false) {
    RoundsLayout L;
    L.per = n;
    if (maxW <= 0 || n * 30.f <= maxW + 4) return L;
    L.lead = lead;
    const int m = n - (lead ? 2 : 1);
    for (L.lines = 2; L.lines < 8; L.lines++) {
        L.wheelD = L.lines * 34.f - 8;
        L.per = (m + L.lines - 1) / L.lines;
        if (L.per * 30.f + (L.wheelD + 10) * (lead ? 2 : 1) <= maxW + 4) break;
    }
    return L;
}
/** Round i's left edge x, centre cy and size d (26, or the tall ones'). */
struct Round { float x, cy, d; };
Round roundAt(int i, int n, float rx, float cy, float maxW, bool lead = false) {
    const RoundsLayout L = roundsLayout(n, maxW, lead);
    if (L.lines == 1) return { rx - float(n - i) * 30, cy, 26 };
    const float midY = cy + (L.lines - 1) * 17.f;
    if (i == n - 1) return { rx - L.wheelD, midY, L.wheelD };
    const float colsR = rx - L.wheelD - 10 + 4;   // the colours' columns end here (each round 26 in its 30)
    if (L.lead && i == 0) return { colsR - float(L.per) * 30 - 10 - L.wheelD, midY, L.wheelD };
    const int j = i - (L.lead ? 1 : 0), m = n - (L.lead ? 2 : 1), line = j / L.per;
    const int inLine = line + 1 < L.lines ? L.per : m - L.per * (L.lines - 1);
    return { colsR - float(inLine - j % L.per) * 30, cy + float(line) * 34, 26 };
}
/** The A round (Songate's own colour, from the cover) at size d: a faint disc with an A. */
void drawRoundA(Gfx& g, const Palette& pal, float sx, float cy, float d) {
    g.circle(sx + d / 2, cy, (d - 2) / 2, pal.text, 0.1f);   // the wheel's own size (12 at 26)
    g.textC("A", { d > 40 ? css::Xl : css::Xs, Weight::Bold }, sx + d / 2, cy, pal.text2, 1, Align::Center);
}
// The sidebar's tabs dragged by touch (the user, 2026-11-04: "you can actually grab and bring it over to the desired sorting
// order"): the held row follows the finger; a row it passes slides into the place it left; let go, it settles into its own.
std::map<int, float> tabRowH;     // each tab row's height, last frame (what the finger must pass to change places)
std::map<int, float> tabSlide;    // each tab row's offset from its place, easing back to 0
int tabHeld = -1;                 // the tab under the finger

// with: the card it opens with when a page folds its cards (Custom with Interface colours: the user, 2026-11-16, "when the custom
// theme is used the custom row should be opened too")
struct Card { std::string kicker; std::vector<Row> rows; std::string with = {}; };
// SongateOS's guide: updating from a computer, for a player without a microSD card (Settings › Updates' Guide)
constexpr const char* kSongateOSGuide = "https://github.com/AndryTheBeast/SongateOS-HiBy_R4/blob/main/docs/03-install-os.md#updates-need-a-microsd-card";
struct Section { const char* name; const char* icon; std::vector<Card> cards; };

}  // namespace

/** Gatefold's .seg: a pill holding the options, the chosen one raised. Right-aligned at rx; returns its width. */
/** A choice bar spread to a width (fill) with too little room round its words (under 20 each: a phone upright's four long ones,
 *  the user's screenshot, 2026-11-04: "too cramped and not centered"): two rows of equal cells instead of one. */
bool App::segGrid(float words, size_t n, float maxW) const { return maxW > 0 && n >= 4 && (maxW - 6 - words) / float(n) < 20; }

float App::segmented(Id id, float rx, float cy, const std::vector<std::string>& opts, int cur, int* picked, unsigned disabled, float maxW, int fill, float hgt) {
    const Font f{ css::Sm, Weight::SemiBold };
    float words = 0;
    for (const auto& o : opts) words += g.measure(o, f);
    if (fill == 1 && segGrid(words, opts.size(), maxW)) {
        const int cols = int((opts.size() + 1) / 2);
        const float ch = 36, w = maxW, h = ch * 2 + 6, x0 = rx - w, y0 = cy - h / 2, cw = (w - 6) / cols;
        g.roundRect(x0, y0, w, h, 12, pal.text, 0.05f);
        g.roundStroke(x0, y0, w, h, 12, 1, pal.text, 0.09f);
        for (int i = 0; i < int(opts.size()); i++) {
            const float x = x0 + 3 + (i % cols) * cw, y = y0 + 3 + (i / cols) * ch;
            const Id oid = id + 0x9e3779b97f4a7c15ull * uint64_t(i + 1);
            const bool off = disabled & (1u << i);
            if (!off && ui.item(oid, { x, y, cw, ch }, 9) && picked) *picked = i;
            const bool on = i == cur, hot = !off && (ui.pressed(oid) || (ui.focused(oid) && ui.showFocusRing()));
            if (on) g.roundRect(x, y, cw, ch, 9, pal.text, 0.13f);
            else if (hot) g.roundRect(x, y, cw, ch, 9, pal.text, 0.065f);
            g.textC(opts[i], f, x + cw / 2, y + ch / 2, on ? pal.text : pal.text2, off ? 0.35f : 1.f, Align::Center, int(cw - 8));
        }
        return w;
    }
    const float room = maxW > 0 && opts.size() ? std::clamp((maxW - 6 - words) / opts.size(), 12.f, fill ? 1e4f : 26.f) : 26.f;   // fill: exactly maxW
    const float w = 6 + words + room * opts.size();
    // (32 high: the search box's corners; a phone upright: a pill, as every control there, the user chose "B", 2026-11-15)
    const float h = hgt, x0 = rx - w, ro = phoneUpright() ? h / 2 : 12 * h / 36, ri = phoneUpright() ? (h - 6) / 2 : 9 * (h - 6) / 30;
    g.roundRect(x0, cy - h / 2, w, h, ro, pal.text, 0.05f);
    g.roundStroke(x0, cy - h / 2, w, h, ro, 1, pal.text, 0.09f);
    float x = x0 + 3;
    for (int i = 0; i < int(opts.size()); i++) {
        const float ow = g.measure(opts[i], f) + room;
        const Id oid = id + 0x9e3779b97f4a7c15ull * uint64_t(i + 1);
        const bool off = disabled & (1u << i);   // greyed: drawn, but not focusable or pressable
#ifdef GF_ANDROID
        // a finger: the whole height of the bar and 4 more above and below, 44 (drawn the same; the user chose it, 2026-11-04)
        const SDL_FRect hitR{ x, cy - h / 2 - 4, ow, h + 8 };
#else
        const SDL_FRect hitR{ x, cy - h / 2 + 3, ow, h - 6 };
#endif
        if (!off && ui.item(oid, hitR, 9) && picked) *picked = i;
        const bool on = i == cur, hot = !off && (ui.pressed(oid) || (ui.focused(oid) && ui.showFocusRing()));
        if (on) g.roundRect(x, cy - h / 2 + 3, ow, h - 6, ri, pal.text, 0.13f);
        else if (hot) g.roundRect(x, cy - h / 2 + 3, ow, h - 6, ri, pal.text, 0.065f);
        g.textC(opts[i], f, x + ow / 2, cy, on ? pal.text : pal.text2, off ? 0.35f : 1.f, Align::Center);
        x += ow;
    }
    return w;
}

/** A folder as shown in Settings: on Android, one in Songate's own app storage (/data/user/0/<package>/files/...) said so, not its path. */
std::string placeWords(const std::string& p) {
#ifdef GF_ANDROID
    const size_t f = p.find("/files/");
    if (p.rfind("/data/", 0) == 0 && f != std::string::npos) {
        std::string rest = p.substr(f + 7), out = "Songate's app storage";
        size_t a = 0;
        while (a < rest.size()) {
            size_t b = rest.find('/', a);
            if (b == std::string::npos) b = rest.size();
            if (b > a) out += " › " + rest.substr(a, b - a);
            a = b + 1;
        }
        return out;
    }
#endif
    return p;
}

std::string App::eqPresetName(int preset) const {
    if (preset >= 0 && preset < GFEQ_PRESETS) return kGfEqPresets[preset].name;
    if (preset >= GFEQ_PRESETS && preset - GFEQ_PRESETS < int(store.eqUser.size())) return store.eqUser[size_t(preset - GFEQ_PRESETS)].name;
    return "Custom";
}

void App::saveEqPreset(int into) {
    std::string name = "My preset " + std::to_string(store.eqUser.size() + 1);
    if (into >= int(store.eqUser.size())) into = -1;
    if (into >= 0) { saveEqPresetNamed(into, store.eqUser[size_t(into)].name); return; }   // updated where it is, under its name
    askName("Name the preset", name, [this](const std::string& n) { saveEqPresetNamed(-1, n); });
}

void App::saveEqPresetNamed(int into, std::string name) {
    if (into >= int(store.eqUser.size())) into = -1;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    for (char& c : name) if (c == '\t') c = ' ';
    if (name.empty()) return;
    if (name.size() > 63) name.resize(63);
    GfEqUser u{};
    snprintf(u.name, sizeof u.name, "%s", name.c_str());
    u.preamp = store.eq.preamp;
    u.bass = store.eq.bass;
    u.treble = store.eq.treble;
    for (int b = 0; b < GFEQ_BANDS; b++) u.bands[b] = store.eq.bands[b];
    int k = into;
    if (k < 0)
        for (int i = 0; i < int(store.eqUser.size()); i++) if (name == store.eqUser[size_t(i)].name) k = i;   // the same name: replaced
    if (k < 0) {
        if (store.eqUser.size() >= GFEQ_USER_MAX) { toast("That is a hundred presets already: delete one first"); return; }
        store.eqUser.push_back(u);
        k = int(store.eqUser.size()) - 1;
    } else {
        store.eqUser[size_t(k)] = u;
    }
    store.saveEqPresets();
    GfbgEq e = store.eq;
    e.preset = int8_t(GFEQ_PRESETS + k);
    e.edited = 0;
    setEq(e);
    ui.focus(uid("set.eqpreset"));   // the Save row goes with the change: without this the focus fell back to Find a setting
    toast(into >= 0 ? "Updated the preset “" + name + "”" : "Saved the preset “" + name + "”");
}

void App::renameEqPreset(int k) {
    if (k < 0 || k >= int(store.eqUser.size())) return;
    const std::string old = store.eqUser[size_t(k)].name;
    askName("Rename the preset", old, [this, k, old](std::string name) {
        if (k >= int(store.eqUser.size()) || old != store.eqUser[size_t(k)].name) return;   // (gone or moved while typing)
        while (!name.empty() && name.back() == ' ') name.pop_back();
        for (char& c : name) if (c == '\t') c = ' ';
        if (name.empty()) return;
        snprintf(store.eqUser[size_t(k)].name, sizeof store.eqUser[size_t(k)].name, "%s", name.c_str());
        store.saveEqPresets();
    });
}

void App::deleteEqPreset(int k) {
    if (k < 0 || k >= int(store.eqUser.size())) return;
    const std::string name = store.eqUser[size_t(k)].name;
    confirm(ICON_EQ, "Equalizer", name, "Your preset", "Delete this preset?", "It leaves the preset list. The sound playing now stays as it is, as Custom.",
            "Delete", [this, k, name] {
                if (k >= int(store.eqUser.size()) || name != store.eqUser[size_t(k)].name) return;
                store.eqUser.erase(store.eqUser.begin() + k);
                store.saveEqPresets();
                GfbgEq e = store.eq;
                if (e.preset == GFEQ_PRESETS + k) gfEqMakeCustom(&e);      // the one on: its sound stays, as Custom
                else if (e.preset > GFEQ_PRESETS + k) e.preset--;           // the ones after it move up
                setEq(e);
                ui.focus(uid("set.eqpreset"));   // its rows go with it
                toast("Deleted the preset “" + name + "”");
            });
}

/** The equalizer buttons' short press (player bar, Canvas). */
void App::toggleEq() {
    GfbgEq e = store.eq;
    e.on = !e.on;
    setEq(e);
    toast(e.on ? "Equalizer on: " + eqPresetName(e.preset) : std::string("Equalizer off"));
}

/** A long press on an equalizer button: Settings → Equalizer, its switch focused. */
void App::openEqualizer() {
    settingsCat = SetEqualizer;
    settingsQuery.clear();
    settingsPhoneOpen_ = settingsPhoneDirect_ = true;   // upright: its page, and back returns from where it came
    area("settings.pane").target = area("settings.pane").y = 0;
    navigate(View::Settings);
    settingsJump = "Equalizer";   // drawSettings focuses its switch and lights the row, as showSetting does
    settingsJumpAt = time;
}

void App::openFindSetting() {
    // the console's and the phone's keys: what it finds shows as the words come
    if (openTextKb(settingsQuery, [this](const std::string& s) { settingsQuery = s; area("settings.pane").target = area("settings.pane").y = 0; }, "settings"))
        return;
    char out[128] = {};
    const plat::TextResult r = plat::askText("Settings", "Find a setting", settingsQuery.c_str(), out, sizeof out);
    if (r == plat::TextResult::Unavailable) toast("The keyboard is not available in applet mode");
    if (r != plat::TextResult::Done) return;
    settingsQuery = out;   // an empty answer clears the search
    area("settings.pane").target = area("settings.pane").y = 0;
}

/** A row of a list put in order by touch (the sidebar's tabs, the battery's details): a drag handle beside its switch, held and
 *  dragged. base: added to each item for its row's dragKey (two lists on one page must not share one); after: called each time
 *  it changes places. */
void App::dragSort(std::vector<int>& order, int base, const char* key, int i, float rx, float cy, const std::function<void()>& after,
                   const std::vector<int>* rows) {
    const int n = int(order.size());
    // rows: the items that have a row on the page (the sidebar's tabs: Server, which is in Library now, has none). The
    // rest are stepped over with the row dragged; counted as rows of the usual height they were phantom steps under the finger
    auto hasRow = [&](int idx) { return !rows || std::find(rows->begin(), rows->end(), order[size_t(idx)]) != rows->end(); };
    if (i < 0 || i >= n) return;
    const int me = base + order[size_t(i)];
    const float x = rx - 40 - 12;   // what comes before the switch's column
    const Id hid = uid(key, me);
    ui.item(hid, { x - 44, cy - 22, 44, 44 }, 10, Slider | Slider2D);   // held: it follows the finger at once (no scroll)
    static Id dragging = 0;
    static float anchorY = 0;   // where the finger would be with the row in its place
    const float dy = ui.sliderDragY(hid);
    if (dy >= 0) {
        if (dragging != hid) { dragging = hid; anchorY = dy; }
        auto heightOf = [](int t) { const auto f = tabRowH.find(t); return f != tabRowH.end() ? f->second : 56.f; };
        const float myH = heightOf(me);
        int at = i;
        // past half of the next row: they change places; the row passed slides from where it was to the place left
        auto nextRow = [&](int from) { int k = from + 1; while (k < n && !hasRow(k)) k++; return k; };          // n: none
        auto prevRow = [&](int from) { int k = from - 1; while (k >= 0 && !hasRow(k)) k--; return k; };         // -1: none
        for (int nx = nextRow(at); nx < n && dy - anchorY > heightOf(base + order[size_t(nx)]) * 0.5f; nx = nextRow(at)) {
            const int other = base + order[size_t(nx)];
            anchorY += heightOf(other);
            tabSlide[other] += myH;
            std::rotate(order.begin() + at, order.begin() + at + 1, order.begin() + nx + 1);   // (past it, and what has no row between)
            at = nx;
            if (after) after();
        }
        for (int pv = prevRow(at); pv >= 0 && anchorY - dy > heightOf(base + order[size_t(pv)]) * 0.5f; pv = prevRow(at)) {
            const int other = base + order[size_t(pv)];
            anchorY -= heightOf(other);
            tabSlide[other] -= myH;
            std::rotate(order.begin() + pv, order.begin() + at, order.begin() + at + 1);
            at = pv;
            if (after) after();
        }
        // under the finger, but not far past the first or the last place
        float off = dy - anchorY;
        if (prevRow(at) < 0) off = std::max(off, -10.f);
        if (nextRow(at) >= n) off = std::min(off, 10.f);
        tabSlide[me] = off;
        tabHeld = me;   // drawn floating, with its shadow (App::drawSettings)
    } else if (dragging == hid) {
        dragging = 0;
        tabHeld = -1;   // it settles from where the finger left it (tabSlide eases to 0)
        persistSoon(0.3f);
    }
    const bool hot = dy >= 0 || (ui.focused(hid) && ui.showFocusRing());
    dragGlyph(x - 44, cy, hot);
}

/** The drag handle's drawing (Settings' lists and every list in sort mode): its 44 wide box from boxX, the glyph in its middle. */
void App::dragGlyph(float boxX, float cy, bool hot) {
    if (hot) g.roundRect(boxX, cy - 20, 44, 40, 10, pal.text, 0.08f);
    g.icon(ICON_MENU, boxX + 22, cy, 20, hot ? pal.text : pal.text2);
}

// ============================================================ a list put in order by touch (its sort mode)
// The song lists and the Playlists page (App::TouchSort, app.hpp): Settings' way above, for lists that are not Settings rows. The
// list says where each item belongs this frame (slot) and draws each where sortAt puts it; the one held or settling last, over
// sortFloatBox. Items are told apart by their place, and their slides move with them when two change places.

bool App::sortBegin(Grab::Kind kind, int at, bool upNext, int ref, const std::string& srvPl) {
    grabDrop();
    sortEnd();
    if (!sortByDrag()) return false;   // a button or a key chose Move: the row is picked up for the D-pad (App::grab)
    tsort_ = TouchSort{};
    tsort_.on = true;
    tsort_.kind = kind;
    tsort_.upNext = upNext;
    tsort_.ref = ref;
    tsort_.srvPl = srvPl;
    tsort_.show = at;
    tsort_.ran = true;
    toast("Drag them into order, then " GF_TAP " Done");
    return true;
}

void App::sortEnd() {
    if (!tsort_.on) return;
    sortSave();
    tsort_ = TouchSort{};
}

/** What a finger lifting saves (and the mode ending): once for the whole drag, never at each place passed. */
void App::sortSave() {
    if (!tsort_.moved) return;
    tsort_.moved = false;
    switch (tsort_.kind) {
    case Grab::Playlists: store.save(); break;
    case Grab::Apps:
#ifdef GF_ANDROID
        appsSave();
#endif
        break;
    case Grab::SrvPlaylists: saveServerPlaylistOrder(); break;
    case Grab::SrvList: {   // a server playlist's new order, to the server (as grabDrop)
        const SrvPage& pg = srvPage_[0];
        if (pg.key == "P:" + tsort_.srvPl) {
            std::vector<std::string> ids;
            for (const auto& so : pg.songs) ids.push_back(so.id);
            subsonic.sendPlaylist(pg.name, tsort_.srvPl, ids);
        }
        break;
    }
    case Grab::List: persistSoon(0.3f); break;
    default: persistSoon(0.5f); break;   // the queue (queueEdited asked already; App::update waited for the finger)
    }
}

/** The item at from goes to to, the ones between closing up (each list's own way of holding its order). */
void App::sortMove(int from, int to) {
    if (from == to) return;
    switch (tsort_.kind) {
    case Grab::Playlists: movePlaylist(from, to, false); break;
    case Grab::Apps:
#ifdef GF_ANDROID
        appsMove(from, to);
#endif
        break;
    case Grab::SrvPlaylists: moveServerPlaylist(from, to, false); break;
    case Grab::SrvList: {
        SrvPage& pg = srvPage_[0];
        const int n = int(pg.songs.size());
        if (pg.key != "P:" + tsort_.srvPl || from < 0 || to < 0 || from >= n || to >= n) return;
        if (from < to) std::rotate(pg.songs.begin() + from, pg.songs.begin() + from + 1, pg.songs.begin() + to + 1);
        else std::rotate(pg.songs.begin() + to, pg.songs.begin() + from, pg.songs.begin() + from + 1);
        break;
    }
    case Grab::List: {   // a playlist of yours: its rows on the page are the files found; their paths change places in the playlist
        if (tsort_.ref < 0 || tsort_.ref >= int(store.playlists.size()) || !store.playlists[size_t(tsort_.ref)].own()) return;
        FolderPlaylist& p = store.playlists[size_t(tsort_.ref)];
        const std::vector<int> rows = playlistTracks(tsort_.ref);
        const int n = int(rows.size());
        if (from < 0 || to < 0 || from >= n || to >= n) return;
        const int d = to > from ? 1 : -1;
        std::vector<std::vector<std::string>::iterator> its;
        for (int k = from; k != to + d; k += d) {
            const auto it = std::find(p.tracks.begin(), p.tracks.end(), lib.tracks[size_t(rows[size_t(k)])].path);
            if (it == p.tracks.end()) return;
            its.push_back(it);
        }
        const std::string moved = *its.front();
        for (size_t k = 0; k + 1 < its.size(); k++) *its[k] = *its[k + 1];
        *its.back() = moved;
        if (p.automix) p.manual = true;   // moved by hand: it keeps this order until Order again
        break;
    }
    default: queueMove(from, to); break;
    }
    tsort_.moved = true;
}

void App::sortRun(int n, int lo, int hi, ScrollArea& sa, const std::function<SDL_FRect(int)>& slot, bool grid) {
    TouchSort& t = tsort_;
    t.ran = true;
    if (int(t.slide.size()) != n) {   // (the list changed under it: nothing held, nothing sliding)
        t.slide.assign(size_t(std::max(0, n)), SDL_FPoint{ 0, 0 });
        t.held = t.settle = -1;
    }
    hi = std::min(hi, n - 1);
    if (n <= 0 || lo > hi) return;
    const float k = store.motion ? std::exp(-dt / 0.07f) : 0.f;
    for (int i = 0; i < n; i++) {
        if (i == t.held) continue;
        SDL_FPoint& s = t.slide[size_t(i)];
        s.x *= k;
        s.y *= k;
        if (std::fabs(s.x) < 0.3f) s.x = 0;
        if (std::fabs(s.y) < 0.3f) s.y = 0;
    }
    if (t.settle >= n || (t.settle >= 0 && t.slide[size_t(t.settle)].x == 0 && t.slide[size_t(t.settle)].y == 0)) t.settle = -1;
    if (t.show >= 0) {   // the row Move was chosen on, in view (clear of Done at the foot)
        if (t.show < n) {
            const SDL_FRect r = slot(t.show);
            if (r.y < sa.view.y + 8 || r.y + r.h > sa.view.y + sa.view.h - 90) {
                sa.target += r.y + r.h / 2 - (sa.view.y + sa.view.h / 2);
                sa.clamp();
            }
        }
        t.show = -1;
    }
    if (t.held < 0) return;
    if (t.held < lo || t.held > hi) { t.held = -1; return; }
    const float fx = ui.sliderDrag(t.heldId), fy = ui.sliderDragY(t.heldId);
    if (fy < 0) {   // let go: it settles from where the finger left it, and its new place is saved
        const SDL_FRect r = slot(t.held);
        t.slide[size_t(t.held)] = { t.hx - r.x, t.hy - r.y };
        t.settle = t.held;
        t.held = -1;
        sortSave();
        return;
    }
    // under the finger, but not far past the first or the last place
    const SDL_FRect first = slot(lo), last = slot(hi);
    t.hy = std::clamp(fy - t.offY, first.y - 10, last.y + 10);
    // an item passed gives its place and slides from where it was: its offset is what it had plus the way between the two places
    auto pass = [&](int to) {
        const int from = t.held, d = to > from ? 1 : -1;
        const SDL_FPoint mine = t.slide[size_t(from)];
        for (int i = from; i != to; i += d) {
            const SDL_FRect a = slot(i + d), b = slot(i);
            t.slide[size_t(i)] = { t.slide[size_t(i + d)].x + a.x - b.x, t.slide[size_t(i + d)].y + a.y - b.y };
        }
        t.slide[size_t(to)] = mine;
        sortMove(from, to);
        t.settle = -1;
        t.held = to;
    };
    if (grid) {
        float minX = first.x, maxX = first.x;
        for (int i = lo; i <= hi && i < lo + 64; i++) { const float sx = slot(i).x; minX = std::min(minX, sx); maxX = std::max(maxX, sx); }
        t.hx = std::clamp(fx - t.offX, minX - 10, maxX + 10);
        // the place its middle is over
        const SDL_FRect me = slot(t.held);
        const float cx = t.hx + me.w / 2, cy = t.hy + me.h / 2;
        for (int i = lo; i <= hi; i++) {
            const SDL_FRect r = slot(i);
            if (cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h) { if (i != t.held) pass(i); break; }
        }
    } else {
        // past half of the next row: they change places (Settings' rule)
        t.hx = slot(t.held).x;
        while (t.held < hi && t.hy - slot(t.held).y > slot(t.held + 1).h * 0.5f) pass(t.held + 1);
        while (t.held > lo && slot(t.held).y - t.hy > slot(t.held - 1).h * 0.5f) pass(t.held - 1);
    }
    // held near the top or the foot of the list: the list moves under it, faster the nearer the edge
    const float edge = std::min(56.f, sa.view.h * 0.18f), top = sa.view.y, foot = sa.view.y + sa.view.h;   // (a short panel: a narrow band)
    float v = 0;
    if (fy < top + edge) v = -(1 - std::max(0.f, fy - top) / edge);
    else if (fy > foot - edge) v = 1 - std::max(0.f, foot - fy) / edge;
    if (v != 0 && sa.max() > 0) {
        sa.target = std::clamp(sa.target + v * 700 * dt, 0.f, sa.max());
        sa.velocity = 0;
        sa.manual = ui.now();
    }
}

SDL_FPoint App::sortAt(int i, const SDL_FRect& slot) const {
    const TouchSort& t = tsort_;
    if (!t.on || i < 0 || i >= int(t.slide.size())) return { slot.x, slot.y };
    if (i == t.held) return { t.hx, t.hy };
    return { slot.x + t.slide[size_t(i)].x, slot.y + t.slide[size_t(i)].y };
}

void App::sortGrip(const char* key, int i, const SDL_FRect& touch, const SDL_FRect& at) {
    TouchSort& t = tsort_;
    Id id = uid(key, i);
    // the item held keeps the id the finger took, wherever it is now; the one now in that place takes another meanwhile
    if (t.held >= 0) {
        if (i == t.held) id = t.heldId;
        else if (id == t.heldId) id = uid(key, -1000 - i);
    }
    ui.item(id, touch, 10, Slider | Slider2D);   // held: it follows the finger at once (no scroll, no tap)
    if (t.held < 0 && t.endIn == 0 && ui.sliderDragY(id) >= 0) {
        t.held = i;
        t.heldId = id;
        t.settle = -1;
        t.offX = ui.sliderDrag(id) - at.x;
        t.offY = ui.sliderDragY(id) - at.y;
        t.hx = at.x;
        t.hy = at.y;
        if (i < int(t.slide.size())) t.slide[size_t(i)] = { 0, 0 };
    }
}

void App::sortHandle(const char* key, int i, float rx, float cy, const SDL_FRect& at) {
    const float boxX = rx + 12 - 44;   // (Settings': the glyph 20 wide in the middle of its 44 box)
    sortGrip(key, i, { boxX, cy - 22, 44, 44 }, at);
    dragGlyph(boxX, cy, i == tsort_.held);
}

void App::sortFloatBox(float x, float y, float w, float h, float rad) {
    // the shadow, 6 lower: round its box and inside it too (the strip of it under the row showed the page through, a bright lip
    // on a page lighter than Settings' cards)
    g.glowAround(x, y + 6, w, h, rad, 18, 10, { 0, 0, 0 }, 0.45f);
    g.glowInside(x, y + 6, w, h, rad, { 0, 0, 0 }, 0.45f);
    g.roundRect(x, y, w, h, rad, pal.bg, 1);
    g.roundRect(x, y, w, h, rad, pal.surface, 0.9f);
}

/** Done, over the page's foot while a list is in sort mode. */
void App::drawSortDone() {
    sortPill_ = {};
    if (!tsort_.on || canvas) return;
    const Font bf{ css::Sm, Weight::SemiBold };
    const float h = 52, doneW = 16 + 24 + g.measure("Done", bf) + 16, w = 9 + doneW + 9;
    float bottom = css::H - css::BarH - 14;
#ifdef GF_ANDROID
    if (phonePortrait()) bottom = phoneFoot();
#endif
    const float x = std::round((css::W - w) / 2), y = bottom - h, cy = y + h / 2;
    sortPill_ = { x - 8, y - 8, w + 16, h + 16 };
#ifdef GF_DEV
    const int layerWas = g.saidLayer;
    g.saidLayer = 1;   // (tests: over the page)
#endif
    g.roundRect(x, y + 6, w, h, h / 2, { 0, 0, 0 }, 0.25f);
    g.roundRect(x, y, w, h, h / 2, pal.surface2);
    g.roundStroke(x, y, w, h, h / 2, 1, pal.text, 0.12f);
    bool done = false;
    button(uid("sort.done"), ICON_CHECK, "Done", x + 9, cy - 17, true, &done);
    if (done) sortEnd();
#ifdef GF_DEV
    g.saidLayer = layerWas;
#endif
}

void App::drawSettings(float x, float y, float w, float h) {
    const Font labelF{ css::Md, Weight::SemiBold }, hintF{ css::Sm }, valueF{ css::Md };
    // A category chosen last frame: its page was drawn then, so its first setting is known now
    if (settingsFocusFirst) {
        settingsFocusFirst = false;
        // (none shown: a page whose top is only words, as Controls' Buttons on the Switch and the PC: its first setting further down;
        // the user, 2026-11-20: Controls did not take the focus as the other pages do)
        Id first = ui.firstIn(&area("settings.pane"));
        if (!first) first = ui.firstIn(&area("settings.pane"), true);
        if (first) ui.focus(first);
    }

    // ------------------------------------------------------------ the rows, by category
    auto toggleRow = [&](const std::string& label, const std::string& hint, const char* key, bool& v) {
        return Row{ label, "", hint, 40, [this, key, &v](float rx, float cy) {
                       if (toggle(uid(key), rx - 40, cy, v)) { v = !v; persistSoon(0.3f); }
                   } };
    };
    auto sliderRow = [&](const std::string& label, const std::string& value, const std::string& hint, const char* key, float& v,
                         std::function<void()> changed = nullptr) {
        return Row{ label, value, hint, 180, [this, key, &v, changed](float rx, float cy) {
                       float nv = v;
                       slider(uid(key), rx - setSliderW(), cy, setSliderW(), v, &nv);
                       if (nv != v) { v = nv; persistSoon(0.8f); if (changed) changed(); }
                   } };
    };
    auto segRow = [&](const std::string& label, const std::string& hint, const char* key, std::vector<std::string> opts, int cur,
                      std::function<void(int)> pick, unsigned disabled = 0) {
        float sw = 6;
        for (const auto& o : opts) sw += g.measure(o, { css::Sm, Weight::SemiBold }) + 26;
        Row r{ label, "", hint, sw, [this, key, opts, cur, pick, disabled](float rx, float cy) {
                  int p = -1;
                  segmented(uid(key), rx, cy, opts, cur, &p, disabled, settingsCtlMax_, settingsCtlMax_ > 0);   // (on its own line: the line's width)
                  if (p >= 0 && p != cur) { pick(p); persistSoon(0.3f); }
              } };
        r.fill = true;
        for (const auto& o : opts) r.words += g.measure(o, { css::Sm, Weight::SemiBold });
        r.nOpts = opts.size();
        return r;
    };
    auto buttonRow = [&](const std::string& label, const std::string& hint, const char* key, const char* icon, const std::string& text,
                         std::function<void()> act, bool danger = false, float minW = 0) {
        const float bw = std::max(minW, float(g.measure(text, { css::Sm, Weight::SemiBold }) + 32 + (icon ? 24 : 0)));
        Row r{ label, "", hint, bw, [this, key, icon, text, act, bw, danger](float rx, float cy) {
                  bool c = false;
                  button(uid(key), icon, text, rx - bw, cy - 17, false, &c, false, danger, 0, bw);
                  if (c) act();
              } };
        r.focus = uid(key);
        return r;
    };
    // Two buttons on one row (the equalizer's Save / Save new): the first is the row's focus
    // (w: at least this wide, its words centred: buttons in one column, one width; the user, 2026-11-16)
    struct Btn { const char* key; const char* icon; std::string text; std::function<void()> act; bool danger = false; float w = 0; };
    auto buttonsRow = [&](const std::string& label, const std::string& hint, std::vector<Btn> btns) {
        float total = 0;
        std::vector<float> ws;
        for (const Btn& b : btns) {
            ws.push_back(std::max(b.w, float(g.measure(b.text, { css::Sm, Weight::SemiBold }) + 32 + (b.icon ? 24 : 0))));
            total += ws.back() + (total > 0 ? 8 : 0);
        }
        Row r{ label, "", hint, total, [this, btns, ws, total](float rx, float cy) {
                  float bx = rx - total;
                  for (size_t i = 0; i < btns.size(); i++) {
                      bool c = false;
                      button(uid(btns[i].key), btns[i].icon, btns[i].text, bx, cy - 17, false, &c, false, btns[i].danger, 0, ws[i]);
                      if (c) btns[i].act();
                      bx += ws[i] + 8;
                  }
              } };
        if (!btns.empty()) r.focus = uid(btns[0].key);
        return r;
    };
    auto infoRow = [&](const std::string& label, const std::string& hint, const std::string& value = {}) {
        // Values are counts that change (tags read, covers ready): fixed-width digits, so they do not jitter.
        const float vw = value.empty() ? 0 : g.measureNum(value, valueF);
        return Row{ label, "", hint, vw, value.empty() ? std::function<void(float, float)>() : [this, value](float rx, float cy) {
                                                              g.textNum(value, { css::Md }, rx, cy - g.lineHeight({ css::Md }) / 2.f, pal.text2, 1,
                                                                        Align::Right);
                                                          } };
    };
    auto pct = [](float v) { return std::to_string(int(std::lround(v * 100))) + "%"; };
    auto oneDecimal = [](double v) { char b[16]; snprintf(b, sizeof b, "%.1f", v); return std::string(b); };

    std::vector<Section> sections;
    std::vector<Card> r4LedCards;   // the HiBy R4's Status LED page (listed after Appearance)
    static float satShown, trShown;   // Saturation 0..100% and Colour transition 0..3 s, as the slider's 0..1
    static float sideOpShown;         // Sidebar opacity 50..100 %, as the slider's 0..1
    sideOpShown = (std::clamp(store.sideOpacity, 0.5f, 1.f) - 0.5f) / 0.5f;
    satShown = store.saturation;   // 0..100%: past 100% the palette changes no further
    trShown = store.colourTransition / 3.f;

    // Appearance
    std::vector<Row> sidebarRows;
    {
        // Settings → Appearance → Sidebar: the tabs in the sidebar's order (store.tabOrder), each shown or hidden with its
        // switch (Store::hiddenTabs; Now playing always shows) and put in order as a queue row is: Move picks it up, the
        // D-pad moves it, A or B puts it down (App::grab). The button keeps its id per tab, so the focus moves with it.
        const Font bf{ css::Sm, Weight::SemiBold };
        const float mw = g.measure("Move", bf) + 32 + 24;
        const float rw = 34;   // Remove: a round icon button
        int lines = 0;
        // By touch: a drag handle beside each tab's switch, held and dragged (the user chose it for a phone upright, 2026-11-04: "B";
        // Move put the tab down at the next touch). Then "only change to the sort mode by touch when touch is detected and be how it
        // was for joycons": the handle once a finger is on the screen, Move again at a button, a key or a click.
        const int sortWay = css::W < kNarrowList || sortByDrag() ? 1 : 0;
        const float sortW = 40 + 12 + 44;
        auto sortCtl = [this](int i, int, float rx, float cy) {
            std::vector<int> rows;   // the tabs with a row here (sidebarTabsShown), by what they are
            for (int k : sidebarTabsShown()) rows.push_back(store.tabOrder[size_t(k)]);
            dragSort(store.tabOrder, 0, "set.tabdrag", i, rx, cy, nullptr, &rows);
        };
        // The rows are the tabs the sidebar can show, in its order (App::sidebarTabsShown), and Move steps along this same list.
        // The real store.tabOrder index is kept (i) so Move, the drag and Remove act on the right tab.
        const std::vector<int> order = sidebarTabsShown();
        for (int oi = 0; oi < int(order.size()); oi++) {
            const int i = order[size_t(oi)];
            const int tab = store.tabOrder[size_t(i)];
            const unsigned bit = Store::tabBit(tab);
            const bool held = grab.kind == Grab::Tabs && grab.pos == i;
            if (Store::isLine(tab)) {   // one of your lines: Move it, or Remove it
                lines++;
                // The same columns as the tabs' rows: Move where their Move is, the cross centred where their switch is
                Row r{ "Line", "", held && !sortWay ? GF_MOVING_WORDS : "A thin line in the sidebar between the tabs around it",
                       sortWay ? sortW : 40 + 16 + mw, [this, tab, i, mw, rw, sortWay, sortCtl](float rx, float cy) {
                           bool c = false;
                           if (sortWay) sortCtl(i, tab, rx, cy);
                           {   // Remove: round, the red cross only (as the playlist page's compact Delete)
                               const float cx = rx - 20 - rw / 2;
                               const Id did = uid("set.tabdel", tab);
                               c = ui.item(did, { cx, cy - 17, 34, 34 }, 17);
                               const bool hot = ui.pressed(did) || (ui.focused(did) && ui.showFocusRing());
                               g.roundRect(cx, cy - 17, 34, 34, 17, pal.text, hot ? 0.11f : 0.065f);
                               g.roundStroke(cx, cy - 17, 34, 34, 17, 1, pal.text, 0.15f);
                               g.icon(ICON_X, cx + 17, cy, 16, dangerInk());
                           }
                           if (c) {
                               if (i < int(store.tabOrder.size()) && store.tabOrder[size_t(i)] == tab) store.tabOrder.erase(store.tabOrder.begin() + i);
                               persistSoon(0.3f);
                               return;
                           }
                           if (sortWay) return;
                           button(uid("set.tabmove", tab), ICON_SORT_DOWN, "Move", rx - 40 - 16 - mw, cy - 17, false, &c);
                           if (c && !sortByDrag()) {
                               grab = Grab{};
                               grab.kind = Grab::Tabs;
                               grab.pos = i;
                               toast(GF_MOVE_WORDS);
                           }
                       } };
                r.lifted = held;
                r.minH = 48;
                r.dragKey = tab;
                sidebarRows.push_back(r);
                continue;
            }
            const char* hint = held && !sortWay        ? GF_MOVING_WORDS
                             : tab == Store::TabNow    ? "Always shown. Settings stays at the bottom"
                             : tab == Store::TabFiles  ? "Everything " GF_ON_CARD ": open, rename, copy, move and delete, and what fits each file"
                                                       : "";
            Row r{ Store::tabName(tab), "", hint, sortWay ? sortW : 40 + 16 + mw, [this, tab, bit, i, mw, sortWay, sortCtl](float rx, float cy) {
                      if (bit && !(r4Dev_ && tab == Store::TabApps)) {   // (the R4's Apps tab: always there, no switch)
                          const bool shown = !(store.hiddenTabs & bit);
                          if (toggle(uid("set.tab", int(bit)), rx - 40, cy, shown)) {
                              store.hiddenTabs ^= bit;
                              persistSoon(0.3f);
                          }
                      }
                      if (sortWay) { sortCtl(i, tab, rx, cy); return; }
                      bool c = false;
                      button(uid("set.tabmove", tab), ICON_SORT_DOWN, "Move", rx - 40 - 16 - mw, cy - 17, false, &c);
                      if (c && !sortByDrag()) {
                          grab = Grab{};
                          grab.kind = Grab::Tabs;
                          grab.pos = i;
                          toast(GF_MOVE_WORDS);
                      }
                  } };
            r.lifted = held;
            r.dragKey = tab;
            sidebarRows.push_back(r);
        }
        // A new line goes in at the end, picked up, so it is placed straight away
        if (lines < Store::MaxLines) {
            Row add = buttonRow("Add a line", "Separates the tabs in groups, as you like (up to four)", "set.tabaddline", ICON_PLUS, "Add", [this] {
                int id = Store::TabLine;
                while (std::find(store.tabOrder.begin(), store.tabOrder.end(), id) != store.tabOrder.end()) id++;
                store.tabOrder.push_back(id);
                persistSoon(0.3f);
                grab = Grab{};
                grab.kind = Grab::Tabs;
                grab.pos = int(store.tabOrder.size()) - 1;
                toast(GF_MOVE_WORDS);
            });
            sidebarRows.push_back(add);
        } else {
            sidebarRows.push_back(infoRow("Add a line", "Four lines at most: remove one to add another"));
        }
    }
    {
        // Interface colours: Gatefold's cards in one row (From the cover, Dark, Light, Custom). Grey is left out at the
        // user's request, to keep the page shorter; a saved Grey reads as Dark (Store::load).
        Row themes;
        themes.blockH = 150;
        themes.block = [this](float bx, float by, float bw) {
            struct Opt { const char* name; int id; Palette p; };
            const Palette custom = store.theme == 4 ? target : customPalette();
            const Opt opts[4] = { { "From the cover", 0, coverPal }, { "Dark", 1, paletteFixed(FixedTheme::Dark) },
                                  { "Light", 3, paletteFixed(FixedTheme::Light) }, { "Custom", 4, custom } };
            const float gap = 12, cw = (bw - gap * 3) / 4, ch = 150;
            // (the longest name with the chosen one's dot: where it does not fit on the tile's line, every tile as the narrow ones)
            const bool smallNames = cw < 110 || g.measure(opts[0].name, { css::Base, Weight::SemiBold }) + 14 + 14 + 10 > cw;
            for (int i = 0; i < 4; i++) {
                const float cx = bx + i * (cw + gap), cy = by;
                const Id id = uid("set.theme", opts[i].id);
                if (ui.item(id, { cx, cy, cw, ch }, 14)) { store.theme = opts[i].id; applyTheme(); persistSoon(0.3f); }
                const bool on = store.theme == opts[i].id, hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
                g.roundRect(cx, cy, cw, ch, 14, pal.text, hot ? 0.08f : 0.04f);
                g.roundStroke(cx, cy, cw, ch, 14, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.09f);
                // .theme-card .pv: a tiny window in that palette — sidebar strip, two text lines, an accent pill
                const Palette& p = opts[i].p;
                const float px = cx + 10, py = cy + 10, pw = cw - 20, ph = ch - 50;
                g.roundRect(px, py, pw, ph, 9, p.bg);
                if (i == 0) {
                    g.glow(px + pw * 0.2f, py + ph * 0.3f, pw * 0.6f, p.blobs[0], 0.9f);
                    g.glow(px + pw * 0.85f, py + ph * 0.8f, pw * 0.5f, p.blobs[2], 0.9f);
                    g.glow(px + pw * 0.7f, py + ph * 0.1f, pw * 0.35f, p.accent, 0.45f);
                }
                g.roundRect(px, py, pw * 0.24f, ph, 9, p.surface, 0.85f);
                const float tx = px + pw * 0.34f;
                g.roundRect(tx, py + ph * 0.24f, pw * 0.48f, 7, 3.5f, p.text);
                g.roundRect(tx, py + ph * 0.42f, pw * 0.32f, 6, 3, p.text3);
                g.roundRect(tx, py + ph * 0.62f, pw * 0.24f, 14, 7, p.accent);
                g.roundStroke(px, py, pw, ph, 9, 1, p.dark ? Rgb{ 1, 1, 1 } : Rgb{ 0, 0, 0 }, 0.06f);
                if (smallNames) {   // a narrow tile (a phone upright): its name centred, smaller, on two lines if it needs them
                    const Font nf{ css::Sm, Weight::SemiBold };
                    const auto nl2 = g.wrap(opts[i].name, nf, int(cw - 12), 2);
                    float ny = cy + ch - 20 - (nl2.size() - 1) * 8.f;
                    for (const auto& l : nl2) { g.textC(l, nf, cx + cw / 2, ny, on ? pal.text : pal.text2, 1, Align::Center); ny += 16; }
                } else {
                    float lx = cx + 14;
                    if (on) { g.circle(lx + 4, cy + ch - 20, 4, pal.accent); lx += 14; }
                    g.textC(opts[i].name, { css::Base, Weight::SemiBold }, lx, cy + ch - 20, on ? pal.text : pal.text2, 1, Align::Left,
                            int(cx + cw - 10 - lx));
                }
            }
        };
        static const std::vector<std::string> brightNames = { "Dark", "Light" };
        // Custom has its own: Dark, Light, or the background exactly as picked
        static const std::vector<std::string> customBrightNames = { "Dark", "Light", "As picked" };
        const Row brightness =
            store.theme == 4
                ? segRow("Brightness", "Your colours on a dark or a light background, or yours as picked, kept readable",
                         "set.cbright", customBrightNames, store.customMode, [this](int i) { store.customMode = i; applyTheme(); })
                : segRow("Brightness",
                         "From the cover, on a dark or a light background",
                         "set.bright", brightNames, store.brightness, [this](int i) { store.brightness = i; applyTheme(); });
        const Row saturation = sliderRow("Saturation", std::to_string(int(std::lround(store.saturation * 100))) + "%", "How strongly the cover tints the interface",
                                         "set.sat", satShown, [this] { store.saturation = std::round(satShown * 100) / 100; applyTheme(); });
        char trV[16];
        snprintf(trV, sizeof trV, "%.1f s", double(store.colourTransition));
        const Row transition = sliderRow("Colour transition", trV, "How long the colours take to follow a new cover", "set.tr", trShown,
                                         [this] { store.colourTransition = std::round(trShown * 3 * 10) / 10; });
        // Cover palette: the eight colours read from the cover playing (.swatches)
        Row swatches{ "Cover palette", "", coverPal.swatchCount ? "" : "Play something to see its colours", 8 * 34.f, [this](float rx, float cy) {
                         const int n = coverPal.swatchCount;
                         for (int i = 0; i < n; i++) {
                             const float sx = rx - (n - i) * 34;
                             g.roundRect(sx, cy - 15, 28, 30, 8, coverPal.swatches[i].c);
                             g.roundStroke(sx, cy - 15, 28, 30, 8, 1, pal.text, 0.12f);
                         }
                     } };
        // Custom: your background and two accents, each from the cover playing or a few of Gatefold's own
        std::vector<Row> customRows;
        if (store.theme == 4) {
            // The last swatch opens the colour picker; it shows your own colour, ringed, when it is none of the others.
            auto chooser = [this](const char* label, const char* hint, const char* key, int* field, bool allowAuto, Rgb shown) {
                std::vector<int> choices;
                if (allowAuto) choices.push_back(-1);
                for (int i = 0; i < coverPal.swatchCount; i++) {
                    const Rgb& c = coverPal.swatches[i].c;
                    choices.push_back(int(std::lround(c.r * 255)) << 16 | int(std::lround(c.g * 255)) << 8 | int(std::lround(c.b * 255)));
                }
                for (int v : { 0x16181f, 0xf4f1ea, 0x8fb4ff, 0xff5a6a, 0x4cd6a0, 0xffc857 }) choices.push_back(v);
                const float cw = (choices.size() + 1) * 30.f;
                Row r{ label, "", hint, cw, [this, label, key, field, choices, shown](float rx, float cy0) {
                               const int n = int(choices.size()) + 1;
                               const bool lead = choices[0] < 0;
                               for (int i = 0; i < int(choices.size()); i++) {
                                   const Round at = roundAt(i, n, rx, cy0, settingsCtlMax_, lead);
                                   const float sx = at.x, cy = at.cy, d = at.d;
                                   const Id id = uid(key, i);
                                   if (ui.item(id, { sx, cy - d / 2, d, d }, d / 2)) { *field = choices[i]; applyTheme(); persistSoon(0.3f); }
                                   const int v = choices[i];
                                   if (v < 0) drawRoundA(g, pal, sx, cy, d);
                                   else g.circle(sx + 13, cy, 12, Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f });
                                   const bool on = *field == v, hot = ui.focused(id) && ui.showFocusRing();
                                   if (on || hot) g.roundStroke(sx - 2, cy - d / 2 - 2, d + 4, d + 4, d / 2 + 2, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.4f);
                               }
                               // the picker: a colour wheel, or your own colour in the middle of it
                               const Round at = roundAt(n - 1, n, rx, cy0, settingsCtlMax_, lead);
                               const float sx = at.x, cy = at.cy, d = at.d, k = d / 26;
                               const Id id = uid(key, 99);
                               if (ui.item(id, { sx, cy - d / 2, d, d }, d / 2))
                                   openColourPicker(field, label, *field >= 0 ? Rgb{ ((*field >> 16) & 255) / 255.f, ((*field >> 8) & 255) / 255.f, (*field & 255) / 255.f } : shown);
                               const bool own = *field >= 0 && std::find(choices.begin(), choices.end(), *field) == choices.end();
                               if (SDL_Texture* wheel = wheelTexture()) g.imageStretched(wheel, sx + 1, cy - d / 2 + 1, d - 2, d - 2, 0);
                               if (own) {
                                   g.circle(sx + d / 2, cy, 8.5f * k, { 0, 0, 0 }, 0.35f);
                                   g.circle(sx + d / 2, cy, 7 * k, Rgb{ ((*field >> 16) & 255) / 255.f, ((*field >> 8) & 255) / 255.f, (*field & 255) / 255.f });
                               }
                               const bool hot = ui.focused(id) && ui.showFocusRing();
                               if (own || hot) g.roundStroke(sx - 2, cy - d / 2 - 2, d + 4, d + 4, d / 2 + 2, own ? 2.f : 1.f, own ? pal.accent : pal.text, own ? 1.f : 0.4f);
                           } };
                r.rounds = int(choices.size()) + 1;
                r.roundsLead = allowAuto;
                return r;
            };
            customRows.push_back(chooser("Background", "Behind every page and the bars", "set.cbg", &store.customBg, true, pal.bg));
            customRows.push_back(chooser("Accent", "Buttons, sliders and what is playing", "set.cacc", &store.customAccent, false, pal.accent));
            customRows.push_back(chooser("Second accent", "Where gradients start: the seek bar, the spectrum", "set.cacc2", &store.customAccent2,
                                         true, pal.accent2));
            customRows.push_back(infoRow("Your colours", "Text too close to the background moves just enough to stay readable"));
        }
        // Brightness only where it does something: From the cover and Custom (Dark and Light are their own)
        std::vector<Row> colourRows = { themes };
        if (store.theme == 0 || store.theme == 4) colourRows.push_back(brightness);
        for (const Row& r : { saturation, transition, swatches }) colourRows.push_back(r);
        std::vector<Card> cards = { { "Interface colours", colourRows } };
        if (!customRows.empty()) cards.push_back({ "Custom", customRows, "Interface colours" });   // only while Custom is chosen
        // Collapse the sidebar: the Sidebar card's first row (the user, 2026-11-18: "should be in the same row as sidebar and not
        // layout"); Layout keeps what is left (Android's Size and Screen rotation), and goes where nothing is left
        sidebarRows.insert(sidebarRows.begin(), toggleRow("Collapse the sidebar",
#ifdef GF_WINDOWED
                                      // (narrow, it is folded whatever this says: said, so the switch is not taken for broken)
                                      narrow_ ? "This window is narrow, so it is folded for now: your choice is for a window with room" :
#endif
                                      "Icons only, more room for everything else", "set.side", store.sidebarCollapsed));
        cards.push_back({ "Layout", {} });
        const size_t layoutCard = cards.size() - 1;   // (Android adds its rows to it below: Screen rotation, and Size sideways)
        // Background, a card of its own (the user, 2026-11-13: "Layout should have Collapse setting and screen rotation. Background
        // (New row) should have Background motion, blur and smoothness")
        cards.push_back({ "Background",
                          { toggleRow("Background motion", "The slowly drifting colours behind every view and the Canvas", "set.motion",
                                      store.motion),
                            // (the user, 2026-11-13: today's blur "looks pixelated but I like it this way but some people might not")
                            segRow("Background blur", "How the cover is blurred behind every page", "set.bgblur", { "Classic", "Smooth" }, store.bgBlur,
                                   [this](int i) { setBackdrop(i, store.bgSmooth); }) } });
        if (store.bgBlur == 1)
            cards.back().rows.push_back(segRow("Smoothness", "Light keeps the cover's shapes; Strong is an even wash of its colours", "set.bgsmooth",
                                               { "Light", "Medium", "Strong" }, store.bgSmooth,
                                               [this](int i) { setBackdrop(store.bgBlur, i); }));
#ifdef GF_ANDROID
        // Screen rotation (the user, 2026-11-13, Android only: "so the screen does not rotate to landscape even if autorotate is
        // enabled in android settings", and the way to change between the two): Auto is the device's own turning, as it was
        cards[layoutCard].rows.push_back(segRow("Screen rotation", "Auto follows the device; Portrait and Landscape hold Songate that way", "set.turn",
                                           { "Auto", "Portrait", "Landscape" }, store.screenTurn,
                                           [this](int i) { store.screenTurn = i; plat::screenRotation(i); }));
        // Size, sideways only (the user, 2026-11-04: "in portrait the default is standard but there will not be options to choose. in
        // landscape yes"): upright is always Standard, its row not shown there
        // (both ways up, each its own: the user, 2026-11-15, "each layout has it's own layout size"; upright had none till then)
        {
            int& sz = css::Phone ? store.uiSizeUp : store.uiSize;
            const char* hint = sz == 1 ? "A little larger, for a phone held close" : sz == 2 ? "Larger still: less fits on each page"
                             : css::Phone ? "As drawn for a phone held upright" : "As on the Switch and the PC";
            cards[layoutCard].rows.insert(cards[layoutCard].rows.begin(),
                                     segRow("Size", hint, "set.uisize", { "Standard", "Medium", "Largest" }, sz, [this](int i) {
                                                (css::Phone ? store.uiSizeUp : store.uiSize) = i;
                                                fitPending_ = true;   // laid out again between frames
                                            }));
        }
        // The camera's cutouts and the rounded corners, whatever the phone or tablet has (the user, 2026-11-03: "works on every android
        // or tablet ... give users lots of customisation options"): App::updateEdges. Not on the HiBy R4: no cutout, no camera, its
        // corners square (the user, 2026-11-16)
        if (!r4Dev_) {
            static const char* const edgeHints[] = {
                "Only what a cutout would cover moves, by just enough",
                "Nothing is placed on a cutout's side; Songate's colours still reach the edge",
                "A black band on a cutout's side, as Android draws one",
                "Nothing moves: a camera may cover a corner of the bars" };
            const int extraPx = int(std::lround(store.edgeExtra * 24));
            cards.push_back({ "Screen edges",
                              { segRow("Around the camera", edgeHints[std::clamp(store.edgeMode, 0, 3)], "set.edgemode",
                                       { "Step aside", "Clear side", "Black strip", "Whole screen" }, store.edgeMode,
                                       [this](int i) { store.edgeMode = i; }),
                                sliderRow("Extra room", std::to_string(extraPx) + " px",
                                          "More space around a cutout, if something is still covered", "set.edgeextra",
                                          store.edgeExtra),
                                toggleRow("Rounded corners", "The bars' ends keep clear of the screen's rounded corners", "set.edgecorners",
                                          store.edgeCorners),
                                buttonRow("Show where it is", "Outlines the cutouts and the corners for a few seconds", "set.edgeshow", ICON_INFO,
                                          "Show", [this] { edgeShowUntil_ = time + 5; }) } });
        }
#endif
        // The opening can be turned off, and its bells with it left on (the user, 2026-11-02): the sound's row only while there is an
        // opening to ring with. Here, with how Songate looks, not in Playback (the user, 2026-11-03: "I think it makes more sense")
        // (Not on the HiBy R4: its boot animation plays the opening; the user chose to hide the card there, 2026-11-15)
        if (!bootOpening_) {
            std::vector<Row> opening{ toggleRow("Opening animation", "A star crosses the name as Songate opens. Any press skips it",
                                                "set.opening", store.opening) };
            if (store.opening)
                opening.push_back(toggleRow("Opening sound", "A few soft bells with it. Never over music", "set.opensound", store.openingSound));
            cards.push_back({ "Opening", opening });
        }
        // How an Automix join shows (the user, 2026-10-29), on Now playing and on the Canvas, each its own; where the hint
        // sits, only for Now playing's Simple (the user, 2026-10-29: every other animation has its own place)
        std::vector<Row> mixRows = { segRow("On Now playing",
                                   store.mixAnimNow == 0   ? "The cover glides to the middle; everything else steps aside"
                                   : store.mixAnimNow == 1 ? "The cover glides to the middle; the bars stay"
                                                           : "The page changes from one song to the next, left to right",
                                   "set.mixnow", { "Full", "Immersive", "Simple" }, store.mixAnimNow, [this](int i) { store.mixAnimNow = i; }),
                            segRow("On the Canvas",
                                   store.mixAnimCanvas == 0 ? "The cover glides to the middle, frosted with the hint"
                                                            : "The page changes from one song to the next, left to right",
                                   "set.mixcanvas", { "Full", "Simple" }, store.mixAnimCanvas, [this](int i) { store.mixAnimCanvas = i; }) };
        if (store.mixAnimNow == 2)
            mixRows.push_back(segRow("The hint on Now playing",
                                     store.mixHint == 1 ? "On the cover; on the Lyrics tab, in the lyrics' place"
                                                        : "Above the cover; on the Lyrics tab, in the lyrics' place",
                                     "set.mixhint", { "Automatic", "On the cover", "Above" }, store.mixHint, [this](int i) { store.mixHint = i; }));
        if (!css::Phone) cards.push_back({ "Automix", mixRows });   // upright: Full only, nothing to choose (App::mixCanvasMode)
#ifdef GF_WINDOWED
        // Sidebar opacity, where the sidebar opens over the page: a narrow window, a phone or a small player (held sideways there;
        // upright has no sidebar, and says so nowhere: the row is for the other way of holding it)
#ifndef GF_ANDROID
        if (narrow_)
#endif
        {
            char opV[16];
            snprintf(opV, sizeof opV, "%d %%", int(std::lround(std::clamp(store.sideOpacity, 0.5f, 1.f) * 100)));
            sidebarRows.push_back(sliderRow("Sidebar opacity", opV, "How solid the sidebar is when it opens over the page", "set.sideop", sideOpShown,
                                            [this] { store.sideOpacity = std::round((0.5f + sideOpShown * 0.5f) * 100) / 100; }));
        }
#endif
        // (Android with the screen held upright for good, Layout › Screen rotation Portrait: no sidebar ever, so no card for it; the user,
        // 2026-11-16. Upright's Library keeps its Files row by its own switch, Storage › Files)
        bool sidebarEver = true;
#ifdef GF_ANDROID
        sidebarEver = store.screenTurn != 1;
#endif
        if (sidebarEver) cards.push_back({ "Sidebar", sidebarRows });
        {   // the #-to-Z rail down a list's right edge, per list and per library (the user, 2026-11-09: each library its own set, the
            // card listing the lists that library has; the title bar's switch says which): all on to begin with
            struct RailOpt { const char* name; const char* hint; unsigned bit; };
            static const RailOpt rails[] = { { "Albums", "By artist and by title", 1u }, { "Artists", "And an artist's page", 2u }, { "Genres", "And a genre's page", 4u },
                                             { "Folders", "A folder's folders, or its songs", 8u }, { "Songs", "When the sort has letters", 16u } };
            // (no row for playlists: the Playlists page and a playlist's songs are in your order, so no rail there; the user, 2026-11-10)
            const int set = railSet();
            std::string who = set == 0 ? std::string("Local") : subsonic.view().kind;
            if (who.empty()) who = "the server"; else who[0] = char(toupper(uint8_t(who[0])));
            std::vector<Row> railRows;
            railRows.push_back(infoRow("A rail of letters down the right edge", "Tap a letter, or slide a finger along the rail, to jump through a long list. Each library keeps its own: switch up top to set another"));
            for (const RailOpt& o : rails) {
                const unsigned bit = o.bit;
                Row r{ o.name, "", o.hint, 40, [this, bit, set](float rx, float cy) {
                          if (toggle(uid("set.rail", int(bit)), rx - 40, cy, (store.letterRails[set] & bit) != 0)) { store.letterRails[set] ^= bit; persistSoon(0.3f); }
                      } };
                railRows.push_back(r);
            }
            cards.push_back({ set == 0 ? std::string("Letter rail") : "Letter rail · " + who, railRows });   // (the local library's: no word after it, the user 2026-11-16)
        }
#ifdef GF_WINDOWED
        unsigned batteryNow = 0;
        if (plat::batteryLevel(batteryNow))   // a desktop has none
#endif
        {   // the battery (BETA 3: "almost everything" yours): what shows beside it, in your order; each state's colour and animation
            // what shows beside it: put in order as the sidebar's tabs are (a handle by touch; Move with the buttons)
            std::vector<Row> top;
            static std::vector<int> vis;
            vis = batteryItems();
            const Font bf{ css::Sm, Weight::SemiBold };
            const float mw = g.measure("Move", bf) + 32 + 24;
            const bool sortWay = css::W < kNarrowList || sortByDrag();
            const float sortW = 40 + 12 + 44;
            static const char* names[7] = { "Percentage", "Time", "Charger watts", "Temperature", "Voltage", "Health", "Power" };
            static const char* hints[7] = { "Beside the battery, top right, in this order. Press the battery for everything about it",
                                            "Time to empty, or to full while charging", "While charging: what the charger agreed to give",
                                            "The battery's", "The battery's", "How much it holds against when it was new",
                                            "What the battery gives now (−) or takes in (+), in watts" };
            // only as many as fit beside it (the user, 2026-11-05: "once 2/3 option are already enable ... grey out according to space
            // available"): each at its widest, against the title bar's room; one that would not fit is greyed until another is off
            static const char* widest[7] = { "100%", "full in 2 h 10", "100 W", "100 °F", "4.40 V", "health 100%", "\u221212.5 W" };
            const Font tf{ css::Base };
            const float sep = float(g.measure("x  ·  x", tf) - g.measure("xx", tf));
            float used = 0;
            for (int it : vis)
                if (*batteryShown(it)) used += (used > 0 ? sep : 0) + float(g.measure(widest[it], tf));
            // Home | Canvas (the user, 2026-11-11: "a setting to switch between canvas info and home info, each one shows the battery
            // information settings for each mode"): the rows under it are that side's own, its order too. The Canvas's first row:
            // whether its battery leaves with the controls when the Canvas goes idle (the Canvas page's "Controls fade after")
            const bool forCanvas = batFor_ == 1;
            top.push_back(segRow("Set it for", forCanvas ? "The battery on the Canvas, and what shows beside it there"
                                                         : phoneUpright() ? "The battery at the top of the screen, and what shows beside it there"   // (upright: no title bar)
                                                         : "The battery in the title bar, and what shows beside it there",
                                 "set.batfor", { "Home", "Canvas" }, batFor_, [this](int i) { batFor_ = i; if (grab.kind == Grab::Battery) grab = Grab{}; }));
            if (forCanvas) {
                Row r{ "Fade with the controls", "", store.cvBatteryFade ? "When the Canvas goes idle the battery leaves with its controls, and returns with them"
                                                                         : "Off: the battery stays on the Canvas while its controls are away", 40,
                       [this](float rx, float cy) { if (toggle(uid("set.cvbatfade"), rx - 40, cy, store.cvBatteryFade)) { store.cvBatteryFade = !store.cvBatteryFade; persistSoon(0.3f); } } };
                top.push_back(r);
            }
            for (int i = 0; i < int(vis.size()); i++) {
                const int it = vis[size_t(i)];
                const bool held = grab.kind == Grab::Battery && grab.pos == i;
                // (the Canvas has room for all of them: nothing greyed there; the user, 2026-11-11)
                const bool noRoom = !forCanvas && batteryRoom_ > 0 && !*batteryShown(it) && used + (used > 0 ? sep : 0) + float(g.measure(widest[it], tf)) > batteryRoom_;
                Row r{ names[it], "", held && !sortWay ? GF_MOVING_WORDS : noRoom ? "No room beside the battery: turn another off first"
                                       : it == 0 && forCanvas ? "Beside the Canvas's battery, in this order. Press it for everything about it" : hints[it],
                       sortWay ? sortW : 40 + 16 + mw,
                       [this, it, i, mw, sortWay, noRoom](float rx, float cy) {
                           bool* on = batteryShown(it);
                           if (toggle(uid("set.batshow", it), rx - 40, cy, *on, noRoom ? 0.35f : 1.f)) {
                               if (noRoom) toast("No room beside the battery: turn another off first");
                               else { *on = !*on; persistSoon(0.3f); }
                           }
                           if (sortWay) { dragSort(vis, 1000, "set.batdrag", i, rx, cy, [this] { setBatteryItems(vis); }); return; }
                           bool c = false;
                           button(uid("set.batmove", it), ICON_SORT_DOWN, "Move", rx - 40 - 16 - mw, cy - 17, false, &c);
                           if (c && !sortByDrag()) {
                               grab = Grab{};
                               grab.kind = Grab::Battery;
                               grab.pos = i;
                               toast(GF_MOVE_WORDS);
                           }
                       } };
                r.lifted = held;
                r.dragKey = 1000 + it;
                top.push_back(r);
            }
            if (battery.infoOk && !(battery.info.missing >> 3 & 1))   // (no temperature read here: no unit to choose)
                top.push_back(segRow("Temperature in", "", "set.batunit", { "°C", "°F" }, store.batteryFahrenheit ? 1 : 0,
                                     [this](int i) { store.batteryFahrenheit = i == 1; persistSoon(0.3f); }));
            cards.push_back({ "Battery info", top });   // (the user, 2026-11-16: was "Battery")

            // each state: the battery as it will look (live), its colour, its animation
            static const char* stName[Store::BatteryStates] = { "On battery", "Low", "Charging slowly", "Charging", "Charging fast", "Charged" };
            static const char* stHint[Store::BatteryStates] = { "Unplugged", "At or under Low at", "A charger with little power",
                                                                "A full-power charger", "Fast charging: the bolt beats too", "Full, still plugged in" };
            static const std::vector<int> stLooks[Store::BatteryStates] = { { 0, 1, 2, 3, 7, 6 }, { 0, 1, 2, 5, 3 },{ 0, 1, 4, 7, 6, 3 },
                                                                            { 0, 1, 4, 7, 6, 3 }, { 0, 1, 4, 7, 6, 3 }, { 0, 3, 6, 7, 2 } };
            static const char* lookName[Store::BatteryLooks] = { "Off", "Sweep", "Pulse", "Glow", "Fill up", "Blink", "Sparkle", "Shimmer" };
            static const double mockPct[Store::BatteryStates] = { 64, 12, 41, 58, 63, 100 };
            const bool narrowSet = w < kNarrowList;
            auto swatches = [this](const char* key, int keyBase, int cur, float bx, float bw, float scy, bool narrow, bool dim,
                                   const std::function<void(int)>& pick) {
                const int nc = Store::BatteryColours;
                const float pitch = narrow ? bw / nc : 30.f;
                for (int c = 0; c < nc; c++) {
                    const float sx = narrow ? bx + c * pitch + pitch / 2 - 13 : bx + bw - nc * 30 + 4 + c * 30;
                    const Id id = uid(key, keyBase + c);
#ifdef GF_ANDROID
                    // a finger: the swatch's whole place along the row and 42 high (drawn the same; they were 26 by 26)
                    const SDL_FRect hitR{ sx + 13 - pitch / 2, scy - 21, pitch, 42 };
#else
                    const SDL_FRect hitR{ sx, scy - 13, 26, 26 };
#endif
                    if (ui.item(id, hitR, 13)) { pick(c); persistSoon(0.3f); }
                    const float a = dim ? 0.4f : 1.f;
                    if (c == 2) g.roundRectH(sx + 1, scy - 12, 24, 24, 12, pal.accent2, pal.accent, a);
                    else if (c == 3) g.roundRectH(sx + 1, scy - 12, 24, 24, 12, batteryInk(3, 0), batteryInk(3, 100), a);
                    else g.circle(sx + 13, scy, 12, batteryInk(c, 50), a);
                    const bool on = !dim && cur == c, hot = ui.focused(id) && ui.showFocusRing();
                    if (on || hot) g.roundStroke(sx - 2, scy - 15, 30, 30, 15, on ? 2.f : 1.f, pal.text, on ? 0.95f : 0.4f);
                }
            };
            std::vector<Row> states;
            for (int st = 0; st < Store::BatteryStates; st++) {
                if ((st == 2 || st == 4) && !battery.infoOk) continue;   // (PC, Android: no charger's details, so just Charging)
                Row r;
                r.label = stName[st];
                r.hint = stHint[st];
                r.blockH = narrowSet ? 136 : 92;
                // (its effects' words, as segmented() measures them: six of them on a narrow screen go on two lines, 42 taller,
                // which the page allows for below and the bar here: it grew both ways from its one-line place, up into the colours)
                for (int look : stLooks[st]) r.words += g.measure(lookName[look], { css::Sm, Weight::SemiBold });
                r.nOpts = stLooks[st].size();
                r.block = [this, st, narrowSet, swatches](float bx, float by, float bw) {
                    // the battery in this state, as the title bar will draw it, larger
                    g.roundRect(bx, by + 2, 64, 38, 10, pal.text, 0.05f);
                    g.roundStroke(bx, by + 2, 64, 38, 10, 1, pal.text, 0.08f);
                    drawBatteryIcon(bx + 32 - 20, by + 21, 1.5f, mockPct[st], st);
                    g.text(stName[st], { css::Md, Weight::SemiBold }, bx + 80, by + 1, pal.text);
                    g.text(stHint[st], { css::Sm }, bx + 80, by + 25, pal.text3);
                    // its colour: as the text, the accent, the palette's gradient, by charge, then seven of its own
                    swatches("set.batcol", st * 16, store.batteryCol[st], bx, bw, narrowSet ? by + 64 : by + 21, narrowSet, false,
                             [this, st](int c) { store.batteryCol[st] = c; });
                    // its animation
                    const std::vector<int>& looks = stLooks[st];
                    std::vector<std::string> opts;
                    int cur = 0;
                    for (size_t k = 0; k < looks.size(); k++) {
                        opts.push_back(lookName[looks[k]]);
                        if (looks[k] == store.batteryLook[st]) cur = int(k);
                    }
                    int p = -1;
                    float words = 0;
                    for (const auto& o : opts) words += g.measure(o, { css::Sm, Weight::SemiBold });
                    const float twoLines = segGrid(words, opts.size(), bw) ? 21.f : 0.f;   // (its middle, half the second line lower)
                    segmented(uid("set.batlook", st), bx + bw, (narrowSet ? by + 108 : by + 68) + twoLines, opts, cur, &p, 0, bw, 1);
                    if (p >= 0 && p != cur) { store.batteryLook[st] = looks[size_t(p)]; persistSoon(0.3f); }
                };
                states.push_back(r);
            }
            {   // the outline: laid out as a state is (its battery, the colours, then its choice): as each state's fill, or its own colour
                Row r;
                r.label = "Outline";
                r.hint = store.batteryOutline < 0 ? "The battery's edge: as each state's fill" : "The battery's edge, the same in every state";
                r.blockH = narrowSet ? 136 : 92;
                r.block = [this, narrowSet, swatches](float bx, float by, float bw) {
                    g.roundRect(bx, by + 2, 64, 38, 10, pal.text, 0.05f);
                    g.roundStroke(bx, by + 2, 64, 38, 10, 1, pal.text, 0.08f);
                    drawBatteryIcon(bx + 32 - 20, by + 21, 1.5f, mockPct[0], 0);
                    g.text("Outline", { css::Md, Weight::SemiBold }, bx + 80, by + 1, pal.text);
                    g.text(store.batteryOutline < 0 ? "As each state's fill" : "The same in every state", { css::Sm }, bx + 80, by + 25, pal.text3);
                    // a colour picked is its own from then on
                    swatches("set.batedge", 0, store.batteryOutline, bx, bw, narrowSet ? by + 64 : by + 21, narrowSet, store.batteryOutline < 0,
                             [this](int c) { store.batteryOutline = c; });
                    int p = -1;
                    const int cur = store.batteryOutline < 0 ? 0 : 1;
                    segmented(uid("set.batedgeway"), bx + bw, narrowSet ? by + 108 : by + 68, { "As the fill", "Its own" }, cur, &p, 0, bw, 1);
                    if (p >= 0 && p != cur) { store.batteryOutline = p == 0 ? -1 : 0; persistSoon(0.3f); }
                };
                states.push_back(r);
            }
            static const int lows[4] = { 10, 15, 20, 30 };
            int lowPick = 1;
            for (int i = 0; i < 4; i++) if (store.batteryLowAt == lows[i]) lowPick = i;
            states.push_back(segRow("Low at", "", "set.batlowat", { "10%", "15%", "20%", "30%" }, lowPick,
                                    [this](int i) { static const int l[4] = { 10, 15, 20, 30 }; store.batteryLowAt = l[i]; persistSoon(0.3f); }));
            states.push_back(segRow("Pace", store.motion ? "How quickly the animations move. Charging, they follow the charger too"
                                                         : "Motion is off in Appearance: they stay still",
                                    "set.bapace", { "Calm", "Normal", "Lively" }, store.batteryAnimPace, [this](int i) { store.batteryAnimPace = i; persistSoon(0.3f); }));
            cards.push_back({ "Battery icon", states });   // (the user, 2026-11-16: was "Battery states")
        }
        if (r4::on()) {   // the HiBy R4's status LED (its ROM's service; hibyr4.hpp): that device only
            std::vector<Card> ledCards;
            std::vector<Row> led;
            static const char* modeKey[3] = { "hiby", "off", "custom" };
            static const char* modeHint[3] = { "HiBy's own rules, for the front light and the back", "Both lights stay off",
                                               "Your own colour, alerts and back light" };
            const std::string modeNow = r4::get("led_mode");
            const int mode = modeNow == "off" ? 1 : modeNow == "custom" ? 2 : 0;
            led.push_back(segRow("LED mode", modeHint[mode], "r4.ledmode", { "HiBy", "Off", "Custom" }, mode,
                                 [](int i) { r4::set("led_mode", modeKey[i]); }));
            if (mode == 2) {
                // the colour: A (Songate's accent, set again with each cover: App::r4Tick), eleven of its own, then the wheel
                static const std::vector<int> choices = { -1, 0xffffff, 0xff3b30, 0xff9500, 0xffcc00, 0x34c759, 0x00c7be, 0x5ac8fa, 0x00a0ff, 0xaf52de, 0xff2d55 };
                const int colour = store.r4LedAccent ? -1 : int(strtol(r4::get("led_color").c_str(), nullptr, 16));
                const float cw = (choices.size() + 1) * 30.f;
                led.push_back(Row{ "Colour", "", colour < 0 ? "Follows the cover's colour" : "The light's colour", cw, [this, colour](float rx, float cy0) {
                                      const int n = int(choices.size()) + 1;
                                      for (int i = 0; i < int(choices.size()); i++) {
                                          const Round at = roundAt(i, n, rx, cy0, settingsCtlMax_, true);
                                          const float sx = at.x, cy = at.cy, d = at.d;
                                          const Id id = uid("r4.ledcol", i);
                                          const int v = choices[i];
                                          if (ui.item(id, { sx, cy - d / 2, d, d }, d / 2)) {
                                              char hex[8];
                                              if (v < 0)
                                                  snprintf(hex, sizeof hex, "%02x%02x%02x", int(std::lround(target.accent.r * 255)),
                                                           int(std::lround(target.accent.g * 255)), int(std::lround(target.accent.b * 255)));
                                              else
                                                  snprintf(hex, sizeof hex, "%06x", v);
                                              store.r4LedAccent = v < 0;
                                              r4::set("led_color", hex);
                                              persistSoon(0.3f);
                                          }
                                          if (v < 0) drawRoundA(g, pal, sx, cy, d);   // A: Songate's accent, as Custom's rows draw it
                                          else g.circle(sx + 13, cy, 12, Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f });
                                          const bool on = colour == v, hot = ui.focused(id) && ui.showFocusRing();
                                          if (on || hot) g.roundStroke(sx - 2, cy - d / 2 - 2, d + 4, d + 4, d / 2 + 2, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.4f);
                                      }
                                      // the wheel: your own colour, on the light as you pick it (App::r4Tick)
                                      const Round at = roundAt(n - 1, n, rx, cy0, settingsCtlMax_, true);
                                      const float sx = at.x, cy = at.cy, d = at.d, k = d / 26;
                                      const Id id = uid("r4.ledcol", 99);
                                      const bool own = colour >= 0 && std::find(choices.begin(), choices.end(), colour) == choices.end();
                                      if (ui.item(id, { sx, cy - d / 2, d, d }, d / 2)) {
                                          r4LedPick_ = colour >= 0 ? colour : 0x00a0ff;
                                          r4LedPickSent_ = -1;
                                          openColourPicker(&r4LedPick_, "Status LED",
                                                           Rgb{ ((r4LedPick_ >> 16) & 255) / 255.f, ((r4LedPick_ >> 8) & 255) / 255.f, (r4LedPick_ & 255) / 255.f });
                                      }
                                      if (SDL_Texture* wheel = wheelTexture()) g.imageStretched(wheel, sx + 1, cy - d / 2 + 1, d - 2, d - 2, 0);
                                      if (own) {
                                          g.circle(sx + d / 2, cy, 8.5f * k, { 0, 0, 0 }, 0.35f);
                                          g.circle(sx + d / 2, cy, 7 * k, Rgb{ ((colour >> 16) & 255) / 255.f, ((colour >> 8) & 255) / 255.f, (colour & 255) / 255.f });
                                      }
                                      const bool hot = ui.focused(id) && ui.showFocusRing();
                                      if (own || hot) g.roundStroke(sx - 2, cy - d / 2 - 2, d + 4, d + 4, d / 2 + 2, own ? 2.f : 1.f, own ? pal.accent : pal.text, own ? 1.f : 0.4f);
                                  } });
                led.back().rounds = int(choices.size()) + 1;
                led.back().roundsLead = true;
                // brightness: on the light as it is dragged, saved when let go
                const int brightNow = r4LedBright_ >= 0 ? r4LedBright_ : r4::getInt("led_bright", 60);
                led.push_back(Row{ "Brightness", std::to_string(brightNow) + "%", "", 180, [this, brightNow](float rx, float cy) {
                                      const Id id = uid("r4.ledbright");
                                      const float cur = brightNow / 100.f;
                                      float nv = cur;
                                      slider(id, rx - setSliderW(), cy, setSliderW(), cur, &nv);
                                      const bool held = ui.sliderDrag(id) >= 0;
                                      if (nv != cur) {
                                          r4LedBright_ = int(std::lround(nv * 100));
                                          r4::ledLive(int(strtol(r4::get("led_color").c_str(), nullptr, 16)), r4LedBright_, r4::get("led_effect") == "breath");
                                      }
                                      if (!held && r4LedBright_ >= 0) {   // let go (or a step with the D-pad): saved
                                          r4::set("led_bright", std::to_string(r4LedBright_));
                                          r4LedBright_ = -1;
                                      }
                                  } });
                led.push_back(segRow("Effect", "", "r4.ledfx", { "Solid", "Breathing" }, r4::get("led_effect") == "breath" ? 1 : 0,
                                     [](int i) { r4::set("led_effect", i ? "breath" : "solid"); }));
                auto sw = [&](const char* label, const char* hint, const char* key, const char* name) {
                    const bool on = r4::get(name) == "1";
                    return Row{ label, "", hint, 40, [this, key, name, on](float rx, float cy) {
                                   if (toggle(uid(key), rx - 40, cy, on)) r4::set(name, on ? "0" : "1");
                               } };
                };
                led.push_back(sw("Off with screen", "The light goes out while the screen is off", "r4.ledscreen", "led_screen"));
                ledCards.push_back({ "Status LED", led });
                // A feature's colour (an alert's, a sample rate's): saturated ones (the LED has nothing behind it: pale looks white),
                // the ROM's own among them, then the wheel. Set when chosen; seen when the feature lights. off: greyed, its switch off
                auto colourRow = [&](const char* label, const char* key, const char* name, bool off) {
                    // (Songate's coral and amber as the LED shows them: ff3c30 and ff9030, the user's picks on the device, SongateOS 2026-11-15;
                    // the red that was beside the coral, ff3b30, is the same light: one swatch)
                    static const std::vector<int> choices = { 0xff3c30, 0xff9030, 0xffcc00, 0x34c759, 0x00c7be, 0x4fc3ff, 0x00a0ff, 0xaf52de, 0xff3c8c };
                    const int colour = int(strtol(r4::get(name).c_str(), nullptr, 16));
                    const int hi = std::max({ colour >> 16 & 255, colour >> 8 & 255, colour & 255 }), lo = std::min({ colour >> 16 & 255, colour >> 8 & 255, colour & 255 });
                    const bool pale = hi > 0 && float(hi - lo) / float(hi) < 0.5f;   // (HiBy's first amber, e9b589, is one)
                    Row r{ label, "", pale ? "Pale colours look white on the LED" : "", (choices.size() + 1) * 30.f,
                           [this, key, name, off, colour](float rx, float cy0) {
                               const float a = off ? 0.35f : 1.f;
                               const int n = int(choices.size()) + 1;
                               for (int i = 0; i < int(choices.size()); i++) {
                                   const Round at = roundAt(i, n, rx, cy0, settingsCtlMax_);
                                   const float sx = at.x, cy = at.cy;
                                   const Id id = uid(key, i);
                                   const int v = choices[i];
                                   if (ui.item(id, { sx, cy - 13, 26, 26 }, 13) && !off) {
                                       char hex[8];
                                       snprintf(hex, sizeof hex, "%06x", v);
                                       r4::set(name, hex);
                                   }
                                   g.circle(sx + 13, cy, 12, Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f }, a);
                                   const bool on = colour == v, hot = ui.focused(id) && ui.showFocusRing();
                                   if (on || hot) g.roundStroke(sx - 2, cy - 15, 30, 30, 15, on ? 2.f : 1.f, on ? pal.accent : pal.text, (on ? 1.f : 0.4f) * a);
                               }
                               const Round at = roundAt(n - 1, n, rx, cy0, settingsCtlMax_);
                               const float sx = at.x, cy = at.cy, d = at.d, k = d / 26;
                               const Id id = uid(key, 99);
                               const bool own = std::find(choices.begin(), choices.end(), colour) == choices.end();
                               if (ui.item(id, { sx, cy - d / 2, d, d }, d / 2) && !off) {
                                   r4FeatPick_ = colour;
                                   r4FeatKey_ = name;
                                   openColourPicker(&r4FeatPick_, "Status LED", Rgb{ ((colour >> 16) & 255) / 255.f, ((colour >> 8) & 255) / 255.f, (colour & 255) / 255.f });
                               }
                               if (SDL_Texture* wheel = wheelTexture()) g.imageStretched(wheel, sx + 1, cy - d / 2 + 1, d - 2, d - 2, 0, a);
                               if (own) {
                                   g.circle(sx + d / 2, cy, 8.5f * k, { 0, 0, 0 }, 0.35f * a);
                                   g.circle(sx + d / 2, cy, 7 * k, Rgb{ ((colour >> 16) & 255) / 255.f, ((colour >> 8) & 255) / 255.f, (colour & 255) / 255.f }, a);
                               }
                               const bool hot = ui.focused(id) && ui.showFocusRing();
                               if (own || hot) g.roundStroke(sx - 2, cy - d / 2 - 2, d + 4, d + 4, d / 2 + 2, own ? 2.f : 1.f, own ? pal.accent : pal.text, (own ? 1.f : 0.4f) * a);
                           } };
                    r.rounds = int(choices.size()) + 1;
                    r.dim = off;
                    return r;
                };
                // Alerts: they come before the colour (low battery first of all)
                const bool lowbat = r4::get("led_lowbat") == "1", notif = r4::get("led_notif") == "1";
                static const char* levels[6] = { "5", "10", "15", "20", "25", "30" };
                const int lvlNow = r4::getInt("led_lowbat_level", 15);
                int lvl = 2;
                for (int i = 0; i < 6; i++) if (atoi(levels[i]) == lvlNow) lvl = i;
                Row warn = segRow("Warn below", "", "r4.ledlowat", { "5", "10", "15", "20", "25", "30%" }, lvl,
                                  [](int i) { r4::set("led_lowbat_level", levels[i]); }, lowbat ? 0u : 0x3Fu);
                warn.dim = !lowbat;
                ledCards.push_back({ "Alerts",
                                     { sw("Low battery", "Breathes red below the level while not charging. It comes before everything else", "r4.ledlow", "led_lowbat"),
                                       warn,
                                       sw("Notifications", "Screen off: breathes in its colour for new notifications until you wake the player", "r4.lednotif", "led_notif"),
                                       colourRow("Notification colour", "r4.lednotifcol", "led_notif_color", !notif),
                                       sw("Charging", "HiBy's red breathing while it charges", "r4.ledcharge", "led_charge") } });
                // While music plays
                const bool quality = r4::get("led_quality") == "1";
                ledCards.push_back({ "While music plays",
                                     { sw("Audio quality colour", "The colour follows the song's sample rate. DSD lights the white LED", "r4.ledq", "led_quality"),
                                       colourRow("Up to 48 kHz", "r4.ledqcd", "led_q_cd", !quality),
                                       colourRow("88 to 192 kHz", "r4.ledqhi", "led_q_hires", !quality),
                                       colourRow("Above 192 kHz", "r4.ledqul", "led_q_ultra", !quality),
                                       sw("Bass pulse", "Screen on: the brightness follows the bass", "r4.ledbeat", "led_beat") } });
                // Back light: white, on or off, behind the strips on the back
                static const char* backKey[4] = { "screen", "on", "off", "music" };
                const std::string backNow = r4::get("led_back");
                const int back = backNow == "on" ? 1 : backNow == "off" ? 2 : backNow == "music" ? 3 : 0;
                ledCards.push_back({ "Back light",
                                     { segRow("On when", "The white light behind the strips on the back", "r4.back",
                                              { "The screen is on", "Always", "Never", "Music plays" }, back, [](int i) { r4::set("led_back", backKey[i]); }),
                                       sw("Notifications", "Screen off: on for new notifications until you wake the player", "r4.backnotif", "led_back_notif"),
                                       sw("Charging", "Blinks slowly while charging, steady when full", "r4.backcharge", "led_back_charge"),
                                       sw("Bass hits", "Screen on, music playing: it lights on the strongest hits", "r4.backbeat", "led_back_beat") } });
            } else {
                ledCards.push_back({ "Status LED", led });
            }
            // a page of its own, listed right after Appearance (the user, 2026-11-15: "Own page after appearance")
            ledCards[0].kicker.clear();   // (the page's own title says it)
            r4LedCards = std::move(ledCards);
        }
#ifdef GF_ANDROID
        // System colours (SongateOS's theme_get / theme_set; the user chose B, 2026-11-16): the colour and darkness of Android's
        // own screens (its questions, the installer, its settings). Songate's colour is copied once when chosen, never per cover:
        // every change makes Android colour its screens again, Songate's restarting once. The R4 only.
        if (r4Dev_ && sysapi::has("theme")) {
            const sysapi::Theme t = sysapi::theme();
            std::vector<Row> rows;
            if (!t.known) rows.push_back(infoRow("Colour", "…"));
            else {
                auto hexRgb = [](const Rgb& c) {
                    char b[8];
                    snprintf(b, sizeof b, "#%02x%02x%02x", int(std::lround(c.r * 255)), int(std::lround(c.g * 255)), int(std::lround(c.b * 255)));
                    return std::string(b);
                };
                const int way = t.colour.empty() || t.colour == t.def ? 0 : store.sysThemeSongate ? 1 : 2;
                rows.push_back(segRow("Colour", "Android's own screens: its questions, the installer, its settings", "sys.themeway",
                                      { "Android's", "Songate's", "Your own" }, way, [this, hexRgb](int i) {
                                          store.sysThemeSongate = i == 1;
                                          sysThemeOwn_ = i == 2;   // (the rounds only under Your own)
                                          if (i == 0) sysapi::act("theme_set", "", { { "colour", "default" } }, "theme");
                                          else if (i == 1) sysapi::act("theme_set", "", { { "colour", hexRgb(target.accent) } }, "theme");
                                          else sysThemeOwn_ = true;   // (its colour picked below)
                                      }));
                if (way == 2 || sysThemeOwn_) {
                    static const std::vector<int> choices = { 0xff3b30, 0xff9500, 0xffcc00, 0x34c759, 0x00c7be, 0x5ac8fa, 0x00a0ff, 0xaf52de, 0xff2d55 };
                    const int cur = t.colour.size() == 7 ? int(strtol(t.colour.c_str() + 1, nullptr, 16)) : -1;
                    Row r{ "Colour", "", "", (choices.size() + 1) * 30.f, [this, cur](float rx, float cy0) {
                              const int n = int(choices.size()) + 1;
                              auto send = [this](int v) {
                                  char b[8];
                                  snprintf(b, sizeof b, "#%06x", v & 0xffffff);
                                  store.sysThemeSongate = false;
                                  persistSoon(0.3f);
                                  sysapi::act("theme_set", "", { { "colour", b } }, "theme");
                              };
                              for (int i = 0; i < int(choices.size()); i++) {
                                  const Round at = roundAt(i, n, rx, cy0, settingsCtlMax_);
                                  const float sx = at.x, cy = at.cy, d = at.d;
                                  const int v = choices[i];
                                  const Id id = uid("sys.themecol", i);
                                  if (ui.item(id, { sx, cy - d / 2, d, d }, d / 2) && v != cur) send(v);
                                  g.circle(sx + d / 2, cy, d / 2 - 1, Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f });
                                  const bool on = cur == v, hot = ui.focused(id) && ui.showFocusRing();
                                  if (on || hot) g.roundStroke(sx - 2, cy - d / 2 - 2, d + 4, d + 4, d / 2 + 2, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.4f);
                              }
                              // the wheel: your own colour, sent once the picker closes with it (App::sysTick: each change restarts screens)
                              const Round at = roundAt(n - 1, n, rx, cy0, settingsCtlMax_);
                              const float sx = at.x, cy = at.cy, d = at.d, k = d / 26;
                              const Id id = uid("sys.themecol", 99);
                              const bool own = cur >= 0 && std::find(choices.begin(), choices.end(), cur) == choices.end();
                              if (ui.item(id, { sx, cy - d / 2, d, d }, d / 2)) {
                                  sysThemePick_ = sysThemePickFrom_ = cur >= 0 ? cur : 0x00a0ff;
                                  openColourPicker(&sysThemePick_, "System colours",
                                                   Rgb{ ((sysThemePick_ >> 16) & 255) / 255.f, ((sysThemePick_ >> 8) & 255) / 255.f, (sysThemePick_ & 255) / 255.f });
                              }
                              if (SDL_Texture* wheel = wheelTexture()) g.imageStretched(wheel, sx + 1, cy - d / 2 + 1, d - 2, d - 2, 0);
                              if (own) {
                                  g.circle(sx + d / 2, cy, 8.5f * k, { 0, 0, 0 }, 0.35f);
                                  g.circle(sx + d / 2, cy, 7 * k, Rgb{ ((cur >> 16) & 255) / 255.f, ((cur >> 8) & 255) / 255.f, (cur & 255) / 255.f });
                              }
                              const bool hot = ui.focused(id) && ui.showFocusRing();
                              if (own || hot) g.roundStroke(sx - 2, cy - d / 2 - 2, d + 4, d + 4, d / 2 + 2, own ? 2.f : 1.f, own ? pal.accent : pal.text, own ? 1.f : 0.4f);
                          } };
                    r.rounds = int(choices.size()) + 1;
                    rows.push_back(r);
                }
                rows.push_back(Row{ "Dark", "", "A change makes the screen go blank for a moment, while Android colours itself again", 40,
                                    [this, dark = t.dark](float rx, float cy) {
                                        if (toggle(uid("sys.themedark"), rx - 40, cy, dark)) sysapi::act("theme_set", "", { { "dark", dark ? "0" : "1" } }, "theme");
                                    } });
            }
            // right below Interface colours (and its Custom card, which opens with it): the user, 2026-11-16
            size_t at = 0;
            while (at < cards.size() && (cards[at].kicker == "Interface colours" || cards[at].with == "Interface colours")) at++;
            cards.insert(cards.begin() + long(at), { "System colours", rows });
        }
#endif
#ifdef GF_ANDROID
        // Screen (SongateOS's display_get / display_set; the user chose A, 2026-11-16): after the colours, how long the screen stays
        // on first (Never there: Android's own way of staying awake, in place of Playback's Keep awake on the R4), then Android's
        // own brightness. The R4 only
        if (r4Dev_ && sysapi::has("display")) {
            const sysapi::Display d = sysapi::display();
            std::vector<Row> rows;
            if (d.known) {
                std::vector<std::string> opts;
                int cur = -1;
                for (size_t i = 0; i < d.timeouts.size(); i++) {
                    const int s = d.timeouts[i];
                    if (s == d.timeout) cur = int(i);
                    opts.push_back(s == 0 ? std::string("Never") : s < 60 ? std::to_string(s) + " s" : std::to_string(s / 60) + " min");
                }
                rows.push_back(segRow("Turns off after", "", "sys.timeout", opts, cur, [ts = d.timeouts](int i) {
                    sysapi::act("display_set", "timeout", { { "value", std::to_string(ts[size_t(i)]) } }, "display");
                }));
                // the brightness: Songate's slider while it is dragged (sent as it moves, ten times a second at most, and where it
                // was let go), the service's value otherwise (the quick settings' slider moves it too)
                const Id bid = uid("sys.bright");
                if (!ui.engaged(bid)) {
                    if (sysBrightSent_ < 0) {   // (let go since the last one sent)
                        sysapi::act("display_set", "brightness", { { "value", std::to_string(int(std::lround(sysBright_ * 100))) } }, "display");
                        sysBrightSent_ = 0;
                    } else sysBright_ = std::clamp(d.brightness / 100.f, 0.f, 1.f);
                }
                rows.push_back(sliderRow("Screen brightness", pct(sysBright_), "The same as Android's own slider", "sys.bright", sysBright_, [this] {
                    if (time - sysBrightSent_ >= 0.1) {
                        sysapi::act("display_set", "brightness", { { "value", std::to_string(int(std::lround(sysBright_ * 100))) } }, "display");
                        sysBrightSent_ = time;
                    } else sysBrightSent_ = -1;   // (the last move: sent when let go)
                }));
            } else rows.push_back(infoRow("Turns off after", "", "…"));
            size_t at = 0;
            while (at < cards.size() && (cards[at].kicker == "Interface colours" || cards[at].with == "Interface colours" ||
                                         cards[at].kicker == "System colours")) at++;
            cards.insert(cards.begin() + long(at), { "Screen", rows });
        }
#endif
        cards.erase(std::remove_if(cards.begin(), cards.end(), [](const Card& c) { return c.kicker == "Layout" && c.rows.empty(); }), cards.end());   // (the Switch and the PC: Layout had only Collapse)
        sections.push_back({ "Appearance", ICON_SPARKLE, cards });
    }

    // Playback
    {
        // The background player (sysmodule): music on the HOME menu and in games, and its pause-for-apps.
        std::vector<Row> bgRows;
#ifdef GF_ANDROID
        // what Android does (SongateActivity.keepNativeWhenDestroyed): no switch, it always does it (a switch here changed nothing)
        // (the user chose a switch, 2026-11-04: "B"; SongateActivity.keepNativeWhenDestroyed reads it)
        bgRows.push_back(toggleRow("Keep playing when swiped away",
                                   // ("on devices that allow it", the user's choice, 2026-11-13: a Hiby R4's system force-stops a swiped app)
                                   "Music keeps playing after you close Songate from the recent apps, on devices that allow it. Off: it stops",
                                   "set.swipe", store.swipeKeep));
#elif defined(GF_WINDOWED)
        bgRows.push_back(toggleRow("Keep playing when the window closes", "Songate stays by the clock, playing; its icon brings it back. Off: closing the window quits",
                                   "set.bg", store.background));
#else
        const bool on = player.remote();
        // While the server's queue plays the background player has handed the music over, since it cannot stream: the row says
        // so, greyed, and the music goes back to it after (serverTick).
        const bool handed = srvq_.hadBackground;
        std::string streamer = subsonic.view().kind;
        if (streamer.empty()) streamer = "the server";
        streamer[0] = char(toupper(uint8_t(streamer[0])));
        Row bgRow{ "Keep playing outside Songate", "",
                   handed ? "Handed over while " + streamer + " plays: it cannot stream. It gets the music back after"
                   : on ? "Connected. The music carries on on the HOME menu, in games and after Songate closes"
                   : store.background ? "Not installed: copy atmosphere/contents/4200000047464E58 to " GF_THE_CARD " and restart"
                                      : "Off: the music stops when you leave Songate",
                   handed ? 0.f : 40.f, handed ? std::function<void(float, float)>() : [this](float rx, float cy) {
                       if (toggle(uid("set.bg"), rx - 40, cy, store.background && player.remote())) setBackground(!player.remote());
                   } };
        bgRow.dim = handed;
        bgRows.push_back(bgRow);
        if (bgShown()) {
            // Each game event gets an action, like sys-tune's focus options.
            // Playlist (GfbgAct_Playlist, 4): one of your playlists, chosen on the row under it.
            static const std::vector<std::string> acts = { "Nothing", "Play", "Resume", "Pause", "Playlist" };
            auto actRow = [&](const std::string& label, const std::string& hint, const char* key, int BgConfig::*field, int event) {
                bgRows.push_back(segRow(label, hint, key, acts, std::clamp(bgs.cfg.*field, 0, 4), [this, field, event](int i) {
                    bgs.cfg.*field = i;
                    bg.setConfig(bgs.cfg);
                    if (i == 4 && !bgs.cfg.lists[event]) openActionList(event);   // none yet: choose one now
                }));
                if (bgs.cfg.*field != 4) return;
                const std::string name = listName(bgs.cfg.lists[event]);
                static const char* keys[4] = { "set.bglist0", "set.bglist1", "set.bglist2", "set.bglist3" };   // ids that outlive the frame
                bgRows.push_back(buttonRow("Its playlist", name.empty() ? "None chosen yet: until then the music resumes" : name, keys[event],
                                           ICON_LIST, "Choose", [this, event] { openActionList(event); }));
            };
            // Fades from 0 to 5 s, on a 0..1 slider.
            auto fadeRow = [this](const std::string& label, const char* key, float BgConfig::*field) {
                char v[16];
                snprintf(v, sizeof v, "%.1f s", double(bgs.cfg.*field));
                return Row{ label, v, "", 180, [this, key, field](float rx, float cy) {
                               const float cur = bgs.cfg.*field / 5.f;
                               float nv = cur;
                               slider(uid(key), rx - setSliderW(), cy, setSliderW(), cur, &nv);
                               if (nv != cur) { bgs.cfg.*field = std::round(nv * 50) / 10; bg.setConfig(bgs.cfg); }
                           } };
            };
            actRow("When a game opens", "When it starts. Play picks a random folder and shuffles it; Playlist plays one of yours",
                   "set.bgopen", &BgConfig::onOpen, 0);
            actRow("On the HOME menu", "While a game waits in the background", "set.bghome", &BgConfig::onHome, 1);
            actRow("Back to the game", "Going back from the HOME menu to a game that never closed", "set.bgreturn", &BgConfig::onReturn, 3);
            actRow("When a game closes", "", "set.bgclose", &BgConfig::onClose, 2);
            bgRows.push_back(segRow("Action playlists", bgs.cfg.listStart ? "Shuffled afresh each time a game event plays one"
                                                                          : "Each carries on at the song and time it was left at",
                                    "set.bglstart", { "Continue", "Shuffle" }, bgs.cfg.listStart ? 1 : 0, [this](int i) {
                                        bgs.cfg.listStart = i;
                                        bg.setConfig(bgs.cfg);
                                    }));
            bgRows.push_back(fadeRow("Fade out", "set.bgfout", &BgConfig::fadeOut));
            bgRows.push_back(fadeRow("Fade in", "set.bgfin", &BgConfig::fadeIn));
            // The overlay's covers: made for each queue that reaches the player, or for everything here.
            {
                if (time - bgs.coverCountAt > 1) {
                    bgs.coverCountAt = time;
                    bgs.coverCount = 0;
                    for (const auto& t : lib.tracks) bgs.coverCount += thumbs.has(t.path);
                }
                // The count goes beside the label (fixed-width digits); the hint stays the same sentence, so its
                // lines do not reflow as the count rises.
                int made = 0, left = 0;
                thumbs.progress(made, left);
                // Stop / Resume while some are missing; Make all once every one is made (or none was asked for).
                const bool stopped = store.stopThumbs;
                Row covers = buttonRow("Covers for the overlay",
                                       stopped ? "Stopped. Only the song playing gets one until you resume"
                                       : left > 0 ? "Being made in the background while Songate is open"
                                                  : "Made for each queue sent to the player, or for the whole library here",
                                       "set.bgcovers", stopped ? ICON_PLAY : left > 0 ? ICON_X : ICON_REFRESH,
                                       stopped ? "Resume" : left > 0 ? "Stop" : "Make all", [this, stopped, left] {
                                           if (stopped) { setJobStopped(Stoppable::Thumbs, false); return; }
                                           if (left > 0) { setJobStopped(Stoppable::Thumbs, true); return; }
                                           for (int t = 0; t < int(lib.tracks.size()); t++) thumbFor(t, false);
                                           toast(std::string("Making them in the background: ") + workCardWords());
                                       });
                covers.value = std::to_string(bgs.coverCount) + " of " + std::to_string(lib.tracks.size());
                bgRows.push_back(covers);
            }
            bgRows.push_back(infoRow("Controls outside Gatefold", "The Songate overlay in Ultrahand's menu (L + D-pad down + R3 unless you changed it)"));
        }
#endif
        // Measured: the count beside the label; Stop / Resume while the library is not all measured.
        const size_t measured = std::min(levels.count(), lib.tracks.size());
        const std::string measuredHint =
            store.levelMode == 0 ? "Starts once levelling is on, from the song playing"
            : store.stopLevels       ? "Stopped. Only the song playing and the next ones are measured until you resume"
            : levels.secondsPerTrack() > 0 ? "In the background while Songate is open, about " + oneDecimal(levels.secondsPerTrack()) + " s a song"
                                     : "In the background while Songate is open, the song playing first";
        Row measuredRow = store.levelMode && (store.stopLevels || measured < lib.tracks.size())
                              ? buttonRow("Measured", measuredHint, "set.lvlstop", store.stopLevels ? ICON_PLAY : ICON_X,
                                          store.stopLevels ? "Resume" : "Stop",
                                          [this] { setJobStopped(Stoppable::Levels, !store.stopLevels); })
                              : infoRow("Measured", measuredHint);
        measuredRow.value = std::to_string(measured) + " of " + std::to_string(lib.tracks.size());
        static const std::vector<std::string> orders = { "Default", "Repeat list", "Repeat song", "Shuffle" };
        // (the HiBy R4 with SongateOS's screen settings: staying awake is Android's own, Appearance › Screen's Never; the user,
        // 2026-11-16: Keep awake goes, Dim the screen follows it)
        bool screenSetting = false;
#ifdef GF_ANDROID
        screenSetting = screenNeverOffered();   // (an older service with no Never: Keep awake stays)
#endif
        sections.push_back(
            { "Playback", ICON_PLAY,
              { { "", { segRow("Order", automixQueue() ? "An Automix playlist keeps its own order: no Shuffle"
                                                    : phoneUpright() ? "Also the Order button on Now playing"   // (upright: no player bar)
                                                    : "Also the first button in the player bar", "set.order", orders, int(store.order),
                               [this](int i) { setOrder(Order(i)); }, automixQueue() ? 1u << 3 : 0u),
                        Row{ "Stop after the current song", "", "Then playback stops until you start it again", 40,
                             [this](float rx, float cy) {
                                 if (toggle(uid("set.sac"), rx - 40, cy, stopAfterCurrent)) stopAfterCurrent = !stopAfterCurrent;
                             } },
                        Row{ "Volume", pct(volumeShown), "", 180,
                             [this](float rx, float cy) {
                                 float v = volumeShown;
                                 slider(uid("set.vol"), rx - setSliderW(), cy, setSliderW(), volumeShown, &v);
                                 if (v != volumeShown) setVolume(v);
                             } } } },
                { "Volume levelling",
                  { segRow("Volume levelling", "Every song about as loud as the next. Your files are not changed",
                           "set.level", { "Off", "Song", "Album", "Auto" }, store.levelMode,
                           [this](int i) { store.levelMode = i; persistSoon(0.3f); }),
                    segRow("Target loudness", "Quiet songs are raised only as far as their peaks allow", "set.lvltarget",
                           { "−18", "−14", "−11 LUFS" }, store.levelTarget <= -16 ? 0 : store.levelTarget <= -12 ? 1 : 2,
                           [this](int i) {
                               static const int t[] = { -18, -14, -11 };
                               store.levelTarget = t[i];
                               persistSoon(0.3f);
                           },
                           store.levelMode ? 0u : 0x7u),
                    measuredRow } },
                { "Crossfade",
                  { segRow("Crossfade", "The end of a song fades into the next one. Automix playlists mix their own",

                           "set.xfade", { "Off", "2", "4", "6", "8", "12 s" },
                           store.crossfade <= 0 ? 0 : store.crossfade <= 2 ? 1 : store.crossfade <= 4 ? 2 : store.crossfade <= 6 ? 3
                                                    : store.crossfade <= 8 ? 4 : 5,
                           [this](int i) {
                               static const float secs[] = { 0, 2, 4, 6, 8, 12 };
                               store.crossfade = secs[i];
                           }),
                    toggleRow("Not within an album", "An album's songs in a row keep their gapless join"
                                                     "",
                              "set.xfadealbum", store.xfadeNotInAlbum) } },
                // What plays when Songate opens; the opening itself is in Appearance (the user, 2026-11-03)
                { "Startup",
                  { toggleRow("Resume where I left off", "The queue comes back paused at the same place in the same song", "set.resume",
                              store.resume) } },
                { "Sleep",
                  { segRow("Sleep timer", "The music fades out and pauses", "set.sleep",
                           { "Off", "15", "30", "45", "60", "90 min" }, sleepLeft() > 0 ? sleepChoice : 0,
                           [this](int i) {
                               static const int mins[] = { 0, 15, 30, 45, 60, 90 };
                               setSleep(mins[i] * 60.0, i);
                           }),
                    segRow("Then sleep " GF_THE_CONSOLE, "When the timer ends, after a countdown you can cancel",
                           "set.sleepcon", { "Off", "10 s", "30 s", "1 min" },
                           store.sleepConsole <= 0 ? 0 : store.sleepConsole <= 10 ? 1 : store.sleepConsole <= 30 ? 2 : 3,
                           [this](int i) {
                               static const int secs[] = { 0, 10, 30, 60 };
                               store.sleepConsole = secs[i];
                               persistSoon(0.3f);
                           }),
                    toggleRow("Keep " GF_THE_CONSOLE " awake", "While Songate is open the screen does not dim and " GF_THE_CONSOLE " does not sleep by "
                                                        "itself. After a sleep timer ends, it may again.",
                              "set.awake", store.keepAwake),
                    segRow("Dim the screen", keepAwakeOn() ? "After this long without input; any button brings it back"
                                             : screenSetting ? "With the screen set never to turn off (Appearance › Screen); otherwise it turns off by itself"
                                                             : "With Keep " GF_THE_CONSOLE " awake on; otherwise " GF_THE_CONSOLE " dims by itself",
                           "set.dim", { "Off", "1", "2", "5", "10 min" },
                           store.dimAfter <= 0 ? 0 : store.dimAfter <= 1 ? 1 : store.dimAfter <= 2 ? 2 : store.dimAfter <= 5 ? 3 : 4,
                           [this](int i) {
                               static const int mins[] = { 0, 1, 2, 5, 10 };
                               store.dimAfter = mins[i];
                               persistSoon(0.3f);
                           },
                           keepAwakeOn() ? 0u : 0x1Fu) } },
                { GF_BG_CARD, bgRows } } });
        if (screenSetting)
            for (Card& c : sections.back().cards)
                c.rows.erase(std::remove_if(c.rows.begin(), c.rows.end(), [](const Row& r) { return r.label == "Keep " GF_THE_CONSOLE " awake"; }),
                             c.rows.end());
        if (!sleepCardOffered())   // (Android but the HiBy R4 with SongateOS's power sleep: nothing puts the phone to sleep, so no countdown)
            for (Card& c : sections.back().cards)
                c.rows.erase(std::remove_if(c.rows.begin(), c.rows.end(), [](const Row& r) { return r.label == "Then sleep " GF_THE_CONSOLE; }),
                             c.rows.end());
#ifdef GF_ANDROID
        {   // Playback › Volume (Android; the user, 2026-11-15): the volume is Android's own music volume, in the device's steps (the
            // HiBy R4: its Volume steps); then how the volume keys show it. Line out on the R4 holds it at full: shown, not moved
            auto& pc = sections.back().cards;
            Row vol = pc[0].rows.back();
            pc[0].rows.pop_back();
            const int maxStep = plat::mediaVolume().max;
            const bool fixed = volumeFixed();
            vol.value = fixed ? std::string("Fixed (line out)") : store.volNum ? pct(volumeShown) : std::to_string(int(std::lround(volumeShown * maxStep)));
            vol.hint = "Android's own volume: the volume keys move it too";
            if (fixed)
                vol.control = [this](float rx, float cy) {   // the slider greyed, at full, nothing to move
                    const float w = setSliderW(), x0 = rx - w;
                    g.roundRect(x0, cy - 2, w, 4, 2, pal.text, 0.16f);
                    g.roundRect(x0, cy - 2, w, 4, 2, pal.text, 0.22f);
                    g.circle(rx - 7, cy, 7, pal.text, 0.35f);
                };
            std::vector<Row> rows = { vol };
            auto shown = [this] {   // a choice just made: the pop-up as it now is, for a moment
                if (store.volPop != 0) return;
                volPopFrom_ = time;
                volPopUntil_ = time + 2.5;
            };
            rows.push_back(segRow("Volume keys show", store.volPop ? "Android's own panel" : "Songate's own, while Songate is in front",
                                  "set.volpop", { "Songate", "Android" }, store.volPop, [this, shown](int i) { store.volPop = i; shown(); }));
            if (!store.volPop) {
                rows.push_back(segRow("Where", "Where it shows on the screen", "set.volwhere", { "Top", "Side", "Bottom" }, store.volWhere,
                                      [this, shown](int i) { store.volWhere = i; shown(); }));
                // how long it stays after the last press (the user, 2026-11-17); 1.5 s as it always was
                static const float stays[4] = { 1.f, 1.5f, 3.f, 5.f };
                int si = 1;
                for (int i = 0; i < 4; i++) if (std::fabs(store.volStay - stays[i]) < 0.05f) si = i;
                rows.push_back(segRow("Stays for", "How long it shows after the last press", "set.volstay", { "1 s", "1.5 s", "3 s", "5 s" }, si,
                                      [this](int i) {
                                          store.volStay = stays[i];
                                          if (store.volPop == 0) { volPopFrom_ = time; volPopUntil_ = time + stays[i]; }   // (as it now is)
                                      }));
            }
            rows.push_back(segRow("Volume as", store.volNum ? "0 to 100%" : "The device's steps: 0 to " + std::to_string(maxStep) + " here",
                                  "set.volnum", { "Steps", "Percent" }, store.volNum, [this, shown](int i) { store.volNum = i; shown(); }));
            pc.insert(pc.begin() + 1, Card{ "Volume", rows });
        }
#endif
    }

    // Equalizer (gfeq.hpp: the same sound in Gatefold's own player and the background player)
    {
        auto dbText = [](float v) {
            char b[16];
            if (std::fabs(v) < 0.05f) return std::string("0 dB");
            snprintf(b, sizeof b, "%+.1f dB", double(v));
            return std::string(b);
        };
        // A ±12 dB value on a flat slider, in steps of 0.5 dB
        // curve: part of the curve (the preamp, as the bands): moved by hand it is Custom; bass and treble are global, a preset of yours keeps them
        // (bass, treble and the preamp are Songate's own stages: greyed while another app's music is shown, which they never reach.
        // The HiBy R4's bands are the ROM's equalizer, the whole device's, so they stay; the user, 2026-11-19)
        auto dbRow = [&](const std::string& label, const std::string& hint, const char* key, float GfbgEq::*field, bool curve = false) {
            Row r{ label, dbText(store.eq.*field), hint, 180, [this, key, field, curve](float rx, float cy) {
                           const float cur = (store.eq.*field + 12) / 24;
                           float nv = cur;
                           const float was = g.fade();
                           if (extApp_) g.setFade(was * 0.35f);
                           slider(uid(key), rx - setSliderW(), cy, setSliderW(), cur, &nv);
                           g.setFade(was);
                           if (nv != cur && !extApp_) {
                               GfbgEq e = store.eq;
                               e.*field = std::round((nv * 24 - 12) * 2) / 2;
                               if (curve || e.preset >= GFEQ_PRESETS) gfEqKeepCustom(&e);   // (the user, 2026-11-08: the preamp did not make a Custom)
                               setEq(e);
                           }
                       } };
            r.dim = extApp_;
            return r;
        };
        const bool adv = store.eq.advanced != 0;
        const int own = store.eq.preset - GFEQ_PRESETS;   // ≥ 0: one of your saved presets
        const bool edited = own >= 0 && store.eq.edited;  // ... changed since it was saved
        const std::string preset = eqPresetName(store.eq.preset) + (edited ? " (edited)" : "");
        Row onRow{ "Equalizer", "", std::string(GF_EQ_WHAT " Strong boosts lower the level a little to avoid distortion") +
                                        (r4::on() && store.bitPerfect ? ". While it is on, bit-perfect output is off" : ""),
                   40, [this](float rx, float cy) {
                       if (toggle(uid("set.eqon"), rx - 40, cy, store.eq.on != 0)) {
                           GfbgEq e = store.eq;
                           e.on = !e.on;
                           setEq(e);
                       }
                   } };
        onRow.focus = uid("set.eqon");
        Row presetRow = buttonRow("Preset", edited      ? "Changed since you saved it: save the change into it, or as a new preset"
                                            : own >= 0  ? "Yours: the bands, bass, treble and the preamp as you saved them"
                                            : store.eq.preset >= 0 ? (store.eq.hasCustom ? "Sets the ten bands; Custom keeps your own curve"
                                                                                         : "Sets the ten bands")
                                                                   : "Your own curve, kept when you choose a preset", "set.eqpreset",
                                  nullptr, preset, [this] { openEqPresets(); });
        // Under the preset: Save while the curve is your own, Rename and Delete while one of yours is on
        std::vector<Row> presetRows = { presetRow };
        const Row saveNew = buttonRow("Save as a new preset", "Keeps this sound as a preset of your own",
                                      "set.eqsave", ICON_PLUS, "Save new", [this] { saveEqPreset(); });
        if (store.eq.preset < 0) {
            presetRows.push_back(saveNew);
        } else if (own >= 0 && own < int(store.eqUser.size())) {
            if (edited) {   // your preset, changed: into it, or as a new one (one row, two buttons)
                presetRows.push_back(buttonsRow("Save the change", "Into “" + std::string(store.eqUser[size_t(own)].name) +
                                                                       "”, or as a new preset under its own name",
                                                { { "set.equpdate", ICON_CHECK, "Save", [this, own] { saveEqPreset(own); } },
                                                  { "set.eqsave", ICON_PLUS, "Save new", [this] { saveEqPreset(); } } }));
            }
            presetRows.push_back(buttonRow("Rename this preset", "", "set.eqrename", ICON_TAG, "Rename", [this, own] { renameEqPreset(own); }));
            presetRows.push_back(buttonRow("Delete this preset", "The sound stays as it is, as Custom", "set.eqdelete", ICON_X, "Delete",
                                           [this, own] { deleteEqPreset(own); }, true));
        }
        std::vector<Card> cards;
        cards.push_back({ "", { onRow,
                                segRow("Mode", adv ? "The ten bands and a preamp, as well as the preset, bass and treble"
                                                   : "A preset, bass and treble. Advanced adds the ten bands and a preamp",
                                       "set.eqmode", { "Simple", "Advanced" }, adv ? 1 : 0,
                                       [this](int i) {
                                           GfbgEq e = store.eq;
                                           e.advanced = uint8_t(i);
                                           setEq(e);
                                       }),
                                } });
        for (const Row& r : presetRows) cards.back().rows.push_back(r);
        cards.push_back({ "Tone", { dbRow("Bass", "Below about 100 Hz, under the muddy part, so a boost stays clean", "set.eqbass", &GfbgEq::bass),
                                    dbRow("Treble", "Above about 10 kHz: air and cymbals", "set.eqtreble", &GfbgEq::treble) } });
        if (adv) {
            // The ten bands, upright, with their value above and their frequency below
            Row bands;
            // The ten bands as a curve (the user chose "A, the curve", session 20, for every device): a ground with the dB lines,
            // the curve through the points and the area to 0 dB under it; a band's value shows while it is held or picked
            // Each band's value always shown, in a pill over the curve; tapped, it is typed (the user chose "A", 2026-11-15: "db info on
            // each EQ slider"; a tap, not a hold, since 2026-11-19)
            constexpr float valH = 32;   // the values' row
            bands.blockH = 262 + valH;
            bands.block = [this](float bx, float by, float bw) {
                static const char* names[GFEQ_BANDS] = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
                const float padL = 34.f;   // the dB scale's room
                const float px = bx + padL, pw = bw - padL - 8;
                const float col = pw / GFEQ_BANDS;
                const float top = by + 26 + valH, hh = 186.f;
                const float y0 = top + 7, travel = hh - 14, mid = y0 + travel * 0.5f;
                auto yOf = [&](float db) { return y0 + travel * (1 - (db + 12) / 24); };
                {   // the values, always shown; held, typed
                    const float vcy = by + 26;
                    g.textC("dB", { css::Xs }, bx + padL - 8, vcy, pal.text3, 1, Align::Right);
                    for (int i = 0; i < GFEQ_BANDS; i++) {
                        const float cx = px + col * (i + 0.5f), cw = std::min(col - 6, 46.f);
                        const Id vid = uid("set.eqval", i);
                        const bool tapped = ui.item(vid, { cx - cw / 2, vcy - 13, cw, 26 }, 13);   // (a tap, no longer a hold: the user, 2026-11-19)
                        const bool hot = ui.pressed(vid) || (ui.focused(vid) && ui.showFocusRing());
                        const float v = store.eq.bands[i];
                        g.roundRect(cx - cw / 2, vcy - 13, cw, 26, 13, std::fabs(v) < 0.05f ? pal.text : pal.accent, hot ? 0.2f : std::fabs(v) < 0.05f ? 0.06f : 0.14f);
                        char b[8];
                        snprintf(b, sizeof b, std::fabs(v) < 0.05f ? "0" : "%+.0f", double(v));
                        g.textNumC(b, { css::Xs, Weight::SemiBold }, cx, vcy, std::fabs(v) < 0.05f ? pal.text2 : pal.accentInk, 1, Align::Center);
                        if (tapped) {
                            std::string out;
                            char hdr[40];
                            const size_t nl = strlen(names[i]);
                            if (names[i][nl - 1] == 'k') snprintf(hdr, sizeof hdr, "%.*s kHz, in dB (−12 to +12)", int(nl - 1), names[i]);
                            else snprintf(hdr, sizeof hdr, "%s Hz, in dB (−12 to +12)", names[i]);
                            // Songate's card (the pad upright): the band follows each key; cancelled, the EQ as it was (its preset too)
                            const std::string ini = b[0] == '+' ? b + 1 : b;
                            if (askKb(KbKind::Number, hdr, ini, nullptr, [this, i, ini, before = store.eq](const std::string& t) {
                                    if (t == ini) { setEq(before); return; }
                                    char* end = nullptr;
                                    const double d = strtod(t.c_str(), &end);
                                    if (end == t.c_str()) return;   // (nothing, or a minus alone: the band stays)
                                    GfbgEq e = store.eq;
                                    e.bands[i] = float(std::clamp(std::round(d), -12.0, 12.0));
                                    gfEqKeepCustom(&e);
                                    setEq(e);
                                })) {}
                            else if (keyboard(hdr, b[0] == '+' ? b + 1 : b, out)) {
                                const char* s = out.c_str();
                                while (*s == ' ' || *s == '+') s++;
                                char* end = nullptr;
                                const double d = strtod(s, &end);
                                if (end != s) {
                                    GfbgEq e = store.eq;
                                    e.bands[i] = float(std::clamp(std::round(d), -12.0, 12.0));
                                    gfEqKeepCustom(&e);
                                    setEq(e);
                                }
                            }
                        }
                    }
                }
                {   // the ground: as the settings list's, the dB lines faint, 0 dB stronger
                    g.roundRect(bx, by, bw, 250 + valH, 16, pal.text, 0.04f);
                    for (int db : { 12, 6, 0, -6, -12 }) {
                        const float ly = yOf(float(db));
                        g.rect(px, ly - 0.5f, pw, 1, pal.text, db == 0 ? 0.22f : 0.07f);
                        char b[8];
                        snprintf(b, sizeof b, db > 0 ? "+%d" : "%d", db);
                        g.textNumC(b, { css::Xs }, bx + padL - 8, ly, pal.text3, 1, Align::Right);
                    }
                    for (int i = 0; i < GFEQ_BANDS; i++) g.rect(px + col * (i + 0.5f) - 0.5f, top, 1, hh, pal.text, 0.05f);
                }
                // the values (as dragged this frame), then the curve through them
                float val[GFEQ_BANDS];
                for (int i = 0; i < GFEQ_BANDS; i++) val[i] = store.eq.bands[i];
                for (int i = 0; i < GFEQ_BANDS; i++) {
                    const float cx = px + col * (i + 0.5f);
                    const Id id = uid("set.eqband", i);
                    const float cur = (store.eq.bands[i] + 12) / 24;
                    float nv = cur;
                    vslider(id, cx, top, hh, cur, &nv, i > 0 ? uid("set.eqband", i - 1) : 0, i + 1 < GFEQ_BANDS ? uid("set.eqband", i + 1) : 0,
                            false);
                    if (nv != cur) {
                        GfbgEq e = store.eq;
                        e.bands[i] = std::round(nv * 24 - 12);
                        val[i] = e.bands[i];
                        gfEqKeepCustom(&e);
                        setEq(e);
                    }
                }
                // Catmull-Rom through the points, flat out to the edges
                std::vector<SDL_FPoint> pts;
                auto at = [&](int i) { return SDL_FPoint{ px + col * (std::clamp(i, -1, GFEQ_BANDS) + 0.5f), yOf(val[std::clamp(i, 0, GFEQ_BANDS - 1)]) }; };
                pts.push_back({ px, yOf(val[0]) });
                for (int i = 0; i + 1 < GFEQ_BANDS; i++) {
                    const SDL_FPoint p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
                    for (int s = 0; s < 12; s++) {
                        const float t = s / 12.f, t2 = t * t, t3 = t2 * t;
                        pts.push_back({ p1.x + (p2.x - p1.x) * t,
                                        0.5f * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
                                                (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3) });
                    }
                }
                pts.push_back(at(GFEQ_BANDS - 1));
                pts.push_back({ px + pw, yOf(val[GFEQ_BANDS - 1]) });
                // the area between the curve and 0 dB, in thin strips (nothing overlaps)
                for (size_t k = 0; k + 1 < pts.size(); k++) {
                    const float x0 = pts[k].x, x1 = pts[k + 1].x, yy = (pts[k].y + pts[k + 1].y) / 2;
                    g.rect(x0, std::min(yy, mid), x1 - x0, std::fabs(yy - mid), pal.accent, 0.16f);
                }
                g.polyline(pts, 3.f, pal.accent, 1.f);
                // the points, and each band's value and frequency
                for (int i = 0; i < GFEQ_BANDS; i++) {
                    const float cx = px + col * (i + 0.5f), vy = yOf(val[i]);
                    const Id id = uid("set.eqband", i);
                    const bool live = ui.engaged(id), hot = live || (ui.focused(id) && ui.showFocusRing());
                    if (hot) g.circle(cx, vy, 17, pal.accent, live ? 0.3f : 0.18f);
                    g.circle(cx, vy + 1, 10.5f, { 0, 0, 0 }, 0.25f);
                    g.circle(cx, vy, 9.5f, pal.text);
                    g.circle(cx, vy, 6.5f, pal.accent);
                    char b[8];
                    snprintf(b, sizeof b, std::fabs(val[i]) < 0.05f ? "0" : "%+.0f", double(val[i]));
                    // the value only while a band is held or picked, in a bubble over its point
                    if (hot) {
                        const float bwid = g.measureNum(b, { css::Xs, Weight::SemiBold }) + 16, byy = vy - 36;
                        g.roundRect(cx - bwid / 2, byy - 11, bwid, 22, 11, pal.accent);
                        g.textNumC(b, { css::Xs, Weight::SemiBold }, cx, byy, pal.onAccent, 1, Align::Center);
                    }
                    g.textC(names[i], { css::Xs }, cx, top + hh + 18, pal.text3, 1, Align::Center);
                }
            };
            cards.push_back({ "Bands", { bands, dbRow("Preamp", "Everything louder or quieter; a limiter holds the peaks", "set.eqpre",
                                                      &GfbgEq::preamp, true) } });
        }
        if (r4::on()) {
            // The HiBy R4: its own equalizer's Automatic preamp (the same switch as its Settings › HiBy audio › Equalizer; the ROM's
            // prompt, 2026-11-15). Its equalizer plays the bands after Songate, so this lowers that stage by the largest boost;
            // Songate's own preamp above is a stage of its own and stays as it is. While on, what it takes off now
            const bool autoOn = r4::autoPreamp();
            int boost = 0;
            for (float b : store.eq.bands) boost = std::max(boost, int(std::lround(b)));
            boost = std::clamp(boost, 0, 12);
            const std::string cut = !store.eq.on ? std::string() : boost > 0 ? "−" + std::to_string(boost) + " dB" : std::string("0 dB");
            Row autoRow{ "Automatic preamp", autoOn ? cut : std::string(), "Lowers the level by the largest boost, so boosting never clips", 40,
                         [this, autoOn](float rx, float cy) {
                             if (toggle(uid("r4.eqauto"), rx - 40, cy, autoOn)) r4::setAutoPreamp(!autoOn);
                         } };
            autoRow.focus = uid("r4.eqauto");
            cards.back().rows.push_back(autoRow);
        }
        cards.push_back({ "", { buttonRow("Reset", "Flat, with bass, treble and the preamp at 0 dB", "set.eqreset", ICON_REFRESH, "Reset",
                                          [this] {
                                              GfbgEq e = store.eq;
                                              gfEqApplyPreset(&e, 0);
                                              e.bass = e.treble = e.preamp = 0;
                                              setEq(e);
                                              toast("Equalizer reset to Flat");
                                          }) } });
        sections.push_back({ "Equalizer", ICON_EQ, cards });
    }
    // The HiBy R4's Sound & DAC (its ROM's service, hibyr4.hpp; the user's choices, session 20): what plays through and how, the
    // outputs, the volume's rules. That device only; Songate installed before the service: one line saying what to do
    Section soundDac{ nullptr, ICON_HEADPHONES, {} };
    if (r4::avail() == r4::Avail::NoPermission) {
        soundDac.name = "Sound & DAC";
        soundDac.cards.push_back({ "", { infoRow("Reinstall Songate to enable HiBy settings", "") } });
    } else if (r4::on()) {
        soundDac.name = "Sound & DAC";
        std::vector<Card> cards;
        static const char* gainName[2] = { "Low gain", "High gain" };
        static const char* filterKey[3] = { "fast_rolloff", "slow_rolloff", "minimum_phase" };
        static const char* filterName[3] = { "Fast roll-off", "Slow roll-off", "Minimum phase" };
        const int gain = r4::get("gain") == "high" ? 1 : 0;
        const std::string fNow = r4::get("filter");
        const int filter = fNow == "slow_rolloff" ? 1 : fNow == "minimum_phase" ? 2 : 0;
        const int out35 = r4::get("out35") == "lineout" ? 1 : 0, out44 = r4::get("out44") == "bal_lo" ? 1 : 0;
        const int loGain = r4::get("lo_gain") == "low" ? 0 : 1;
        const std::string st = r4::get("st_output");
        const int rateHz = r4::getInt("st_rate", 0);
        char rate[24] = "";
        if (rateHz > 0) {
            if (rateHz >= 1000000) snprintf(rate, sizeof rate, "%.1f MHz", rateHz / 1000000.0);   // (DSD: 2.8 MHz)
            else if (rateHz % 1000 == 0) snprintf(rate, sizeof rate, "%d kHz", rateHz / 1000);
            else snprintf(rate, sizeof rate, "%.1f kHz", rateHz / 1000.0);
        }
        // the header: what plays through and where, the rate, then chips of what is set
        const bool lineOut = (st != "Balance" && st != "BalanceLo" && st != "none" && out35) || st == "BalanceLo";
        const std::string what = st == "none" ? "Nothing plugged in" : st == "Balance" ? "Balanced" : lineOut ? "Line out" : "Headphone";
        const std::string where = st == "none" ? "3.5 mm or 4.4 mm" : (st == "Balance" || st == "BalanceLo") ? "4.4 mm" : "3.5 mm";
        // (what HiBy's driver decodes, when it adds something: MQA, MQA Studio, MQB, DSD; as SongateOS's own header says it)
        const std::string q = r4::get("st_quality");
        const char* qName = q == "mqastudio" ? "MQA Studio" : q == "mqa" ? "MQA" : q == "mqb" ? "MQB" : q == "dsd" ? "DSD" : nullptr;
        std::string sub = rate[0] && st != "none" ? where + " · " + rate : where;
        if (qName && rate[0] && st != "none") sub += std::string(" · ") + qName;
        const std::string eqChip = store.eq.on ? "EQ " + eqPresetName(store.eq.preset) : std::string("EQ off");
        const std::string gainChip = lineOut ? (loGain ? "High line level" : "Low line level") : gainName[gain];
        const std::string filterChip = filterName[filter];
        // What plays, as SongateOS says (output_get's active; the user, 2026-11-16, through the SongateOS session): Bluetooth
        // headphones or a USB DAC named, with the codec and rate; what only the R4's own DAC does dimmed on the card (the
        // headphones or the DAC decode the sound themselves), its settings still there to change, the equalizer as it is; and what
        // else is connected, not playing, on a smaller line
        bool btHead = false, usbHead = false;
        std::string headWhat = what, headSub = sub, also;
        if (sysapi::has("output")) {
            const sysapi::Output o = sysapi::output();
            std::string others;
            for (const sysapi::Out& x : o.outs) {
                if (x.id != o.active) { others += (others.empty() ? "" : ", ") + x.name; continue; }
                if (x.kind == "bluetooth") {
                    btHead = true;
                    headWhat = x.name;
                    const sysapi::Codec cd = sysapi::codec();
                    headSub = "Bluetooth";
                    const std::string c = cd.known && !cd.codec.empty() ? cd.codec : x.codec;
                    if (!c.empty()) headSub += " · " + codecName(c);
                    if (cd.known && cd.rate > 0 && cd.bits > 0) {
                        char r[40];
                        if (cd.rate % 1000 == 0) snprintf(r, sizeof r, " · %d kHz / %d-bit", cd.rate / 1000, cd.bits);
                        else snprintf(r, sizeof r, " · %.1f kHz / %d-bit", cd.rate / 1000.0, cd.bits);
                        headSub += r;
                    }
                } else if (x.kind == "usb_dac") {
                    usbHead = true;
                    headWhat = x.name;
                    headSub = "USB DAC";
                }
            }
            if (!others.empty()) also = "Also connected: " + others;
            if (o.known && !o.connected) { headWhat = "Nothing plugged in"; headSub = "Headphones go in the 3.5 or 4.4 mm jack"; }
        }
        const bool away = btHead || usbHead;   // (the R4's own DAC not playing)
        const char* awayHint = btHead ? "Not used while Bluetooth plays" : "Not used while a USB DAC plays";
        Row head;
        head.blockH = also.empty() ? 116 : 140;
        head.block = [this, what = headWhat, sub = headSub, gainChip, filterChip, eqChip, btHead, usbHead, away, also](float bx, float by, float) {
            g.roundRect(bx, by + 8, 60, 60, 16, pal.accent, 0.12f);
            g.glow(bx + 30, by + 38, 30, pal.glow, 0.5f);
            g.icon(btHead ? outIcon("bluetooth") : usbHead ? outIcon("usb_dac") : ICON_HEADPHONES, bx + 30, by + 38, 32, pal.accent);
            g.text(what, { css::Xl, Weight::Bold }, bx + 76, by + 12, pal.text);
            g.text(sub, { css::Base }, bx + 76, by + 46, pal.text2);
            if (!also.empty()) g.text(also, { css::Sm }, bx + 76, by + 72, pal.text3);
            const float chipY = by + (also.empty() ? 84 : 108);
            float cx = bx;
            int k = 0;
            for (const std::string& c : { gainChip, filterChip, eqChip }) {
                const float cw = g.measure(c, { css::Sm, Weight::SemiBold }) + 24;
                const float ca = away && k < 2 ? 0.4f : 1.f;   // (dimmed: the DAC's own, not in effect now)
                g.roundRect(cx, chipY, cw, 28, 14, pal.text, 0.07f * ca);
                g.textC(c, { css::Sm, Weight::SemiBold }, cx + 12, chipY + 14, pal.text2, ca);
                cx += cw + 8;
                k++;
            }
        };
        cards.push_back({ "", { head } });
        // Sound
        const int balance = std::clamp(r4::getInt("balance", 0), -10, 10);
        const std::string bal = balance == 0 ? "Centre" : (balance < 0 ? "Left " : "Right ") + std::to_string(std::abs(balance));
        Row balRow{ "Balance", bal, "Left or right, in ten steps each way", 180, [this, balance](float rx, float cy) {
                       const float w = setSliderW(), cur = float(balance + 10) / 20.f;
                       g.rect(rx - w / 2 - 0.5f, cy - 9, 1, 18, pal.text, 0.28f);   // the middle, as the bands' 0 dB
                       float nv = cur;
                       slider(uid("r4.balance"), rx - w, cy, w, cur, &nv);
                       const int v = int(std::lround(nv * 20 - 10));
                       if (v != balance) r4::set("balance", std::to_string(v));   // each step while dragged: the ROM eases it
                   } };
        cards.push_back({ "Sound",
                          { segRow("Gain", away ? awayHint : "High drives harder headphones", "r4.gain", { "Low", "High" }, gain,
                                   [](int i) { r4::set("gain", i ? "high" : "low"); }),
                            segRow("Digital filter", away ? awayHint : "The DAC's filter. The difference is very slight", "r4.filter",
                                   { "Fast", "Slow", "Minimum phase" }, filter, [](int i) { r4::set("filter", filterKey[i]); }),
                            balRow } });
        // Output: line out only after the question (the ROM's words); back to Headphone at once
        auto lineOutAsk = [this](bool on35) {
            confirm(ICON_HEADPHONES, "Output", on35 ? "3.5 mm" : "4.4 mm balanced", "Line out", "Switch to line out?",
                    std::string("The ") + (on35 ? "3.5 mm" : "4.4 mm") + " output will play at fixed full volume and the volume keys "
                    "won't change it. Use it only with an amplifier or powered speakers, never with headphones or earphones. It switches back to "
                    "headphone when you unplug the cable or restart the player.",
                    "Switch", [on35] { if (on35) r4::set("out35", "lineout"); else r4::set("out44", "bal_lo"); });
        };
        cards.push_back({ "Output",
                          { segRow("3.5 mm", out35 ? "Fixed full volume, for an amplifier or powered speakers" : "Line out for an amplifier or powered speakers",
                                   "r4.out35", { "Headphone", "Line out" }, out35,
                                   [lineOutAsk](int i) { if (i) lineOutAsk(true); else r4::set("out35", "headphone"); }),
                            segRow("4.4 mm balanced", "Line out here is untested", "r4.out44", { "Headphone", "Line out (untested)" }, out44,
                                   [lineOutAsk](int i) { if (i) lineOutAsk(false); else r4::set("out44", "bal_po"); }),
                            segRow("Line-out level", "The DAC's gain while line out is on. Gain comes back when it ends", "r4.logain", { "Low", "High" },
                                   loGain, [](int i) { r4::set("lo_gain", i ? "high" : "low"); }) } });
        // Each song at its own rate (SongateOS's out_rate; the user's words B, 2026-11-15): first in Output
        cards.back().rows.insert(cards.back().rows.begin(),
                                 toggleRow("Each song at its own rate",
                                           "The 3.5 mm and 4.4 mm outputs play each song at its own rate, with a pause of two or three seconds when it changes",
                                           "r4.outrate", store.matchRate));
        // The output switcher here too (SongateOS's output_get / output_set): first in Output, what media plays on now; Change opens
        // the same list as the volume pop-up's line
        if (sysapi::has("output")) {
            const sysapi::Output o = sysapi::output();
            const sysapi::Out* cur = nullptr;
            for (const sysapi::Out& x : o.outs) if (x.id == o.active) cur = &x;
            const std::string hint = cur ? cur->name + (cur->codec.empty() ? std::string() : " · " + codecName(cur->codec)) : std::string();
            cards.back().rows.insert(cards.back().rows.begin(),
                                     buttonRow("Plays on", hint, "r4.outpick", cur ? outIcon(cur->kind) : ICON_HEADPHONES, "Change", [this] {
                                         outList_ = true;
                                         outListWhere_ = -1;
                                         outListAt_ = -1;
                                     }));
            // two or more connected (wired and Bluetooth at once, one plays: the user's choice): Plays on is a switch between them,
            // the same choice as the volume pop-up's (output_set; the user, 2026-11-16, through the SongateOS session)
            if (o.outs.size() >= 2) {
                std::vector<std::string> opts, ids;
                int at = 0;
                for (const sysapi::Out& x : o.outs) {
                    const std::string k = x.kind;
                    if (x.id == o.active) at = int(opts.size());
                    opts.push_back(k == "headphone_35" ? "3.5 mm" : k == "balanced_44" ? "4.4 mm" : k == "lineout_35" ? "Line out 3.5"
                                 : k == "balanced_lineout_44" ? "Line out 4.4" : x.name);
                    ids.push_back(x.id);
                }
                cards.back().rows[0] = segRow("Plays on", "", "r4.playson", opts, at, [ids, cur = o.active](int i) {
                    if (ids[size_t(i)] != cur) sysapi::act("output_set", ids[size_t(i)], {}, "output");
                });
            }
        }
        // Charge from USB audio (SongateOS 2.1.1's usb_nocharge, 0 by default; the user chose Output and words A, 2026-11-22): off,
        // a USB DAC or speaker that powers the player (a Charge 6) no longer charges it; only when the service has the setting
        if (!r4::get("usb_nocharge").empty()) {
            const bool charge = r4::get("usb_nocharge") != "1", held = r4::get("st_usb_nocharge") == "1";
            cards.back().rows.push_back(Row{ "Charge from USB audio", "",
                                             held ? "Not charging from this device" : "A USB DAC or speaker can charge the player. Off, it never draws power from one",
                                             40, [this, charge](float rx, float cy) {
                                                 if (toggle(uid("r4.usbcharge"), rx - 40, cy, charge)) r4::set("usb_nocharge", charge ? "1" : "0");
                                             } });
        }
        // Bit-perfect output (the user's ROM prompt and choices, 2026-11-15): each song to the DAC untouched when the output offers
        // its rate and depth (a USB DAC today, SongateOS's own outputs later), while nothing changes the sound
        // (on, but the equalizer or levelling changes the sound: says so; the user, 2026-11-20)
        const bool bpEq = store.bitPerfect && (store.eq.on || r4::eqOn()), bpLevel = store.bitPerfect && store.levelMode != 0;
        cards.back().rows.push_back(toggleRow("Bit-perfect output",
                                              bpEq    ? "Off while the equalizer is on: it changes the sound. Turn the equalizer off to hear each song unchanged"
                                              : bpLevel ? "Off while volume levelling is on: it changes the sound. Set Volume levelling to Off to hear each song unchanged"
                                                        : "Each song straight to the DAC, unchanged, while the equalizer, levelling and crossfades are off. "
                                                          "Its format's badge sparkles",
                                              "r4.bitperfect", store.bitPerfect));
        // Controls
        static const char* stepsKey[4] = { "15", "25", "50", "100" };
        const int stepsNow = r4::getInt("vol_steps", 25);
        const int steps = stepsNow >= 100 ? 3 : stepsNow >= 50 ? 2 : stepsNow >= 25 ? 1 : 0;
        const bool fixedVol = r4::get("vol_restore") == "fixed";
        const bool remote = r4::get("remote") != "0";
        std::vector<Row> ctl = {
            Row{ "Headphone remote", "", "The buttons on a headphone's cable: play, pause, skip", 40, [this, remote](float rx, float cy) {
                    if (toggle(uid("r4.remote"), rx - 40, cy, remote)) r4::set("remote", remote ? "0" : "1");
                } },
            // SongateOS's btn_wake (the user approved, 2026-11-15): beside Headphone remote, the ROM's title
            Row{ "Media buttons wake the screen", "", "Play and pause turn the screen on, as HiBy's own player did. After a restart", 40,
                 [this](float rx, float cy) { const bool on = r4::get("btn_wake") == "1"; if (toggle(uid("r4.btnwake"), rx - 40, cy, on)) r4::set("btn_wake", on ? "0" : "1"); } },
            segRow("Volume steps", "How finely the volume buttons move. After a restart", "r4.steps", { "15", "25", "50", "100" }, steps,
                   [](int i) { r4::set("vol_steps", stepsKey[i]); }),
            segRow("Volume after a restart", fixedVol ? "Always starts at the level below" : "As you left it", "r4.volrestore",
                   { "As it was", "Fixed" }, fixedVol ? 1 : 0, [](int i) { r4::set("vol_restore", i ? "fixed" : "last"); }) };
        if (fixedVol) {
            const int boot = std::clamp(r4::getInt("vol_boot", 20), 0, 100);
            ctl.push_back(Row{ "Start volume", std::to_string(boot) + "%", "", 180, [this, boot](float rx, float cy) {
                                  const Id id = uid("r4.bootvol");
                                  const float cur = boot / 100.f;
                                  float nv = cur;
                                  slider(id, rx - setSliderW(), cy, setSliderW(), cur, &nv);
                                  const int v = int(std::lround(nv * 100));
                                  if (v != boot) r4::set("vol_boot", std::to_string(v));
                              } });
        }
        cards.push_back({ "Controls", ctl });
        soundDac.cards = std::move(cards);
    }

    // Library
    {
        const int tagged = meta.taggedCount();
        const int done = lib.tracks.empty() ? 100 : tagged * 100 / int(lib.tracks.size());
        std::string roots;
        for (const auto& r : lib.roots) roots += (roots.empty() ? "" : ", ") + r;
        // Covers ready: the count beside the label; Stop / Resume while the cover cache is being filled.
        const int n = int(lib.tracks.size()), cached = std::min(meta.coversCached(), n);
        const bool sweeping = meta.sweepPos() > 0 && meta.sweepPos() < n && cached < n;
        const std::string coversHint = store.stopCovers ? "Stopped. Covers are still made for what is on screen; Resume caches the rest"
                                                        : "Small copies of every cover, so lists open with them. Made in the background";
        Row coversRow = coversHeld && !store.stopCovers && cached < n   // held at launch, or Not now on the covers question
                            ? buttonRow("Covers ready", "Not made yet: covers are made for what is on screen; Make them caches the rest",
                                        "set.covstop", ICON_PLAY, "Make them", [this] { appCoverNotice = 2; releaseCovers(); })
                        : store.stopCovers || sweeping
                            ? buttonRow("Covers ready", coversHint, "set.covstop", store.stopCovers ? ICON_PLAY : ICON_X,
                                        store.stopCovers ? "Resume" : "Stop", [this] { setJobStopped(Stoppable::Covers, !store.stopCovers); })
                            : infoRow("Covers ready", coversHint);
        coversRow.value = std::to_string(cached) + " of " + std::to_string(n);
        sections.push_back(
            { "Library", ICON_GRID,
              { { "", { buttonRow("Music folder", roots.empty() ? "Not chosen yet" : roots, "set.folder", ICON_FOLDER, "Change",
                                  [this] {
                                      folderFor = -1;
                                      SDL_Log("Music folder: Change, the picker at %s%s", plat::parentDir(store.musicRoot).c_str(),
                                              setupStep_ >= 0 ? " (the setup)" : "");
                                      navigate(View::PickFolder, -1, plat::parentDir(store.musicRoot));
                                  }),
                        infoRow("Songs", "Albums and artists come from the tags, however the folders are organised",
                                std::to_string(lib.tracks.size()) + " · " + std::to_string(lib.albums.size()) + " albums"),
                        infoRow("Tags read", "Kept " GF_ON_CARD " for next time", std::to_string(done) + "%"),
                        coversRow,
                        buttonRow("Rescan", "After adding or removing music", "set.rescan", ICON_REFRESH, "Rescan",
                                  [this] { rescan(); toast("Library rescanned: " + std::to_string(lib.tracks.size()) + " tracks"); }),
                        buttonRow("Read all tags again", "Forgets the tag cache and reads every file", "set.forget", nullptr, "Forget",
                                  [this] { meta.stop(); meta.clearDiskCache(); rescan(); toast("Reading all tags again"); }),
                        // tag and cover changes: where they go (the user, 2026-11-07: asked each time unless this says)
                        segRow("Tag and cover changes", store.tagWrite == 0 ? "Asked each time: Songate only, or into the file too"
                                                        : store.tagWrite == 1 ? "Kept in Songate; the files stay as they are"
                                                                              : "Into the file too, and kept in Songate",
                               "set.tagwrite", { "Ask", "Songate only", "Into the file" }, store.tagWrite,
                               [this](int i) { store.tagWrite = i; persistSoon(0.3f); }),
                        [&] {
                            const int left = int(tagPending_.size()) + (tagJobs.idle() ? 0 : 1);
                            const std::string hint = store.stopTagWrites ? "Stopped. The changes are in Songate; Resume writes the rest"
                                                   : left > 0 ? "Writing into the files, a song at a time (never the one playing)"
                                                              : "What the files held is kept, so a song's menu can put it back";
                            Row r = store.stopTagWrites || left > 0
                                        ? buttonRow("Written into files", hint, "set.tagwritestop", store.stopTagWrites ? ICON_PLAY : ICON_X,
                                                    store.stopTagWrites ? "Resume" : "Stop", [this] { setJobStopped(Stoppable::TagWrites, !store.stopTagWrites); })
                                        : infoRow("Written into files", hint);
                            const int n = int(tagUndo_.size());
                            r.value = std::to_string(n) + (n == 1 ? " song · " : " songs · ") + std::to_string(int((tagUndoBytes() + 512 * 1024) / (1024 * 1024))) + " MB kept";
                            return r;
                        }() } } } });
        {   // Subsonic (BETA 3, item 8): music from your own server; nothing goes online before you sign in (subsonic.hpp)
            const Subsonic::View sv = subsonic.view();
            std::string kind = sv.kind;
            if (!kind.empty()) kind[0] = char(toupper(uint8_t(kind[0])));
            std::vector<Row> rows;
            if (sv.state == Subsonic::SignedOut) {
                rows.push_back(buttonRow("Server", "Navidrome, Airsonic, Gonic… any server with the Subsonic API: its music plays here", "set.subin",
                                         ICON_LINK, "Sign in", [this] { subsonicSignIn(); }));
            } else if (sv.state == Subsonic::SigningIn) {
                rows.push_back(infoRow("Server", "Signing in to " + sv.server + "…"));
            } else if (sv.state == Subsonic::Failed) {
                rows.push_back(buttonRow("Server", sv.error + " · " + sv.server, "set.subin", ICON_REFRESH, "Try again", [this] { subsonicSignIn(); }));
                rows.push_back(buttonRow("Forget this server", "Its address and your sign-in leave this device", "set.subout", nullptr, "Forget",
                                         [this] { serverSignOutCard(); }));
            } else {
                rows.push_back(infoRow("Server", (kind.empty() ? std::string("Server") : kind + (sv.version.empty() ? "" : " " + sv.version)) + " at " + sv.server +
                                                              " · " + sv.user, "Signed in"));
                if (sv.songs >= 0) rows.push_back(infoRow("Songs on it", "", std::to_string(sv.songs)));
                {   // Downloads (stage 4; the user: "User's choice"): where, and what is here
                    const bool noMusic = store.musicRoot.empty();
                    rows.push_back(segRow("Downloads", store.serverDlPlace == 1 && !noMusic ? "In your music folder, under Songate downloads: they become your own songs"
                                                     : noMusic ? "In Songate's own folder (no music folder chosen)"
                                                               : "In Songate's own folder: played from here, shown as the server's",
                                          "set.subdl", { "Songate's folder", "Music folder" }, noMusic ? 0 : store.serverDlPlace,
                                          [this](int p) { store.serverDlPlace = p; }, noMusic ? 2u : 0u));
                    int songs = 0, albums = 0, done = 0, total = 0;
                    long long bytes = 0;
                    subsonic.dlCounts(songs, bytes, albums);
                    subsonic.dlProgress(done, total);
                    const bool working = total > 0 && done < total;
                    const std::string hint = store.stopDownloads ? "Stopped. What is downloaded stays; Resume gets the rest"
                                           : working ? "Albums you downloaded, one song at a time"
                                                     : "Albums you downloaded from the server: Download on an album's page";
                    Row r = store.stopDownloads || working
                                ? buttonRow("Downloaded", hint, "set.subdlstop", store.stopDownloads ? ICON_PLAY : ICON_X, store.stopDownloads ? "Resume" : "Stop",
                                            [this] { setJobStopped(Stoppable::Downloads, !store.stopDownloads); })
                                : infoRow("Downloaded", hint);
                    r.value = std::to_string(songs) + (songs == 1 ? " track · " : " tracks · ") + std::to_string(int((bytes + 512 * 1024) / (1024 * 1024))) + " MB";
                    rows.push_back(r);
                }
                rows.push_back(buttonRow("Sign out", "This device forgets the server and your sign-in; nothing on the server changes", "set.subout", nullptr,
                                         "Sign out", [this] { serverSignOutCard(); }));
            }
            sections.back().cards.push_back({ "Subsonic server", rows });
        }
    }

    // Lyrics
    sections.push_back({ "Lyrics", ICON_QUOTE,
                         { { "", { toggleRow("Lyrics in the Canvas", "Also the quote button in the Canvas", "set.cvlyr", store.canvasLyrics),
                                   toggleRow("Timing control", "Earlier · 0.0 s · Later above synced lyrics, kept for each song",
                                             "set.lyrtime", store.lyricTiming),
                                   segRow("Show it for", "Where the lyrics come from", "set.lyrtimefor", { "Tags", ".lrc files", "Both" },
                                          store.lyricTimingFor, [this](int i) { store.lyricTimingFor = i; }, store.lyricTiming ? 0u : 0x7u),
                                   infoRow("Where lyrics come from",
                                           "An .lrc file named as the song, or its LYRICS tag. " GF_CAP_TAP " a synced line to jump there"),
                                   // as Measured and Tags read (the user, 2026-11-05: "We should have something like this in lyrics settings too")
                                   [&] {   // Stop / Resume while not all read, as Measured and Covers ready
                                       const LyricIndex::Progress lp = lyrIndex.progress();
                                       const std::string hint = store.stopLyrics ? "Stopped. Search finds what is read so far; Resume reads the rest"
                                                              : lyrIndex.held() && lp.read < lp.total ? "Waits for the tags to be read first, then reads in the background"
                                                              : lp.checking ? "Looking for lyrics changed outside Songate, in the background"
                                                              : lp.read < lp.total ? "In the background while Songate is open, for search by lyrics"
                                                                                   : "For search by lyrics. Changed them on a computer? Read all again";
                                       Row r = lyrHeld && !store.stopLyrics && lp.read < lp.total
                                                   ? buttonRow("Lyrics read", "Not read yet: search finds what is read so far; Read them reads the rest", "set.lyrstop",
                                                               ICON_PLAY, "Read them", [this] { lyrNotice = 2; releaseLyrics(); })
                                               : store.stopLyrics || lp.read < lp.total
                                                   ? buttonRow("Lyrics read", hint, "set.lyrstop", store.stopLyrics ? ICON_PLAY : ICON_X, store.stopLyrics ? "Resume" : "Stop",
                                                               [this] { setJobStopped(Stoppable::Lyrics, !store.stopLyrics); })
                                                   : buttonRow("Lyrics read", hint, "set.lyrstop", ICON_REFRESH, "Read all again", [this] { lyrIndex.checkAll(); });
                                       r.value = std::to_string(lp.read) + " of " + std::to_string(lp.total);
                                       return r;
                                   }() } } } });
#ifdef GF_ANDROID
    // Upright Now playing, a song with synced lyrics: after a few seconds without a touch, the line being sung under the cover in the
    // buttons' place; a touch brings them back (the user, 2026-11-17, from a recording of a streaming app's; both ways, a choice here)
    if (css::Phone)   // (upright only: sideways has no button row under the cover; the user, 2026-11-17)
    sections.back().cards[0].rows.insert(sections.back().cards[0].rows.begin() + 1,
        segRow("Under the cover",
               store.lyrUnder == 0 ? "Off: the buttons always stay"
               : store.lyrUnder == 1 ? "After 4 s without a touch, the line being sung in place of the buttons; a touch brings them back"
                                     : "As This line, with the next one under it, dimmed",
               "set.lyrunder", { "Off", "This line", "And the next" }, store.lyrUnder, [this](int i) { store.lyrUnder = i; }));
#endif
    // The page in titled cards (the user, 2026-11-17: "organize the lyrics page better"): General (where they show and where they come
    // from, together), Timing, then Online below; three titled cards, so it folds, one open at a time
    {
        std::vector<Row> all = sections.back().cards[0].rows, show, timing, from;
        for (const Row& r : all) {
            if (r.label == "Lyrics in the Canvas" || r.label == "Under the cover") show.push_back(r);
            else if (r.label == "Timing control" || r.label == "Show it for") timing.push_back(r);
            else from.push_back(r);
        }
        show.insert(show.end(), from.begin(), from.end());
        sections.back().cards = { { "General", show }, { "Timing", timing } };
    }
    // Online (BETA 3, lyrics_online.cpp): off until turned on; what is sent, said where it is turned on
    sections.back().cards.push_back(
        { "Online",
          { toggleRow("Find lyrics online", "When a song with no lyrics plays, LRCLIB (lrclib.net) is asked for them: its title, artist, album and length are sent, nothing else",
                      "set.lyronline", store.lyricsOnline),
            segRow("When found", "Play them at once (Keep or Not these), or show them on a card first (Save or Not these)", "set.lyrway", { "Play them", "Ask first" },
                   store.lyricsOnlineWay, [this](int i) { store.lyricsOnlineWay = i; }, store.lyricsOnline ? 0u : 0x3u),
            // The server showing: its lyrics can only go to Songate's folder (no file of yours to write beside or into), so the
            // row shows that, the other two greyed; your own songs keep your choice (the user, 2026-11-09)
            [&] {
                const bool stream = store.libSource == 1 && subsonic.view().state == Subsonic::SignedIn;
                std::string who = subsonic.view().kind;
                if (who.empty()) who = "The server"; else who[0] = char(toupper(uint8_t(who[0])));
                return segRow("Save them",
                              stream ? who + "'s songs are not files of yours: their lyrics go to Songate's folder. Your own songs keep your choice"
                              : store.lyricsSaveTo == 0   ? "An .lrc beside the song: other players see it too"
                              : store.lyricsSaveTo == 2 ? "Written inside the song file, once it is not playing (the rest of the file as it was)"
                                                        : "Kept in Songate's folder: your music folder is not touched",
                              "set.lyrsave", { "Beside the song", "Inside the song", "In Songate's folder" },
                              stream ? 2 : store.lyricsSaveTo == 2 ? 1 : store.lyricsSaveTo == 1 ? 2 : 0,
                              [this](int i) { store.lyricsSaveTo = i == 1 ? 2 : i == 2 ? 1 : 0; }, !store.lyricsOnline ? 0x7u : stream ? 0x3u : 0u);
            }() } });

    // Canvas
    {
        static const std::vector<std::string> layouts = { "Cover", "Lyrics", "Minimal" };
        // Idle: when the controls fade, and how long the progress line takes to grow into the spectrum.
        char idleV[16], vizV[16];
        snprintf(idleV, sizeof idleV, "%.1f s", double(store.canvasIdleAfter));
        snprintf(vizV, sizeof vizV, "%.1f s", double(store.canvasVizTime));
        const Row idleAfterRow{ "Controls fade after", idleV, "Without a button or a touch", 180, [this](float rx, float cy) {
                                   const float cur = (store.canvasIdleAfter - 2) / 13;   // 2..15 s
                                   float nv = cur;
                                   slider(uid("set.cvidle"), rx - setSliderW(), cy, setSliderW(), cur, &nv);
                                   if (nv != cur) { store.canvasIdleAfter = std::round((2 + nv * 13) * 2) / 2; persistSoon(0.8f); }
                               } };
        const Row vizTimeRow{ "Spectrum rise", vizV, "How long the progress line takes to become the spectrum, and back", 180,
                              [this](float rx, float cy) {
                                  const float cur = (store.canvasVizTime - 0.2f) / 2.8f;   // 0.2..3 s
                                  float nv = cur;
                                  slider(uid("set.cvviz"), rx - setSliderW(), cy, setSliderW(), cur, &nv);
                                  if (nv != cur) { store.canvasVizTime = std::round((0.2f + nv * 2.8f) * 10) / 10; persistSoon(0.8f); }
                              } };
        sections.push_back(
            { "Canvas", ICON_CANVAS,
              { { "", { segRow("Layout",
                               canvasLayoutsOff() == 0b011 ? "This song has no lyrics: Minimal until one that has them"
                                                           : "Also at the bottom of the Canvas",
                               "set.cvlayout", layouts, canvasLayoutShown(), [this](int i) { store.canvasLayout = i; }, canvasLayoutsOff()),
                        toggleRow("Minimal when there are no lyrics", "Your layout comes back with the next song that has lyrics",
                                  "set.cvauto", store.canvasAutoMinimal) } },
                { "Cover motion",
                  { toggleRow("Float", "The cover rises and settles slowly", "set.cvfloat", store.canvasFloat),
                    toggleRow("Slow zoom", "A gentle Ken Burns zoom across the cover", "set.cvzoom", store.canvasZoom) } },
                { "When idle",
                  { idleAfterRow, vizTimeRow } } } });
    }

    // Visualizer
    {
        static const std::vector<std::string> bands = { "32", "56", "80" };
        const int bandIdx = store.vizBands <= 40 ? 0 : store.vizBands <= 64 ? 1 : 2;
        static const std::vector<std::string> cvBands = { "32", "56", "80", "112" };
        const int cvBandIdx = store.canvasBars <= 40 ? 0 : store.canvasBars <= 64 ? 1 : store.canvasBars <= 96 ? 2 : 3;
        sections.push_back(
            { "Visualizer", ICON_WAVE,
              { { "Spectrum",
                  { toggleRow("Spectrum on Now playing", "The Canvas turns its progress line into the same bars when idle", "set.spec",
                              store.showSpectrum),
                    sliderRow("Sensitivity", pct(store.vizSensitivity), "Higher for quiet recordings, lower if the bars hit the top",
                              "set.vsens", store.vizSensitivity),
                    sliderRow("Smoothing", pct(store.vizSmoothing), "Higher is calmer, lower follows every hit", "set.vsmooth",
                              store.vizSmoothing),
                    segRow("Bars on Now playing", "", "set.vbands", bands, bandIdx, [this](int i) { store.vizBands = i == 0 ? 32 : i == 1 ? 56 : 80; }),
                    segRow("Bars in the Canvas", "The spectrum the Canvas grows when it is idle", "set.cvbands", cvBands, cvBandIdx,
                           [this](int i) { store.canvasBars = i == 0 ? 32 : i == 1 ? 56 : i == 2 ? 80 : 112; }),
                    toggleRow("Peak caps", "The thin marks that fall slowly above the bars", "set.vpeaks", store.vizPeaks) } },
                { "Seek bar",   // (the user, 2026-11-06: "Let's make the waveform togglable on settings")
                  { toggleRow("Waveform", store.seekWave ? "The song's loudness as bars along the seek line" : "A plain line, the part played lit",
                              "set.seekwave", store.seekWave)
#ifdef GF_ANDROID
                    // (the user, session 19, of the three ways the bar came back in the clips: "I think we should have both as a
                    // setting", then all three, these words, Smooth by default)
                    , segRow("Seek bar animation", "How the bars come back after scrubbing", "set.seekanim", { "Quick", "Smooth", "Wave" }, store.seekAnim,
                             [this](int i) { store.seekAnim = i; })
#endif
                  } },
                { "Cover",
                  { sliderRow("Bass pulse", store.coverPulse < 0.01f ? "Off" : pct(store.coverPulse),
                              "How much the cover breathes with the bass. 100% is the default, 0 turns it off", "set.vpulse",
                              store.coverPulse),
                    toggleRow("Pulse in the Canvas too", "The full-screen cover breathes with the same strength", "set.vcvpulse",
                              store.canvasPulse) } } } });
    }

    // Controls
    {
        std::vector<Row> keys;
#ifdef GF_ANDROID
        // a touch screen first; a keyboard or a pad when one is attached (no window here: no full screen key, no window buttons)
        const std::pair<const char*, const char*> map[] = { { "Hold a song", "Its menu: tags, cover, queue, delete" },
                                                            { "Hold a button", "What it does" },
                                                            { "Enter  ·  pad A", "Select" },           { "Esc  ·  pad B", "Back" },
                                                            { "Space  ·  pad X", "Play / pause" },    { "Ctrl F  ·  pad Y", "Search" },
                                                            { "Ctrl ← →  ·  LB RB", "Previous / next song" },
                                                            { "Shift ← →  ·  LT RT", "Seek 10 seconds" },
                                                            { "C  ·  View", "Canvas (full screen)" },
                                                            { "Arrows  ·  D-pad", "Move around" } };
#elif defined(GF_WINDOWED)
        const std::pair<const char*, const char*> map[] = { { "Enter  ·  pad A", "Select" },           { "Esc  ·  pad B", "Back" },
                                                            { "Space  ·  pad X", "Play / pause" },    { "Ctrl F  ·  pad Y", "Search" },
                                                            { "Ctrl ← →  ·  LB RB", "Previous / next song" },
                                                            { "Shift ← →  ·  LT RT", "Seek 10 seconds" },
                                                            { "C  ·  View", "Canvas (full screen)" },
                                                            { "F11  ·  Alt Enter", "Full screen" },
                                                            { "Mouse to the top left corner", "The window's close, minimise and full screen buttons" },
                                                            { "Right-click  ·  Menu", "Song menu: tags, cover, queue, delete" },
                                                            { "Arrows  ·  D-pad", "Move around" },
                                                            { "Wheel  ·  right stick", "Scroll" } };
#else
        const std::pair<const char*, const char*> map[] = { { "A", "Select" },           { "B", "Back" },        { "X", "Play / pause" },
                                                            { "Y", "Search" },           { "L  R", "Previous / next song" },
                                                            { "ZL  ZR", "Seek 10 seconds" }, { "−", "Canvas (full screen)" },
                                                            { "+", "Song menu: tags, cover, queue, delete" },
                                                            { "HOME", "Leave Songate (close it from the HOME menu)" },
                                                            { "Right stick", "Scroll" } };
#endif
        for (auto& [k, l] : map) {
            const Font f{ css::Sm, Weight::SemiBold };
            const float kw = g.measure(k, f) + 18;
            Row r{ l, "", "", kw, [this, k = std::string(k), kw, f](float rx, float cy) {
                      g.roundRect(rx - kw, cy - 13, kw, 26, 7, pal.text, 0.065f);
                      g.roundStroke(rx - kw, cy - 13, kw, 26, 7, 1, pal.text, 0.15f);
                      g.textC(k, f, rx - kw / 2, cy, pal.text2, 1, Align::Center);
                  } };
            r.minH = 44;
            keys.push_back(r);
        }
        const Row speedRow = sliderRow("Pointer speed", pct(store.pointerSpeed * 2),
                                       GF_POINTER_WORDS, "set.ptrspeed", store.pointerSpeed);
        const Row accelRow = toggleRow("Pointer acceleration", "Slow for small moves, fast when the stick is pushed all the way",
                                       "set.ptraccel", store.pointerAccel);
        std::vector<Card> controlCards = { { "Buttons", keys },
                                           { "Pointer", { speedRow, accelRow } } };   // (Touch's card out: it said nothing to set; the user, 2026-11-08)
#ifdef __SWITCH__
        if (pocketAvailable()) {   // the pocket lock (a Reddit user, 2026-11-07; "super customisable": the user)
            std::vector<Row> pk;
            pk.push_back(toggleRow("Lock button on Now playing", "Under the cover: the screen off and every button and touch ignored, the music "
                                   "playing; HOME too when Songate runs in place of a game (from the album, HOME brings the screen back)", "set.pocketbtn", store.pocketButton));
            pk.push_back(segRow("Unlock with", store.pocketKey == 3 ? "Capture pressed twice, no Joy-Con needed in handheld: no screenshots while locked, "
                                                                     "the volume buttons as always"
                                : store.pocketHow == 0 ? "The volume buttons, no Joy-Con needed: a quick press still changes the volume, a hold unlocks"
                                                       : "The volume buttons, no Joy-Con needed: holding still changes the volume, two quick presses unlock",
                                "set.pocketkey",
                                { "Volume −", "Volume +", "Capture" }, store.pocketKey == 3 ? 2 : store.pocketKey,   // one key only (the user)
                                [this](int i) { store.pocketKey = i == 2 ? 3 : i; persistSoon(0.3f); }));
            if (store.pocketKey != 3)   // (Capture: pressed twice only; an app is never told it is held)
                pk.push_back(segRow("How", store.pocketHow == 0 ? "Held down" : "Pressed twice within a moment", "set.pockethow", { "Hold", "Press twice" },
                                    store.pocketHow, [this](int i) { store.pocketHow = i; persistSoon(0.3f); }));
            if (store.pocketKey != 3 && store.pocketHow == 0)
                pk.push_back(segRow("Hold for", "How long, so a pocket does not do it", "set.pockethold", { "1 s", "2 s", "3 s" }, store.pocketHold - 1,
                                    [this](int i) { store.pocketHold = i + 1; persistSoon(0.3f); }));
            pk.push_back(toggleRow("Turn off the controllers", "Joy-Con off the console and other wireless controllers, at the lock; a press "
                                   "wakes them (attached Joy-Con stay on)", "set.pocketpads", store.pocketPads));
            pk.push_back(toggleRow("Show how to unlock", "A press while locked lights a dim hint for a few seconds", "set.pockethint", store.pocketHint));
            pk.push_back(buttonRow("Lock now", "Unlock: " + pocketHowWords(), "set.pocketnow", ICON_LOCK, "Lock", [this] { pocketLock(true); }));
            controlCards.push_back({ "Pocket lock", pk });
        }
#endif
#ifdef GF_ANDROID
        // The HiBy R4: its hold switch (SongateOS's hold_get / hold_set, service v45) in place of Buttons and Pointer, which it does not
        // need: touch only (the user, 2026-11-17)
        // (and every Android device: touch only, so no Buttons and no Pointer; the user, 2026-11-18. A phone's Controls is then empty
        // and not listed)
        controlCards.clear();
        if (r4Dev_) {
            if (sysapi::has("hold")) {
                const sysapi::Hold h = sysapi::hold();
                static const char* key[3] = { "media", "volume", "all" };
                static const char* hints[3] = { "Play, pause and skip; the volume and power still work",
                                                "Play, pause, skip and the volume; power still turns the screen on and off",
                                                "Every button, until you slide it back" };
                const int lv = h.level == "media" ? 0 : h.level == "all" ? 2 : 1;
                std::vector<Row> rows = { infoRow("Hold switch", "On top of the player", !h.known ? "…" : h.locked ? "On" : "Off"),
                                          segRow("Locks", hints[lv], "sys.holdlevel", { "Media", "Volume", "All" }, lv,
                                                 [](int i) { sysapi::act("hold_set", "", { { "level", key[i] } }, "hold"); }),
                                          Row{ "Sleep when locked", "", "The screen turns off as you lock and on as you unlock", 40,
                                               [this, on = h.sleep](float rx, float cy) {
                                                   if (toggle(uid("sys.holdsleep"), rx - 40, cy, on))
                                                       sysapi::act("hold_set", "", { { "sleep", on ? "0" : "1" } }, "hold");
                                               } } };
                controlCards.push_back({ "Hold switch", rows });
            }
        }
#endif
        if (!r4Dev_) {
#if defined(__SWITCH__) || defined(GF_PREVIEW)
            const char* vibeHint = "The Joy-Con buzz as each key is pressed";
#elif defined(GF_ANDROID)
            const char* vibeHint = "A tick as each key is pressed";
#else
            const char* vibeHint = "The controller buzzes as each key is pressed";
#endif
            Card kb{ "Keyboard", { toggleRow("Key sound", "A sound as each key is pressed", "set.keysound", store.keySound),
                                   toggleRow("Key vibration", vibeHint, "set.keyvibe", store.keyVibe) } };
            // which sound, under Key sound while it is on (the user chose them and the words, 2026-11-18); a pick plays it
            if (store.keySound)
                kb.rows.insert(kb.rows.begin() + 1, segRow("Sound", "How each key sounds", "set.keysoundkind", { "Soft", "Tick", "Pop", "Clack" },
                                                           std::clamp(store.keySoundKind, 0, 3), [this](int i) {
                                                               store.keySoundKind = i;
                                                               keyClickPlay(true);
                                                           }));
            // its strength, under it while it is on (the user, 2026-11-18)
            if (store.keyVibe)
                kb.rows.push_back(segRow("Strength", "How strongly, each key", "set.keyvibelevel", { "Light", "Medium", "Strong" },
                                         std::clamp(store.keyVibeLevel, 1, 3) - 1, [this](int i) {
                                             store.keyVibeLevel = i + 1;
                                             keyFeedback();   // (a taste of it)
                                         }));
            // above Pocket lock (the user, 2026-11-18)
            auto at = std::find_if(controlCards.begin(), controlCards.end(), [](const Card& c) { return c.kicker == "Pocket lock"; });
            controlCards.insert(at, kb);
        }
        // the buttons' list last: only words, so the page opens on its first setting at the top (the user, 2026-11-20)
        std::stable_partition(controlCards.begin(), controlCards.end(), [](const Card& c) { return c.kicker != "Buttons"; });
        sections.push_back({ "Controls", ICON_SLIDERS, controlCards });
    }

    // Storage and reset: where each browser opens (view_storage.cpp), what Gatefold keeps on the card, and the reset
    std::vector<Row> folderRows, keptRows;
    {
        struct F { const char* label; const char* dflt; std::string* field; };
        static const std::string backupsDflt = placeWords(plat::backupDir());
        const F fs[5] = { { "Files opens in", "The top of " GF_THE_CARD, &store.filesDir },
                          { "Playlist imports open in", "The top of " GF_THE_CARD ", then where you last picked one",
                            &store.importDir },
                          { "Playlist exports open in", "The music folder's Playlists folder, then where you last exported",
                            &store.exportDir },
                          { "Cover picker opens in", "The song's own folder", &store.coverDir },
                          { "Backups go to", backupsDflt.c_str(), &store.backupDir } };
        static const char* chooseKeys[5] = { "set.dir0", "set.dir1", "set.dir2", "set.dir3", "set.dir4" };
        static const char* defaultKeys[5] = { "set.dirdef0", "set.dirdef1", "set.dirdef2", "set.dirdef3", "set.dirdef4" };
        for (int i = 0; i < 5; i++) {
            std::string* field = fs[i].field;
            std::vector<Btn> btns = { { chooseKeys[i], ICON_FOLDER, "Choose", [this, i] { chooseFolder(i); } } };
            if (!field->empty())
                btns.push_back({ defaultKeys[i], nullptr, "Default", [this, field] {
                                    field->clear();
                                    persistSoon(0.3f);
                                    toast("Back to its own default");
                                } });
            folderRows.push_back(buttonsRow(fs[i].label, field->empty() ? std::string(fs[i].dflt) : *field, btns));
        }
        struct K { const char* label; const char* hint; };
        const K ks[9] = { { "Covers cache", "Small copies of every cover, so lists open with their covers (" GF_CONFIG_NAME "/art)" },
                          { "Covers for the overlay", "What the overlay shows for each song (thumbs)" },
                          { "Loudness measurements", "Each song's levelling, Automix and mood measurements" },
                          { "Tag cache", "The tags read from your files, so opening is quick" },
                          { "Backups", "Your settings as they were before each install or restore" },
                          { "Update downloads", GF_DOWNLOADS_WORDS },
                          { "Lyrics found online", "Kept in Songate's folder (Settings › Lyrics › Online). Those beside the songs stay" },
                          { "Songs kept from the server", "The server's songs played lately, so they start at once (Subsonic server)" },
                          { "Server downloads", "Albums downloaded from the server into Songate's folder (Subsonic server)" } };
        static const char* clearKeys[9] = { "set.clr0", "set.clr1", "set.clr2", "set.clr3", "set.clr4", "set.clr5", "set.clr6", "set.clr7", "set.clr8" };
        const bool server = subsonic.view().state != Subsonic::SignedOut;
        // What Songate makes again by itself first, then what only comes back through you: the lyrics found online above Backups
        // (the user, 2026-11-04: "move the online lyrics from storage above Backups")
        for (int i : { 0, 1, 2, 3, 6, 7, 8, 5, 4 }) {   // (the user, 2026-11-06: lyrics, the server's two together, update downloads, backups)
#ifdef GF_WINDOWED
            if (i == 1) continue;   // the overlay's: none on a PC
#endif
            // the server's: only while signed in, or while something of it is still here (none of it shows otherwise: the user, 2026-11-06)
            if (i >= 7 && !server && !(storage.seen && storage.use[i].files > 0)) continue;
            Row r = buttonRow(ks[i].label, ks[i].hint, clearKeys[i], ICON_X, "Clear", [this, i] { clearStorage(i); }, true);
            r.value = storageValue(i);
            keptRows.push_back(r);
        }
    }
    // Storage (the SD card: where browsers open, Files, what Gatefold keeps); Reset is its own category after Updates
    std::vector<Row> filesRows = { toggleRow("Show hidden files", "Names that start with a dot, in Files", "set.hidden", store.showHidden) };
#ifdef GF_ANDROID
    // Files on the Library page held upright, between Folders and Liked (the user, 2026-11-16: its own switch here, every Android device;
    // it was the sidebar's Files tab's, and the Sidebar card goes when the screen is held upright)
    filesRows.push_back(Row{ "Files in Library", "", "A Files row on the Library page, held upright", 40, [this](float rx, float cy) {
                                 const bool on = store.libFiles > 0;
                                 if (toggle(uid("set.libfiles"), rx - 40, cy, on)) { store.libFiles = on ? 0 : 1; persistSoon(0.3f); }
                             } });
#endif
    sections.push_back({ "Storage", ICON_FOLDER,
                         { { "Where browsers open", folderRows },
                           { "Files", filesRows },
                           { "Kept " GF_ON_CARD, keptRows } } });
#ifdef GF_ANDROID
    // The player's own storage first (SongateOS's storage_get; the user's pick, 2026-11-16): each with its room left, a card or drive
    // ejected (the music paused first: Songate's library may be on it) or used again; then USB (usb_get / usb_set; the user chose
    // B: here). The R4 only
    if (r4Dev_ && sysapi::has("storage")) {
        const sysapi::Storage st = sysapi::storage();
        std::vector<Row> volRows;
        auto gb = [](long long b) {
            const double v = double(b) / 1e9;
            char s[24];
            snprintf(s, sizeof s, v < 100 ? "%.1f GB" : "%.0f GB", v);
            return std::string(s);
        };
        const bool anyBtn = std::any_of(st.vols.begin(), st.vols.end(), [](const sysapi::Volume& v) { return v.kind != "internal"; });
        for (size_t i = 0; i < st.vols.size(); i++) {
            const sysapi::Volume v = st.vols[i];
            const bool card = v.kind != "internal", mounted = v.state == "mounted";
            const char* kindName = v.kind == "sd" ? "microSD card" : v.kind == "usb" ? "USB drive" : "";
            std::string sub = card && v.label != kindName ? std::string(kindName) : std::string();
            const std::string state = mounted ? (v.total > 0 ? gb(v.free) + " free of " + gb(v.total) : std::string())
                                    : v.state == "ejecting" ? "Ejecting…" : v.state == "checking" ? "Checking…" : std::string("Ejected");
            if (!state.empty()) sub += (sub.empty() ? "" : " · ") + state;
            const float used = mounted && v.total > 0 ? 1 - float(double(v.free) / double(v.total)) : -1;
            const char* btn = !card ? nullptr : mounted ? "Eject" : v.state == "unmounted" ? "Mount" : nullptr;
            Row r;
            r.blockH = 64;
            r.block = [this, v, sub, used, btn, i, anyBtn, icon = !card ? ICON_FOLDER : v.kind == "usb" ? ICON_USB : ICON_SDCARD](float bx, float by, float bw) {
                g.icon(icon, bx + 14, by + 20, 22, pal.text2);
                const float tx = bx + 42;
                const Font ef{ css::Sm, Weight::SemiBold };
                float ew = 0;
                for (const char* t : { "Eject", "Mount" }) ew = std::max(ew, float(g.measure(t, ef)) + 32);   // (one width, either word)
                const float room = bw - 42 - (btn ? ew + 12 : 0);
                g.text(v.label, { css::Md, Weight::SemiBold }, tx, by + 2, pal.text, 1, Align::Left, int(room));
                g.text(sub, { css::Sm }, tx, by + 26, pal.text3, 1, Align::Left, int(room));
                if (used >= 0) {
                    const float barW = bw - 42 - (anyBtn ? ew + 16 : 0);   // (every bar one length, whether its row has a button or not)
                    g.roundRect(tx, by + 54, barW, 6, 3, pal.text, 0.12f);
                    g.roundRectH(tx, by + 54, std::max(6.f, barW * used), 6, 3, pal.accent2, pal.accent, 1);
                }
                if (btn) {
                    bool c = false;
                    button(uid("sys.vol", int(i)), nullptr, btn, bx + bw - ew, by + 14, false, &c, false, false, 0, ew);
                    if (c) {
                        if (!strcmp(btn, "Eject") && isPlaying()) player.togglePause();
                        sysapi::act(!strcmp(btn, "Eject") ? "storage_eject" : "storage_mount", v.id, {}, "storage");
                    }
                }
            };
            volRows.push_back(r);
        }
        if (volRows.empty()) volRows.push_back(infoRow("Internal storage", "", "…"));
        sections.back().cards.insert(sections.back().cards.begin(), { "On this player", volRows });
    }
    if (r4Dev_ && sysapi::has("usb")) {
        const sysapi::Usb u = sysapi::usb();
        static const char* usbKey[4] = { "charge", "mtp", "ptp", "midi" };
        static const char* usbName[4] = { "Charging", "Files", "Photos", "MIDI" };
        std::vector<std::string> opts, keys;
        int cur = 0;
        for (int i = 0; i < 4; i++)
            if (u.modes.empty() || std::find(u.modes.begin(), u.modes.end(), usbKey[i]) != u.modes.end()) {
                if (u.mode == usbKey[i]) cur = int(opts.size());
                opts.push_back(usbName[i]);
                keys.push_back(usbKey[i]);
            }
        std::vector<Row> usbRows = { infoRow("Computer", "", !u.known ? "…" : u.connected ? "Connected" : "Not connected"),
                                     segRow("Use it for", "", "sys.usb", opts, cur, [keys](int i) { sysapi::act("usb_set", keys[size_t(i)], {}, "usb"); },
                                            u.connected ? 0u : 0xFFu) };   // (only with a computer connected: the service's rule)
        auto& sc = sections.back().cards;
        const size_t usbAt = sysapi::has("storage") ? 1 : 0;
        sc.insert(sc.begin() + long(usbAt), { "USB", usbRows });
        // Files right below USB (the user, 2026-11-16)
        for (size_t k = usbAt + 1; k < sc.size(); k++)
            if (sc[k].kicker == "Files" && k != usbAt + 1) {
                Card f = sc[k];
                sc.erase(sc.begin() + long(k));
                sc.insert(sc.begin() + long(usbAt + 1), f);
                break;
            }
    }
#endif
    // Reset's buttons are one column, one width (the user, 2026-10-28: Reset sat narrower than Back up, Restore and Choose)
    float resetW = 0;
    for (const char* t : { "Back up", "Restore", "Reset", "Choose" }) resetW = std::max(resetW, float(g.measure(t, { css::Sm, Weight::SemiBold })) + 32 + 24);
    Section resetSection{ "Reset", ICON_REFRESH,
          { { "", { infoRow("Saved in", placeWords(Store::Dir) + ". Saved as you change things"),
                    buttonRow("Back up the settings now", "A copy in " + placeWords(backupRoot()) + ", to restore below",
                              "set.backupnow", ICON_HISTORY, "Back up", [this] {
                                  persistNow();
                                  Store::flush();
                                  // On the worker, in the progress stack: the folder can hold far more than Gatefold's own
                                  // files (a folder copied in by hand), and copying them on the main thread froze the app
                                  if (!startFileWork("Backing up the settings", backupRoot(), false, [this, root = backupRoot()](FileWork& w) {
                                          w.total = 0;
                                          const std::string b = updater.backupNow(root, "by hand");
                                          if (b.empty()) w.result = "Could not back up the settings";
                                          else w.result = "Backed up in " + b.substr(b.find_last_of('/') + 1);
                                      }, [this] { backupsAt = storage.at = -100; }))
                                      return;
                                  fileWork.cat = SetReset;
                              }, false, resetW),
                    buttonRow("Reset settings", GF_RESET_WORDS " Your music, playlists and Liked stay",
                              "set.reset", ICON_REFRESH, "Reset", [this] { resetSettings(); }, true, resetW),
                    buttonRow("Factory reset", "Erase what you choose, or everything. Your music files and backups stay",
                              "set.factory", ICON_X, "Choose", [this] { openFactory(); }, true, resetW) } } } };

    // Updates (view_updates.cpp, updater.hpp): off until allowed; then releases with their changelogs, install or go back
    {
        std::vector<Card> cards;
        const std::string me = Updater::thisVersion();
        Row onRow{ "Updates", "", store.updates ? "Online only to GitHub, when you check, install or go back"
                                                : "Off: Songate never looks for updates. On: new versions from GitHub, and going back",
                   40, [this](float rx, float cy) {
                       if (toggle(uid("set.updon"), rx - 40, cy, store.updates != 0)) {
                           if (store.updates) {
                               store.updates = 0;
                               updater.cancel();
                               persistSoon(0.3f);
                               toast("Updates off: Songate does not go online");
                           } else {
                               allowUpdates();   // asks first, saying what going online means
                           }
                       }
                   } };
        onRow.focus = uid("set.updon");
        std::vector<Row> top = { onRow };
        if (store.updates) {
            top.push_back(toggleRow("Check when Songate starts", phoneUpright() ? "A newer version shows at the top of Settings, until you answer it"   // (upright: no sidebar)
                                                                 : "A newer version shows above Settings in the sidebar, until you answer it", "set.updstart",
                                    store.updateAtStart));
            std::string hint;
            if (updater.busy()) hint = updateStatus();
            else if (!updater.error.empty()) hint = "Last try: " + updater.error;
            else if (updater.checkedAt) {
                char b[64];
                const time_t t = time_t(updater.checkedAt);
                strftime(b, sizeof b, "Last checked %d %b, %H:%M", localtime(&t));
                const int n = updater.newer(store.betas);
                hint = std::string(b) + (n >= 0 ? " · " + updater.releases[size_t(n)].version + " is out" : " · up to date");
            } else hint = "Lists every release below, with what changed";
            // Beta versions (the user, 2026-10-29: "a beta toggle so users can test features early"; asked first, with the
            // warning: App::allowBetas)
            Row betaRow{ "Beta versions", "", store.betas ? "Listed apart below. They can be unstable and have bugs: please report them"
                                                          : "New features before their release. Off: releases only",
                         40, [this](float rx, float cy) {
                             if (toggle(uid("set.updbeta"), rx - 40, cy, store.betas)) {
                                 if (store.betas) {
                                     store.betas = false;
                                     persistSoon(0.3f);
                                     toast("Beta versions off: only releases are offered");
                                 } else {
                                     allowBetas();
                                 }
                             }
                         } };
            betaRow.focus = uid("set.updbeta");
            top.push_back(betaRow);
            const bool busy = updater.busy();
            top.push_back(buttonRow("Check for updates", hint, "set.updcheck", busy ? ICON_X : ICON_REFRESH, busy ? "Stop" : "Check", [this, busy] {
                if (busy) updater.cancel();
                else checkUpdates(false);
            }));
        }
#ifdef GF_WINDOWED
        top.push_back(infoRow("This version", "", me));
#else
        top.push_back(infoRow("This version", "The app, the background player and the overlay come from one release and are updated together. "
                                              "Background player: " + std::string(player.remote() ? "connected" : "not connected (off, missing, or of another version)"),
                              me + " · protocol " + std::to_string(Updater::thisProtocol())));
#endif
        cards.push_back({ "", top });
        // Installed, waiting for a restart: the background player (if its file changed), then Gatefold
        const UpdInstalled& ins = updater.installed;
        if (!ins.version.empty() && (ins.appPending || ins.bgChanged || ins.overlayPending || ins.fresh)) {
            std::string what = Updater::releaseName(ins.version) + (ins.appPending ? " starts when this one closes" : " is in");   // (its own name: 1.x was Gatefold NX)
            if (ins.bgChanged) what += "; the background player restarts with it (the music stops for a moment)";
            if (ins.overlayPending) what += "; the overlay is replaced once Ultrahand lets go of it (close the Songate overlay)";
            if (ins.fresh) what += "; your settings are cleared (they are in the backup)";
            Row fin = buttonRow("Restart to finish", what, "set.updfinish", ICON_REFRESH, "Restart now", [this] { finishUpdate(); });
            fin.value = ins.from + " to " + ins.version;
            cards.push_back({ "Installed", { fin } });
        }
        // Every release: Details opens its changelog and Install / Go back
        if (store.updates || !updater.releases.empty()) {
            std::vector<Row> rel, betas;
            for (int i = 0; i < int(updater.releases.size()); i++) {
                const UpdRelease& r = updater.releases[size_t(i)];
                if (r.beta && !store.betas) continue;   // betas only with Beta versions on, and apart
                const int cmp = Updater::compare(r.version, me);
                char mb[24];
                snprintf(mb, sizeof mb, "%.1f MB", double(r.zipSize) / (1024.0 * 1024.0));
                const std::string hint = r.date + " · " + (cmp > 0 ? "newer" : cmp < 0 ? "earlier" : "this version") + " · " + mb +
                                         (r.manifest ? "" : " · without in-app updates");
                const float bw = g.measure("Details", { css::Sm, Weight::SemiBold }) + 32;
                Row row{ Updater::releaseName(r.version), "", hint, bw, [this, i, bw](float rx, float cy) {
                            bool c = false;
                            button(uid("set.updrel", i), nullptr, "Details", rx - bw, cy - 17, false, &c);
                            if (c) openRelease(i);
                        } };
                row.focus = uid("set.updrel", i);
                if (cmp == 0) row.value = "· installed";
                (r.beta ? betas : rel).push_back(row);
            }
            if (rel.empty()) rel.push_back(infoRow("No releases listed yet", "Check for updates reads them from GitHub"));
            cards.push_back({ "Releases", rel });
            if (store.betas) {
                if (betas.empty()) betas.push_back(infoRow("No beta versions right now", "Check for updates lists them here when there are"));
                cards.push_back({ "Beta versions", betas });
            }
        }
        // Automix's models (the user, 2026-10-30): the built-in one, the downloaded ones, and those the model repository lists;
        // Details opens each one's sheet (Download and use, Use, Go back)
        {
            std::vector<Row> mods;
            const std::vector<ModelRow> list = modelList();
            const std::string inUse = mixName();
            for (int i = 0; i < int(list.size()); i++) {
                const ModelRow& r = list[size_t(i)];
                const UpdModel* m = r.offer >= 0 ? &updater.models[size_t(r.offer)] : nullptr;
                const std::string hint = r.builtin ? "Built in · comes with Songate"
                                       : r.downloaded ? "Downloaded" + (m ? " · " + m->date : std::string())
                                       : m && Updater::modelUsable(*m) ? m->date + " · available · " + std::to_string(std::max<int64_t>(1, m->size / 1024)) + " KB"
                                                                      : "Needs a newer Songate";
                const float bw = g.measure("Details", { css::Sm, Weight::SemiBold }) + 32;
                Row row{ r.name, "", hint, bw, [this, i, bw, name = r.name](float rx, float cy) {
                            bool c = false;
                            button(uid("set.model", i), nullptr, "Details", rx - bw, cy - 17, false, &c);
                            if (c) openModel(name);
                        } };
                row.focus = uid("set.model", i);
                if (r.name == inUse) row.value = "· in use";
                mods.push_back(row);
            }
            cards.push_back({ "Automix models", mods });
        }
        cards.push_back({ "Backups", { infoRow("Before every install", "Your settings are backed up first; Settings › Reset puts them back") } });
#ifdef GF_ANDROID
        // SongateOS's updates: the page is the OS's alone (the user, 2026-11-18: Songate updates only with the OS, so no Songate tab,
        // no check of its own, no APK; before, a pill between the two). The service's updater (ota_status, watched always: App::sysTick)
        if (r4Dev_ && sysapi::has("ota")) {
            updTab_ = 1;
            {
                const sysapi::Ota o = sysapi::ota();
                auto bytes = [](long long b) {
                    char s[32];
                    if (b >= 1000LL * 1000 * 1000) snprintf(s, sizeof s, "%.1f GB", double(b) / 1e9);
                    else snprintf(s, sizeof s, "%d MB", int(std::lround(double(b) / 1e6)));
                    return std::string(s);
                };
                std::vector<Card> os;
                const std::string& st = o.state;
                const bool found = st == "available" || st == "downloading" || st == "verifying" || st == "ready" || st == "staging";
                if (!o.known) {
                    os.push_back({ "", { infoRow("SongateOS", "…") } });
                } else if (st == "available") {
                    std::string hint = bytes(o.size) + (o.prerelease ? " · a beta" : "") + " · you have " + o.current;
                    os.push_back({ "", { buttonRow("SongateOS " + o.latest + " is out", hint, "set.ota.dl", ICON_DOWNLOAD, "Download",
                                                   [] { sysapi::act("ota_download", "", {}, "ota"); }) } });
                } else if (st == "staging" || (st == "ready" && osInstallAt_ >= 0 && time - osInstallAt_ < 180)) {
                    // Install answered: the service checks the file again (about 45 s for 1 GB) before the player restarts, and nothing
                    // showed meanwhile ("nothing happened"; the user's words, 2026-11-21)
                    os.push_back({ "", { infoRow("Preparing the install…", "The player restarts by itself when it is ready") } });
                } else if (st == "verifying") {
                    // the download checked (about 75 s for 1 GB: it said "Downloading 0 MB of …" before; the user's words, 2026-11-21)
                    Row chk = infoRow("Checking the download", bytes(o.size));
                    Row bar;
                    bar.blockH = 18;
                    bar.block = [this](float bx, float by, float bw) { g.roundRectH(bx, by + 6, bw, 6, 3, pal.accent2, pal.accent); };
                    os.push_back({ "", { chk, bar } });
                } else if (st == "downloading") {
                    const std::string hint = bytes(o.progress) + " of " + bytes(o.size);
                    Row dl = buttonRow("Downloading SongateOS " + o.latest, hint, "set.ota.pause", nullptr, "Pause", [] { sysapi::act("ota_cancel", "", {}, "ota"); });
                    Row bar;
                    bar.blockH = 18;
                    const float f = o.size > 0 ? std::clamp(float(double(o.progress) / double(o.size)), 0.f, 1.f) : 0.f;
                    bar.block = [this, f](float bx, float by, float bw) {
                        g.roundRect(bx, by + 6, bw, 6, 3, pal.text, 0.12f);
                        if (f > 0.005f) g.roundRectH(bx, by + 6, std::max(6.f, bw * f), 6, 3, pal.accent2, pal.accent);
                    };
                    os.push_back({ "", { dl, bar } });
                } else if (st == "ready" && !o.recovery.empty() && o.recovery.rfind("ok:", 0) != 0) {
                    // the recovery not checked (a fresh install) or changed: the service refuses to install until it is, so this in
                    // Install's place (the Recovery card below says it too, but it can be folded away). The user's words A, 2026-11-21
                    os.push_back({ "", { buttonRow("Check the recovery", "The player restarts into the recovery and back, about half a minute",
                                                   "set.ota.rec1", nullptr, "Check it", [] { sysapi::act("power", "recovery_check", {}, "ota"); }) } });
                } else if (st == "ready") {
                    // Install: the service's checks first (battery, recovery, the file: App::sysTick asks the question once they pass)
                    os.push_back({ "", { buttonRow("SongateOS " + o.latest + " is ready", "The player restarts to install it", "set.ota.install", nullptr, "Install",
                                                   [this, v = o.latest] { installAsked_ = v; sysapi::preflight(); }) } });
                } else {   // idle, checking, up to date, an error
                    std::string hint;
                    if (st == "error" && !o.error.empty()) hint = o.error;
                    else if (o.lastCheck > 0) {
                        char b[64];
                        const time_t t = time_t(o.lastCheck / 1000);
                        strftime(b, sizeof b, "Last checked %d %b, %H:%M", localtime(&t));
                        hint = std::string(b) + " · up to date";
                    }
                    const bool checking = st == "checking";
                    os.push_back({ "", { buttonRow("SongateOS " + o.current, hint, "set.ota.check", ICON_REFRESH, checking ? "…" : "Check",
                                                   [checking] { if (!checking) sysapi::act("ota_check", "", {}, "ota"); }) } });
                }
                if (o.known && !o.card && !os.empty()) {
                    // no microSD card (service v97): updates are kept on it, so none come to the player without one; the guide says
                    // how to update from a computer (the user chose A, a Guide button, 2026-11-19)
                    os.front().rows.push_back(buttonRow("Updates need a microSD card", "Without one, you can update from a computer", "set.ota.card",
                                                        ICON_SDCARD, "Guide", [] { plat::openUrl(kSongateOSGuide); }));
                }
                if (o.known && !o.recovery.empty() && o.recovery.rfind("ok:", 0) != 0)
                    // (never checked after a fresh install, or changed: the service refuses to install until the recovery has written its
                    // record; this restarts into it, which writes it and comes back by itself. The user's words A, 2026-11-21)
                    os.push_back({ "Recovery", { buttonRow("Check the recovery", "The player restarts into the recovery and back, about half a minute",
                                                           "set.ota.rec", nullptr, "Check it",
                                                           [] { sysapi::act("power", "recovery_check", {}, "ota"); }) } });
                if (!o.notes.empty() && !setupPane_) {
                    // the release's notes read as Songate reads its own (updNotesLog: "## " parts, "- " lines, emoji as Songate's icons,
                    // install parts left out; Inter has no emoji and GitHub's boxes and tables came out as raw text, 2026-11-21), as tall
                    // as they are (wrapped to the card's text width: the page's, less its margins); a body in no such shape as it is
                    const float tw = (w < kNarrowList ? w : w - 300) - 2 * 48;   // (upright the page's width; sideways beside the categories)
                    const std::vector<UpdLog> log = updNotesLog(o.notes);
                    Row notes;
                    if (log.empty()) {
                        notes.blockH = 8 + mdBlock(o.notes, 0, 0, tw, false);
                        notes.block = [this, text = o.notes](float bx, float by, float bw) { mdBlock(text, bx, by + 4, bw, true); };
                    } else {
                        auto lay = [this, log](float bx, float by, float bw, bool draw) {
                            const Font tf{ css::Base, Weight::SemiBold }, lf{ css::Sm };
                            float y = by;
                            for (const UpdLog& sec : log) {
                                if (sec.items.empty()) continue;   // (a part in paragraphs alone: "Coming from a beta", its steps)
                                if (draw) {
                                    if (const char* ic = changelogIcon(sec.icon)) g.icon(ic, bx + 9, y + 12, 18, pal.accent);
                                    g.text(sec.title, tf, bx + 28, y + 2, pal.text);
                                }
                                y += 32;
                                for (const UpdItem& it : sec.items) {
                                    const auto lines = g.wrap(it.text, lf, int(bw - 28), 12);
                                    if (draw) {
                                        if (const char* ic = changelogIcon(it.icon)) g.icon(ic, bx + 9, y + 10, 15, pal.text3);
                                        float ly = y;
                                        for (const auto& l : lines) { g.text(l, lf, bx + 28, ly, pal.text2); ly += 19; }
                                    }
                                    y += lines.size() * 19.f + 8;
                                }
                                y += 10;
                            }
                            return y - by;
                        };
                        notes.blockH = 8 + lay(0, 0, tw, false);
                        notes.block = [lay](float bx, float by, float bw) { lay(bx, by + 4, bw, true); };
                    }
                    os.push_back({ "What's new", { notes } });
                }
                (void)found;
                if (o.known) {
                    auto optRow = [this](const std::string& label, const std::string& hint, const char* key, bool on, const char* which) {
                        return Row{ label, "", hint, 40, [this, key, on, which](float rx, float cy) {
                                       if (toggle(uid(key), rx - 40, cy, on)) sysapi::act("ota_set", which, { { "value", on ? "0" : "1" } }, "ota");
                                   } };
                    };
                    os.push_back({ "Options", { optRow("Check every day", "On Wi-Fi; a newer version shows here", "set.ota.auto", o.autoCheck, "auto"),
                                                optRow("Beta versions", "New features before their release. They can be unstable", "set.ota.beta", o.beta, "beta") } });
                    if (setupPane_)   // (the first-boot setup's Updates screen: view_setup.cpp)
                        os.push_back({ "Setup updates", { optRow("Check for updates automatically", "On Wi-Fi, once a day", "setup.ota.auto", o.autoCheck, "auto"),
                                                          optRow("Beta updates", "New features first. They can be unstable", "setup.ota.beta", o.beta, "beta") } });
                } else if (setupPane_)
                    os.push_back({ "Setup updates", { infoRow("Updates", "", "…") } });
                // Recovery and Root, a card each after the version's (SongateOS's recovery switch, v46, and root, v47; the user chose
                // A, 2026-11-17). Busy: what it is doing in the line; failed: the service's own words
                {
                    auto newer = [](const std::string& a, const std::string& b) {   // a dotted version newer than b
                        size_t i = 0, k = 0;
                        while (i < a.size() || k < b.size()) {
                            long x = 0, y = 0;
                            while (i < a.size() && isdigit(uint8_t(a[i]))) x = x * 10 + (a[i++] - '0');
                            while (k < b.size() && isdigit(uint8_t(b[k]))) y = y * 10 + (b[k++] - '0');
                            if (x != y) return x > y;
                            if (i < a.size()) i++;
                            if (k < b.size()) k++;
                        }
                        return false;
                    };
                    size_t at = std::min<size_t>(1, os.size());   // (after the version, and after What's new when there are notes)
                    for (size_t k = 0; k < os.size(); k++) if (os[k].kicker == "What's new") at = k + 1;
                    std::vector<Card> add;
                    if (sysapi::has("recovery")) {
                        const sysapi::Recovery r = sysapi::recovery();
                        if (r.known) {
                            auto name = [](const std::string& k) { return k == "twrp" ? std::string("TWRP") : k == "orangefox" ? std::string("OrangeFox")
                                                                          : k == "none" ? std::string("No recovery") : std::string("Another recovery"); };
                            const bool busy = r.state == "checking" || r.state == "downloading" || r.state == "verifying" || r.state == "switching";
                            std::string hint = r.ready ? "Ready for SongateOS updates" : "Not ready for SongateOS updates";
                            if (r.state == "checking") hint = "Looking for recovery packages…";
                            else if (r.state == "downloading") {
                                const long long size = r.installed == "twrp" ? r.foxSize : r.twrpSize;
                                hint = size > 0 ? "Downloading… " + std::to_string(int(std::min(100.0, 100.0 * double(r.progress) / double(size)))) + "%"
                                                : std::string("Downloading…");
                            } else if (r.state == "verifying") hint = "Checking the package…";
                            else if (r.state == "switching") hint = "Putting it in place…";
                            else if (r.state == "error" && !r.error.empty()) hint = r.error;
                            const std::string other = r.installed == "twrp" ? "orangefox" : "twrp";
                            const std::string otherVer = other == "twrp" ? r.twrpVersion : r.foxVersion;
                            // the one in place, Update on it when a newer build is offered (else Check); the other one's build below
                            // with Switch (the user, 2026-11-18: "Put the update button on the first one and list orangefox build with a
                            // button to switch")
                            std::vector<Row> rows;
                            const std::string title = name(r.installed) + (r.version.empty() ? std::string() : " " + r.version);
                            const std::string mine = r.installed == "twrp" ? r.twrpVersion : r.installed == "orangefox" ? r.foxVersion : std::string();
                            // the recovery not checked since it was put in (a fresh install: no record) or changed: SongateOS refuses to
                            // install until it is, and this card used to say "No recovery · Not ready" (OrangeFox was in). In that row's
                            // place: restart into it, which writes the record and comes back by itself (the user's words A, 2026-11-21;
                            // the card that said it apart was taken out below by this one, being called "Recovery" too)
                            const sysapi::Ota oo = sysapi::ota();
                            const bool unchecked = oo.known && !oo.recovery.empty() && oo.recovery.rfind("ok:", 0) != 0;
                            if (busy) rows.push_back(infoRow(title, hint));
                            else if (unchecked)
                                rows.push_back(buttonRow("Check the recovery", "The player restarts into the recovery and back, about half a minute",
                                                         "sys.rec.restart", nullptr, "Check it",
                                                         [] { sysapi::act("power", "recovery_check", {}, "ota"); }));
                            // (not after a failed update: Check then, its error the hint. The list it offered from can be out of date
                            // (a package taken down: "HTTP 404"), and only a check brings it up to date; with Update there, there was no
                            // way out on the player (SongateOS's report, 2026-11-22))
                            else if (!mine.empty() && newer(mine, r.version) && r.state != "error")
                                rows.push_back(buttonRow(title, hint + " · " + mine + " is out", "sys.rec.update", ICON_DOWNLOAD, "Update",
                                                         [kind = r.installed] { sysapi::act("recovery_switch", kind, {}, "recovery"); }));
                            else
                                rows.push_back(buttonRow(title, hint, "sys.rec.check", ICON_REFRESH, "Check",
                                                         [] { sysapi::act("recovery_check", "", {}, "recovery"); }));
                            if (!busy && !otherVer.empty())
                                rows.push_back(buttonRow(name(other) + " " + otherVer, "", "sys.rec.switch", nullptr, "Switch",
                                                         [other] { sysapi::act("recovery_switch", other, {}, "recovery"); }));
                            add.push_back({ "Recovery", rows });
                        } else add.push_back({ "Recovery", { infoRow("Recovery", "", "…") } });
                    }
                    if (sysapi::has("root")) {
                        const sysapi::Root rt = sysapi::root();
                        std::vector<Row> rows;
                        if (!rt.known) rows.push_back(infoRow("Root", "", "…"));
                        else if (rt.state == "restart")
                            rows.push_back(buttonRow(rt.installed ? "Magisk " + rt.version : std::string("Root"), "Restart the player to finish", "sys.root.restart",
                                                     ICON_REFRESH, "Restart now", [] { sysapi::act("power", "restart", {}, "root"); }));
                        else if (rt.state == "checking" || rt.state == "downloading" || rt.state == "verifying" || rt.state == "installing" ||
                                 rt.state == "removing") {
                            const char* s = rt.state == "checking" ? "Looking for Magisk…" : rt.state == "downloading" ? "Downloading Magisk…"
                                          : rt.state == "verifying" ? "Checking Magisk…" : rt.state == "installing" ? "Installing…" : "Removing…";
                            rows.push_back(infoRow(rt.installed ? "Magisk " + rt.version : std::string("Root"), s));
                        } else if (!rt.installed) {
                            const std::string v = rt.latest;
                            rows.push_back(buttonRow("Root", rt.state == "error" && !rt.error.empty() ? rt.error : std::string("Off"), "sys.root",
                                                     nullptr, "Install Magisk", [this, v] {
                                confirm(ICON_LOCK, "Updates", v.empty() ? std::string("Magisk") : "Magisk " + v, "", "Install Magisk?",
                                        "Root apps can change anything on the player, its sound and its files too. The player restarts once when it's done. "
                                        "Remove puts the original back.", "Install", [] { sysapi::act("root_install", "", {}, "root"); });
                                sheet.askSafe = true;   // (an install, not a loss: the accent button, the download symbol)
                                sheet.askYesIcon = ICON_DOWNLOAD;
                            }));
                        } else {
                            const std::string v = rt.version;
                            rows.push_back(buttonRow("Magisk " + v, rt.state == "error" && !rt.error.empty() ? rt.error : std::string("Root is on"), "sys.root",
                                                     nullptr, "Remove", [this, v] {
                                confirm(ICON_LOCK, "Updates", "Magisk " + v, "", "Remove Magisk?",
                                        "The player's original start-up is put back. It restarts once when it's done.", "Remove",
                                        [] { sysapi::act("root_remove", "", {}, "root"); });
                            }));
                            rows.push_back(Row{ "Keep root after updates", "", "A SongateOS update keeps Magisk", 40, [this, on = rt.keep](float rx, float cy) {
                                                   if (toggle(uid("sys.root.keep"), rx - 40, cy, on)) sysapi::act("root_set", "", { { "keep", on ? "0" : "1" } }, "root");
                                               } });
                        }
                        add.push_back({ "Root", rows });
                    }
                    if (sysapi::has("gapps")) {
                        const sysapi::Gapps ga = sysapi::gapps();
                        auto mb = [](long long b) {
                            char s[32];
                            snprintf(s, sizeof s, "%d MB", int(std::lround(double(b) / 1e6)));
                            return std::string(s);
                        };
                        const std::string& st = ga.state;
                        std::string which;
                        for (const sysapi::GappsVariant& v : ga.variants) if (v.id == ga.installed) which = v.name;
                        if (which.empty()) which = ga.installed == "core" ? "Core" : ga.installed == "basic" ? "Basic" : ga.installed;
                        const std::string name = ga.installed.empty() ? std::string("Google apps") : "Google apps " + which;
                        std::vector<Row> rows;
                        const bool noCard = o.known && !o.card;   // (kept on the microSD card too: none without one)
                        if (!ga.known) rows.push_back(infoRow("Google apps", "", "…"));
                        else if (noCard && ga.installed.empty()) {
                            Row r = infoRow("Google apps", "Needs a microSD card");
                            r.dim = true;
                            rows.push_back(r);
                        } else if (noCard) {
                            Row r = infoRow(name, "Needs a microSD card");
                            r.dim = true;
                            rows.push_back(r);
                        }
                        else if (st == "checking" || st == "downloading" || st == "ready" || st == "installing") {
                            const std::string s = st == "checking" ? "Looking for Google apps…"
                                                : st == "downloading" ? (ga.progress > 0 ? "Downloading… " + std::to_string(ga.progress) + "%" : std::string("Downloading…"))
                                                : st == "ready" ? "Restarting into the recovery…" : "Installing…";
                            rows.push_back(infoRow(name, s));
                        } else if (ga.installed.empty()) {
                            rows.push_back(infoRow("Google apps", st == "error" && !ga.error.empty() ? ga.error
                                                                  : "Play Store and Google services. Some apps may still refuse to run on SongateOS"));
                            static const char* const keys[] = { "sys.gapps.v0", "sys.gapps.v1", "sys.gapps.v2", "sys.gapps.v3" };
                            for (size_t i = 0; i < ga.variants.size() && i < 4; i++) {
                                const sysapi::GappsVariant& v = ga.variants[i];
                                rows.push_back(buttonRow(v.name, v.summary + (v.size > 0 ? " · " + mb(v.size) : std::string()), keys[i], nullptr, "Install",
                                                         [this, v] {
                                    confirm(ICON_DOWNLOAD, "Updates", "Google apps " + v.name, "", "Install Google apps " + v.name + "?",
                                            "Downloaded, then put in place from the recovery: the player restarts into it and back. Remove takes them out again.",
                                            "Install", [id = v.id] { sysapi::act("gapps_install", id, {}, "gapps"); });
                                    sheet.askSafe = true;   // (an install, not a loss: the accent button, the download symbol)
                                    sheet.askYesIcon = ICON_DOWNLOAD;
                                }));
                            }
                            if (ga.variants.empty())
                                rows.push_back(buttonRow("Nothing listed yet", "", "sys.gapps.check", ICON_REFRESH, "Check",
                                                         [] { sysapi::act("gapps_check", "", {}, "gapps"); }));
                        } else {
                            rows.push_back(buttonRow(name, st == "error" && !ga.error.empty() ? ga.error : ga.installedVersion, "sys.gapps", nullptr, "Remove",
                                                     [this, name] {
                                confirm(ICON_DOWNLOAD, "Updates", name, "", "Remove Google apps?",
                                        "Taken out from the recovery: the player restarts into it and back. Their apps and what they keep go too.", "Remove",
                                        [] { sysapi::act("gapps_remove", "", {}, "gapps"); });
                            }));
                            rows.push_back(Row{ "Keep Google apps after updates", "", "A SongateOS update keeps them", 40, [this, on = ga.keep](float rx, float cy) {
                                                   if (toggle(uid("sys.gapps.keep"), rx - 40, cy, on))
                                                       sysapi::act("gapps_set", "keep", { { "value", on ? "0" : "1" } }, "gapps");
                                               } });
                        }
                        add.push_back({ "Google apps", rows });
                    }
                    if (!add.empty()) {   // (the old Recovery card's line is part of this one now)
                        for (size_t k = 0; k < os.size(); k++) if (os[k].kicker == "Recovery") { os.erase(os.begin() + long(k)); break; }
                        os.insert(os.begin() + long(std::min(at, os.size())), add.begin(), add.end());
                    }
                }
                cards = os;
            }
        }
#endif
        sections.push_back({ "Updates", ICON_DOWNLOAD, cards });
    }
    // Reset: the backups (made by hand, before each install, before a restore) are listed here and put back from here,
    // between Back up and the two red ones, last: Reset settings and Factory reset
    {
        std::vector<Row>& first = resetSection.cards[0].rows;
        const std::vector<Row> resetRows(first.end() - 2, first.end());
        first.erase(first.end() - 2, first.end());
        {
            std::vector<Row> bk = { infoRow("Your backups", "Made above and before every install, in " + placeWords(backupRoot())) };
            const std::string pend = updater.restorePending();
            if (!pend.empty())
                bk.push_back(buttonRow("A restore waits", "When Songate closes: " + pend, "set.updrestx", ICON_X, "Cancel", [this] {
                    updater.cancelRestore();
                    toast("The restore is cancelled");
                }));
            const auto& list = updateBackups();
            for (int i = 0; i < int(list.size()) && i < 20; i++) {
                const UpdBackup& b = list[size_t(i)];
                const float bw = resetW;
                Row row{ b.name, "", b.note, bw, [this, i, bw](float rx, float cy) {
                            bool c = false;
                            button(uid("set.updbk", i), ICON_HISTORY, "Restore", rx - bw, cy - 17, false, &c, false, false, 0, bw);
                            if (c && i < int(backupsShown.size())) restoreBackup(backupsShown[size_t(i)]);
                        } };
                row.focus = uid("set.updbk", i);
                bk.push_back(row);
            }
            resetSection.cards.push_back({ "Backups", bk });
        }
        resetSection.cards.push_back({ "", resetRows });
    }
    sections.push_back(resetSection);

    // Support (the user, 2026-10-30: "Settings › Support"): a report from the console to Songate's reports on GitHub, through
    // a form (view_support.cpp, report.hpp); Atmosphère's crash reports; what a report says of this console. Just above About.
    {
        std::vector<Card> cards;
        std::vector<Row> rep;
        if (report::available()) {
            // off until allowed, as Updates (the user, 2026-10-30): the prompt says what is sent and where (allowReports)
            Row onRow{ "Reports", "", store.reports ? "Sent only when you press Send, to Songate's private reports"
                                                    : "Off: nothing is sent. On: report a bug or suggest a feature",
                       40, [this](float rx, float cy) {
                           if (toggle(uid("set.repon"), rx - 40, cy, store.reports)) {
                               if (store.reports) {
                                   store.reports = false;
                                   persistSoon(0.3f);
                                   toast("Reports off: Songate sends nothing");
                               } else {
                                   allowReports();
                               }
                           }
                       } };
            onRow.focus = uid("set.repon");
            rep.push_back(onRow);
            if (store.reports) {
                rep.push_back(buttonRow("Report a bug", "A short form: what happened, and where", "set.repbug", ICON_BUG, "Report",
                                        [this] { openReport(0); }));
                rep.push_back(buttonRow("Suggest a feature", "Something Songate could do, sent the same way", "set.repidea", PLI_BULB, "Suggest",
                                        [this] { openReport(1); }));
            }
        } else {
            rep.push_back(infoRow("Reports cannot be sent from this build", "Open an issue on GitHub instead: " ISSUES_URL));
        }
        cards.push_back({ "Report", rep });
        // read on a worker the first time this page shows (supportScan): never the card on the main thread
        const bool read = supportAsked_ && !supportThread_.joinable();
#ifndef GF_WINDOWED
        std::vector<Row> cr;
        if (!read) cr.push_back(infoRow("Atmosphère's crash reports", "Reading them…"));
        else if (supportCrashes_.empty()) cr.push_back(infoRow("Atmosphère's crash reports", "None on " GF_THIS_CONSOLE));
        else
            cr.push_back(buttonRow("Atmosphère's crash reports", std::to_string(supportCrashes_.size()) + (supportCrashes_.size() == 1 ? " report" : " reports") +
                                       ", the latest " + supportCrashes_.front().when, "set.crashes", ICON_BUG, "See them", [this] {
                                           if (sheet.preview) SDL_DestroyTexture(sheet.preview);
                                           const Id back = ui.focusedId();
                                           sheet = Sheet{};
                                           sheet.page = Sheet::Crashes;
                                           sheet.returnFocus = back;
                                           ui.focusDefault();
                                       }));
        cards.push_back({ "Crash reports", cr });
#endif
        // what a report says of this console, each as it is now (never the files' names: the switch's prompt says so)
        std::string playing = "Nothing";
        if (now.track >= 0 && !now.info.codec.empty()) {
            playing = now.info.codec;
            if (now.info.sampleRate > 0) { char r[24]; snprintf(r, sizeof r, " · %.1f kHz", now.info.sampleRate / 1000.0); playing += r; }
        }
        cards.push_back({ "Sent with a report",
                          { infoRow("Songate", "", Updater::thisVersion()),
#ifdef GF_WINDOWED
                            infoRow("System", "", read ? supportSys_.firmware : std::string("…")),
#else
                            infoRow("Firmware", "", read ? supportSys_.firmware : std::string("…")),
                            infoRow("Atmosphère", "", read ? supportSys_.atmosphere : std::string("…")),
                            infoRow("Background player", "", player.remote() ? "Connected" : "Not connected"),
#endif
                            infoRow("Playing", "Its format only, never its name", playing),
                            infoRow("Library", "", std::to_string(lib.tracks.size()) + " tracks") } });
        sections.push_back({ "Support", ICON_HELP, cards });
    }

    // Devices: other Songates, paired and synced (connect.hpp, view_connect.cpp): off until allowed
    {
        std::vector<Card> cards;
        const Connect::View cv = connect.view();
        auto deviceRow = [&](const std::string& label, const std::string& rowHint, const char* key, int i, const char* icon, const std::string& text, std::function<void()> go) {
            const float bw = g.measure(text.c_str(), { css::Sm, Weight::SemiBold }) + 32 + (icon ? 24 : 0);
            Row row{ label, "", rowHint, bw, [this, key, i, icon, text, bw, go](float rx, float cy) {
                        bool c = false;
                        button(uid(key, i), icon, text.c_str(), rx - bw, cy - 17, false, &c, false, false, 0, bw);
                        if (c) go();
                    } };
            row.focus = uid(key, i);
            return row;
        };
        Row onRow{ "Devices", "", store.connections ? "Songate looks for your paired devices on this network while it is open"
                                                        : "Off: Songate does not look for other devices, and they cannot find it",
                   40, [this](float rx, float cy) {
                       if (toggle(uid("set.cnon"), rx - 40, cy, store.connections)) {
                           if (store.connections) {
                               store.connections = false;
                               persistSoon(0.3f);
                               toast("Devices off");
                           } else {
                               allowConnections();
                           }
                       }
                   } };
        onRow.focus = uid("set.cnon");
        std::vector<Row> top = { onRow };
        if (store.connections) {
            top.push_back(segRow("Sync", store.syncAuto ? "When a paired device links, and a few seconds after anything changes" : "When you press Sync now", "set.cnauto", { "By hand", "Automatically" }, store.syncAuto ? 1 : 0, [this](int i) {
                store.syncAuto = i == 1;
                persistSoon(0.3f);
            }));
            top.push_back(infoRow("This device", cv.me.empty() ? std::string("Waiting for a network") : cv.me, cv.name));
            if (!cv.error.empty()) top.push_back(infoRow("Devices cannot start", cv.error));
        }
        cards.push_back({ "", top });
        if (store.connections) {
            if (!cv.code.empty()) {
                Row code{ "", "", "", 0, nullptr };
                code.blockH = 150;
                const std::string shown = cv.code.substr(0, 3) + " " + cv.code.substr(3);
                const std::string what = "Type this code on the other device.";
                const std::string howLong = "It works for " + std::to_string(cv.codeSeconds) + (cv.codeSeconds == 1 ? " more second, for one try." : " more seconds, for one try.");
                // a phone upright: its two sentences on a line each (as one they ran off both sides of the card: the user's
                // screenshot, 2026-11-11); the one that counts down in fixed-width digits
                const bool two = css::W < kNarrowList;
                if (two) code.blockH = 174;
                code.block = [this, shown, what, howLong, two](float bx, float by, float bw) {
                    g.text(shown, { 58, Weight::SemiBold }, bx + bw / 2, by + 16, pal.text, 1, Align::Center);
                    if (!two) { g.text(what + " " + howLong, { css::Md }, bx + bw / 2, by + 100, pal.text2, 1, Align::Center); return; }
                    g.text(what, { css::Md }, bx + bw / 2, by + 100, pal.text2, 1, Align::Center, int(bw));
                    g.textNum(howLong, { css::Md }, bx + bw / 2, by + 100 + 24, pal.text2, 1, Align::Center);
                };
                cards.push_back({ "Pairing", { code, buttonRow("Stop pairing", "", "set.cnstop", ICON_X, "Cancel", [this] { connect.cancelCode(); }) } });
                if (cnCodeFocus_) {   // Show a code went away with the press: the page goes to the code, not where focus fell back to
                    cnCodeFocus_ = false;   // (the user, 2026-11-05: "the page scroll to the middle instead of the section where the code is")
                    ui.focus(uid("set.cnstop"));
                    area("settings.pane").target = 0;
                }
            }
            const Syncer::View sv = syncer.view();
            const bool syncBusy = syncWaiting || sv.state == Syncer::State::Reading || sv.state == Syncer::State::Trading || sv.state == Syncer::State::Asking ||
                                  sv.state == Syncer::State::Backing || sv.state == Syncer::State::Applying || sv.state == Syncer::State::Sharing;
            std::string syncNote;
            if (syncWaiting || sv.state == Syncer::State::Reading || sv.state == Syncer::State::Trading || sv.state == Syncer::State::Backing ||
                sv.state == Syncer::State::Applying) syncNote = "Syncing…";   // no counters: each new number is a new text to draw
            else if (sv.state == Syncer::State::Sharing) syncNote = "Sharing measurements and covers…";
            else if (sv.state == Syncer::State::Failed) syncNote = sv.error;
            else if (sv.state == Syncer::State::Done) syncNote = "Last sync: " + sv.summary;
            std::vector<Row> paired;
            for (int i = 0; i < int(cv.paired.size()); i++) {
                const Connect::Paired& p = cv.paired[size_t(i)];
                const std::string state = p.linked ? "Linked, on this network" : p.here ? "On this network, connecting" : "Not on this network now";
                static std::string syncKeys[16], forgetKeys[16];
                const int slot = std::min(i, 15);
                syncKeys[slot] = "set.cnsync" + std::to_string(slot);
                forgetKeys[slot] = "set.cnforget" + std::to_string(slot);
                std::vector<Btn> btns;
                // (a device never synced with: First sync, which asks before anything changes; the question closed without an answer
                // is asked again from here)
                const bool waits = sv.peerAsks && sv.peer == p.name, firstSync = !syncer.met(p.id);
                if (p.linked) btns.push_back({ syncKeys[slot].c_str(), ICON_REFRESH, waits ? "Waiting…" : syncBusy ? "Syncing…" : firstSync ? "First sync" : "Sync now",
                                               [this, id = p.id, busy = syncBusy] { if (!busy) syncNow(id, false); } });
                btns.push_back({ forgetKeys[slot].c_str(), nullptr, "Forget", [this, id = p.id, name = p.name] {
                    connect.forget(id);
                    toast(name + " forgotten: pair again to link");
                } });
                paired.push_back(buttonsRow(p.name, waits ? "Waiting for " + p.name + " to confirm" : !syncNote.empty() ? syncNote
                                                    : firstSync && p.linked ? state + " · not synced yet" : state, btns));
            }
            if (paired.empty()) paired.push_back(infoRow("None yet", "Add a device: one shows a code, the other types it"));
            cards.push_back({ "Paired devices", paired });
            // listening together: the wording as in the mock-up the user saw (session 10)
            {
                static float nudge = 0.5f;
                int& delay = store.togetherDelay[std::clamp(store.togetherOutput, 0, 2)];
                static constexpr int kMax = Store::kTimingMax;
                nudge = float(delay + kMax) / (2.f * kMax);
                char ms[16];
                snprintf(ms, sizeof ms, "%+d ms", delay);
                auto timing = [&](const char* hint) {
                    // the D-pad a millisecond a press, as the user needs to land on a value exactly (2026-11-02: 39 could not be reached in 10s); a drag for the long way
                    return Row{ "Timing", ms, hint, 180, [this, &delay](float rx, float cy) {
                                   const Id id = uid("set.tgnudge");
                                   if (const int st = ui.sliderStep(id)) {
                                       ui.useSlide();
                                       delay = std::clamp(delay + st, -kMax, kMax);
                                       nudge = float(delay + kMax) / (2.f * kMax);
                                       persistSoon(0.8f);
                                   }
                                   float nv = nudge;
                                   slider(id, rx - setSliderW(), cy, setSliderW(), nudge, &nv);
                                   if (nv != nudge) {
                                       nudge = nv;
                                       delay = int(std::lround(nudge * 2 * kMax)) - kMax;
                                       persistSoon(0.8f);
                                   }
                               } };
                };
                if (tog.stage == Tog::On || tog.stage == Tog::Joining || tog.stage == Tog::Inviting) {
                    std::vector<Row> rows;
                    const std::string others = togetherNames();
                    if (tog.stage != Tog::On) rows.push_back(infoRow("Waiting", tog.stage == Tog::Inviting ? "Asking " + others + " to join" : "Agreeing on the queue and the time"));
                    else rows.push_back(infoRow(tog.linkLost ? "Waiting for " + tog.peerName : tog.rejoining ? "Getting back in time" : tog.startAt > 0 ? "In time" : "Getting ready", tog.linkLost ? "The link dropped. Each plays on, and the session goes on when it is back" : tog.rejoining ? "This device fell behind: it comes back in a few seconds" : tog.startAt > 0 ? "Corrected as it plays" : "Opening the song on every device", ms));
                    if (tog.host) {
                        // the devices in it, and those linked that are not (BETA 3: three or more, added from here)
                        std::vector<std::pair<std::string, std::string>> addable;
                        for (const Connect::Paired& p : cv.paired) if (p.linked && !togMember(p.id)) addable.push_back({ p.id, p.name });
#if defined(GF_DEV) && !defined(GF_WINDOWED) && !defined(GF_AIRPLAY)
                        if (getenv("GF_TOG_MOCK")) addable = { { "deck", "Steam Deck" } };
#endif
                        // a row for each device, and an Add for each linked one outside it (the user, 2026-11-05: "A, a row each")
                        static std::string addKeys[16];
                        for (const Tog::Member& mb : tog.members) rows.push_back(infoRow(mb.name, togetherStateOf(mb)));
                        for (int i = 0; i < std::min(int(addable.size()), 16); i++) {
                            addKeys[i] = "set.tgadd" + std::to_string(i);
                            rows.push_back(buttonRow(addable[size_t(i)].second, "Linked, not in this session", addKeys[i].c_str(), ICON_PLUS, "Add",
                                                     [this, id = addable[size_t(i)].first, name = addable[size_t(i)].second] { togetherInvite(id, name); }));
                        }
                    } else if (!tog.others.empty()) {
                        std::string list;
                        for (const std::string& o : tog.others) list += (list.empty() ? "" : ", ") + o;
                        rows.push_back(infoRow("Also listening", list));
                    }
                    if (tog.host && tog.missing > 0) rows.push_back(infoRow(std::to_string(tog.missing) + (tog.missing == 1 ? " song is not on every device" : " songs are not on every device"), "Skipped on all: copy them with Music files, then start again"));
                    // Who changes what plays (T7): the starter chooses, the others are told
                    if (tog.stage == Tog::On) {
                        if (tog.host)
                            rows.push_back(segRow("Who changes what plays", tog.onlyHost ? "Only this device: the others' Next, pause and seek are turned off" : "Next, pause, seek and a song chosen, from any device",
                                                  "set.tgwho", { "Everyone", "Only this device" }, tog.onlyHost ? 1 : 0, [this](int i) { togetherSetOnlyHost(i == 1); }));
                        else if (tog.onlyHost)
                            rows.push_back(infoRow(tog.peerName + " decides what plays", "It turned off Next, pause and seek here for this session. Leave is still yours"));
                        else
                            rows.push_back(infoRow("Every device changes what plays", "Next, pause, seek and a song chosen, from any device"));
                    }
                    rows.push_back(timing("Move it until this device sounds as one with the others"));
                    if (tog.host && tog.stage == Tog::On) {   // the delay, measured with the chirps (micmeasure.hpp)
                        const std::string who = togetherListener();
                        std::string whoName = "this device";
                        if (const Tog::Member* mb = who.empty() || who == "-" ? nullptr : togMember(who)) whoName = mb->name;
                        if (who.empty()) rows.push_back(infoRow("Measure the delay", "No device in this session has a microphone: set Timing by ear"));
                        else rows.push_back(buttonRow("Measure the delay", tog.measure.stage ? "Each device chirps in turn: keep it quiet for a few seconds"
                                                                                             : "Each device chirps in turn; " + whoName + " listens with its microphone. Put it where you sit",
                                                      "set.tgmeasure", ICON_REFRESH, tog.measure.stage ? "Measuring…" : "Measure", [this] { togetherMeasure(false); }));
                    }
                    rows.push_back(buttonRow("Leave", tog.host ? "This device stops, and the session ends; each of the others plays on by itself" : "This device stops; the others play on",
                                             "set.tgleave", ICON_X, "Leave", [this] { togetherLeave(); }));
                    cards.push_back({ "Listening with " + others, rows });
                } else {
                    std::vector<Row> rows;
                    static std::string startKeys[16];
                    for (int i = 0; i < std::min(int(cv.paired.size()), 16); i++) {
                        const Connect::Paired& p = cv.paired[size_t(i)];
                        if (!p.linked) continue;
                        startKeys[i] = "set.tgstart" + std::to_string(i);
                        rows.push_back(buttonRow("Listen with " + p.name, "One queue on both devices, in time. It is temporary: nothing is saved", startKeys[i].c_str(), ICON_LINK, "Start", [this, id = p.id, name = p.name] { togetherStart(id, name); }));
                    }
                    if (rows.empty()) rows.push_back(infoRow("Listen together", "Link a paired device on this network first"));
                    rows.push_back(segRow("This device plays through", "Each has its own delay, and Songate allows for it", "set.tgout", { "Speakers", "Headphones", "Bluetooth" }, store.togetherOutput, [this](int i) { store.togetherOutput = i; }));
                    rows.push_back(timing("Set once for this pair: move it until the two sound as one"));
                    cards.push_back({ "Listen together", rows });
                }
#ifdef GF_DEV
                std::vector<Row> dev;
                Row r{ "Test chirps", "", "Dev: a chirp each second of the song, for the microphone test", 40, [this](float rx, float cy) {
                          if (toggle(uid("set.tgchirp"), rx - 40, cy, togChirps)) { togChirps = !togChirps; player.syncChirps(togChirps); }
                      } };
                dev.push_back(r);
                cards.push_back({ "Listen together, dev", dev });
#endif
#if defined(GF_AIRPLAY) || (defined(GF_DEV) && !defined(GF_WINDOWED))
                {
                    std::string apError, apTitle, apPeer;
                    bool apStreaming = false, apShow = true;
#ifdef GF_AIRPLAY
                    const AirPlay::View av = airplay.view();
                    apError = av.error;
                    apTitle = av.title;
                    apPeer = av.peer;
                    apStreaming = av.streaming;
#else
                    // the preview has no AirPlay: GF_AIRPLAY_MOCK draws this card as the console does, for a mock-up
                    static const bool mock = getenv("GF_AIRPLAY_MOCK") != nullptr;
                    apShow = mock;
#endif
                    if (apShow) {
                        Row ar{ "Receive AirPlay", "", apError.empty() ? "iPhones on this network can play here, about a second late. New ones ask first" : apError, 40, [this](float rx, float cy) {
                                   if (toggle(uid("set.airplay"), rx - 40, cy, store.airplayReceive)) { store.airplayReceive = !store.airplayReceive; persistSoon(0.3f); }
                               } };
                        ar.focus = uid("set.airplay");
                        // ... and the other way (the user, 2026-11-10: Receive had a switch, playing on speakers had none)
                        Row sr{ "Play on AirPlay speakers", "", store.airplaySpeakers ? phoneUpright() ? "Speakers and TVs on this network are listed in the Devices card, on Now playing"   // (upright: no title bar)
                                                                                         : "Speakers and TVs on this network are listed under the title bar's AirPlay button"
                                                                                       : "Off: Songate does not look for speakers, and plays only here", 40, [this](float rx, float cy) {
                                   if (toggle(uid("set.airspk"), rx - 40, cy, store.airplaySpeakers)) {
                                       store.airplaySpeakers = !store.airplaySpeakers;
#ifdef GF_AIRPLAY
                                       if (!store.airplaySpeakers) airsend.chooseNone();   // what was playing on speakers comes back here
#endif
                                       persistSoon(0.3f);
                                   }
                               } };
                        sr.focus = uid("set.airspk");
                        std::vector<Row> rows = { sr, ar };
                        if (apStreaming) rows.push_back(infoRow(apTitle.empty() ? "Playing" : apTitle, "From " + apPeer));
                        // the devices that play without asking (Always), each forgotten on its own
                        static const char* const forgetKeys[] = { "set.apf0", "set.apf1", "set.apf2", "set.apf3", "set.apf4", "set.apf5", "set.apf6", "set.apf7",
                                                                  "set.apf8", "set.apf9", "set.apf10", "set.apf11", "set.apf12", "set.apf13", "set.apf14", "set.apf15" };
                        for (size_t i = 0; i < store.airplayDevices.size() && i < 16; i++) {
                            const std::string id = store.airplayDevices[i].id;
                            rows.push_back(buttonRow(store.airplayDevices[i].name, "Plays here without asking", forgetKeys[i], nullptr, "Forget", [this, id] {
                                std::erase_if(store.airplayDevices, [&](const Store::AirPlayDevice& d) { return d.id == id; });
                                store.saveAirplayDevices();
                                toast("Forgotten: it asks again next time");
                            }));
                        }
                        cards.push_back({ "AirPlay", rows });
                    }
                }
#endif
            }
            if (!cv.paired.empty()) {
                auto kindRow = [&](const char* label, const char* rowHint, const char* key, unsigned bit) {
                    Row r{ label, "", rowHint, 40, [this, key, bit](float rx, float cy) {
                              bool on = (store.syncKinds & bit) != 0;
                              if (toggle(uid(key), rx - 40, cy, on)) {
                                  store.syncKinds ^= bit;
                                  persistSoon(0.3f);
                              }
                          } };
                    r.focus = uid(key);
                    return r;
                };
                cards.push_back({ "What to share",
                                  { kindRow("Liked", "A song unliked on one device is unliked on the other", "set.cnk1", syncdata::KindLiked),
                                    kindRow("Playlists", "Your own playlists, in their order. One deleted here is deleted there too", "set.cnk2", syncdata::KindLists),
                                    kindRow("Tag edits", "Your files are never changed", "set.cnk4", syncdata::KindEdits),
                                    kindRow("Play counts", "Each device keeps its own count; both show the total", "set.cnk8", syncdata::KindPlays),
                                    kindRow("Lyrics", "Lyrics found online and kept. A song's own lyrics are never replaced", "set.cnk32", syncdata::KindLyrics),
                                    kindRow("Moods", "The moods you chose for songs: every device learns from them", "set.cnk64", syncdata::KindMoods),
                                    toggleRow("Measurements and covers", "Saves the other device the work, for the same files", "set.cnres", store.syncResults) } });
                {
                    const Copier::View fv = copier.view();
                    std::vector<Row> files;
                    files.push_back(toggleRow("Music files", store.syncFiles ? "Copies the songs missing here into the same folders. Never replaces anything" : "Off: no songs are copied here. Each device has its own switch", "set.cnfiles", store.syncFiles));
                    if (store.syncFiles) {
                        if (fv.state == Copier::State::Copying) {
                            char t[120];
                            snprintf(t, sizeof t, "%d of %d songs, %.1f of %.1f GB%s", fv.done, fv.total, double(fv.bytes) / 1073741824.0, double(fv.totalBytes) / 1073741824.0, isPlaying() ? " (slowly, while music plays)" : "");
                            files.push_back(buttonRow("Copying", std::string(t) + (fv.current.empty() ? "" : ". " + fv.current), "set.cnstopcopy", ICON_X, "Stop", [this] { copier.pause(); }));
                        } else if (fv.state == Copier::State::Looking) {
                            files.push_back(infoRow("Copying", "Looking at what the other device has…"));
                        } else if (fv.state == Copier::State::Failed) {
                            files.push_back(infoRow("Copying", fv.error));
                        } else if (fv.state == Copier::State::Done) {
                            files.push_back(infoRow("Copying", std::to_string(fv.done) + " copied" + (fv.failed ? ", " + std::to_string(fv.failed) + " failed" : "")));
                        }
                    }
                    Row delRow{ "Follow deletions", "", store.syncDeletes ? "A song deleted inside Songate on another device is deleted here too" : "Off: a song deleted on another device stays here",
                                40, [this](float rx, float cy) {
                                    if (toggle(uid("set.cndel"), rx - 40, cy, store.syncDeletes)) {
                                        if (store.syncDeletes) {
                                            store.syncDeletes = false;
                                            persistSoon(0.3f);
                                            toast("Songs here are no longer deleted from elsewhere");
                                        } else {
                                            allowDeletes();
                                        }
                                    }
                                } };
                    delRow.focus = uid("set.cndel");
                    files.push_back(delRow);
                    cards.push_back({ "Music files", files });
                }
            }
            std::vector<Row> near;
            for (int i = 0; i < int(cv.peers.size()); i++) {
                const Connect::Peer& p = cv.peers[size_t(i)];
                if (p.protocol != Connect::kProtocol) near.push_back(infoRow(p.name, p.address + " · another version of Songate: update both"));
                else if (p.offering) near.push_back(deviceRow(p.name, cv.pairing ? "Checking the code…" : p.address + " · showing a code", "set.cncode", i, nullptr, "Type the code", [this, id = p.id] { connectEnterCode(id); }));
                else near.push_back(infoRow(p.name, p.address + " · to pair, press Add a device on either one"));
            }
            if (cv.code.empty()) near.push_back(buttonRow("Add a device", "Shows a code here. Both devices need to be on this network", "set.cnadd", nullptr, "Show a code", [this] { connect.offerCode(); cnCodeFocus_ = true; }));
            near.push_back(buttonRow("Can't see it?", store.connectAim.empty() ? "Some networks carry no hellos. Type its address" : "Also saying hello to " + store.connectAim, "set.cnaim", nullptr, "Type an address", [this] { connectAddress(); }));
            cards.push_back({ "On this network", near });
        }
        sections.push_back({ "Devices", ICON_LINK, cards });
    }

#ifdef GF_DEV
    // Debug: dev builds only (the Makefile's DEV), never in a release: the testing tools
    sections.push_back(
        { "Debug", ICON_BOLT,
          { { "Self test",
              { toggleRow("Basic test", "Every page once, playing, Now playing's tabs, every Settings category: each step's frames and what looks wrong, "
                                        "in the log and selftest-basic.txt", "set.stbasic", selfTestOn_[0]),
                toggleRow("Advanced test", "Basic, then every list view and sort, the sheets, Canvas, search, Files, the first albums and artists, the "
                                           "server's views: in selftest-advanced.txt", "set.stadv", selfTestOn_[1]),
                toggleRow("Full test", "Advanced, then every album and artist page, and each control of the main pages focused in turn (nothing of "
                                       "yours is changed): in selftest-full.txt", "set.stfull", selfTestOn_[2]),
                buttonRow("New user test", "As newly installed: your settings folder set aside, then the first start, the tour and every feature; "
                                           "your folder put back at the end: in selftest-new-user.txt", "set.stnew", ICON_BOLT,
                          selfTestFresh_ ? "Running" : "Start", [this] { if (!selfTestFresh_) newUserTestAsk(); }) } },
            { "Testing",
              { toggleRow("Frame times", "A graph of the last two seconds of frames: green is on time, red took longer than a "
                                         "sixtieth of a second, the white part is the app's own work",
                          "set.frametimes", store.frameTimes),
                buttonRow("1-minute sleep timer", "The end of the sleep timer without the wait: the fade, the pause and the card",
                          "set.sleep1", ICON_MOON, sleepLeft() > 0 && sleepChoice == 9 ? "Running" : "Start", [this] {
                              stopAfterCurrent = false;
                              setSleep(60.0, 9);
                              toast("The music pauses in 1 minute");
                          }),
                infoRow("Background player memory", "Free on its heap (1.5 MB in all). A crossfade needs 320 KB of it for the second "
                                                    "song, else the join stays gapless",
                        player.remote() ? std::to_string(bg.state().heapFree / 1024) + " KB" : std::string("Not connected")) } } } });
    {   // Connections' first stage, measured on the console (identity.hpp): nothing runs it in a release yet
        const Identity::Progress ip = identity.progress();
        char hint[120];
        if (ip.running) snprintf(hint, sizeof hint, "%d of %d, %d of them read", ip.done, ip.total, ip.hashed);
        else if (ip.total) snprintf(hint, sizeof hint, "%d tracks, %d read, %d failed: %.2f ms each, %.1f s in all", ip.total, ip.hashed, ip.failed,
                                    ip.hashed ? ip.hashSeconds * 1000 / ip.hashed : 0.0, ip.seconds);
        else snprintf(hint, sizeof hint, "Reads what makes each song the same one on two devices, into identity.tsv");
        std::vector<Row> rows = { buttonRow("Identity", hint, "set.identity", ICON_REFRESH, "Measure", [this] { identityMeasure(); }) };
        sections.back().cards.push_back({ "Devices", rows });
    }
    {   // Automix's models, to compare on the console (the user, 2026-10-29; automix.hpp): a release plays the newest
        std::vector<std::string> models;
        for (int i = 0; i < int(automix::Model::Count); i++) {
            const std::string n = automix::name(automix::Model(i));
            models.push_back(n.rfind("Automix ", 0) == 0 ? n.substr(8) : n);   // "V1.0.0"
        }
        sections.back().cards.push_back(
            { "Automix", { segRow("Model", "How the joins are planned and an Automix playlist ordered. The next join uses it; Order again "
                                           "on a playlist to see its order by it. A release plays the newest",
                                  "set.mixmodel", models, int(mixModel()), [this](int i) {
                                      store.automixModel = automix::name(automix::Model(i));
                                      toast(std::string(automix::name(automix::Model(i))) + " for the next join");
                                  }) } });
    }
#endif

    // About
    {
        Row head;
        head.blockH = 92;
        head.block = [this](float bx, float by, float) {
            const Rgb note{ pal.accent.r + (pal.text.r - pal.accent.r) * 0.45f, pal.accent.g + (pal.text.g - pal.accent.g) * 0.45f, pal.accent.b + (pal.text.b - pal.accent.b) * 0.45f };   // (as the sidebar's mark)
            g.roundRect(bx, by + 8, 60, 60, 16, pal.accent, 0.12f);
            g.glow(bx + 30, by + 38, 30, pal.glow, 0.5f);
            g.icon(ICON_BRAND_ARCH, bx + 30, by + 38, 34, pal.accent);
            g.icon(ICON_BRAND_NOTE, bx + 30, by + 38, 34, note);

            // (the R4: SongateOS, the system it is; This player has no SongateOS row: the user, 2026-11-19)
            g.text(r4Dev_ ? "SongateOS" : "Songate for " GF_PLATFORM, { css::Xl, Weight::Bold }, bx + 76, by + 12, pal.text);
            g.text("Version " GF_VERSION, { css::Base }, bx + 76, by + 46, pal.text2);
            g.text("by AndryTheBeast", { css::Sm }, bx + 76, by + 68, pal.text3);   // (its own line: upright it ran off the side)
        };
        sections.push_back(
            { "About", ICON_INFO,
              { { "", { head } },
                { "Tour", { buttonRow("Take the tour", "Songate shown on its own screens: Basic, Advanced or Complete",
                                      "set.tour", ICON_SPARKLE, "Start", [this] { startTour(); }) } },
                { "Credits",
                  // the project it began as (the user, 2026-10-29: mentioned only as that, respectfully and factually; the
                  // licence requires the notice: LICENSE-gatefold)
                  { infoRow("Gatefold", "Songate began as a recreation of Gatefold, DEVIROYrus's interface for foobar2000. Its palette "
                                        "algorithm, icons, app icon and typeface come from that project. © Gatefold contributors, MIT licence"),
                    infoRow("Inter", "The typeface. © The Inter Project Authors, SIL Open Font Licence"),
                    infoRow("FFmpeg", "Decoding in Songate's own player: MP3, M4A (AAC) and FLAC. LGPL 2.1"),
                    infoRow("SDL 2", "Graphics, sound, fonts and images: SDL, SDL_mixer, SDL_ttf, SDL_image. zlib licence"),
                    infoRow("libFLAC, mpg123, Opus, Tremor", "SDL_mixer's decoders, for a file FFmpeg cannot open. BSD and LGPL licences"),
                    infoRow("libcurl and zlib", "Updates: the connection to GitHub, over " GF_THE_CONSOLE "'s own secure connection. curl and zlib licences"),
                    infoRow("Monocypher", "The encryption between your devices. © Loup Vaillant and contributors, BSD or CC0 licence"),
                    infoRow("libjpeg-turbo and stb_image", "Covers, decoded quickly. libjpeg-turbo's IJG and BSD licences; stb_image, public "
                                                           "domain or MIT"),
                    infoRow("Fraunces", "The Songate wordmark. © The Fraunces Project Authors, SIL Open Font Licence")
#ifdef GF_AIRPLAY
                    , infoRow("Mbed TLS", "AirPlay: answering an iPhone and decoding what it sends. © The Mbed TLS Contributors, Apache 2.0 "
                                          "licence"),
                    // how AirPlay works was learned from these (airplay.cpp): credited, though none of their code is used
                    infoRow("AirPlay", "How an iPhone speaks AirPlay was learned from shairport-sync (Mike Brady and contributors) and "
                                       "James Laird's ShairPort, which published the AirPort Express key it answers with. None of their code is "
                                       "in Songate. AirPlay is Apple's; Songate is not affiliated with Apple")
#endif
#ifndef GF_WINDOWED
                    , infoRow("libnx and devkitPro", "The Nintendo Switch homebrew toolchain. ISC licence"),
                    infoRow("Background player", "Helix AAC decoder (RealNetworks RPSL), minimp3 (CC0), dr_flac (public domain), and "
                                                 "the overlay is built on sys-tune's, with libultrahand (GPL 2)")
#endif
                  } } } });
#ifdef GF_ANDROID
        // The player, after Songate's own (SongateOS's about; the user chose A, 2026-11-16: one card). Nothing personal: the service
        // gives no serial. The R4 only
        if (r4Dev_ && sysapi::has("about")) {
            const sysapi::About ab = sysapi::about();
            std::vector<Row> rows;
            if (ab.known) {
                if (!ab.device.empty()) rows.push_back(infoRow("Device", "", ab.device));
                if (!ab.android.empty()) {
                    std::string h = ab.patch.empty() ? std::string() : "Security update " + ab.patch;
                    if (!ab.vendorPatch.empty()) h += (h.empty() ? "" : " · ") + std::string("HiBy's ") + ab.vendorPatch;
                    rows.push_back(infoRow("Android", h, ab.android));
                }
                if (!ab.build.empty()) rows.push_back(infoRow("Build", ab.build));
                if (!ab.kernel.empty()) rows.push_back(infoRow("Kernel", ab.kernel));
                // (the recovery as the updater reads it, "ok:TWRP" or "changed:TWRP:…": its name)
                if (const size_t c = ab.recovery.find(':'); c != std::string::npos) {
                    const std::string name = ab.recovery.substr(c + 1, ab.recovery.find(':', c + 1) - c - 1);
                    if (!name.empty()) rows.push_back(infoRow("Recovery", "", name));
                }
                if (!ab.service.empty()) rows.push_back(infoRow("Settings service", "", ab.service));
                if (ab.ram > 0) rows.push_back(infoRow("Memory", "", std::to_string(int(std::lround(double(ab.ram) / (1024.0 * 1024 * 1024)))) + " GB"));
                if (ab.battery >= 0) {
                    char t[24];
                    snprintf(t, sizeof t, "%.1f °C", ab.batteryTemp / 10.0);
                    const std::string st = ab.batteryStatus == "charging" ? "Charging · " : ab.batteryStatus == "full" ? "Full · " : "";
                    rows.push_back(infoRow("Battery", st + t, std::to_string(ab.battery) + "%"));
                }
            } else rows.push_back(infoRow("Device", "", "…"));
            sections.back().cards.insert(sections.back().cards.begin() + 1, { "This player", rows });
        }
#endif
    }
#ifdef GF_ANDROID
    // Home screen (the user, 2026-11-13: "turn songate into kinda of a launcher ... an awesome feature for people that have DAPs
    // like the hiby and don't want to see the android OS"). Android's own word is the truth (plat::homeHeld, asked once a second
    // while Settings is open): the switch asks Android, which asks the user in its own dialog. While Songate is the home screen
    // the device's other apps are listed here, Android's Settings among them: the way out of Songate.
    // The HiBy R4: no Home screen page (the user, 2026-11-16: Songate is its only launcher, the app drawer always on). The section
    // stays, empty and not listed, so the categories keep their numbers (SetFirst); hidden apps come back from the Apps page's menu
    if (r4Dev_) sections.push_back({ "Home screen", ICON_HOMESCREEN, {} });
    else {
        const bool held = homeHeld();
        Row sw{ "Use Songate as the home screen", "",
                "Songate opens when the device starts and at the home button. For players used only for music", 40,
                [this, held](float rx, float cy) {
                    if (toggle(uid("set.home"), rx - 40, cy, held)) {
                        plat::homeSet(!held);
                        homeHeldAt_ = time - 0.5;   // (Android's answer read soon; after its dialog, within the second)
                    }
                } };
        // Show Apps (the user, 2026-11-13: "a toggle to enable an extra menu on songate bottom bar in portrait... apps will only be
        // visible in portrait"; usable only with the home screen on, their choice): greyed until then
        Row ap{ "Show Apps", "", "Adds the device's other apps: a button in the bottom bar upright, a tab in the sidebar sideways", 40,
                [this, held](float rx, float cy) {
                    if (toggle(uid("set.apps"), rx - 40, cy, held && store.appsTab, held ? 1.f : 0.4f) && held) {
                        store.appsTab = !store.appsTab;
                        persistSoon(0.3f);
                    }
                } };
        ap.dim = !held;
        std::vector<Row> homeRows = { sw, ap };
        // (apps hidden from the Apps page, in an app's menu there: brought back here)
        if (const int hid = appsHidden(); hid > 0)
            homeRows.push_back(buttonRow("Hidden apps", std::to_string(hid) + (hid == 1 ? " app is" : " apps are") + " hidden from the Apps page",
                                         "set.unhide", nullptr, "Show all", [this] { appsShowAll(); }));
        sections.push_back({ "Home screen", ICON_HOMESCREEN, { { "", homeRows } } });
    }
#endif
#ifdef GF_ANDROID
    // Other apps' music (the user's mock-up, 2026-11-18): a card of Playback's
    {
        auto pb = std::find_if(sections.begin(), sections.end(), [](const Section& s) { return std::string(s.name) == "Playback"; });
        if (pb != sections.end()) {
            // the HiBy R4: headphones in start the music (the user, 2026-11-20), under Stop after the current song
            if (r4Dev_ && !pb->cards.empty() && pb->cards[0].rows.size() >= 2)
                pb->cards[0].rows.insert(pb->cards[0].rows.begin() + 2,
                                         toggleRow("Play when plugged in", "Plugging headphones into either jack starts the music where it is",
                                                   "set.playonplug", store.playOnPlug));
            if (time - otherAllowedAt_ > 1 || time < otherAllowedAt_) {   // (Android's answer crosses into the system: once a second)
                otherAllowed_ = plat::otherAllowed();
                otherAllowedAt_ = time;
            }
            const bool allowed = otherAllowed_;
            Row note;   // (the rule, under the switch)
            note.blockH = 40;
            note.block = [this](float bx, float by, float bw) {
                float ly = by + 4;
                for (const auto& l : g.wrap("Songate's own music always comes first: another app shows only while Songate is paused", { css::Sm }, int(bw), 2)) {
                    g.text(l, { css::Sm }, bx, ly, pal.text3);
                    ly += 18;
                }
            };
            // the bars for another app's music (the user chose B, 2026-11-19): on, Android's audio recording permission asked
            const Row specRow{ "Spectrum for other apps", "", "Songate hears what they play, for the bars only. Android asks to allow audio recording", 40,
                               [this](float rx, float cy) {
                                   if (!toggle(uid("set.otherspec"), rx - 40, cy, store.otherSpectrum)) return;
                                   if (store.otherSpectrum) {
                                       store.otherSpectrum = false;
                                       store.otherSpecAsked = true;
                                       persistSoon(0.3f);
                                   } else otherSpecAllow();
                               } };
            // between Sleep and Closing Songate (the user, 2026-11-19), else at the end
            auto closing = std::find_if(pb->cards.begin(), pb->cards.end(), [](const Card& c) { return c.kicker == GF_BG_CARD; });
            pb->cards.insert(closing, { "Other apps", { toggleRow("Show other apps' music", "What another app plays shows in Songate, with its controls", "set.otherapps",
                                                                   store.otherApps),
                                                         specRow,
                                                         buttonRow("Notification access", allowed ? "Allowed: Songate reads only what plays" : "Android asks once; Songate reads only what plays",
                                                                   "set.otheraccess", nullptr, allowed ? "Change" : "Allow", [] { plat::otherAccess(); }),
                                                         note } });
        }
    }
#endif
    // Storage goes with Library (your music, then where it and Gatefold's own files are): built later, placed after it
    {
        auto st = std::find_if(sections.begin(), sections.end(), [](const Section& s) { return std::string(s.name) == "Storage"; });
        if (st != sections.end()) {
            Section s = std::move(*st);
            sections.erase(st);
            auto li = std::find_if(sections.begin(), sections.end(), [](const Section& x) { return std::string(x.name) == "Library"; });
            sections.insert(li == sections.end() ? sections.end() : li + 1, std::move(s));
        }
    }
    // Reset above Updates (the user, 2026-10-30), as App::SetReset and SetUpdates number them
    {
        auto rs = std::find_if(sections.begin(), sections.end(), [](const Section& s) { return std::string(s.name) == "Reset"; });
        if (rs != sections.end()) {
            Section s = std::move(*rs);
            sections.erase(rs);
            auto up = std::find_if(sections.begin(), sections.end(), [](const Section& x) { return std::string(x.name) == "Updates"; });
            sections.insert(up, std::move(s));
        }
    }
    // Devices after Controls: what you use above, the upkeep from Reset down (the user, 2026-10-31), as App::SetDevices numbers it
    for (const char* name : { "Devices" }) {
        auto cn = std::find_if(sections.begin(), sections.end(), [&](const Section& s) { return std::string(s.name) == name; });
        if (cn != sections.end()) {
            Section s = std::move(*cn);
            sections.erase(cn);
            auto rs = std::find_if(sections.begin(), sections.end(), [](const Section& x) { return std::string(x.name) == "Reset"; });
            sections.insert(rs, std::move(s));
        }
    }
#ifdef GF_ANDROID
    // Home screen first (the user, 2026-11-13: "should on top of settings, the first tab"), the others after it as they were
    // (App::SetFirst numbers them)
    {
        auto hs = std::find_if(sections.begin(), sections.end(), [](const Section& s) { return std::string(s.name) == "Home screen"; });
        if (hs != sections.end() && hs != sections.begin()) std::rotate(sections.begin(), hs, hs + 1);
    }
#endif
    // the HiBy R4's pages last (the category numbers stay as they are), listed after Equalizer
    int r4Pages = 0;
    if (soundDac.name) { sections.push_back(soundDac); r4Pages++; }
    if (!r4LedCards.empty()) { sections.push_back({ "Status LED", ICON_LED, r4LedCards }); r4Pages++; }
#ifdef GF_ANDROID
    // SongateOS's system pages (session 21, phase 1; the SongateOS session's system API, sysapi.hpp): Connectivity (Wi-Fi and
    // Bluetooth, one page, a pill between them: the user, 2026-11-16) and System (update, power). The R4 only. A part the service
    // does not offer (an older one), or refuses Songate, opens Android's own page instead.
    if (r4Dev_) {
        // a network's signal: four bars, as many lit as it has (0..4)
        auto bars = [this](float rx, float cy, int level) {
            for (int b = 0; b < 4; b++) {
                const float h = 5.f + b * 3.f;
                g.roundRect(rx + b * 5.f, cy + 7 - h, 3, h, 1.5f, pal.text, b < level ? 0.85f : 0.18f);
            }
        };
        // the buttons down the right of Wi-Fi's page, one width (Forget, Connect, Open, Add); Bluetooth's two columns likewise
        // (the user, 2026-11-16: "they are in the same column so it stays consistent")
        const Font bf{ css::Sm, Weight::SemiBold };
        float wifiBtnW = float(g.measure("Add", bf)) + 32 + 24;
        for (const char* t : { "Forget", "Connect", "Open" }) wifiBtnW = std::max(wifiBtnW, float(g.measure(t, bf)) + 32);
        float btCol1 = 0, btCol2 = 0;
        for (const char* t : { "Disconnect", "Connect" }) btCol1 = std::max(btCol1, float(g.measure(t, bf)) + 32);
        for (const char* t : { "Forget", "Look", "Stop", "Pair" }) btCol2 = std::max(btCol2, float(g.measure(t, bf)) + 32);
        auto netRow = [this, bars, wifiBtnW](const std::string& ssid, const std::string& hint, int level, bool lock, const char* key, const char* btn,
                                             std::function<void()> act) {
            const float bw = wifiBtnW;
            Row r{ ssid, "", hint, 22 + 14 + 18 + 12 + bw, [this, bars, level, lock, key, btn, bw, act](float rx, float cy) {
                      const float bx = rx - bw - 12 - 18;
                      if (level >= 0) bars(bx, cy, level);
                      if (lock) g.icon(ICON_LOCK, bx - 12, cy, 14, pal.text3);
                      bool c = false;
                      button(uid(key), nullptr, btn, rx - bw, cy - 17, false, &c, false, false, 0, bw);
                      if (c && act) act();
                  } };
            r.focus = uid(key);
            return r;
        };
        auto onRow = [this](const std::string& label, const std::string& hint, const char* key, bool on, const char* method, const char* area) {
            return Row{ label, "", hint, 40, [this, key, on, method, area](float rx, float cy) {
                           if (toggle(uid(key), rx - 40, cy, on)) sysapi::act(method, "", { { "value", on ? "0" : "1" } }, area);
                       } };
        };
        // ---- Wi-Fi
        std::vector<Card> wifi;
        if (!sysapi::has("wifi")) {
            wifi.push_back({ "", { buttonRow("Wi-Fi", "Open in Android settings", "sys.wifi.android", nullptr, "Open", [] { plat::sysOpen("wifi"); }) } });
        } else {
            const sysapi::Wifi w = sysapi::wifi();
            wifi.push_back({ "", { onRow("Wi-Fi", "", "sys.wifion", w.enabled, "wifi_set_enabled", "wifi") } });
            if (w.enabled) {
                // joining a network: a saved one at once, an open one at once, else its password on Songate's own card, as dots,
                // passed once to the service and kept nowhere (the API's rule); again after a wrong one (ask: the password asked anyway)
                auto connectTo = [this](const sysapi::Net& n, bool ask) {
                    const bool open = n.security == "open" || n.security == "owe";
                    joinSsid_ = n.ssid;
                    if (n.saved && !ask) { sysapi::act("wifi_connect_saved", n.key, {}, "wifi"); return; }
                    if (open) { sysapi::act("wifi_connect", "", { { "ssid", n.ssid }, { "security", n.security }, { "hidden", "0" } }, "wifi"); return; }
                    askName(("Password for " + n.ssid).c_str(), "", [n](const std::string& pw) {
                        if (pw.empty()) return;
                        sysapi::act("wifi_connect", "", { { "ssid", n.ssid }, { "security", n.security.empty() ? "psk" : n.security }, { "password", pw },
                                                          { "hidden", "0" } }, "wifi");
                    }, true);
                };
                // the network in use (or being joined): its state in its line; Forget asks first (the API's rule); after a failure,
                // Connect again (a wrong password asked anew)
                const bool joining = w.link == "connecting" || w.link == "authenticating" || w.link == "obtaining_ip";
                const bool failed = w.link == "failed";
                if (w.hasCur || joining || failed) {
                    sysapi::Net n = w.cur;
                    if (!w.hasCur) {   // (no network in use yet: the one being joined, from the list by its name)
                        n.ssid = joinSsid_;
                        for (const sysapi::Net& x : w.nets) if (x.ssid == joinSsid_) n = x;
                    }
                    const std::string state = joining ? std::string("Connecting…")
                                            : failed ? (w.fail == "wrong_password" ? std::string("Wrong password") : std::string("Not connected"))
                                                     : "Connected" + (n.ip.empty() ? std::string() : " · " + n.ip);
                    const bool lock = n.security != "open" && n.security != "owe" && !n.security.empty();
                    if (failed)
                        wifi.push_back({ "Connected", { netRow(n.ssid, state, n.level, lock, "sys.wifi.cur", "Connect",
                                                               [connectTo, n, wrong = w.fail == "wrong_password"] { connectTo(n, wrong); }) } });
                    else
                        wifi.push_back({ "Connected", { netRow(n.ssid, state, n.level, lock, "sys.wifi.cur", "Forget", [this, n] {
                                                               if (n.key.empty()) return;
                                                               confirm(ICON_WIFI, "Connectivity", n.ssid, "", "Forget this network?",
                                                                       "The player stops joining it by itself.", "Forget",
                                                                       [key = n.key] { sysapi::act("wifi_forget", key, {}, "wifi"); });
                                                           }) } });
                }
                std::vector<Row> nets;
                for (size_t i = 0; i < w.nets.size(); i++) {
                    const sysapi::Net n = w.nets[i];
                    if ((joining || failed) && !w.hasCur && n.ssid == joinSsid_) continue;   // (shown above, as the one being joined)
                    const bool open = n.security == "open" || n.security == "owe";
                    const std::string hint = n.saved ? "Saved" : open ? "Open" : "";
                    // (enterprise and WEP networks: the service cannot join them (unsupported): Android's own page, where they can)
                    const bool android = n.security == "eap" || n.security == "wep";
                    if (android)
                        nets.push_back(netRow(n.ssid, "Open in Android settings", n.level, true, uidKey("sys.wifi.n", int(i)), "Open",
                                              [] { plat::sysOpen("wifi"); }));
                    else
                        nets.push_back(netRow(n.ssid, hint, n.level, !open, uidKey("sys.wifi.n", int(i)), "Connect", [connectTo, n] { connectTo(n, false); }));
                }
                nets.push_back(buttonRow("Add a hidden network", "One that does not show its name", "sys.wifi.hidden", ICON_PLUS, "Add", [this] {
                    askName("The network's name", "", [this](std::string ssid) {
                        while (!ssid.empty() && ssid.back() == ' ') ssid.pop_back();
                        if (ssid.empty()) return;
                        joinSsid_ = ssid;
                        askName(("Password for " + ssid).c_str(), "", [ssid](const std::string& pw) {   // (none: an open network)
                            sysapi::act("wifi_connect", "", { { "ssid", ssid }, { "security", pw.empty() ? "open" : "psk" }, { "password", pw }, { "hidden", "1" } }, "wifi");
                        }, true);
                    });
                }, false, wifiBtnW));
                wifi.push_back({ "Networks", nets });
            }
        }
        // ---- Bluetooth
        std::vector<Card> bt;
        if (!sysapi::has("bt")) {
            bt.push_back({ "", { buttonRow("Bluetooth", "Open in Android settings", "sys.bt.android", nullptr, "Open", [] { plat::sysOpen("bluetooth"); }) } });
        } else {
            const sysapi::Bt b = sysapi::bt();
            bt.push_back({ "", { onRow("Bluetooth", b.name.empty() ? std::string() : "Other devices see this player as " + b.name, "sys.bton", b.enabled,
                                       "bt_set_enabled", "bt") } });
            if (b.enabled) {
                std::vector<Row> paired, found;
                int k = 0;
                for (const sysapi::BtDev& d : b.devs) {
                    const std::string addr = d.address, name = d.name;
                    if (d.bonded) {
                        const std::string hint = d.connected ? "Connected" + (d.battery >= 0 ? " · Battery " + std::to_string(d.battery) + "%" : std::string())
                                                             : std::string("Not connected");
                        Btn c1{ uidKey("sys.bt.c", k), nullptr, d.connected ? "Disconnect" : "Connect",
                                [addr, on = d.connected] { sysapi::act(on ? "bt_disconnect" : "bt_connect", addr, {}, "bt"); } };
                        c1.w = btCol1;
                        Btn c2{ uidKey("sys.bt.f", k), nullptr, "Forget", [this, addr, name] {
                                   confirm(ICON_BLUETOOTH, "Connectivity", name, "", "Forget this device?", "To use it again, pair it again.", "Forget",
                                           [addr] { sysapi::act("bt_unpair", addr, {}, "bt"); });
                               } };
                        c2.w = btCol2;
                        paired.push_back(buttonsRow(name, hint, { c1, c2 }));
                    } else {
                        const char* kind = d.kind == "headphones" ? "Headphones" : d.kind == "speaker" ? "Speaker" : "";
                        found.push_back(buttonRow(name, kind, uidKey("sys.bt.p", k), nullptr, "Pair", [addr] { sysapi::act("bt_pair", addr, {}, "bt"); },
                                                  false, btCol2));
                    }
                    k++;
                }
                // The connected headphones' own card (phase 2; the user chose A, 2026-11-16): earbuds the service controls (battery,
                // noise control, wind) and, for any headphones, the codec. Read when the page opens (each earbuds call takes a second)
                const sysapi::BtDev* on = nullptr;
                for (const sysapi::BtDev& d : b.devs) if (d.connected && d.bonded) { on = &d; break; }
                std::vector<Row> hp;
                const sysapi::Earbuds eb = sysapi::earbuds();
                if (on && sysapi::has("earbuds") && eb.known && eb.supported) {
                    // Battery: a tile each for the left bud, the right one and the case, its icon, a battery filled to its level and the
                    // level (the user, 2026-11-19, every earbuds card). The case: live while it reports (a bud in it, the lid open),
                    // else its last level (no age: the user); left out when never seen
                    struct Tile { const char* icon; const char* name; int pct; bool charging; };
                    std::vector<Tile> tiles;
                    if (eb.batteryL >= 0) tiles.push_back({ ICON_BUD_L, "Left", eb.batteryL, eb.chargingL });
                    if (eb.batteryR >= 0) tiles.push_back({ ICON_BUD_R, "Right", eb.batteryR, eb.chargingR });
                    if (eb.batteryCase >= 0) tiles.push_back({ ICON_BUDCASE, "Case", eb.batteryCase, eb.chargingCase });
                    else if (eb.batteryCaseLast >= 0) tiles.push_back({ ICON_BUDCASE, "Case", eb.batteryCaseLast, false });
                    if (!tiles.empty()) {
                        Row bat;
                        bat.blockH = 120;   // (no "Battery" over it: the glyphs say it; the user, 2026-11-19)
                        bat.block = [this, tiles](float bx, float by, float bw) {
                            const float gap = 8, tw = (bw - gap * 2) / 3, ty = by + 8, th = 104;
                            for (size_t i = 0; i < tiles.size(); i++) {
                                const Tile& t = tiles[i];
                                const float n = float(tiles.size()), x0 = bx + (bw - (n * tw + (n - 1) * gap)) / 2;   // (two: centred)
                                const float tx = x0 + float(i) * (tw + gap), cx = tx + tw / 2;
                                g.roundRect(tx, ty, tw, th, 12, pal.text, 0.05f);
                                g.icon(t.icon, cx, ty + 22, 26, pal.text);
                                // the battery: an outline, its nub, the fill to its level (the accent; under 20% red; charging green
                                // with a bolt)
                                const float bwid = 30, bh = 14, bl = cx - bwid / 2 - 1, bt = ty + 46;
                                const Rgb fill = t.charging ? Rgb{ 0.42f, 0.82f, 0.52f } : t.pct < 20 ? dangerInk() : pal.accent;
                                g.roundStroke(bl, bt, bwid, bh, 3.5f, 1.3f, pal.text2, 1);
                                g.roundRect(bl + bwid + 1.5f, bt + bh / 2 - 3, 2.5f, 6, 1, pal.text2);
                                const float fw = std::max(2.f, (bwid - 5) * std::clamp(t.pct, 0, 100) / 100.f);
                                g.roundRect(bl + 2.5f, bt + 2.5f, fw, bh - 5, 2, fill);
                                if (t.charging) g.icon(ICON_BOLT, cx, bt + bh / 2, 11, pal.bg);
                                g.textNumC(std::to_string(t.pct) + "%", { css::Sm, Weight::SemiBold }, cx, ty + 74, pal.text, 1, Align::Center);
                                g.textC(t.name, { css::Xs }, cx, ty + 92, pal.text3, 1, Align::Center);
                            }
                        };
                        hp.push_back(bat);
                    }
                    if (eb.kind == "galaxybuds") {
                        // Galaxy Buds (the SongateOS session's request, 2026-11-19; the user chose B, step buttons: each tap one change, as
                        // the Buds4 Pro drop the connection when settings come too fast): Off, Cancelling, Ambient; Cancelling's
                        // strength, Ambient's volume; Conversation detection and how soon it goes back; noise control with one bud
                        static const char* gxKey[3] = { "off", "anc", "ambient" };
                        static const char* gxName[3] = { "Off", "Cancelling", "Ambient" };
                        std::vector<std::string> opts;
                        std::vector<int> idx;
                        int cur = 0;
                        for (int i = 0; i < 3; i++)
                            if (eb.modes.empty() || std::find(eb.modes.begin(), eb.modes.end(), gxKey[i]) != eb.modes.end()) {
                                if (eb.mode == gxKey[i]) cur = int(opts.size());
                                opts.push_back(gxName[i]);
                                idx.push_back(i);
                            }
                        hp.push_back(segRow("Noise control", "", "sys.eb.mode", opts, cur, [idx](int i) { sysapi::earbudsSet({ { "mode", gxKey[idx[size_t(i)]] } }); }));
                        if (eb.mode == "anc") {   // 0 Low … 2 High (their own default)
                            const int top = eb.ancLevelMax > 0 ? eb.ancLevelMax : 2;
                            if (top == 2)
                                hp.push_back(segRow("Cancelling strength", "", "sys.eb.gxanc", { "Low", "Medium", "High" }, std::clamp(eb.ancLevel, 0, 2),
                                                    [](int i) { sysapi::earbudsSet({ { "anc_level", std::to_string(i) } }); }));
                        } else if (eb.mode == "ambient" && eb.ambientLevel >= 0) {   // 0 … 4, shown 1 … 5
                            const int top = eb.ambientLevelMax > 0 ? eb.ambientLevelMax : 4;
                            std::vector<std::string> steps;
                            for (int i = 0; i <= top; i++) steps.push_back(std::to_string(i + 1));
                            hp.push_back(segRow("Ambient volume", "", "sys.eb.gxamb", steps, std::clamp(eb.ambientLevel, 0, top),
                                                [](int i) { sysapi::earbudsSet({ { "ambient_level", std::to_string(i) } }); }));
                        }
                        if (!eb.conversation.empty()) {
                            const bool conv = eb.conversation == "1";
                            hp.push_back(Row{ "Conversation detection", "", "Switches to Ambient sound while you talk", 40, [this, conv](float rx, float cy) {
                                                  if (toggle(uid("sys.eb.conv"), rx - 40, cy, conv)) sysapi::earbudsSet({ { "conversation", conv ? "0" : "1" } });
                                              } });
                            if (conv) {
                                static const int secs[3] = { 5, 10, 15 };
                                int t = 1;
                                for (int i = 0; i < 3; i++) if (eb.conversationTimeout == secs[i]) t = i;
                                hp.push_back(segRow("Back after", "", "sys.eb.gxconvt", { "5 s", "10 s", "15 s" }, t,
                                                    [](int i) { sysapi::earbudsSet({ { "conversation_timeout", std::to_string(secs[i]) } }); }));
                            }
                        }
                        if (!eb.oneBud.empty())
                            hp.push_back(Row{ "Noise control with one earbud", "", "Cancelling and Ambient with only one earbud in", 40,
                                              [this, one = eb.oneBud == "1"](float rx, float cy) {
                                                  if (toggle(uid("sys.eb.gxone"), rx - 40, cy, one)) sysapi::earbudsSet({ { "one_bud", one ? "0" : "1" } });
                                              } });
                    } else if (eb.kind == "airpods") {
                        // AirPods (the user approved the mock-up, 2026-11-19): their four noise modes and Conversation awareness; no
                        // wind or cancelling options (theirs are the earbuds' own)
                        static const char* apKey[4] = { "off", "anc", "transparency", "adaptive" };
                        static const char* apName[4] = { "Off", "Cancelling", "Transparency", "Adaptive" };
                        std::vector<std::string> opts;
                        std::vector<int> idx;
                        int cur = 0;
                        for (int i = 0; i < 4; i++)
                            if (eb.modes.empty() || std::find(eb.modes.begin(), eb.modes.end(), apKey[i]) != eb.modes.end()) {
                                if (eb.mode == apKey[i]) cur = int(opts.size());
                                opts.push_back(apName[i]);
                                idx.push_back(i);
                            }
                        hp.push_back(segRow("Noise control", "", "sys.eb.mode", opts, cur, [idx](int i) { sysapi::earbudsSet({ { "mode", apKey[idx[size_t(i)]] } }); }));
                        // Adaptive's strength (the user, 2026-11-19, as an iPhone's Customise Adaptive Audio): only in Adaptive; sent
                        // while dragged at most every 0.3 s and on release (each change takes the earbuds up to 1.5 s)
                        if (eb.mode == "adaptive" && eb.adaptiveLevel >= 0) {
                            const int shown = int(std::lround(apLevelShown(eb.adaptiveLevel) * 100));
                            hp.push_back(Row{ "Adaptive strength", std::to_string(shown) + "%", "", 180, [this, lvl = eb.adaptiveLevel](float rx, float cy) {
                                const Id id = uid("sys.eb.adaptive");
                                const float was = apLevelShown(lvl);
                                float nv = was;
                                // its ends as Noise control's own icons beside it (the R4's test, 2026-11-19: 0 lets in the most of the
                                // surroundings, 100 cancels the most; the user chose A, icons): the ear left, the shield right
                                const float sw = setSliderW();
                                g.icon(ICON_EAR, rx - sw + 10, cy, 18, pal.text3);
                                g.icon(ICON_ANC, rx - 10, cy, 18, pal.text3);
                                slider(id, rx - sw + 32, cy, sw - 64, was, &nv);
                                const bool held = ui.sliderDrag(id) >= 0 || ui.pressed(id);
                                if (nv != was) apLevelDrag_ = std::clamp(nv, 0.f, 1.f);
                                if (apLevelDrag_ < 0) return;
                                const int want = int(std::lround(apLevelDrag_ * 100));
                                const bool release = !held || ui.sliderReleased(id);
                                if (want != apLevelSent_ && (release || time - apLevelSentAt_ >= 0.3)) {
                                    sysapi::earbudsSet({ { "adaptive_level", std::to_string(want) } });
                                    apLevelSent_ = want;
                                    apLevelSentAt_ = time;
                                }
                                if (release) {   // (kept on screen until the earbuds say the same: apLevelShown)
                                    apLevelHold_ = want;
                                    apLevelHoldAt_ = time;
                                    apLevelAnim_ = apLevelDrag_;
                                    apLevelDrag_ = -1;
                                    apLevelSent_ = -1;
                                }
                            } });
                        }
                        if (!eb.conversation.empty())
                            hp.push_back(Row{ "Conversation awareness", "", "Lowers the music and lets voices through when you start speaking", 40,
                                              [this, conv = eb.conversation == "1"](float rx, float cy) {
                                                  if (toggle(uid("sys.eb.conv"), rx - 40, cy, conv)) sysapi::earbudsSet({ { "conversation", conv ? "0" : "1" } });
                                              } });
                    } else {
                    static const char* modeKey[3] = { "noise_cancelling", "transparency", "normal" };
                    static const char* modeName[3] = { "Cancelling", "Transparency", "Normal" };
                    std::vector<std::string> opts;
                    std::vector<int> idx;
                    int cur = 0;
                    for (int i = 0; i < 3; i++)
                        if (eb.modes.empty() || std::find(eb.modes.begin(), eb.modes.end(), modeKey[i]) != eb.modes.end()) {
                            if (eb.mode == modeKey[i]) cur = int(opts.size());
                            opts.push_back(modeName[i]);
                            idx.push_back(i);
                        }
                    hp.push_back(segRow("Noise control", "", "sys.eb.mode", opts, cur, [idx](int i) { sysapi::earbudsSet({ { "mode", modeKey[idx[size_t(i)]] } }); }));
                    if (eb.mode == "noise_cancelling") {
                        static const char* ancKey[3] = { "adaptive", "manual", "transport" };
                        const int a = eb.anc == "manual" ? 1 : eb.anc == "transport" ? 2 : 0;
                        hp.push_back(segRow("Cancelling", "", "sys.eb.anc", { "Adaptive", "Manual", "Transport" }, a,
                                            [](int i) { sysapi::earbudsSet({ { "anc", ancKey[i] } }); }));
                        if (a == 1)
                            hp.push_back(segRow("Strength", "", "sys.eb.level", { "1", "2", "3", "4", "5" }, std::clamp(eb.ancLevel, 1, 5) - 1,
                                                [](int i) { sysapi::earbudsSet({ { "anc_level", std::to_string(i + 1) } }); }));
                        if (a == 2) {
                            static const char* trKey[4] = { "plane", "train", "bus", "car" };
                            int t = 0;
                            for (int i = 0; i < 4; i++) if (eb.transport == trKey[i]) t = i;
                            hp.push_back(segRow("Transport", "", "sys.eb.transport", { "Plane", "Train", "Bus", "Car" }, t,
                                                [](int i) { sysapi::earbudsSet({ { "transport", trKey[i] } }); }));
                        }
                    } else if (eb.mode == "transparency")
                        hp.push_back(segRow("Transparency", "", "sys.eb.trans", { "Full", "Voices" }, eb.transparency == "vocal" ? 1 : 0,
                                            [](int i) { sysapi::earbudsSet({ { "transparency", i ? "vocal" : "full" } }); }));
                    hp.push_back(Row{ "Wind reduction", "", "Less wind noise outdoors", 40, [this, wind = eb.wind](float rx, float cy) {
                                         if (toggle(uid("sys.eb.wind"), rx - 40, cy, wind)) sysapi::earbudsSet({ { "wind", wind ? "0" : "1" } });
                                     } });
                    }
                }
                const sysapi::Codec cd = sysapi::codec();
                if (on && sysapi::has("bt_codec") && cd.known && !cd.available.empty()) {
                    std::vector<std::string> opts;
                    int cur = 0;
                    for (size_t i = 0; i < cd.available.size(); i++) {
                        if (cd.available[i] == cd.codec) cur = int(i);
                        opts.push_back(codecName(cd.available[i]));
                    }
                    char rate[24] = "";
                    if (cd.rate > 0) {
                        if (cd.rate % 1000 == 0) snprintf(rate, sizeof rate, "%d kHz", cd.rate / 1000);
                        else snprintf(rate, sizeof rate, "%.1f kHz", cd.rate / 1000.0);
                    }
                    const std::string hint = cd.rate > 0 && cd.bits > 0 ? std::string(rate) + " · " + std::to_string(cd.bits) + "-bit: the best both sides support"
                                                                        : std::string();
                    hp.push_back(segRow("Codec", hint, "sys.bt.codec", opts, cur, [av = cd.available](int i) {
                        sysapi::act("bt_codec_set", "", { { "codec", av[size_t(i)] } }, "bt");
                    }));
                }
                if (on && !hp.empty()) {
                    // (its name as the card's title, the maker's word first left off: "Soundcore Liberty 4 NC" → "Liberty 4 NC" when
                    // the earbuds say what kind they are)
                    std::string title = eb.supported && !eb.name.empty() ? eb.name : on->name;
                    if (eb.supported && !eb.kind.empty() && eb.kind != "airpods" && title.size() > eb.kind.size() + 1 &&
                        lowerAscii(title.substr(0, eb.kind.size())) == lowerAscii(eb.kind) && title[eb.kind.size()] == ' ')
                        title = title.substr(eb.kind.size() + 1);
                    bt.push_back({ title, hp });
                }
                sysBudsShown_ = on != nullptr;
                if (!paired.empty()) bt.push_back({ "Paired devices", paired });
                found.insert(found.begin(), buttonRow("Pair a new device", b.discovering ? "Looking for devices nearby…" : "Put your headphones in pairing mode first",
                                                      "sys.bt.look", nullptr, b.discovering ? "Stop" : "Look",
                                                      [on = b.discovering] { sysapi::act("bt_discover", on ? "stop" : "start", {}, "bt"); }, false, btCol2));
                bt.push_back({ "New devices", found });
            }
        }
        // the page's top: the pill (the user, 2026-11-16: "a pill toggle to switch between wifi and bluetooth settings")
        Row pill;
        pill.blockH = 44;
        pill.block = [this](float bx, float by, float bw) {
            int p = -1;
            segmented(uid("sys.conntab"), bx + bw, by + 22, { "Wi-Fi", "Bluetooth" }, connTab_, &p, 0, bw, 2, 40);
            if (p >= 0 && p != connTab_) { connTab_ = p; area("settings.pane").target = area("settings.pane").y = 0; }
        };
        std::vector<Card> conn = { { "", { pill } } };
        const std::vector<Card>& half = connTab_ == 0 ? wifi : bt;
        conn.insert(conn.end(), half.begin(), half.end());
        sections.push_back({ "Connectivity", ICON_WIFI, conn });
        // (no System page: the user, 2026-11-16: Android's own power menu (the power button held) restarts and powers off, and
        // SongateOS's updates are in Settings › Updates, beside Songate's)
        r4Pages += 1;
    }
#endif
    if (!settingsWant_.empty()) {   // (a page asked for by its name: the R4's tour)
        for (int i = 0; i < int(sections.size()); i++)
            if (settingsWant_ == sections[size_t(i)].name) settingsCat = i;
        settingsWant_.clear();
    }
    settingsCat = std::clamp(settingsCat, 0, int(sections.size()) - 1);
    // the list's order: the HiBy R4's pages (kept last) shown where the user put them: Status LED right after Appearance,
    // Sound & DAC right after Equalizer
    std::vector<int> listed;
    for (int i = 0; i < int(sections.size()) - r4Pages; i++) listed.push_back(i);
    for (int k = int(sections.size()) - r4Pages; k < int(sections.size()); k++) {
        const char* nm = sections[size_t(k)].name;
        // (SongateOS's pages: Connectivity first, as a phone's settings have Wi-Fi and Bluetooth; System last)
        if (!strcmp(nm, "Connectivity")) { listed.insert(listed.begin(), k); continue; }
        const int after = !strcmp(nm, "Status LED") ? int(SetAppearance) : int(SetEqualizer);
        const auto at = std::find(listed.begin(), listed.end(), after);
        listed.insert(at == listed.end() ? listed.end() : at + 1, k);
    }
#ifdef GF_ANDROID
    // (a phone's Controls: empty, touch only; not listed either. Its section stays, so the categories keep their numbers)
    for (int k = 0; k < int(listed.size()); k++)
        if (!strcmp(sections[size_t(listed[size_t(k)])].name, "Controls") && sections[size_t(listed[size_t(k)])].cards.empty()) {
            if (settingsCat == listed[size_t(k)]) settingsCat = SetAppearance;
            listed.erase(listed.begin() + k);
            break;
        }
    if (r4Dev_)   // the R4's Home screen page is not listed (it is empty there)
        for (int k = 0; k < int(listed.size()); k++)
            if (!strcmp(sections[size_t(listed[size_t(k)])].name, "Home screen")) {
                if (settingsCat == listed[size_t(k)]) settingsCat = SetAppearance;
                listed.erase(listed.begin() + k);
                break;
            }
#endif
#ifdef GF_ANDROID
    // SongateOS's areas watched while their page shows (the service's notifications, no polling); Wi-Fi scanned when its page
    // opens, then at most every 10 s; a search for devices stopped when its page goes (App::sysTick lets them go off Settings)
    if (r4Dev_) {
        const bool connOpen = setupPane_ ? !strcmp(setupPane_->section, "Connectivity")
                                         : settingsQuery.empty() && !strcmp(sections[size_t(settingsCat)].name, "Connectivity") &&
                                               (w >= kNarrowList || settingsPhoneOpen_);
        sysapi::watch("wifi", connOpen && connTab_ == 0);
        sysapi::watch("bt", connOpen && connTab_ == 1);
        if (connOpen && connTab_ == 0 && sysapi::has("wifi") && sysapi::wifi().enabled) sysapi::scanIfDue();
        sysBtShown_ = connOpen && connTab_ == 1;
        if (setupPane_) sysPage_ = setupPane_->section;   // (the first-boot setup's page: its areas as that page's)
        else if (settingsQuery.empty() && (w >= kNarrowList || settingsPhoneOpen_)) sysPage_ = sections[size_t(settingsCat)].name;   // (App::sysTick)
        // SongateOS's updates: watched while Updates shows them (the service notifies each state, once a second while downloading)
        // (SongateOS's updater: watched always, App::sysTick)
    }
#endif
    // the R4's pages read again when they open, and Sound & DAC's status (what plays, the rate) each second while it is open
    if (r4::on() && settingsCat != settingsCatSeen_) r4::refresh();
    if (!strcmp(sections[size_t(settingsCat)].name, "Sound & DAC") && time - r4PageReadAt_ > 1) {
        r4PageReadAt_ = time;
        r4::refresh();
    }
    // Pages that read the SD card do it once when they open: Storage and reset's sizes, Updates' backups
    if (settingsCat != settingsCatSeen_) {
        settingsCatSeen_ = settingsCat;
        if (!strcmp(sections[size_t(settingsCat)].name, "Support")) supportScan();   // its reads, on a worker
        if (settingsCat == SetStorage && time - storage.at > 60) storage.at = -100;   // sizes kept a minute (a Clear measures again)
        if (settingsCat == SetReset) backupsAt = -100;
    }

    // ------------------------------------------------------------ narrow (a phone upright): the categories as a list of their own,
    // each a large row with one line of what is in it; a category's page takes the whole width (back returns to the list)
    const bool narrow = w < kNarrowList;
    if (narrow && !settingsPhoneOpen_ && settingsQuery.empty() && !setupPane_) {
        static const std::pair<const char*, const char*> what[] = {
            { "Appearance", "Colours, size, the sidebar, screen edges" }, { "Playback", "Order, crossfade, levelling, sleep" },
            { "Equalizer", "Ten bands, bass, treble, presets" },          { "Library", "The music folder, rescan, covers" },
            { "Storage", "Where browsers open, what is kept" },          { "Lyrics", "Where they come from, their timing" },
            { "Canvas", "Its layouts and what it shows" },               { "Visualizer", "Bars, colours, motion" },
            { "Controls", "Buttons, the pointer" },                       { "Devices", "Pairing, listening together, AirPlay" },
            { "Reset", "Settings, or everything" },
            { "Updates", "New versions, betas, going back" },             { "Support", "Reports and what they hold" },
            { "About", "Songate, its credits, the tour" }, { "Home screen", "Songate as the home screen, the device's apps" },
            { "Sound & DAC", "Gain, filter, balance, the volume" }, { "Status LED", "Colour, alerts, the back light" },
            { "Connectivity", "Wi-Fi, Bluetooth" } };   // (the last three: the HiBy R4 only)
        ScrollArea& la = area("settings.phone");
        const float rowH = 66, lx = x + 18, lw = w - 36;
        la.view = { x, y, w, h };
        la.content = 24 + 16 + 50 + 44 + 16 + listed.size() * rowH + 24;
        ui.pushArea(&la);
        float cy = y + 24 - la.y;
        kicker("Songate", lx, cy + 6, pal.text3, 0.14f);
        cy += 16;
        g.text("Settings", { css::Xxl, Weight::ExtraBold }, lx - 1, cy, pal.text);
        cy += 50;
#ifdef GF_ANDROID
        {   // what the sidebar carries above Settings (no sidebar upright): the work's cards, a newer version's banner (view_phone.cpp)
            const float used = phoneWorkCard(lx, cy, lw);
            cy += used;
            la.content += used;
        }
#endif
        const Id sid = uid("set.find");
        if (ui.item(sid, { lx, cy, lw, 44 }, 12)) openFindSetting();
        g.roundRect(lx, cy, lw, 44, 12, pal.text, ui.pressed(sid) ? 0.09f : 0.05f);
        g.roundStroke(lx, cy, lw, 44, 12, 1, pal.text, 0.12f);
        g.icon(ICON_SEARCH, lx + 22, cy + 22, 18, pal.text3);
        g.textC("Find a setting", { css::Base }, lx + 42, cy + 22, pal.text3);
        typingMark("settings", lx, cy, lw, 44, 12, lx + 42, std::string(), { css::Base }, lw - 60);
        cy += 44 + 16;
        g.roundRect(lx - 8, cy, lw + 16, listed.size() * rowH, 16, pal.text, 0.04f);
        for (int k = 0; k < int(listed.size()); k++, cy += rowH) {
            const int i = listed[size_t(k)];
            const Id id = uid("set.cat", i);
            if (ui.item(id, { lx - 8, cy, lw + 16, rowH }, 12)) {
                settingsCat = i;
                settingsPhoneOpen_ = true;
                settingsPhoneDirect_ = false;
                area("settings.pane").target = area("settings.pane").y = 0;
            }
            if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(lx - 8, cy, lw + 16, rowH, 12, pal.text, 0.06f);
            const float ccy = cy + rowH / 2;
            g.roundRect(lx + 2, ccy - 20, 40, 40, 12, pal.text, 0.065f);
            g.icon(sections[i].icon, lx + 22, ccy, 19, pal.text2);
            const char* line = "";
            for (const auto& [n, l] : what) if (!strcmp(n, sections[i].name)) line = l;
            if (r4Dev_ && !strcmp(sections[i].name, "Controls")) line = "The hold switch";   // (the R4: nothing else there)
#ifdef GF_ANDROID
            else if (!strcmp(sections[i].name, "Controls")) line = "The keyboard";   // (a phone: touch only, the keyboard's card alone)
#endif
            g.text(sections[i].name, { css::Md, Weight::SemiBold }, lx + 58, ccy - (*line ? 21 : 10), pal.text);
            if (*line) g.text(line, { css::Sm }, lx + 58, ccy + 2, pal.text3, 1, Align::Left, int(lw - 58 - 30));
            g.icon(ICON_CHEV, lx + lw - 8, ccy, 14, pal.text3);
            if (!strcmp(sections[i].name, "Updates") && osUpdateMark()) g.circle(lx + lw - 30, ccy, 4, pal.accent);   // (SongateOS has one)
            if (k + 1 < int(listed.size())) g.rect(lx + 58, cy + rowH - 1, lw - 58, 1, pal.text, 0.07f);
        }
        ui.popArea();
        settingsPaneX = 1e9f;
        return;
    }

    // ------------------------------------------------------------ left column (not when narrow: the list above is it)
    const float lx = x + 24, lw = narrow ? 0.f : 214.f;
    float cy = y + 24;
    if (!narrow && !setupPane_) {
    kicker("Songate", lx, cy + 6, pal.text3, 0.14f);
    cy += 16;
    g.text("Settings", { css::Xxl, Weight::ExtraBold }, lx - 1, cy, pal.text);
    cy += 50;
    // .st-search: opens the console keyboard; what it finds replaces the category on the right
    {
        const Id sid = uid("set.find");
        if (ui.item(sid, { lx, cy, lw, 38 }, 10)) openFindSetting();
        const bool hot = ui.focused(sid) && ui.showFocusRing();
        g.roundRect(lx, cy, lw, 38, 10, pal.text, hot ? 0.09f : 0.05f);
        g.roundStroke(lx, cy, lw, 38, 10, 1, pal.text, 0.12f);
        g.icon(ICON_SEARCH, lx + 20, cy + 19, 16, pal.text3);
        g.textC(settingsQuery.empty() ? "Find a setting" : settingsQuery, { css::Base }, lx + 38, cy + 19, settingsQuery.empty() ? pal.text3 : pal.text,
                1, Align::Left, int(lw - 50));
        typingMark("settings", lx, cy, lw, 38, 10, lx + 38, settingsQuery, { css::Base }, lw - 50);
        cy += 38 + 12;
    }
    // Eleven categories and more sit a little closer; past what fits (Support, a dev build's Debug: About went under the
    // player bar, the user, 2026-10-30) the list scrolls, the focused category kept in view (Ui, as every scrolled list)
    const float step = listed.size() > 10 ? 36 : 40, catH = step - 2;
    ScrollArea& ca = area("settings.cats");
    const float listTop = cy;
    ca.view = { lx - 8, listTop, lw + 16, y + h - listTop };
    ca.content = listed.size() * step + 8;
    ca.clamp();
    ui.pushArea(&ca);
    g.clip(lx - 8, listTop, lw + 16, y + h - listTop);
    cy -= ca.y;
    for (int k = 0; k < int(listed.size()); k++) {
        const int i = listed[size_t(k)];
        const Id id = uid("set.cat", i);
        const bool on = settingsQuery.empty() && settingsCat == i;
        if (ui.item(id, { lx, cy, lw, catH }, 10, on ? DefaultFocus : 0)) {
            settingsCat = i;
            settingsQuery.clear();
            area("settings.pane").target = area("settings.pane").y = 0;
            settingsFocusFirst = true;   // the page's first setting takes the focus (the user: as an artist's page does)
        }
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (on) g.roundRect(lx, cy, lw, catH, 10, pal.accent, 0.14f);
        else if (hot) g.roundRect(lx, cy, lw, catH, 10, pal.text, 0.065f);
        g.icon(sections[i].icon, lx + 12 + 9, cy + catH / 2, 18, on ? pal.accentInk : pal.text2);
        g.textC(sections[i].name, { css::Base, Weight::SemiBold }, lx + 12 + 18 + 12, cy + catH / 2, on ? pal.accentInk : hot ? pal.text : pal.text2, 1,
                Align::Left, int(lw - 50));
        if (!strcmp(sections[i].name, "Updates") && osUpdateMark()) g.circle(lx + lw - 16, cy + catH / 2, 4, pal.accent);   // (SongateOS has one)
        cy += step;
    }
    g.unclip();
    ui.popArea();
    }

    // ------------------------------------------------------------ right pane (narrow: the whole width)
#if defined(GF_WINDOWED) && !defined(GF_ANDROID)
    // (a very wide window: a row's name and its switch no further apart than kPaneMax, beside the list, as on any page this wide)
    constexpr float kPaneMax = 1160;
    const float px = setupPane_ ? x : narrow ? x + 18 : lx + lw + 28, pw = setupPane_ ? w : std::min(kPaneMax, x + w - (narrow ? 18 : 24) - px);
#else
    const float px = setupPane_ ? x : narrow ? x + 18 : lx + lw + 28, pw = setupPane_ ? w : x + w - (narrow ? 18 : 24) - px;
#endif
    settingsPaneX = px - 8;
    ScrollArea& sa = area("settings.pane");
    sa.view = { px - 8, y, x + w - (px - 8), h };
    ui.pushArea(&sa);
    g.clip(px - 8, y, x + w - (px - 8), h);
    float py = (setupPane_ ? y + 4 : y + 22) - sa.y;

    // What to show: the category, or every row the search matches, grouped under its category.
    std::vector<Card> cards;
    std::string title = sections[settingsCat].name;
    const char* icon = sections[settingsCat].icon;
    if (!settingsQuery.empty()) {
        const std::string q = lowerAscii(settingsQuery);
        for (const auto& s : sections) {
            Card hit{ s.name, {} };
            for (const auto& c : s.cards)
                for (const auto& r : c.rows)
                    if (!r.block && lowerAscii(r.label + " " + r.hint + " " + c.kicker + " " + s.name).find(q) != std::string::npos)
                        hit.rows.push_back(r);
            if (!hit.rows.empty()) cards.push_back(hit);
        }
        title = "“" + settingsQuery + "”";
        icon = ICON_SEARCH;
    } else if (setupPane_) {
        // the first-boot setup's screen (view_setup.cpp): a page's cards by their titles, or its rows by their labels in one card
        const SetupPane& sp = *setupPane_;
        for (const auto& s : sections) {
            if (strcmp(s.name, sp.section)) continue;
            Card merged{ sp.kicker, {} };
            for (size_t ci = 0; ci < s.cards.size(); ci++) {
                const Card& c = s.cards[ci];
                if (sp.skipFirst && ci == 0) continue;
                if (!sp.labels.empty()) {
                    for (const std::string& want : sp.labels)
                        for (const Row& r : c.rows) if (r.label == want) merged.rows.push_back(r);
                } else if (sp.cards.empty() || std::find(sp.cards.begin(), sp.cards.end(), c.kicker) != sp.cards.end()) {
                    cards.push_back(c);
                }
            }
            if (!sp.labels.empty()) {   // (in the order the screen names them)
                std::vector<Row> ordered;
                for (const std::string& want : sp.labels)
                    for (const Row& r : merged.rows) if (r.label == want) { ordered.push_back(r); break; }
                merged.rows = ordered;
                if (!merged.rows.empty()) cards.push_back(merged);
            }
        }
    } else {
        cards = sections[settingsCat].cards;
    }

    // .st-head: a 44 px tile with the icon, the title beside it (not on a setup screen: it has its own)
    if (!setupPane_) {
    g.roundRect(px, py, 44, 44, 12, pal.accent, 0.16f);
    g.icon(icon, px + 22, py + 22, 22, pal.accentInk);
    g.textC(title, { 30, Weight::ExtraBold }, px + 44 + 16, py + 22, pal.text, 1, Align::Left, int(pw - 60));
    if (!settingsQuery.empty() && skb_.open && !strcmp(skb_.field, "settings") && std::fmod(time - skb_.at, 1.06) < 0.62)   // typing: the caret in the title
        g.rect(px + 44 + 16 + std::min(g.measureF(title, { 30, Weight::ExtraBold }), pw - 60) + 2, py + 22 - 15, 2, 30, pal.text);
    py += 44 + 20;
    }
    if (cards.empty() && !setupPane_) {
        g.text("Nothing matches. Press the search field to try other words.", { css::Base }, px, py + 8, pal.text3, 1, Align::Left, int(pw));
        py += 40;
    }

    // .st-group cards
    const float pad = 20, innerW = pw - pad * 2;
    settingsRows.clear();
    {
        static int settling = -1;   // the row let go, still on its way to its place: drawn over the others until there
        if (tabHeld >= 0) settling = tabHeld;
        const float k = store.motion ? std::exp(-dt / 0.07f) : 0.f;
        for (auto& [tab, off] : tabSlide) {
            if (tab == tabHeld) continue;
            off *= k;
            if (std::fabs(off) < 0.3f) off = 0;
        }
        if (settling >= 0 && tabHeld < 0 && tabSlide[settling] == 0) settling = -1;
        tabFloat_ = settling;
    }
    // Folded cards (the user chose B, 2026-11-16: "Appearance for example has so many settings"): on a page with three titled
    // cards or more, one is open at a time; a tap on a title opens that card and folds the one before (again on it: all folded).
    // Each page keeps its open card (Store::foldOpen); a jump to a setting (Find a setting, showSetting) opens its card. Find a
    // setting's results and the untitled cards (a page's head, its pill) are never folded.
    int titled = 0;
    for (const Card& c : cards) titled += c.kicker.empty() ? 0 : 1;
    // Android only: the Switch and the PC show every card open (the user, 2026-11-18: folding is for a phone's small screen and
    // its touch only)
#ifdef GF_ANDROID
    const bool folding = settingsQuery.empty() && titled >= 3 && !setupPane_ && tourStep < 0;   // (the tour: every card open, its rows lit)
#else
    const bool folding = false;
    (void)titled;
#endif
    std::string openCard;
    if (folding) {
        const auto it = store.foldOpen.find(title);
        if (it != store.foldOpen.end()) openCard = it->second;
        // (a page seen first: its first; also a card kept open that is no longer there, renamed or merged: its first. Empty stays:
        // all folded on purpose)
        // A card kept open that is missing for a moment (the R4's Updates page while SongateOS checks: its Root card came and went)
        // opens no other in its place: the first one opened there, the cards moved under the finger and a tap meant for Install
        // Magisk could land on Google apps' Remove (SongateOS's tester, 2026-11-22). It comes back open when it is there again;
        // gone for good (renamed, merged), the page shows them all folded until one is opened
        if (it == store.foldOpen.end())
            for (const Card& c : cards) if (!c.kicker.empty()) { openCard = c.kicker; break; }
        if (!settingsJump.empty())   // (a jump: the card that holds the row, open)
            for (const Card& c : cards)
                for (const Row& r : c.rows)
                    if (!c.kicker.empty() && r.label == settingsJump && openCard != c.kicker) {
                        openCard = c.kicker;
                        store.foldOpen[title] = openCard;
                    }
    }
    for (const Card& c : cards) {
        if (folding && !c.kicker.empty()) {
            const bool open = c.kicker == openCard || (!c.with.empty() && c.with == openCard);
            const Id hid = uid(("set.fold:" + title + "/" + c.kicker).c_str());
            // (open: its title's band is what a tap takes, and its focus ring is a round one around the chevron alone, clear of the
            // first row and the card's edge; the user's Switch, 2026-11-18: "outline clipping", then "just outline this". Folded:
            // the whole pill, ring and all)
            const SDL_FRect hr = open ? SDL_FRect{ px, py, pw, 42 } : SDL_FRect{ px, py, pw, 52 };
            if (open) ringAt_[hid] = { px + pw - pad - 8 - 16, py + 26 - 16, 32, 32 };
            else ringAt_.erase(hid);
            if (ui.item(hid, hr, 18)) {
                store.foldOpen[title] = open ? std::string() : c.kicker;
                persistSoon(1.f);
                if (!open) foldScrollTo_ = c.kicker;   // (opened: its title brought to the page's top next frame, as the card above folds)
            }
            if (open && foldScrollTo_ == c.kicker) {
                sa.target = py - y + sa.y - 12;
                sa.clamp();
                foldScrollTo_.clear();
            }
            if (!open) {
                const bool hot = ui.pressed(hid) || (ui.focused(hid) && ui.showFocusRing());
                g.roundRect(px, py, pw, 52, 18, pal.surface, 0.55f);
                if (hot) g.roundRect(px, py, pw, 52, 18, pal.text, 0.05f);
                g.roundStroke(px, py, pw, 52, 18, 1, pal.text, 0.09f);
                kicker(c.kicker, px + pad, py + 26, pal.text3, 0.12f);
                g.icon(ICON_CHEV, px + pw - pad - 8, py + 26, 16, pal.text3);
                py += 52 + 16;
                continue;
            }
        }
        // stacked: a narrow page (a phone upright) puts a wide control on a line of its own under its label and hint
        struct Laid { const Row* r; std::vector<std::string> hint; float h, labelW; bool stacked = false; std::vector<std::string> label; bool valueBelow = false; };
        std::vector<Laid> laid;
        float ch = c.kicker.empty() ? 6 : 42;
        for (const Row& r : c.rows) {
            Laid l{ &r, {}, 0, 0 };
            if (r.block) {
                l.h = r.blockH + 16;
                if (r.nOpts && segGrid(r.words, r.nOpts, innerW)) l.h += 42;   // (a block with a choice bar that takes two lines: a battery state)
            } else {
                // upright (narrow; the user, 2026-11-04: "Settings that have one button, that button will be in the right"): a slider or
                // a choice bar on a line of its own, the row's whole width; a button, a switch or a value stays on the right of its
                // text, the name taking two lines if it needs them; only one too wide to share the line goes under it, still right
                l.stacked = narrow && (r.fill || r.ctlW == 180.f || r.ctlW > innerW * 0.55f);
                const float sideW = innerW - (r.ctlW > 0 ? r.ctlW + 24 : 0);
                l.labelW = l.stacked ? innerW : sideW;
                l.label = narrow ? g.wrap(r.label, labelF, int(l.labelW), 2) : std::vector<std::string>{ r.label };
                if (l.label.empty()) l.label.push_back(r.label);
                l.valueBelow = narrow && !r.value.empty() && g.measure(l.label.back(), labelF) + 7 + g.measure(r.value, valueF) > l.labelW;
                if (!r.hint.empty()) l.hint = g.wrap(r.hint, hintF, int(l.labelW), narrow ? 8 : 4);
                const float labelH = (l.label.size() - 1 + (l.valueBelow ? 1 : 0)) * 24.f;
                const float textH = 21 + labelH + (l.hint.empty() ? 0 : 3 + l.hint.size() * 18.f);
                l.h = l.stacked ? textH + 28 + 40 : std::max(r.minH, textH + 28);
                if (l.stacked && r.fill && segGrid(r.words, r.nOpts, innerW)) l.h += 42;   // its choices on two rows
                if (l.stacked && r.rounds) {   // a colour row's rounds on as many lines as they need (roundAt)
                    l.h += float(roundsLayout(r.rounds, innerW, r.roundsLead).lines - 1) * 34;
                }
            }
            ch += l.h;
            laid.push_back(l);
        }
        ch += 6;
        // .st-group: surface at 55%, 1 px line, 18 px radius
        g.roundRect(px, py, pw, ch, 18, pal.surface, 0.55f);
        g.roundStroke(px, py, pw, ch, 18, 1, pal.text, 0.09f);
        float ry = py + (c.kicker.empty() ? 6 : 42);
        if (!c.kicker.empty()) kicker(c.kicker, px + pad, py + 26, pal.text3, 0.12f);
        if (folding && !c.kicker.empty()) g.icon(ICON_CHEV_DOWN, px + pw - pad - 8, py + 26, 16, pal.text3);   // (open: a tap on its title folds it)
        // one row's contents at ry (the loop below: in its place, or where it slides to)
        auto drawRow = [&](const Laid& l, float ry) {
            const Row& r = *l.r;
            // A row wholly out of the pane's view (a margin's worth): drawn as a ghost (Gfx::setGhost), so its controls still register
            // (the D-pad scrolls by moving the focus onto them) and its words are made ready, but nothing is drawn. Every row was
            // drawn every frame: Appearance took 11.8 ms of the console's frame and missed half of them (the user's clip, 2026-11-20)
            const bool wasGhost = g.ghost(), away = ry + l.h < y - 40 || ry > y + h + 40;
            if (away) g.setGhost(true);
            struct GhostBack { Gfx& g; bool was; ~GhostBack() { g.setGhost(was); } } ghostBack{ g, wasGhost };
            if (r.lifted) {   // held to be moved: lifted, the accent around it (the queue's Move)
                g.roundRect(px + 8, ry + 4, pw - 16, l.h - 8, 12, pal.accent, 0.14f);
                g.roundStroke(px + 8, ry + 4, pw - 16, l.h - 8, 12, 2, pal.accent, 0.95f);
            }
            // showSetting: for its first frames the page scrolls to the row and focuses its control; the row is lit
            // in the accent and fades over a second and a half.
            if (settingsQuery.empty() && !settingsJump.empty() && r.label == settingsJump) {
                const double since = time - settingsJumpAt;
                if (since < 0.15) {
                    sa.target = ry - y + sa.y - 90;
                    sa.clamp();
                    if (r.focus) ui.focus(r.focus);
                }
                const float lit = float(std::clamp(1.0 - (since - 0.5) / 1.2, 0.0, 1.0));
                if (lit > 0) g.roundRect(px + 8, ry + 4, pw - 16, l.h - 8, 12, pal.accent, 0.16f * lit);
                else settingsJump.clear();
            }
            settingsRows.push_back({ px + 8, ry + 4, pw - 16, l.h - 8 });   // as showSetting lights it
            {   // every row registers, even scrolled out of view: the D-pad scrolls by moving focus onto it
                if (r.block) {
                    r.block(px + pad, ry + 8, innerW);
                } else {
                    const float labelH = (l.label.size() - 1 + (l.valueBelow ? 1 : 0)) * 24.f;
                    const float textH = 21 + labelH + (l.hint.empty() ? 0 : 3 + l.hint.size() * 18.f);
                    float ty = l.stacked ? ry + 14 : ry + (l.h - textH) / 2;
                    int lw2 = 0;
                    for (size_t k = 0; k < l.label.size(); k++) {
                        lw2 = g.text(l.label[k], labelF, px + pad, ty, r.dim ? pal.text3 : pal.text, 1, Align::Left, int(l.labelW));
                        if (k + 1 < l.label.size()) ty += 24;
                    }
                    if (!r.value.empty()) {
                        if (l.valueBelow) { ty += 24; g.textNum(r.value, valueF, px + pad, ty, pal.text2); }
                        else g.textNum(r.value, valueF, px + pad + lw2 + 7, ty, pal.text2);
                    }
                    ty += 24;
                    for (const auto& hl : l.hint) { g.text(hl, hintF, px + pad, ty, pal.text3); ty += 18; }
                    // on its own line: a slider or a choice bar the row's whole width; anything else on the right, as always
                    settingsCtlMax_ = l.stacked ? innerW : 0;
                    const float crx = px + pw - pad;
                    const float gridDy = l.stacked && r.fill && segGrid(r.words, r.nOpts, innerW) ? 21.f : 0.f;
                    if (r.control) { r.control(crx, l.stacked ? ry + 14 + textH + 8 + 18 + gridDy : ry + l.h / 2); settingsCtlMax_ = 0; }
                    else ui.item(uid("set.row", int64_t(std::hash<std::string>{}(r.label))), { px + 8, ry + 4, pw - 16, l.h - 8 }, 10);
                }
            }
        };
        const Laid* floating = nullptr;
        float floatY = 0;
        for (size_t i = 0; i < laid.size(); i++) {
            const Laid& l = laid[i];
            const Row& r = *l.r;
            float slide = 0;
            if (r.dragKey >= 0) {
                tabRowH[r.dragKey] = l.h;
                const auto f = tabSlide.find(r.dragKey);
                if (f != tabSlide.end()) slide = f->second;
            }
            if (r.dragKey >= 0 && r.dragKey == tabFloat_) {
                floating = &l;
                floatY = ry + slide;
                ry += l.h;
                continue;
            }
            if (i > 0) g.rect(px + pad, ry + slide, innerW, 1, pal.text, 0.09f);
            drawRow(l, ry + slide);
            ry += l.h;
        }
        if (floating) {   // lifted off the card: a shadow under it, opaque, the accent round it
            const float fx = px + 8, fy = floatY + 4, fw = pw - 16, fh = floating->h - 8;
            sortFloatBox(fx, fy, fw, fh, 12);   // (one drawing with every list in sort mode)
            drawRow(*floating, floatY);
        }
        py += ch + 16;
    }
    sa.content = py + sa.y - y + 24;
    g.unclip();
    ui.popArea();
}


std::vector<int> App::sidebarTabsShown() const {
    std::vector<int> order;
    for (int idx = 0; idx < int(store.tabOrder.size()); idx++) {
        if (store.tabOrder[size_t(idx)] == Store::TabServer) continue;   // (Server is in Library now: Local | Server)
        if (store.tabOrder[size_t(idx)] == Store::TabApps && !appsTabShown()) continue;   // (Android's, with Show Apps on)
        order.push_back(idx);
    }
    return order;
}

bool App::osUpdates() const {
#ifdef GF_ANDROID
    return r4Dev_ && (sysapi::avail() == sysapi::Avail::Unknown || sysapi::has("ota"));
#else
    return false;
#endif
}

bool App::osUpdateMark() const {
#ifdef GF_ANDROID
    if (!r4Dev_ || !sysapi::has("ota")) return false;
    const sysapi::Ota o = sysapi::ota();
    const std::string& st = o.state;
    return o.known && (st == "available" || st == "downloading" || st == "verifying" || st == "ready" || st == "staging");
#else
    return false;
#endif
}

float App::mdBlock(const std::string& md, float x, float y, float w, bool draw) {
    // a line's words, each bold or not: **…** marks bold; [words](link) keeps the words; `code` its text
    struct Word { std::string s; bool bold; };
    auto words = [](const std::string& line) {
        std::vector<Word> out;
        std::string cur;
        bool bold = false;
        auto flush = [&] { if (!cur.empty()) { out.push_back({ cur, bold }); cur.clear(); } };
        for (size_t i = 0; i < line.size();) {
            if (line.compare(i, 2, "**") == 0 || line.compare(i, 2, "__") == 0) { flush(); bold = !bold; i += 2; continue; }
            if (line[i] == '`') { i++; continue; }
            if (line[i] == '[') {
                const size_t close = line.find("](", i);
                const size_t end = close == std::string::npos ? close : line.find(')', close);
                if (end != std::string::npos) {
                    for (char c : line.substr(i + 1, close - i - 1)) { if (c == ' ') flush(); else cur += c; }
                    i = end + 1;
                    continue;
                }
            }
            if (line[i] == ' ' || line[i] == '\t') { flush(); i++; continue; }
            cur += line[i++];
        }
        flush();
        return out;
    };
    const Font body{ css::Sm }, strong{ css::Sm, Weight::SemiBold };
    const float lineH = 20;
    float cy = y;
    bool first = true;
    // the words laid out from left in width, wrapped; drawn at the line's top
    auto flow = [&](const std::vector<Word>& ws, float left, float width, const Rgb& ink, Font plainF, Font boldF) {
        float px = left;
        const float space = float(g.measure(" ", plainF));
        for (const Word& wd : ws) {
            const Font f = wd.bold ? boldF : plainF;
            const float ww = float(g.measure(wd.s, f));
            if (px > left && px + ww > left + width) { px = left; cy += lineH; }
            if (draw) g.text(wd.s, f, px, cy, wd.bold ? pal.text : ink, 1, Align::Left, int(std::max(1.f, left + width - px)));
            px += ww + space;
        }
        cy += lineH;
    };
    size_t at = 0;
    bool gap = false;
    while (at <= md.size()) {
        size_t nl = md.find('\n', at);
        if (nl == std::string::npos) nl = md.size();
        std::string line = md.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t lead = line.find_first_not_of(" \t");
        if (lead == std::string::npos) { gap = !first; if (nl >= md.size()) break; continue; }
        const std::string t = line.substr(lead);
        if (t.rfind("<!--", 0) == 0) { if (nl >= md.size()) break; continue; }   // (a comment GitHub hides)
        if (t[0] == '#') {   // a heading: its words, larger, a little room above (not as the first thing)
            const size_t h = t.find_first_not_of('#');
            if (!first) cy += 8;
            const std::string text = h == std::string::npos ? std::string() : t.substr(h);
            const Font hf{ css::Md, Weight::Bold };
            std::vector<Word> ws = words(text);
            for (Word& wd : ws) wd.bold = false;
            const float savedH = lineH;
            (void)savedH;
            flow(ws, x, w, pal.text, hf, hf);
            cy += 4;
        } else if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {   // a bullet, its level by its indent
            if (gap) cy += 6;
            const float indent = std::min(2.f, float(lead / 2)) * 16;
            if (draw) g.circle(x + indent + 4, cy + lineH / 2, 2.5f, pal.accent);
            flow(words(t.substr(2)), x + indent + 16, w - indent - 16, pal.text2, body, strong);
        } else if (t.rfind("---", 0) == 0 || t.rfind("***", 0) == 0) {   // a rule
            if (draw) g.rect(x, cy + 8, w, 1, pal.text, 0.09f);
            cy += 16;
        } else {   // a paragraph's line (a table's too: plain text, its bars as they are)
            if (gap) cy += 8;
            flow(words(t), x, w, pal.text2, body, strong);
        }
        first = false;
        gap = false;
        if (nl >= md.size()) break;
    }
    return cy - y;
}

float App::apLevelShown(int lvl) {
    // the finger wins while it is on the slider; then the value let go on, until the earbuds' answer matches it (3 s at most)
    if (apLevelDrag_ >= 0) { apLevelAnim_ = apLevelDrag_; apLevelAnimAt_ = time; return apLevelDrag_; }
    if (apLevelHold_ >= 0 && (lvl == apLevelHold_ || time - apLevelHoldAt_ > 3)) apLevelHold_ = -1;
    const float target = (apLevelHold_ >= 0 ? apLevelHold_ : lvl) / 100.f;
    // a change from outside (a reconnect): the thumb eases there, ~0.2 s
    const double dt = std::clamp(time - apLevelAnimAt_, 0.0, 0.1);
    apLevelAnimAt_ = time;
    if (apLevelAnim_ < 0 || !store.motion) apLevelAnim_ = target;
    else apLevelAnim_ += (target - apLevelAnim_) * float(1 - std::exp(-dt / 0.06));
    if (std::fabs(apLevelAnim_ - target) < 0.002f) apLevelAnim_ = target;
    return apLevelAnim_;
}
