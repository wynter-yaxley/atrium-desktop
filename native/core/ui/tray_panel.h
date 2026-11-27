// Atrium's tray panel: the notification area's icons (TrayModel) in a grid on
// the bar's glass, above the bar's tray entry. A click is the icon's left
// click, a right click opens the app's own menu; the name of the icon under
// the pointer shows below the grid. A button there opens the system's own
// hidden-icons window as a fallback.
#pragma once

#include "motion.h"
#include "shell/task_model.h"
#include "shell/tray_model.h"
#include "ui/surface.h"

#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atrium {

inline bool TrayFollowCancelled(DWORD inputTime, DWORD clickInput, DWORD syntheticInput, bool syntheticInFlight, POINT anchor, POINT cursor,
                                SIZE doubleClickRect, bool keyDown)
{
    if (inputTime == clickInput || inputTime == syntheticInput)
        return false;
    if (syntheticInFlight) // the model's own pointer fallback moves the cursor and presses the button
        return false;
    if (keyDown)
        return true;
    return std::abs(cursor.x - anchor.x) > doubleClickRect.cx / 2 || std::abs(cursor.y - anchor.y) > doubleClickRect.cy / 2;
}

class TrayPanel {
public:
    TrayPanel(HINSTANCE instance, TrayModel& model);
    ~TrayPanel();
    TrayPanel(const TrayPanel&) = delete;
    TrayPanel& operator=(const TrayPanel&) = delete;

    // anchor: the tray entry on screen; bar: the bar's glass shape.
    // previous: the window that had the focus before (it gets it back if a click leaves it nowhere).
    void Open(RECT anchor, RECT bar, HWND previous);
    HWND Previous() const { return previous_; }
    void Close();
    void YieldTopmostForPointerFallback();
    bool Visible() const { return surface_.Visible(); }
    bool MenuSessionActive() const { return sessionMode_ != SessionMode::None; }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(); // the model has a new list
    void SetAttention(const std::vector<TaskItem>& tasks);
    void SetRadius(float dip)
    {
        radius_ = dip;
        surface_.SetRadius(dip);
    }

    std::function<void()> onSystemTray; // the fallback button

private:
    enum class SessionMode { None, Menu, Hover };
    static constexpr UINT_PTR kSettleTimer = 0x5A13, kMenuTimer = 0x5A19;
    static constexpr UINT kSettleMs = 150, kMenuPollMs = 120, kOutsideClickMsg = WM_APP + 0x5A;
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    static std::optional<size_t> IconIndex(std::wstring_view id);
    static void FollowMenuAt(POINT at, const std::wstring& exe, DWORD preferredPid);
    static void StopMenuFollow();
    static bool MatchesIconProcess(DWORD pid, const TrayIcon& icon);
    void Paint(Canvas& c);
    void PaintIconCell(Canvas& c, size_t i, float plateR, int iconPx);
    void PaintFooter(Canvas& c, float w, float footTop, float divider);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool OnTimerMessage(WPARAM w);
    void OnMouseDownMessage(POINT pt);
    void OnMouseUpMessage(UINT m, POINT pt);
    void OnOutsideMouseDown(LPARAM l);
    void HandleDeactivate(HWND activating);
    void HandleSettleTimer();
    void HandleMenuSessionTimer();
    void HandleHoverSessionTimer();
    void StartMenuSession(const TrayIcon& icon);
    void StartHoverSession(const TrayIcon& icon, HWND popup);
    void EndPopupSession();
    void RestoreTopmostIfYielded();
    bool IsPointInActiveMenu(POINT screenPt) const;
    bool IsPointInActivePopup(POINT screenPt) const;
    bool IsActiveMenuOpen() const;
    bool IsActiveHoverPopupOpen() const;
    void Place();
    std::wstring HitAt(POINT pt) const;
    void Activate(const std::wstring& id, bool right);
    float Hover(Canvas& c, const std::wstring& id, bool on);
    D2D1_SIZE_F Size() const; // content, DIP
    int Cols() const;
    float Margin() const; // DIP: from the panel's edges to what is seen inside
    D2D1_RECT_F CellRect(size_t i) const; // icon i's cell, DIP
    RECT CellScreenRect(size_t i) const;  // icon i's cell, physical screen pixels
    void ArmTip();  // the hovered icon's tooltip after the system's delay
    void ShowTip();
    void HideTip();
    void HoverAt(POINT pt);
    void LeaveHover();
    bool HasAttention(const TrayIcon& icon) const;

    Surface surface_;
    TrayModel& model_;
    std::vector<TrayIcon> icons_;
    std::vector<std::wstring> attention_;
    std::vector<DWORD> attentionPids_;
    std::vector<Hit> hits_;
    std::wstring hover_, pressed_;
    HWND tip_ = nullptr;            // the standard tooltip, as the system tray shows it
    std::wstring tipFor_, tipText_; // the hover id and text it currently shows
    RECT tipCell_{};
    std::optional<TrayIcon> lastClick_;
    std::optional<TrayIcon> pressedIcon_;
    POINT lastAt_{};
    ULONGLONG lastWhen_ = 0;
    bool tracking_ = false;
    int columns_ = 0; // fixed for one open session, so additions do not reflow existing rows
    RECT anchor_{}, bar_{};
    HWND previous_ = nullptr;
    float radius_ = 30;
    ULONGLONG lastClosed_ = 0;
    bool private_ = false;
    SessionMode sessionMode_ = SessionMode::None;
    bool menuObserved_ = false;
    bool hoverSettle_ = false;
    bool yieldedTopmost_ = false;
    ULONGLONG menuSessionAt_ = 0;
    ULONGLONG panelReenterUntil_ = 0;
    DWORD menuPid_ = 0;
    HWND hoverPopupHwnd_ = nullptr;
    std::wstring menuExe_;
    int mouseWatcherToken_ = 0;
    std::map<std::wstring, motion::Follow> hoverAnim_;
    double openedMs_ = 0;
    std::wstring rippleId_;
    float rippleX_ = 0, rippleY_ = 0;
    double rippleMs_ = 0;
};

} // namespace atrium
