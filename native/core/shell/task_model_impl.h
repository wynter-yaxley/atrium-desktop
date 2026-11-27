// Internal TaskModel::Impl declaration shared across task_model.cpp and task_model_actions.cpp.
#pragma once

#include "shell/task_model.h"
#include "shell/app_processes.h"
#include "shell/task_rules.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace atrium {

struct TaskMeta {
    detail::TaskIconSource source;
    std::vector<AppProcessIdentity> processTargets; // retained for pinned apps closed to the tray
};

struct TaskScanRec {
    HWND hwnd = nullptr;
    int z = 0; // EnumWindows order, top first
    std::wstring title;
    detail::TaskWinInfo info;
};

struct TaskModel::Impl {
    HWND ui = nullptr;
    std::function<void()> onChanged;
    std::function<bool(const std::wstring&)> openExisting;
    std::function<void(HWND)> onFlash;
    bool comOwned = false;
    UINT shellHookMsg = 0, readyMsg = 0, restoreMsg = 0;
    std::vector<HWINEVENTHOOK> hooks;
    bool shellHooked = false, timerArmed = false, restoreTimerArmed = false, dirty = true, notify = false, showRunning = true;

    // UI-thread state.
    std::vector<detail::TaskPin> pins;
    std::unordered_map<std::wstring, size_t> pinByKey, pinByTarget;
    std::unordered_map<HWND, detail::TaskWinInfo> infos; // identity cache per window
    std::unordered_map<HWND, uint64_t> lastActive;       // activation sequence number
    std::unordered_set<HWND> attention;
    uint64_t seq = 0;
    HWND lastFg = nullptr;
    std::vector<TaskScanRec> scan;
    std::vector<std::wstring> order; // unpinned groups, first seen first
    std::vector<TaskItem> items;     // pins + running groups (not filtered by showRunning)
    std::vector<HWND> lastIconic;    // minimized windows at the last Rebuild()
    std::unordered_map<std::wstring, TaskMeta> meta;
    std::unordered_map<std::wstring, std::wstring> names;
    std::unordered_set<std::wstring> namesRequested;
    std::unordered_map<std::wstring, uint64_t> iconVersion;
    std::vector<PendingRestore> pendingRestores;

    std::unique_ptr<detail::TaskIconWorker> iconWorker;
    std::unique_ptr<detail::TaskFallbackRunner> fallback;

    static Impl* g_self;

    Impl(HWND window, std::function<void()> changed, std::function<bool(const std::wstring&)> opener);
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);
    static BOOL CALLBACK CollectWindow(HWND h, LPARAM lp);
    static void Minimize(HWND h);
    static void BringToFront(HWND h, bool allowStep = true);
    static void Raise(HWND h);

    void OnEvent(DWORD event, HWND hwnd);
    void NoteForeground(HWND fg);
    void MarkDirty();
    void ArmTimer();
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void OnShellHook(WPARAM code, HWND hwnd);
    void OnReady();
    void Tick();

    HWND ForegroundRoot() const;
    void Rescan();
    int MatchPin(const detail::TaskWinInfo& info) const;
    void ApplyHiddenAttention(std::vector<TaskItem>& out, const std::unordered_map<std::wstring, TaskMeta>& newMeta) const;
    void FinalizeItemMetadata(std::vector<TaskItem>& result, const std::unordered_map<std::wstring, TaskMeta>& newMeta,
                              const std::unordered_map<std::wstring, std::wstring>& firstTitle);
    void SeedPinnedItems(std::vector<TaskItem>& out, std::unordered_map<std::wstring, TaskMeta>& newMeta, std::unordered_map<std::wstring, size_t>& at) const;
    std::vector<const TaskScanRec*> SortScanByActivity() const;
    void RecordWindowInGroup(const TaskScanRec& r, size_t pinCount, HWND fg, std::vector<TaskItem>& out, std::unordered_map<std::wstring, TaskMeta>& newMeta,
                             std::unordered_map<std::wstring, size_t>& at, std::unordered_map<std::wstring, ULONGLONG>& firstSeen,
                             std::unordered_map<std::wstring, std::wstring>& firstTitle, std::unordered_set<std::wstring>& running) const;
    void RetainHiddenProcessTargets(std::unordered_map<std::wstring, TaskMeta>& newMeta) const;
    std::vector<TaskItem> AssembleOrderedItems(std::vector<TaskItem>& out, std::unordered_map<std::wstring, size_t>& at,
                                               const std::unordered_set<std::wstring>& running, std::unordered_map<std::wstring, ULONGLONG>& firstSeen,
                                               size_t pinCount);
    bool CommitRebuildResult(std::vector<TaskItem> result);
    bool Rebuild();

    const TaskItem* Find(const std::wstring& key) const;
    std::vector<TaskItem> Snapshot(bool all) const;
    void SetPins(const std::vector<PinnedApp>& list);
    void SetShowRunning(bool show);
    IconImage Icon(const std::wstring& key, int px) const;

    void Launch(const std::wstring& key);
    std::optional<detail::LaunchPlan> PlanFor(const std::wstring& key);
    void DeferActivate(HWND h);
    void ProcessPendingRestores();
    void Activate(const std::wstring& key, HMONITOR monitor);
    std::vector<AppProcessIdentity> TerminationTargets(const std::wstring& key) const;
    std::wstring PinTarget(const std::wstring& key) const;
    PinnedApp PinFor(const std::wstring& key) const;
    std::wstring DesktopExe(const std::wstring& key) const;
    std::wstring Aumid(const std::wstring& key) const;
    bool Show(const std::wstring& key, DWORD pid = 0);
    void MoveRunning(const std::wstring& key, int to);
    bool OpenFiles(const std::wstring& key, const std::vector<std::wstring>& files) const;
};

} // namespace atrium
