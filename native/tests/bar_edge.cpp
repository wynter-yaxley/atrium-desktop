#include "bar_edge.h"
#include "bar_stacking.h"

#include <iostream>

using namespace atrium;

namespace {
const RECT kScreen{-1920, 0, 0, 1080}; // a display left of the primary one
bool g_failed = false;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        g_failed = true;
    }
}

bool Same(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }

// A 400 x 64 bar centred along the edge, its outer side on the edge.
RECT Strip(const EdgeFrame& f) { return f.ToArea(RECT{LONG(f.Along() / 2) - 200, LONG(f.Across()) - 64, LONG(f.Along() / 2) + 200, LONG(f.Across())}); }

void CheckPlacement()
{
    Require(Same(Strip({BarEdge::Bottom, kScreen}), {-1160, 1016, -760, 1080}), "bottom: the strip sits on the bottom edge");
    Require(Same(Strip({BarEdge::Top, kScreen}), {-1160, 0, -760, 64}), "top: the strip sits on the top edge");
    Require(Same(Strip({BarEdge::Left, kScreen}), {-1920, 340, -1856, 740}), "left: the strip runs down the left edge");
    Require(Same(Strip({BarEdge::Right, kScreen}), {-64, 340, 0, 740}), "right: the strip runs down the right edge");
}

void CheckRoundTrip()
{
    for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right}) {
        const EdgeFrame f{edge, kScreen};
        const POINT p{-700, 333};
        const POINT back = f.ToArea(f.FromArea(p));
        Require(back.x == p.x && back.y == p.y, "a point comes back to where it was");
        const RECT r{-1500, 100, -1400, 180};
        Require(Same(f.ToArea(f.FromArea(r)), r), "a rect comes back to where it was");
    }
}

void CheckAxes()
{
    // u grows left to right on horizontal edges and top to bottom on vertical ones; v grows toward the edge.
    const EdgeFrame left{BarEdge::Left, kScreen}, top{BarEdge::Top, kScreen};
    Require(left.Along() == 1080 && left.Across() == 1920, "a side edge runs along the screen's height");
    Require(left.FromArea(POINT{-1920, 10}).x == 10 && left.FromArea(POINT{-1920, 10}).y == 1920, "left: the edge is the far end of v");
    Require(top.FromArea(POINT{-1910, 0}).y == 1080, "top: the edge is the far end of v");
    Require(ParseBarEdge(L"right") == BarEdge::Right && ParseBarEdge(L"") == BarEdge::Bottom, "setting names map to edges");
}

void CheckEdgeOf()
{
    // The primary display, whatever its size: each strip is told by its shape and side.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right})
        Require(EdgeOf(Strip({edge, mi.rcMonitor})) == edge, "a strip's edge is read back from its rect");
}

RECT GlassBand(BarEdge edge, const RECT& win, int winW, int insetTop, int barH)
{
    return EdgeFrame{edge, win}.ToArea(RECT{0, insetTop, winW, insetTop + barH});
}

RECT ShortenEdge(RECT r, BarEdge edge, int by)
{
    switch (edge) {
    case BarEdge::Bottom: r.bottom -= by; break;
    case BarEdge::Top: r.top += by; break;
    case BarEdge::Left: r.left += by; break;
    case BarEdge::Right: r.right -= by; break;
    }
    return r;
}

LONG OuterEdge(const RECT& r, BarEdge edge)
{
    switch (edge) {
    case BarEdge::Bottom: return r.bottom;
    case BarEdge::Top: return r.top;
    case BarEdge::Left: return r.left;
    case BarEdge::Right: return r.right;
    }
    return 0;
}

void CheckBarWindowPlacement()
{
    const int winW = 600, barH = 60, insetTop = 18, inset = 14, winH = barH + insetTop + inset;

    // #141: secondary display at positive or negative coordinates, hidden taskbar, stale work-area deduction.
    for (const RECT& mon : {RECT{1920, 0, 3840, 1080}, RECT{-1920, -200, 0, 880}}) {
        for (int shortBy : {48, 60, 72, barH}) {
            RECT work = ShortenEdge(mon, BarEdge::Bottom, shortBy);
            work.left += 80; // other axis keeps using work area
            const RECT win = PlaceBarWindow({BarEdge::Bottom, mon, work, winW, winH, barH, 0, insetTop, true, 0});
            const RECT glass = GlassBand(BarEdge::Bottom, win, winW, insetTop, barH);
            Require(glass.bottom == mon.bottom, "#141: hidden taskbar anchors glass to monitor bottom");
            Require((win.left + win.right) == (work.left + work.right), "other axis stays centred in work area");
        }
    }

    // Coexisting with a visible system taskbar: work area + own reserve, clamped to monitor.
    const RECT primary{0, 0, 1920, 1080};
    {
        const RECT work{0, 0, 1920, 1080 - 48 - barH};
        const RECT win = PlaceBarWindow({BarEdge::Bottom, primary, work, winW, winH, barH, 0, insetTop, false, barH});
        const RECT glass = GlassBand(BarEdge::Bottom, win, winW, insetTop, barH);
        Require(glass.bottom == 1080 - 48, "coexisting: glass sits above the 48 px system taskbar");
    }
    for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right}) {
        const RECT win = PlaceBarWindow({edge, primary, primary, winW, winH, barH, 0, insetTop, false, barH});
        const RECT glass = GlassBand(edge, win, winW, insetTop, barH);
        Require(OuterEdge(glass, edge) == OuterEdge(primary, edge), "coexisting: own reserve is clamped inside monitor");
    }

    // All four edges with hidden taskbar and margin 0 and 10.
    for (int margin : {0, 10}) {
        for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right}) {
            const RECT work = ShortenEdge(kScreen, edge, 48);
            const RECT win = PlaceBarWindow({edge, kScreen, work, winW, winH, barH, margin, insetTop, true, 0});
            const RECT glass = GlassBand(edge, win, winW, insetTop, barH);
            const LONG sign = (edge == BarEdge::Top || edge == BarEdge::Left) ? 1 : -1;
            Require(OuterEdge(glass, edge) == OuterEdge(kScreen, edge) + sign * margin, "four edges: glass sits margin px from monitor edge");
        }
    }
}

void CheckTopmostCoverPolicy()
{
    Require(ShouldIgnoreTopmostCover(L"#32768", 0, 0, false, false), "native menu class is ignored");
    Require(ShouldIgnoreTopmostCover(L"tooltips_class32", 0, 0, false, false), "tooltip class is ignored");
    Require(ShouldIgnoreTopmostCover(L"CustomOverlay", 0, WS_EX_TRANSPARENT | WS_EX_LAYERED, false, false), "click-through overlay is ignored");

    // Foreign menu-shaped popups (including single-bit border/dlgframe, since WS_CAPTION is two bits).
    Require(ShouldIgnoreTopmostCover(L"Qt5QWindowPopupDropShadowSaveBits", WS_POPUP | WS_BORDER, WS_EX_TOOLWINDOW, false, false),
            "toolwindow popup with 1px border is ignored");
    Require(ShouldIgnoreTopmostCover(L"WpsSyncPopup", WS_POPUP | WS_DLGFRAME, WS_EX_NOACTIVATE, false, false), "noactivate popup with dlgframe is ignored");
    Require(ShouldIgnoreTopmostCover(L"Chrome_WidgetWin_1", WS_POPUP, 0, true, false), "foreground popup menu is ignored");
    Require(ShouldIgnoreTopmostCover(L"CustomTrayMenu", WS_POPUP, 0, false, true), "popup whose owner is foreground is ignored");

    // Non-menu windows must not be ignored.
    Require(!ShouldIgnoreTopmostCover(L"CustomPopup", WS_POPUP, 0, false, false), "background popup without tool/noactivate is not ignored");
    Require(!ShouldIgnoreTopmostCover(L"CustomDialog", WS_POPUP | WS_CAPTION, WS_EX_TOOLWINDOW, true, true), "popup with full WS_CAPTION is not ignored");
    Require(!ShouldIgnoreTopmostCover(L"CustomApp", WS_POPUP, WS_EX_APPWINDOW | WS_EX_TOOLWINDOW, true, true), "WS_EX_APPWINDOW is not ignored");
    Require(!ShouldIgnoreTopmostCover(L"OverlappedApp", WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW, true, true), "non-popup window is not ignored");
}

void CheckBarReserve()
{
    Require(BarReservePx(60, 0) == 60, "zero margin reserves only bar height");
    Require(BarReservePx(60, 10) == 80, "non-zero margin keeps symmetric inner and outer gaps");

    const int winW = 600, barH = 60, margin = 10, insetTop = 18, inset = 14, winH = barH + insetTop + inset;
    const int reserve = BarReservePx(barH, margin);
    for (BarEdge edge : {BarEdge::Bottom, BarEdge::Top, BarEdge::Left, BarEdge::Right}) {
        const RECT work = ShortenEdge(kScreen, edge, reserve);
        const RECT win = PlaceBarWindow({edge, kScreen, work, winW, winH, barH, margin, insetTop, true, 0});
        const RECT glass = GlassBand(edge, win, winW, insetTop, barH);
        const LONG sign = (edge == BarEdge::Top || edge == BarEdge::Left) ? 1 : -1;
        const LONG outerGap = sign * (OuterEdge(glass, edge) - OuterEdge(kScreen, edge));
        const LONG workEdge = OuterEdge(work, edge);
        const BarEdge innerSide = edge == BarEdge::Bottom ? BarEdge::Top :
                                  edge == BarEdge::Top    ? BarEdge::Bottom :
                                  edge == BarEdge::Left   ? BarEdge::Right :
                                                            BarEdge::Left;
        const LONG innerGap = sign * (workEdge - OuterEdge(glass, innerSide));
        Require(outerGap == margin && innerGap == margin, "reserved work area leaves equal margin on outer and inner edges of the bar");
    }
}

void CheckDodgeOverlap()
{
    const RECT barZone{660, 1010, 1260, 1080};
    Require(WindowOverlapsBar(RECT{700, 500, 1100, 1050}, barZone), "window overlapping the bar triggers dodge");
    Require(WindowOverlapsBar(RECT{-200, 900, 700, 1060}, barZone), "window straddling from another monitor onto the bar triggers dodge");
    Require(!WindowOverlapsBar(RECT{600, 200, 1400, 1010}, barZone), "window ending right at the bar inner edge does not trigger dodge");
    Require(!WindowOverlapsBar(RECT{100, 500, 660, 1070}, barZone), "window ending right at the bar left edge does not trigger dodge");
    Require(!WindowOverlapsBar(RECT{1260, 500, 1800, 1070}, barZone), "window starting right at the bar right edge does not trigger dodge");
    Require(!WindowOverlapsBar(RECT{100, 100, 600, 800}, barZone), "window elsewhere on screen does not trigger dodge");
}

void CheckAutoHideEdgeZones()
{
    // #189: full-length summon strip and reveal zone on both negative and positive extended monitors.
    const RECT negMon{-1920, 0, 0, 1080};
    const RECT posMon{1920, -200, 3840, 880};
    Require(Same(MonitorEdgeStrip(BarEdge::Bottom, negMon, 2), {-1920, 1078, 0, 1080}), "#189: bottom strip on negative monitor");
    Require(Same(MonitorEdgeStrip(BarEdge::Top, posMon, 2), {1920, -200, 3840, -198}), "#189: top strip on positive extended monitor");
    Require(Same(MonitorEdgeStrip(BarEdge::Left, negMon, 2), {-1920, 0, -1918, 1080}), "#189: left strip on negative monitor");
    Require(Same(MonitorEdgeStrip(BarEdge::Right, posMon, 2), {3838, -200, 3840, 880}), "#189: right strip on positive extended monitor");
    const RECT zeroStrip = MonitorEdgeStrip(BarEdge::Bottom, negMon, 0);
    const RECT emptyMonStrip = MonitorEdgeStrip(BarEdge::Bottom, RECT{0, 0, 0, 0}, 2);
    Require(IsRectEmpty(&zeroStrip) && IsRectEmpty(&emptyMonStrip), "#189: invalid thickness or monitor produces empty strip");

    for (const RECT& mon : {negMon, posMon}) {
        const LONG midX = (mon.left + mon.right) / 2;
        const LONG midY = (mon.top + mon.bottom) / 2;
        Require(InEdgeSummonZone(BarEdge::Bottom, POINT{mon.left + 10, mon.bottom - 1}, mon), "#189: bottom summon zone spans full monitor width");
        Require(!InEdgeSummonZone(BarEdge::Bottom, POINT{midX, mon.bottom - 3}, mon), "#189: bottom summon zone stops after strip thickness");
        Require(!InEdgeSummonZone(BarEdge::Bottom, POINT{midX, mon.bottom}, mon), "#189: bottom summon zone excludes past monitor edge");
        Require(InEdgeSummonZone(BarEdge::Top, POINT{mon.right - 10, mon.top}, mon), "#189: top summon zone");
        Require(InEdgeSummonZone(BarEdge::Left, POINT{mon.left + 1, midY}, mon), "#189: left summon zone");
        Require(InEdgeSummonZone(BarEdge::Right, POINT{mon.right - 2, midY}, mon), "#189: right summon zone");
    }

    // Reveal zone covers both the full-length summon strip and the bar + outer screen margin.
    const RECT win{-1260, 990, -660, 1070}; // 600x80 window above negMon bottom with 10 px outer margin
    const D2D1_RECT_F bar{20.f, 10.f, 580.f, 70.f};
    Require(InEdgeRevealZone(BarEdge::Bottom, POINT{-1800, 1079}, win, negMon, bar), "#189: reveal zone accepts far corner inside monitor summon strip");
    Require(InEdgeRevealZone(BarEdge::Bottom, POINT{-960, 1074}, win, negMon, bar), "#189: reveal zone accepts outer margin between glass and monitor edge");
    Require(InEdgeRevealZone(BarEdge::Bottom, POINT{-960, 1020}, win, negMon, bar), "#189: reveal zone accepts glass interior");
    Require(!InEdgeRevealZone(BarEdge::Bottom, POINT{-960, 989}, win, negMon, bar), "#189: reveal zone rejects above bar window top");
    Require(!InEdgeRevealZone(BarEdge::Bottom, POINT{-1250, 1020}, win, negMon, bar), "#189: reveal zone rejects outside bar span");
    Require(!InEdgeRevealZone(BarEdge::Bottom, POINT{-1500, 1030}, win, negMon, bar), "#189: reveal zone rejects beside bar outside summon strip");
}

void CheckTaskbarWorkSync()
{
    // #191: taskbar edge detection, work-area hold, expansion/shrink, and snapshot sync.
    const RECT mon{0, 0, 1920, 1080};
    const RECT tbBottom{0, 1032, 1920, 1080};
    const RECT tbTop{0, 0, 1920, 48};
    const RECT tbLeft{0, 0, 64, 1080};
    const RECT tbRight{1856, 0, 1920, 1080};

    Require(TaskbarEdgeOnMonitor(tbBottom, mon) == BarEdge::Bottom, "#191: detects bottom taskbar");
    Require(TaskbarEdgeOnMonitor(tbTop, mon) == BarEdge::Top, "#191: detects top taskbar");
    Require(TaskbarEdgeOnMonitor(tbLeft, mon) == BarEdge::Left, "#191: detects left taskbar");
    Require(TaskbarEdgeOnMonitor(tbRight, mon) == BarEdge::Right, "#191: detects right taskbar");
    Require(!TaskbarEdgeOnMonitor(RECT{0, 1078, 1920, 1080}, mon).has_value(), "#191: auto-hidden thin taskbar returns nullopt");
    Require(!TaskbarEdgeOnMonitor(mon, mon).has_value(), "#191: full-monitor rect is not a docked taskbar");

    const RECT workBottom{0, 0, 1920, 1032};
    const RECT workLeft{64, 0, 1920, 1080};
    Require(WorkHoldsTaskbarEdge(tbBottom, mon, workBottom), "#191: work bottom aligns with taskbar top");
    Require(!WorkHoldsTaskbarEdge(tbBottom, mon, mon), "#191: expanded work area no longer holds taskbar edge");
    Require(WorkHoldsTaskbarEdge(tbLeft, mon, workLeft), "#191: work left aligns with taskbar right");

    Require(WorkAreaEdgeExpanded(workBottom, mon, mon, BarEdge::Bottom), "#191: bottom work area expanded to monitor edge");
    Require(WorkAreaEdgeExpanded(RECT{0, 0, 1920, 1000}, RECT{0, 0, 1920, 1032}, mon, BarEdge::Bottom), "#191: bottom work area expanded toward monitor edge");
    Require(!WorkAreaEdgeExpanded(workBottom, workBottom, mon, BarEdge::Bottom), "#191: unchanged restricted bottom work area not expanded");
    Require(WorkAreaEdgeExpanded(workLeft, mon, mon, BarEdge::Left), "#191: left work area expanded to monitor edge");

    const auto snap = MakeTaskbarSnapshot(reinterpret_cast<HMONITOR>(1), tbBottom, mon, workBottom, true);
    Require(snap.has_value() && snap->edge == BarEdge::Bottom, "#191: MakeTaskbarSnapshot captures docked bottom taskbar");
    Require(!MakeTaskbarSnapshot(reinterpret_cast<HMONITOR>(1), tbBottom, mon, mon, true).has_value(),
            "#191: MakeTaskbarSnapshot rejects already-expanded work area when docked edge required");
    Require(!WorkAreaSnapshotSynced(*snap, workBottom, true), "#191: release not synced while work still holds taskbar");
    Require(WorkAreaSnapshotSynced(*snap, mon, true), "#191: release synced once work expands");
    const auto restoreSnap = MakeTaskbarSnapshot(reinterpret_cast<HMONITOR>(1), tbBottom, mon, mon, false);
    Require(restoreSnap.has_value() && WorkAreaSnapshotSynced(*restoreSnap, workBottom, false), "#191: restore synced once work shrinks back");
}
} // namespace

int main()
{
    CheckPlacement();
    CheckRoundTrip();
    CheckAxes();
    CheckEdgeOf();
    CheckBarWindowPlacement();
    CheckTopmostCoverPolicy();
    CheckBarReserve();
    CheckDodgeOverlap();
    CheckAutoHideEdgeZones();
    CheckTaskbarWorkSync();
    if (g_failed)
        return 1;
    std::cout << "bar edge: all checks passed\n";
    return 0;
}
