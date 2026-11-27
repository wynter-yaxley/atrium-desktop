#include "ui/surface.h"

#include "clock.h"
#include "log.h"
#include "render/glass_renderer.h"

#include <algorithm>
#include <vector>

namespace atrium {

void Surface::Paint()
{
    if (painting_) {
        frameWanted_ = true;
        return;
    }
    painting_ = true;
    PaintFrame();
    painting_ = false;
}

bool Surface::PrepareSwapTarget(int w, int h)
{
    if (w != bufW_ || h != bufH_) {
        d2d_->SetTarget(nullptr);
        backTarget_ = nullptr;
        if (FAILED(swap_->ResizeBuffers(2, UINT(w), UINT(h), DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            RecoverDevice();
            return false;
        }
        bufW_ = w;
        bufH_ = h;
    }
    if (!backTarget_) {
        winrt::com_ptr<IDXGISurface> back;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(back.put()))) || FAILED(d2d_->CreateBitmapFromDxgiSurface(back.get(), &bp, backTarget_.put()))) {
            RecoverDevice();
            return false;
        }
    }
    return true;
}

void Surface::PaintFrame()
{
    if (presentPending_) {
        // Keep this buffer paired with its glass. Input and time continue to
        // advance; the next frame uses their latest state once it is presented.
        frameWanted_ = true;
        return;
    }
    if (!visible_ || !EnsureDevice())
        return;
    const auto size = BufferSize();
    int w = int(size.width), h = int(size.height);
    if (!PrepareSwapTarget(w, h))
        return;
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    d2d_->SetTarget(backTarget_.get());
    d2d_->BeginDraw();
    d2d_->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2d_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    frameWanted_ = false;
    if (paint) {
        float inset = float(InsetPx());
        d2d_->SetTransform(D2D1::Matrix3x2F::Translation(inset, inset));
        Canvas c(*this, d2d_.get(), scale_);
        c.now = ClockMs();
        SetupCanvasInk(c);
        paint(c);
        d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    HRESULT hr = d2d_->EndDraw();
    d2d_->SetTarget(nullptr);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    if (firstPaint_ || ms > 16) {
        wchar_t title[64] = {};
        GetWindowTextW(hwnd_, title, 64);
        Log(L"surface: %s painted %dx%d in %.1f ms%s", title, w, h, ms, firstPaint_ ? L" (first after show)" : L"");
        firstPaint_ = false;
    }
    if (FAILED(hr)) {
        Log(L"surface: paint failed 0x%08X, recreating", unsigned(hr));
        RecoverDevice();
        return;
    }
    paintMs_ = ms;
    presentGen_ = glassGen_;
    presentPending_ = true;
    bool waitForGlass = glassPending_ && glassReady_ && !IsGlassPresented(hwnd_, presentGen_);
    glassPending_ = false;
    if (!waitForGlass)
        Present();
}

void Surface::Present()
{
    if (!visible_ || !presentPending_ || !swap_)
        return;
    if (desktopLayer_) {
        ApplyPlacement();
        const DXGI_MATRIX_3X2_F matrix{1, 0, 0, 1, float(window_.left - nativeWindow_.left), float(window_.top - nativeWindow_.top)};
        if (FAILED(swap_.as<IDXGISwapChain2>()->SetMatrixTransform(&matrix))) {
            RecoverDevice();
            return;
        }
        // One composed frame later the region can forget the previous bounds.
        frameWanted_ |= !EqualRect(&presentedWindow_, &window_);
        presentedWindow_ = window_;
    }
    presentPending_ = false;
    HRESULT hr = swap_->Present(0, 0);
    if (FAILED(hr)) {
        Log(L"surface: present failed 0x%08X, recreating", unsigned(hr));
        RecoverDevice();
        return;
    }
    failedFrames_ = 0;
    const double ms = paintMs_;
    // Frame statistics of an animation (a run of frames that asked for more).
    double end = ClockMs();
    if (frameWanted_) {
        if (!burst_.frames)
            burst_ = Burst{end, end, 0, 0, 0};
        burst_.maxGap = std::max(burst_.maxGap, end - burst_.last);
        burst_.maxPaint = std::max(burst_.maxPaint, ms);
        burst_.last = end;
        ++burst_.frames;
    } else if (burst_.frames) {
        if (burst_.frames >= 4) {
            wchar_t title[64] = {};
            GetWindowTextW(hwnd_, title, 64);
            double span = end - burst_.start;
            Log(L"surface: %s animated %d frames in %.0f ms (%.0f fps), longest gap %.1f ms, slowest paint %.1f ms", title, burst_.frames + 1, span,
                span > 0 ? 1000.0 * burst_.frames / span : 0.0, std::max(burst_.maxGap, end - burst_.last), std::max(burst_.maxPaint, ms));
        }
        burst_ = Burst{};
    }
    if (nextResize_) {
        RECT next = *nextResize_;
        nextResize_.reset();
        Move(next);
    }
    if (releaseResize_ && !presentPending_ && !nextResize_)
        ReserveResize(D2D1::SizeF());
    if (frameWanted_ && visible_)
        FramePump::Get().Want(hwnd_);
}

} // namespace atrium
