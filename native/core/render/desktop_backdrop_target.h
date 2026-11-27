// Picks which top-level desktop window holds the visible wallpaper for widget
// glass capture:
//  - On Windows 11 24H2+ (Raised Desktop) or an unsplit desktop, Progman owns
//    SHELLDLL_DefView and any live-wallpaper WorkerW child inside itself.
//  - On Windows 10 and Windows 11 23H2, tools such as Wallpaper Engine split
//    the desktop into sibling top-level WorkerW windows: one holds
//    SHELLDLL_DefView and another directly behind it in Z-order hosts the
//    wallpaper window, while Progman retains only the static wallpaper.
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace atrium {

// Pre-allocated Win32 threadpool work slot so retiring blocking WGC objects at teardown cannot fail
// on pool exhaustion or fall back to synchronous Close() on the calling thread.
class AsyncWorkSlot : public std::enable_shared_from_this<AsyncWorkSlot> {
public:
    AsyncWorkSlot() = default;
    ~AsyncWorkSlot()
    {
        if (work_ && !submitted_)
            ::CloseThreadpoolWork(work_);
        if (doneEvent_)
            CloseHandle(doneEvent_);
    }
    AsyncWorkSlot(const AsyncWorkSlot&) = delete;
    AsyncWorkSlot& operator=(const AsyncWorkSlot&) = delete;

    static std::shared_ptr<AsyncWorkSlot> Create(PTP_WORK (*createWork)(PTP_WORK_CALLBACK, PVOID) = nullptr)
    {
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done)
            return {};
        auto slot = std::make_shared<AsyncWorkSlot>();
        slot->doneEvent_ = done;
        PTP_WORK work = createWork ? createWork(&WorkCallback, slot.get()) : ::CreateThreadpoolWork(&WorkCallback, slot.get(), nullptr);
        if (!work)
            return {};
        slot->work_ = work;
        return slot;
    }

    // Submits `task` on the pre-allocated threadpool work item; non-failing once Create() succeeded.
    void Retire(std::function<void()> task)
    {
        if (submitted_ || !work_) {
            finished_.store(true, std::memory_order_release);
            if (doneEvent_)
                SetEvent(doneEvent_);
            return;
        }
        submitted_ = true;
        task_ = std::move(task);
        self_ = shared_from_this();
        ::SubmitThreadpoolWork(work_);
        ::CloseThreadpoolWork(work_);
        work_ = nullptr;
    }

    HANDLE DoneEvent() const { return doneEvent_; }
    bool Finished() const { return finished_.load(std::memory_order_acquire); }

private:
    static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK)
    {
        auto* raw = static_cast<AsyncWorkSlot*>(context);
        std::shared_ptr<AsyncWorkSlot> keepAlive = std::move(raw->self_);
        if (keepAlive && keepAlive->task_)
            keepAlive->task_();
        if (keepAlive) {
            keepAlive->task_ = {};
            keepAlive->finished_.store(true, std::memory_order_release);
            if (keepAlive->doneEvent_)
                SetEvent(keepAlive->doneEvent_);
        }
    }

    PTP_WORK work_ = nullptr;
    HANDLE doneEvent_ = nullptr;
    bool submitted_ = false;
    std::atomic<bool> finished_{false};
    std::function<void()> task_;
    std::shared_ptr<AsyncWorkSlot> self_;
};

// Waits at most `budgetMs` for every handle in `handles`; returns true if all signaled or empty.
inline bool WaitBoundedHandles(const std::vector<HANDLE>& handles, DWORD budgetMs)
{
    if (handles.empty())
        return true;
    const DWORD count = static_cast<DWORD>(std::min<size_t>(handles.size(), MAXIMUM_WAIT_OBJECTS));
    return WaitForMultipleObjects(count, handles.data(), TRUE, budgetMs) == WAIT_OBJECT_0;
}

struct DesktopHostCandidate {
    HWND hwnd = nullptr;
    bool isProgman = false;
    bool hasDefView = false;
    bool visible = false;
    bool cloaked = false;
    bool hasVisibleChild = false; // visible child window >= 64x64 (excluding SHELLDLL_DefView)
    RECT rect{};
};

struct DesktopBackdropTarget {
    HWND hwnd = nullptr;
    bool isProgman = true;
};

inline DesktopBackdropTarget SelectDesktopBackdropTarget(const std::vector<DesktopHostCandidate>& windows, HWND skipWorker = nullptr)
{
    const DesktopHostCandidate* progman = nullptr;
    size_t defViewIdx = windows.size();
    for (size_t i = 0; i < windows.size(); ++i) {
        const DesktopHostCandidate& w = windows[i];
        if (!w.hwnd)
            continue;
        if (w.isProgman && !progman)
            progman = &w;
        if (!w.isProgman && w.hasDefView && defViewIdx == windows.size())
            defViewIdx = i;
    }
    if (progman && progman->hasDefView)
        return {progman->hwnd, true};

    if (defViewIdx < windows.size()) {
        for (size_t i = defViewIdx + 1; i < windows.size(); ++i) {
            const DesktopHostCandidate& w = windows[i];
            if (w.isProgman || !w.hwnd || w.hwnd == skipWorker || !w.visible || w.cloaked || !w.hasVisibleChild)
                continue;
            const LONG width = w.rect.right - w.rect.left;
            const LONG height = w.rect.bottom - w.rect.top;
            if (width >= 320 && height >= 200)
                return {w.hwnd, false};
        }
    }

    if (progman)
        return {progman->hwnd, true};
    return {};
}

} // namespace atrium
