#include "ui/ime_bubble_model.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace atrium;

// The bubble's melt rides in the paint's own parts budget: one shape carries at
// most six parts (ime_bubble.cpp publishes the skin, and every group of drops,
// as such a shape), so the model must never ask for more.
static_assert(ImeBubbleModel::kMaxMeltParts == 6, "the bubble's melt fits the paint's parts");

int main()
{
    int failures = 0;
    auto require = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    };

    run("the bubble swells in from below with a springy overshoot", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const auto emerging = bubble.Step(1030);
        require(emerging.active && emerging.animating, "the announce is running");
        require(emerging.opacity > 0 && emerging.opacity < 1, "the bubble fades in");
        require(emerging.cy > ImeBubbleModel::kCyRest && emerging.scale < 1, "it starts below its rest, small");
        const auto sprung = bubble.Step(1000 + 300);
        require(sprung.scale > 1, "the emerge passes its end by a little before settling");
    });
    run("the float drifts up gently while fully visible", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const auto rested = bubble.Step(1000 + ImeBubbleModel::kEmergeMs + 1);
        require(std::abs(rested.cy - ImeBubbleModel::kCyRest) < 0.5f, "the emerge ends at the resting place");
        require(rested.opacity == 1 && std::abs(rested.scale - 1) < 0.02f, "a settled bubble is whole");
        const auto floated = bubble.Step(1000 + ImeBubbleModel::kPopAt - 1);
        require(floated.cy < ImeBubbleModel::kCyRest - 5, "the float rises the rest of the way");
        require(floated.beadCount == 0, "nothing bursts early");
    });
    run("the pop tears the skin into foam and mist", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const auto burst = bubble.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 10);
        require(burst.beadCount >= ImeBubbleModel::kMinBeads && burst.beadCount <= ImeBubbleModel::kMaxBeads, "every pop has its own number of drops");
        require(burst.foamCount >= 5 && burst.foamCount < burst.beadCount, "drops lead, mist follows");
        require(burst.beadOpacity > 0.9f, "the foam starts fully visible");
        require(burst.opacity < 1 && burst.opacity > 0, "the bubble's skin is leaving");
        require(burst.scale > 1.15f, "the burst inflates the skin as it goes");
        // The drops leave the rim: all of them real, none grown yet.
        for (int i = 0; i < burst.beadCount; ++i)
            require(burst.beads[i].r > 0, "every drop of the foam is a real one");
        const auto flying = bubble.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 40);
        const float by = ImeBubbleModel::kCyRest - ImeBubbleModel::kFloatRise;
        int above = 0;
        for (int i = 0; i < flying.beadCount; ++i)
            above += flying.beads[i].y < by - 1;
        require(above >= 3, "the foam scatters upwards, like a bubble breaking");
    });
    run("every pop of its own pattern", [&] {
        ImeBubbleModel first, second;
        first.Begin(1000);
        second.Begin(3100); // another seed: another burst
        const auto a = first.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 60);
        const auto b = second.Step(3100 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 60);
        bool same = a.beadCount == b.beadCount && a.foamCount == b.foamCount;
        for (int i = 0; same && i < a.beadCount; ++i)
            same = std::abs(a.beads[i].x - b.beads[i].x) < 0.5f && std::abs(a.beads[i].y - b.beads[i].y) < 0.5f;
        require(!same, "two pops cannot share one frozen pattern");
    });
    run("the whole life stays inside the small fixed window", [&] {
        for (int seed = 0; seed < 3; ++seed) { // several patterns must all fit
            const double begin = 1000 + seed * 7919;
            ImeBubbleModel bubble;
            bubble.Begin(begin);
            for (double t = begin + 8; t < bubble.EndMs(); t += 8) {
                const auto f = bubble.Step(t);
                require(f.active && f.animating, "every mid-life frame animates");
                require(std::isfinite(f.cx) && std::isfinite(f.cy) && std::isfinite(f.scale) && std::isfinite(f.opacity) && f.opacity >= 0 && f.opacity <= 1 &&
                            f.scale >= 0.4f && f.scale <= 1.7f,
                        "the bubble stays visible and bounded");
                const float r = ImeBubbleModel::kRadius * f.scale;
                require(f.cy - r >= 0 && f.cy + r <= ImeBubbleModel::kH, "the skin never leaves the window");
                for (int i = 0; i < f.beadCount; ++i) {
                    const auto& bead = f.beads[i];
                    require(std::isfinite(bead.x) && std::isfinite(bead.y) && std::isfinite(bead.r) && bead.r >= 0,
                            "every bead is a real drop, or one that has thinned away");
                    require(bead.x - bead.r >= 0 && bead.x + bead.r <= ImeBubbleModel::kW && bead.y - bead.r >= 0 && bead.y + bead.r <= ImeBubbleModel::kH,
                            "the burst stays inside the window");
                }
            }
        }
    });
    run("the announce ends bounded, and a restart starts clean", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(!bubble.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + ImeBubbleModel::kBurstMs + 1).active,
                "the bubble is done after its total");
        require(!bubble.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + ImeBubbleModel::kBurstMs + 500).animating, "it never animates again");
        bubble.Begin(3000);
        const auto fresh = bubble.Step(3050);
        require(fresh.active && fresh.opacity < 1 && fresh.cy > ImeBubbleModel::kCyRest, "a restarted bubble emerges like the first");
        bubble.Cancel();
        require(!bubble.Step(3100).active, "cancel removes it at once");
    });
    run("a late frame lands where a punctual one would", [&] {
        ImeBubbleModel slow, jumped;
        slow.Begin(1000);
        jumped.Begin(1000);
        const double at = 1000 + 617;
        for (double t = 1008; t < at; t += 8)
            slow.Step(t);
        const auto a = slow.Step(at), b = jumped.Step(at);
        require(std::abs(a.cy - b.cy) < 0.01f && std::abs(a.scale - b.scale) < 0.01f && std::abs(a.opacity - b.opacity) < 0.01f && a.beadCount == b.beadCount,
                "no integration state: the frame is a pure function of time");
        for (int i = 0; i < a.beadCount; ++i)
            require(std::abs(a.beads[i].x - b.beads[i].x) < 0.01f && std::abs(a.beads[i].y - b.beads[i].y) < 0.01f,
                    "the drawn burst pattern is the same for late and punctual frames");
    });
    run("a late frame lands where a punctual one would, even mid-merge", [&] {
        ImeBubbleModel slow, jumped;
        slow.Begin(1000);
        jumped.Begin(1000);
        slow.Fuse(1200);
        jumped.Fuse(1200);
        slow.Fuse(1300);
        jumped.Fuse(1300);
        const double at = 1700; // both newcomers in flight, both already touched
        for (double t = 1008; t < at; t += 8)
            slow.Step(t);
        const auto a = slow.Step(at), b = jumped.Step(at);
        require(a.lobeCount == 2 && b.lobeCount == 2, "both rising newcomers are in the frame");
        for (int i = 0; i < a.lobeCount; ++i)
            require(std::abs(a.lobes[i].x - b.lobes[i].x) < 0.01f && std::abs(a.lobes[i].y - b.lobes[i].y) < 0.01f &&
                        std::abs(a.lobes[i].r - b.lobes[i].r) < 0.01f && std::abs(a.lobes[i].u - b.lobes[i].u) < 0.0001f,
                    "each rising newcomer is the same");
        require(std::abs(a.grow - b.grow) < 0.01f && std::abs(a.wob - b.wob) < 0.01f && std::abs(a.scale - b.scale) < 0.01f,
                "the size and wobble are the same");
        require(a.beadCount == b.beadCount, "no burst comes from integrating");
        for (int i = 0; i < a.beadCount; ++i)
            require(std::abs(a.beads[i].x - b.beads[i].x) < 0.01f && std::abs(a.beads[i].y - b.beads[i].y) < 0.01f, "any drawn drops are the same");
    });
    run("merges grow the bubble logarithmically and stop at ten", [&] {
        const ImeBubbleModel defaults{};
        require(std::abs(defaults.ScaleAt(1) - 1.f) < 1e-6f, "one bubble is the nominal size");
        require(std::abs(defaults.RadiusAt(1) - ImeBubbleModel::kRadius) < 1e-6f, "at the start radius, in DIP");
        float last = defaults.ScaleAt(1);
        for (int x = 2; x <= ImeBubbleModel::kMaxFusions; ++x) {
            const float s = defaults.ScaleAt(x);
            require(s > last, "every merge is larger than the last");
            last = s;
        }
        require(std::abs(defaults.RadiusAt(ImeBubbleModel::kMaxFusions) - 51) < 1e-4f &&
                    std::abs(defaults.ScaleAt(ImeBubbleModel::kMaxFusions) - 51.f / ImeBubbleModel::kRadius) < 1e-5f,
                "ten merges reach the default end radius: 51/32 of the base");
        require(defaults.ScaleAt(14) == defaults.ScaleAt(ImeBubbleModel::kMaxFusions), "past ten the size stays");
        require(defaults.ScaleAt(0) == defaults.ScaleAt(1), "the count clamps at one");
        require(defaults.ScaleAt(2) - defaults.ScaleAt(1) > defaults.ScaleAt(ImeBubbleModel::kMaxFusions) - defaults.ScaleAt(ImeBubbleModel::kMaxFusions - 1),
                "the growth tapers, as a logarithmic size must");
    });
    run("the constants keep the old sizes, and the letter rides the frame's scale", [&] {
        // The two sizes are the original feature's: a 32-DIP first bubble and a
        // 51-DIP ten-merge one, and the start is the nominal radius, so the
        // first bubble's scale is 1. The glyph is drawn inside the content
        // group that the frame's scale carries, so the letter grows with the
        // bubble: it opens at the start size, ten merges reach the end size.
        static_assert(ImeBubbleModel::kStartRadius == 32.f && ImeBubbleModel::kEndRadius == 51.f, "the original sizes");
        static_assert(ImeBubbleModel::kStartRadius == ImeBubbleModel::kRadius, "the first bubble is the nominal one");
        static_assert(ImeBubbleModel::kEndRadius > ImeBubbleModel::kStartRadius, "a bubble grows or stays, never shrinks");
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const auto opened = bubble.Step(1000 + ImeBubbleModel::kEmergeMs + 1);
        require(std::abs(opened.scale - 1.f) < 0.02f, "an unmerged frame rides the start size");
        for (int i = 0; i < ImeBubbleModel::kMaxFusions; ++i)
            require(bubble.Fuse(1150 + i * 120.0), "every switch merges");
        const auto grown = bubble.Step(bubble.PopAt() - 1);
        require(std::abs(grown.grow - ImeBubbleModel::kEndRadius / ImeBubbleModel::kRadius) < 0.02f, "ten merges ride the end size");
    });
    run("the merge feel and the life are the model's own, and a restart keeps them", [&] {
        // The rebound is the old spring: ~31 % over its target, about 1.31 of
        // the way at 196 ms, all but settled by 900 ms — and a tiny rebound
        // barely passes it, so the curve really follows the constant.
        require(std::abs(ImeBubbleModel::softBounce(196, ImeBubbleModel::kRebound) - 1.308f) < 0.01f, "the bounce matches the old one");
        require(ImeBubbleModel::softBounce(900, ImeBubbleModel::kRebound) < 1.01f, "and has all but settled by 900 ms");
        require(ImeBubbleModel::softBounce(196, 0.05f) < 1.10f, "a tiny rebound overshoots by under ten per cent");
        // The feel, life and size are those constants.
        static_assert(ImeBubbleModel::kCompression == 0.10f && ImeBubbleModel::kRebound == 0.31f && ImeBubbleModel::kWobbleAmp == 0.875f &&
                          ImeBubbleModel::kWobbleHz == 4.3f && ImeBubbleModel::kLifeMs == 1060.f,
                      "the defaults are the original feel and life");
        ImeBubbleModel plain;
        plain.Begin(1000);
        require(std::abs(plain.EndMs() - (1000 + 1060 + 46 + 440)) < 0.5, "the life is the old one");
        plain.Begin(2500); // a restart is the same bubble again
        require(std::abs(plain.EndMs() - (2500 + 1060 + 46 + 440)) < 0.5, "Begin keeps the life");
        plain.Cancel();
        plain.Begin(4000);
        require(std::abs(plain.EndMs() - (4000 + 1060 + 46 + 440)) < 0.5, "Cancel keeps the life");

        // The hit squashes the skin by kCompression, deepest at the touch: the
        // frame's scale against its own grow (the size without the hit) is the
        // hit's curve times the float's breath, which is all but one.
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(bubble.Fuse(1200), "it merges");
        const double hit = 1200 + bubble.TouchMs();
        const auto at = bubble.Step(hit);
        require(std::abs(at.scale / at.grow - (1 - ImeBubbleModel::kCompression)) < 0.02f, "the compression is the constant's");
        require(std::abs(bubble.Step(hit + 60).wob) <= 1.15f * ImeBubbleModel::kWobbleAmp, "and the wobble stays inside its own bound");
    });
    run("the birth dips and the newcomer's three acts are the model's", [&] {
        // The original feel: the first bubble 18 DIP below its rest, a newcomer
        // 10 DIP below the window's bottom, and an absorb of 380 + 243 + 137 =
        // 760 ms whose touch (rise + attach) is at 623 ms.
        static_assert(ImeBubbleModel::kFirstBirthDip == 18 && ImeBubbleModel::kLobeBirthDip == 10 && ImeBubbleModel::kLobeRiseMs == 380 &&
                          ImeBubbleModel::kLobeAttachMs == 243 && ImeBubbleModel::kLobeSwallowMs == 137,
                      "the constants are the old birth points and acts");
        const ImeBubbleModel defaults{};
        require(std::abs(defaults.RiseEndMs() - 380) < 1e-9 && std::abs(defaults.AbsorbMs() - 760) < 1e-9 && std::abs(defaults.AttachEndMs() - 623) < 1e-9 &&
                    std::abs(defaults.TouchMs() - 623) < 1e-9 && std::abs(defaults.TouchFrac() - 623.0 / 760.0) < 1e-9,
                "the derived timeline is the old one");

        ImeBubbleModel bubble;
        bubble.Begin(1000);
        // The first bubble is born kFirstBirthDip below its resting centre.
        require(std::abs(bubble.Step(1000).cy - (ImeBubbleModel::kCyRest + 18)) < 1e-4f, "the first bubble is born below its resting centre");
        // The newcomer's centre is born kLobeBirthDip below the window's bottom,
        // at the start of the radius rhythm.
        require(bubble.Fuse(1200), "it merges");
        const auto born = bubble.Step(1200);
        require(std::abs(born.lobes[0].y - (ImeBubbleModel::kH + 10)) < 1e-4f, "the newcomer is born below the window's bottom edge");
        require(std::abs(born.lobes[0].r - 0.30f * ImeBubbleModel::kStartRadius) < 0.01f,
                "it is born at the start of its own radius rhythm, off the bubble's start size");
        // The three acts: the slow rise ends at the hover radius at 380 ms, the
        // fast approach reaches the rim by the touch at 623 ms, then the swallow
        // draws it in and it is gone at 760 ms.
        const float rTouch = 0.55f * ImeBubbleModel::kStartRadius;
        require(std::abs(bubble.Step(1200 + 379.5).lobes[0].r - 0.8f * rTouch) < 0.5f, "the slow rise ends on the hover radius at its own time");
        require(bubble.Step(1200 + 622.5).lobes[0].r > 0.95f * rTouch, "the fast approach reaches the rim by the touch");
        require(std::abs(bubble.Step(1200 + 622).grow - 1.f) < 1e-4f, "the touch comes at rise + attach, not later");
        require(bubble.Step(1200 + 624).grow > 1.f, "and the size steps at once");
        require(bubble.Step(1200 + 645).lobes[0].r < 0.65f * rTouch, "the swallow has well begun by 645 ms");
        require(bubble.Step(1200 + 684).lobes[0].r < 0.25f * rTouch, "and is far in by 0.9 of the absorb");
        require(bubble.Step(1200 + 761).lobeCount == 0, "and gone at the absorb's own end");

        // A restart is born the same way, and its newcomer too.
        bubble.Begin(2500);
        require(std::abs(bubble.Step(2500).cy - (ImeBubbleModel::kCyRest + 18)) < 1e-4f, "Begin keeps the birth dip");
        bubble.Cancel();
        bubble.Begin(4000);
        require(bubble.Fuse(4200) && std::abs(bubble.Step(4200).lobes[0].y - (ImeBubbleModel::kH + 10)) < 1e-4f,
                "and a newcomer after the restart is still born the birth dip below the window");
    });
    run("the absorb's three acts stay ordered and its timeline stays finite", [&] {
        // None of the acts may be zero: the absorb divides by them, and a
        // newcomer's place in it must advance from its birth to its swallow.
        static_assert(ImeBubbleModel::kLobeRiseMs > 0 && ImeBubbleModel::kLobeAttachMs > 0 && ImeBubbleModel::kLobeSwallowMs > 0, "no act is zero");
        const ImeBubbleModel model{};
        require(std::abs(model.AbsorbMs() - double(ImeBubbleModel::kLobeRiseMs + ImeBubbleModel::kLobeAttachMs + ImeBubbleModel::kLobeSwallowMs)) < 1e-9,
                "the absorb is exactly its three acts");
        require(model.RiseEndMs() < model.TouchMs() && model.TouchMs() < model.AbsorbMs(),
                "the rise ends before the touch, the touch before the swallow's end");
        require(std::abs(model.AttachEndMs() - model.TouchMs()) < 1e-9, "the fast approach ends at the touch");
        require(model.TouchFrac() > 0 && model.TouchFrac() < 1, "the touch sits inside the absorb");
        require(std::abs(model.TouchFrac() - model.TouchMs() / model.AbsorbMs()) < 1e-12, "and is that share of it");
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(bubble.Fuse(1200), "it merges");
        double prevU = -1;
        for (double dt = 0; dt < bubble.AbsorbMs(); dt += 8) {
            const auto f = bubble.Step(1200 + dt);
            require(f.active && f.lobeCount == 1, "its newcomer is drawn from birth to swallow");
            require(f.lobes[0].u > prevU, "its place in the absorb only advances");
            prevU = f.lobes[0].u;
            require(std::isfinite(f.lobes[0].x) && std::isfinite(f.lobes[0].y) && std::isfinite(f.lobes[0].r), "a real newcomer, never a broken one");
            require(std::isfinite(f.cx) && std::isfinite(f.cy) && std::isfinite(f.scale) && std::isfinite(f.wob), "every frame of it is finite");
        }
        require(bubble.Step(1200 + bubble.AbsorbMs()).lobeCount == 0, "and is gone at the absorb's own end");
    });
    run("a broken clock never starts, merges or advances a bubble", [&] {
        const double nan = std::nan("");
        const double inf = INFINITY;
        ImeBubbleModel bubble;
        bubble.Begin(nan);
        require(!bubble.active() && !bubble.Step(1000).active, "a broken start never begins");
        bubble.Begin(inf);
        require(!bubble.active(), "and neither does an infinite one");
        bubble.Begin(0);
        require(!bubble.active() && !bubble.Mergeable(1) && !bubble.Fuse(1), "time nought is no clock either");
        bubble.Begin(1000);
        require(!bubble.Fuse(nan) && !bubble.Fuse(inf) && bubble.fusions() == 1, "a broken merge time changes nothing");
        require(!bubble.Step(nan).active && !bubble.Step(inf).active, "a broken frame time draws nothing");
        require(bubble.Step(1100).active, "while the real clock still runs it");
    });
    run("the pop is one life out, and the burst follows it", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(std::abs(bubble.PopAt() - (1000 + 1060)) < 0.5, "the default life sets the pop");
        require(bubble.Mergeable(bubble.PopAt() - 5), "a state just before the pop still merges");
        require(!bubble.Mergeable(bubble.PopAt() + 5), "and none once the pop is due");
        require(std::abs(bubble.EndMs() - (bubble.PopAt() + 486)) < 0.5, "the swell and burst follow the pop");
        // The pop is the longest of the merge's own life and a fresh state's,
        // so a merge never shortens the life it was already promised.
        const double before = bubble.EndMs();
        require(bubble.Fuse(bubble.PopAt() - 100), "a late state still merges");
        require(bubble.EndMs() > before, "the newcomer is absorbed and the pop is pushed back");
    });
    run("a merge pushes the pop past its own life and its newcomer's swallow", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double fuseAt = bubble.PopAt() - 100;
        require(bubble.Fuse(fuseAt), "a state just before the pop still merges");
        require(std::abs(bubble.PopAt() - (fuseAt + ImeBubbleModel::kLifeMs)) < 0.5, "the pop moves a whole life out from the merge");
        require(bubble.PopAt() >= fuseAt + bubble.AbsorbMs(), "and never lands before the newcomer is swallowed");
        // The newcomer is a sliver when its absorb ends, and gone well before
        // the skin bursts: no swallow is ever cut off by the pop.
        const auto lastFrame = bubble.Step(fuseAt + bubble.AbsorbMs() - 1);
        require(lastFrame.lobeCount == 1 && lastFrame.lobes[0].r < 1.f, "its newcomer is all but swallowed when its absorb ends");
        const auto atPop = bubble.Step(bubble.PopAt() - 0.5);
        require(atPop.active && atPop.lobeCount == 0, "and swallowed long before the bubble bursts");
        require(!bubble.Mergeable(bubble.PopAt()), "no merge comes after the pop");
    });
    run("a live bubble merges: soft, springy, alive, one step larger", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double fuseAt = 1000 + ImeBubbleModel::kPopAt - 100;
        const double end1 = bubble.EndMs();
        const auto before = bubble.Step(fuseAt);
        require(bubble.fusions() == 1 && before.active, "one bubble, out there");
        require(bubble.Fuse(fuseAt), "a state while it lives merges");
        require(bubble.fusions() == 2, "the count moves");
        require(bubble.EndMs() > end1, "the pop is pushed back so the newest state reads");
        const auto at = bubble.Step(fuseAt);
        require(std::abs(at.scale - before.scale) < 0.01f, "the size cannot jump at the merge");
        require(std::abs(at.grow - 1.f) < 1e-4f, "the size waits for the newcomer to touch");
        const double touch = fuseAt + bubble.TouchMs();
        require(std::abs(bubble.Step(touch - 30).grow - 1.f) < 1e-3f, "it holds until the touch");
        require(bubble.Step(touch + 60).grow > 1.f, "then the touch presses the skin wider");
        float peak = 0;
        for (int ms = 0; ms <= 900; ms += 10)
            peak = std::max(peak, bubble.Step(touch + ms).grow);
        require(peak > bubble.ScaleAt(2) + 0.05f, "a real merge overshoots its new size, visibly");
        // The spring has not fully settled by the pop — the touch now comes
        // late enough in the absorb (about 0.82 of it by default) that a whole
        // life leaves it at about 94 %, not all but done — but it has clearly
        // all but reached it, and it settles fully just after (below).
        const auto nearPop = bubble.Step(bubble.PopAt() - 1);
        require(nearPop.active && std::abs(nearPop.grow - bubble.ScaleAt(2)) < 0.02f, "by the pop it has all but reached the new size");
        const auto settled = bubble.Step(touch + 920);
        require(std::abs(settled.grow - bubble.ScaleAt(2)) < 0.001f, "and settles at it");
        require(settled.active, "still the same bubble");
    });
    run("a lone merge keeps the old feel: size, squash and wobble", [&] {
        // The feel anchor: with one newcomer the frame is exactly the old
        // closed form — the base plus one springy step, the hit's own squash,
        // one decaying rhythm — however the merges are added up.
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(bubble.Fuse(1200), "it merges");
        const double touch = 1200 + bubble.TouchMs();
        const float step = bubble.ScaleAt(2) - bubble.ScaleAt(1);
        for (double t = 1200; t < bubble.PopAt(); t += 7) {
            const float want = t < touch ? bubble.ScaleAt(1) : bubble.ScaleAt(1) + step * ImeBubbleModel::softBounce(t - touch, ImeBubbleModel::kRebound);
            require(std::abs(bubble.Step(t).grow - want) < 1e-5f, "the size is the base plus one step");
        }
        // The squash is the old hit's, at the old moment: the frame's scale
        // against its own grow (the size without the hit) is the float's
        // breath times the hit's curve — deepest at the touch, decaying over
        // kDipSpan — exactly, over the whole absorb that follows the emerge.
        const double absorb = bubble.AbsorbMs();
        const float touchFrac = float(bubble.TouchFrac());
        for (double t = 1000 + ImeBubbleModel::kEmergeMs + 40; t < 1200 + absorb; t += 7) {
            const float u = float((t - 1200) / absorb);
            const float d = (u - touchFrac) / float(ImeBubbleModel::kDipSpan);
            const float breath = 1.f + 0.012f * float(std::sin(6.2831853f * float((t - 1000 - ImeBubbleModel::kEmergeMs) / ImeBubbleModel::kFloatMs)));
            const float want = breath * (1 - ImeBubbleModel::kCompression * float(std::exp(-d * d)));
            const auto f = bubble.Step(t);
            require(std::abs(f.scale / f.grow - want) < 1e-4f, "the hit squashes the skin by the old amount");
        }
        // The wobble is one decaying sine of the newcomer's own strength and
        // pace: doubling either is that of its own frame — and the doubled pace
        // at a moment is the plain one twice as far along, its decay undone.
        const double s = 20;
        for (int ms = 20; ms <= 420; ms += 20) {
            require(std::abs(ImeBubbleModel::WobbleAt(ms, 2 * ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz) -
                             2 * ImeBubbleModel::WobbleAt(ms, ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz)) < 1e-6f,
                    "the wobble's strength is its own");
            require(std::abs(ImeBubbleModel::WobbleAt(ms, ImeBubbleModel::kWobbleAmp, 2 * ImeBubbleModel::kWobbleHz) -
                             ImeBubbleModel::WobbleAt(2.0 * ms, ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz) * float(std::exp(ms / 260.0))) < 1e-4f,
                    "its pace is its own, and it decays by the same spring");
        }
        require(std::abs(ImeBubbleModel::WobbleAt(s, ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz)) > 0.3f, "and it is a real rock at 20 ms");
        require(std::abs(std::abs(bubble.Step(touch + 50).wob) -
                         std::abs(ImeBubbleModel::WobbleAt(50, ImeBubbleModel::kWobbleAmp, ImeBubbleModel::kWobbleHz))) < 0.2f,
                "the frame's rock is that rhythm at its own strength and pace");
    });
    run("merges chain, extend the life, and stop growing at ten", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        double now = 1200;
        for (int i = 0; i < 30; ++i, now += 100)
            require(bubble.Fuse(now), "each state inside the life merges");
        require(bubble.fusions() > ImeBubbleModel::kMaxFusions, "the count itself may pass ten");
        require(bubble.EndMs() > now, "the last merge still owns the bubble");
        require(bubble.EndMs() - 1000 >= ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + ImeBubbleModel::kBurstMs - 1,
                "the bubble outlives at least one whole life");
        // Every newcomer's step lands on its own touch; the frame before the
        // burst has had all of them, and none past the tenth adds size.
        const auto grown = bubble.Step(bubble.PopAt() - 1);
        require(std::abs(grown.grow - bubble.ScaleAt(ImeBubbleModel::kMaxFusions)) < 0.02f, "the size stops growing past ten merges");
        require(grown.lobeCount == 0, "no newcomer is still in flight when the skin bursts");
        require(!bubble.Fuse(bubble.PopAt() + 100), "once the burst begins, no merge exists");
    });
    run("a bubble merged to ten still lives inside the window", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        for (int i = 0; i < 12; ++i)
            bubble.Fuse(1100 + i * 100.0);
        // Every newcomer, however big the bubble and however many are already
        // in flight, has its centre born the newcomer birth dip below the
        // window's bottom edge, at the start of its own radius rhythm.
        const float birthDip = ImeBubbleModel::kLobeBirthDip;
        for (int i = 0; i < 12; ++i) {
            const auto start = bubble.Step(1100 + i * 100.0);
            require(start.active && start.lobeCount > 0, "each newcomer is a visible bubble");
            const auto& own = start.lobes[start.lobeCount - 1];
            require(own.r > 0.5f, "the newcomer of this very switch is a real bubble");
            require(std::abs(own.y - (ImeBubbleModel::kH + birthDip)) < 0.01f, "its centre is born the birth dip below the window's bottom edge");
            require(std::abs(own.r - 0.30f * ImeBubbleModel::kStartRadius) < 0.01f, "it is born the first bubble's size, not the grown skin's");
        }
        for (double t = 1008; t < bubble.EndMs(); t += 8) {
            const auto f = bubble.Step(t);
            if (!f.active)
                continue;
            const float r = ImeBubbleModel::kRadius * f.scale;
            require(f.cy - r >= 0 && f.cy + r <= ImeBubbleModel::kH, "the fused skin never leaves the window");
            require(f.lobeCount <= ImeBubbleModel::kMaxFrameLobes, "a frame carries no more than it can draw");
            // A newcomer born below the window is clipped until it rises in, so
            // only one already in the frame is held to the frame; its top edge
            // never crosses the window's own top.
            for (int i = 0; i < f.lobeCount; ++i) {
                const auto& lobe = f.lobes[i];
                require(lobe.y - lobe.r >= 0, "no newcomer ever leaves the window through the top");
                if (lobe.y + lobe.r <= ImeBubbleModel::kH)
                    require(lobe.x - lobe.r >= 0 && lobe.x + lobe.r <= ImeBubbleModel::kW && lobe.y - lobe.r >= 0,
                            "a newcomer inside the window stays inside it");
            }
            for (int i = 0; i < f.beadCount; ++i) {
                const auto& bead = f.beads[i];
                require(bead.x - bead.r >= 0 && bead.x + bead.r <= ImeBubbleModel::kW && bead.y - bead.r >= 0 && bead.y + bead.r <= ImeBubbleModel::kH,
                        "even the largest burst stays inside the window");
            }
        }
    });
    run("a newcomer keeps the first bubble's size, never the grown skin's", [&] {
        // The skin grows from 32 to 51 DIP, but every newcomer is still born the
        // first bubble's size — its touch radius does not inherit the skin's
        // growth.
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        for (int i = 0; i < 4; ++i)
            require(bubble.Fuse(1150 + i * 120.0), "each switch merges");
        const double launch = 1150 + 4 * 120.0; // a fresh newcomer, long after four fusions
        require(bubble.Fuse(launch), "one more switch merges");
        const auto atTouch = bubble.Step(launch + bubble.TouchMs());
        require(atTouch.grow > 1.2f, "the skin has grown well past its start by now");
        require(atTouch.lobeCount >= 1, "the newcomer is still in the frame at its own touch");
        const auto& own = atTouch.lobes[atTouch.lobeCount - 1];
        require(std::abs(own.r - 0.55f * ImeBubbleModel::kStartRadius) < 0.5f, "at its touch it is the first bubble's rim, not the grown skin's");
        require(ImeBubbleModel::kRadius * atTouch.grow > ImeBubbleModel::kStartRadius + 5, "while the skin it meets is clearly larger than the first bubble");
    });
    run("an unmerged float is perfectly still, so its glass publishes once", [&] {
        // The paint publishes a still, whole skin once rather than on every
        // frame: no newcomer, no burst, opacity one and not a ripple.
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        for (double t = 1000 + ImeBubbleModel::kEmergeMs + 40; t < 1000 + ImeBubbleModel::kPopAt; t += 8) {
            const auto f = bubble.Step(t);
            require(f.active && f.opacity == 1, "the settled bubble is fully there");
            require(f.grow == 1 && f.lobeCount == 0 && f.beadCount == 0, "its size is the first bubble's, with no newcomer and no burst");
            require(f.wob == 0, "and not a ripple: the frame is still");
        }
        // A merge gives it back at once: a newcomer's rhythm is a moving frame.
        const double fuseAt = 1000 + ImeBubbleModel::kPopAt - 300;
        require(bubble.Fuse(fuseAt), "a state still merges");
        require(std::abs(bubble.Step(fuseAt + bubble.TouchMs() + 50).wob) > 0.3f, "the touch is a moving frame again");
    });
    run("a merge is born below the window, rises, wobbles and lengthens the life", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double fuseAt = 1200;
        require(bubble.Mergeable(fuseAt), "a fresh bubble is mergeable");
        require(bubble.Fuse(fuseAt), "the merge happens");
        require(!bubble.Mergeable(bubble.PopAt()), "and closes when the burst begins");
        const double touch = fuseAt + bubble.TouchMs();
        const auto first = bubble.Step(fuseAt);
        require(first.lobeCount == 1, "one newcomer");
        require(first.lobes[0].r > 2, "the newcomer is a visible bubble");
        require(first.lobes[0].y - first.lobes[0].r >= ImeBubbleModel::kH - 0.5f, "it is born below the window's bottom edge, off the screen");
        // It only ever rises until it meets the skin.
        float prevY = first.lobes[0].y;
        for (double t = fuseAt + 8; t < touch; t += 8) {
            const auto f = bubble.Step(t);
            require(f.lobes[0].y <= prevY + 0.001f, "the newcomer rises monotonically to the skin");
            prevY = f.lobes[0].y;
        }
        const auto later = bubble.Step(fuseAt + 300);
        require(later.lobes[0].r > 0.5f, "the lobe is still being swallowed");
        require(std::hypot(later.lobes[0].x - later.cx, later.lobes[0].y - later.cy) < std::hypot(first.lobes[0].x - first.cx, first.lobes[0].y - first.cy),
                "it slides into the skin");
        require(std::abs(bubble.Step(touch + 50).wob) > 0.3f, "the touch rocks the skin like liquid");
        const auto doneFrame = bubble.Step(fuseAt + bubble.AbsorbMs() + 20);
        require(doneFrame.lobeCount == 0, "and is gone once swallowed");
        require(std::abs(bubble.Step(touch + ImeBubbleModel::kWobbleMs + 40).wob) < 0.01f, "the wobble dies down");
        const double hold = bubble.EndMs() - fuseAt;
        require(hold >= ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + ImeBubbleModel::kBurstMs - 1,
                "a merge keeps the bubble a whole life longer than its first");
        const auto before = bubble.EndMs();
        require(bubble.Fuse(2000), "the next merge lands while it lives");
        require(bubble.EndMs() > before, "and lengthens the life further");
    });
    run("a newcomer is born off the screen and then only rises", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(bubble.Fuse(1200), "it merges");
        const auto born = bubble.Step(1201);
        require(born.lobeCount == 1, "the newcomer is drawn at once");
        require(born.lobes[0].y - born.lobes[0].r >= ImeBubbleModel::kH - 0.5f, "it is born entirely below the window, off the screen");
        require(born.lobes[0].y + born.lobes[0].r > ImeBubbleModel::kH, "its whole body is below the window's bottom");
        float prevY = born.lobes[0].y;
        int frames = 0;
        for (double t = 1208; t < 1200 + bubble.AbsorbMs(); t += 8) {
            const auto f = bubble.Step(t);
            if (f.lobeCount == 0)
                break;
            require(f.lobes[0].y <= prevY + 0.05f, "the newcomer only ever rises into the frame");
            prevY = f.lobes[0].y;
            ++frames;
        }
        require(frames > 0 && prevY < ImeBubbleModel::kCyRest + ImeBubbleModel::kRadius, "by the end it has climbed right up to the skin");
    });
    run("the slow rise gives way to a far quicker approach into the skin", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(bubble.Fuse(1200), "it merges");
        // The newcomer's centre y, sampled across each act: how far it moves
        // per millisecond there.
        auto avgSpeed = [&](double from, double to) {
            float prev = bubble.Step(1200 + from).lobes[0].y;
            double moved = 0;
            int n = 0;
            for (double t = from + 8; t <= to; t += 8) {
                const float y = bubble.Step(1200 + t).lobes[0].y;
                moved += std::abs(prev - y);
                prev = y;
                ++n;
            }
            return n ? float(moved / (n * 8.0)) : 0.f;
        };
        const float slow = avgSpeed(0, bubble.RiseEndMs());
        const float fast = avgSpeed(bubble.RiseEndMs(), bubble.TouchMs());
        require(slow > 0, "the newcomer is moving during its slow rise");
        require(fast >= 2.f * slow, "the fast approach is at least twice the slow rise's pace");
    });
    run("the touch pivots the whole hit: size, squash and wobble await it", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(bubble.Fuse(1200), "it merges");
        const double touch = 1200 + bubble.TouchMs();
        require(std::abs(bubble.TouchFrac() - 0.82) < 0.001, "the touch sits at about 0.82 of the absorb by default");
        require(std::abs(bubble.Step(touch - 1).grow - 1.f) < 1e-4f, "the size waits for the touch");
        require(bubble.Step(touch + 40).grow > 1.f, "the touch then presses the skin wider");
        require(std::abs(bubble.Step(touch - 1).wob) < 1e-6f, "the skin is still before the touch");
        require(std::abs(bubble.Step(touch + 50).wob) > 0.3f, "the touch rocks it, like liquid");
        // The squash is deepest at the touch, not before it: the frame's scale
        // against its own grow is the smallest at that very moment.
        const float atTouch = bubble.Step(touch).scale / bubble.Step(touch).grow;
        const float before = bubble.Step(1200 + 0.3 * bubble.AbsorbMs()).scale / bubble.Step(1200 + 0.3 * bubble.AbsorbMs()).grow;
        const float after = bubble.Step(1200 + 0.95 * bubble.AbsorbMs()).scale / bubble.Step(1200 + 0.95 * bubble.AbsorbMs()).grow;
        require(atTouch < before && atTouch < after, "the hit compresses the skin deepest at the touch");
    });
    run("a newcomer's whole journey: off the screen, into the frame, swallowed", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(bubble.Fuse(1200), "it merges");
        const double A = bubble.AbsorbMs();
        const auto away = bubble.Step(1200 + 0.02 * A);
        require(away.lobeCount == 1 && away.lobes[0].y - away.lobes[0].r >= ImeBubbleModel::kH - 0.5f, "it starts off the screen");
        const auto within = bubble.Step(1200 + 0.78 * A);
        require(within.lobes[0].y - within.lobes[0].r >= 0 && within.lobes[0].y + within.lobes[0].r <= ImeBubbleModel::kH,
                "by the fast approach it is wholly inside the frame");
        require(std::hypot(within.lobes[0].x - within.cx, within.lobes[0].y - within.cy) < std::hypot(away.lobes[0].x - away.cx, away.lobes[0].y - away.cy),
                "and nearer the skin than it began");
        require(std::abs(bubble.Step(1200 + A - 0.5).lobes[0].r) < 1.f, "its last frame is a sliver");
        require(bubble.Step(1200 + A + 1).lobeCount == 0, "and it is swallowed with nothing left");
    });
    run("rapid merges rise together and fuse side by side", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double f1 = 1200, f2 = 1300; // 100 ms apart, far faster than one absorb
        require(bubble.Fuse(f1) && bubble.Fuse(f2), "both rapid states merge");
        const auto first = bubble.Step(f1); // the first newcomer, exactly at its launch
        require(first.lobeCount == 1 && first.lobes[0].r > 2, "the first newcomer rises at once");
        require(first.lobes[0].y - first.lobes[0].r >= ImeBubbleModel::kH - 0.5f, "it is born below the window's bottom edge");
        const auto both = bubble.Step(f2);
        require(both.lobeCount == 2, "the second rises too: it does not wait for the first swallow");
        require(both.lobes[1].y - both.lobes[1].r >= ImeBubbleModel::kH - 0.5f, "its own birth is below the window too");
        require(both.lobes[1].y > both.lobes[0].y, "the first is already climbing above it");
        require(std::abs(both.lobes[0].x - both.lobes[1].x) > 0.01f, "each newcomer draws its own place on the emerge line");
        // Each touches its own touch-time after its own launch.
        const double touch1 = f1 + bubble.TouchMs(), touch2 = f2 + bubble.TouchMs();
        require(std::abs(bubble.Step(touch1 - 1).grow - 1.f) < 1e-4f, "no touch has come yet");
        require(bubble.Step(touch1 + 40).grow > 1.f, "the first touch presses the skin wider");
        require(bubble.Step(touch2 - 1).grow > bubble.ScaleAt(2), "and has passed the first step's size before the second touch");
        require(bubble.Step(touch2 + 40).grow > bubble.Step(touch2 - 1).grow, "the second touch adds its own");
        // A launch adds nothing to the size, and a later touch never takes back
        // the step an earlier one took: the sizes are added up, not replaced.
        require(std::abs(bubble.Step(f2).grow - bubble.Step(f2 - 1).grow) < 1e-6f, "a launch changes no size at all");
        ImeBubbleModel solo;
        solo.Begin(1000);
        solo.Fuse(f1);
        for (double t = f1 + 1; t < bubble.PopAt(); t += 13)
            require(bubble.Step(t).grow >= solo.Step(t).grow - 1e-5f, "the second rise never takes the first one's step back");
        float peak = 0;
        for (int ms = 0; ms <= 340; ms += 5) // the first touch's own spring, still overshooting
            peak = std::max(peak, bubble.Step(touch1 + ms).grow);
        require(peak > bubble.ScaleAt(2) + 0.05f, "the first touch still overshoots visibly");
        // Both steps have landed by the burst: three bubbles' size.
        const auto nearPop = bubble.Step(bubble.PopAt() - 1);
        require(std::abs(nearPop.grow - bubble.ScaleAt(3)) < 0.01f, "once both have settled the skin is two steps larger");
    });
    run("two states in the same millisecond both rise", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        require(bubble.Fuse(1200) && bubble.Fuse(1200), "both merge, at the very same time");
        require(bubble.fusions() == 3, "both count");
        const auto both = bubble.Step(1200);
        require(both.lobeCount == 2, "both newcomers rise");
        const float birthDip = ImeBubbleModel::kLobeBirthDip;
        for (int i = 0; i < both.lobeCount; ++i) {
            require(both.lobes[i].r > 1, "each is a real bubble");
            require(std::abs(both.lobes[i].u) < 1e-6f, "and both have just launched");
            require(std::abs(both.lobes[i].y - (ImeBubbleModel::kH + birthDip)) < 0.01f,
                    "both are born with their centre the birth dip below the window's bottom edge");
        }
        const double touch = 1200 + bubble.TouchMs();
        require(std::abs(bubble.Step(touch - 1).grow - 1.f) < 1e-4f, "neither has touched yet");
        require(bubble.Step(touch + 1).grow > 1.f, "both touches press at once");
        // Their squashes add up, but the skin can only be squashed so far: two
        // hits at once leave exactly one kCompression's worth behind.
        const auto hit = bubble.Step(touch);
        require(std::abs(hit.scale / hit.grow - (1 - ImeBubbleModel::kCompression)) < 0.02f, "two hits at once squash to the limit, not twice as deep");
        // Both steps have all but landed by the pop (the later touch in the
        // longer absorb leaves the spring at about 94 % of the way, as above).
        require(std::abs(bubble.Step(bubble.PopAt() - 1).grow - bubble.ScaleAt(3)) < 0.02f, "and both steps are the skin's by the pop");
    });
    run("ten merges, ten newcomers, one newest state at a time", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        double now = 1200;
        const float birthDip = ImeBubbleModel::kLobeBirthDip;
        for (int i = 1; i <= 10; ++i, now += 150) {
            require(bubble.Fuse(now), "every switch before the burst merges");
            const auto own = bubble.Step(now + 1);
            require(own.lobeCount >= 1 && own.lobeCount <= ImeBubbleModel::kMaxFrameLobes, "the frame carries the newcomers in flight");
            const auto& newest = own.lobes[own.lobeCount - 1];
            require(newest.r > 1 && std::abs(newest.y - (ImeBubbleModel::kH + birthDip)) < 0.01f,
                    "the newcomer of this very switch is born below the window at once, not after the others");
        }
        require(bubble.fusions() == 11, "ten merges on top of the first bubble");
        require(bubble.Step(now - 150 + 1).lobeCount == 6, "at 150 ms apart six rise side by side");
        const auto nearPop = bubble.Step(bubble.PopAt() - 1);
        require(std::abs(nearPop.grow - bubble.ScaleAt(ImeBubbleModel::kMaxFusions)) < 0.02f, "the size has reached its ten-merge cap");
        require(nearPop.lobeCount == 0, "and every newcomer is swallowed before the burst");
    });
    run("a frame carries the newest newcomers, the model counts them all", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        // Eight switches within 40 ms: more newcomers at once than a frame can
        // draw, all of them real merges.
        for (int i = 0; i < 8; ++i)
            require(bubble.Fuse(1200 + i * 5.0), "every switch merges");
        require(bubble.fusions() == 9, "the model counts them all");
        const auto f = bubble.Step(1240);
        require(f.lobeCount == ImeBubbleModel::kMaxFrameLobes, "the frame is full");
        const float thirdU = float((1240 - (1200 + 2 * 5.0)) / bubble.AbsorbMs());
        const float newestU = float((1240 - (1200 + 7 * 5.0)) / bubble.AbsorbMs());
        require(std::abs(f.lobes[0].u - thirdU) < 1e-5f, "the oldest newcomers made room");
        require(std::abs(f.lobes[f.lobeCount - 1].u - newestU) < 1e-5f, "the newest is always there");
        require(f.lobes[0].u > f.lobes[f.lobeCount - 1].u, "oldest first, newest last");
        require(std::abs(bubble.Step(bubble.PopAt() - 1).grow - bubble.ScaleAt(9)) < 0.02f, "the size takes every one of the eight, drawn or not");
    });
    run("the melt stays inside the paint's six parts", [&] {
        static_assert(ImeBubbleModel::kMaxMeltParts == 6, "the paint's own parts budget");
        for (int n = 0; n <= ImeBubbleModel::kMaxFrameLobes; ++n) {
            for (bool split : {false, true}) {
                const auto b = ImeBubbleModel::MeltFor(n, split);
                require(b.skinParts >= 1 && b.skinParts <= 2, "the skin is one piece or two");
                require(b.lobes == ImeBubbleModel::DrawnLobes(n, split), "one budget, one drawn count");
                require(b.lobes >= 0 && b.lobes <= n, "never more newcomers than the frame carries");
                require(b.skinParts + b.lobes <= ImeBubbleModel::kMaxMeltParts, "the skin and the newcomers fit the one melt");
                require(n <= 4 ? b.lobes == n : b.lobes == ImeBubbleModel::kMaxMeltParts - b.skinParts,
                        "every newcomer fits while there is room, then the newest do");
                if (split && b.skinParts == 1)
                    require(n >= ImeBubbleModel::kMaxMeltParts - 1, "the split only goes for a newcomer's slot");
            }
        }
        require(ImeBubbleModel::DrawnLobes(0, true) == 0, "no newcomer, nothing to draw");
        require(ImeBubbleModel::DrawnLobes(4, true) == 4, "four newcomers still split the skin");
        require(ImeBubbleModel::DrawnLobes(5, true) == 5, "a fifth newcomer drops the split, not itself");
        require(ImeBubbleModel::DrawnLobes(6, true) == 5, "six newcomers: the oldest is dropped, the newest kept");
        require(ImeBubbleModel::DrawnLobes(6, false) == 5, "a whole skin has no room for a sixth either");
    });
    run("ten rapid switches keep every frame inside the window", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        for (int i = 0; i < 10; ++i)
            bubble.Fuse(1150 + i * 120.0);
        require(bubble.fusions() == 11, "ten switches on top of the first bubble");
        for (double t = 1008; t < bubble.EndMs(); t += 8) {
            const auto f = bubble.Step(t);
            if (!f.active)
                continue;
            require(f.lobeCount <= ImeBubbleModel::kMaxFrameLobes, "a frame never overfills");
            const float r = ImeBubbleModel::kRadius * f.scale;
            require(f.cy - r >= 0 && f.cy + r <= ImeBubbleModel::kH, "the skin stays in the window");
            for (int i = 0; i < f.lobeCount; ++i) {
                const auto& lobe = f.lobes[i];
                require(std::isfinite(lobe.x) && std::isfinite(lobe.y) && std::isfinite(lobe.r) && lobe.r > 0, "every newcomer drawn is a real bubble");
                require(lobe.u >= 0 && lobe.u < 1, "each has its own place in its absorb");
                // A newcomer is born below the window and clipped until it has
                // risen in; once in the frame it stays in the frame.
                require(lobe.y - lobe.r >= 0, "no newcomer ever leaves the window through the top");
                if (lobe.y + lobe.r <= ImeBubbleModel::kH)
                    require(lobe.x - lobe.r >= 0 && lobe.x + lobe.r <= ImeBubbleModel::kW && lobe.y - lobe.r >= 0, "and stays inside the window");
            }
            for (int i = 0; i < f.beadCount; ++i) {
                const auto& bead = f.beads[i];
                require(bead.x - bead.r >= 0 && bead.x + bead.r <= ImeBubbleModel::kW && bead.y - bead.r >= 0 && bead.y + bead.r <= ImeBubbleModel::kH,
                        "the burst stays inside the window too");
            }
        }
    });
    run("the foam thins away to nothing with a last sparkle", [&] {
        ImeBubbleModel bubble;
        bubble.Begin(1000);
        const double burstAt = 1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs;
        const int n = bubble.Step(burstAt + 4).beadCount;
        require(n >= ImeBubbleModel::kMinBeads, "the burst has its foam");
        float prev[ImeBubbleModel::kMaxBeads], lastAlive[ImeBubbleModel::kMaxBeads], peak[ImeBubbleModel::kMaxBeads];
        for (int i = 0; i < n; ++i) {
            prev[i] = -1;
            lastAlive[i] = 0;
            peak[i] = 0;
        }
        for (double t = burstAt + 4; t < bubble.EndMs(); t += 4) {
            const auto f = bubble.Step(t);
            if (!f.active)
                break;
            for (int i = 0; i < n; ++i) {
                const auto& bead = f.beads[i];
                require(bead.r <= (prev[i] < 0 ? 1e9f : prev[i] + 0.001f), "a drop only ever thins, never jumps back");
                if (bead.r > 0)
                    lastAlive[i] = bead.r;
                peak[i] = std::max(peak[i], bead.glint);
                prev[i] = bead.r;
            }
        }
        for (int i = 0; i < n; ++i) {
            require(peak[i] > 0.3f, "every drop sparkles once just before it goes");
            require(prev[i] == 0, "every drop is gone by the end");
            require(lastAlive[i] < 2.f, "it thinned to a sliver first — it did not blink off");
        }
    });
    run("a bigger bubble throws more foam, and a fuller swarm scatters wider", [&] {
        // The burst's count is the base swarm, the burst's temper, and the size
        // the merges have reached — clamped to the cap.
        const ImeBubbleModel defaults{};
        static_assert(ImeBubbleModel::kMinBeads == 9 && ImeBubbleModel::kMaxBeads == 30, "nine to thirty drops");
        require(defaults.BeadsFor(1, 0) == ImeBubbleModel::kMinBeads, "a lone bubble's calmest burst is the base swarm");
        require(defaults.BeadsFor(1, 4) == 13, "its wildest stays small: nine plus the temper");
        require(defaults.BeadsFor(2, 2) == 18, "one merge adds the temper and the size step");
        require(defaults.BeadsFor(ImeBubbleModel::kMaxFusions, 0) == ImeBubbleModel::kMaxBeads, "ten merges reach the cap even at the calmest temper");
        require(defaults.BeadsFor(ImeBubbleModel::kMaxFusions, 4) == ImeBubbleModel::kMaxBeads, "up to the cap");
        for (int fusions = 1; fusions <= ImeBubbleModel::kMaxFusions; ++fusions) {
            for (int roll = 0; roll <= 4; ++roll) {
                const int want = std::clamp(ImeBubbleModel::kMinBeads + std::min(roll, 4) + int((defaults.ScaleAt(fusions) - 1) * 40),
                                            ImeBubbleModel::kMinBeads, ImeBubbleModel::kMaxBeads);
                require(defaults.BeadsFor(fusions, roll) == want, "the count is the base, the temper and the size, capped");
                if (fusions > 1)
                    require(defaults.BeadsFor(fusions, roll) >= defaults.BeadsFor(fusions - 1, roll), "never fewer drops with more merges");
            }
        }
        require(ImeBubbleModel::SpreadFor(ImeBubbleModel::kMaxBeads, 1.f) > ImeBubbleModel::SpreadFor(ImeBubbleModel::kMinBeads, 1.f),
                "more drops, a wider scatter");
        require(std::abs(ImeBubbleModel::SpreadFor(ImeBubbleModel::kMinBeads, 1.f) - 1.f) < 0.001f, "the base swarm keeps its temper");
        // And a merged bubble's burst really carries more drops than a plain one.
        ImeBubbleModel plain;
        plain.Begin(1000);
        const int plainCount = plain.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 10).beadCount;
        ImeBubbleModel fused;
        fused.Begin(1000);
        for (int i = 0; i < 9; ++i)
            fused.Fuse(1150 + i * 120.0);
        const auto burst = fused.Step(fused.PopAt() + ImeBubbleModel::kSwellMs + 10);
        require(burst.beadCount > plainCount, "a merged bubble's skin holds more foam");
        require(burst.beadCount >= ImeBubbleModel::kMinBeads + 5, "and the difference is visible in the count");
    });
    run("a ten-merge bubble's foam is plainly larger than a lone bubble's", [&] {
        ImeBubbleModel lone;
        lone.Begin(1000);
        const int loneCount = lone.Step(1000 + ImeBubbleModel::kPopAt + ImeBubbleModel::kSwellMs + 10).beadCount;
        ImeBubbleModel merged;
        merged.Begin(1000);
        for (int i = 0; i < 10; ++i)
            merged.Fuse(1150 + i * 120.0);
        require(merged.fusions() == 11, "ten merges on top of the first bubble");
        const int mergedCount = merged.Step(merged.PopAt() + ImeBubbleModel::kSwellMs + 10).beadCount;
        require(mergedCount - loneCount >= 15, "a ten-merge bubble throws at least fifteen drops more than a lone one");
        require(mergedCount == ImeBubbleModel::kMaxBeads, "and it reaches the cap");
        require(loneCount >= ImeBubbleModel::kMinBeads && loneCount <= 13, "while a lone bubble's swarm stays small");
    });
    run("a fuller swarm flies further than a lone bubble's", [&] {
        // The bigger skin throws from a wider rim, on a fuller swarm's spread, so
        // its drops' mean reach from the burst's own centre is the larger — the
        // scatter grows with the bubble, not just the count.
        auto meanReach = [](ImeBubbleModel& bubble) {
            const auto burst = bubble.Step(bubble.PopAt() + ImeBubbleModel::kSwellMs + 40);
            const float bx = ImeBubbleModel::kCx, by = ImeBubbleModel::kCyRest - ImeBubbleModel::kFloatRise;
            double sum = 0;
            for (int i = 0; i < burst.beadCount; ++i)
                sum += std::hypot(double(burst.beads[i].x - bx), double(burst.beads[i].y - by));
            return burst.beadCount > 0 ? sum / burst.beadCount : 0.0;
        };
        ImeBubbleModel lone;
        lone.Begin(1000);
        ImeBubbleModel merged;
        merged.Begin(1000);
        for (int i = 0; i < 10; ++i)
            merged.Fuse(1150 + i * 120.0);
        require(meanReach(merged) > meanReach(lone), "the merged swarm scatters further on the whole");
    });
    run("every drop the burst throws gets a melt shape to ride in", [&] {
        require(ImeBubbleModel::kMaxMeltParts == 6, "the shapes ride the paint's own parts budget");
        require(ImeBubbleModel::ChunksFor(0) == 0 && ImeBubbleModel::ChunksFor(1) == 1 && ImeBubbleModel::ChunksFor(6) == 1 &&
                    ImeBubbleModel::ChunksFor(7) == 2 && ImeBubbleModel::ChunksFor(30) == 5,
                "one shape per six drops: 0/1/6/7/30 -> 0/1/1/2/5");
        for (int parts = 0; parts <= ImeBubbleModel::kMaxBeads; ++parts) {
            const int chunks = ImeBubbleModel::ChunksFor(parts);
            require(chunks * ImeBubbleModel::kMaxMeltParts >= parts, "the shapes carry every drop — the paint's silent six-drop cut can never bite");
            require(chunks == 0 || (chunks - 1) * ImeBubbleModel::kMaxMeltParts < parts, "and no shape sits empty: the last one is the one with room");
            require(chunks <= ImeBubbleModel::ChunksFor(ImeBubbleModel::kMaxBeads), "no swarm ever needs more shapes than the largest bubble");
        }
    });
    return failures ? 1 : 0;
}
