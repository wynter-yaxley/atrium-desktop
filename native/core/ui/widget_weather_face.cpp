#include "ui/widget_faces_internal.h"

#include "localization.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace atrium {

namespace {

// What the sky does, for the icon and the widget's tint.
enum class Sky { Clear, Partly, Cloudy, Fog, Rain, Snow, Storm };

Sky SkyOf(int code)
{
    if (code <= 1)
        return Sky::Clear;
    if (code == 2)
        return Sky::Partly;
    if (code == 3)
        return Sky::Cloudy;
    if (code == 45 || code == 48)
        return Sky::Fog;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return Sky::Snow;
    if (code >= 95)
        return Sky::Storm;
    return Sky::Rain;
}

// The symbols' colours over each ink. White that is not the glass's ink stays white over bright
// glass (InkAt only switches the theme's own ink).
struct IconInk {
    D2D1_COLOR_F sun, moon, cloud, grey, rain, arrow;
};

IconInk IconColors(WidgetInk ink)
{
    switch (ink) {
    case WidgetInk::Dark:
        return {WidgetRgba(0xFFD60A, 1), WidgetRgba(0xF5C451, 1), WidgetRgba(0xA4ADB8, 1),
                WidgetRgba(0x7D8794, 1), WidgetRgba(0x64D2FF, 1), WidgetRgba(0x6E7681, 1)};
    case WidgetInk::Mono:
        return {WidgetRgba(0xFFFFFF, 0.72f), WidgetRgba(0xFFFFFF, 0.9f),  WidgetRgba(0xFFFFFF, 0.96f),
                WidgetRgba(0xFFFFFF, 0.8f),  WidgetRgba(0xFFFFFF, 0.85f), WidgetRgba(0xFFFFFF, 0.96f)};
    default:
        return {WidgetRgba(0xFFD60A, 1), WidgetRgba(0xF2F2F7, 1), WidgetRgba(0xF7F8FA, 1),
                WidgetRgba(0xD1D5DB, 1), WidgetRgba(0x64D2FF, 1), WidgetRgba(0xF2F5F9, 1)};
    }
}

// A cloud filling (about) a box s wide, its middle at (cx, cy).
void Cloud(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F ink)
{
    c.Fill(D2D1::RectF(cx - 0.42f * s, cy - 0.02f * s, cx + 0.42f * s, cy + 0.26f * s), 0.14f * s, ink);
    c.Circle(cx - 0.17f * s, cy + 0.02f * s, 0.19f * s, ink);
    c.Circle(cx + 0.08f * s, cy - 0.06f * s, 0.25f * s, ink);
    c.Circle(cx + 0.29f * s, cy + 0.09f * s, 0.15f * s, ink);
}

void Sun(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F gold)
{
    constexpr float kDeg = std::numbers::pi_v<float> / 180.f;
    c.Circle(cx, cy, 0.2f * s, gold);
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 45.f * kDeg;
        c.Capsule(cx + std::cos(a) * 0.31f * s, cy + std::sin(a) * 0.31f * s, cx + std::cos(a) * 0.43f * s, cy + std::sin(a) * 0.43f * s, gold,
                  std::max(1.f, 0.075f * s));
    }
}

void Moon(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F ink) { c.Crescent(cx, cy, 0.3f * s, 0.17f * s, -0.12f * s, 0.25f * s, ink); }

void PaintSunEvent(Canvas& c, float cx, float cy, float s, int event, const IconInk& k)
{
    c.Clip(D2D1::RectF(cx - s, cy - s, cx + s, cy + 0.12f * s));
    Sun(c, cx, cy + 0.12f * s, s * 0.9f, k.sun);
    c.Unclip();
    c.Capsule(cx - 0.42f * s, cy + 0.2f * s, cx + 0.42f * s, cy + 0.2f * s, k.sun, std::max(1.f, 0.07f * s));
    const float y0 = cy - 0.46f * s, y1 = cy - 0.2f * s, w = std::max(1.f, 0.07f * s);
    c.Capsule(cx, y0, cx, y1, k.arrow, w);
    float tip = event == 1 ? y0 : y1, back = event == 1 ? y0 + 0.1f * s : y1 - 0.1f * s;
    c.Capsule(cx - 0.1f * s, back, cx, tip, k.arrow, w);
    c.Capsule(cx + 0.1f * s, back, cx, tip, k.arrow, w);
}

void PaintPrecipitation(Canvas& c, Sky sky, float cx, float cy, float s, WidgetInk ink, const IconInk& k)
{
    const bool snow = sky == Sky::Snow, storm = sky == Sky::Storm;
    Cloud(c, cx, cy - 0.16f * s, s, storm ? k.grey : k.cloud);
    for (int i = 0; i < 3; ++i) {
        float x = cx - 0.2f * s + float(i) * 0.2f * s, y = cy + 0.24f * s;
        if (snow)
            c.Circle(x, y + (i % 2 ? 0.08f : 0.f) * s, 0.055f * s, k.cloud);
        else if (!storm || i != 1)
            c.Capsule(x + 0.03f * s, y - 0.04f * s, x - 0.04f * s, y + 0.12f * s, k.rain, std::max(1.f, 0.07f * s));
    }
    if (storm) {
        const D2D1_COLOR_F bolt = ink == WidgetInk::Mono ? k.arrow : k.sun;
        const float w = std::max(1.f, 0.08f * s);
        c.Capsule(cx + 0.04f * s, cy + 0.08f * s, cx - 0.06f * s, cy + 0.26f * s, bolt, w);
        c.Capsule(cx - 0.06f * s, cy + 0.26f * s, cx + 0.05f * s, cy + 0.26f * s, bolt, w);
        c.Capsule(cx + 0.05f * s, cy + 0.26f * s, cx - 0.05f * s, cy + 0.44f * s, bolt, w);
    }
}

// macOS's weather symbols in their colours: a yellow sun, a pale moon, white clouds, blue rain
// (IconColors: grey clouds over bright glass, all white on the dimmed glass).
// event: 1 a sunrise, 2 a sunset (a half sun on the horizon, an arrow up or down).
void WeatherIcon(Canvas& c, int code, bool day, float cx, float cy, float s, int event, WidgetInk ink)
{
    const IconInk k = IconColors(ink);
    if (event) {
        PaintSunEvent(c, cx, cy, s, event, k);
        return;
    }
    const Sky sky = SkyOf(code);
    switch (sky) {
    case Sky::Clear: day ? Sun(c, cx, cy, s, k.sun) : Moon(c, cx, cy, s, k.moon); break;
    case Sky::Partly:
        day ? Sun(c, cx - 0.16f * s, cy - 0.14f * s, s * 0.78f, k.sun) : Moon(c, cx - 0.14f * s, cy - 0.14f * s, s * 0.8f, k.moon);
        Cloud(c, cx + 0.08f * s, cy + 0.06f * s, s * 0.82f, k.cloud);
        break;
    case Sky::Cloudy: Cloud(c, cx, cy - 0.04f * s, s, k.cloud); break;
    case Sky::Fog:
        for (int i = 0; i < 3; ++i) {
            float y = cy - 0.2f * s + float(i) * 0.2f * s, inset = i == 1 ? 0.f : 0.08f * s;
            c.Capsule(cx - 0.4f * s + inset, y, cx + 0.4f * s - inset, y, k.cloud, std::max(1.f, 0.09f * s));
        }
        break;
    case Sky::Rain:
    case Sky::Snow:
    case Sky::Storm: PaintPrecipitation(c, sky, cx, cy, s, ink, k); break;
    }
}

std::wstring Degrees(float t) { return std::to_wstring(int(std::lround(t))) + L"°"; }

// A day's span of the week's temperatures: blue when cold, through green and yellow, orange when hot.
D2D1_COLOR_F TempColor(float t)
{
    struct Stop {
        float t;
        uint32_t rgb;
    };
    const Stop stops[] = {{-5, 0x5AC8FA}, {8, 0x63E6BE}, {18, 0xFFD60A}, {28, 0xFF9F0A}, {36, 0xFF453A}};
    if (t <= stops[0].t)
        return WidgetRgba(stops[0].rgb, 1);
    for (size_t i = 1; i < std::size(stops); ++i)
        if (t <= stops[i].t) {
            float k = (t - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
            D2D1_COLOR_F a = WidgetRgba(stops[i - 1].rgb, 1), b = WidgetRgba(stops[i].rgb, 1);
            return D2D1::ColorF(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, 1);
        }
    return WidgetRgba(stops[std::size(stops) - 1].rgb, 1);
}

void PaintWeatherHourly(Canvas& c, const Weather& w, const WidgetPal& p, float fw, float pad, float hy, WidgetInk ink)
{
    const float colW = (fw - 2 * pad) / 6;
    for (size_t i = 0; i < w.hours.size() && i < 6; ++i) {
        const WeatherHour& h = w.hours[i];
        float x0 = pad + colW * float(i), cx = x0 + colW / 2;
        c.Text(h.label, D2D1::RectF(x0 - 4, hy, x0 + colW + 4, hy + 16), 11.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        WeatherIcon(c, h.code, h.day, cx, hy + 31, 22, h.event, ink);
        const std::wstring val = h.event ? (h.event == 1 ? Tr(L"Sunrise") : Tr(L"Sunset")) : Degrees(h.temp);
        c.Text(val, D2D1::RectF(x0 - 4, hy + 46, x0 + colW + 4, hy + 64), h.event ? 12.f : 13.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
}

void PaintWeatherDaily(Canvas& c, const Weather& w, const WidgetPal& p, float fw, float fh, float pad, float top, WidgetInk ink)
{
    const bool dark = ink == WidgetInk::Dark;
    const size_t n = std::min<size_t>(w.days.size(), 5);
    float weekLo = 1e9f, weekHi = -1e9f;
    for (size_t i = 0; i < n; ++i)
        weekLo = std::min(weekLo, w.days[i].lo), weekHi = std::max(weekHi, w.days[i].hi);
    const float rowH = (fh - 12 - top) / float(std::max<size_t>(n, 1));
    const float barL = 170, barR = fw - pad - 44, span = std::max(1.f, weekHi - weekLo);
    for (size_t i = 0; i < n; ++i) {
        const WeatherDay& d = w.days[i];
        float y = top + rowH * float(i), cy = y + rowH / 2;
        c.Text(d.label, D2D1::RectF(pad, y, pad + 60, y + rowH), 14, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        WeatherIcon(c, d.code, true, pad + 84, cy, 22, 0, ink);
        c.Text(Degrees(d.lo), D2D1::RectF(barL - 46, y, barL - 8, y + rowH), 14, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
        c.Fill(D2D1::RectF(barL, cy - 2.5f, barR, cy + 2.5f), 2.5f, p.track);
        float x0 = barL + (barR - barL) * (d.lo - weekLo) / span, x1 = barL + (barR - barL) * (d.hi - weekLo) / span;
        x1 = std::max(x1, x0 + 5);
        if (ink == WidgetInk::Mono)
            c.Fill(D2D1::RectF(x0, cy - 2.5f, x1, cy + 2.5f), 2.5f, p.text);
        else
            c.Gradient(D2D1::RectF(x0, cy - 2.5f, x1, cy + 2.5f), 2.5f, TempColor(d.lo), TempColor(d.hi), false);
        if (i == 0) {
            // Today: where it is now on the span.
            float x = barL + (barR - barL) * (std::clamp(w.temp, d.lo, d.hi) - weekLo) / span;
            c.Circle(x, cy, 4.5f, dark ? WidgetRgba(0xFFFFFF, 1) : WidgetRgba(0x1C2333, 0.55f));
            c.Circle(x, cy, 3.2f, dark ? WidgetRgba(0x1C2333, 0.9f) : WidgetRgba(0xFFFFFF, 1));
        }
        c.Text(Degrees(d.hi), D2D1::RectF(barR + 6, y, fw - pad, y + rowH), 14, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

} // namespace

// The full-colour weather widget's sky, top and bottom: blue by day, navy by night, greyer
// under clouds, rain and fog (as macOS's Weather).
std::pair<uint32_t, uint32_t> WeatherSkyFace(const Weather& w)
{
    if (!w.ok)
        return {0x2F7BD6, 0x63A9EA};
    switch (SkyOf(w.code)) {
    case Sky::Clear:
    case Sky::Partly: return w.day ? std::pair<uint32_t, uint32_t>{0x2F7BD6, 0x63A9EA} : std::pair<uint32_t, uint32_t>{0x0C1631, 0x2A3E68};
    case Sky::Cloudy:
    case Sky::Fog: return w.day ? std::pair<uint32_t, uint32_t>{0x6A7E92, 0x98A8B8} : std::pair<uint32_t, uint32_t>{0x262F3C, 0x47525F};
    case Sky::Snow: return w.day ? std::pair<uint32_t, uint32_t>{0x7F95AC, 0xAFC0D0} : std::pair<uint32_t, uint32_t>{0x2B3646, 0x4C5868};
    default: // rain, storms
        return w.day ? std::pair<uint32_t, uint32_t>{0x4A5D70, 0x728699} : std::pair<uint32_t, uint32_t>{0x1D2530, 0x3A4552};
    }
}

// macOS's weather widget: the place, the temperature large, the sky and the day's high and low;
// the medium one adds the next hours, the large one the next days with their spans.
void PaintWeatherFace(Canvas& c, WidgetSize size, const WidgetWorld& world, const WidgetPal& p, float fw, float fh)
{
    const Weather& w = world.weather;
    const WidgetInk ink = p.icon;
    const float pad = 16;
    const std::wstring city = w.city.empty() ? (world.city.empty() ? Tr(L"Weather") : world.city) : w.city;
    c.Text(city, D2D1::RectF(pad, 12, fw - pad - (size == WidgetSize::Small ? 0 : 120), 32), 14.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!w.ok) {
        c.Text(w.error.empty() ? Tr(L"Fetching weather…") : w.error, D2D1::RectF(pad, fh / 2 - 10, fw - pad, fh / 2 + 10), 13, p.sub,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, size == WidgetSize::Small ? 0 : 1);
        return;
    }
    c.Text(Degrees(w.temp), D2D1::RectF(pad - 2, 30, fw / 2 + 40, 82), 44, p.text, DWRITE_FONT_WEIGHT_NORMAL);
    const std::wstring range = TrFormat(L"H %s  L %s", Degrees(w.hi).c_str(), Degrees(w.lo).c_str());
    if (size == WidgetSize::Small) {
        WeatherIcon(c, w.code, w.day, pad + 11, fh - 62, 22, 0, ink);
        c.Text(WeatherCondition(w.code), D2D1::RectF(pad, fh - 48, fw - pad, fh - 30), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(range, D2D1::RectF(pad, fh - 30, fw - pad, fh - 12), 12, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        return;
    }
    // The sky and the day's span at the top right.
    WeatherIcon(c, w.code, w.day, fw - pad - 10, 24, 20, 0, ink);
    c.Text(WeatherCondition(w.code), D2D1::RectF(fw / 2, 40, fw - pad, 58), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    c.Text(range, D2D1::RectF(fw / 2 - 20, 58, fw - pad, 76), 12.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    // The next hours.
    const float hy = size == WidgetSize::Large ? 92.f : 90.f;
    if (size == WidgetSize::Large)
        c.Line(pad, hy - 6, fw - pad, hy - 6, p.track);
    PaintWeatherHourly(c, w, p, fw, pad, hy, ink);
    if (size != WidgetSize::Large)
        return;
    // The next days: each one's low, its span on the week's scale, its high.
    const float top = hy + 76;
    c.Line(pad, top - 6, fw - pad, top - 6, p.track);
    PaintWeatherDaily(c, w, p, fw, fh, pad, top, ink);
}

} // namespace atrium
