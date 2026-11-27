// Pass the physical Windows key down to the next hook so third-party shortcuts
// can see it. If it comes up alone, mask the system Start menu and release the
// key before opening Atrium Start. Ordinary combinations stay physical.
#pragma once

#include <windows.h>

#include <atomic>

namespace atrium {

enum class WinKeyUse { Alone, Forwarded, BarDigit };

struct WinKeyAction {
    bool swallow = false, notify = false, maskRelease = false;
    WPARAM notifyParam = 0;
};

struct WinKeyState {
    DWORD key = 0, digit = 0;
    WinKeyUse use = WinKeyUse::Alone;
};

inline bool IsWindowsKey(DWORD vk) { return vk == VK_LWIN || vk == VK_RWIN; }

// Ignore the current key-up when checking for stale state: the low-level hook
// runs before Windows applies that event to its asynchronous key state.
inline bool LostWindowsKey(const WinKeyState& s, DWORD vk, bool down, bool winAsyncDown) { return s.key != 0 && (vk != s.key || down) && !winAsyncDown; }

inline WinKeyAction StepWinKey(WinKeyState& s, DWORD vk, bool down, bool winAsyncDown, bool elevated, bool modified, bool injected, bool alone, bool numbers)
{
    const bool win = IsWindowsKey(vk);
    if (win && injected)
        return {};
    if (LostWindowsKey(s, vk, down, winAsyncDown))
        s = {};
    if (s.digit && vk != s.digit && (!s.key || (!win && down)))
        s.digit = 0;
    if (win && down) {
        if (!s.key) {
            s.key = vk;
            s.use = elevated || modified ? WinKeyUse::Forwarded : WinKeyUse::Alone;
        } else if (vk != s.key) {
            s.use = WinKeyUse::Forwarded;
        }
        return {};
    }
    if (win) {
        if (vk != s.key)
            return {};
        s.key = 0;
        const bool notify = alone && s.use == WinKeyUse::Alone;
        return {.notify = notify, .maskRelease = notify || s.use == WinKeyUse::BarDigit};
    }
    if (s.digit && vk == s.digit && !injected) {
        if (!down)
            s.digit = 0;
        return {.swallow = true};
    }
    if (!down || !s.key || s.use == WinKeyUse::Forwarded)
        return {};
    if (numbers && vk >= '0' && vk <= '9' && !modified && !injected) {
        s.use = WinKeyUse::BarDigit;
        s.digit = vk;
        return {.swallow = true, .notify = true, .notifyParam = vk == '0' ? 10 : WPARAM(vk - '0')};
    }
    // KwikPaste can consume the physical V before Atrium sees it. Its Start
    // suppression key is also a non-Win key and cancels a pending solo tap here.
    s.use = WinKeyUse::Forwarded;
    return {};
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
