// Shared internal palette, locale helpers, and face entry points across widget face translation units.
#pragma once

#include "ui/widget_faces.h"

#include <utility>

namespace atrium {

// What the weather's symbols are drawn over: light ink (clear glass, a sky), dark ink (bright
// glass: grey clouds, a gold moon), or the dimmed glass, where everything is white.
enum class WidgetInk { Light, Dark, Mono };

// The colours of a widget: its ink, and macOS's accents.
struct WidgetPal {
    D2D1_COLOR_F text, sub, faint, face, red, orange, green, track, blue; // blue: the calendar's accent
    D2D1_COLOR_F onAccent;  // ink on an accent's disc (today, a done reminder)
    D2D1_COLOR_F nightFace; // the world clock's faces where it is night
    WidgetInk icon;
};

inline constexpr float kWidgetPad = 14.f;

inline D2D1_COLOR_F WidgetRgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }
inline D2D1_COLOR_F WidgetAlpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

const wchar_t* WidgetWeekLong(int wd);
const wchar_t* WidgetWeekShort(int wd);
const wchar_t* WidgetMonthName(int m);

void PaintCalendarFace(Canvas& c, WidgetSize size, const WidgetWorld& w, const WidgetPal& p, float fw, float fh);
void PaintWeatherFace(Canvas& c, WidgetSize size, const WidgetWorld& world, const WidgetPal& p, float fw, float fh);
std::pair<uint32_t, uint32_t> WeatherSkyFace(const Weather& w);
void PaintNetworkFace(Canvas& c, WidgetSize size, const WidgetWorld& w, const WidgetPal& p, float fw, float fh);

} // namespace atrium
