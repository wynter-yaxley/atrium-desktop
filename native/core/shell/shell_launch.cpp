// Dedicated STA shell-launch worker, Explorer COM de-elevation, and bounded synchronous dispatch.
#include "shell/shell_launch.h"

#include "log.h"
#include "shell/shell_taskbar.h"

#include <objbase.h>
#include <ole2.h>
#include <ocidl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <exdisp.h>
#include <servprov.h>
#include <shellapi.h>
#include <oleauto.h>

#include <winrt/base.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace atrium {

namespace {

bool IsRunAsVerb(const std::wstring& verb) { return !verb.empty() && _wcsicmp(verb.c_str(), L"runas") == 0; }

bool StartsWithCaseInsensitive(const std::wstring& value, const std::wstring& prefix)
{
    return value.size() >= prefix.size() && _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) == 0;
}

class StaComScope {
public:
    StaComScope() : owned_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {}
    StaComScope(const StaComScope&) = delete;
    StaComScope& operator=(const StaComScope&) = delete;
    ~StaComScope()
    {
        if (owned_)
            CoUninitialize();
    }

private:
    bool owned_ = false;
};

class ScopedBstr {
public:
    explicit ScopedBstr(const std::wstring& text) : bstr_(SysAllocStringLen(text.data(), UINT(text.size()))) {}
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;
    ~ScopedBstr()
    {
        if (bstr_)
            SysFreeString(bstr_);
    }

    BSTR Get() const { return bstr_; }

private:
    BSTR bstr_ = nullptr;
};

class ScopedVariant {
public:
    explicit ScopedVariant(const std::wstring& text)
    {
        VariantInit(&var_);
        if (!text.empty()) {
            var_.vt = VT_BSTR;
            var_.bstrVal = SysAllocStringLen(text.data(), UINT(text.size()));
        }
    }

    explicit ScopedVariant(int value)
    {
        VariantInit(&var_);
        var_.vt = VT_I4;
        var_.lVal = value;
    }

    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;
    ~ScopedVariant() { VariantClear(&var_); }

    const VARIANT& Get() const { return var_; }

private:
    VARIANT var_{};
};

bool IsWindowElevatedOrProtected(HWND hwnd)
{
    if (!hwnd)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid)
        return false;
    const winrt::handle proc{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!proc)
        return GetLastError() == ERROR_ACCESS_DENIED;
    const DWORD rid = ProcessIntegrityRid(proc.get());
    return rid != 0 && IsElevatedIntegrityRid(rid);
}

std::wstring NormalizeAppsFolderTarget(const LaunchRequest& request)
{
    if (request.kind == LaunchTargetKind::AppUserModelId && !StartsWithCaseInsensitive(request.target, L"shell:"))
        return L"shell:AppsFolder\\" + request.target;
    return request.target;
}

LaunchResult ExecuteDirect(const LaunchRequest& request, bool forceRunAs)
{
    std::wstring effectiveTarget = request.target;
    if (request.kind == LaunchTargetKind::AppUserModelId && !StartsWithCaseInsensitive(request.target, L"shell:")) {
        const bool defaultVerb = request.verb.empty() || _wcsicmp(request.verb.c_str(), L"open") == 0;
        if (!forceRunAs && request.parameters.empty() && defaultVerb && request.target.find(L'!') != std::wstring::npos) {
            winrt::com_ptr<IApplicationActivationManager> aam;
            DWORD pid = 0;
            if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(aam.put()))) && aam &&
                SUCCEEDED(aam->ActivateApplication(request.target.c_str(), nullptr, AO_NONE, &pid))) {
                return LaunchResult{LaunchOutcome::Succeeded, 0, nullptr};
            }
        }
        effectiveTarget = L"shell:AppsFolder\\" + request.target;
    }

    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI | (request.noAsync ? SEE_MASK_NOASYNC : 0UL) | (request.captureProcess ? SEE_MASK_NOCLOSEPROCESS : 0UL);
    sei.lpVerb = forceRunAs ? L"runas" : (request.verb.empty() ? nullptr : request.verb.c_str());
    sei.lpFile = effectiveTarget.c_str();
    sei.lpParameters = request.parameters.empty() ? nullptr : request.parameters.c_str();
    sei.lpDirectory = request.workingDirectory.empty() ? nullptr : request.workingDirectory.c_str();
    sei.nShow = request.show;
    if (!ShellExecuteExW(&sei))
        return LaunchResult{LaunchOutcome::Failed, GetLastError(), nullptr};
    return LaunchResult{LaunchOutcome::Succeeded, 0, request.captureProcess ? sei.hProcess : nullptr};
}

LaunchResult ExecuteExplorerComMedium(const LaunchRequest& request)
{
    const HWND shellWindow = GetShellWindow();
    if (!shellWindow)
        return LaunchResult{LaunchOutcome::Failed, ERROR_SERVICE_NOT_ACTIVE, nullptr};
    if (IsHungAppWindow(shellWindow) || !ShellResponsive(shellWindow, 300, false))
        return LaunchResult{LaunchOutcome::TimedOutOrUnknown, ERROR_TIMEOUT, nullptr};
    if (IsWindowElevatedOrProtected(shellWindow))
        return LaunchResult{LaunchOutcome::Failed, ERROR_ACCESS_DENIED, nullptr};

    winrt::com_ptr<IShellWindows> shellWindows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(shellWindows.put()));
    if (FAILED(hr) || !shellWindows)
        return LaunchResult{LaunchOutcome::Failed, static_cast<DWORD>(hr), nullptr};

    VARIANT empty{};
    VariantInit(&empty);
    long desktopHwnd = 0;
    winrt::com_ptr<IDispatch> desktopDisp;
    hr = shellWindows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &desktopHwnd, SWFO_NEEDDISPATCH, desktopDisp.put());
    if (hr != S_OK || !desktopDisp)
        return LaunchResult{LaunchOutcome::Failed, FAILED(hr) ? static_cast<DWORD>(hr) : ERROR_NOT_FOUND, nullptr};
    if (desktopHwnd != 0 && IsWindowElevatedOrProtected(reinterpret_cast<HWND>(static_cast<LONG_PTR>(desktopHwnd))))
        return LaunchResult{LaunchOutcome::Failed, ERROR_ACCESS_DENIED, nullptr};

    auto serviceProvider = desktopDisp.try_as<IServiceProvider>();
    if (!serviceProvider)
        return LaunchResult{LaunchOutcome::Failed, static_cast<DWORD>(E_NOINTERFACE), nullptr};

    winrt::com_ptr<IShellBrowser> shellBrowser;
    hr = serviceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(shellBrowser.put()));
    if (FAILED(hr) || !shellBrowser)
        return LaunchResult{LaunchOutcome::Failed, static_cast<DWORD>(hr), nullptr};

    winrt::com_ptr<IShellView> shellView;
    hr = shellBrowser->QueryActiveShellView(shellView.put());
    if (FAILED(hr) || !shellView)
        return LaunchResult{LaunchOutcome::Failed, static_cast<DWORD>(hr), nullptr};

    winrt::com_ptr<IDispatch> viewDisp;
    hr = shellView->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(viewDisp.put()));
    auto folderView = viewDisp ? viewDisp.try_as<IShellFolderViewDual>() : nullptr;
    if (FAILED(hr) || !folderView)
        return LaunchResult{LaunchOutcome::Failed, FAILED(hr) ? static_cast<DWORD>(hr) : static_cast<DWORD>(E_NOINTERFACE), nullptr};

    winrt::com_ptr<IDispatch> appDisp;
    hr = folderView->get_Application(appDisp.put());
    auto shellDispatch = appDisp ? appDisp.try_as<IShellDispatch2>() : nullptr;
    if (FAILED(hr) || !shellDispatch)
        return LaunchResult{LaunchOutcome::Failed, FAILED(hr) ? static_cast<DWORD>(hr) : static_cast<DWORD>(E_NOINTERFACE), nullptr};

    const std::wstring target = NormalizeAppsFolderTarget(request);
    const ScopedBstr fileBstr(target);
    if (!fileBstr.Get())
        return LaunchResult{LaunchOutcome::Failed, ERROR_OUTOFMEMORY, nullptr};
    const ScopedVariant argsVar(request.parameters);
    const ScopedVariant dirVar(request.workingDirectory);
    const ScopedVariant verbVar(request.verb);
    const ScopedVariant showVar(request.show);

    hr = shellDispatch->ShellExecute(fileBstr.Get(), argsVar.Get(), dirVar.Get(), verbVar.Get(), showVar.Get());
    if (hr == S_OK)
        return LaunchResult{LaunchOutcome::Succeeded, 0, nullptr};
    if (hr == RPC_E_TIMEOUT || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
        return LaunchResult{LaunchOutcome::TimedOutOrUnknown, ERROR_TIMEOUT, nullptr};
    return LaunchResult{LaunchOutcome::Failed, FAILED(hr) ? static_cast<DWORD>(hr) : ERROR_GEN_FAILURE, nullptr};
}

struct LaunchWaitState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    LaunchResult result;
};

struct QueuedLaunch {
    LaunchRequest request;
    LaunchRoute route = LaunchRoute::DirectCurrentToken;
    std::shared_ptr<LaunchWaitState> waitState;
};

void CompleteWaitState(const std::shared_ptr<LaunchWaitState>& state, LaunchResult result)
{
    if (!state)
        return;
    std::lock_guard lock(state->mutex);
    if (state->abandoned) {
        if (result.process)
            CloseHandle(result.process);
        return;
    }
    state->result = result;
    state->done = true;
    state->cv.notify_all();
}

void LogLaunchFailureIfNeeded(const LaunchRequest& request, LaunchRoute route, const LaunchResult& result)
{
    if (result.outcome == LaunchOutcome::Succeeded)
        return;
    if (route == LaunchRoute::DirectRunAs && result.error == ERROR_CANCELLED)
        return;
    if (route == LaunchRoute::ExplorerComMedium) {
        Log(L"launch: Explorer medium delegation failed for '%s' (outcome=%d, err=0x%08lX)", request.target.c_str(), int(result.outcome), result.error);
    } else if (result.outcome == LaunchOutcome::TimedOutOrUnknown) {
        Log(L"launch: launch timed out for '%s' (err=%lu)", request.target.c_str(), result.error);
    } else {
        Log(L"launch: failed for '%s' (err=0x%08lX)", request.target.c_str(), result.error);
    }
}

void CALLBACK RunAsThreadpoolProc(PTP_CALLBACK_INSTANCE, void* context)
{
    const std::unique_ptr<QueuedLaunch> job(static_cast<QueuedLaunch*>(context));
    const StaComScope com;
    const LaunchResult result = ExecuteDirect(job->request, true);
    if (!job->waitState)
        LogLaunchFailureIfNeeded(job->request, job->route, result);
    CompleteWaitState(job->waitState, result);
}

class ShellLauncher {
public:
    ShellLauncher() = default;
    ShellLauncher(const ShellLauncher&) = delete;
    ShellLauncher& operator=(const ShellLauncher&) = delete;
    ~ShellLauncher() { Stop(); }

    void Start()
    {
        std::lock_guard lifecycleLock(lifecycleMutex_);
        StartLocked();
    }

    void Stop()
    {
        constexpr DWORD kStopWaitMs = 1500;
        winrt::handle workerToWait;
        std::shared_ptr<WorkerState> state;
        std::deque<QueuedLaunch> cancelled;
        {
            std::lock_guard lifecycleLock(lifecycleMutex_);
            if (!running_)
                return;
            state = std::move(state_);
            workerToWait = std::move(workerThread_);
            running_ = false;
        }
        if (state) {
            {
                std::lock_guard queueLock(state->queueMutex);
                state->stop = true;
                cancelled.swap(state->queue);
            }
            if (state->wakeEvent)
                SetEvent(state->wakeEvent.get());
        }
        for (const QueuedLaunch& pending : cancelled)
            CompleteWaitState(pending.waitState, LaunchResult{LaunchOutcome::Failed, ERROR_OPERATION_ABORTED, nullptr});
        if (workerToWait && GetThreadId(workerToWait.get()) != GetCurrentThreadId() && WaitForSingleObject(workerToWait.get(), kStopWaitMs) != WAIT_OBJECT_0) {
            Log(L"launch: worker stop timed out; abandoning stuck launch call");
        }
    }

    void Enqueue(QueuedLaunch job)
    {
        std::lock_guard lifecycleLock(lifecycleMutex_);
        if (!running_)
            StartLocked();
        if (!state_) {
            CompleteWaitState(job.waitState, LaunchResult{LaunchOutcome::Failed, ERROR_NOT_READY, nullptr});
            return;
        }
        {
            std::lock_guard queueLock(state_->queueMutex);
            state_->queue.push_back(std::move(job));
        }
        if (state_->wakeEvent)
            SetEvent(state_->wakeEvent.get());
    }

private:
    struct WorkerState {
        std::mutex queueMutex;
        std::deque<QueuedLaunch> queue;
        winrt::handle wakeEvent;
        winrt::handle readyEvent;
        bool stop = false;
    };

    void StartLocked()
    {
        if (running_)
            return;
        auto state = std::make_shared<WorkerState>();
        state->wakeEvent = winrt::handle{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        state->readyEvent = winrt::handle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        auto holder = std::make_unique<std::shared_ptr<WorkerState>>(state);
        workerThread_ = winrt::handle{CreateThread(nullptr, 0, &ShellLauncher::WorkerThreadProc, holder.get(), 0, nullptr)};
        if (!workerThread_)
            return;
        [[maybe_unused]] auto* raw = holder.release();
        state_ = std::move(state);
        running_ = true;
        if (state_->readyEvent)
            WaitForSingleObject(state_->readyEvent.get(), 5000);
    }

    static DWORD WINAPI WorkerThreadProc(LPVOID param)
    {
        const std::unique_ptr<std::shared_ptr<WorkerState>> holder(static_cast<std::shared_ptr<WorkerState>*>(param));
        WorkerMain(*holder);
        return 0;
    }

    static void WorkerMain(const std::shared_ptr<WorkerState>& state)
    {
        const StaComScope com;
        MSG msg{};
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        if (state->readyEvent)
            SetEvent(state->readyEvent.get());

        for (;;) {
            QueuedLaunch job;
            bool hasJob = false;
            {
                std::lock_guard queueLock(state->queueMutex);
                if (state->stop)
                    break;
                if (!state->queue.empty()) {
                    job = std::move(state->queue.front());
                    state->queue.pop_front();
                    hasJob = true;
                }
            }
            if (hasJob) {
                const LaunchResult result =
                    job.route == LaunchRoute::ExplorerComMedium ? ExecuteExplorerComMedium(job.request) : ExecuteDirect(job.request, false);
                if (!job.waitState)
                    LogLaunchFailureIfNeeded(job.request, job.route, result);
                CompleteWaitState(job.waitState, result);
                continue;
            }
            const HANDLE wake = state->wakeEvent.get();
            const DWORD wait = wake ? MsgWaitForMultipleObjects(1, &wake, FALSE, INFINITE, QS_ALLINPUT) : WAIT_FAILED;
            if (wait == WAIT_FAILED)
                break;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    return;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    std::mutex lifecycleMutex_;
    std::shared_ptr<WorkerState> state_;
    winrt::handle workerThread_;
    bool running_ = false;
};

ShellLauncher& LauncherInstance()
{
    static ShellLauncher instance;
    return instance;
}

LaunchResult WaitForLaunch(const std::shared_ptr<LaunchWaitState>& state, DWORD timeoutMs)
{
    std::unique_lock lock(state->mutex);
    if (timeoutMs == INFINITE) {
        state->cv.wait(lock, [&] { return state->done; });
        return state->result;
    }
    if (state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return state->done; }))
        return state->result;
    state->abandoned = true;
    return LaunchResult{LaunchOutcome::TimedOutOrUnknown, WAIT_TIMEOUT, nullptr};
}

} // namespace

LaunchRoute ResolveLaunchRoute(PrivilegeMode mode, LaunchPrivilege privilege, LaunchTargetKind /*kind*/, const std::wstring& verb)
{
    if (privilege == LaunchPrivilege::RequestElevation || IsRunAsVerb(verb))
        return LaunchRoute::DirectRunAs;
    if (privilege == LaunchPrivilege::Inherit)
        return LaunchRoute::DirectCurrentToken;
    switch (mode) {
    case PrivilegeMode::ElevatedSplit:
        // Both ShellDefault and RequireMedium de-elevate through the medium-integrity Explorer shell.
        return LaunchRoute::ExplorerComMedium;
    case PrivilegeMode::Standard: return LaunchRoute::DirectCurrentToken;
    case PrivilegeMode::ElevatedUnified:
    case PrivilegeMode::CrossAccountElevated:
    case PrivilegeMode::Unknown:
        // Never delegate when Explorer is elevated, cross-account, or unknown; fail closed when Medium is required.
        return privilege == LaunchPrivilege::RequireMedium ? LaunchRoute::BlockedMismatch : LaunchRoute::DirectCurrentToken;
    }
    return privilege == LaunchPrivilege::RequireMedium ? LaunchRoute::BlockedMismatch : LaunchRoute::DirectCurrentToken;
}

void StartShellLaunch() { LauncherInstance().Start(); }

void StopShellLaunch() { LauncherInstance().Stop(); }

LaunchResult ShellLaunch(const LaunchRequest& request, DWORD timeoutMs)
{
    if (request.target.empty())
        return LaunchResult{LaunchOutcome::Failed, ERROR_INVALID_PARAMETER, nullptr};

    const LaunchRoute route = ResolveLaunchRoute(CurrentPrivilegeMode(), request.privilege, request.kind, request.verb);
    if (route == LaunchRoute::BlockedMismatch) {
        Log(L"launch: blocked medium-only launch of '%s' under current privilege mode", request.target.c_str());
        return LaunchResult{LaunchOutcome::Failed, ERROR_ACCESS_DENIED, nullptr};
    }

    auto state = std::make_shared<LaunchWaitState>();
    if (route == LaunchRoute::DirectRunAs) {
        auto job = std::make_unique<QueuedLaunch>(QueuedLaunch{request, route, state});
        if (!TrySubmitThreadpoolCallback(RunAsThreadpoolProc, job.get(), nullptr))
            return LaunchResult{LaunchOutcome::Failed, GetLastError(), nullptr};
        [[maybe_unused]] QueuedLaunch* raw = job.release();
        return WaitForLaunch(state, timeoutMs);
    }

    LauncherInstance().Enqueue(QueuedLaunch{request, route, state});
    const LaunchResult result = WaitForLaunch(state, timeoutMs);
    LogLaunchFailureIfNeeded(request, route, result);
    return result;
}

void ShellLaunchAsync(LaunchRequest request)
{
    if (request.target.empty())
        return;

    const LaunchRoute route = ResolveLaunchRoute(CurrentPrivilegeMode(), request.privilege, request.kind, request.verb);
    if (route == LaunchRoute::BlockedMismatch) {
        Log(L"launch: blocked medium-only async launch of '%s' under current privilege mode", request.target.c_str());
        return;
    }

    if (route == LaunchRoute::DirectRunAs) {
        auto job = std::make_unique<QueuedLaunch>(QueuedLaunch{std::move(request), route, nullptr});
        if (!TrySubmitThreadpoolCallback(RunAsThreadpoolProc, job.get(), nullptr)) {
            Log(L"launch: threadpool dispatch failed (%lu)", GetLastError());
            return;
        }
        [[maybe_unused]] QueuedLaunch* raw = job.release();
        return;
    }

    LauncherInstance().Enqueue(QueuedLaunch{std::move(request), route, nullptr});
}

} // namespace atrium
