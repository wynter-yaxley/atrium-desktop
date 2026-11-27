// The calendar, opened from the bar's clock: a month on a piece of the bar's
// glass (the Chinese lunar date under each day, festivals in the accent
// colour), and below it a capsule shaped like the bar with the time and the
// selected day. Drawn by Atrium instead of the system's calendar flyout; the
// lunar calendar comes from Windows' ICU.
#pragma once

#include "motion.h"
#include "shell/lunar.h"
#include "ui/surface.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace atrium {

inline constexpr const wchar_t* kCalendarWeekNames[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
inline constexpr const wchar_t* kCalendarMonthsEn[] = {L"January", L"February", L"March",     L"April",   L"May",      L"June",
                                                       L"July",    L"August",   L"September", L"October", L"November", L"December"};
inline constexpr const wchar_t* kCalendarWeekShortEn[] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};

inline std::wstring CalendarCapsuleDate(int year, int month, int day, int weekday, int currentYear, bool english)
{
    const int wd = std::clamp(weekday, 0, 6);
    if (english)
        return std::wstring(kCalendarWeekShortEn[wd]) + L", " + kCalendarMonthsEn[std::clamp(month, 1, 12) - 1] + L" " + std::to_wstring(day) +
               (year != currentYear ? L", " + std::to_wstring(year) : std::wstring());
    return (year != currentYear ? std::to_wstring(year) + L"年" : std::wstring()) + std::to_wstring(month) + L"月" + std::to_wstring(day) + L"日 星期" +
           kCalendarWeekNames[wd];
}

class CalendarPanel {
public:
    explicit CalendarPanel(HINSTANCE instance);

    // anchor: the clock on screen; bar: the bar's glass shape.
    void Open(RECT anchor, RECT bar);
    void Close();
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void SetRadius(float dip)
    {
        radius_ = dip;
        surface_.SetRadius(dip);
    }
    void SetBarHeight(float dip) { barH_ = dip; }

private:
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    struct Day {
        int y, m, d;
        std::wstring lunar; // the lunar day, or the festival
        bool festival = false;
    };
    void Paint(Canvas& c);
    void PaintMonth(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintNavButton(Canvas& c, const Theme& t, const std::wstring& id, D2D1_RECT_F b, const std::wstring& glyph, const std::wstring& label);
    void PaintMonthHeader(Canvas& c, const Theme& t, D2D1_RECT_F card, const SYSTEMTIME& now);
    void PaintDayCell(Canvas& c, const Theme& t, const SYSTEMTIME& now, size_t index, D2D1_RECT_F cell);
    void PaintMonthGrid(Canvas& c, const Theme& t, D2D1_RECT_F card, const SYSTEMTIME& now);
    void PaintMonthFooter(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintCapsule(Canvas& c, const Theme& t, D2D1_RECT_F cap);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    std::wstring HitAt(POINT pt) const;
    void Activate(const std::wstring& id);
    void ShowMonth(int y, int m, int direction);
    void Place();
    float Hover(Canvas& c, const std::wstring& id, bool on);
    D2D1_SIZE_F Size() const;
    Surface surface_;
    ChineseCalendar lunar_;
    int firstWeekday_ = 0; // 0 Monday .. 6 Sunday
    int year_ = 0, month_ = 0;      // shown
    SYSTEMTIME selected_{};          // the selected day (today when opened)
    std::vector<Day> days_;          // 42 cells from the first shown day
    std::vector<Hit> hits_;
    std::wstring hover_, pressed_;
    bool tracking_ = false;
    RECT anchor_{}, bar_{};
    float radius_ = 30, barH_ = 60;
    ULONGLONG lastClosed_ = 0;
    std::map<std::wstring, motion::Follow> hoverAnim_;
    double monthMs_ = 0;
    int monthDir_ = 0;
};

} // namespace atrium
