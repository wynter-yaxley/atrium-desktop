// Core-side supervisor and IPC client for the Medium-integrity desktop interaction agent.
#pragma once

#include "shell/desktop_agent_ipc.h"
#include "shell/tray_host.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct DesktopAgentCallbacks {
    std::function<void()> onTrayChanged;
    std::function<void()> onYieldChanged;
    std::function<AgentDragOverResult(POINT)> onDragOver;
    std::function<AgentDragOverResult(POINT, uint64_t)> onDragOverEpoch;
    std::function<void()> onDragLeave;
    std::function<void(uint64_t)> onDragLeaveEpoch;
    std::function<void(const std::wstring&, const std::vector<std::wstring>&)> onDropFiles;
    std::function<bool(const std::wstring&, const std::vector<std::wstring>&)> onDropFilesAccepted;
    std::function<bool(const std::wstring&, const std::vector<std::wstring>&, uint64_t)> onDropFilesAcceptedEpoch;
    std::function<void(DWORD)> onAgentPidChanged;
};

struct DesktopAgentClientOptions {
    DWORD sessionId = 0;
    DWORD corePid = 0;
    uint64_t generation = 0;
    std::wstring cmdPipeOverride;
    std::wstring evtPipeOverride;
    bool launchSupervisor = true;
    bool allowUnboundTestEvtPeer = false;
    bool allowSameProcessPeer = false;
    // Optional test hook replacing ShellLaunch(..., RequireMedium). Returns an owned waitable handle or nullptr.
    std::function<HANDLE(const std::wstring& args, DWORD& errorOut)> spawnAgentOverride;
    DWORD pingWaitMs = 3000;
    std::function<void()> onBeforeHandleEventForTest;
    bool allowElevatedTestPeer = false;
};

class DesktopAgentClient {
public:
    explicit DesktopAgentClient(DesktopAgentCallbacks callbacks = {}, DesktopAgentClientOptions options = {});
    ~DesktopAgentClient();
    DesktopAgentClient(const DesktopAgentClient&) = delete;
    DesktopAgentClient& operator=(const DesktopAgentClient&) = delete;

    bool EvtServerReady() const { return evtReady_.load(); }
    DWORD AgentPid() const { return agentPid_.load(); }
    uint64_t ActiveAgentEpoch() const
    {
        std::lock_guard lock(stateMutex_);
        return activeAgentPid_ != 0 ? activeAgentEpoch_ : 0;
    }
    bool IsActiveEpoch(uint64_t epoch) const
    {
        std::lock_guard lock(stateMutex_);
        return epoch != 0 && activeAgentPid_ != 0 && activeAgentEpoch_ == epoch;
    }
    template<class Fn> bool RunIfActiveEpoch(uint64_t epoch, Fn&& fn) const
    {
        std::shared_lock actionLock(epochActionMutex_);
        if (!IsActiveEpoch(epoch))
            return false;
        fn();
        return true;
    }
    bool Running() const { return agentPid_.load() != 0; }
    bool Degraded() const { return degraded_.load(); }

    void SetTrayHosting(bool on);
    bool Hosting() const;
    bool HostingSynced(bool on) const;
    bool Yielded() const
    {
        std::lock_guard lock(stateMutex_);
        return yielded_;
    }
    TrayHostYieldReason YieldReason() const
    {
        std::lock_guard lock(stateMutex_);
        return yieldReason_;
    }
    std::vector<HostIcon> Icons() const;
    std::vector<std::wstring> Flashing() const;
    std::vector<FlashingTrayIcon> FlashingIcons() const;

    HostSendStatus Send(const std::wstring& key, tray::HostAction action, POINT at, RECT cell = {}, bool* higherIntegrityOut = nullptr);
    void Hover(const std::wstring& key, POINT at, RECT cell = {});
    void SetPlaces(RECT anchor);
    void SyncDropBars(const std::vector<AgentBarRect>& bars);

    void Shutdown();

private:
    struct CachedIcon {
        HostIcon icon;
        bool flashing = false;
    };

    enum class SendCommandOutcome { Succeeded, RejectedBeforeWrite, FailedOrTimedOutAfterWrite };

    void EvtServerLoop();
    void ServeEvtPipe(HANDLE pipe);
    bool VerifyEvtPeer(HANDLE pipe, uint64_t& verifiedEpochOut);
    bool IsEvtEpochValidLocked(uint64_t verifiedEpoch) const;
    bool ApplyTraySnapshot(const AgentTraySnapshot& snapshot, uint64_t verifiedEpoch, bool& yieldChangedOut);
    AgentEventReply HandleDragOrDropEvent(const AgentEventMessage& evt, uint64_t verifiedEpoch, bool& epochValidOut);
    AgentEventReply HandleEvent(const AgentEventMessage& evt, uint64_t verifiedEpoch, bool& epochValidOut);

    void SupervisorLoop();
    bool SpawnAndConnect(winrt::handle& procOut, DWORD& pidOut);
    int TryAttachCmdPeer(HANDLE pipe, const std::wstring& exePath, const std::wstring& spawnNonce, const FILETIME& spawnFloor, HANDLE launchedHandle,
                         DWORD launchedPid, const FILETIME& launchedCreation, bool requireNonElevated, winrt::handle& procOut, DWORD& pidOut);
    void ReplayDesiredState();
    template<class PullFn, class CommitFn> void FlushDesiredLoop(DWORD timeoutMs, PullFn&& pull, CommitFn&& commit);
    void FlushTrayHosting();
    void FlushPlaces();
    void FlushDropBars();
    void ClearAgentStateOnExit();
    bool EnsureCmdPipeConnectedLocked(DWORD timeoutMs, uint64_t requiredEpoch);
    SendCommandOutcome SendCommand(const AgentCommandMessage& cmd, AgentCommandReply* replyOut, DWORD timeoutMs, uint64_t requiredEpoch = 0);
    void CloseCmdPipeLocked();

    DesktopAgentCallbacks callbacks_;
    DesktopAgentClientOptions options_;
    DWORD sessionId_ = 0;
    DWORD corePid_ = 0;
    uint64_t generation_ = 0;
    std::wstring cmdPipeName_;
    std::wstring evtPipeName_;

    HANDLE quitEvent_ = nullptr;
    std::atomic_bool shutdownCalled_{false};
    std::atomic_bool evtReady_{false};
    std::atomic<DWORD> agentPid_{0};
    std::atomic_bool degraded_{false};
    std::atomic<uint64_t> nextOpId_{1};

    mutable std::mutex stateMutex_;
    DWORD activeAgentPid_ = 0;
    FILETIME activeAgentCreationTime_{};
    uint64_t activeAgentEpoch_ = 0;
    bool hosting_ = false;
    uint64_t desiredHostingSeq_ = 0;
    uint64_t confirmedHostingSeq_ = 0;
    uint64_t confirmedHostingEpoch_ = 0;
    uint64_t attemptedHostingEpoch_ = 0;
    bool hasAnchor_ = false;
    RECT anchor_{};
    uint64_t desiredAnchorSeq_ = 0;
    uint64_t confirmedAnchorSeq_ = 0;
    uint64_t confirmedAnchorEpoch_ = 0;
    std::vector<AgentBarRect> desiredDropBars_;
    std::vector<AgentBarRect> confirmedDropBars_;
    uint64_t desiredDropBarsSeq_ = 0;
    uint64_t confirmedDropBarsSeq_ = 0;
    uint64_t confirmedDropBarsEpoch_ = 0;
    uint64_t attemptedDropBarsEpoch_ = 0;
    bool yielded_ = false;
    TrayHostYieldReason yieldReason_ = TrayHostYieldReason::None;
    std::vector<CachedIcon> icons_;
    std::map<uint64_t, std::shared_ptr<const IconImage>> imageCache_;
    RecentOpTracker seenDropOps_;

    mutable std::shared_mutex epochActionMutex_;
    std::mutex desiredStateSendMutex_;
    std::mutex activeEvtPipeMutex_;
    HANDLE activeEvtPipe_ = INVALID_HANDLE_VALUE;
    HANDLE activeEvtCancelEvent_ = nullptr;
    std::mutex cmdPipeMutex_;
    HANDLE cmdPipe_ = INVALID_HANDLE_VALUE;
    uint64_t cmdPipeEpoch_ = 0;

    std::thread evtThread_;
    std::thread supervisorThread_;
};

} // namespace atrium
