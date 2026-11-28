#include "ui/surface.h"

#include "clock.h"
#include "render/capture.h"
#include "render/continuous_corners.h"
#include "log.h"
#include "render/glass_renderer.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace atrium {

namespace {

constexpr wchar_t kClass[] = L"AtriumSurface";
constexpr UINT_PTR kHideTimer = 0x5A01, kGlassWaitTimer = 0x5A03;
constexpr UINT kHideMs = 140;        // the closing pop (PopVisual) lasts 130 ms
constexpr double kGlassWaitMs = 300; // then a plain panel stands in for missing glass

GlassHost* g_host = nullptr;
bool g_capturable = false, g_captureVisible = false;
std::vector<Surface*> g_all;

bool HasPendingUserInput()
{
    MSG msg{};
    return PeekMessageW(&msg, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) || PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_NOREMOVE);
}

} // namespace

// ---- Surface -----------------------------------------------------------------

struct Surface::Shared {
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<IDXGIFactory2> dxgi;
    winrt::com_ptr<IDCompositionDevice> dcomp;
    winrt::com_ptr<ID2D1Factory1> d2dFactory;
    winrt::com_ptr<ID2D1Device> d2d;
    bool lost = false;
    Shared() = default;
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;
    ~Shared()
    {
        if (d2d)
            ForgetContinuousMaskDomain(d2d.as<IUnknown>().get());
    }
};

std::shared_ptr<Surface::Shared> Surface::SharedDevice()
{
    static std::weak_ptr<Shared> cache;
    if (auto s = cache.lock(); s && !s->lost)
        return s;
    auto s = std::make_shared<Shared>();
    // WARP (software): popups are small 2D content, and a second hardware
    // device costs 70-100 MB of driver memory and wakes the GPU (V16).
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, s->d3d.put(), nullptr,
                                 nullptr))) {
        Log(L"surface: no D3D11 device");
        return nullptr;
    }
    auto dxgiDevice = s->d3d.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(adapter.put());
    adapter->GetParent(IID_PPV_ARGS(s->dxgi.put()));
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, s->d2dFactory.put_void())) ||
        FAILED(s->d2dFactory->CreateDevice(dxgiDevice.get(), s->d2d.put())) ||
        FAILED(DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(s->dcomp.put())))) {
        Log(L"surface: device setup failed");
        return nullptr;
    }
    cache = s;
    return s;
}

Surface::Surface(HINSTANCE instance, const wchar_t* title, bool activatable, float insetDip, bool desktopLayer, bool clickThrough)
    : activatable_(activatable),
      desktopLayer_(desktopLayer),
      clickThrough_(clickThrough),
      insetDip_(insetDip)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.lpfnWndProc = Proc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    DWORD ex = WS_EX_TOOLWINDOW | (desktopLayer ? 0 : WS_EX_TOPMOST) | WS_EX_NOREDIRECTIONBITMAP | (activatable ? 0 : WS_EX_NOACTIVATE) |
               (clickThrough ? (WS_EX_TRANSPARENT | WS_EX_LAYERED) : 0);
    hwnd_ = CreateWindowExW(ex, kClass, title, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (clickThrough)
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // The glass below the content samples the screen: the window must not be
    // in what it samples (Desktop Duplication always leaves it out; other
    // captures only while the bar uses Windows.Graphics.Capture).
    g_all.push_back(this);
    ApplyCaptureExclusion();
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.put()));
}

Surface::~Surface()
{
    std::erase(g_all, this);
    if (hwnd_) {
        if (g_host)
            g_host->SetPanel(GlassPanel{hwnd_}); // not visible: removes the layer
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

void Surface::SetGlassHost(GlassHost* host)
{
    g_host = host;
    if (!host)
        FramePump::Get().Shutdown();
}
void Surface::SetCapturable(bool capturable) { g_capturable = capturable; }
void Surface::ApplyCaptureExclusion() { SetCaptureExclusion(hwnd_, (g_capturable || g_captureVisible) && !private_); }

void Surface::SetCaptureVisible(bool visible)
{
    g_captureVisible = visible;
    for (Surface* s : g_all)
        s->ApplyCaptureExclusion();
}

void Surface::SetPrivate(bool on)
{
    if (on == private_)
        return;
    private_ = on;
    ApplyCaptureExclusion();
}

int Surface::InsetPx() const { return int(std::lround(insetDip_ * scale_)); }

D2D1_SIZE_U Surface::BufferSize() const
{
    const int inset = 2 * InsetPx();
    return D2D1::SizeU(UINT32(std::max({1, int(window_.right - window_.left), int(std::lround(resizeReserve_.width * scale_)) + inset})),
                       UINT32(std::max({1, int(window_.bottom - window_.top), int(std::lround(resizeReserve_.height * scale_)) + inset})));
}

void Surface::SetRadius(float dip)
{
    if (radius_ == dip)
        return;
    radius_ = dip;
    if (visible_)
        PublishPanel();
}

void Surface::SetShapes(const std::vector<ShapeDip>& shapes, bool animating)
{
    auto same = [](const ShapeDip& a, const ShapeDip& b) {
        auto sameRect = [](const auto& x, const auto& y) { return x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom; };
        auto samePart = [&](const ShapeDip::Part& x, const ShapeDip::Part& y) { return sameRect(x.rect, y.rect) && std::equal(x.radii, x.radii + 4, y.radii); };
        return a.radius == b.radius && a.fade == b.fade && a.rotate == b.rotate && sameRect(a.rect, b.rect) && a.melt == b.melt && a.partMix == b.partMix &&
               a.tailX == b.tailX && a.tailHalfWidth == b.tailHalfWidth && a.tailHeight == b.tailHeight && a.tailInset == b.tailInset &&
               a.parts.size() == b.parts.size() && std::equal(a.parts.begin(), a.parts.end(), b.parts.begin(), samePart);
    };
    if (!animating && !shapesAnimating_ && shapes.size() == shapes_.size() && std::equal(shapes.begin(), shapes.end(), shapes_.begin(), same))
        return;
    shapes_ = shapes;
    shapesAnimating_ = animating;
    if (visible_ || hiding_)
        PublishPanel();
}

void Surface::PublishPanel()
{
    if (!g_host || glassless_)
        return;
    GlassPanel p;
    p.hwnd = hwnd_;
    p.gen = ++glassGen_;
    p.shown = glassShown_;
    glassPending_ = true;
    p.window = window_;
    const auto buffer = BufferSize();
    p.bufferW = int(buffer.width);
    p.bufferH = int(buffer.height);
    p.inset = float(InsetPx());
    p.radius = radius_ * scale_;
    for (const ShapeDip& s : shapes_) {
        // Whole px, so the glass and the content's washes line up.
        auto px = [&](float v) { return std::round(v * scale_) + p.inset; };
        GlassShape g{px(s.rect.left), px(s.rect.top), px(s.rect.right), px(s.rect.bottom), s.radius * scale_, s.fade};
        for (const ShapeDip::Part& part : s.parts)
            g.parts.push_back({px(part.rect.left),
                               px(part.rect.top),
                               px(part.rect.right),
                               px(part.rect.bottom),
                               {part.radii[0] * scale_, part.radii[1] * scale_, part.radii[2] * scale_, part.radii[3] * scale_}});
        g.rotate = s.rotate;
        g.melt = s.melt * scale_;
        g.partMix = s.partMix;
        g.tailX = px(s.tailX);
        g.tailHalfWidth = s.tailHalfWidth * scale_;
        g.tailHeight = s.tailHeight * scale_;
        g.tailInset = s.tailInset * scale_;
        p.shapes.push_back(std::move(g));
    }
    p.visible = visible_ || hiding_;
    p.desktop = desktopLayer_;
    if (desktopLayer_)
        p.offset = POINT{window_.left - nativeWindow_.left, window_.top - nativeWindow_.top};
    p.animating = shapesAnimating_;
    p.pop = pop_;
    p.popOpen = popOpen_;
    p.popX = float(PopOrigin(anchor_).x);
    p.popY = float(PopOrigin(anchor_).y);
    p.rippleX = rippleX_;
    p.rippleY = rippleY_;
    p.rippleMs = rippleMs_;
    g_host->SetPanel(p);
}

void Surface::StartGlassWait()
{
    glassReady_ = glassless_; // nothing to wait for
    ++glassShown_;
    shownMs_ = ClockMs();
    if (!glassless_)
        SetTimer(hwnd_, kGlassWaitTimer, UINT(kGlassWaitMs) + 20, nullptr);
}

void Surface::SetGlassless(bool on)
{
    if (on == glassless_)
        return;
    if (on) {
        // Withdraw the glass layer while it is still published, then paint without waiting for it.
        const bool visible = visible_, hiding = hiding_;
        visible_ = hiding_ = false;
        PublishPanel();
        visible_ = visible;
        hiding_ = hiding;
        glassless_ = true;
        glassPending_ = false;
        glassReady_ = true;
        KillTimer(hwnd_, kGlassWaitTimer);
        Present(); // a frame waiting for the glass that will not come
        Invalidate();
        return;
    }
    glassless_ = false;
    if (!visible_)
        return;
    // As on Show: the content waits a little for its glass, then a plain panel stands in.
    StartGlassWait();
    PublishPanel();
    Invalidate();
}

void Surface::Ripple(float x, float y)
{
    int inset = InsetPx();
    rippleX_ = x * scale_ + float(inset);
    rippleY_ = y * scale_ + float(inset);
    rippleMs_ = ClockMs();
    PublishPanel();
}

bool Surface::HandleMouse(UINT m, LPARAM& l)
{
    // Mouse positions in content coordinates; presses on the margin are the caller's to decide.
    const bool mouse = (m >= WM_MOUSEFIRST && m <= WM_MBUTTONDBLCLK && m != WM_MOUSEWHEEL) || m == WM_MOUSEHWHEEL;
    if (!mouse || m == WM_MOUSEHWHEEL)
        return false;
    const int inset = InsetPx();
    int x = GET_X_LPARAM(l) - inset, y = GET_Y_LPARAM(l) - inset;
    if (desktopLayer_) {
        x -= window_.left - nativeWindow_.left;
        y -= window_.top - nativeWindow_.top;
    }
    const bool outside = x < 0 || y < 0 || x >= rect_.right - rect_.left || y >= rect_.bottom - rect_.top;
    if (outside && GetCapture() != hwnd_ && (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN)) {
        if (onDismiss)
            onDismiss();
        return true;
    }
    if (hiding_)
        return true;
    l = MAKELPARAM(x, y);
    return false;
}

LRESULT CALLBACK Surface::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Surface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(h, m, w, l);
    if (self->clickThrough_ && m == WM_NCHITTEST)
        return HTTRANSPARENT;
    switch (m) {
    case WM_TIMER:
        if (w == kHideTimer) {
            self->FinishHide();
            return 0;
        }
        if (w == kGlassWaitTimer) {
            KillTimer(h, kGlassWaitTimer);
            self->Invalidate(); // a plain panel if the glass still has not come
            if (self->onReady && !self->glassReady_)
                self->onReady();
            return 0;
        }
        break;
    case WM_SURFACE_FRAME:
        if (self->visible_) {
            if (HasPendingUserInput())
                InvalidateRect(h, nullptr, FALSE);
            else
                self->Paint();
        }
        return 0;
    case WM_GLASS_PRESENTED:
        if (self->presentPending_ && unsigned(w) >= self->presentGen_)
            self->Present();
        return 0;
    case WM_GLASS_READY:
        if (!self->glassReady_ && self->visible_) {
            self->glassReady_ = true;
            KillTimer(h, kGlassWaitTimer);
            self->Invalidate();
            if (self->onReady)
                self->onReady();
        }
        return 0;
    case WM_GLASS_INK: self->SetInk(w != 0, InkFor(h)); return 0;
    default: break;
    }
    if (self->HandleMouse(m, l))
        return 0;
    if (self->message) {
        LRESULT result = 0;
        if (self->message(m, w, l, result))
            return result;
    }
    switch (m) {
    case WM_MOUSEACTIVATE: return self->activatable_ ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        self->Paint();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED:
        self->Place(self->rect_);
        self->PublishPanel();
        self->Invalidate();
        return 0;
    case WM_CLOSE:
        if (self->onDismiss)
            self->onDismiss();
        return 0;
    default: return DefWindowProcW(h, m, w, l);
    }
}

void Surface::Place(RECT rect, bool keepZOrder)
{
    rect_ = rect;
    scale_ = PopupScale(rect);
    int inset = InsetPx();
    window_ = RECT{rect.left - inset, rect.top - inset, rect.right + inset, rect.bottom + inset};
    ApplyPlacement(keepZOrder);
}

void Surface::ApplyPlacement(bool keepZOrder)
{
    if (desktopLayer_) {
        HMONITOR mon = MonitorFromRect(&rect_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor{sizeof(monitor)};
        RECT targetMonitor{};
        if (!mon || GetMonitorInfoW(mon, &monitor) == FALSE)
            targetMonitor = !IsRectEmpty(&window_) ? window_ : rect_;
        else
            targetMonitor = monitor.rcMonitor;
        if (IsRectEmpty(&targetMonitor))
            return;
        if (!EqualRect(&nativeWindow_, &targetMonitor)) {
            nativeWindow_ = targetMonitor;
            SetWindowPos(hwnd_, nullptr, nativeWindow_.left, nativeWindow_.top, nativeWindow_.right - nativeWindow_.left,
                         nativeWindow_.bottom - nativeWindow_.top, SWP_NOACTIVATE | SWP_NOZORDER);
            inputRegion_ = {};
        }
        // The host has no full-screen bitmap. Its region admits only the
        // widget and shadow; include the previous frame while it is in flight.
        RECT region = window_;
        if (!IsRectEmpty(&presentedWindow_))
            UnionRect(&region, &region, &presentedWindow_);
        OffsetRect(&region, -nativeWindow_.left, -nativeWindow_.top);
        if (!EqualRect(&region, &inputRegion_)) {
            HRGN shape = CreateRectRgn(region.left, region.top, region.right, region.bottom);
            if (SetWindowRgn(hwnd_, shape, FALSE))
                inputRegion_ = region; // Windows owns the region after success
            else
                DeleteObject(shape);
        }
        return;
    }
    const bool noZOrder = keepZOrder && visible_;
    const UINT flags = SWP_NOACTIVATE | (noZOrder ? SWP_NOZORDER : 0u);
    SetWindowPos(hwnd_, noZOrder ? nullptr : HWND_TOPMOST, window_.left, window_.top, window_.right - window_.left, window_.bottom - window_.top, flags);
    ApplyRegion();
}

void Surface::SetRegion(D2D1_RECT_F dip)
{
    region_ = dip;
    regionNone_ = false;
    ApplyRegion();
}

void Surface::SetRegionNone()
{
    regionNone_ = true;
    ApplyRegion();
}

void Surface::ApplyRegion()
{
    if (desktopLayer_)
        return; // its own region (ApplyPlacement)
    RECT r{};
    if (regionNone_) {
        r = RECT{-1, -1, -1, -1}; // apart from the whole window's empty rectangle
    } else if (region_.right > region_.left && region_.bottom > region_.top) {
        const int inset = InsetPx();
        r = RECT{LONG(std::floor(region_.left * scale_)), LONG(std::floor(region_.top * scale_)), LONG(std::ceil(region_.right * scale_)) + 2 * inset,
                 LONG(std::ceil(region_.bottom * scale_)) + 2 * inset};
    }
    if (EqualRect(&r, &inputRegion_))
        return;
    HRGN shape = regionNone_ ? CreateRectRgn(0, 0, 0, 0) : IsRectEmpty(&r) ? nullptr : CreateRectRgn(r.left, r.top, r.right, r.bottom);
    if (SetWindowRgn(hwnd_, shape, FALSE))
        inputRegion_ = r; // Windows owns the region after success
    else if (shape)
        DeleteObject(shape);
}

void Surface::Show(RECT rect, bool activate, POINT anchor, bool pop)
{
    KillTimer(hwnd_, kHideTimer);
    Place(rect);
    anchor_ = anchor;
    const bool wasVisible = visible_;
    bool wasHiding = hiding_;
    hiding_ = false;
    if (!visible_) {
        visible_ = true;
        failedFrames_ = 0;
        firstPaint_ = !wasHiding;
        if (!wasHiding)
            StartGlassWait();
        ShowWindow(hwnd_, activate && activatable_ ? SW_SHOW : SW_SHOWNOACTIVATE);
    }
    if (!desktopLayer_)
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | (activate && activatable_ ? 0 : SWP_NOACTIVATE));
    if (activate && activatable_ && !SetForegroundWindow(hwnd_))
        SwitchToThisWindow(hwnd_, TRUE);
    // Content and glass pop out of the anchor together (DWM runs both
    // animations). The animation is in place before the first frame shows.
    popOpen_ = true;
    if (pop)
        ++pop_;
    if (EnsureDevice() && pop)
        PopVisual(shared_->dcomp.get(), visual_.get(), true, float(PopOrigin(anchor).x), float(PopOrigin(anchor).y));
    Paint();
    PublishPanel();
    if (!wasVisible && glassReady_ && onReady)
        onReady();
}

void Surface::Move(RECT rect, bool keepZOrder)
{
    // Keep each desktop content frame paired with its glass geometry.
    // Input can replace the next request
    // while the current one is in flight, without waiting on the UI thread.
    if (desktopLayer_ && visible_ && glassReady_ && presentPending_) {
        nextResize_ = rect;
        frameWanted_ = true;
        return;
    }
    if (EqualRect(&rect, &rect_))
        return;
    bool resized = rect.right - rect.left != rect_.right - rect_.left || rect.bottom - rect.top != rect_.bottom - rect_.top;
    Place(rect, keepZOrder);
    if (visible_) {
        PublishPanel();
        if (resized || desktopLayer_) // desktop translation travels with the pixel buffer
            Invalidate();
    }
}

void Surface::ReserveResize(D2D1_SIZE_F sizeDip)
{
    const bool release = sizeDip.width == 0 && sizeDip.height == 0;
    releaseResize_ = release && (presentPending_ || nextResize_.has_value());
    if (releaseResize_)
        return; // first finish the final geometry frame, then return its spare buffers
    if (resizeReserve_.width == sizeDip.width && resizeReserve_.height == sizeDip.height)
        return;
    resizeReserve_ = sizeDip;
    presentPending_ = false;
    if (visible_) {
        PublishPanel();
        Invalidate();
    }
}

void Surface::Hide(bool animate)
{
    if (!visible_)
        return;
    visible_ = false;
    presentPending_ = false;
    presentedWindow_ = {};
    nextResize_.reset();
    releaseResize_ = false;
    shapesAnimating_ = false;
    burst_ = Burst{}; // time spent hidden is not an animation gap
    if (!animate || !visual_ || !shared_) {
        FinishHide();
        return;
    }
    hiding_ = true;
    ++pop_;
    popOpen_ = false;
    PopVisual(shared_->dcomp.get(), visual_.get(), false, float(PopOrigin(anchor_).x), float(PopOrigin(anchor_).y));
    PublishPanel();
    SetTimer(hwnd_, kHideTimer, kHideMs, nullptr);
}

void Surface::FinishHide()
{
    KillTimer(hwnd_, kHideTimer);
    KillTimer(hwnd_, kGlassWaitTimer);
    hiding_ = false;
    visible_ = false;
    glassReady_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    PublishPanel(); // not visible: the glass layer goes too
    ResetDevice();  // the swap chain is recreated on the next Show
}

void Surface::SetInk(bool dark, const InkGrid& grid)
{
    double now = ClockMs();
    // Not on screen yet (or a new grid): the ink is just taken.
    bool snap = !visible_ || !glassReady_;
    if (!snap && dark == darkInk_ && grid == inkGrid_)
        return;
    darkInk_ = dark;
    if (snap)
        inkAll_ = InkFade{};
    inkAll_.Target(dark ? 1.f : 0.f, now);
    if (snap || grid.w != inkGrid_.w || grid.h != inkGrid_.h || grid.dark.size() != inkCells_.size())
        inkCells_.assign(grid.dark.size(), InkFade{});
    inkGrid_ = grid;
    for (size_t i = 0; i < inkCells_.size(); ++i)
        inkCells_[i].Target(grid.dark[i] ? 1.f : 0.f, now);
    if (!visible_)
        return;
    if (HasPendingUserInput())
        InvalidateRect(hwnd_, nullptr, FALSE);
    else
        Invalidate();
}

void Surface::Invalidate()
{
    if (visible_)
        Paint();
}

void Surface::ResetDevice()
{
    presentPending_ = false;
    presentedWindow_ = {};
    nextResize_.reset();
    releaseResize_ = false;
    d2d_ = nullptr;
    if (target_)
        target_->SetRoot(nullptr);
    visual_ = nullptr;
    target_ = nullptr;
    backTarget_ = nullptr;
    swap_ = nullptr;
    brush_ = nullptr;
    calloutMask_ = nullptr;
    roundedMask_ = nullptr;
    blur_ = nullptr; // an effect belongs to the device context that made it
    bitmaps_.clear();
    bufW_ = bufH_ = 0;
    if (shared_)
        shared_->dcomp->Commit();
    shared_ = nullptr;
}

// A lost frame is drawn again on the fresh device at once: nothing else may ask for one, and the
// window would show only its glass. A device that keeps failing is not retried in a loop.
void Surface::RetryFrame()
{
    if (++failedFrames_ <= 3 && visible_)
        FramePump::Get().Want(hwnd_);
}

bool Surface::EnsureDevice()
{
    if (d2d_)
        return true;
    shared_ = SharedDevice();
    if (!shared_) {
        RetryFrame();
        return false;
    }
    const auto size = BufferSize();
    int w = int(size.width), h = int(size.height);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    auto& s = *shared_;
    if (FAILED(s.dxgi->CreateSwapChainForComposition(s.d3d.get(), &desc, nullptr, swap_.put())) ||
        FAILED(s.dcomp->CreateTargetForHwnd(hwnd_, TRUE, target_.put())) || FAILED(s.dcomp->CreateVisual(visual_.put())) ||
        FAILED(visual_->SetContent(swap_.get())) || FAILED(target_->SetRoot(visual_.get())) || FAILED(s.dcomp->Commit()) ||
        FAILED(s.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put()))) {
        Log(L"surface: swap chain setup failed");
        RecoverDevice(); // marks shared_->lost, resets, and retries boundedly
        return false;
    }
    bufW_ = w;
    bufH_ = h;
    return true;
}

void Surface::RecoverDevice()
{
    if (shared_)
        shared_->lost = true;
    ResetDevice();
    RetryFrame();
}

void Surface::SetupCanvasInk(Canvas& c)
{
    c.panel = !g_host || (!glassReady_ && c.now - shownMs_ > kGlassWaitMs);
    if (c.panel) {
        c.theme = Theme::Panel();
        c.inkDark = c.theme.light ? 1.f : 0.f;
        return;
    }
    // The inks fade to what the glass needs now (InkFade).
    bool fading = inkAll_.Moving(c.now);
    c.inkDark = inkAll_.set ? inkAll_.Value(c.now) : (darkInk_ ? 1.f : 0.f);
    c.theme = Theme::Mix(c.inkDark);
    if (!inkCells_.empty()) {
        c.ink.w = inkGrid_.w;
        c.ink.h = inkGrid_.h;
        c.ink.cellX = inkGrid_.cellX;
        c.ink.cellY = inkGrid_.cellY;
        c.ink.dark.resize(inkCells_.size());
        for (size_t i = 0; i < inkCells_.size(); ++i) {
            c.ink.dark[i] = inkCells_[i].Value(c.now);
            fading |= inkCells_[i].Moving(c.now);
        }
    }
    if (fading)
        frameWanted_ = true;
}

} // namespace atrium
