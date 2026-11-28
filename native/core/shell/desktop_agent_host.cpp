#include "shell/desktop_agent_host.h"

#include "log.h"
#include "settings.h"
#include "shell/desktop_agent_drop.h"
#include "shell/desktop_agent_ipc.h"
#include "shell/process_integrity.h"
#include "shell/tray_host.h"
#include "win32_path.h"

#include <ole2.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace atrium {

namespace {

constexpr wchar_t kHostWindowClass[] = L"AtriumDesktopAgentHost";
constexpr wchar_t kOverlayWindowClass[] = L"AtriumDesktopAgentDropOverlay";
constexpr UINT kMsgTrayNotify = WM_APP + 1;
constexpr UINT kMsgTraySnapshot = WM_APP + 2;
constexpr UINT kMsgTrayYield = WM_APP + 3;
constexpr UINT kMsgRunTasks = WM_APP + 4;
constexpr UINT_PTR kDropPollTimerId = 1;
constexpr UINT kDropPollMs = 25;
constexpr UINT_PTR kWatchdogTimerId = 2;
constexpr UINT kWatchdogMs = 1000;
constexpr DWORD kDragOverTimeoutMs = 300;
constexpr DWORD kDropEventTimeoutMs = 500;
constexpr DWORD kTrayEventTimeoutMs = 1500;
constexpr size_t kSnapshotRawPixelBudget = 1300 * 1024;

uint64_t ReadGenerationFile()
{
    const std::wstring dir = DataDirectory();
    const std::wstring text = dir.empty() ? std::wstring{} : TrimAsciiWhitespace(ReadTextFile(dir + L"\\instance.gen"));
    if (text.empty() || !std::ranges::all_of(text, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
        return 0;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long val = std::wcstoull(text.c_str(), &end, 10);
    return (errno == 0 && end != text.c_str() && *end == L'\0') ? static_cast<uint64_t>(val) : 0;
}

struct DropOverlay {
    uint64_t id = 0;
    RECT rect{};
    HWND hwnd = nullptr;
    winrt::com_ptr<AgentDropTarget> target;
    bool transparent = true;
};

struct StaTaskState {
    enum class Status { Queued, Running, Completed, Cancelled };

    std::mutex mu;
    Status status = Status::Queued;
    AgentCommandMessage cmd;
    AgentCommandReply reply;
    winrt::handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

class DesktopAgentHostApp final : public IAgentDropHost {
public:
    DesktopAgentHostApp(DWORD corePid, uint64_t generation, HINSTANCE instance, DesktopAgentHostOptions options)
        : corePid_(corePid),
          generation_(generation),
          instance_(instance),
          options_(std::move(options)),
          quitEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          remainingFailSnapshotPostsForTest_(options_.failSnapshotPostCountForTest)
    {
    }
    ~DesktopAgentHostApp() override { Cleanup(); }
    DesktopAgentHostApp(const DesktopAgentHostApp&) = delete;
    DesktopAgentHostApp& operator=(const DesktopAgentHostApp&) = delete;

    int Run();
    AgentDragOverResult QueryDragOver(POINT pt) override
    {
        AgentEventMessage msg;
        msg.type = AgentEventType::DragOver, msg.pt = pt;
        AgentEventReply reply;
        return SendEvent(msg, reply, kDragOverTimeoutMs) ? AgentDragOverResult{reply.allow, std::move(reply.key)} : AgentDragOverResult{};
    }
    void NotifyDragLeave(uint64_t barId) override
    {
        SetOverlayTransparentById(barId, true);
        AgentEventMessage msg;
        msg.type = AgentEventType::DragLeave;
        AgentEventReply reply;
        SendEvent(msg, reply, kDropEventTimeoutMs);
    }
    bool NotifyDropFiles(uint64_t barId, const std::wstring& key, const std::vector<std::wstring>& paths) override
    {
        SetOverlayTransparentById(barId, true);
        if (!IsValidAgentDropPayload(key, paths))
            return false;
        AgentEventMessage msg;
        msg.type = AgentEventType::Drop, msg.opId = nextOpId_.fetch_add(1), msg.key = key, msg.paths = paths;
        AgentEventReply reply;
        return SendEvent(msg, reply, kDropEventTimeoutMs) && reply.ok && reply.allow;
    }

private:
    bool IsCorePeer(HANDLE pipe, bool isServerSide, const wchar_t* channel) const
    {
        AgentPeerIdentity peer;
        if (!VerifyNamedPipePeerProcess(pipe, isServerSide, expectedImagePath_, false, peer) || peer.pid != corePid_ ||
            CompareFileTime(&peer.creationTime, &coreCreationTime_) != 0) {
            Log(L"desktop agent: rejected %s peer pid=%lu (expected core=%lu)", channel, peer.pid, corePid_);
            return false;
        }
        return true;
    }
    bool VerifyCoreProcessIdentity()
    {
        expectedImagePath_ = ModulePath();
        coreProc_.attach(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, corePid_));
        FILETIME ex{}, k{}, u{};
        if (expectedImagePath_.empty() || !coreProc_ || !GetProcessTimes(coreProc_.get(), &coreCreationTime_, &ex, &k, &u))
            return false;
        const std::wstring coreExe = ProcessImagePath(coreProc_.get());
        const auto& self = SelfSecurityContext();
        if (coreExe.empty() || FoldCase(coreExe) != FoldCase(expectedImagePath_) || !self.valid)
            return false;
        DWORD secErr = 0, coreSession = 0;
        const ProcessSecurityContext coreSec = QueryProcessSecurityContext(coreProc_.get(), &secErr);
        if (coreSec.valid)
            return _wcsicmp(coreSec.userSid.c_str(), self.userSid.c_str()) == 0 && coreSec.sessionId == self.sessionId;
        return secErr == ERROR_ACCESS_DENIED && ProcessIdToSessionId(corePid_, &coreSession) && coreSession == self.sessionId;
    }
    bool InitWindows()
    {
        WNDCLASSEXW hc{sizeof(hc)}, oc{sizeof(oc)};
        hc.lpfnWndProc = HostWndProc, hc.hInstance = instance_, hc.lpszClassName = kHostWindowClass, RegisterClassExW(&hc);
        oc.lpfnWndProc = OverlayWndProc, oc.hInstance = instance_, oc.lpszClassName = kOverlayWindowClass, RegisterClassExW(&oc);
        hostHwnd_ = CreateWindowExW(0, kHostWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, this);
        return hostHwnd_ ? (SetTimer(hostHwnd_, kWatchdogTimerId, kWatchdogMs, nullptr), true) :
                           (Log(L"desktop agent: CreateWindowExW host failed (%lu)", GetLastError()), false);
    }
    void Cleanup()
    {
        quitRequested_ = true, evtPushCv_.notify_all();
        if (quitEvent_)
            SetEvent(quitEvent_.get());
        if (cmdThread_.joinable())
            cmdThread_.join();
        if (evtPushThread_.joinable())
            evtPushThread_.join();
        DestroyAllOverlays(), trayHost_.reset();
        if (hostHwnd_)
            KillTimer(hostHwnd_, kWatchdogTimerId), DestroyWindow(hostHwnd_), hostHwnd_ = nullptr;
        std::lock_guard lock(evtPipeMutex_);
        CloseEvtPipeLocked();
    }
    void CmdServerLoop();
    void ServeCmdPipe(HANDLE pipe);
    AgentCommandReply HandleCommand(const AgentCommandMessage& cmd)
    {
        AgentCommandReply reply;
        if (cmd.type == AgentCommandType::Ping)
            return reply.ok = true, reply.pid = GetCurrentProcessId(), reply.generation = generation_, reply.nonce = options_.nonce, reply;
        if (cmd.type == AgentCommandType::Quit)
            return RequestQuit(), reply.ok = true, reply;
        return PostStaCommand(cmd);
    }
    AgentCommandReply PostStaCommand(const AgentCommandMessage& cmd);
    AgentCommandReply ExecuteStaCommand(const AgentCommandMessage& cmd);
    void DrainStaTasks();
    void EvtPushLoop();
    void HandleIdleEvtReconnect(DWORD& resyncBackoffMs);
    void RecordSentSnapshotAck(uint64_t sessionUsed, const AgentEventMessage& msg, bool deferred);
    void QueueEvtPush(AgentEventMessage msg, bool isSnapshot, bool deferred = false)
    {
        {
            std::lock_guard lock(evtPushMutex_);
            if (isSnapshot)
                queuedSnapshot_ = std::move(msg), pendingSnapshot_ = true, pendingSnapshotDeferred_ = deferred;
            else
                queuedYield_ = std::move(msg), pendingYield_ = true;
        }
        evtPushCv_.notify_one();
    }
    bool RequestTraySnapshotResync()
    {
        for (int n = remainingFailSnapshotPostsForTest_.load(); n > 0;) {
            if (remainingFailSnapshotPostsForTest_.compare_exchange_weak(n, n - 1))
                return false;
        }
        return hostHwnd_ != nullptr && PostMessageW(hostHwnd_, kMsgTraySnapshot, 0, 0) != FALSE;
    }
    bool EnsureEvtConnectedLocked(DWORD timeoutMs)
    {
        if (evtPipe_ != INVALID_HANDLE_VALUE)
            return true;
        HANDLE pipe = ConnectSameUserPipeClient(options_.evtPipeOverride, timeoutMs, quitEvent_.get());
        if (pipe == INVALID_HANDLE_VALUE)
            return false;
        if (!IsCorePeer(pipe, false, L"Evt"))
            return CloseHandle(pipe), false;
        evtPipe_ = pipe, evtSession_.fetch_add(1);
        bool needResync = false;
        {
            std::lock_guard lock(evtPushMutex_);
            needResync = resyncNeeded_;
        }
        if (needResync)
            (void)RequestTraySnapshotResync();
        return true;
    }
    bool SendEvent(const AgentEventMessage& msg, AgentEventReply& replyOut, DWORD timeoutMs, uint64_t* sessionOut = nullptr);
    void CloseEvtPipeLocked() { evtPipe_ != INVALID_HANDLE_VALUE ? (CloseHandle(evtPipe_), evtPipe_ = INVALID_HANDLE_VALUE) : INVALID_HANDLE_VALUE; }
    bool SetTrayHosting(bool on);
    void ApplyAnchorIfHosted()
    {
        if (hasTrayAnchor_ && trayHost_) {
            trayHost_->SetPlaces(trayAnchor_);
            if (options_.onAnchorAppliedForTest)
                options_.onAnchorAppliedForTest(trayAnchor_);
        }
    }
    void PushTraySnapshot();
    void PushTrayYield()
    {
        AgentEventMessage msg{AgentEventType::TrayYield};
        if (trayHost_)
            msg.yielded = trayHost_->Yielded(), msg.yieldReason = trayHost_->YieldReason();
        QueueEvtPush(std::move(msg), false);
    }
    bool SyncDropBars(const std::vector<AgentBarRect>& bars);
    void DestroyAllOverlays()
    {
        if (dropTimerActive_ && hostHwnd_)
            KillTimer(hostHwnd_, kDropPollTimerId), dropTimerActive_ = false;
        for (auto& [id, ov] : overlays_)
            DestroyOverlay(ov);
        overlays_.clear();
    }
    void SetOverlayTransparent(DropOverlay& ov, bool transparent)
    {
        if (!ov.hwnd || ov.transparent == transparent)
            return;
        const LONG_PTR ex = GetWindowLongPtrW(ov.hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(ov.hwnd, GWL_EXSTYLE, transparent ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT));
        SetWindowPos(ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ov.transparent = transparent;
    }
    void SetOverlayTransparentById(uint64_t barId, bool transparent)
    {
        if (const auto it = overlays_.find(barId); it != overlays_.end())
            SetOverlayTransparent(it->second, transparent);
    }
    void DestroyOverlay(DropOverlay& ov)
    {
        if (ov.hwnd)
            RevokeDragDrop(ov.hwnd), DestroyWindow(ov.hwnd), ov.hwnd = nullptr;
    }
    void RequestQuit()
    {
        quitRequested_ = true;
        if (quitEvent_)
            SetEvent(quitEvent_.get());
        if (hostHwnd_)
            PostMessageW(hostHwnd_, WM_CLOSE, 0, 0);
    }
    void OnDropPollTimer();
    void OnWatchdogTimer()
    {
        if (generation_ == 0)
            return;
        if (const uint64_t active = ReadGenerationFile(); active != 0 && active > generation_) {
            Log(L"desktop agent: generation rotated (%llu -> %llu); exiting", static_cast<unsigned long long>(generation_),
                static_cast<unsigned long long>(active));
            RequestQuit();
        }
    }

    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<const CREATESTRUCTW*>(lp)->lpCreateParams));
            return TRUE;
        }
        auto* self = reinterpret_cast<DesktopAgentHostApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == kMsgRunTasks)
            return self->DrainStaTasks(), 0;
        if (msg == kMsgTraySnapshot)
            return self->PushTraySnapshot(), 0;
        if (msg == kMsgTrayYield)
            return self->PushTrayYield(), 0;
        if (msg == WM_TIMER) {
            wp == kDropPollTimerId ? self->OnDropPollTimer() : (wp == kWatchdogTimerId ? self->OnWatchdogTimer() : void());
            return 0;
        }
        if (msg == WM_CLOSE) {
            self->quitRequested_ = true, self->quitEvent_ ? (SetEvent(self->quitEvent_.get()), 0) : 0;
            return PostQuitMessage(0), 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (msg == WM_ERASEBKGND)
            return 1;
        if (msg == WM_NCHITTEST)
            return ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) ? HTTRANSPARENT : HTCLIENT;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    DWORD corePid_ = 0;
    uint64_t generation_ = 0;
    HINSTANCE instance_ = nullptr;
    DesktopAgentHostOptions options_;
    std::wstring expectedImagePath_;
    FILETIME coreCreationTime_{};
    winrt::handle quitEvent_, coreProc_;
    HWND hostHwnd_ = nullptr;
    std::atomic_bool quitRequested_{false};

    std::unique_ptr<TrayHost> trayHost_;
    bool hasTrayAnchor_ = false;
    RECT trayAnchor_{};
    std::mutex imageVersionsMutex_;
    TraySnapshotAckState snapshotAckState_;
    RecentOpTracker seenTraySendOps_;
    std::atomic<uint64_t> nextOpId_{1};
    std::map<uint64_t, DropOverlay> overlays_;
    bool dropTimerActive_ = false;

    std::mutex taskMutex_;
    std::deque<std::shared_ptr<StaTaskState>> taskQueue_;
    std::mutex evtPipeMutex_;
    HANDLE evtPipe_ = INVALID_HANDLE_VALUE;
    std::atomic<uint64_t> evtSession_{0};
    std::mutex evtPushMutex_;
    std::condition_variable evtPushCv_;
    bool pendingSnapshot_ = false, pendingSnapshotDeferred_ = false, pendingYield_ = false, resyncNeeded_ = false;
    std::atomic<int> remainingFailSnapshotPostsForTest_{0};
    AgentEventMessage queuedSnapshot_, queuedYield_;
    std::thread cmdThread_, evtPushThread_;
};

int DesktopAgentHostApp::Run()
{
    if (!corePid_ || !quitEvent_ || FAILED(OleInitialize(nullptr)))
        return 1;
    if (!VerifyCoreProcessIdentity() || !InitWindows())
        return Cleanup(), OleUninitialize(), 1;
    evtPushThread_ = std::thread([this] { EvtPushLoop(); });
    cmdThread_ = std::thread([this] { CmdServerLoop(); });
    if (options_.onReadyForTest) {
        HWND hwnd = hostHwnd_;
        options_.onReadyForTest([hwnd] { PostMessageW(hwnd, kMsgTraySnapshot, 0, 0); }, [hwnd] { PostMessageW(hwnd, kMsgTrayYield, 0, 0); });
    }

    HANDLE waitHandles[3] = {coreProc_.get(), quitEvent_.get(), options_.stopEvent};
    const DWORD handleCount = options_.stopEvent ? 3U : 2U;
    for (bool done = false; !done;) {
        const DWORD wait = MsgWaitForMultipleObjects(handleCount, waitHandles, FALSE, INFINITE, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0 + handleCount) {
            RequestQuit();
            break;
        }
        for (MSG msg{}; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);) {
            if (msg.message == WM_QUIT) {
                done = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return Cleanup(), OleUninitialize(), 0;
}

void DesktopAgentHostApp::CmdServerLoop()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!quitRequested_.load() && WaitForSingleObject(quitEvent_.get(), 0) == WAIT_TIMEOUT) {
        const winrt::handle pipe{CreateSameUserNamedPipeServer(options_.cmdPipeOverride)};
        if (pipe.get() == INVALID_HANDLE_VALUE || !pipe) {
            Log(L"desktop agent: CreateSameUserNamedPipeServer failed (%lu)", GetLastError());
            if (WaitForSingleObject(quitEvent_.get(), 500) != WAIT_TIMEOUT)
                break;
            continue;
        }
        ServeCmdPipe(pipe.get());
    }
    winrt::uninit_apartment();
}

void DesktopAgentHostApp::ServeCmdPipe(HANDLE pipe)
{
    const winrt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event)
        return;
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    if (!ConnectNamedPipeOverlapped(pipe, ov, quitEvent_.get()))
        return;
    bool verified = false;
    while (!quitRequested_.load() && WaitForSingleObject(quitEvent_.get(), 0) == WAIT_TIMEOUT) {
        std::string raw;
        bool disconnected = false;
        if (!ReadPipeMessageOverlapped(pipe, ov, quitEvent_.get(), raw, disconnected))
            break;
        if (!verified) {
            if (!IsCorePeer(pipe, true, L"Cmd"))
                break;
            verified = true;
        }
        AgentCommandMessage cmd;
        AgentCommandReply reply;
        if (!ValidateAndParseAgentCommand(raw, cmd))
            reply.error = L"invalid_command";
        else
            reply = HandleCommand(cmd);
        if (!WritePipeMessageOverlapped(pipe, ov, quitEvent_.get(), SerializeAgentCommandReply(reply)))
            break;
        if (cmd.type == AgentCommandType::Quit)
            break;
    }
    DisconnectNamedPipe(pipe);
}

AgentCommandReply DesktopAgentHostApp::ExecuteStaCommand(const AgentCommandMessage& cmd)
{
    if (options_.onBeforeStaCommandForTest)
        options_.onBeforeStaCommandForTest(cmd);
    AgentCommandReply reply;
    switch (cmd.type) {
    case AgentCommandType::SetTrayHosting:
        reply.ok = SetTrayHosting(cmd.hosting);
        if (!reply.ok)
            reply.error = L"tray_host_start_failed";
        break;
    case AgentCommandType::SetTrayPlaces: return hasTrayAnchor_ = true, trayAnchor_ = cmd.anchor, ApplyAnchorIfHosted(), reply.ok = true, reply;
    case AgentCommandType::TraySend:
        if (!seenTraySendOps_.MarkSeenIfNew(cmd.opId))
            return reply.ok = true, reply.sendStatus = HostSendStatus::NotSent, reply;
        if (!trayHost_)
            return reply.error = L"tray_host_disabled", reply;
        reply.sendStatus = trayHost_->Send(cmd.key, cmd.action, cmd.at, cmd.cell);
        if (reply.sendStatus != HostSendStatus::Sent) {
            reply.higherIntegrity =
                std::ranges::any_of(trayHost_->Icons(), [&](const auto& icon) { return icon.key == cmd.key && WindowHasHigherIntegrity(icon.hwnd); });
        }
        return reply.ok = true, reply;
    case AgentCommandType::TrayHover: return trayHost_ ? trayHost_->Hover(cmd.key, cmd.at, cmd.cell) : (void)0, reply.ok = true, reply;
    case AgentCommandType::SyncDropBars:
        reply.ok = SyncDropBars(cmd.bars);
        if (!reply.ok)
            reply.error = L"overlay_sync_failed";
        break;
    case AgentCommandType::Ping:
    case AgentCommandType::Quit: break;
    }
    return reply;
}

AgentCommandReply DesktopAgentHostApp::PostStaCommand(const AgentCommandMessage& cmd)
{
    AgentCommandReply reply;
    if (!hostHwnd_ || quitRequested_.load())
        return reply.error = L"sta_unavailable", reply;
    auto state = std::make_shared<StaTaskState>();
    if (!state->done)
        return reply.error = L"sta_unavailable", reply;
    state->cmd = cmd;
    {
        std::lock_guard lock(taskMutex_);
        if (quitRequested_.load())
            return reply.error = L"sta_unavailable", reply;
        taskQueue_.push_back(state);
    }
    if (!PostMessageW(hostHwnd_, kMsgRunTasks, 0, 0)) {
        std::lock_guard lock(taskMutex_);
        std::erase(taskQueue_, state);
        return reply.error = L"sta_unavailable", reply;
    }
    HANDLE hs[2] = {state->done.get(), quitEvent_.get()};
    const DWORD wait = WaitForMultipleObjects(2, hs, FALSE, options_.staTaskTimeoutMs);
    std::lock_guard taskLock(state->mu);
    if (wait == WAIT_OBJECT_0 && state->status == StaTaskState::Status::Completed)
        return state->reply;
    if (state->status == StaTaskState::Status::Queued)
        state->status = StaTaskState::Status::Cancelled;
    reply.error = (wait == WAIT_TIMEOUT) ? L"sta_timeout" : L"sta_unavailable";
    return reply;
}

void DesktopAgentHostApp::DrainStaTasks()
{
    for (;;) {
        std::shared_ptr<StaTaskState> state;
        {
            std::lock_guard lock(taskMutex_);
            if (taskQueue_.empty())
                return;
            state = std::move(taskQueue_.front());
            taskQueue_.pop_front();
        }
        {
            std::lock_guard taskLock(state->mu);
            if (state->status == StaTaskState::Status::Cancelled)
                continue;
            state->status = StaTaskState::Status::Running;
        }
        AgentCommandReply out = ExecuteStaCommand(state->cmd);
        std::lock_guard taskLock(state->mu);
        state->reply = std::move(out);
        state->status = StaTaskState::Status::Completed;
        if (state->done)
            SetEvent(state->done.get());
    }
}

bool DesktopAgentHostApp::SendEvent(const AgentEventMessage& msg, AgentEventReply& replyOut, DWORD timeoutMs, uint64_t* sessionOut)
{
    replyOut = {};
    if (sessionOut)
        *sessionOut = 0;
    const std::string payload = SerializeAgentEvent(msg);
    if (payload.empty() || payload.size() > kMaxAgentMessageBytes)
        return false;
    const int maxAttempts = (msg.type == AgentEventType::Drop) ? 1 : 2;
    std::lock_guard lock(evtPipeMutex_);
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (quitRequested_.load() || !EnsureEvtConnectedLocked(timeoutMs))
            return false;
        if (sessionOut)
            *sessionOut = evtSession_.load();
        std::string rawReply;
        if (SendPipeRequestOverlapped(evtPipe_, payload, rawReply, timeoutMs, quitEvent_.get()) && ValidateAndParseAgentEventReply(rawReply, replyOut) &&
            replyOut.ok) {
            return true;
        }
        CloseEvtPipeLocked();
    }
    return false;
}

void DesktopAgentHostApp::HandleIdleEvtReconnect(DWORD& resyncBackoffMs)
{
    bool connected = false;
    {
        std::lock_guard pipeLock(evtPipeMutex_);
        connected = EnsureEvtConnectedLocked(500);
    }
    if (!connected) {
        resyncBackoffMs = (std::min)(DWORD(2000), resyncBackoffMs * 2);
        return;
    }
    bool needResync = false;
    {
        std::lock_guard lock(evtPushMutex_);
        needResync = resyncNeeded_;
    }
    if (needResync && !RequestTraySnapshotResync()) {
        resyncBackoffMs = (std::min)(DWORD(2000), resyncBackoffMs * 2);
        return;
    }
    resyncBackoffMs = 250;
}

void DesktopAgentHostApp::RecordSentSnapshotAck(uint64_t sessionUsed, const AgentEventMessage& msg, bool deferred)
{
    bool needResend = deferred;
    {
        std::lock_guard lock(imageVersionsMutex_);
        snapshotAckState_.RecordAck(sessionUsed, msg.snapshot, true);
        needResend = needResend || snapshotAckState_.HasUnconfirmedWithEmptyPixels(sessionUsed, msg.snapshot);
    }
    if (needResend && !RequestTraySnapshotResync()) {
        std::lock_guard lock(evtPushMutex_);
        resyncNeeded_ = true;
    }
}

void DesktopAgentHostApp::EvtPushLoop()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    DWORD resyncBackoffMs = 250;
    for (;;) {
        AgentEventMessage msg;
        bool deferred = false;
        bool tryIdleReconnect = false;
        {
            std::unique_lock lock(evtPushMutex_);
            const auto hasWork = [&] { return quitRequested_.load() || pendingSnapshot_ || pendingYield_; };
            if (!hasWork() && resyncNeeded_)
                evtPushCv_.wait_for(lock, std::chrono::milliseconds(resyncBackoffMs), hasWork);
            else
                evtPushCv_.wait(lock, hasWork);
            if (quitRequested_.load())
                break;
            if (pendingSnapshot_)
                msg = std::move(queuedSnapshot_), deferred = pendingSnapshotDeferred_, pendingSnapshot_ = false, pendingSnapshotDeferred_ = false;
            else if (pendingYield_)
                msg = std::move(queuedYield_), pendingYield_ = false;
            else if (resyncNeeded_)
                tryIdleReconnect = true;
        }
        if (tryIdleReconnect) {
            HandleIdleEvtReconnect(resyncBackoffMs);
            continue;
        }
        AgentEventReply reply;
        uint64_t sessionUsed = 0;
        const bool ok = SendEvent(msg, reply, kTrayEventTimeoutMs, &sessionUsed);
        if (ok && reply.ok) {
            resyncBackoffMs = 250;
            if (msg.type == AgentEventType::TraySnapshot) {
                {
                    std::lock_guard lock(evtPushMutex_);
                    resyncNeeded_ = false;
                }
                RecordSentSnapshotAck(sessionUsed, msg, deferred);
            }
        } else {
            std::lock_guard lock(evtPushMutex_);
            resyncNeeded_ = true;
        }
    }
    winrt::uninit_apartment();
}

bool DesktopAgentHostApp::SetTrayHosting(bool on)
{
    if (on == (trayHost_ != nullptr))
        return true;
    {
        std::lock_guard lock(imageVersionsMutex_);
        snapshotAckState_.Reset();
    }
    if (!on) {
        trayHost_.reset(), PushTraySnapshot();
        return true;
    }
    auto onYield = [this] {
        if (hostHwnd_)
            PostMessageW(hostHwnd_, kMsgTrayYield, 0, 0);
    };
    auto onIcons = [this] {
        if (hostHwnd_)
            PostMessageW(hostHwnd_, kMsgTraySnapshot, 0, 0);
    };
    trayHost_ =
        options_.failTrayHostStartForTest ? nullptr : std::make_unique<TrayHost>(hostHwnd_, kMsgTrayNotify, 3000, 0, std::move(onYield), std::move(onIcons));
    if (!trayHost_ || !trayHost_->Running()) {
        Log(L"desktop agent: TrayHost failed to start"), trayHost_.reset();
        return false;
    }
    ApplyAnchorIfHosted(), PushTraySnapshot();
    return true;
}

void DesktopAgentHostApp::PushTraySnapshot()
{
    AgentEventMessage msg;
    msg.type = AgentEventType::TraySnapshot;
    bool deferredBitmap = false;
    if (trayHost_) {
        msg.snapshot.yielded = trayHost_->Yielded();
        msg.snapshot.yieldReason = trayHost_->YieldReason();
        const std::vector<HostIcon> icons = trayHost_->Icons();
        const ULONGLONG now = GetTickCount64();
        const uint64_t session = evtSession_.load();
        size_t pixelBudgetUsed = 0;
        const size_t limit = (std::min)(icons.size(), kMaxAgentTrayIcons);
        msg.snapshot.icons.reserve(limit);
        std::lock_guard imgLock(imageVersionsMutex_);
        for (size_t i = 0; i < limit; ++i)
            msg.snapshot.icons.push_back(
                BuildAgentTrayIconDto(icons[i], now, session, kSnapshotRawPixelBudget, pixelBudgetUsed, snapshotAckState_, deferredBitmap));
    }
    QueueEvtPush(std::move(msg), true, deferredBitmap);
}

bool DesktopAgentHostApp::SyncDropBars(const std::vector<AgentBarRect>& bars)
{
    bool allOk = true;
    std::unordered_set<uint64_t> activeIds;
    for (const auto& bar : bars) {
        if (!bar.visible || IsRectEmpty(&bar.rect) || activeIds.size() >= kMaxAgentDropBars)
            continue;
        activeIds.insert(bar.id);
        const int w = bar.rect.right - bar.rect.left, h = bar.rect.bottom - bar.rect.top;
        if (auto it = overlays_.find(bar.id); it != overlays_.end()) {
            it->second.rect = bar.rect;
            if (options_.failOverlayShowForTest ||
                !SetWindowPos(it->second.hwnd, HWND_TOPMOST, bar.rect.left, bar.rect.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW))
                allOk = false;
            continue;
        }
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT, kOverlayWindowClass,
                                    L"", WS_POPUP, bar.rect.left, bar.rect.top, w, h, nullptr, nullptr, instance_, nullptr);
        if (!hwnd) {
            allOk = false;
            continue;
        }
        auto target = winrt::make_self<AgentDropTarget>(this, bar.id);
        if (options_.failOverlaySyncForTest || FAILED(RegisterDragDrop(hwnd, target.get()))) {
            DestroyWindow(hwnd);
            allOk = false;
            continue;
        }
        if (options_.failOverlayShowForTest || !SetWindowPos(hwnd, HWND_TOPMOST, bar.rect.left, bar.rect.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
            RevokeDragDrop(hwnd);
            DestroyWindow(hwnd);
            allOk = false;
            continue;
        }
        overlays_.emplace(bar.id, DropOverlay{bar.id, bar.rect, hwnd, std::move(target), true});
    }
    for (auto it = overlays_.begin(); it != overlays_.end();)
        it = !activeIds.contains(it->first) ? (DestroyOverlay(it->second), overlays_.erase(it)) : std::next(it);
    if (!overlays_.empty() && !dropTimerActive_ && hostHwnd_)
        SetTimer(hostHwnd_, kDropPollTimerId, kDropPollMs, nullptr), dropTimerActive_ = true;
    else if (overlays_.empty() && dropTimerActive_ && hostHwnd_)
        KillTimer(hostHwnd_, kDropPollTimerId), dropTimerActive_ = false;
    return allOk;
}

void DesktopAgentHostApp::OnDropPollTimer()
{
    POINT pt{};
    if (overlays_.empty() || !GetCursorPos(&pt))
        return;
    const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) < 0) || (GetAsyncKeyState(VK_RBUTTON) < 0);
    GUITHREADINFO gui{sizeof(gui)};
    DWORD capturePid = 0;
    if (GetGUIThreadInfo(0, &gui) && gui.hwndCapture)
        GetWindowThreadProcessId(gui.hwndCapture, &capturePid);
    const bool externalDrag = buttonDown && (capturePid != corePid_) && (GetCapture() == nullptr);
    for (auto& [id, ov] : overlays_) {
        const bool cursorIn = PtInRect(&ov.rect, pt) != FALSE;
        if (ov.target && ov.target->inDrag && (!buttonDown || !cursorIn)) {
            ov.target->ResetDrag();
            if (!buttonDown)
                NotifyDragLeave(id);
        }
        SetOverlayTransparent(ov, !(cursorIn && externalDrag));
    }
}

} // namespace

int RunDesktopAgentWithOptions(DWORD corePid, uint64_t generation, HINSTANCE instance, const DesktopAgentHostOptions& options)
{
    const DWORD sid = CurrentProcessSessionId();
    DesktopAgentHostOptions resolved = options;
    if (resolved.cmdPipeOverride.empty())
        resolved.cmdPipeOverride = DesktopAgentCommandPipeName(sid, corePid, generation);
    if (resolved.evtPipeOverride.empty())
        resolved.evtPipeOverride = DesktopAgentEventPipeName(sid, corePid, generation);
    return DesktopAgentHostApp(corePid, generation, instance, std::move(resolved)).Run();
}

int RunDesktopAgent(DWORD corePid, uint64_t generation, HINSTANCE instance, const wchar_t* cmdPipeOverride, const wchar_t* evtPipeOverride,
                    const wchar_t* nonceOverride)
{
    DesktopAgentHostOptions o;
    o.cmdPipeOverride = cmdPipeOverride ? cmdPipeOverride : L"";
    o.evtPipeOverride = evtPipeOverride ? evtPipeOverride : L"";
    o.nonce = nonceOverride ? nonceOverride : L"";
    return RunDesktopAgentWithOptions(corePid, generation, instance, o);
}

} // namespace atrium
