// The folder stack's popup: the folders the user added (shell/folder_stacks.h), opened from their
// entry on the bar. As a fan (folder_stack_layout.h) the folders fly out of the entry one after
// another and settle on their arc with a soft overshoot, each name's capsule of glass arriving just
// after its icon; closing gathers them back into the entry. As a column, one card of glass with a
// row per folder. A click opens the folder (an Explorer window already on it comes forward), a
// middle click a new window; the arrows and Enter work as the pointer does.
#pragma once

#include "motion.h"
#include "shell/folder_stacks.h"
#include "ui/folder_stack_layout.h"
#include "ui/surface.h"

#include <windows.h>
#include <dwrite.h>
#include <winrt/base.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace atrium {

class FolderStackPopup {
public:
    FolderStackPopup(HINSTANCE instance, FolderStackModel& model);

    // entry: the stack's entry on the bar (screen px); bar: the bar's glass.
    void Open(RECT entry, RECT bar);
    void Close();
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(); // the model read the folders again (names, icons)
    // The settings folderStackLayout and folderStackMax.
    void SetOptions(bool fan, size_t maxVisible)
    {
        fan_ = fan;
        maxVisible_ = maxVisible;
    }
    // folderStackPinned: losing the focus leaves the stack open; a second click on its entry closes it.
    void SetPinned(bool on) { pinned_ = on; }
    void SetRadius(float dip)
    {
        radius_ = dip;
        surface_.SetRadius(dip);
    }
    std::function<void()> onManageFolders; // the empty stack's button: the settings page

private:
    void Place(bool show);
    void Paint(Canvas& c);
    void PaintFan(Canvas& c);
    void PaintColumn(Canvas& c);
    void PaintEmpty(Canvas& c);
    void PaintIcon(Canvas& c, size_t i, D2D1_RECT_F r, float opacity);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool Key(WPARAM key);
    void Activate(size_t i, bool newWindow);
    int HitAt(POINT pt) const; // the folder under a content point (px); -1 none, -2 the empty stack's button
    float Hover(Canvas& c, int id, bool on);
    // The fan's progress for folder i: 0 in the entry .. 1 in its place (past 1 while overshooting).
    float Unfold(size_t i, double now, float& opacity) const;
    void PublishFanGlass(double now);

    Surface surface_;
    FolderStackModel& model_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    std::vector<StackFolder> folders_;
    StackPlacement place_;
    RECT entry_{}, bar_{};
    bool fan_ = true;
    bool pinned_ = false;
    size_t maxVisible_ = 8;
    float radius_ = 24;
    int hover_ = -1, pressed_ = -1, sel_ = -1;
    bool tracking_ = false;
    bool closing_ = false;
    double openedMs_ = 0, closedMs_ = 0;
    ULONGLONG lastClosed_ = 0;
    std::map<int, motion::Follow> hoverAnim_;
};

} // namespace atrium
