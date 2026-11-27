#include "shell/tray_click.h"
#include "shell/tray_image.h"
#include "shell/tray_model.h"
#include "shell/tray_target.h"

#include <iostream>
#include <vector>

using namespace atrium::tray;

namespace {
bool g_failed = false;

void Check(bool value, const char* message)
{
    if (!value) {
        std::cerr << message << '\n';
        g_failed = true;
    }
}

void TestMenuFollow()
{
    const RECT monitor{0, 0, 2160, 1440};
    const RECT workWithBar{0, 0, 2160, 1380};
    const POINT click{1615, 1185};
    const RECT wpsCard{1970, 1120, 2370, 1600};
    Check(ShouldMoveFollowedMenu(wpsCard, click, monitor, monitor, false), "off-screen WPS card is moved");
    const POINT placed = PlaceMenu(click, wpsCard.right - wpsCard.left, wpsCard.bottom - wpsCard.top, monitor);
    Check(placed.x == 1615 && placed.y == 705, "WPS card moves inside 2160x1440 screen next to click");
    Check(PlaceMenu({2100, 100}, 400, 480, monitor).x == 1700 && PlaceMenu({100, 100}, 400, 480, monitor).y == 100,
          "PlaceMenu flips left/below when near edges");
    Check(!ShouldMoveFollowedMenu({1600, 1120, 1800, 1448}, click, monitor, workWithBar, true),
          "native #32768 near pointer keeps 8 px shadow tolerance past monitor");
    Check(ShouldMoveFollowedMenu({1600, 1120, 1800, 1449}, click, monitor, workWithBar, true), "native #32768 extending >8 px past monitor is moved");
    Check(!ShouldMoveFollowedMenu({1600, 1120, 1800, 1388}, click, monitor, workWithBar, false),
          "custom popup near pointer keeps 8 px tolerance past work area");
    Check(ShouldMoveFollowedMenu({1600, 1120, 1800, 1389}, click, monitor, workWithBar, false), "custom popup extending >8 px past work area is moved");
    Check(ShouldMoveFollowedMenu({1600, 1120, 1800, 1441}, click, monitor, monitor, false), "custom popup extending past monitor is moved");
}
void TestClassicGesture()
{
    for (Action action : {Action::Left, Action::Right, Action::Double}) {
        const std::vector<MouseEvent> expected =
            action == Action::Right  ? std::vector{MouseEvent::Move, MouseEvent::RightDown, MouseEvent::RightUp} :
            action == Action::Double ? std::vector{MouseEvent::Move, MouseEvent::LeftDown, MouseEvent::LeftUp, MouseEvent::LeftDouble, MouseEvent::LeftUp} :
                                       std::vector{MouseEvent::Move, MouseEvent::LeftDown, MouseEvent::LeftUp};
        std::vector<MouseEvent> actual;
        auto posting = PostClassicGesture(action, [&](MouseEvent event) {
            actual.push_back(event);
            return true;
        });
        Check(actual == expected && posting.complete, "classic message order implements native gesture");
        for (size_t failure = 0; failure < expected.size(); ++failure) {
            size_t calls = 0;
            posting = PostClassicGesture(action, [&](MouseEvent) { return calls++ != failure; });
            Check(!posting.complete && posting.queued == failure && calls == failure + 1, "posting stops at first failed message");
            Check(posting.actionQueued == (failure > 1), "failure after button down has uncertain delivery");
        }
    }
}
void TestArtworkAndDispatch()
{
    int fallbackCalls = 0;
    auto art = LoadArtwork([] { return true; },
                           [&] {
                               ++fallbackCalls;
                               return true;
                           });
    Check(art == Artwork::Snapshot && fallbackCalls == 0, "valid tray state image cannot be replaced by branding");
    art = LoadArtwork([] { return false; },
                      [&] {
                          ++fallbackCalls;
                          return true;
                      });
    Check(art == Artwork::Application && fallbackCalls == 1, "missing or corrupt snapshot uses application fallback");
    auto fail = [](Route) { return int32_t(-1); };
    auto unseen = [](bool) { return false; };
    auto r = Dispatch(Action::Left, true, fail, unseen);
    Check(r.attempts.size() == 3 && r.attempts[1].route == Route::Toolbar && r.attempts[2].route == Route::LegacyDefault, "classic left fallback order");
    r = Dispatch(Action::Right, true, fail, unseen);
    Check(r.attempts.size() == 2 && r.attempts.back().route == Route::Toolbar, "classic right messages after failed context menu");
    r = Dispatch(Action::Left, false, [](Route) { return int32_t(0); }, unseen);
    Check(r.outcome == Outcome::AcceptedUnconfirmed && r.attempts.size() == 1, "S_OK without evidence is not success or safe to repeat");
    r = Dispatch(Action::Left, true, [](Route) { return kProviderTimeout; }, unseen);
    Check(r.outcome == Outcome::AcceptedUnconfirmed && r.attempts.size() == 1, "timeout does not prove the asynchronous action was cancelled");
    r = Dispatch(Action::Right, false, fail, [](bool) { return true; });
    Check(r.outcome == Outcome::Observed && r.attempts.size() == 1, "observed response wins even with provider error");
    r = Dispatch(Action::Double, false, fail, unseen);
    Check(r.attempts.empty() && r.outcome == Outcome::Unavailable, "XAML double click cannot be emulated by Invoke twice");
    r = Dispatch(
        Action::Double, true,
        [](Route route) {
            Check(route == Route::Toolbar, "classic double route");
            return int32_t(0);
        },
        [](bool) { return true; });
    Check(r.outcome == Outcome::Observed, "classic double response");
}

void TestTargetResolution()
{
    const ShellSession shell{42, 100, 200};
    TargetKey a{shell, {1, 2}, L"guid:a", L"a.exe", L"A"};
    TargetKey b{shell, {1, 3}, L"guid:b", L"b.exe", L"B"};
    auto target = ResolveTarget(a, shell, {b, a});
    Check(target.index == 1 && target.reason == Resolution::Runtime, "reordering preserves exact runtime target");
    auto changed = a;
    changed.tip = L"A - syncing";
    Check(ResolveTarget(a, shell, {changed}).index == 0, "live record and runtime allow changing status");
    changed.identity = b.identity;
    auto reassigned = b;
    reassigned.identity = a.identity;
    reassigned.exe = a.exe;
    Check(ResolveTarget(a, shell, {changed, reassigned}).reason == Resolution::Conflict, "record reassignment cannot redirect runtime click");
    Check(ResolveTarget(a, shell, {reassigned}).reason == Resolution::Missing, "missing runtime cannot jump to matching record");
    Check(ResolveTarget(a, shell, {a, a}).reason == Resolution::Ambiguous, "duplicate runtime fails closed");
    changed = a;
    changed.exe = b.exe;
    Check(ResolveTarget(a, shell, {changed}).reason == Resolution::Conflict, "executable metadata conflict fails closed");
    auto recordOnly = a;
    recordOnly.runtime.clear();
    Check(ResolveTarget(recordOnly, shell, {b, a}).index == 1, "initially unmaterialized icon resolves by live identity");
    auto restarted = shell;
    ++restarted.started;
    Check(ResolveTarget(a, restarted, {a}).reason == Resolution::ShellChanged, "PID reuse does not preserve Explorer instance");
    Check(ResolveTarget(recordOnly, restarted, {a}).reason == Resolution::ShellChanged, "record clicks also reject old shell snapshots");
    restarted = shell;
    ++restarted.window;
    Check(ResolveTarget(a, restarted, {a}).reason == Resolution::ShellChanged, "taskbar window replacement invalidates runtime");
    auto unknown = a;
    unknown.identity.clear();
    unknown.exe.clear();
    changed = unknown;
    changed.tip = L"Different app";
    Check(ResolveTarget(unknown, shell, {changed}).reason == Resolution::Conflict, "unidentified recycled runtime cannot change name silently");
    TargetKey program;
    program.exe = a.exe;
    Check(ResolveTarget(program, shell, {a}).reason == Resolution::Program, "explicit program activation needs unique candidate");
    Check(ResolveTarget(program, shell, {a, a}).reason == Resolution::Ambiguous, "same exe multiple icons is ambiguous");
    Check(SameGestureTarget(a, a), "same icon forms a double gesture");
    changed = a;
    changed.shell = restarted;
    Check(!SameGestureTarget(a, changed), "double gesture cannot span shell replacement");
    changed = a;
    changed.runtime = b.runtime;
    Check(!SameGestureTarget(a, changed), "record remapping cannot combine separate gestures");
    Check(!SameGestureTarget({}, {}), "unknown gestures are not identical");
    Check(StableOrder({a, b}, {b, a}) == std::vector<size_t>{1, 0}, "refresh preserves surviving icon order");
    auto c = a;
    c.identity = L"guid:c";
    c.runtime = {9};
    Check(StableOrder({a, b}, {c, b, a}) == std::vector<size_t>{2, 1, 0}, "new icons append after survivors");
    Check(StableOrder({a, b}, {c, b}) == std::vector<size_t>{1, 0}, "removed icons disappear without reordering survivors");
    Check(StableOrder({recordOnly, b}, {b, a}) == std::vector<size_t>{1, 0}, "materializing a record preserves its place");
}

void TestDoubleContinuation()
{
    auto unseen = [](bool) { return false; };
    std::vector<MouseEvent> continuation;
    PostClassicGesture(Action::DoubleContinuation, [&](MouseEvent e) {
        continuation.push_back(e);
        return true;
    });
    Check(continuation == std::vector{MouseEvent::Move, MouseEvent::LeftDouble, MouseEvent::LeftUp}, "double continuation never replays first click");
    int duplicateCalls = 0;
    auto r = Dispatch(
        Action::DoubleContinuation, false,
        [&](Route) {
            ++duplicateCalls;
            return 0;
        },
        unseen);
    Check(r.outcome == Outcome::AcceptedUnconfirmed && r.attempts.size() == 1 && r.attempts[0].route == Route::Invoke && duplicateCalls == 1,
          "XAML double continuation is one second Invoke, never a fallback");
    duplicateCalls = 0;
    r = Dispatch(
        Action::DoubleContinuation, false,
        [&](Route) {
            ++duplicateCalls;
            return -1;
        },
        unseen);
    Check(r.attempts.size() == 1 && duplicateCalls == 1, "a refused second Invoke is not retried through another route");
    r = Dispatch(Action::Double, true, [](Route) { return int32_t(1); }, unseen);
    Check(r.outcome == Outcome::AcceptedUnconfirmed && r.attempts.size() == 1, "partial toolbar delivery cannot replay a gesture");
}

void TestPointerRightFallbackAndActionTarget()
{
    auto unseen = [](bool) { return false; };
    auto seen = [](bool) { return true; };

    // #28: XAML ShowContextMenu returning S_OK (0) without observed popup permits physical pointer fallback.
    const auto xamlOk = Dispatch(Action::Right, false, [](Route) { return int32_t(0); }, unseen);
    Check(xamlOk.outcome == Outcome::AcceptedUnconfirmed && ShouldAttemptPointerRightFallback(xamlOk, false),
          "#28 XAML ShowContextMenu S_OK AcceptedUnconfirmed permits pointer right fallback");

    // Provider timeout must NOT repeat via physical pointer fallback.
    const auto timedOut = Dispatch(Action::Right, false, [](Route) { return kProviderTimeout; }, unseen);
    Check(timedOut.outcome == Outcome::AcceptedUnconfirmed && !ShouldAttemptPointerRightFallback(timedOut, false),
          "kProviderTimeout blocks pointer right fallback");

    // Classic toolbar S_FALSE (1) or classic=true blocks pointer right fallback.
    const auto sFalse = Dispatch(Action::Right, false, [](Route) { return int32_t(1); }, unseen);
    Check(sFalse.outcome == Outcome::AcceptedUnconfirmed && !ShouldAttemptPointerRightFallback(sFalse, false), "S_FALSE blocks pointer right fallback");
    Check(!ShouldAttemptPointerRightFallback(xamlOk, true), "classic toolbar blocks pointer right fallback");

    // Observed menu evidence blocks pointer right fallback.
    const auto observed = Dispatch(Action::Right, false, [](Route) { return int32_t(0); }, seen);
    Check(observed.outcome == Outcome::Observed && !ShouldAttemptPointerRightFallback(observed, false), "Observed outcome blocks pointer right fallback");

    // Unavailable permits fallback on XAML.
    const auto unavail = Dispatch(Action::Right, false, [](Route) { return int32_t(-1); }, unseen);
    Check(unavail.outcome == Outcome::Unavailable && ShouldAttemptPointerRightFallback(unavail, false),
          "Unavailable XAML right-click permits pointer fallback");

    // TrayIcon::ActionTarget() vs Target() separation.
    const ShellSession shell{42, 100, 200};
    atrium::TrayIcon icon;
    icon.tip = L"App";
    icon.exe = L"app.exe";
    icon.hostKey = L"hwnd:1:1";
    icon.recordId = L"hosted:1";
    icon.runtimeId = {10};
    icon.shell = shell;
    Check(icon.ActionTarget().identity == L"hosted:1" && icon.ActionTarget().runtime == std::vector<int>{10},
          "ActionTarget falls back to Target when explorerTarget is unset");

    icon.explorerTarget = TargetKey{shell, {20, 30}, L"uid:2:2:2", L"app.exe", L"App"};
    Check(icon.Target().identity == L"hosted:1" && icon.Target().runtime == std::vector<int>{10},
          "Target preserves original icon identity when explorerTarget is set");
    Check(icon.ActionTarget().identity == L"uid:2:2:2" && icon.ActionTarget().runtime == std::vector<int>{20, 30},
          "ActionTarget returns strict Explorer binding when explorerTarget is set");

    Check(!ShouldAllowExeForegroundFallback(false), "ordinary tray clicks never fall back to ForegroundExisting by exe");
    Check(ShouldAllowExeForegroundFallback(true), "Dock OpenExisting retains exe foreground wake fallback");
}

void TestRecordPidAndPopupPolicies()
{
    Check(RecordPid(L"uid:14288:131158:1051") == 14288, "RecordPid parses numeric PID from uid identity");
    Check(RecordPid(L"uid:0:1:2") == 0, "RecordPid rejects zero PID");
    Check(RecordPid(L"guid:{7120A42D-CB02-4A51-B3B5-58E36E8B14E1}") == 0, "RecordPid ignores guid identities");
    Check(RecordPid(L"uid:abc:1:2") == 0, "RecordPid rejects non-numeric PID");
    Check(RecordPid(L"uid:999999999999999:1:2") == 0, "RecordPid rejects overflowing PID");
    Check(RecordPid(L"uid:42") == 0, "RecordPid rejects malformed uid without second colon");

    Check(ShouldObserveUnattributedMenu(true, false, false, false), "native #32768 menu is always observed");
    Check(ShouldObserveUnattributedMenu(false, true, true, true), "elevated access-denied popup on right-click is observed");
    Check(!ShouldObserveUnattributedMenu(false, true, false, true), "elevated popup is ignored for non-right clicks");
    Check(!ShouldObserveUnattributedMenu(false, true, true, false), "non-popup window is ignored even when access is denied");

    const RECT work{0, 0, 3200, 1920};
    Check(IsHoverPopupStyle(WS_POPUP, 0, false, {100, 100, 420, 480}, work), "frameless WS_POPUP hover window accepted");
    Check(IsHoverPopupStyle(WS_POPUP, WS_EX_TOOLWINDOW | WS_EX_TOPMOST, false, {100, 100, 500, 400}, work), "toolwindow hover popup accepted");
    Check(IsHoverPopupStyle(WS_POPUP, 0, true, {100, 100, 300, 300}, work), "native #32768 accepted");
    Check(!IsHoverPopupStyle(WS_POPUP | WS_MINIMIZEBOX, 0, false, {100, 100, 400, 300}, work), "window with minimize button rejected");
    Check(!IsHoverPopupStyle(WS_POPUP, WS_EX_APPWINDOW, false, {100, 100, 400, 300}, work), "app window rejected");
    Check(!IsHoverPopupStyle(WS_POPUP, 0, false, {0, 0, 2200, 1600}, work), "oversized window rejected as hover popup");
}
} // namespace

int main()
{
    TestArtworkAndDispatch();
    TestTargetResolution();
    TestDoubleContinuation();
    TestClassicGesture();
    TestMenuFollow();
    TestPointerRightFallbackAndActionTarget();
    TestRecordPidAndPopupPolicies();
    if (g_failed)
        return 1;
    std::cout << "tray dispatch: all scenarios passed\n";
    return 0;
}
