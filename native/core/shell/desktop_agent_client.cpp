#include "shell/desktop_agent_client.h"

#include "log.h"
#include "settings.h"
#include "shell/shell_launch.h"
#include "win32_path.h"

#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace atrium {

namespace {

constexpr DWORD kCmdTimeoutMs = 1500;
constexpr DWORD kFastCmdTimeoutMs = 500;
constexpr DWORD kQuitCmdTimeoutMs = 300;
constexpr DWORD kMonitorIntervalMs = 1000;

HostIcon ConvertDtoToHostIcon(const AgentTrayIconDto& dto, const std::shared_ptr<const IconImage>& cachedImage)
{
    HostIcon icon;
    icon.key = dto.key, icon.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(dto.hwnd)), icon.uid = dto.uid;
    icon.callback = dto.callback, icon.version = dto.version, icon.showTip = dto.showTip, icon.tip = dto.tip;
    icon.exe = dto.exe, icon.pid = dto.pid, icon.imageVersion = dto.imageVersion, icon.hidden = dto.hidden, icon.added = dto.added;
    if (dto.width > 0 && dto.height > 0 && !dto.pixels.empty())
        icon.image = std::make_shared<const IconImage>(dto.width, dto.height, dto.pixels);
    else if (dto.imageVersion != 0)
        icon.image = cachedImage;
    return icon;
}

FILETIME ComputeSpawnFloor()
{
    FILETIME spawnFloor{};
    GetSystemTimeAsFileTime(&spawnFloor);
    ULARGE_INTEGER floorTicks{};
    floorTicks.LowPart = spawnFloor.dwLowDateTime;
    floorTicks.HighPart = spawnFloor.dwHighDateTime;
    constexpr ULONGLONG kSpawnFloorSlack100Ns = 1'000'000ULL;
    floorTicks.QuadPart = (floorTicks.QuadPart > kSpawnFloorSlack100Ns) ? (floorTicks.QuadPart - kSpawnFloorSlack100Ns) : 0;
    spawnFloor.dwLowDateTime = floorTicks.LowPart;
    spawnFloor.dwHighDateTime = floorTicks.HighPart;
    return spawnFloor;
}

bool LaunchAgentProcessHandle(const DesktopAgentClientOptions& options, const std::wstring& exePath, const std::wstring& params, winrt::handle& handleOut,
                              DWORD& pidOut, FILETIME& creationOut)
{
    handleOut = {};
    pidOut = 0;
    creationOut = {};
    if (options.spawnAgentOverride) {
        DWORD err = 0;
        handleOut.attach(options.spawnAgentOverride(params, err));
        if (!handleOut && err != 0)
            return false;
    } else {
        LaunchRequest req;
        req.target = exePath, req.parameters = params, req.kind = LaunchTargetKind::Executable;
        req.privilege = LaunchPrivilege::RequireMedium, req.show = SW_HIDE, req.captureProcess = true;
        const LaunchResult result = ShellLaunch(req, kDefaultShellLaunchTimeoutMs);
        if (result.outcome == LaunchOutcome::Failed)
            return Log(L"desktop agent client: ShellLaunch RequireMedium failed (%lu)", result.error), false;
        handleOut.attach(result.process);
    }
    if (!handleOut)
        return true;
    pidOut = GetProcessId(handleOut.get());
    FILETIME ex{}, kt{}, ut{};
    if (pidOut != 0 && GetProcessTimes(handleOut.get(), &creationOut, &ex, &kt, &ut) != FALSE)
        return true;
    Log(L"desktop agent client: launched process handle missing queryable identity");
    return false;
}

} // namespace

DesktopAgentClient::DesktopAgentClient(DesktopAgentCallbacks callbacks, DesktopAgentClientOptions options)
    : callbacks_(std::move(callbacks)),
      options_(std::move(options)),
      sessionId_(options_.sessionId != 0 ? options_.sessionId : CurrentProcessSessionId()),
      corePid_(options_.corePid != 0 ? options_.corePid : GetCurrentProcessId()),
      generation_(options_.generation != 0 ? options_.generation : 1),
      cmdPipeName_(!options_.cmdPipeOverride.empty() ? options_.cmdPipeOverride : DesktopAgentCommandPipeName(sessionId_, corePid_, generation_)),
      evtPipeName_(!options_.evtPipeOverride.empty() ? options_.evtPipeOverride : DesktopAgentEventPipeName(sessionId_, corePid_, generation_)),
      quitEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (!quitEvent_)
        return;
    evtThread_ = std::thread([this] { EvtServerLoop(); });
    if (options_.launchSupervisor)
        supervisorThread_ = std::thread([this] { SupervisorLoop(); });
}

DesktopAgentClient::~DesktopAgentClient() { Shutdown(); }

void DesktopAgentClient::Shutdown()
{
    if (shutdownCalled_.exchange(true))
        return;
    if (agentPid_.load() != 0) {
        AgentCommandMessage quitCmd;
        quitCmd.type = AgentCommandType::Quit;
        SendCommand(quitCmd, nullptr, kQuitCmdTimeoutMs);
    }
    if (quitEvent_)
        SetEvent(quitEvent_);
    {
        std::lock_guard evtLock(activeEvtPipeMutex_);
        if (activeEvtCancelEvent_)
            SetEvent(activeEvtCancelEvent_);
        if (activeEvtPipe_ != INVALID_HANDLE_VALUE)
            CancelIoEx(activeEvtPipe_, nullptr);
    }
    if (supervisorThread_.joinable())
        supervisorThread_.join();
    if (evtThread_.joinable())
        evtThread_.join();
    {
        std::lock_guard lock(cmdPipeMutex_);
        CloseCmdPipeLocked();
    }
    if (quitEvent_) {
        CloseHandle(quitEvent_);
        quitEvent_ = nullptr;
    }
}

bool DesktopAgentClient::Hosting() const
{
    std::lock_guard lock(stateMutex_);
    return hosting_ && activeAgentPid_ != 0 && activeAgentEpoch_ != 0 && desiredHostingSeq_ == confirmedHostingSeq_ &&
           confirmedHostingEpoch_ == activeAgentEpoch_;
}

bool DesktopAgentClient::HostingSynced(bool on) const
{
    std::lock_guard lock(stateMutex_);
    if (on) {
        return hosting_ && activeAgentPid_ != 0 && activeAgentEpoch_ != 0 && desiredHostingSeq_ == confirmedHostingSeq_ &&
               confirmedHostingEpoch_ == activeAgentEpoch_;
    }
    if (hosting_)
        return false;
    if (activeAgentPid_ == 0 || activeAgentEpoch_ == 0)
        return true;
    if (confirmedHostingEpoch_ != activeAgentEpoch_ && attemptedHostingEpoch_ != activeAgentEpoch_)
        return true;
    return confirmedHostingEpoch_ == activeAgentEpoch_ && desiredHostingSeq_ == confirmedHostingSeq_;
}

std::vector<HostIcon> DesktopAgentClient::Icons() const
{
    std::lock_guard lock(stateMutex_);
    std::vector<HostIcon> out;
    out.reserve(icons_.size());
    for (const auto& entry : icons_)
        out.push_back(entry.icon);
    return out;
}

std::vector<std::wstring> DesktopAgentClient::Flashing() const
{
    std::lock_guard lock(stateMutex_);
    std::vector<std::wstring> out;
    for (const auto& entry : icons_) {
        if (entry.flashing && !entry.icon.exe.empty())
            out.push_back(entry.icon.exe);
    }
    return out;
}

std::vector<FlashingTrayIcon> DesktopAgentClient::FlashingIcons() const
{
    std::lock_guard lock(stateMutex_);
    std::vector<FlashingTrayIcon> out;
    for (const auto& entry : icons_) {
        if (entry.flashing && (!entry.icon.exe.empty() || entry.icon.pid != 0))
            out.push_back({entry.icon.key, entry.icon.exe, entry.icon.tip, entry.icon.pid, entry.icon.hwnd});
    }
    return out;
}

void DesktopAgentClient::SetTrayHosting(bool on)
{
    {
        std::lock_guard lock(stateMutex_);
        if (hosting_ == on && activeAgentPid_ != 0 && activeAgentEpoch_ != 0 && desiredHostingSeq_ == confirmedHostingSeq_ &&
            confirmedHostingEpoch_ == activeAgentEpoch_) {
            return;
        }
        hosting_ = on;
        ++desiredHostingSeq_;
        if (!on)
            icons_.clear(), imageCache_.clear(), yielded_ = false, yieldReason_ = TrayHostYieldReason::None;
    }
    if (agentPid_.load() != 0)
        FlushTrayHosting();
    if (!on) {
        if (callbacks_.onTrayChanged)
            callbacks_.onTrayChanged();
        if (callbacks_.onYieldChanged)
            callbacks_.onYieldChanged();
    }
}

HostSendStatus DesktopAgentClient::Send(const std::wstring& key, tray::HostAction action, POINT at, RECT cell, bool* higherIntegrityOut)
{
    if (higherIntegrityOut)
        *higherIntegrityOut = false;
    if (key.empty() || agentPid_.load() == 0)
        return HostSendStatus::NotSent;
    AgentCommandMessage cmd;
    cmd.type = AgentCommandType::TraySend;
    cmd.opId = nextOpId_.fetch_add(1);
    cmd.key = key;
    cmd.action = action;
    cmd.at = at;
    cmd.cell = cell;
    AgentCommandReply reply;
    const SendCommandOutcome outcome = SendCommand(cmd, &reply, kCmdTimeoutMs);
    if (outcome == SendCommandOutcome::RejectedBeforeWrite)
        return HostSendStatus::NotSent;
    if (outcome == SendCommandOutcome::FailedOrTimedOutAfterWrite)
        return HostSendStatus::Unknown;
    if (!reply.ok) {
        if (reply.error == L"tray_host_disabled" || reply.error == L"invalid_command")
            return HostSendStatus::NotSent;
        return HostSendStatus::Unknown;
    }
    if (higherIntegrityOut)
        *higherIntegrityOut = reply.higherIntegrity;
    return reply.sendStatus;
}

void DesktopAgentClient::Hover(const std::wstring& key, POINT at, RECT cell)
{
    if (agentPid_.load() == 0)
        return;
    AgentCommandMessage cmd;
    cmd.type = AgentCommandType::TrayHover;
    cmd.key = key;
    cmd.at = at;
    cmd.cell = cell;
    SendCommand(cmd, nullptr, kFastCmdTimeoutMs);
}

void DesktopAgentClient::SetPlaces(RECT anchor)
{
    {
        std::lock_guard lock(stateMutex_);
        if (hasAnchor_ && EqualRect(&anchor_, &anchor) && activeAgentPid_ != 0 && activeAgentEpoch_ != 0 && desiredAnchorSeq_ == confirmedAnchorSeq_ &&
            confirmedAnchorEpoch_ == activeAgentEpoch_) {
            return;
        }
        hasAnchor_ = true;
        anchor_ = anchor;
        ++desiredAnchorSeq_;
    }
    if (agentPid_.load() != 0)
        FlushPlaces();
}

void DesktopAgentClient::SyncDropBars(const std::vector<AgentBarRect>& bars)
{
    {
        std::lock_guard lock(stateMutex_);
        if (bars == desiredDropBars_ && activeAgentPid_ != 0 && activeAgentEpoch_ != 0 && desiredDropBarsSeq_ == confirmedDropBarsSeq_ &&
            confirmedDropBarsEpoch_ == activeAgentEpoch_) {
            return;
        }
        desiredDropBars_ = bars;
        ++desiredDropBarsSeq_;
    }
    if (agentPid_.load() != 0)
        FlushDropBars();
}

template<class PullFn, class CommitFn> void DesktopAgentClient::FlushDesiredLoop(DWORD timeoutMs, PullFn&& pull, CommitFn&& commit)
{
    std::lock_guard sendLock(desiredStateSendMutex_);
    for (;;) {
        AgentCommandMessage cmd;
        uint64_t seq = 0, epoch = 0;
        {
            std::lock_guard lock(stateMutex_);
            if (agentPid_.load() == 0 || activeAgentPid_ == 0 || activeAgentEpoch_ == 0)
                return;
            epoch = activeAgentEpoch_;
            if (!pull(cmd, seq, epoch))
                return;
        }
        AgentCommandReply reply;
        if (SendCommand(cmd, &reply, timeoutMs, epoch) != SendCommandOutcome::Succeeded || !reply.ok)
            return;
        std::lock_guard lock(stateMutex_);
        if (activeAgentPid_ == 0 || activeAgentEpoch_ != epoch)
            return;
        commit(seq, epoch);
    }
}

void DesktopAgentClient::FlushTrayHosting()
{
    FlushDesiredLoop(
        kCmdTimeoutMs,
        [this](AgentCommandMessage& cmd, uint64_t& seq, uint64_t epoch) {
            if (desiredHostingSeq_ == confirmedHostingSeq_ && confirmedHostingEpoch_ == epoch)
                return false;
            if (!hosting_ && confirmedHostingEpoch_ != epoch && attemptedHostingEpoch_ != epoch)
                return confirmedHostingSeq_ = desiredHostingSeq_, confirmedHostingEpoch_ = epoch, false;
            attemptedHostingEpoch_ = epoch;
            cmd.type = AgentCommandType::SetTrayHosting, cmd.hosting = hosting_, seq = desiredHostingSeq_;
            return true;
        },
        [this](uint64_t seq, uint64_t epoch) {
            if (confirmedHostingEpoch_ != epoch || seq > confirmedHostingSeq_)
                confirmedHostingSeq_ = seq, confirmedHostingEpoch_ = epoch;
        });
}

void DesktopAgentClient::FlushPlaces()
{
    FlushDesiredLoop(
        kFastCmdTimeoutMs,
        [this](AgentCommandMessage& cmd, uint64_t& seq, uint64_t epoch) {
            if (!hasAnchor_ || (desiredAnchorSeq_ == confirmedAnchorSeq_ && confirmedAnchorEpoch_ == epoch))
                return false;
            cmd.type = AgentCommandType::SetTrayPlaces, cmd.anchor = anchor_, seq = desiredAnchorSeq_;
            return true;
        },
        [this](uint64_t seq, uint64_t epoch) {
            if (confirmedAnchorEpoch_ != epoch || seq > confirmedAnchorSeq_)
                confirmedAnchorSeq_ = seq, confirmedAnchorEpoch_ = epoch;
        });
}

void DesktopAgentClient::FlushDropBars()
{
    std::vector<AgentBarRect> sentBars;
    FlushDesiredLoop(
        kFastCmdTimeoutMs,
        [this, &sentBars](AgentCommandMessage& cmd, uint64_t& seq, uint64_t epoch) {
            if (desiredDropBars_.empty() && confirmedDropBarsEpoch_ != epoch && attemptedDropBarsEpoch_ != epoch)
                return confirmedDropBars_.clear(), confirmedDropBarsSeq_ = desiredDropBarsSeq_, confirmedDropBarsEpoch_ = epoch, false;
            if (desiredDropBarsSeq_ == confirmedDropBarsSeq_ && confirmedDropBarsEpoch_ == epoch)
                return false;
            attemptedDropBarsEpoch_ = epoch;
            cmd.type = AgentCommandType::SyncDropBars, cmd.bars = desiredDropBars_, sentBars = cmd.bars, seq = desiredDropBarsSeq_;
            return true;
        },
        [this, &sentBars](uint64_t seq, uint64_t epoch) {
            if (confirmedDropBarsEpoch_ != epoch || seq > confirmedDropBarsSeq_)
                confirmedDropBars_ = std::move(sentBars), confirmedDropBarsSeq_ = seq, confirmedDropBarsEpoch_ = epoch;
        });
}

void DesktopAgentClient::CloseCmdPipeLocked()
{
    if (cmdPipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(cmdPipe_);
        cmdPipe_ = INVALID_HANDLE_VALUE;
    }
    cmdPipeEpoch_ = 0;
}

bool DesktopAgentClient::EnsureCmdPipeConnectedLocked(DWORD timeoutMs, uint64_t requiredEpoch)
{
    DWORD expectedPid = 0;
    FILETIME expectedCreation{};
    uint64_t expectedEpoch = 0;
    {
        std::lock_guard stateLock(stateMutex_);
        expectedPid = activeAgentPid_, expectedCreation = activeAgentCreationTime_, expectedEpoch = activeAgentEpoch_;
    }
    const std::wstring exePath = ModulePath();
    if (expectedPid == 0 || expectedEpoch == 0 || (requiredEpoch != 0 && expectedEpoch != requiredEpoch) || exePath.empty())
        return false;
    HANDLE pipe = ConnectSameUserPipeClient(cmdPipeName_, timeoutMs, quitEvent_);
    if (pipe == INVALID_HANDLE_VALUE)
        return false;
    const bool requireNonElevated = !options_.allowSameProcessPeer && options_.launchSupervisor && IsElevatedProcess();
    AgentPeerIdentity peer;
    if (!VerifyNamedPipePeerProcess(pipe, false, exePath, requireNonElevated, peer) || (!options_.allowSameProcessPeer && peer.pid == GetCurrentProcessId()) ||
        peer.pid != expectedPid || CompareFileTime(&peer.creationTime, &expectedCreation) != 0) {
        Log(L"desktop agent client: rejected reconnected Cmd peer pid=%lu (expected %lu)", peer.pid, expectedPid);
        return CloseHandle(pipe), false;
    }
    std::lock_guard stateLock(stateMutex_);
    if (activeAgentPid_ != expectedPid || activeAgentEpoch_ != expectedEpoch)
        return CloseHandle(pipe), false;
    cmdPipe_ = pipe, cmdPipeEpoch_ = expectedEpoch;
    return true;
}

DesktopAgentClient::SendCommandOutcome DesktopAgentClient::SendCommand(const AgentCommandMessage& cmd, AgentCommandReply* replyOut, DWORD timeoutMs,
                                                                       uint64_t requiredEpoch)
{
    if (replyOut)
        *replyOut = {};
    if (!quitEvent_)
        return SendCommandOutcome::RejectedBeforeWrite;
    const std::string payload = SerializeAgentCommand(cmd);
    if (payload.empty() || payload.size() > kMaxAgentMessageBytes)
        return SendCommandOutcome::RejectedBeforeWrite;
    const int maxAttempts = (cmd.type == AgentCommandType::TraySend) ? 1 : 2;
    bool anyWriteCompleted = false;
    std::lock_guard lock(cmdPipeMutex_);
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (WaitForSingleObject(quitEvent_, 0) != WAIT_TIMEOUT)
            break;
        if (requiredEpoch != 0) {
            std::lock_guard stateLock(stateMutex_);
            if (activeAgentPid_ == 0 || activeAgentEpoch_ != requiredEpoch)
                break;
        }
        if (cmdPipe_ != INVALID_HANDLE_VALUE && requiredEpoch != 0 && cmdPipeEpoch_ != requiredEpoch)
            break;
        if (cmdPipe_ == INVALID_HANDLE_VALUE && !EnsureCmdPipeConnectedLocked(timeoutMs, requiredEpoch))
            break;
        std::string rawReply;
        AgentCommandReply parsed;
        bool writeCompleted = false;
        const bool reqOk = SendPipeRequestOverlapped(cmdPipe_, payload, rawReply, timeoutMs, quitEvent_, &writeCompleted);
        anyWriteCompleted = anyWriteCompleted || writeCompleted;
        if (reqOk && ValidateAndParseAgentCommandReply(rawReply, parsed)) {
            if (replyOut)
                *replyOut = std::move(parsed);
            return SendCommandOutcome::Succeeded;
        }
        CloseCmdPipeLocked();
    }
    return anyWriteCompleted ? SendCommandOutcome::FailedOrTimedOutAfterWrite : SendCommandOutcome::RejectedBeforeWrite;
}

void DesktopAgentClient::EvtServerLoop()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (WaitForSingleObject(quitEvent_, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateSameUserNamedPipeServer(evtPipeName_);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"desktop agent client: CreateSameUserNamedPipeServer failed (%lu)", GetLastError());
            evtReady_ = false;
            break;
        }
        evtReady_ = true;
        ServeEvtPipe(pipe);
        CloseHandle(pipe);
    }
    evtReady_ = false;
    winrt::uninit_apartment();
}

bool DesktopAgentClient::IsEvtEpochValidLocked(uint64_t verifiedEpoch) const
{
    return options_.allowUnboundTestEvtPeer || (activeAgentPid_ != 0 && verifiedEpoch != 0 && activeAgentEpoch_ == verifiedEpoch);
}

bool DesktopAgentClient::VerifyEvtPeer(HANDLE pipe, uint64_t& verifiedEpochOut)
{
    if (options_.allowUnboundTestEvtPeer) {
        if (!VerifyNamedPipeClientSameUser(pipe))
            return Log(L"desktop agent client: rejected Evt client with mismatched user SID"), false;
        return verifiedEpochOut = 0, true;
    }
    DWORD expectedPid = 0;
    FILETIME expectedCreation{};
    {
        std::lock_guard lock(stateMutex_);
        expectedPid = activeAgentPid_, expectedCreation = activeAgentCreationTime_, verifiedEpochOut = activeAgentEpoch_;
    }
    const std::wstring exePath = ModulePath();
    const bool requireNonElevated = !options_.allowSameProcessPeer && options_.launchSupervisor && IsElevatedProcess();
    AgentPeerIdentity peer;
    if (expectedPid == 0 || verifiedEpochOut == 0 || exePath.empty() || !VerifyNamedPipePeerProcess(pipe, true, exePath, requireNonElevated, peer) ||
        (!options_.allowSameProcessPeer && peer.pid == GetCurrentProcessId()) || peer.pid != expectedPid ||
        CompareFileTime(&peer.creationTime, &expectedCreation) != 0) {
        return Log(L"desktop agent client: rejected Evt client pid=%lu (expected %lu)", peer.pid, expectedPid), false;
    }
    std::lock_guard lock(stateMutex_);
    return activeAgentPid_ == expectedPid && activeAgentEpoch_ == verifiedEpochOut;
}

void DesktopAgentClient::ServeEvtPipe(HANDLE pipe)
{
    const winrt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, connCancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event || !connCancel)
        return;
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    if (!ConnectNamedPipeOverlapped(pipe, ov, quitEvent_))
        return;
    {
        std::lock_guard pipeLock(activeEvtPipeMutex_);
        if (shutdownCalled_.load() || (quitEvent_ && WaitForSingleObject(quitEvent_, 0) != WAIT_TIMEOUT))
            return;
        activeEvtPipe_ = pipe, activeEvtCancelEvent_ = connCancel.get();
    }
    bool verified = false;
    uint64_t verifiedEpoch = 0;
    while (WaitForSingleObject(connCancel.get(), 0) == WAIT_TIMEOUT && WaitForSingleObject(quitEvent_, 0) == WAIT_TIMEOUT) {
        std::string raw;
        bool disconnected = false;
        if (!ReadPipeMessageOverlapped(pipe, ov, connCancel.get(), raw, disconnected))
            break;
        if (!verified) {
            if (!VerifyEvtPeer(pipe, verifiedEpoch))
                break;
            verified = true;
        }
        AgentEventMessage evt;
        if (!ValidateAndParseAgentEvent(raw, evt)) {
            if (!WritePipeMessageOverlapped(pipe, ov, connCancel.get(), SerializeAgentEventReply(AgentEventReply{false, false, {}, L"invalid_event"})))
                break;
            continue;
        }
        if (options_.onBeforeHandleEventForTest)
            options_.onBeforeHandleEventForTest();
        bool epochValid = true;
        const AgentEventReply reply = HandleEvent(evt, verifiedEpoch, epochValid);
        if (!epochValid || !WritePipeMessageOverlapped(pipe, ov, connCancel.get(), SerializeAgentEventReply(reply)))
            break;
    }
    {
        std::lock_guard pipeLock(activeEvtPipeMutex_);
        if (activeEvtPipe_ == pipe)
            activeEvtPipe_ = INVALID_HANDLE_VALUE, activeEvtCancelEvent_ = nullptr;
    }
    DisconnectNamedPipe(pipe);
}

bool DesktopAgentClient::ApplyTraySnapshot(const AgentTraySnapshot& snapshot, uint64_t verifiedEpoch, bool& yieldChangedOut)
{
    std::lock_guard lock(stateMutex_);
    if (!IsEvtEpochValidLocked(verifiedEpoch))
        return false;
    yieldChangedOut = (yielded_ != snapshot.yielded) || (yieldReason_ != snapshot.yieldReason);
    yielded_ = snapshot.yielded, yieldReason_ = snapshot.yieldReason;
    std::map<uint64_t, std::shared_ptr<const IconImage>> nextCache;
    std::vector<CachedIcon> nextIcons;
    nextIcons.reserve(snapshot.icons.size());
    for (const auto& dto : snapshot.icons) {
        std::shared_ptr<const IconImage> cached;
        if (dto.imageVersion != 0) {
            if (const auto it = imageCache_.find(dto.imageVersion); it != imageCache_.end())
                cached = it->second;
        }
        HostIcon hostIcon = ConvertDtoToHostIcon(dto, cached);
        if (hostIcon.image && hostIcon.imageVersion != 0)
            nextCache[hostIcon.imageVersion] = hostIcon.image;
        nextIcons.push_back({std::move(hostIcon), dto.flashing});
    }
    std::ranges::sort(nextIcons, [](const CachedIcon& a, const CachedIcon& b) { return a.icon.added < b.icon.added; });
    imageCache_ = std::move(nextCache), icons_ = std::move(nextIcons);
    return true;
}

AgentEventReply DesktopAgentClient::HandleDragOrDropEvent(const AgentEventMessage& evt, uint64_t verifiedEpoch, bool& epochValidOut)
{
    AgentEventReply reply;
    reply.ok = true;
    {
        std::lock_guard lock(stateMutex_);
        if (!IsEvtEpochValidLocked(verifiedEpoch))
            return epochValidOut = false, reply.ok = false, reply;
        if (evt.type == AgentEventType::Drop && evt.opId != 0 && !seenDropOps_.MarkSeenIfNew(evt.opId))
            return reply.allow = false, reply;
    }
    if (evt.type == AgentEventType::DragOver && (callbacks_.onDragOverEpoch || callbacks_.onDragOver)) {
        const AgentDragOverResult res = callbacks_.onDragOverEpoch ? callbacks_.onDragOverEpoch(evt.pt, verifiedEpoch) : callbacks_.onDragOver(evt.pt);
        reply.allow = res.allow, reply.key = res.key;
    } else if (evt.type == AgentEventType::DragLeave) {
        if (callbacks_.onDragLeaveEpoch)
            callbacks_.onDragLeaveEpoch(verifiedEpoch);
        else if (callbacks_.onDragLeave)
            callbacks_.onDragLeave();
    } else if (evt.type == AgentEventType::Drop) {
        reply.allow = callbacks_.onDropFilesAcceptedEpoch ? callbacks_.onDropFilesAcceptedEpoch(evt.key, evt.paths, verifiedEpoch) :
                      callbacks_.onDropFilesAccepted      ? callbacks_.onDropFilesAccepted(evt.key, evt.paths) :
                      callbacks_.onDropFiles              ? (callbacks_.onDropFiles(evt.key, evt.paths), true) :
                                                            false;
    }
    return reply;
}

AgentEventReply DesktopAgentClient::HandleEvent(const AgentEventMessage& evt, uint64_t verifiedEpoch, bool& epochValidOut)
{
    epochValidOut = true;
    AgentEventReply reply;
    reply.ok = true;
    if (evt.type == AgentEventType::TraySnapshot) {
        bool yieldChanged = false;
        if (!ApplyTraySnapshot(evt.snapshot, verifiedEpoch, yieldChanged))
            return epochValidOut = false, reply.ok = false, reply;
        if (callbacks_.onTrayChanged)
            callbacks_.onTrayChanged();
        if (yieldChanged && callbacks_.onYieldChanged)
            callbacks_.onYieldChanged();
        return reply;
    }
    if (evt.type != AgentEventType::TrayYield)
        return HandleDragOrDropEvent(evt, verifiedEpoch, epochValidOut);
    bool yieldChanged = false;
    {
        std::lock_guard lock(stateMutex_);
        if (!IsEvtEpochValidLocked(verifiedEpoch))
            return epochValidOut = false, reply.ok = false, reply;
        yieldChanged = (yielded_ != evt.yielded) || (yieldReason_ != evt.yieldReason);
        yielded_ = evt.yielded, yieldReason_ = evt.yieldReason;
    }
    if (yieldChanged && callbacks_.onYieldChanged)
        callbacks_.onYieldChanged();
    return reply;
}

int DesktopAgentClient::TryAttachCmdPeer(HANDLE pipe, const std::wstring& exePath, const std::wstring& spawnNonce, const FILETIME& spawnFloor,
                                         HANDLE launchedHandle, DWORD launchedPid, const FILETIME& launchedCreation, bool requireNonElevated,
                                         winrt::handle& procOut, DWORD& pidOut)
{
    AgentPeerIdentity peer;
    winrt::handle verifiedProc;
    if (!VerifyNamedPipePeerProcess(pipe, false, exePath, requireNonElevated, peer, &verifiedProc) ||
        (!options_.allowSameProcessPeer && peer.pid == GetCurrentProcessId()) ||
        (!launchedHandle && !options_.allowSameProcessPeer && CompareFileTime(&peer.creationTime, &spawnFloor) < 0) ||
        (launchedHandle &&
         (!VerifyLaunchedProcessMatchesPeer(launchedHandle, peer) || peer.pid != launchedPid || CompareFileTime(&peer.creationTime, &launchedCreation) != 0))) {
        Log(L"desktop agent client: rejected Cmd pipe peer pid=%lu", peer.pid);
        return CloseHandle(pipe), -1;
    }
    AgentCommandMessage ping;
    ping.type = AgentCommandType::Ping;
    std::string rawPingReply;
    AgentCommandReply reply;
    const bool pingOk =
        SendPipeRequestOverlapped(pipe, SerializeAgentCommand(ping), rawPingReply, 200, quitEvent_) && ValidateAndParseAgentCommandReply(rawPingReply, reply);
    const bool nonceOk =
        options_.allowSameProcessPeer ? (reply.nonce.empty() || reply.nonce == spawnNonce) : (!reply.nonce.empty() && reply.nonce == spawnNonce);
    if (pingOk && reply.ok && reply.pid == peer.pid && (generation_ == 0 || reply.generation == generation_) && nonceOk) {
        {
            std::lock_guard pipeLock(cmdPipeMutex_);
            CloseCmdPipeLocked();
            std::lock_guard lock(stateMutex_);
            activeAgentPid_ = peer.pid, activeAgentCreationTime_ = peer.creationTime, ++activeAgentEpoch_;
            cmdPipe_ = pipe, cmdPipeEpoch_ = activeAgentEpoch_;
        }
        pidOut = peer.pid, procOut = std::move(verifiedProc);
        return 1;
    }
    CloseHandle(pipe);
    if (pingOk) {
        Log(L"desktop agent client: rejected Cmd pipe peer pid=%lu due to Ping identity/nonce mismatch", peer.pid);
        return -1;
    }
    return 0;
}

bool DesktopAgentClient::SpawnAndConnect(winrt::handle& procOut, DWORD& pidOut)
{
    procOut = {}, pidOut = 0;
    const std::wstring exePath = ModulePath();
    if (exePath.empty())
        return false;
    const std::wstring spawnNonce = GenerateAgentSpawnNonce();
    const std::wstring params = L"--desktop-agent " + std::to_wstring(corePid_) + L" --agent-gen " + std::to_wstring(generation_) + DataDirectoryArgs() +
                                L" --agent-nonce " + spawnNonce;
    const FILETIME spawnFloor = ComputeSpawnFloor();
    winrt::handle launchedHandle;
    DWORD launchedPid = 0;
    FILETIME launchedCreation{};
    if (!LaunchAgentProcessHandle(options_, exePath, params, launchedHandle, launchedPid, launchedCreation))
        return false;

    const bool requireNonElevated = !options_.allowSameProcessPeer && options_.launchSupervisor && IsElevatedProcess();
    const ULONGLONG deadline = GetTickCount64() + options_.pingWaitMs;
    while (WaitForSingleObject(quitEvent_, 0) == WAIT_TIMEOUT) {
        if (launchedHandle && WaitForSingleObject(launchedHandle.get(), 0) != WAIT_TIMEOUT)
            return false;
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline ? 1 : static_cast<DWORD>((std::min)(ULONGLONG(200), deadline - now));
        HANDLE pipe = ConnectSameUserPipeClient(cmdPipeName_, remaining, quitEvent_);
        if (pipe != INVALID_HANDLE_VALUE) {
            const int status = TryAttachCmdPeer(pipe, exePath, spawnNonce, spawnFloor, launchedHandle.get(), launchedPid, launchedCreation, requireNonElevated,
                                                procOut, pidOut);
            if (status != 0)
                return status > 0;
        }
        const ULONGLONG after = GetTickCount64();
        if (after >= deadline)
            break;
        const DWORD step = (std::min)(DWORD(50), static_cast<DWORD>(deadline - after));
        if (WaitForSingleObject(quitEvent_, step) != WAIT_TIMEOUT)
            break;
    }
    return false;
}

void DesktopAgentClient::ReplayDesiredState()
{
    FlushTrayHosting();
    FlushPlaces();
    FlushDropBars();
}

void DesktopAgentClient::ClearAgentStateOnExit()
{
    {
        std::lock_guard lock(cmdPipeMutex_);
        CloseCmdPipeLocked();
    }
    bool hadHostedState = false;
    {
        std::lock_guard lock(stateMutex_);
        activeAgentPid_ = 0, activeAgentCreationTime_ = {}, ++activeAgentEpoch_;
        confirmedHostingSeq_ = 0, confirmedHostingEpoch_ = 0, attemptedHostingEpoch_ = 0;
        confirmedAnchorSeq_ = 0, confirmedAnchorEpoch_ = 0;
        confirmedDropBars_.clear(), confirmedDropBarsSeq_ = 0, confirmedDropBarsEpoch_ = 0, attemptedDropBarsEpoch_ = 0;
        seenDropOps_.Clear();
        hadHostedState = !icons_.empty() || yielded_;
        icons_.clear(), imageCache_.clear(), yielded_ = false, yieldReason_ = TrayHostYieldReason::None;
        {
            std::lock_guard evtLock(activeEvtPipeMutex_);
            if (activeEvtCancelEvent_)
                SetEvent(activeEvtCancelEvent_);
            if (activeEvtPipe_ != INVALID_HANDLE_VALUE)
                CancelIoEx(activeEvtPipe_, nullptr);
        }
    }
    {
        std::unique_lock drainLock(epochActionMutex_);
    }
    agentPid_.store(0);
    if (callbacks_.onAgentPidChanged)
        callbacks_.onAgentPidChanged(0);
    if (hadHostedState) {
        if (callbacks_.onTrayChanged)
            callbacks_.onTrayChanged();
        if (callbacks_.onYieldChanged)
            callbacks_.onYieldChanged();
    }
}

void DesktopAgentClient::SupervisorLoop()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    DesktopAgentSupervisorPolicy policy;
    while (WaitForSingleObject(quitEvent_, 0) == WAIT_TIMEOUT) {
        winrt::handle agentProc;
        DWORD pid = 0;
        if (SpawnAndConnect(agentProc, pid)) {
            agentPid_.store(pid);
            if (callbacks_.onAgentPidChanged)
                callbacks_.onAgentPidChanged(pid);
            ReplayDesiredState();

            HANDLE waitHandles[2] = {quitEvent_, agentProc.get()};
            for (;;) {
                const DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, kMonitorIntervalMs);
                if (wait == WAIT_OBJECT_0) {
                    winrt::uninit_apartment();
                    return;
                }
                if (wait == WAIT_OBJECT_0 + 1)
                    break;
                if (wait == WAIT_TIMEOUT)
                    policy.RecordHealthy(GetTickCount64());
                else
                    break;
            }
            ClearAgentStateOnExit();
        }
        if (WaitForSingleObject(quitEvent_, 0) != WAIT_TIMEOUT)
            break;
        if (!policy.RecordFailure(GetTickCount64())) {
            degraded_.store(true);
            Log(L"desktop agent client: supervisor exhausted restart budget (%d attempts); running in degraded mode",
                DesktopAgentSupervisorPolicy::kMaxRestartAttempts);
            break;
        }
        const DWORD delay = static_cast<DWORD>(policy.NextDelayMs());
        if (WaitForSingleObject(quitEvent_, delay) != WAIT_TIMEOUT)
            break;
        policy.MarkRestartDispatched();
    }
    winrt::uninit_apartment();
}

} // namespace atrium
