#include "shell/desktop_agent_host.h"

#include "log.h"
#include "settings.h"
#include "shell/desktop_agent_ipc.h"
#include "shell/process_integrity.h"
#include "shell/tray_host.h"

#include <ole2.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
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
constexpr DWORD kStaTaskTimeoutMs = 5000;
constexpr DWORD kDragOverTimeoutMs = 300;
constexpr DWORD kDropEventTimeoutMs = 500;
constexpr DWORD kTrayEventTimeoutMs = 1500;
constexpr size_t kSnapshotRawPixelBudget = 1300 * 1024;

uint64_t ReadGenerationFile()
{
    const std::wstring dir = DataDirectory();
    if (dir.empty())
        return 0;
    const std::wstring raw = ReadTextFile(dir + L"\\instance.gen");
    std::wstring_view text(raw);
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t' || text.front() == L'\r' || text.front() == L'\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\r' || text.back() == L'\n'))
        text.remove_suffix(1);
    if (text.empty() || !std::ranges::all_of(text, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
        return 0;
    const std::wstring copy(text);
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long val = std::wcstoull(copy.c_str(), &end, 10);
    return (errno == 0 && end != copy.c_str() && *end == L'\0') ? static_cast<uint64_t>(val) : 0;
}

class DesktopAgentHostApp;

struct AgentDropTarget : winrt::implements<AgentDropTarget, IDropTarget> {
    DesktopAgentHostApp* app = nullptr;
    uint64_t barId = 0;
    bool hasFiles = false;
    bool inDrag = false;
    std::wstring lastKey;

    AgentDropTarget(DesktopAgentHostApp* owner, uint64_t id) : app(owner), barId(id) {}
    void ResetDrag() { hasFiles = false, inDrag = false, lastKey.clear(); }
    void EvalEffect(POINTL pt, DWORD* effect);

    HRESULT __stdcall DragEnter(IDataObject* data, DWORD grfKeyState, POINTL pt, DWORD* effect) override;
    HRESULT __stdcall DragOver(DWORD grfKeyState, POINTL pt, DWORD* effect) override;
    HRESULT __stdcall DragLeave() override;
    HRESULT __stdcall Drop(IDataObject* data, DWORD grfKeyState, POINTL pt, DWORD* effect) override;
};

struct DropOverlay {
    uint64_t id = 0;
    RECT rect{};
    HWND hwnd = nullptr;
    winrt::com_ptr<AgentDropTarget> target;
    bool transparent = true;
};

struct StaTask {
    std::function<void()> fn;
    HANDLE doneEvent = nullptr;
};

class DesktopAgentHostApp {
public:
    DesktopAgentHostApp(DWORD corePid, uint64_t generation, HINSTANCE instance, std::wstring cmdPipe, std::wstring evtPipe)
        : corePid_(corePid),
          generation_(generation),
          instance_(instance),
          cmdPipeName_(std::move(cmdPipe)),
          evtPipeName_(std::move(evtPipe)),
          quitEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }
    ~DesktopAgentHostApp() { Cleanup(); }
    DesktopAgentHostApp(const DesktopAgentHostApp&) = delete;
    DesktopAgentHostApp& operator=(const DesktopAgentHostApp&) = delete;

    int Run();
    AgentDragOverResult QueryDragOver(POINT pt);
    void NotifyDragLeave(uint64_t barId);
    void NotifyDropFiles(uint64_t barId, const std::wstring& key, const std::vector<std::wstring>& paths);

private:
    bool InitWindows();
    void Cleanup();
    void CmdServerLoop();
    void ServeCmdPipe(HANDLE pipe);
    AgentCommandReply HandleCommand(const AgentCommandMessage& cmd);
    bool PostStaTask(std::function<void()> fn);
    void DrainStaTasks();
    void EvtPushLoop();
    void QueueEvtPush(AgentEventMessage msg, bool isSnapshot);
    bool SendEvent(const AgentEventMessage& msg, AgentEventReply& replyOut, DWORD timeoutMs);
    void CloseEvtPipeLocked();
    void SetTrayHosting(bool on);
    void PushTraySnapshot();
    void PushTrayYield();
    void SyncDropBars(const std::vector<AgentBarRect>& bars);
    void DestroyAllOverlays();
    void SetOverlayTransparent(DropOverlay& ov, bool transparent);
    void SetOverlayTransparentById(uint64_t barId, bool transparent);
    void DestroyOverlay(DropOverlay& ov);
    void RequestQuit();
    void OnDropPollTimer();
    void OnWatchdogTimer();

    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    DWORD corePid_ = 0;
    uint64_t generation_ = 0;
    HINSTANCE instance_ = nullptr;
    std::wstring cmdPipeName_, evtPipeName_;
    winrt::handle quitEvent_, coreProc_;
    HWND hostHwnd_ = nullptr;
    std::atomic_bool quitRequested_{false};

    std::unique_ptr<TrayHost> trayHost_;
    std::unordered_set<uint64_t> sentImageVersions_;
    uint64_t lastEvtSession_ = 0;
    std::map<uint64_t, DropOverlay> overlays_;
    bool dropTimerActive_ = false;

    std::mutex taskMutex_;
    std::deque<std::shared_ptr<StaTask>> taskQueue_;
    std::mutex evtPipeMutex_;
    HANDLE evtPipe_ = INVALID_HANDLE_VALUE;
    std::atomic<uint64_t> evtSession_{0};
    std::mutex evtPushMutex_;
    std::condition_variable evtPushCv_;
    bool pendingSnapshot_ = false, pendingYield_ = false;
    AgentEventMessage queuedSnapshot_, queuedYield_;
    std::thread cmdThread_, evtPushThread_;
};

void AgentDropTarget::EvalEffect(POINTL pt, DWORD* effect)
{
    const AgentDragOverResult res = (hasFiles && app) ? app->QueryDragOver(POINT{pt.x, pt.y}) : AgentDragOverResult{};
    lastKey = res.key;
    *effect = (hasFiles && res.allow && !res.key.empty()) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

HRESULT __stdcall AgentDropTarget::DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    hasFiles = data && data->QueryGetData(&fe) == S_OK;
    inDrag = true;
    EvalEffect(pt, effect);
    return S_OK;
}

HRESULT __stdcall AgentDropTarget::DragOver(DWORD, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    EvalEffect(pt, effect);
    return S_OK;
}

HRESULT __stdcall AgentDropTarget::DragLeave()
{
    ResetDrag();
    if (app)
        app->NotifyDragLeave(barId);
    return S_OK;
}

HRESULT __stdcall AgentDropTarget::Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    const bool files = hasFiles;
    ResetDrag();
    if (!app)
        return *effect = DROPEFFECT_NONE, S_OK;
    const AgentDragOverResult res = files ? app->QueryDragOver(POINT{pt.x, pt.y}) : AgentDragOverResult{};
    app->NotifyDragLeave(barId);
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (!files || !res.allow || res.key.empty() || !data || FAILED(data->GetData(&fe, &sm)))
        return *effect = DROPEFFECT_NONE, S_OK;
    std::vector<std::wstring> paths;
    if (auto* drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && paths.size() < kMaxAgentDropPaths; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(len > 0 && len <= kMaxAgentPathLength ? len + 1 : 0, L'\0');
            if (!path.empty()) {
                if (const UINT copied = DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())); copied > 0 && copied <= kMaxAgentPathLength)
                    path.resize(copied), paths.push_back(std::move(path));
            }
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    if (paths.empty())
        return *effect = DROPEFFECT_NONE, S_OK;
    return *effect = DROPEFFECT_COPY, app->NotifyDropFiles(barId, res.key, paths), S_OK;
}

LRESULT CALLBACK DesktopAgentHostApp::HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
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
        if (wp == kDropPollTimerId)
            self->OnDropPollTimer();
        else if (wp == kWatchdogTimerId)
            self->OnWatchdogTimer();
        return 0;
    }
    if (msg == WM_CLOSE) {
        self->quitRequested_ = true;
        if (self->quitEvent_)
            SetEvent(self->quitEvent_.get());
        return PostQuitMessage(0), 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DesktopAgentHostApp::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_NCHITTEST)
        return ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) ? HTTRANSPARENT : HTCLIENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool DesktopAgentHostApp::InitWindows()
{
    WNDCLASSEXW hc{sizeof(hc)}, oc{sizeof(oc)};
    hc.lpfnWndProc = HostWndProc, hc.hInstance = instance_, hc.lpszClassName = kHostWindowClass, RegisterClassExW(&hc);
    oc.lpfnWndProc = OverlayWndProc, oc.hInstance = instance_, oc.lpszClassName = kOverlayWindowClass, RegisterClassExW(&oc);
    hostHwnd_ = CreateWindowExW(0, kHostWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, this);
    if (!hostHwnd_)
        return Log(L"desktop agent: CreateWindowExW host failed (%lu)", GetLastError()), false;
    return SetTimer(hostHwnd_, kWatchdogTimerId, kWatchdogMs, nullptr), true;
}

void DesktopAgentHostApp::Cleanup()
{
    quitRequested_ = true;
    if (quitEvent_)
        SetEvent(quitEvent_.get());
    evtPushCv_.notify_all();
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

int DesktopAgentHostApp::Run()
{
    if (!corePid_ || !quitEvent_ || FAILED(OleInitialize(nullptr)))
        return 1;
    coreProc_.attach(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, corePid_));
    if (!coreProc_ || !InitWindows())
        return Cleanup(), OleUninitialize(), 1;
    evtPushThread_ = std::thread([this] { EvtPushLoop(); });
    cmdThread_ = std::thread([this] { CmdServerLoop(); });

    HANDLE waitHandles[2] = {coreProc_.get(), quitEvent_.get()};
    for (bool done = false; !done;) {
        if (MsgWaitForMultipleObjects(2, waitHandles, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0 + 2)
            break;
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
        const winrt::handle pipe{CreateSameUserNamedPipeServer(cmdPipeName_)};
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
            if (!VerifyNamedPipeClientSameUser(pipe)) {
                Log(L"desktop agent: rejected Cmd client with mismatched user SID");
                break;
            }
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
        FlushFileBuffers(pipe);
        if (cmd.type == AgentCommandType::Quit)
            break;
    }
    DisconnectNamedPipe(pipe);
}

AgentCommandReply DesktopAgentHostApp::HandleCommand(const AgentCommandMessage& cmd)
{
    AgentCommandReply reply;
    if (cmd.type == AgentCommandType::Ping) {
        reply.ok = true;
        reply.pid = GetCurrentProcessId();
        reply.generation = generation_;
        return reply;
    }
    if (cmd.type == AgentCommandType::Quit)
        return RequestQuit(), reply.ok = true, reply;
    const bool dispatched = PostStaTask([&] {
        switch (cmd.type) {
        case AgentCommandType::SetTrayHosting: SetTrayHosting(cmd.hosting), reply.ok = true; break;
        case AgentCommandType::SetTrayPlaces:
            if (trayHost_)
                trayHost_->SetPlaces(cmd.anchor);
            reply.ok = true;
            break;
        case AgentCommandType::TraySend:
            if (!trayHost_) {
                reply.error = L"tray_host_disabled";
            } else {
                reply.sendStatus = trayHost_->Send(cmd.key, cmd.action, cmd.at, cmd.cell);
                if (reply.sendStatus != HostSendStatus::Sent) {
                    for (const auto& icon : trayHost_->Icons()) {
                        if (icon.key == cmd.key && WindowHasHigherIntegrity(icon.hwnd)) {
                            reply.higherIntegrity = true;
                            break;
                        }
                    }
                }
                reply.ok = true;
            }
            break;
        case AgentCommandType::TrayHover:
            if (trayHost_)
                trayHost_->Hover(cmd.key, cmd.at, cmd.cell);
            reply.ok = true;
            break;
        case AgentCommandType::SyncDropBars: SyncDropBars(cmd.bars), reply.ok = true; break;
        case AgentCommandType::Ping:
        case AgentCommandType::Quit: break;
        }
    });
    if (!dispatched)
        reply.ok = false, reply.error = L"sta_unavailable";
    return reply;
}

bool DesktopAgentHostApp::PostStaTask(std::function<void()> fn)
{
    if (!hostHwnd_ || quitRequested_.load())
        return false;
    const winrt::handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!done)
        return false;
    auto task = std::make_shared<StaTask>();
    task->fn = std::move(fn);
    task->doneEvent = done.get();
    {
        std::lock_guard lock(taskMutex_);
        if (quitRequested_.load())
            return false;
        taskQueue_.push_back(task);
    }
    if (!PostMessageW(hostHwnd_, kMsgRunTasks, 0, 0)) {
        std::lock_guard lock(taskMutex_);
        std::erase(taskQueue_, task);
        return false;
    }
    HANDLE hs[2] = {done.get(), quitEvent_.get()};
    return WaitForMultipleObjects(2, hs, FALSE, kStaTaskTimeoutMs) == WAIT_OBJECT_0;
}

void DesktopAgentHostApp::DrainStaTasks()
{
    for (;;) {
        std::shared_ptr<StaTask> task;
        {
            std::lock_guard lock(taskMutex_);
            if (taskQueue_.empty())
                return;
            task = std::move(taskQueue_.front());
            taskQueue_.pop_front();
        }
        if (task->fn)
            task->fn();
        if (task->doneEvent)
            SetEvent(task->doneEvent);
    }
}

void DesktopAgentHostApp::CloseEvtPipeLocked()
{
    if (evtPipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(evtPipe_);
        evtPipe_ = INVALID_HANDLE_VALUE;
    }
}

bool DesktopAgentHostApp::SendEvent(const AgentEventMessage& msg, AgentEventReply& replyOut, DWORD timeoutMs)
{
    replyOut = {};
    const std::string payload = SerializeAgentEvent(msg);
    if (payload.empty() || payload.size() > kMaxAgentMessageBytes)
        return false;
    std::lock_guard lock(evtPipeMutex_);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (quitRequested_.load())
            return false;
        if (evtPipe_ == INVALID_HANDLE_VALUE) {
            evtPipe_ = ConnectSameUserPipeClient(evtPipeName_, timeoutMs, quitEvent_.get());
            if (evtPipe_ == INVALID_HANDLE_VALUE)
                return false;
            evtSession_.fetch_add(1);
        }
        std::string rawReply;
        if (SendPipeRequestOverlapped(evtPipe_, payload, rawReply, timeoutMs, quitEvent_.get()) && ValidateAndParseAgentEventReply(rawReply, replyOut) &&
            replyOut.ok) {
            return true;
        }
        CloseEvtPipeLocked();
    }
    return false;
}

void DesktopAgentHostApp::QueueEvtPush(AgentEventMessage msg, bool isSnapshot)
{
    {
        std::lock_guard lock(evtPushMutex_);
        if (isSnapshot)
            queuedSnapshot_ = std::move(msg), pendingSnapshot_ = true;
        else
            queuedYield_ = std::move(msg), pendingYield_ = true;
    }
    evtPushCv_.notify_one();
}

void DesktopAgentHostApp::EvtPushLoop()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    for (;;) {
        AgentEventMessage msg;
        {
            std::unique_lock lock(evtPushMutex_);
            evtPushCv_.wait(lock, [&] { return quitRequested_.load() || pendingSnapshot_ || pendingYield_; });
            if (quitRequested_.load())
                break;
            if (pendingSnapshot_)
                msg = std::move(queuedSnapshot_), pendingSnapshot_ = false;
            else
                msg = std::move(queuedYield_), pendingYield_ = false;
        }
        AgentEventReply reply;
        SendEvent(msg, reply, kTrayEventTimeoutMs);
    }
    winrt::uninit_apartment();
}

void DesktopAgentHostApp::SetTrayHosting(bool on)
{
    if (on == (trayHost_ != nullptr))
        return;
    sentImageVersions_.clear();
    if (!on) {
        trayHost_.reset();
        PushTraySnapshot();
        return;
    }
    auto onYield = [this] {
        if (hostHwnd_)
            PostMessageW(hostHwnd_, kMsgTrayYield, 0, 0);
    };
    auto onIcons = [this] {
        if (hostHwnd_)
            PostMessageW(hostHwnd_, kMsgTraySnapshot, 0, 0);
    };
    trayHost_ = std::make_unique<TrayHost>(hostHwnd_, kMsgTrayNotify, 3000, 0, std::move(onYield), std::move(onIcons));
    if (!trayHost_->Running()) {
        Log(L"desktop agent: TrayHost failed to start");
        trayHost_.reset();
        return;
    }
    PushTraySnapshot();
}

void DesktopAgentHostApp::PushTraySnapshot()
{
    AgentEventMessage msg;
    msg.type = AgentEventType::TraySnapshot;
    if (const uint64_t session = evtSession_.load(); session != lastEvtSession_) {
        sentImageVersions_.clear();
        lastEvtSession_ = session;
    }
    if (trayHost_) {
        msg.snapshot.yielded = trayHost_->Yielded();
        msg.snapshot.yieldReason = trayHost_->YieldReason();
        const std::vector<HostIcon> icons = trayHost_->Icons();
        const ULONGLONG now = GetTickCount64();
        std::unordered_set<uint64_t> nextSent;
        size_t pixelBudgetUsed = 0;
        bool deferredBitmap = false;
        const size_t limit = (std::min)(icons.size(), kMaxAgentTrayIcons);
        msg.snapshot.icons.reserve(limit);
        for (size_t i = 0; i < limit; ++i) {
            const HostIcon& src = icons[i];
            AgentTrayIconDto dto;
            dto.key = src.key, dto.hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(src.hwnd));
            dto.uid = src.uid, dto.callback = src.callback, dto.version = src.version, dto.showTip = src.showTip;
            dto.tip = src.tip, dto.exe = src.exe, dto.pid = src.pid, dto.imageVersion = src.imageVersion;
            dto.hidden = src.hidden, dto.added = src.added, dto.flashing = src.flash.until > now;
            if (src.image && !src.image->Empty() && src.imageVersion != 0) {
                const size_t byteSize = src.image->ByteSize();
                const bool dimsOk = src.image->width > 0 && src.image->height > 0 && src.image->width <= kMaxAgentIconDimension &&
                                    src.image->height <= kMaxAgentIconDimension &&
                                    byteSize == static_cast<size_t>(src.image->width) * static_cast<size_t>(src.image->height) * 4;
                if (sentImageVersions_.contains(src.imageVersion)) {
                    nextSent.insert(src.imageVersion);
                } else if (dimsOk && pixelBudgetUsed + byteSize <= kSnapshotRawPixelBudget) {
                    dto.width = src.image->width, dto.height = src.image->height, dto.pixels = src.image->Bytes();
                    pixelBudgetUsed += byteSize, nextSent.insert(src.imageVersion);
                } else if (dimsOk) {
                    deferredBitmap = true;
                }
            }
            msg.snapshot.icons.push_back(std::move(dto));
        }
        sentImageVersions_ = std::move(nextSent);
        if (deferredBitmap && hostHwnd_)
            PostMessageW(hostHwnd_, kMsgTraySnapshot, 0, 0);
    }
    QueueEvtPush(std::move(msg), true);
}

void DesktopAgentHostApp::PushTrayYield()
{
    AgentEventMessage msg;
    msg.type = AgentEventType::TrayYield;
    if (trayHost_)
        msg.yielded = trayHost_->Yielded(), msg.yieldReason = trayHost_->YieldReason();
    QueueEvtPush(std::move(msg), false);
}

AgentDragOverResult DesktopAgentHostApp::QueryDragOver(POINT pt)
{
    AgentEventMessage msg;
    msg.type = AgentEventType::DragOver, msg.pt = pt;
    AgentEventReply reply;
    return SendEvent(msg, reply, kDragOverTimeoutMs) ? AgentDragOverResult{reply.allow, std::move(reply.key)} : AgentDragOverResult{};
}

void DesktopAgentHostApp::NotifyDragLeave(uint64_t barId)
{
    SetOverlayTransparentById(barId, true);
    AgentEventMessage msg;
    msg.type = AgentEventType::DragLeave;
    AgentEventReply reply;
    SendEvent(msg, reply, kDropEventTimeoutMs);
}

void DesktopAgentHostApp::NotifyDropFiles(uint64_t barId, const std::wstring& key, const std::vector<std::wstring>& paths)
{
    SetOverlayTransparentById(barId, true);
    if (!IsValidAgentDropPayload(key, paths))
        return;
    AgentEventMessage msg;
    msg.type = AgentEventType::Drop, msg.key = key, msg.paths = paths;
    AgentEventReply reply;
    SendEvent(msg, reply, kDropEventTimeoutMs);
}

void DesktopAgentHostApp::SetOverlayTransparent(DropOverlay& ov, bool transparent)
{
    if (!ov.hwnd || ov.transparent == transparent)
        return;
    LONG_PTR ex = GetWindowLongPtrW(ov.hwnd, GWL_EXSTYLE);
    ex = transparent ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    SetWindowLongPtrW(ov.hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ov.transparent = transparent;
}

void DesktopAgentHostApp::SetOverlayTransparentById(uint64_t barId, bool transparent)
{
    if (const auto it = overlays_.find(barId); it != overlays_.end())
        SetOverlayTransparent(it->second, transparent);
}

void DesktopAgentHostApp::DestroyOverlay(DropOverlay& ov)
{
    if (ov.hwnd)
        RevokeDragDrop(ov.hwnd), DestroyWindow(ov.hwnd), ov.hwnd = nullptr;
}

void DesktopAgentHostApp::RequestQuit()
{
    quitRequested_ = true;
    if (quitEvent_)
        SetEvent(quitEvent_.get());
    if (hostHwnd_)
        PostMessageW(hostHwnd_, WM_CLOSE, 0, 0);
}

void DesktopAgentHostApp::DestroyAllOverlays()
{
    if (dropTimerActive_ && hostHwnd_)
        KillTimer(hostHwnd_, kDropPollTimerId), dropTimerActive_ = false;
    for (auto& [id, ov] : overlays_)
        DestroyOverlay(ov);
    overlays_.clear();
}

void DesktopAgentHostApp::SyncDropBars(const std::vector<AgentBarRect>& bars)
{
    std::unordered_set<uint64_t> activeIds;
    for (const auto& bar : bars) {
        if (!bar.visible || IsRectEmpty(&bar.rect) || activeIds.size() >= kMaxAgentDropBars)
            continue;
        activeIds.insert(bar.id);
        const int w = bar.rect.right - bar.rect.left, h = bar.rect.bottom - bar.rect.top;
        if (auto it = overlays_.find(bar.id); it != overlays_.end()) {
            it->second.rect = bar.rect;
            SetWindowPos(it->second.hwnd, HWND_TOPMOST, bar.rect.left, bar.rect.top, w, h, SWP_NOACTIVATE);
            continue;
        }
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT, kOverlayWindowClass,
                                    L"", WS_POPUP, bar.rect.left, bar.rect.top, w, h, nullptr, nullptr, instance_, nullptr);
        if (!hwnd)
            continue;
        auto target = winrt::make_self<AgentDropTarget>(this, bar.id);
        if (FAILED(RegisterDragDrop(hwnd, target.get()))) {
            DestroyWindow(hwnd);
            continue;
        }
        SetWindowPos(hwnd, HWND_TOPMOST, bar.rect.left, bar.rect.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        overlays_.emplace(bar.id, DropOverlay{bar.id, bar.rect, hwnd, std::move(target), true});
    }
    for (auto it = overlays_.begin(); it != overlays_.end();) {
        if (!activeIds.contains(it->first)) {
            DestroyOverlay(it->second);
            it = overlays_.erase(it);
        } else {
            ++it;
        }
    }
    if (!overlays_.empty() && !dropTimerActive_ && hostHwnd_)
        SetTimer(hostHwnd_, kDropPollTimerId, kDropPollMs, nullptr), dropTimerActive_ = true;
    else if (overlays_.empty() && dropTimerActive_ && hostHwnd_)
        KillTimer(hostHwnd_, kDropPollTimerId), dropTimerActive_ = false;
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

void DesktopAgentHostApp::OnWatchdogTimer()
{
    if (generation_ == 0)
        return;
    if (const uint64_t active = ReadGenerationFile(); active != 0 && active > generation_) {
        Log(L"desktop agent: generation rotated (%llu -> %llu); exiting", static_cast<unsigned long long>(generation_),
            static_cast<unsigned long long>(active));
        RequestQuit();
    }
}

} // namespace

int RunDesktopAgent(DWORD corePid, uint64_t generation, HINSTANCE instance, const wchar_t* cmdPipeOverride, const wchar_t* evtPipeOverride)
{
    const DWORD sid = CurrentProcessSessionId();
    const std::wstring cmd = (cmdPipeOverride && *cmdPipeOverride) ? cmdPipeOverride : DesktopAgentCommandPipeName(sid, corePid, generation);
    const std::wstring evt = (evtPipeOverride && *evtPipeOverride) ? evtPipeOverride : DesktopAgentEventPipeName(sid, corePid, generation);
    return DesktopAgentHostApp(corePid, generation, instance, cmd, evt).Run();
}

} // namespace atrium
