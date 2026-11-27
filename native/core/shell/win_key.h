// The Windows key alone opens Atrium's start menu (setting winKeyStart), Win +
// 1..9, 0 the bar's apps (winNumbers); both off by default. It listens on the
// app's one keyboard hook (shell/keyboard_hook.h, on a thread of its own) and
// holds the key back as it goes down: when another key follows, the two go on
// to Windows together (Win + E and every other shortcut work as before); a
// digit is kept and `notify` hears of it; when the key comes up alone, Windows
// never saw it, so its start menu stays shut and `notify` hears of it instead
// (or, without winKeyStart, the key goes on to Windows after all). Over an
// elevated window, whose input Atrium may not send to, the key is left alone.
// Nothing is injected into Explorer.
#pragma once

#include <windows.h>

#include <atomic>

namespace atrium {

struct WinKeyAction {
    bool swallow = false, notify = false, replayAlone = false, replayCombo = false;
    WPARAM notifyParam = 0;
    DWORD winVk = 0, winScan = 0;
    bool winExtended = false;
};

struct WinKeyState {
    bool held = false, passed = false, used = false, extended = false;
    DWORD digit = 0, key = 0, scan = 0;
    ULONGLONG lastDownMs = 0;
};

// Pure state-machine step for WinKey::OnKey. `winAsyncDown` is queried only when `s.held && s.passed` (swallowed keys never set GetAsyncKeyState).
inline WinKeyAction StepWinKey(WinKeyState& s, DWORD vk, DWORD scan, bool extended, bool down, ULONGLONG nowMs, bool winAsyncDown, bool elevated, bool modified,
                               bool alone, bool numbers)
{
    const bool win = vk == VK_LWIN || vk == VK_RWIN;
    if (s.held && s.passed && (vk != s.key || down) && !winAsyncDown)
        s = WinKeyState{};
    if (s.digit && vk != s.digit && (!s.held || (!win && down)))
        s.digit = 0;
    if (win && down) {
        if (s.held && !s.passed && nowMs - s.lastDownMs > 2000)
            s = WinKeyState{};
        if (!s.held) {
            s.held = true;
            s.used = false;
            s.key = vk;
            s.scan = scan;
            s.extended = extended;
            s.passed = elevated;
        }
        s.lastDownMs = nowMs;
        return {.swallow = !s.passed};
    }
    if (win) {
        const bool held = s.held && vk == s.key && !s.passed;
        if (s.held && vk == s.key)
            s.held = false;
        if (held && !s.used)
            return alone ? WinKeyAction{.swallow = true, .notify = true} :
                           WinKeyAction{.swallow = true, .replayAlone = true, .winVk = s.key, .winScan = s.scan, .winExtended = s.extended};
        return {.swallow = held};
    }
    if (s.digit && vk == s.digit) {
        if (!down)
            s.digit = 0;
        return {.swallow = true};
    }
    if (!down || !s.held || s.passed)
        return {};
    if (vk >= '0' && vk <= '9' && numbers && !modified) {
        s.used = true;
        s.digit = vk;
        return {.swallow = true, .notify = true, .notifyParam = vk == '0' ? 10 : vk - '0'};
    }
    s.passed = true;
    return {.swallow = true, .replayCombo = true, .winVk = s.key, .winScan = s.scan, .winExtended = s.extended};
}

class WinKey {
public:
    // Posts `msg` to `notify`: wParam 0 when the Windows key was pressed alone
    // (if `alone`), 1..10 for Win + 1..9, 0 (if `numbers`).
    WinKey(HWND notify, UINT msg, bool alone, bool numbers);
    ~WinKey();
    WinKey(const WinKey&) = delete;
    WinKey& operator=(const WinKey&) = delete;
    // While Atrium's Start menu is open, the Windows key toggles it even if an elevated window still holds focus.
    void SetStartMenuOpen(bool open);

private:
    static bool OnKey(void* context, DWORD vk, DWORD scan, bool extended, bool down, bool injected, ULONG_PTR extraInfo);
    HWND notify_;
    UINT msg_;
    bool alone_, numbers_;
    std::atomic<bool> startMenuOpen_{false};
    WinKeyState state_;
    int keyToken_ = 0; // the shared hook's registration
};

} // namespace atrium
