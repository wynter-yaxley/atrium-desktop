#include "near_hover.h"

#include <iostream>

using namespace atrium;

namespace {
const D2D1_RECT_F kGlass{20, 200, 420, 264};
bool g_failed = false;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        g_failed = true;
    }
}

void CheckApproach()
{
    NearHover hover;
    Require(hover.Update(kGlass, {120, 100}, 1, 40, 160) == -1, "an inactive pointer between the enter and leave distances does not start the wave");
    Require(hover.Update(kGlass, {120, 160}, 1, 40, 160) == 120, "the wave starts at the enter boundary");
    Require(hover.Update(kGlass, {160, 100}, 1, 40, 160) == 160, "an active wave follows the pointer beyond the enter distance");
    Require(hover.Update(kGlass, {160, 40}, 1, 40, 160) == 160, "the wave holds through the leave boundary");
    Require(hover.Update(kGlass, {160, 39}, 1, 40, 160) == -1, "the wave stops beyond the leave distance");
    Require(hover.Update(kGlass, {160, 100}, 1, 40, 160) == -1, "a stopped wave must enter again rather than restart in the hold band");
}

void CheckBarDeparture()
{
    NearHover hover;
    Require(hover.Update(kGlass, {120, 180}, 1, 40, 160) == 120, "approaching the bar starts the early wave");
    Require(hover.Update(kGlass, {120, 220}, 1, 40, 160) == -1, "the bar itself uses ordinary hover rather than a second early-wave centre");
    Require(hover.Update(kGlass, {160, 100}, 1, 40, 160) == 160, "leaving the bar directly into the hold band must use the leave distance");
    Require(hover.Update(kGlass, {160, 39}, 1, 40, 160) == -1, "leaving the bar must still stop beyond the leave distance");
}

void CheckDirectVisit()
{
    NearHover shortHold, longHold;
    // A mouse-move can visit the bar and leave between two timer ticks.
    Require(shortHold.Update(kGlass, {120, 220}, 1, 40, 48) == -1 && longHold.Update(kGlass, {120, 220}, 1, 40, 160) == -1,
            "a direct visit leaves the ordinary hover in charge");
    Require(shortHold.Update(kGlass, {120, 100}, 1, 40, 48) == -1, "a short leave distance releases a departing pointer");
    Require(longHold.Update(kGlass, {120, 100}, 1, 40, 160) == 120, "changing only the leave distance changes the result after a direct visit");
}

void CheckEdges()
{
    NearHover hover;
    Require(hover.Update(kGlass, {4, 180}, 1, 40, 160) == 20, "the left side margin clamps the wave centre to the glass");
    Require(hover.Update(kGlass, {436, 100}, 1, 40, 160) == 420, "the right side margin still holds an active wave");
    Require(hover.Update(kGlass, {437, 100}, 1, 40, 160) == -1, "leaving the side margin releases the wave");
    Require(hover.Update(kGlass, {120, 100}, 1, 40, 160) == -1, "returning from the side into only the hold band does not restart the wave");
    Require(hover.Update(kGlass, {120, 160}, 1, 40, 160) == 120, "the enter band rearms the wave after a side exit");
    Require(hover.Update(kGlass, {120, 264}, 1, 40, 160) == -1 && hover.Update(kGlass, {120, 100}, 1, 40, 160) == -1,
            "leaving below the bar releases the hold too");
}

void CheckScale()
{
    const D2D1_RECT_F scaled{30, 300, 630, 396};
    NearHover hover;
    Require(hover.Update(scaled, {180, 239}, 1.5f, 40, 160) == -1, "DIP entry distance is scaled into client pixels");
    Require(hover.Update(scaled, {6, 240}, 1.5f, 40, 160) == 30, "both the entry boundary and side margin scale with monitor DPI");
    Require(hover.Update(scaled, {180, 330}, 1.5f, 40, 160) == -1, "ordinary hover takes over on a scaled bar");
    Require(hover.Update(scaled, {180, 60}, 1.5f, 40, 160) == 180 && hover.Update(scaled, {180, 59}, 1.5f, 40, 160) == -1,
            "a scaled departure holds through exactly the scaled leave distance");
}

void CheckReset()
{
    NearHover hover;
    Require(hover.Update(kGlass, {120, 180}, 1, 40, 160) == 120, "the wave is active before hiding or disabling it");
    hover.Reset();
    Require(hover.Update(kGlass, {120, 100}, 1, 40, 160) == -1, "hiding or disabling the wave clears the hold");
    hover.Update(kGlass, {120, 220}, 1, 40, 160);
    hover.Reset();
    Require(hover.Update(kGlass, {120, 100}, 1, 40, 160) == -1, "reset also clears a hold armed by ordinary hover alone");
}

void CheckSmallLeave()
{
    NearHover hover;
    hover.Update(kGlass, {120, 220}, 1, 40, 4);
    Require(hover.Update(kGlass, {120, 196}, 1, 40, 4) == 120, "a departing pointer stays active through the configured four-DIP boundary");
    Require(hover.Update(kGlass, {120, 195}, 1, 40, 4) == -1, "leave four must release at five DIP even when enter is forty");
    for (int i = 0; i < 10; ++i)
        Require(hover.Update(kGlass, {120, 195}, 1, 40, 4) == -1, "a released pointer resting inside the enter band must not reactivate");
    Require(hover.Update(kGlass, {120, 180}, 1, 40, 4) == -1 && hover.Update(kGlass, {120, 181}, 1, 40, 4) == -1,
            "small reversals outside the leave band do not flicker the wave back on");
    Require(hover.Update(kGlass, {120, 196}, 1, 40, 4) == 120, "returning within the close band can reactivate the wave");
}

void CheckSmallLeaveApproach()
{
    NearHover hover;
    Require(hover.Update(kGlass, {120, 140}, 1, 40, 4) == -1 && hover.Update(kGlass, {120, 160}, 1, 40, 4) == 120 &&
                hover.Update(kGlass, {120, 180}, 1, 40, 4) == 120,
            "a short leave distance must not shrink the independent forty-DIP approach distance");
    for (int i = 0; i < 10; ++i)
        Require(hover.Update(kGlass, {120, 180}, 1, 40, 4) == 120, "an approaching pointer can rest without alternating enter and leave");
    Require(hover.Update(kGlass, {120, 179}, 1, 40, 4) == -1, "reversing away outside the short leave band ends the early wave");
    Require(hover.Update(kGlass, {120, 180}, 1, 40, 4) == -1, "an exit must not immediately rearm inside the wider enter band");
    Require(hover.Update(kGlass, {120, 159}, 1, 40, 4) == -1 && hover.Update(kGlass, {120, 160}, 1, 40, 4) == 120,
            "a fresh approach from outside the enter band rearms the wave");
}

void CheckSmallLeaveScale()
{
    const D2D1_RECT_F scaled{30, 300, 630, 396};
    NearHover hover;
    hover.Update(scaled, {180, 330}, 1.5f, 40, 4);
    Require(hover.Update(scaled, {180, 294}, 1.5f, 40, 4) == 180, "four DIP holds through six client pixels at 150 percent DPI");
    Require(hover.Update(scaled, {180, 293}, 1.5f, 40, 4) == -1, "a small scaled leave distance is not replaced with the enter distance");
}
} // namespace

int main()
{
    for (auto check : {CheckApproach, CheckBarDeparture, CheckDirectVisit, CheckEdges, CheckScale, CheckReset, CheckSmallLeave, CheckSmallLeaveApproach,
                       CheckSmallLeaveScale})
        check();
    if (g_failed)
        return 1;
    std::cout << "Early hover entry, bar departure, leave distance, DPI and reset checks passed.\n";
    return 0;
}
