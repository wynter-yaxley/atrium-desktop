#include "ui/window_snapshot.h"

#include "log.h"
#include "shell/process_integrity.h"
#include "render/window_capture.h"
#include "win32_path.h"

#include <dwmapi.h>
#include <roapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <thread>
#include <utility>

namespace atrium {

namespace {

// Coverage of a rounded rectangle (0..w, 0..h, corners r) at the centre of pixel (x, y).
float RoundedCoverage(int x, int y, int w, int h, float r)
{
    if (x < 0 || x >= w || y < 0 || y >= h)
        return 0.f;
    r = std::clamp(r, 0.f, 0.5f * float(std::min(w, h)));
    if (r <= 0.5f)
        return 1.f;
    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
    if ((px >= r && px <= float(w) - r) || (py >= r && py <= float(h) - r))
        return 1.f;
    const float cx = px < r ? r : float(w) - r, cy = py < r ? r : float(h) - r;
    return std::clamp(r + 0.5f - std::hypot(px - cx, py - cy), 0.f, 1.f);
}

// All-zero buffers and near-uniform frames (not yet drawn) are blank; 32+ differing pixels indicate content.
bool Blank(const uint8_t* src, int w, int h, int stride)
{
    const uint8_t* mid = src + size_t(h / 2) * stride + size_t(w / 2) * 4;
    bool nonZero = (mid[0] | mid[1] | mid[2] | mid[3]) != 0;
    int diff = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const uint8_t* p = src + size_t(y) * stride + size_t(x) * 4;
            nonZero = nonZero || (p[0] | p[1] | p[2] | p[3]) != 0;
            if ((std::abs(p[0] - mid[0]) > 6 || std::abs(p[1] - mid[1]) > 6 || std::abs(p[2] - mid[2]) > 6) && ++diff >= 32)
                return false;
        }
    return !nonZero || diff < 32;
}

} // namespace

// A box's Gaussian blur, axis by axis (the corners' rounding is lost in the blur).
IconImage SoftShadow(int w, int h)
{
    const int pad = kShadowPad / 2, sw = std::max(1, w / 2), sh = std::max(1, h / 2);
    const int outW = sw + 2 * pad, outH = sh + 2 * pad;
    std::vector<uint8_t> px(size_t(outW) * outH * 4, 0);
    const float sigma = float(pad) / 2.6f;
    auto edge = [&](float v, float len) {
        auto erfAt = [&](float t) { return 0.5f * (1 + std::erf(t / (sigma * std::numbers::sqrt2_v<float>))); };
        return erfAt(v) - erfAt(v - len);
    };
    std::vector<float> fx(static_cast<size_t>(outW)), fy(static_cast<size_t>(outH));
    for (int x = 0; x < outW; ++x)
        fx[size_t(x)] = edge(float(x) + 0.5f - float(pad), float(sw));
    for (int y = 0; y < outH; ++y)
        fy[size_t(y)] = edge(float(y) + 0.5f - float(pad), float(sh));
    for (int y = 0; y < outH; ++y)
        for (int x = 0; x < outW; ++x)
            px[(size_t(y) * outW + x) * 4 + 3] = uint8_t(std::lround(255 * std::clamp(fx[size_t(x)] * fy[size_t(y)], 0.f, 1.f)));
    return IconImage::FromBgra(outW, outH, std::move(px));
}

WindowPicture BuildWindowPictureFromBgra(const uint8_t* src, int sw, int sh, int stride, int tw, int th, float radius, bool shadow)
{
    if (!src || sw <= 0 || sh <= 0 || tw <= 0 || th <= 0 || stride < sw * 4)
        return {};
    WindowPicture p;
    p.width = tw;
    p.height = th;
    const int outW = tw + 2 * kPicturePad, outH = th + 2 * kPicturePad;
    std::vector<uint8_t> bgra(size_t(outW) * outH * 4, 0);
    auto writePixel = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
        uint8_t* d = &bgra[(size_t(y + kPicturePad) * outW + x + kPicturePad) * 4];
        const float a = radius <= 0.5f ? 1.f : RoundedCoverage(x, y, tw, th, radius);
        if (a >= 1.f) {
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = 255;
            return;
        }
        d[0] = uint8_t(std::lround(b * a));
        d[1] = uint8_t(std::lround(g * a));
        d[2] = uint8_t(std::lround(r * a));
        d[3] = uint8_t(std::lround(255 * a));
    };
    if (sw == tw && sh == th) {
        for (int y = 0; y < th; ++y) {
            const uint8_t* row = src + size_t(y) * stride;
            for (int x = 0; x < tw; ++x) {
                const uint8_t* s = row + size_t(x) * 4;
                writePixel(x, y, s[0], s[1], s[2]);
            }
        }
    } else {
        std::vector<int> col(static_cast<size_t>(sw));
        for (int x = 0; x < sw; ++x)
            col[size_t(x)] = std::min(tw - 1, int(int64_t(x) * tw / sw));
        std::vector<uint64_t> rowSum(size_t(tw) * 3, 0);
        std::vector<uint32_t> rowCount(size_t(tw), 0);
        auto flushRow = [&](int ty) {
            for (int tx = 0; tx < tw; ++tx) {
                const uint32_t n = std::max<uint32_t>(1, rowCount[size_t(tx)]);
                const size_t idx = size_t(tx) * 3;
                writePixel(tx, ty, uint8_t(rowSum[idx] / n), uint8_t(rowSum[idx + 1] / n), uint8_t(rowSum[idx + 2] / n));
            }
            std::ranges::fill(rowSum, 0);
            std::ranges::fill(rowCount, 0);
        };
        int curTy = 0;
        for (int y = 0; y < sh; ++y) {
            const int ty = std::min(th - 1, int(int64_t(y) * th / sh));
            while (curTy < ty)
                flushRow(curTy++);
            const uint8_t* row = src + size_t(y) * stride;
            for (int x = 0; x < sw; ++x) {
                const size_t tx = size_t(col[size_t(x)]);
                const uint8_t* s = row + size_t(x) * 4;
                rowSum[tx * 3] += s[0];
                rowSum[tx * 3 + 1] += s[1];
                rowSum[tx * 3 + 2] += s[2];
                ++rowCount[tx];
            }
        }
        while (curTy < th)
            flushRow(curTy++);
    }
    p.image = IconImage::FromBgra(outW, outH, std::move(bgra));
    if (shadow)
        p.shadow = SoftShadow(tw, th);
    p.ok = true;
    return p;
}

namespace {

// The window's own corners, px: rounded on Windows 11 (unless maximized, or the app asks otherwise),
// square before.
float CornerRadius(HWND window)
{
    if (IsZoomed(window))
        return 0;
    static const bool kEleven = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{sizeof(v)};
        auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return get && get(&v) == 0 && v.dwBuildNumber >= 22000;
    }();
    if (!kEleven)
        return 0;
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_DEFAULT;
    DwmGetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    if (pref == DWMWCP_DONOTROUND)
        return 0;
    return (pref == DWMWCP_ROUNDSMALL ? 4.f : 8.f) * float(GetDpiForWindow(window)) / 96.f;
}

bool CanPrintWindow(HWND window) { return IsWindow(window) && !IsIconic(window) && IsWindowVisible(window) && !IsHungAppWindow(window); }

void LogCaptureFailure(std::unordered_set<HWND>& logged, HWND window, const wchar_t* reason, int w, int h, DWORD err = 0)
{
    if (!logged.insert(window).second)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const std::wstring exe = pid ? ProcessImagePath(pid) : L"";
    wchar_t cls[96] = {};
    GetClassNameW(window, cls, int(std::size(cls)));
    Log(L"snapshot: %s for %p err=%lu exe=%s class=%s size=%dx%d", reason, window, err, exe.empty() ? L"?" : exe.c_str(), cls[0] ? cls : L"?", w, h);
}

WindowPicture BuildPictures(HWND window, const uint8_t* src, int pw, int ph, int stride, int maxWidth, int maxHeight, float radius, int largeMax,
                            WindowPicture* large)
{
    const float f = std::min({1.f, float(maxWidth) / float(pw), float(maxHeight) / float(ph)});
    const int tw = std::max(1, int(std::lround(pw * f))), th = std::max(1, int(std::lround(ph * f)));
    WindowPicture result = BuildWindowPictureFromBgra(src, pw, ph, stride, tw, th, radius);
    if (largeMax > 0 && large) {
        // Near the window's own size, with its own corners: where it flies to its place.
        const float g = std::min(1.f, float(largeMax) / float(std::max(pw, ph)));
        const int lw = std::max(1, int(std::lround(pw * g))), lh = std::max(1, int(std::lround(ph * g)));
        const float cr = CornerRadius(window) * g;
        *large = BuildWindowPictureFromBgra(src, pw, ph, stride, lw, lh, cr, false);
        large->corner = cr;
    }
    return result;
}

WindowPicture TryPrintWindow(HWND window, const RECT& win, const RECT& frame, int maxWidth, int maxHeight, float radius, int largeMax, WindowPicture* large,
                             HANDLE stopEvent, bool& printed, DWORD& pwErr)
{
    if ((stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) || !CanPrintWindow(window))
        return {};
    DWORD_PTR ping = 0;
    if (!SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &ping)) {
        pwErr = GetLastError();
        return {};
    }
    const int ww = win.right - win.left, wh = win.bottom - win.top;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = ww;
    bi.bmiHeader.biHeight = -wh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dc || !dib || !bits) {
        if (dib)
            DeleteObject(dib);
        if (dc)
            DeleteDC(dc);
        return {};
    }
    HGDIOBJ old = SelectObject(dc, dib);
    // PW_RENDERFULLCONTENT: what DWM shows, also of windows drawn with the GPU.
    if (CanPrintWindow(window)) {
        printed = PrintWindow(window, dc, 2) != FALSE;
        if (!printed)
            pwErr = GetLastError();
    }
    GdiFlush();
    WindowPicture result;
    if (printed) {
        // The window without its invisible resize borders.
        RECT part{std::max(0L, frame.left - win.left), std::max(0L, frame.top - win.top), std::min<LONG>(ww, frame.right - win.left),
                  std::min<LONG>(wh, frame.bottom - win.top)};
        const int pw = part.right - part.left, ph = part.bottom - part.top;
        const uint8_t* src = static_cast<const uint8_t*>(bits) + (size_t(part.top) * ww + part.left) * 4;
        // Nothing came (some windows cannot be printed), or one plain colour (a window that has not
        // drawn itself yet): fall back to Windows.Graphics.Capture.
        if (pw > 0 && ph > 0 && !Blank(src, pw, ph, ww * 4))
            result = BuildPictures(window, src, pw, ph, ww * 4, maxWidth, maxHeight, radius, largeMax, large);
    }
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    return result;
}

WindowPicture Capture(HWND window, int maxWidth, int maxHeight, float radius, int largeMax, WindowPicture* large, std::unordered_set<HWND>& logged,
                      WindowFrameCapturer& capturer, HANDLE stopEvent)
{
    if (WindowHasHigherIntegrity(window)) {
        RECT win{};
        GetWindowRect(window, &win);
        LogCaptureFailure(logged, window, L"skipped (higher integrity / UIPI)", win.right - win.left, win.bottom - win.top, ERROR_ACCESS_DENIED);
        return {};
    }
    if (!CanPrintWindow(window)) {
        RECT win{};
        GetWindowRect(window, &win);
        LogCaptureFailure(logged, window, L"CanPrintWindow false", win.right - win.left, win.bottom - win.top);
        return {};
    }
    RECT win{}, frame{};
    if (!GetWindowRect(window, &win) || win.right <= win.left || win.bottom <= win.top)
        return {};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) || frame.right <= frame.left || frame.bottom <= frame.top)
        frame = win;
    if (int64_t(win.right) - win.left > 16384 || int64_t(win.bottom) - win.top > 16384)
        return {};
    const int ww = win.right - win.left, wh = win.bottom - win.top;
    bool printed = false;
    DWORD pwErr = 0;
    WindowPicture result = TryPrintWindow(window, win, frame, maxWidth, maxHeight, radius, largeMax, large, stopEvent, printed, pwErr);
    if (result.ok)
        return result;
    if (!CanPrintWindow(window)) {
        LogCaptureFailure(logged, window, L"CanPrintWindow false", ww, wh);
        return {};
    }
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
        return {};
    CapturedWindowFrame wgcFrame;
    HRESULT wgcHr = S_OK;
    if (capturer.Capture(window, stopEvent, 180, wgcFrame, &wgcHr) && wgcFrame.width > 0 && wgcFrame.height > 0 &&
        !Blank(wgcFrame.bgra.data(), wgcFrame.width, wgcFrame.height, wgcFrame.width * 4)) {
        return BuildPictures(window, wgcFrame.bgra.data(), wgcFrame.width, wgcFrame.height, wgcFrame.width * 4, maxWidth, maxHeight, radius, largeMax, large);
    }
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
        return {};
    const wchar_t* reason = !printed ? L"PrintWindow+WGC failed" : (wgcFrame.width > 0 ? L"Blank (PrintWindow+WGC)" : L"Blank+WGC failed");
    LogCaptureFailure(logged, window, reason, ww, wh, FAILED(wgcHr) ? static_cast<DWORD>(wgcHr) : pwErr);
    return {};
}

} // namespace

WindowPicture Smaller(const WindowPicture& picture, int largeMax)
{
    if (!picture.ok || std::max(picture.width, picture.height) <= largeMax)
        return picture;
    const float g = float(largeMax) / float(std::max(picture.width, picture.height));
    const int w = std::max(1, int(std::lround(picture.width * g))), h = std::max(1, int(std::lround(picture.height * g)));
    const int stride = picture.image.width * 4;
    const uint8_t* src = picture.image.Data() + size_t(kPicturePad) * stride + size_t(kPicturePad) * 4;
    WindowPicture out = BuildWindowPictureFromBgra(src, picture.width, picture.height, stride, w, h, picture.corner * g, false);
    out.corner = picture.corner * g;
    return out;
}

WindowPicture PlaceholderPicture(const IconImage& icon, int width, int height, bool dark)
{
    std::vector<uint8_t> px(size_t(width) * height * 4);
    const uint8_t tone = dark ? 0x2C : 0xF2;
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = tone;
        px[i * 4 + 3] = 255;
    }
    if (!icon.Empty()) {
        // The icon (premultiplied) over the panel, in the middle.
        const uint8_t* iconData = icon.Data();
        const int ox = (width - icon.width) / 2, oy = (height - icon.height) / 2;
        for (int y = 0; y < icon.height; ++y)
            for (int x = 0; x < icon.width; ++x) {
                const int dx = ox + x, dy = oy + y;
                if (dx < 0 || dy < 0 || dx >= width || dy >= height)
                    continue;
                const uint8_t* s = &iconData[(size_t(y) * icon.width + x) * 4];
                uint8_t* d = &px[(size_t(dy) * width + dx) * 4];
                const int inv = 255 - s[3];
                for (int c = 0; c < 3; ++c)
                    d[c] = uint8_t(std::min(255, s[c] + d[c] * inv / 255));
            }
    }
    return BuildWindowPictureFromBgra(px.data(), width, height, width * 4, width, height, float(std::max(6, width / 24)));
}

WindowSnapshots::WindowSnapshots(HWND notify, UINT message)
    : notify_(notify),
      message_(message),
      stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      doneEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      worker_([this] { Worker(); })
{
}

WindowSnapshots::~WindowSnapshots()
{
    SignalStop();
    if (worker_.joinable())
        worker_.join();
    if (stopEvent_)
        CloseHandle(stopEvent_);
    if (doneEvent_)
        CloseHandle(doneEvent_);
}

void WindowSnapshots::SignalStop()
{
    {
        std::lock_guard lk(lock_);
        stop_ = true;
        queue_.clear();
        shrinkQueue_.clear();
        pending_.clear();
    }
    if (stopEvent_)
        SetEvent(stopEvent_);
    cv_.notify_all();
}

bool WindowSnapshots::WaitStop(DWORD timeoutMs) const { return !doneEvent_ || WaitForSingleObject(doneEvent_, timeoutMs) == WAIT_OBJECT_0; }

void WindowSnapshots::Request(HWND window, int maxWidth, int maxHeight, float radius, int largeMax, bool urgent)
{
    // PrintWindow waits for the window's thread: a hung one would hold the worker and its bitmap for good.
    if (!window || IsIconic(window) || IsHungAppWindow(window))
        return;
    std::unique_lock lk(lock_);
    if (stop_ || !notify_)
        return;
    if (pending_.contains(window)) {
        if (!urgent)
            return;
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->window == window) {
                Job job = *it;
                job.maxWidth = maxWidth;
                job.maxHeight = maxHeight;
                job.largeMax = largeMax;
                job.radius = radius;
                queue_.erase(it);
                queue_.push_front(job);
                break;
            }
        }
        return;
    }
    if (!urgent && queue_.size() >= 32)
        return;
    if (urgent && queue_.size() >= 32) {
        pending_.erase(queue_.back().window);
        queue_.pop_back();
    }
    pending_.insert(window);
    const Job job{window, maxWidth, maxHeight, largeMax, radius};
    if (urgent)
        queue_.push_front(job);
    else
        queue_.push_back(job);
    lk.unlock();
    cv_.notify_one();
}

void WindowSnapshots::RequestShrink(HWND window, unsigned gen, std::shared_ptr<const WindowPicture> large, int largeMax)
{
    if (!window || gen == 0 || !large || !large->ok || largeMax <= 0)
        return;
    std::unique_lock lk(lock_);
    if (stop_ || !notify_)
        return;
    ShrinkJob job{window, gen, std::move(large), largeMax};
    auto it = std::ranges::find_if(shrinkQueue_, [window](const ShrinkJob& j) { return j.window == window; });
    if (it != shrinkQueue_.end())
        *it = std::move(job);
    else
        shrinkQueue_.push_back(std::move(job));
    lk.unlock();
    cv_.notify_one();
}

void WindowSnapshots::Worker()
{
    struct DoneSignal {
        HANDLE event = nullptr;
        explicit DoneSignal(HANDLE e) : event(e) {}
        DoneSignal(const DoneSignal&) = delete;
        DoneSignal& operator=(const DoneSignal&) = delete;
        ~DoneSignal()
        {
            if (event)
                SetEvent(event);
        }
    } doneSignal(doneEvent_);
    // Physical px of every monitor, as the window's own picture is.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
    std::unordered_set<HWND> loggedFailures;
    WindowFrameCapturer capturer;
    for (;;) {
        Job job{};
        ShrinkJob sjob{};
        bool isCapture = false;
        {
            std::unique_lock lk(lock_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty() || !shrinkQueue_.empty(); });
            if (stop_)
                break;
            if (!queue_.empty()) {
                isCapture = true;
                job = queue_.front();
                queue_.pop_front();
            } else {
                sjob = std::move(shrinkQueue_.front());
                shrinkQueue_.pop_front();
            }
        }
        if (isCapture) {
            WindowPicture large, p = Capture(job.window, job.maxWidth, job.maxHeight, job.radius, job.largeMax, &large, loggedFailures, capturer, stopEvent_);
            std::lock_guard g(lock_);
            pending_.erase(job.window);
            if (stop_ || !notify_)
                break;
            done_.push_back(Result{job.window, std::move(p), std::move(large)});
            PostMessageW(notify_, message_, 0, 0);
        } else {
            WindowPicture shrunk = Smaller(*sjob.large, sjob.largeMax);
            std::lock_guard g(lock_);
            if (stop_ || !notify_)
                break;
            done_.push_back(Result{sjob.window, {}, std::move(shrunk), sjob.gen});
            PostMessageW(notify_, message_, 0, 0);
        }
    }
    if (SUCCEEDED(roHr))
        RoUninitialize();
}

std::vector<WindowSnapshots::Result> WindowSnapshots::Take()
{
    std::lock_guard g(lock_);
    return std::exchange(done_, {});
}

} // namespace atrium
