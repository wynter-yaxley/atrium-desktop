#include "bar.h"
#include "bar_entries.h"

#include "clock.h"
#include "log.h"
#include "render/capture.h"
#include "render/glass_renderer.h"
#include "render/panel_glass_host.h"

#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

namespace atrium {

namespace {

// DCompositionWaitForCompositorClock (Windows 11): waits for the next composed frame or a handle;
// WAIT_OBJECT_0 + count is the clock. Looked up at run time (absent before build 22000).
using WaitClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
const WaitClockFn kWaitForCompositorClock = reinterpret_cast<WaitClockFn>(GetProcAddress(LoadLibraryW(L"dcomp.dll"), "DCompositionWaitForCompositorClock"));

constexpr DWORD kStaleMs = 2000; // present at least this often (echo filter cap)

struct RenderMinuteStats {
    unsigned frames = 0;
    unsigned presents = 0;
    unsigned skipped = 0;
    unsigned glassDraws = 0;
    unsigned snapCopies = 0;
    unsigned flushes = 0;
    std::vector<double> workMs;
    std::vector<double> presentMs;
    std::vector<double> panelsMs;
    ULONGLONG startMs = GetTickCount64();
};

void SampleBarInks(const BarScene& s, const Settings& st, RECT w, const Readback& rb, bool& darkClock, bool& darkApps)
{
    const EdgeFrame ef{s.edge, D2D1::RectF(0, 0, float(w.right - w.left), float(w.bottom - w.top))};
    auto decide = [&](bool& dark, D2D1_RECT_F r) {
        if (r.right <= r.left)
            return;
        r = ef.ToArea(r);
        const float p = float(GlassRenderer::kPad), lum = GlassLuma(st, rb.RegionLuma(r.left + p, r.top + p, r.right + p, r.bottom + p));
        dark = lum > kDarkInkAbove ? true : lum < kLightInkBelow ? false : dark;
    };
    D2D1_RECT_F r = s.clock;
    for (const auto& it : s.extras)
        if (s.clock.right <= s.clock.left && it.key != kStartKey && it.key != kMoreKey)
            r = r.right <= r.left ? it.plate : D2D1::RectF(std::min(r.left, it.plate.left), s.bar.top, std::max(r.right, it.plate.right), s.bar.bottom);
    if (r.right <= r.left)
        r = D2D1::RectF(std::max(s.bar.left, s.bar.right - (s.bar.bottom - s.bar.top)), s.bar.top, s.bar.right, s.bar.bottom);
    decide(darkClock, r);
    decide(darkApps, s.items.empty() ? D2D1::RectF(s.bar.left, s.bar.top, s.separatorX > s.bar.left ? s.separatorX : s.bar.right, s.bar.bottom) :
                                       D2D1::RectF(s.items.front().icon.left, s.bar.top, s.items.back().icon.right, s.bar.bottom));
}

// Whether the screen on the inner side of `bar` (screen px), as GDI reads it, has any light in it: the
// capture's picture of the same place coming all black then means the capture is what is black
// (duplication after a sleep on some drivers, a video in an overlay plane), not the desktop.
bool ScreenLitAbove(BarEdge edge, RECT bar)
{
    constexpr int kW = 32, kH = 4;
    const EdgeFrame frame{edge, bar};
    const LONG d = std::max(8L, std::min(64L, LONG(frame.Across())));
    const RECT sample = frame.ToArea(RECT{0, -d, LONG(frame.Along()), 0});
    const int w = Vertical(edge) ? kH : kW, h = Vertical(edge) ? kW : kH;
    bool lit = false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    if (HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
        HGDIOBJ old = SelectObject(mem, dib);
        SetStretchBltMode(mem, HALFTONE);
        if (StretchBlt(mem, 0, 0, w, h, screen, sample.left, sample.top, sample.right - sample.left, sample.bottom - sample.top, SRCCOPY)) {
            GdiFlush();
            const auto* px = static_cast<const uint8_t*>(bits);
            for (int i = 0; i < w * h && !lit; ++i)
                lit = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2] > 24;
        }
        SelectObject(mem, old);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return lit;
}

bool FreezeBackdrop(const Gpu& gpu, winrt::com_ptr<ID3D11Texture2D>& frozen, winrt::com_ptr<ID3D11Texture2D>& frame)
{
    if (!frame)
        return true;
    if (!frozen) {
        D3D11_TEXTURE2D_DESC d{};
        frame->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = 0;
        const HRESULT hr = gpu.device->CreateTexture2D(&d, nullptr, frozen.put());
        if (FAILED(hr)) {
            Log(L"render: freeze backdrop failed (0x%08X); retrying", static_cast<unsigned>(hr));
            return false;
        }
        gpu.context->CopyResource(frozen.get(), frame.get());
    }
    frame = frozen;
    return true;
}

bool IsDeviceRemovedHr(HRESULT hr) { return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG; }

bool DeviceLost(const Gpu& gpu, const wchar_t* where)
{
    if (!gpu.device)
        return true;
    const HRESULT reason = gpu.device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
        return false;
    Log(L"render: device lost (%s, 0x%08X); retrying", where, static_cast<unsigned>(reason));
    return true;
}

void LogRenderMinuteStats(RenderMinuteStats& stats, unsigned keepFrameCopies)
{
    auto pct = [](std::vector<double>& v, double q) {
        std::ranges::sort(v);
        return v.empty() ? 0.0 : v[std::min(v.size() - 1, size_t(q * double(v.size())))];
    };
    const double w50 = pct(stats.workMs, 0.5), w95 = pct(stats.workMs, 0.95), p95 = pct(stats.presentMs, 0.95), pan95 = pct(stats.panelsMs, 0.95);
    PROCESS_MEMORY_COUNTERS_EX mem{sizeof(mem)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
    Log(L"render: last minute %u frames, %u presents, %u echoes skipped; work p50 %.2f ms, p95 %.2f ms, max %.2f ms; "
        L"presenting p95 %.2f ms; popups and widgets p95 %.2f ms, max %.2f ms; private %zu MB; glass %u, snap %u, keepFrame %u, flushes %u",
        stats.frames, stats.presents, stats.skipped, w50, w95, stats.workMs.empty() ? 0.0 : stats.workMs.back(), p95, pan95,
        stats.panelsMs.empty() ? 0.0 : stats.panelsMs.back(), size_t(mem.PrivateUsage >> 20), stats.glassDraws, stats.snapCopies, keepFrameCopies,
        stats.flushes);
    stats.frames = stats.presents = stats.skipped = stats.glassDraws = stats.snapCopies = stats.flushes = 0;
    stats.workMs.clear();
    stats.presentMs.clear();
    stats.panelsMs.clear();
    stats.startMs = GetTickCount64();
}

GlassParams BarFrameParams(const Settings& settings, float scale, const BarFrame& bf, BarEdge edge, RECT window)
{
    GlassParams params = ParamsFor(settings, scale, float(Bar::kInsetDip));
    params.radius = std::min(params.radius, std::min(bf.shape.right - bf.shape.left, bf.shape.bottom - bf.shape.top) / 2);
    params.shapeL = bf.shape.left;
    params.shapeT = bf.shape.top;
    params.shapeR = bf.shape.right;
    params.shapeB = bf.shape.bottom;
    params.rippleX = bf.rippleX;
    params.rippleY = bf.rippleY;
    params.rippleR = bf.rippleR;
    params.rippleA = bf.rippleA;
    params.waveX = bf.waveX;
    params.waveWidth = bf.waveWidth;
    params.waveHeight = bf.waveHeight;
    params.fromArea = EdgeFrame{edge, D2D1::RectF(0, 0, float(window.right - window.left), float(window.bottom - window.top))}.FromAreaMatrix();
    return params;
}

std::wstring RemoteBackdropMode(const Settings& s) { return s.widgetBackdrop == L"wallpaper" ? L"wallpaper" : L"desktop"; }

DWORD WaitNextRenderTick(bool paced, bool readback, bool paused, bool pollCapture, bool inkDue, bool showClock, bool showClockSeconds, bool& pendingSnap,
                         const HANDLE* handles, uint64_t& tick)
{
    const DWORD hc = 3 - DWORD(paused);
    if (pendingSnap) {
        pendingSnap = false;
        return WAIT_OBJECT_0 + 1;
    }
    if (paced && kWaitForCompositorClock) {
        DWORD w = kWaitForCompositorClock(hc, handles, 100);
        if (w == WAIT_OBJECT_0 + hc || w == WAIT_TIMEOUT)
            ++tick;
        else if (w > WAIT_OBJECT_0 + hc) {
            w = WaitForMultipleObjects(hc, handles, FALSE, 6); // no clock (the display is off)
            ++tick;
        }
        return w;
    }
    // Idle, a clock with seconds wakes just past the next whole second: a plain second's
    // timeout drifts by the work of each pass, and every so often a second is skipped.
    DWORD idle = paused ? INFINITE : 1000;
    if (!paused && showClock && showClockSeconds) {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        idle = DWORD(1000 - t.wMilliseconds) + 16;
    }
    if (!paused && pollCapture)
        idle = std::min<DWORD>(idle, 16);
    if (inkDue)
        idle = std::min<DWORD>(idle, 250);
    const DWORD w = WaitForMultipleObjects(hc, handles, FALSE, paced ? 6 : readback ? 3 : idle);
    ++tick;
    return w;
}

} // namespace

struct BarRender {
    explicit BarRender(Bar& owner) : bar(owner) { QueryPerformanceFrequency(&freq); }

    static Bar::SessionEnd ToSessionEnd(PanelGlassHost::StepResult result)
    {
        return result == PanelGlassHost::StepResult::Exit ? Bar::SessionEnd::Exit : Bar::SessionEnd::Retry;
    }

    double ElapsedMs(LARGE_INTEGER t0) const
    {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
    }

    void CopyLatestSnapshot()
    {
        if (bar.publishSeq_.load(std::memory_order_acquire) == seenPublishSeq)
            return;
        std::lock_guard g(bar.lock_);
        snap = bar.snapshot_;
        seenPublishSeq = bar.publishSeq_.load(std::memory_order_relaxed);
        ++stats.snapCopies;
    }

    void CopyLatestGenieScene()
    {
        const uint64_t seq = bar.genieSeq_.load(std::memory_order_acquire);
        if (seq == seenGenieSeq)
            return;
        std::lock_guard g(bar.genieLock_);
        genieSnap = bar.genieScene_;
        seenGenieSeq = genieSnap.seq;
    }

    bool RenderGenieStep(GenieRenderer& genieRenderer, double nowMs, bool barVisible, Bar::SessionEnd& stop)
    {
        CopyLatestGenieScene();
        const std::span<const RECT> barCutout = barVisible ? std::span<const RECT>(&snap.window, 1) : std::span<const RECT>{};
        const GenieRenderer::RenderResult gr = genieRenderer.Render(
            genieSnap, nowMs,
            [&](uint64_t flightId, double /*nowMs*/, bool pourFinished) {
                if (genieSnap.overlayHwnd) {
                    const LPARAM lParam = static_cast<LPARAM>((genieSnap.epoch << 1) | (pourFinished ? 1ULL : 0ULL));
                    PostMessageW(genieSnap.overlayHwnd, WM_GENIE_FRAME_PRESENTED, static_cast<WPARAM>(flightId), lParam);
                }
            },
            barCutout);
        genieActive = gr.active;
        if (gr.status == GenieRenderer::RenderStatus::DeviceLost) {
            stop = Bar::SessionEnd::Retry;
            return false;
        }
        return true;
    }

    Bar::SessionEnd WaitForInitialSnapshot()
    {
        HANDLE first[] = {bar.quit_, bar.wake_};
        for (;;) {
            CopyLatestSnapshot();
            if (snap.hmonitor)
                return Bar::SessionEnd::Switch;
            if (WaitForMultipleObjects(2, first, FALSE, INFINITE) == WAIT_OBJECT_0)
                return Bar::SessionEnd::Exit;
        }
    }

    std::unique_ptr<BackdropSource> SelectInitialCapture(const Gpu& gpu, Bar::SessionEnd& stop)
    {
        const auto monitorId = reinterpret_cast<uintptr_t>(captureMonitor);
        bar.capturePolicy_.BeginSession(monitorId, captureGeneration);
        const CapturePolicy::InitialSource initial = bar.capturePolicy_.ChooseInitialSource(remote, GetTickCount64());
        blackOut = !remote && initial == CapturePolicy::InitialSource::Wallpaper;
        std::unique_ptr<BackdropSource> capture;
        if (initial == CapturePolicy::InitialSource::Wallpaper) {
            capture = std::make_unique<WallpaperCapture>(gpu, snap.hmonitor, bar.frame_, remote ? remoteBackdropMode : std::wstring(L"wallpaper"));
            Log(remote ? L"render: remote-control mode, the glass shows %s" : L"render: the glass shows the wallpaper for now", remoteBackdropMode.c_str());
        } else if (initial == CapturePolicy::InitialSource::Duplication) {
            HRESULT why = S_OK;
            bool elsewhere = false;
            for (int tries = 0;; ++tries) {
                capture = DuplicationCapture::Create(gpu, snap.hmonitor, why, elsewhere);
                if (capture || !DuplicationCapture::Temporary(why))
                    break;
                if (DeviceLost(gpu, L"select capture")) {
                    stop = Bar::SessionEnd::Retry;
                    return nullptr;
                }
                if (tries == 0)
                    Log(L"render: desktop duplication unavailable for now (0x%08X), trying again every second", static_cast<unsigned>(why));
                if (WaitForSingleObject(bar.quit_, 1000) != WAIT_TIMEOUT) {
                    stop = Bar::SessionEnd::Exit;
                    return nullptr;
                }
            }
            if (!capture && (IsDeviceRemovedHr(why) || DeviceLost(gpu, L"duplication init"))) {
                stop = Bar::SessionEnd::Retry;
                return nullptr;
            }
            if (!capture && elsewhere)
                PreferPowerSavingGpu();
            if (!capture) {
                const auto reason = elsewhere ? CapturePolicy::DuplicationFailure::Elsewhere : CapturePolicy::DuplicationFailure::Transient;
                bar.capturePolicy_.OnDuplicationUnavailable(GetTickCount64(), reason, monitorId, captureGeneration);
                Log(L"render: desktop duplication unavailable (0x%08X); Windows.Graphics.Capture, Atrium kept out of screenshots", static_cast<unsigned>(why));
            }
        }
        if (!capture) {
            if (!bar.ShowOwnWindowsToCapture(false)) { // before the first frame, or the glass would sample itself
                stop = Bar::SessionEnd::Exit;
                return nullptr;
            }
            capture = std::make_unique<MonitorCapture>(gpu, snap.hmonitor, bar.frame_);
        } else if (!bar.ShowOwnWindowsToCapture(true)) {
            stop = Bar::SessionEnd::Exit;
            return nullptr;
        }
        return capture;
    }

    bool ProbeCaptureRecovery(const Gpu& gpu, bool needScreen, bool captureKeepsOwnOut, ULONGLONG tickMs, Bar::SessionEnd& stop)
    {
        const auto monitorId = reinterpret_cast<uintptr_t>(captureMonitor);
        if (bar.capturePolicy_.ShouldProbeDuplication(needScreen, remote, captureKeepsOwnOut, monitorId, captureGeneration, tickMs)) {
            HRESULT why = S_OK;
            bool elsewhere = false;
            if (DuplicationCapture::Create(gpu, snap.hmonitor, why, elsewhere)) {
                bar.capturePolicy_.OnDuplicationRecovered();
                Log(L"render: desktop duplication available again");
                stop = Bar::SessionEnd::Switch;
                return true;
            }
            if (IsDeviceRemovedHr(why) || DeviceLost(gpu, L"duplication probe")) {
                stop = Bar::SessionEnd::Retry;
                return true;
            }
            if (elsewhere) {
                bar.capturePolicy_.OnDuplicationUnavailable(tickMs, CapturePolicy::DuplicationFailure::Elsewhere, monitorId, captureGeneration);
            }
        }
        if (bar.capturePolicy_.ShouldExitBlackoutWallpaper(blackOut, tickMs)) {
            Log(L"render: back to capturing the screen");
            stop = Bar::SessionEnd::Switch;
            return true;
        }
        return false;
    }

    bool CheckBlackBackdrop(const GlassRenderer& renderer)
    {
        if (!bar.capturePolicy_.PollBlackBackdrop(!remote && !blackOut && renderer.HasBackdrop(), backdropBrightest, GetTickCount64()) ||
            !ScreenLitAbove(snap.scene.edge, snap.window)) {
            return false;
        }
        switch (bar.capturePolicy_.OnBlackOverLitScreen(captureGeneration, GetTickCount64())) {
        case CapturePolicy::BlackAction::RestartCapture: Log(L"render: the capture is black over a lit screen; starting it again"); break;
        case CapturePolicy::BlackAction::SwitchToMonitorCapture:
            // Desktop Duplication keeps coming back black: try Windows.Graphics.Capture
            // before the wallpaper, so a capture that is black for any reason cannot
            // hide every window behind the wallpaper for minutes.
            Log(L"render: the capture stays black over a lit screen; Windows.Graphics.Capture for now (retrying duplication with backoff)");
            break;
        case CapturePolicy::BlackAction::FallbackToWallpaper:
            Log(L"render: the capture stays black over a lit screen; the wallpaper for thirty seconds");
            break;
        }
        return true;
    }

    bool RenderHiddenSessionStep(const Gpu& gpu, GlassRenderer& renderer, BackdropSource& capture, PanelGlassHost& panelHost, GenieRenderer& genieRenderer,
                                 ID3D11Texture2D* frame, bool needScreen, LARGE_INTEGER t0, Bar::SessionEnd& stop)
    {
        bar.capturePolicy_.ResetBlackTimer();
        // Retire the bar's reads even while hidden. Otherwise its last pending
        // slot keeps the idle loop waking every 3 ms until the bar is revealed.
        renderer.Poll();
        drawnGeneration = 0; // its ink and glass are measured again when revealed
        missedFrame |= frame != nullptr;
        winrt::com_ptr<ID3D11Texture2D> hidden;
        hidden.copy_from(frame);
        if (freeze && !FreezeBackdrop(gpu, frozen, hidden)) {
            if (needScreen)
                capture.Release();
            stop = Bar::SessionEnd::Retry;
            return false;
        }
        const bool copiedBackup = !freeze && panelHost.KeepFrame(frame, panelHost.HasScreenPanelOnCaptureMonitor(snap.panels));
        const double p0 = ElapsedMs(t0);
        const PanelGlassHost::RenderResult pr =
            panelHost.Render(snap.panels, snap.settings, snap.monitor, snap.scale, hidden.get(), tick, capture.KeepsOwnWindowsOut());
        panelsMoving = pr.moving;
        if (!snap.panels.empty())
            stats.panelsMs.push_back(ElapsedMs(t0) - p0);
        hidden = nullptr;
        // Submit the copies from the duplicated frame before handing it back.
        if (frame != nullptr || copiedBackup || pr.gpuSubmitted) {
            gpu.context->Flush();
            ++stats.flushes;
        }
        if (needScreen)
            capture.Release();
        if (pr.status != PanelGlassHost::StepResult::Ok) {
            stop = ToSessionEnd(pr.status);
            return false;
        }
        barHadGenie = false;
        if (!RenderGenieStep(genieRenderer, ClockMs(), false, stop))
            return false;
        if (DeviceLost(gpu, L"hidden")) {
            stop = Bar::SessionEnd::Retry;
            return false;
        }
        animating = barOwed = pendingBackdropDirty = owedBody = owedMusic = wasGlass = wasBody = wasMusic = false;
        wasHidden = true;
        readyStalledSinceMs = 0;
        return true;
    }

    winrt::com_ptr<ID3D11Texture2D> AcquireBarBackdrop(const Gpu& gpu, const GlassRenderer& renderer, BackdropSource& capture, PanelGlassHost& panelHost,
                                                       ID3D11Texture2D* frame, bool& deviceLost)
    {
        // Backdrop source: the newest capture frame, or in --debug-capturable
        // mode a private copy of the first one (later frames contain the glass).
        winrt::com_ptr<ID3D11Texture2D> source;
        if (frame) {
            source.copy_from(frame);
            ++stats.frames;
            if (freeze && !FreezeBackdrop(gpu, frozen, source)) {
                deviceLost = true;
                return nullptr;
            }
        } else if (frozen && snap.generation != drawnGeneration) {
            source = frozen; // size-dependent textures were recreated
        } else if (missedFrame || !renderer.HasBackdrop() || !EqualRect(&backdropAt, &snap.window)) {
            // No new frame, yet the glass needs one: its backdrop is blank (resized, a new
            // session), stale (it moved) or a frame came while it was hidden. The newest picture
            // held: the source's own (it sends only on change), else the frame kept for popups.
            if (ID3D11Texture2D* again = capture.Again())
                source.copy_from(again);
            else if (!freeze && panelHost.BackupFrame())
                source.copy_from(panelHost.BackupFrame());
        }
        return source;
    }

    bool DrawBarFrame(GlassRenderer& renderer, BarPainter& painter, const PanelGlassHost& panelHost, GenieRenderer& genieRenderer, double frameNowMs,
                      bool forceGlass, bool hadBackdrop, LARGE_INTEGER t0)
    {
        bool changed = !renderer.HasPresented();
        if (auto rb = renderer.Poll()) {
            changed |= rb->changed;
            if (!rb->luma.empty())
                backdropBrightest = *std::max_element(rb->luma.begin(), rb->luma.end());
            SampleBarInks(snap.scene, snap.settings, snap.window, *rb, darkClock, darkApps);
            dotBackdrop = std::move(*rb);
        }
        auto ink = [&](bool autoDark) { return snap.settings.textColor == L"dark" || (snap.settings.textColor == L"auto" && autoDark); };
        const bool dark = ink(darkClock), darkIcons = ink(darkApps);

        std::wstring time, date;
        if (snap.settings.showClock)
            Bar::ClockStrings(time, date, snap.settings.showClockSeconds);
        const std::wstring textKey = time + L"|" + date;
        const double nowMs = frameNowMs;
        BarAnimation anim = painter.Animate(snap.scene, nowMs);
        clockInk.Target(dark ? 1.f : 0.f, nowMs);
        appsInk.Target(darkIcons ? 1.f : 0.f, nowMs);
        anim.body |= painter.UpdateDotInk(snap.scene, dotBackdrop, snap.settings, nowMs);
        anim.body |= clockInk.Moving(nowMs) || appsInk.Moving(nowMs);
        animating = anim.Any();
        const Ink inks{clockInk.Value(nowMs), appsInk.Value(nowMs)};
        const ULONGLONG now = GetTickCount64();
        const bool hasGenieFlights = !genieSnap.flights.empty();
        const bool genieBarDirty = hasGenieFlights || barHadGenie;
        const bool backdropDirty = forceGlass || changed || pendingBackdropDirty;
        const bool needPresent = backdropDirty || snap.generation != drawnGeneration || textKey != drawnText || dark != drawnDark ||
                                 darkIcons != drawnDarkIcons || anim.Any() || wasGlass || wasBody || wasMusic || barOwed || owedBody || owedMusic ||
                                 genieBarDirty || now - lastPresent > kStaleMs;
        if (!needPresent) {
            readyStalledSinceMs = 0;
            ++stats.skipped; // echo of our own present, or nothing new under the glass
            if (hadBackdrop)
                stats.workMs.push_back(ElapsedMs(t0));
            return true;
        }
        const bool panelsOwed = panelHost.AnyOwed();
        if (anim.MusicOnly() && !panelsMoving && !panelsOwed && !backdropDirty && !genieBarDirty && snap.generation == drawnGeneration &&
            textKey == drawnText && dark == drawnDark && darkIcons == drawnDarkIcons && !wasBody && !wasGlass && !barOwed &&
            ShouldThrottleMusicOnlyFrame(nowMs - lastPresentMs)) {
            owedMusic |= anim.music;
            barOwed = false;
            if (hadBackdrop)
                stats.workMs.push_back(ElapsedMs(t0));
            return true;
        }
        pendingBackdropDirty = backdropDirty;
        const bool ready = renderer.Ready();
        if (!ready) {
            if (IsPresentReadyStalled(false, nowMs, readyStalledSinceMs, 1000.0)) {
                Log(L"render: swap chain latency waitable stalled; rebuilding");
                return false;
            }
            barOwed = true; // the last frame is not on screen yet: drawn when it is
            owedBody |= anim.body;
            owedMusic |= anim.music;
            return true;
        }
        if (!renderer.HasBackdrop() && ClockMs() - sessionStartMs < 1500) {
            barOwed = true; // no backdrop yet: drawn when it is
            owedBody |= anim.body;
            owedMusic |= anim.music;
            return true;
        }
        barOwed = false;
        const GlassParams params = BarFrameParams(snap.settings, snap.scale, painter.Frame(), snap.scene.edge, snap.window);
        const bool forceBody = owedBody && !anim.body;
        BarAnimation drawAnim = anim;
        drawAnim.music = anim.music || wasMusic || owedMusic;
        const std::wstring contentKey = std::to_wstring(snap.generation) + textKey + (dark ? L"d" : L"l") + (darkIcons ? L"d" : L"l");
        bool genieMidLost = false;
        const auto midLayer = hasGenieFlights ? std::function<bool(ID3D11RenderTargetView*, int, int)>([&](ID3D11RenderTargetView* rtv, int, int) {
            return genieRenderer.DrawIntoTarget(genieSnap, frameNowMs, rtv, snap.window, genieMidLost);
        }) :
                                                std::function<bool(ID3D11RenderTargetView*, int, int)>{};
        const bool ok = renderer.Present(
            params, [&](ID2D1DeviceContext2* d2d) { painter.Draw(d2d, snap.scene, inks, time, date, contentKey, drawAnim, forceBody); }, backdropDirty,
            midLayer);
        if (!ok) {
            Log(genieMidLost ? L"render: genie mid-layer device lost" : L"render: device lost");
            return false;
        }
        if (renderer.LastPresentBusy()) {
            if (IsPresentReadyStalled(false, nowMs, readyStalledSinceMs, 1000.0)) {
                Log(L"render: swap chain Present stayed busy; rebuilding");
                return false;
            }
            barOwed = true;
            owedBody |= anim.body;
            owedMusic |= anim.music;
            return true;
        }
        readyStalledSinceMs = 0;
        barHadGenie = hasGenieFlights;
        if (renderer.LastPresentDrewGlass())
            ++stats.glassDraws;
        ++stats.presents;
        stats.workMs.push_back(ElapsedMs(t0));
        stats.presentMs.push_back(stats.workMs.back());
        drawnGeneration = snap.generation;
        drawnText = textKey;
        drawnDark = dark;
        drawnDarkIcons = darkIcons;
        lastPresent = now;
        lastPresentMs = nowMs;
        wasGlass = anim.glass;
        wasBody = anim.body;
        wasMusic = anim.music;
        owedBody = false;
        owedMusic = false;
        pendingBackdropDirty = false;
        return true;
    }

    bool CaptureSourceChanged() const
    {
        return snap.hmonitor != captureMonitor || snap.displayGeneration != captureGeneration || RemoteVisible() != remote ||
               (remote && RemoteBackdropMode(snap.settings) != remoteBackdropMode);
    }

    bool StepFrame(const Gpu& gpu, BackdropSource& capture, GlassRenderer& renderer, BarPainter& painter, PanelGlassHost& panelHost,
                   GenieRenderer& genieRenderer, const HANDLE* handles, bool& pendingSnap, Bar::SessionEnd& stop)
    {
        const bool owed = barOwed || owedBody || owedMusic || panelHost.AnyOwed();
        const bool paced = animating || panelsMoving || owed || genieActive;
        const bool readback = renderer.ReadbackPending() || panelHost.AnyReadbackPending();
        const bool pollCapture = (!paused && capture.RequiresPolling()) || panelHost.AnyCaptureRequiresPolling();
        if (WaitNextRenderTick(paced, readback, anyScreenPaused, pollCapture, panelHost.InkDue(), snap.settings.showClock, snap.settings.showClockSeconds,
                               pendingSnap, handles, tick) == WAIT_OBJECT_0) {
            stop = Bar::SessionEnd::Exit;
            return false;
        }
        if (GetTickCount64() - stats.startMs >= 60000)
            LogRenderMinuteStats(stats, panelHost.TakeBackupCopyCount());
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        CopyLatestSnapshot();
        if (CaptureSourceChanged()) {
            stop = Bar::SessionEnd::Switch; // selected display or its capture source changed
            return false;
        }
        const bool needPrimaryScreen = snap.visible || panelHost.HasScreenPanelOnCaptureMonitor(snap.panels);
        const bool needAnyScreen = needPrimaryScreen || panelHost.HasAnyVisibleScreenPanel(snap.panels);
        anyScreenPaused = !needAnyScreen;
        if (paused != !needPrimaryScreen) {
            paused = !needPrimaryScreen;
            capture.SetPaused(paused);
            missedFrame |= paused;
            if (paused && !capture.KeepsOwnWindowsOut())
                panelHost.ReleaseBackupFrame();
        }
        ID3D11Texture2D* frame = needPrimaryScreen ? capture.Acquire() : nullptr;
        if (capture.Lost() || DeviceLost(gpu, L"acquire")) {
            if (frame && needPrimaryScreen)
                capture.Release();
            stop = Bar::SessionEnd::Retry; // a new source (display mode change, secure desktop, WDDM reset)
            return false;
        }
        if (ProbeCaptureRecovery(gpu, needPrimaryScreen, capture.KeepsOwnWindowsOut(), GetTickCount64(), stop))
            return false;
        if (!snap.visible)
            return RenderHiddenSessionStep(gpu, renderer, capture, panelHost, genieRenderer, frame, needPrimaryScreen, t0, stop);
        if (wasHidden) {
            wasHidden = false;
            readyStalledSinceMs = 0;
            renderer.Commit();
        }
        renderer.Resize(snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);
        bool freezeLost = false;
        winrt::com_ptr<ID3D11Texture2D> source = AcquireBarBackdrop(gpu, renderer, capture, panelHost, frame, freezeLost);
        if (freezeLost) {
            if (needPrimaryScreen)
                capture.Release();
            stop = Bar::SessionEnd::Retry;
            return false;
        }
        const bool hadBackdrop = source != nullptr;
        const bool forceGlass = !renderer.HasPresented() || drawnGeneration == 0 || missedFrame || !EqualRect(&backdropAt, &snap.window);
        if (source) {
            renderer.UpdateBackdrop(source.get(), snap.window.left - snap.monitor.left, snap.window.top - snap.monitor.top);
            backdropAt = snap.window;
            missedFrame = false;
        }
        const bool copiedBackup = !freeze && panelHost.KeepFrame(frame, panelHost.HasScreenPanelOnCaptureMonitor(snap.panels));
        const double p0 = ElapsedMs(t0);
        const PanelGlassHost::RenderResult pr =
            panelHost.Render(snap.panels, snap.settings, snap.monitor, snap.scale, source.get(), tick, capture.KeepsOwnWindowsOut());
        panelsMoving = pr.moving;
        if (!snap.panels.empty())
            stats.panelsMs.push_back(ElapsedMs(t0) - p0);
        source = nullptr;
        if (frame != nullptr || hadBackdrop || copiedBackup || pr.gpuSubmitted) {
            gpu.context->Flush(); // the panels' copies and comparisons start now, in one submission
            ++stats.flushes;
        }
        if (needPrimaryScreen)
            capture.Release(); // after the Flush (see above)
        if (pr.status != PanelGlassHost::StepResult::Ok) {
            if (pr.status == PanelGlassHost::StepResult::Retry)
                Log(L"render: device lost (popup glass)");
            stop = ToSessionEnd(pr.status);
            return false;
        }
        CopyLatestGenieScene();
        const double frameNowMs = ClockMs();
        if (CheckBlackBackdrop(renderer) || !DrawBarFrame(renderer, painter, panelHost, genieRenderer, frameNowMs, forceGlass, hadBackdrop, t0)) {
            stop = Bar::SessionEnd::Retry;
            return false;
        }
        if (!RenderGenieStep(genieRenderer, frameNowMs, true, stop))
            return false;
        return true;
    }

    Bar::SessionEnd Run()
    {
        if (WaitForInitialSnapshot() == Bar::SessionEnd::Exit)
            return Bar::SessionEnd::Exit;

        Gpu gpu = Gpu::ForMonitor(snap.hmonitor);
        captureMonitor = snap.hmonitor;
        captureGeneration = snap.displayGeneration;
        remote = RemoteVisible();
        remoteBackdropMode = RemoteBackdropMode(snap.settings);
        Bar::SessionEnd stop = Bar::SessionEnd::Switch;
        std::unique_ptr<BackdropSource> capture = SelectInitialCapture(gpu, stop);
        if (!capture)
            return stop;

        freeze = bar.debugCapturable_ && !capture->KeepsOwnWindowsOut();
        GlassRenderer renderer(gpu, bar.visual_, snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);
        BarPainter painter(renderer.D2D(), renderer.DWrite());
        PanelGlassHost panelHost(
            gpu, captureMonitor, bar.wake_, bar.frame_, remote, bar.debugCapturable_, capture->KeepsOwnWindowsOut(),
            [this](const Gpu& g, HWND hwnd, int width, int height, bool desktop) { return bar.TryCreatePanelGlass(g, hwnd, width, height, desktop); },
            [this](bool visible) { return bar.ShowOwnWindowsToCapture(visible); });
        GenieRenderer genieRenderer(gpu);
        Log(L"render: started on %s, window %ldx%ld", gpu.adapterName.c_str(), snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);

        HANDLE handles[] = {bar.quit_, bar.wake_, bar.frame_};
        bool pendingSnap = !snap.visible;
        while (StepFrame(gpu, *capture, renderer, painter, panelHost, genieRenderer, handles, pendingSnap, stop)) {}
        return stop;
    }

    Bar& bar;
    LARGE_INTEGER freq{};
    Bar::Snapshot snap;
    uint64_t seenPublishSeq = 0;
    HMONITOR captureMonitor = nullptr;
    unsigned captureGeneration = 0;
    bool remote = false;
    std::wstring remoteBackdropMode = L"desktop";
    bool blackOut = false;
    bool freeze = false;

    unsigned drawnGeneration = 0;
    float backdropBrightest = 1; // the brightest cell of the last backdrop read back
    std::wstring drawnText;
    bool darkClock = false, darkApps = false, drawnDark = false, drawnDarkIcons = false, animating = false;
    InkFade clockInk, appsInk; // the inks fade to a new colour, not switch
    Readback dotBackdrop;      // newest completed luminance grid, reused while the desktop is still
    ULONGLONG lastPresent = 0;
    double lastPresentMs = 0;
    const double sessionStartMs = ClockMs();
    RECT backdropAt{};
    bool missedFrame = false;
    bool panelsMoving = false, barOwed = false, paused = false, anyScreenPaused = false, wasHidden = false;
    bool genieActive = false;
    bool barHadGenie = false;
    GenieSceneSnapshot genieSnap;
    uint64_t seenGenieSeq = 0;
    bool wasGlass = false, wasBody = false, wasMusic = false;
    bool owedBody = false, owedMusic = false, pendingBackdropDirty = false;
    double readyStalledSinceMs = 0;
    uint64_t tick = 1; // compositor frames (every wake-up when not paced by the clock)
    winrt::com_ptr<ID3D11Texture2D> frozen; // --debug-capturable only
    RenderMinuteStats stats;
};

Bar::SessionEnd Bar::RenderSession()
{
    BarRender session(*this);
    return session.Run();
}

} // namespace atrium
