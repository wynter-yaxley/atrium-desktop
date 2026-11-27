#include "render/genie_mesh.h"
#include "render/genie_renderer.h"
#include "ui/window_snapshot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
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

std::shared_ptr<atrium::WindowPicture> MakeTestPicture(int w, int h)
{
    const int outW = w + 2 * atrium::kPicturePad;
    const int outH = h + 2 * atrium::kPicturePad;
    std::vector<uint8_t> bgra(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4, 200);
    auto pic = std::make_shared<atrium::WindowPicture>();
    pic->width = w;
    pic->height = h;
    pic->image = atrium::IconImage::FromBgra(outW, outH, std::move(bgra));
    pic->ok = true;
    return pic;
}

void VerifyMeshInvariants(const atrium::GenieMeshInput& in, atrium::GenieDirection dir, const char* label)
{
    const atrium::GenieMeshOutput mesh = atrium::BuildGenieMesh(in);
    if (mesh.degenerate) {
        Require(mesh.vertices.empty() && mesh.indices.empty(), "degenerate mesh must emit empty buffers");
        return;
    }

    const int cols = std::clamp(in.cols, 1, 128);
    const int rows = std::clamp(in.rows, 1, 256);
    const size_t stride = static_cast<size_t>(cols) + 1;
    Require(mesh.vertices.size() == stride * (static_cast<size_t>(rows) + 1), label);
    Require(mesh.indices.size() == static_cast<size_t>(rows) * static_cast<size_t>(cols) * 6, label);
    for (const uint16_t idx : mesh.indices)
        Require(idx < mesh.vertices.size(), label);

    const float uMin = static_cast<float>(in.padPx) / static_cast<float>(in.texWidth) - 1e-5f;
    const float uMax = static_cast<float>(in.padPx + in.picWidth) / static_cast<float>(in.texWidth) + 1e-5f;
    const float vMin = static_cast<float>(in.padPx) / static_cast<float>(in.texHeight) - 1e-5f;
    const float vMax = static_cast<float>(in.padPx + in.picHeight) / static_cast<float>(in.texHeight) + 1e-5f;

    const float surfL = static_cast<float>(in.surfaceBounds.left);
    const float surfT = static_cast<float>(in.surfaceBounds.top);
    const float surfW = static_cast<float>(in.surfaceBounds.right - in.surfaceBounds.left);
    const float surfH = static_cast<float>(in.surfaceBounds.bottom - in.surfaceBounds.top);
    const bool vertical = (dir == atrium::GenieDirection::Down || dir == atrium::GenieDirection::Up);
    const float sign = (dir == atrium::GenieDirection::Down || dir == atrium::GenieDirection::Right) ? 1.0f : -1.0f;

    auto toAxial = [&](const atrium::GenieVertex& vtx) {
        const float sx = surfL + 0.5f * (vtx.x + 1.0f) * surfW;
        const float sy = surfT + 0.5f * (1.0f - vtx.y) * surfH;
        return vertical ? sign * sy : sign * sx;
    };
    auto toCross = [&](const atrium::GenieVertex& vtx) {
        const float sx = surfL + 0.5f * (vtx.x + 1.0f) * surfW;
        const float sy = surfT + 0.5f * (1.0f - vtx.y) * surfH;
        return vertical ? sx : sy;
    };

    float prevRowA = -std::numeric_limits<float>::infinity();
    for (int r = 0; r <= rows; ++r) {
        const float rowA = toAxial(mesh.vertices[static_cast<size_t>(r) * stride]);
        if (r > 0)
            Require(rowA >= prevRowA - 1e-3f, label);
        prevRowA = rowA;

        float prevB = -std::numeric_limits<float>::infinity();
        for (int c = 0; c <= cols; ++c) {
            const atrium::GenieVertex& vtx = mesh.vertices[static_cast<size_t>(r) * stride + static_cast<size_t>(c)];
            Require(std::isfinite(vtx.x) && std::isfinite(vtx.y) && std::isfinite(vtx.u) && std::isfinite(vtx.v), label);
            Require(std::isfinite(vtx.edgeU) && std::isfinite(vtx.edgeV) && std::isfinite(vtx.clipSignedPx), label);
            Require(vtx.u >= 0.0f && vtx.u <= 1.0f && vtx.v >= 0.0f && vtx.v <= 1.0f, label);
            Require(vtx.u >= uMin && vtx.u <= uMax && vtx.v >= vMin && vtx.v <= vMax, label);
            Require(vtx.edgeU >= -1e-5f && vtx.edgeU <= 1.0f + 1e-5f, label);
            Require(vtx.edgeV >= -1e-5f && vtx.edgeV <= 1.0f + 1e-5f, label);
            const float b = toCross(vtx);
            if (c > 0)
                Require(b >= prevB - 1e-3f, label);
            prevB = b;
        }
    }
}

void TestGenieMeshInvariants()
{
    struct Case {
        atrium::GenieDirection dir;
        RECT from;
        RECT to;
        RECT surface;
    };
    const std::array<Case, 8> cases = {{
        {atrium::GenieDirection::Down, {200, 100, 1000, 700}, {560, 1010, 620, 1070}, {0, 0, 1920, 1080}},
        {atrium::GenieDirection::Up, {200, 300, 1000, 900}, {560, 10, 620, 60}, {0, 0, 1920, 1080}},
        {atrium::GenieDirection::Right, {200, 200, 900, 800}, {1850, 480, 1910, 540}, {0, 0, 1920, 1080}},
        {atrium::GenieDirection::Left, {500, 200, 1400, 800}, {10, 480, 60, 540}, {0, 0, 1920, 1080}},
        // Overlapping window/icon rect extending behind Bottom Dock
        {atrium::GenieDirection::Down, {300, 120, 900, 1060}, {560, 1010, 620, 1060}, {0, 0, 1920, 1080}},
        // Ultrawide on negative dual-monitor surface bounds
        {atrium::GenieDirection::Down, {-1800, 50, -200, 280}, {-1020, 1010, -960, 1070}, {-1920, 0, 1920, 1080}},
        // Tall narrow window into Left Dock
        {atrium::GenieDirection::Left, {20, 40, 180, 1040}, {8, 480, 56, 528}, {0, 0, 1920, 1080}},
        // Tiny valid window
        {atrium::GenieDirection::Up, {400, 400, 420, 420}, {404, 10, 424, 30}, {0, 0, 1920, 1080}},
    }};

    for (const Case& c : cases) {
        Require(atrium::InferGenieDirection(c.from, c.to) == c.dir, "InferGenieDirection mismatch");
        for (int step = 0; step <= 40; ++step) {
            atrium::GenieMeshInput in;
            in.fromRect = c.from;
            in.toRect = c.to;
            in.surfaceBounds = c.surface;
            in.picWidth = 400;
            in.picHeight = 300;
            in.texWidth = in.picWidth + 2 * atrium::kGenieDefaultPicturePadPx;
            in.texHeight = in.picHeight + 2 * atrium::kGenieDefaultPicturePadPx;
            in.t = static_cast<float>(step) / 40.0f;
            VerifyMeshInvariants(in, c.dir, "mesh invariants across 4 directions and dense t");
        }
    }

    // Verify clipSignedPx sign flips cleanly across the Dock icon line on overlapped window.
    atrium::GenieMeshInput overlap;
    overlap.fromRect = {300, 120, 900, 1070};
    overlap.toRect = {560, 1010, 620, 1060}; // icy = 1035
    overlap.surfaceBounds = {0, 0, 1920, 1080};
    overlap.picWidth = 600;
    overlap.picHeight = 950;
    overlap.texWidth = 604;
    overlap.texHeight = 954;
    overlap.t = 0.0f;
    const atrium::GenieMeshOutput overlapMesh = atrium::BuildGenieMesh(overlap);
    Require(!overlapMesh.degenerate, "overlapped mesh at t=0 must not be degenerate");
    Require(overlapMesh.vertices.front().clipSignedPx > 0.0f, "top row must be >= 0 before Dock icon line");
    Require(overlapMesh.vertices.back().clipSignedPx < 0.0f, "bottom row past Dock icon line must be < 0 for SV_ClipDistance0");
}

void TestDegenerateTailGuard()
{
    atrium::GenieMeshInput base;
    base.fromRect = {200, 100, 800, 600};
    base.toRect = {470, 1010, 530, 1070};
    base.surfaceBounds = {0, 0, 1920, 1080};
    base.picWidth = 600;
    base.picHeight = 500;
    base.texWidth = 604;
    base.texHeight = 504;

    base.t = 1.0f;
    Require(atrium::BuildGenieMesh(base).degenerate, "t=1.0 trailing tail must be degenerate");

    base.t = 0.999f;
    Require(atrium::BuildGenieMesh(base).degenerate, "sub-2px trailing tail at t=0.999 must be degenerate");

    base.t = 0.0f;
    base.fromRect = {200, 100, 201, 200};
    Require(atrium::BuildGenieMesh(base).degenerate, "<2px wide window must be degenerate");

    base.fromRect = {200, 100, 800, 600};
    base.t = std::numeric_limits<float>::quiet_NaN();
    Require(atrium::BuildGenieMesh(base).degenerate, "NaN t must be degenerate");
}

void TestTightFlightBounds()
{
    const RECT virtualScreen{-3840, 0, 3840, 2160}; // Dual 4K monitors
    const std::array<RECT, 2> flightRects = {{{100, 120, 900, 720}, {460, 1010, 520, 1070}}};
    const RECT tight = atrium::ComputeTightFlightBounds(flightRects, virtualScreen, 8);
    Require(tight.left == 92 && tight.top == 112 && tight.right == 908 && tight.bottom == 1078, "tight bounds must equal union + padPx");
    const int64_t tightArea = static_cast<int64_t>(tight.right - tight.left) * (tight.bottom - tight.top);
    const int64_t monitorArea = static_cast<int64_t>(virtualScreen.right - virtualScreen.left) * (virtualScreen.bottom - virtualScreen.top);
    Require(tightArea < monitorArea / 4, "tight flight bounds must be much smaller than dual-4K monitor union");

    // Clamping at monitor edges
    const std::array<RECT, 2> edgeRects = {{{-3838, -10, -3500, 400}, {-3700, 2140, -3640, 2170}}};
    const RECT clamped = atrium::ComputeTightFlightBounds(edgeRects, virtualScreen, 12);
    Require(clamped.left == -3840 && clamped.top == 0 && clamped.right == -3488 && clamped.bottom == 2160, "bounds must clamp cleanly to monitor union");

    // Empty / offscreen cases
    const RECT emptyBounds = atrium::ComputeTightFlightBounds({}, virtualScreen, 8);
    Require(IsRectEmpty(&emptyBounds), "empty rect span must return empty RECT");
    const std::array<RECT, 1> offscreen = {{{5000, 5000, 5200, 5200}}};
    const RECT offBounds = atrium::ComputeTightFlightBounds(offscreen, virtualScreen, 8);
    Require(IsRectEmpty(&offBounds), "completely offscreen flight must return empty RECT");
}

void TestWindowPictureCache()
{
    auto picA = MakeTestPicture(20, 20); // 24*24*4 = 2304 bytes
    auto picB = MakeTestPicture(20, 20);
    auto picC = MakeTestPicture(20, 20);
    const size_t entryBytes = picA->image.ByteSize();
    Require(entryBytes == 2304, "unexpected test picture byte size");

    const HWND wA = FakeHwnd(0x101);
    const HWND wB = FakeHwnd(0x102);
    const HWND wC = FakeHwnd(0x103);
    const FILETIME ft1{100, 1};
    const FILETIME ft2{200, 1};

    atrium::WindowPictureCache cache(entryBytes * 2);
    Require(atrium::WindowPictureCache::kDefaultMaxBytes == 64ULL * 1024ULL * 1024ULL, "default cache budget must be 64 MiB");

    cache.Put({wA, 10, ft1, 1}, {0, 0, 20, 20}, picA);
    cache.Put({wB, 20, ft1, 1}, {0, 0, 20, 20}, picB);
    Require(cache.Count() == 2 && cache.TotalBytes() == entryBytes * 2, "cache holds 2 entries within budget");

    // Touch wA so wB becomes LRU, then insert wC -> wB must be evicted.
    Require(cache.Find(wA, 10, ft1, 1) != nullptr, "wA hit expected");
    cache.Put({wC, 30, ft1, 1}, {0, 0, 20, 20}, picC);
    Require(cache.Count() == 2 && cache.TotalBytes() == entryBytes * 2, "LRU eviction must keep total bytes within budget");
    Require(cache.Find(wB, 20, ft1, 1) == nullptr, "wB (LRU) must be evicted");
    Require(cache.Find(wA, 10, ft1, 1) != nullptr && cache.Find(wC, 30, ft1, 1) != nullptr, "wA and wC must remain cached");

    // Minimum generation check does not hit stale generation.
    Require(cache.Find(wA, 10, ft1, 2) == nullptr, "older generation must not satisfy higher minGeneration");

    // Recycled HWND with different PID or creationTime invalidates & erases stale entry.
    Require(cache.Find(wA, 99, ft1, 1) == nullptr, "mismatched PID must invalidate recycled HWND");
    Require(cache.Count() == 1 && cache.TotalBytes() == entryBytes, "invalidated recycled HWND must be erased");
    Require(!cache.Touch(wC, 30, ft2), "mismatched creationTime must invalidate recycled HWND");
    Require(cache.Count() == 0 && cache.TotalBytes() == 0, "invalidated creationTime entry must be erased");

    // Single entry larger than non-zero budget stays alone (all others evicted); SetMaxBytes(0) clears all.
    auto huge = MakeTestPicture(60, 60);
    cache.Put({wA, 10, ft1, 1}, {}, picA);
    cache.Put({wB, 20, ft1, 1}, {}, huge);
    Require(cache.Count() == 1 && cache.Find(wB, 20, ft1, 1) != nullptr, "single oversized entry evicts older entries");
    cache.SetMaxBytes(0);
    Require(cache.Count() == 0 && cache.TotalBytes() == 0, "SetMaxBytes(0) evicts all entries");

    // QueryWindowSnapshotKey checks live valid vs invalid HWNDs.
    HWND msgWin = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(msgWin != nullptr, "CreateWindowExW failed");
    atrium::WindowSnapshotKey queried{};
    Require(atrium::QueryWindowSnapshotKey(msgWin, 7, queried), "QueryWindowSnapshotKey must succeed for live HWND");
    Require(queried.window == msgWin && queried.pid == GetCurrentProcessId() && queried.generation == 7, "queried key metadata mismatch");
    Require(queried.creationTime.dwLowDateTime != 0 || queried.creationTime.dwHighDateTime != 0, "queried creationTime must be populated");
    DestroyWindow(msgWin);
    Require(!atrium::QueryWindowSnapshotKey(nullptr, 1, queried), "null HWND must return false");
    Require(!atrium::QueryWindowSnapshotKey(FakeHwnd(0xDEAD), 1, queried), "invalid HWND must return false");
}

void TestGenieFlightStateMachine()
{
    const HWND w1 = FakeHwnd(0x5001);
    const HWND w2 = FakeHwnd(0x5002);

    // 1. CoverReady active minimize: holds minimize until cover present, then runs strictly once.
    {
        atrium::GenieFlightStateMachine sm;
        int minCount = 0;
        const uint64_t id = sm.BeginMinimize(w1, 100.0, [&] { ++minCount; }, true);
        Require(sm.Tick(110.0).empty() && minCount == 0, "CoverWait must hold minimize before cover frame or 50ms timeout");
        sm.NotifyCoverPresented(id, 116.0);
        auto due = sm.Tick(116.0);
        Require(due.size() == 1, "CoverPresented must release minimize callback once");
        for (const auto& fn : due)
            fn();
        Require(minCount == 1, "minimize callback must run strictly once");
        Require(sm.Tick(200.0).empty() && minCount == 1, "no duplicate minimize callback");
        Require(sm.Tick(116.0 + atrium::kGenieFlightMs).empty() && sm.Empty(), "minimize flight completes at flightMs");
    }

    // 2. CoverTimeout (50ms) fires minimize strictly once even if cover never presents.
    {
        atrium::GenieFlightStateMachine sm;
        int minCount = 0;
        sm.BeginMinimize(w1, 10.0, [&] { ++minCount; }, true);
        Require(sm.Tick(59.0).empty(), "before 50ms timeout minimize stays held");
        auto due = sm.Tick(60.0);
        Require(due.size() == 1, "50ms timeout must emit minimize callback");
        for (const auto& fn : due)
            fn();
        Require(minCount == 1, "timeout minimize runs strictly once");
    }

    // 3. Restore flow: pours out, fires restore once, waits for restoreObserved or bounded max hold, dissolves.
    {
        atrium::GenieFlightStateMachine sm;
        int resCount = 0;
        const uint64_t id = sm.BeginRestore(w1, 0.0, [&] { ++resCount; });
        Require(sm.Tick(280.0).empty() && resCount == 0, "restore callback stays pending while pouring out");
        auto due = sm.Tick(atrium::kGenieFlightMs);
        Require(due.size() == 1, "restore callback due when pour finishes");
        for (const auto& fn : due)
            fn();
        Require(resCount == 1, "restore callback runs strictly once");
        Require(!sm.ActiveFrames().empty() && sm.ActiveFrames()[0].phase == atrium::GenieFlightPhase::RestoreHolding, "enters RestoreHolding");

        sm.NotifyRestoreContentObserved(id, atrium::kGenieFlightMs + 20.0);
        Require(sm.Tick(atrium::kGenieFlightMs + 20.0).empty(), "transitions to Dissolving on content observed");
        Require(!sm.ActiveFrames().empty() && sm.ActiveFrames()[0].phase == atrium::GenieFlightPhase::Dissolving, "in Dissolving phase");
        Require(sm.Tick(atrium::kGenieFlightMs + 20.0 + atrium::kGenieDissolveMs).empty() && sm.Empty(), "completes after dissolveMs");
    }

    // 4. Bounded restore max-hold fallback when content-ready is never observed.
    {
        atrium::GenieFlightStateMachine sm;
        int resCount = 0;
        sm.BeginRestore(w1, 0.0, [&] { ++resCount; });
        for (const auto& fn : sm.Tick(atrium::kGenieFlightMs))
            fn();
        Require(resCount == 1, "restore called once");
        Require(sm.Tick(atrium::kGenieFlightMs + atrium::kGenieRestoreMaxHoldMs).empty(), "max hold transitions to Dissolving");
        Require(!sm.ActiveFrames().empty() && sm.ActiveFrames()[0].phase == atrium::GenieFlightPhase::Dissolving, "dissolving after bounded max hold");
        Require(sm.Tick(atrium::kGenieFlightMs + atrium::kGenieRestoreMaxHoldMs + atrium::kGenieDissolveMs).empty() && sm.Empty(), "completes cleanly");
    }

    // 5. CancelOnDeviceLost & DrainOnShutdown guarantee zero lost callbacks for valid windows and no double-calls.
    {
        atrium::GenieFlightStateMachine sm;
        int minCalls = 0;
        int resCalls = 0;
        int deadCalls = 0;
        sm.BeginMinimize(w1, 0.0, [&] { ++minCalls; }, true);
        sm.BeginRestore(w1, 0.0, [&] { ++resCalls; });
        sm.BeginRestore(w2, 0.0, [&] { ++deadCalls; });
        auto drained = sm.CancelOnDeviceLost([&](HWND h) { return h == w1; });
        Require(sm.Empty() && drained.size() == 2, "CancelOnDeviceLost collects valid pending minimize and restore");
        for (const auto& fn : drained)
            fn();
        Require(minCalls == 1 && resCalls == 1 && deadCalls == 0, "valid callbacks invoked once, invalid window skipped");
        Require(sm.DrainOnShutdown([&](HWND) { return true; }).empty(), "subsequent DrainOnShutdown is idempotent");
    }
}

void TestGenieSceneEvaluation()
{
    auto pic = MakeTestPicture(400, 300);
    atrium::GenieFlightDraw draw;
    draw.flightId = 42;
    draw.pictureId = 7;
    draw.image = std::shared_ptr<const atrium::IconImage>(pic, &pic->image);
    draw.picWidth = pic->width;
    draw.picHeight = pic->height;
    draw.fromRect = {100, 100, 500, 400};
    draw.toRect = {280, 1010, 330, 1060};

    // CoverWait: static cover frame at t=0, opacity=1.
    draw.phase = atrium::GenieFlightPhase::CoverWait;
    draw.phaseStartMs = 1000.0;
    {
        const auto ev = atrium::EvaluateGenieFlightAt(draw, 1020.0);
        Require(ev.meshT == 0.0f && ev.opacity == 1.0f && !ev.animating && !ev.pourFinished, "CoverWait evaluation mismatch");
    }

    // Flying: animates t=0..1 over flightMs, fades tail after 0.86.
    draw.phase = atrium::GenieFlightPhase::Flying;
    draw.phaseStartMs = 1000.0;
    {
        const auto mid = atrium::EvaluateGenieFlightAt(draw, 1000.0 + 0.5 * atrium::kGenieFlightMs);
        Require(std::abs(mid.meshT - 0.5f) < 1e-4f && mid.opacity == 1.0f && mid.animating && !mid.pourFinished, "Flying midpoint evaluation mismatch");
        const auto end = atrium::EvaluateGenieFlightAt(draw, 1000.0 + atrium::kGenieFlightMs);
        Require(end.meshT == 1.0f && end.opacity == 0.0f && !end.animating, "Flying end evaluation mismatch");
    }

    // PouringOut: animates t=1..0 and signals pourFinished when elapsed >= flightMs.
    draw.phase = atrium::GenieFlightPhase::PouringOut;
    draw.phaseStartMs = 2000.0;
    {
        const auto mid = atrium::EvaluateGenieFlightAt(draw, 2000.0 + 0.5 * atrium::kGenieFlightMs);
        Require(std::abs(mid.meshT - 0.5f) < 1e-4f && mid.animating && !mid.pourFinished, "PouringOut midpoint evaluation mismatch");
        const auto landed = atrium::EvaluateGenieFlightAt(draw, 2000.0 + atrium::kGenieFlightMs);
        Require(landed.meshT == 0.0f && landed.opacity == 1.0f && !landed.animating && landed.pourFinished, "PouringOut landing must signal pourFinished");
    }

    // RestoreHolding: static opaque hold.
    draw.phase = atrium::GenieFlightPhase::RestoreHolding;
    {
        const auto hold = atrium::EvaluateGenieFlightAt(draw, 3000.0);
        Require(hold.meshT == 0.0f && hold.opacity == 1.0f && !hold.animating && !hold.pourFinished, "RestoreHolding evaluation mismatch");
    }

    // Dissolving: fades opacity 1..0 over dissolveMs.
    draw.phase = atrium::GenieFlightPhase::Dissolving;
    draw.phaseStartMs = 3000.0;
    {
        const auto mid = atrium::EvaluateGenieFlightAt(draw, 3000.0 + 0.5 * atrium::kGenieDissolveMs);
        Require(mid.meshT == 0.0f && mid.opacity > 0.4f && mid.opacity < 0.6f && mid.animating, "Dissolving midpoint evaluation mismatch");
        const auto done = atrium::EvaluateGenieFlightAt(draw, 3000.0 + atrium::kGenieDissolveMs);
        Require(done.meshT == 0.0f && done.opacity == 0.0f && !done.animating, "Dissolving end evaluation mismatch");
    }

    // GenieSceneSnapshot builds tight overlay bounds across concurrent flights.
    atrium::GenieSceneSnapshot scene;
    scene.seq = 1;
    scene.epoch = 3;
    const std::array<RECT, 4> rects = {draw.fromRect, draw.toRect, {700, 120, 1200, 620}, {900, 1010, 950, 1060}};
    scene.overlayBounds = atrium::ComputeTightFlightBounds(rects, {0, 0, 1920, 1080}, 8);
    scene.flights.push_back(draw);
    Require(scene.overlayBounds.left == 92 && scene.overlayBounds.top == 92 && scene.overlayBounds.right == 1208 && scene.overlayBounds.bottom == 1068,
            "GenieSceneSnapshot tight overlay bounds mismatch");
}

} // namespace

int main()
{
    TestGenieMeshInvariants();
    TestDegenerateTailGuard();
    TestTightFlightBounds();
    TestWindowPictureCache();
    TestGenieFlightStateMachine();
    TestGenieSceneEvaluation();
    if (g_failures > 0)
        return 1;
    std::printf("genie mesh: all checks passed\n");
    return 0;
}
