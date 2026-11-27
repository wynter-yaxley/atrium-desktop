#include "ui/widget_internal.h"

#include "localization.h"
#include "log.h"
#include "settings.h"
#include "../posted.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace widget_detail {

namespace {

struct KindName {
    WidgetKind kind;
    const wchar_t* name;
};

const KindName kKinds[] = {{WidgetKind::Clock, L"clock"},         {WidgetKind::Calendar, L"calendar"}, {WidgetKind::Battery, L"battery"},
                           {WidgetKind::Digital, L"digital"},     {WidgetKind::System, L"system"},     {WidgetKind::Notes, L"notes"},
                           {WidgetKind::Reminders, L"reminders"}, {WidgetKind::Photos, L"photos"},     {WidgetKind::Weather, L"weather"},
                           {WidgetKind::Bluetooth, L"bluetooth"}, {WidgetKind::Network, L"network"}};

} // namespace

const wchar_t* NameOf(WidgetKind kind)
{
    const auto* it = std::ranges::find(kKinds, kind, &KindName::kind);
    return it != std::end(kKinds) ? it->name : L"clock";
}

bool ParseKind(const std::wstring& name, WidgetKind& kind)
{
    const auto* it = std::ranges::find(kKinds, name, &KindName::name);
    return it != std::end(kKinds) ? (kind = it->kind, true) : false;
}

std::wstring ClampText(std::wstring text, size_t max)
{
    if (text.size() <= max)
        return text;
    return text.substr(0, max - (max && IS_HIGH_SURROGATE(text[max - 1]) ? 1 : 0));
}

// The first city of the local time zone's name: "(UTC+08:00) 北京，重庆，…" -> "北京".
std::wstring LocalCity()
{
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID)
        return {};
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\" + std::wstring(tz.TimeZoneKeyName);
    wchar_t display[256] = {};
    DWORD size = sizeof(display);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Display", RRF_RT_REG_SZ, nullptr, display, &size) != ERROR_SUCCESS)
        return {};
    std::wstring d = display;
    size_t open = d.find(L") ");
    if (open != std::wstring::npos)
        d = d.substr(open + 2);
    size_t end = d.find_first_of(L"，,");
    return CanonicalCity(end == std::wstring::npos ? d : d.substr(0, end));
}

} // namespace widget_detail

using namespace widget_detail;

namespace {

std::wstring LayoutPath()
{
    std::wstring p = SettingsPath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\widgets.json";
}

std::vector<std::wstring> ParseCityList(const json::JsonObject& e)
{
    std::vector<std::wstring> cities;
    if (!e.HasKey(L"cities") || e.GetNamedValue(L"cities").ValueType() != json::JsonValueType::Array)
        return cities;
    for (const auto& cv : e.GetNamedArray(L"cities"))
        if (cv.ValueType() == json::JsonValueType::String)
            cities.emplace_back(CanonicalCity(std::wstring(cv.GetString())));
    return cities;
}

std::vector<Reminder> ParseReminderItems(const json::JsonObject& e)
{
    std::vector<Reminder> items;
    if (!e.HasKey(L"items") || e.GetNamedValue(L"items").ValueType() != json::JsonValueType::Array)
        return items;
    for (const auto& iv : e.GetNamedArray(L"items")) {
        if (iv.ValueType() != json::JsonValueType::Object)
            continue;
        json::JsonObject io = iv.GetObject();
        items.push_back({ClampText(std::wstring(io.GetNamedString(L"text", L"")), kMaxReminderText), io.GetNamedBoolean(L"done", false)});
    }
    return items;
}

WidgetContent ParseWidgetContent(const json::JsonObject& e)
{
    WidgetContent content;
    content.text = ClampText(std::wstring(e.GetNamedString(L"text", L"")), kMaxNoteText);
    content.folder = e.GetNamedString(L"folder", L"");
    content.cities = ParseCityList(e);
    content.items = ParseReminderItems(e);
    return content;
}

json::JsonObject SerializeWidget(WidgetKind kind, WidgetSize size, int col, int row, const WidgetContent& c)
{
    json::JsonObject e;
    e.SetNamedValue(L"kind", json::JsonValue::CreateStringValue(NameOf(kind)));
    e.SetNamedValue(L"size", json::JsonValue::CreateStringValue(SizeName(size)));
    e.SetNamedValue(L"col", json::JsonValue::CreateNumberValue(col));
    e.SetNamedValue(L"row", json::JsonValue::CreateNumberValue(row));
    if (kind == WidgetKind::Notes)
        e.SetNamedValue(L"text", json::JsonValue::CreateStringValue(c.text));
    if (kind == WidgetKind::Reminders) {
        json::JsonArray items;
        for (const Reminder& r : c.items) {
            if (r.text.empty())
                continue; // one being typed
            json::JsonObject io;
            io.SetNamedValue(L"text", json::JsonValue::CreateStringValue(r.text));
            io.SetNamedValue(L"done", json::JsonValue::CreateBooleanValue(r.done));
            items.Append(io);
        }
        e.SetNamedValue(L"items", items);
    }
    if (kind == WidgetKind::Photos && !c.folder.empty())
        e.SetNamedValue(L"folder", json::JsonValue::CreateStringValue(c.folder));
    if (!c.cities.empty()) {
        json::JsonArray cities;
        for (const std::wstring& city : c.cities)
            cities.Append(json::JsonValue::CreateStringValue(city));
        e.SetNamedValue(L"cities", cities);
    }
    return e;
}

std::wstring PicturesFolder()
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

// The pictures of a folder and its subfolders (two levels, a few thousand at most).
void ListPhotos(const std::wstring& dir, std::vector<std::wstring>& out, int depth = 0)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 2)
                ListPhotos(dir + L"\\" + name, out, depth + 1);
            continue;
        }
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos)
            continue;
        std::wstring ext = name.substr(dot);
        std::ranges::transform(ext, ext.begin(), ::towlower);
        static constexpr const wchar_t* kExts[] = {L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".webp", L".heic", L".tif", L".tiff"};
        if (std::ranges::find(kExts, ext) != std::end(kExts))
            out.push_back(dir + L"\\" + name);
    } while (out.size() < 5000 && FindNextFileW(h, &fd));
    FindClose(h);
}

// A photo cropped to fill w x h px (centred) and turned upright, premultiplied BGRA.
IconImage ReadPhoto(const std::wstring& path, UINT w, UINT h)
{
    IconImage out;
    winrt::com_ptr<IWICImagingFactory> f;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()))) ||
        FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())))
        return out;
    winrt::com_ptr<IWICBitmapSource> src = frame;
    // The camera's orientation (EXIF): phone pictures are often stored sideways.
    WICBitmapTransformOptions turn = WICBitmapTransformRotate0;
    winrt::com_ptr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(frame->GetMetadataQueryReader(meta.put()))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2)
            turn = v.uiVal == 3 ? WICBitmapTransformRotate180 :
                   v.uiVal == 6 ? WICBitmapTransformRotate90 :
                                  (v.uiVal == 8 ? WICBitmapTransformRotate270 : WICBitmapTransformRotate0);
        PropVariantClear(&v);
    }
    if (turn != WICBitmapTransformRotate0) {
        winrt::com_ptr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(f->CreateBitmapFlipRotator(rot.put())) && SUCCEEDED(rot->Initialize(src.get(), turn)))
            src = rot;
    }
    UINT sw = 0, sh = 0;
    if (FAILED(src->GetSize(&sw, &sh)) || !sw || !sh)
        return out;
    double k = std::max(double(w) / sw, double(h) / sh);
    UINT cw = std::min(sw, UINT(std::lround(w / k))), ch = std::min(sh, UINT(std::lround(h / k)));
    WICRect crop{INT((sw - cw) / 2), INT((sh - ch) / 2), INT(cw), INT(ch)};
    winrt::com_ptr<IWICBitmapClipper> clip;
    winrt::com_ptr<IWICBitmapScaler> scale;
    winrt::com_ptr<IWICFormatConverter> conv;
    if (FAILED(f->CreateBitmapClipper(clip.put())) || FAILED(clip->Initialize(src.get(), &crop)) || FAILED(f->CreateBitmapScaler(scale.put())) ||
        FAILED(scale->Initialize(clip.get(), w, h, WICBitmapInterpolationModeHighQualityCubic)) || FAILED(f->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(scale.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return out;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data())))
        return {};
    return IconImage::FromBgra(int(w), int(h), std::move(px));
}

} // namespace

bool DesktopWidgets::ParseNames(const std::wstring& kind, const std::wstring& size, WidgetKind& outKind, WidgetSize& outSize)
{
    outSize = ParseSize(size);
    return ParseKind(kind, outKind);
}

// ---- grid --------------------------------------------------------------------

RECT DesktopWidgets::WorkArea() const
{
    MONITORINFO mi{sizeof(mi)};
    HMONITOR m = monitor_;
    if (!m || !GetMonitorInfoW(m, &mi)) {
        m = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(m, &mi);
    }
    RECT wa = mi.rcWork;
    const LONG room = LONG(std::lround(90 * PopupScale(wa)));
    switch (barEdge_) {
    case BarEdge::Top: wa.top = std::min(wa.bottom, wa.top + room); break;
    case BarEdge::Left: wa.left = std::min(wa.right, wa.left + room); break;
    case BarEdge::Right: wa.right = std::max(wa.left, wa.right - room); break;
    default: wa.bottom = std::max(wa.top, wa.bottom - room); break;
    }
    return wa;
}

float DesktopWidgets::Scale() const { return PopupScale(WorkArea()); }

int DesktopWidgets::Cols() const
{
    RECT wa = WorkArea();
    return std::max(1, int((float(wa.right - wa.left) / Scale() - 2 * kMargin + kWidgetGap) / (kWidgetCell + kWidgetGap)));
}

int DesktopWidgets::Rows() const
{
    RECT wa = WorkArea();
    return std::max(1, int((float(wa.bottom - wa.top) / Scale() - 2 * kMargin + kWidgetGap) / (kWidgetCell + kWidgetGap)));
}

// Each half of the grid hugs its own edge (D44): a widget whose middle is left of the grid's
// middle stands kMargin from the left edge, any other kMargin from the right, so whatever the
// grid does not fill is left in the middle and both sides have the same margin.
int DesktopWidgets::VisualCol(int col, WidgetSize size) const { return onRight_ ? Cols() - col - WidgetCols(size) : col; }

bool DesktopWidgets::FromRight(int col, WidgetSize size) const { return 2 * VisualCol(col, size) + WidgetCols(size) >= Cols(); }

RECT DesktopWidgets::CellRect(int col, int row, WidgetSize size) const
{
    RECT wa = WorkArea();
    float s = Scale();
    const float step = kWidgetCell + kWidgetGap;
    const int visualCol = VisualCol(col, size);
    float left = FromRight(col, size) ? float(wa.right - wa.left) / s - kMargin - float(Cols() - visualCol - WidgetCols(size)) * step - WidgetWidth(size) :
                                        kMargin + float(visualCol) * step;
    int x = wa.left + int(std::lround(left * s));
    int y = wa.top + int(std::lround((kMargin + float(row) * (kWidgetCell + kWidgetGap)) * s));
    return RECT{x, y, x + int(std::lround(WidgetWidth(size) * s)), y + int(std::lround(WidgetHeight(size) * s))};
}

bool DesktopWidgets::Fits(int col, int row, WidgetSize size, const Widget* except) const
{
    if (col < 0 || row < 0 || col + WidgetCols(size) > Cols() || row + WidgetRows(size) > Rows())
        return false;
    return std::ranges::none_of(widgets_, [&](const auto& w) {
        return w.get() != except && col < w->col + WidgetCols(w->size) && w->col < col + WidgetCols(size) && row < w->row + WidgetRows(w->size) &&
               w->row < row + WidgetRows(size);
    });
}

bool DesktopWidgets::FreeCellNear(float col, float row, WidgetSize size, const Widget* except, int& outCol, int& outRow) const
{
    float best = 1e9f;
    for (int r = 0; r < Rows(); ++r)
        for (int c = 0; c < Cols(); ++c) {
            if (!Fits(c, r, size, except))
                continue;
            float d = (float(c) - col) * (float(c) - col) + (float(r) - row) * (float(r) - row);
            if (d < best) {
                best = d;
                outCol = c;
                outRow = r;
            }
        }
    return best < 1e9f;
}

// ---- persistence -------------------------------------------------------------

bool DesktopWidgets::AddLoadedWidget(WidgetKind kind, WidgetSize size, int col, int row, const WidgetContent& content)
{
    if (!WidgetSupports(kind, size))
        size = WidgetSize::Small;
    if (!Fits(col, row, size, nullptr) && !FreeCellNear(float(col), float(row), size, nullptr, col, row))
        return false; // no room left on this screen
    Create(kind, size, col, row).content = content;
    return true;
}

bool DesktopWidgets::LoadFromJson(const std::wstring& text)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(text, o) || !o.HasKey(L"widgets") || o.GetNamedValue(L"widgets").ValueType() != json::JsonValueType::Array)
        return false;
    for (const auto& v : o.GetNamedArray(L"widgets")) {
        if (v.ValueType() != json::JsonValueType::Object)
            continue;
        json::JsonObject e = v.GetObject();
        WidgetKind k;
        if (!ParseKind(std::wstring(e.GetNamedString(L"kind", L"")), k))
            continue;
        WidgetContent content = ParseWidgetContent(e);
        if (!AddLoadedWidget(k, ParseSize(std::wstring(e.GetNamedString(L"size", L"small"))), int(e.GetNamedNumber(L"col", 0)),
                             int(e.GetNamedNumber(L"row", 0)), content))
            unplaced_.emplace_back(e.Stringify());
    }
    return true;
}

void DesktopWidgets::LoadDefaults()
{
    // First run: a column of widgets at the top left, as macOS starts.
    AddLoadedWidget(WidgetKind::Clock, WidgetSize::Small, 0, 0, {});
    AddLoadedWidget(WidgetKind::Battery, WidgetSize::Small, 1, 0, {});
    AddLoadedWidget(WidgetKind::Calendar, WidgetSize::Medium, 0, 1, {});
    AddLoadedWidget(WidgetKind::Clock, WidgetSize::Medium, 0, 2, {});
}

void DesktopWidgets::Load()
{
    widgets_.clear();
    unplaced_.clear();
    std::wstring text = ReadTextFile(LayoutPath());
    bool read = false;
    if (!text.empty()) {
        read = LoadFromJson(text);
        if (!read)
            Log(L"widgets: %s is not valid, starting afresh", LayoutPath().c_str());
    }
    if (!read)
        LoadDefaults();
}

void DesktopWidgets::Save() const
{
    json::JsonArray list;
    for (const auto& w : widgets_)
        list.Append(SerializeWidget(w->kind, w->size, w->col, w->row, w->content));
    for (const auto& s : unplaced_)
        if (json::JsonObject e{nullptr}; json::JsonObject::TryParse(s, e))
            list.Append(e);
    json::JsonObject o;
    o.SetNamedValue(L"widgets", list);
    if (!WriteTextFile(LayoutPath(), std::wstring(o.Stringify())))
        Log(L"widgets: could not save the layout");
}

void DesktopWidgets::SaveSoon() { SetTimer(host_, kSaveTimer, kSaveMs, nullptr); }

// ---- photos ------------------------------------------------------------------

void DesktopWidgets::PickFolder(Widget& w)
{
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return;
    FILEOPENDIALOGOPTIONS o = 0;
    dialog->GetOptions(&o);
    dialog->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(Tr(L"Choose a photo folder"));
    Widget* wp = &w;
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(host_)) || FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    if (Has(wp)) { // the dialog ran its own message loop
        wp->content.folder = path;
        Save();
        LoadPhoto(*wp, true);
        Log(L"widgets: photos from a chosen folder");
    }
    CoTaskMemFree(path);
}

// another: a different picture of the folder; else the one shown, read again (a new size).
void DesktopWidgets::LoadPhoto(Widget& w, bool another)
{
    if (!w.surface)
        return;
    float s = Scale();
    UINT pw = UINT(std::lround(WidgetWidth(w.size) * s)), ph = UINT(std::lround(WidgetHeight(w.size) * s));
    unsigned gen = w.photoGen = ++photoGen_;
    w.photoAt = GetTickCount64(); // no second request while this one runs
    {
        std::lock_guard g(photoMu_);
        std::erase_if(photoJobs_, [&](const PhotoJob& j) { return j.widget == &w; });
        photoJobs_.push_back({w.content.photo, another, w.content.folder.empty() ? PicturesFolder() : w.content.folder, &w, gen, pw, ph, host_});
        if (!photoWorker_.joinable() && !photoStop_)
            photoWorker_ = std::thread([this] { PhotoWorker(); });
    }
    photoCv_.notify_one();
}

void DesktopWidgets::PhotoWorker()
{
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    std::mt19937 rng{std::random_device{}()};
    for (PhotoJob job;;) {
        {
            std::unique_lock g(photoMu_);
            photoCv_.wait(g, [this] { return photoStop_ || !photoJobs_.empty(); });
            if (photoStop_)
                break;
            job = std::move(photoJobs_.front());
            photoJobs_.pop_front();
        }
        std::wstring path = job.current;
        if (job.another || path.empty()) {
            std::vector<std::wstring> all;
            ListPhotos(job.folder, all);
            if (all.size() > 1)
                std::erase(all, job.current);
            path = all.empty() ? L"" : all[std::uniform_int_distribution<size_t>(0, all.size() - 1)(rng)];
        }
        auto img = path.empty() ? IconImage{} : ReadPhoto(path, job.pw, job.ph);
        PostOwned(job.host, WM_WIDGET_PHOTO, 0, std::make_unique<PhotoRead>(PhotoRead{job.widget, job.gen, path, std::move(img)}));
    }
    if (com)
        CoUninitialize();
}

} // namespace atrium
