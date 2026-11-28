// Task model for the bar: pinned apps plus running app windows, grouped per
// application (D13). Lives on the UI thread (WinEvent hooks need its message
// loop); the bar reads immutable snapshots.
#pragma once

#include "icon_image.h"
#include "pinned_app.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace atrium {

struct TaskItem {
    std::wstring key;   // stable grouping key: lowercase AUMID if known, else lowercase exe path
    std::wstring name;  // pinned name, else window/app display name
    bool pinned = false;
    int pinIndex = -1;  // position in the pin list, -1 if not pinned
    std::vector<HWND> windows; // visible app windows of this group, most recently active first
    bool active = false;       // one of `windows` is the foreground window
    bool attention = false;    // a window is flashing (FlashWindowEx / HSHELL_FLASH)
    int badge = 0;             // > 0: unread count from window title (or injected by host)
    uint64_t iconVersion = 0;  // changes when the icon should be fetched again
    bool multipleWindows = false;
};

bool AllowsMultipleWindows(const PinnedApp& app);

class TaskModel {
public:
    // Calls `onChanged` on the UI thread (coalesced, at most every ~30 ms)
    // whenever Snapshot() would return something different.
    TaskModel(HWND uiWindow, std::function<void()> onChanged, const std::function<bool(const std::wstring&)>& openExisting = {});
    ~TaskModel();
    TaskModel(const TaskModel&) = delete;
    TaskModel& operator=(const TaskModel&) = delete;

    // Pinned items first in pin order, then running unpinned groups in the
    // order they first appeared. Unpinned groups are omitted if !showRunning,
    // unless `all` (Stage Manager sees every app).
    std::vector<TaskItem> Snapshot(bool all = false) const;

    void SetPins(const std::vector<PinnedApp>& pins);
    void SetShowRunning(bool show);
    // A window flashed for attention (HSHELL_FLASH), also a hidden one (WeChat in the tray flashes
    // its window for a message); called on the UI thread, while that window is not in front.
    void SetOnFlash(std::function<void(HWND)> onFlash);

    // Icon for an item at `sizePx` (square), cached per key and size. Never
    // blocks for long: returns empty until loaded, then onChanged fires.
    IconImage Icon(const std::wstring& key, int sizePx);

    // Left click, like the Windows taskbar: no window -> launch; one window ->
    // minimize if it is the foreground window, otherwise restore and activate;
    // several windows -> activate the next one in turn.
    void Activate(const std::wstring& key, HMONITOR monitor = nullptr); // monitor restricts cycling to this taskbar's windows
    // Middle click / "new window": start another instance.
    void Launch(const std::wstring& key);
    // End the entire application, including tray processes and child helpers.
    bool CanTerminate(const std::wstring& key) const;
    void Terminate(const std::wstring& key);
    // One window of a group (the window previews): restore and bring it to the front, or close it.
    static void ActivateWindow(HWND window);
    // Same, without invoking the UI-thread restore animation (safe from StageManager's worker).
    static void ActivateWindowDirect(HWND window);
    // An animation that plays a window's minimize or restore by these clicks (the genie): given the window
    // and the step itself, it returns true to take the step over and run it when it is ready. UI thread.
    using WindowStep = std::function<bool(HWND, std::function<void()>)>;
    static void SetWindowSteps(WindowStep minimizing, WindowStep restoring);
    // True means the native close command was queued; the application still
    // owns confirmation, cancellation and close-to-tray behavior.
    static bool CloseWindow(HWND window);
    // A pin for a running unpinned item (to append to settings).
    PinnedApp PinFor(const std::wstring& key) const;
    // The path a pinned item was pinned from (a shortcut when that is what was
    // pinned): the shell's menus and AppUserModelID live there, not on the exe.
    std::wstring PinTarget(const std::wstring& key) const;
    // Brings a window of the group to the front (never minimizes or launches);
    // when `pid` is non-zero, targets that specific instance. Returns true if one was shown.
    bool Show(const std::wstring& key, DWORD pid = 0);
    // Moves a running unpinned group to `toIndex` among the unpinned groups.
    void MoveRunning(const std::wstring& key, int toIndex);
    // Files dropped on an item: open them with that app.
    bool CanOpenFiles(const std::wstring& key) const;
    bool OpenFiles(const std::wstring& key, const std::vector<std::wstring>& files);
    // The desktop exe behind an item ("open file location"); empty for packaged apps.
    std::wstring FileLocation(const std::wstring& key) const;

    // The user's current Windows taskbar pins (desktop apps, from
    // %APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar).
    static std::vector<PinnedApp> ImportWindowsPins();

    // Reloads failed icons immediately once AppCatalog finishes its initial refresh.
    void OnCatalogLoaded();

    // Forwarded from the UI window procedure; returns true if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace detail {

struct LaunchPlan {
    std::wstring file, args, dir;
};

struct TaskFallbackRunner {
    TaskFallbackRunner() = default;
    virtual ~TaskFallbackRunner() = default;
    TaskFallbackRunner(const TaskFallbackRunner&) = delete;
    TaskFallbackRunner& operator=(const TaskFallbackRunner&) = delete;
    virtual void Queue(std::optional<LaunchPlan> plan, std::wstring exe) = 0;
};

std::unique_ptr<TaskFallbackRunner> CreateFallbackRunner(HWND ui, UINT restoreMsg);
std::wstring AppsFolderPath(const std::wstring& aumid);
bool IsPackagedPath(const std::wstring& path);
std::wstring PackagedAumidForExe(const std::wstring& exe);
std::wstring TaskProcessImagePath(HANDLE process);
bool ExecTaskLaunch(const std::wstring& file, const std::wstring& args, const std::wstring& dir, bool noAsync = false);
bool ProcessRuns(const std::wstring& exe);
std::optional<LaunchPlan> BuildLaunchPlan(const PinnedApp* pin, const std::wstring& aumid, const std::wstring& exePath, const std::wstring& key,
                                          const std::wstring& relaunchCommand = {});
bool OpenFilesWithTaskApp(const std::wstring& desktopExe, const std::wstring& aumid, const std::vector<std::wstring>& files);
bool SendTaskSysCommand(HWND h, WPARAM command);
void RaiseTaskWindow(HWND h, HWND ui, UINT restoreMsg, const std::function<void(HWND)>& deferActivate, const std::function<bool(HWND)>& isPending);

// Window identity, scanning, and pin resolution helpers (task_model_windows.cpp).
struct TaskWinInfo {
    DWORD pid = 0;
    std::wstring exePath;
    std::wstring aumid;
    std::wstring key;
    ULONGLONG created = 0;
    bool webApp = false;
    std::wstring launcher;
    std::wstring relaunchCommand;
    std::wstring relaunchIcon;
    std::wstring relaunchName;
};

struct TaskPin {
    PinnedApp app;
    std::wstring key, target, exe, aumid;
    bool multipleWindows = false;
    bool specific = false;
    bool webApp = false;
};

std::wstring TaskHwndKey(HWND h);
TaskWinInfo ComputeTaskWinInfo(HWND h, DWORD pid, const wchar_t* cls);
bool IsTaskAppWindow(HWND h, DWORD& pid, wchar_t* cls, int clsCap, std::wstring& title);
bool ReadTaskLink(const std::wstring& path, std::wstring& target, std::wstring& aumid, std::wstring* args = nullptr);
std::wstring TaskFileDescription(const std::wstring& path);
std::wstring TaskShellDisplayName(const std::wstring& parsingName);
bool TaskWindowDirectIcon(HWND h, int size, IconImage& out);
bool TaskWindowIcon(HWND h, int size, IconImage& out);
TaskPin ResolveTaskPin(const PinnedApp& app);

// Background icon and display-name worker (task_model_icons.cpp).
struct TaskIconSource {
    std::wstring aumid, pinPath, exePath;
    std::wstring relaunchIcon, relaunchName;
    bool webApp = false;
    std::vector<HWND> hwnds;
    std::wstring Signature() const;
};

class TaskIconWorker {
public:
    TaskIconWorker(HWND ui, UINT readyMsg);
    ~TaskIconWorker();
    TaskIconWorker(const TaskIconWorker&) = delete;
    TaskIconWorker& operator=(const TaskIconWorker&) = delete;

    IconImage GetOrRequest(const std::wstring& key, int sizePx, const TaskIconSource& source);
    void RequestName(const std::wstring& key, const TaskIconSource& source);
    void SyncLiveKeys(const std::unordered_set<std::wstring>& liveKeys);
    void RetryFailedIcons();
    void TakeReady(std::vector<std::wstring>& doneIcons, std::vector<std::pair<std::wstring, std::wstring>>& doneNames);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace detail

} // namespace atrium
