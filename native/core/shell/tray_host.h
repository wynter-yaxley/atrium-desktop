// The tray host (setting `trayHost`, "Host the tray"): Atrium receives the apps'
// notification-area icons itself, as the shell does, instead of reading them back
// from Explorer's tray.
//
// Shell_NotifyIcon sends its data to the first window of class Shell_TrayWnd
// (WM_COPYDATA). Atrium keeps a hidden window of that class ahead of Explorer's,
// takes a copy of every add, change and removal (the live picture, the tooltip,
// the callback message, the protocol version) and passes each call on to Explorer
// unchanged, so the system tray stays complete and takes over again the moment
// the window is gone. Every other message to the window goes on to Explorer's
// taskbar too. Apps that were already running add their icons again on
// TaskbarCreated, which the host broadcasts when it starts (and again after
// Explorer restarts). Clicks and hovering are the callback messages the shell
// itself sends (shell/tray_protocol.h). Nothing is injected anywhere. The pattern
// is ManagedShell's TrayService (Cairo, RetroBar).
//
// Never in the way of Windows: the window has a thread of its own (a busy UI
// thread never holds up an app's call); a failure while reading a call still
// passes it on; another program's tray host in front is left there; and the
// guard process puts Explorer's taskbar in front if the window stops answering.
#pragma once

#include "icon_image.h"
#include "shell/tray_protocol.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

enum class TrayHostYieldReason { None, OtherProcess, RapidLoss, StartAllBack };
enum class HostSendStatus { NotSent, Partial, Sent };

const wchar_t* YieldReasonName(TrayHostYieldReason reason);
// Every Shell_TrayWnd in z-order with its program, logged when that changes (#146).
void LogShellTrayWindows();

inline bool ShouldYieldToStartAllBack(bool hasModule, bool disabledForCurrentUser) { return hasModule && !disabledForCurrentUser; }

struct TrayHostYieldPolicy {
    static constexpr ULONGLONG kLossWindowMs = 5000;
    static constexpr ULONGLONG kLossSampleMs = 400;
    static constexpr ULONGLONG kRapidBackoffMs = 60000;
    static constexpr int kRapidLossLimit = 6;

    DWORD shellPid = 0;
    bool hasStartAllBackModule = false;
    bool hasStartAllBack = false;
    ULONGLONG lossWindowStart = 0;
    ULONGLONG lastLossAt = 0;
    ULONGLONG retryAt = 0;
    int rapidLosses = 0;
    int backoffStep = 0;
    TrayHostYieldReason reason = TrayHostYieldReason::None;

    bool Yielded() const { return reason != TrayHostYieldReason::None; }
    TrayHostYieldReason Reason() const { return reason; }

    bool OnShellPid(DWORD pid, bool hasModule, bool disabledForCurrentUser)
    {
        if (pid == shellPid)
            return false;
        shellPid = pid;
        hasStartAllBackModule = hasModule;
        const bool active = ShouldYieldToStartAllBack(hasModule, disabledForCurrentUser);
        hasStartAllBack = active;
        lossWindowStart = 0;
        lastLossAt = 0;
        retryAt = 0;
        rapidLosses = 0;
        backoffStep = 0;
        const bool wasYielded = Yielded();
        reason = active ? TrayHostYieldReason::StartAllBack : TrayHostYieldReason::None;
        return wasYielded && !active;
    }

    bool OnShellPid(DWORD pid, bool startAllBack) { return OnShellPid(pid, startAllBack, false); }

    bool OnStartAllBackActive(bool active)
    {
        if (active == hasStartAllBack)
            return false;
        hasStartAllBack = active;
        lossWindowStart = 0;
        lastLossAt = 0;
        retryAt = 0;
        rapidLosses = 0;
        backoffStep = 0;
        if (active) {
            reason = TrayHostYieldReason::StartAllBack;
            return false;
        }
        const bool cleared = (reason == TrayHostYieldReason::StartAllBack);
        if (cleared)
            reason = TrayHostYieldReason::None;
        return cleared;
    }

    bool OnFirst()
    {
        if (hasStartAllBack) {
            reason = TrayHostYieldReason::StartAllBack;
            return false;
        }
        lossWindowStart = 0;
        lastLossAt = 0;
        retryAt = 0;
        rapidLosses = 0;
        backoffStep = 0;
        const bool wasYielded = Yielded();
        reason = TrayHostYieldReason::None;
        return wasYielded;
    }

    bool OnOtherProcess()
    {
        lossWindowStart = 0;
        lastLossAt = 0;
        rapidLosses = 0;
        if (reason == TrayHostYieldReason::StartAllBack)
            return false;
        const bool newlyYielded = (reason == TrayHostYieldReason::None);
        reason = TrayHostYieldReason::OtherProcess;
        return newlyYielded;
    }

    bool OnExplorerFirst(ULONGLONG now, bool& newlyYielded)
    {
        newlyYielded = false;
        if (hasStartAllBack) {
            newlyYielded = (reason == TrayHostYieldReason::None);
            reason = TrayHostYieldReason::StartAllBack;
            return false;
        }
        if (reason == TrayHostYieldReason::RapidLoss) {
            if (now < retryAt)
                return false;
            reason = TrayHostYieldReason::None;
            lossWindowStart = 0;
            retryAt = 0;
            rapidLosses = 0;
        } else if (reason == TrayHostYieldReason::OtherProcess) {
            reason = TrayHostYieldReason::None;
            lossWindowStart = 0;
            retryAt = 0;
            rapidLosses = 0;
        }
        if (rapidLosses > 0 && now >= lastLossAt && now - lastLossAt < kLossSampleMs)
            return true;
        lastLossAt = now;
        if (rapidLosses == 0 || now - lossWindowStart > kLossWindowMs) {
            lossWindowStart = now;
            rapidLosses = 1;
        } else {
            ++rapidLosses;
        }
        if (rapidLosses >= kRapidLossLimit) {
            const ULONGLONG backoff = backoffStep == 0 ? 10000 : backoffStep == 1 ? 30000 : kRapidBackoffMs;
            backoffStep = std::min(backoffStep + 1, 2);
            reason = TrayHostYieldReason::RapidLoss;
            retryAt = now + backoff;
            newlyYielded = true;
            return false;
        }
        return true;
    }
};

struct FlashingTrayIcon {
    std::wstring hostKey, exe, tip;
    DWORD pid = 0;
    HWND hwnd = nullptr;
    bool operator==(const FlashingTrayIcon&) const = default;
};

struct HostIcon {
    std::wstring key;  // "guid:{...}" or "hwnd:<hwnd>:<uid>"
    HWND hwnd = nullptr;
    UINT uid = 0;
    UINT callback = 0; // 0: the app takes no clicks
    UINT version = 0;  // NOTIFYICON_VERSION; 0 for the oldest protocol
    bool showTip = false; // NIF_SHOWTIP: a version 4 app wants the standard tooltip, no popup requests
    std::wstring tip;
    std::wstring exe;  // lower-case full path of the window's process
    DWORD pid = 0;
    std::shared_ptr<const IconImage> image;
    uint64_t imageVersion = 0;
    bool hidden = false; // NIS_HIDDEN
    uint64_t added = 0;  // order of arrival
    tray::IconFlashState flash; // picture changes for a flashing icon
    ULONGLONG flashUntil = 0;   // flashing (a new message in QQ or WeChat) until then
};

class TrayHost {
public:
    // Posts `msg` (wParam 0) to `notify` when the icons change (unless `onIconsChange` handles it). `readyWaitMs` and `startDelayMs` are for tests only.
    TrayHost(HWND notify, UINT msg, DWORD readyWaitMs = 3000, DWORD startDelayMs = 0, std::function<void()> onYieldChange = {},
             std::function<void()> onIconsChange = {});
    ~TrayHost();
    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;

    bool Running() const { return window_.load() != nullptr; }
    // Another program's tray host (RetroBar, a second shell) is in front: Atrium
    // leaves it there and reads Explorer's tray meanwhile.
    bool Yielded() const { return yielded_; }
    TrayHostYieldReason YieldReason() const { return yieldReason_.load(); }
    std::vector<HostIcon> Icons() const;
    // The programs whose icon flashes now: QQ and WeChat flash theirs for a new message (they
    // have no window to flash while they sit in the tray).
    std::vector<std::wstring> Flashing() const;
    std::vector<FlashingTrayIcon> FlashingIcons() const;
    // Sends the shell's notifications for `action` on `key` at the screen point `at`
    // (the panel's pointer). Returns NotSent when nothing could be delivered, Partial
    // when delivery failed mid-sequence, or Sent when all notifications were queued.
    HostSendStatus Send(const std::wstring& key, tray::HostAction action, POINT at, RECT cell = {});
    // The pointer rests on `key` (empty: on none): the arrival, and for a version 4
    // app drawing its own tooltip, its popup request after a moment and its close.
    void Hover(const std::wstring& key, POINT at, RECT cell = {});
    // Where icons are, for apps that ask (Shell_NotifyIconGetRect): around the last
    // click for the icon clicked, `anchor` (the bar's tray entry) for the others.
    void SetPlaces(RECT anchor)
    {
        std::lock_guard g(mutex_);
        anchor_ = anchor;
    }

private:
    void Run();
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    static void CALLBACK ShellEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG, DWORD, DWORD);
    LRESULT Message(HWND h, UINT m, WPARAM w, LPARAM l);
    void OnTimer(HWND h, WPARAM w);
    void HandleExplorerFront(HWND w, ULONGLONG now, bool wasYielded);
    LRESULT OnNotifyIcon(WPARAM w, const COPYDATASTRUCT* cds);
    void Record(DWORD message, DWORD cbData, const void* nidPtr, HWND hwnd, bool hasGuid, const std::wstring& key, std::shared_ptr<const IconImage> image,
                LRESULT result);
    LRESULT OnIconRect(WPARAM w, const COPYDATASTRUCT* cds);
    LRESULT Forward(UINT m, WPARAM w, LPARAM l, BOOL* sent = nullptr, DWORD* err = nullptr);
    template<class Map> static auto FindIcon(Map& icons, const std::wstring& key, bool hasGuid, HWND hwnd, UINT uid)
    {
        auto it = icons.find(key);
        if (it != icons.end() || hasGuid || !hwnd)
            return it;
        auto match = icons.end();
        for (auto cur = icons.begin(); cur != icons.end(); ++cur) {
            if (cur->second.hwnd == hwnd && cur->second.uid == uid) {
                if (match != icons.end())
                    return icons.end();
                match = cur;
            }
        }
        return match;
    }
    auto FindIconLocked(const std::wstring& key, bool hasGuid, HWND hwnd, UINT uid) { return FindIcon(icons_, key, hasGuid, hwnd, uid); }
    auto FindIconLocked(const std::wstring& key, bool hasGuid, HWND hwnd, UINT uid) const { return FindIcon(icons_, key, hasGuid, hwnd, uid); }
    bool HasIcon(const std::wstring& key, bool hasGuid, HWND hwnd, UINT uid) const
    {
        std::lock_guard g(mutex_);
        return FindIconLocked(key, hasGuid, hwnd, uid) != icons_.end();
    }
    void FinishKeepFirst(HWND first, HWND shell, DWORD shellPid, bool wasYielded);
    void Broadcast();
    void KeepFirst();
    void Prune();
    void LogList();
    void Popup(bool open);

    HWND notify_;
    UINT msg_;
    DWORD startDelayMs_ = 0;
    std::function<void()> onYieldChange_;
    std::function<void()> onIconsChange_;
    UINT taskbarCreated_ = 0, quit_ = 0, hover_ = 0;
    HANDLE ready_ = nullptr, stop_ = nullptr;
    std::atomic<HWND> window_{nullptr};
    std::atomic_bool dirty_ = false;   // changed since the last notice (posted at most every 100 ms)
    std::atomic_bool yielded_ = false;
    std::atomic<TrayHostYieldReason> yieldReason_{TrayHostYieldReason::None};
    std::atomic_bool stopping_ = false; // going: no longer moves itself in front
    std::thread thread_;
    mutable std::mutex mutex_;
    std::map<std::wstring, HostIcon> icons_;
    uint64_t nextAdded_ = 0, nextImage_ = 0;
    RECT anchor_{}, clicked_{}, hovered_{};
    std::wstring clickedKey_;
    std::wstring hoverKey_, popupKey_; // host thread: the icon under the pointer, the one asked to show its popup
    POINT hoverAt_{};
    ULONGLONG lostAt_ = 0, broadcastAt_ = 0;
    TrayHostYieldPolicy yieldPolicy_;
    std::wstring loggedKeys_; // host thread: the list as last logged
    std::vector<std::wstring> flashingKeys_; // host thread: icons flashing at the last look
    bool loggedState_ = false;
    TrayHostYieldReason lastLoggedReason_ = TrayHostYieldReason::None;
    bool loggedRefusedForward_ = false;
};

} // namespace atrium
