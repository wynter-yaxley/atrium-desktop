// The IME bubble's window: its placement on the display, its end timer, and
// the drawing of the model's frame as glass pieces and ink. The shape and the
// timeline are the model's (ime_bubble_model.h).

#include "ui/ime_bubble.h"

#include "clock.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace atrium {

ImeBubble::ImeBubble(HINSTANCE instance) : bubble_(instance, L"Atrium IME Bubble", false, 0)
{
    bubble_.paint = [this](Canvas& c) { Paint(c); };
    bubble_.message = [this](UINT m, WPARAM w, LPARAM, LRESULT& result) {
        if (m == WM_TIMER && w == kEndTimer) {
            const double now = ClockMs();
            if (!model_.Step(now).active)
                Stop(); // outside Paint; a stale timer cannot cut a newer bubble short
            else
                ArmEndTimer(now); // a merge pushed the pop back after the timer was armed
            return true;
        }
        if (m != WM_NCHITTEST)
            return false;
        result = HTTRANSPARENT;
        return true;
    };
}

void ImeBubble::Show(const ImeStatus& status, RECT work, int screenBottom)
{
    glyph_ = ImeGlyph(status); // the OS answers for this language, once per announce
    const bool moved = !EqualRect(&work_, &work) || screenBottom != screenBottom_;
    work_ = work;
    screenBottom_ = screenBottom;
    const double now = ClockMs();
    if (!bubble_.Visible() || !model_.Fuse(now)) {
        model_.Begin(now);
        const RECT rect = RectFor();
        if (bubble_.Visible())
            bubble_.Move(rect);
        else
            bubble_.Show(rect, false, POINT{(rect.left + rect.right) / 2, rect.bottom}, false);
    } else if (moved) {
        bubble_.Move(RectFor()); // the display changed with the bubble up
    }
    bubble_.Invalidate();
    ArmEndTimer(now);
}

void ImeBubble::Cancel() { Stop(); }

RECT ImeBubble::RectFor() const
{
    const float scale = PopupScale(work_);
    const int w = int(std::lround(ImeBubbleModel::kW * scale)), h = int(std::lround(ImeBubbleModel::kH * scale));
    const int bottom = screenBottom_;
    return RECT{(work_.left + work_.right - w) / 2, bottom - h, (work_.left + work_.right + w) / 2, bottom};
}

void ImeBubble::ArmEndTimer(double now)
{
    const HWND hwnd = bubble_.Hwnd();
    if (!hwnd)
        return; // no window to draw in: no end to wait for either
    const double ms = model_.EndMs() - now + 60;
    if (!SetTimer(hwnd, kEndTimer, UINT(std::max(60.0, ms)), nullptr))
        Stop(); // the end can no longer be delivered: take the bubble down now
}

void ImeBubble::Stop()
{
    if (const HWND hwnd = bubble_.Hwnd())
        KillTimer(hwnd, kEndTimer);
    model_.Cancel();
    bubble_.Hide(false); // the content is already transparent; no second animation
}

void ImeBubble::Paint(Canvas& c)
{
    const auto f = model_.Step(c.now);
    if (!f.active) { // done: every piece of glass gone; the end timer hides the window
        // A 1x1 faded shape, not an empty rectangle: the glass renderer
        // reads a degenerate shape as "the whole window" and would flash
        // a panel-sized rectangle.
        std::vector<ShapeDip> gone{ShapeDip{D2D1::RectF(0, 0, 1, 1), 0}};
        gone[0].fade = 1;
        bubble_.SetShapes(gone);
        return;
    }
    // The bubble as one circle, then the burst as two fusing pieces — the
    // foam drops and the mist behind them. Only shapes with real geometry
    // are published: an empty one would be read as the whole window by the
    // glass renderer (the square flash), and the groups never fade as a
    // whole anyway (each drop dies by thinning its own radius to zero).
    const float r = ImeBubbleModel::kRadius * f.scale;
    // A still, whole skin — no newcomer, no wobble, no burst yet: its glass
    // is published rounded to whole DIPs, so the frames of the slow float
    // are byte-identical and the bar's render thread skips that glass
    // Present. Frames that merge, wobble or burst are never rounded.
    const float wob = std::abs(f.wob);
    const bool steady = f.lobeCount == 0 && f.beadCount == 0 && f.opacity >= 1 && wob < 0.04f;
    std::vector<ShapeDip> shapes;
    shapes.reserve(3);
    ShapeDip main{};
    const auto budget = ImeBubbleModel::MeltFor(f.lobeCount, wob > 0.04f);
    if (budget.lobes > 0 || budget.skinParts == 2) {
        // The merge, as one liquid body: the skin squashed wider by its
        // wobble (two melt-fused lobes), the newcomers swallowed into it —
        // the glass renderer's melt keeps them a single blob throughout.
        auto part = [](float px, float py, float rad) {
            ShapeDip::Part p;
            p.rect = D2D1::RectF(px - rad, py - rad, px + rad, py + rad);
            std::fill(std::begin(p.radii), std::end(p.radii), rad);
            return p;
        };
        std::vector<ShapeDip::Part> parts;
        if (budget.skinParts == 2) {
            const float d = r * 0.42f * wob;        // stretch across the impact
            const float rr = r * (1 - 0.10f * wob); // and thin along it
            parts.push_back(part(f.cx - d, f.cy, rr));
            parts.push_back(part(f.cx + d, f.cy, rr));
        } else {
            parts.push_back(part(f.cx, f.cy, r));
        }
        // The newest newcomers are the ones with room in the melt budget.
        for (int i = f.lobeCount - budget.lobes; i < f.lobeCount; ++i)
            if (f.lobes[i].r > 0.5f)
                parts.push_back(part(f.lobes[i].x, f.lobes[i].y, f.lobes[i].r));
        main.rect = parts.front().rect;
        for (const auto& p : parts) {
            main.rect.left = std::min(main.rect.left, p.rect.left);
            main.rect.top = std::min(main.rect.top, p.rect.top);
            main.rect.right = std::max(main.rect.right, p.rect.right);
            main.rect.bottom = std::max(main.rect.bottom, p.rect.bottom);
        }
        main.parts = std::move(parts);
        main.melt = 12; // a liquid neck, not separate beads
        main.radius = r;
    } else {
        const float rr = steady ? std::round(r) : r;
        const float cx = steady ? std::round(f.cx) : f.cx;
        const float cy = steady ? std::round(f.cy) : f.cy;
        main.rect = D2D1::RectF(cx - rr, cy - rr, cx + rr, cy + rr);
        main.radius = rr;
    }
    main.fade = 1 - f.opacity;
    const D2D1_RECT_F mainRect = main.rect;
    shapes.push_back(std::move(main));
    // The burst's drops, foam first and the mist behind it. The glass
    // renderer melts at most kMaxMeltParts (6) parts per shape and drops
    // the rest without a word, so a bubble whose swarm is larger than that
    // (a merged one throws up to kMaxBeads drops) would show only its first
    // six — and a small bubble then looks exactly as full as a big one.
    // Every full group of drops is published as its own melt shape, so the
    // whole swarm is drawn, and the count finally reads.
    std::vector<ShapeDip::Part> foamParts, mistParts;
    foamParts.reserve(size_t(f.foamCount));
    mistParts.reserve(size_t(f.beadCount - f.foamCount));
    for (int i = 0; i < f.beadCount; ++i) {
        const auto& bead = f.beads[i];
        if (bead.r < 0.35f)
            continue; // dissolved: it thinned away, it did not blink off
        ShapeDip::Part part;
        part.rect = D2D1::RectF(bead.x - bead.r, bead.y - bead.r, bead.x + bead.r, bead.y + bead.r);
        std::fill(std::begin(part.radii), std::end(part.radii), bead.r);
        (i < f.foamCount ? foamParts : mistParts).push_back(part);
    }
    // One melt shape per group of at most kMaxMeltParts drops, its rectangle
    // the union of the group it carries (ChunksFor gives the count).
    auto publish = [&](const std::vector<ShapeDip::Part>& parts, float melt) {
        for (int done = 0; done < ImeBubbleModel::ChunksFor(int(parts.size())); ++done) {
            const int first = done * ImeBubbleModel::kMaxMeltParts;
            const int last = std::min(first + ImeBubbleModel::kMaxMeltParts, int(parts.size()));
            ShapeDip shape{};
            shape.parts.assign(parts.begin() + first, parts.begin() + last);
            shape.rect = parts[size_t(first)].rect;
            for (int i = first; i < last; ++i) {
                const auto& p = parts[size_t(i)].rect;
                shape.rect.left = std::min(shape.rect.left, p.left);
                shape.rect.top = std::min(shape.rect.top, p.top);
                shape.rect.right = std::max(shape.rect.right, p.right);
                shape.rect.bottom = std::max(shape.rect.bottom, p.bottom);
            }
            shape.melt = melt;
            shape.fade = 0;
            shapes.push_back(std::move(shape));
        }
    };
    publish(foamParts, 2.5f + (f.grow - 1) * 2); // a larger bubble fuses a larger foam
    publish(mistParts, 1.5f);
    bubble_.SetShapes(shapes, !steady); // a still skin publishes once, not on every frame

    // The letter, over the glass — or a plain stand-in while the glass is on its way.
    const auto theme = c.panel ? Theme::Panel() : c.theme;
    if (c.panel) {
        c.Circle(f.cx, f.cy, r, theme.panel);
        for (int i = 0; i < f.lobeCount; ++i)
            if (f.lobes[i].r > 0.5f)
                c.Circle(f.lobes[i].x, f.lobes[i].y, f.lobes[i].r, theme.panel);
        c.Stroke(mainRect, r, theme.border);
        for (int i = 0; i < f.beadCount; ++i)
            if (f.beads[i].r >= 0.35f)
                c.Circle(f.beads[i].x, f.beads[i].y, f.beads[i].r, theme.panel);
    }
    // A drop's last sparkle, as two layers so it reads on light and dark
    // alike: a wide contrasting halo, then a bright white core.
    for (int i = 0; i < f.beadCount; ++i) {
        const auto& bead = f.beads[i];
        if (bead.glint <= 0.02f)
            continue;
        const float radius = std::max(1.8f, bead.r * 0.9f + 0.8f);
        D2D1_COLOR_F haloInk = theme.halo;
        haloInk.a *= 0.35f * bead.glint;
        c.Circle(bead.x, bead.y, radius * 1.7f, haloInk);
        c.Circle(bead.x, bead.y, radius, D2D1::ColorF(1.f, 1.f, 1.f, 0.95f * bead.glint));
    }
    if (f.opacity <= 0.01f) {
        c.Continue(); // still fading in (or out): the chain of frames must not stop
        return;
    }
    // Drawn at the resting place, carried to the frame's centre and size.
    // The liquid wobble rocks the letter with the glass: a small sideways
    // shove, an opposing nudge down, and a matching breath in scale.
    c.BeginGroup(f.opacity, f.scale * (1 + 0.035f * f.wob), f.cx, f.cy, f.cx - ImeBubbleModel::kCx + f.wob * 2.5f,
                 f.cy - ImeBubbleModel::kCyRest - f.wob * 0.8f);
    // The glyph is the OS's for this language (中/a/A/その言語の印). A
    // lowercase latin letter rides the baseline, and a line box centred on
    // the bubble leaves its body a little low: lift it so 'a' sits in the
    // middle. Capitals and the scripts' own marks already fill theirs.
    const bool lowLatin = glyph_.size() == 1 && glyph_[0] >= L'a' && glyph_[0] <= L'z';
    const float lift = lowLatin ? 2.5f : 0.f;
    c.Text(glyph_, D2D1::RectF(ImeBubbleModel::kCx - 32, ImeBubbleModel::kCyRest - 17 - lift, ImeBubbleModel::kCx + 32, ImeBubbleModel::kCyRest + 17 - lift),
           26, theme.text, DWRITE_FONT_WEIGHT_DEMI_BOLD, 1);
    c.EndGroup();
    c.Continue();
}

} // namespace atrium
