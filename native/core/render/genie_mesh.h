// Pure Genie continuous mesh geometry, tight flight bounds, and flight state machine.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace atrium {

inline constexpr int kGenieGridCols = 24;
inline constexpr int kGenieGridRows = 48;
inline constexpr float kGenieMinExtentPx = 2.0f;
inline constexpr int kGenieDefaultPicturePadPx = 2;

inline constexpr double kGenieFlightMs = 560.0;
inline constexpr double kGenieCoverTimeoutMs = 50.0;
inline constexpr double kGenieRestoreMaxHoldMs = 180.0;
inline constexpr double kGenieDissolveMs = 160.0;

// Vertex layout matching shaders/genie.hlsl VSIn (32 bytes, 16-byte aligned).
struct GenieVertex {
    float x = 0.0f;            // NDC (-1..1, screen-top = +1)
    float y = 0.0f;
    float u = 0.0f;            // Texture UV (0..1)
    float v = 0.0f;
    float edgeU = 0.0f;        // Content-local 0..1 coord for fwidth() edge AA
    float edgeV = 0.0f;
    float clipSignedPx = 0.0f; // >= 0 on window side of Dock icon line, < 0 past it (SV_ClipDistance0)
    float pad = 0.0f;
};
static_assert(sizeof(GenieVertex) == 32);

enum class GenieDirection { Down, Up, Right, Left };

struct GenieMeshInput {
    RECT fromRect{};
    RECT toRect{};
    RECT surfaceBounds{};
    int texWidth = 0;
    int texHeight = 0;
    int picWidth = 0;
    int picHeight = 0;
    int padPx = kGenieDefaultPicturePadPx;
    float t = 0.0f;
    int cols = kGenieGridCols;
    int rows = kGenieGridRows;
    GenieDirection direction = GenieDirection::Down;
    bool inferDirection = true;
};

struct GenieMeshOutput {
    std::vector<GenieVertex> vertices;
    std::vector<uint16_t> indices;
    float opacity = 0.0f;
    bool degenerate = false;
};

inline float GenieSmoothStep(float a, float b, float x)
{
    if (b <= a)
        return x >= b ? 1.0f : 0.0f;
    const float u = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

inline float GenieLerp(float a, float b, float t) { return a + (b - a) * t; }

inline GenieDirection InferGenieDirection(const RECT& fromRect, const RECT& toRect)
{
    const float ww = std::max(1.0f, static_cast<float>(fromRect.right - fromRect.left));
    const float wh = std::max(1.0f, static_cast<float>(fromRect.bottom - fromRect.top));
    const float wcx = 0.5f * static_cast<float>(fromRect.left + fromRect.right);
    const float wcy = 0.5f * static_cast<float>(fromRect.top + fromRect.bottom);
    const float icx = 0.5f * static_cast<float>(toRect.left + toRect.right);
    const float icy = 0.5f * static_cast<float>(toRect.top + toRect.bottom);
    const bool vertical = std::abs(icy - wcy) * ww >= std::abs(icx - wcx) * wh;
    if (vertical)
        return icy >= wcy ? GenieDirection::Down : GenieDirection::Up;
    return icx >= wcx ? GenieDirection::Right : GenieDirection::Left;
}

namespace detail {

struct GenieRowSlices {
    std::vector<float> a;
    std::vector<float> lo;
    std::vector<float> hi;
    bool vertical = true;
    float sign = 1.0f;
    float aEnd = 0.0f;
    bool valid = false;
};

inline GenieRowSlices ComputeGenieRows(const GenieMeshInput& in, float t, int rows)
{
    GenieRowSlices s;
    const GenieDirection dir = in.inferDirection ? InferGenieDirection(in.fromRect, in.toRect) : in.direction;
    s.vertical = (dir == GenieDirection::Down || dir == GenieDirection::Up);
    s.sign = (dir == GenieDirection::Down || dir == GenieDirection::Right) ? 1.0f : -1.0f;

    const float wLeft = static_cast<float>(in.fromRect.left);
    const float wTop = static_cast<float>(in.fromRect.top);
    const float wRight = static_cast<float>(in.fromRect.right);
    const float wBottom = static_cast<float>(in.fromRect.bottom);
    const float iLeft = static_cast<float>(in.toRect.left);
    const float iTop = static_cast<float>(in.toRect.top);
    const float iRight = static_cast<float>(in.toRect.right);
    const float iBottom = static_cast<float>(in.toRect.bottom);
    const float icx = 0.5f * (iLeft + iRight);
    const float icy = 0.5f * (iTop + iBottom);

    const float a0 = s.vertical ? (s.sign > 0.0f ? wTop : -wBottom) : (s.sign > 0.0f ? wLeft : -wRight);
    const float a1 = s.vertical ? (s.sign > 0.0f ? wBottom : -wTop) : (s.sign > 0.0f ? wRight : -wLeft);
    const float b0 = s.vertical ? wLeft : wTop;
    const float b1 = s.vertical ? wRight : wBottom;
    s.aEnd = s.vertical ? s.sign * icy : s.sign * icx;
    const float iconHalf = 0.35f * std::max(0.0f, s.vertical ? (iRight - iLeft) : (iBottom - iTop));
    const float iconMid = s.vertical ? icx : icy;

    const float bend = GenieSmoothStep(0.0f, 0.42f, t);
    const float slide = GenieSmoothStep(0.12f, 1.0f, t);
    const size_t n = static_cast<size_t>(rows) + 1;
    s.a.resize(n);
    constexpr float kLead = 0.9f;
    for (int r = 0; r <= rows; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rows);
        const float u = std::clamp(slide * (1.0f + kLead) - (1.0f - v) * kLead, 0.0f, 1.0f);
        float a = GenieLerp(a0 + v * (a1 - a0), s.aEnd, u * u * (3.0f - 2.0f * u));
        // Overlapped window/icon rects can invert the lerp target; clamp monotonic so strips never fold.
        if (r > 0)
            a = std::max(s.a[static_cast<size_t>(r - 1)], a);
        s.a[static_cast<size_t>(r)] = a;
    }
    if (s.a.back() - s.a.front() < kGenieMinExtentPx)
        return s;

    s.lo.resize(n);
    s.hi.resize(n);
    const float aDen = std::max(1.0f, s.aEnd - a0);
    float maxCrossWidth = 0.0f;
    for (int r = 0; r <= rows; ++r) {
        const float uSide = std::clamp((s.a[static_cast<size_t>(r)] - a0) / aDen, 0.0f, 1.0f);
        const float k = bend * GenieSmoothStep(0.0f, 1.0f, uSide);
        float lo = GenieLerp(b0, iconMid - iconHalf, k);
        float hi = GenieLerp(b1, iconMid + iconHalf, k);
        if (hi < lo) {
            const float mid = 0.5f * (lo + hi);
            lo = mid;
            hi = mid;
        }
        s.lo[static_cast<size_t>(r)] = lo;
        s.hi[static_cast<size_t>(r)] = hi;
        maxCrossWidth = std::max(maxCrossWidth, hi - lo);
    }
    s.valid = maxCrossWidth >= kGenieMinExtentPx;
    return s;
}

inline void PopulateGenieGrid(const GenieMeshInput& in, const GenieRowSlices& s, int cols, int rows, GenieMeshOutput& out)
{
    const float uMin = std::clamp(static_cast<float>(in.padPx) / static_cast<float>(in.texWidth), 0.0f, 1.0f);
    const float uMax = std::clamp(static_cast<float>(in.padPx + in.picWidth) / static_cast<float>(in.texWidth), uMin, 1.0f);
    const float vMin = std::clamp(static_cast<float>(in.padPx) / static_cast<float>(in.texHeight), 0.0f, 1.0f);
    const float vMax = std::clamp(static_cast<float>(in.padPx + in.picHeight) / static_cast<float>(in.texHeight), vMin, 1.0f);

    const float surfLeft = static_cast<float>(in.surfaceBounds.left);
    const float surfTop = static_cast<float>(in.surfaceBounds.top);
    const float invSurfW = 1.0f / static_cast<float>(in.surfaceBounds.right - in.surfaceBounds.left);
    const float invSurfH = 1.0f / static_cast<float>(in.surfaceBounds.bottom - in.surfaceBounds.top);

    const size_t stride = static_cast<size_t>(cols) + 1;
    out.vertices.reserve((static_cast<size_t>(rows) + 1) * stride);
    for (int r = 0; r <= rows; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rows);
        const float a = s.a[static_cast<size_t>(r)];
        const float lo = s.lo[static_cast<size_t>(r)];
        const float hi = s.hi[static_cast<size_t>(r)];
        const float clipSignedPx = s.aEnd - a;
        for (int c = 0; c <= cols; ++c) {
            const float w = static_cast<float>(c) / static_cast<float>(cols);
            const float b = GenieLerp(lo, hi, w);
            const float sx = s.vertical ? b : s.sign * a;
            const float sy = s.vertical ? s.sign * a : b;
            const float edgeU = s.vertical ? w : (s.sign > 0.0f ? v : 1.0f - v);
            const float edgeV = s.vertical ? (s.sign > 0.0f ? v : 1.0f - v) : w;
            GenieVertex vtx;
            vtx.x = 2.0f * (sx - surfLeft) * invSurfW - 1.0f;
            vtx.y = 1.0f - 2.0f * (sy - surfTop) * invSurfH;
            vtx.u = GenieLerp(uMin, uMax, edgeU);
            vtx.v = GenieLerp(vMin, vMax, edgeV);
            vtx.edgeU = edgeU;
            vtx.edgeV = edgeV;
            vtx.clipSignedPx = clipSignedPx;
            out.vertices.push_back(vtx);
        }
    }

    out.indices.reserve(static_cast<size_t>(rows) * static_cast<size_t>(cols) * 6);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const auto i00 = static_cast<uint16_t>(static_cast<size_t>(r) * stride + static_cast<size_t>(c));
            const auto i10 = static_cast<uint16_t>(i00 + 1);
            const auto i01 = static_cast<uint16_t>(i00 + stride);
            const auto i11 = static_cast<uint16_t>(i01 + 1);
            out.indices.push_back(i00);
            out.indices.push_back(i10);
            out.indices.push_back(i11);
            out.indices.push_back(i00);
            out.indices.push_back(i11);
            out.indices.push_back(i01);
        }
    }
}

} // namespace detail

inline GenieMeshOutput BuildGenieMesh(const GenieMeshInput& in)
{
    GenieMeshOutput out;
    const int64_t surfW = static_cast<int64_t>(in.surfaceBounds.right) - in.surfaceBounds.left;
    const int64_t surfH = static_cast<int64_t>(in.surfaceBounds.bottom) - in.surfaceBounds.top;
    if (!std::isfinite(in.t) || in.texWidth <= 0 || in.texHeight <= 0 || in.picWidth <= 0 || in.picHeight <= 0 || in.padPx < 0 || surfW <= 0 || surfH <= 0 ||
        in.fromRect.right <= in.fromRect.left || in.fromRect.bottom <= in.fromRect.top) {
        out.degenerate = true;
        return out;
    }

    const float t = std::clamp(in.t, 0.0f, 1.0f);
    const int cols = std::clamp(in.cols, 1, 128);
    const int rows = std::clamp(in.rows, 1, 256);
    out.opacity = 1.0f - GenieSmoothStep(0.86f, 1.0f, t);

    const detail::GenieRowSlices slices = detail::ComputeGenieRows(in, t, rows);
    if (!slices.valid) {
        out.degenerate = true;
        return out;
    }
    detail::PopulateGenieGrid(in, slices, cols, rows, out);
    return out;
}

inline RECT ComputeTightFlightBounds(std::span<const RECT> rects, const RECT& clampBounds, int padPx = 8)
{
    if (rects.empty() || clampBounds.right <= clampBounds.left || clampBounds.bottom <= clampBounds.top)
        return {};
    bool hasAny = false;
    int64_t left = 0;
    int64_t top = 0;
    int64_t right = 0;
    int64_t bottom = 0;
    for (const RECT& r : rects) {
        if (r.right < r.left || r.bottom < r.top || (r.right == r.left && r.bottom == r.top))
            continue;
        if (!hasAny) {
            left = r.left;
            top = r.top;
            right = r.right;
            bottom = r.bottom;
            hasAny = true;
        } else {
            left = std::min<int64_t>(left, r.left);
            top = std::min<int64_t>(top, r.top);
            right = std::max<int64_t>(right, r.right);
            bottom = std::max<int64_t>(bottom, r.bottom);
        }
    }
    if (!hasAny)
        return {};
    const int64_t pad = std::max(0, padPx);
    left = std::max<int64_t>(left - pad, clampBounds.left);
    top = std::max<int64_t>(top - pad, clampBounds.top);
    right = std::min<int64_t>(right + pad, clampBounds.right);
    bottom = std::min<int64_t>(bottom + pad, clampBounds.bottom);
    if (left >= right || top >= bottom)
        return {};
    return RECT{static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right), static_cast<LONG>(bottom)};
}

enum class GenieFlightPhase { CoverWait, Flying, PouringOut, RestoreHolding, Dissolving, Completed };

struct GenieFlightConfig {
    double flightMs = kGenieFlightMs;
    double coverTimeoutMs = kGenieCoverTimeoutMs;
    double restoreMaxHoldMs = kGenieRestoreMaxHoldMs;
    double dissolveMs = kGenieDissolveMs;
};

struct GenieFlightFrame {
    uint64_t id = 0;
    HWND window = nullptr;
    GenieFlightPhase phase = GenieFlightPhase::Completed;
    bool isRestore = false;
    float meshT = 0.0f;
    float opacity = 0.0f;
    bool restoreObserved = false;
    double restoreObservedAtMs = 0.0;
};

class GenieFlightStateMachine {
public:
    explicit GenieFlightStateMachine(GenieFlightConfig config = {}) : config_(config) {}

    uint64_t BeginMinimize(HWND window, double nowMs, std::function<void()> onMinimize, bool waitForCover = true)
    {
        FlightEntry e;
        e.id = ++nextId_;
        e.window = window;
        e.isRestore = false;
        e.minimizeCb = std::move(onMinimize);
        if (waitForCover && e.minimizeCb) {
            e.phase = GenieFlightPhase::CoverWait;
            e.coverStartMs = nowMs;
            e.meshT = 0.0f;
            e.opacity = 1.0f;
        } else {
            e.phase = GenieFlightPhase::Flying;
            e.flightStartMs = nowMs;
            e.meshT = 0.0f;
            e.opacity = 1.0f;
            if (e.minimizeCb)
                std::exchange(e.minimizeCb, nullptr)();
        }
        entries_.push_back(std::move(e));
        return entries_.back().id;
    }

    uint64_t BeginRestore(HWND window, double nowMs, std::function<void()> onRestore)
    {
        FlightEntry e;
        e.id = ++nextId_;
        e.window = window;
        e.isRestore = true;
        e.phase = GenieFlightPhase::PouringOut;
        e.flightStartMs = nowMs;
        e.meshT = 1.0f;
        e.opacity = 0.0f;
        e.restoreCb = std::move(onRestore);
        entries_.push_back(std::move(e));
        return entries_.back().id;
    }

    void NotifyCoverPresented(uint64_t id, double nowMs)
    {
        for (FlightEntry& e : entries_)
            if (e.id == id && e.phase == GenieFlightPhase::CoverWait) {
                e.coverPresented = true;
                e.coverPresentedAtMs = nowMs;
            }
    }

    void NotifyAllCoversPresented(double nowMs)
    {
        for (FlightEntry& e : entries_)
            if (e.phase == GenieFlightPhase::CoverWait) {
                e.coverPresented = true;
                e.coverPresentedAtMs = nowMs;
            }
    }

    void NotifyRestoreContentObserved(uint64_t id, double nowMs)
    {
        for (FlightEntry& e : entries_)
            if (e.id == id && e.phase == GenieFlightPhase::RestoreHolding && !e.restoreObserved) {
                e.restoreObserved = true;
                e.restoreObservedAtMs = nowMs;
            }
    }

    void NotifyRestoreContentObservedForWindow(HWND window, double nowMs)
    {
        for (FlightEntry& e : entries_)
            if (e.window == window && e.phase == GenieFlightPhase::RestoreHolding && !e.restoreObserved) {
                e.restoreObserved = true;
                e.restoreObservedAtMs = nowMs;
            }
    }

    std::vector<std::function<void()>> Tick(double nowMs)
    {
        std::vector<std::function<void()>> due;
        for (FlightEntry& e : entries_)
            AdvanceFlightEntry(e, nowMs, due);
        std::erase_if(entries_, [](const FlightEntry& e) { return e.phase == GenieFlightPhase::Completed; });
        return due;
    }

    template<class IsValidFn> std::vector<std::function<void()>> CancelOnDeviceLost(IsValidFn&& isValid)
    {
        return DrainInternal(std::forward<IsValidFn>(isValid));
    }

    std::vector<std::function<void()>> CancelOnDeviceLost()
    {
        return DrainInternal([](HWND) { return true; });
    }

    template<class IsValidFn> std::vector<std::function<void()>> DrainOnShutdown(IsValidFn&& isValid)
    {
        return DrainInternal(std::forward<IsValidFn>(isValid));
    }

    std::vector<GenieFlightFrame> ActiveFrames() const
    {
        std::vector<GenieFlightFrame> frames;
        frames.reserve(entries_.size());
        for (const FlightEntry& e : entries_) {
            frames.push_back(GenieFlightFrame{
                .id = e.id,
                .window = e.window,
                .phase = e.phase,
                .isRestore = e.isRestore,
                .meshT = e.meshT,
                .opacity = e.opacity,
                .restoreObserved = e.restoreObserved,
                .restoreObservedAtMs = e.restoreObservedAtMs,
            });
        }
        return frames;
    }

    bool Empty() const { return entries_.empty(); }
    size_t Count() const { return entries_.size(); }

private:
    struct FlightEntry {
        uint64_t id = 0;
        HWND window = nullptr;
        GenieFlightPhase phase = GenieFlightPhase::Completed;
        bool isRestore = false;
        double coverStartMs = 0.0;
        bool coverPresented = false;
        double coverPresentedAtMs = 0.0;
        double flightStartMs = 0.0;
        double restoreHoldStartMs = 0.0;
        bool restoreObserved = false;
        double restoreObservedAtMs = 0.0;
        double dissolveStartMs = 0.0;
        float meshT = 0.0f;
        float opacity = 0.0f;
        std::function<void()> minimizeCb;
        std::function<void()> restoreCb;
    };

    void AdvanceMinimizeEntry(FlightEntry& e, double nowMs, std::vector<std::function<void()>>& due) const
    {
        const double coverTimeoutMs = std::max(0.0, config_.coverTimeoutMs);
        if (e.phase == GenieFlightPhase::CoverWait) {
            if (!e.coverPresented && (nowMs - e.coverStartMs) < coverTimeoutMs) {
                e.meshT = 0.0f;
                e.opacity = 1.0f;
                return;
            }
            e.flightStartMs = e.coverPresented ? e.coverPresentedAtMs : (e.coverStartMs + coverTimeoutMs);
            e.phase = GenieFlightPhase::Flying;
            if (e.minimizeCb)
                due.push_back(std::exchange(e.minimizeCb, nullptr));
        }
        if (e.phase == GenieFlightPhase::Flying) {
            const double flightMs = std::max(1.0, config_.flightMs);
            const double elapsed = std::max(0.0, nowMs - e.flightStartMs);
            const float t = static_cast<float>(std::clamp(elapsed / flightMs, 0.0, 1.0));
            e.meshT = t;
            e.opacity = 1.0f - GenieSmoothStep(0.86f, 1.0f, t);
            if (elapsed >= flightMs)
                e.phase = GenieFlightPhase::Completed;
        }
    }

    void AdvanceRestoreEntry(FlightEntry& e, double nowMs, std::vector<std::function<void()>>& due) const
    {
        const double flightMs = std::max(1.0, config_.flightMs);
        if (e.phase == GenieFlightPhase::PouringOut) {
            const double elapsed = std::max(0.0, nowMs - e.flightStartMs);
            const float t = 1.0f - static_cast<float>(std::clamp(elapsed / flightMs, 0.0, 1.0));
            e.meshT = t;
            e.opacity = 1.0f - GenieSmoothStep(0.86f, 1.0f, t);
            if (elapsed < flightMs)
                return;
            e.phase = GenieFlightPhase::RestoreHolding;
            e.restoreHoldStartMs = e.flightStartMs + flightMs;
            if (e.restoreCb)
                due.push_back(std::exchange(e.restoreCb, nullptr));
        }
        if (e.phase == GenieFlightPhase::RestoreHolding) {
            const double restoreMaxHoldMs = std::max(0.0, config_.restoreMaxHoldMs);
            e.meshT = 0.0f;
            e.opacity = 1.0f;
            if (!e.restoreObserved && (nowMs - e.restoreHoldStartMs) < restoreMaxHoldMs)
                return;
            e.dissolveStartMs = e.restoreObserved ? std::max(e.restoreHoldStartMs, e.restoreObservedAtMs) : (e.restoreHoldStartMs + restoreMaxHoldMs);
            e.phase = GenieFlightPhase::Dissolving;
        }
        if (e.phase == GenieFlightPhase::Dissolving) {
            const double dissolveMs = std::max(1.0, config_.dissolveMs);
            const double elapsed = std::max(0.0, nowMs - e.dissolveStartMs);
            e.meshT = 0.0f;
            e.opacity = 1.0f - GenieSmoothStep(0.0f, static_cast<float>(dissolveMs), static_cast<float>(elapsed));
            if (elapsed >= dissolveMs)
                e.phase = GenieFlightPhase::Completed;
        }
    }

    void AdvanceFlightEntry(FlightEntry& e, double nowMs, std::vector<std::function<void()>>& due) const
    {
        if (e.isRestore)
            AdvanceRestoreEntry(e, nowMs, due);
        else
            AdvanceMinimizeEntry(e, nowMs, due);
    }

    template<class IsValidFn> std::vector<std::function<void()>> DrainInternal(IsValidFn&& isValid)
    {
        std::vector<std::function<void()>> pending;
        for (FlightEntry& e : entries_) {
            const bool valid = e.window && isValid(e.window);
            if (std::function<void()> minCb = std::exchange(e.minimizeCb, nullptr); minCb && valid)
                pending.push_back(std::move(minCb));
            if (std::function<void()> resCb = std::exchange(e.restoreCb, nullptr); resCb && valid)
                pending.push_back(std::move(resCb));
        }
        entries_.clear();
        return pending;
    }

    GenieFlightConfig config_;
    std::vector<FlightEntry> entries_;
    uint64_t nextId_ = 0;
};

} // namespace atrium
