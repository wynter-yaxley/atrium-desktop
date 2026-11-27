#include "keyboard_hook.h"

#include "log.h"

#include <atomic>
#include <cstddef>
#include <vector>

namespace atrium {

namespace {

constexpr UINT kRehookMs = 10000;
constexpr UINT kMsgSyncHooks = WM_APP + 1;

std::atomic<KeyboardHook*> g_hook{nullptr}; // the hook procedure has no context of its own

enum class HookResult { Unchanged, Installed, Failed, Removed };

HookResult SyncHook(HHOOK& hook, int hookId, HOOKPROC proc, bool want, bool rehook)
{
    if (!want) {
        if (!hook)
            return HookResult::Unchanged;
        UnhookWindowsHookEx(hook);
        hook = nullptr;
        return HookResult::Removed;
    }
    if (hook && !rehook)
        return HookResult::Unchanged;
    const bool wasInstalled = hook != nullptr;
    if (HHOOK again = SetWindowsHookExW(hookId, proc, GetModuleHandleW(nullptr), 0)) {
        if (hook)
            UnhookWindowsHookEx(hook);
        hook = again;
        return wasInstalled ? HookResult::Unchanged : HookResult::Installed;
    }
    return wasInstalled ? HookResult::Unchanged : HookResult::Failed;
}

} // namespace

KeyboardHook& KeyboardHook::Instance()
{
    static KeyboardHook hook;
    return hook;
}

KeyboardHook::~KeyboardHook()
{
    StopThreadLocked();
    g_hook = nullptr;
}

int KeyboardHook::Add(Handler handler, void* context)
{
    const std::lock_guard lifecycle(lifecycleMutex_);
    bool firstHandler = false;
    int token = 0;
    {
        const std::lock_guard lock(handlersMutex_);
        token = nextToken_++;
        handlers_.push_back({token, handler, context});
        firstHandler = handlers_.size() == 1;
    }
    ActivateHookThreadLocked(firstHandler);
    return token;
}

void KeyboardHook::Remove(int token)
{
    const std::lock_guard lifecycle(lifecycleMutex_);
    bool handlersBecameEmpty = false;
    {
        const std::lock_guard lock(handlersMutex_);
        const std::size_t before = handlers_.size();
        std::erase_if(handlers_, [&](const Entry& entry) { return entry.token == token; });
        handlersBecameEmpty = before > 0 && handlers_.empty();
    }
    if (handlersBecameEmpty)
        DeactivateOrSyncHookThreadLocked();
}

int KeyboardHook::AddMouseDownWatcher(HWND target, UINT msg)
{
    if (!target || msg == 0)
        return 0;
    const std::lock_guard lifecycle(lifecycleMutex_);
    bool firstWatcher = false;
    int token = 0;
    {
        const std::lock_guard lock(mouseWatchersMutex_);
        token = nextMouseToken_++;
        mouseWatchers_.push_back({token, target, msg});
        firstWatcher = mouseWatchers_.size() == 1;
    }
    ActivateHookThreadLocked(firstWatcher);
    return token;
}

void KeyboardHook::RemoveMouseDownWatcher(int token)
{
    if (token <= 0)
        return;
    const std::lock_guard lifecycle(lifecycleMutex_);
    bool watchersBecameEmpty = false;
    {
        const std::lock_guard lock(mouseWatchersMutex_);
        const std::size_t before = mouseWatchers_.size();
        std::erase_if(mouseWatchers_, [&](const MouseWatcher& watcher) { return watcher.token == token; });
        watchersBecameEmpty = before > 0 && mouseWatchers_.empty();
    }
    if (watchersBecameEmpty)
        DeactivateOrSyncHookThreadLocked();
}

void KeyboardHook::ActivateHookThreadLocked(bool needSync)
{
    if (!thread_.joinable()) {
        stop_ = false;
        ready_ = false;
        thread_ = std::thread([this] { Run(); });
        std::unique_lock lock(readyMutex_);
        readyCv_.wait(lock, [this] { return ready_.load(); });
        return;
    }
    if (needSync) {
        if (const DWORD tid = threadId_.load())
            PostThreadMessageW(tid, kMsgSyncHooks, 0, 0);
    }
}

void KeyboardHook::DeactivateOrSyncHookThreadLocked()
{
    if (!HasHandlers() && !HasMouseWatchers()) {
        StopThreadLocked();
        return;
    }
    if (const DWORD tid = threadId_.load())
        PostThreadMessageW(tid, kMsgSyncHooks, 0, 0);
}

void KeyboardHook::StopThreadLocked()
{
    if (!thread_.joinable())
        return;
    stop_ = true;
    if (const DWORD tid = threadId_.load())
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    std::thread finished = std::move(thread_);
    finished.join();
    threadId_ = 0;
}

bool KeyboardHook::HasHandlers()
{
    const std::lock_guard lock(handlersMutex_);
    return !handlers_.empty();
}

bool KeyboardHook::HasMouseWatchers()
{
    const std::lock_guard lock(mouseWatchersMutex_);
    return !mouseWatchers_.empty();
}

void KeyboardHook::Run()
{
    g_hook = this;
    threadId_ = GetCurrentThreadId();
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // the thread's queue, for WM_QUIT and kMsgSyncHooks
    HHOOK keyboardHook = nullptr;
    HHOOK mouseHook = nullptr;
    auto syncAll = [&](bool rehook) {
        const HookResult kbResult = SyncHook(keyboardHook, WH_KEYBOARD_LL, HookProc, HasHandlers(), rehook);
        if (kbResult == HookResult::Installed)
            Log(L"keyboard: one low-level hook on its own thread");
        else if (kbResult == HookResult::Failed)
            Log(L"keyboard: could not watch the keyboard (%lu)", GetLastError());
        const HookResult mouseResult = SyncHook(mouseHook, WH_MOUSE_LL, MouseHookProc, HasMouseWatchers(), rehook);
        if (mouseResult == HookResult::Failed)
            Log(L"mouse: could not watch the mouse (%lu)", GetLastError());
    };
    syncAll(false);
    {
        const std::lock_guard lock(readyMutex_);
        ready_ = true;
    }
    readyCv_.notify_all();
    // Low-level hooks are called on this thread while it waits for messages:
    // it does nothing else, so a busy UI thread never delays a key. Windows
    // drops a hook that once answered too late without a word, so it is set
    // again now and then, between presses (#71).
    const UINT_PTR timerId = SetTimer(nullptr, 0, kRehookMs, nullptr);
    while (!stop_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER)
            syncAll(true);
        else if (msg.message == kMsgSyncHooks)
            syncAll(false);
        else
            DispatchMessageW(&msg);
    }
    if (timerId)
        KillTimer(nullptr, timerId);
    if (mouseHook)
        UnhookWindowsHookEx(mouseHook);
    if (keyboardHook)
        UnhookWindowsHookEx(keyboardHook);
    g_hook = nullptr;
}

LRESULT CALLBACK KeyboardHook::HookProc(int code, WPARAM wparam, LPARAM lparam)
{
    KeyboardHook* self = g_hook.load();
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    if (code != HC_ACTION || !self)
        return CallNextHookEx(nullptr, code, wparam, lparam);
    const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
    const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
    if (!down && !up)
        return CallNextHookEx(nullptr, code, wparam, lparam);
    const bool injected = (key->flags & LLKHF_INJECTED) != 0;
    // A handler may have the key kept from Windows. Handlers run under the
    // recursive lock: Remove waits for the one in flight, so a destroyed handler
    // is never called, and a handler that sends a key (WinKey's replay) may
    // re-enter this hook on this same thread without deadlocking.
    const bool extended = (key->flags & LLKHF_EXTENDED) != 0;
    const std::lock_guard lock(self->handlersMutex_);
    for (const Entry& entry : self->handlers_)
        if (entry.handler(entry.context, key->vkCode, key->scanCode, extended, down, injected, key->dwExtraInfo))
            return 1;
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK KeyboardHook::MouseHookProc(int code, WPARAM wparam, LPARAM lparam)
{
    KeyboardHook* self = g_hook.load();
    if (code != HC_ACTION || !self)
        return CallNextHookEx(nullptr, code, wparam, lparam);
    if (wparam != WM_LBUTTONDOWN && wparam != WM_RBUTTONDOWN && wparam != WM_MBUTTONDOWN)
        return CallNextHookEx(nullptr, code, wparam, lparam);
    const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    if (!mouse || mouse->dwExtraInfo == kAtriumSyntheticKey)
        return CallNextHookEx(nullptr, code, wparam, lparam);
    const LPARAM packedPt = MAKELPARAM(mouse->pt.x, mouse->pt.y);
    {
        const std::lock_guard lock(self->mouseWatchersMutex_);
        for (const MouseWatcher& watcher : self->mouseWatchers_)
            PostMessageW(watcher.target, watcher.msg, wparam, packedPt);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

} // namespace atrium
