// Internal types, constants, and helpers shared across desktop widget modules.
#pragma once

#include "ui/desktop_widgets.h"

#include <string>
#include <vector>

namespace atrium {

struct DesktopWidgets::Widget {
    WidgetKind kind = WidgetKind::Clock;
    WidgetSize size = WidgetSize::Small;
    int col = 0, row = 0, downCol = 0;
    std::unique_ptr<Surface> surface;
    bool down = false, dragging = false;
    bool hover = false;    // the mouse is over it: the resize grip shows
    bool tracking = false; // TrackMouseEvent asked for its WM_MOUSELEAVE
    bool resizing = false; // dragged by the grip
    bool growLeft = false; // the selected grip is at the bottom left
    WidgetSize downSize = WidgetSize::Small;
    POINT downAt{};
    RECT downRect{}, glideFrom{}, glideTo{};
    double glideMs = 0;
    WidgetContent content;
    std::vector<WidgetHit> hits; // of the last paint
    ULONGLONG photoAt = 0;       // when the photo was last changed
    unsigned photoGen = 0;       // the photo being read
};

namespace widget_detail {

inline constexpr float kMargin = 24;
inline constexpr float kGrip = 30; // DIP: the corner that resizes a widget when dragged
inline constexpr double kGlideMs = 240;
inline constexpr UINT_PTR kTickTimer = 1, kGlideTimer = 3, kSafetyTimer = 4, kCaretTimer = 5, kSaveTimer = 6;
inline constexpr UINT kCaretMs = 530, kSaveMs = 800;
inline constexpr ULONGLONG kPhotoMs = 10 * 60 * 1000; // a new photo every ten minutes
inline constexpr size_t kMaxNoteText = 8192, kMaxReminderText = 512;
inline constexpr const wchar_t* kHostClass = L"AtriumWidgetsHost";
inline constexpr UINT WM_WIDGET_REMOVE = WM_APP + 1;    // lParam Widget*: removed once its own message is done
inline constexpr UINT WM_WIDGET_RESTACK = WM_APP + 2;   // a burst of window events, one restack
inline constexpr UINT WM_WIDGET_PHOTO = WM_APP + 3;     // lParam PhotoRead*: a photo was read
inline constexpr UINT WM_WIDGET_WEATHER = WM_APP + 4;   // a new forecast (WeatherSource::Latest)
inline constexpr UINT WM_WIDGET_BLUETOOTH = WM_APP + 5; // the Bluetooth batteries changed (BluetoothBatteries::Latest)

struct PhotoRead {
    void* widget;
    unsigned gen;
    std::wstring path;
    IconImage image;
};

const wchar_t* NameOf(WidgetKind kind);
bool ParseKind(const std::wstring& name, WidgetKind& kind);
std::wstring ClampText(std::wstring text, size_t max);
std::wstring LocalCity();

inline const wchar_t* SizeName(WidgetSize s) { return s == WidgetSize::Small ? L"small" : s == WidgetSize::Medium ? L"medium" : L"large"; }
inline WidgetSize ParseSize(const std::wstring& s) { return s == L"large" ? WidgetSize::Large : s == L"medium" ? WidgetSize::Medium : WidgetSize::Small; }
inline bool Editable(WidgetKind k) { return k == WidgetKind::Notes || k == WidgetKind::Reminders; }
inline bool Resizable(WidgetKind k) { return WidgetSupports(k, WidgetSize::Medium) || WidgetSupports(k, WidgetSize::Large); }

} // namespace widget_detail

} // namespace atrium
