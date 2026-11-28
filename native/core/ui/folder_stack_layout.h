// Where the folder stack's folders go when it opens, in its two layouts. Fan: the folders rise
// from the entry one above another along a gentle arc that leans away from the bar as it climbs,
// each with its name on a small capsule of glass beside it (the icons float on nothing), as the
// Dock's stacks fan out on macOS. Column: one card of glass above the entry, a row per folder.
// The fan opens on the bar's horizontal edges; on a side edge the column stands in. Pure geometry:
// screen px in, the surface's window (px) and the content's places (DIP) out.
#pragma once

#include "bar_edge.h"

#include <windows.h>
#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace atrium {

struct StackItemPlace {
    D2D1_RECT_F icon{};  // content DIP
    D2D1_RECT_F label{}; // the name's capsule (fan), or the row (column); in the fan before its tilt
    float tilt = 0;      // fan: radians, clockwise about the capsule's centre, along the arc's radius
};

struct StackPlacement {
    RECT window{};         // the content, screen px
    D2D1_POINT_2F origin{}; // the entry's centre in content DIP: the fan unfolds out of it
    std::vector<StackItemPlace> items; // nearest the bar first; fewer than asked when the screen ends
    bool fan = false;
    bool labelsRight = false; // fan: the names to the right of the icons (no room on the left)
};

constexpr float kStackGap = 12;          // from the bar's glass, as every popup (kPopupGapDip)
constexpr float kStackFanIcon = 40;      // a folder's icon in the fan
constexpr float kStackFanStep = 50;      // from one folder to the next up the arc
constexpr float kStackFanRadius = 1000;  // the arc's: 20 steps, the lean of the reference fan
constexpr float kStackLabelH = 28;       // the name's capsule: a tooltip's height, round ends
constexpr float kStackLabelPadX = 12;
constexpr float kStackLabelMinW = 56, kStackLabelMaxW = 240;
constexpr float kStackLabelGap = 10;     // between a capsule and its icon
constexpr float kStackMargin = 16;       // around the fan inside the window: room for the hover lift
constexpr float kStackTilt = 0.6f;       // of the arc's own turn: how far the names lean with it
constexpr float kStackColumnW = 264, kStackRowH = 44, kStackColumnPad = 8, kStackColumnIcon = 26;

inline float StackLabelWidth(float textW) { return std::clamp(textW + 2 * kStackLabelPadX, kStackLabelMinW, kStackLabelMaxW); }

// How far the arc has leaned sideways `rise` DIP above its foot.
inline float StackLean(float rise) { return kStackFanRadius - std::sqrt(std::max(0.f, kStackFanRadius * kStackFanRadius - rise * rise)); }

// The column: a card centred on the entry along the bar, kStackGap off the bar's glass, inside the
// work area; as many rows as fit.
inline StackPlacement ColumnPlacement(RECT bar, RECT entry, RECT work, float scale, BarEdge edge, size_t count)
{
    StackPlacement p;
    const float gap = kStackGap * scale, w = kStackColumnW * scale;
    const float room = float(Vertical(edge) ? work.bottom - work.top : edge == BarEdge::Top ? work.bottom - bar.bottom : bar.top - work.top) - 2 * gap;
    const size_t rows = std::min(count, size_t(std::max(1.f, (room / scale - 2 * kStackColumnPad) / kStackRowH)));
    const float h = (2 * kStackColumnPad + float(rows) * kStackRowH) * scale;
    const float ex = float(entry.left + entry.right) / 2, ey = float(entry.top + entry.bottom) / 2;
    float left = 0, top = 0;
    if (Vertical(edge)) {
        left = edge == BarEdge::Left ? float(bar.right) + gap : float(bar.left) - gap - w;
        top = std::clamp(ey - h / 2, float(work.top) + gap, std::max(float(work.top) + gap, float(work.bottom) - gap - h));
    } else {
        left = std::clamp(ex - w / 2, float(work.left) + gap, std::max(float(work.left) + gap, float(work.right) - gap - w));
        top = edge == BarEdge::Top ? float(bar.bottom) + gap : float(bar.top) - gap - h;
    }
    p.window = RECT{LONG(std::lround(left)), LONG(std::lround(top)), LONG(std::lround(left + w)), LONG(std::lround(top + h))};
    p.origin = D2D1::Point2F((ex - left) / scale, (ey - top) / scale);
    for (size_t i = 0; i < rows; ++i) {
        // Nearest the bar first: from the bottom up over a bottom bar, from the top down elsewhere.
        const size_t slot = edge == BarEdge::Bottom ? rows - 1 - i : i;
        const float y = kStackColumnPad + float(slot) * kStackRowH;
        StackItemPlace item;
        item.label = D2D1::RectF(kStackColumnPad, y, kStackColumnW - kStackColumnPad, y + kStackRowH);
        item.icon = D2D1::RectF(kStackColumnPad + 10, y + (kStackRowH - kStackColumnIcon) / 2, kStackColumnPad + 10 + kStackColumnIcon,
                                y + (kStackRowH + kStackColumnIcon) / 2);
        p.items.push_back(item);
    }
    return p;
}

// The fan over a horizontal bar: `labelW` holds each name's text width (DIP), nearest the bar
// first. On a side edge, the column.
inline StackPlacement FanPlacement(RECT bar, RECT entry, RECT work, float scale, BarEdge edge, const std::vector<float>& labelW)
{
    if (Vertical(edge))
        return ColumnPlacement(bar, entry, work, scale, edge, labelW.size());
    StackPlacement p;
    p.fan = true;
    const bool down = edge == BarEdge::Top;
    // The fan's own space, DIP: x across from the entry's centre, y away from the bar (up is +).
    const float ex = float(entry.left + entry.right) / 2;
    const float foot = float(down ? bar.bottom : bar.top);
    const float room = (down ? float(work.bottom) - foot : foot - float(work.top)) / scale - kStackGap - kStackMargin;
    const size_t n = std::min(labelW.size(), size_t(std::max(1.f, (room - kStackFanIcon) / kStackFanStep + 1)));
    float widest = 0;
    for (size_t i = 0; i < n; ++i)
        widest = std::max(widest, StackLabelWidth(labelW[i]) + kStackLabelGap);
    // The names go on the left unless the left runs off the work area; the arc leans right, as the
    // Dock's fan does, unless that runs off the right.
    p.labelsRight = (ex - float(work.left)) / scale < kStackFanIcon / 2 + widest + kStackMargin;
    const float reach =
        kStackFanIcon / 2 + StackLean(kStackGap + kStackFanIcon / 2 + float(n) * kStackFanStep) + kStackMargin + (p.labelsRight ? kStackLabelGap + widest : 0);
    const float side = (float(work.right) - ex) / scale < reach ? -1.f : 1.f;
    float minX = -kStackFanIcon / 2, maxX = kStackFanIcon / 2, top = 0;
    std::vector<D2D1_POINT_2F> centres, labels;
    std::vector<float> turns;
    for (size_t i = 0; i < n; ++i) {
        const float rise = kStackGap + kStackFanIcon / 2 + float(i) * kStackFanStep;
        const D2D1_POINT_2F c = D2D1::Point2F(side * StackLean(rise), rise);
        const float lw = StackLabelWidth(labelW[i]);
        // The name lies along the arc's radius through its folder, leaning a little less than the arc turns.
        const float turn = kStackTilt * std::asin(std::min(1.f, rise / kStackFanRadius)), cs = std::cos(turn), sn = std::sin(turn);
        const float out = (p.labelsRight ? 1.f : -1.f), along = kStackFanIcon / 2 + kStackLabelGap + lw / 2;
        const D2D1_POINT_2F l = D2D1::Point2F(c.x + out * cs * along, c.y - out * side * sn * along);
        const float hx = lw / 2 * cs + kStackLabelH / 2 * sn, hy = lw / 2 * sn + kStackLabelH / 2 * cs;
        // A tilted name can extend farther than its icon. Keep only the prefix that fits.
        const float itemTop = std::max(rise + kStackFanIcon / 2, l.y + hy);
        if (i > 0 && itemTop > room + kStackGap)
            break;
        minX = std::min({minX, c.x - kStackFanIcon / 2, l.x - hx});
        maxX = std::max({maxX, c.x + kStackFanIcon / 2, l.x + hx});
        top = std::max(top, itemTop);
        centres.push_back(c);
        labels.push_back(l);
        turns.push_back(turn);
    }
    // The window: the fan with a margin, its foot kStackGap off the glass; content y grows away from
    // the bar's far side as on screen.
    const float leftDip = minX - kStackMargin, widthDip = maxX - minX + 2 * kStackMargin, heightDip = top - kStackGap + kStackMargin;
    // Kept inside the work area: near its edge the fan stands a little aside from its entry.
    const float left = std::clamp(ex + leftDip * scale, float(work.left), std::max(float(work.left), float(work.right) - widthDip * scale));
    const float shift = (ex - left) / scale + leftDip; // the entry's x, past the fan's left edge, DIP
    const float rim = down ? foot + kStackGap * scale : foot - kStackGap * scale;
    const float y0 = down ? rim : rim - heightDip * scale;
    p.window = RECT{LONG(std::lround(left)), LONG(std::lround(y0)), LONG(std::lround(left + widthDip * scale)), LONG(std::lround(y0 + heightDip * scale))};
    auto toContent = [&](float x, float rise) {
        const float along = rise - kStackGap; // from the window's bar side
        return D2D1::Point2F(x - leftDip + shift, down ? along : heightDip - along);
    };
    const float ey = float(entry.top + entry.bottom) / 2;
    p.origin = D2D1::Point2F(-leftDip + shift, (ey - y0) / scale);
    for (size_t i = 0; i < centres.size(); ++i) {
        const D2D1_POINT_2F c = toContent(centres[i].x, centres[i].y), l = toContent(labels[i].x, labels[i].y);
        const float lw = StackLabelWidth(labelW[i]), h = kStackFanIcon / 2;
        StackItemPlace item;
        item.icon = D2D1::RectF(c.x - h, c.y - h, c.x + h, c.y + h);
        item.label = D2D1::RectF(l.x - lw / 2, l.y - kStackLabelH / 2, l.x + lw / 2, l.y + kStackLabelH / 2);
        // Over a bottom bar the arc turns clockwise as it leans right; mirrored by the lean and the edge.
        item.tilt = side * (down ? -1.f : 1.f) * turns[i];
        p.items.push_back(item);
    }
    return p;
}

} // namespace atrium
