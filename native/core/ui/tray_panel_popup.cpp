// TrayPanel external menu-follow, hover-popup session retention, and topmost Z-order coordination.
#include "ui/tray_panel.h"

#include "log.h"
#include "shell/keyboard_hook.h"
#include "shell/process_integrity.h"
#include "shell/tray_click.h"

#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace atrium {

namespace {

enum class ProcessMatch { Match, Mismatch, AccessDenied };

ProcessMatch MatchProcessExe(DWORD pid, const std::wstring& exe)
{
    if (!pid || exe.empty())
        return ProcessMatch::Mismatch;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[32768];
        DWORD size = DWORD(std::size(buf));
        const bool same = QueryFullProcessImageNameW(process, 0, buf, &size) && CompareStringOrdinal(buf, int(size), exe.c_str(), -1, TRUE) == CSTR_EQUAL;
        CloseHandle(process);
        return same ? ProcessMatch::Match : ProcessMatch::Mismatch;
    }
    return GetLastError() == ERROR_ACCESS_DENIED ? ProcessMatch::AccessDenied : ProcessMatch::Mismatch;
}

bool IsShellTrayClass(const wchar_t* cls)
{
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 || wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0;
}

bool IsCustomMenuStyle(HWND hwnd, const wchar_t* cls)
{
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION && (ex & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) && !(ex & WS_EX_APPWINDOW) &&
           _wcsicmp(cls, L"tooltips_class32") != 0;
}

struct MenuFollow {
    HWINEVENTHOOK hook = nullptr;
    POINT at{}, placed{};
    std::wstring exe;
    UINT_PTR timer = 0;
    DWORD pid = 0, clickInput = 0;
    ULONGLONG until = 0;
    HWND menu = nullptr;
    int moves = 0;
} g_follow;

void StopFollow()
{
    if (g_follow.hook)
        UnhookWinEvent(g_follow.hook);
    if (g_follow.timer)
        KillTimer(nullptr, g_follow.timer);
    g_follow.hook = nullptr, g_follow.menu = nullptr, g_follow.timer = 0, g_follow.pid = 0;
}

void CALLBACK FollowExpired(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (timer == g_follow.timer && GetTickCount64() >= g_follow.until)
        StopFollow();
}

POINT MenuPlace(POINT at, int w, int h)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    return tray::PlaceMenu(at, w, h, mi.rcWork);
}

bool FollowStillValid()
{
    if (GetTickCount64() > g_follow.until)
        return StopFollow(), false;
    constexpr int kKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_RETURN, VK_ESCAPE, VK_SPACE};
    const bool keyDown = std::ranges::any_of(kKeys, [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; });
    LASTINPUTINFO input{sizeof(input)};
    POINT cursor{};
    const SIZE dblClick{GetSystemMetrics(SM_CXDOUBLECLK), GetSystemMetrics(SM_CYDOUBLECLK)};
    if (!GetLastInputInfo(&input) || !GetCursorPos(&cursor) ||
        TrayFollowCancelled(input.dwTime, g_follow.clickInput, TrayModel::LastSyntheticInput(), TrayModel::SyntheticPointerInFlight(), g_follow.at, cursor,
                            dblClick, keyDown)) {
        StopFollow();
        return false;
    }
    return true;
}

bool FollowProcessAllowed(DWORD pid, std::wstring& path)
{
    if (g_follow.exe.empty() || !pid)
        return false;
    DWORD selfSession = 0;
    DWORD targetSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &selfSession) || !ProcessIdToSessionId(pid, &targetSession) || selfSession != targetSession)
        return false;
    DWORD targetRid = 0;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[32768];
        DWORD size = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(process, 0, buf, &size))
            path.assign(buf, size);
        targetRid = ProcessIntegrityRid(process);
        CloseHandle(process);
    }
    // Target at higher or unknown integrity cannot be moved across UIPI.
    const IntegrityRelation relation = CompareIntegrity(targetRid, SelfIntegrityRid());
    if (relation == IntegrityRelation::Higher || relation == IntegrityRelation::Unknown)
        return false;
    if (path.empty() || CompareStringOrdinal(path.c_str(), -1, g_follow.exe.c_str(), -1, TRUE) != CSTR_EQUAL)
        return false;
    return g_follow.menu || g_follow.pid == 0 || pid == g_follow.pid;
}

void AdjustMovedMenu(HWND hwnd)
{
    RECT r{};
    if (hwnd != g_follow.menu || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r))
        return;
    if ((std::abs(r.left - g_follow.placed.x) > 2 || std::abs(r.top - g_follow.placed.y) > 2) && g_follow.moves < 3) {
        const POINT p = MenuPlace(g_follow.at, r.right - r.left, r.bottom - r.top);
        g_follow.placed = p, ++g_follow.moves;
        SetWindowPos(hwnd, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Log(L"tray: the menu went to %ld,%ld again: back to %ld,%ld", r.left, r.top, p.x, p.y);
    }
}

void PlaceInitialFollowedMenu(HWND hwnd, DWORD pid, const wchar_t* name, const wchar_t* cls, bool nativeMenu)
{
    RECT r{};
    if (GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r))
        return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(g_follow.at, MONITOR_DEFAULTTONEAREST), &mi);
    if (w < 40 || h < 20 || w > (mi.rcWork.right - mi.rcWork.left) * 3 / 5 || h > (mi.rcWork.bottom - mi.rcWork.top) * 19 / 20)
        return;
    if (!tray::ShouldMoveFollowedMenu(r, g_follow.at, mi.rcMonitor, mi.rcWork, nativeMenu)) {
        StopFollow();
        return;
    }
    const POINT p = MenuPlace(g_follow.at, w, h);
    if (!SetWindowPos(hwnd, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        Log(L"tray: %s's menu (%s) at %ld,%ld could not be moved (%lu)", name, cls, r.left, r.top, GetLastError());
        StopFollow();
        return;
    }
    Log(L"tray: %s's menu (%s) moved from %ld,%ld to %ld,%ld", name, cls, r.left, r.top, p.x, p.y);
    g_follow.menu = hwnd, g_follow.pid = pid, g_follow.placed = p, g_follow.moves = 0, g_follow.until = GetTickCount64() + 800;
    if (g_follow.timer)
        KillTimer(nullptr, g_follow.timer);
    g_follow.timer = SetTimer(nullptr, 0, 800, FollowExpired);
    if (!g_follow.timer)
        StopFollow();
}

void CALLBACK MenuShown(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if ((event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_LOCATIONCHANGE) || !FollowStillValid())
        return;
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || (!g_follow.menu && event != EVENT_OBJECT_SHOW))
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring path;
    if (!pid || (g_follow.menu && (hwnd != g_follow.menu || pid != g_follow.pid)) || !FollowProcessAllowed(pid, path))
        return;
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, 64))
        return;
    const bool nativeMenu = wcscmp(cls, L"#32768") == 0;
    if (!nativeMenu && !IsCustomMenuStyle(hwnd, cls))
        return;
    g_follow.menu ? AdjustMovedMenu(hwnd) : PlaceInitialFollowedMenu(hwnd, pid, PathFindFileNameW(path.c_str()), cls, nativeMenu);
}

void FollowMenu(POINT at, const std::wstring& exe, DWORD preferredPid)
{
    StopFollow();
    if (exe.empty() || _wcsicmp(PathFindFileNameW(exe.c_str()), L"explorer.exe") == 0)
        return;
    LASTINPUTINFO input{sizeof(input)};
    if (!GetLastInputInfo(&input))
        return;
    g_follow.clickInput = input.dwTime, g_follow.at = at, g_follow.exe = exe, g_follow.pid = preferredPid, g_follow.until = GetTickCount64() + 3000;
    g_follow.hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, MenuShown, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (g_follow.hook)
        g_follow.timer = SetTimer(nullptr, 0, 3000, FollowExpired);
    if (!g_follow.timer)
        StopFollow();
}

bool IsMenuPopupWindow(HWND root, DWORD menuPid, const std::wstring& menuExe)
{
    if (!root || !IsWindowVisible(root))
        return false;
    if (g_follow.menu && root == g_follow.menu)
        return true;
    wchar_t cls[64]{};
    if (!GetClassNameW(root, cls, 64) || IsShellTrayClass(cls))
        return false;
    if (wcscmp(cls, L"#32768") == 0)
        return true;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(root, &pid);
    if (!pid || pid == GetCurrentProcessId())
        return false;
    const ProcessMatch match = (menuPid && pid == menuPid) ? ProcessMatch::Match : MatchProcessExe(pid, menuExe);
    if (match == ProcessMatch::AccessDenied)
        return IsCustomMenuStyle(root, cls) && (root == GetForegroundWindow() || (GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
    if (match != ProcessMatch::Match)
        return false;
    GUITHREADINFO gui{sizeof(gui)};
    return (GetGUIThreadInfo(tid, &gui) && (gui.flags & GUI_INMENUMODE)) || IsCustomMenuStyle(root, cls);
}

bool IsHoverPopupWindow(HWND hwnd, DWORD hoverPid, const std::wstring& hoverExe)
{
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (!root || !IsWindowVisible(root))
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    if (!pid || pid == GetCurrentProcessId())
        return false;
    if ((!hoverPid || pid != hoverPid) && MatchProcessExe(pid, hoverExe) != ProcessMatch::Match)
        return false;
    wchar_t cls[64]{};
    if (!GetClassNameW(root, cls, 64) || _wcsicmp(cls, L"tooltips_class32") == 0 || IsShellTrayClass(cls))
        return false;
    RECT r{};
    if (!GetWindowRect(root, &r))
        return false;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST), &mi);
    const LONG_PTR style = GetWindowLongPtrW(root, GWL_STYLE), ex = GetWindowLongPtrW(root, GWL_EXSTYLE);
    return tray::IsHoverPopupStyle(style, ex, wcscmp(cls, L"#32768") == 0, r, mi.rcWork);
}

HWND FindVisibleHoverPopup(DWORD hoverPid, const std::wstring& hoverExe)
{
    HWND fg = GetForegroundWindow();
    if (IsHoverPopupWindow(fg, hoverPid, hoverExe))
        return GetAncestor(fg, GA_ROOT);
    struct Ctx {
        DWORD pid;
        const std::wstring& exe;
        HWND found = nullptr;
    } ctx{hoverPid, hoverExe};
    EnumWindows(
        [](HWND h, LPARAM param) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(param);
            return IsHoverPopupWindow(h, c.pid, c.exe) ? (c.found = h, FALSE) : TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

} // namespace

void TrayPanel::FollowMenuAt(POINT at, const std::wstring& exe, DWORD preferredPid) { FollowMenu(at, exe, preferredPid); }
void TrayPanel::StopMenuFollow() { StopFollow(); }

bool TrayPanel::MatchesIconProcess(DWORD pid, const TrayIcon& icon)
{
    return pid != 0 && pid != GetCurrentProcessId() && ((icon.pid && pid == icon.pid) || MatchProcessExe(pid, icon.exe) == ProcessMatch::Match);
}

void TrayPanel::YieldTopmostForPointerFallback()
{
    if (!surface_.Visible())
        return;
    HideTip();
    SetWindowPos(surface_.Hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    yieldedTopmost_ = true;
}

void TrayPanel::RestoreTopmostIfYielded()
{
    if (yieldedTopmost_ && surface_.Visible())
        SetWindowPos(surface_.Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    yieldedTopmost_ = false;
}

void TrayPanel::StartMenuSession(const TrayIcon& icon)
{
    EndPopupSession();
    sessionMode_ = SessionMode::Menu, menuObserved_ = false, menuSessionAt_ = GetTickCount64();
    menuPid_ = icon.pid ? icon.pid : tray::RecordPid(icon.recordId);
    menuExe_ = icon.exe, hoverPopupHwnd_ = nullptr;
    if (HWND hwnd = surface_.Hwnd()) {
        mouseWatcherToken_ = KeyboardHook::Instance().AddMouseDownWatcher(hwnd, kOutsideClickMsg);
        SetTimer(hwnd, kMenuTimer, kMenuPollMs, nullptr);
    }
}

void TrayPanel::StartHoverSession(const TrayIcon& icon, HWND popup)
{
    EndPopupSession();
    sessionMode_ = SessionMode::Hover, menuObserved_ = true, menuSessionAt_ = GetTickCount64();
    menuPid_ = icon.pid ? icon.pid : tray::RecordPid(icon.recordId);
    menuExe_ = icon.exe, hoverPopupHwnd_ = popup;
    if (HWND hwnd = surface_.Hwnd()) {
        mouseWatcherToken_ = KeyboardHook::Instance().AddMouseDownWatcher(hwnd, kOutsideClickMsg);
        SetTimer(hwnd, kMenuTimer, kMenuPollMs, nullptr);
    }
}

void TrayPanel::EndPopupSession()
{
    if (mouseWatcherToken_ > 0) {
        KeyboardHook::Instance().RemoveMouseDownWatcher(mouseWatcherToken_);
        mouseWatcherToken_ = 0;
    }
    if (HWND hwnd = surface_.Hwnd())
        KillTimer(hwnd, kMenuTimer);
    sessionMode_ = SessionMode::None, menuObserved_ = false, hoverSettle_ = false, menuSessionAt_ = 0, panelReenterUntil_ = 0, menuPid_ = 0,
    hoverPopupHwnd_ = nullptr, menuExe_.clear();
    if (yieldedTopmost_ && surface_.Visible() && GetForegroundWindow() == surface_.Hwnd())
        RestoreTopmostIfYielded();
}

bool TrayPanel::IsPointInActiveMenu(POINT screenPt) const
{
    HWND under = WindowFromPoint(screenPt);
    HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
    return root && (root == surface_.Hwnd() || (tip_ && root == tip_) || IsMenuPopupWindow(root, menuPid_, menuExe_));
}

bool TrayPanel::IsPointInActivePopup(POINT screenPt) const
{
    if (sessionMode_ == SessionMode::Menu)
        return IsPointInActiveMenu(screenPt);
    HWND under = WindowFromPoint(screenPt);
    HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
    return root &&
           (root == surface_.Hwnd() || (tip_ && root == tip_) || (hoverPopupHwnd_ && root == hoverPopupHwnd_) || IsHoverPopupWindow(root, menuPid_, menuExe_));
}

bool TrayPanel::IsActiveMenuOpen() const
{
    if (g_follow.menu && IsWindowVisible(g_follow.menu))
        return true;
    struct Ctx {
        DWORD pid;
        const std::wstring& exe;
        bool open = false;
    } ctx{menuPid_, menuExe_};
    EnumWindows(
        [](HWND h, LPARAM param) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(param);
            return IsMenuPopupWindow(h, c.pid, c.exe) ? (c.open = true, FALSE) : TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.open;
}

bool TrayPanel::IsActiveHoverPopupOpen() const
{
    if (hoverPopupHwnd_ && IsWindow(hoverPopupHwnd_) && IsWindowVisible(hoverPopupHwnd_))
        return true;
    return FindVisibleHoverPopup(menuPid_, menuExe_) != nullptr;
}

void TrayPanel::HandleMenuSessionTimer()
{
    if (IsActiveMenuOpen()) {
        menuObserved_ = true;
        return;
    }
    if (!menuObserved_ && GetTickCount64() - menuSessionAt_ < 3500)
        return;
    if (GetForegroundWindow() == surface_.Hwnd()) {
        EndPopupSession();
        return;
    }
    if (menuObserved_ && GetTickCount64() < panelReenterUntil_) {
        SetForegroundWindow(surface_.Hwnd());
        if (GetForegroundWindow() == surface_.Hwnd())
            EndPopupSession();
        return;
    }
    Close();
}

void TrayPanel::HandleHoverSessionTimer()
{
    HWND fg = GetForegroundWindow();
    if (fg == surface_.Hwnd()) {
        EndPopupSession();
        return;
    }
    const RECT panelRect = surface_.Rect();
    POINT cur{};
    const bool cursorInPanel = GetCursorPos(&cur) && PtInRect(&panelRect, cur);
    const bool fgIsHoverPopup = fg && (fg == hoverPopupHwnd_ || IsHoverPopupWindow(fg, menuPid_, menuExe_));
    if (fg && !fgIsHoverPopup) {
        if (cursorInPanel && GetTickCount64() < panelReenterUntil_)
            SetForegroundWindow(surface_.Hwnd());
        else
            Close();
        return;
    }
    if (IsActiveHoverPopupOpen())
        return;
    if (cursorInPanel || GetTickCount64() < panelReenterUntil_) {
        SetForegroundWindow(surface_.Hwnd());
        if (GetForegroundWindow() == surface_.Hwnd())
            EndPopupSession();
        return;
    }
    Close();
}

void TrayPanel::HandleSettleTimer()
{
    KillTimer(surface_.Hwnd(), kSettleTimer);
    const bool wasHoverSettle = std::exchange(hoverSettle_, false);
    HWND fg = GetForegroundWindow();
    if (wasHoverSettle && sessionMode_ == SessionMode::None) {
        if (const auto idx = IconIndex(hover_); idx && *idx < icons_.size() && !icons_[*idx].hostKey.empty()) {
            const TrayIcon& hovered = icons_[*idx];
            const DWORD hPid = hovered.pid ? hovered.pid : tray::RecordPid(hovered.recordId);
            if (IsHoverPopupWindow(fg, hPid, hovered.exe)) {
                StartHoverSession(hovered, GetAncestor(fg, GA_ROOT));
                return;
            }
            if (HWND found = FindVisibleHoverPopup(hPid, hovered.exe)) {
                StartHoverSession(hovered, found);
                return;
            }
        }
    }
    wchar_t cls[64] = {};
    if (fg)
        GetClassNameW(fg, cls, 64);
    if (sessionMode_ == SessionMode::None && fg != surface_.Hwnd() && !IsShellTrayClass(cls))
        Close();
}

void TrayPanel::OnOutsideMouseDown(LPARAM l)
{
    if (!surface_.Visible() || sessionMode_ == SessionMode::None)
        return;
    const POINT screenPt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    const RECT panelRect = surface_.Rect();
    if (PtInRect(&panelRect, screenPt)) {
        panelReenterUntil_ = GetTickCount64() + 300;
        return;
    }
    if (!IsPointInActivePopup(screenPt))
        Close();
}

void TrayPanel::HandleDeactivate(HWND activating)
{
    if (sessionMode_ != SessionMode::None)
        return;
    if (lastClick_ && GetTickCount64() - lastWhen_ <= GetDoubleClickTime()) {
        hoverSettle_ = false;
        SetTimer(surface_.Hwnd(), kSettleTimer, kSettleMs, nullptr);
        return;
    }
    if (const auto idx = IconIndex(hover_); idx && *idx < icons_.size() && !icons_[*idx].hostKey.empty()) {
        const TrayIcon& hovered = icons_[*idx];
        const DWORD hPid = hovered.pid ? hovered.pid : tray::RecordPid(hovered.recordId);
        HWND other = activating ? activating : GetForegroundWindow();
        if (IsHoverPopupWindow(other, hPid, hovered.exe)) {
            StartHoverSession(hovered, GetAncestor(other, GA_ROOT));
            return;
        }
        if (HWND found = FindVisibleHoverPopup(hPid, hovered.exe)) {
            StartHoverSession(hovered, found);
            return;
        }
        const RECT panelRect = surface_.Rect();
        POINT cur{};
        if ((!other || other == surface_.Hwnd()) && GetCursorPos(&cur) && PtInRect(&panelRect, cur)) {
            hoverSettle_ = true;
            SetTimer(surface_.Hwnd(), kSettleTimer, 50, nullptr);
            return;
        }
    }
    Close();
}

} // namespace atrium
