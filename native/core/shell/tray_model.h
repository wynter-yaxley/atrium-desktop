// Tray snapshots and actions; evidence contracts: docs/TRAY.md.
#pragma once

#include <windows.h>
#include <unknwn.h>

#include "icon_image.h"
#include "log.h"
#include "pinned_app.h"
#include "shell/desktop_agent_client.h"
#include "shell/tray_click.h"
#include "shell/tray_host.h"
#include "shell/tray_match.h"
#include "shell/tray_pointer.h"
#include "shell/tray_target.h"

#include <winrt/base.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct IUIAutomation;

namespace atrium {

struct TrayIcon {
    std::wstring tip;      // the tooltip; may span lines
    std::wstring exe;      // the program it belongs to, when recognised
    std::shared_ptr<const IconImage> image; // null when unknown
    uint64_t imageVersion = 0; // renderer cache identity; allocator addresses can be reused
    // A one-shade glyph (white or black, as many tray icons are drawn for the taskbar's theme)
    // redrawn in the other ink: for bright glass (`inkedOnLight`) or dark. Null for coloured icons.
    std::shared_ptr<const IconImage> inked;
    bool inkedOnLight = false;
    bool promoted = false; // on the system taskbar rather than in its overflow
    int index = 0;         // position among the icons of the same place
    std::vector<int> runtimeId; // UIA identity, never an ordinal click target
    tray::ShellSession shell; // PID + process creation time + taskbar HWND
    std::wstring matchMethod;
    bool fromRecord = false;
    std::wstring recordId; // stable Windows identity, also before the overflow UI exists
    std::wstring hostKey;  // an icon the tray host received (TrayHost); empty: read from Explorer
    DWORD pid = 0;         // its window's process (hosted)
    bool appTooltip = false; // a version 4 app drawing its own tooltip (NIN_POPUPOPEN): no standard one
    bool flashing = false;   // its icon flashes (hosted): a message waiting
    size_t order = SIZE_MAX; // place in Windows' own order of the tray icons (UIOrderList), if known
    int exeOrdinal = -1;     // position among icons of the same program
    std::optional<tray::TargetKey> explorerTarget; // strict 1-to-1 Explorer binding for hosted fallback
    tray::TargetKey Target() const { return {shell, runtimeId, recordId, exe, tip}; }
    tray::TargetKey ActionTarget() const { return explorerTarget.value_or(Target()); }
};

namespace tray_detail {
struct Found {
    winrt::com_ptr<IUnknown> element;
    HWND host = nullptr;    // the XAML button's top-level window: the taskbar or the overflow popup
    HWND toolbar = nullptr; // a button of the classic tray's toolbar (Windows 10): clicked by messages to it
    std::wstring tip;
    bool promoted = false;
    int index = 0;
    RECT bounds{};
    std::wstring recordId;
    tray::Method method = tray::Method::Unknown;
    std::vector<int> runtimeId;
};
struct Record {
    std::wstring id, exe, tip;
    std::vector<BYTE> png;
    bool promoted = false, hasUid = false;
    UINT uid = 0;
    GUID guid{};
    size_t order = SIZE_MAX;
};
struct ReadDiagnostics {
    size_t records = 0, live = 0, aliases = 0, shared = 0, unknown = 0;
};
bool WaitStep(HANDLE stopEvent, DWORD ms);
tray::ShellSession ReadShellSession();
std::vector<Found> FindButtons(IUIAutomation* uia, bool& available);
std::vector<int> MatchButtons(std::vector<Found>& buttons, std::vector<Record>& records, bool& ordered, const std::vector<PinnedApp>& apps = {},
                              ReadDiagnostics* diagnostics = nullptr);
HWND OverflowWindow();
bool ShowOverflow(IUIAutomation* uia, HANDLE stopEvent = nullptr);
} // namespace tray_detail

class TrayModel {
public:
    // Posts msg: 0=list changed, 1=observed app response, 2=verified native popup,
    // 3=needs user intervention, 4=accepted/unconfirmed (quiet), 5=explicit popup opened,
    // 6=yield tray panel topmost Z-order before physical pointer right-click fallback.
    // No HRESULT is treated as an application acknowledgement.
    TrayModel(HWND notify, UINT msg, std::function<std::vector<PinnedApp>()> appNames = {});
    ~TrayModel();
    TrayModel(const TrayModel&) = delete;
    TrayModel& operator=(const TrayModel&) = delete;

    void Refresh(); // asynchronous
    std::vector<TrayIcon> Icons() const
    {
        std::lock_guard g(mutex_);
        return snapshot_ ? *snapshot_ : std::vector<TrayIcon>{};
    }
    bool Loaded() const
    {
        std::lock_guard g(mutex_);
        return loaded_;
    }
    bool Available() const // the shell's tray was found in the last read
    {
        std::lock_guard g(mutex_);
        return available_;
    }
    // A left click (Invoke) or a right click (the context menu) on `icon`.
    void Click(const TrayIcon& icon, bool right, RECT cell = {});
    void DoubleClick(const TrayIcon& icon, bool continuation = false, RECT cell = {});
    void OpenSystemTray(); // explicit asynchronous fallback
    // GetLastInputInfo time of the last real pointer click sent to a tray button (0: none).
    static DWORD LastSyntheticInput();
    // True while the physical pointer right-click fallback is moving the cursor.
    static bool SyntheticPointerInFlight() { return tray::g_pointerInFlight; }
    // Opens an existing background session; false when the app is not in the tray/running.
    bool OpenExisting(const std::wstring& exe, DWORD preferredPid = 0);
    // The tray host (setting `trayHost`): the apps' icons as they send them, clicked with
    // the shell's own messages. Icons only Explorer's tray has stay in the list. UI thread.
    void SetDesktopAgent(DesktopAgentClient* agent)
    {
        std::lock_guard buildGuard(snapshotBuildMutex_);
        std::lock_guard g(mutex_);
        desktopAgent_.store(agent);
        hostInk_.clear();
    }
    void NotifyRemoteChanged(bool refresh = false)
    {
        RebuildSnapshot(refresh);
        if (refresh)
            Refresh();
    }
    void SetHosting(bool on);
    bool Hosting() const
    {
        if (DesktopAgentClient* agent = desktopAgent_.load())
            return agent->Hosting();
        return host_ != nullptr;
    }
    bool HostingSynced(bool on) const
    {
        if (DesktopAgentClient* agent = desktopAgent_.load())
            return agent->HostingSynced(on);
        std::lock_guard g(mutex_);
        return on == (host_ != nullptr);
    }
    bool HostYielded() const
    {
        if (DesktopAgentClient* agent = desktopAgent_.load())
            return agent->Yielded();
        std::lock_guard g(mutex_);
        return host_ && host_->Yielded();
    }
    TrayHostYieldReason HostYieldReason() const
    {
        if (DesktopAgentClient* agent = desktopAgent_.load())
            return agent->YieldReason();
        std::lock_guard g(mutex_);
        return host_ ? host_->YieldReason() : TrayHostYieldReason::None;
    }
    // A hosted icon only (the shell protocol has these; Explorer's buttons do not):
    // the middle button, and the pointer resting on an icon (null: on none).
    void MiddleClick(const TrayIcon& icon, RECT cell = {});
    HostSendStatus SendHosted(const std::wstring& key, tray::HostAction action, POINT at, RECT cell);
    void Hover(const TrayIcon* icon, RECT cell = {});
    void SetHostPlaces(RECT anchor)
    {
        if (DesktopAgentClient* agent = desktopAgent_.load())
            agent->SetPlaces(anchor);
        else if (host_)
            host_->SetPlaces(anchor);
    }
    // The programs whose tray icon flashes now (hosted icons only): a message waiting.
    std::vector<std::wstring> Flashing() const
    {
        if (DesktopAgentClient* agent = desktopAgent_.load())
            return !agent->Yielded() ? agent->Flashing() : std::vector<std::wstring>{};
        std::lock_guard g(mutex_);
        return host_ && !host_->Yielded() ? host_->Flashing() : std::vector<std::wstring>{};
    }
    std::vector<FlashingTrayIcon> FlashingIcons() const
    {
        if (DesktopAgentClient* agent = desktopAgent_.load())
            return !agent->Yielded() ? agent->FlashingIcons() : std::vector<FlashingTrayIcon>{};
        std::lock_guard g(mutex_);
        return host_ && !host_->Yielded() ? host_->FlashingIcons() : std::vector<FlashingTrayIcon>{};
    }

    // First line of a tooltip, for display.
    static std::wstring Title(const std::wstring& tip) { return tray::Title(tip); }

private:
    struct Job {
        bool refresh = false;
        TrayIcon icon;
        tray::Action action = tray::Action::Left;
        bool systemTray = false;
        bool allowProgramWake = false;
    };
    struct WorkerState;
    void Worker();
    bool DequeueWorkerJob(WorkerState& ws, Job& job);
    void RunClickJob(IUIAutomation* uia, const Job& job, const tray::ShellSession& session, ULONGLONG findStart);
    void RunRefreshJob(WorkerState& ws, const tray::ShellSession& session, ULONGLONG findStart);
    void RebuildSnapshot(bool forceNotify);
    bool ClickHosted(const TrayIcon& icon, tray::HostAction action, RECT cell = {}, bool allowProgramWake = false);
    void EnqueueUrgent(Job job);
    HWND notify_;
    UINT msg_;
    std::function<std::vector<PinnedApp>()> appNames_;
    HANDLE stopEvent_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex snapshotBuildMutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<TrayIcon> icons_;
    std::shared_ptr<const std::vector<TrayIcon>> snapshot_;
    bool loaded_ = false, available_ = false, stop_ = false;
    std::thread thread_;
    std::atomic<DesktopAgentClient*> desktopAgent_{nullptr};
    std::unique_ptr<TrayHost> host_;
    // Ink for a hosted one-shade glyph, by picture version.
    mutable std::map<uint64_t, std::pair<std::shared_ptr<const IconImage>, bool>> hostInk_;
    mutable std::map<std::wstring, std::pair<std::wstring, std::wstring>> hostNames_; // FileDescription and ProductName, for icons without a tooltip
    // Windows' order of the icons by "guid:{...}", "uid:<exe>|<uid>" and "exe:<exe>" (the first of the program).
    std::map<std::wstring, size_t> windowsOrder_;
    tray::ShellSession lastReconcileSession_; // worker: what the last "reconcile" line said
    size_t lastReconcileRecords_ = 0, lastReconcileLive_ = 0;
};

// Syncs the `trayHost` setting against the active epoch's confirmed state so a failed
// start or stop is retried even if the same boolean setting is applied again.
inline bool ApplyTrayHostSetting(TrayModel* tray, bool enabled)
{
    if (!tray || tray->HostingSynced(enabled))
        return false;
    tray->SetHosting(enabled);
    Log(L"core: tray host %s", tray->Hosting() ? L"on" : enabled ? L"failed to start" : L"off");
    return true;
}

} // namespace atrium
