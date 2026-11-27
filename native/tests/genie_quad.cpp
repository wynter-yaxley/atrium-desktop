#include "ui/genie_minimize.h"
#include "ui/quad_sampling.h"

#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int g_failures = 0;

void Require(bool ok, const char* message)
{
    if (ok)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

HWND FakeHwnd(uintptr_t id) { return reinterpret_cast<HWND>(id); }

void TestNeedsMultiSampleQuad()
{
    // 1:1 quad -> false
    {
        const D2D1_POINT_2F quad[4] = {{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 100.0f}, {0.0f, 100.0f}};
        Require(!atrium::NeedsMultiSampleQuad(100.0f, 100.0f, quad), "1:1 quad must not require multi-sample");
    }

    // Bottom Dock vertical funnel neck -> true
    {
        const D2D1_POINT_2F quad[4] = {{0.0f, 0.0f}, {100.0f, 0.0f}, {60.0f, 10.0f}, {40.0f, 10.0f}};
        Require(atrium::NeedsMultiSampleQuad(100.0f, 100.0f, quad), "bottom dock vertical funnel neck must require multi-sample");
    }

    // Top Dock vertical funnel neck -> true
    {
        const D2D1_POINT_2F quad[4] = {{40.0f, 0.0f}, {60.0f, 0.0f}, {100.0f, 12.0f}, {0.0f, 12.0f}};
        Require(atrium::NeedsMultiSampleQuad(100.0f, 100.0f, quad), "top dock vertical funnel neck must require multi-sample");
    }

    // Left Dock horizontal funnel neck -> true
    {
        const D2D1_POINT_2F quad[4] = {{0.0f, 40.0f}, {12.0f, 0.0f}, {12.0f, 100.0f}, {0.0f, 60.0f}};
        Require(atrium::NeedsMultiSampleQuad(100.0f, 100.0f, quad), "left dock horizontal funnel neck must require multi-sample");
    }

    // Right Dock horizontal funnel neck -> true
    {
        const D2D1_POINT_2F quad[4] = {{0.0f, 0.0f}, {12.0f, 40.0f}, {12.0f, 60.0f}, {0.0f, 100.0f}};
        Require(atrium::NeedsMultiSampleQuad(100.0f, 100.0f, quad), "right dock horizontal funnel neck must require multi-sample");
    }

    // Trapezoidal quad with only ONE edge compressed >4:1 -> true
    {
        const D2D1_POINT_2F botEdgeCompressed[4] = {{0.0f, 0.0f}, {100.0f, 0.0f}, {60.0f, 100.0f}, {40.0f, 100.0f}};
        Require(atrium::NeedsMultiSampleQuad(100.0f, 100.0f, botEdgeCompressed), "single compressed horizontal edge >4:1 must require multi-sample");

        const D2D1_POINT_2F rightEdgeCompressed[4] = {{0.0f, 0.0f}, {100.0f, 40.0f}, {100.0f, 60.0f}, {0.0f, 100.0f}};
        Require(atrium::NeedsMultiSampleQuad(100.0f, 100.0f, rightEdgeCompressed), "single compressed vertical edge >4:1 must require multi-sample");
    }

    // Exact 4:1 boundary -> false; slightly above -> true
    {
        const D2D1_POINT_2F exact4[4] = {{0.0f, 0.0f}, {25.0f, 0.0f}, {25.0f, 25.0f}, {0.0f, 25.0f}};
        Require(!atrium::NeedsMultiSampleQuad(100.0f, 100.0f, exact4), "exact 4:1 compression must not require multi-sample");
        Require(atrium::NeedsMultiSampleQuad(100.1f, 100.0f, exact4), "just over 4:1 compression must require multi-sample");
    }

    // Rotated 1:1 quad (100x100 rotated 45 deg: side length 100) -> false
    {
        constexpr float kDiag = 70.710678f;
        const D2D1_POINT_2F rotated[4] = {{kDiag, 0.0f}, {2.0f * kDiag, kDiag}, {kDiag, 2.0f * kDiag}, {0.0f, kDiag}};
        Require(!atrium::NeedsMultiSampleQuad(100.0f, 100.0f, rotated), "rotated 1:1 quad must use Euclidean edge length");
    }

    // Degenerate pinched quad (< 0.5f / 0.0f edge) with valid source -> true without NaN/div-by-zero
    {
        const D2D1_POINT_2F zeroEdge[4] = {{10.0f, 10.0f}, {10.0f, 10.0f}, {50.0f, 80.0f}, {0.0f, 80.0f}};
        Require(atrium::NeedsMultiSampleQuad(100.0f, 100.0f, zeroEdge), "pinched zero-width edge with valid source must require multi-sample");

        const D2D1_POINT_2F tinyEdge[4] = {{10.0f, 10.0f}, {10.2f, 10.0f}, {50.0f, 80.0f}, {0.0f, 80.0f}};
        Require(atrium::NeedsMultiSampleQuad(4.0f, 100.0f, tinyEdge), "sub-half-pixel edge with >2px source must require multi-sample");
    }

    // Non-finite and zero/negative source edge cases -> false safely
    {
        const D2D1_POINT_2F unit[4] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}};
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        Require(!atrium::NeedsMultiSampleQuad(100.0f, 100.0f, nullptr), "nullptr quad must return false");
        Require(!atrium::NeedsMultiSampleQuad(0.0f, 100.0f, unit), "zero srcW must return false");
        Require(!atrium::NeedsMultiSampleQuad(100.0f, -1.0f, unit), "negative srcH must return false");
        Require(!atrium::NeedsMultiSampleQuad(nan, 100.0f, unit), "NaN srcW must return false");
        Require(!atrium::NeedsMultiSampleQuad(100.0f, inf, unit), "Inf srcH must return false");

        const D2D1_POINT_2F nanQuad[4] = {{0.0f, 0.0f}, {nan, 0.0f}, {10.0f, 10.0f}, {0.0f, 10.0f}};
        const D2D1_POINT_2F infQuad[4] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, inf}, {0.0f, 10.0f}};
        Require(!atrium::NeedsMultiSampleQuad(100.0f, 100.0f, nanQuad), "NaN quad coordinate must return false");
        Require(!atrium::NeedsMultiSampleQuad(100.0f, 100.0f, infQuad), "Inf quad coordinate must return false");
    }
}

void TestTransitionLeases()
{
    const HWND w1 = FakeHwnd(0x1001);
    const HWND w2 = FakeHwnd(0x1002);
    const HWND w3 = FakeHwnd(0x1003);

    std::unordered_map<HWND, bool> dwmDisabled{{w1, false}, {w2, true}, {w3, false}};
    std::unordered_map<HWND, bool> valid{{w1, true}, {w2, true}, {w3, true}};
    std::unordered_map<HWND, bool> skipped{{w1, false}, {w2, false}, {w3, false}};
    int setCalls = 0;

    auto query = [&](HWND h, BOOL& out) {
        if (h == w3)
            return false; // simulate DwmGetWindowAttribute failure -> defaults to FALSE
        out = dwmDisabled[h] ? TRUE : FALSE;
        return true;
    };
    auto set = [&](HWND h, bool disabled) {
        ++setCalls;
        dwmDisabled[h] = disabled;
    };
    auto isValid = [&](HWND h) { return valid[h]; };
    auto isSkipped = [&](HWND h) { return skipped[h]; };

    atrium::GenieTransitionLeases leases;

    // Nested leases on w1: prior false -> disabled while held -> restored only when final lease releases.
    leases.Acquire(w1, query, set);
    Require(leases.Count(w1) == 1 && dwmDisabled[w1] && setCalls == 1, "first acquire disables transitions");
    leases.Acquire(w1, query, set);
    Require(leases.Count(w1) == 2 && dwmDisabled[w1] && setCalls == 1, "nested acquire increments lease without re-querying");
    leases.Release(w1, true, isValid, set);
    Require(leases.Count(w1) == 1 && dwmDisabled[w1] && setCalls == 1, "first release keeps transitions disabled while lease count > 0");
    leases.Release(w1, true, isValid, set);
    Require(!leases.Contains(w1) && !dwmDisabled[w1] && setCalls == 2, "final release restores prior enabled transitions");

    // Prior DWMWA_TRANSITIONS_FORCEDISABLED == TRUE (e.g. Stage Manager) must NOT be clobbered to FALSE.
    leases.Acquire(w2, query, set);
    Require(leases.Count(w2) == 1 && dwmDisabled[w2], "acquire on already-disabled window keeps disabled");
    leases.Release(w2, true, isValid, set);
    Require(!leases.Contains(w2) && dwmDisabled[w2], "release preserves prior disabled transitions state");

    // Failed query defaults priorDisabled to FALSE and restores FALSE on release.
    leases.Acquire(w3, query, set);
    Require(leases.Count(w3) == 1 && dwmDisabled[w3], "failed prior query still disables transitions");
    leases.Release(w3, true, isValid, set);
    Require(!leases.Contains(w3) && !dwmDisabled[w3], "failed prior query defaults restore to FALSE");

    // Release with restoreAttribute=false (skipped owner) clears lease without changing DWM state.
    leases.Acquire(w1, query, set);
    const int callsBeforeSkip = setCalls;
    leases.Release(w1, false, isValid, set);
    Require(!leases.Contains(w1) && dwmDisabled[w1] && setCalls == callsBeforeSkip, "restoreAttribute=false does not clobber external owner");

    // Clear restores non-skipped valid windows and skips dead or skipped windows.
    dwmDisabled[w1] = false;
    dwmDisabled[w2] = false;
    dwmDisabled[w3] = false;
    leases.Acquire(w1, query, set);
    leases.Acquire(w2, query, set);
    leases.Acquire(w3, query, set);
    skipped[w2] = true;
    valid[w3] = false;
    leases.Clear(isSkipped, isValid, set);
    Require(leases.Empty(), "Clear empties all leases");
    Require(!dwmDisabled[w1], "Clear restores valid non-skipped window w1");
    Require(dwmDisabled[w2], "Clear leaves skipped window w2 untouched");
    Require(dwmDisabled[w3], "Clear skips invalid window w3");
}

void TestDrainShutdownRestores()
{
    struct TestFlight {
        std::wstring key;
        HWND window = nullptr;
        std::function<void()> restore;
        bool restoreTransitions = false;
    };

    const HWND validWin = FakeHwnd(0x2001);
    const HWND deadWin = FakeHwnd(0x2002);
    const HWND doneWin = FakeHwnd(0x2003);

    int validInvoked = 0;
    int deadInvoked = 0;
    std::vector<std::wstring> forgotten;
    std::vector<HWND> releasedLeases;

    std::vector<TestFlight> flights;
    flights.push_back({L"f1", validWin, [&] { ++validInvoked; }, true});
    flights.push_back({L"f2", deadWin, [&] { ++deadInvoked; }, true});
    flights.push_back({L"f3", doneWin, nullptr, false});

    auto isValid = [&](HWND h) { return h == validWin || h == doneWin; };
    auto forget = [&](const std::wstring& k) { forgotten.push_back(k); };
    auto release = [&](HWND h) { releasedLeases.push_back(h); };

    const auto restores = atrium::DrainGenieShutdownRestores(flights, isValid, forget, release);
    Require(flights.empty(), "flights cleared before invoking restores");
    Require(forgotten.size() == 3, "all flight image keys forgotten");
    Require(releasedLeases.size() == 2 && releasedLeases[0] == validWin && releasedLeases[1] == deadWin, "transition leases released for flagged flights");
    Require(restores.size() == 1, "only valid window with pending restore is collected");

    for (const auto& fn : restores)
        fn();
    Require(validInvoked == 1 && deadInvoked == 0, "valid restore invoked once and dead window skipped");

    // Second drain (idempotent shutdown) must produce nothing and never double-invoke.
    const auto second = atrium::DrainGenieShutdownRestores(flights, isValid, forget, release);
    Require(second.empty() && validInvoked == 1, "second shutdown drain does not double-invoke");
}

} // namespace

int main()
{
    TestNeedsMultiSampleQuad();
    TestTransitionLeases();
    TestDrainShutdownRestores();
    if (g_failures > 0)
        return 1;
    std::printf("genie quad: all checks passed\n");
    return 0;
}
