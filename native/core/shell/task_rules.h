// Pure taskbar window filtering and icon visual comparison rules.
#pragma once

#include "icon_image.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atrium {

// True when the set of minimized windows changed between two snapshots.
inline bool IconicSetChanged(std::vector<HWND> prev, std::vector<HWND> next)
{
    std::ranges::sort(prev);
    std::ranges::sort(next);
    return prev != next;
}

// A window still minimized when Atrium's restore timed out: it restores itself when it reads the
// command, and is brought to front only then (#84).
struct PendingRestore {
    HWND window = nullptr;
    ULONGLONG deadlineMs = 0;
};

enum class WindowShow { Gone, Minimized, Shown };

inline WindowShow ClassifyWindowShow(bool isWindow, bool isVisible, bool isIconic)
{
    if (!isWindow)
        return WindowShow::Gone;
    if (!isVisible || isIconic)
        return WindowShow::Minimized;
    return WindowShow::Shown;
}

// The windows that came back meanwhile; those gone, hidden or past their time are dropped.
template<class ShowOf> std::vector<HWND> TakeRestored(std::vector<PendingRestore>& pending, ULONGLONG nowMs, ShowOf showOf)
{
    std::vector<HWND> restored;
    std::erase_if(pending, [&](const PendingRestore& p) {
        const WindowShow show = showOf(p.window);
        if (show == WindowShow::Shown)
            restored.push_back(p.window);
        return show != WindowShow::Minimized || nowMs >= p.deadlineMs;
    });
    return restored;
}

enum class WindowRestoreMethod { None, SysCommand, AsyncRestore, AsyncShowMaximized };

inline WindowRestoreMethod DecideWindowRestoreMethod(bool iconic, bool enabled, bool hung, bool restoreToMaximized)
{
    if (!iconic)
        return WindowRestoreMethod::None;
    if (!enabled)
        return restoreToMaximized ? WindowRestoreMethod::AsyncShowMaximized : WindowRestoreMethod::AsyncRestore;
    if (hung)
        return WindowRestoreMethod::AsyncRestore;
    return WindowRestoreMethod::SysCommand;
}

struct ActivationCandidate {
    HWND hwnd = nullptr;
    bool visible = false;
    bool enabled = false;
};

inline HWND SelectActivationTarget(HWND owner, bool ownerEnabled, ActivationCandidate enabledPopup, ActivationCandidate lastActivePopup)
{
    auto usable = [owner](const ActivationCandidate& c) { return c.hwnd && c.hwnd != owner && c.visible && c.enabled; };
    if (!ownerEnabled && usable(enabledPopup))
        return enabledPopup.hwnd;
    if (usable(lastActivePopup))
        return lastActivePopup.hwnd;
    return owner;
}

inline bool IsSystemHelperWindowClass(std::wstring_view className) { return className == L"IME" || className == L"MSCTFIME UI" || className == L"GDI+ Window"; }

inline bool IsHiddenMainWindowCandidate(bool visible, bool owned, DWORD exStyle, bool enabled, bool cloaked, std::wstring_view title,
                                        std::wstring_view className)
{
    return !visible && !owned && (exStyle & WS_EX_TOOLWINDOW) == 0 && enabled && !cloaked && !title.empty() && !IsSystemHelperWindowClass(className);
}

struct HiddenWindow {
    RECT rect{};
    DWORD style = 0, exStyle = 0;
    bool owned = false;
    bool enabled = true;
    bool cloaked = false;
    bool hasTitle = true;
    bool systemHelper = false;
};

// The main window of an app that shows none, restored as a last resort (#147): an unowned enabled
// uncloaked titled non-helper window of a window's size, captioned before uncaptioned, then the largest.
// -1 when none is one.
inline int PickHiddenMainWindow(const std::vector<HiddenWindow>& windows)
{
    int best = -1;
    bool bestCaptioned = false;
    int64_t bestArea = 0;
    for (int i = 0; i < int(windows.size()); ++i) {
        const HiddenWindow& w = windows[size_t(i)];
        const int64_t width = int64_t{w.rect.right} - w.rect.left, height = int64_t{w.rect.bottom} - w.rect.top;
        if (w.owned || (w.exStyle & WS_EX_TOOLWINDOW) || !w.enabled || w.cloaked || !w.hasTitle || w.systemHelper || width < 200 || height < 150)
            continue;
        const bool captioned = (w.style & WS_CAPTION) == WS_CAPTION;
        if (best < 0 || captioned > bestCaptioned || (captioned == bestCaptioned && width * height > bestArea)) {
            best = i;
            bestCaptioned = captioned;
            bestArea = width * height;
        }
    }
    return best;
}

inline std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

inline std::wstring FileStem(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        name.resize(dot);
    return name;
}

// A browser's web app (an installed site in its own window) is its own app, not the browser.
inline bool IsWebApp(const std::wstring& aumid, const std::wstring& command)
{
    const std::wstring id = Lower(aumid), cmd = Lower(command);
    return id.find(L"_crx_") != std::wstring::npos || cmd.find(L"--app-id=") != std::wstring::npos || cmd.find(L"--app=") != std::wstring::npos;
}

inline bool IsTaskbarWindowCandidate(LONG_PTR style, LONG_PTR exStyle, bool hasOwner, bool iconic, LONG width, LONG height)
{
    if ((exStyle & WS_EX_TRANSPARENT) && (exStyle & WS_EX_LAYERED))
        return false;
    if ((exStyle & WS_EX_TOOLWINDOW) && (exStyle & WS_EX_NOACTIVATE))
        return false;
    const bool appWindow = (exStyle & WS_EX_APPWINDOW) != 0;
    if (!appWindow && ((exStyle & WS_EX_TOOLWINDOW) != 0 || hasOwner || (exStyle & WS_EX_NOACTIVATE) != 0))
        return false;
    if (!iconic && (width <= 0 || height <= 0))
        return false;
    // Floating desktop mini-widgets (e.g. Thunder accelerator ball) have WS_POPUP without frame chrome.
    const bool popupOnly = (style & WS_POPUP) != 0 && (style & (WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) == 0;
    const bool floatingEx = (exStyle & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0;
    const bool miniRect = (width <= 280 && height <= 160) || (width <= 160 && height <= 280);
    return iconic || !popupOnly || !floatingEx || !miniRect;
}

inline bool IsValidIconImage(const IconImage& img)
{
    return img.width > 0 && img.height > 0 && img.ByteSize() >= static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4;
}

inline std::array<double, 4> SampleIconCell(const IconImage& img, int gx, int gy, int grid)
{
    const int x0 = static_cast<int>((static_cast<int64_t>(gx) * img.width) / grid);
    const int x1 = std::min(img.width, std::max(x0 + 1, static_cast<int>((static_cast<int64_t>(gx + 1) * img.width) / grid)));
    const int y0 = static_cast<int>((static_cast<int64_t>(gy) * img.height) / grid);
    const int y1 = std::min(img.height, std::max(y0 + 1, static_cast<int>((static_cast<int64_t>(gy + 1) * img.height) / grid)));
    const uint8_t* data = img.Data();
    std::array<double, 4> sum{};
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(img.width);
        for (int x = x0; x < x1; ++x) {
            const size_t off = (row + static_cast<size_t>(x)) * 4;
            for (size_t c = 0; c < 4; ++c)
                sum[c] += static_cast<double>(data[off + c]);
            ++count;
        }
    }
    const double scale = count > 0 ? 1.0 / (255.0 * static_cast<double>(count)) : 0.0;
    for (double& v : sum)
        v *= scale;
    return sum;
}

inline bool ImagesVisuallyDifferent(const IconImage& a, const IconImage& b)
{
    const bool validA = IsValidIconImage(a);
    const bool validB = IsValidIconImage(b);
    if (!validA || !validB)
        return validA != validB;
    constexpr int kGrid = 16;
    constexpr double kThreshold = 0.045;
    double diffSum = 0.0;
    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            const auto ca = SampleIconCell(a, gx, gy, kGrid);
            const auto cb = SampleIconCell(b, gx, gy, kGrid);
            for (size_t c = 0; c < 4; ++c)
                diffSum += std::abs(ca[c] - cb[c]);
        }
    }
    const double avgDiff = diffSum / static_cast<double>(kGrid * kGrid * 4);
    return avgDiff > kThreshold;
}

// Browser web apps and mail/chat clients that put a leading "(N) " or "(99+) " unread count in their window title.
inline bool IsBadgeTitleApp(bool webApp, const std::wstring& exePath, const std::wstring& aumid)
{
    if (webApp)
        return true;
    const std::wstring stem = Lower(FileStem(exePath));
    for (const wchar_t* name :
         {L"discord", L"slack", L"telegram", L"thunderbird", L"outlook", L"olk", L"teams", L"ms-teams", L"element", L"signal", L"whatsapp"})
        if (stem == name)
            return true;
    const std::wstring id = Lower(aumid);
    if (!id.empty()) {
        for (const wchar_t* pkg : {L"discord", L"slack", L"telegram", L"thunderbird", L"outlook", L"msteams", L"element", L"signal", L"whatsapp"})
            if (id.find(pkg) != std::wstring::npos)
                return true;
    }
    return false;
}

inline int ParseTitleBadge(const std::wstring& title, bool webApp, const std::wstring& exePath, const std::wstring& aumid)
{
    if (!IsBadgeTitleApp(webApp, exePath, aumid) || title.size() < 4 || title[0] != L'(')
        return 0;
    const size_t close = title.find(L')', 1);
    if (close == std::wstring::npos || close + 1 >= title.size() || title[close + 1] != L' ')
        return 0;
    const std::wstring inner = title.substr(1, close - 1);
    if (inner == L"99+")
        return 100;
    if (inner.empty() || inner.size() > 3 || inner[0] == L'0')
        return 0;
    int value = 0;
    for (wchar_t c : inner) {
        if (c < L'0' || c > L'9')
            return 0;
        value = value * 10 + (c - L'0');
    }
    return value;
}

struct MonitorBounds {
    RECT monitor{};
    RECT work{};
};

inline bool RectIntersectsAnyMonitor(const RECT& r, const std::vector<MonitorBounds>& monitors)
{
    return std::ranges::any_of(monitors, [&](const MonitorBounds& m) {
        return std::min(r.right, m.monitor.right) > std::max(r.left, m.monitor.left) && std::min(r.bottom, m.monitor.bottom) > std::max(r.top, m.monitor.top);
    });
}

struct OffscreenRestorePlan {
    bool shouldRestore = false;
    RECT workspaceRect{};
    UINT showCmd = SW_SHOWNORMAL;
};

// Restores an off-screen non-iconic window (e.g. parked at -32000) to its last valid monitor
// placement, or centers it in the primary work area when rcNormalPosition is also off-screen (#176).
inline OffscreenRestorePlan ComputeOffscreenRestorePlan(const RECT& windowRect, const RECT& normalWorkspaceRect, UINT wpFlags,
                                                        const std::vector<MonitorBounds>& monitors, const MonitorBounds& primary)
{
    if (monitors.empty() || RectIntersectsAnyMonitor(windowRect, monitors))
        return {false, {}, SW_SHOWNORMAL};
    const UINT showCmd = (wpFlags & WPF_RESTORETOMAXIMIZED) != 0 ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    const LONG offX = primary.work.left - primary.monitor.left;
    const LONG offY = primary.work.top - primary.monitor.top;
    const RECT normalScreen{normalWorkspaceRect.left + offX, normalWorkspaceRect.top + offY, normalWorkspaceRect.right + offX,
                            normalWorkspaceRect.bottom + offY};
    const bool validNormal = normalScreen.right > normalScreen.left && normalScreen.bottom > normalScreen.top;
    const bool normalVisible = validNormal && std::ranges::any_of(monitors, [&](const MonitorBounds& m) {
                                   const LONG ow = std::min(normalScreen.right, m.monitor.right) - std::max(normalScreen.left, m.monitor.left);
                                   const LONG oh = std::min(normalScreen.bottom, m.monitor.bottom) - std::max(normalScreen.top, m.monitor.top);
                                   return ow >= 64 && oh >= 40;
                               });
    if (normalVisible)
        return {true, normalWorkspaceRect, showCmd};
    LONG w = normalWorkspaceRect.right - normalWorkspaceRect.left;
    LONG h = normalWorkspaceRect.bottom - normalWorkspaceRect.top;
    if (w <= 0)
        w = windowRect.right - windowRect.left;
    if (h <= 0)
        h = windowRect.bottom - windowRect.top;
    if (w <= 0)
        w = 960;
    if (h <= 0)
        h = 640;
    const LONG primaryWorkW = std::max<LONG>(1, primary.work.right - primary.work.left);
    const LONG primaryWorkH = std::max<LONG>(1, primary.work.bottom - primary.work.top);
    w = std::clamp<LONG>(w, 1, primaryWorkW);
    h = std::clamp<LONG>(h, 1, primaryWorkH);
    const LONG left = primary.work.left + (primaryWorkW - w) / 2;
    const LONG top = primary.work.top + (primaryWorkH - h) / 2;
    return {true, RECT{left - offX, top - offY, left + w - offX, top + h - offY}, showCmd};
}

inline bool OffscreenBelongsToMonitor(HMONITOR windowNearestMonitor, HMONITOR normalNearestMonitor, HMONITOR targetMonitor, HMONITOR primaryMonitor)
{
    if (targetMonitor == nullptr)
        return true;
    if (windowNearestMonitor != nullptr)
        return windowNearestMonitor == targetMonitor;
    return (normalNearestMonitor ? normalNearestMonitor : primaryMonitor) == targetMonitor;
}

// A left click on an app's taskbar button: with several windows it presents their picker, so the user
// chooses the window there (as the Windows taskbar does), instead of one being brought forward in turn.
inline bool ClickOpensWindowPicker(size_t windowCount) { return windowCount > 1; }

inline void FilterWindowsToMonitor(std::vector<HWND>& windows, HMONITOR targetMonitor)
{
    if (!targetMonitor)
        return;
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::erase_if(windows, [&](HWND h) {
        if (MonitorFromWindow(h, MONITOR_DEFAULTTONULL) != nullptr)
            return !OffscreenBelongsToMonitor(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), nullptr, targetMonitor, primary);
        HMONITOR normalMon = nullptr;
        WINDOWPLACEMENT wp{sizeof(wp)};
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(h, &wp) && GetMonitorInfoW(primary, &mi)) {
            const LONG ox = mi.rcWork.left - mi.rcMonitor.left, oy = mi.rcWork.top - mi.rcMonitor.top;
            const RECT sr{wp.rcNormalPosition.left + ox, wp.rcNormalPosition.top + oy, wp.rcNormalPosition.right + ox, wp.rcNormalPosition.bottom + oy};
            if (sr.right > sr.left && sr.bottom > sr.top && MonitorFromRect(&sr, MONITOR_DEFAULTTONULL) != nullptr)
                normalMon = MonitorFromRect(&sr, MONITOR_DEFAULTTONEAREST);
        }
        return !OffscreenBelongsToMonitor(nullptr, normalMon, targetMonitor, primary);
    });
}

} // namespace atrium
