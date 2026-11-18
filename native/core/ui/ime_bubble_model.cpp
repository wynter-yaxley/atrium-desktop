// The IME bubble's timeline: the frames of its life (emerge, float, pop, foam),
// the merges that grow and rock the skin, and the budgets the glass needs. The
// model is described in ime_bubble_model.h; nothing here draws or opens a
// window.

#include "ui/ime_bubble_model.h"

#include "motion.h"

#include <algorithm>
#include <cmath>

namespace atrium {

ImeBubbleModel::MeltBudget ImeBubbleModel::MeltFor(int lobeCount, bool split)
{
    const int n = std::clamp(lobeCount, 0, kMaxFrameLobes);
    MeltBudget b;
    if (split && n <= kMaxMeltParts - 2) {
        b.skinParts = 2;
        b.lobes = n;
    } else {
        b.lobes = std::min(n, kMaxMeltParts - 1); // the split dropped: one more slot for a newcomer
    }
    return b;
}

int ImeBubbleModel::DrawnLobes(int lobeCount, bool split) { return MeltFor(lobeCount, split).lobes; }

int ImeBubbleModel::ChunksFor(int parts) { return parts <= 0 ? 0 : (parts + kMaxMeltParts - 1) / kMaxMeltParts; }

int ImeBubbleModel::BeadsFor(int fusions, int roll) const
{
    const int base = kMinBeads + std::min(roll, 4);
    return std::clamp(base + int((ScaleAt(fusions) - 1) * 40), kMinBeads, kMaxBeads); // +-0..24
}

float ImeBubbleModel::SpreadFor(int beads, float temper)
{
    const float more = float(beads - kMinBeads) / float(kMaxBeads - kMinBeads);
    return temper * (1 + 0.25f * std::clamp(more, 0.f, 1.f)); // up to 1.25
}

float ImeBubbleModel::RadiusAt(int fusions) const
{
    const float t = std::log(float(std::clamp(fusions, 1, kMaxFusions))) / std::log(float(kMaxFusions));
    return kStartRadius + (kEndRadius - kStartRadius) * t;
}

float ImeBubbleModel::ScaleAt(int fusions) const { return RadiusAt(fusions) / kRadius; }

double ImeBubbleModel::RiseEndMs() const { return kLobeRiseMs; }
double ImeBubbleModel::AttachEndMs() const { return TouchMs(); }
double ImeBubbleModel::AbsorbMs() const { return double(kLobeRiseMs + kLobeAttachMs + kLobeSwallowMs); }
double ImeBubbleModel::TouchMs() const { return std::min(RiseEndMs() + kLobeAttachMs, AbsorbMs() - 1); }
double ImeBubbleModel::TouchFrac() const { return TouchMs() / AbsorbMs(); }

float ImeBubbleModel::softBounce(double ms, float rebound)
{
    const double r = std::clamp(double(rebound), 0.02, 0.6);
    const double k = std::max(2.0, -16.0 * std::log(r) / 3.14159265358979323846);
    const double t = ms / 1000;
    return 1 - float(std::exp(-k * t) * std::cos(16 * t));
}

bool ImeBubbleModel::Mergeable(double now) const { return active_ && std::isfinite(now) && now >= since_ && now < popAt_; }

void ImeBubbleModel::Begin(double now)
{
    *this = ImeBubbleModel{};
    since_ = std::isfinite(now) && now > 0 ? now : 0;
    if (!since_)
        return;
    active_ = true;
    popAt_ = since_ + kLifeMs;
    fusions_ = 1;
    // ClockMs() may run past 2^32 after weeks of uptime; keep the seed conversion
    // inside unsigned's range (the mix only needs to differ between bubbles).
    rng_.seed(static_cast<unsigned>(std::fmod(since_, 4294967296.0)) ^ 0x9E3779B9u);
    PlanBurst();
}

bool ImeBubbleModel::Fuse(double now)
{
    if (!Mergeable(now))
        return false;
    ++fusions_;
    popAt_ = std::max({popAt_, now + double(kLifeMs), now + AbsorbMs()});
    lobes_.push_back({now, kCx + U(-50.f, 50.f), // where it is born, below the window
                      kWobbleHz * U(0.85f, 1.15f),  // its own liquid rhythm
                      kWobbleAmp * U(0.85f, 1.15f), // and strength
                      fusions_});                   // the size its touch adds
    PlanBurst();
    return true;
}

void ImeBubbleModel::Cancel() { *this = ImeBubbleModel{}; }

ImeBubbleModel::Frame ImeBubbleModel::Step(double now)
{
    if (!active_ || !std::isfinite(now))
        return {};
    const double t = now - since_;
    if (t < 0 || now >= EndMs())
        return {};
    const double popRel = popAt_ - since_; // where the pop sits in the life
    // The newcomer's absorb and its two stage boundaries, from the constants:
    // [0, uRise) the slow rise, [uRise, uAttach) the fast approach into the
    // rim, [uAttach, 1) the swallow.
    const double absorb = AbsorbMs();
    const double uRise = RiseEndMs() / absorb;
    const double uAttach = TouchFrac();
    Frame f;
    f.active = f.animating = true;
    // The merges as one elastic body: every newcomer whose touch has come
    // adds its own step to the size — optimistically, so a newer touch
    // never takes an older step back — and its own liquid rhythm to the
    // wobble. Before any touch the size is the base.
    float grow = ScaleAt(1), wob = 0;
    for (const auto& l : lobes_) {
        const double sinceTouch = now - (l.launch + TouchMs());
        if (sinceTouch < 0)
            continue;
        grow += (ScaleAt(l.step) - ScaleAt(l.step - 1)) * softBounce(sinceTouch, kRebound);
        if (sinceTouch < kWobbleMs)
            wob += WobbleAt(sinceTouch, l.amp, l.freq);
    }
    const float wobLimit = 1.15f * kWobbleAmp;
    f.grow = grow;
    f.wob = std::clamp(wob, -wobLimit, wobLimit);
    // The centre: up from below during the emerge, then the float carries
    // it a little past its rest, upwards. How far below it is born is
    // kFirstBirthDip.
    float rise = kFirstBirthDip * float(1 - ease(t / kEmergeMs));
    if (t > kEmergeMs)
        rise -= kFloatRise * easeInOut(float((t - kEmergeMs) / kFloatMs));
    f.cy = kCyRest + rise;
    // The size: a springy swell in, a breath while floating, the pressure
    // before the burst — which swells once more, this time for real, and
    // pushes the skin up as it leaves. A merge rides along as `grow`.
    float phase;
    if (t < kEmergeMs)
        phase = 0.5f + 0.5f * motion::EaseOutBack(float(t / kEmergeMs), 1.70158f);
    else if (t < popRel)
        phase = 1 + 0.012f * float(std::sin(6.2831853f * float((t - kEmergeMs) / kFloatMs)));
    else if (t - popRel < kSwellMs)
        phase = 1 + (swellPeak_ - 1) * ease(float(t - popRel) / kSwellMs);
    else
        phase = swellPeak_ + (burstScale_ - swellPeak_) * ease(float((t - popRel - kSwellMs) / 110));
    // The newcomers, oldest first: each rises from the emerge line as a
    // bubble of its own, touches the skin and is swallowed. A rapid run
    // shows them side by side — a frame carries the newest ones — and where
    // one meets the skin it squashes it, the squashes adding up.
    int live = 0;
    for (const auto& l : lobes_)
        live += now >= l.launch && now < l.launch + absorb;
    float dip = 0;
    for (const auto& l : lobes_) {
        if (now < l.launch || now >= l.launch + absorb)
            continue;
        const bool room = live <= kMaxFrameLobes;
        --live;
        if (!room)
            continue; // an older newcomer the frame has no room for
        const float u = float((now - l.launch) / absorb);
        const float rMain = kRadius * grow; // the skin it meets (path/touch geometry keeps using this)
        // Its own size does not follow the skin's growth: every newcomer is born
        // the size of the first newcomer after the initial bubble — the bubble's
        // start radius (ScaleAt(1) · kRadius), not the skin's current radius.
        const float rRef = kStartRadius;
        const float rStart = rRef * 0.30f, rTouch = rRef * 0.55f;
        // Born below the window: its centre kLobeBirthDip past the window's
        // bottom, so the first frames are clipped and it then drills up out of
        // the screen's very bottom edge.
        const float startX = l.startX, startY = kH + kLobeBirthDip;
        // The line from the main centre out to the birth point; every point
        // of the journey lies on it, so the newcomer only ever rises.
        float dx = startX - kCx, dy = startY - f.cy;
        const float len = std::max(1.f, std::hypot(dx, dy));
        const float ux = dx / len, uy = dy / len;
        // P1, the hover it floats up to: below the skin, on that line. P2,
        // the rim: their rims meet at touchDist — but never past the birth
        // point, so a big main bubble (which sits near the bottom) cannot
        // push the touch point below the start and off the window.
        const float hoverDist = rMain + rTouch * 2.2f;
        const float hx = kCx + ux * hoverDist, hy = f.cy + uy * hoverDist;
        const float touchDist = std::min(rMain + rTouch * 0.10f, len);
        const float tx = kCx + ux * touchDist, ty = f.cy + uy * touchDist;
        LobeFrame& lobe = f.lobes[f.lobeCount++];
        lobe.u = u;
        if (u < float(uRise)) { // the slow rise: off-screen up to the hover
            const float e = easeInOut(u / float(uRise));
            lobe.x = startX + (hx - startX) * e;
            lobe.y = startY + (hy - startY) * e;
            lobe.r = rStart + (rTouch * 0.8f - rStart) * e;
        } else if (u < float(uAttach)) { // the fast approach: the hover into the rim
            const float e = easeIn((u - float(uRise)) / float(uAttach - uRise));
            lobe.x = hx + (tx - hx) * e;
            lobe.y = hy + (ty - hy) * e;
            lobe.r = rTouch * 0.8f + (rTouch - rTouch * 0.8f) * e;
        } else { // the swallow: into the middle, shrinking away
            const float e = ease((u - float(uAttach)) / float(1 - uAttach));
            lobe.x = tx + (kCx - tx) * e;
            lobe.y = ty + (f.cy - ty) * e;
            lobe.r = rTouch * (1 - 0.96f * e);
        }
        const float d = (u - float(uAttach)) / float(kDipSpan);
        dip += float(std::exp(-d * d)); // the hit compresses the skin
    }
    phase *= 1 - kCompression * std::min(dip, 1.f);
    f.scale = grow * phase;
    if (t < popRel)
        f.opacity = t < 220 ? ease(float(t) / 220) : 1;
    else {
        const float tb = float((t - popRel - kSwellMs) / 1000); // s since the skin gave way
        f.opacity = 1 - ease(tb * 1000 / 120);
        f.cy -= 3 * ease(tb * 1000 / 120);
        Tear(tb, grow, f);
    }
    return f;
}

void ImeBubbleModel::PlanBurst()
{
    beadCount_ = std::max(kMinBeads, BeadsFor(fusions_, int(rng_() % 5)));
    spread_ = SpreadFor(beadCount_, U(0.88f, 1.12f));
    foamCount_ = std::clamp(beadCount_ - (2 + int(rng_() % 3)), 5, beadCount_ - 2); // the rest is mist
    auto plan = [&](float degA, float degB, float speedA, float speedB, float rA, float rB, float lifeA, float lifeB) {
        BeadPlan b;
        b.deg = U(degA, degB);
        b.speed = U(speedA, speedB);
        b.r0 = U(rA, rB);
        b.life = U(lifeA, lifeB);
        b.wobbleAmp = U(0.8f, 3.2f);
        b.wobbleHz = U(6, 13);
        b.phase = U(0, 6.2831853f);
        b.droopAfter = U(0.08f, 0.16f);
        return b;
    };
    // A jet at the top (the liquid spike of a real pop), a few drops, then mist.
    beads_[kJet] = plan(-104.f, -76.f, 175.f, 215.f, 5.0f, 6.8f, 330.f, 420.f);
    for (int i = 1; i < foamCount_; ++i)
        beads_[i] = plan(-175.f, -5.f, i < 4 ? 115.f : 80.f, i < 4 ? 170.f : 130.f, i < 4 ? 4.0f : 2.6f, i < 4 ? 5.6f : 4.0f, 300.f, 420.f);
    for (int i = foamCount_; i < beadCount_; ++i)
        beads_[i] = plan(-175.f, -5.f, 45.f, 105.f, 1.1f, 2.2f, 170.f, 290.f);
    swellPeak_ = U(1.15f, 1.22f);
    burstScale_ = U(1.42f, 1.58f);
    jitterX_ = U(-2, 2);
    jitterY_ = U(-1.5f, 1.5f);
}

void ImeBubbleModel::Tear(float tb, float grow, Frame& f)
{
    const float rSkin = kRadius * grow * swellPeak_; // where the skin stood when it burst
    const float bx = kCx + jitterX_, by = kCyRest - kFloatRise + jitterY_;
    const float glide = std::min(tb, kGlideS);
    const float travel = glide - glide * glide / (2 * kGlideS); // v*t with a constant deceleration
    const float size = (0.75f + 0.25f * grow) * spread_; // a bigger, fuller swarm scatters further
    f.beadCount = beadCount_;
    f.foamCount = foamCount_;
    for (int i = 0; i < beadCount_; ++i) {
        const auto& b = beads_[i];
        const float ms = tb * 1000;
        const float u = std::clamp(ms / b.life, 0.f, 1.f); // its own lifetime, 0..1
        const float rad = b.deg * (3.1415926f / 180);
        // The swarm scatters a touch wider sideways than along the pop's own
        // vertical: a drop flying level (or at any angle off straight up)
        // carries this extra reach, while the jet at the top keeps its own
        // pace (it already reaches the window's top edge). 1.20 level,
        // 1.06 at 45 degrees, 1.00 straight up.
        const float dirSpread = 1 + 0.20f * (1 - std::abs(std::sin(rad)));
        const float away = rSkin + b.speed * dirSpread * size * travel; // drops leave from the rim, not the middle
        const float wob = std::sin(6.2831853f * b.wobbleHz * tb + b.phase) * b.wobbleAmp * float(1.0 - u);
        f.beads[i].x = bx + float(std::cos(rad) * away - std::sin(rad) * wob);
        f.beads[i].y = by + float(std::sin(rad) * away + std::cos(rad) * wob) + kDroop * std::max(0.f, tb - b.droopAfter) * std::max(0.f, tb - b.droopAfter);
        // It thins away to nothing over its own life — never blinks off.
        f.beads[i].r = u >= 1 ? 0 : b.r0 * grow * (1 - u * u);
        // And its last sparkle: the final 30 % of its life.
        const float g = (u - 0.70f) / 0.30f;
        f.beads[i].glint = (u >= 0.70f && u < 1) ? std::sin(3.1415926f * std::clamp(g, 0.f, 1.f)) : 0.f;
    }
    // The foam lives longer than the mist dissolving behind it.
    f.beadOpacity = 1 - ease(tb * 1000 / 420);
    f.mistOpacity = 1 - ease(tb * 1000 / 300);
}

float ImeBubbleModel::ease(double t)
{
    const float p = float(std::clamp(t, 0.0, 1.0));
    return 1 - (1 - p) * (1 - p) * (1 - p);
}

float ImeBubbleModel::easeIn(double t)
{
    const float p = float(std::clamp(t, 0.0, 1.0));
    return p * p * p;
}

float ImeBubbleModel::easeInOut(double t)
{
    const float p = float(std::clamp(t, 0.0, 1.0));
    return p < 0.5f ? 2 * p * p : 1 - float(std::pow(-2 * p + 2, 2)) / 2;
}

float ImeBubbleModel::WobbleAt(double ms, float amp, float freqHz)
{
    return amp * float(std::exp(-ms / 260.0)) * float(std::sin(6.2831853f * freqHz * float(ms / 1000)));
}

float ImeBubbleModel::U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng_); }

} // namespace atrium
