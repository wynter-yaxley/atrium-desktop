#include "shell/games.h"
#include "shell/shell_taskbar.h"
#include "shell/task_rules.h"
#include <cstdio>
#include <thread>
#include <vector>

namespace {

bool TestWindowResponsive()
{
    auto run = [](bool pump, UINT timeoutMs, ULONGLONG& elapsedMs) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr), stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HWND hwnd = nullptr;
        std::thread worker([&ready, &stop, &hwnd, pump] {
            hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
            MSG msg{};
            if (pump)
                PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
            SetEvent(ready);
            while (pump && MsgWaitForMultipleObjectsEx(1, &stop, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1)
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                    DispatchMessageW(&msg);
            if (!pump)
                WaitForSingleObject(stop, 10000);
            DestroyWindow(hwnd);
        });
        const bool created = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 && hwnd != nullptr;
        const ULONGLONG start = GetTickCount64();
        const bool ok = created && atrium::WindowResponsive(hwnd, timeoutMs);
        elapsedMs = GetTickCount64() - start;
        SetEvent(stop);
        worker.join();
        for (HANDLE h : {ready, stop})
            CloseHandle(h);
        return created ? (ok ? 1 : 0) : -1;
    };
    ULONGLONG elapsed = 0;
    if (atrium::WindowResponsive(nullptr, 50) || run(true, 1000, elapsed) != 1 || run(false, 150, elapsed) != 0 || elapsed < 100 || elapsed >= 1000) {
        std::fprintf(stderr, "FAIL: shell_taskbar WindowResponsive check failed (%llu ms)\n", elapsed);
        return false;
    }
    const HWND a = reinterpret_cast<HWND>(uintptr_t{0x101}), b = reinterpret_cast<HWND>(uintptr_t{0x202});
    if (atrium::IconicSetChanged({a, b}, {b, a}) || !atrium::IconicSetChanged({a}, {a, b}) || !atrium::IconicSetChanged({a, b}, {a})) {
        std::fprintf(stderr, "FAIL: shell_taskbar IconicSetChanged check failed\n");
        return false;
    }
    return true;
}

bool TestGamePredicates()
{
    const RECT mon{0, 0, 1920, 1080}, work{0, 0, 1920, 1040}, negMon{-1920, 0, 0, 1080};
    if (!atrium::IsFullscreenGameBounds(mon, mon, WS_POPUP, false) || !atrium::IsFullscreenGameBounds(mon, mon, WS_POPUP, true) ||
        !atrium::IsFullscreenGameBounds(negMon, negMon, WS_POPUP, false) || atrium::IsFullscreenGameBounds(work, mon, WS_OVERLAPPEDWINDOW, true) ||
        atrium::IsFullscreenGameBounds(mon, mon, WS_OVERLAPPEDWINDOW, true) || atrium::IsFullscreenGameBounds(work, mon, WS_OVERLAPPEDWINDOW, false)) {
        std::fprintf(stderr, "FAIL: shell_taskbar IsFullscreenGameBounds check failed\n");
        return false;
    }
    if (!atrium::IsGamePidCacheValid(1000, 3000, 2, 2) || atrium::IsGamePidCacheValid(3000, 3000, 2, 2) || atrium::IsGamePidCacheValid(1000, 3000, 1, 2)) {
        std::fprintf(stderr, "FAIL: shell_taskbar IsGamePidCacheValid check failed\n");
        return false;
    }
    return true;
}

bool TestPendingRestoresAndHiddenWindows()
{
    const HWND a = reinterpret_cast<HWND>(uintptr_t{0x101});
    if (atrium::ClassifyWindowShow(false, false, false) != atrium::WindowShow::Gone ||
        atrium::ClassifyWindowShow(true, false, false) != atrium::WindowShow::Minimized ||
        atrium::ClassifyWindowShow(true, true, true) != atrium::WindowShow::Minimized ||
        atrium::ClassifyWindowShow(true, true, false) != atrium::WindowShow::Shown) {
        std::fprintf(stderr, "FAIL: shell_taskbar ClassifyWindowShow check failed\n");
        return false;
    }
    std::vector<atrium::PendingRestore> pending{{a, 2000}};
    auto ready = atrium::TakeRestored(pending, 500, [](HWND) { return atrium::ClassifyWindowShow(true, false, false); });
    if (!ready.empty() || pending.size() != 1) {
        std::fprintf(stderr, "FAIL: shell_taskbar hidden pending restore dropped early\n");
        return false;
    }
    ready = atrium::TakeRestored(pending, 1000, [](HWND) { return atrium::ClassifyWindowShow(true, true, false); });
    if (ready.size() != 1 || ready[0] != a || !pending.empty()) {
        std::fprintf(stderr, "FAIL: shell_taskbar shown pending restore not raised\n");
        return false;
    }
    if (!atrium::IsSystemHelperWindowClass(L"IME") || !atrium::IsSystemHelperWindowClass(L"MSCTFIME UI") ||
        !atrium::IsSystemHelperWindowClass(L"GDI+ Window") || atrium::IsSystemHelperWindowClass(L"Notepad")) {
        std::fprintf(stderr, "FAIL: shell_taskbar IsSystemHelperWindowClass check failed\n");
        return false;
    }
    if (atrium::IsHiddenMainWindowCandidate(false, false, 0, false, false, L"App", L"MainWnd") ||
        atrium::IsHiddenMainWindowCandidate(false, false, 0, true, true, L"App", L"MainWnd") ||
        atrium::IsHiddenMainWindowCandidate(false, false, 0, true, false, L"", L"MainWnd") ||
        atrium::IsHiddenMainWindowCandidate(false, false, 0, true, false, L"Default IME", L"IME") ||
        atrium::IsHiddenMainWindowCandidate(false, false, 0, true, false, L"MSCTFIME UI", L"MSCTFIME UI") ||
        atrium::IsHiddenMainWindowCandidate(false, false, 0, true, false, L"GDI+ Window", L"GDI+ Window") ||
        !atrium::IsHiddenMainWindowCandidate(false, false, 0, true, false, L"Main Window", L"AppFrame")) {
        std::fprintf(stderr, "FAIL: shell_taskbar IsHiddenMainWindowCandidate check failed\n");
        return false;
    }
    const std::vector<atrium::HiddenWindow> hidden{
        {{0, 0, 1200, 800}, WS_OVERLAPPEDWINDOW, 0, false, false, false, true, false},
        {{0, 0, 1200, 800}, WS_OVERLAPPEDWINDOW, 0, false, true, true, true, false},
        {{0, 0, 1200, 800}, WS_OVERLAPPEDWINDOW, 0, false, true, false, false, false},
        {{0, 0, 1200, 800}, WS_OVERLAPPEDWINDOW, 0, false, true, false, true, true},
    };
    if (atrium::PickHiddenMainWindow(hidden) != -1) {
        std::fprintf(stderr, "FAIL: shell_taskbar PickHiddenMainWindow accepted invalid candidate\n");
        return false;
    }
    return true;
}

bool TestModalRestoreAndActivation()
{
    using atrium::ActivationCandidate;
    using atrium::ClickOpensWindowPicker;
    using atrium::DecideWindowRestoreMethod;
    using atrium::SelectActivationTarget;
    using atrium::WindowRestoreMethod;
    if (ClickOpensWindowPicker(0) || ClickOpensWindowPicker(1) || !ClickOpensWindowPicker(2) || !ClickOpensWindowPicker(3)) {
        std::fprintf(stderr, "FAIL: shell_taskbar ClickOpensWindowPicker check failed\n");
        return false;
    }
    if (DecideWindowRestoreMethod(false, true, false, false) != WindowRestoreMethod::None ||
        DecideWindowRestoreMethod(false, false, true, true) != WindowRestoreMethod::None ||
        DecideWindowRestoreMethod(true, true, false, false) != WindowRestoreMethod::SysCommand ||
        DecideWindowRestoreMethod(true, true, false, true) != WindowRestoreMethod::SysCommand ||
        DecideWindowRestoreMethod(true, false, false, false) != WindowRestoreMethod::AsyncRestore ||
        DecideWindowRestoreMethod(true, false, false, true) != WindowRestoreMethod::AsyncShowMaximized ||
        DecideWindowRestoreMethod(true, true, true, false) != WindowRestoreMethod::AsyncRestore ||
        DecideWindowRestoreMethod(true, false, true, true) != WindowRestoreMethod::AsyncShowMaximized) {
        std::fprintf(stderr, "FAIL: shell_taskbar DecideWindowRestoreMethod check failed\n");
        return false;
    }
    const HWND owner = reinterpret_cast<HWND>(uintptr_t{0x101});
    const HWND modal = reinterpret_cast<HWND>(uintptr_t{0x202});
    const HWND popup = reinterpret_cast<HWND>(uintptr_t{0x303});
    if (SelectActivationTarget(owner, false, {modal, true, true}, {popup, true, false}) != modal ||
        SelectActivationTarget(owner, false, {modal, false, true}, {popup, true, true}) != popup ||
        SelectActivationTarget(owner, false, {owner, true, true}, {popup, true, true}) != popup ||
        SelectActivationTarget(owner, false, {modal, true, false}, {popup, false, true}) != owner ||
        SelectActivationTarget(owner, true, {modal, true, true}, {owner, true, true}) != owner ||
        SelectActivationTarget(owner, true, {modal, true, true}, {popup, true, true}) != popup ||
        SelectActivationTarget(owner, true, {modal, true, true}, {popup, true, false}) != owner) {
        std::fprintf(stderr, "FAIL: shell_taskbar SelectActivationTarget check failed\n");
        return false;
    }
    return true;
}

} // namespace

int main()
{
    if (!TestWindowResponsive() || !TestGamePredicates() || !TestPendingRestoresAndHiddenWindows() || !TestModalRestoreAndActivation())
        return 1;
    std::printf("ok: shell_taskbar checks passed\n");
    return 0;
}
