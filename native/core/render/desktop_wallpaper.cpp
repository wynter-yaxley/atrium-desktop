#include "desktop_backdrop.h"

#include "../log.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace atrium {

namespace {

constexpr DWORD kWallpaperPollMs = 3000;

std::wstring TranscodedWallpaper()
{
    PWSTR roaming = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)))
        p = std::wstring(roaming) + L"\\Microsoft\\Windows\\Themes\\TranscodedWallpaper";
    CoTaskMemFree(roaming);
    return p;
}

std::wstring FileStamp(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return L"-";
    return std::to_wstring(a.ftLastWriteTime.dwLowDateTime) + L"." + std::to_wstring(a.nFileSizeLow);
}

struct WallpaperScreen {
    RECT rect;
    std::wstring path;
};

std::vector<WallpaperScreen> QueryWallpaperScreens(IDesktopWallpaper* dw)
{
    std::vector<WallpaperScreen> screens;
    UINT n = 0;
    dw->GetMonitorDevicePathCount(&n);
    for (UINT i = 0; i < n; ++i) {
        LPWSTR id = nullptr, path = nullptr;
        RECT r{};
        if (SUCCEEDED(dw->GetMonitorDevicePathAt(i, &id)) && SUCCEEDED(dw->GetMonitorRECT(id, &r)) && r.right > r.left) {
            dw->GetWallpaper(id, &path);
            std::wstring p = path ? path : L"";
            if (p.empty() || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
                p = TranscodedWallpaper(); // the picture Windows shows (Spotlight, a file moved away)
            screens.push_back({r, p});
        }
        CoTaskMemFree(id);
        CoTaskMemFree(path);
    }
    return screens;
}

std::wstring BuildWallpaperSignature(DESKTOP_WALLPAPER_POSITION pos, COLORREF bg, const RECT& virt, const std::vector<WallpaperScreen>& screens)
{
    std::wstring sig = std::to_wstring(int(pos)) + L"|" + std::to_wstring(bg) + L"|" + std::to_wstring(virt.left) + L"," + std::to_wstring(virt.top) + L"," +
                       std::to_wstring(virt.right) + L"," + std::to_wstring(virt.bottom);
    for (const WallpaperScreen& s : screens)
        sig += L"|" + std::to_wstring(s.rect.left) + L"," + std::to_wstring(s.rect.top) + L"," + std::to_wstring(s.rect.right) + L"," +
               std::to_wstring(s.rect.bottom) + L":" + s.path + L"@" + FileStamp(s.path);
    return sig;
}

// One monitor's wallpaper as Windows lays it out, drawn into `canvas` (BGRA, width x height) at
// `target` (canvas px). Explorer's monitor rects can lag a display change: what falls off the canvas is left out.
bool Place(IWICImagingFactory* wic, const std::wstring& path, RECT target, DESKTOP_WALLPAPER_POSITION pos, uint8_t* canvas, int width, int height)
{
    const int stride = width * 4;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT iw = 0, ih = 0;
    converter->GetSize(&iw, &ih);
    int tw = target.right - target.left, th = target.bottom - target.top;
    if (!iw || !ih || tw <= 0 || th <= 0)
        return false;
    double sx = 1, sy = 1;
    switch (pos) {
    case DWPOS_FILL:
    case DWPOS_SPAN: sx = sy = std::max(double(tw) / iw, double(th) / ih); break;
    case DWPOS_FIT: sx = sy = std::min(double(tw) / iw, double(th) / ih); break;
    case DWPOS_STRETCH:
        sx = double(tw) / iw;
        sy = double(th) / ih;
        break;
    default: // centre, tile: the picture's own size
        break;
    }
    UINT sw = std::max(1u, UINT(std::lround(iw * sx))), sh = std::max(1u, UINT(std::lround(ih * sy)));
    winrt::com_ptr<IWICBitmapSource> source = converter.as<IWICBitmapSource>();
    if (sw != iw || sh != ih) {
        winrt::com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(scaler.put())) || FAILED(scaler->Initialize(converter.get(), sw, sh, WICBitmapInterpolationModeHighQualityCubic)))
            return false;
        source = scaler.as<IWICBitmapSource>();
    }
    // Copies the picture placed at (dx, dy) of the target, clipped to it.
    auto blit = [&](int dx, int dy) {
        int x0 = std::max({0, dx, int(-target.left)}), y0 = std::max({0, dy, int(-target.top)});
        int x1 = std::min({tw, dx + int(sw), width - int(target.left)}), y1 = std::min({th, dy + int(sh), height - int(target.top)});
        if (x1 <= x0 || y1 <= y0)
            return true;
        WICRect rc{x0 - dx, y0 - dy, x1 - x0, y1 - y0};
        uint8_t* at = canvas + size_t(target.top + y0) * size_t(stride) + size_t(target.left + x0) * 4;
        return SUCCEEDED(source->CopyPixels(&rc, UINT(stride), UINT(size_t(stride) * size_t(y1 - y0 - 1) + size_t(x1 - x0) * 4), at));
    };
    if (pos == DWPOS_TILE) {
        for (int y = 0; y < th; y += int(sh))
            for (int x = 0; x < tw; x += int(sw))
                if (!blit(x, y))
                    return false;
        return true;
    }
    return blit((tw - int(sw)) / 2, (th - int(sh)) / 2);
}

// A picture WIC cannot read (a format without its codec, a slideshow's file mid-change): fall back
// to the copy Windows made of it for the desktop.
bool PlaceWithFallback(IWICImagingFactory* wic, const std::wstring& path, RECT target, DESKTOP_WALLPAPER_POSITION pos, uint8_t* canvas, int width, int height)
{
    if (Place(wic, path, target, pos, canvas, width, height))
        return true;
    const std::wstring transcoded = TranscodedWallpaper();
    return path != transcoded && Place(wic, transcoded, target, pos, canvas, width, height);
}

} // namespace

void DesktopBackdrop::UploadWallpaperPicture(std::unique_ptr<Picture> pic)
{
    if (!pic || pic->width <= 0 || pic->height <= 0 || pic->pixels.size() < size_t(pic->width) * size_t(pic->height) * 4)
        return;
    D3D11_TEXTURE2D_DESC d{};
    if (texture_)
        texture_->GetDesc(&d);
    if (!texture_ || int(d.Width) != pic->width || int(d.Height) != pic->height) {
        texture_ = nullptr;
        D3D11_TEXTURE2D_DESC nd{UINT(pic->width), UINT(pic->height), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        if (FAILED(gpu_.device->CreateTexture2D(&nd, nullptr, texture_.put())))
            return;
    }
    gpu_.context->UpdateSubresource(texture_.get(), 0, nullptr, pic->pixels.data(), UINT(pic->width * 4), 0);
    origin_ = pic->origin;
    // Release the temporary CPU buffer immediately after GPU upload; device recovery recreates
    // DesktopBackdrop and recomposes from disk if needed.
    pic.reset();
    ++version_;
    Changed(RECT{LONG_MIN / 2, LONG_MIN / 2, LONG_MAX / 2, LONG_MAX / 2}); // a new wallpaper: everywhere
}

DesktopBackdrop::Wallpaper::Wallpaper(HANDLE frameEvent) : stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)), done(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (frameEvent)
        DuplicateHandle(GetCurrentProcess(), frameEvent, GetCurrentProcess(), &wake, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

DesktopBackdrop::Wallpaper::~Wallpaper()
{
    for (HANDLE h : {stop, done, wake})
        if (h)
            CloseHandle(h);
}

void DesktopBackdrop::StartWallpaper()
{
    ReclaimRetired();
    if (wallpaper_ || !retired_.empty())
        return;
    static std::mutex workLock;
    static std::unordered_map<void*, std::shared_ptr<Wallpaper>> jobs;
    auto work = std::make_shared<Wallpaper>(frameEvent_);
    {
        std::lock_guard g(workLock);
        jobs.emplace(work.get(), work);
    }
    auto callback = [](PTP_CALLBACK_INSTANCE, PVOID ctx) {
        std::shared_ptr<Wallpaper> local;
        {
            std::lock_guard g(workLock);
            if (auto it = jobs.find(ctx); it != jobs.end()) {
                local = std::move(it->second);
                jobs.erase(it);
            }
        }
        if (!local)
            return;
        WallpaperLoop(*local);
        local->finished.store(true, std::memory_order_release);
        if (local->done)
            SetEvent(local->done);
        if (local->wake)
            SetEvent(local->wake);
    };
    if (!TrySubmitThreadpoolCallback(callback, work.get(), nullptr)) {
        std::lock_guard g(workLock);
        jobs.erase(work.get());
        return;
    }
    wallpaper_ = std::move(work);
    Log(L"render: widgets' backdrop: the wallpaper");
}

std::unique_ptr<DesktopBackdrop::Picture> DesktopBackdrop::TakePicture()
{
    if (!wallpaper_)
        return nullptr;
    std::lock_guard g(wallpaper_->lock);
    return std::move(wallpaper_->picture);
}

void DesktopBackdrop::StopWallpaper()
{
    ReclaimRetired();
    if (!wallpaper_)
        return;
    if (wallpaper_->stop)
        SetEvent(wallpaper_->stop);
    {
        std::lock_guard g(wallpaper_->lock);
        wallpaper_->picture.reset();
    }
    retired_.push_back(std::move(wallpaper_));
}

std::unique_ptr<DesktopBackdrop::Picture> DesktopBackdrop::ComposeWallpaper(const Wallpaper& work, std::wstring& composed, bool& composedOnce)
{
    winrt::com_ptr<IDesktopWallpaper> dw;
    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(dw.put()))) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))))
        return nullptr;
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    COLORREF bg = 0;
    dw->GetPosition(&pos);
    dw->GetBackgroundColor(&bg);
    const std::vector<WallpaperScreen> screens = QueryWallpaperScreens(dw.get());
    RECT virt{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    virt.right = virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virt.bottom = virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const std::wstring sig = BuildWallpaperSignature(pos, bg, virt, screens);
    if (sig == composed)
        return nullptr;
    auto pic = std::make_unique<Picture>();
    pic->width = virt.right - virt.left;
    pic->height = virt.bottom - virt.top;
    pic->origin = POINT{virt.left, virt.top};
    if (pic->width <= 0 || pic->height <= 0)
        return nullptr;
    if (work.stop && WaitForSingleObject(work.stop, 0) == WAIT_OBJECT_0)
        return nullptr;
    pic->pixels.resize(size_t(pic->width) * size_t(pic->height) * 4);
    for (size_t i = 0; i < pic->pixels.size(); i += 4) {
        pic->pixels[i] = GetBValue(bg);
        pic->pixels[i + 1] = GetGValue(bg);
        pic->pixels[i + 2] = GetRValue(bg);
        pic->pixels[i + 3] = 255;
    }
    bool ok = true;
    auto local = [&](RECT r) { return RECT{r.left - virt.left, r.top - virt.top, r.right - virt.left, r.bottom - virt.top}; };
    if (pos == DWPOS_SPAN && !screens.empty()) {
        ok = PlaceWithFallback(wic.get(), screens[0].path, local(virt), pos, pic->pixels.data(), pic->width, pic->height);
    } else {
        for (const WallpaperScreen& s : screens) {
            if (work.stop && WaitForSingleObject(work.stop, 0) == WAIT_OBJECT_0)
                return nullptr;
            ok &= PlaceWithFallback(wic.get(), s.path, local(s.rect), pos, pic->pixels.data(), pic->width, pic->height);
        }
    }
    if (!ok && composedOnce) {
        Log(L"render: a wallpaper could not be read; the last picture stays");
        composed = sig;
        return nullptr;
    }
    for (size_t i = 3; i < pic->pixels.size(); i += 4)
        pic->pixels[i] = 255; // opaque, as the desktop is
    composed = sig;
    composedOnce = true;
    Log(L"render: wallpaper composed %dx%d (%zu screens, position %d)%s", pic->width, pic->height, screens.size(), int(pos),
        ok ? L"" : L", a picture could not be read");
    return pic;
}

void DesktopBackdrop::WallpaperLoop(Wallpaper& work)
{
    SetThreadDescription(GetCurrentThread(), L"Atrium wallpaper");
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return;
    std::wstring composed; // what the last picture was made of
    bool composedOnce = false;
    do {
        if (std::unique_ptr<Picture> pic = ComposeWallpaper(work, composed, composedOnce)) {
            {
                std::lock_guard g(work.lock);
                work.picture = std::move(pic);
            }
            if (work.wake)
                SetEvent(work.wake);
        }
    } while (work.stop && WaitForSingleObject(work.stop, kWallpaperPollMs) == WAIT_TIMEOUT);
    CoUninitialize();
}

void WallpaperCapture::SetPaused(bool paused)
{
    if (paused == paused_)
        return;
    paused_ = paused;
    if (paused_) {
        wallpaper_.Update(L"off");
        version_ = 0;
    }
}

ID3D11Texture2D* WallpaperCapture::Acquire()
{
    if (paused_)
        return nullptr;
    wallpaper_.Update(mode_);
    ID3D11Texture2D* picture = wallpaper_.Texture();
    if (!picture || wallpaper_.Version() == version_)
        return nullptr;
    version_ = wallpaper_.Version();
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor_, &mi))
        return nullptr;
    const RECT m = mi.rcMonitor;
    const UINT w = UINT(m.right - m.left), h = UINT(m.bottom - m.top);
    D3D11_TEXTURE2D_DESC pd{}, have{};
    picture->GetDesc(&pd);
    if (texture_)
        texture_->GetDesc(&have);
    if (!texture_ || have.Width != w || have.Height != h || have.Format != pd.Format) {
        texture_ = nullptr;
        D3D11_TEXTURE2D_DESC d{w, h, 1, 1, pd.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(gpu_.device->CreateTexture2D(&d, nullptr, texture_.put())))
            return nullptr;
    }
    // The monitor's part of the desktop's picture.
    const POINT o = wallpaper_.Origin();
    D3D11_BOX box{UINT(std::max(0L, m.left - o.x)), UINT(std::max(0L, m.top - o.y)), 0, 0, 0, 1};
    box.right = std::min(pd.Width, box.left + w);
    box.bottom = std::min(pd.Height, box.top + h);
    if (box.right <= box.left || box.bottom <= box.top)
        return nullptr;
    gpu_.context->CopySubresourceRegion(texture_.get(), 0, 0, 0, 0, picture, 0, &box);
    return texture_.get();
}

} // namespace atrium
