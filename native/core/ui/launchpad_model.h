// Pure layout, paging, keyboard navigation, and tile-mutation logic for the Launchpad overlay.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

struct LaunchpadGrid {
    int cols = 0;
    int rows = 0;
    int perPage = 0;
    float iconDip = 0.0f;
    float cellW = 0.0f;
    float cellH = 0.0f;
    float gridLeft = 0.0f;
    float gridTop = 0.0f;
};

struct LaunchpadTile {
    std::wstring id;
    std::wstring name;
    bool folder = false;
    std::vector<std::wstring> appIds;
};

struct LaunchpadSlice {
    size_t begin = 0;
    size_t end = 0;
};

// fullscreen: the whole screen, as macOS's: seven by five, larger icons, room for the bar below.
inline LaunchpadGrid ComputeLaunchpadGrid(float widthDip, float heightDip, bool fullscreen = false)
{
    const float kTopReserve = fullscreen ? 120.0f : 96.0f;
    const float kBottomReserve = fullscreen ? 140.0f : 64.0f;
    const float kMaxCellW = fullscreen ? 240.0f : 168.0f;
    const float kMaxCellH = fullscreen ? 184.0f : 144.0f;

    LaunchpadGrid grid;
    if (fullscreen) {
        grid.cols = 7;
        grid.rows = 5;
        grid.iconDip = std::clamp(std::min(widthDip / 7.0f * 0.42f, (heightDip - kTopReserve - kBottomReserve) / 5.0f * 0.6f), 64.0f, 104.0f);
    } else if (widthDip >= 1200.0f && heightDip >= 720.0f) {
        grid.cols = 7;
        grid.rows = 5;
        grid.iconDip = 64.0f;
    } else if (widthDip >= 900.0f && heightDip >= 540.0f) {
        grid.cols = 6;
        grid.rows = 4;
        grid.iconDip = 56.0f;
    } else {
        grid.cols = 5;
        grid.rows = 4;
        grid.iconDip = 52.0f;
    }
    grid.perPage = grid.cols * grid.rows;

    const float sideMargin = fullscreen ? widthDip * 0.1f : std::clamp(widthDip * 0.06f, 16.0f, 96.0f);
    const float availW = std::max(1.0f, widthDip - 2.0f * sideMargin);
    const float availH = std::max(1.0f, heightDip - kTopReserve - kBottomReserve);

    grid.cellW = std::min(availW / static_cast<float>(grid.cols), kMaxCellW);
    grid.cellH = std::min(availH / static_cast<float>(grid.rows), kMaxCellH);

    const float gridW = static_cast<float>(grid.cols) * grid.cellW;
    const float gridH = static_cast<float>(grid.rows) * grid.cellH;
    grid.gridLeft = (widthDip - gridW) * 0.5f;
    grid.gridTop = kTopReserve + (availH - gridH) * 0.5f;
    return grid;
}

inline int PageCount(size_t count, int perPage)
{
    if (count == 0 || perPage <= 0)
        return 0;
    return static_cast<int>((count + static_cast<size_t>(perPage) - 1) / static_cast<size_t>(perPage));
}

inline LaunchpadSlice PageSlice(size_t count, int page, int perPage)
{
    if (count == 0 || perPage <= 0 || page < 0)
        return {};
    const size_t begin = static_cast<size_t>(page) * static_cast<size_t>(perPage);
    if (begin >= count)
        return {};
    return {begin, std::min(count, begin + static_cast<size_t>(perPage))};
}

// Keep keyboard focus only on a tile visible on the destination page.
inline int SelectionOnPage(int sel, size_t count, int page, int perPage)
{
    const auto slice = PageSlice(count, page, perPage);
    return sel >= 0 && static_cast<size_t>(sel) >= slice.begin && static_cast<size_t>(sel) < slice.end ? sel : -1;
}

inline int MoveSelection(int sel, int dx, int dy, int count, int cols, int rows, int& page)
{
    if (count <= 0 || cols <= 0 || rows <= 0) {
        page = 0;
        return -1;
    }
    const int perPage = cols * rows;
    const int pages = PageCount(static_cast<size_t>(count), perPage);
    page = std::clamp(page, 0, pages - 1);

    if (sel < 0 || sel >= count) {
        if (dx == 0 && dy == 0)
            return -1;
        const int pageStart = page * perPage;
        const int pageEnd = std::min(count - 1, pageStart + perPage - 1);
        return (dx < 0 || dy < 0) ? pageEnd : pageStart;
    }

    int curPage = sel / perPage;
    const int local = sel % perPage;
    int row = local / cols;
    int col = local % cols;

    if (dy != 0)
        row = std::clamp(row + dy, 0, rows - 1);

    col += dx;
    if (col >= cols) {
        if (curPage + 1 < pages) {
            ++curPage;
            col = 0;
        } else {
            col = cols - 1;
        }
    } else if (col < 0) {
        if (curPage > 0) {
            --curPage;
            col = cols - 1;
        } else {
            col = 0;
        }
    }

    const int nextSel = std::min(count - 1, curPage * perPage + row * cols + col);
    page = nextSel / perPage;
    return nextSel;
}

// Negative dragFraction / velocityPagesPerSec drags left to advance to the next page.
inline int SettlePageDrag(int curPage, int pageCount, float dragFraction, float velocityPagesPerSec)
{
    if (pageCount <= 0)
        return 0;
    constexpr float kDistanceThreshold = 0.25f;
    constexpr float kFlingVelocity = 1.0f;

    int target = curPage;
    if (std::abs(velocityPagesPerSec) >= kFlingVelocity)
        target = velocityPagesPerSec < 0.0f ? curPage + 1 : curPage - 1;
    else if (dragFraction < -kDistanceThreshold)
        target = curPage + 1;
    else if (dragFraction > kDistanceThreshold)
        target = curPage - 1;

    return std::clamp(target, 0, pageCount - 1);
}

inline bool ReorderTiles(std::vector<LaunchpadTile>& tiles, size_t from, size_t to)
{
    if (from >= tiles.size() || to >= tiles.size() || from == to)
        return false;
    if (from < to)
        std::rotate(tiles.begin() + static_cast<std::ptrdiff_t>(from), tiles.begin() + static_cast<std::ptrdiff_t>(from + 1),
                    tiles.begin() + static_cast<std::ptrdiff_t>(to + 1));
    else
        std::rotate(tiles.begin() + static_cast<std::ptrdiff_t>(to), tiles.begin() + static_cast<std::ptrdiff_t>(from),
                    tiles.begin() + static_cast<std::ptrdiff_t>(from + 1));
    return true;
}

inline bool MergeIntoFolder(std::vector<LaunchpadTile>& tiles, size_t from, size_t target, const std::wstring& folderId, const std::wstring& defaultName)
{
    if (from >= tiles.size() || target >= tiles.size() || from == target || tiles[from].folder)
        return false;

    auto appIdOf = [](const LaunchpadTile& tile) -> const std::wstring& { return !tile.appIds.empty() ? tile.appIds.front() : tile.id; };

    const std::wstring& draggedId = appIdOf(tiles[from]);
    if (tiles[target].folder) {
        auto& ids = tiles[target].appIds;
        if (std::ranges::find(ids, draggedId) == ids.end())
            ids.push_back(draggedId);
    } else {
        LaunchpadTile folder;
        folder.id = folderId;
        folder.name = defaultName;
        folder.folder = true;
        folder.appIds = {appIdOf(tiles[target]), draggedId};
        tiles[target] = std::move(folder);
    }
    tiles.erase(tiles.begin() + static_cast<std::ptrdiff_t>(from));
    return true;
}

inline bool RemoveFromFolder(std::vector<LaunchpadTile>& tiles, size_t folderIdx, size_t appIdx)
{
    if (folderIdx >= tiles.size())
        return false;
    LaunchpadTile& tile = tiles[folderIdx];
    if (!tile.folder || appIdx >= tile.appIds.size())
        return false;

    tile.appIds.erase(tile.appIds.begin() + static_cast<std::ptrdiff_t>(appIdx));
    if (tile.appIds.empty()) {
        tiles.erase(tiles.begin() + static_cast<std::ptrdiff_t>(folderIdx));
    } else if (tile.appIds.size() == 1) {
        std::wstring remainingId = std::move(tile.appIds.front());
        tile.folder = false;
        tile.id = std::move(remainingId);
        tile.name.clear();
        tile.appIds.clear();
    }
    return true;
}

} // namespace atrium
