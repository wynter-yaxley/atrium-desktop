// One low-level keyboard hook for the whole of Atrium, on a thread of its own:
// the UI thread never waits for a key, and the Windows key and the input method
// watcher share this hook instead of each installing one. Windows silently
// drops a hook that once answered too late, so it is set again now and then,
// between presses (#71). Handlers run on the hook thread: they must be quick
// and never block.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace atrium {

// The keys Atrium itself sends on (a replayed Win or Space); handlers let them
// through, and the input method watcher recognizes its own Win+Space replay.
constexpr ULONG_PTR kAtriumSyntheticKey = 0x41545257; // "ATRW"

class KeyboardHook {
public:
    // Called on the hook thread for every key. Return true to keep the key
    // from Windows; false lets it through.
    using Handler = bool (*)(void* context, DWORD vk, DWORD scan, bool extended, bool down, bool injected, ULONG_PTR extraInfo);

    static KeyboardHook& Instance();

    // The hook runs while at least one handler is registered. The token is for Remove.
    int Add(Handler handler, void* context);
    void Remove(int token);

    // Watchers receive non-blocking PostMessageW(target, msg, wparam, MAKELPARAM(pt.x, pt.y))
    // on WH_MOUSE_LL button-down events (WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN).
    // Receivers must unpack lparam with GET_X_LPARAM / GET_Y_LPARAM (signed) for negative secondary-display coordinates.
    // WH_MOUSE_LL is installed only while at least one watcher is active.
    int AddMouseDownWatcher(HWND target, UINT msg);
    void RemoveMouseDownWatcher(int token);

    KeyboardHook(const KeyboardHook&) = delete;
    KeyboardHook& operator=(const KeyboardHook&) = delete;

private:
    KeyboardHook() = default;
    ~KeyboardHook();
    struct Entry {
        int token;
        Handler handler;
        void* context;
    };
    struct MouseWatcher {
        int token;
        HWND target;
        UINT msg;
    };
    static LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wparam, LPARAM lparam);
    void Run();
    void ActivateHookThreadLocked(bool needSync);
    void DeactivateOrSyncHookThreadLocked();
    void StopThreadLocked();
    bool HasHandlers();
    bool HasMouseWatchers();

    // Recursive: a handler may send keys (WinKey replays Win + a shortcut), and
    // the hook sees those on this same thread before the handler returns.
    std::recursive_mutex handlersMutex_;
    std::mutex mouseWatchersMutex_;
    std::mutex lifecycleMutex_; // serializes Add/Remove thread start and join; never taken in hook procs
    std::vector<Entry> handlers_;
    std::vector<MouseWatcher> mouseWatchers_;
    int nextToken_ = 1;
    int nextMouseToken_ = 1;
    std::thread thread_;
    std::atomic<DWORD> threadId_{0};
    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_{false};
};

} // namespace atrium
