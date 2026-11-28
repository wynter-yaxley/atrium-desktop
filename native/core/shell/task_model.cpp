// Task model implementation (D13): EnumWindows + WinEvent hooks on the UI
// thread, AUMID grouping, icons and display names loaded on one worker thread.
//
// Threading: everything except the icon worker's shared state is touched by the
// UI thread only. The worker never touches model state; it publishes results
// under its mutex and posts a registered message to the UI window.

#include "shell/task_model_impl.h"
#include "shell/app_processes.h"
#include "shell/task_rules.h"

#include "log.h"

#include <windows.h>
#include <objbase.h>

#include <winrt/base.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace atrium {

namespace {

constexpr UINT_PTR kTimerId = 0xA701;     // >= 0xA700: clear of the host's own timers
constexpr UINT_PTR kRestoreTimerId = 0xA702;
constexpr UINT kCoalesceMs = 30;

bool Same(const TaskItem& a, const TaskItem& b)
{
    return a.key == b.key && a.name == b.name && a.pinned == b.pinned && a.pinIndex == b.pinIndex && a.windows == b.windows && a.active == b.active &&
           a.attention == b.attention && a.badge == b.badge && a.iconVersion == b.iconVersion;
}

} // namespace

TaskModel::Impl* TaskModel::Impl::g_self = nullptr;

TaskModel::Impl::Impl(HWND window, std::function<void()> changed, std::function<bool(const std::wstring&)> opener)
    : ui(window),
      onChanged(std::move(changed)),
      openExisting(std::move(opener)),
      comOwned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))),
      shellHookMsg(RegisterWindowMessageW(L"SHELLHOOK")),
      readyMsg(RegisterWindowMessageW(L"Atrium.TaskModel.Ready")),
      restoreMsg(RegisterWindowMessageW(L"Atrium.TaskModel.PendingRestore")),
      shellHooked(RegisterShellHookWindow(window) != FALSE)
{
    g_self = this;
    if (!shellHooked)
        Log(L"tasks: RegisterShellHookWindow failed (%lu); no flash notifications", GetLastError());
    iconWorker = std::make_unique<detail::TaskIconWorker>(window, readyMsg);
    fallback = detail::CreateFallbackRunner(window, restoreMsg);
    struct Range {
        DWORD lo, hi;
    };
    const Range ranges[] = {{EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE},
                            {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE},
                            {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
                            {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                            {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND}};
    for (const Range& r : ranges) {
        HWINEVENTHOOK h = SetWinEventHook(r.lo, r.hi, nullptr, &Impl::WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h)
            hooks.push_back(h);
        else
            Log(L"tasks: SetWinEventHook(0x%X-0x%X) failed (%lu)", r.lo, r.hi, GetLastError());
    }
    Rescan();
    Rebuild();
    ArmTimer(); // catches events that raced the initial scan
}

TaskModel::Impl::~Impl()
{
    for (HWINEVENTHOOK h : hooks)
        UnhookWinEvent(h);
    if (shellHooked)
        DeregisterShellHookWindow(ui);
    if (timerArmed)
        KillTimer(ui, kTimerId);
    if (restoreTimerArmed)
        KillTimer(ui, kRestoreTimerId);
    fallback.reset();
    iconWorker.reset();
    if (comOwned)
        CoUninitialize();
    g_self = nullptr;
}

void CALLBACK TaskModel::Impl::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (!g_self || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
        return;
    g_self->OnEvent(event, hwnd);
}

void TaskModel::Impl::OnEvent(DWORD event, HWND hwnd)
{
    if (!pendingRestores.empty() && (event == EVENT_SYSTEM_MINIMIZEEND || event == EVENT_SYSTEM_FOREGROUND || event == EVENT_OBJECT_SHOW))
        ProcessPendingRestores();
    if (event == EVENT_SYSTEM_FOREGROUND) {
        NoteForeground(hwnd);
    } else if (event == EVENT_OBJECT_DESTROY) {
        if (!infos.contains(hwnd))
            return; // not something we show
    } else if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) {
        return; // child window
    }
    if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED || event == EVENT_OBJECT_CREATE)
        infos.erase(hwnd); // identity (AUMID) may have been set meanwhile
    MarkDirty();
}

void TaskModel::Impl::NoteForeground(HWND fg)
{
    if (!fg)
        return;
    HWND root = GetAncestor(fg, GA_ROOTOWNER);
    lastActive[fg] = ++seq;
    attention.erase(fg);
    // The app in front: also a window of it that flashed hidden (in the tray) has been answered.
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid)
        std::erase_if(attention, [&](HWND h) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            return pid == fgPid;
        });
    if (root && root != fg) {
        lastActive[root] = ++seq;
        attention.erase(root);
    }
    lastFg = root ? root : fg;
}

void TaskModel::Impl::MarkDirty()
{
    dirty = true;
    ArmTimer();
}

void TaskModel::Impl::ArmTimer()
{
    if (!timerArmed && SetTimer(ui, kTimerId, kCoalesceMs, nullptr))
        timerArmed = true;
}

bool TaskModel::Impl::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == kTimerId) {
        KillTimer(ui, kTimerId);
        timerArmed = false;
        Tick();
        return true;
    }
    if (msg == WM_TIMER && wp == kRestoreTimerId) {
        ProcessPendingRestores();
        return true;
    }
    if (shellHookMsg && msg == shellHookMsg) {
        OnShellHook(wp, reinterpret_cast<HWND>(lp));
        return true;
    }
    if (readyMsg && msg == readyMsg) {
        OnReady();
        return true;
    }
    if (restoreMsg && msg == restoreMsg) {
        DeferActivate(reinterpret_cast<HWND>(lp));
        return true;
    }
    return false;
}

void TaskModel::Impl::OnShellHook(WPARAM code, HWND hwnd)
{
    if (code == HSHELL_FLASH && hwnd) {
        HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
        root = root ? root : hwnd;
        HWND fg = GetForegroundWindow();
        if (root != fg && root != GetAncestor(fg, GA_ROOTOWNER)) {
            attention.insert(root);
            if (onFlash)
                onFlash(root);
        }
    }
    MarkDirty(); // window created/destroyed/activated/redrawn
}

void TaskModel::Impl::OnReady()
{
    std::vector<std::wstring> doneIcons;
    std::vector<std::pair<std::wstring, std::wstring>> doneNames;
    iconWorker->TakeReady(doneIcons, doneNames);
    for (const auto& key : doneIcons)
        if (meta.contains(key))
            ++iconVersion[key];
    for (const auto& [key, name] : doneNames)
        if (meta.contains(key))
            names[key] = name;
    if (Rebuild()) {
        notify = true;
        ArmTimer();
    }
}

void TaskModel::Impl::Tick()
{
    bool changed = false;
    if (dirty) {
        dirty = false;
        Rescan();
        changed = Rebuild();
    }
    if (changed || notify) {
        notify = false;
        if (onChanged)
            onChanged();
    }
}

HWND TaskModel::Impl::ForegroundRoot() const
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return nullptr;
    HWND root = GetAncestor(fg, GA_ROOTOWNER);
    return root ? root : fg;
}

BOOL CALLBACK TaskModel::Impl::CollectWindow(HWND h, LPARAM lp)
{
    reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
    return TRUE;
}

void TaskModel::Impl::Rescan()
{
    std::vector<HWND> all;
    EnumWindows(&Impl::CollectWindow, reinterpret_cast<LPARAM>(&all));
    scan.clear();
    std::unordered_set<HWND> seen;
    int z = 0;
    for (HWND h : all) {
        ++z;
        DWORD pid = 0;
        wchar_t cls[256] = {};
        std::wstring title;
        if (!detail::IsTaskAppWindow(h, pid, cls, ARRAYSIZE(cls), title))
            continue;
        auto it = infos.find(h);
        if (it == infos.end() || it->second.pid != pid)
            it = infos.insert_or_assign(h, detail::ComputeTaskWinInfo(h, pid, cls)).first;
        seen.insert(h);
        scan.push_back(TaskScanRec{h, z, std::move(title), it->second});
    }
    std::erase_if(infos, [&](const auto& kv) { return !seen.contains(kv.first); });
    std::erase_if(lastActive, [](const auto& kv) { return !IsWindow(kv.first); });
    std::erase_if(attention, [](HWND h) { return !IsWindow(h); });
    // Foreground changes normally arrive as events; this covers a missed one.
    HWND fg = ForegroundRoot();
    if (fg && fg != lastFg)
        NoteForeground(fg);
}

// Pin that a window with this identity belongs to, or -1.
int TaskModel::Impl::MatchPin(const detail::TaskWinInfo& info) const
{
    auto it = pinByKey.find(info.key);
    if (it != pinByKey.end())
        return static_cast<int>(it->second);
    // Desktop launchers can give the actual window a different identity or executable.
    // Exact AUMIDs still win; an executable shared by several pins is ambiguous. A web app
    // is not its browser.
    if (!info.exePath.empty() && !info.webApp) {
        auto jt = pinByTarget.find(Lower(info.exePath));
        if (jt != pinByTarget.end() && jt->second < pins.size())
            return static_cast<int>(jt->second);
    }
    // A window without an identity of its own, of a program its pinned launcher started.
    if (!info.launcher.empty()) {
        auto jt = pinByTarget.find(Lower(info.launcher));
        if (jt != pinByTarget.end() && jt->second < pins.size())
            return static_cast<int>(jt->second);
    }
    // Versioned subfolder executables opened directly (e.g. WPS opening a .docx bypasses ksolaunch.exe).
    if (!info.exePath.empty() && !info.webApp) {
        int matched = -1;
        for (size_t i = 0; i < pins.size(); ++i) {
            if (pins[i].specific)
                continue;
            if (PinMatchesSubdirLauncherExe(pins[i].exe, info.exePath) || PinMatchesSubdirLauncherExe(pins[i].target, info.exePath)) {
                if (matched >= 0)
                    return -1; // ambiguous across multiple pins
                matched = static_cast<int>(i);
            }
        }
        if (matched >= 0)
            return matched;
    }
    return -1;
}

void TaskModel::Impl::ApplyHiddenAttention(std::vector<TaskItem>& out, const std::unordered_map<std::wstring, TaskMeta>& newMeta) const
{
    for (HWND h : attention) {
        if (std::ranges::any_of(scan, [&](const TaskScanRec& r) { return r.hwnd == h; }))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        winrt::handle proc(pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr);
        const std::wstring exe = proc ? detail::TaskProcessImagePath(proc.get()) : std::wstring();
        if (exe.empty())
            continue;
        for (TaskItem& t : out)
            if (const TaskMeta& m = newMeta.at(t.key); !m.source.exePath.empty() && _wcsicmp(m.source.exePath.c_str(), exe.c_str()) == 0) {
                t.attention = true;
                break;
            }
    }
}

void TaskModel::Impl::FinalizeItemMetadata(std::vector<TaskItem>& result, const std::unordered_map<std::wstring, TaskMeta>& newMeta,
                                           const std::unordered_map<std::wstring, std::wstring>& firstTitle)
{
    for (TaskItem& t : result) {
        const TaskMeta& m = newMeta.at(t.key);
        if (!t.pinned)
            t.multipleWindows = !m.webApp && AllowsMultipleWindows(PinnedApp{t.name, m.source.exePath, {}, m.source.aumid});
        if (t.name.empty()) {
            auto n = names.find(t.key);
            if (n != names.end()) {
                t.name = n->second;
            } else {
                t.name = !m.source.relaunchName.empty() ? m.source.relaunchName :
                         !m.source.exePath.empty()      ? FileStem(m.source.exePath) :
                         !m.source.pinPath.empty()      ? FileStem(m.source.pinPath) :
                                                          firstTitle.at(t.key);
                if (t.name.empty())
                    t.name = m.source.aumid;
            }
        }
        auto v = iconVersion.find(t.key);
        t.iconVersion = v == iconVersion.end() ? 0 : v->second;
        const bool pinNamed = t.pinned && !pins[static_cast<size_t>(t.pinIndex)].app.name.empty();
        if (!pinNamed && !names.contains(t.key) && (!m.source.aumid.empty() || !m.source.exePath.empty()) && namesRequested.insert(t.key).second)
            iconWorker->RequestName(t.key, m.source);
    }
}

void TaskModel::Impl::SeedPinnedItems(std::vector<TaskItem>& out, std::unordered_map<std::wstring, TaskMeta>& newMeta,
                                      std::unordered_map<std::wstring, size_t>& at) const
{
    for (size_t i = 0; i < pins.size(); ++i) {
        const detail::TaskPin& p = pins[i];
        TaskItem t;
        t.key = p.key;
        t.name = p.app.name;
        t.pinned = true;
        t.pinIndex = static_cast<int>(i);
        t.multipleWindows = p.multipleWindows;
        at[p.key] = out.size();
        out.push_back(std::move(t));
        TaskMeta& m = newMeta[p.key];
        m.webApp = p.webApp;
        m.source.aumid = p.aumid;
        m.source.pinPath = p.app.path;
        m.source.exePath = p.exe;
        m.source.webApp = p.webApp;
    }
}

// Most recently active first; never-activated windows keep z-order.
std::vector<const TaskScanRec*> TaskModel::Impl::SortScanByActivity() const
{
    std::vector<const TaskScanRec*> sorted;
    sorted.reserve(scan.size());
    for (const TaskScanRec& r : scan)
        sorted.push_back(&r);
    auto activity = [&](HWND h) {
        auto it = lastActive.find(h);
        return it == lastActive.end() ? uint64_t(0) : it->second;
    };
    std::ranges::stable_sort(sorted, [&](const TaskScanRec* a, const TaskScanRec* b) {
        const uint64_t x = activity(a->hwnd), y = activity(b->hwnd);
        return x != y ? x > y : a->z < b->z;
    });
    return sorted;
}

void TaskModel::Impl::RecordWindowInGroup(const TaskScanRec& r, size_t pinCount, HWND fg, std::vector<TaskItem>& out,
                                          std::unordered_map<std::wstring, TaskMeta>& newMeta, std::unordered_map<std::wstring, size_t>& at,
                                          std::unordered_map<std::wstring, ULONGLONG>& firstSeen, std::unordered_map<std::wstring, std::wstring>& firstTitle,
                                          std::unordered_set<std::wstring>& running) const
{
    const int pin = MatchPin(r.info);
    const std::wstring& key = pin >= 0 ? pins[static_cast<size_t>(pin)].key : r.info.key;
    auto it = at.find(key);
    if (it == at.end()) {
        TaskItem t;
        t.key = key;
        it = at.emplace(key, out.size()).first;
        out.push_back(std::move(t));
        firstTitle[key] = r.title;
    }
    TaskItem& t = out[it->second];
    t.windows.push_back(r.hwnd);
    if (fg && (r.hwnd == fg || GetAncestor(r.hwnd, GA_ROOTOWNER) == fg))
        t.active = true;
    if (attention.contains(r.hwnd))
        t.attention = true;
    t.badge = std::max(t.badge, ParseTitleBadge(r.title, r.info.webApp, r.info.exePath, r.info.aumid));
    TaskMeta& m = newMeta[key];
    if (r.info.webApp) {
        m.webApp = true;
        m.source.webApp = true;
    }
    if (m.source.exePath.empty())
        m.source.exePath = r.info.exePath;
    if (m.source.aumid.empty())
        m.source.aumid = r.info.aumid;
    if (m.relaunchCommand.empty() && !r.info.relaunchCommand.empty())
        m.relaunchCommand = r.info.relaunchCommand;
    if (m.source.relaunchIcon.empty() && !r.info.relaunchIcon.empty())
        m.source.relaunchIcon = r.info.relaunchIcon;
    if (m.source.relaunchName.empty() && !r.info.relaunchName.empty())
        m.source.relaunchName = r.info.relaunchName;
    m.source.hwnds.push_back(r.hwnd);
    if (std::ranges::none_of(m.processTargets,
                             [&](const AppProcessIdentity& target) { return target.exePath == r.info.exePath && target.aumid == r.info.aumid; }))
        m.processTargets.push_back({r.info.exePath, r.info.aumid});
    if (it->second >= pinCount) {
        running.insert(key);
        auto f = firstSeen.find(key);
        if (f == firstSeen.end() || (r.info.created && r.info.created < f->second))
            firstSeen[key] = r.info.created;
    }
}

// A shortcut may point to a launcher; retain the actual app identity after it hides.
void TaskModel::Impl::RetainHiddenProcessTargets(std::unordered_map<std::wstring, TaskMeta>& newMeta) const
{
    for (auto& [key, value] : newMeta) {
        const auto previous = meta.find(key);
        if (value.processTargets.empty() && previous != meta.end())
            value.processTargets = previous->second.processTargets;
    }
}

// Unpinned groups: keep first-seen order, append new ones by process start.
std::vector<TaskItem> TaskModel::Impl::AssembleOrderedItems(std::vector<TaskItem>& out, std::unordered_map<std::wstring, size_t>& at,
                                                            const std::unordered_set<std::wstring>& running,
                                                            std::unordered_map<std::wstring, ULONGLONG>& firstSeen, size_t pinCount)
{
    std::erase_if(order, [&](const std::wstring& k) { return !running.contains(k); });
    std::vector<std::wstring> fresh;
    for (const auto& k : running)
        if (std::ranges::find(order, k) == order.end())
            fresh.push_back(k);
    std::ranges::sort(fresh, [&](const std::wstring& a, const std::wstring& b) { return firstSeen[a] != firstSeen[b] ? firstSeen[a] < firstSeen[b] : a < b; });
    order.insert(order.end(), fresh.begin(), fresh.end());

    std::vector<TaskItem> result(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(pinCount));
    for (const auto& k : order)
        result.push_back(std::move(out[at[k]]));
    return result;
}

bool TaskModel::Impl::CommitRebuildResult(std::vector<TaskItem> result)
{
    std::vector<HWND> newIconic;
    for (const TaskItem& t : result)
        for (HWND h : t.windows)
            if (IsIconic(h))
                newIconic.push_back(h);
    const bool iconicChanged = IconicSetChanged(lastIconic, newIconic);
    lastIconic = std::move(newIconic);

    bool changed = iconicChanged || result.size() != items.size();
    for (size_t i = 0; !changed && i < result.size(); ++i)
        changed = !Same(result[i], items[i]);
    items = std::move(result);
    std::erase_if(names, [&](const auto& entry) { return !meta.contains(entry.first); });
    std::erase_if(namesRequested, [&](const auto& key) { return !meta.contains(key); });
    std::erase_if(iconVersion, [&](const auto& entry) { return !meta.contains(entry.first); });

    std::unordered_set<std::wstring> liveKeys;
    liveKeys.reserve(meta.size());
    for (const auto& [key, value] : meta)
        liveKeys.insert(key);
    iconWorker->SyncLiveKeys(liveKeys);
    return changed;
}

// Recomputes `items`/`meta` from `scan` and the pins; true if `items` changed.
bool TaskModel::Impl::Rebuild()
{
    std::vector<TaskItem> out;
    std::unordered_map<std::wstring, TaskMeta> newMeta;
    std::unordered_map<std::wstring, size_t> at;
    std::unordered_map<std::wstring, ULONGLONG> firstSeen;
    std::unordered_map<std::wstring, std::wstring> firstTitle;

    SeedPinnedItems(out, newMeta, at);
    const size_t pinCount = out.size();

    const std::vector<const TaskScanRec*> sorted = SortScanByActivity();
    const HWND fg = ForegroundRoot();
    std::unordered_set<std::wstring> running;
    for (const TaskScanRec* r : sorted)
        RecordWindowInGroup(*r, pinCount, fg, out, newMeta, at, firstSeen, firstTitle, running);

    ApplyHiddenAttention(out, newMeta);
    RetainHiddenProcessTargets(newMeta);

    std::vector<TaskItem> result = AssembleOrderedItems(out, at, running, firstSeen, pinCount);
    FinalizeItemMetadata(result, newMeta, firstTitle);
    meta = std::move(newMeta);
    return CommitRebuildResult(std::move(result));
}

const TaskItem* TaskModel::Impl::Find(const std::wstring& key) const
{
    for (const TaskItem& t : items)
        if (t.key == key)
            return &t;
    return nullptr;
}

std::vector<TaskItem> TaskModel::Impl::Snapshot(bool all) const
{
    if (showRunning || all)
        return items;
    std::vector<TaskItem> out;
    for (const TaskItem& t : items)
        if (t.pinned)
            out.push_back(t);
    return out;
}

void TaskModel::Impl::SetPins(const std::vector<PinnedApp>& list)
{
    bool same = list.size() == pins.size();
    for (size_t i = 0; same && i < list.size(); ++i)
        same = list[i] == pins[i].app;
    if (same)
        return;
    pins.clear();
    pinByKey.clear();
    pinByTarget.clear();
    for (const PinnedApp& app : list) {
        detail::TaskPin p = detail::ResolveTaskPin(app);
        if (pinByKey.contains(p.key))
            p.key += L"#" + std::to_wstring(pins.size()); // duplicate pin: shown, never matched
        else
            pinByKey[p.key] = pins.size();
        auto addTarget = [&](const std::wstring& target) {
            if (target.empty())
                return;
            auto [it, added] = pinByTarget.emplace(target, pins.size());
            if (!added && it->second != pins.size())
                it->second = size_t(-1);
        };
        // A browser profile's or web app's shortcut keeps its explicit identity; the browser's
        // own pin takes its other windows, whichever profile (#52).
        if (!p.specific) {
            addTarget(p.target);
            addTarget(Lower(p.exe));
        }
        pins.push_back(std::move(p));
    }
    if (Rebuild()) {
        notify = true;
        ArmTimer();
    }
}

void TaskModel::Impl::SetShowRunning(bool show)
{
    if (show == showRunning)
        return;
    showRunning = show;
    notify = true;
    ArmTimer();
}

IconImage TaskModel::Impl::Icon(const std::wstring& key, int px) const
{
    auto mit = meta.find(key);
    if (mit == meta.end())
        return {};
    return iconWorker->GetOrRequest(key, px, mit->second.source);
}

TaskModel::TaskModel(HWND uiWindow, std::function<void()> onChanged, const std::function<bool(const std::wstring&)>& openExisting)
    : impl_(std::make_unique<Impl>(uiWindow, std::move(onChanged), openExisting))
{
}

TaskModel::~TaskModel() = default;

std::vector<TaskItem> TaskModel::Snapshot(bool all) const { return impl_->Snapshot(all); }

void TaskModel::SetPins(const std::vector<PinnedApp>& pins) { impl_->SetPins(pins); }

void TaskModel::SetShowRunning(bool show) { impl_->SetShowRunning(show); }

void TaskModel::SetOnFlash(std::function<void(HWND)> onFlash) { impl_->onFlash = std::move(onFlash); }

IconImage TaskModel::Icon(const std::wstring& key, int sizePx) { return impl_->Icon(key, sizePx); }

void TaskModel::Activate(const std::wstring& key, HMONITOR monitor) { impl_->Activate(key, monitor); }

void TaskModel::Launch(const std::wstring& key) { impl_->Launch(key); }

bool TaskModel::CanTerminate(const std::wstring& key) const { return std::ranges::any_of(impl_->TerminationTargets(key), CanTerminateApp); }

void TaskModel::Terminate(const std::wstring& key)
{
    TerminateAppProcesses(impl_->TerminationTargets(key));
    impl_->MarkDirty();
}

PinnedApp TaskModel::PinFor(const std::wstring& key) const { return impl_->PinFor(key); }

bool TaskModel::Show(const std::wstring& key, DWORD pid) { return impl_->Show(key, pid); }

void TaskModel::MoveRunning(const std::wstring& key, int toIndex) { impl_->MoveRunning(key, toIndex); }

bool TaskModel::CanOpenFiles(const std::wstring& key) const { return !impl_->DesktopExe(key).empty() || !impl_->Aumid(key).empty(); }

bool TaskModel::OpenFiles(const std::wstring& key, const std::vector<std::wstring>& files) { return impl_->OpenFiles(key, files); }

std::wstring TaskModel::PinTarget(const std::wstring& key) const { return impl_->PinTarget(key); }

std::wstring TaskModel::FileLocation(const std::wstring& key) const { return impl_->DesktopExe(key); }

void TaskModel::OnCatalogLoaded() { impl_->iconWorker->RetryFailedIcons(); }

bool TaskModel::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) { return impl_->HandleMessage(msg, wp, lp); }

} // namespace atrium
