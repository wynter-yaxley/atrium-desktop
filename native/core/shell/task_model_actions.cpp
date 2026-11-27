// TaskModel launch, activation, window restore, termination, and Windows pin import actions.

#include "shell/task_model_impl.h"
#include "shell/messengers.h"
#include "shell/task_rules.h"

#include "log.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

namespace {

TaskModel::WindowStep g_minimizing, g_restoring; // SetWindowSteps

constexpr UINT_PTR kRestoreTimerId = 0xA702;
constexpr UINT kRestorePollMs = 50, kRestoreWaitMs = 2000;

struct ComScope {
    bool owned;
    explicit ComScope(DWORD flags = COINIT_APARTMENTTHREADED) : owned(SUCCEEDED(CoInitializeEx(nullptr, flags))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (owned)
            CoUninitialize();
    }
};

} // namespace

void TaskModel::Impl::Launch(const std::wstring& key)
{
    const TaskItem* task = Find(key);
    if (task && !task->multipleWindows) {
        for (HWND window : task->windows)
            if (IsWindowVisible(window)) {
                BringToFront(window);
                return;
            }
        auto m = meta.find(key);
        if (m != meta.end() && openExisting && Lower(PathFindFileNameW(m->second.source.exePath.c_str())) != L"explorer.exe" &&
            openExisting(m->second.source.exePath)) {
            // Its tray icon was clicked (an app running in the background). Most open their
            // window then; one that does not within a moment is started as its pin starts it
            // (a single-instance app then shows its window) instead of the click doing nothing (#15).
            // Never a chat app: WeChat, QQ and the like run several accounts side by side, and
            // starting one again opens another one's login window.
            if (auto plan = PlanFor(key); plan && MessengerOfProgram(m->second.source.exePath) == Messenger::None)
                fallback->Queue(std::move(plan), m->second.source.exePath);
            return;
        }
        // A chat app running in the tray whose icon could not be clicked (not listed yet, the
        // tray not hosted): started again, it would open another account's login window.
        if (m != meta.end() && MessengerOfProgram(m->second.source.exePath) != Messenger::None && detail::ProcessRuns(m->second.source.exePath)) {
            Log(L"tasks: %s runs, its tray icon not reached: not started again", PathFindFileNameW(m->second.source.exePath.c_str()));
            return;
        }
    }
    if (auto plan = PlanFor(key)) {
        auto m = meta.find(key);
        const std::wstring exe = m != meta.end() ? m->second.source.exePath : std::wstring{};
        const bool wasRunning = !exe.empty() && detail::ProcessRuns(exe);
        if (detail::ExecTaskLaunch(plan->file, plan->args, plan->dir) && wasRunning && MessengerOfProgram(exe) == Messenger::None)
            fallback->Queue(std::nullopt, exe);
    }
}

// How `key` is started: its pin's shortcut or program (a packaged program by its app id),
// else the running app's program or app id.
std::optional<detail::LaunchPlan> TaskModel::Impl::PlanFor(const std::wstring& key)
{
    auto pit = pinByKey.find(key);
    size_t pinIndex = pit != pinByKey.end() ? pit->second : pins.size();
    if (pinIndex == pins.size()) // duplicate pins carry a suffixed key
        for (size_t i = 0; i < pins.size(); ++i)
            if (pins[i].key == key)
                pinIndex = i;
    if (pinIndex < pins.size())
        return detail::BuildLaunchPlan(&pins[pinIndex].app, {}, {}, key);
    auto mit = meta.find(key);
    if (mit == meta.end()) {
        Log(L"tasks: launch of unknown item %s", key.c_str());
        return std::nullopt;
    }
    return detail::BuildLaunchPlan(nullptr, mit->second.source.aumid, mit->second.source.exePath, key);
}

void TaskModel::Impl::Minimize(HWND h)
{
    if (!g_minimizing || !g_minimizing(h, [h] { detail::SendTaskSysCommand(h, SC_MINIMIZE); }))
        detail::SendTaskSysCommand(h, SC_MINIMIZE);
}

void TaskModel::Impl::BringToFront(HWND h, bool allowStep)
{
    // A closed-to-tray window must be restored by its app, not by the task switcher.
    if (!IsWindowVisible(h))
        return;
    if (allowStep && IsIconic(h) && g_restoring && g_restoring(h, [h] { Raise(h); }))
        return;
    Raise(h);
}

void TaskModel::Impl::DeferActivate(HWND h)
{
    std::erase_if(pendingRestores, [h](const PendingRestore& p) { return p.window == h; });
    pendingRestores.push_back({h, GetTickCount64() + kRestoreWaitMs});
    ProcessPendingRestores();
}

void TaskModel::Impl::ProcessPendingRestores()
{
    const auto ready = TakeRestored(pendingRestores, GetTickCount64(),
                                    [](HWND h) { return ClassifyWindowShow(IsWindow(h) != FALSE, IsWindowVisible(h) != FALSE, IsIconic(h) != FALSE); });
    if (pendingRestores.empty() && restoreTimerArmed) {
        KillTimer(ui, kRestoreTimerId);
        restoreTimerArmed = false;
    } else if (!pendingRestores.empty() && !restoreTimerArmed && SetTimer(ui, kRestoreTimerId, kRestorePollMs, nullptr)) {
        restoreTimerArmed = true;
    }
    for (HWND h : ready)
        Raise(h);
}

void TaskModel::Impl::Raise(HWND h)
{
    Impl* self = g_self;
    detail::RaiseTaskWindow(
        h, self ? self->ui : nullptr, self ? self->restoreMsg : 0,
        [self](HWND target) {
            if (self)
                self->DeferActivate(target);
        },
        [self](HWND target) { return self && std::ranges::any_of(self->pendingRestores, [target](const PendingRestore& p) { return p.window == target; }); });
}

void TaskModel::Impl::Activate(const std::wstring& key, HMONITOR monitor)
{
    const TaskItem* t = Find(key);
    auto windows = t ? t->windows : std::vector<HWND>{};
    std::erase_if(windows, [](HWND h) { return !IsWindowVisible(h); });
    FilterWindowsToMonitor(windows, monitor);
    if (windows.empty()) {
        Launch(key);
        return;
    }
    HWND fg = ForegroundRoot();
    bool inGroup = fg && std::ranges::find(windows, fg) != windows.end();
    if (windows.size() == 1) {
        HWND w = windows[0];
        if (inGroup && !IsIconic(w))
            Minimize(w);
        else
            BringToFront(w);
        return;
    }
    // Windows are most-recently-active first: the last one is the next in turn.
    BringToFront(inGroup ? windows.back() : windows.front());
}

std::vector<AppProcessIdentity> TaskModel::Impl::TerminationTargets(const std::wstring& key) const
{
    const auto found = meta.find(key);
    if (found == meta.end())
        return {};
    if (!found->second.processTargets.empty())
        return found->second.processTargets;
    // Pinned apps can keep running in the tray after their last window closes.
    return {{found->second.source.exePath, found->second.source.aumid}};
}

std::wstring TaskModel::Impl::PinTarget(const std::wstring& key) const
{
    auto m = meta.find(key);
    return m == meta.end() ? std::wstring{} : m->second.source.pinPath;
}

PinnedApp TaskModel::Impl::PinFor(const std::wstring& key) const
{
    const TaskItem* t = Find(key);
    auto m = meta.find(key);
    return (!t || m == meta.end()) ? PinnedApp{} : PinnedApp{t->name, m->second.source.exePath, {}, m->second.source.aumid};
}

// Exe that starts the item, unless it is a packaged app (those are only
// started through their AUMID).
std::wstring TaskModel::Impl::DesktopExe(const std::wstring& key) const
{
    auto m = meta.find(key);
    return (m == meta.end() || m->second.source.exePath.empty() || detail::IsPackagedPath(m->second.source.exePath)) ? std::wstring() :
                                                                                                                       m->second.source.exePath;
}

std::wstring TaskModel::Impl::Aumid(const std::wstring& key) const
{
    auto m = meta.find(key);
    return m == meta.end() ? std::wstring() : m->second.source.aumid;
}

bool TaskModel::Impl::Show(const std::wstring& key, DWORD pid)
{
    const TaskItem* t = Find(key);
    if (!t || t->windows.empty())
        return false;
    if (pid == 0)
        return t->active ? true : (BringToFront(t->windows.front()), true);
    for (DWORD winPid = 0; HWND h : t->windows)
        if (GetWindowThreadProcessId(h, &winPid) && winPid == pid)
            return BringToFront(h), true;
    return false;
}

void TaskModel::Impl::MoveRunning(const std::wstring& key, int to)
{
    auto it = std::ranges::find(order, key);
    if (it == order.end())
        return;
    order.erase(it);
    order.insert(order.begin() + std::clamp<std::ptrdiff_t>(to, 0, static_cast<std::ptrdiff_t>(order.size())), key);
    if (Rebuild()) {
        notify = true;
        ArmTimer();
    }
}

bool TaskModel::Impl::OpenFiles(const std::wstring& key, const std::vector<std::wstring>& files) const
{
    return detail::OpenFilesWithTaskApp(DesktopExe(key), Aumid(key), files);
}

bool AllowsMultipleWindows(const PinnedApp& app)
{
    if (app.multipleWindows)
        return *app.multipleWindows;
    std::wstring exe = Lower(FileStem(app.path));
    return exe == L"explorer" || Lower(app.aumid) == L"microsoft.windows.explorer" || exe == L"chrome" || exe == L"msedge" || exe == L"firefox" ||
           exe == L"brave" || exe == L"opera" || exe == L"vivaldi" || exe == L"chromium";
}

void TaskModel::ActivateWindow(HWND window)
{
    if (IsWindow(window))
        Impl::BringToFront(window);
}

void TaskModel::ActivateWindowDirect(HWND window)
{
    if (IsWindow(window))
        Impl::BringToFront(window, false);
}

void TaskModel::SetWindowSteps(WindowStep minimizing, WindowStep restoring)
{
    g_minimizing = std::move(minimizing);
    g_restoring = std::move(restoring);
}

bool TaskModel::CloseWindow(HWND window)
{
    if (!IsWindow(window)) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    if (!PostMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0)) {
        const DWORD error = GetLastError();
        Log(L"tasks: window close request failed for %p (%lu)", window, error);
        return false;
    }
    return true;
}

std::vector<PinnedApp> TaskModel::ImportWindowsPins()
{
    std::vector<PinnedApp> pins;
    ComScope com;
    PWSTR base = nullptr;
    const std::wstring dir = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)) && base ?
                                 std::wstring(base) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar" :
                                 std::wstring{};
    CoTaskMemFree(base);
    WIN32_FIND_DATAW fd{};
    HANDLE find = !dir.empty() ? FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd) : INVALID_HANDLE_VALUE;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                PinnedApp p{fd.cFileName, dir + L"\\" + fd.cFileName};
                if (p.name.size() > 4)
                    p.name.resize(p.name.size() - 4);
                std::wstring target;
                detail::ReadTaskLink(p.path, target, p.aumid);
                pins.push_back(std::move(p));
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return pins;
}

} // namespace atrium
