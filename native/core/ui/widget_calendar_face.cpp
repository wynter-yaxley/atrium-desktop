#include "ui/widget_faces_internal.h"

#include "localization.h"

#include <algorithm>

namespace atrium {

namespace {

int FirstWeekday()
{
    DWORD fd = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&fd), 2))
        return int(fd % 7);
    return 0;
}

std::wstring CalendarLargeHead(const WidgetWorld& w, const SYSTEMTIME& now)
{
    std::wstring lunarToday = w.lunar ? w.lunar->MonthDay(now.wYear, now.wMonth, now.wDay) : std::wstring();
    if (LanguageIsEnglish())
        return std::to_wstring(now.wYear) + (lunarToday.empty() ? std::wstring() : L" · " + lunarToday);
    return std::to_wstring(now.wYear) + L"年" + (lunarToday.empty() ? std::wstring() : L" · 农历" + lunarToday);
}

void PaintCalendarLargeCell(Canvas& c, const WidgetWorld& w, const WidgetPal& p, int first, int lead, int d, float gx, float y, float cw, float rh)
{
    const SYSTEMTIME& now = w.now;
    const int cell = lead + d - 1, row = cell / 7;
    const float x0 = gx + float(cell % 7) * cw, y0 = y + float(row) * rh, cx = x0 + cw / 2;
    const bool today = d == now.wDay, weekend = (first + cell % 7) % 7 >= 5;
    if (today)
        c.Fill(D2D1::RectF(cx - 18, y0 + 1, cx + 18, y0 + rh - 1), 10, p.blue);
    const D2D1_COLOR_F ink = today ? p.onAccent : weekend ? p.sub : p.text;
    c.Text(std::to_wstring(d), D2D1::RectF(x0, y0, x0 + cw, y0 + rh * 0.56f), 14, ink, today ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    bool festival = false;
    const std::wstring label = w.lunar ? w.lunar->DayLabel(now.wYear, now.wMonth, WORD(d), &festival) : std::wstring();
    const D2D1_COLOR_F lunarInk = today ? WidgetAlpha(p.onAccent, 0.85f) : festival ? p.blue : p.sub;
    c.Text(label, D2D1::RectF(x0, y0 + rh * 0.48f, x0 + cw, y0 + rh - 5), 9, lunarInk, DWRITE_FONT_WEIGHT_NORMAL, 1);
}

void PaintCalendarLarge(Canvas& c, const WidgetWorld& w, const WidgetPal& p, int first, int lead, int days, float fw, float fh)
{
    const SYSTEMTIME& now = w.now;
    c.Text(WidgetMonthName(now.wMonth), D2D1::RectF(kWidgetPad + 4, kWidgetPad, fw / 2, kWidgetPad + 22), 16, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(CalendarLargeHead(w, now), D2D1::RectF(fw / 2 - 40, kWidgetPad, fw - kWidgetPad - 4, kWidgetPad + 22), 11.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 2);
    const float gx = kWidgetPad, gw = fw - 2 * kWidgetPad, cw = gw / 7;
    float y = kWidgetPad + 28;
    for (int i = 0; i < 7; ++i)
        c.Text(WidgetWeekShort((first + i) % 7), D2D1::RectF(gx + float(i) * cw, y, gx + float(i + 1) * cw, y + 16), 10.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD,
               1);
    y += 20;
    const int rows = (lead + days + 6) / 7;
    const float rh = (fh - kWidgetPad - y) / float(std::max(rows, 5));
    for (int d = 1; d <= days; ++d)
        PaintCalendarLargeCell(c, w, p, first, lead, d, gx, y, cw, rh);
}

void PaintCalendarLeft(Canvas& c, const WidgetWorld& w, const WidgetPal& p, float leftW, float fh)
{
    const SYSTEMTIME& now = w.now;
    const int wd = Weekday(now.wYear, now.wMonth, now.wDay);
    c.Text(WidgetWeekLong(wd), D2D1::RectF(kWidgetPad + 2, kWidgetPad, kWidgetPad + leftW, kWidgetPad + 20), 14, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(now.wDay), D2D1::RectF(kWidgetPad, kWidgetPad + 16, kWidgetPad + leftW, kWidgetPad + 80), 54, p.text, DWRITE_FONT_WEIGHT_NORMAL);
    bool festival = false;
    const std::wstring label = w.lunar ? w.lunar->DayLabel(now.wYear, now.wMonth, now.wDay, &festival) : std::wstring();
    const std::wstring lunar = w.lunar ? w.lunar->MonthDay(now.wYear, now.wMonth, now.wDay) : std::wstring();
    const std::wstring lunarLine = LanguageIsEnglish() ? lunar : (lunar.empty() ? std::wstring() : L"农历" + lunar);
    c.Text(lunar.empty() ? std::wstring(WidgetMonthName(now.wMonth)) : lunarLine,
           D2D1::RectF(kWidgetPad + 2, fh - kWidgetPad - 40, kWidgetPad + leftW, fh - kWidgetPad - 20), 12.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::wstring yearMonth = LanguageIsEnglish() ? std::to_wstring(now.wYear) + L" " + WidgetMonthName(now.wMonth) :
                                                         std::to_wstring(now.wYear) + L"年" + WidgetMonthName(now.wMonth);
    c.Text(festival ? label : yearMonth, D2D1::RectF(kWidgetPad + 2, fh - kWidgetPad - 20, kWidgetPad + leftW, fh - kWidgetPad), 12, festival ? p.blue : p.sub,
           DWRITE_FONT_WEIGHT_NORMAL);
}

void PaintCalendarCompactMonth(Canvas& c, const SYSTEMTIME& now, const WidgetPal& p, int first, int lead, int days, float gx, float gw, float fh)
{
    // The month: weekday initials, the days, today in a red disc.
    const float cw = gw / 7;
    c.Text(WidgetMonthName(now.wMonth), D2D1::RectF(gx + 4, kWidgetPad - 1, gx + gw, kWidgetPad + 17), 12.5f, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float y = kWidgetPad + 20;
    for (int i = 0; i < 7; ++i)
        c.Text(WidgetWeekShort((first + i) % 7), D2D1::RectF(gx + float(i) * cw, y, gx + float(i + 1) * cw, y + 14), 9.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD,
               1);
    y += 16;
    const int rows = (lead + days + 6) / 7;
    const float rh = std::min(17.f, (fh - kWidgetPad - y) / float(rows));
    for (int d = 1; d <= days; ++d) {
        const int cell = lead + d - 1, row = cell / 7;
        const float x0 = gx + float(cell % 7) * cw, y0 = y + float(row) * rh, cx = x0 + cw / 2, cy = y0 + rh / 2;
        const bool today = d == now.wDay;
        if (today)
            c.Circle(cx, cy, rh / 2 + 0.5f, p.blue);
        const bool weekend = (first + cell % 7) % 7 >= 5;
        const D2D1_COLOR_F ink = today ? p.onAccent : weekend ? p.sub : p.text;
        // MiSans centres its figures in the line box (ascent 1.044, descent 0.282, figures 0.76
        // high): the box centred on the disc centres the digits.
        c.Text(std::to_wstring(d), D2D1::RectF(x0, cy - rh / 2, x0 + cw, cy + rh / 2), 10.5f, ink,
               today ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
}

} // namespace

// The day: the weekday in red, the date large, the lunar date (a festival in
// red); the medium one adds the month, the large one is the month with the
// lunar day under every date.
void PaintCalendarFace(Canvas& c, WidgetSize size, const WidgetWorld& w, const WidgetPal& p, float fw, float fh)
{
    const SYSTEMTIME& now = w.now;
    const int first = FirstWeekday();
    const int lead = (Weekday(now.wYear, now.wMonth, 1) - first + 7) % 7, days = DaysIn(now.wYear, now.wMonth);
    if (size == WidgetSize::Large) {
        PaintCalendarLarge(c, w, p, first, lead, days, fw, fh);
        return;
    }
    const float leftW = size == WidgetSize::Small ? fw - 2 * kWidgetPad : 126;
    PaintCalendarLeft(c, w, p, leftW, fh);
    if (size == WidgetSize::Small)
        return;
    const float gx = kWidgetPad + leftW + 6, gw = fw - kWidgetPad - gx;
    PaintCalendarCompactMonth(c, now, p, first, lead, days, gx, gw, fh);
}

} // namespace atrium
