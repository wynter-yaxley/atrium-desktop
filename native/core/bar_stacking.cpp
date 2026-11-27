// Bar stacking guard and auto-hide cover detection: keeps the bar topmost
// unless a full-screen or maximized app covers its monitor, or the user is
// interacting with a revealed system taskbar.
#include "bar.h"
#include "bar_stacking.h"

#include "desktop_helpers.h"
#include "log.h"
#include "shell/games.h"
#include "shell/shell_taskbar.h"
#include "win32_path.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace atrium {

namespace {

constexpr UINT kGuardFastMs = 150; // while below a system taskbar the user is using

enum class TrayAction { None, Done, Raise };

// Windows the bar never climbs over: menus, tooltips, the shell's own
// surfaces, input method windows.
bool IgnoredCover(HWND w)
{
    wchar_t cls[96] = {};
    GetClassNameW(w, cls, 96);
    const LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    const HWND fg = GetForegroundWindow();
    if (ShouldIgnoreTopmostCover(cls, style, ex, fg && w == fg, fg && GetWindow(w, GW_OWNER) == fg))
        return true;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked != FALSE;
}

bool AllowRaise(std::map<HWND, std::pair<int, ULONGLONG>>& raises, HWND target)
{
    const ULONGLONG now = GetTickCount64();
    for (auto it = raises.begin(); it != raises.end();)
        it = now - it->second.second > 10000 || !IsWindow(it->first) ? raises.erase(it) : std::next(it);
    auto& count = raises.try_emplace(target, 0, now).first->second;
    if (++count.first > 4) {
        if (count.first == 5) {
            wchar_t cls[64] = {};
            GetClassNameW(target, cls, 64);
            Log(L"bar: %s keeps covering the bar; leaving it in front", cls);
        }
        return false;
    }
    return true;
}

bool TrayOverlaps(HWND tray, const RECT& bar, RECT& trayRect)
{
    RECT overlap{};
    return tray && IsWindowVisible(tray) && GetWindowRect(tray, &trayRect) && IntersectRect(&overlap, &trayRect, &bar) && overlap.right - overlap.left > 2 &&
           overlap.bottom - overlap.top > 2;
}

TrayAction GuardTrayTaskbar(HWND visual, HWND input, HWND tray, bool trayOver, bool trayAbove, const RECT& trayRect, const RECT& ours, bool& belowTray,
                            bool& trayHeldUp, std::map<HWND, std::pair<int, ULONGLONG>>& raises, UINT_PTR guardTimer, UINT guardMs)
{
    if (trayOver) {
        POINT cur{};
        GetCursorPos(&cur);
        HWND fg = GetForegroundWindow();
        wchar_t fgCls[64] = {};
        GetClassNameW(fg, fgCls, 64);
        // The pointer counts on the system taskbar outside the bar: over the bar it is meant for the bar.
        const bool usingTray = (PtInRect(&trayRect, cur) && !PtInRect(&ours, cur)) || fg == tray ||
                               wcscmp(fgCls, L"TopLevelWindowForOverflowXamlIsland") == 0 || wcscmp(fgCls, L"Shell_TrayWnd") == 0;
        if (usingTray) {
            // The revealed taskbar stays in front; it slides away again by itself.
            if (!trayAbove) {
                SetWindowPos(visual, tray, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                SetWindowPos(input, visual, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                if (!belowTray)
                    Log(L"bar: below the revealed system taskbar (pointer %ld,%ld, foreground %s)", cur.x, cur.y, fgCls);
            }
            if (!belowTray) // the system taskbar gets the pointer now: watch for it coming back to the bar
                SetTimer(input, guardTimer, kGuardFastMs, nullptr);
            belowTray = true;
            return TrayAction::Done;
        }
        if (trayAbove) {
            const bool wasBelow = belowTray;
            if (wasBelow)
                SetTimer(input, guardTimer, guardMs, nullptr);
            belowTray = false;
            const bool wasHeld = trayHeldUp;
            trayHeldUp = true;
            if (!AllowRaise(raises, tray))
                return TrayAction::Done;
            if (wasBelow || !wasHeld)
                Log(L"bar: over the system taskbar, which an app holds up");
            return TrayAction::Raise;
        }
    }
    if (belowTray) {
        Log(L"bar: the system taskbar went down again");
        SetTimer(input, guardTimer, guardMs, nullptr);
    }
    belowTray = false;
    trayHeldUp = trayOver;
    return TrayAction::None;
}

bool ExclusiveD3dInFront(HMONITOR monitor)
{
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
    if (!root || IsIconic(root) || MonitorFromWindow(root, MONITOR_DEFAULTTONULL) != monitor)
        return false;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(root, &pid) || pid == GetCurrentProcessId())
        return false;
    QUERY_USER_NOTIFICATION_STATE state{};
    return SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
}

} // namespace

// Focus may be on another screen; use this monitor's app Z-order instead.
bool Bar::MonitorCoveredByApp() const
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hmonitor_, &mi);
    std::vector<RECT> tiles; // snapped windows in front, filling the screen together
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (!IsWindowVisible(window) || IsIconic(window))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        const HMONITOR windowMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        if (pid == GetCurrentProcessId() || windowMonitor != hmonitor_)
            continue;
        const LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (!(ex & WS_EX_APPWINDOW) && ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || GetWindow(window, GW_OWNER)))
            continue; // transient UI must not reveal a bar over a full-screen app
        if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED))
            continue;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked)
            continue; // windows on another virtual desktop do not cover this one
        wchar_t cls[64] = {};
        GetClassNameW(window, cls, 64);
        if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 || wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0)
            continue;
        RECT frame{};
        if (!GetWindowRect(window, &frame))
            continue;
        if (auto covered = WindowCoverOnMonitor(windowMonitor, frame, IsZoomed(window) != FALSE, hmonitor_, monitor_)) {
            if (*covered)
                return true;
            // Snapped side by side, the windows fill the screen as a maximized one does.
            RECT bounds = frame; // without the invisible resize borders
            DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds));
            const int slack = MulDiv(8, int(GetDpiForWindow(window)), 96);
            if (IsTile(bounds, mi.rcWork, slack)) {
                tiles.push_back(bounds);
                if (TilesFill(tiles, mi.rcWork, slack))
                    return true;
            }
            // A smaller window in front (a mail opened from a chat, a dialog, a float) does not
            // decide: what fills the screen behind it still does.
        }
    }
    return false;
}

bool WindowOverlapsBar(const RECT& windowBounds, const RECT& barZone)
{
    RECT overlap{};
    return IntersectRect(&overlap, &windowBounds, &barZone) != FALSE && overlap.right > overlap.left && overlap.bottom > overlap.top;
}

bool IsMeetingShareToolbarBounds(LONG_PTR exStyle, const RECT& bounds, const RECT& barZone, float scale)
{
    if (!(exStyle & WS_EX_TOPMOST) || ((exStyle & WS_EX_TRANSPARENT) && (exStyle & WS_EX_LAYERED)))
        return false;
    if (!WindowOverlapsBar(bounds, barZone))
        return false;
    const LONG w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    const float s = std::max(scale, 0.5f);
    return w >= LONG(100.f * s) && h >= LONG(20.f * s) && h <= LONG(140.f * s) && w > h;
}

bool IsMeetingShareExeStem(std::wstring_view exeStem)
{
    if (exeStem.empty())
        return false;
    const std::wstring stem = FoldCase(std::wstring(exeStem));
    static constexpr std::array<std::wstring_view, 9> kStems = {L"feishu",   L"lark",   L"wemeetapp", L"wemeet",  L"zoom",
                                                                L"dingtalk", L"wxwork", L"teams",     L"ms-teams"};
    return std::ranges::find(kStems, std::wstring_view(stem)) != kStems.end();
}

namespace {

bool IsWindowMeetingToolbar(HWND window, LONG_PTR ex, const RECT& bounds, const RECT& barZone, float scale)
{
    if (!IsMeetingShareToolbarBounds(ex, bounds, barZone, scale))
        return false;
    const std::wstring program = ProgramOf(window);
    if (program.empty())
        return false;
    std::wstring file = program.substr(program.find_last_of(L"\\/") + 1);
    if (file.size() > 4 && FoldCase(file.substr(file.size() - 4)) == L".exe")
        file.resize(file.size() - 4);
    return IsMeetingShareExeStem(file);
}

bool ShouldSkipForegroundOverlapCandidate(HWND window, RECT& boundsOut)
{
    if (!IsWindowVisible(window) || IsIconic(window))
        return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId())
        return true;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked)
        return true;
    wchar_t cls[64] = {};
    GetClassNameW(window, cls, 64);
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 || wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0)
        return true;
    return FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &boundsOut, sizeof(boundsOut))) && !GetWindowRect(window, &boundsOut);
}

} // namespace

RECT Bar::BarOverlapZone() const
{
    const LONG barLeft = LONG(scene_.bar.left), barTop = LONG(scene_.bar.top), barRight = LONG(scene_.bar.right);
    const LONG barBottom = LONG(scene_.bar.bottom);
    if (barRight <= barLeft || barBottom <= barTop)
        return RECT{};
    const EdgeFrame frame{edge_, window_};
    const LONG outer = std::max<LONG>(barBottom, frame.FromArea(monitor_).bottom);
    return frame.ToArea(RECT{barLeft, barTop, barRight, outer});
}

bool Bar::MonitorCoveredByMeetingToolbar() const
{
    const RECT zone = BarOverlapZone();
    if (zone.right <= zone.left || zone.bottom <= zone.top)
        return false;
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        RECT bounds{};
        if (ShouldSkipForegroundOverlapCandidate(window, bounds))
            continue;
        if (IsWindowMeetingToolbar(window, GetWindowLongPtrW(window, GWL_EXSTYLE), bounds, zone, scale_))
            return true;
    }
    return false;
}

bool Bar::MonitorOverlappedByApp() const
{
    const RECT zone = BarOverlapZone();
    if (zone.right <= zone.left || zone.bottom <= zone.top)
        return false;
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        RECT bounds{};
        if (ShouldSkipForegroundOverlapCandidate(window, bounds))
            continue;
        const LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (IsWindowMeetingToolbar(window, ex, bounds, zone, scale_))
            return true;
        if (!(ex & WS_EX_APPWINDOW) && ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || GetWindow(window, GW_OWNER)))
            continue;
        if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED))
            continue;
        if (WindowOverlapsBar(bounds, zone))
            return true;
    }
    return false;
}

void Bar::UpdateCover()
{
    if (!visible_) {
        covered_ = revealed_ = false;
        leftAt_ = 0;
        KillTimer(input_, kPointerTimer);
        UpdateAutoHide();
        return;
    }
    // Apply the hide mode independently on each display.
    const std::wstring& mode = settings_.autoHide;
    const bool dodge = settings_.barDodge && mode != L"always";
    const bool allowFullscreenHide = mode != L"never" || settings_.barFullscreenHide;
    const bool expected = mode != L"never" && GetTickCount64() < expectCoverUntil_;
    bool knownFullScreen = false;
    bool covered = false;
    if (!allowFullscreenHide) {
        // Respect autoHide == "never" + barFullscreenHide == false for non-exclusive windows/games;
        // exclusive D3D fullscreen still owns the display.
        knownFullScreen = ExclusiveD3dInFront(hmonitor_);
        covered = knownFullScreen || (dodge && MonitorOverlappedByApp());
    } else if (mode == L"never") {
        knownFullScreen = FullScreenInFront(hmonitor_);
        covered = knownFullScreen || MonitorCoveredByMeetingToolbar() || (dodge && MonitorOverlappedByApp());
    } else if (mode == L"always") {
        knownFullScreen = GameInFront(hmonitor_);
        covered = true;
    } else {
        const bool game = GameInFront(hmonitor_);
        knownFullScreen = game;
        covered = (dodge && MonitorOverlappedByApp()) || MonitorCoveredByApp() || MonitorCoveredByMeetingToolbar() || game || expected;
    }
    if (covered == covered_)
        return;
    covered_ = covered;
    // Maximizing with the pointer on the bar keeps it up until the pointer leaves;
    // in dodge mode, dragging a window edge down with the mouse button pressed lets it tuck right away.
    POINT p{};
    GetCursorPos(&p);
    RECT zone = EdgeZone();
    const bool draggingWindow = dodge && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    revealed_ = covered && !slidOut_ && !draggingWindow && PtInRect(&zone, p) && !(knownFullScreen || FullScreenInFront(hmonitor_));
    leftAt_ = 0;
    if (mode == L"fullscreen" || dodge)
        Log(L"bar: monitor %p %s (%s%s)", hmonitor_, covered ? L"covered: auto-hide" : L"clear", mode.c_str(), dodge ? L"+dodge" : L"");
    else
        Log(L"bar: auto-hide %s (%s)", covered ? L"always" : L"never", mode.c_str());
    if (covered)
        SetTimer(input_, kPointerTimer, kPointerMs, nullptr);
    else
        KillTimer(input_, kPointerTimer);
    UpdateAutoHide();
}

void Bar::Raise()
{
    SetWindowPos(visual_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(input_, visual_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Bar::PlaceWindows(const RECT& win, bool shown)
{
    const UINT show = shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
    const UINT visualZ = (shown && !appliedVisible_) ? 0 : SWP_NOZORDER;
    SetWindowPos(visual_, HWND_TOPMOST, win.left, win.top, win.right - win.left, win.bottom - win.top, SWP_NOACTIVATE | show | visualZ);
    // Input window right below the visual one, region = the rounded shape.
    SetWindowPos(input_, visual_, win.left, win.top, win.right - win.left, win.bottom - win.top, SWP_NOACTIVATE | show);
    applied_ = win;
    appliedVisible_ = shown;
}

bool ShouldIgnoreTopmostCover(std::wstring_view cls, LONG_PTR style, LONG_PTR exStyle, bool isForeground, bool ownerIsForeground)
{
    static const wchar_t* const kClasses[] = {L"#32768",
                                              L"tooltips_class32",
                                              L"Shell_SecondaryTrayWnd",
                                              L"NotifyIconOverflowWindow",
                                              L"TopLevelWindowForOverflowXamlIsland",
                                              L"Xaml_WindowedPopupClass",
                                              L"XamlExplorerHostIslandWindow",
                                              L"Windows.UI.Core.CoreWindow",
                                              L"SysShadow",
                                              L"IME",
                                              L"MSCTFIME UI",
                                              L"ForegroundStaging",
                                              L"MultitaskingViewFrame",
                                              L"Shell_InputSwitchTopLevelWindow",
                                              L"Progman",
                                              L"WorkerW"};
    for (const wchar_t* c : kClasses)
        if (cls == c)
            return true;
    if ((exStyle & WS_EX_TRANSPARENT) && (exStyle & WS_EX_LAYERED))
        return true; // click-through overlays
    const bool menuPopup = (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION && !(exStyle & WS_EX_APPWINDOW);
    return menuPopup && ((exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || isForeground || ownerIsForeground);
}

// Keeps the bar on top: hidden behind a full-screen app, below a revealed
// system taskbar that overlaps it while the user is using that taskbar (the
// pointer on it outside the bar, or the keyboard in it), and otherwise raised again when
// another topmost window covers it. A taskbar held up only by an app's
// flashing button does not push the bar down (the bar shows the app's
// attention itself). A window that keeps pushing itself over the bar
// wins after a few rounds, so two always-on-top windows never fight.
void Bar::GuardStacking(const wchar_t* why)
{
    UpdateCover();
    if (!visible_ || tucked_ || dragging_)
        return;
    const RECT bar = BarRect();
    RECT r{}, overlap{}, trayRect{};
    HWND tray = ShellTaskbar();
    const bool trayOver = TrayOverlaps(tray, bar, trayRect);
    bool trayAbove = false;
    HWND cover = nullptr;
    for (HWND w = GetTopWindow(nullptr); w && w != visual_; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == tray) {
            trayAbove = true;
            continue;
        }
        if (w == input_ || !IsWindowVisible(w))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId())
            continue;
        if (!(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            cover = w; // the bar has lost its topmost place
            break;
        }
        if (GetWindowRect(w, &r) && IntersectRect(&overlap, &r, &bar) && !IgnoredCover(w)) {
            cover = w;
            break;
        }
    }
    const TrayAction action =
        GuardTrayTaskbar(visual_, input_, tray, trayOver, trayAbove, trayRect, EdgeZone(), belowTray_, trayHeldUp_, raises_, kGuardTimer, kGuardMs);
    if (action == TrayAction::Raise) {
        // Up without the user (an app's button flashing): the bar stays in front.
        Raise();
        return;
    }
    if (action == TrayAction::Done || !cover || !AllowRaise(raises_, cover))
        return;
    Raise();
    if (cover != lastCover_) {
        wchar_t cls[64] = {};
        GetClassNameW(cover, cls, 64);
        Log(L"bar: raised over %s (%s)", cls, why ? why : L"check");
        lastCover_ = cover;
    }
}

} // namespace atrium
