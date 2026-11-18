// The one-shot IME bubble: while the bar of the display a window types in is
// hidden, a change of the input method or its Chinese/English state announces
// itself as a liquid glass bubble at the bottom of that display — it swells in,
// drifts up a little, bursts into foam and is gone. Same material and adaptive
// ink as every popup (the bar's render thread draws the glass, the bubble
// itself being several pieces like the stage's drag carrier); the window never
// takes focus or input.
//
// Quick successive changes merge the bubble rather than restart it, as real
// bubbles do: each merge is one step larger (a logarithmic size in the number
// of merges, up to ten — a volume argument in spirit, the asked-for log in
// shape), bounces softly with a springy overshoot, pushes its pop back, and
// the bubble shows only the newest state. A change never queues behind the
// last one: it launches its own newcomer at once, so a rapid run rises as
// several small bubbles side by side and fuses them into the skin together.
//
// The timeline and its constants are the model's (ime_bubble_model.h); this
// surface follows it frame by frame and takes itself down at the model's end.
#pragma once

#include "shell/ime_status.h"
#include "ui/ime_bubble_model.h"
#include "ui/surface.h"

#include <windows.h>

#include <string>

namespace atrium {

// The surface: a small fixed window at the bottom of the work area, whose
// content is the bubble's letter and whose glass follows the model frame by
// frame. Lives on the UI thread.
class ImeBubble {
public:
    explicit ImeBubble(HINSTANCE instance);

    bool Visible() const { return bubble_.Visible(); }

    // The freshly settled state: merges with a live bubble (one step larger,
    // newest state shown), or starts a new one. `work` is the display's work
    // area and `screenBottom` its physical bottom edge: the window's bottom
    // edge sits there, so the resting bubble hangs just above the screen's
    // bottom and a newcomer can be born off the screen below it.
    void Show(const ImeStatus& status, RECT work, int screenBottom);

    // Takes a live bubble down at once (its setting was turned off): no glass
    // left behind, no waiting for the model's life to end.
    void Cancel();

private:
    static constexpr UINT_PTR kEndTimer = 1;

    // The window: centred on the display, its bottom edge on the display's own
    // physical bottom (`screenBottom`, from GetMonitorInfo's rcMonitor.bottom),
    // tall enough for the whole model (kH*scale) and growing upward from there.
    // The resting bubble therefore hangs just above the screen's bottom edge,
    // and a newcomer can be born below the window and rise up out of the screen.
    RECT RectFor() const;
    void ArmEndTimer(double now);
    void Stop();
    void Paint(Canvas& c);

    std::wstring glyph_;       // the OS's mark for this language and mode, from Show
    RECT work_{};              // the display's work area, from GetMonitorInfo
    int screenBottom_ = 0;     // the display's physical bottom edge, from rcMonitor.bottom: the window's own bottom
    ImeBubbleModel model_;
    Surface bubble_;
};

} // namespace atrium
