// Verifies IconImage shared pixel buffers, AppIconCache FIFO priority & budget eviction, and Launchpad #system expansion.
#include "icon_image.h"
#include "shell/app_icon_cache.h"
#include "shell/shell_icon.h"
#include "ui/launchpad_page_ids.h"

#include <roapi.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* msg)
{
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        ++g_failures;
    }
}

atrium::IconImage MakeSolidImage(int side, size_t byteOverride = 0)
{
    const size_t bytes = byteOverride ? byteOverride : size_t(side) * size_t(side) * 4;
    return atrium::IconImage::FromBgra(side, side, std::vector<uint8_t>(bytes, 0x7F));
}

uint8_t PixelAlpha(const atrium::IconImage& img, int x, int y) { return img.Data()[(size_t(y) * size_t(img.width) + size_t(x)) * 4 + 3]; }

bool IsPremultipliedBgra(const atrium::IconImage& img)
{
    const size_t total = size_t(img.width) * size_t(img.height);
    const uint8_t* p = img.Data();
    for (size_t i = 0; i < total; ++i) {
        const uint8_t a = p[i * 4 + 3];
        if (p[i * 4 + 0] > a || p[i * 4 + 1] > a || p[i * 4 + 2] > a)
            return false;
    }
    return true;
}

void CheckSquircleMask()
{
    using namespace atrium;
    constexpr int kSide = 64;

    // 1. Opaque full-bleed square -> masked, rounded corners, preserved center, premultiplied, idempotent.
    std::vector<uint8_t> solid(size_t(kSide) * kSide * 4);
    for (size_t i = 0; i < size_t(kSide) * kSide; ++i) {
        solid[i * 4 + 0] = 100;
        solid[i * 4 + 1] = 150;
        solid[i * 4 + 2] = 200;
        solid[i * 4 + 3] = 255;
    }
    const IconImage fullBleed = IconImage::FromBgra(kSide, kSide, solid);
    Check(IsFullBleedSquareIcon(fullBleed), "#135: opaque square icon detected as full-bleed");
    const IconImage masked = ApplySquircleMaskIfFullBleed(fullBleed);
    Check(PixelAlpha(masked, 0, 0) == 0 && PixelAlpha(masked, kSide - 1, 0) == 0 && PixelAlpha(masked, 0, kSide - 1) == 0 &&
              PixelAlpha(masked, kSide - 1, kSide - 1) == 0,
          "#135: squircle mask rounds all four corners");
    const size_t center = (size_t(kSide / 2) * kSide + size_t(kSide / 2)) * 4;
    Check(masked.Data()[center + 0] == 100 && masked.Data()[center + 1] == 150 && masked.Data()[center + 2] == 200 && masked.Data()[center + 3] == 255,
          "#135: squircle mask preserves center pixel");
    Check(IsPremultipliedBgra(masked), "#135: masked image preserves premultiplied BGRA invariant");
    Check(!IsFullBleedSquareIcon(masked), "#135: masked image is no longer full-bleed");
    const IconImage maskedTwice = ApplySquircleMaskIfFullBleed(masked);
    Check(maskedTwice.Bytes() == masked.Bytes(), "#135: ApplySquircleMaskIfFullBleed is idempotent");

    // 2. Non-square image stays byte-identical.
    const IconImage nonSquare = IconImage::FromBgra(kSide, kSide / 2, std::vector<uint8_t>(size_t(kSide) * (kSide / 2) * 4, 255));
    Check(!IsFullBleedSquareIcon(nonSquare) && ApplySquircleMaskIfFullBleed(nonSquare).Bytes() == nonSquare.Bytes(), "#135: non-square bitmap left untouched");

    // 3. Circular icon stays byte-identical.
    std::vector<uint8_t> circlePx(size_t(kSide) * kSide * 4, 0);
    const float mid = (kSide - 1) * 0.5f;
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            const float dx = float(x) - mid, dy = float(y) - mid;
            if (dx * dx + dy * dy <= 24.f * 24.f) {
                const size_t idx = (size_t(y) * kSide + size_t(x)) * 4;
                circlePx[idx + 0] = circlePx[idx + 1] = circlePx[idx + 2] = circlePx[idx + 3] = 255;
            }
        }
    }
    const IconImage circle = IconImage::FromBgra(kSide, kSide, circlePx);
    Check(!IsFullBleedSquareIcon(circle) && ApplySquircleMaskIfFullBleed(circle).Bytes() == circle.Bytes(), "#135: circular bitmap left untouched");

    // 4. Transparent-border icon stays byte-identical.
    std::vector<uint8_t> borderedPx = solid;
    for (int i = 0; i < kSide; ++i) {
        borderedPx[size_t(i) * 4 + 3] = 0;
        borderedPx[(size_t(kSide - 1) * kSide + size_t(i)) * 4 + 3] = 0;
        borderedPx[(size_t(i) * kSide) * 4 + 3] = 0;
        borderedPx[(size_t(i) * kSide + size_t(kSide - 1)) * 4 + 3] = 0;
    }
    const IconImage bordered = IconImage::FromBgra(kSide, kSide, borderedPx);
    Check(!IsFullBleedSquareIcon(bordered) && ApplySquircleMaskIfFullBleed(bordered).Bytes() == bordered.Bytes(),
          "#135: transparent-border bitmap left untouched");

    // 5. X-shaped icon (opaque corners, transparent mid-edges) stays byte-identical.
    std::vector<uint8_t> xPx(size_t(kSide) * kSide * 4, 0);
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            if (std::abs(x - y) <= 4 || std::abs((kSide - 1 - x) - y) <= 4) {
                const size_t idx = (size_t(y) * kSide + size_t(x)) * 4;
                xPx[idx + 0] = xPx[idx + 1] = xPx[idx + 2] = xPx[idx + 3] = 255;
            }
        }
    }
    const IconImage xShape = IconImage::FromBgra(kSide, kSide, xPx);
    Check(!IsFullBleedSquareIcon(xShape) && ApplySquircleMaskIfFullBleed(xShape).Bytes() == xShape.Bytes(), "#135: X-shaped bitmap left untouched");
}

// The Launchpad entry's own icon (settings.launchpadIcon): a picture file read as the bar draws it.
std::vector<uint8_t> EncodePng(IWICImagingFactory* wic, UINT w, UINT h, const std::vector<uint8_t>& bgra)
{
    winrt::com_ptr<IStream> stream;
    winrt::com_ptr<IWICBitmap> bitmap;
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) ||
        FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, UINT(bgra.size()), const_cast<BYTE*>(bgra.data()), bitmap.put())) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) || FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(w, h)))
        return {};
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(bitmap.get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    STATSTG stat{};
    LARGE_INTEGER start{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)))
        return {};
    std::vector<uint8_t> bytes(size_t(stat.cbSize.QuadPart));
    ULONG read = 0;
    return SUCCEEDED(stream->Read(bytes.data(), ULONG(bytes.size()), &read)) && read == bytes.size() ? bytes : std::vector<uint8_t>{};
}

void CheckLoadPictureIcon()
{
    using namespace atrium;
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    Check(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "COM starts for LoadPictureIcon");
    winrt::com_ptr<IWICImagingFactory> wic;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) && wic,
          "WIC factory starts for LoadPictureIcon");

    // A wide picture (96 x 48, opaque red): fitted, it leaves transparent bands above and below.
    constexpr UINT kWidth = 96, kHeight = 48;
    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4);
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i + 2] = 220;
        bgra[i + 3] = 255;
    }
    const std::vector<uint8_t> png = EncodePng(wic.get(), kWidth, kHeight, bgra);
    wchar_t temp[MAX_PATH] = {}, path[MAX_PATH] = {};
    Check(!png.empty() && GetTempPathW(ARRAYSIZE(temp), temp) > 0 && GetTempFileNameW(temp, L"atr", 0, path) != 0, "a temporary picture file is created");
    {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, png.data(), DWORD(png.size()), &written, nullptr) && written == png.size();
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        Check(ok, "the picture file is written");
    }

    IconImage icon;
    Check(LoadPictureIcon(path, 64, icon) && icon.width == 64 && icon.height == 64, "LoadPictureIcon fits a wide picture into a 64 x 64 icon");
    Check(PixelAlpha(icon, 32, 2) == 0 && PixelAlpha(icon, 32, 61) == 0 && PixelAlpha(icon, 32, 32) == 255,
          "LoadPictureIcon keeps the whole picture: transparent bands, opaque middle");
    Check(IsPremultipliedBgra(icon), "LoadPictureIcon hands the painter premultiplied BGRA");
    Check(!LoadPictureIcon(path, 0, icon), "LoadPictureIcon refuses a size of zero");
    DeleteFileW(path);
    IconImage missing;
    Check(!LoadPictureIcon(path, 64, missing), "LoadPictureIcon fails when the file is gone");
    Check(!LoadPictureIcon(L"", 64, missing), "LoadPictureIcon fails without a path");
}

} // namespace

int main()
{
    using namespace atrium;

    // 1. IconImage shared read-only pixels & FitToSquare zero-copy fast path.
    const IconImage orig = MakeSolidImage(64);
    IconImage copy;
    copy = orig;
    const IconImage fitSame = FitToSquare(orig, 64);
    Check(!orig.Empty() && orig.ByteSize() == 64 * 64 * 4, "IconImage reports exact byte size");
    Check(copy.Data() == orig.Data() && fitSame.Data() == orig.Data(), "IconImage copy and same-size FitToSquare share buffer");

    // 2. Priority lanes: FIFO within tier, Visible > Ahead > Prefetch, and rapid SetAhead replacement.
    AppIconCache cache;
    cache.QueueIcon({L"pf1", 64}, IconJobTier::Prefetch, 100);
    cache.QueueIcon({L"pf2", 64}, IconJobTier::Prefetch, 100);
    cache.SetAhead({L"stale1", L"stale2"}, 64, 100);
    // Flip page before stale1/stale2 ran: replace with adj1/adj2 and promote pf2 to Visible.
    cache.SetAhead({L"adj1", L"adj2"}, 64, 101);
    cache.QueueIcon({L"pf2", 64}, IconJobTier::Visible, 101);

    IconKey popped;
    std::vector<std::wstring> order;
    while (cache.PopNextJob(popped)) {
        order.push_back(popped.first);
        cache.activeJob.reset();
    }
    const std::vector<std::wstring> expectedOrder = {L"pf2", L"adj1", L"adj2", L"pf1"};
    Check(order == expectedOrder, "queue must decode Visible first, fresh Ahead next, FIFO Prefetch last, dropping stale Ahead");

    // 3. Incremental cacheBytes accounting, SetVisible no-op short circuit, stale sweep, and 512 / 16 MiB eviction.
    cache = {};
    for (int i = 0; i < 600; ++i) {
        const std::wstring id = L"app" + std::to_wstring(i);
        cache.liveIds.insert(id);
        cache.InsertIcon({id, 64}, MakeSolidImage(40, 40 * 1024));
    }
    Check(cache.cacheBytes == 600ULL * 40ULL * 1024ULL, "cacheBytes tracks inserts accurately");

    // Replace one icon with double size.
    cache.InsertIcon({L"app0", 64}, MakeSolidImage(40, 80 * 1024));
    Check(cache.cacheBytes == 599ULL * 40ULL * 1024ULL + 80ULL * 1024ULL, "cacheBytes tracks replacement size delta");

    Check(cache.SetVisible(0, {{L"app0", 64}, {L"app1", 64}}), "first SetVisible returns changed");
    Check(!cache.SetVisible(0, {{L"app1", 64}, {L"app0", 64}}), "unchanged visible set short-circuits");
    cache.SetAhead({L"app2"}, 64, 200);

    cache.Prune(true, true, [](const std::wstring&) { return false; });
    Check(cache.icons.size() <= kIconCacheMaxCount && cache.cacheBytes <= kIconCacheMaxBytes, "Prune enforces 16 MiB and 512-entry limits");
    Check(cache.icons.contains({L"app0", 64}) && cache.icons.contains({L"app1", 64}) && cache.icons.contains({L"app2", 64}),
          "visible and narrowed ahead icons stay protected across eviction");
    Check(!cache.icons.contains({L"app3", 64}), "oldest unprotected prewarmed entry is evicted");

    // Verify stale sweep updates cacheBytes accurately.
    const size_t beforeSweep = cache.cacheBytes;
    cache.liveIds.erase(L"app599");
    const bool had599 = cache.icons.contains({L"app599", 64});
    cache.Prune(true, true, [](const std::wstring&) { return false; });
    if (had599)
        Check(cache.cacheBytes == beforeSweep - 40ULL * 1024ULL && !cache.icons.contains({L"app599", 64}),
              "stale sweep removes unreferenced icon and subtracts its bytes");

    // 4. Launchpad #system folder expansion.
    std::vector<CatalogApp> shown = {{L"A", L"a"}, {L"System", L"#system"}, {L"B", L"b"}};
    std::vector<CatalogApp> sys;
    sys.reserve(12);
    for (int i = 0; i < 12; ++i)
        sys.push_back({L"Sys" + std::to_wstring(i), L"sys" + std::to_wstring(i)});
    const std::vector<std::wstring> pageIds = ExpandLaunchpadPageIds(shown, sys, 0, 10, L"#system");
    Check(pageIds.size() == 11 && pageIds[0] == L"a" && pageIds[1] == L"sys0" && pageIds[9] == L"sys8" && pageIds[10] == L"b",
          "ExpandLaunchpadPageIds expands #system into its first 9 apps");

    CheckSquircleMask();
    CheckLoadPictureIcon();

    return g_failures ? 1 : 0;
}
