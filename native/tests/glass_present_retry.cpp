#include "render/glass_renderer.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Require(bool ok, const char* message)
{
    if (ok)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void TestBusyOnFirstFrameThenSucceeds()
{
    const atrium::PresentStateTransition busy = atrium::ResolvePresentTransition(DXGI_ERROR_WAS_STILL_DRAWING, false, true, true);
    Require(busy.ready, "busy first-frame Present must keep ready_ = true so next tick can retry");
    Require(!busy.hasPresented, "busy first-frame Present must not mark hasPresented_ = true");
    Require(busy.lastPresentBusy, "busy first-frame Present must report lastPresentBusy = true");
    Require(!busy.commitPresentedBackdrop, "busy first-frame Present must not commit presented_ backdrop");

    const atrium::PresentStateTransition ok = atrium::ResolvePresentTransition(S_OK, busy.hasPresented, true, true);
    Require(!ok.ready, "successful retry must consume ready_ = false");
    Require(ok.hasPresented, "successful retry must mark hasPresented_ = true");
    Require(!ok.lastPresentBusy, "successful retry must clear lastPresentBusy");
    Require(ok.commitPresentedBackdrop, "successful retry must commit presented_ backdrop");
}

void TestBusyPreservesPreviousPresentedBackdrop()
{
    const atrium::PresentStateTransition busy = atrium::ResolvePresentTransition(DXGI_ERROR_WAS_STILL_DRAWING, true, true, true);
    Require(busy.ready, "busy Present on later frame must keep ready_ = true");
    Require(busy.hasPresented, "busy Present must preserve prior hasPresented_ = true");
    Require(busy.lastPresentBusy, "busy Present must report lastPresentBusy = true");
    Require(!busy.commitPresentedBackdrop, "busy Present must not overwrite presented_ backdrop with unqueued frame");

    const atrium::PresentStateTransition ok = atrium::ResolvePresentTransition(S_OK, busy.hasPresented, true, true);
    Require(!ok.ready && ok.hasPresented && !ok.lastPresentBusy && ok.commitPresentedBackdrop,
            "subsequent S_OK must commit the new presented_ backdrop and consume ready_");
}

void TestNonBusyErrorAndOverlayOnly()
{
    const atrium::PresentStateTransition lost = atrium::ResolvePresentTransition(DXGI_ERROR_DEVICE_REMOVED, true, true, true);
    Require(!lost.ready && lost.hasPresented && !lost.lastPresentBusy && !lost.commitPresentedBackdrop,
            "device error must clear ready/busy and leave presented_ untouched");

    const atrium::PresentStateTransition overlayOnly = atrium::ResolvePresentTransition(S_OK, true, true, false);
    Require(!overlayOnly.ready && overlayOnly.hasPresented && !overlayOnly.lastPresentBusy && !overlayOnly.commitPresentedBackdrop,
            "overlay-only S_OK must preserve hasPresented_ without re-copying presented_");
}

void TestPresentReadyStallWatchdog()
{
    double stalledSince = 0;
    Require(!atrium::IsPresentReadyStalled(true, 100.0, stalledSince) && stalledSince == 0.0, "ready == true keeps stalledSinceMs reset");
    Require(!atrium::IsPresentReadyStalled(false, 200.0, stalledSince) && stalledSince == 200.0, "first !ready records stall start without tripping");
    Require(!atrium::IsPresentReadyStalled(false, 1199.0, stalledSince), "!ready before 1000 ms does not trip watchdog");
    Require(atrium::IsPresentReadyStalled(false, 1200.0, stalledSince), "!ready at >= 1000 ms trips stall watchdog");
    Require(!atrium::IsPresentReadyStalled(true, 1300.0, stalledSince) && stalledSince == 0.0, "subsequent ready == true resets stalledSinceMs");
}

} // namespace

int main()
{
    TestBusyOnFirstFrameThenSucceeds();
    TestBusyPreservesPreviousPresentedBackdrop();
    TestNonBusyErrorAndOverlayOnly();
    TestPresentReadyStallWatchdog();
    if (g_failures > 0)
        return 1;
    std::printf("glass present retry: all checks passed\n");
    return 0;
}
