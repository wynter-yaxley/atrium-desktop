// Shared layout metrics and geometry helpers for StartMenu implementation files.
#pragma once

#include "motion.h"
#include "shell/start_layout.h"

#include <d2d1.h>

#include <cwchar>
#include <string>

namespace atrium::start_menu_detail {

// DIP. The panel above, the status capsule (the bar's height) kGap below it.
// The panel: the search row, the grid (cols x rows cells, kGridPad at the
// sides), the page dots.
inline constexpr float kGap = 12;
inline constexpr float kSearchX = 24, kSearchTop = 20, kSearchH = 44, kAllW = 108;
inline constexpr float kGridTop = 86, kGridFoot = 42, kGridPad = 28;
// The menu's size (start.json's "size"): the tiles across and down, small to
// the largest. Medium is the 9 x 4, 704 x 480 of DESIGN.md.
struct StartGrid {
    int cols, rows;
};
inline constexpr StartGrid kStartGrids[StartLayout::kSizes] = {
    {7, 3},  // small
    {9, 4},  // medium
    {11, 5}, // large
    {13, 6}, // the largest
};
// The icons' size (start.json's "icon"), a choice of its own: how large a
// tile's icon and a folder's cover are drawn, the cell around them, and the
// name under the icon, which grows with the icon. Bigger icons need more
// room, so the panel follows.
struct StartIconSize {
    float icon, cellW, cellH, label, labelH;
};
inline constexpr StartIconSize kStartIcons[StartLayout::kSizes] = {
    {34, 62, 82, 11.5f, 16},   // small
    {40, 72, 88, 12.5f, 17},   // medium
    {52, 94, 100, 14.5f, 19},  // large
    {64, 116, 112, 16.5f, 21}, // the largest
};
inline constexpr float kRowH = 52, kRowIcon = 28, kHeadH = 34, kListTop = 84;
inline constexpr float kFolderHead = 52, kFolderFoot = 24, kFolderPad = 14; // the open folder: its name above its apps, its dots below
inline constexpr float kIndexW = 22, kHint = 52;                          // the A-Z list's letters down its left side; the letter picked, large
inline constexpr float kPowerW = 176, kPowerInset = 8;
inline constexpr float kGrip = 30; // the resize grip: the panel's top right corner
inline constexpr double kStageMs = 260, kStaggerMs = 12, kRippleMs = 520;
// The folder's card out of its tile, with a little overshoot; back into it quicker, without.
inline constexpr motion::Spring kFolderOpen{0.46f, 0.72f};
inline constexpr double kFolderAppMs = 300, kFolderAppStaggerMs = 22; // its apps, one after another
inline constexpr double kRestMs = 220, kEdgeMs = 550;                 // carrying a tile: the others make room; the page turns
inline constexpr float kGlideRate = 16;                               // tiles moving to their places (per second)
inline constexpr motion::Spring kResize{0.40f, 0.82f};                // the panel and the capsule to another size
inline constexpr wchar_t kLetters[] = L"#ABCDEFGHIJKLMNOPQRSTUVWXYZ";

inline D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
inline bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
inline int Index(const std::wstring& id, size_t prefix) { return int(wcstol(id.c_str() + prefix, nullptr, 10)); }
inline float Mix(float a, float b, float t) { return a + (b - a) * t; }
inline D2D1_RECT_F Mix(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float t)
{
    return D2D1::RectF(Mix(a.left, b.left, t), Mix(a.top, b.top, t), Mix(a.right, b.right, t), Mix(a.bottom, b.bottom, t));
}

} // namespace atrium::start_menu_detail
