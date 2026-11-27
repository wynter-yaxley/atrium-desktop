#include "ui/widget_stacking.h"

#include <dwmapi.h>

#include <algorithm>
#include <cstddef>

namespace atrium {

namespace {

bool RectsIntersect(const RECT& a, const RECT& b, LONG& ow, LONG& oh)
{
    ow = std::min(a.right, b.right) - std::max(a.left, b.left);
    oh = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
    return ow > 0 && oh > 0;
}

bool IsRealDesktopHost(const StackWindow& w, const RECT& monitorRect)
{
    LONG ow = 0, oh = 0;
    return w.visible && !w.cloaked && !w.topmost && (w.cls == L"Progman" || (w.cls == L"WorkerW" && w.hasDefView)) &&
           RectsIntersect(w.rect, monitorRect, ow, oh);
}

bool IsAppOnMonitor(const StackWindow& w, const RECT& monitorRect)
{
    if (!w.visible || w.cloaked || w.iconic || w.topmost || w.toolwindow || w.owned || w.clickThrough || w.ours || w.widget)
        return false;
    if (w.cls == L"Progman" || w.cls == L"WorkerW" || w.cls == L"Shell_TrayWnd" || w.cls == L"Shell_SecondaryTrayWnd" || w.cls == L"Windows.UI.Core.CoreWindow")
        return false;
    LONG ow = 0, oh = 0;
    return RectsIntersect(w.rect, monitorRect, ow, oh) && (w.hasCaption || (ow >= 64 && oh >= 64) || w.hasTitle || w.appWindow);
}

} // namespace

WidgetStackPlan ComputeWidgetStackPlan(const std::vector<StackWindow>& windows, const RECT& monitorRect, HWND fgRoot, bool fgIsOurs, bool wasShowDesktop)
{
    WidgetStackPlan plan;
    size_t desktopIdx = windows.size(), lowestIdx = windows.size();
    HWND above = nullptr;
    bool fgIsHost = false;
    for (size_t i = 0; i < windows.size(); ++i) {
        const StackWindow& w = windows[i];
        if (IsRealDesktopHost(w, monitorRect)) {
            fgIsHost = fgIsHost || (fgRoot && w.hwnd == fgRoot);
            if (!plan.desktopHost) {
                plan.desktopHost = w.hwnd;
                plan.desktopClass = w.cls;
                desktopIdx = i;
            }
        }
        if (!plan.desktopHost && w.visible && !w.cloaked && !w.widget && !w.topmost)
            above = w.hwnd;
        if (IsAppOnMonitor(w, monitorRect)) {
            plan.lowestApp = w.hwnd;
            plan.lowestClass = w.cls;
            lowestIdx = i;
        }
    }
    plan.showDesktop = plan.desktopHost && plan.lowestApp && desktopIdx < lowestIdx && (fgIsHost || (wasShowDesktop && fgIsOurs));
    if (plan.showDesktop || (!plan.lowestApp && plan.desktopHost))
        plan.after = above ? above : HWND_TOP;
    else
        plan.after = plan.lowestApp ? plan.lowestApp : HWND_BOTTOM;
    return plan;
}

std::wstring WindowClassOf(HWND h)
{
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return cls;
}

std::vector<StackWindow> CollectStackSnapshot(const std::vector<HWND>& widgetHwnds)
{
    const DWORD ownPid = GetCurrentProcessId();
    std::vector<StackWindow> windows;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        StackWindow sw;
        sw.hwnd = h;
        sw.cls = WindowClassOf(h);
        sw.visible = IsWindowVisible(h) != FALSE;
        if (sw.visible) {
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE), ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            wchar_t title[2] = {};
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            sw.cloaked = cloaked != FALSE;
            sw.iconic = IsIconic(h) != FALSE;
            sw.topmost = (ex & WS_EX_TOPMOST) != 0;
            sw.toolwindow = (ex & WS_EX_TOOLWINDOW) != 0;
            sw.appWindow = (ex & WS_EX_APPWINDOW) != 0;
            sw.owned = GetWindow(h, GW_OWNER) != nullptr && !sw.appWindow;
            sw.clickThrough = (ex & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) == (WS_EX_TRANSPARENT | WS_EX_LAYERED);
            sw.hasCaption = (style & WS_CAPTION) != 0;
            sw.hasTitle = InternalGetWindowText(h, title, 2) > 0;
            sw.hasDefView = sw.cls == L"WorkerW" && FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
            sw.ours = pid == ownPid;
            sw.widget = std::ranges::find(widgetHwnds, h) != widgetHwnds.end();
            GetWindowRect(h, &sw.rect);
        }
        windows.push_back(std::move(sw));
    }
    return windows;
}

void ApplyWidgetZOrder(const std::vector<HWND>& widgetHwnds, const WidgetStackPlan& plan, DWORD& swpErr)
{
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    for (HWND hw : widgetHwnds)
        if (GetWindowLongPtrW(hw, GWL_EXSTYLE) & WS_EX_TOPMOST)
            SetWindowPos(hw, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    HWND after = plan.after;
    for (HWND hw : widgetHwnds) {
        if (SetWindowPos(hw, after, 0, 0, 0, 0, flags)) {
            after = hw;
            continue;
        }
        const DWORD err = GetLastError();
        if (plan.lowestApp && after != plan.lowestApp && SetWindowPos(hw, plan.lowestApp, 0, 0, 0, 0, flags)) {
            after = hw;
            continue;
        }
        if (after != HWND_BOTTOM && SetWindowPos(hw, HWND_BOTTOM, 0, 0, 0, 0, flags) && err == ERROR_ACCESS_DENIED)
            continue;
        swpErr = err;
    }
}

} // namespace atrium
