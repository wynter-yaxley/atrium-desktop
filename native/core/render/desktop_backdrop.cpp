#include "desktop_backdrop.h"

#include "desktop_backdrop_target.h"
#include "../log.h"
#include "../clock.h"

#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace atrium {

namespace {

constexpr double kRetryMs = 2000;   // the desktop window is gone (Explorer restarting)
constexpr double kTargetCheckMs = 2000;
constexpr double kFailedWorkerBackoffMs = 300000.0;

bool HasVisibleWallpaperChild(HWND parent)
{
    for (HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr); child; child = FindWindowExW(parent, child, nullptr, nullptr)) {
        if (!IsWindowVisible(child))
            continue;
        wchar_t cls[32] = {};
        if (GetClassNameW(child, cls, int(std::size(cls))) && wcscmp(cls, L"SHELLDLL_DefView") == 0)
            continue;
        RECT r{};
        if (GetWindowRect(child, &r) && (r.right - r.left) >= 64 && (r.bottom - r.top) >= 64)
            return true;
    }
    return false;
}

DesktopBackdropTarget ResolveDesktopBackdropTarget(HWND skipWorker)
{
    std::vector<DesktopHostCandidate> candidates;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            wchar_t cls[32] = {};
            if (!GetClassNameW(h, cls, int(std::size(cls))))
                return TRUE;
            const bool isProgman = wcscmp(cls, L"Progman") == 0;
            if (!isProgman && wcscmp(cls, L"WorkerW") != 0)
                return TRUE;
            DesktopHostCandidate c;
            c.hwnd = h;
            c.isProgman = isProgman;
            c.hasDefView = FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
            c.visible = IsWindowVisible(h) != FALSE;
            BOOL cloaked = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
                c.cloaked = cloaked != FALSE;
            GetWindowRect(h, &c.rect);
            if (!isProgman && !c.hasDefView && c.visible && !c.cloaked)
                c.hasVisibleChild = HasVisibleWallpaperChild(h);
            reinterpret_cast<std::vector<DesktopHostCandidate>*>(lp)->push_back(c);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&candidates));
    return SelectDesktopBackdropTarget(candidates, skipWorker);
}

bool ExtractDesktopFrame(wgc::Direct3D11CaptureFramePool& pool, const wgd::Direct3D11::IDirect3DDevice& device, HWND desktop,
                         winrt::Windows::Graphics::SizeInt32& size, POINT& origin, wgc::Direct3D11CaptureFrame& outFrame,
                         winrt::com_ptr<ID3D11Texture2D>& outTex, RECT& outDirty)
{
    wgc::Direct3D11CaptureFrame latest{nullptr};
    try {
        while (auto f = pool.TryGetNextFrame()) {
            if (latest)
                latest.Close();
            latest = f;
        }
        if (!latest)
            return false;
        const auto cs = latest.ContentSize();
        if (cs.Width <= 0 || cs.Height <= 0) {
            latest.Close();
            return false;
        }
        if (cs.Width != size.Width || cs.Height != size.Height) {
            pool.Recreate(device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, cs);
            size = cs; // the desktop changed size: later frames come at the new one
        }
        auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        if (FAILED(access->GetInterface(IID_PPV_ARGS(outTex.put())))) {
            latest.Close();
            return false;
        }
    } catch (const winrt::hresult_error& e) {
        if (latest)
            latest.Close();
        Log(L"render: desktop frame extraction failed (0x%08X)", static_cast<unsigned>(e.code()));
        return false;
    }
    RECT r{};
    GetWindowRect(desktop, &r);
    origin = POINT{r.left, r.top};
    // Where this frame differs from the last one; all of it when Windows cannot say.
    const RECT whole{r.left, r.top, r.left + size.Width, r.top + size.Height};
    outDirty = whole;
    try {
        auto regions = latest.DirtyRegions();
        if (regions.Size() > 0) {
            outDirty = RECT{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
            for (const auto& d : regions)
                outDirty = RECT{std::min(outDirty.left, LONG(r.left + d.X)), std::min(outDirty.top, LONG(r.top + d.Y)),
                                std::max(outDirty.right, LONG(r.left + d.X + d.Width)), std::max(outDirty.bottom, LONG(r.top + d.Y + d.Height))};
        }
    } catch (const winrt::hresult_error&) {
        outDirty = whole;
    }
    outFrame = std::move(latest);
    return true;
}

struct ApartmentGuard {
    ApartmentGuard() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~ApartmentGuard() { winrt::uninit_apartment(); }
    ApartmentGuard(const ApartmentGuard&) = delete;
    ApartmentGuard& operator=(const ApartmentGuard&) = delete;
};

void CloseWgcObjects(wgc::GraphicsCaptureItem& item, wgc::Direct3D11CaptureFramePool& pool, wgc::GraphicsCaptureSession& session)
{
    try {
        if (session)
            session.Close();
        if (pool)
            pool.Close();
    } catch (const winrt::hresult_error&) {
    }
    session = nullptr;
    pool = nullptr;
    item = nullptr;
}

} // namespace

DesktopBackdrop::DesktopBackdrop(const Gpu& gpu, HANDLE frameEvent) : gpu_(gpu), frameEvent_(frameEvent)
{
    auto dxgi = gpu.device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    device_ = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();
}

DesktopBackdrop::~DesktopBackdrop()
{
    StopCapture();
    StopWallpaper();
    std::vector<HANDLE> pending;
    pending.reserve(retired_.size() + retiredStarting_.size() + retiredCleanups_.size());
    for (const auto& r : retired_) {
        if (r->stop)
            SetEvent(r->stop);
        if (r->done && pending.size() < MAXIMUM_WAIT_OBJECTS)
            pending.push_back(r->done);
    }
    for (const auto& r : retiredStarting_) {
        r->stop.store(true, std::memory_order_release);
        {
            std::lock_guard g(r->lock);
            r->abandoned = true;
        }
        if (r->doneEvent && pending.size() < MAXIMUM_WAIT_OBJECTS)
            pending.push_back(r->doneEvent);
    }
    for (const auto& cleanup : retiredCleanups_)
        if (cleanup && cleanup->DoneEvent() && pending.size() < MAXIMUM_WAIT_OBJECTS)
            pending.push_back(cleanup->DoneEvent());
    WaitBoundedHandles(pending, 250);
    // Bounded wait above prevents blocking render/shutdown if Windows hangs in #66 or Explorer.
    // Threadpool callbacks still retain their own shared_ptr until completion.
    ReclaimRetired();
    retired_.clear();
    retiredStarting_.clear();
    retiredCleanups_.clear();
}

void DesktopBackdrop::Update(const std::wstring& mode)
{
    ReclaimRetired();
    const std::wstring want = mode == L"wallpaper" || mode == L"off" ? mode : L"desktop";
    if (want != mode_) {
        StopCapture();
        StopWallpaper();
        if (mode_ == L"off" || want == L"off" || want == L"wallpaper")
            texture_ = nullptr; // not a picture of what comes next
        mode_ = want;
        retryMs_ = 0;
    }
    if (mode_ == L"wallpaper" && !wallpaper_)
        StartWallpaper();
    if (mode_ == L"desktop")
        UpdateDesktopMode();
    if (std::unique_ptr<Picture> pic = TakePicture(); pic && (mode_ == L"wallpaper" || !session_))
        UploadWallpaperPicture(std::move(pic));
}

void DesktopBackdrop::UpdateDesktopMode()
{
    CheckCaptureTarget();
    if (session_ && (CaptureClosed() || !IsWindow(desktop_))) {
        Log(L"render: the desktop window went away; capturing it again soon");
        StopCapture();
        retryMs_ = ClockMs() + kRetryMs;
    }
    if (!session_ && (starting_ || ClockMs() >= retryMs_)) {
        const bool wasStarting = starting_ != nullptr;
        if (StartCapture()) {
            StopWallpaper();
        } else {
            if (wasStarting && !starting_ && retryMs_ < ClockMs())
                retryMs_ = ClockMs() + kRetryMs; // it failed: again in a while
            if (!wallpaper_)
                StartWallpaper(); // meanwhile, the wallpaper files
        }
    }
    if (session_) {
        wgc::Direct3D11CaptureFrame frame{nullptr};
        winrt::com_ptr<ID3D11Texture2D> tex;
        RECT dirty{};
        if (ExtractDesktopFrame(pool_, device_, desktop_, size_, origin_, frame, tex, dirty)) {
            Store(tex.get());
            Changed(dirty);
            frame.Close();
        }
    }
}

void DesktopBackdrop::Changed(const RECT& rect)
{
    changes_.push_back({version_, rect});
    if (changes_.size() > 32)
        changes_.erase(changes_.begin());
}

bool DesktopBackdrop::ChangedSince(unsigned version, const RECT& area) const
{
    if (version == version_)
        return false;
    // Pictures older than what is kept: assume they changed it.
    if (changes_.empty() || changes_.front().version > version + 1)
        return true;
    RECT overlap;
    for (const Change& c : changes_)
        if (c.version > version && IntersectRect(&overlap, &c.rect, &area))
            return true;
    return false;
}

DesktopBackdrop::ClosedState::ClosedState(HANDLE frameEvent)
{
    if (frameEvent)
        DuplicateHandle(GetCurrentProcess(), frameEvent, GetCurrentProcess(), &wake, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

DesktopBackdrop::ClosedState::~ClosedState()
{
    if (wake)
        CloseHandle(wake);
}

DesktopBackdrop::Starting::Starting(HANDLE frameEvent) : doneEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (frameEvent)
        DuplicateHandle(GetCurrentProcess(), frameEvent, GetCurrentProcess(), &wake, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

DesktopBackdrop::Starting::~Starting()
{
    if (wake)
        CloseHandle(wake);
    if (doneEvent)
        CloseHandle(doneEvent);
}

void DesktopBackdrop::CheckCaptureTarget()
{
    if (!session_)
        return;
    const double now = ClockMs();
    if (now < targetCheckMs_)
        return;
    targetCheckMs_ = now + kTargetCheckMs;
    const HWND skip = now < failedWorkerUntilMs_ ? failedWorker_ : nullptr;
    const DesktopBackdropTarget want = ResolveDesktopBackdropTarget(skip);
    if (!want.hwnd || want.hwnd == desktop_)
        return;
    Log(L"render: widgets' backdrop target changed to %s; switching capture", want.isProgman ? L"Progman" : L"WorkerW");
    StopCapture();
    retryMs_ = 0;
}

void DesktopBackdrop::RunCaptureStart(const std::shared_ptr<Starting>& start, const wgd::Direct3D11::IDirect3DDevice& device)
{
    ApartmentGuard apartment;
    Starting s;
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(start->desktop, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(s.item)));
        s.size = s.item.Size();
        if (s.size.Width <= 0 || s.size.Height <= 0) {
            s.error = E_INVALIDARG;
        } else if (!start->stop.load(std::memory_order_acquire)) {
            s.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, s.size);
            s.pool.FrameArrived([start](auto&&, auto&&) {
                if (!start->stop.load(std::memory_order_acquire) && start->wake)
                    SetEvent(start->wake);
            });
            s.session = s.pool.CreateCaptureSession(s.item);
            s.session.IsCursorCaptureEnabled(false);
            try {
                s.session.IsBorderRequired(false);
            } catch (const winrt::hresult_error& e) {
                Log(L"render: the desktop capture keeps its border (0x%08X)", static_cast<unsigned>(e.code()));
            }
            if (!start->stop.load(std::memory_order_acquire)) {
                s.session.StartCapture();
                s.ok = true;
            }
        }
    } catch (const winrt::hresult_error& e) {
        s.error = e.code();
    }
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem closeItem{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool closePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession closeSession{nullptr};
    {
        std::lock_guard g(start->lock);
        if (start->abandoned || !s.ok) { // too late or failed: release local WGC objects outside lock
            closeItem = std::move(s.item);
            closeSession = std::move(s.session);
            closePool = std::move(s.pool);
            start->ok = false;
            start->error = s.error;
        } else {
            start->item = std::move(s.item);
            start->pool = std::move(s.pool);
            start->session = std::move(s.session);
            start->size = s.size;
            start->ok = true;
            start->error = S_OK;
        }
        start->done = true;
    }
    CloseWgcObjects(closeItem, closePool, closeSession);
    start->finished.store(true, std::memory_order_release);
    if (start->doneEvent)
        SetEvent(start->doneEvent);
    if (start->wake)
        SetEvent(start->wake); // the render thread looks again
}

bool DesktopBackdrop::SubmitCaptureStart(const std::shared_ptr<Starting>& start, const wgd::Direct3D11::IDirect3DDevice& device)
{
    auto startSlot = AsyncWorkSlot::Create();
    if (!startSlot)
        return false;
    startSlot->Retire([start, device] { RunCaptureStart(start, device); });
    return true;
}

void DesktopBackdrop::RetireCaptureObjects(std::shared_ptr<AsyncWorkSlot> slot, wgc::GraphicsCaptureItem item, wgc::Direct3D11CaptureFramePool pool,
                                           wgc::GraphicsCaptureSession session)
{
    if (!item && !pool && !session)
        return;
    if (!slot)
        slot = AsyncWorkSlot::Create();
    if (!slot) {
        CloseWgcObjects(item, pool, session);
        return;
    }
    auto sharedItem = std::make_shared<wgc::GraphicsCaptureItem>(std::move(item));
    auto sharedPool = std::make_shared<wgc::Direct3D11CaptureFramePool>(std::move(pool));
    auto sharedSession = std::make_shared<wgc::GraphicsCaptureSession>(std::move(session));
    slot->Retire([sharedItem, sharedPool, sharedSession] {
        ApartmentGuard apartment;
        CloseWgcObjects(*sharedItem, *sharedPool, *sharedSession);
    });
    retiredCleanups_.push_back(std::move(slot));
}

bool DesktopBackdrop::StartCapture()
{
    ReclaimRetired();
    if (!starting_) {
        const double now = ClockMs();
        const HWND skip = now < failedWorkerUntilMs_ ? failedWorker_ : nullptr;
        const DesktopBackdropTarget target = ResolveDesktopBackdropTarget(skip);
        if (!target.hwnd)
            return false;
        auto retirement = AsyncWorkSlot::Create();
        if (!retirement)
            return false;
        auto start = std::make_shared<Starting>(frameEvent_);
        start->desktop = target.hwnd;
        start->isProgman = target.isProgman;
        start->retirement = std::move(retirement);
        if (!SubmitCaptureStart(start, device_))
            return false;
        starting_ = start;
        startingSince_ = now;
        return false;
    }
    std::shared_ptr<Starting> start = starting_;
    bool isDone = false;
    bool timedOut = false;
    {
        std::lock_guard g(start->lock);
        isDone = start->done;
        if (!isDone && ClockMs() - startingSince_ > 5000) {
            start->abandoned = true;
            start->stop.store(true, std::memory_order_release);
            timedOut = true;
        }
    }
    if (!isDone) {
        if (timedOut) {
            const double now = ClockMs();
            if (!start->finished.load(std::memory_order_acquire))
                retiredStarting_.push_back(starting_);
            starting_.reset();
            if (!start->isProgman) {
                failedWorker_ = start->desktop;
                failedWorkerUntilMs_ = now + kFailedWorkerBackoffMs;
                Log(L"render: live-wallpaper WorkerW capture did not start in 5 s; falling back to Progman");
            } else {
                Log(L"render: the desktop window's capture did not start in 5 s; widgets use the wallpaper");
                retryMs_ = now + 300000.0; // not every few seconds: it may never start on this system
            }
        }
        return false;
    }
    starting_.reset();
    if (!start->ok) {
        if (!start->isProgman) {
            failedWorker_ = start->desktop;
            failedWorkerUntilMs_ = ClockMs() + kFailedWorkerBackoffMs;
            Log(L"render: live-wallpaper WorkerW cannot be captured (0x%08X); falling back to Progman", static_cast<unsigned>(start->error));
        } else {
            Log(L"render: the desktop window cannot be captured (0x%08X); widgets use the wallpaper", static_cast<unsigned>(start->error));
        }
        return false;
    }
    desktop_ = start->desktop;
    item_ = std::move(start->item);
    pool_ = std::move(start->pool);
    session_ = std::move(start->session);
    activeRetirement_ = std::move(start->retirement);
    size_ = start->size;
    targetCheckMs_ = ClockMs() + kTargetCheckMs;
    closedState_ = std::make_shared<ClosedState>(frameEvent_);
    closedRevoker_ = item_.Closed(winrt::auto_revoke, [state = closedState_](auto&&, auto&&) {
        state->closed.store(true, std::memory_order_release);
        if (state->wake)
            SetEvent(state->wake);
    });
    Log(L"render: widgets' backdrop: %s, %dx%d", start->isProgman ? L"Progman" : L"WorkerW", size_.Width, size_.Height);
    return true;
}

void DesktopBackdrop::StopCapture()
{
    if (starting_) { // still starting: dropped when it is done, or release adopted WGC objects outside lock
        starting_->stop.store(true, std::memory_order_release);
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem abandonItem{nullptr};
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool abandonPool{nullptr};
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession abandonSession{nullptr};
        std::shared_ptr<AsyncWorkSlot> abandonSlot;
        {
            std::lock_guard g(starting_->lock);
            starting_->abandoned = true;
            abandonItem = std::move(starting_->item);
            abandonSession = std::move(starting_->session);
            abandonPool = std::move(starting_->pool); // its handler holds `starting_`
            abandonSlot = std::move(starting_->retirement);
        }
        RetireCaptureObjects(std::move(abandonSlot), std::move(abandonItem), std::move(abandonPool), std::move(abandonSession));
        if (!starting_->finished.load(std::memory_order_acquire))
            retiredStarting_.push_back(starting_);
        starting_.reset();
    }
    desktop_ = nullptr;
    closedRevoker_.revoke();
    closedState_.reset();
    RetireCaptureObjects(std::move(activeRetirement_), std::move(item_), std::move(pool_), std::move(session_));
    activeRetirement_.reset();
    session_ = nullptr;
    pool_ = nullptr;
    item_ = nullptr;
}

void DesktopBackdrop::Store(ID3D11Texture2D* frame)
{
    D3D11_TEXTURE2D_DESC fd{}, have{};
    frame->GetDesc(&fd);
    UINT w = std::min(fd.Width, UINT(size_.Width)), h = std::min(fd.Height, UINT(size_.Height));
    if (texture_)
        texture_->GetDesc(&have);
    if (!texture_ || have.Width != w || have.Height != h) {
        texture_ = nullptr;
        D3D11_TEXTURE2D_DESC d{w, h, 1, 1, fd.Format, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        if (FAILED(gpu_.device->CreateTexture2D(&d, nullptr, texture_.put())))
            return;
    }
    D3D11_BOX box{0, 0, 0, w, h, 1};
    gpu_.context->CopySubresourceRegion(texture_.get(), 0, 0, 0, 0, frame, 0, &box);
    ++version_;
}

void DesktopBackdrop::ReclaimRetired()
{
    for (auto it = retired_.begin(); it != retired_.end();) {
        const auto& state = *it;
        if (state->finished.load(std::memory_order_acquire) || (state->done && WaitForSingleObject(state->done, 0) == WAIT_OBJECT_0))
            it = retired_.erase(it);
        else
            ++it;
    }
    for (auto it = retiredStarting_.begin(); it != retiredStarting_.end();) {
        const auto& state = *it;
        if (state->finished.load(std::memory_order_acquire) || (state->doneEvent && WaitForSingleObject(state->doneEvent, 0) == WAIT_OBJECT_0))
            it = retiredStarting_.erase(it);
        else
            ++it;
    }
    for (auto it = retiredCleanups_.begin(); it != retiredCleanups_.end();) {
        const auto& cleanup = *it;
        if (cleanup->Finished() || (cleanup->DoneEvent() && WaitForSingleObject(cleanup->DoneEvent(), 0) == WAIT_OBJECT_0))
            it = retiredCleanups_.erase(it);
        else
            ++it;
    }
}

} // namespace atrium
