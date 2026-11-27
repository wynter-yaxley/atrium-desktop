#include "ui/stage_merge.h"
#include "ui/stage_scroll.h"
#include "ui/stage_fade.h"
#include "ui/stage_open.h"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace atrium;

namespace {
bool g_failed = false;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        g_failed = true;
    }
}

void CheckMerge()
{
    Require(StageEntryAt({}, 80, 100) == -1, "empty strip hits nothing");
    const std::vector<D2D1_RECT_F> left = {StageBoxRect(kStageStripW, 40, false), StageBoxRect(kStageStripW, 40 + kStageBoxH + kStageSlotGap, false)};
    Require(StageEntryAt(left, 100, 90) == 0 && StageEntryAt(left, 100, 240) == 1, "centres hit their strip cards");
    Require(StageEntryAt(left, left[0].left - 14, left[0].bottom + 23.5f) == 0, "icon overhang padding still hits its card");
    Require(StageEntryAt(left, 1, 90) == -1 && StageEntryAt(left, 100, left[0].bottom + 25) == -1, "snap edge and slot gap hit nothing");
    const std::vector<D2D1_RECT_F> right = {StageBoxRect(kStageStripW, 40, true)};
    Require(StageEntryAt(right, right[0].left + 20, 90) == 0 && StageEntryAt(right, kStageStripW - 1, 90) == -1, "right strip hits card, not snap edge");
    RECT frame{10, 100, 710, 600};
    Require(StageBesideRect(frame, RECT{0, 50, 238, 900}, RECT{0, 0, 1920, 1040}, false) && frame.left == 238 && frame.right == 938,
            "window dropped over left strip shifts beside it");

    using Set = std::vector<std::wstring>;
    Set stage = {L"notepad.exe|1"};
    std::vector<Set> recent = {{L"calc.exe|2"}, {L"msedge.exe|3", L"code.exe|4"}};
    Require(StageMergeGroups(stage, recent, DefaultStageExcluded(), L"notepad.exe|1", {L"code.exe|4", L"msedge.exe|3"}) &&
                stage == Set{L"msedge.exe|3", L"code.exe|4", L"notepad.exe|1"} && recent == std::vector<Set>{{L"calc.exe|2"}},
            "lone stage window joins target group and emptied source disappears");

    stage = {L"a.exe|1", L"b.exe|2"};
    recent = {{L"c.exe|3"}, {L"d.exe|4"}};
    Require(StageMergeGroups(stage, recent, {}, L"a.exe|1", {L"d.exe|4"}) && stage == Set{L"d.exe|4", L"a.exe|1"} &&
                recent == std::vector<Set>{{L"b.exe|2"}, {L"c.exe|3"}},
            "multi-window stage parks remaining window at front of strip");

    stage = {L"front.exe|9"};
    recent = {{L"lone.exe|5"}, {L"pair.exe|6", L"pair.exe|7"}, {L"dest.exe|8"}};
    Require(StageMergeGroups(stage, recent, {}, L"lone.exe|5", {L"dest.exe|8"}) && stage == Set{L"dest.exe|8", L"lone.exe|5"} &&
                recent == std::vector<Set>{{L"front.exe|9"}, {L"pair.exe|6", L"pair.exe|7"}},
            "prior active group parks at front and emptied strip group goes away");

    const Set keepStage = stage;
    const std::vector<Set> keepRecent = recent;
    Require(!StageMergeGroups(stage, recent, {L"wechat.exe"}, L"d:\\apps\\wechat\\wechat.exe|10", {L"front.exe|9"}), "stageExcluded window never merges");
    Require(!StageMergeGroups(stage, recent, DefaultStageExcluded(), L"c:\\windows\\system32\\snippingtool.exe|11", {L"front.exe|9"}),
            "default excluded screenshot window never merges");
    Require(!StageMergeGroups(stage, recent, {}, L"other.exe|12", {L"missing.exe|99"}), "unknown target changes nothing");
    Require(!StageMergeGroups(stage, recent, {}, L"front.exe|9", {L"front.exe|9"}), "self-only group cannot merge into itself");
    Require(stage == keepStage && recent == keepRecent, "rejected merges leave stage and strip groups untouched");
}

void CheckFade()
{
    Require(StageFadeOpacity(0) == 1 && StageFadeOpacity(1) == 0 && StageFadeBlur(0) == 0 && StageFadeBlur(1) == 1, "the fade's ends");
    for (int i = 0; i < 20; ++i) {
        const float v = float(i) / 20, next = float(i + 1) / 20;
        Require(StageFadeOpacity(next) <= StageFadeOpacity(v) && StageFadeBlur(next) >= StageFadeBlur(v), "blurs more and fades more along");
    }
    IconImage white{40, 40, std::vector<uint8_t>(40 * 40 * 4, 255)};
    const StageFade f = StageFadeFor(40, 40);
    Require(f.width == 40 && f.margin > f.maxBlur && f.maxBlur > 0, "a small picture kept at its size, room for the blur");
    const IconImage down = FadeAway(white, 0, 40, f, false), up = FadeAway(white, 0, 40, f, true);
    const int pad = (down.width - 40) / 2;
    auto alpha = [&](const IconImage& i, int y) { return i.Data()[(size_t(y + pad) * size_t(i.width) + size_t(i.width / 2)) * 4 + 3]; };
    Require(down.width == 40 + 2 * pad && down.height == down.width, "the margin around");
    Require(alpha(down, 2) > 245 && alpha(down, 39) < 10 && alpha(down, 20) > alpha(down, 32), "away down: whole at the top, gone at the bottom");
    Require(alpha(up, 37) > 245 && alpha(up, 0) < 10, "away up: the other way");
    Require(alpha(down, -pad) == 0, "nothing reaches the margin's far edge");
}

void CheckStageScrollAndMerge()
{
    auto require = Require;
    StageScroll scroll;
    require(!scroll.Wheel(-120, false, 0) && scroll.Offset() == 0, "empty strip does not scroll");
    require(!scroll.Wheel(-120, false, 1), "one group does not scroll");
    require(scroll.Wheel(-120, false, 10) && scroll.Offset() == 1, "wheel down advances beyond the visible page");
    require(scroll.Index(5, 10) == 6, "viewport indexes the entire list, not just six cards");
    require(scroll.Wheel(240, false, 10) && scroll.Offset() == 9, "wheel up wraps past the start");
    require(scroll.Index(1, 10) == 0, "viewport wraps at the end");
    scroll.Reset();
    for (int i = 0; i < 3; ++i)
        require(!scroll.Wheel(-30, false, 8), "partial notches must accumulate");
    require(scroll.Wheel(-30, false, 8) && scroll.Offset() == 1, "four precision deltas equal one notch");
    require(!scroll.Wheel(30, false, 8), "opposite partial delta waits");
    require(!scroll.Wheel(-30, false, 8), "reversing partial input cancels its remainder");
    require(scroll.Wheel(120, true, 8) && scroll.Offset() == 2, "horizontal right advances");
    require(scroll.Wheel(-360, true, 8) && scroll.Offset() == 7, "horizontal left wraps");
    scroll.Fit(3);
    require(scroll.Offset() == 1, "shrinking list normalizes offset");
    scroll.Anchor(2, 3);
    require(scroll.Index(0, 3) == 2, "rebuild preserves the first visible identity");
    scroll.Reset();
    require(scroll.Offset() == 0 && !scroll.Wheel(-90, false, 3), "reset clears both offset and fractional input");
    require(scroll.Wheel(-32760, false, 8) && scroll.Offset() < 8, "large wheel input stays bounded");
    scroll.Fit(0);
    require(scroll.Offset() == 0 && scroll.Index(4, 0) == 0, "empty rebuild is safe");

    // Non-wrapping strip scrolling (wrap = false).
    require(scroll.Wrap(), "StageScroll wraps by default");
    scroll.SetWrap(false);
    require(!scroll.Wrap(), "SetWrap(false) disables cyclic wraparound");
    require(!scroll.Wheel(-120, false, 4, 4) && scroll.Offset() == 0, "strip fitting within capacity does not scroll");
    require(!scroll.Wheel(120, false, 2, 4) && scroll.Offset() == 0, "fewer cards than capacity never scroll");
    require(!scroll.Wheel(120, false, 6, 4) && scroll.Offset() == 0, "wheel up at start stops at 0");
    require(scroll.Wheel(-120, false, 6, 4) && scroll.Offset() == 1, "wheel down advances when count exceeds capacity");
    require(scroll.Wheel(-120, false, 6, 4) && scroll.Offset() == 2, "wheel down reaches count - capacity");
    require(!scroll.Wheel(-120, false, 6, 4) && scroll.Offset() == 2, "wheel down at end stops at count - capacity");
    require(!scroll.Wheel(120, true, 6, 4) && scroll.Offset() == 2, "horizontal right at end stops at count - capacity");
    require(scroll.Wheel(-120, true, 6, 4) && scroll.Offset() == 1, "horizontal left moves back from end");
    require(scroll.Wheel(-120, true, 6, 4) && scroll.Offset() == 0, "horizontal left reaches 0");
    require(!scroll.Wheel(-120, true, 6, 4) && scroll.Offset() == 0, "horizontal left at start stops at 0");

    // Boundary remainder clears so reversing responds on the next notch.
    require(!scroll.Wheel(60, false, 6, 4) && scroll.Offset() == 0, "partial upward overflow at start is clamped");
    require(scroll.Wheel(-120, false, 6, 4) && scroll.Offset() == 1, "single downward notch responds immediately after start overflow");
    require(scroll.Wheel(-120, false, 6, 4) && scroll.Offset() == 2, "reaches end before end-overflow check");
    require(!scroll.Wheel(-60, false, 6, 4) && scroll.Offset() == 2, "partial downward overflow at end is clamped");
    require(scroll.Wheel(120, false, 6, 4) && scroll.Offset() == 1, "single upward notch responds immediately after end overflow");
    scroll.Reset();
    require(!scroll.Wheel(-60, false, 6, 4) && scroll.Offset() == 0, "partial downward delta away from start waits");
    require(scroll.Wheel(-60, false, 6, 4) && scroll.Offset() == 1, "second partial downward delta away from start completes notch");
    scroll.Reset();
    require(!scroll.Wheel(-60, true, 6, 4) && scroll.Offset() == 0, "partial horizontal left overflow at start is clamped");
    require(scroll.Wheel(120, true, 6, 4) && scroll.Offset() == 1, "single horizontal right notch responds immediately");
    scroll.Reset();
    require(scroll.Wheel(120, true, 5, 4) && scroll.Offset() == 1, "horizontal right reaches end (5 - 4 = 1)");
    require(!scroll.Wheel(60, true, 5, 4) && scroll.Offset() == 1, "partial horizontal right overflow at end is clamped");
    require(scroll.Wheel(-120, true, 5, 4) && scroll.Offset() == 0, "single horizontal left notch responds immediately after end overflow");

    // Fit and Anchor clamp when wrap = false, and SetWrap(true -> false) clamps overflow.
    scroll.Anchor(5, 6, 4);
    require(scroll.Offset() == 2, "Anchor clamps to count - capacity when wrap is off");
    scroll.Fit(5, 4);
    require(scroll.Offset() == 1, "Fit clamps when count shrinks");
    scroll.Fit(5, 6);
    require(scroll.Offset() == 0, "Fit clamps to 0 when capacity grows past count");
    scroll.Anchor(2, 0, 4);
    require(scroll.Offset() == 0, "Anchor resets to 0 on empty strip");
    scroll.SetWrap(true);
    scroll.Anchor(5, 6, 4);
    require(scroll.Offset() == 5, "Anchor allows offset past count - capacity while wrap is on");
    scroll.SetWrap(false);
    scroll.Fit(6, 4);
    require(scroll.Offset() == 2, "SetWrap(false) followed by Fit clamps prior cyclic offset");
    CheckMerge();
}

} // namespace

int main()
{
    CheckFade();
    auto require = Require;
    StageOpenIntent intent;
    intent.Arm(L"editor", 100);
    require(!intent.Matches(L"browser", 101), "a launch intent never targets another app");
    require(intent.Matches(L"editor", 10099), "the requested app joins when its window arrives");
    require(!intent.Matches(L"editor", 10100), "a stale launch intent expires");
    intent.Arm(L"editor", 20000);
    intent.Cancel();
    require(!intent.Matches(L"editor", 20001), "disable or stage switching cancels a launch intent");
    CheckStageScrollAndMerge();

    // Park predicate: disabled windows (e.g. blocked by a modal dialog) stay where they are.
    require(ShouldParkStageWindow(false, true, false, true, false, false, true), "enabled non-transient window on monitor parks");
    require(!ShouldParkStageWindow(false, true, false, true, false, false, false), "disabled window is never parked");
    require(!ShouldParkStageWindow(true, true, false, true, false, false, true) && !ShouldParkStageWindow(false, false, false, true, false, false, true) &&
                !ShouldParkStageWindow(false, true, true, true, false, false, true) && !ShouldParkStageWindow(false, true, false, false, false, false, true) &&
                !ShouldParkStageWindow(false, true, false, true, true, false, true) && !ShouldParkStageWindow(false, true, false, true, false, true, true),
            "parked, hidden, iconic, off-monitor, transient, and fullscreen windows do not park");

    // Adopt / wait / drop decisions for stage windows.
    require(DecideStageAdopt(false, true, false, true, false, false) == StageAdoptAction::Up, "visible non-iconic on monitor stays up");
    require(DecideStageAdopt(false, true, false, false, true, false) == StageAdoptAction::Drop, "non-iconic moved off monitor is dropped");
    require(DecideStageAdopt(false, true, true, false, true, false) == StageAdoptAction::Adopt, "minimized in up_ at -32000 off-monitor is adopted");
    require(DecideStageAdopt(false, true, true, false, false, false) == StageAdoptAction::Adopt, "minimized stage window that missed up_ is adopted");
    require(DecideStageAdopt(false, true, true, false, false, true) == StageAdoptAction::Wait, "minimized window still restoring waits");
    require(ShouldWaitStageRestore(true, 5000, 2000, true), "outbound strip flight keeps waiting beyond settle timeout");
    require(ShouldWaitStageRestore(false, 0, 2000, true), "outbound strip flight waits even without prior restoring entry");
    require(DecideStageAdopt(false, true, true, false, false, ShouldWaitStageRestore(true, 100, 2000, false)) == StageAdoptAction::Wait,
            "post-flight restoring window within settle window waits");
    require(DecideStageAdopt(false, true, true, false, false, ShouldWaitStageRestore(true, 2001, 2000, false)) == StageAdoptAction::Adopt,
            "restoring window exceeding settle window after flight ends is adopted");
    require(!ShouldWaitStageRestore(false, 0, 2000, false), "inbound flight does not suppress adoption when not restoring");
    require(DecideStageAdopt(true, true, true, true, true, false) == StageAdoptAction::Drop, "already parked window is skipped");
    require(DecideStageAdopt(false, false, true, true, true, false) == StageAdoptAction::Drop, "hidden window is skipped");

    // First-picture short delay and failed/blank retry backoff.
    require(!ShouldTakeStagePicture(false, false, 0, 299, ULLONG_MAX), "first picture waits 300 ms away from caption buttons");
    require(ShouldTakeStagePicture(false, false, 0, 300, ULLONG_MAX), "first picture fires at 300 ms");
    require(!ShouldTakeStagePicture(false, true, 0, 199, ULLONG_MAX), "first picture waits 200 ms on caption buttons");
    require(ShouldTakeStagePicture(false, true, 0, 200, ULLONG_MAX), "first picture fires at 200 ms on caption buttons");
    require(!ShouldTakeStagePicture(false, true, 0, 400, 100), "in-flight first picture does not re-request immediately");
    require(!ShouldTakeStagePicture(false, false, 1, 1000, 699) && ShouldTakeStagePicture(false, false, 1, 1000, 700), "first blank retries after 700 ms");
    require(!ShouldTakeStagePicture(false, false, 2, 3000, 1499) && ShouldTakeStagePicture(false, false, 2, 3000, 1500), "second blank retries after 1500 ms");
    require(!ShouldTakeStagePicture(false, false, 3, 9000, 4999) && ShouldTakeStagePicture(false, false, 3, 9000, 5000), "further blanks cap at 5000 ms");
    require(!ShouldTakeStagePicture(true, true, 0, 1499, 5000), "refresh with picture still waits 1500 ms settle");
    require(!ShouldTakeStagePicture(true, false, 0, 2000, 19999) && ShouldTakeStagePicture(true, false, 0, 2000, 20000),
            "steady refresh away from buttons is 20 s");
    require(!ShouldTakeStagePicture(true, true, 0, 2000, 1499) && ShouldTakeStagePicture(true, true, 0, 2000, 1500), "steady refresh on buttons is 1500 ms");

    if (g_failed)
        return 1;
    std::cout << "PASS: cyclic Stage scrolling, strip hit-testing and group merges\n";
    return 0;
}
