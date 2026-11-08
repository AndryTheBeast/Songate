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
#include <cmath>
#include <ctime>
#include <functional>
#include <map>

#include "app.hpp"
#include "icons.inc"
#include "plicons.inc"

#ifndef GF_VERSION
#define GF_VERSION "2.0.0 BETA 2"
#endif

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

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
};
// The sidebar's tabs dragged by touch (the user, 2026-11-04: "you can actually grab and bring it over to the desired sorting
// order"): the held row follows the finger; a row it passes slides into the place it left; let go, it settles into its own.
std::map<int, float> tabRowH;     // each tab row's height, last frame (what the finger must pass to change places)
std::map<int, float> tabSlide;    // each tab row's offset from its place, easing back to 0
int tabHeld = -1;                 // the tab under the finger

struct Card { std::string kicker; std::vector<Row> rows; };
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
    const float h = hgt, x0 = rx - w, ro = 12 * h / 36, ri = 9 * (h - 6) / 30;   // (32 high: the search box's corners)
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
    if (into >= 0) name = store.eqUser[size_t(into)].name;   // updated where it is, under its name
    else if (!keyboard("Name the preset", name, name)) return;
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
    std::string name = store.eqUser[size_t(k)].name;
    if (!keyboard("Rename the preset", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    for (char& c : name) if (c == '\t') c = ' ';
    if (name.empty()) return;
    snprintf(store.eqUser[size_t(k)].name, sizeof store.eqUser[size_t(k)].name, "%s", name.c_str());
    store.saveEqPresets();
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
void App::dragSort(std::vector<int>& order, int base, const char* key, int i, float rx, float cy, const std::function<void()>& after) {
    const int n = int(order.size());
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
        while (at + 1 < n && dy - anchorY > heightOf(base + order[size_t(at + 1)]) * 0.5f) {
            const int other = base + order[size_t(at + 1)];
            anchorY += heightOf(other);
            tabSlide[other] += myH;
            std::swap(order[size_t(at)], order[size_t(at + 1)]);
            at++;
            if (after) after();
        }
        while (at > 0 && anchorY - dy > heightOf(base + order[size_t(at - 1)]) * 0.5f) {
            const int other = base + order[size_t(at - 1)];
            anchorY -= heightOf(other);
            tabSlide[other] -= myH;
            std::swap(order[size_t(at)], order[size_t(at - 1)]);
            at--;
            if (after) after();
        }
        // under the finger, but not far past the first or the last place
        float off = dy - anchorY;
        if (at == 0) off = std::max(off, -10.f);
        if (at == n - 1) off = std::min(off, 10.f);
        tabSlide[me] = off;
        tabHeld = me;   // drawn floating, with its shadow (App::drawSettings)
    } else if (dragging == hid) {
        dragging = 0;
        tabHeld = -1;   // it settles from where the finger left it (tabSlide eases to 0)
        persistSoon(0.3f);
    }
    const bool hot = dy >= 0 || (ui.focused(hid) && ui.showFocusRing());
    if (hot) g.roundRect(x - 44, cy - 20, 44, 40, 10, pal.text, 0.08f);
    g.icon(ICON_MENU, x - 22, cy, 20, hot ? pal.text : pal.text2);
}

void App::drawSettings(float x, float y, float w, float h) {
    const Font labelF{ css::Md, Weight::SemiBold }, hintF{ css::Sm }, valueF{ css::Md };
    // A category chosen last frame: its page was drawn then, so its first setting is known now
    if (settingsFocusFirst) {
        settingsFocusFirst = false;
        if (const Id first = ui.firstIn(&area("settings.pane"))) ui.focus(first);
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
    struct Btn { const char* key; const char* icon; std::string text; std::function<void()> act; bool danger = false; };
    auto buttonsRow = [&](const std::string& label, const std::string& hint, std::vector<Btn> btns) {
        float total = 0;
        std::vector<float> ws;
        for (const Btn& b : btns) {
            ws.push_back(g.measure(b.text, { css::Sm, Weight::SemiBold }) + 32 + (b.icon ? 24 : 0));
            total += ws.back() + (total > 0 ? 8 : 0);
        }
        Row r{ label, "", hint, total, [this, btns, ws, total](float rx, float cy) {
                  float bx = rx - total;
                  for (size_t i = 0; i < btns.size(); i++) {
                      bool c = false;
                      button(uid(btns[i].key), btns[i].icon, btns[i].text, bx, cy - 17, false, &c, false, btns[i].danger);
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
    static float satShown, trShown;   // Saturation 0..100% and Colour transition 0..3 s, as the slider's 0..1
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
        const int sortWay = css::W < kNarrowList || byTouch_ ? 1 : 0;
        const float sortW = 40 + 12 + 44;
        auto sortCtl = [this](int i, int, float rx, float cy) { dragSort(store.tabOrder, 0, "set.tabdrag", i, rx, cy, nullptr); };
        for (int i = 0; i < int(store.tabOrder.size()); i++) {
            const int tab = store.tabOrder[size_t(i)];
            if (tab == Store::TabServer) continue;   // (in Library now: Local | Server)
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
                           if (c && !byTouch_) {
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
                             : tab == Store::TabFiles  ? "The whole " GF_CARD_NAME ": open, rename, copy, move and delete, and what fits each file"
                                                       : "";
            Row r{ Store::tabName(tab), "", hint, sortWay ? sortW : 40 + 16 + mw, [this, tab, bit, i, mw, sortWay, sortCtl](float rx, float cy) {
                      if (bit) {
                          const bool shown = !(store.hiddenTabs & bit);
                          if (toggle(uid("set.tab", int(bit)), rx - 40, cy, shown)) {
                              store.hiddenTabs ^= bit;
                              persistSoon(0.3f);
                          }
                      }
                      if (sortWay) { sortCtl(i, tab, rx, cy); return; }
                      bool c = false;
                      button(uid("set.tabmove", tab), ICON_SORT_DOWN, "Move", rx - 40 - 16 - mw, cy - 17, false, &c);
                      if (c && !byTouch_) {
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
                if (cw < 110) {   // a narrow tile (a phone upright): its name centred, smaller, on two lines if it needs them
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
                return Row{ label, "", hint, cw, [this, label, key, field, choices, shown](float rx, float cy) {
                               const int n = int(choices.size()) + 1;
                               for (int i = 0; i < int(choices.size()); i++) {
                                   const float sx = rx - (n - i) * 30;
                                   const Id id = uid(key, i);
                                   if (ui.item(id, { sx, cy - 13, 26, 26 }, 13)) { *field = choices[i]; applyTheme(); persistSoon(0.3f); }
                                   const int v = choices[i];
                                   if (v < 0) {
                                       g.circle(sx + 13, cy, 12, pal.text, 0.1f);
                                       g.textC("A", { css::Xs, Weight::Bold }, sx + 13, cy, pal.text2, 1, Align::Center);
                                   } else {
                                       g.circle(sx + 13, cy, 12, Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f });
                                   }
                                   const bool on = *field == v, hot = ui.focused(id) && ui.showFocusRing();
                                   if (on || hot) g.roundStroke(sx - 2, cy - 15, 30, 30, 15, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.4f);
                               }
                               // the picker: a colour wheel, or your own colour in the middle of it
                               const float sx = rx - 30;
                               const Id id = uid(key, 99);
                               if (ui.item(id, { sx, cy - 13, 26, 26 }, 13))
                                   openColourPicker(field, label, *field >= 0 ? Rgb{ ((*field >> 16) & 255) / 255.f, ((*field >> 8) & 255) / 255.f, (*field & 255) / 255.f } : shown);
                               const bool own = *field >= 0 && std::find(choices.begin(), choices.end(), *field) == choices.end();
                               if (SDL_Texture* wheel = wheelTexture()) g.imageStretched(wheel, sx + 1, cy - 12, 24, 24, 0);
                               if (own) {
                                   g.circle(sx + 13, cy, 8.5f, { 0, 0, 0 }, 0.35f);
                                   g.circle(sx + 13, cy, 7, Rgb{ ((*field >> 16) & 255) / 255.f, ((*field >> 8) & 255) / 255.f, (*field & 255) / 255.f });
                               }
                               const bool hot = ui.focused(id) && ui.showFocusRing();
                               if (own || hot) g.roundStroke(sx - 2, cy - 15, 30, 30, 15, own ? 2.f : 1.f, own ? pal.accent : pal.text, own ? 1.f : 0.4f);
                           } };
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
        if (!customRows.empty()) cards.push_back({ "Custom", customRows });   // only while Custom is chosen
        cards.push_back({ "Layout",
                          { toggleRow("Collapse the sidebar", "Icons only, more room for everything else", "set.side", store.sidebarCollapsed),
                            toggleRow("Background motion", "The slowly drifting colours behind every view and the Canvas", "set.motion",
                                      store.motion) } });
#ifdef GF_ANDROID
        // Size, sideways only (the user, 2026-11-04: "in portrait the default is standard but there will not be options to choose. in
        // landscape yes"): upright is always Standard, its row not shown there
        if (css::W >= css::H)
            cards.back().rows.insert(cards.back().rows.begin(),
                                     segRow("Size", store.uiSize == 0 ? "As on the Switch and the PC" : store.uiSize == 1 ? "A little larger, for a phone held close"
                                                                                                           : "Larger still: less fits on each page",
                                            "set.uisize", { "Standard", "Medium", "Largest" }, store.uiSize, [this](int i) {
                                                store.uiSize = i;
                                                store.save();
                                                fitPending_ = true;   // laid out again between frames
                                            }));
        // The camera's cutouts and the rounded corners, whatever the phone or tablet has (the user, 2026-11-03: "works on every android
        // or tablet ... give users lots of customisation options"): App::updateEdges
        {
            static const char* const edgeHints[] = {
                "Only what a cutout would cover moves, by just enough",
                "Nothing is placed on a cutout's side; Songate's colours still reach the edge",
                "A black band on a cutout's side, as Android draws one",
                "Nothing moves: a camera may cover a corner of the bars" };
            const int extraPx = int(std::lround(store.edgeExtra * 24));
            cards.push_back({ "Screen edges",
                              { segRow("Around the camera", edgeHints[std::clamp(store.edgeMode, 0, 3)], "set.edgemode",
                                       { "Step aside", "Clear side", "Black strip", "Whole screen" }, store.edgeMode,
                                       [this](int i) { store.edgeMode = i; store.save(); }),
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
        {
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
                                   "set.mixnow", { "Full", "Immersive", "Simple" }, store.mixAnimNow, [this](int i) { store.mixAnimNow = i; store.save(); }),
                            segRow("On the Canvas",
                                   store.mixAnimCanvas == 0 ? "The cover glides to the middle, frosted with the hint"
                                                            : "The page changes from one song to the next, left to right",
                                   "set.mixcanvas", { "Full", "Simple" }, store.mixAnimCanvas, [this](int i) { store.mixAnimCanvas = i; store.save(); }) };
        if (store.mixAnimNow == 2)
            mixRows.push_back(segRow("The hint on Now playing",
                                     store.mixHint == 1 ? "On the cover; on the Lyrics tab, in the lyrics' place"
                                                        : "Above the cover; on the Lyrics tab, in the lyrics' place",
                                     "set.mixhint", { "Automatic", "On the cover", "Above" }, store.mixHint, [this](int i) { store.mixHint = i; store.save(); }));
        if (css::W >= css::H) cards.push_back({ "Automix", mixRows });   // upright: Full only, nothing to choose (App::mixCanvasMode)
        cards.push_back({ "Sidebar", sidebarRows });
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
            const bool sortWay = css::W < kNarrowList || byTouch_;
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
            for (int i = 0; i < int(vis.size()); i++) {
                const int it = vis[size_t(i)];
                const bool held = grab.kind == Grab::Battery && grab.pos == i;
                const bool noRoom = batteryRoom_ > 0 && !*batteryShown(it) && used + (used > 0 ? sep : 0) + float(g.measure(widest[it], tf)) > batteryRoom_;
                Row r{ names[it], "", held && !sortWay ? GF_MOVING_WORDS : noRoom ? "No room beside the battery: turn another off first" : hints[it],
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
                           if (c && !byTouch_) {
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
            if (battery.infoOk)
                top.push_back(segRow("Temperature in", "", "set.batunit", { "°C", "°F" }, store.batteryFahrenheit ? 1 : 0,
                                     [this](int i) { store.batteryFahrenheit = i == 1; persistSoon(0.3f); }));
            cards.push_back({ "Battery", top });

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
                    if (ui.item(id, { sx, scy - 13, 26, 26 }, 13)) { pick(c); persistSoon(0.3f); }
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
                    segmented(uid("set.batlook", st), bx + bw, narrowSet ? by + 108 : by + 68, opts, cur, &p, 0, bw, 1);
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
            cards.push_back({ "Battery states", states });
        }
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
                                   "Music keeps playing after you close Songate from the recent apps. Off: it stops",
                                   "set.swipe", store.swipeKeep));
#elif defined(GF_WINDOWED)
        bgRows.push_back(toggleRow("Keep playing when the window closes", "Songate stays by the clock, playing; its icon brings it back. Off: closing the window quits",
                                   "set.bg", store.background));
#else
        const bool on = player.remote();
        bgRows.push_back(Row{ "Keep playing outside Songate", "",
                              on ? "Connected. The music carries on on the HOME menu, in games and after Songate closes"
                              : store.background ? "Not installed: copy atmosphere/contents/4200000047464E58 to " GF_THE_CARD " and restart"
                                                 : "Off: the music stops when you leave Songate",
                              40, [this](float rx, float cy) {
                                  if (toggle(uid("set.bg"), rx - 40, cy, store.background && player.remote())) setBackground(!player.remote());
                              } });
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
                                                                          : "Each carries on at the track and time it was left at",
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
                                       stopped ? "Stopped. Only the track playing gets one until you resume"
                                       : left > 0 ? "Being made in the background while Songate is open"
                                                  : "Made for each queue sent to the player, or for the whole library here",
                                       "set.bgcovers", stopped ? ICON_PLAY : left > 0 ? ICON_X : ICON_REFRESH,
                                       stopped ? "Resume" : left > 0 ? "Stop" : "Make all", [this, stopped, left] {
                                           if (stopped) { setJobStopped(Stoppable::Thumbs, false); return; }
                                           if (left > 0) { setJobStopped(Stoppable::Thumbs, true); return; }
                                           for (int t = 0; t < int(lib.tracks.size()); t++) thumbFor(t, false);
                                           toast("Making them in the background: the card above Settings shows how far it is");
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
            store.levelMode == 0 ? "Starts once levelling is on, from the track playing"
            : store.stopLevels       ? "Stopped. Only the track playing and the next ones are measured until you resume"
            : levels.secondsPerTrack() > 0 ? "In the background while Songate is open, about " + oneDecimal(levels.secondsPerTrack()) + " s a track"
                                     : "In the background while Songate is open, the track playing first";
        Row measuredRow = store.levelMode && (store.stopLevels || measured < lib.tracks.size())
                              ? buttonRow("Measured", measuredHint, "set.lvlstop", store.stopLevels ? ICON_PLAY : ICON_X,
                                          store.stopLevels ? "Resume" : "Stop",
                                          [this] { setJobStopped(Stoppable::Levels, !store.stopLevels); })
                              : infoRow("Measured", measuredHint);
        measuredRow.value = std::to_string(measured) + " of " + std::to_string(lib.tracks.size());
        static const std::vector<std::string> orders = { "Default", "Repeat list", "Repeat track", "Shuffle" };
        sections.push_back(
            { "Playback", ICON_PLAY,
              { { "", { segRow("Order", automixQueue() ? "An Automix playlist keeps its own order: no Shuffle"
                                                    : "Also the first button in the player bar", "set.order", orders, int(store.order),
                               [this](int i) { setOrder(Order(i)); }, automixQueue() ? 1u << 3 : 0u),
                        Row{ "Stop after the current track", "", "Then playback stops until you start it again", 40,
                             [this](float rx, float cy) {
                                 if (toggle(uid("set.sac"), rx - 40, cy, stopAfterCurrent)) stopAfterCurrent = !stopAfterCurrent;
                             } },
                        Row{ "Volume", pct(volumeShown), "", 180,
                             [this](float rx, float cy) {
                                 float v = volumeShown;
                                 slider(uid("set.vol"), rx - setSliderW(), cy, setSliderW(), volumeShown, &v);
                                 if (v != volumeShown) { volumeShown = v; player.setVolume(v); }
                             } } } },
                { "Volume levelling",
                  { segRow("Volume levelling", "Every track about as loud as the next. Your files are not changed",
                           "set.level", { "Off", "Track", "Album", "Auto" }, store.levelMode,
                           [this](int i) { store.levelMode = i; persistSoon(0.3f); }),
                    segRow("Target loudness", "Quiet tracks are raised only as far as their peaks allow", "set.lvltarget",
                           { "−18", "−14", "−11 LUFS" }, store.levelTarget <= -16 ? 0 : store.levelTarget <= -12 ? 1 : 2,
                           [this](int i) {
                               static const int t[] = { -18, -14, -11 };
                               store.levelTarget = t[i];
                               persistSoon(0.3f);
                           },
                           store.levelMode ? 0u : 0x7u),
                    measuredRow } },
                { "Crossfade",
                  { segRow("Crossfade", "The end of a track fades into the next one. Automix playlists mix their own",

                           "set.xfade", { "Off", "2", "4", "6", "8", "12 s" },
                           store.crossfade <= 0 ? 0 : store.crossfade <= 2 ? 1 : store.crossfade <= 4 ? 2 : store.crossfade <= 6 ? 3
                                                    : store.crossfade <= 8 ? 4 : 5,
                           [this](int i) {
                               static const float secs[] = { 0, 2, 4, 6, 8, 12 };
                               store.crossfade = secs[i];
                           }),
                    toggleRow("Not within an album", "An album's tracks in a row keep their gapless join"
                                                     "",
                              "set.xfadealbum", store.xfadeNotInAlbum) } },
                // What plays when Songate opens; the opening itself is in Appearance (the user, 2026-11-03)
                { "Startup",
                  { toggleRow("Resume where I left off", "The queue comes back paused at the same place in the same track", "set.resume",
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
                    segRow("Dim the screen", store.keepAwake ? "After this long without input; any button brings it back"
                                                             : "With Keep " GF_THE_CONSOLE " awake on; otherwise " GF_THE_CONSOLE " dims by itself",
                           "set.dim", { "Off", "1", "2", "5", "10 min" },
                           store.dimAfter <= 0 ? 0 : store.dimAfter <= 1 ? 1 : store.dimAfter <= 2 ? 2 : store.dimAfter <= 5 ? 3 : 4,
                           [this](int i) {
                               static const int mins[] = { 0, 1, 2, 5, 10 };
                               store.dimAfter = mins[i];
                               persistSoon(0.3f);
                           },
                           store.keepAwake ? 0u : 0x1Fu) } },
                { GF_BG_CARD, bgRows } } });
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
        auto dbRow = [&](const std::string& label, const std::string& hint, const char* key, float GfbgEq::*field) {
            return Row{ label, dbText(store.eq.*field), hint, 180, [this, key, field](float rx, float cy) {
                           const float cur = (store.eq.*field + 12) / 24;
                           float nv = cur;
                           slider(uid(key), rx - setSliderW(), cy, setSliderW(), cur, &nv);
                           if (nv != cur) {
                               GfbgEq e = store.eq;
                               e.*field = std::round((nv * 24 - 12) * 2) / 2;
                               if (e.preset >= GFEQ_PRESETS) gfEqKeepCustom(&e);   // your preset keeps these too: now an edit of it
                               setEq(e);
                           }
                       } };
        };
        const bool adv = store.eq.advanced != 0;
        const int own = store.eq.preset - GFEQ_PRESETS;   // ≥ 0: one of your saved presets
        const bool edited = own >= 0 && store.eq.edited;  // ... changed since it was saved
        const std::string preset = eqPresetName(store.eq.preset) + (edited ? " (edited)" : "");
        Row onRow{ "Equalizer", "", GF_EQ_WHAT " Strong boosts lower the level a little to avoid distortion",
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
        presetRow.value = "";
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
            bands.blockH = 214;
            bands.block = [this](float bx, float by, float bw) {
                static const char* names[GFEQ_BANDS] = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
                const float col = bw / GFEQ_BANDS, top = by + 30, hh = 150;
                for (int i = 0; i < GFEQ_BANDS; i++) {
                    const float cx = bx + col * (i + 0.5f);
                    const Id id = uid("set.eqband", i);
                    const float cur = (store.eq.bands[i] + 12) / 24;
                    float nv = cur;
                    vslider(id, cx, top, hh, cur, &nv, i > 0 ? uid("set.eqband", i - 1) : 0, i + 1 < GFEQ_BANDS ? uid("set.eqband", i + 1) : 0);
                    const float v = store.eq.bands[i];
                    char b[8];
                    snprintf(b, sizeof b, std::fabs(v) < 0.05f ? "0" : "%+.0f", double(v));
                    g.textC(b, { css::Xs, Weight::SemiBold }, cx, by + 12, std::fabs(v) < 0.05f ? pal.text3 : pal.accentInk, 1, Align::Center);
                    g.textC(names[i], { css::Xs }, cx, top + hh + 18, pal.text3, 1, Align::Center);
                    if (nv != cur) {
                        GfbgEq e = store.eq;
                        e.bands[i] = std::round(nv * 24 - 12);
                        gfEqKeepCustom(&e);   // moved by hand: Custom, kept for the preset sheet
                        setEq(e);
                    }
                }
            };
            cards.push_back({ "Bands", { bands, dbRow("Preamp", "Everything louder or quieter; a limiter holds the peaks", "set.eqpre",
                                                      &GfbgEq::preamp) } });
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
                                      navigate(View::PickFolder, -1, plat::parentDir(store.musicRoot));
                                  }),
                        infoRow("Tracks", "Albums and artists come from the tags, however the folders are organised",
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
                if (sv.songs >= 0) rows.push_back(infoRow("Tracks on it", "", std::to_string(sv.songs)));
                rows.push_back(segRow("Its music", store.serverPlace == 0 ? "Apart, in Library › Server"
                                                                          : "Among your own albums, artists and genres, marked as the server's; also in Library › Server",
                                      "set.subplace", { "Apart", "Mixed in" }, store.serverPlace, [this](int p) { store.serverPlace = p; }));
                {   // Downloads (stage 4; the user: "User's choice"): where, and what is here
                    const bool noMusic = store.musicRoot.empty();
                    rows.push_back(segRow("Downloads", store.serverDlPlace == 1 && !noMusic ? "In your music folder, under Songate downloads: they become your own tracks"
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
                                           : working ? "Albums you downloaded, one track at a time"
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
                                   toggleRow("Timing control", "Earlier · 0.0 s · Later above synced lyrics, kept for each track",
                                             "set.lyrtime", store.lyricTiming),
                                   segRow("Show it for", "Where the lyrics come from", "set.lyrtimefor", { "Tags", ".lrc files", "Both" },
                                          store.lyricTimingFor, [this](int i) { store.lyricTimingFor = i; }, store.lyricTiming ? 0u : 0x7u),
                                   infoRow("Where lyrics come from",
                                           "An .lrc file named as the track, or its LYRICS tag. " GF_CAP_TAP " a synced line to jump there"),
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
    // Online (BETA 3, lyrics_online.cpp): off until turned on; what is sent, said where it is turned on
    sections.back().cards.push_back(
        { "Online",
          { toggleRow("Find lyrics online", "When a song with no lyrics plays, LRCLIB (lrclib.net) is asked for them: its title, artist, album and length are sent, nothing else",
                      "set.lyronline", store.lyricsOnline),
            segRow("When found", "Play them at once (Keep or Not these), or show them on a card first (Save or Not these)", "set.lyrway", { "Play them", "Ask first" },
                   store.lyricsOnlineWay, [this](int i) { store.lyricsOnlineWay = i; }, store.lyricsOnline ? 0u : 0x3u),
            segRow("Save them",
                   store.lyricsSaveTo == 0   ? "An .lrc beside the song: other players see it too"
                   : store.lyricsSaveTo == 2 ? "Written inside the song file, once it is not playing (the rest of the file as it was)"
                                             : "Kept in Songate's folder: your music folder is not touched",
                   "set.lyrsave", { "Beside the song", "Inside the song", "In Songate's folder" }, store.lyricsSaveTo == 2 ? 1 : store.lyricsSaveTo == 1 ? 2 : 0,
                   [this](int i) { store.lyricsSaveTo = i == 1 ? 2 : i == 2 ? 1 : 0; }, store.lyricsOnline ? 0u : 0x7u) } });

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
                               canvasLayoutsOff() == 0b011 ? "This track has no lyrics: Minimal until one that has them"
                                                           : "Also at the bottom of the Canvas",
                               "set.cvlayout", layouts, canvasLayoutShown(), [this](int i) { store.canvasLayout = i; }, canvasLayoutsOff()),
                        toggleRow("Minimal when there are no lyrics", "Your layout comes back with the next track that has lyrics",
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
                              "set.seekwave", store.seekWave) } },
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
        const std::pair<const char*, const char*> map[] = { { "Hold a track", "Its menu: tags, cover, queue, delete" },
                                                            { "Hold a button", "What it does" },
                                                            { "Enter  ·  pad A", "Select" },           { "Esc  ·  pad B", "Back" },
                                                            { "Space  ·  pad X", "Play / pause" },    { "Ctrl F  ·  pad Y", "Search" },
                                                            { "Ctrl ← →  ·  LB RB", "Previous / next track" },
                                                            { "Shift ← →  ·  LT RT", "Seek 10 seconds" },
                                                            { "C  ·  View", "Canvas (full screen)" },
                                                            { "Arrows  ·  D-pad", "Move around" } };
#elif defined(GF_WINDOWED)
        const std::pair<const char*, const char*> map[] = { { "Enter  ·  pad A", "Select" },           { "Esc  ·  pad B", "Back" },
                                                            { "Space  ·  pad X", "Play / pause" },    { "Ctrl F  ·  pad Y", "Search" },
                                                            { "Ctrl ← →  ·  LB RB", "Previous / next track" },
                                                            { "Shift ← →  ·  LT RT", "Seek 10 seconds" },
                                                            { "C  ·  View", "Canvas (full screen)" },
                                                            { "F11  ·  Alt Enter", "Full screen" },
                                                            { "Mouse to the top left corner", "The window's close, minimise and full screen buttons" },
                                                            { "Right-click  ·  Menu", "Track menu: tags, cover, queue, delete" },
                                                            { "Arrows  ·  D-pad", "Move around" },
                                                            { "Wheel  ·  right stick", "Scroll" } };
#else
        const std::pair<const char*, const char*> map[] = { { "A", "Select" },           { "B", "Back" },        { "X", "Play / pause" },
                                                            { "Y", "Search" },           { "L  R", "Previous / next track" },
                                                            { "ZL  ZR", "Seek 10 seconds" }, { "−", "Canvas (full screen)" },
                                                            { "+", "Track menu: tags, cover, queue, delete" },
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
                          { "Cover picker opens in", "The track's own folder", &store.coverDir },
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
                          { "Covers for the overlay", "What the overlay shows for each track (thumbs)" },
                          { "Loudness measurements", "Each track's levelling, Automix and mood measurements" },
                          { "Tag cache", "The tags read from your files, so opening is quick" },
                          { "Backups", "Your settings as they were before each install or restore" },
                          { "Update downloads", GF_DOWNLOADS_WORDS },
                          { "Lyrics found online", "Kept in Songate's folder (Settings › Lyrics › Online). Those beside the songs stay" },
                          { "Tracks kept from the server", "The server's tracks played lately, so they start at once (Subsonic server)" },
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
    sections.push_back({ "Storage", ICON_FOLDER,
                         { { "Where browsers open", folderRows },
                           { "Files", { toggleRow("Show hidden files", "Names that start with a dot, in Files", "set.hidden", store.showHidden) } },
                           { "Kept " GF_ON_CARD, keptRows } } });
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
                                                : "Off: Songate never goes online. On: new versions from GitHub, and going back",
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
            top.push_back(toggleRow("Check when Songate starts", "A newer version is announced once, at the bottom of the screen", "set.updstart",
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

    // Last.fm (lastfm.hpp, view_lastfm.cpp): off until allowed, as Updates; a tab of its own, after Devices (the user, 2026-10-31)
    {
        std::vector<Card> cards;
        const Lastfm::View lf = lastfm.view();
        std::vector<Row> top;
        if (Lastfm::available()) {
            Row onRow{ "Last.fm", "", store.lastfm ? "Songate tells Last.fm what you play, and nothing else"
                                                   : lf.signedIn ? "Off: nothing is sent, and you stay signed in"
                                                                 : "Off: nothing is sent. Turned on, Songate asks you first, then you sign in",
                       40, [this](float rx, float cy) {
                           if (toggle(uid("set.lfon"), rx - 40, cy, store.lastfm)) {
                               if (store.lastfm) {
                                   store.lastfm = false;
                                   lastfm.cancelSignIn();
                                   persistSoon(0.3f);
                                   toast("Last.fm off: nothing is sent");
                               } else {
                                   allowLastfm();
                               }
                           }
                       } };
            onRow.focus = uid("set.lfon");
            top.push_back(onRow);
        } else {
            top.push_back(infoRow("Last.fm cannot be used in this build", "This build was made without Songate's Last.fm key"));
        }
        cards.push_back({ "", top });
        if (Lastfm::available() && store.lastfm) {
            if (lf.sign == Lastfm::Sign::Asking) {
                cards.push_back({ "Sign in", { infoRow("Asking Last.fm…", "") } });
            } else if (lf.sign == Lastfm::Sign::Waiting) {
                std::vector<Row> in;
                const int left = std::max(1, int((lf.expires - int64_t(std::time(nullptr))) / 60));
                const std::string minutes = "It works for the next " + std::to_string(left) + (left == 1 ? " minute." : " minutes.");
#ifdef GF_WINDOWED
                if (lfmOpened != lf.url) {
                    lfmOpened = lf.url;
                    plat::openUrl(lf.url);
                }
                in.push_back(buttonRow("Allow Songate in your browser", "Last.fm opened there. Sign in, and allow Songate. This page moves on by itself once you have. " + minutes,
                                       "set.lfopen", ICON_REFRESH, "Open it again", [url = lf.url] { plat::openUrl(url); }));
#else
                Row qr{ "", "", "", 0, nullptr };
                qr.blockH = 288;
                qr.block = [this, minutes](float bx, float by, float) {
                    int side = 0;
                    SDL_Texture* t = lastfmCode(side);
                    constexpr float kMost = 268;
                    const float cell = side > 0 ? std::max(1.f, std::floor(kMost / float(side))) : 1, s = float(side) * cell;
                    if (t) g.image(t, bx + 12, by + 10, s, s, 0);
                    const float tx = bx + 12 + kMost + 32;
                    g.text("Scan this with your phone", { css::Lg, Weight::SemiBold }, tx, by + 40, pal.text, 1);
                    g.text("Last.fm opens there. Sign in, and allow Songate.", { css::Md }, tx, by + 84, pal.text2, 1);
                    g.text("This page moves on by itself once you have.", { css::Md }, tx, by + 112, pal.text2, 1);
                    g.text(minutes, { css::Sm }, tx, by + 156, pal.text3, 1);
                };
                in.push_back(qr);
#endif
                if (lfmShowUrl) in.push_back(infoRow("The address", lf.url));
                else in.push_back(buttonRow("Having trouble?", "Open the same page on any device", "set.lfurl", nullptr, "Show the address", [this] { lfmShowUrl = true; }));
                in.push_back(buttonRow("Stop signing in", "", "set.lfstop", ICON_X, "Cancel", [this] {
                    lastfm.cancelSignIn();
                    lfmShowUrl = false;
                }));
                cards.push_back({ "Sign in", in });
            } else if (!lf.signedIn) {
                const std::string hint = !lf.error.empty() ? "Last try: " + lf.error
                                       : lf.lapsed ? "Last.fm no longer takes this sign-in. What is waiting stays here until you sign in again"
                                                   : "On Last.fm's own page. Your password is never typed into Songate";
                cards.push_back({ "Account", { buttonRow(lf.lapsed ? "Sign in again" : "Sign in to Last.fm", hint, "set.lfsign", ICON_MUSIC, "Sign in", [this] {
                    lfmShowUrl = false;
                    lastfm.signIn();
                }) } });
            } else {
                cards.push_back({ "Account", { infoRow("Signed in as", "", lf.user),
                                               buttonRow("Sign out", "What is waiting stays here until you sign in again", "set.lfout", ICON_X, "Sign out", [this] {
                                                   lastfm.signOut();
                                                   toast("Signed out of Last.fm");
                                               }) } });
                cards.push_back({ "Sending", { toggleRow("Scrobbling", "Each tagged track, after half of it or 4 minutes", "set.lfscrob", store.lastfmScrobble),
                                               toggleRow("Now playing", "Shows the track on your Last.fm profile while it plays", "set.lfnow", store.lastfmNow) } });
            }
            if (lf.signedIn || lf.lapsed || lf.waiting > 0)
                cards.push_back({ "Waiting", { infoRow("Waiting to be sent", lf.status.empty() ? "Sent oldest first, as soon as Last.fm can be reached" : lf.status, std::to_string(lf.waiting)) } });
        }
        sections.push_back({ "Last.fm", ICON_MUSIC, cards });
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
                const std::string left = "Type this code on the other device. It works for " + std::to_string(cv.codeSeconds) + (cv.codeSeconds == 1 ? " more second, for one try." : " more seconds, for one try.");
                code.block = [this, shown, left](float bx, float by, float bw) {
                    g.text(shown, { 58, Weight::SemiBold }, bx + bw / 2, by + 16, pal.text, 1, Align::Center);
                    g.text(left, { css::Md }, bx + bw / 2, by + 100, pal.text2, 1, Align::Center);
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
            if (syncWaiting || sv.state == Syncer::State::Reading || sv.state == Syncer::State::Trading) syncNote = "Syncing…";   // no counters: each new number is a new text to draw
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
                if (p.linked) btns.push_back({ syncKeys[slot].c_str(), ICON_REFRESH, syncBusy ? "Syncing…" : "Sync now", [this, id = p.id, busy = syncBusy] { if (!busy) syncNow(id, false); } });
                btns.push_back({ forgetKeys[slot].c_str(), nullptr, "Forget", [this, id = p.id, name = p.name] {
                    connect.forget(id);
                    toast(name + " forgotten: pair again to link");
                } });
                paired.push_back(buttonsRow(p.name, syncNote.empty() ? state : syncNote, btns));
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
                        std::vector<Row> rows = { ar };
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
                                                    "track, else the join stays gapless",
                        player.remote() ? std::to_string(bg.state().heapFree / 1024) + " KB" : std::string("Not connected")) } } } });
    {   // Connections' first stage, measured on the console (identity.hpp): nothing runs it in a release yet
        const Identity::Progress ip = identity.progress();
        char hint[120];
        if (ip.running) snprintf(hint, sizeof hint, "%d of %d, %d of them read", ip.done, ip.total, ip.hashed);
        else if (ip.total) snprintf(hint, sizeof hint, "%d tracks, %d read, %d failed: %.2f ms each, %.1f s in all", ip.total, ip.hashed, ip.failed,
                                    ip.hashed ? ip.hashSeconds * 1000 / ip.hashed : 0.0, ip.seconds);
        else snprintf(hint, sizeof hint, "Reads what makes each track the same one on two devices, into identity.tsv");
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
                                      store.save();
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
            g.text("Songate for " GF_PLATFORM, { css::Xl, Weight::Bold }, bx + 76, by + 12, pal.text);
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
                    infoRow("FFmpeg", "Decoding in Songate's own player: AAC, ALAC, FLAC, MP3, Ogg Vorbis, Opus and WAV. LGPL 2.1"),
                    infoRow("SDL 2", "Graphics, sound, fonts and images: SDL, SDL_mixer, SDL_ttf, SDL_image. zlib licence"),
                    infoRow("libFLAC, mpg123, Opus, Tremor", "SDL_mixer's decoders, for a file FFmpeg cannot open. BSD and LGPL licences"),
                    infoRow("libcurl and zlib", "Updates: the connection to GitHub, over " GF_THE_CONSOLE "'s own secure connection. curl and zlib licences"),
                    infoRow("QR Code generator", "The Last.fm sign-in code. © Project Nayuki, MIT licence"),
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
    }
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
    // Devices and Last.fm after Controls: what you use above, the upkeep from Reset down (the user, 2026-10-31), as App::SetDevices and
    // SetLastfm number them
    for (const char* name : { "Devices", "Last.fm" }) {
        auto cn = std::find_if(sections.begin(), sections.end(), [&](const Section& s) { return std::string(s.name) == name; });
        if (cn != sections.end()) {
            Section s = std::move(*cn);
            sections.erase(cn);
            auto rs = std::find_if(sections.begin(), sections.end(), [](const Section& x) { return std::string(x.name) == "Reset"; });
            sections.insert(rs, std::move(s));
        }
    }
    settingsCat = std::clamp(settingsCat, 0, int(sections.size()) - 1);
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
    if (narrow && !settingsPhoneOpen_ && settingsQuery.empty()) {
        static const std::pair<const char*, const char*> what[] = {
            { "Appearance", "Colours, size, the sidebar, screen edges" }, { "Playback", "Order, crossfade, levelling, sleep" },
            { "Equalizer", "Ten bands, bass, treble, presets" },          { "Library", "The music folder, rescan, covers" },
            { "Storage", "Where browsers open, what is kept" },          { "Lyrics", "Where they come from, their timing" },
            { "Canvas", "Its layouts and what it shows" },               { "Visualizer", "Bars, colours, motion" },
            { "Controls", "Buttons, the pointer" },                       { "Devices", "Pairing, listening together, AirPlay" },
            { "Last.fm", "Scrobbles and now playing" },                   { "Reset", "Settings, or everything" },
            { "Updates", "New versions, betas, going back" },             { "Support", "Reports and what they hold" },
            { "About", "Songate, its credits, the tour" } };
        ScrollArea& la = area("settings.phone");
        const float rowH = 66, lx = x + 18, lw = w - 36;
        la.view = { x, y, w, h };
        la.content = 24 + 16 + 50 + 44 + 16 + sections.size() * rowH + 24;
        ui.pushArea(&la);
        float cy = y + 24 - la.y;
        kicker("Songate", lx, cy + 6, pal.text3, 0.14f);
        cy += 16;
        g.text("Settings", { css::Xxl, Weight::ExtraBold }, lx - 1, cy, pal.text);
        cy += 50;
        const Id sid = uid("set.find");
        if (ui.item(sid, { lx, cy, lw, 44 }, 12)) openFindSetting();
        g.roundRect(lx, cy, lw, 44, 12, pal.text, ui.pressed(sid) ? 0.09f : 0.05f);
        g.roundStroke(lx, cy, lw, 44, 12, 1, pal.text, 0.12f);
        g.icon(ICON_SEARCH, lx + 22, cy + 22, 18, pal.text3);
        g.textC("Find a setting", { css::Base }, lx + 42, cy + 22, pal.text3);
        cy += 44 + 16;
        g.roundRect(lx - 8, cy, lw + 16, sections.size() * rowH, 16, pal.text, 0.04f);
        for (int i = 0; i < int(sections.size()); i++, cy += rowH) {
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
            g.text(sections[i].name, { css::Md, Weight::SemiBold }, lx + 58, ccy - (*line ? 21 : 10), pal.text);
            if (*line) g.text(line, { css::Sm }, lx + 58, ccy + 2, pal.text3, 1, Align::Left, int(lw - 58 - 30));
            g.icon(ICON_CHEV, lx + lw - 8, ccy, 14, pal.text3);
            if (i + 1 < int(sections.size())) g.rect(lx + 58, cy + rowH - 1, lw - 58, 1, pal.text, 0.07f);
        }
        ui.popArea();
        settingsPaneX = 1e9f;
        return;
    }

    // ------------------------------------------------------------ left column (not when narrow: the list above is it)
    const float lx = x + 24, lw = narrow ? 0.f : 214.f;
    float cy = y + 24;
    if (!narrow) {
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
        cy += 38 + 12;
    }
    // Eleven categories and more sit a little closer; past what fits (Support, a dev build's Debug: About went under the
    // player bar, the user, 2026-10-30) the list scrolls, the focused category kept in view (Ui, as every scrolled list)
    const float step = sections.size() > 10 ? 36 : 40, catH = step - 2;
    ScrollArea& ca = area("settings.cats");
    const float listTop = cy;
    ca.view = { lx - 8, listTop, lw + 16, y + h - listTop };
    ca.content = sections.size() * step + 8;
    ca.clamp();
    ui.pushArea(&ca);
    g.clip(lx - 8, listTop, lw + 16, y + h - listTop);
    cy -= ca.y;
    for (int i = 0; i < int(sections.size()); i++) {
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
        cy += step;
    }
    g.unclip();
    ui.popArea();
    }

    // ------------------------------------------------------------ right pane (narrow: the whole width)
    const float px = narrow ? x + 18 : lx + lw + 28, pw = x + w - (narrow ? 18 : 24) - px;
    settingsPaneX = px - 8;
    ScrollArea& sa = area("settings.pane");
    sa.view = { px - 8, y, x + w - (px - 8), h };
    ui.pushArea(&sa);
    g.clip(px - 8, y, x + w - (px - 8), h);
    float py = y + 22 - sa.y;

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
    } else {
        cards = sections[settingsCat].cards;
    }

    // .st-head: a 44 px tile with the icon, the title beside it
    g.roundRect(px, py, 44, 44, 12, pal.accent, 0.16f);
    g.icon(icon, px + 22, py + 22, 22, pal.accentInk);
    g.textC(title, { 30, Weight::ExtraBold }, px + 44 + 16, py + 22, pal.text, 1, Align::Left, int(pw - 60));
    py += 44 + 20;
    if (cards.empty()) {
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
    for (const Card& c : cards) {
        // stacked: a narrow page (a phone upright) puts a wide control on a line of its own under its label and hint
        struct Laid { const Row* r; std::vector<std::string> hint; float h, labelW; bool stacked = false; std::vector<std::string> label; bool valueBelow = false; };
        std::vector<Laid> laid;
        float ch = c.kicker.empty() ? 6 : 42;
        for (const Row& r : c.rows) {
            Laid l{ &r, {}, 0, 0 };
            if (r.block) {
                l.h = r.blockH + 16;
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
        // one row's contents at ry (the loop below: in its place, or where it slides to)
        auto drawRow = [&](const Laid& l, float ry) {
            const Row& r = *l.r;
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
                        lw2 = g.text(l.label[k], labelF, px + pad, ty, pal.text, 1, Align::Left, int(l.labelW));
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
            g.glowAround(fx, fy + 6, fw, fh, 12, 18, 10, { 0, 0, 0 }, 0.45f);
            g.roundRect(fx, fy, fw, fh, 12, pal.bg, 1);
            g.roundRect(fx, fy, fw, fh, 12, pal.surface, 0.9f);
            drawRow(*floating, floatY);
        }
        py += ch + 16;
    }
    sa.content = py + sa.y - y + 24;
    g.unclip();
    ui.popArea();
}
