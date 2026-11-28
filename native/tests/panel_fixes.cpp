#include "render/panel_glass_host.h"
#include "shell/task_rules.h"
#include "ui/calendar_panel.h"
#include "ui/genie_minimize.h"
#include "ui/surface.h"
#include "ui/tray_panel.h"
#include "ui/window_peek.h"
#include "ui/window_preview.h"

#include <dwmapi.h>

#include <algorithm>
#include <iostream>

using namespace atrium;

namespace {

struct TimerPumpStats {
    int timerTicks = 0;
    int messages = 0;
};

TimerPumpStats PumpTimersWithHiddenOverlay(HINSTANCE inst, const std::function<void(Surface&, Surface&)>& exercise)
{
    Surface regular(inst, L"Atrium Timer Check Regular", false, 0);
    Surface overlay(inst, L"Atrium Timer Check Overlay", false, 0, false, true);
    if (exercise)
        exercise(regular, overlay);

    constexpr UINT_PTR kTimerId = 0x7711;
    TimerPumpStats stats{};
    regular.message = [&](UINT m, WPARAM w, LPARAM, LRESULT&) {
        if (m == WM_TIMER && w == kTimerId) {
            ++stats.timerTicks;
            return true;
        }
        return false;
    };
    if (!regular.Hwnd() || !overlay.Hwnd() || !SetTimer(regular.Hwnd(), kTimerId, 20, nullptr))
        return stats;

    const ULONGLONG deadline = GetTickCount64() + 500;
    while (GetTickCount64() < deadline && stats.timerTicks < 4 && stats.messages < 400) {
        MSG msg{};
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ++stats.messages;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (stats.timerTicks >= 4)
            break;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(std::min<ULONGLONG>(50, deadline - now)), QS_ALLINPUT);
    }
    KillTimer(regular.Hwnd(), kTimerId);
    return stats;
}

void CheckClickThroughAndPeek(auto& check)
{
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    Surface regular(inst, L"Atrium Regular Test", false, 0);
    Surface overlay(inst, L"Atrium ClickThrough Test", false, 0, false, true);
    const LONG_PTR regEx = GetWindowLongPtrW(regular.Hwnd(), GWL_EXSTYLE);
    const LONG_PTR ovEx = GetWindowLongPtrW(overlay.Hwnd(), GWL_EXSTYLE);
    check((regEx & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) == 0, "regular surface is not click-through layered");
    check((ovEx & (WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)) == (WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP),
          "click-through surface sets layered transparent + no redirection bitmap");
    check(SendMessageW(overlay.Hwnd(), WM_NCHITTEST, 0, 0) == HTTRANSPARENT, "click-through surface returns HTTRANSPARENT");

    // Verify DWM thumbnail registration and update on a click-through layered Surface.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"AtriumPeekDummyTarget";
    RegisterClassExW(&wc);
    HWND target =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Target", WS_POPUP, -4000, -4000, 240, 160, nullptr, nullptr, inst, nullptr);
    ShowWindow(target, SW_SHOWNOACTIVATE);
    UpdateWindow(target);
    DwmFlush();
    HTHUMBNAIL thumb = nullptr;
    const HRESULT regHr = DwmRegisterThumbnail(overlay.Hwnd(), target, &thumb);
    check(SUCCEEDED(regHr) && thumb != nullptr, "DwmRegisterThumbnail succeeds on click-through layered Surface");
    if (thumb) {
        SIZE src{};
        const HRESULT szHr = DwmQueryThumbnailSourceSize(thumb, &src);
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcSource = RECT{0, 0, std::max<LONG>(1, src.cx), std::max<LONG>(1, src.cy)};
        p.rcDestination = RECT{0, 0, 240, 160};
        p.fVisible = TRUE;
        p.fSourceClientAreaOnly = FALSE;
        const HRESULT updHr = DwmUpdateThumbnailProperties(thumb, &p);
        check(SUCCEEDED(szHr) && SUCCEEDED(updHr), "DwmUpdateThumbnailProperties succeeds on click-through layered Surface");
        DwmUnregisterThumbnail(thumb);
    }
    WindowPeek peek(inst);
    peek.Show(nullptr);
    check(peek.Window() == nullptr, "WindowPeek ignores null window");
    peek.Show(target);
    peek.Hide();
    check(peek.Window() == nullptr, "WindowPeek clears window on Hide");
    DestroyWindow(target);

    GenieMinimize genie(inst, [](HWND) -> std::optional<RECT> { return std::nullopt; }, [](HWND) { return false; });
    genie.SetEnabled(true);
    genie.SetEnabled(false);

    const TimerPumpStats hiddenStats = PumpTimersWithHiddenOverlay(inst, {});
    check(hiddenStats.timerTicks >= 4 && hiddenStats.messages < 400, "hidden click-through layered Surface does not spin WM_PAINT or starve WM_TIMER");

    const TimerPumpStats showHideStats = PumpTimersWithHiddenOverlay(inst, [](Surface&, Surface& overlay) {
        overlay.Show(RECT{10, 10, 110, 110}, false, POINT{10, 10}, false);
        overlay.Hide(false);
    });
    check(showHideStats.timerTicks >= 4 && showHideStats.messages < 400,
          "click-through layered Surface after Show/Hide does not spin WM_PAINT or starve WM_TIMER");

    // #223: pending keyboard/mouse input yields deferred Surface frames instead of starving input.
    Surface yielding(inst, L"Atrium Yielding Test", false, 0);
    std::vector<int> order; // 1 = key, 2 = paint
    bool armed = false;
    yielding.paint = [&](Canvas&) {
        if (armed)
            order.push_back(2);
    };
    yielding.message = [&](UINT m, WPARAM w, LPARAM, LRESULT& r) {
        if (armed && m == WM_KEYDOWN && w == VK_ESCAPE) {
            order.push_back(1);
            r = 0;
            return true;
        }
        return false;
    };
    yielding.Show(RECT{-4000, -4000, -3760, -3840}, false, POINT{-4000, -4000}, false);
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    armed = true;
    order.clear();
    PostMessageW(yielding.Hwnd(), WM_SURFACE_FRAME, 0, 0);
    PostMessageW(yielding.Hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
    const ULONGLONG yieldDeadline = GetTickCount64() + 1000;
    while (GetTickCount64() < yieldDeadline && (std::ranges::find(order, 1) == order.end() || std::ranges::find(order, 2) == order.end())) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            const ULONGLONG now = GetTickCount64();
            if (now >= yieldDeadline)
                break;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(std::min<ULONGLONG>(50, yieldDeadline - now)), QS_ALLINPUT);
        }
    }
    auto keyIt = std::ranges::find(order, 1);
    auto paintIt = std::ranges::find(order, 2);
    check(keyIt != order.end() && paintIt != order.end() && keyIt < paintIt, "WM_SURFACE_FRAME yields to queued user input before running deferred WM_PAINT");
    yielding.Hide(false);
}

bool RectEq(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }

void CheckOffscreenRestorePlan(auto& check)
{
    const MonitorBounds primary{{0, 0, 1920, 1080}, {0, 48, 1920, 1080}};
    const MonitorBounds leftMon{{-1920, 0, 0, 1080}, {-1920, 0, 0, 1080}};
    const std::vector<MonitorBounds> monitors{primary, leftMon};

    check(!RectIntersectsAnyMonitor({-32000, -32000, -31040, -31360}, {}), "RectIntersectsAnyMonitor returns false for empty monitor list");
    check(!RectIntersectsAnyMonitor({-32000, -32000, -31040, -31360}, monitors), "RectIntersectsAnyMonitor rejects parked -32000 window");
    check(RectIntersectsAnyMonitor({100, 100, 900, 700}, monitors), "RectIntersectsAnyMonitor detects window on primary monitor");
    check(RectIntersectsAnyMonitor({-1500, 100, -500, 700}, monitors), "RectIntersectsAnyMonitor handles negative multi-monitor coordinates");
    check(!RectIntersectsAnyMonitor({1920, 0, 2500, 500}, monitors), "RectIntersectsAnyMonitor ignores edge-touching non-overlapping rect");
    check(!RectIntersectsAnyMonitor({500, 500, 400, 400}, monitors), "RectIntersectsAnyMonitor rejects inverted rect");

    const OffscreenRestorePlan emptyMonPlan = ComputeOffscreenRestorePlan({-32000, -32000, -31040, -31360}, {100, 100, 900, 700}, 0, {}, primary);
    check(!emptyMonPlan.shouldRestore, "ComputeOffscreenRestorePlan is a no-op with empty monitor list");

    const OffscreenRestorePlan onscreenPlan = ComputeOffscreenRestorePlan({100, 100, 900, 700}, {100, 100, 900, 700}, 0, monitors, primary);
    check(!onscreenPlan.shouldRestore, "ComputeOffscreenRestorePlan is a no-op for already on-screen window");

    const OffscreenRestorePlan validNormalPlan = ComputeOffscreenRestorePlan({-32000, -32000, -31040, -31360}, {120, 80, 1080, 720}, 0, monitors, primary);
    check(validNormalPlan.shouldRestore && RectEq(validNormalPlan.workspaceRect, {120, 80, 1080, 720}) && validNormalPlan.showCmd == SW_SHOWNORMAL,
          "Kaspersky -32000 window with valid rcNormalPosition restores to rcNormalPosition");

    const OffscreenRestorePlan maxPlan =
        ComputeOffscreenRestorePlan({-32000, -32000, -31040, -31360}, {120, 80, 1080, 720}, WPF_RESTORETOMAXIMIZED, monitors, primary);
    check(maxPlan.shouldRestore && RectEq(maxPlan.workspaceRect, {120, 80, 1080, 720}) && maxPlan.showCmd == SW_SHOWMAXIMIZED,
          "WPF_RESTORETOMAXIMIZED selects SW_SHOWMAXIMIZED");

    // Both windowRect and rcNormalPosition parked at -32000 with 960x640 size -> centered in primary.work (0,48,1920,1080),
    // then converted back to workspace coordinates by subtracting (offX=0, offY=48).
    const OffscreenRestorePlan offscreenNormalPlan =
        ComputeOffscreenRestorePlan({-32000, -32000, -31040, -31360}, {-32000, -32000, -31040, -31360}, 0, monitors, primary);
    check(offscreenNormalPlan.shouldRestore && RectEq(offscreenNormalPlan.workspaceRect, {480, 196, 1440, 836}) && offscreenNormalPlan.showCmd == SW_SHOWNORMAL,
          "-32000 rcNormalPosition is centered inside primary work area in workspace coordinates");

    const OffscreenRestorePlan clampedPlan =
        ComputeOffscreenRestorePlan({-32000, -32000, -31040, -31360}, {-32000, -32000, -29000, -30000}, 0, monitors, primary);
    check(clampedPlan.shouldRestore && RectEq(clampedPlan.workspaceRect, {0, 0, 1920, 1032}),
          "oversized off-screen rcNormalPosition is clamped to primary work area");

    const OffscreenRestorePlan fallbackSizePlan =
        ComputeOffscreenRestorePlan({-32000, -32000, -32000, -32000}, {-32000, -32000, -32000, -32000}, 0, monitors, primary);
    check(fallbackSizePlan.shouldRestore && RectEq(fallbackSizePlan.workspaceRect, {480, 196, 1440, 836}),
          "degenerate windowRect and rcNormalPosition fall back to 960x640 centered");

    const OffscreenRestorePlan windowSizePlan =
        ComputeOffscreenRestorePlan({-32000, -32000, -31200, -31400}, {-32000, -32000, -32000, -32000}, 0, monitors, primary);
    check(windowSizePlan.shouldRestore && RectEq(windowSizePlan.workspaceRect, {560, 216, 1360, 816}),
          "degenerate rcNormalPosition falls back to windowRect dimensions");

    const OffscreenRestorePlan negMonPlan = ComputeOffscreenRestorePlan({-32000, -32000, -31040, -31360}, {-1500, 100, -500, 700}, 0, monitors, primary);
    check(negMonPlan.shouldRestore && RectEq(negMonPlan.workspaceRect, {-1500, 100, -500, 700}), "negative multi-monitor rcNormalPosition is preserved");

    const OffscreenRestorePlan sliverPlan = ComputeOffscreenRestorePlan({-32000, -32000, -31040, -31360}, {1900, 100, 2400, 700}, 0, monitors, primary);
    check(sliverPlan.shouldRestore && RectEq(sliverPlan.workspaceRect, {710, 216, 1210, 816}),
          "barely-overlapping rcNormalPosition (< 64px visible) is recentered to primary work area");

    auto* const monA = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x1001));
    auto* const monB = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x1002));
    check(OffscreenBelongsToMonitor(monB, nullptr, nullptr, monA), "null targetMonitor accepts any window");
    check(OffscreenBelongsToMonitor(monB, monA, monB, monA), "on-screen window uses windowNearestMonitor");
    check(!OffscreenBelongsToMonitor(monB, monA, monA, monA), "on-screen window on another monitor is excluded");
    check(OffscreenBelongsToMonitor(nullptr, monB, monB, monA), "off-screen window uses normalNearestMonitor when available");
    check(OffscreenBelongsToMonitor(nullptr, nullptr, monA, monA), "off-screen window falls back to primaryMonitor");
    check(!OffscreenBelongsToMonitor(nullptr, nullptr, monB, monA), "off-screen window without normal monitor is excluded from secondary");
}

} // namespace

int main()
{
    int fail = 0;
    auto check = [&](bool ok, const char* msg) {
        if (!ok) {
            std::cerr << "FAIL: " << msg << '\n';
            ++fail;
        }
    };
    check(CalendarCapsuleDate(2026, 11, 18, 2, 2026, false) == L"11月18日 星期三", "Chinese same-year date");
    check(CalendarCapsuleDate(2027, 1, 1, 4, 2026, false) == L"2027年1月1日 星期五", "Chinese cross-year date");
    check(CalendarCapsuleDate(2026, 11, 18, 2, 2026, true) == L"Wed, November 18", "English same-year date");
    check(CalendarCapsuleDate(2027, 1, 1, 4, 2026, true) == L"Fri, January 1, 2027", "English cross-year date");

    const POINT a{500, 900};
    const SIZE d96{4, 4}, d200{8, 8};
    check(!TrayFollowCancelled(100, 100, 90, false, a, {800, 800}, d96, false), "matching click input");
    check(!TrayFollowCancelled(110, 100, 110, false, a, {800, 800}, d96, false), "matching synthetic input");
    check(!TrayFollowCancelled(120, 100, 90, false, a, {501, 901}, d96, false), "1-pixel mouse jitter");
    check(!TrayFollowCancelled(120, 100, 90, false, a, {502, 898}, d96, false), "inside double-click rect");
    check(TrayFollowCancelled(120, 100, 90, false, a, {503, 900}, d96, false), "outside double-click rect horizontally");
    check(TrayFollowCancelled(120, 100, 90, false, a, {500, 897}, d96, false), "outside double-click rect vertically");
    check(!TrayFollowCancelled(120, 100, 90, false, a, {504, 904}, d200, false), "DPI-scaled double-click rect allows 4 px");
    check(!TrayFollowCancelled(120, 100, 90, true, a, {1200, 20}, d96, false), "synthetic pointer in flight ignores cursor distance");
    check(!TrayFollowCancelled(120, 100, 90, true, a, {1200, 20}, d96, true), "the fallback's own button press does not cancel it");
    check(ValidThumbnailSource(S_OK, {1280, 800}), "valid thumbnail source size");
    check(!ValidThumbnailSource(S_OK, {0, 0}) && !ValidThumbnailSource(S_OK, {0, 800}) && !ValidThumbnailSource(S_OK, {1280, 0}),
          "zero thumbnail source size is rejected");
    check(!ValidThumbnailSource(S_OK, {-1, 800}), "negative thumbnail source size is rejected");
    check(!ValidThumbnailSource(E_FAIL, {1280, 800}), "failed DwmQueryThumbnailSourceSize is rejected");
    check(BackupKeepIntervalMs(false, 2000.0) == kBackupIdleIntervalMs, "only desktop widgets use the 250 ms backup tier");
    check(BackupKeepIntervalMs(true, 2000.0) == kBackupActiveIntervalMs, "visible screen panels on the capture monitor use the 100 ms backup tier");
    check(BackupKeepIntervalMs(false, 500.0) == 0.0 && BackupKeepIntervalMs(true, 1000.0) == 0.0,
          "recent screen panel movement keeps every frame up to 1000 ms");
    check(!ShouldDeferPanelInk(true, -1, 100.0, 90.0), "initial panel ink (postedInk == -1) is never deferred");
    check(ShouldDeferPanelInk(true, 0, 200.0, 100.0), "subsequent panel ink defers while backdrop animates within 250 ms");
    check(!ShouldDeferPanelInk(true, 0, 350.0, 100.0), "deferred panel ink flushes at >= 250 ms");
    check(!ShouldDeferPanelInk(false, 1, 200.0, 100.0), "panel ink does not defer when !inkOwed");
    CheckOffscreenRestorePlan(check);
    CheckClickThroughAndPeek(check);
    if (fail)
        return 1;
    std::cout << "PASS: panel_fixes\n";
    return 0;
}
