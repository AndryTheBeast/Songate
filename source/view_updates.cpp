/*
 * Gatefold NX · view_updates.cpp
 * Settings → Updates (its rows are in view_settings.cpp): asking before anything goes online, checking GitHub, a
 * release's changelog in the sheet (Sheet::Release) with Install / Go back, the question that says all an install
 * does, finishing it (the background player and Gatefold start again), and the settings backups. The work itself is
 * the Updater's (updater.hpp), on a worker.
 */
#include "words.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <strings.h>

#include "app.hpp"
#include "gfcfgdir.h"
#include "icons.inc"
#include "plicons.inc"

namespace {

// Two more in Gatefold's line style (plicons.inc's) for the changelog: a fix, and privacy
#define UPD_SVG R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">)SVG"
inline constexpr char UPD_WRENCH[] = UPD_SVG R"SVG(<path d="M14.7 6.3a4 4 0 0 0 5 5L21 10a5.5 5.5 0 0 1-7.3 6.4l-6.1 6.1a2.1 2.1 0 0 1-3-3l6.1-6.1A5.5 5.5 0 0 1 17 3l-1.3 1.3a4 4 0 0 0-1 2z"/></svg>)SVG";
inline constexpr char UPD_SHIELD[] = UPD_SVG R"SVG(<path d="M12 3l7.5 3v5.5c0 4.6-3.2 8.2-7.5 9.5-4.3-1.3-7.5-4.9-7.5-9.5V6z"/><path d="M9 12l2.2 2.2L15.5 10"/></svg>)SVG";

std::string megabytes(int64_t bytes) {
    char b[32];
    snprintf(b, sizeof b, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    return b;
}

/** "2026-10-27" → "27 October 2026". */
std::string longDate(const std::string& ymd) {
    static const char* months[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    int y = 0, m = 0, d = 0;
    if (sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return ymd;
    return std::to_string(d) + " " + months[m - 1] + " " + std::to_string(y);
}

}  // namespace

std::string App::backupRoot() const {
    // Never inside the settings folder (a settings.txt edited by hand): a backup would copy itself into itself
    const std::string cfg = Store::Dir;
    const bool inside = strncasecmp(store.backupDir.c_str(), cfg.c_str(), cfg.size()) == 0 &&
                        (store.backupDir.size() == cfg.size() || store.backupDir[cfg.size()] == '/');
    return store.backupDir.empty() || inside ? std::string(plat::backupDir()) : store.backupDir;
}

const std::vector<UpdBackup>& App::updateBackups() {
    if (backupsAt < -50) {   // listed once each time the page opens, and after an install or a Clear (not every few seconds)
        backupsAt = time;
        backupsShown = updater.backups(backupRoot());
#ifndef GF_WINDOWED
        // the old backups folder, if the move left it (its backups could not all be moved in): listed too, they restore as they are
        if (backupRoot() != GFCFG_OLD_BACKUPS)
            for (UpdBackup& b : updater.backups(GFCFG_OLD_BACKUPS)) backupsShown.push_back(std::move(b));
#endif
    }
    return backupsShown;
}

const char* App::changelogIcon(const std::string& name) const {
    if (name == "fix") return UPD_WRENCH;
    if (name == "shield") return UPD_SHIELD;
    if (name == "download") return ICON_DOWNLOAD;
    if (name == "palette") return ICON_SPARKLE;
    if (name == "volume") return ICON_VOL;
    if (name == "shuffle") return ICON_SHUFFLE;
    if (name == "info") return ICON_INFO;
    if (name == "search") return ICON_SEARCH;
    if (name == "image") return ICON_IMAGE;
    if (name == "refresh") return ICON_REFRESH;
    if (name == "check") return ICON_CHECK;
    if (name == "tag") return ICON_TAG;
    if (name == "x") return ICON_X;
    if (name == "play") return ICON_PLAY;
    if (name == "canvas") return ICON_CANVAS;
    if (name == "settings" || name == "gear") return ICON_GEAR;
    if (name == "quote" || name == "lyrics") return ICON_QUOTE;
    for (const auto& k : kPlaylistIcons) if (name == k.key) return k.svg;   // music, headphones, eq, folder, moon, …
    return nullptr;   // a dot
}

std::string App::updateStatus() const {
    const int64_t d = updater.done(), t = updater.total();
    switch (updater.phase()) {
    case Updater::Checking: return "Asking GitHub for the releases…";
    case Updater::Downloading:
        return "Downloading " + updater.working() + (t > 0 ? " · " + megabytes(d).substr(0, megabytes(d).size() - 3) + " of " + megabytes(t) : std::string());
    case Updater::Verifying: return "Checking the download" + (t > 0 ? " · " + std::to_string(int(d * 100 / std::max<int64_t>(1, t))) + "%" : std::string());
    case Updater::Unpacking: return "Unpacking · " + std::to_string(d) + " of " + std::to_string(t) + " files";
    case Updater::BackingUp: return "Backing up your settings";
    case Updater::Installing: return "Putting the new files in place";
    default: return {};
    }
}

/** Updates are off until allowed here: what going online means, said before anything does. */
void App::allowUpdates() {
    confirm(UPD_SHIELD, "Updates", "Connect to the internet?", "GitHub · AndryTheBeast/Songate", "Allow Songate to go online?",
            "Songate connects to GitHub only when you check for updates or install one, and at launch if you turn that on. "
            "GitHub sees " GF_YOUR_CONSOLE "'s IP address and Songate's version, nothing else. You can turn this off at any time.",
            "Allow", [this] {
                store.updates = 1;
                persistSoon(0.3f);
                toast("Updates allowed: Songate goes online only when you ask");
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
}

/** Beta versions are asked for first: what a beta is, its risks, and to report bugs (the user, 2026-10-29). */
void App::allowBetas() {
    confirm(ICON_BOLT, "Updates", "Beta versions", "New features, before they are released", "Try beta versions?",
            "Betas let you try new features early. They can be unstable and have bugs: something may not work, or may stop "
            "working, until the release. Please report the bugs you find: " ISSUES_URL ". Your settings are backed up before "
            "every install, and you can go back to a release from here.",
            "Turn on", [this] {
                store.betas = true;
                persistSoon(0.3f);
                toast("Beta versions on: listed apart in Settings › Updates");
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_BOLT;
    sheet.askWarn = "Beta versions can be unstable and have bugs. Please report them.";
}

/** The release the banner above Settings offers: the newest one above this version (betas only with Beta versions on), while
 *  Check when Songate starts is on, until it is answered (Later or Update), and not one already installed and waiting for a
 *  restart. From the last check's list, so it is there at once at launch, and again after the check. */
int App::updateOffer() const {
#if defined(GF_PREVIEW) || (defined(GF_WINDOWED) && defined(GF_DEV))   // (Android's dev build: test-update / test-jobs in its settings folder, platform_android.cpp; the PC's dev build: the same names in its environment)
    if (getenv("GF_FAKEUPDATE")) return updLater_ ? -1 : updater.newer(true);   // (the preview's made-up release: app.cpp)
#endif
    if (!store.updates || !store.updateAtStart || updLater_) return -1;
    const int n = updater.newer(store.betas);
    if (n < 0 || updater.installed.version == updater.releases[size_t(n)].version) return -1;
    return n;
}

/** The banner pressed: its sheet, made as a release's (App::openRelease). */
void App::openUpdateOffer(int rel) {
    if (rel < 0 || rel >= int(updater.releases.size())) return;
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::UpdateOffer;
    sheet.recipe = rel;
    sheet.name = updater.releases[size_t(rel)].version;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

/** Update or Later: either puts the banner away until Songate next starts. Update: the release's row in Settings › Updates, lit
 *  (as App::showSetting), where its Details has everything that changed and Install. */
void App::answerUpdateOffer(int rel, bool update) {
    closeSheet();
    updLater_ = true;
    if (!update || rel < 0 || rel >= int(updater.releases.size())) {
        toast("Offered again the next time Songate starts. Settings \u203a Updates has it meanwhile");
        return;
    }
    settingsCat = SetUpdates;
    settingsQuery.clear();
    settingsPhoneOpen_ = settingsPhoneDirect_ = true;
    navigate(View::Settings);
    settingsJump = Updater::releaseName(updater.releases[size_t(rel)].version);
    settingsJumpAt = time;
}

void App::updateOfferHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub) {
    const UpdRelease& r = updater.releases[size_t(sheet.recipe)];
    icon = ICON_DOWNLOAD;
    kick = std::string(r.beta ? "Beta \u00b7 " : "") + "Update available";
    title = Updater::releaseName(r.version);
    sub = "You have " + Updater::pretty(Updater::thisVersion()) + " \u00b7 " + longDate(r.date) + " \u00b7 " + megabytes(r.zipSize);
}

void App::updateOfferRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    if (sheet.recipe < 0 || sheet.recipe >= int(updater.releases.size()) || updater.releases[size_t(sheet.recipe)].version != sheet.name) {
        closeSheet();   // the list was read again meanwhile
        return;
    }
    const int i = sheet.recipe;
    const UpdRelease& r = updater.releases[size_t(i)];
    const float textW = cardW(600) - 48 - 48;   // as view_smart.cpp draws a Text row
    auto text = [&](const char* icon, const std::string& s) {
        Row t{ Row::Text, icon, "", s };
        t.h = g.wrap(s, { css::Sm }, int(textW), 12).size() * 19.f + 14;
        rows.push_back(t);
    };
    int all = 0;
    for (const UpdLog& sec : r.log) all += int(sec.items.size());
    // (the whole list's size said here, where the whole list is: the few under What's new are its top)
    rows.push_back({ Row::Action, ICON_DOWNLOAD, "Update",
                     all > 1 ? "Opens it in Settings \u203a Updates: all " + std::to_string(all) + " changes, and Install"
                             : std::string("Opens it in Settings \u203a Updates: what changed, and Install"),
                     "", [this, i] { answerUpdateOffer(i, true); } });
    rows.back().h = 62;
    rows.push_back({ Row::Action, ICON_HOURGLASS, "Later", "Offered again the next time Songate starts", "", [this, i] { answerUpdateOffer(i, false); } });
    rows.back().h = 62;
    // The top of what is new: the first line of each part of the changelog, then their second lines, five at most
    constexpr int kTop = 5;
    std::vector<const UpdItem*> top;
    for (size_t depth = 0; int(top.size()) < kTop; depth++) {
        bool any = false;
        for (const UpdLog& sec : r.log)
            if (depth < sec.items.size() && int(top.size()) < kTop) { top.push_back(&sec.items[depth]); any = true; }
        if (!any) break;
    }
    if (top.empty()) return;
    rows.push_back({ Row::Group, nullptr, "What's new" });
    rows.back().h = 40;
    for (const UpdItem* it : top) text(changelogIcon(it->icon), it->text);
}

void App::checkUpdates(bool quiet) {
    if (!store.updates) { if (!quiet) allowUpdates(); return; }
    if (updater.busy()) return;
    updateQuiet = quiet;
    updater.check();
}

void App::updateTick() {
    if (!updater.poll()) return;
    if (updater.justChecked) {
        const int n = updater.newer(store.betas);
        // a model this Songate can play, not downloaded yet: announced once (the user: "Automix V1.2.0 available")
        std::string newModel;
        for (const ModelRow& r : modelList())
            if (r.offer >= 0 && !r.downloaded && Updater::modelUsable(updater.models[size_t(r.offer)]) && r.name != store.modelToasted) newModel = r.name;
        if (updateQuiet) {   // the check at launch: a newer version gets its banner above Settings (App::updateOffer), a toast only
                             // where there is no sidebar to carry it (a phone upright), once each
            // (a phone upright has the banner too now, at the head of Settings' list, and a dot on the bar's Settings: view_phone.cpp)
            const bool sayIt = false;
            if (updater.error.empty() && n >= 0 && sayIt && updater.releases[size_t(n)].version != store.updateToasted) {
                store.updateToasted = updater.releases[size_t(n)].version;
                persistSoon(0.3f);
                toast(Updater::releaseName(store.updateToasted) + " is out: Settings › Updates");
            } else if (!newModel.empty()) {
                store.modelToasted = newModel;
                persistSoon(0.3f);
                toast(newModel + " is available: Settings › Updates");
            }
        } else if (!updater.error.empty()) {
            toast("Could not check: " + updater.error);
        } else {
            toast(n >= 0 ? Updater::releaseName(updater.releases[size_t(n)].version) + " is out" : "Songate is up to date");
            if (!newModel.empty()) {
                store.modelToasted = newModel;
                persistSoon(0.3f);
                toast(newModel + " is available");
            }
        }
        updateQuiet = false;
        return;
    }
    if (updater.justModel) {   // in: it plays from the next join (Download and use)
        loadModelFiles();
        useModel(updater.working());
        return;
    }
    backupsAt = -100;
    if (updater.justInstalled) {
        if (sheet.page == Sheet::Release) closeSheet();
        toast("Installed " + updater.installed.version + ": restart to finish");
        settingsCat = SetUpdates;
        settingsQuery.clear();
        settingsPhoneOpen_ = settingsPhoneDirect_ = true;   // upright: its page, and back returns from where it came
        if (route.view == View::Settings) area("settings.pane").target = 0;
    } else if (!updater.error.empty()) {
        toast("Not installed: " + updater.error);
    }
}

void App::openRelease(int i) {
    if (i < 0 || i >= int(updater.releases.size())) return;
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Release;
    sheet.recipe = i;
    sheet.name = updater.releases[size_t(i)].version;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

void App::updateSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub) {
    const UpdRelease& r = updater.releases[size_t(sheet.recipe)];
    const int cmp = Updater::compare(r.version, Updater::thisVersion());
    icon = cmp > 0 ? ICON_DOWNLOAD : cmp < 0 ? ICON_HISTORY : ICON_CHECK;
    kick = std::string(r.beta ? "Beta · " : "") + (cmp > 0 ? "Newer" : cmp < 0 ? "Earlier version" : "This version");
    title = Updater::releaseName(r.version);
    sub = longDate(r.date) + " · " + megabytes(r.zipSize) + (r.manifest ? "" : " · without in-app updates");
}

void App::updateSheetRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    if (sheet.recipe < 0 || sheet.recipe >= int(updater.releases.size()) || updater.releases[size_t(sheet.recipe)].version != sheet.name) {
        closeSheet();   // the list was read again meanwhile
        return;
    }
    const int i = sheet.recipe;
    const UpdRelease& r = updater.releases[size_t(i)];
    const int cmp = Updater::compare(r.version, Updater::thisVersion());
    const float textW = cardW(600) - 48 - 48;   // as view_smart.cpp draws a Text row
    auto text = [&](const char* icon, const std::string& s, bool strong = false) {
        Row t{ Row::Text, icon, "", s };
        t.on = strong;   // a warning: in the text colour, not the quieter one
        t.h = g.wrap(s, { css::Sm }, int(textW), 12).size() * 19.f + 14;
        rows.push_back(t);
    };
    // ---- what it does: one action, or what is going on
    if (updater.busy()) {
        rows.push_back({ Row::Action, ICON_HOURGLASS, updater.working() == r.version ? updateStatus() : "Busy with " + updater.working(),
                         "Settings › Updates shows how far", "", nullptr });
    } else {
        const std::string label = cmp > 0 ? "Install " + r.version : cmp < 0 ? "Go back to " + r.version : "Install " + r.version + " again";
        const std::string sub = cmp > 0   ? GF_UPDATE_WHAT
                              : cmp < 0   ? (r.manifest ? "Your settings are backed up first" : "Your settings are backed up first; back from it by hand")
                                          : "Puts this version's files back as they were released";
        rows.push_back({ Row::Action, cmp > 0 ? ICON_DOWNLOAD : cmp < 0 ? ICON_HISTORY : ICON_REFRESH, label, sub, megabytes(r.zipSize),
                         [this, i] { installRelease(i); } });
    }
    rows.back().h = 62;
    if (r.beta)
        text(ICON_BOLT, "A beta: it can be unstable and have bugs. Please report what you find: " ISSUES_URL, true);
    if (!r.manifest)
        text(ICON_INFO, r.version + " cannot update itself (it has no Updates page). To come back later, copy a newer release from "
                            "github.com/AndryTheBeast/Songate/releases to " GF_THE_CARD " by hand; your settings are backed up first.", true);
    // ---- the changelog
    if (r.log.empty()) {
        rows.push_back({ Row::Group, nullptr, "What's new" });
        rows.back().h = 38;
        text(nullptr, "This release has no notes.");
    }
    for (const UpdLog& sec : r.log) {
        rows.push_back({ Row::Group, changelogIcon(sec.icon), sec.title });
        rows.back().h = 40;
        for (const UpdItem& it : sec.items) text(changelogIcon(it.icon), it.text);
    }
}

// ============================================================ Automix models (over the air)

std::vector<App::ModelRow> App::modelList() const {
    std::vector<ModelRow> out;
    ModelRow b;
    b.name = automix::name(automix::Latest);   // the built-in one a release plays
    b.builtin = true;
    out.push_back(b);
    for (const ModelEntry& e : modelFiles) {
        ModelRow r;
        r.name = e.m.name;
        r.downloaded = true;
        r.builtin = e.file == bundledModelFile();   // comes with Songate too (written to models/ at launch)
        out.push_back(r);
    }
    for (int i = 0; i < int(updater.models.size()); i++) {
        const std::string& n = updater.models[size_t(i)].name;
        bool had = false;
        for (ModelRow& r : out) if (r.name == n) { r.offer = i; had = true; }
        if (!had) {
            ModelRow r;
            r.name = n;
            r.offer = i;
            out.push_back(r);
        }
    }
    return out;
}

void App::openModel(const std::string& name) {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Model;
    sheet.name = name;
    sheet.returnFocus = ui.focusedId();
    ui.focusDefault();
}

void App::useModel(const std::string& name) {
    store.automixModel = name;
    store.save();
    toast(name + " plans the next join");
}

void App::downloadModel(int offer) {
    if (!store.updates) { allowUpdates(); return; }
    if (updater.busy()) { toast("Wait for " + updater.working() + " first"); return; }
    updater.installModel(offer, store.path("models"));
}

void App::modelSheetHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub) {
    ModelRow row;
    for (const ModelRow& r : modelList()) if (r.name == sheet.name) row = r;
    const UpdModel* m = row.offer >= 0 ? &updater.models[size_t(row.offer)] : nullptr;
    icon = ICON_SPARKLE;
    kick = std::string("Automix model · ") + (row.name == mixName() ? "In use" : row.builtin ? "Built in" : row.downloaded ? "Downloaded" : "Available");
    title = row.name;
    sub = row.builtin ? "Comes with Songate" : m ? longDate(m->date) + " · " + std::to_string(std::max<int64_t>(1, m->size / 1024)) + " KB" : "Downloaded";
}

void App::modelSheetRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    ModelRow row;
    bool found = false;
    for (const ModelRow& r : modelList()) if (r.name == sheet.name) { row = r; found = true; }
    if (!found) { closeSheet(); return; }
    const UpdModel* m = row.offer >= 0 ? &updater.models[size_t(row.offer)] : nullptr;
    const float textW = cardW(600) - 48 - 48;
    auto text = [&](const char* icon, const std::string& s, bool strong = false) {
        Row t{ Row::Text, icon, "", s };
        t.on = strong;
        t.h = g.wrap(s, { css::Sm }, int(textW), 12).size() * 19.f + 14;
        rows.push_back(t);
    };
    const std::string name = row.name;
    std::string why;
    if (row.name == mixName()) {
        rows.push_back({ Row::Action, ICON_CHECK, "In use", "It plans every join, and the order of Automix playlists ordered again", "", nullptr });
    } else if (row.builtin || row.downloaded) {
        // the first built-in model is gone back to; the bundled one (V1.2.0, a file) is used, as a download is
        const bool back = row.builtin && name == automix::name(automix::Latest);
        rows.push_back({ Row::Action, back ? ICON_HISTORY : ICON_SPARKLE, (back ? "Go back to " : "Use ") + name,
                         back ? "Songate's first trained model: back to it at any time" : "From the next join", "", [this, name] { useModel(name); } });
    } else if (updater.busy()) {
        rows.push_back({ Row::Action, ICON_HOURGLASS, updater.working() == name ? updateStatus() : "Busy with " + updater.working(),
                         "Settings › Updates shows how far", "", nullptr });
    } else if (m && Updater::modelUsable(*m, &why)) {
        const int offer = row.offer;
        rows.push_back({ Row::Action, ICON_DOWNLOAD, "Download and use", "Checked by its checksum, kept in " GF_CONFIG_NAME "/models",
                         std::to_string(std::max<int64_t>(1, m->size / 1024)) + " KB", [this, offer] { downloadModel(offer); } });
    } else {
        text(ICON_INFO, why.empty() ? "It needs a newer Songate" : why, true);
    }
    if (!rows.empty() && rows.back().kind == Row::Action) rows.back().h = 62;
    rows.push_back({ Row::Group, nullptr, "What's new" });
    rows.back().h = 38;
    if (row.builtin && row.name == automix::name(automix::Latest))
        text(nullptr, "The first trained model: four rounds of rated demos from a real library, frozen on 29 October 2026.");
    else if (row.builtin)
        text(nullptr, "The second trained model, after a fifth round of rated demos: on drops and on joins by the songs' structure, "
                      "the next song comes in stabs on every beat, and joins on the beat blend for longer.");
    else if (m && !m->notes.empty()) for (const std::string& n : m->notes) text(nullptr, n);
    else text(nullptr, "This model has no notes.");
    text(ICON_INFO, "A model comes over the air when only its numbers change. One that needs new logic (a new kind of join) or a new "
                    "analysis comes with a Songate update.");
}

void App::installRelease(int i) {
    if (!store.updates) { allowUpdates(); return; }
    if (updater.busy()) { toast("Wait for " + updater.working() + " first"); return; }
    if (i < 0 || i >= int(updater.releases.size())) return;
    const UpdRelease& r = updater.releases[size_t(i)];
    const std::string me = Updater::thisVersion();
    const int cmp = Updater::compare(r.version, me);
    std::string text = "Your settings are backed up first. Then " GF_UPDATE_PARTS "; "
                       "Restart now finishes it (the music stops for a moment).";
    if (cmp < 0) {
        text += " Your playlists, Liked and edits stay. Start fresh clears the settings when Songate closes; the backup keeps them.";
    }

    confirm(cmp > 0 ? ICON_DOWNLOAD : cmp < 0 ? ICON_HISTORY : ICON_REFRESH, cmp > 0 ? "Update" : cmp < 0 ? "Go back" : "Install again",
            Updater::releaseName(r.version), longDate(r.date) + " · " + megabytes(r.zipSize),
            cmp > 0 ? "Install " + r.version + "?" : cmp < 0 ? "Go back to " + r.version + "?" : "Install " + r.version + " again?", text,
            cmp < 0 ? "Keep my settings" : "Install", [this, i] { updater.install(i, false, backupRoot()); });
    sheet.askSafe = true;
    sheet.askYesIcon = cmp < 0 ? ICON_HISTORY : ICON_DOWNLOAD;
    if (!r.manifest) sheet.askWarn = r.version + " cannot update itself. Coming back to a newer version is by hand.";
    else if (r.beta) sheet.askWarn = "A beta can be unstable and have bugs. Please report them.";
    if (cmp < 0) {
        sheet.askAlt = "Start fresh";
        sheet.askAltYes = [this, i] { updater.install(i, true, backupRoot()); };
    }
}

void App::restoreBackup(const UpdBackup& b) {
    confirm(ICON_HISTORY, "Backups", b.name, b.note, "Put these settings back?",
            "Your settings now are backed up first. Then Songate restarts with these: settings, playlists, Liked, edits and statistics "
            "as they were then.",
            "Restore and restart", [this, path = b.path] {
                persistNow();
                Store::flush();
                // The backup first, on the worker (a big settings folder froze the app), then the restart
                auto made = std::make_shared<std::string>();
                if (!startFileWork("Backing up the settings", backupRoot(), false, [this, made, root = backupRoot()](FileWork& w) {
                        w.total = 0;
                        *made = updater.backupNow(root, "before a restore");
                        w.result = made->empty() ? "Could not back up the settings now: nothing was changed" : "Backed up: restarting";
                    }, [this, made, path] {
                        if (made->empty()) return;
                        updater.restoreAtExit(path);
                        relaunch = true;
                        running = false;
                    }))
                    return;
                fileWork.cat = SetReset;
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_HISTORY;
}

/**
 * Settings → Reset → Reset settings: every setting as Gatefold came, after a question; only settings (the user,
 * 2026-10-28: everything else is the factory reset's). Before, it reset only the store, so the equalizer kept playing
 * as it was (both players hold their own copy), the background player kept its game actions and fades, the sidebar
 * stayed collapsed until the next launch, and your tag edits, covers, lyric timings and saved presets vanished until
 * the next launch (dropped in memory, not on the card).
 */
void App::resetSettings() {
    confirm(ICON_REFRESH, "Reset", "Reset settings", "Every setting, as Songate came", "Reset every setting?",
            "Colours, the sidebar, playback, the equalizer, lyrics, the Canvas, the visualizer, controls, where browsers open, "
            GF_RESET_LIST " Only settings: your music "
            "folder, playlists, Liked, tag edits, covers, statistics and saved presets stay (Factory reset, below, erases those).",
            "Reset", [this] {
                defaultSettings();
                persistNow();
                toast("Every setting is as it came");
            });
}

void App::defaultSettings() {
    Store fresh;
    // Yours, not settings: kept
    fresh.musicRoot = store.musicRoot;
    fresh.playlists = store.playlists;
    fresh.liked = store.liked;
    fresh.edits = store.edits;
    fresh.covers = store.covers;
    fresh.lyricNudge = store.lyricNudge;
    fresh.eqUser = store.eqUser;
    const bool bgWasOff = !player.remote();
    if (store.updates) updater.cancel();
    store = std::move(fresh);
    sideW = store.sidebarCollapsed ? css::SideWCollapsed : css::SideW;
    sideP_ = store.sidebarCollapsed ? 0.f : 1.f;
#ifdef GF_ANDROID
    fitPending_ = true;   // Size back to Medium: laid out again between frames
#endif
    volumeShown = store.volume;
    player.setVolume(volumeShown);
    setEq(store.eq);   // Gatefold's player, and the background player's copy (sent within 0.1 s)
    if (player.remote()) {   // its own settings: game actions, fades, action playlists
        bgs.cfg = BgConfig{};
        bg.setConfig(bgs.cfg);
    }
    applyTheme();
    coversHeld = false;
    applyJobStops();   // stopped work starts again
    if (bgWasOff) setBackground(true);   // on, as it came (when it is installed)
}

namespace {
struct FactoryPartInfo { const char* label; const char* sub; };
// In App::FactoryPart's order (and Updater::factoryFiles')
const FactoryPartInfo kFactoryParts[] = {
    { "Settings", GF_FACTORY_SET },
    { "Music folder", "Songate asks where your music is again, as at its first start" },
    { "Playlists", "Your playlists, smart and folder ones, and their icons" },
    { "Liked", "Every song you liked" },
    { "Tag edits, covers and moods", "Edits, covers and moods; tags written into files stay, can't be restored" },   // (tag-undo goes too)
    { "Lyric timings", "How far you moved each song's synced lyrics" },
    { "Lyrics found online", "Those saved in Songate's folder (those saved beside the songs stay)" },
    { "Saved equalizer presets", "The presets you saved under your own names" },
    { "Play counts and statistics", "Plays, last played and dates added (smart playlists use them)" },
    { "The queue", GF_FACTORY_QUEUE },
    { "Last.fm", "Signs out, and erases what was waiting to be sent" },
    { "Devices", "This device's key and the devices paired with it: they must pair again" },
    { "Caches", GF_CACHE_WORDS ": made again from your music" },
};
}  // namespace

/** Settings → Reset → Factory reset: the sheet of what Gatefold keeps, each with a switch (all off at first; what you
 *  ticked stays ticked when you come back after a Cancel on the question). */
void App::openFactory() {
    const Id back = ui.focusedId();
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    sheet = Sheet{};
    sheet.page = Sheet::Factory;
    sheet.returnFocus = back;
    ui.focusDefault();
}

void App::factoryRows(std::vector<SheetRow>& rows) {
    rows.push_back({ SheetRow::Group, nullptr, "Erase what you tick" });
    rows.back().h = 34;
    const bool server = subsonic.view().state != Subsonic::SignedOut;   // (a server's mentioned only while there is one)
    for (int k = 0; k < FacParts; k++) {
        const bool both = k == FacLastfm && server;
        rows.push_back({ SheetRow::Toggle, nullptr, both ? "Last.fm and the Subsonic server" : kFactoryParts[k].label,
                         both ? "Signs out of both; erases what was waiting to be sent, and the server's downloads in Songate's folder" : kFactoryParts[k].sub, "",
                         [this, k] { factoryPick ^= 1u << k; } });
        rows.back().h = 60;
        rows.back().on = factoryPick & (1u << k);
    }
    rows.push_back({ SheetRow::Group, nullptr, "" });
    rows.back().h = 14;
    int n = 0;
    for (int k = 0; k < FacParts; k++) n += (factoryPick >> k) & 1;
    rows.push_back({ SheetRow::Action, ICON_X, "Erase what is ticked",
                     n ? std::to_string(n) + " of " + std::to_string(int(FacParts)) + " ticked: they go when Songate closes and starts again"
                       : "Tick what to erase above first",
                     "", n ? std::function<void()>([this] { factoryErase(factoryPick); }) : nullptr });
    rows.back().h = 60;
    rows.push_back({ SheetRow::Action, ICON_X, "Erase everything", "As if Songate were newly installed: every part above, and the rest", "",
                     [this] { factoryErase(~0u); } });
    rows.back().h = 60;
}

/** Asks, then closes Gatefold: shutdown moves the ticked parts aside (Updater::factoryReset) and it starts again.
 *  Everything asks twice. The backups (another folder) and the music files always stay. */
void App::factoryErase(unsigned mask) {
    const unsigned allParts = (1u << FacParts) - 1;
    const bool all = (mask & allParts) == allParts;
    auto go = [this, mask, all] {
        // Settings and the music folder live together in settings.txt, saved as Gatefold closes: they are reset here
        if (!all && (mask & (1u << FacSettings))) defaultSettings();
        if (!all && (mask & (1u << FacMusic))) store.musicRoot.clear();
        factoryMask_ = all ? ~0u : mask;
        relaunch = true;
        running = false;
    };
    if (!all) {
        std::string list;
        for (int k = 0; k < FacParts; k++)
            if (mask & (1u << k)) list += (list.empty() ? "" : ", ") + std::string(kFactoryParts[k].label);
        confirm(ICON_X, "Factory reset", "Erase what is ticked", list, "Erase these for good?",
                "Erased: " + list + ". Songate closes now and starts again without them. Only a backup made before can bring them back; "
                "your music files and backups in " + backupRoot() + " stay.",
                "Erase", go);
        sheet.askWarn = "THIS CANNOT BE UNDONE";
        return;
    }
    confirm(ICON_X, "Factory reset", "Erase everything", "Songate as if it were newly installed", "Erase everything Songate keeps?",
            "Every setting, playlist and Liked song; your tag edits, chosen covers, lyric timings, lyrics found online and saved equalizer presets; play "
            "counts and statistics; your Last.fm sign-in and paired devices; " GF_FACTORY_LIST " "
            "Songate then closes and starts again as new. Your music files are not touched, and backups in " + backupRoot() + " stay.",
            "Erase everything", [this, go] {
                confirm(ICON_X, "Factory reset", "Are you absolutely sure?", "There is no way back", "Erase everything now?",
                        "Only a backup made before can bring any of it back. If you might want your playlists, Liked or settings "
                        "again, choose Cancel and use Back up the settings now first.",
                        "Yes, erase everything", go);
                sheet.askWarn = "EVERYTHING WILL BE ERASED FOR GOOD";
            });
    sheet.askWarn = "THIS CANNOT BE UNDONE: SETTINGS, PLAYLISTS, LIKED AND STATISTICS ARE LOST";
}

/** The background player from its new files (pm:shell), then Gatefold: it closes, the new .nro is swapped in, and it starts again. */
void App::finishUpdate() {
#ifdef GF_ANDROID
    // Android replaces an app itself: its installer opens on the download, and Songate stays open until the user agrees there
    if (const std::string setup = updater.installerFile(); !setup.empty()) {
        if (!plat::relaunch(setup)) toast("Could not open Android's installer");
        return;
    }
#endif
    if (updater.installed.bgChanged) {
        bg.disconnect();
        std::string err;
        if (!Updater::restartBackground(err)) {   // the console's restart does it (and Gatefold's .nro waits for the next close or launch)
            toast(err);
            return;
        }
        updater.markBgRestarted();
    }
    relaunch = true;
    running = false;
}
