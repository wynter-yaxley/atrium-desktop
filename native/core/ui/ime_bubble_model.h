// The IME bubble's one-shot model: the analytic timeline — emerge, float, pop,
// foam — and the merges that grow and rock the skin. Every frame is a pure
// function of the time since Begin and the merges drawn at Fuse (the burst's
// randomness is drawn once, at Begin and at each merge), so a late frame lands
// where a punctual one would, and the whole model is testable without a device.
//
// The feel is this file's constants: no setting is read, no per-application
// list is consulted, and nothing here touches a window. The surface, the paint
// and the end timer live in ime_bubble.h/.cpp.
#pragma once

#include <random>
#include <vector>

namespace atrium {

// DIP and ms. Emerge: it swells up from below with a springy overshoot. Float:
// it drifts up a little and breathes. Pop: one more swell of pressure, then the
// skin tears at its rim into foam — a handful of drops and a dust of smaller
// ones, every pop of its own sizes, directions, wobble and pace, slowing,
// drooping and dissolving.
class ImeBubbleModel {
public:
    static constexpr int kW = 320, kH = 240; // the window around it, DIP
    static constexpr float kCx = kW / 2.f, kCyRest = 172; // the resting centre
    static constexpr float kRadius = 32;     // the nominal bubble the model is laid out around (ScaleAt's base)
    static constexpr float kFloatRise = 10; // DIP further up during the float
    static constexpr unsigned kEmergeMs = 360, kFloatMs = 700, kSwellMs = 46, kBurstMs = 440;
    static constexpr unsigned kPopAt = kEmergeMs + kFloatMs; // the first possible pop
    static constexpr int kMaxFusions = 10;   // past this the size stops growing

    // The feel, as constants: how deeply the skin compresses at a hit, how far
    // the spring rebounds, the liquid wobble's base amplitude and rhythm, how
    // long one bubble lives, where the two kinds of bubble are born, how a
    // newcomer's absorb divides into its three acts, and the bubble's own size
    // (the first bubble's radius and the one ten merges reach). These are the
    // original values, so the bubble behaves as it always has.
    static constexpr float kCompression = 0.10f; // the hit squashes the skin by this fraction
    static constexpr float kRebound = 0.31f;     // the spring passes its target by this much
    static constexpr float kWobbleAmp = 0.875f;  // the liquid wobble's base amplitude
    static constexpr float kWobbleHz = 4.3f;     // the liquid wobble's base rhythm, Hz
    static constexpr float kLifeMs = 1060.f;     // how long the bubble lives before it bursts (kEmergeMs + kFloatMs)
    static constexpr float kFirstBirthDip = 18;  // the first bubble is born this far below its resting centre, DIP
    static constexpr float kLobeBirthDip = 10;   // a newcomer's centre is born this far below the window's bottom, DIP
    static constexpr float kLobeRiseMs = 380;    // the newcomer's slow rise, ms
    static constexpr float kLobeAttachMs = 243;  // its fast approach into the rim, ms
    static constexpr float kLobeSwallowMs = 137; // the swallow into the skin, ms
    // The bubble's own size, DIP of radius: the first bubble's and the one ten
    // merges reach. The merges in between are interpolated in the merge count's
    // logarithm, the same shape the size has always had. The start is kRadius,
    // so the first bubble's scale is 1.
    static constexpr float kStartRadius = 32;
    static constexpr float kEndRadius = 51;

    // The newcomer's absorb, in three acts: it floats up slowly from off the
    // screen to a hover point below the skin, rushes the last stretch into the
    // rim, then is drawn in. The three lengths are kLobeRiseMs, kLobeAttachMs
    // and kLobeSwallowMs (380 + 243 + 137 = 760 ms); where in the absorb its
    // rim meets the skin, from there it presses the skin wider and rocks it:
    // the touch is rise + attach into the whole absorb.
    static constexpr double kDipSpan = 0.20; // the hit's squash decays over this share of the absorb
    static constexpr double kWobbleMs = 900; // the skin's liquid wobble after a merge
    // How many newcomers one frame carries. A rapid run may launch more at once
    // than an eye can tell apart; a frame shows the newest ones.
    static constexpr int kMaxFrameLobes = 6;
    // The pieces of glass one shape may melt (kMaxGlassParts in
    // render/glass_renderer.h): the skin's one or two and the drawn newcomers,
    // all in one body.
    static constexpr int kMaxMeltParts = 6;
    // The resting geometry, in model DIP. A newcomer is no longer born on a
    // line inside the window: its centre is born below the window's bottom edge
    // (kLobeBirthDip past it) and floats up into the frame. A lone bubble throws
    // 9..13 drops; a fully merged one (ten merges) throws the cap, so a big
    // bubble's foam reads as plainly more than a small one's.
    static constexpr int kMinBeads = 9, kMaxBeads = 30;

    // What one frame's melt publishes: the skin as one piece or two (squashed
    // across the impact), and how many of the newest newcomers fit beside it.
    struct MeltBudget {
        int skinParts = 1;
        int lobes = 0;
    };
    static MeltBudget MeltFor(int lobeCount, bool split);
    static int DrawnLobes(int lobeCount, bool split);
    // How many melt shapes a run of drops needs. The glass renderer melts at
    // most kMaxMeltParts parts per shape and drops the rest silently, so a swarm
    // larger than that is published as several shapes, each carrying up to six
    // drops; this is the count of them, ceil(parts / kMaxMeltParts). Nought
    // drops need nought shapes. A pure function, so the count is testable
    // without a device (the paint groups the drops by it).
    static int ChunksFor(int parts);

    // The number of drops a burst throws: the base swarm, plus more from a
    // bubble that merged to a larger size — its skin holds more liquid — less
    // from a bubble set smaller than the nominal, but never under the base.
    // `roll` (0..4) is the burst's own temper.
    int BeadsFor(int fusions, int roll) const;
    // The swarm's spread: faster and wider with more drops, and every burst
    // has its own temper (0.88..1.12).
    static float SpreadFor(int beads, float temper);

    // The radius after `fusions` merges, DIP: kStartRadius at the first bubble,
    // kEndRadius at ten, and the merges between interpolated in the count's
    // logarithm (a real merge adds volume, so the growth tapers instead of
    // running away). Never past either end.
    float RadiusAt(int fusions) const;
    // The same curve as the scale the glass and the letter ride: 1 at the first
    // bubble (kStartRadius is the nominal kRadius), and kEndRadius/kRadius at ten.
    float ScaleAt(int fusions) const;

    // The newcomer's timeline, derived from the constants' three acts. The
    // touch — where the rim meets the skin — is the end of the fast approach,
    // and at most one millisecond short of the swallow's end so the skin never
    // has to read a touch past its own absorb.
    double RiseEndMs() const;
    double AttachEndMs() const;
    double AbsorbMs() const;
    double TouchMs() const;
    double TouchFrac() const;

    // 0 -> 1 with a decaying springy overshoot, all but settled by 900 ms. The
    // rebound is how far the first swing passes its target (0.31: about 31 %,
    // the old feel); the decay follows from it, so 0.31 gives the old
    // exp(-6 t) cos(16 t). Used both for the size and tested as the feel.
    static float softBounce(double ms, float rebound);
    // One newcomer's own liquid rhythm, `ms` after its touch: a decaying sine
    // at its own pace and strength. The frame's wobble is the sum of these,
    // and the feel is tested as this closed form.
    static float WobbleAt(double ms, float amp, float freqHz);

    struct Bead {
        float x, y, r;
        float glint; // 0..1: its last sparkle, just before it dissolves
    };
    // One merge's newcomer as this frame draws it: its place and size, and how
    // far its absorb has come (0: just launched, 1: swallowed).
    struct LobeFrame {
        float x = 0, y = 0, r = 0, u = 0;
    };
    struct Frame {
        float cx = kCx, cy = kCyRest, scale = 1, grow = 1, opacity = 0;
        // The merges in progress, oldest first: every newcomer still rising or
        // being swallowed (0 of them: no merge now), and the skin's liquid
        // wobble — the sum of their own rhythms.
        LobeFrame lobes[kMaxFrameLobes];
        int lobeCount = 0;
        float wob = 0;
        Bead beads[kMaxBeads]{};
        int beadCount = 0, foamCount = 0; // the mist trails after the foam beads
        float beadOpacity = 0, mistOpacity = 0;
        bool active = false, animating = false;
    };

    bool active() const { return active_; }
    int fusions() const { return fusions_; }
    // The bubble is out there and has not begun to burst: a newer state can
    // still merge into it.
    bool Mergeable(double now) const;
    double PopAt() const { return popAt_; }
    double EndMs() const { return popAt_ + kSwellMs + kBurstMs; }

    void Begin(double now);
    // A newer state arrived while the bubble lives: it merges — one step
    // larger (up to ten), its burst re-rolled for the new size, its pop pushed
    // back so the newest state stays readable. Every merge is a newcomer of its
    // own, launched at once: a rapid run shows several rise side by side and
    // fuse together, instead of waiting for the last one to be swallowed.
    // False once the burst has begun.
    bool Fuse(double now);
    void Cancel();
    Frame Step(double now);

private:
    // One merge's newcomer: when it leaves the emerge line, where, its own
    // liquid rhythm and strength, and the size step its touch adds (fusions_
    // after that merge). Launches ascend, one per merge; several may be in
    // flight at once.
    struct Lobe {
        double launch;
        float startX, freq, amp;
        int step;
    };
    // One drop's own character, drawn when the burst begins: where it leaves
    // the skin, how fast and far it flies, how it wobbles, when it dies.
    struct BeadPlan {
        float deg, speed, r0, life, wobbleAmp, wobbleHz, phase, droopAfter;
    };

    void PlanBurst();
    void Tear(float tb, float grow, Frame& f);

    static float ease(double t);
    // Accelerating: still at first, quickest at the end (the rush into the rim).
    static float easeIn(double t);
    static float easeInOut(double t);

    float U(float a, float b);

    static constexpr float kGlideS = 0.55f; // the drops slow to a stop in this long
    static constexpr float kDroop = 240;    // DIP/s²: gravity's pull
    static constexpr int kJet = 0;          // beads_[0] is the pop's spike
    double since_ = 0, popAt_ = 0;
    bool active_ = false;
    int fusions_ = 1;
    float spread_ = 1;
    std::vector<Lobe> lobes_; // one per merge, launches ascending
    std::mt19937 rng_;
    BeadPlan beads_[kMaxBeads];
    int beadCount_ = 0, foamCount_ = 0;
    float swellPeak_ = 1.18f, burstScale_ = 1.5f, jitterX_ = 0, jitterY_ = 0;
};

} // namespace atrium
