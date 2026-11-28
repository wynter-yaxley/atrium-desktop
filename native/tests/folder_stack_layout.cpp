#include "ui/folder_stack_layout.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace atrium;

namespace {

int g_failures = 0;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

bool Inside(const D2D1_RECT_F& r, float w, float h) { return r.left >= 0 && r.top >= 0 && r.right <= w && r.bottom <= h; }

bool Overlap(const D2D1_RECT_F& a, const D2D1_RECT_F& b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }

// A 1920 x 1080 display at 100 %, the bar 60 px tall at the bottom with the work area above it.
constexpr RECT kWork{0, 0, 1920, 1010};
constexpr RECT kBar{300, 1010, 1620, 1070};

void CheckFan(const StackPlacement& p, float scale, const char* what)
{
    const float w = float(p.window.right - p.window.left) / scale, h = float(p.window.bottom - p.window.top) / scale;
    for (size_t i = 0; i < p.items.size(); ++i) {
        const auto& it = p.items[i];
        Require(Inside(it.icon, w, h) && Inside(it.label, w, h), what);
        Require(!Overlap(it.icon, it.label), "a name never covers its icon");
        if (i > 0)
            Require(!Overlap(it.label, p.items[i - 1].label) && !Overlap(it.icon, p.items[i - 1].icon), "neighbours never overlap");
        // The name lies on the line through its icon at its tilt (the arc's radius), leaning more up the arc.
        const float cx = (it.icon.left + it.icon.right) / 2, cy = (it.icon.top + it.icon.bottom) / 2;
        const float lx = (it.label.left + it.label.right) / 2, ly = (it.label.top + it.label.bottom) / 2;
        Require(std::abs((cy - ly) * std::cos(it.tilt) - (cx - lx) * std::sin(it.tilt)) < 0.5f, "a name lies along its folder's radius");
        if (i > 0)
            Require(std::abs(it.tilt) > std::abs(p.items[i - 1].tilt), "the names lean more up the arc");
    }
}

} // namespace

int main()
{
    const std::vector<float> names{40, 90, 60, 200, 30, 120, 75, 50};
    const RECT entry{1400, 1015, 1450, 1065};
    const auto fan = FanPlacement(kBar, entry, kWork, 1.f, BarEdge::Bottom, names);
    Require(fan.fan && fan.items.size() == names.size(), "eight folders fan out over a bottom bar");
    Require(!fan.labelsRight, "names go to the left with room there");
    Require(fan.window.bottom == kBar.top - 12, "the fan's window stands 12 DIP off the bar's glass");
    Require(fan.window.top >= kWork.top && fan.window.left >= kWork.left && fan.window.right <= kWork.right, "the fan stays on the display");
    CheckFan(fan, 1.f, "every piece of the fan lies inside its window");
    const auto& first = fan.items.front().icon;
    const auto& last = fan.items.back().icon;
    Require(first.bottom > last.bottom, "the first folder is nearest the bar");
    Require(last.left > first.left, "the arc leans away from the names as it climbs");
    const float firstStep = fan.items[1].icon.left - fan.items[0].icon.left, lastStep = last.left - fan.items[6].icon.left;
    Require(lastStep > firstStep, "the lean grows with the height: an arc, not a slope");
    Require(fan.origin.x > first.left && fan.origin.x < first.right, "the fan unfolds out of the entry");
    Require(fan.origin.y > float(fan.window.bottom - fan.window.top), "the entry lies below the window, on the bar");

    const auto hi = FanPlacement(RECT{450, 1525, 2430, 1615}, RECT{2100, 1532, 2175, 1607}, RECT{0, 0, 2880, 1515}, 1.5f, BarEdge::Bottom, names);
    CheckFan(hi, 1.5f, "at 150 % the fan still lies inside its window");
    Require(hi.window.bottom == 1525 - 18, "the gap scales with the display");

    const auto left = FanPlacement(RECT{0, 1010, 1320, 1070}, RECT{20, 1015, 70, 1065}, kWork, 1.f, BarEdge::Bottom, names);
    Require(left.labelsRight, "no room on the left: the names go right");
    Require(left.window.left >= kWork.left, "a fan by the left edge stays on the display");
    Require(left.items.back().icon.left > left.items.front().icon.left, "names on the right: the arc still leans right, away from the edge");
    const auto right = FanPlacement(RECT{600, 1010, 1920, 1070}, RECT{1860, 1015, 1910, 1065}, kWork, 1.f, BarEdge::Bottom, names);
    Require(!right.labelsRight && right.items.back().icon.left < right.items.front().icon.left, "by the right edge the arc leans left");
    Require(right.window.right <= kWork.right, "a fan by the right edge stays on the display");
    CheckFan(right, 1.f, "the fan by the right edge lies inside its window");
    CheckFan(left, 1.f, "the mirrored fan lies inside its window");

    const auto top = FanPlacement(RECT{300, 0, 1620, 60}, RECT{1400, 5, 1450, 55}, RECT{0, 70, 1920, 1080}, 1.f, BarEdge::Top, names);
    Require(top.window.top == 72, "under a top bar the fan opens downwards");
    Require(top.items.front().icon.top < top.items.back().icon.top, "under a top bar the first folder is the top one");
    Require(top.origin.y < 0, "under a top bar the entry lies above the window");

    const std::vector<float> many(40, 80);
    const auto tall = FanPlacement(kBar, entry, kWork, 1.f, BarEdge::Bottom, many);
    Require(tall.items.size() < many.size() && tall.window.top >= kWork.top, "a fan taller than the display ends at its top");

    // On a short, scaled display, the slanted long names need more height than the icons.
    const std::vector<float> longNames(8, 240.f);
    const auto shortBottom = FanPlacement(RECT{300, 630, 1100, 720}, RECT{670, 640, 730, 710}, RECT{0, 0, 1366, 630}, 1.5f, BarEdge::Bottom, longNames);
    Require(shortBottom.items.size() == 7 && shortBottom.window.top >= 0 && shortBottom.window.bottom == 612,
            "long names on a short scaled bottom bar fit within the work area");
    const auto shortTop = FanPlacement(RECT{300, 0, 1100, 90}, RECT{670, 10, 730, 80}, RECT{0, 90, 1366, 720}, 1.5f, BarEdge::Top, longNames);
    Require(shortTop.items.size() == 7 && shortTop.window.bottom <= 720 && shortTop.window.top == 108,
            "long names on a short scaled top bar fit within the work area");

    const auto tight = FanPlacement(RECT{300, 100, 1620, 160}, RECT{1400, 105, 1450, 155}, RECT{0, 60, 1920, 160}, 1.f, BarEdge::Bottom, names);
    Require(tight.fan && tight.items.size() == 1, "tight room still keeps at least one fan item");
    CheckFan(tight, 1.f, "single tight-room fan item lies inside its window");
    const auto emptyFan = FanPlacement(kBar, entry, kWork, 1.f, BarEdge::Bottom, {});
    Require(emptyFan.fan && emptyFan.items.empty(), "empty input yields empty fan");

    const auto side = FanPlacement(RECT{0, 100, 60, 980}, RECT{5, 500, 55, 550}, RECT{70, 0, 1920, 1080}, 1.f, BarEdge::Left, names);
    Require(!side.fan && side.items.size() == names.size(), "on a side edge the column stands in");
    Require(side.window.left == 72, "the column stands 12 DIP off a left bar");

    const auto column = ColumnPlacement(kBar, entry, kWork, 1.f, BarEdge::Bottom, 5);
    const float cw = float(column.window.right - column.window.left), ch = float(column.window.bottom - column.window.top);
    Require(column.items.size() == 5 && ch == 8 * 2 + 5 * 44, "the column holds a row per folder");
    Require(column.items.front().label.top > column.items.back().label.top, "over a bottom bar the first folder is the lowest row");
    for (const auto& it : column.items)
        Require(Inside(it.label, cw, ch) && Inside(it.icon, cw, ch), "rows lie inside the card");
    const auto edgeColumn = ColumnPlacement(kBar, RECT{1880, 1015, 1920, 1065}, kWork, 1.f, BarEdge::Bottom, 3);
    Require(edgeColumn.window.right <= kWork.right - 12, "the column keeps off the display's edge");
    Require(ColumnPlacement(kBar, entry, kWork, 1.f, BarEdge::Bottom, 100).window.top >= kWork.top, "a long column ends at the work area's top");

    std::cout << (g_failures ? "folder_stack_layout: FAILED" : "folder_stack_layout: ok") << '\n';
    return g_failures ? 1 : 0;
}
