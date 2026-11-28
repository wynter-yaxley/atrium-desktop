#include "panel_glass_host.h"

#include "../clock.h"
#include "../log.h"
#include "panel_capture_bounds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <utility>

namespace atrium {

namespace {

// A widget's ink follows a desktop that keeps changing (a slideshow's fade, about a second) once it has
// settled: cell by cell all along, the widgets were painted at every frame of it, and the render thread
// (the bar's too) was held up to 50 ms.
constexpr double kDesktopSettleMs = 250;

RECT PanelCaptureRect(const GlassPanel& p)
{
    const int w = std::max(int(p.window.right - p.window.left), p.bufferW);
    const int h = std::max(int(p.window.bottom - p.window.top), p.bufferH);
    return RECT{p.window.left, p.window.top, p.window.left + w, p.window.top + h};
}

bool CopySecondaryRegion(const Gpu& gpu, ID3D11Texture2D* src, ID3D11Texture2D* dst, RECT monitorBounds, RECT target)
{
    D3D11_TEXTURE2D_DESC desc{}, dstDesc{};
    src->GetDesc(&desc);
    dst->GetDesc(&dstDesc);
    const UINT x = UINT(std::max(0L, target.left - monitorBounds.left));
    const UINT y = UINT(std::max(0L, target.top - monitorBounds.top));
    const UINT wantW = UINT(std::max(0L, target.right - target.left));
    const UINT wantH = UINT(std::max(0L, target.bottom - target.top));
    const UINT maxW = std::min(desc.Width, dstDesc.Width);
    const UINT maxH = std::min(desc.Height, dstDesc.Height);
    const UINT copyW = x < maxW ? std::min(wantW, maxW - x) : 0u;
    const UINT copyH = y < maxH ? std::min(wantH, maxH - y) : 0u;
    if (copyW == 0 || copyH == 0)
        return false;
    const D3D11_BOX box{x, y, 0, x + copyW, y + copyH, 1};
    gpu.context->CopySubresourceRegion(dst, 0, x, y, 0, src, 0, &box);
    return true;
}

void UpdatePanelInkGrid(const Readback& rb, const Settings& settings, float panelScale, int w, int h, InkGrid& current, bool& dark, bool& inkOwed)
{
    const float padX = float(rb.origin.x), padY = float(rb.origin.y);
    const float cell = std::max(8.f, 20.f * panelScale);
    InkGrid grid;
    grid.w = std::max(1, int(std::ceil(float(w) / cell)));
    grid.h = std::max(1, int(std::ceil(float(h) / cell)));
    grid.cellX = float(w) / float(grid.w);
    grid.cellY = float(h) / float(grid.h);
    grid.dark.resize(size_t(grid.w) * size_t(grid.h));
    const std::wstring& tc = settings.textColor;
    const bool sameSize = current.w == grid.w && current.h == grid.h;
    int darkCells = 0;
    for (int y = 0; y < grid.h; ++y) {
        for (int x = 0; x < grid.w; ++x) {
            const size_t i = size_t(y) * size_t(grid.w) + size_t(x);
            const bool was = sameSize ? current.dark[i] != 0 : dark;
            bool d = was;
            if (tc == L"dark" || tc == L"light") {
                d = tc == L"dark";
            } else {
                const float x0 = float(x) * grid.cellX, y0 = float(y) * grid.cellY;
                const float lum = GlassLuma(settings, rb.RegionLuma(x0 + padX, y0 + padY, x0 + grid.cellX + padX, y0 + grid.cellY + padY));
                if (!was && lum > kDarkInkAbove)
                    d = true;
                else if (was && lum < kLightInkBelow)
                    d = false;
            }
            grid.dark[i] = d ? 1 : 0;
            darkCells += d ? 1 : 0;
        }
    }
    dark = 2 * darkCells >= int(grid.dark.size());
    if (!(grid == current)) {
        current = grid;
        inkOwed = true;
    }
}

} // namespace

GlassParams ParamsFor(const Settings& s, float scale, float insetDip)
{
    GlassParams p{};
    p.radius = float(s.cornerRadius * scale);
    p.edge = float(s.edge * scale);
    p.refract = float(s.refraction * scale);
    p.dispersion = float(s.dispersion);
    // Blur strength is mostly the mip level; mixing a sharp and a blurred sample
    // only at the low end avoids double images at medium settings.
    p.blurMix = float(std::min(1.0, s.blur * 4.0));
    p.blurLod = float(1.0 + 4.5 * s.blur);
    p.tintMix = float(1.0 - s.transparency);
    p.tintR = float((s.tint >> 16) & 0xFF) / 255.f;
    p.tintG = float((s.tint >> 8) & 0xFF) / 255.f;
    p.tintB = float(s.tint & 0xFF) / 255.f;
    p.tintA = 1;
    p.highlight = float(s.highlight);
    p.shadow = float(s.shadow);
    p.lightX = -0.6f;
    p.lightY = -0.8f;
    p.inset = insetDip * scale;
    return p;
}

// Luminance of what the glass shows (backdrop mixed with the tint), the
// bar's rule for its ink.
float GlassLuma(const Settings& s, float backdrop)
{
    const float tm = float(1.0 - s.transparency);
    const float tintLuma = (0.2126f * float((s.tint >> 16) & 0xFF) + 0.7152f * float((s.tint >> 8) & 0xFF) + 0.0722f * float(s.tint & 0xFF)) / 255.f;
    return backdrop * (1 - tm) + tintLuma * tm;
}

// The popups' glass: exactly the bar's material (D26), one set of parameters
// per piece of glass in the window.
void PanelParams(const Settings& s, float scale, const GlassPanel& p, std::vector<GlassParams>& out)
{
    out.clear();
    GlassParams g = ParamsFor(s, scale);
    g.inset = p.inset;
    if (p.shapes.empty()) {
        g.shapeL = g.shapeT = p.inset;
        g.shapeR = float(p.window.right - p.window.left) - p.inset;
        g.shapeB = float(p.window.bottom - p.window.top) - p.inset;
        g.radius = p.radius;
        out.push_back(g);
        return;
    }
    for (const GlassShape& sh : p.shapes) {
        g.shapeL = sh.left;
        g.shapeT = sh.top;
        g.shapeR = sh.right;
        g.shapeB = sh.bottom;
        g.radius = std::min(sh.radius, std::min(sh.right - sh.left, sh.bottom - sh.top) / 2);
        g.fade = sh.fade;
        g.rotate = sh.rotate;
        g.partCount = float(std::min<size_t>(sh.parts.size(), kMaxGlassParts));
        g.melt = sh.melt;
        g.partMix = sh.partMix;
        g.tailX = sh.tailX;
        g.tailHalfWidth = sh.tailHalfWidth;
        g.tailHeight = sh.tailHeight;
        g.tailInset = sh.tailInset;
        for (size_t i = 0; i < sh.parts.size() && i < kMaxGlassParts; ++i) {
            const GlassShape::Part& pt = sh.parts[i];
            g.parts[i][0] = pt.left;
            g.parts[i][1] = pt.top;
            g.parts[i][2] = pt.right;
            g.parts[i][3] = pt.bottom;
            for (int k = 0; k < 4; ++k)
                g.partRadii[i][k] = pt.radii[k];
        }
        out.push_back(g);
    }
}

PanelGlassHost::PanelGlassHost(Gpu gpu, HMONITOR captureMonitor, HANDLE wakeEvent, HANDLE frameEvent, bool remote, bool debugCapturable,
                               bool initialMayShowInCapture, CreateRendererFn createRenderer, ShowOwnWindowsFn showOwnWindows)
    : gpu_(std::move(gpu)),
      captureMonitor_(captureMonitor),
      frameEvent_(frameEvent),
      remote_(remote),
      debugCapturable_(debugCapturable),
      mayShowInCapture_(initialMayShowInCapture),
      createRenderer_(std::move(createRenderer)),
      showOwnWindows_(std::move(showOwnWindows)),
      desktop_(gpu_, wakeEvent)
{
}

bool PanelGlassHost::HasScreenPanelOnCaptureMonitor(const std::vector<GlassPanel>& panels) const
{
    return std::ranges::any_of(
        panels, [this](const GlassPanel& p) { return p.visible && !p.desktop && MonitorFromRect(&p.window, MONITOR_DEFAULTTONEAREST) == captureMonitor_; });
}

bool PanelGlassHost::HasAnyVisibleScreenPanel(const std::vector<GlassPanel>& panels) const
{
    return std::ranges::any_of(panels, [](const GlassPanel& p) { return p.visible && !p.desktop; });
}

bool PanelGlassHost::KeepFrame(ID3D11Texture2D* frame, bool hasScreenPanel)
{
    if (!frame)
        return false;
    const double now = ClockMs();
    const double minInterval = BackupKeepIntervalMs(hasScreenPanel, now - panelMovedMs_);
    if (now - keptMs_ < minInterval)
        return false;
    keptMs_ = now;
    D3D11_TEXTURE2D_DESC d{};
    frame->GetDesc(&d);
    D3D11_TEXTURE2D_DESC have{};
    if (lastFrame_)
        lastFrame_->GetDesc(&have);
    if (!lastFrame_ || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
        lastFrame_ = nullptr;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        if (FAILED(gpu_.device->CreateTexture2D(&d, nullptr, lastFrame_.put())))
            return false;
    }
    gpu_.context->CopyResource(lastFrame_.get(), frame);
    ++backupCopyCount_;
    return true;
}

unsigned PanelGlassHost::TakeBackupCopyCount() { return std::exchange(backupCopyCount_, 0); }

bool PanelGlassHost::AnyOwed() const
{
    return std::ranges::any_of(panels_, [](const auto& e) { return e.second.owed; });
}

bool PanelGlassHost::AnyCaptureRequiresPolling() const
{
    return std::ranges::any_of(panelCaptures_, [](const auto& entry) { return entry.second.source->RequiresPolling(); });
}

bool PanelGlassHost::AnyReadbackPending() const
{
    return std::ranges::any_of(panels_, [](const auto& e) { return e.second.renderer && e.second.renderer->ReadbackPending(); });
}

PanelGlassHost::StepResult PanelGlassHost::SyncPanelCaptures(const std::vector<GlassPanel>& panels, bool barKeepsOwnWindowsOut,
                                                             const std::wstring& widgetBackdrop)
{
    // A menu or Stage Manager can remain on a different display from the Dock.
    std::erase_if(panelCaptures_, [&](const auto& entry) {
        return std::none_of(panels.begin(), panels.end(),
                            [&](const GlassPanel& p) { return !p.desktop && MonitorFromRect(&p.window, MONITOR_DEFAULTTONEAREST) == entry.first; });
    });
    const std::wstring remoteMode = widgetBackdrop == L"wallpaper" ? L"wallpaper" : L"desktop";
    for (const GlassPanel& p : panels) {
        HMONITOR monitor = MonitorFromRect(&p.window, MONITOR_DEFAULTTONEAREST);
        if (p.desktop || monitor == captureMonitor_ || panelCaptures_.contains(monitor))
            continue;
        MONITORINFO info{sizeof(info)};
        if (!monitor || GetMonitorInfoW(monitor, &info) == FALSE)
            continue;
        PanelCapture& extra = panelCaptures_[monitor];
        extra.bounds = info.rcMonitor;
        HRESULT why = S_OK;
        bool elsewhere = false;
        if (remote_)
            extra.source = std::make_unique<WallpaperCapture>(gpu_, monitor, frameEvent_, remoteMode);
        else
            extra.source = DuplicationCapture::Create(gpu_, monitor, why, elsewhere);
        if (!extra.source) {
            if (!showOwnWindows_(false))
                return StepResult::Exit;
            mayShowInCapture_ = false;
            extra.source = std::make_unique<MonitorCapture>(gpu_, monitor, frameEvent_);
        }
    }
    const bool canShow =
        barKeepsOwnWindowsOut && std::ranges::all_of(panelCaptures_, [](const auto& entry) { return entry.second.source->KeepsOwnWindowsOut(); });
    if (canShow != mayShowInCapture_) {
        if (!showOwnWindows_(canShow))
            return StepResult::Exit;
        mayShowInCapture_ = canShow;
    }
    return AcquirePanelCaptures(panels);
}

PanelGlassHost::StepResult PanelGlassHost::AcquirePanelCaptures(const std::vector<GlassPanel>& panels)
{
    for (auto& [monitor, extra] : panelCaptures_) {
        std::vector<RECT> rects;
        for (const GlassPanel& p : panels) {
            if (!p.desktop && p.visible && MonitorFromRect(&p.window, MONITOR_DEFAULTTONEAREST) == monitor)
                rects.push_back(PanelCaptureRect(p));
        }
        const RECT desired = ComputeSecondaryCopyBounds(rects, extra.bounds, GlassRenderer::kPad, 128);
        const RECT target = IsRectEmpty(&desired) ? extra.bounds : desired;
        ID3D11Texture2D* frame = extra.source->Acquire();
        if (extra.source->Lost())
            return StepResult::Retry;
        extra.fresh = frame && !(debugCapturable_ && extra.texture);
        if (extra.fresh) {
            D3D11_TEXTURE2D_DESC desc{}, previous{};
            frame->GetDesc(&desc);
            if (extra.texture)
                extra.texture->GetDesc(&previous);
            const bool needAlloc = !extra.texture || previous.Width != desc.Width || previous.Height != desc.Height || previous.Format != desc.Format;
            if (needAlloc) {
                extra.texture = nullptr;
                D3D11_TEXTURE2D_DESC texDesc = desc;
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                texDesc.MiscFlags = texDesc.CPUAccessFlags = 0;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                winrt::check_hresult(gpu_.device->CreateTexture2D(&texDesc, nullptr, extra.texture.put()));
                // Populate the full monitor once so panels opening or moving later during desktop idle never smear edges.
                gpu_.context->CopyResource(extra.texture.get(), frame);
                extra.copyBounds = extra.bounds;
                extra.partialCopy = false;
            } else if (!extra.source->CanReplay()) {
                // Live screen sources (DXGI/WGC) cannot replay off-region pixels once idle, so keep the full monitor fresh.
                gpu_.context->CopyResource(extra.texture.get(), frame);
                extra.copyBounds = extra.bounds;
                extra.partialCopy = false;
            } else if (CopySecondaryRegion(gpu_, frame, extra.texture.get(), extra.bounds, target)) {
                extra.copyBounds = target;
                extra.partialCopy = !EqualRect(&target, &extra.bounds);
            }
            gpu_.context->Flush(); // submit secondary copy before releasing the DXGI/WGC frame
        } else if (!frame && extra.texture && extra.partialCopy && !CopyBoundsCoverPanels(extra.copyBounds, rects, extra.bounds, GlassRenderer::kPad)) {
            if (ID3D11Texture2D* cached = extra.source->Again()) {
                if (CopySecondaryRegion(gpu_, cached, extra.texture.get(), extra.bounds, target)) {
                    gpu_.context->Flush();
                    extra.copyBounds = target;
                    extra.partialCopy = !EqualRect(&target, &extra.bounds);
                    extra.fresh = true;
                }
            }
        }
        extra.source->Release();
    }
    return StepResult::Ok;
}

void PanelGlassHost::FlushIdleBursts(double now)
{
    for (auto& [hwnd, ps] : panels_) {
        if (ps.burstFrames && now - ps.burstLast > 250) {
            if (ps.burstFrames >= 4) {
                wchar_t title[64] = {};
                InternalGetWindowText(hwnd, title, 64); // no message to the UI thread (it may be waiting for this glass)
                const double span = ps.burstLast - ps.burstStart;
                Log(L"render: %s glass animated %d presents in %.0f ms (%.0f fps), longest gap %.1f ms (moved %d, desktop %d, under %d, params %d)", title,
                    ps.burstFrames, span, span > 0 ? 1000.0 * (ps.burstFrames - 1) / span : 0.0, ps.burstGap, ps.burstMoved, ps.burstDesktop, ps.burstReadback,
                    ps.burstParams);
            }
            ps.burstFrames = 0;
            ps.burstMoved = ps.burstDesktop = ps.burstReadback = ps.burstParams = 0;
        }
    }
}

void PanelGlassHost::UpdatePanelBackdrop(PanelState& ps, const GlassPanel& p, ID3D11Texture2D* source, RECT monitorRect, double now, bool moved, bool changed,
                                         bool& newDesktop, bool& moving, bool& gpuSubmitted)
{
    GlassRenderer& r = *ps.renderer;
    if (p.desktop) {
        ID3D11Texture2D* pic = desktop_.Texture();
        // Again once the glass is on screen if its ink was never measured: the
        // first copy (before any present) is not compared, and a still desktop
        // would leave a widget with the default ink over any wallpaper.
        // A new picture of the desktop only counts where it changed under this glass (the
        // backdrop it copies: the window, the refraction margin, rounded out to 64 px).
        RECT around = p.window;
        InflateRect(&around, 128, 128);
        bool fresh = ps.desktopVersion != desktop_.Version();
        if (fresh && r.HasBackdrop() && ps.grid.w != 0 && !changed && !desktop_.ChangedSince(ps.desktopVersion, around)) {
            ps.desktopVersion = desktop_.Version();
            fresh = false;
        }
        // Consume the latest desktop frame on the same render loop as the bar.
        if (pic && (changed || fresh || !r.HasBackdrop() || ps.grid.w == 0)) {
            r.UpdateBackdrop(pic, p.window.left - desktop_.Origin().x, p.window.top - desktop_.Origin().y);
            gpuSubmitted = true;
            newDesktop = fresh;
            ps.desktopVersion = desktop_.Version();
            if (fresh)
                ps.inkSettleMs = now;
        }
        return;
    }
    if (const auto extra = panelCaptures_.find(MonitorFromRect(&p.window, MONITOR_DEFAULTTONEAREST)); extra != panelCaptures_.end()) {
        const auto& picture = extra->second;
        const double elapsed = ps.uncoveredSinceMs > 0.0 ? now - ps.uncoveredSinceMs : 0.0;
        const SecondaryBackdropDecision decision =
            SecondaryPanelBackdropReady(picture.partialCopy, picture.copyBounds, PanelCaptureRect(p), picture.bounds, GlassRenderer::kPad,
                                        picture.texture != nullptr, picture.fresh, moved, r.HasBackdrop(), ps.pendingFresh, elapsed);
        ps.pendingFresh = decision.pendingFresh;
        if (decision.waitForFresh) {
            if (ps.uncoveredSinceMs == 0.0)
                ps.uncoveredSinceMs = now;
            moving = true;
        } else {
            ps.uncoveredSinceMs = 0.0;
        }
        if (decision.updateBackdrop) {
            r.UpdateBackdrop(picture.texture.get(), p.window.left - picture.bounds.left, p.window.top - picture.bounds.top);
            gpuSubmitted = true;
        }
        return;
    }
    if (ID3D11Texture2D* below = source ? source : (moved || !r.HasBackdrop()) ? lastFrame_.get() : nullptr) {
        r.UpdateBackdrop(below, p.window.left - monitorRect.left, p.window.top - monitorRect.top);
        gpuSubmitted = true;
    }
}

void PanelGlassHost::UpdatePanelInk(PanelState& ps, HWND hwnd, const Settings& settings, float panelScale, int w, int h, double now, bool& changed, bool& under)
{
    if (auto rb = ps.renderer->Poll()) {
        changed |= rb->changed;
        under = rb->changed;
        if (rb->changed)
            ps.inkSettleMs = now;
        // The ink region by region (cells of about 20 DIP), from what
        // the glass shows there, each cell with the bar's hysteresis;
        // the popup's own ink is what most of it needs.
        UpdatePanelInkGrid(*rb, settings, panelScale, w, h, ps.grid, ps.dark, ps.inkOwed);
    }
    if (ShouldDeferPanelInk(ps.inkOwed, ps.ink, now, ps.inkSettleMs, kDesktopSettleMs)) {
        inkDue_ = true;
    } else if (ps.inkOwed) {
        ps.inkOwed = false;
        PublishInk(hwnd, ps.grid);
        ps.ink = ps.dark ? 1 : 0;
        PostMessageW(hwnd, WM_GLASS_INK, WPARAM(ps.ink), 0);
    }
}

void PanelGlassHost::RecordPanelPresent(PanelState& ps, const GlassPanel& p, int w, int h, double now, double t0, double created, double t1, bool moved,
                                        bool newDesktop, bool under, bool same)
{
    ps.presentedGen = p.gen;
    GlassPresented(p.hwnd, p.gen);
    // A panel that holds the render thread up: what took the time (rate-limited).
    if (const double t2 = ClockMs(); t2 - t0 > 16 && t2 - slowLogged_ > 2000) {
        slowLogged_ = t2;
        wchar_t title[64] = {};
        InternalGetWindowText(p.hwnd, title, 64);
        Log(L"render: slow glass for %s: %.1f ms (renderer created in %.1f ms, backdrop %.1f ms, present %.1f ms)", title, t2 - t0, created, t1 - t0 - created,
            t2 - t1);
    }
    ps.burstMoved += moved ? 1 : 0;
    ps.burstDesktop += newDesktop ? 1 : 0;
    ps.burstReadback += under ? 1 : 0;
    ps.burstParams += same ? 0 : 1;
    ps.params = gp_;
    if (!ps.burstFrames || now - ps.burstLast > 250) {
        ps.burstStart = now;
        ps.burstGap = 0;
        ps.burstFrames = 0;
    } else {
        ps.burstGap = std::max(ps.burstGap, now - ps.burstLast);
    }
    ps.burstLast = now;
    ++ps.burstFrames;
    if (!ps.ready) {
        ps.ready = true;
        PostMessageW(p.hwnd, WM_GLASS_READY, 0, 0);
        wchar_t title[64] = {};
        InternalGetWindowText(p.hwnd, title, 64);
        Log(L"render: popup glass ready for %s (%dx%d px, %zu shapes, radius %.0f px, tint %.2f, ink %s)", title, w, h, gp_.size(),
            gp_.empty() ? 0.f : gp_[0].radius, gp_.empty() ? 0.f : gp_[0].tintMix, ps.dark ? L"dark" : L"light");
    }
}

PanelGlassHost::StepResult PanelGlassHost::RenderSinglePanel(const GlassPanel& p, const Settings& settings, RECT monitorRect, float barScale,
                                                             ID3D11Texture2D* source, uint64_t tick, double now, bool& moving, bool& gpuSubmitted)
{
    const int w = p.window.right - p.window.left, h = p.window.bottom - p.window.top;
    if (w <= 0 || h <= 0 || !IsWindow(p.hwnd) || !IsWindowVisible(p.hwnd))
        return StepResult::Ok;
    const float panelScale = float(GetDpiForWindow(p.hwnd)) / 96.f;
    PanelState& ps = panels_.try_emplace(p.hwnd, p.hwnd).first->second;
    const int bufferW = std::max(w, p.bufferW), bufferH = std::max(h, p.bufferH);
    const double t0 = ClockMs();
    double created = 0;
    if (!ps.renderer) {
        ps.renderer = createRenderer_(gpu_, p.hwnd, bufferW, bufferH, p.desktop);
        if (!ps.renderer)
            return StepResult::Ok;
        created = ClockMs() - t0;
    }
    GlassRenderer& r = *ps.renderer;
    const bool moved = !EqualRect(&ps.window, &p.window);
    bool changed = r.Width() != bufferW || r.Height() != bufferH || moved;
    if (ps.shown != p.shown) {
        // Shown again (a menu replaced by the next one between two frames): it waits for
        // its glass, which is drawn and announced anew; else a plain panel stands in.
        ps.shown = p.shown;
        ps.ready = false;
        ps.ink = -1;
        ps.inkOwed = false;
        ps.inkSettleMs = -1e9;
        changed = true;
    }
    bool newDesktop = false, under = false;
    ps.window = p.window;
    if (moved && !p.desktop && MonitorFromRect(&p.window, MONITOR_DEFAULTTONEAREST) == captureMonitor_)
        panelMovedMs_ = now;
    r.Resize(bufferW, bufferH);
    UpdatePanelBackdrop(ps, p, source, monitorRect, now, moved, changed, newDesktop, moving, gpuSubmitted);
    PanelParams(settings, panelScale, p, gp_);
    UpdatePanelInk(ps, p.hwnd, settings, panelScale, w, h, now, changed, under);
    if (p.pop != ps.pop) {
        r.Pop(p.popOpen, p.popX, p.popY);
        ps.pop = p.pop;
    }
    if (p.rippleMs > 0 && now - p.rippleMs < 720) {
        const float u = float((now - p.rippleMs) / 720);
        for (GlassParams& g : gp_) {
            g.rippleX = p.rippleX;
            g.rippleY = p.rippleY;
            g.rippleR = 150 * barScale * (1 - (1 - u) * (1 - u) * (1 - u));
            g.rippleA = 0.8f * (1 - u) * (1 - u);
        }
        moving = true;
    }
    if (!r.HasBackdrop())
        return StepResult::Ok; // waits for the first capture frame under it
    // During a geometry animation the content supplies each frame.
    // A background-only present just before that update would consume
    // this composition's slot and push the matching geometry a frame
    // late. Keep the latest backdrop and draw it with the new geometry.
    if (p.animating && r.HasPresented() && p.gen == ps.presentedGen)
        return StepResult::Ok;
    const double t1 = ClockMs();
    const bool same = gp_.size() == ps.params.size() && std::memcmp(gp_.data(), ps.params.data(), gp_.size() * sizeof(GlassParams)) == 0;
    if (!changed && !ps.owed && r.HasPresented() && same) {
        ps.presentedGen = p.gen;
        GlassPresented(p.hwnd, p.gen);
        return StepResult::Ok;
    }
    // Once per composed frame (a new picture under it or new shapes may come in between),
    // and only when its last frame is on screen; else at the next tick.
    if ((ps.drawnTick == tick && !moved) || !r.Ready()) {
        if (!r.Ready() && IsPresentReadyStalled(false, now, ps.readyStalledSinceMs, 1000.0)) {
            Log(L"render: popup glass swap chain stalled; recreating");
            ps.renderer = nullptr;
            ps.ready = false;
            ps.readyStalledSinceMs = 0.0;
            ps.owed = true;
            return StepResult::Ok;
        }
        ps.owed = true;
        return StepResult::Ok;
    }
    ps.owed = false;
    ps.drawnTick = tick;
    if (p.desktop)
        r.SetOffset(p.offset);
    if (!r.Present(gp_, nullptr))
        return StepResult::Retry;
    if (r.LastPresentBusy()) {
        if (IsPresentReadyStalled(false, now, ps.readyStalledSinceMs, 1000.0)) {
            Log(L"render: popup glass Present stayed busy; recreating");
            ps.renderer = nullptr;
            ps.ready = false;
            ps.readyStalledSinceMs = 0.0;
        }
        ps.owed = true;
        return StepResult::Ok;
    }
    ps.readyStalledSinceMs = 0.0;
    gpuSubmitted = true;
    RecordPanelPresent(ps, p, w, h, now, t0, created, t1, moved, newDesktop, under, same);
    return StepResult::Ok;
}

PanelGlassHost::RenderResult PanelGlassHost::Render(const std::vector<GlassPanel>& panels, const Settings& settings, RECT monitorRect, float barScale,
                                                    ID3D11Texture2D* source, uint64_t tick, bool barKeepsOwnWindowsOut)
{
    inkDue_ = false;
    if (const StepResult cap = SyncPanelCaptures(panels, barKeepsOwnWindowsOut, settings.widgetBackdrop); cap != StepResult::Ok)
        return {cap, false, false};
    for (auto it = panels_.begin(); it != panels_.end();) {
        const bool listed = std::ranges::any_of(panels, [&](const GlassPanel& p) { return p.hwnd == it->first; });
        it = listed ? std::next(it) : panels_.erase(it);
    }
    const double now = ClockMs();
    // Widgets sample the desktop, never the windows above them; only a
    // new picture of it (or a move) draws their glass again.
    const bool widgets = std::ranges::any_of(panels, [](const GlassPanel& p) { return p.desktop && p.visible; });
    const unsigned desktopBefore = desktop_.Version();
    desktop_.Update(widgets ? settings.widgetBackdrop : std::wstring(L"off"));
    bool gpuSubmitted = desktop_.Version() != desktopBefore;
    FlushIdleBursts(now);
    bool moving = false;
    for (const GlassPanel& p : panels) {
        const StepResult res = RenderSinglePanel(p, settings, monitorRect, barScale, source, tick, now, moving, gpuSubmitted);
        if (res != StepResult::Ok)
            return {res, moving, gpuSubmitted};
    }
    return {StepResult::Ok, moving, gpuSubmitted};
}

} // namespace atrium
