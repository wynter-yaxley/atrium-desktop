#include "ui/widget_faces.h"
#include "ui/widget_faces_internal.h"

#include "localization.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace atrium {

namespace {

using Ink = WidgetInk;
using Pal = WidgetPal;

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return WidgetRgba(rgb, a); }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return WidgetAlpha(c, a); }

Pal GlassPal(const Theme& t)
{
    return {t.text,
            t.subtext,
            t.faint,
            Alpha(t.text, 0.10f),
            Rgba(0xFF453A, 0.98f),
            Rgba(0xFF9F0A, 0.98f),
            Rgba(0x30D158, 0.98f),
            Alpha(t.text, 0.18f),
            Rgba(0x0A84FF, 0.98f),
            Rgba(0xFFFFFF, 0.98f),
            Rgba(0x000000, 0.42f),
            t.light ? Ink::Dark : Ink::Light};
}

// Dusk: white only, the accents too (macOS's dimmed widgets are monochrome); a disc's ink is dark.
Pal DuskPal()
{
    const D2D1_COLOR_F white = Rgba(0xFFFFFF, 0.96f);
    return {white,
            Rgba(0xFFFFFF, 0.72f),
            Rgba(0xFFFFFF, 0.50f),
            Rgba(0xFFFFFF, 0.12f),
            white,
            white,
            white,
            Rgba(0xFFFFFF, 0.22f),
            white,
            Rgba(0x1C1C1E, 0.92f),
            Rgba(0x000000, 0.32f),
            Ink::Mono};
}

// Color, on a white face: dark ink and macOS's light-mode accents.
Pal PaperPal()
{
    const D2D1_COLOR_F ink = Rgba(0x1D1D1F, 0.94f);
    return {ink,
            Rgba(0x1D1D1F, 0.58f),
            Rgba(0x1D1D1F, 0.36f),
            Rgba(0x1D1D1F, 0.05f),
            Rgba(0xFF3B30, 1),
            Rgba(0xFF9500, 1),
            Rgba(0x34C759, 1),
            Rgba(0x1D1D1F, 0.10f),
            Rgba(0x007AFF, 1),
            Rgba(0xFFFFFF, 0.98f),
            Rgba(0x1C1C1E, 1),
            Ink::Dark};
}

const wchar_t* const kWeekLong[] = {L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六", L"星期日"};
const wchar_t* const kWeekLongEn[] = {L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday", L"Sunday"};
const wchar_t* const kWeekShort[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
const wchar_t* const kWeekShortEn[] = {L"M", L"T", L"W", L"T", L"F", L"S", L"S"};
const wchar_t* const kMonths[] = {L"一月", L"二月", L"三月", L"四月", L"五月", L"六月", L"七月", L"八月", L"九月", L"十月", L"十一月", L"十二月"};
const wchar_t* const kMonthsEn[] = {L"January", L"February", L"March",     L"April",   L"May",      L"June",
                                    L"July",    L"August",   L"September", L"October", L"November", L"December"};
const wchar_t* WeekLong(int wd) { return WidgetWeekLong(wd); }

bool ZoneInfo(const wchar_t* key, DYNAMIC_TIME_ZONE_INFORMATION& out)
{
    static std::map<std::wstring, DYNAMIC_TIME_ZONE_INFORMATION> zones; // read once
    if (zones.empty()) {
        DYNAMIC_TIME_ZONE_INFORMATION d{};
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &d) == ERROR_SUCCESS; ++i)
            zones[d.TimeZoneKeyName] = d;
    }
    auto it = zones.find(key);
    if (it == zones.end())
        return false;
    out = it->second;
    return true;
}

int64_t Minutes(const SYSTEMTIME& t)
{
    FILETIME f{};
    SystemTimeToFileTime(&t, &f);
    return int64_t((ULARGE_INTEGER{f.dwLowDateTime, f.dwHighDateTime}.QuadPart) / 600000000ULL);
}

// The time in a city, and how far it is from here: "今天 +1小时" (half hours as "+5.5小时").
SYSTEMTIME CityTime(const std::wstring& name, const SYSTEMTIME& now, std::wstring& when)
{
    SYSTEMTIME utc, ct = now;
    GetSystemTime(&utc);
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    for (const WorldCity& city : WorldCities())
        if (name == city.name && ZoneInfo(city.zone, z))
            SystemTimeToTzSpecificLocalTimeEx(&z, &utc, &ct);
    const int64_t minutes = Minutes(ct) - Minutes(now);
    // Minutes() floors the seconds: a quarter-hour zone may come out a minute short.
    const int64_t quarters = (minutes + (minutes >= 0 ? 7 : -7)) / 15;
    int dayDiff = int((Minutes(ct) / 1440) - (Minutes(now) / 1440));
    std::wstring hours = std::to_wstring(quarters / 4);
    if (quarters % 4) {
        wchar_t b[16];
        swprintf_s(b, L"%g", double(quarters) / 4);
        hours = b;
    }
    std::wstring offset = (quarters >= 0 ? L"+" : L"") + hours;
    when = dayDiff > 0 ? TrFormat(L"Tomorrow %s h", offset.c_str()) :
           dayDiff < 0 ? TrFormat(L"Yesterday %s h", offset.c_str()) :
                         TrFormat(L"Today %s h", offset.c_str());
    return ct;
}

} // namespace

const wchar_t* WidgetWeekLong(int wd) { return LanguageIsEnglish() ? Tr(kWeekLongEn[wd]) : kWeekLong[wd]; }
const wchar_t* WidgetWeekShort(int wd) { return LanguageIsEnglish() ? kWeekShortEn[wd] : kWeekShort[wd]; }
const wchar_t* WidgetMonthName(int m) { return LanguageIsEnglish() ? Tr(kMonthsEn[std::clamp(m, 1, 12) - 1]) : kMonths[std::clamp(m, 1, 12) - 1]; }

const std::vector<WorldCity>& WorldCities()
{
    static const std::vector<WorldCity> kCities = {
        {L"Beijing", L"China Standard Time"},        {L"Shanghai", L"China Standard Time"},
        {L"Hong Kong", L"China Standard Time"},      {L"Taipei", L"Taipei Standard Time"},
        {L"Tokyo", L"Tokyo Standard Time"},          {L"Seoul", L"Korea Standard Time"},
        {L"Singapore", L"Singapore Standard Time"},  {L"Bangkok", L"SE Asia Standard Time"},
        {L"New Delhi", L"India Standard Time"},      {L"Dubai", L"Arabian Standard Time"},
        {L"Moscow", L"Russian Standard Time"},       {L"Istanbul", L"Turkey Standard Time"},
        {L"Cairo", L"Egypt Standard Time"},          {L"Johannesburg", L"South Africa Standard Time"},
        {L"Berlin", L"W. Europe Standard Time"},     {L"Paris", L"Romance Standard Time"},
        {L"London", L"GMT Standard Time"},           {L"São Paulo", L"E. South America Standard Time"},
        {L"New York", L"Eastern Standard Time"},     {L"Toronto", L"Eastern Standard Time"},
        {L"Chicago", L"Central Standard Time"},      {L"Denver", L"Mountain Standard Time"},
        {L"Los Angeles", L"Pacific Standard Time"},  {L"Vancouver", L"Pacific Standard Time"},
        {L"Anchorage", L"Alaskan Standard Time"},    {L"Honolulu", L"Hawaiian Standard Time"},
        {L"Sydney", L"AUS Eastern Standard Time"},   {L"Melbourne", L"AUS Eastern Standard Time"},
        {L"Auckland", L"New Zealand Standard Time"},
    };
    return kCities;
}

bool HasWorldClocks(WidgetKind kind, WidgetSize size) { return size != WidgetSize::Small && (kind == WidgetKind::Clock || kind == WidgetKind::Digital); }

std::vector<std::wstring> WidgetCities(WidgetKind kind, const std::vector<std::wstring>& chosen)
{
    // macOS's medium Clock shows four cities; the digital one lists three beside the local time.
    using V = std::vector<std::wstring>;
    V cities = kind == WidgetKind::Digital ? V{L"Tokyo", L"London", L"New York"} : V{L"Beijing", L"Tokyo", L"London", L"New York"};
    for (size_t i = 0; i < cities.size() && i < chosen.size(); ++i)
        if (std::wstring c = CanonicalCity(chosen[i]); std::any_of(WorldCities().begin(), WorldCities().end(), [&](const WorldCity& w) { return c == w.name; }))
            cities[i] = std::move(c);
    return cities;
}

namespace {

std::wstring HourMinute(const SYSTEMTIME& t)
{
    wchar_t b[16];
    swprintf_s(b, L"%d:%02d", t.wHour, t.wMinute);
    return b;
}

// An analogue face: hands as on macOS (a thin neck, then the blade), the
// second hand in orange over the others. numbers: 1-12 inside the rim, else ticks.
void ClockFace(Canvas& c, float cx, float cy, float rad, const SYSTEMTIME& t, const Pal& p, D2D1_COLOR_F face, D2D1_COLOR_F ink, bool numbers,
               const std::wstring& label, D2D1_COLOR_F labelInk)
{
    constexpr float kDeg = std::numbers::pi_v<float> / 180.f;
    c.Circle(cx, cy, rad, face);
    for (int h = 1; h <= 12; ++h) {
        float a = (float(h) * 30.f - 90.f) * kDeg;
        if (numbers) {
            float r = rad * 0.78f, fs = rad * 0.21f;
            std::wstring n = std::to_wstring(h);
            c.Text(n, D2D1::RectF(cx + r * std::cos(a) - fs, cy + r * std::sin(a) - fs * 0.72f, cx + r * std::cos(a) + fs, cy + r * std::sin(a) + fs * 0.72f),
                   fs, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        } else {
            float r0 = rad * 0.80f, r1 = rad * 0.92f;
            c.Capsule(cx + r0 * std::cos(a), cy + r0 * std::sin(a), cx + r1 * std::cos(a), cy + r1 * std::sin(a), Alpha(ink, h % 3 ? 0.45f : 0.9f),
                      std::max(1.f, rad * 0.045f));
        }
    }
    if (!label.empty())
        c.Text(label, D2D1::RectF(cx - rad, cy + rad * 0.26f, cx + rad, cy + rad * 0.26f + rad * 0.2f), rad * 0.15f, labelInk, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    float sec = float(t.wSecond), min = float(t.wMinute) + sec / 60.f, hour = float(t.wHour % 12) + min / 60.f;
    auto hand = [&](float angle, float length, float width) {
        float a = (angle - 90.f) * kDeg, neck = rad * 0.16f;
        float dx = std::cos(a), dy = std::sin(a);
        c.Capsule(cx, cy, cx + dx * neck, cy + dy * neck, ink, std::max(1.2f, width * 0.36f));
        c.Capsule(cx + dx * neck, cy + dy * neck, cx + dx * length, cy + dy * length, ink, width);
    };
    hand(hour * 30.f, rad * 0.52f, std::max(2.5f, rad * 0.075f));
    hand(min * 6.f, rad * 0.80f, std::max(2.5f, rad * 0.075f));
    c.Circle(cx, cy, std::max(2.2f, rad * 0.055f), ink);
    float a = (sec * 6.f - 90.f) * kDeg;
    c.Capsule(cx - std::cos(a) * rad * 0.18f, cy - std::sin(a) * rad * 0.18f, cx + std::cos(a) * rad * 0.86f, cy + std::sin(a) * rad * 0.86f, p.orange,
              std::max(1.f, rad * 0.02f));
    c.Circle(cx, cy, std::max(1.8f, rad * 0.04f), p.orange);
    c.Circle(cx, cy, std::max(0.8f, rad * 0.016f), face);
}

constexpr float kPad = kWidgetPad;

void PaintClock(Canvas& c, WidgetSize size, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float fw, float fh)
{
    if (size == WidgetSize::Small) {
        ClockFace(c, fw / 2, fh / 2, fh / 2 - 9, w.now, p, p.face, p.text, true, w.city.empty() ? std::wstring{} : Tr(w.city.c_str()), p.sub);
        return;
    }
    // The world clock: four faces, dark where it is night.
    const float colW = (fw - 2 * kPad) / 4, rad = 31;
    const std::vector<std::wstring> cities = WidgetCities(WidgetKind::Clock, content.cities);
    for (int i = 0; i < 4; ++i) {
        std::wstring when;
        SYSTEMTIME ct = CityTime(cities[size_t(i)], w.now, when);
        bool night = ct.wHour < 6 || ct.wHour >= 18;
        float cx = kPad + colW * (float(i) + 0.5f), cy = (fh - 2 * rad - 42) / 2 + rad - 2;
        D2D1_COLOR_F face = night ? p.nightFace : p.face, ink = night ? Rgba(0xFFFFFF, 0.96f) : p.text;
        ClockFace(c, cx, cy, rad, ct, p, face, ink, false, {}, ink);
        c.Text(Tr(cities[size_t(i)].c_str()), D2D1::RectF(cx - colW / 2, cy + rad + 8, cx + colW / 2, cy + rad + 26), 12.5f, p.text,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(when, D2D1::RectF(cx - colW / 2, cy + rad + 26, cx + colW / 2, cy + rad + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
}

// Digits instead of hands: the time large, the day under it; the medium one
// lists three more cities on the right.
void PaintDigital(Canvas& c, WidgetSize size, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float fw, float fh)
{
    const int wd = Weekday(w.now.wYear, w.now.wMonth, w.now.wDay);
    const float leftW = size == WidgetSize::Small ? fw - 2 * kPad : 150;
    c.Text(Tr(w.city.empty() ? L"Local" : w.city.c_str()), D2D1::RectF(kPad + 2, kPad, kPad + leftW, kPad + 18), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    // Keep one size all day. Leave one DIP for glyph placement / rounding:
    // tabular figures can still differ slightly at the exact trimming boundary.
    const float fit = c.TextWidth(L"00:00", 50, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float timeW = leftW - 1;
    const float timeSize = fit > timeW ? 50 * timeW / fit : 50.f;
    c.Text(HourMinute(w.now), D2D1::RectF(kPad, kPad + 22, kPad + leftW, kPad + 84), timeSize, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(WeekLong(wd), D2D1::RectF(kPad + 2, fh - kPad - 40, kPad + leftW, fh - kPad - 20), 13, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::wstring dateText = LanguageIsEnglish() ? std::wstring(kMonthsEn[std::clamp(int(w.now.wMonth), 1, 12) - 1]) + L" " + std::to_wstring(w.now.wDay) :
                                                        std::to_wstring(w.now.wMonth) + L"月" + std::to_wstring(w.now.wDay) + L"日";
    c.Text(dateText, D2D1::RectF(kPad + 2, fh - kPad - 20, kPad + leftW, fh - kPad), 13, p.text, DWRITE_FONT_WEIGHT_NORMAL);
    if (size == WidgetSize::Small)
        return;
    const float x0 = kPad + leftW + 8, rowH = (fh - 2 * kPad) / 3;
    const std::vector<std::wstring> cities = WidgetCities(WidgetKind::Digital, content.cities);
    for (int i = 0; i < 3; ++i) {
        std::wstring when;
        SYSTEMTIME ct = CityTime(cities[size_t(i)], w.now, when);
        float y = kPad + rowH * float(i);
        if (i > 0)
            c.Line(x0, y, fw - kPad, y, Alpha(p.text, 0.12f));
        c.Text(Tr(cities[size_t(i)].c_str()), D2D1::RectF(x0, y + 4, fw - kPad - 70, y + rowH / 2 + 2), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(when, D2D1::RectF(x0, y + rowH / 2, fw - kPad - 70, y + rowH - 4), 10.5f, p.sub);
        c.Text(HourMinute(ct), D2D1::RectF(fw - kPad - 80, y, fw - kPad, y + rowH), 22, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

// Batteries: a ring round the computer, the level under it.
void PaintBattery(Canvas& c, const WidgetWorld& w, const Pal& p, float fw, float fh)
{
    const float cx = fw / 2, cy = fh / 2 - 12, rad = 44;
    c.Arc(cx, cy, rad, -90, 270, p.track, 9);
    if (w.status.battery) {
        int pct = std::clamp(w.status.batteryPercent, 0, 100);
        D2D1_COLOR_F level = pct <= 20 && !w.status.charging ? p.red : p.green;
        c.Arc(cx, cy, rad, -90, -90 + 360.f * float(pct) / 100.f, level, 9);
        c.Glyph(L"\uE7F8", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 24, p.text);
        // The level, with a bolt before it while charging.
        std::wstring n = std::to_wstring(pct) + L"%";
        float tw = c.TextWidth(n, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD), bolt = w.status.charging ? 16.f : 0.f, x0 = (fw - tw - bolt) / 2;
        if (w.status.charging)
            c.Glyph(L"\uE945", D2D1::RectF(x0 - 2, cy + rad + 10, x0 + bolt, cy + rad + 36), 13, level);
        c.Text(n, D2D1::RectF(x0 + bolt, cy + rad + 10, x0 + bolt + tw + 4, cy + rad + 36), 19, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    } else {
        c.Arc(cx, cy, rad, -90, 270, p.green, 9);
        c.Glyph(L"\uE83E", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 22, p.text);
        c.Text(Tr(L"Plugged in"), D2D1::RectF(0, cy + rad + 12, fw, cy + rad + 34), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
}

// The processor, the memory and the system disk: bars (small) or rings
// (medium), green, orange from 70 %, red from 90 %.
void PaintSystem(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float fw, float fh)
{
    struct Gauge {
        const wchar_t* name;
        float value;
        std::wstring detail;
    };
    wchar_t mem[32];
    swprintf_s(mem, L"%.1f / %.0f GB", w.memoryGb, w.memoryTotalGb);
    const std::wstring disk = TrFormat(L"%.0f GB free", w.diskFreeGb);
    const Gauge gauges[] = {{L"CPU", w.cpu, Tr(L"CPU")}, {Tr(L"Memory"), w.memory, mem}, {Tr(L"Disk"), w.disk, disk}};
    auto colour = [&](float v) { return v >= 0.9f ? p.red : v >= 0.7f ? p.orange : p.green; };
    auto pct = [](float v) { return std::to_wstring(int(std::lround(std::clamp(v, 0.f, 1.f) * 100))) + L"%"; };
    if (size == WidgetSize::Small) {
        c.Text(Tr(L"System"), D2D1::RectF(kPad + 2, kPad - 2, fw - kPad, kPad + 16), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float rowH = (fh - 2 * kPad - 18) / 3;
        for (int i = 0; i < 3; ++i) {
            float y = kPad + 18 + rowH * float(i);
            c.Text(gauges[i].name, D2D1::RectF(kPad + 2, y + 2, fw / 2, y + 22), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(pct(gauges[i].value), D2D1::RectF(fw / 2, y + 2, fw - kPad - 2, y + 22), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
            D2D1_RECT_F track = D2D1::RectF(kPad + 2, y + 26, fw - kPad - 2, y + 32);
            c.Fill(track, 3, p.track);
            float v = std::clamp(gauges[i].value, 0.f, 1.f);
            if (v > 0.01f)
                c.Fill(D2D1::RectF(track.left, track.top, track.left + std::max(6.f, (track.right - track.left) * v), track.bottom), 3, colour(v));
        }
        return;
    }
    const float colW = (fw - 2 * kPad) / 3, rad = 34, cy = kPad + 6 + rad;
    for (int i = 0; i < 3; ++i) {
        float cx = kPad + colW * (float(i) + 0.5f), v = std::clamp(gauges[i].value, 0.f, 1.f);
        c.Arc(cx, cy, rad, -90, 270, p.track, 8);
        if (v > 0.005f)
            c.Arc(cx, cy, rad, -90, -90 + 360.f * v, colour(v), 8);
        c.Text(pct(v), D2D1::RectF(cx - rad, cy - 12, cx + rad, cy + 12), 17, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(gauges[i].name, D2D1::RectF(cx - colW / 2, cy + rad + 8, cx + colW / 2, cy + rad + 26), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(gauges[i].detail, D2D1::RectF(cx - colW / 2, cy + rad + 26, cx + colW / 2, cy + rad + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
}

// Bluetooth devices' batteries (macOS's Batteries widget): small, four rings, each the device's
// symbol over its level; medium, four rows of symbol, name, bar and level. Red at 20 % or less.
const wchar_t* BluetoothGlyph(BluetoothKind k)
{
    switch (k) {
    case BluetoothKind::Headset: return L"\uE7F6";
    case BluetoothKind::Gamepad: return L"\uE7FC";
    case BluetoothKind::Keyboard: return L"\uE765";
    case BluetoothKind::Mouse: return L"\uE962";
    case BluetoothKind::Pen: return L"\uEDC6";
    case BluetoothKind::Phone: return L"\uE8EA";
    default: return L"\uE702";
    }
}

void PaintBluetooth(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float fw, float fh)
{
    const auto& devices = w.bluetooth;
    const size_t n = std::min<size_t>(devices.size(), 4);
    auto colour = [&](int pct) { return pct <= 20 ? p.red : p.green; };
    if (w.bluetoothRead && n == 0) {
        c.Glyph(L"\uE702", D2D1::RectF(0, fh / 2 - 40, fw, fh / 2 - 4), 26, p.sub);
        c.Text(Tr(L"No connected devices"), D2D1::RectF(kPad, fh / 2 + 4, fw - kPad, fh / 2 + 24), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        return;
    }
    if (size == WidgetSize::Small) {
        const float cw = (fw - 2 * kPad) / 2, ch = (fh - 2 * kPad) / 2, rad = 21;
        for (size_t i = 0; i < 4; ++i) {
            const size_t row = i / 2;
            const float cx = kPad + cw * (float(i % 2) + 0.5f), cy = kPad + ch * float(row) + 26;
            c.Arc(cx, cy, rad, -90, 270, p.track, 5);
            if (i >= n)
                continue;
            const int pct = std::clamp(devices[i].percent, 0, 100);
            if (pct > 0)
                c.Arc(cx, cy, rad, -90, -90 + 360.f * float(pct) / 100.f, colour(pct), 5);
            c.Glyph(BluetoothGlyph(devices[i].kind), D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad), 15, p.text);
            c.Text(std::to_wstring(pct) + L"%", D2D1::RectF(cx - cw / 2, cy + rad + 4, cx + cw / 2, cy + rad + 20), 11.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                   1);
        }
        return;
    }
    const float rowH = (fh - 2 * kPad) / 4;
    for (size_t i = 0; i < n; ++i) {
        const float y = kPad + rowH * float(i), mid = y + rowH / 2;
        const int pct = std::clamp(devices[i].percent, 0, 100);
        c.Glyph(BluetoothGlyph(devices[i].kind), D2D1::RectF(kPad, mid - 12, kPad + 26, mid + 12), 16, p.text);
        c.Text(devices[i].name, D2D1::RectF(kPad + 34, mid - 10, fw - kPad - 112, mid + 10), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const D2D1_RECT_F track = D2D1::RectF(fw - kPad - 106, mid - 3, fw - kPad - 50, mid + 3);
        c.Fill(track, 3, p.track);
        if (pct > 0)
            c.Fill(D2D1::RectF(track.left, track.top, track.left + std::max(6.f, (track.right - track.left) * float(pct) / 100.f), track.bottom), 3,
                   colour(pct));
        c.Text(std::to_wstring(pct) + L"%", D2D1::RectF(fw - kPad - 46, mid - 10, fw - kPad, mid + 10), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

// A note: its title, then the text; while editing, the caret, and the text
// scrolled up when the caret goes below the card.
void PaintNotes(Canvas& c, const WidgetContent& n, const Pal& p, float fw, float fh, std::vector<WidgetHit>& hits)
{
    c.Text(Tr(L"Notes"), D2D1::RectF(kPad + 2, kPad - 2, fw - kPad, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const D2D1_RECT_F box = D2D1::RectF(kPad + 2, kPad + 24, fw - kPad - 2, fh - kPad);
    hits.push_back({D2D1::RectF(0, 0, fw, fh), L"text"});
    const float size = 13.5f;
    if (n.text.empty() && !n.editing) {
        c.Text(Tr(L"Tap to write…"), box, size, p.faint, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        return;
    }
    if (!n.editing) {
        c.Text(n.text, box, size, p.text, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        return;
    }
    D2D1_RECT_F caret = c.Caret(n.text, box, size, DWRITE_FONT_WEIGHT_NORMAL, n.caret);
    float scroll = std::max(0.f, caret.bottom - box.bottom);
    c.Clip(box);
    c.Text(n.text, D2D1::RectF(box.left, box.top - scroll, box.right, box.bottom + scroll + 64.f), size, p.text, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    caret.top -= scroll;
    caret.bottom -= scroll;
    if (n.caretOn)
        c.Fill(caret, 0.75f, p.orange);
    c.Unclip();
    hits.push_back({caret, L"caret"});
}

// Reminders: the title and the count still to do, then the list
// in their original order, each with its circle; a last row adds one.
void PaintReminders(Canvas& c, const WidgetContent& r, const Pal& p, float fw, float fh, std::vector<WidgetHit>& hits)
{
    int open = int(std::count_if(r.items.begin(), r.items.end(), [](const Reminder& i) { return !i.done; }));
    c.Text(Tr(L"Reminders"), D2D1::RectF(kPad + 2, kPad - 2, fw - kPad - 40, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(open), D2D1::RectF(fw - kPad - 48, kPad - 6, fw - kPad - 2, kPad + 22), 22, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    const float bottom = fh - kReminderBottom, view = bottom - kReminderTop;
    const float limit = std::max(0.f, float(r.items.size() + 1) * kReminderRow - view);
    const float scroll = std::clamp(r.scroll, 0.f, limit);
    auto hit = [&](D2D1_RECT_F rect, const std::wstring& id) {
        rect.top = std::max(rect.top, kReminderTop);
        rect.bottom = std::min(rect.bottom, bottom);
        if (rect.bottom > rect.top)
            hits.push_back({rect, id});
    };
    c.Clip(D2D1::RectF(kPad - 4, kReminderTop, fw - kPad, bottom));
    int first = int(scroll / kReminderRow);
    int last = std::min(int(r.items.size()), int(std::ceil((scroll + view) / kReminderRow)));
    for (int idx = first; idx < last; ++idx) {
        const Reminder& it = r.items[size_t(idx)];
        float y = kReminderTop + kReminderRow * float(idx) - scroll;
        float cx = kPad + 9, cy = y + kReminderRow / 2;
        if (it.done) {
            c.Circle(cx, cy, 8, p.orange);
            c.Circle(cx, cy, 3.2f, p.onAccent);
        } else {
            c.Arc(cx, cy, 7.4f, -90, 270, p.sub, 1.5f);
        }
        hit(D2D1::RectF(kPad - 4, y, kPad + 22, y + kReminderRow), L"toggle:" + std::to_wstring(idx));
        D2D1_RECT_F text = D2D1::RectF(kPad + 26, y, fw - kPad, y + kReminderRow);
        bool editing = r.editing && r.item == idx;
        c.Text(it.text.empty() && !editing ? Tr(L"New Reminder") : it.text, text, 13.5f, it.done ? p.sub : it.text.empty() ? p.faint : p.text);
        if (it.done && !it.text.empty())
            c.Line(text.left, cy, std::min(text.right, text.left + c.TextWidth(it.text, 13.5f)), cy, p.sub);
        if (editing) {
            // One line: the caret after the text up to it.
            float x = text.left + c.TextWidth(it.text.substr(0, std::min(r.caret, it.text.size())), 13.5f);
            D2D1_RECT_F caret = D2D1::RectF(std::min(x, text.right - 2), cy - 9, std::min(x, text.right - 2) + 1.5f, cy + 9);
            if (r.caretOn)
                c.Fill(caret, 0.75f, p.orange);
            hit(caret, L"caret");
        }
        hit(text, L"item:" + std::to_wstring(idx));
    }
    if (!r.editing) {
        float y = kReminderTop + kReminderRow * float(r.items.size()) - scroll;
        if (y < bottom) {
            c.Text(L"+", D2D1::RectF(kPad - 1, y, kPad + 19, y + kReminderRow), 17, p.faint, DWRITE_FONT_WEIGHT_NORMAL, 1);
            c.Text(r.items.empty() ? Tr(L"Tap to add a reminder") : Tr(L"New Reminder"), D2D1::RectF(kPad + 26, y, fw - kPad, y + kReminderRow), 13.5f,
                   p.faint);
            hit(D2D1::RectF(kPad - 4, y, fw - kPad, y + kReminderRow), L"add");
        }
    }
    c.Unclip();
    if (limit > 0) {
        float thumb = std::max(18.f, view * view / (view + limit));
        float y = kReminderTop + (view - thumb) * scroll / limit;
        c.Fill(D2D1::RectF(fw - 7, y, fw - 5, y + thumb), 1, p.faint);
    }
}

// A photo filling the card; without one, how to choose a folder.
void PaintPhotos(Canvas& c, const WidgetContent& ph, const Pal& p, float fw, float fh)
{
    if (ph.image && !ph.image->Empty()) {
        c.Image(ph.imageKey, *ph.image, D2D1::RectF(0, 0, fw, fh)); // the widget masks its complete content once
        return;
    }
    c.Glyph(L"\uE91B", D2D1::RectF(0, fh / 2 - 46, fw, fh / 2 - 6), 30, p.sub);
    c.Text(Tr(L"Photos"), D2D1::RectF(0, fh / 2, fw, fh / 2 + 20), 14, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(Tr(L"Right-click to choose a photo folder"), D2D1::RectF(0, fh / 2 + 20, fw, fh / 2 + 38), 11.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
}

// The material's face under the content and the ink over it. Faces cover the largest size: while
// a widget resizes its viewport outgrows the size it lays out.
Pal Face(Canvas& c, WidgetKind kind, WidgetMaterial material, const WidgetWorld& world, float fh)
{
    if (material == WidgetMaterial::Glass)
        return GlassPal(c.theme);
    // One ink everywhere: the face, not the desktop, decides it.
    c.ink = {};
    const D2D1_RECT_F all = D2D1::RectF(0, 0, WidgetWidth(WidgetSize::Large), WidgetHeight(WidgetSize::Large));
    if (material == WidgetMaterial::Dusk) {
        c.theme = Theme::For(false);
        c.theme.halo = Rgba(0x000000, 0.5f); // a deeper shadow under the white keeps it legible over bright glass
        c.inkDark = 0;
        c.Fill(all, 0, Rgba(0x000000, 0.22f)); // a light veil: the glass stays clear
        return DuskPal();
    }
    c.halo = false; // an opaque face needs none
    if (kind == WidgetKind::Weather) {
        c.theme = Theme::For(false);
        c.inkDark = 0;
        auto [top, bottom] = WeatherSkyFace(world.weather);
        c.Fill(all, 0, Rgba(bottom, 1));
        c.Gradient(D2D1::RectF(0, 0, all.right, fh), 0, Rgba(top, 1), Rgba(bottom, 1));
        Pal p = GlassPal(c.theme);
        p.icon = Ink::Light;
        return p;
    }
    c.theme = Theme::For(true);
    c.inkDark = 1;
    c.Fill(all, 0, Rgba(0xFFFFFF, 1));
    return PaperPal();
}

} // namespace

float ReminderScrollMax(const WidgetContent& content, WidgetSize size)
{
    return std::max(0.f, float(content.items.size() + 1) * kReminderRow - (WidgetHeight(size) - kReminderTop - kReminderBottom));
}

bool WidgetSupports(WidgetKind kind, WidgetSize size)
{
    switch (kind) {
    case WidgetKind::Battery: return size == WidgetSize::Small;
    case WidgetKind::Bluetooth:
    case WidgetKind::Network:
    case WidgetKind::Clock:
    case WidgetKind::Digital:
    case WidgetKind::System: return size != WidgetSize::Large;
    default: return true;
    }
}

void PaintWidget(Canvas& c, WidgetKind kind, WidgetSize size, WidgetMaterial material, const WidgetContent& content, const WidgetWorld& world,
                 std::vector<WidgetHit>& hits)
{
    const float fw = WidgetWidth(size), fh = WidgetHeight(size);
    const Pal p = Face(c, kind, material, world, fh);
    hits.clear();
    switch (kind) {
    case WidgetKind::Clock: PaintClock(c, size, content, world, p, fw, fh); break;
    case WidgetKind::Digital: PaintDigital(c, size, content, world, p, fw, fh); break;
    case WidgetKind::Calendar: PaintCalendarFace(c, size, world, p, fw, fh); break;
    case WidgetKind::Battery: PaintBattery(c, world, p, fw, fh); break;
    case WidgetKind::System: PaintSystem(c, size, world, p, fw, fh); break;
    case WidgetKind::Notes: PaintNotes(c, content, p, fw, fh, hits); break;
    case WidgetKind::Reminders: PaintReminders(c, content, p, fw, fh, hits); break;
    case WidgetKind::Photos: PaintPhotos(c, content, p, fw, fh); break;
    case WidgetKind::Weather: PaintWeatherFace(c, size, world, p, fw, fh); break;
    case WidgetKind::Bluetooth: PaintBluetooth(c, size, world, p, fw, fh); break;
    case WidgetKind::Network: PaintNetworkFace(c, size, world, p, fw, fh); break;
    }
}

} // namespace atrium
