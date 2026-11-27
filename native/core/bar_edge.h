// The screen edge the bar sits on and the geometry that follows from it.
#pragma once

#include <windows.h>
#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace atrium {

// The screen edge the bar sits on (setting barEdge).
enum class BarEdge { Bottom, Top, Left, Right };

constexpr BarEdge ParseBarEdge(std::wstring_view name)
{
    return name == L"top" ? BarEdge::Top : name == L"left" ? BarEdge::Left : name == L"right" ? BarEdge::Right : BarEdge::Bottom;
}

constexpr bool Vertical(BarEdge edge) { return edge == BarEdge::Left || edge == BarEdge::Right; }

constexpr BarEdge EdgeInMonitor(const RECT& bar, const RECT& m)
{
    if (bar.bottom - bar.top > bar.right - bar.left)
        return bar.left + bar.right < m.left + m.right ? BarEdge::Left : BarEdge::Right;
    return bar.top + bar.bottom < m.top + m.bottom ? BarEdge::Top : BarEdge::Bottom;
}

// The edge a bar's glass (screen px) sits on, from the rect alone: popups need no other word of it.
inline BarEdge EdgeOf(const RECT& bar)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&bar, MONITOR_DEFAULTTONEAREST), &mi);
    return EdgeInMonitor(bar, mi.rcMonitor);
}

// The bar is laid out as at the bottom of a landscape area: u along the bar, v across it, growing
// toward the edge it sits on. An EdgeFrame puts that layout on the real edge of `area` (screen or
// window px): Top mirrors v; Left and Right run u down the edge. Lengths keep their axis, so a slot
// w along and h across is w tall on a side edge; nothing is rotated, only placed.
struct EdgeFrame {
    BarEdge edge = BarEdge::Bottom;
    D2D1_RECT_F area{};

    EdgeFrame() = default;
    EdgeFrame(BarEdge e, const RECT& r) : edge(e), area{float(r.left), float(r.top), float(r.right), float(r.bottom)} {}
    EdgeFrame(BarEdge e, const D2D1_RECT_F& r) : edge(e), area(r) {}

    float Along() const { return Vertical(edge) ? area.bottom - area.top : area.right - area.left; }
    float Across() const { return Vertical(edge) ? area.right - area.left : area.bottom - area.top; }

    // Area px to layout {u, v}: (x, y) -> (x * _11 + y * _21 + _31, x * _12 + y * _22 + _32).
    // Every edge is a reflection or a quarter turn, so the way back is the transpose.
    D2D1_MATRIX_3X2_F FromAreaMatrix() const
    {
        switch (edge) {
        case BarEdge::Top: return {1, 0, 0, -1, -area.left, area.bottom};
        case BarEdge::Left: return {0, -1, 1, 0, -area.top, area.right};
        case BarEdge::Right: return {0, 1, 1, 0, -area.top, -area.left};
        default: return {1, 0, 0, 1, -area.left, -area.top};
        }
    }

    D2D1_POINT_2F FromArea(D2D1_POINT_2F p) const
    {
        const auto m = FromAreaMatrix();
        return {p.x * m._11 + p.y * m._21 + m._31, p.x * m._12 + p.y * m._22 + m._32};
    }
    D2D1_POINT_2F ToArea(D2D1_POINT_2F c) const
    {
        const auto m = FromAreaMatrix();
        const float u = c.x - m._31, v = c.y - m._32;
        return {u * m._11 + v * m._12, u * m._21 + v * m._22};
    }
    D2D1_RECT_F FromArea(const D2D1_RECT_F& r) const { return Span(FromArea(D2D1_POINT_2F{r.left, r.top}), FromArea(D2D1_POINT_2F{r.right, r.bottom})); }
    D2D1_RECT_F ToArea(const D2D1_RECT_F& r) const { return Span(ToArea(D2D1_POINT_2F{r.left, r.top}), ToArea(D2D1_POINT_2F{r.right, r.bottom})); }

    POINT FromArea(POINT p) const { return Round(FromArea(D2D1_POINT_2F{float(p.x), float(p.y)})); }
    POINT ToArea(POINT c) const { return Round(ToArea(D2D1_POINT_2F{float(c.x), float(c.y)})); }
    RECT FromArea(const RECT& r) const { return Round(FromArea(D2D1_RECT_F{float(r.left), float(r.top), float(r.right), float(r.bottom)})); }
    RECT ToArea(const RECT& r) const { return Round(ToArea(D2D1_RECT_F{float(r.left), float(r.top), float(r.right), float(r.bottom)})); }

private:
    static D2D1_RECT_F Span(D2D1_POINT_2F a, D2D1_POINT_2F b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)}; }
    static LONG Round(float f) { return LONG(f < 0 ? f - 0.5f : f + 0.5f); }
    static POINT Round(D2D1_POINT_2F p) { return {Round(p.x), Round(p.y)}; }
    static RECT Round(const D2D1_RECT_F& r) { return {Round(r.left), Round(r.top), Round(r.right), Round(r.bottom)}; }
};

struct BarPlacement {
    BarEdge edge = BarEdge::Bottom;
    RECT monitor{};
    RECT work{};
    int winW = 0;
    int winH = 0;
    int barH = 0;
    int margin = 0;
    int insetTop = 0;
    bool anchorToMonitor = true;
    int ownReserve = 0;
};

// Places the bar window on `p.edge`. When Atrium hides Windows' taskbar, the docked edge
// anchors to `p.monitor` regardless of stale work-area deductions; otherwise it sits inside
// `p.work` with the bar's own app-bar reserve restored, clamped to `p.monitor`.
inline RECT PlaceBarWindow(const BarPlacement& p)
{
    RECT anchor = p.work;
    switch (p.edge) {
    case BarEdge::Bottom: anchor.bottom = p.anchorToMonitor ? p.monitor.bottom : std::min(p.monitor.bottom, p.work.bottom + p.ownReserve); break;
    case BarEdge::Top: anchor.top = p.anchorToMonitor ? p.monitor.top : std::max(p.monitor.top, p.work.top - p.ownReserve); break;
    case BarEdge::Left: anchor.left = p.anchorToMonitor ? p.monitor.left : std::max(p.monitor.left, p.work.left - p.ownReserve); break;
    case BarEdge::Right: anchor.right = p.anchorToMonitor ? p.monitor.right : std::min(p.monitor.right, p.work.right + p.ownReserve); break;
    }
    const EdgeFrame frame{p.edge, anchor};
    const int x = (int(frame.Along()) - p.winW) / 2;
    const int y = int(frame.Across()) - p.margin - p.barH - p.insetTop;
    return frame.ToArea(RECT{x, y, x + p.winW, y + p.winH});
}

// Always-shown taskbar with "keep room for windows" reserves `margin + barH + margin`
// so the inner gap between maximized windows and the bar equals the outer screen margin.
constexpr int BarReservePx(int barH, int margin) { return barH + 2 * std::max(0, margin); }

// Full-length edge strip (screen px) used to summon an auto-hidden bar anywhere along `edge` of `monitor`.
inline RECT MonitorEdgeStrip(BarEdge edge, const RECT& monitor, LONG thickness = 2)
{
    if (thickness <= 0 || monitor.right <= monitor.left || monitor.bottom <= monitor.top)
        return RECT{};
    const LONG spanX = std::min(thickness, monitor.right - monitor.left);
    const LONG spanY = std::min(thickness, monitor.bottom - monitor.top);
    switch (edge) {
    case BarEdge::Top: return RECT{monitor.left, monitor.top, monitor.right, monitor.top + spanY};
    case BarEdge::Left: return RECT{monitor.left, monitor.top, monitor.left + spanX, monitor.bottom};
    case BarEdge::Right: return RECT{monitor.right - spanX, monitor.top, monitor.right, monitor.bottom};
    default: return RECT{monitor.left, monitor.bottom - spanY, monitor.right, monitor.bottom};
    }
}

// Interactive bar region extended outward to `monitor`'s edge so hovering icons or the margin keeps a revealed bar up.
inline RECT BarEdgeZone(BarEdge edge, const RECT& window, const RECT& monitor, const D2D1_RECT_F& bar)
{
    if (bar.right <= bar.left || window.right <= window.left || window.bottom <= window.top || monitor.right <= monitor.left || monitor.bottom <= monitor.top)
        return RECT{};
    const EdgeFrame f{edge, window};
    return f.ToArea(RECT{LONG(bar.left), 0, LONG(bar.right), std::max<LONG>(0, f.FromArea(monitor).bottom)});
}

inline bool InEdgeSummonZone(BarEdge edge, POINT pointer, const RECT& monitor, LONG thickness = 2)
{
    const RECT strip = MonitorEdgeStrip(edge, monitor, thickness);
    return PtInRect(&strip, pointer) != FALSE;
}

inline bool InEdgeRevealZone(BarEdge edge, POINT pointer, const RECT& window, const RECT& monitor, const D2D1_RECT_F& bar, LONG thickness = 2)
{
    if (InEdgeSummonZone(edge, pointer, monitor, thickness))
        return true;
    const RECT zone = BarEdgeZone(edge, window, monitor, bar);
    return PtInRect(&zone, pointer) != FALSE;
}

// Edge a docked system taskbar occupies on `mon`, or nullopt if auto-hidden/unrecognized.
inline std::optional<BarEdge> TaskbarEdgeOnMonitor(const RECT& tb, const RECT& mon)
{
    const LONG w = tb.right - tb.left, h = tb.bottom - tb.top, mw = mon.right - mon.left, mh = mon.bottom - mon.top;
    if (w <= 0 || h <= 0 || mw <= 0 || mh <= 0)
        return std::nullopt;
    if (w >= h && w >= mw / 2 && h >= 16 && h < mh / 2) {
        if (std::abs(tb.bottom - mon.bottom) <= 4 && tb.top > mon.top)
            return BarEdge::Bottom;
        if (std::abs(tb.top - mon.top) <= 4 && tb.bottom < mon.bottom)
            return BarEdge::Top;
    }
    if (h > w && h >= mh / 2 && w >= 16 && w < mw / 2) {
        if (std::abs(tb.left - mon.left) <= 4 && tb.right < mon.right)
            return BarEdge::Left;
        if (std::abs(tb.right - mon.right) <= 4 && tb.left > mon.left)
            return BarEdge::Right;
    }
    return std::nullopt;
}

constexpr LONG RectEdge(const RECT& r, BarEdge edge)
{
    return edge == BarEdge::Top ? r.top : edge == BarEdge::Left ? r.left : edge == BarEdge::Right ? r.right : r.bottom;
}

constexpr bool WorkAreaEdgeExpanded(const RECT& before, const RECT& after, const RECT& mon, BarEdge edge)
{
    const LONG a = RectEdge(after, edge), m = RectEdge(mon, edge), b = RectEdge(before, edge);
    return (edge == BarEdge::Top || edge == BarEdge::Left) ? (a <= m || a < b) : (a >= m || a > b);
}

constexpr bool WorkAreaEdgeShrunk(const RECT& before, const RECT& after, BarEdge edge)
{
    const LONG a = RectEdge(after, edge), b = RectEdge(before, edge);
    return (edge == BarEdge::Top || edge == BarEdge::Left) ? (a > b) : (a < b);
}

struct TaskbarWorkSnapshot {
    HMONITOR monitor = nullptr;
    BarEdge edge = BarEdge::Bottom;
    RECT work{};
    RECT monitorRect{};
};

constexpr bool WorkAreaSnapshotSynced(const TaskbarWorkSnapshot& s, const RECT& currentWork, bool release)
{
    return release ? WorkAreaEdgeExpanded(s.work, currentWork, s.monitorRect, s.edge) : WorkAreaEdgeShrunk(s.work, currentWork, s.edge);
}

inline std::optional<TaskbarWorkSnapshot> MakeTaskbarSnapshot(HMONITOR mon, const RECT& tb, const RECT& monitor, const RECT& work, bool requireDockedEdge)
{
    auto edge = TaskbarEdgeOnMonitor(tb, monitor);
    if (!edge && !requireDockedEdge)
        edge = EdgeInMonitor(tb, monitor);
    if (!edge || (requireDockedEdge && WorkAreaEdgeExpanded(work, work, monitor, *edge)))
        return std::nullopt;
    return TaskbarWorkSnapshot{mon, *edge, work, monitor};
}

inline bool WorkHoldsTaskbarEdge(const RECT& tb, const RECT& mon, const RECT& work)
{
    const auto edge = TaskbarEdgeOnMonitor(tb, mon);
    if (!edge)
        return false;
    const BarEdge inner = *edge == BarEdge::Bottom ? BarEdge::Top :
                          *edge == BarEdge::Top    ? BarEdge::Bottom :
                          *edge == BarEdge::Left   ? BarEdge::Right :
                                                     BarEdge::Left;
    return std::abs(RectEdge(work, *edge) - RectEdge(tb, inner)) <= 1;
}

} // namespace atrium
