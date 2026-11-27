// Task launch, single-instance fallback, window activation, and file opening helpers.
#include "shell/task_model.h"
#include "shell/shell_icon.h"
#include "shell/shell_launch.h"
#include "shell/task_rules.h"

#include "log.h"

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <winrt/base.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace atrium::detail {

namespace {

// Quotes a single command-line argument according to CommandLineToArgvW rules.
std::wstring QuoteArg(const std::wstring& arg)
{
    std::wstring out(1, L'"');
    size_t bs = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++bs;
        } else {
            out.append(ch == L'"' ? bs * 2 + 1 : bs, L'\\');
            out.push_back(ch);
            bs = 0;
        }
    }
    return out.append(bs * 2, L'\\') + L'"';
}

bool IsAsciiWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string XmlAttr(std::string_view tag, std::string_view ltag, std::string_view name)
{
    for (size_t a = ltag.find(name); a != std::string_view::npos; a = ltag.find(name, a + 1)) {
        if (!a || !IsAsciiWs(ltag[a - 1]))
            continue;
        size_t v = a + name.size();
        while (v < ltag.size() && IsAsciiWs(ltag[v]))
            ++v;
        if (v >= ltag.size() || ltag[v] != '=')
            continue;
        ++v;
        while (v < ltag.size() && IsAsciiWs(ltag[v]))
            ++v;
        if (v < ltag.size() && (ltag[v] == '"' || ltag[v] == '\'')) {
            const size_t q = tag.find(ltag[v], v + 1);
            return q == std::string_view::npos ? std::string() : std::string(tag.substr(v + 1, q - v - 1));
        }
    }
    return {};
}

// Whether a program shows a window of its own (any visible, unowned, ordinary one).
bool ShowsAppWindow(const std::wstring& exe)
{
    struct Look {
        const std::wstring* exe;
        bool found = false;
    } look{&exe};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto& k = *reinterpret_cast<Look*>(l);
            if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t path[MAX_PATH * 2];
                DWORD size = DWORD(std::size(path));
                k.found = QueryFullProcessImageNameW(p, 0, path, &size) && _wcsicmp(path, k.exe->c_str()) == 0;
                CloseHandle(p);
            }
            return !k.found;
        },
        reinterpret_cast<LPARAM>(&look));
    return look.found;
}

HWND FindHiddenMainWindowForExe(const std::wstring& exe)
{
    struct Look {
        const std::wstring* exe;
        std::vector<HWND> hwnds;
        std::vector<HiddenWindow> candidates;
    } look{&exe};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto& k = *reinterpret_cast<Look*>(l);
            const DWORD ex = static_cast<DWORD>(GetWindowLongW(h, GWL_EXSTYLE));
            const bool owned = GetWindow(h, GW_OWNER) != nullptr;
            const bool visible = IsWindowVisible(h) != FALSE;
            const bool enabled = IsWindowEnabled(h) != FALSE;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            wchar_t cls[128] = {};
            GetClassNameW(h, cls, ARRAYSIZE(cls));
            wchar_t title[512] = {};
            InternalGetWindowText(h, title, ARRAYSIZE(title));
            if (!IsHiddenMainWindowCandidate(visible, owned, ex, enabled, cloaked != 0, title, cls))
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            winrt::handle p(pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr);
            RECT r{};
            if (!p || _wcsicmp(TaskProcessImagePath(p.get()).c_str(), k.exe->c_str()) != 0 || !GetWindowRect(h, &r))
                return TRUE;
            k.hwnds.push_back(h);
            k.candidates.push_back(
                {r, static_cast<DWORD>(GetWindowLongW(h, GWL_STYLE)), ex, owned, enabled, cloaked != 0, title[0] != L'\0', IsSystemHelperWindowClass(cls)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&look));
    const int index = PickHiddenMainWindow(look.candidates);
    return index >= 0 ? look.hwnds[size_t(index)] : nullptr;
}

class FallbackRunnerImpl final : public TaskFallbackRunner {
public:
    FallbackRunnerImpl(HWND ui, UINT restoreMsg) : ui_(ui), restoreMsg_(restoreMsg), thread_([this] { Main(); }) {}

    ~FallbackRunnerImpl() override
    {
        {
            std::lock_guard g(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    FallbackRunnerImpl(const FallbackRunnerImpl&) = delete;
    FallbackRunnerImpl& operator=(const FallbackRunnerImpl&) = delete;

    void Queue(std::optional<LaunchPlan> plan, std::wstring exe) override
    {
        {
            std::lock_guard g(mutex_);
            ++gen_;
            pending_.emplace(FallbackJob{std::move(plan), std::move(exe)});
        }
        cv_.notify_all();
    }

private:
    struct FallbackJob {
        std::optional<LaunchPlan> plan;
        std::wstring exe;
    };

    bool WaitForWindow(std::unique_lock<std::mutex>& lk, const std::wstring& exe, uint64_t gen, int polls)
    {
        for (int i = 0; i < polls; ++i) {
            if (cv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return stop_ || gen_ != gen; }))
                return true;
            lk.unlock();
            const bool shown = ShowsAppWindow(exe);
            lk.lock();
            if (shown || stop_ || gen_ != gen)
                return true;
        }
        return false;
    }

    void Main()
    {
        for (std::unique_lock lk(mutex_);;) {
            cv_.wait(lk, [&] { return stop_ || pending_.has_value(); });
            if (stop_)
                return;
            const FallbackJob job = std::move(*pending_);
            const uint64_t gen = gen_;
            pending_.reset();
            if (job.plan) {
                if (WaitForWindow(lk, job.exe, gen, 30))
                    continue;
                lk.unlock();
                Log(L"tasks: %s showed no window from its tray icon: started", PathFindFileNameW(job.exe.c_str()));
                const bool started = ExecTaskLaunch(job.plan->file, job.plan->args, job.plan->dir, true);
                lk.lock();
                if (!started || stop_ || gen_ != gen)
                    continue;
            }
            if (WaitForWindow(lk, job.exe, gen, 15))
                continue;
            lk.unlock();
            if (HWND h = FindHiddenMainWindowForExe(job.exe)) {
                Log(L"tasks: %s showed no window after launch: restoring hidden window", PathFindFileNameW(job.exe.c_str()));
                ShowWindowAsync(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
                if (restoreMsg_)
                    PostMessageW(ui_, restoreMsg_, 0, reinterpret_cast<LPARAM>(h));
            }
            lk.lock();
        }
    }

    HWND ui_;
    UINT restoreMsg_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<FallbackJob> pending_;
    uint64_t gen_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace

std::unique_ptr<TaskFallbackRunner> CreateFallbackRunner(HWND ui, UINT restoreMsg) { return std::make_unique<FallbackRunnerImpl>(ui, restoreMsg); }

std::wstring AppsFolderPath(const std::wstring& aumid) { return L"shell:AppsFolder\\" + aumid; }

bool IsPackagedPath(const std::wstring& path) { return Lower(path).find(L"\\windowsapps\\") != std::wstring::npos; }

std::wstring PackagedAumidForExe(const std::wstring& exe)
{
    const size_t at = Lower(exe).find(L"\\windowsapps\\");
    const size_t start = at == std::wstring::npos ? std::wstring::npos : at + 13;
    const size_t end = start == std::wstring::npos ? std::wstring::npos : exe.find(L'\\', start);
    if (end == std::wstring::npos)
        return {};
    const std::wstring folder = exe.substr(start, end - start);
    const size_t first = folder.find(L'_'), last = folder.rfind(L'_');
    if (first == std::wstring::npos || last == first)
        return {};
    const std::wstring family = folder.substr(0, first) + L"_" + folder.substr(last + 1);
    HANDLE file = CreateFileW((exe.substr(0, end) + L"\\AppxManifest.xml").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::string xml(size_t(std::min<DWORD>(GetFileSize(file, nullptr), 1u << 20)), '\0');
    DWORD got = 0;
    ReadFile(file, xml.data(), DWORD(xml.size()), &got, nullptr);
    CloseHandle(file);
    xml.resize(got);
    auto lowerA = [](std::string s) {
        for (char& ch : s)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string lx = lowerA(xml), lrel = lowerA(winrt::to_string(exe.substr(end + 1)));
    std::string id;
    for (size_t p = lx.find("<application"); p != std::string::npos; p = lx.find("<application", p + 12)) {
        if (p + 12 >= lx.size() || !IsAsciiWs(lx[p + 12]))
            continue;
        const size_t close = lx.find('>', p);
        if (close == std::string::npos)
            break;
        const std::string_view tag(xml.data() + p, close - p), ltag(lx.data() + p, close - p);
        const std::string executable = lowerA(XmlAttr(tag, ltag, "executable")), appId = XmlAttr(tag, ltag, "id");
        if (id.empty())
            id = appId;
        if (!executable.empty() && executable == lrel) {
            id = appId;
            break;
        }
    }
    return id.empty() ? std::wstring() : family + L"!" + std::wstring(winrt::to_hstring(id));
}

std::wstring TaskProcessImagePath(HANDLE process)
{
    std::wstring buf(512, L'\0');
    for (;;) {
        DWORD n = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(process, 0, buf.data(), &n)) {
            buf.resize(n);
            return buf;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buf.size() >= 32768)
            return {};
        buf.resize(buf.size() * 2);
    }
}

bool ExecTaskLaunch(const std::wstring& file, const std::wstring& args, const std::wstring& dir, bool noAsync)
{
    LaunchRequest req;
    req.target = file;
    req.parameters = args;
    req.workingDirectory = dir;
    if (file.starts_with(L"shell:AppsFolder\\"))
        req.kind = LaunchTargetKind::AppUserModelId;
    else if (IsLink(file))
        req.kind = LaunchTargetKind::Shortcut;
    else
        req.kind = LaunchTargetKind::Executable;
    req.privilege = LaunchPrivilege::ShellDefault;
    req.noAsync = noAsync;

    const LaunchResult result = ShellLaunch(req);
    if (result.outcome != LaunchOutcome::Succeeded) {
        Log(L"tasks: launch of %s failed (%lu)", file.c_str(), result.error);
        return false;
    }
    Log(L"tasks: started %s", file.c_str());
    return true;
}

bool ProcessRuns(const std::wstring& exe)
{
    const wchar_t* name = PathFindFileNameW(exe.c_str());
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) != 0)
            continue;
        winrt::handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID));
        found = p && _wcsicmp(TaskProcessImagePath(p.get()).c_str(), exe.c_str()) == 0;
    }
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
    return found;
}

std::optional<LaunchPlan> BuildLaunchPlan(const PinnedApp* pin, const std::wstring& aumid, const std::wstring& exePath, const std::wstring& key)
{
    if (pin) {
        if (!pin->path.empty() && IsPackagedPath(pin->path) && !IsLink(pin->path)) {
            const std::wstring id = !pin->aumid.empty() ? pin->aumid : PackagedAumidForExe(pin->path);
            if (!id.empty())
                return LaunchPlan{AppsFolderPath(id), {}, {}};
            Log(L"tasks: pin %s is a packaged program without an app id", pin->name.c_str());
        }
        if (!pin->path.empty()) {
            const bool link = IsLink(pin->path);
            const size_t slash = link ? std::wstring::npos : pin->path.find_last_of(L"\\/");
            return LaunchPlan{pin->path, link ? std::wstring() : pin->args, slash == std::wstring::npos ? std::wstring() : pin->path.substr(0, slash)};
        }
        if (!pin->aumid.empty())
            return LaunchPlan{AppsFolderPath(pin->aumid), {}, {}};
        Log(L"tasks: pin %s has nothing to launch", pin->name.c_str());
        return std::nullopt;
    }
    const bool packaged = IsPackagedPath(exePath);
    if (!exePath.empty() && !packaged)
        return LaunchPlan{exePath, {}, {}};
    const std::wstring resolvedAumid = !aumid.empty() ? aumid : packaged ? PackagedAumidForExe(exePath) : std::wstring();
    if (!resolvedAumid.empty())
        return LaunchPlan{AppsFolderPath(resolvedAumid), {}, {}};
    Log(L"tasks: no way to start %s", key.c_str());
    return std::nullopt;
}

bool OpenFilesWithTaskApp(const std::wstring& desktopExe, const std::wstring& aumid, const std::vector<std::wstring>& files)
{
    if (files.empty())
        return false;
    if (!desktopExe.empty()) {
        std::wstring args;
        for (const auto& f : files)
            args += (args.empty() ? L"" : L" ") + QuoteArg(f);
        Log(L"tasks: opening %zu file(s) with %s", files.size(), desktopExe.c_str());
        return ExecTaskLaunch(desktopExe, args, {});
    }
    if (aumid.empty())
        return false;
    winrt::com_ptr<IApplicationActivationManager> aam;
    if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(aam.put()))))
        return false;
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& f : files) {
        PIDLIST_ABSOLUTE id = nullptr;
        if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &id, 0, nullptr)))
            ids.push_back(id);
    }
    winrt::com_ptr<IShellItemArray> array;
    bool ok = !ids.empty() && SUCCEEDED(SHCreateShellItemArrayFromIDLists(UINT(ids.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(ids.data()), array.put()));
    DWORD pid = 0;
    ok = ok && SUCCEEDED(aam->ActivateForFile(aumid.c_str(), array.get(), L"open", &pid));
    if (ok)
        Log(L"tasks: opening %zu file(s) with %s", ids.size(), aumid.c_str());
    for (auto* id : ids)
        CoTaskMemFree(id);
    if (!ok)
        Log(L"tasks: opening files with %s failed", aumid.c_str());
    return ok;
}

bool SendTaskSysCommand(HWND h, WPARAM command)
{
    DWORD_PTR ignored = 0;
    if (IsHungAppWindow(h)) {
        ShowWindowAsync(h, command == SC_RESTORE ? SW_RESTORE : SW_MINIMIZE);
        return true;
    }
    SendMessageTimeoutW(h, WM_SYSCOMMAND, command, 0, SMTO_ABORTIFHUNG, 500, &ignored);
    return false;
}

namespace {

void RestoreOffscreenWindowIfNeeded(HWND h)
{
    RECT wr{};
    if (!GetWindowRect(h, &wr) || MonitorFromRect(&wr, MONITOR_DEFAULTTONULL) != nullptr)
        return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(h, &wp))
        return;
    if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE) {
        if (IsWindowEnabled(h))
            SendTaskSysCommand(h, SC_RESTORE);
        else
            ShowWindowAsync(h, SW_RESTORE);
        if (!GetWindowRect(h, &wr) || MonitorFromRect(&wr, MONITOR_DEFAULTTONULL) != nullptr)
            return;
        if (!GetWindowPlacement(h, &wp))
            return;
    }
    std::vector<MonitorBounds> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hm, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(hm, &mi))
                reinterpret_cast<std::vector<MonitorBounds>*>(data)->push_back({mi.rcMonitor, mi.rcWork});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    MONITORINFO primaryInfo{sizeof(primaryInfo)};
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &primaryInfo))
        return;
    const OffscreenRestorePlan plan = ComputeOffscreenRestorePlan(wr, wp.rcNormalPosition, wp.flags, monitors, {primaryInfo.rcMonitor, primaryInfo.rcWork});
    if (!plan.shouldRestore)
        return;
    wp.flags |= WPF_ASYNCWINDOWPLACEMENT;
    wp.showCmd = plan.showCmd;
    wp.rcNormalPosition = plan.workspaceRect;
    if (!SetWindowPlacement(h, &wp)) {
        const DWORD err = GetLastError();
        static std::unordered_set<HWND> loggedFailed;
        if (loggedFailed.size() >= 64)
            loggedFailed.clear();
        if (loggedFailed.insert(h).second)
            Log(L"tasks: SetWindowPlacement failed for off-screen window 0x%p (%lu)", static_cast<void*>(h), err);
    }
    if (GetWindowRect(h, &wr) && MonitorFromRect(&wr, MONITOR_DEFAULTTONULL) == nullptr)
        ShowWindowAsync(h, SW_RESTORE);
}

} // namespace

void RaiseTaskWindow(HWND h, HWND ui, UINT restoreMsg, const std::function<void(HWND)>& deferActivate, const std::function<bool(HWND)>& isPending)
{
    const bool onUi = ui && GetWindowThreadProcessId(ui, nullptr) == GetCurrentThreadId();
    if (IsIconic(h)) {
        if (onUi && isPending && isPending(h)) {
            if (deferActivate)
                deferActivate(h);
            return;
        }
        const bool enabled = IsWindowEnabled(h) != FALSE;
        bool restoreToMaximized = false;
        if (!enabled) {
            WINDOWPLACEMENT wp{sizeof(wp)};
            restoreToMaximized = GetWindowPlacement(h, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED) != 0;
        }
        switch (DecideWindowRestoreMethod(true, enabled, IsHungAppWindow(h) != FALSE, restoreToMaximized)) {
        case WindowRestoreMethod::None: break;
        case WindowRestoreMethod::SysCommand: SendTaskSysCommand(h, SC_RESTORE); break;
        case WindowRestoreMethod::AsyncRestore: ShowWindowAsync(h, SW_RESTORE); break;
        case WindowRestoreMethod::AsyncShowMaximized: ShowWindowAsync(h, SW_SHOWMAXIMIZED); break;
        }
        if (IsIconic(h)) {
            if (onUi) {
                if (deferActivate)
                    deferActivate(h);
            } else if (ui && restoreMsg) {
                PostMessageW(ui, restoreMsg, 0, reinterpret_cast<LPARAM>(h));
            }
            return;
        }
    }
    RestoreOffscreenWindowIfNeeded(h);
    const bool ownerEnabled = IsWindowEnabled(h) != FALSE;
    auto candidate = [](HWND w) -> ActivationCandidate { return {w, w && IsWindowVisible(w) != FALSE, w && IsWindowEnabled(w) != FALSE}; };
    const HWND enabledPopup = ownerEnabled ? nullptr : GetWindow(h, GW_ENABLEDPOPUP);
    const HWND target = SelectActivationTarget(h, ownerEnabled, candidate(enabledPopup), candidate(GetLastActivePopup(h)));
    auto isFront = [&] {
        HWND fg = GetForegroundWindow();
        return fg == target || fg == h || (fg && GetAncestor(fg, GA_ROOTOWNER) == h);
    };
    if (isFront() || (SetForegroundWindow(target) && isFront()))
        return;
    static bool logged = false;
    if (!logged) {
        Log(L"tasks: SetForegroundWindow refused, switching with SwitchToThisWindow (logged once)");
        logged = true;
    }
    if (IsWindowVisible(target))
        SwitchToThisWindow(target, TRUE);
}

} // namespace atrium::detail
