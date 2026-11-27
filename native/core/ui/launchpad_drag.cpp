// Launchpad's arranging & folder management: cross-page drag, drag-to-merge folders, drag out of
// custom folders, inline folder rename, and remove/restore (shell/launchpad_order.h).
#include "ui/launchpad.h"

#include "clock.h"
#include "localization.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace atrium {

namespace {

constexpr float kEdge = 40;      // DIP from the sheet's side where a lifted app turns the page
constexpr UINT kEdgeMs = 450;    // ... after waiting there this long
constexpr double kRestMs = 120;  // rest before neighbours slide aside
constexpr float kGlideRate = 16; // the tiles' approach to their places, per second
constexpr float kLiftScale = 1.12f;

void EraseFolderApp(LaunchpadLayout& layout, std::wstring_view folderId, std::wstring_view appId)
{
    for (size_t fi = 0; fi < layout.folders.size(); ++fi) {
        if (layout.folders[fi].id != folderId)
            continue;
        std::erase(layout.folders[fi].apps, appId);
        if (layout.folders[fi].apps.size() == 1) {
            const std::wstring survivor = layout.folders[fi].apps.front();
            auto at = std::ranges::find(layout.order, folderId);
            if (at != layout.order.end())
                *at = survivor;
            else
                layout.order.push_back(survivor);
            layout.folders.erase(layout.folders.begin() + ptrdiff_t(fi));
        } else if (layout.folders[fi].apps.empty()) {
            std::erase(layout.order, folderId);
            layout.folders.erase(layout.folders.begin() + ptrdiff_t(fi));
        }
        return;
    }
}

void CommitMerge(LaunchpadLayout& layout, const std::wstring& movedId, const std::wstring& targetId, bool targetIsCustomFolder, std::wstring& openedFolderId)
{
    std::erase(layout.order, movedId);
    for (LaunchpadFolder& f : layout.folders) {
        if (f.id == targetId && targetIsCustomFolder) {
            if (std::ranges::find(f.apps, movedId) == f.apps.end())
                f.apps.push_back(movedId);
            openedFolderId = f.id;
            return;
        }
    }
    LaunchpadFolder created{layout.NewFolderId(), Tr(L"New folder"), {targetId, movedId}};
    openedFolderId = created.id;
    layout.folders.push_back(std::move(created));
    auto at = std::ranges::find(layout.order, targetId);
    if (at != layout.order.end())
        *at = openedFolderId;
    else
        layout.order.push_back(openedFolderId);
}

} // namespace

void Launchpad::ApplyOrder()
{
    if (layout_.order.empty())
        return;
    std::unordered_map<std::wstring, size_t> rank;
    for (size_t i = 0; i < layout_.order.size(); ++i)
        rank.try_emplace(layout_.order[i], i);
    // Those the user placed, as placed; new ones after them, as they came.
    std::ranges::stable_sort(tiles_, {}, [&](const CatalogApp& a) {
        const auto it = rank.find(a.id);
        return it == rank.end() ? layout_.order.size() : it->second;
    });
}

std::vector<int> Launchpad::DragOrder() const
{
    const auto* folder = dragInFolder_ ? FolderApps(dragFolderId_) : nullptr;
    const std::vector<CatalogApp>& source = folder ? *folder : tiles_;
    const bool sameList = dragInFolder_ == liftedFromFolder_;
    const int n = int(source.size());
    const int itemCount = std::max(0, n - (sameList && lifted_ >= 0 && lifted_ < n ? 1 : 0));
    std::vector<int> out;
    out.reserve(size_t(itemCount) + 1);
    for (int i = 0; i < n; ++i)
        if (!sameList || i != lifted_)
            out.push_back(i);
    const int hole = std::clamp(dragInsert_, 0, int(out.size()));
    out.insert(out.begin() + hole, -1);
    return out;
}

void Launchpad::Lift()
{
    const auto& source = Shown();
    if (!down_ || dragging_ || pressed_ < 0 || !Arranging() || pressed_ >= int(source.size()))
        return;
    lifted_ = pressed_;
    liftedFromFolder_ = dragInFolder_ = IsCustomFolder(openFolder_);
    dragFolderId_ = openFolder_;
    liftedApp_ = source[size_t(lifted_)];
    liftedId_ = liftedApp_.id;
    dragInsert_ = lifted_;
    dragPendingSlot_ = mergeTarget_ = -1;
    dragEdge_ = 0;
    pressed_ = hover_ = -1;
    POINT p{};
    GetCursorPos(&p);
    p = surface_.ScreenToContent(p);
    liftAt_ = D2D1::Point2F(float(p.x) / surface_.Scale(), float(p.y) / surface_.Scale());
    SetTimer(surface_.Hwnd(), kDragTimer, 50, nullptr);
    surface_.Invalidate();
}

void Launchpad::MoveLifted(POINT pt)
{
    const float s = surface_.Scale();
    liftAt_ = D2D1::Point2F(float(pt.x) / s, float(pt.y) / s);
    UpdateDragTarget();
    surface_.Invalidate();
}

void Launchpad::UpdateDragTarget()
{
    const double now = ClockMs();
    if (dragInFolder_) {
        const float gridRight = grid_.gridLeft + float(grid_.cols) * grid_.cellW;
        const float gridBottom = grid_.gridTop + float(grid_.rows) * grid_.cellH;
        const bool out = liftAt_.x < grid_.gridLeft - 24 || liftAt_.x > gridRight + 24 || liftAt_.y < grid_.gridTop - 24 || liftAt_.y > gridBottom + 24;
        if (out) {
            dragInFolder_ = false;
            openFolder_.clear();
            auto it = std::ranges::find(tiles_, dragFolderId_, &CatalogApp::id);
            const int idx = it != tiles_.end() ? int(it - tiles_.begin()) : int(tiles_.size()) - 1;
            page_ = std::clamp(idx / std::max(1, grid_.perPage), 0, std::max(0, Pages() - 1));
            pageShown_ = float(page_);
            pageSpeed_ = 0;
            dragInsert_ = int(tiles_.size());
            dragPendingSlot_ = mergeTarget_ = -1;
            dragEdge_ = 0;
        }
    }
    const std::vector<int> order = DragOrder();
    const int itemCount = std::max(0, int(order.size()) - 1);
    const int col = std::clamp(int(std::floor((liftAt_.x - grid_.gridLeft) / grid_.cellW)), 0, grid_.cols - 1);
    const int row = std::clamp(int(std::floor((liftAt_.y - grid_.gridTop) / grid_.cellH)), 0, grid_.rows - 1);
    const int slot = page_ * grid_.perPage + row * grid_.cols + col;
    const float cx = grid_.gridLeft + float(col) * grid_.cellW + grid_.cellW * 0.5f;
    const float cy = grid_.gridTop + float(row) * grid_.cellH + grid_.cellH * 0.5f;
    const bool overIcon = std::fabs(liftAt_.x - cx) < grid_.iconDip * 0.55f && std::fabs(liftAt_.y - cy) < grid_.iconDip * 0.75f;
    const int occupant = slot >= 0 && slot < int(order.size()) ? order[size_t(slot)] : -1;
    const bool carriesFolder = FolderApps(liftedId_) != nullptr;
    const bool targetBuiltIn =
        occupant >= 0 && !dragInFolder_ && (tiles_[size_t(occupant)].id == kFolderId || tiles_[size_t(occupant)].id == kMicrosoftFolderId);
    const bool canMerge = !dragInFolder_ && !carriesFolder && overIcon && occupant >= 0 && !targetBuiltIn;
    mergeTarget_ = canMerge ? occupant : -1;
    if (canMerge) {
        dragPendingSlot_ = -1;
    } else {
        const int target = std::min(std::max(0, slot), itemCount);
        if (target == dragInsert_) {
            dragPendingSlot_ = -1;
        } else if (target != dragPendingSlot_) {
            dragPendingSlot_ = target;
            dragPendingMs_ = now;
        }
    }
    const float leftEdge = std::max(kEdge, grid_.gridLeft - 4.f);
    const float rightEdge = std::min(w_ - kEdge, grid_.gridLeft + float(grid_.cols) * grid_.cellW + 4.f);
    const int edge = liftAt_.x < leftEdge ? -1 : liftAt_.x > rightEdge ? 1 : 0;
    if (edge != dragEdge_) {
        dragEdge_ = edge;
        dragEdgeMs_ = now;
    }
}

void Launchpad::DragTick()
{
    if (lifted_ < 0)
        return;
    const double now = ClockMs();
    bool changed = false;
    if (dragPendingSlot_ >= 0 && now - dragPendingMs_ >= kRestMs) {
        dragInsert_ = dragPendingSlot_;
        dragPendingSlot_ = -1;
        changed = true;
    }
    if (dragEdge_ != 0 && now - dragEdgeMs_ >= kEdgeMs) {
        dragEdgeMs_ = now;
        const int next = std::clamp(page_ + dragEdge_, 0, Pages() - 1);
        if (next != page_) {
            SetPage(next);
            UpdateDragTarget();
            changed = true;
        }
    }
    if (changed)
        surface_.Invalidate();
}

void Launchpad::DropLifted()
{
    KillTimer(surface_.Hwnd(), kDragTimer);
    if (lifted_ < 0)
        return;
    const std::vector<int> order = DragOrder();
    const int mergeIdx = mergeTarget_;
    const bool inFolder = dragInFolder_, fromFolder = liftedFromFolder_;
    const std::wstring movedId = liftedId_, folderId = dragFolderId_;
    const D2D1_POINT_2F dropAt{liftAt_.x - grid_.cellW / 2 + float(page_) * w_, liftAt_.y - grid_.cellH / 2};
    lifted_ = -1;
    liftedFromFolder_ = dragInFolder_ = false;
    mergeTarget_ = dragPendingSlot_ = -1;
    dragEdge_ = 0;

    if (mergeIdx >= 0 && mergeIdx < int(tiles_.size())) {
        const std::wstring targetId = tiles_[size_t(mergeIdx)].id;
        const bool targetIsFolder = IsCustomFolder(targetId);
        if (layout_.order.empty())
            for (const CatalogApp& a : tiles_)
                layout_.order.push_back(a.id);
        if (fromFolder)
            EraseFolderApp(layout_, folderId, movedId);
        std::wstring createdOrTarget;
        CommitMerge(layout_, movedId, targetId, targetIsFolder, createdOrTarget);
        layout_.Clamp();
        SaveLaunchpadLayout(layout_);
        Split();
        if (!targetIsFolder && !createdOrTarget.empty()) {
            ShowFolder(createdOrTarget);
            StartFolderNaming();
            nameDraft_.clear();
        }
        Log(L"launchpad: merged %s into %s", movedId.c_str(), createdOrTarget.c_str());
    } else if (inFolder) {
        for (LaunchpadFolder& f : layout_.folders) {
            if (f.id != folderId)
                continue;
            const auto* cur = FolderApps(folderId);
            if (!cur)
                break;
            std::vector<std::wstring> next;
            next.reserve(order.size());
            for (int idx : order)
                next.push_back(idx < 0 ? movedId : (*cur)[size_t(idx)].id);
            f.apps = std::move(next);
            break;
        }
        SaveLaunchpadLayout(layout_);
        Split();
        tilePos_[movedId] = dropAt;
    } else {
        std::vector<std::wstring> nextOrder;
        nextOrder.reserve(order.size());
        for (int idx : order)
            nextOrder.push_back(idx < 0 ? movedId : tiles_[size_t(idx)].id);
        if (fromFolder)
            EraseFolderApp(layout_, folderId, movedId);
        layout_.order = std::move(nextOrder);
        layout_.Clamp();
        SaveLaunchpadLayout(layout_);
        Split();
        tilePos_[movedId] = dropAt;
        Log(L"launchpad: arranged (%zu root tiles)", layout_.order.size());
    }
    if (stale_) {
        stale_ = false;
        Refresh();
    } else {
        UpdatePageTargets(true);
        surface_.Invalidate();
    }
}

void Launchpad::HideApp(const CatalogApp& app)
{
    if (Hidden(app.id))
        return;
    if (layout_.order.empty())
        for (const CatalogApp& t : tiles_)
            layout_.order.push_back(t.id);
    if (IsCustomFolder(openFolder_))
        EraseFolderApp(layout_, openFolder_, app.id);
    std::erase(layout_.order, app.id);
    layout_.hidden.push_back(app.id);
    layout_.Clamp();
    SaveLaunchpadLayout(layout_);
    Split();
    if (!openFolder_.empty() && (!FolderApps(openFolder_) || FolderApps(openFolder_)->empty()))
        openFolder_.clear();
    if (!query_.empty())
        Filter();
    page_ = std::clamp(page_, 0, Pages() - 1);
    pageShown_ = float(page_);
    UpdatePageTargets(true);
    surface_.Invalidate();
    Log(L"launchpad: %s removed", app.name.c_str());
}

void Launchpad::RestoreApp(const std::wstring& id)
{
    std::erase(layout_.hidden, id);
    layout_.Clamp();
    SaveLaunchpadLayout(layout_);
    Split();
    if (!query_.empty())
        Filter();
    UpdatePageTargets(true);
    surface_.Invalidate();
    Log(L"launchpad: restored %s", id.c_str());
}

void Launchpad::RestoreAllHidden()
{
    if (layout_.hidden.empty())
        return;
    layout_.hidden.clear();
    SaveLaunchpadLayout(layout_);
    Split();
    if (!query_.empty())
        Filter();
    UpdatePageTargets(true);
    surface_.Invalidate();
    Log(L"launchpad: restored all removed apps");
}

void Launchpad::RemoveAppFromFolder(const std::wstring& folderId, const std::wstring& appId)
{
    if (!IsCustomFolder(folderId))
        return;
    if (layout_.order.empty())
        for (const CatalogApp& t : tiles_)
            layout_.order.push_back(t.id);
    EraseFolderApp(layout_, folderId, appId);
    auto at = std::ranges::find(layout_.order, folderId);
    if (at != layout_.order.end())
        layout_.order.insert(at + 1, appId);
    else if (std::ranges::find(layout_.order, appId) == layout_.order.end())
        layout_.order.push_back(appId);
    layout_.Clamp();
    SaveLaunchpadLayout(layout_);
    Split();
    if (!FolderApps(folderId) || FolderApps(folderId)->empty())
        openFolder_.clear();
    page_ = std::clamp(page_, 0, Pages() - 1);
    pageShown_ = float(page_);
    UpdatePageTargets(true);
    surface_.Invalidate();
}

void Launchpad::UngroupFolder(const std::wstring& folderId)
{
    if (!IsCustomFolder(folderId))
        return;
    if (layout_.order.empty())
        for (const CatalogApp& t : tiles_)
            layout_.order.push_back(t.id);
    std::vector<std::wstring> freed;
    for (size_t i = 0; i < layout_.folders.size(); ++i) {
        if (layout_.folders[i].id != folderId)
            continue;
        freed = std::move(layout_.folders[i].apps);
        layout_.folders.erase(layout_.folders.begin() + ptrdiff_t(i));
        break;
    }
    auto at = std::ranges::find(layout_.order, folderId);
    if (at != layout_.order.end()) {
        at = layout_.order.erase(at);
        layout_.order.insert(at, freed.begin(), freed.end());
    } else {
        layout_.order.insert(layout_.order.end(), freed.begin(), freed.end());
    }
    if (openFolder_ == folderId)
        openFolder_.clear(), namingFolder_ = false;
    layout_.Clamp();
    SaveLaunchpadLayout(layout_);
    Split();
    page_ = std::clamp(page_, 0, Pages() - 1);
    pageShown_ = float(page_);
    UpdatePageTargets(true);
    surface_.Invalidate();
    Log(L"launchpad: folder %s ungrouped", folderId.c_str());
}

void Launchpad::StartFolderNaming()
{
    if (!IsCustomFolder(openFolder_) || !query_.empty())
        return;
    namingFolder_ = true;
    searchFocused_ = false;
    nameDraft_ = FolderTitle(openFolder_);
    composition_.clear();
    if (const UINT blink = GetCaretBlinkTime(); blink != INFINITE)
        SetTimer(surface_.Hwnd(), kCaretTimer, std::max(1u, blink), nullptr);
    PlaceIme();
    surface_.Invalidate();
}

void Launchpad::FinishFolderNaming(bool commit)
{
    if (!namingFolder_)
        return;
    namingFolder_ = false;
    composition_.clear();
    if (commit && IsCustomFolder(openFolder_)) {
        const size_t a = nameDraft_.find_first_not_of(L" \t\r\n");
        const size_t b = nameDraft_.find_last_not_of(L" \t\r\n");
        const std::wstring clean = a == std::wstring::npos ? std::wstring{} : nameDraft_.substr(a, b - a + 1);
        for (LaunchpadFolder& f : layout_.folders)
            if (f.id == openFolder_) {
                f.name = clean;
                break;
            }
        SaveLaunchpadLayout(layout_);
        Split();
    }
    nameDraft_.clear();
    surface_.Invalidate();
}

// The tile's place on the sheet glides to its cell's; drawn where it is now on this page.
D2D1_RECT_F Launchpad::Glide(const std::wstring& id, D2D1_RECT_F cell, int page, float dx)
{
    const D2D1_POINT_2F goal = D2D1::Point2F(cell.left + float(page) * w_, cell.top);
    auto [it, added] = tilePos_.try_emplace(id, goal);
    D2D1_POINT_2F& p = it->second;
    if (!added && (std::abs(p.x - goal.x) > 0.3f || std::abs(p.y - goal.y) > 0.3f)) {
        p.x = motion::Approach(p.x, goal.x, glideMs_, kGlideRate);
        p.y = motion::Approach(p.y, goal.y, glideMs_, kGlideRate);
        gliding_ = true;
    } else {
        p = goal;
    }
    const float x = p.x - float(page) * w_ + dx;
    return D2D1::RectF(x, p.y, x + (cell.right - cell.left), p.y + (cell.bottom - cell.top));
}

void Launchpad::PaintLifted(Canvas& c, const Theme& t)
{
    if (lifted_ < 0 || !Arranging())
        return;
    const float w = grid_.cellW, h = grid_.cellH;
    const D2D1_RECT_F cell = D2D1::RectF(liftAt_.x - w / 2, liftAt_.y - h / 2, liftAt_.x + w / 2, liftAt_.y + h / 2);
    c.BeginGroup(0.92f, kLiftScale, liftAt_.x, liftAt_.y);
    PaintTile(c, t, liftedApp_, -1, cell);
    c.EndGroup();
    tilePos_.erase(liftedId_);
}

} // namespace atrium
