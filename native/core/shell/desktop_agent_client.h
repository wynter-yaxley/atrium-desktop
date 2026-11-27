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
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct DesktopAgentCallbacks {
    std::function<void()> onTrayChanged;
    std::function<void()> onYieldChanged;
    std::function<AgentDragOverResult(POINT)> onDragOver;
    std::function<void()> onDragLeave;
    std::function<void(const std::wstring&, const std::vector<std::wstring>&)> onDropFiles;
    std::function<bool(const std::wstring&, const std::vector<std::wstring>&)> onDropFilesAccepted;
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
};

class DesktopAgentClient {
public:
    explicit DesktopAgentClient(DesktopAgentCallbacks callbacks = {}, DesktopAgentClientOptions options = {});
    ~DesktopAgentClient();
    DesktopAgentClient(const DesktopAgentClient&) = delete;
    DesktopAgentClient& operator=(const DesktopAgentClient&) = delete;

    bool EvtServerReady() const { return evtReady_.load(); }
    DWORD AgentPid() const { return agentPid_.load(); }
    bool Running() const { return agentPid_.load() != 0; }
    bool Degraded() const { return degraded_.load(); }

    void SetTrayHosting(bool on);
    bool Hosting() const;
    bool Yielded() const;
    TrayHostYieldReason YieldReason() const;
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
    AgentEventReply HandleEvent(const AgentEventMessage& evt);

    void SupervisorLoop();
    bool SpawnAndConnect(winrt::handle& procOut, DWORD& pidOut);
    void ReplayDesiredState();
    void ClearAgentStateOnExit();
    SendCommandOutcome SendCommand(const AgentCommandMessage& cmd, AgentCommandReply* replyOut, DWORD timeoutMs);
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
    bool hosting_ = false;
    bool hasAnchor_ = false;
    RECT anchor_{};
    std::vector<AgentBarRect> desiredDropBars_;
    std::vector<AgentBarRect> confirmedDropBars_;
    bool yielded_ = false;
    TrayHostYieldReason yieldReason_ = TrayHostYieldReason::None;
    std::vector<CachedIcon> icons_;
    std::map<uint64_t, std::shared_ptr<const IconImage>> imageCache_;
    RecentOpTracker seenDropOps_;

    std::mutex cmdPipeMutex_;
    HANDLE cmdPipe_ = INVALID_HANDLE_VALUE;

    std::thread evtThread_;
    std::thread supervisorThread_;
};

} // namespace atrium
