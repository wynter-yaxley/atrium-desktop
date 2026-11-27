// Start menu's popup menus and Windows Start sync.
#include "ui/start_menu.h"
#include "ui/start_menu_power_geom.h"

#include "localization.h"
#include "log.h"

#include <shlobj.h>

#include <algorithm>

namespace atrium {

namespace {

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

constexpr int kOpen = 1, kPinBar = 2, kPinStart = 3, kRemove = 4, kOutOfFolder = 5, kRename = 6, kUngroup = 7, kRemoveFolder = 8, kReset = 9, kHide = 10,
              kUnhide = 11, kShowHidden = 12, kAdmin = 13, kLocation = 14, kSyncPins = 15, kSyncFolders = 16, kSyncBoth = 17, kSize = 18,
              kIcon = kSize + StartLayout::kSizes;

template<class CloseFn> void FinishMenuClose(Surface& surface, GlassMenu* menu, bool& menuOpen, CloseFn&& close)
{
    menuOpen = false;
    if (!surface.Visible())
        return;
    // Back to the start menu, unless the click that closed the menu went to another window.
    HWND fg = GetForegroundWindow();
    if (!fg || fg == surface.Hwnd() || (menu && fg == menu->Hwnd()))
        SetForegroundWindow(surface.Hwnd());
    else
        close();
}

bool TileHasApp(const std::vector<StartTile>& tiles, const std::wstring& id)
{
    for (const StartTile& t : tiles)
        if (t.id == id || std::ranges::any_of(t.apps, [&](const StartTile& a) { return a.id == id; }))
            return true;
    return false;
}

void AppendAppContextEntries(std::vector<MenuEntry>& entries, const CatalogApp& app, bool onStart, bool inFolder, bool hidden)
{
    entries.push_back({Tr(L"Open"), kOpen});
    if (!AppCatalog::FileLocation(app).empty()) {
        entries.push_back({Tr(L"Run as administrator"), kAdmin});
        entries.push_back({Tr(L"Open File Location"), kLocation});
    }
    entries.push_back({});
    entries.push_back({Tr(L"Pin to taskbar"), kPinBar});
    if (!onStart) {
        entries.push_back({Tr(L"Pin to Start"), kPinStart});
    } else {
        entries.push_back({Tr(L"Rename"), kRename});
        if (inFolder)
            entries.push_back({Tr(L"Remove from folder"), kOutOfFolder});
        entries.push_back({Tr(L"Remove from Start"), kRemove});
    }
    entries.push_back(hidden ? MenuEntry{Tr(L"Unhide"), kUnhide} : MenuEntry{Tr(L"Hide"), kHide});
}

void AppendMenuSizeEntries(std::vector<MenuEntry>& entries, int grid, int room, int iconSize, bool hasHidden, bool custom)
{
    const wchar_t* const kNames[] = {Tr(L"Small"), Tr(L"Medium"), Tr(L"Large"), Tr(L"Huge")};
    entries.push_back({Tr(L"Menu size"), 0, false, true, true});
    for (int g = 0; g <= room; ++g)
        entries.push_back({kNames[g], kSize + g, g == grid});
    entries.push_back({});
    entries.push_back({Tr(L"Icon size"), 0, false, true, true});
    for (int icon = 0; icon < StartLayout::kSizes; ++icon)
        entries.push_back({kNames[icon], kIcon + icon, icon == iconSize});
    if (hasHidden) {
        entries.push_back({});
        entries.push_back({Tr(L"Show hidden apps"), kShowHidden});
    }
    if (custom) {
        entries.push_back({});
        entries.push_back({Tr(L"Restore Windows Start pins"), kReset});
    }
}

void MergeWindowsFolder(std::vector<StartTile>& tiles, const CatalogFolder& f, bool win10Both, const std::vector<std::wstring>& hidden, size_t& apps,
                        size_t& newFolders)
{
    std::vector<StartTile> missing;
    for (const CatalogApp& a : f.apps) {
        const bool isHidden = std::ranges::find(hidden, a.id) != hidden.end();
        if (!TileHasApp(tiles, a.id) && !isHidden && std::ranges::none_of(missing, [&](const StartTile& m) { return m.id == a.id; }))
            missing.push_back({a.id, a.name, {}});
    }
    if (!f.folder || (win10Both && missing.size() == 1)) {
        apps += missing.size();
        tiles.insert(tiles.end(), std::make_move_iterator(missing.begin()), std::make_move_iterator(missing.end()));
        return;
    }
    const std::wstring fname = f.name.empty() ? Tr(L"New folder") : f.name;
    auto same = std::ranges::find_if(tiles, [&](const StartTile& t) { return t.Folder() && t.name == fname; });
    if (same != tiles.end()) {
        apps += missing.size();
        same->apps.insert(same->apps.end(), missing.begin(), missing.end());
    } else if (missing.size() >= 2) {
        apps += missing.size();
        ++newFolders;
        StartLayout ids;
        ids.tiles = tiles;
        tiles.push_back({ids.NewFolderId(), fname, std::move(missing)});
    }
}

} // namespace

void StartMenu::OpenSyncMenu()
{
    if (!menu)
        return;
    const float s = surface_.Scale();
    const D2D1_RECT_F b = SyncButton();
    const POINT at = surface_.ContentToScreen(POINT{LONG(b.left * s) + OffsetXPx(), LONG((b.bottom + 6) * s) + OffsetPx()});
    std::vector<MenuEntry> entries = {{Tr(L"Sync with the Windows start menu"), 0, false, true, true},
                                      {Tr(L"Pinned apps and folders"), kSyncBoth},
                                      {Tr(L"Pinned apps only"), kSyncPins},
                                      {Tr(L"Folders and their apps only"), kSyncFolders}};
    if (!catalog_.Loaded())
        for (MenuEntry& e : entries)
            e.disabled = true;
    menuOpen_ = true;
    menu->Open(
        std::move(entries), at, MenuPlace::Below,
        [this](int cmd) {
            if (cmd == kSyncPins || cmd == kSyncFolders || cmd == kSyncBoth)
                SyncFromWindows(cmd != kSyncFolders, cmd != kSyncPins);
        },
        [this] { FinishMenuClose(surface_, menu, menuOpen_, [this] { Close(false); }); });
}

// Pins in the Windows order, at the end; a folder of All apps as a folder of its own (one of the
// same name already here takes the apps it lacks). An app already somewhere in the menu stays where
// it is; hidden apps stay hidden.
void StartMenu::SyncFromWindows(bool pins, bool folders, bool refresh)
{
    if (refresh && catalog_.StartMenuUsesXml())
        pendingSync_ = {true, pins, folders}, catalog_.Refresh(true);
    pins_ = catalog_.StartPins();
    const bool win10Both = pins && folders && catalog_.StartMenuUsesXml();
    const std::vector<CatalogFolder> windowsFolders = win10Both ? catalog_.StartPinGroups() : folders ? catalog_.StartFolders() : std::vector<CatalogFolder>{};
    size_t apps = 0, newFolders = 0;
    Arrange([&](std::vector<StartTile>& tiles) {
        if (pins && !win10Both)
            for (const CatalogApp& p : pins_)
                if (!TileHasApp(tiles, p.id) && !Hidden(p.id)) {
                    tiles.push_back({p.id, p.name, {}});
                    ++apps;
                }
        for (const CatalogFolder& f : windowsFolders)
            MergeWindowsFolder(tiles, f, win10Both, layout_.hidden, apps, newFolders);
    });
    if (pins) {
        layout_.windowsPins.clear();
        for (const CatalogApp& p : pins_)
            layout_.windowsPins.push_back(p.id);
        layout_.windowsPinsKnown = true;
        SaveStartLayout(layout_);
    }
    Log(L"start: synced with the Windows start menu: %zu app(s), %zu new folder(s) (%zu pins, %zu folders there)", apps, newFolders, pins_.size(),
        windowsFolders.size());
}

std::vector<MenuEntry> StartMenu::BuildContextEntries(const std::wstring& id, CatalogApp& app, std::wstring& folderKey) const
{
    std::vector<MenuEntry> entries;
    bool isApp = false, inFolder = false;
    if (id.starts_with(L"tile:")) {
        const int i = ParseIndex(id, 5);
        if (i < 0 || i >= int(tiles_.size()))
            return entries;
        if (tiles_[size_t(i)].folder)
            folderKey = tiles_[size_t(i)].key;
        else
            app = tiles_[size_t(i)].app, isApp = true;
    } else if (id.starts_with(L"fapp:")) {
        const int i = ParseIndex(id, 5);
        if (folder_ < 0 || i < 0 || i >= int(tiles_[size_t(folder_)].apps.size()))
            return entries;
        app = tiles_[size_t(folder_)].apps[size_t(i)];
        isApp = inFolder = true;
    } else if (id.starts_with(L"row:")) {
        const auto& rows = RowsShown();
        const size_t i = size_t(ParseIndex(id, 4));
        if (i >= rows.size() || rows[i].letter)
            return entries;
        if (rows[i].folder >= 0)
            folderKey = tiles_[size_t(rows[i].folder)].key;
        else
            app = rows[i].app, isApp = true;
    }
    if (isApp)
        AppendAppContextEntries(entries, app, OnStart(app.id), inFolder, Hidden(app.id));
    else if (!folderKey.empty())
        entries = {{Tr(L"Open"), kOpen}, {Tr(L"Rename"), kRename}, {}, {Tr(L"Ungroup folder"), kUngroup}, {Tr(L"Remove from Start"), kRemoveFolder}};
    else if (id.empty() && !Listing() && folder_ < 0)
        AppendMenuSizeEntries(entries, grid_, room_, layout_.icon, !layout_.hidden.empty(), layout_.custom);
    return entries;
}

void StartMenu::RunContextCommand(int cmd, const CatalogApp& app, const std::wstring& folderKey, const std::wstring& openKey)
{
    auto tileOf = [&](const std::wstring& key) {
        for (size_t i = 0; i < tiles_.size(); ++i)
            if (tiles_[i].key == key)
                return int(i);
        return -1;
    };
    if (cmd >= kSize && cmd < kSize + 4) {
        SetSize(cmd - kSize, layout_.icon);
        SaveStartLayout(layout_);
        Log(L"start: sized to %d x %d tiles", cols_, rows_);
        if (onSize)
            onSize(SizeName());
        return;
    }
    if (cmd >= kIcon && cmd < kIcon + 4) {
        SetSize(layout_.size, cmd - kIcon);
        SaveStartLayout(layout_);
        Log(L"start: icons %.0f DIP (%d px), %d x %d tiles", IconSize(), IconPx(), cols_, rows_);
        if (onIcon)
            onIcon(IconSizeName());
        return;
    }
    switch (cmd) {
    case kOpen:
        if (!folderKey.empty())
            OpenFolder(tileOf(folderKey));
        else
            Launch(app);
        break;
    case kAdmin:
        Close(true);
        AppCatalog::LaunchAsAdmin(app);
        break;
    case kLocation:
        Close(true);
        if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(AppCatalog::FileLocation(app).c_str())) {
            SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ILFree(item);
        }
        break;
    case kPinBar:
        if (onPin)
            onPin(AppCatalog::PinFor(app));
        break;
    case kPinStart: PinToStart(app); break;
    case kRemove:
        Arrange([&](std::vector<StartTile>& tiles) { TakeTile(tiles, app.id); });
        Log(L"start: %s removed from the start menu", app.name.c_str());
        break;
    case kShowHidden: ShowHidden(); break;
    case kHide: SetHidden(app, true); break;
    case kUnhide: SetHidden(app, false); break;
    case kOutOfFolder:
        Arrange([&](std::vector<StartTile>& tiles) { // next to its folder on the grid
            std::optional<StartTile> moved = TakeTile(tiles, app.id);
            if (!moved)
                return;
            auto at = std::ranges::find_if(tiles, [&](const StartTile& t) { return t.id == openKey; });
            tiles.insert(at == tiles.end() ? at : at + 1, std::move(*moved));
        });
        break;
    case kRename:
        if (!folderKey.empty())
            OpenFolder(tileOf(folderKey));
        StartNaming(folderKey.empty() ? app.id : L"");
        break;
    case kUngroup:
    case kRemoveFolder:
        if (folder_ >= 0 && tiles_[size_t(folder_)].key == folderKey)
            folder_ = -1, folderClosing_ = naming_ = false;
        Arrange([&](std::vector<StartTile>& tiles) {
            if (cmd == kRemoveFolder)
                RemoveStartFolder(tiles, folderKey);
            else
                UngroupStartFolder(tiles, folderKey);
        });
        Log(cmd == kRemoveFolder ? L"start: folder %s removed from the start menu" : L"start: folder %s ungrouped", folderKey.c_str());
        break;
    case kReset:
        layout_.custom = false;
        layout_.tiles.clear();
        SaveStartLayout(layout_);
        Rebuild();
        surface_.Invalidate();
        Log(L"start: back to the Windows start menu's pins");
        break;
    default: break;
    }
}

void StartMenu::ContextMenu(const std::wstring& id, POINT screen)
{
    if (!menu)
        return;
    FinishNaming(true);
    CatalogApp app;
    std::wstring folderKey;
    std::vector<MenuEntry> entries = BuildContextEntries(id, app, folderKey);
    if (entries.empty())
        return;
    const std::wstring openKey = folder_ >= 0 ? tiles_[size_t(folder_)].key : L"";
    menuOpen_ = true;
    menu->Open(
        std::move(entries), screen, MenuPlace::Below, [this, app, folderKey, openKey](int cmd) { RunContextCommand(cmd, app, folderKey, openKey); },
        [this] { FinishMenuClose(surface_, menu, menuOpen_, [this] { Close(false); }); });
    surface_.Invalidate();
}

void StartMenu::StartNaming(const std::wstring& key)
{
    auto begin = [&](const std::wstring& k, const std::wstring& hit, const std::wstring& draft) {
        naming_ = true, namingKey_ = k, namingHit_ = hit, nameDraft_ = draft;
        surface_.Invalidate();
    };
    if (key.empty() || key.starts_with(L"folder:")) {
        if (folder_ >= 0 && !folderClosing_)
            begin(tiles_[size_t(folder_)].key, L"ftitle", tiles_[size_t(folder_)].name);
        return;
    }
    if (Listing())
        query_.clear(), composition_.clear(), results_.clear(), allApps_ = searchFocused_ = false;
    for (size_t i = 0; i < tiles_.size(); ++i) {
        if (!tiles_[i].folder && tiles_[i].key == key) {
            if (folder_ >= 0)
                CloseFolder();
            SetPage(int(i) / std::max(1, PerPage()));
            begin(key, L"tile:" + std::to_wstring(i), tiles_[i].name);
            return;
        }
        if (tiles_[i].folder)
            for (size_t k = 0; k < tiles_[i].apps.size(); ++k)
                if (tiles_[i].apps[k].id == key) {
                    if (folder_ != int(i) || folderClosing_)
                        OpenFolder(int(i));
                    begin(key, L"fapp:" + std::to_wstring(k), tiles_[i].apps[k].name);
                    return;
                }
    }
}

void StartMenu::FinishNaming(bool keep)
{
    if (!naming_)
        return;
    naming_ = false;
    std::wstring name = nameDraft_;
    while (!name.empty() && iswspace(name.back()))
        name.pop_back();
    while (!name.empty() && iswspace(name.front()))
        name.erase(name.begin());
    const std::wstring key = std::move(namingKey_);
    namingKey_.clear();
    namingHit_.clear();
    if (keep) {
        if (key.starts_with(L"folder:")) {
            if (folder_ >= 0 && !name.empty() && name != tiles_[size_t(folder_)].name) {
                Arrange([&](std::vector<StartTile>& tiles) {
                    if (auto it = std::ranges::find_if(tiles, [&](const StartTile& t) { return t.id == key; }); it != tiles.end())
                        it->name = name;
                });
                Log(L"start: folder renamed to '%s'", name.c_str());
            }
        } else {
            std::wstring orig;
            for (const Tile& t : tiles_)
                if (!t.folder && t.key == key)
                    orig = t.app.name;
            const std::wstring alias = (!name.empty() && name == orig) ? L"" : name;
            Arrange([&](std::vector<StartTile>& tiles) { ApplyTileAlias(tiles, key, alias); });
        }
    }
    surface_.Invalidate();
}

void StartMenu::PaintHoverTip(Canvas& c, const Theme& /*t*/)
{
    if (tipText_.empty() || drag_.on || menuOpen_)
        return;
    const D2D1_RECT_F b = MainRect();
    const float sz = std::max(11.5f, LabelSize());
    const float capH = (c.Caret(L"A", b, sz, DWRITE_FONT_WEIGHT_NORMAL, 0).bottom - b.top) + 8.f;
    const float capW = std::min(std::max(40.f, (b.right - b.left) - 16.f), c.TextWidth(tipText_, sz) + 20.f);
    const float left = std::clamp((tipRect_.left + tipRect_.right - capW) / 2.f, b.left + 8.f, b.right - 8.f - capW);
    const float top = std::clamp(tipRect_.top - 2.f, b.top + 8.f, b.bottom - 8.f - capH);
    const D2D1_RECT_F cap = D2D1::RectF(left, top, left + capW, top + capH);
    const bool lightTip = c.DarkAt(cap) >= 0.5f;
    const D2D1_COLOR_F back = lightTip ? D2D1::ColorF(0xF7F7FA, 0.97f) : D2D1::ColorF(0x1D1E23, 0.97f);
    const D2D1_COLOR_F edge = lightTip ? D2D1::ColorF(0x000000, 0.14f) : D2D1::ColorF(0xFFFFFF, 0.22f);
    const D2D1_COLOR_F shadow = D2D1::ColorF(0x000000, lightTip ? 0.14f : 0.36f);
    const D2D1_COLOR_F ink = lightTip ? D2D1::ColorF(0x111317, 0.98f) : D2D1::ColorF(0xFFFFFF, 0.98f);
    const float r = capH / 2.f;
    c.Fill(D2D1::RectF(cap.left, cap.top + 1.5f, cap.right, cap.bottom + 2.5f), r, shadow);
    c.Fill(cap, r, back);
    c.Stroke(cap, r, edge, 1.f);
    const bool halo = c.halo;
    c.halo = false;
    c.Text(tipText_, D2D1::RectF(cap.left + 10.f, cap.top, cap.right - 10.f, cap.bottom), sz, ink, DWRITE_FONT_WEIGHT_NORMAL, 1);
    c.halo = halo;
}

bool StartMenu::DismissPowerCard()
{
    powerOpen_ = false;
    powerSel_ = -1;
    pressed_.clear();
    surface_.Invalidate();
    return true;
}

void StartMenu::StepPowerCard(Canvas& c, float dt, bool& animating)
{
    if (!powerOpen_ && powerGrow_ <= 0)
        return;
    // The power card springs out of its corner as popups open (kPop) and folds back as the folder does.
    const float goal = powerOpen_ ? 1.f : 0.f;
    (powerOpen_ ? motion::kPop : kFolderClose).Step(powerGrow_, powerV_, goal, dt / 1000);
    if (motion::Spring::AtRest(powerGrow_, powerV_, goal) || (!powerOpen_ && powerGrow_ < 0.01f)) {
        powerGrow_ = goal;
        powerV_ = 0;
    } else {
        c.Continue();
        animating = true;
    }
}

void StartMenu::PaintPowerCard(Canvas& c, const Theme& t, std::vector<ShapeDip>& shapes)
{
    if (powerGrow_ <= 0)
        return;
    const D2D1_RECT_F full = PowerRect();
    const float k = 0.5f + 0.5f * powerGrow_;
    const float u = std::clamp(powerGrow_, 0.f, 1.f);
    const float glassAlpha = u * u * (3.f - 2.f * u);
    const D2D1_RECT_F pc = D2D1::RectF(full.right - (full.right - full.left) * k, full.bottom - (full.bottom - full.top) * k, full.right, full.bottom);
    const float r = PowerRadius() * std::min(k, 1.f);
    ShapeDip shape{pc, r};
    shape.fade = 1.f - glassAlpha;
    shapes.push_back(shape);
    if (c.panel)
        c.Fill(pc, r, Alpha(t.panel, glassAlpha));
    PaintPower(c, t);
}

// Power choices, in their own piece of glass above the power button.
void StartMenu::PaintPower(Canvas& c, const Theme& t)
{
    D2D1_RECT_F card = PowerRect();
    const float k = 0.5f + 0.5f * powerGrow_;
    // The rows grow with the card from its corner; they fade in a little behind it.
    c.BeginGroup(std::clamp((powerGrow_ - 0.15f) / 0.6f, 0.f, 1.f), k, card.right, card.bottom);
    constexpr float kRowInset = 8;
    const float rowR = std::max(4.f, PowerRadius() - kRowInset); // nested in the card's corners
    for (int i = 0; i < int(power_.size()); ++i) {
        D2D1_RECT_F row = D2D1::RectF(card.left + kRowInset, card.top + kPowerPad + kPowerRowH * float(i), card.right - kRowInset,
                                      card.top + kPowerPad + kPowerRowH * float(i + 1));
        std::wstring id = L"pw:" + std::to_wstring(i);
        float hv = Hover(c, id, hover_ == id || powerSel_ == i);
        if (hv > 0)
            c.Fill(row, rowR, powerSel_ == i && hover_ != id ? Alpha(t.accent, 0.24f * hv) : Alpha(t.hover, hv));
        c.Glyph(power_[size_t(i)].glyph, D2D1::RectF(row.left + 6, row.top, row.left + 38, row.bottom), 15, t.text);
        c.Text(power_[size_t(i)].name, D2D1::RectF(row.left + 44, row.top, row.right - 8, row.bottom), 14, t.text);
        if (powerOpen_)
            hits_.push_back({ScaleRectAbout(row, card.right, card.bottom, k), id});
    }
    c.EndGroup();
}

} // namespace atrium
