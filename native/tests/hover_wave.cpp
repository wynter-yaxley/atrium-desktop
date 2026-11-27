#include "near_hover.h"
#include "render/bar_painter.h"
#include "render/hover_wave.h"

#include <cmath>
#include <iostream>

using namespace atrium;

namespace {
bool g_failed = false;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        g_failed = true;
    }
}

BarScene Scene(float scale = 1)
{
    BarScene scene;
    scene.scale = scale;
    scene.bar = {20 * scale, 200 * scale, 420 * scale, 264 * scale};
    scene.magnify = 1.7f;
    scene.magnifyRange = 80 * scale;
    scene.magnifyLift = 20 * scale;
    scene.pointerX = 120 * scale;
    scene.pointerY = 220 * scale;
    scene.earlyRise = scene.earlyMagnify = true;
    return scene;
}

void Settle(HoverWave& wave, const BarScene& scene)
{
    for (int i = 0; i < 80; ++i)
        wave.Update(scene, 16);
}

void Move(BarScene& scene, NearHover& control, float distance) { control.Sample(scene, {scene.pointerX, scene.bar.top - distance * scene.scale}, 40, 4, true); }

void CheckSmallLeave()
{
    for (float scale : {1.f, 1.5f}) {
        BarScene scene = Scene(scale);
        HoverWave wave;
        NearHover control;
        Move(scene, control, -20);
        Settle(wave, scene);
        Require(wave.Height(scene.magnifyLift) == 20 * scale && wave.Scale(scene.pointerX, 40 * scale) > 1.6f,
                "ordinary hover establishes real glass rise and icon magnification before departure");
        Move(scene, control, 4);
        Settle(wave, scene);
        Require(wave.Height(scene.magnifyLift) == 20 * scale, "the geometry remains raised through the four-DIP leave boundary");
        Move(scene, control, 5);
        Require(scene.pointerX >= 0 && !scene.nearRise && !scene.nearMagnify,
                "departure clears the early response while the expanded input region still has the pointer");
        wave.Update(scene, 16);
        Require(wave.Height(scene.magnifyLift) < 20 * scale && wave.Scale(scene.pointerX, 40 * scale) < 1.6f,
                "leave four must start collapsing real geometry at five DIP despite ordinary hover input");
        Settle(wave, scene);
        Require(wave.Height(scene.magnifyLift) == 0 && wave.Scale(scene.pointerX, 40 * scale) == 1 && wave.MapX(20 * scale) == 20 * scale &&
                    wave.MapX(420 * scale) == 420 * scale,
                "released glass and icons return to baseline without waiting for the expanded input region to end");
    }
}

void CheckMoveBeforeTick()
{
    BarScene scene = Scene();
    HoverWave wave;
    NearHover control;
    Move(scene, control, -20); // the body tick leaves the early flags off
    Settle(wave, scene);
    control.Sample(scene, {120, 196}, 40, 4, true);
    wave.Update(scene, 16); // render the move before the next timer tick
    Require(wave.Height(20) == 20 && wave.Scale(120, 40) > 1.6f, "a move still within leave four must publish near flags with its new Y before rendering");
    control.Sample(scene, {120, 195}, 40, 4, true);
    wave.Update(scene, 16);
    Require(wave.Height(20) < 20, "the same coherent move sampling must release immediately beyond leave four");
}

void CheckIndependentChannels()
{
    for (bool releaseRise : {false, true}) {
        BarScene scene = Scene();
        scene.earlyRise = releaseRise;
        scene.earlyMagnify = !releaseRise;
        HoverWave wave;
        NearHover control;
        Move(scene, control, -20);
        Settle(wave, scene);
        Move(scene, control, 5);
        Settle(wave, scene);
        Require(wave.Height(20) == (releaseRise ? 0.f : 20.f), "the rise channel uses its own early-response switch rather than the icon switch");
        Require(releaseRise ? wave.Scale(120, 40) > 1.6f : wave.Scale(120, 40) == 1,
                "the magnification channel releases independently while ordinary hover still drives the other channel");
    }
}

void CheckOrdinaryHover()
{
    BarScene scene = Scene();
    scene.earlyRise = scene.earlyMagnify = false;
    scene.pointerY = 195;
    HoverWave wave;
    Settle(wave, scene);
    Require(wave.Height(20) == 20 && wave.Scale(120, 40) > 1.6f, "with early response disabled the expanded region keeps ordinary hover working");
    Require(std::fabs(wave.MapX(20) + 8) < 0.001f && std::fabs(wave.MapX(420) - 448) < 0.001f,
            "the shared production wave still maps the glass edges to the expected stretched positions");
    scene.pointerX = -1;
    Settle(wave, scene);
    Require(wave.Height(20) == 0 && wave.Scale(120, 40) == 1, "ordinary hover collapses when the input pointer leaves");
}

void CheckEarlyOnly()
{
    BarScene scene = Scene();
    scene.pointerX = -1;
    scene.nearX = 120;
    scene.nearRise = scene.nearMagnify = true;
    HoverWave wave;
    Settle(wave, scene);
    Require(wave.Height(20) == 20 && wave.Scale(120, 40) > 1.6f, "early response still animates before the pointer belongs to the input window");
    scene.nearRise = scene.nearMagnify = false;
    scene.nearX = -1;
    Settle(wave, scene);
    Require(wave.Height(20) == 0 && wave.Scale(120, 40) == 1, "early-only geometry collapses after release too");
}

void CheckDragging()
{
    BarScene scene = Scene();
    HoverWave wave;
    Settle(wave, scene);
    scene.dragKey = L"app";
    Settle(wave, scene);
    Require(wave.Height(20) == 0 && wave.Scale(120, 40) == 1, "dragging does not become ordinary hover in the shared wave consumer");
}
} // namespace

int main()
{
    for (auto check : {CheckSmallLeave, CheckMoveBeforeTick, CheckIndependentChannels, CheckOrdinaryHover, CheckEarlyOnly, CheckDragging})
        check();
    if (g_failed)
        return 1;
    std::cout << "Four-DIP departure, real wave geometry, independent channels and ordinary hover checks passed.\n";
    return 0;
}
