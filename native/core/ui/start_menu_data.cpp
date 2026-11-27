// Start menu tile arrangement, catalog projection, search, and pagination.
#include "ui/start_menu.h"

#include "localization.h"
#include "log.h"
#include "ui/start_menu_metrics.h"
#include "ui/start_menu_sort.h"

#include <algorithm>
#include <ranges>

namespace atrium {

namespace {

using namespace start_menu_detail;

bool IsPath(const std::wstring& id) { return id.size() > 2 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/'); }

// An app taken out of the arrangement: a tile of its own or one of a folder's
// (a folder left empty goes with it).
std::optional<StartTile> Take(std::vector<StartTile>& tiles, const std::wstring& id)
{
    for (auto it = tiles.begin(); it != tiles.end(); ++it) {
        if (!it->Folder()) {
            if (it->id == id) {
                StartTile t = std::move(*it);
                tiles.erase(it);
                return t;
            }
            continue;
        }
        for (auto a = it->apps.begin(); a != it->apps.end(); ++a)
            if (a->id == id) {
                StartTile t = std::move(*a);
                it->apps.erase(a);
                if (it->apps.empty())
                    tiles.erase(it);
                return t;
            }
    }
    return std::nullopt;
}

} // namespace

std::optional<StartTile> StartMenu::TakeTile(std::vector<StartTile>& tiles, const std::wstring& id) { return Take(tiles, id); }
int StartMenu::ParseIndex(const std::wstring& id, size_t prefix) { return Index(id, prefix); }

// An arrangement of the user's own still takes what is pinned to the Windows start menu later: it
// joins at the end (#63). A pin the user removed here stays out; one unpinned in Windows and pinned
// again is new again.
void StartMenu::SyncWindowsPins()
{
    if (!layout_.custom || !catalog_.Loaded() || pins_.empty())
        return; // no complete list of the Windows pins (the export can fail): nothing to compare
    std::vector<std::wstring> now;
    now.reserve(pins_.size());
    for (const CatalogApp& p : pins_)
        now.push_back(p.id);
    if (!layout_.windowsPinsKnown) { // an arrangement from before: today's pins are its starting point
        layout_.windowsPins = now;
        layout_.windowsPinsKnown = true;
        SaveStartLayout(layout_);
        return;
    }
    auto has = [](const std::vector<std::wstring>& list, const std::wstring& id) { return std::ranges::find(list, id) != list.end(); };
    auto placed = [&](const std::wstring& id) {
        for (const StartTile& t : layout_.tiles)
            if (t.id == id || std::ranges::any_of(t.apps, [&](const StartTile& a) { return a.id == id; }))
                return true;
        return false;
    };
    size_t added = 0;
    for (const CatalogApp& p : pins_)
        if (!has(layout_.windowsPins, p.id) && !placed(p.id) && !Hidden(p.id)) {
            layout_.tiles.push_back({p.id, p.name, {}});
            ++added;
        }
    if (added || now != layout_.windowsPins) {
        layout_.windowsPins = std::move(now);
        layout_.Clamp();
        SaveStartLayout(layout_);
        if (added)
            Log(L"start: %zu new Windows start pin(s) added at the end", added);
    }
}

void StartMenu::BuildDefaultTiles()
{
    unsigned long nextFolder = 1;
    if (pinGroups_.empty())
        for (const CatalogApp& p : pins_)
            if (!Hidden(p.id))
                tiles_.push_back({p.id, p.name, p, {}, false});
    for (const CatalogFolder& g : pinGroups_) {
        std::vector<CatalogApp> apps;
        for (const CatalogApp& p : g.apps)
            if (!Hidden(p.id))
                apps.push_back(p);
        if (g.folder && apps.size() >= 2)
            tiles_.push_back({L"folder:" + std::to_wstring(nextFolder++), g.name.empty() ? Tr(L"New folder") : g.name, {}, std::move(apps), true});
        else
            for (const CatalogApp& p : apps)
                tiles_.push_back({p.id, p.name, p, {}, false});
    }
}

void StartMenu::BuildCustomTiles(const std::map<std::wstring, const CatalogApp*>& known)
{
    const bool loaded = catalog_.Loaded();
    auto resolve = [&](const StartTile& t, CatalogApp& out) {
        if (auto it = known.find(t.id); it != known.end()) {
            out = *it->second;
            return true;
        }
        if (loaded && !t.id.starts_with(L"::{") && (!IsPath(t.id) || GetFileAttributesW(t.id.c_str()) == INVALID_FILE_ATTRIBUTES))
            return false;
        out = CatalogApp{t.name.empty() ? t.id : t.name, t.id, {}, {}, {}};
        return true;
    };
    for (const StartTile& t : layout_.tiles) {
        Tile tile{t.id, t.name, {}, {}, t.Folder()};
        if (tile.folder) {
            for (const StartTile& a : t.apps)
                if (CatalogApp app; resolve(a, app)) {
                    if (!a.alias.empty())
                        app.name = a.alias;
                    tile.apps.push_back(std::move(app));
                }
            if (tile.apps.empty())
                continue;
        } else {
            if (!resolve(t, tile.app))
                continue;
            tile.name = t.alias.empty() ? tile.app.name : t.alias;
        }
        tiles_.push_back(std::move(tile));
    }
}

void StartMenu::Rebuild()
{
    const std::wstring openKey = folder_ >= 0 && folder_ < int(tiles_.size()) ? tiles_[size_t(folder_)].key : L"";
    std::map<std::wstring, const CatalogApp*> known;
    for (const CatalogApp& a : apps_)
        known.emplace(a.id, &a);
    for (const CatalogApp& a : pins_)
        known.emplace(a.id, &a);
    SyncWindowsPins();
    tiles_.clear();
    if (!layout_.custom)
        BuildDefaultTiles();
    else
        BuildCustomTiles(known);
    std::vector<std::wstring> ids;
    for (const Tile& t : tiles_) {
        // A folder's apps are read ahead with the tiles': opening it must find them there.
        if (!t.folder) {
            ids.push_back(t.key);
            continue;
        }
        for (const CatalogApp& a : t.apps)
            ids.push_back(a.id);
    }
    catalog_.SetStartIds(std::move(ids));
    folder_ = -1;
    for (size_t i = 0; i < tiles_.size() && !openKey.empty(); ++i)
        if (tiles_[i].key == openKey)
            folder_ = int(i);
    if (folder_ < 0)
        folderClosing_ = naming_ = false;
    if (allApps_)
        list_ = AllApps();
    if (!query_.empty())
        results_ = Results();
    page_ = std::clamp(page_, 0, Pages() - 1);
}

std::vector<StartMenu::Row> StartMenu::Results() const
{
    std::vector<Row> out;
    const AppCatalog::PreparedQuery q = AppCatalog::PrepareQuery(query_);
    // Folders first (by name or initials), then the apps.
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].folder && AppCatalog::MatchesPrepared(CatalogApp{tiles_[i].name}, q))
            out.push_back({0, int(i), {}});
    std::vector<CatalogApp> apps;
    for (const CatalogApp& a : apps_)
        if (!Hidden(a.id) && AppCatalog::MatchesPrepared(a, q))
            apps.push_back(a);
    // Names that start with the query first, then the other matches.
    std::ranges::stable_partition(apps, [&](const CatalogApp& a) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.name.c_str(), int(std::min(a.name.size(), query_.size())), query_.c_str(),
                               int(query_.size()), nullptr, nullptr, 0) == CSTR_EQUAL;
    });
    for (CatalogApp& a : apps)
        out.push_back({0, -1, std::move(a)});
    return out;
}

std::vector<StartMenu::Row> StartMenu::AllApps() const
{
    return BuildStartMenuAllAppsRows<Row>(tiles_, apps_, [this](const std::wstring& id) { return Hidden(id); });
}

int StartMenu::Pages() const
{
    int n = int(tiles_.size());
    if (drag_.on && !drag_.fromFolder)
        --n; // the carried tile is not in the grid
    if (drag_.on && !drag_.inFolder)
        ++n; // but there is room for it, also on a page after the last full one
    return std::max(1, (n + PerPage() - 1) / PerPage());
}

int StartMenu::FolderPages() const
{
    if (folder_ < 0)
        return 1;
    int n = int(tiles_[size_t(folder_)].apps.size());
    if (drag_.on && drag_.fromFolder && !drag_.inFolder)
        --n;
    const int per = FolderCols() * FolderRows();
    return std::max(1, (n + per - 1) / per);
}

void StartMenu::SetPage(int page)
{
    page = std::clamp(page, 0, Pages() - 1);
    if (page == page_)
        return;
    page_ = page;
    if (sel_ >= 0 && sel_ / PerPage() != page_ && !Listing())
        sel_ = -1;
    surface_.Invalidate();
}

bool StartMenu::Hidden(const std::wstring& id) const { return std::ranges::find(layout_.hidden, id) != layout_.hidden.end(); }

void StartMenu::SetHidden(const CatalogApp& app, bool hidden)
{
    if (hidden) {
        // Out of the grid and its folders too.
        Arrange([&](std::vector<StartTile>& tiles) {
            Take(tiles, app.id);
            layout_.hidden.push_back(app.id);
        });
    } else {
        std::erase(layout_.hidden, app.id);
        SaveStartLayout(layout_);
        Rebuild();
        surface_.Invalidate();
    }
    Log(L"start: %s %s", app.name.c_str(), hidden ? L"hidden" : L"shown again");
}

void StartMenu::ShowHidden()
{
    // The A-Z list, at its hidden apps.
    allApps_ = true;
    query_.clear();
    results_.clear();
    list_ = AllApps();
    sel_ = -1;
    float top = 0;
    for (const Row& row : list_) {
        if (row.letter == kStartMenuHiddenHead)
            break;
        top += row.letter ? kHeadH : kRowH;
    }
    scroll_ = scrollTarget_ = top; // clamped to the list's end when painted
    Restage();
    surface_.Invalidate();
}

bool StartMenu::OnStart(const std::wstring& id) const
{
    for (const Tile& t : tiles_) {
        if (!t.folder && t.key == id)
            return true;
        if (t.folder && std::ranges::any_of(t.apps, [&](const CatalogApp& a) { return a.id == id; }))
            return true;
    }
    return false;
}

void StartMenu::Arrange(const std::function<void(std::vector<StartTile>&)>& change)
{
    if (!layout_.custom) {
        // The first change: the Windows pins become the menu's own.
        layout_.tiles.clear();
        for (const Tile& t : tiles_) {
            StartTile st{t.key, t.name, {}};
            if (t.folder)
                for (const CatalogApp& a : t.apps)
                    st.apps.push_back({a.id, a.name, {}});
            layout_.tiles.push_back(std::move(st));
        }
        layout_.custom = true;
        layout_.windowsPins.clear();
        for (const CatalogApp& p : pins_)
            layout_.windowsPins.push_back(p.id);
        layout_.windowsPinsKnown = true;
        Log(L"start: arranged by the user from now on (%zu Windows pins copied)", layout_.tiles.size());
    }
    change(layout_.tiles);
    layout_.Clamp();
    SaveStartLayout(layout_);
    Rebuild();
    surface_.Invalidate();
}

void StartMenu::PinToStart(const CatalogApp& app)
{
    if (OnStart(app.id))
        return;
    std::erase(layout_.hidden, app.id);
    Arrange([&](std::vector<StartTile>& tiles) { tiles.push_back({app.id, app.name, {}}); });
    Log(L"start: %s pinned to the start menu", app.name.c_str());
}

} // namespace atrium
