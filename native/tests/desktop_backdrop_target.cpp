#include "render/desktop_backdrop_target.h"

#include <iostream>
#include <vector>

using atrium::DesktopBackdropTarget;
using atrium::DesktopHostCandidate;
using atrium::SelectDesktopBackdropTarget;

namespace {

HWND FakeHwnd(uintptr_t v) { return reinterpret_cast<HWND>(v); }

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

DesktopHostCandidate MakeProgman(uintptr_t id, bool hasDefView)
{
    return DesktopHostCandidate{
        .hwnd = FakeHwnd(id),
        .isProgman = true,
        .hasDefView = hasDefView,
        .visible = true,
        .cloaked = false,
        .hasVisibleChild = false,
        .rect = RECT{0, 0, 1920, 1080},
    };
}

DesktopHostCandidate MakeWorker(uintptr_t id, bool hasDefView, bool visible, bool cloaked, bool hasVisibleChild, RECT rect = {0, 0, 1920, 1080})
{
    return DesktopHostCandidate{
        .hwnd = FakeHwnd(id),
        .isProgman = false,
        .hasDefView = hasDefView,
        .visible = visible,
        .cloaked = cloaked,
        .hasVisibleChild = hasVisibleChild,
        .rect = rect,
    };
}

bool CheckRaisedAndUnsplitDesktop()
{
    // Windows 11 24H2+ Raised Desktop keeps SHELLDLL_DefView inside Progman even if extra WorkerWs exist.
    const std::vector<DesktopHostCandidate> raised = {
        MakeWorker(0x10, false, true, false, true),
        MakeProgman(0x20, true),
    };
    const DesktopBackdropTarget t1 = SelectDesktopBackdropTarget(raised);
    if (!Require(t1.hwnd == FakeHwnd(0x20) && t1.isProgman, "24H2+ Raised Desktop selects Progman"))
        return false;

    const std::vector<DesktopHostCandidate> unsplit = {MakeProgman(0x21, true)};
    const DesktopBackdropTarget t2 = SelectDesktopBackdropTarget(unsplit);
    return Require(t2.hwnd == FakeHwnd(0x21) && t2.isProgman, "unsplit desktop selects Progman");
}

bool CheckSplitDesktopWithLiveWallpaper()
{
    // Win10 / Win11 23H2: DefView WorkerW -> Wallpaper WorkerW (behind DefView) -> Progman (no DefView).
    const std::vector<DesktopHostCandidate> splitLive = {
        MakeWorker(0x100, true, true, false, false),
        MakeWorker(0x200, false, true, false, true),
        MakeProgman(0x300, false),
    };
    const DesktopBackdropTarget t = SelectDesktopBackdropTarget(splitLive);
    return Require(t.hwnd == FakeHwnd(0x200) && !t.isProgman, "23H2 split desktop with live wallpaper selects WorkerW behind DefView");
}

bool CheckSplitDesktopFallbacks()
{
    // Split desktop without a live wallpaper child falls back to Progman.
    const std::vector<DesktopHostCandidate> noChild = {
        MakeWorker(0x100, true, true, false, false),
        MakeWorker(0x200, false, true, false, false),
        MakeProgman(0x300, false),
    };
    const DesktopBackdropTarget t1 = SelectDesktopBackdropTarget(noChild);
    if (!Require(t1.hwnd == FakeHwnd(0x300) && t1.isProgman, "split desktop without visible child falls back to Progman"))
        return false;

    // Hidden, cloaked, or undersized WorkerW behind DefView must be ignored.
    const std::vector<DesktopHostCandidate> invalidWorkers = {
        MakeWorker(0x100, true, true, false, false),
        MakeWorker(0x201, false, false, false, true),
        MakeWorker(0x202, false, true, true, true),
        MakeWorker(0x203, false, true, false, true, RECT{0, 0, 100, 100}),
        MakeProgman(0x300, false),
    };
    const DesktopBackdropTarget t2 = SelectDesktopBackdropTarget(invalidWorkers);
    if (!Require(t2.hwnd == FakeHwnd(0x300) && t2.isProgman, "hidden, cloaked, or tiny WorkerW falls back to Progman"))
        return false;

    // A stray WorkerW above SHELLDLL_DefView in Z-order must not be picked as a wallpaper host.
    const std::vector<DesktopHostCandidate> strayAbove = {
        MakeWorker(0x050, false, true, false, true),
        MakeWorker(0x100, true, true, false, false),
        MakeProgman(0x300, false),
    };
    const DesktopBackdropTarget t3 = SelectDesktopBackdropTarget(strayAbove);
    return Require(t3.hwnd == FakeHwnd(0x300) && t3.isProgman, "stray WorkerW above DefView is ignored");
}

bool CheckSkipFailedWorkerAndMissingDesktop()
{
    const std::vector<DesktopHostCandidate> splitLive = {
        MakeWorker(0x100, true, true, false, false),
        MakeWorker(0x200, false, true, false, true),
        MakeProgman(0x300, false),
    };
    const DesktopBackdropTarget skippedToProgman = SelectDesktopBackdropTarget(splitLive, FakeHwnd(0x200));
    if (!Require(skippedToProgman.hwnd == FakeHwnd(0x300) && skippedToProgman.isProgman, "skipWorker falls back to Progman"))
        return false;

    const std::vector<DesktopHostCandidate> twoWorkers = {
        MakeWorker(0x100, true, true, false, false),
        MakeWorker(0x200, false, true, false, true),
        MakeWorker(0x250, false, true, false, true),
        MakeProgman(0x300, false),
    };
    const DesktopBackdropTarget skippedToSecond = SelectDesktopBackdropTarget(twoWorkers, FakeHwnd(0x200));
    if (!Require(skippedToSecond.hwnd == FakeHwnd(0x250) && !skippedToSecond.isProgman, "skipWorker can select next valid WorkerW"))
        return false;

    const DesktopBackdropTarget empty = SelectDesktopBackdropTarget({});
    return Require(empty.hwnd == nullptr && empty.isProgman, "empty candidate list returns null target");
}

bool CheckBoundedAsyncRetirementBudget()
{
    // Allocation failure prevents starting a session that would lack a guaranteed async retirement slot.
    auto failSlot = atrium::AsyncWorkSlot::Create([](PTP_WORK_CALLBACK, PVOID) -> PTP_WORK { return nullptr; });
    if (!Require(!failSlot, "failed preallocated work creation must abort cleanly without leaking"))
        return false;

    HANDLE releaseEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Require(releaseEvent != nullptr, "create release event"))
        return false;
    auto slot = atrium::AsyncWorkSlot::Create();
    if (!Require(slot != nullptr && slot->DoneEvent() != nullptr, "preallocate async retirement slot")) {
        CloseHandle(releaseEvent);
        return false;
    }
    slot->Retire([releaseEvent] { WaitForSingleObject(releaseEvent, 5000); });
    const ULONGLONG t0 = GetTickCount64();
    const bool completedInBudget = atrium::WaitBoundedHandles({slot->DoneEvent()}, 250);
    const ULONGLONG elapsed = GetTickCount64() - t0;
    SetEvent(releaseEvent);
    const bool completedAfterRelease = atrium::WaitBoundedHandles({slot->DoneEvent()}, 2000);
    CloseHandle(releaseEvent);
    return Require(!completedInBudget && elapsed < 1000 && completedAfterRelease && slot->Finished(),
                   "stalled async close must time out within 250ms budget without blocking caller");
}

} // namespace

int main()
{
    if (!CheckRaisedAndUnsplitDesktop() || !CheckSplitDesktopWithLiveWallpaper() || !CheckSplitDesktopFallbacks() ||
        !CheckSkipFailedWorkerAndMissingDesktop() || !CheckBoundedAsyncRetirementBudget())
        return 1;
    std::cout << "Desktop backdrop target selection checks passed.\n";
    return 0;
}
