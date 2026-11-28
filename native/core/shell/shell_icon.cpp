// Shell item and executable icon loading.

#include "shell/shell_icon.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace atrium {

namespace {

// 32bpp top-down BGRA copy of `bmp`, as the driver stores it (alpha untouched).
bool ReadBits(HBITMAP bmp, int& w, int& h, std::vector<uint8_t>& out)
{
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return false;
    w = bm.bmWidth;
    h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign(size_t(w) * size_t(h) * 4, 0);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, bmp, 0, static_cast<UINT>(h), out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == h;
}

// Makes `px` premultiplied. A bitmap without any alpha is opaque. Where alpha
// exists, a colour channel above alpha cannot be premultiplied data, so the
// bitmap holds straight alpha and gets premultiplied here.
void MakePremultiplied(std::vector<uint8_t>& px)
{
    bool anyAlpha = false, straight = false;
    for (size_t i = 0; i < px.size(); i += 4) {
        uint8_t a = px[i + 3];
        anyAlpha |= a != 0;
        straight |= px[i] > a || px[i + 1] > a || px[i + 2] > a;
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < px.size(); i += 4)
            px[i] = 255;
    } else if (straight) {
        for (size_t i = 0; i < px.size(); i += 4) {
            unsigned a = px[i + 3];
            for (size_t c = 0; c < 3; ++c)
                px[i + c] = static_cast<uint8_t>((px[i + c] * a + 127) / 255);
        }
    }
}

bool BitmapToImage(HBITMAP bmp, IconImage& out)
{
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
    if (!ReadBits(bmp, w, h, bgra))
        return false;
    MakePremultiplied(bgra);
    out = IconImage::FromBgra(w, h, std::move(bgra));
    return true;
}

// IShellItemImageFactory on a parsing name (path, .lnk or shell:AppsFolder\...).
bool ShellImage(const std::wstring& parsingName, int size, IconImage& out, bool thumbnail)
{
    winrt::com_ptr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(factory.put()))) || !factory)
        return false;
    const SIIGBF icons[] = {SIIGBF_ICONONLY, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, SIIGBF_BIGGERSIZEOK};
    const SIIGBF pictures[] = {SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, SIIGBF_RESIZETOFIT, SIIGBF_BIGGERSIZEOK};
    for (SIIGBF flags : thumbnail ? pictures : icons) {
        HBITMAP bmp = nullptr;
        if (SUCCEEDED(factory->GetImage(SIZE{size, size}, flags, &bmp)) && bmp) {
            IconImage raw;
            bool ok = BitmapToImage(bmp, raw);
            DeleteObject(bmp);
            if (ok) {
                out = FitToSquare(raw, size);
                return true;
            }
        }
    }
    return false;
}

// Monochrome icon: double-height mask (top half = AND, bottom half = XOR).
bool MonoIconToImage(HBITMAP maskBmp, IconImage& out)
{
    std::vector<uint8_t> mask;
    int mw = 0, mh = 0;
    if (!ReadBits(maskBmp, mw, mh, mask) || mw <= 0 || mh < 2 || (mh % 2) != 0)
        return false;
    const int h = mh / 2;
    const size_t plane = size_t(mw) * size_t(h) * 4;
    std::vector<uint8_t> bgra(plane, 0);
    for (size_t i = 0; i < plane; i += 4) {
        const bool andBit = (mask[i] | mask[i + 1] | mask[i + 2]) != 0;
        const bool xorBit = (mask[plane + i] | mask[plane + i + 1] | mask[plane + i + 2]) != 0;
        if (!andBit) {
            std::copy_n(&mask[plane + i], 3, &bgra[i]);
            bgra[i + 3] = 255;
        } else if (xorBit) {
            std::fill_n(&bgra[i], 4, uint8_t(255));
        }
    }
    out = IconImage::FromBgra(mw, h, std::move(bgra));
    return true;
}

} // namespace

bool HIconToImage(HICON icon, IconImage& out)
{
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii))
        return false;
    bool ok = false;
    if (ii.hbmColor) {
        std::vector<uint8_t> color;
        int w = 0, h = 0;
        if (ReadBits(ii.hbmColor, w, h, color)) {
            bool hasAlpha = false;
            for (size_t i = 3; i < color.size() && !hasAlpha; i += 4)
                hasAlpha = color[i] != 0;
            if (hasAlpha) {
                MakePremultiplied(color);
            } else {
                // Old-style icon: transparency is in the AND mask (black = opaque).
                std::vector<uint8_t> mask;
                int mw = 0, mh = 0;
                bool haveMask = ii.hbmMask && ReadBits(ii.hbmMask, mw, mh, mask) && mw == w && mh == h;
                for (size_t i = 0; i < color.size(); i += 4) {
                    bool opaque = !haveMask || mask[i] == 0;
                    color[i + 3] = opaque ? 255 : 0;
                    if (!opaque)
                        color[i] = color[i + 1] = color[i + 2] = 0;
                }
            }
            out = IconImage::FromBgra(w, h, std::move(color));
            ok = true;
        }
    } else if (ii.hbmMask) {
        ok = MonoIconToImage(ii.hbmMask, out);
    }
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
    return ok;
}

namespace {

struct SquircleProfile {
    float reach = 0.0f;
    float a = 0.02f;
};

SquircleProfile SquircleFor(float width, float height, float radius)
{
    const float half = std::max(0.0f, std::min(width, height) * 0.5f);
    radius = std::clamp(radius, 0.0f, half);
    const float reach = std::min(radius * 1.6f, half);
    if (reach <= 0.0f)
        return {};
    const float v = 1.0f - 0.2928932188134525f * radius / reach;
    const float v2 = v * v;
    if (width == height && radius == half)
        return {reach, 0.0f};
    return {reach, std::max(0.02f, (2.0f * v2 * v2 - v2) / (1.0f - v2))};
}

float SquircleF(float t, float a)
{
    const float t2 = t * t;
    return t2 * t2 / (a + (1.0f - a) * t2);
}

float SquircleDF(float t, float a)
{
    const float t2 = t * t;
    const float den = a + (1.0f - a) * t2;
    return 2.0f * t * t2 * (2.0f * a + (1.0f - a) * t2) / (den * den);
}

float SquircleEdge(float qx, float qy, SquircleProfile p)
{
    if (p.reach <= 0.0f || std::min(qx, qy) <= 0.0f)
        return std::max(qx, qy) - p.reach;
    const float x = qx / p.reach;
    const float y = qy / p.reach;
    const float f = SquircleF(x, p.a) + SquircleF(y, p.a) - 1.0f;
    return f * p.reach / std::max(std::hypot(SquircleDF(x, p.a), SquircleDF(y, p.a)), 1e-12f);
}

float SquircleCoverage(float x, float y, float width, float height, SquircleProfile p)
{
    const float qx = std::abs(x - width * 0.5f) - width * 0.5f + p.reach;
    const float qy = std::abs(y - height * 0.5f) - height * 0.5f + p.reach;
    return std::clamp(0.5f - SquircleEdge(qx, qy, p), 0.0f, 1.0f);
}

void ResampleRect(const IconImage& src, int sx0, int sy0, int sw, int sh, std::vector<uint8_t>& outBgra, int outWidth, int ox, int oy, int dw, int dh)
{
    const uint8_t* srcData = src.Data();
    auto at = [&](int x, int y, int c) {
        x = std::clamp(x, sx0, sx0 + sw - 1);
        y = std::clamp(y, sy0, sy0 + sh - 1);
        return double(srcData[(size_t(y) * size_t(src.width) + size_t(x)) * 4 + size_t(c)]);
    };
    for (int dy = 0; dy < dh; ++dy) {
        double y0 = sy0 + double(dy) * sh / dh, y1 = sy0 + double(dy + 1) * sh / dh;
        for (int dx = 0; dx < dw; ++dx) {
            double x0 = sx0 + double(dx) * sw / dw, x1 = sx0 + double(dx + 1) * sw / dw;
            uint8_t* d = &outBgra[(size_t(dy + oy) * size_t(outWidth) + size_t(dx + ox)) * 4];
            for (int c = 0; c < 4; ++c) {
                double v = 0;
                if (x1 - x0 <= 1.0 && y1 - y0 <= 1.0) {
                    // Upscale: bilinear around the pixel centre.
                    double fx = (x0 + x1) / 2 - 0.5, fy = (y0 + y1) / 2 - 0.5;
                    int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                    double tx = fx - ix, ty = fy - iy;
                    v = at(ix, iy, c) * (1 - tx) * (1 - ty) + at(ix + 1, iy, c) * tx * (1 - ty) + at(ix, iy + 1, c) * (1 - tx) * ty +
                        at(ix + 1, iy + 1, c) * tx * ty;
                } else {
                    // Downscale: area average with fractional edge weights.
                    double sum = 0, weight = 0;
                    for (int sy = int(std::floor(y0)); sy < int(std::ceil(y1)); ++sy) {
                        double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                        for (int sx = int(std::floor(x0)); sx < int(std::ceil(x1)); ++sx) {
                            double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                            sum += at(sx, sy, c) * wx * wy;
                            weight += wx * wy;
                        }
                    }
                    v = weight > 0 ? sum / weight : 0;
                }
                d[c] = static_cast<uint8_t>(std::clamp(int(std::lround(v)), 0, 255));
            }
        }
    }
}

} // namespace

IconImage FitToSquare(const IconImage& src, int size)
{
    if (src.Empty() || size <= 0)
        return {};
    if (src.width == size && src.height == size)
        return src;
    std::vector<uint8_t> out(size_t(size) * size_t(size) * 4, 0);
    double scale = double(size) / double(std::max(src.width, src.height));
    int dw = std::max(1, int(std::lround(src.width * scale)));
    int dh = std::max(1, int(std::lround(src.height * scale)));
    int ox = (size - dw) / 2, oy = (size - dh) / 2;
    ResampleRect(src, 0, 0, src.width, src.height, out, size, ox, oy, dw, dh);
    return IconImage::FromBgra(size, size, std::move(out));
}

IconImage CropToSquare(const IconImage& src, int size)
{
    if (src.Empty() || size <= 0)
        return {};
    if (src.width == size && src.height == size)
        return src;
    std::vector<uint8_t> out(size_t(size) * size_t(size) * 4, 0);
    const int side = std::min(src.width, src.height);
    const int sx0 = (src.width - side) / 2;
    const int sy0 = (src.height - side) / 2;
    ResampleRect(src, sx0, sy0, side, side, out, size, 0, 0, size, size);
    return IconImage::FromBgra(size, size, std::move(out));
}

bool DecodeWicSquareImage(IWICImagingFactory* wic, std::vector<uint8_t>& bytes, int targetSize, IconImage& out)
{
    constexpr size_t kMaxBytes = 16u * 1024u * 1024u;
    if (!wic || bytes.empty() || bytes.size() > kMaxBytes || targetSize <= 0)
        return false;
    winrt::com_ptr<IWICStream> wicStream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateStream(wicStream.put())) || !wicStream || FAILED(wicStream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(wicStream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) || !decoder ||
        FAILED(decoder->GetFrame(0, frame.put())) || !frame)
        return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 8192 || h > 8192)
        return false;

    // Pull pipeline: convert to 32bppPBGRA first (handles sub-byte indexed pixel formats and premultiplies before
    // filtering), then clip to a centred square, then scale directly to targetSize without decoding the full image.
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put())) || !converter ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    winrt::com_ptr<IWICBitmapSource> source = converter.as<IWICBitmapSource>();
    const UINT side = std::min(w, h);
    if (w != h) {
        winrt::com_ptr<IWICBitmapClipper> clipper;
        const WICRect rect{static_cast<INT>((w - side) / 2), static_cast<INT>((h - side) / 2), static_cast<INT>(side), static_cast<INT>(side)};
        if (FAILED(wic->CreateBitmapClipper(clipper.put())) || !clipper || FAILED(clipper->Initialize(source.get(), &rect)))
            return false;
        source = clipper.as<IWICBitmapSource>();
    }
    if (side != static_cast<UINT>(targetSize)) {
        winrt::com_ptr<IWICBitmapScaler> scaler;
        const auto mode = side > static_cast<UINT>(targetSize) ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeLinear;
        if (FAILED(wic->CreateBitmapScaler(scaler.put())) || !scaler ||
            FAILED(scaler->Initialize(source.get(), static_cast<UINT>(targetSize), static_cast<UINT>(targetSize), mode)))
            return false;
        source = scaler.as<IWICBitmapSource>();
    }
    const UINT stride = static_cast<UINT>(targetSize) * 4;
    std::vector<uint8_t> px(static_cast<size_t>(stride) * static_cast<size_t>(targetSize));
    if (FAILED(source->CopyPixels(nullptr, stride, static_cast<UINT>(px.size()), px.data())))
        return false;
    out = IconImage::FromBgra(targetSize, targetSize, std::move(px));
    return !out.Empty();
}

bool LoadPictureIcon(const std::wstring& path, int sizePx, IconImage& out)
{
    if (path.empty() || sizePx <= 0)
        return false;
    std::wstring file = path;
    if (file.find(L'%') != std::wstring::npos) {
        std::wstring expanded(32768, L'\0');
        const DWORD n = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), DWORD(expanded.size()));
        if (n > 0 && n <= expanded.size()) {
            expanded.resize(n - 1); // the length includes the terminating NUL
            file = std::move(expanded);
        }
    }
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) || !wic ||
        FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) || !decoder)
        return false;
    // An icon file holds the same picture at several sizes: the largest is the one worth drawing.
    UINT frames = 0;
    if (FAILED(decoder->GetFrameCount(&frames)) || frames == 0)
        return false;
    winrt::com_ptr<IWICBitmapFrameDecode> best;
    UINT bestW = 0, bestH = 0;
    for (UINT i = 0; i < frames; ++i) {
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        UINT w = 0, h = 0;
        if (FAILED(decoder->GetFrame(i, frame.put())) || !frame || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
            continue;
        if (!best || size_t(w) * h > size_t(bestW) * bestH) {
            best = frame;
            bestW = w;
            bestH = h;
        }
    }
    if (!best || bestW > 16384 || bestH > 16384)
        return false;
    winrt::com_ptr<IWICBitmapSource> source = best;
    // A camera's orientation (EXIF): pictures taken by a phone are often stored sideways.
    WICBitmapTransformOptions turn = WICBitmapTransformRotate0;
    winrt::com_ptr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(best->GetMetadataQueryReader(meta.put())) && meta) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2)
            turn = v.uiVal == 3 ? WICBitmapTransformRotate180 :
                   v.uiVal == 6 ? WICBitmapTransformRotate90 :
                                  (v.uiVal == 8 ? WICBitmapTransformRotate270 : WICBitmapTransformRotate0);
        PropVariantClear(&v);
    }
    if (turn != WICBitmapTransformRotate0) {
        winrt::com_ptr<IWICBitmapFlipRotator> rotator;
        if (SUCCEEDED(wic->CreateBitmapFlipRotator(rotator.put())) && SUCCEEDED(rotator->Initialize(source.get(), turn))) {
            source = rotator;
            if (turn == WICBitmapTransformRotate90 || turn == WICBitmapTransformRotate270)
                std::swap(bestW, bestH);
        }
    }
    // The whole picture, scaled to fit a square as large as the icon (never cropped); FitToSquare centres it.
    const double k = double(sizePx) / double(std::max(bestW, bestH));
    const UINT dw = std::max(1u, UINT(std::lround(bestW * k))), dh = std::max(1u, UINT(std::lround(bestH * k)));
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) || FAILED(scaler->Initialize(source.get(), dw, dh, WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    const UINT stride = dw * 4;
    std::vector<uint8_t> px(size_t(stride) * dh);
    if (FAILED(converter->CopyPixels(nullptr, stride, UINT(px.size()), px.data())))
        return false;
    out = FitToSquare(IconImage::FromBgra(int(dw), int(dh), std::move(px)), sizePx);
    return !out.Empty();
}

std::wstring AppExecutable(const std::wstring& path)
{
    std::wstring target = path;
    if (IsLink(path)) {
        winrt::com_ptr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))))
            return {};
        auto file = link.try_as<IPersistFile>();
        wchar_t buf[MAX_PATH * 2] = {};
        if (!file || FAILED(file->Load(path.c_str(), STGM_READ)) || FAILED(link->GetPath(buf, ARRAYSIZE(buf), nullptr, 0)))
            return {};
        target = buf;
    }
    size_t slash = target.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring app = target.substr(0, slash + 1) + L"app\\" + target.substr(slash + 1);
        DWORD attr = GetFileAttributesW(app.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return app;
    }
    return target;
}

bool LoadAtriumIcon(const std::wstring& exePath, int sizePx, IconImage& out)
{
    const size_t slash = exePath.find_last_of(L"\\/");
    const wchar_t* name = exePath.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
    if (_wcsicmp(name, L"Atrium.Settings.exe") != 0 && _wcsicmp(name, L"Atrium.exe") != 0)
        return false;
    HICON icon = nullptr;
    if (SHDefExtractIconW(exePath.c_str(), 0, 0, &icon, nullptr, MAKELONG(sizePx, 0)) != S_OK || !icon)
        return false;
    IconImage raw;
    bool ok = HIconToImage(icon, raw);
    DestroyIcon(icon);
    if (ok)
        out = FitToSquare(raw, sizePx);
    return ok;
}

bool HasEmbeddedExeIcon(const std::wstring& path)
{
    if (path.empty())
        return false;
    std::wstring expanded = path;
    if (path.find(L'%') != std::wstring::npos) {
        wchar_t buf[MAX_PATH * 2]{};
        const DWORD n = ExpandEnvironmentStringsW(path.c_str(), buf, ARRAYSIZE(buf));
        if (n > 0 && n <= ARRAYSIZE(buf))
            expanded = buf;
    }
    return ExtractIconExW(expanded.c_str(), -1, nullptr, nullptr, 0) > 0;
}

bool IsFullBleedSquareIcon(const IconImage& img)
{
    if (img.Empty() || img.width != img.height || img.width < 8 || img.ByteSize() < static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4)
        return false;
    const int side = img.width;
    const uint8_t* data = img.Data();
    auto alphaAt = [&](int x, int y) -> uint8_t { return data[(static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(x)) * 4 + 3]; };
    constexpr uint8_t kMinOpaqueAlpha = 240;
    if (alphaAt(0, 0) < kMinOpaqueAlpha || alphaAt(side - 1, 0) < kMinOpaqueAlpha || alphaAt(0, side - 1) < kMinOpaqueAlpha ||
        alphaAt(side - 1, side - 1) < kMinOpaqueAlpha)
        return false;
    for (int x = 0; x < side; ++x) {
        if (alphaAt(x, 0) < kMinOpaqueAlpha || alphaAt(x, side - 1) < kMinOpaqueAlpha)
            return false;
    }
    for (int y = 0; y < side; ++y) {
        if (alphaAt(0, y) < kMinOpaqueAlpha || alphaAt(side - 1, y) < kMinOpaqueAlpha)
            return false;
    }
    return true;
}

IconImage ApplySquircleMaskIfFullBleed(const IconImage& img)
{
    if (!IsFullBleedSquareIcon(img))
        return img;
    const int side = img.width;
    const float fSide = static_cast<float>(side);
    // Match bar_item_painter / launchpad icon plate proportion (22% of side).
    const SquircleProfile profile = SquircleFor(fSide, fSide, fSide * 0.22f);
    std::vector<uint8_t> px = img.Bytes();
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const float cover = SquircleCoverage(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, fSide, fSide, profile);
            if (cover >= 1.0f)
                continue;
            uint8_t* p = &px[(static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(x)) * 4];
            for (int c = 0; c < 4; ++c)
                p[c] = static_cast<uint8_t>(std::lround(static_cast<float>(p[c]) * cover));
            for (int c = 0; c < 3; ++c)
                p[c] = std::min(p[c], p[3]);
        }
    }
    return IconImage::FromBgra(side, side, std::move(px));
}

bool LoadShellImage(const std::wstring& parsingName, int sizePx, IconImage& out, bool thumbnail)
{
    if (!thumbnail) {
        std::wstring source = AppExecutable(parsingName);
        int index = 0;
        if (IsLink(parsingName)) {
            winrt::com_ptr<IShellLinkW> link;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) {
                if (auto file = link.try_as<IPersistFile>()) {
                    wchar_t iconPath[MAX_PATH * 2]{};
                    if (SUCCEEDED(file->Load(parsingName.c_str(), STGM_READ)) && SUCCEEDED(link->GetIconLocation(iconPath, ARRAYSIZE(iconPath), &index)) &&
                        *iconPath)
                        source = iconPath;
                }
            }
        }
        HICON icon = nullptr;
        if (!source.empty() && SHDefExtractIconW(source.c_str(), index, 0, &icon, nullptr, MAKELONG(sizePx, 0)) == S_OK && icon) {
            IconImage raw;
            bool ok = HIconToImage(icon, raw);
            DestroyIcon(icon);
            if (ok) {
                out = ApplySquircleMaskIfFullBleed(FitToSquare(raw, sizePx));
                return true;
            }
        }
    }
    if (!ShellImage(parsingName, sizePx, out, thumbnail))
        return false;
    if (!thumbnail)
        out = ApplySquircleMaskIfFullBleed(out);
    return true;
}

} // namespace atrium
