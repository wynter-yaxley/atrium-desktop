#include "shell/desktop_agent_client.h"

#include "log.h"
#include "settings.h"
#include "shell/shell_launch.h"

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

std::wstring SelfExecutablePath()
{
    wchar_t path[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path))
        return {};
    return std::wstring(path, len);
}

HostIcon ConvertDtoToHostIcon(const AgentTrayIconDto& dto, const std::shared_ptr<const IconImage>& cachedImage)
{
    HostIcon icon;
    icon.key = dto.key;
    icon.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(dto.hwnd));
    icon.uid = dto.uid;
    icon.callback = dto.callback;
    icon.version = dto.version;
    icon.showTip = dto.showTip;
    icon.tip = dto.tip;
    icon.exe = dto.exe;
    icon.pid = dto.pid;
    icon.imageVersion = dto.imageVersion;
    icon.hidden = dto.hidden;
    icon.added = dto.added;
    if (dto.width > 0 && dto.height > 0 && !dto.pixels.empty())
        icon.image = std::make_shared<const IconImage>(dto.width, dto.height, dto.pixels);
    else if (dto.imageVersion != 0)
        icon.image = cachedImage;
    return icon;
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
    return hosting_;
}

bool DesktopAgentClient::Yielded() const
{
    std::lock_guard lock(stateMutex_);
    return yielded_;
}

TrayHostYieldReason DesktopAgentClient::YieldReason() const
{
    std::lock_guard lock(stateMutex_);
    return yieldReason_;
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
        if (hosting_ == on)
            return;
        hosting_ = on;
        if (!on) {
            icons_.clear();
            imageCache_.clear();
            yielded_ = false;
            yieldReason_ = TrayHostYieldReason::None;
        }
    }
    if (agentPid_.load() != 0) {
        AgentCommandMessage cmd;
        cmd.type = AgentCommandType::SetTrayHosting;
        cmd.hosting = on;
        SendCommand(cmd, nullptr, kCmdTimeoutMs);
    }
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
    cmd.key = key;
    cmd.action = action;
    cmd.at = at;
    cmd.cell = cell;
    AgentCommandReply reply;
    if (!SendCommand(cmd, &reply, kCmdTimeoutMs) || !reply.ok)
        return HostSendStatus::NotSent;
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
        hasAnchor_ = true;
        anchor_ = anchor;
    }
    if (agentPid_.load() == 0)
        return;
    AgentCommandMessage cmd;
    cmd.type = AgentCommandType::SetTrayPlaces;
    cmd.anchor = anchor;
    SendCommand(cmd, nullptr, kFastCmdTimeoutMs);
}

void DesktopAgentClient::SyncDropBars(const std::vector<AgentBarRect>& bars)
{
    {
        std::lock_guard lock(stateMutex_);
        if (bars == dropBars_)
            return;
        dropBars_ = bars;
    }
    if (agentPid_.load() == 0)
        return;
    AgentCommandMessage cmd;
    cmd.type = AgentCommandType::SyncDropBars;
    cmd.bars = bars;
    SendCommand(cmd, nullptr, kFastCmdTimeoutMs);
}

void DesktopAgentClient::CloseCmdPipeLocked()
{
    if (cmdPipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(cmdPipe_);
        cmdPipe_ = INVALID_HANDLE_VALUE;
    }
}

bool DesktopAgentClient::SendCommand(const AgentCommandMessage& cmd, AgentCommandReply* replyOut, DWORD timeoutMs)
{
    if (replyOut)
        *replyOut = {};
    if (!quitEvent_)
        return false;
    const std::string payload = SerializeAgentCommand(cmd);
    if (payload.empty() || payload.size() > kMaxAgentMessageBytes)
        return false;
    std::lock_guard lock(cmdPipeMutex_);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (WaitForSingleObject(quitEvent_, 0) != WAIT_TIMEOUT)
            return false;
        if (cmdPipe_ == INVALID_HANDLE_VALUE) {
            cmdPipe_ = ConnectSameUserPipeClient(cmdPipeName_, timeoutMs, quitEvent_);
            if (cmdPipe_ == INVALID_HANDLE_VALUE)
                return false;
        }
        std::string rawReply;
        AgentCommandReply parsed;
        if (SendPipeRequestOverlapped(cmdPipe_, payload, rawReply, timeoutMs, quitEvent_) && ValidateAndParseAgentCommandReply(rawReply, parsed)) {
            if (replyOut)
                *replyOut = std::move(parsed);
            return true;
        }
        CloseCmdPipeLocked();
    }
    return false;
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

void DesktopAgentClient::ServeEvtPipe(HANDLE pipe)
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event)
        return;
    OVERLAPPED ov{};
    ov.hEvent = event;
    if (!ConnectNamedPipeOverlapped(pipe, ov, quitEvent_)) {
        CloseHandle(event);
        return;
    }
    bool verified = false;
    while (WaitForSingleObject(quitEvent_, 0) == WAIT_TIMEOUT) {
        std::string raw;
        bool disconnected = false;
        if (!ReadPipeMessageOverlapped(pipe, ov, quitEvent_, raw, disconnected))
            break;
        if (!verified) {
            if (!VerifyNamedPipeClientSameUser(pipe)) {
                Log(L"desktop agent client: rejected Evt client with mismatched user SID");
                break;
            }
            verified = true;
        }
        AgentEventMessage evt;
        AgentEventReply reply;
        if (!ValidateAndParseAgentEvent(raw, evt)) {
            reply.ok = false;
            reply.error = L"invalid_event";
        } else {
            reply = HandleEvent(evt);
        }
        const std::string encoded = SerializeAgentEventReply(reply);
        if (!WritePipeMessageOverlapped(pipe, ov, quitEvent_, encoded))
            break;
        FlushFileBuffers(pipe);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(event);
}

AgentEventReply DesktopAgentClient::HandleEvent(const AgentEventMessage& evt)
{
    AgentEventReply reply;
    reply.ok = true;
    switch (evt.type) {
    case AgentEventType::TraySnapshot: {
        bool yieldChanged = false;
        {
            std::lock_guard lock(stateMutex_);
            yieldChanged = (yielded_ != evt.snapshot.yielded) || (yieldReason_ != evt.snapshot.yieldReason);
            yielded_ = evt.snapshot.yielded;
            yieldReason_ = evt.snapshot.yieldReason;

            std::map<uint64_t, std::shared_ptr<const IconImage>> nextCache;
            std::vector<CachedIcon> nextIcons;
            nextIcons.reserve(evt.snapshot.icons.size());
            for (const auto& dto : evt.snapshot.icons) {
                std::shared_ptr<const IconImage> cached;
                if (dto.imageVersion != 0) {
                    const auto it = imageCache_.find(dto.imageVersion);
                    if (it != imageCache_.end())
                        cached = it->second;
                }
                HostIcon hostIcon = ConvertDtoToHostIcon(dto, cached);
                if (hostIcon.image && hostIcon.imageVersion != 0)
                    nextCache[hostIcon.imageVersion] = hostIcon.image;
                nextIcons.push_back({std::move(hostIcon), dto.flashing});
            }
            std::ranges::sort(nextIcons, [](const CachedIcon& a, const CachedIcon& b) { return a.icon.added < b.icon.added; });
            imageCache_ = std::move(nextCache);
            icons_ = std::move(nextIcons);
        }
        if (callbacks_.onTrayChanged)
            callbacks_.onTrayChanged();
        if (yieldChanged && callbacks_.onYieldChanged)
            callbacks_.onYieldChanged();
        return reply;
    }
    case AgentEventType::TrayYield: {
        bool yieldChanged = false;
        {
            std::lock_guard lock(stateMutex_);
            yieldChanged = (yielded_ != evt.yielded) || (yieldReason_ != evt.yieldReason);
            yielded_ = evt.yielded;
            yieldReason_ = evt.yieldReason;
        }
        if (yieldChanged && callbacks_.onYieldChanged)
            callbacks_.onYieldChanged();
        return reply;
    }
    case AgentEventType::DragOver: {
        if (callbacks_.onDragOver) {
            const AgentDragOverResult res = callbacks_.onDragOver(evt.pt);
            reply.allow = res.allow;
            reply.key = res.key;
        }
        return reply;
    }
    case AgentEventType::DragLeave:
        if (callbacks_.onDragLeave)
            callbacks_.onDragLeave();
        return reply;
    case AgentEventType::Drop:
        if (callbacks_.onDropFiles)
            callbacks_.onDropFiles(evt.key, evt.paths);
        return reply;
    }
    return reply;
}

bool DesktopAgentClient::SpawnAndConnect(HANDLE& procOut, DWORD& pidOut)
{
    procOut = nullptr;
    pidOut = 0;

    const std::wstring params = L"--desktop-agent " + std::to_wstring(corePid_) + L" --agent-gen " + std::to_wstring(generation_) + DataDirectoryArgs();
    HANDLE launchedHandle = nullptr;
    if (options_.spawnAgentOverride) {
        DWORD err = 0;
        launchedHandle = options_.spawnAgentOverride(params, err);
        if (!launchedHandle && err != 0)
            return false;
    } else {
        const std::wstring exe = SelfExecutablePath();
        if (exe.empty())
            return false;
        LaunchRequest req;
        req.target = exe;
        req.parameters = params;
        req.kind = LaunchTargetKind::Executable;
        req.privilege = LaunchPrivilege::RequireMedium;
        req.show = SW_HIDE;
        req.captureProcess = true;
        const LaunchResult result = ShellLaunch(req, kDefaultShellLaunchTimeoutMs);
        if (result.outcome == LaunchOutcome::Failed) {
            Log(L"desktop agent client: ShellLaunch RequireMedium failed (%lu)", result.error);
            return false;
        }
        launchedHandle = result.process;
    }

    const ULONGLONG deadline = GetTickCount64() + options_.pingWaitMs;
    while (WaitForSingleObject(quitEvent_, 0) == WAIT_TIMEOUT) {
        if (launchedHandle && WaitForSingleObject(launchedHandle, 0) != WAIT_TIMEOUT) {
            CloseHandle(launchedHandle);
            return false;
        }
        AgentCommandMessage ping;
        ping.type = AgentCommandType::Ping;
        AgentCommandReply reply;
        if (SendCommand(ping, &reply, 200) && reply.ok && reply.pid != 0 && (generation_ == 0 || reply.generation == generation_)) {
            pidOut = reply.pid;
            if (launchedHandle) {
                procOut = launchedHandle;
            } else {
                procOut = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, reply.pid);
                if (!procOut)
                    return false;
            }
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        const DWORD step = (std::min)(DWORD(50), static_cast<DWORD>(deadline - now));
        if (WaitForSingleObject(quitEvent_, step) != WAIT_TIMEOUT)
            break;
    }
    if (launchedHandle)
        CloseHandle(launchedHandle);
    return false;
}

void DesktopAgentClient::ReplayDesiredState()
{
    bool hosting = false;
    bool hasAnchor = false;
    RECT anchor{};
    std::vector<AgentBarRect> bars;
    {
        std::lock_guard lock(stateMutex_);
        hosting = hosting_;
        hasAnchor = hasAnchor_;
        anchor = anchor_;
        bars = dropBars_;
    }
    if (hosting) {
        AgentCommandMessage cmd;
        cmd.type = AgentCommandType::SetTrayHosting;
        cmd.hosting = true;
        SendCommand(cmd, nullptr, kCmdTimeoutMs);
    }
    if (hasAnchor) {
        AgentCommandMessage cmd;
        cmd.type = AgentCommandType::SetTrayPlaces;
        cmd.anchor = anchor;
        SendCommand(cmd, nullptr, kFastCmdTimeoutMs);
    }
    if (!bars.empty()) {
        AgentCommandMessage cmd;
        cmd.type = AgentCommandType::SyncDropBars;
        cmd.bars = std::move(bars);
        SendCommand(cmd, nullptr, kFastCmdTimeoutMs);
    }
}

void DesktopAgentClient::ClearAgentStateOnExit()
{
    agentPid_.store(0);
    {
        std::lock_guard lock(cmdPipeMutex_);
        CloseCmdPipeLocked();
    }
    bool hadHostedState = false;
    {
        std::lock_guard lock(stateMutex_);
        hadHostedState = !icons_.empty() || yielded_;
        icons_.clear();
        imageCache_.clear();
        yielded_ = false;
        yieldReason_ = TrayHostYieldReason::None;
    }
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
        HANDLE agentProc = nullptr;
        DWORD pid = 0;
        if (SpawnAndConnect(agentProc, pid)) {
            agentPid_.store(pid);
            if (callbacks_.onAgentPidChanged)
                callbacks_.onAgentPidChanged(pid);
            ReplayDesiredState();

            HANDLE waitHandles[2] = {quitEvent_, agentProc};
            for (;;) {
                const DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, kMonitorIntervalMs);
                if (wait == WAIT_OBJECT_0) {
                    CloseHandle(agentProc);
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
            CloseHandle(agentProc);
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
