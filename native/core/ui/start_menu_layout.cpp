// Start menu geometry, sizing, folder card open/close, and tile drag/resize.
#include "ui/start_menu.h"

#include "bar_edge.h"
#include "clock.h"
#include "localization.h"
#include "log.h"
#include "ui/start_menu_metrics.h"
#include "ui/start_menu_sort.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace atrium {

namespace {

using namespace start_menu_detail;

// The arrangement in the order of `keys` (the tiles shown); tiles not shown
// (apps not found yet) keep to the end.
void Reorder(std::vector<StartTile>& tiles, const std::vector<std::wstring>& keys)
{
    std::vector<StartTile> out;
    for (const std::wstring& key : keys)
        if (auto it = std::ranges::find_if(tiles, [&](const StartTile& t) { return t.id == key; }); it != tiles.end()) {
            out.push_back(std::move(*it));
            tiles.erase(it);
        }
    for (StartTile& t : tiles)
        out.push_back(std::move(t));
    tiles = std::move(out);
}

StartTile* Find(std::vector<StartTile>& tiles, const std::wstring& id)
{
    auto it = std::ranges::find_if(tiles, [&](const StartTile& t) { return t.id == id; });
    return it == tiles.end() ? nullptr : &*it;
}

} // namespace

// ---- Geometry ----

float StartMenu::W() const { return panelW_; }
float StartMenu::H() const { return mainH_ + kGap + barH_; }
float StartMenu::IconSize() const { return kStartIcons[layout_.icon].icon; }
float StartMenu::CellW() const { return kStartIcons[layout_.icon].cellW; }
float StartMenu::CellH() const { return kStartIcons[layout_.icon].cellH; }
float StartMenu::LabelSize() const { return kStartIcons[layout_.icon].label; }
float StartMenu::LabelH() const { return kStartIcons[layout_.icon].labelH; }
// Icons are read once, at the largest size (kStartIcons' last): a smaller size
// draws the same bitmap smaller, so changing the size never reads a file.
int StartMenu::IconPx() const { return int(std::lround(kStartIcons[StartLayout::kSizes - 1].icon * PopupScale(bar_) * 1.1f)); }
// The largest size's icons, for the read-ahead before the menu knows its own
// (main.cpp): every icon is read once at that size and drawn smaller as needed.
int StartMenu::TileIconPx(float scale) { return int(std::lround(kStartIcons[StartLayout::kSizes - 1].icon * scale * 1.1f)); }

D2D1_SIZE_F StartMenu::SizeFor(int grid, int icons) const
{
    const StartGrid& g = kStartGrids[grid];
    const StartIconSize& s = kStartIcons[icons];
    return D2D1::SizeF(float(g.cols) * s.cellW + 2 * kGridPad, kGridTop + float(g.rows) * s.cellH + kGridFoot + kGap + barH_);
}

// The window: the largest grid the work area has room for at these icons, so
// that either size may grow later without the menu leaving the window. The
// panel inside it springs to the size chosen (PanelSize).
D2D1_SIZE_F StartMenu::Size() const { return SizeFor(room_, layout_.icon); }
D2D1_SIZE_F StartMenu::PanelSize() const { return SizeFor(grid_, layout_.icon); }

float StartMenu::OffsetY() const
{
    const BarEdge edge = EdgeOf(bar_);
    const float winH = float(surface_.Rect().bottom - surface_.Rect().top) / surface_.Scale();
    if (edge == BarEdge::Bottom)
        return winH - H();
    return Vertical(edge) && centered_ ? std::max(0.f, (winH - H()) / 2) : 0.f;
}

LONG StartMenu::OffsetPx() const { return std::lround(OffsetY() * surface_.Scale()); }

float StartMenu::OffsetX() const
{
    const BarEdge edge = EdgeOf(bar_);
    const float winW = float(surface_.Rect().right - surface_.Rect().left) / surface_.Scale();
    if (edge == BarEdge::Right)
        return winW - W();
    return !Vertical(edge) && centered_ ? std::max(0.f, (winW - W()) / 2) : 0.f;
}

LONG StartMenu::OffsetXPx() const { return std::lround(OffsetX() * surface_.Scale()); }

bool StartMenu::Fits(int grid, int icons) const
{
    const D2D1_SIZE_F z = SizeFor(grid, icons);
    const float scale = PopupScale(bar_);
    const RECT r = PopupRect(bar_, z, Align());
    return r.bottom - r.top >= std::lround(z.height * scale) - 1 && r.right - r.left >= std::lround(z.width * scale) - 1;
}

// The largest grid that fits at these icons: the window's size, and the most
// the menu may be grown to.
void StartMenu::UpdateRoom()
{
    room_ = 0;
    for (int grid = 1; grid < StartLayout::kSizes; ++grid)
        if (Fits(grid, layout_.icon))
            room_ = grid;
}

// The menu's size and its icons' size are separate choices; either may change
// on its own. The grid and the cells take it at once (the tiles glide to their
// places) and the glass springs to it. No icon is read: they are all kept at
// the largest size and drawn smaller when needed.
void StartMenu::SetSize(int grid, int icons)
{
    // Bigger icons leave room for fewer tiles: the window is measured again,
    // and the menu kept inside it. The choice itself is kept as made (it comes
    // back when the icons leave room for it again).
    layout_.icon = std::clamp(icons, 0, StartLayout::kSizes - 1);
    layout_.size = std::clamp(grid, 0, StartLayout::kSizes - 1);
    UpdateRoom();
    grid_ = std::min(layout_.size, room_);
    cols_ = kStartGrids[grid_].cols;
    rows_ = kStartGrids[grid_].rows;
    page_ = std::clamp(page_, 0, Pages() - 1);
    folderPage_ = std::clamp(folderPage_, 0, FolderPages() - 1);
    // The window follows the icons (the panel springs inside it, it does not).
    if (surface_.Visible()) {
        const RECT want = PopupRect(bar_, Size(), Align());
        const RECT have = surface_.Rect();
        if (!EqualRect(&want, &have))
            surface_.Move(want);
        // Smaller icons shrink the window: the panel takes the new bounds at
        // once, so no frame of the spring is drawn outside them.
        const D2D1_SIZE_F z = Size();
        panelW_ = std::min(panelW_, z.width);
        mainH_ = std::min(mainH_, z.height - kGap - barH_);
    }
    surface_.Invalidate();
}

D2D1_RECT_F StartMenu::MainRect() const { return D2D1::RectF(0, 0, W(), H() - barH_ - kGap); }
D2D1_RECT_F StartMenu::ShelfRect() const { return D2D1::RectF(0, H() - barH_, W(), H()); }
D2D1_RECT_F StartMenu::AllButton() const { return D2D1::RectF(W() - kSearchX - kAllW, kSearchTop, W() - kSearchX, kSearchTop + kSearchH); }
D2D1_RECT_F StartMenu::SyncButton() const { return D2D1::RectF(AllButton().left - 8 - kSearchH, kSearchTop, AllButton().left - 8, kSearchTop + kSearchH); }
D2D1_RECT_F StartMenu::SearchRect() const { return D2D1::RectF(kSearchX, kSearchTop, SyncButton().left - 8, kSearchTop + kSearchH); }
float StartMenu::ListBottom() const { return MainRect().bottom - 44; }
float StartMenu::GridX() const { return (W() - float(cols_) * CellW()) / 2; }
float StartMenu::ButtonSize() const { return std::min(44.f, barH_ - 12); }

D2D1_RECT_F StartMenu::Cell(int slot) const
{
    const int per = PerPage(), page = slot / per, k = slot % per, row = k / cols_;
    const float cw = CellW(), ch = CellH();
    const float x = GridX() + float(k % cols_) * cw + float(page) * W(), y = kGridTop + float(row) * ch;
    return D2D1::RectF(x, y, x + cw, y + ch);
}

D2D1_RECT_F StartMenu::FolderRect() const
{
    // Just big enough for its apps (pages of them beyond), centred over the grid.
    const float w = float(FolderCols()) * CellW() + 2 * kFolderPad, h = kFolderHead + float(FolderRows()) * CellH() + kFolderFoot;
    const float cx = W() / 2, cy = (kListTop - 10 + MainRect().bottom - 14) / 2;
    return D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
}

int StartMenu::FolderApps() const { return folder_ >= 0 && folder_ < int(tiles_.size()) ? int(tiles_[size_t(folder_)].apps.size()) : 4; }
int StartMenu::FolderCols() const { return std::clamp(FolderApps(), 3, 4); }

int StartMenu::FolderRows() const
{
    const float room = MainRect().bottom - 14 - (kListTop - 10) - kFolderHead - kFolderFoot;
    return std::clamp((FolderApps() + FolderCols() - 1) / FolderCols(), 1, std::max(1, int(room / CellH())));
}

D2D1_RECT_F StartMenu::IndexRect() const { return D2D1::RectF(10, kListTop + 2, 10 + kIndexW, ListBottom() - 2); }

D2D1_RECT_F StartMenu::FolderCell(int slot) const
{
    const D2D1_RECT_F f = FolderRect();
    const int cols = FolderCols(), per = cols * FolderRows(), page = slot / per, k = slot % per, row = k / cols;
    const float cw = CellW(), ch = CellH();
    const float x0 = (f.left + f.right - float(cols) * cw) / 2;
    const float x = x0 + float(k % cols) * cw + float(page) * (f.right - f.left), y = f.top + kFolderHead + float(row) * ch;
    return D2D1::RectF(x, y, x + cw, y + ch);
}

D2D1_RECT_F StartMenu::PowerRect() const
{
    const D2D1_RECT_F main = MainRect();
    const float w = std::ranges::any_of(power_, [](const PowerRow& row) { return row.name.size() > 14; }) ? 268.f : kPowerW;
    const float right = main.right - kPowerInset, bottom = main.bottom - kPowerInset, h = float(power_.size()) * kPowerRowH + 2 * kPowerPad;
    return D2D1::RectF(right - w, bottom - h, right, bottom);
}

float StartMenu::PowerRadius() const { return std::max(12.f, radius_ - kPowerInset); }

float StartMenu::Covered(const D2D1_RECT_F& r) const
{
    // Content under the power card fades as it opens: the card's glass is
    // below the content layer, so the content would show through it.
    if (powerGrow_ <= 0)
        return 1;
    D2D1_RECT_F p = PowerRect();
    if (r.right <= p.left || r.left >= p.right || r.bottom <= p.top || r.top >= p.bottom)
        return 1;
    return 1 - std::clamp(powerGrow_ * 1.6f, 0.f, 1.f);
}

// ---- Folders ----

void StartMenu::OpenFolder(int tile)
{
    if (tile < 0 || tile >= int(tiles_.size()) || !tiles_[size_t(tile)].folder)
        return;
    // The card grows out of the folder's tile when the grid shows it, else out of the panel's middle.
    const D2D1_RECT_F card = FolderRect();
    const float icon = IconSize();
    const float mx = (card.left + card.right) / 2, my = (card.top + card.bottom) / 2;
    folderFrom_ = D2D1::RectF(mx - icon / 2, my - icon / 2, mx + icon / 2, my + icon / 2);
    const std::vector<int> order = Order();
    if (auto it = std::ranges::find(order, tile); it != order.end() && !Listing()) {
        D2D1_RECT_F cell = Cell(int(it - order.begin()));
        const float x = cell.left - float(page_) * W(), cx = x + CellW() / 2;
        if (x >= 0 && x + CellW() <= W())
            folderFrom_ = D2D1::RectF(cx - icon / 2, cell.top + 8, cx + icon / 2, cell.top + 8 + icon);
    }
    if (folder_ != tile)
        folderGrow_ = folderV_ = 0;
    folder_ = tile;
    folderClosing_ = false;
    folderMs_ = ClockMs();
    folderPage_ = 0;
    folderPageShown_ = 0;
    naming_ = false;
    sel_ = -1;
    std::erase_if(tilePos_, [](const auto& p) { return p.first.starts_with(L"f|"); });
    surface_.Invalidate();
    Log(L"start: folder '%s' opened (%zu apps)", tiles_[size_t(tile)].name.c_str(), tiles_[size_t(tile)].apps.size());
}

void StartMenu::CloseFolder()
{
    if (folder_ < 0 || folderClosing_)
        return;
    FinishNaming(true);
    if (folder_ < 0)
        return;
    // Back into its tile (which may have moved meanwhile).
    const std::vector<int> order = Order();
    if (auto it = std::ranges::find(order, folder_); it != order.end() && !Listing()) {
        D2D1_RECT_F cell = Cell(int(it - order.begin()));
        const float icon = IconSize();
        const float x = cell.left - float(page_) * W(), cx = x + CellW() / 2;
        if (x >= 0 && x + CellW() <= W())
            folderFrom_ = D2D1::RectF(cx - icon / 2, cell.top + 8, cx + icon / 2, cell.top + 8 + icon);
    }
    folderClosing_ = true;
    sel_ = -1;
    surface_.Invalidate();
}

wchar_t StartMenu::TopLetter() const
{
    wchar_t letter = 0;
    float y = 0;
    for (const Row& row : list_) {
        if (row.letter) {
            if (y > scroll_ + 1)
                break;
            letter = row.letter;
        }
        y += row.letter ? kHeadH : kRowH;
    }
    return letter;
}

void StartMenu::ScrubTo(float y)
{
    const D2D1_RECT_F bar = IndexRect();
    const int n = int(std::size(kLetters)) - 1;
    const int i = std::clamp(int((y - bar.top) / ((bar.bottom - bar.top) / float(n))), 0, n - 1);
    scrubY_ = std::clamp(y, bar.top, bar.bottom);
    // The letter's heading, or the next one with apps under it (past the last: the last).
    float top = 0, at = -1;
    wchar_t found = 0;
    for (const Row& row : list_) {
        if (row.letter && row.letter != kStartMenuHiddenHead) {
            if (StartMenuLetterRank(row.letter) >= StartMenuLetterRank(kLetters[i]) && !found) {
                found = row.letter;
                at = top;
            }
            if (!found)
                at = top, scrubLetter_ = row.letter;
        }
        top += row.letter ? kHeadH : kRowH;
    }
    if (found)
        scrubLetter_ = found;
    if (at >= 0)
        scrollTarget_ = std::clamp(at, 0.f, std::max(0.f, contentH_ - (ListBottom() - kListTop)));
    surface_.Invalidate();
}

// ---- Carrying tiles ----

std::vector<int> StartMenu::Order() const
{
    std::vector<int> order;
    for (int i = 0; i < int(tiles_.size()); ++i)
        if (!drag_.on || drag_.fromFolder || i != drag_.from)
            order.push_back(i);
    if (drag_.on && !drag_.inFolder)
        order.insert(order.begin() + std::clamp(drag_.insert, 0, int(order.size())), -1);
    return order;
}

std::vector<int> StartMenu::FolderOrder() const
{
    std::vector<int> order;
    if (folder_ < 0)
        return order;
    const bool carried = drag_.on && drag_.fromFolder && drag_.folderTile == folder_;
    for (int i = 0; i < int(tiles_[size_t(folder_)].apps.size()); ++i)
        if (!carried || i != drag_.from)
            order.push_back(i);
    if (carried && drag_.inFolder)
        order.insert(order.begin() + std::clamp(drag_.insert, 0, int(order.size())), -1);
    return order;
}

void StartMenu::BeginDrag()
{
    if (naming_)
        return;
    const float s = surface_.Scale(), px = float(pressPt_.x) / s, py = float(pressPt_.y) / s;
    Drag d;
    d.on = true;
    D2D1_RECT_F cell{};
    if (pressed_.starts_with(L"tile:")) {
        d.from = Index(pressed_, 5);
        if (d.from < 0 || d.from >= int(tiles_.size()))
            return;
        d.key = tiles_[size_t(d.from)].key;
        const std::vector<int> order = Order();
        const int slot = int(std::ranges::find(order, d.from) - order.begin());
        d.insert = slot;
        cell = Cell(slot);
        cell.left -= float(page_) * W();
    } else {
        d.from = Index(pressed_, 5);
        if (folder_ < 0 || d.from < 0 || d.from >= int(tiles_[size_t(folder_)].apps.size()))
            return;
        d.fromFolder = d.inFolder = true;
        d.folderTile = folder_;
        d.key = L"f|" + tiles_[size_t(folder_)].apps[size_t(d.from)].id;
        d.insert = d.from;
        cell = FolderCell(d.from);
        cell.left -= float(folderPage_) * (FolderRect().right - FolderRect().left);
    }
    d.x = px;
    d.y = py;
    d.grabX = px - cell.left;
    d.grabY = py - cell.top;
    drag_ = d;
    pressed_.clear();
    hover_.clear();
    sel_ = -1;
    SetTimer(surface_.Hwnd(), kDragTimer, 50, nullptr);
    surface_.Invalidate();
}

void StartMenu::UpdateDragHover(int cols, int rows, float x0, float top, int page, double now)
{
    const float lx = drag_.x - x0, ly = drag_.y - top;
    const float cw = CellW(), ch = CellH(), icon = IconSize();
    const int col = std::clamp(int(std::floor(lx / cw)), 0, cols - 1), row = std::clamp(int(std::floor(ly / ch)), 0, rows - 1);
    const int slot = page * cols * rows + row * cols + col;
    const std::vector<int> order = drag_.inFolder ? FolderOrder() : Order();
    const float cx = x0 + float(col) * cw + cw / 2, cy = top + float(row) * ch + ch / 2;
    const bool overIcon = std::fabs(drag_.x - cx) < icon * 0.55f && std::fabs(drag_.y - cy) < icon * 0.75f;
    const int occupant = slot < int(order.size()) ? order[size_t(slot)] : -1;
    // Onto another tile: a folder of the two (or into that folder). Folders do not go into folders.
    const bool carriesFolder = !drag_.fromFolder && tiles_[size_t(drag_.from)].folder;
    const bool merge = overIcon && occupant >= 0 && !drag_.inFolder && !carriesFolder;
    drag_.merge = merge ? occupant : -1;
    if (merge) {
        drag_.pendingSlot = -1;
    } else {
        // Elsewhere: the others make room there once the pointer rests (passing over
        // tiles does not shuffle them). Not past the last tile.
        const int items = int(order.size()) - int(std::count(order.begin(), order.end(), -1));
        const int target = std::min(slot, items);
        if (target == drag_.insert)
            drag_.pendingSlot = -1;
        else if (target != drag_.pendingSlot) {
            drag_.pendingSlot = target;
            drag_.pendingMs = now;
        }
    }
    // Resting beside the grid turns the page.
    const bool level = drag_.y > top - 10 && drag_.y < top + float(rows) * ch;
    const int edge = !level ? 0 : drag_.x < x0 - 4 ? -1 : drag_.x > x0 + float(cols) * cw + 4 ? 1 : 0;
    if (edge != drag_.edge) {
        drag_.edge = edge;
        drag_.edgeMs = now;
    }
}

void StartMenu::UpdateDrag(POINT client)
{
    const float s = surface_.Scale();
    drag_.x = float(client.x) / s;
    drag_.y = float(client.y) / s;
    const double now = ClockMs();
    if (drag_.inFolder) {
        // Carried off the folder's card: the folder closes, and the app goes onto the grid.
        const D2D1_RECT_F card = FolderRect();
        if (!In(D2D1::RectF(card.left - 12, card.top - 12, card.right + 12, card.bottom + 12), drag_.x, drag_.y)) {
            drag_.inFolder = false;
            drag_.insert = int(tiles_.size());
            drag_.pendingSlot = -1;
            CloseFolder();
        }
    }
    if (drag_.inFolder) {
        const D2D1_RECT_F card = FolderRect();
        const int cols = FolderCols();
        UpdateDragHover(cols, FolderRows(), (card.left + card.right - float(cols) * CellW()) / 2, card.top + kFolderHead, folderPage_, now);
    } else {
        UpdateDragHover(cols_, rows_, GridX(), kGridTop, page_, now);
    }
    surface_.Invalidate();
}

void StartMenu::DragTick()
{
    if (!drag_.on)
        return;
    const double now = ClockMs();
    bool changed = false;
    if (drag_.pendingSlot >= 0 && now - drag_.pendingMs >= kRestMs) {
        drag_.insert = drag_.pendingSlot;
        drag_.pendingSlot = -1;
        changed = true;
    }
    if (drag_.edge && now - drag_.edgeMs >= kEdgeMs) {
        drag_.edgeMs = now;
        if (drag_.inFolder) {
            const int next = std::clamp(folderPage_ + drag_.edge, 0, FolderPages() - 1);
            changed |= next != folderPage_;
            folderPage_ = next;
        } else {
            const int next = std::clamp(page_ + drag_.edge, 0, Pages() - 1);
            changed |= next != page_;
            page_ = next;
        }
        // On the new page, the slot under the pointer.
        const float s = surface_.Scale();
        UpdateDrag(POINT{LONG(drag_.x * s), LONG(drag_.y * s)});
    }
    if (changed)
        surface_.Invalidate();
}

void StartMenu::CommitFolderMerge(const std::wstring& appId, const std::wstring& targetKey, bool targetIsFolder)
{
    std::wstring made;
    Arrange([&](std::vector<StartTile>& tiles) {
        std::optional<StartTile> moved = TakeTile(tiles, appId);
        if (!moved)
            return;
        StartTile* target = Find(tiles, targetKey);
        if (!target)
            tiles.push_back(std::move(*moved));
        else if (target->Folder())
            target->apps.push_back(std::move(*moved));
        else {
            StartTile folder{layout_.NewFolderId(), Tr(L"New folder"), {std::move(*target), std::move(*moved)}};
            made = folder.id;
            *target = std::move(folder);
        }
    });
    Log(L"start: %s put into %s%s", appId.c_str(), targetIsFolder ? L"folder " : L"a new folder with ", targetKey.c_str());
    for (size_t i = 0; i < tiles_.size() && !made.empty(); ++i)
        if (tiles_[i].key == made) {
            OpenFolder(int(i));
            StartNaming();
            nameDraft_.clear();
        }
}

void StartMenu::CommitDrag(const Drag& d, const std::wstring& appId, const std::wstring& targetKey, const std::wstring& folderKey, bool targetIsFolder,
                           const std::vector<std::wstring>& keys)
{
    if (!targetKey.empty()) {
        CommitFolderMerge(appId, targetKey, targetIsFolder);
        return;
    }
    if (d.inFolder) {
        Arrange([&](std::vector<StartTile>& tiles) {
            if (StartTile* f = Find(tiles, folderKey)) {
                std::vector<StartTile> apps;
                for (const std::wstring& key : keys)
                    if (auto it = std::ranges::find_if(f->apps, [&](const StartTile& a) { return a.id == key; }); it != f->apps.end())
                        apps.push_back(*it);
                if (apps.size() == f->apps.size())
                    f->apps = std::move(apps);
            }
        });
        return;
    }
    Arrange([&](std::vector<StartTile>& tiles) {
        if (d.fromFolder)
            if (std::optional<StartTile> moved = TakeTile(tiles, appId))
                tiles.push_back(std::move(*moved));
        Reorder(tiles, keys);
    });
    Log(L"start: %s moved%s", appId.c_str(), d.fromFolder ? L" out of its folder" : L"");
}

void StartMenu::EndDrag(bool commit)
{
    KillTimer(surface_.Hwnd(), kDragTimer);
    const Drag d = drag_;
    std::vector<std::wstring> keys;
    std::wstring appId, targetKey, folderKey;
    bool targetIsFolder = false;
    if (d.on && commit) {
        if (d.fromFolder) {
            folderKey = tiles_[size_t(d.folderTile)].key;
            appId = tiles_[size_t(d.folderTile)].apps[size_t(d.from)].id;
        } else {
            appId = tiles_[size_t(d.from)].key;
        }
        if (d.merge >= 0) {
            targetKey = tiles_[size_t(d.merge)].key;
            targetIsFolder = tiles_[size_t(d.merge)].folder;
        } else if (d.inFolder) {
            for (int i : FolderOrder())
                keys.push_back(i < 0 ? appId : tiles_[size_t(d.folderTile)].apps[size_t(i)].id);
        } else {
            for (int i : Order())
                keys.push_back(i < 0 ? appId : tiles_[size_t(i)].key);
        }
    }
    drag_ = {};
    if (GetCapture() == surface_.Hwnd())
        ReleaseCapture();
    if (stale_) {
        stale_ = false;
        apps_ = catalog_.Apps();
        pins_ = catalog_.StartPins();
        pinGroups_ = catalog_.StartPinGroups();
        Rebuild();
    }
    if (d.on && commit) {
        const float dropX = d.x - d.grabX, dropY = d.y - d.grabY;
        if (d.inFolder)
            tilePos_[d.key] = D2D1::Point2F(dropX + float(folderPage_) * (FolderRect().right - FolderRect().left), dropY);
        else
            tilePos_[d.fromFolder ? appId : d.key] = D2D1::Point2F(dropX + float(page_) * W(), dropY);
        CommitDrag(d, appId, targetKey, folderKey, targetIsFolder, keys);
    } else {
        surface_.Invalidate();
    }
    if (pendingSync_.on) {
        auto sync = pendingSync_;
        pendingSync_.on = false;
        SyncFromWindows(sync.pins, sync.folders, false);
    }
}

// ---- Resizing ----

void StartMenu::Resize(POINT screen)
{
    // The size nearest to where the corner is pulled (of those the screen has room for).
    const float s = surface_.Scale();
    const D2D1_SIZE_F from = SizeFor(sizeStart_, layout_.icon);
    const BarEdge edge = EdgeOf(bar_);
    const float dx = float(edge == BarEdge::Right ? sizeFrom_.x - screen.x : screen.x - sizeFrom_.x) / s;
    const float dy = float(edge == BarEdge::Bottom ? sizeFrom_.y - screen.y : screen.y - sizeFrom_.y) / s;
    // Centred, the corner moves half as far as the menu grows.
    const float w = from.width + (!Vertical(edge) && centered_ ? 2.f : 1.f) * dx, h = from.height + (Vertical(edge) && centered_ ? 2.f : 1.f) * dy;
    int best = grid_;
    float nearest = 1e9f;
    for (int grid = 0; grid <= room_; ++grid) {
        const D2D1_SIZE_F z = SizeFor(grid, layout_.icon);
        if (float d = std::hypot(z.width - w, z.height - h); d < nearest) {
            nearest = d;
            best = grid;
        }
    }
    if (best != grid_) {
        const int preferred = layout_.size;
        SetSize(best, layout_.icon);
        layout_.size = preferred; // the grip previews; EndResize commits the choice
    }
}

void StartMenu::EndResize()
{
    if (!sizing_)
        return;
    sizing_ = false;
    if (GetCapture() == surface_.Hwnd())
        ReleaseCapture();
    if (grid_ != sizeStart_) {
        layout_.size = grid_;
        SaveStartLayout(layout_);
        Log(L"start: sized to %d x %d tiles", cols_, rows_);
        if (onSize)
            onSize(SizeName());
    }
    surface_.Invalidate();
}

} // namespace atrium
