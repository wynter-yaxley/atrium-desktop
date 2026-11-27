#include "bar.h"
#include "bar_entries.h"
#include "clock.h"
#include "render/ui_font.h"
#include "ui/surface.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace atrium {

LRESULT CALLBACK Bar::InputProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->OnInput(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

POINT Bar::LayoutPoint(POINT client) const
{
    RECT cr{};
    GetClientRect(input_, &cr);
    return EdgeFrame{edge_, cr}.FromArea(client);
}

RECT Bar::EdgeZone() const { return BarEdgeZone(edge_, window_, monitor_, scene_.bar); }

std::wstring Bar::HitTest(POINT pt) const
{
    for (const auto& hit : hits_)
        if (PtInRect(&hit.rect, pt))
            return hit.key;
    return {};
}

LRESULT Bar::HandleMouseActivate(HWND h, LPARAM l)
{
    onInteract();
    // Never take focus from the user's app, except for the logo, bell, and Launchpad: the start
    // menu and the notification list need the keyboard, and only the foreground process may hand
    // the foreground to its own window.
    POINT cur;
    GetCursorPos(&cur);
    ScreenToClient(h, &cur);
    cur = LayoutPoint(cur);
    const std::wstring hit = HitTest(cur);
    if ((hit == kStartKey || hit == kNotifyKey || hit == kLaunchpadKey) && HIWORD(l) == WM_LBUTTONDOWN) {
        previousForeground_ = GetForegroundWindow();
        if (previousForeground_ == input_ || previousForeground_ == visual_)
            previousForeground_ = nullptr;
        return MA_ACTIVATE;
    }
    return MA_NOACTIVATE;
}

void Bar::HandleMouseMove(HWND h, WPARAM w, POINT pt)
{
    onInteract();
    if (dragging_) {
        UpdateDrag(pt);
        return;
    }
    if (pressedLeft_ && !pressed_.empty() && !IsEntry(pressed_) && (w & MK_LBUTTON) &&
        (std::abs(pt.x - pressPt_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - pressPt_.y) > GetSystemMetrics(SM_CYDRAG))) {
        BeginDrag(pt);
        return;
    }
    // Remember brief visits; only the current cursor samples determine approach or retreat above it.
    if (float(pt.y) >= scene_.bar.top)
        near_.Update(scene_.bar, {float(pt.x), float(pt.y)}, scale_, settings_.nearEnter, settings_.nearLeave);
    if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        tracking_ = TrackMouseEvent(&tme) != FALSE;
        SetRegion(true); // magnified icons reach above the shape
    }
    const std::wstring hover = HitTest(pt);
    const bool magnify = scene_.magnify > 1.001f;
    // Of the pointer's height only its side of the glass's top edge matters to the wave.
    const bool crossed = (scene_.pointerY < scene_.bar.top) != (float(pt.y) < scene_.bar.top);
    if (hover != scene_.hoverKey || (magnify && (scene_.pointerX != float(pt.x) || crossed))) {
        if (hover != scene_.hoverKey) {
            scene_.hoverKey = hover;
            // With a preview up, the next app's windows follow the pointer without the tooltip's delay.
            const bool quick = previewShown_ || (previewOpen && previewOpen());
            HideTooltip();
            if (!hover.empty())
                SetTimer(h, kTooltipTimer, quick ? kPreviewSwapMs : kTooltipDelayMs, nullptr);
        }
        scene_.pointerX = float(pt.x);
        WatchNear(false); // current cursor Y and near flags belong to this same snapshot
        Publish();
    } else if (magnify && float(pt.y) < scene_.bar.top) {
        WatchNear(); // above the glass a move straight up may end the early response: published if it did
    }
}

bool Bar::HandleInputTimer(HWND h, WPARAM w)
{
    switch (w) {
    case kDisplayTimer: WatchDisplays(); return true;
    case kCoverTimer:
        KillTimer(h, kCoverTimer);
        coverPending_ = false;
        UpdateCover();
        return true;
    case kTooltipTimer:
        KillTimer(h, kTooltipTimer);
        if (pressed_.empty() && !dragging_)
            ShowTooltip();
        return true;
    case kFileHoverTimer:
        KillTimer(h, kFileHoverTimer);
        if (!fileHoverKey_.empty() && !IsEntry(fileHoverKey_) && onFileDragHover)
            onFileDragHover(fileHoverKey_);
        return true;
    case kGuardTimer: GuardStacking(nullptr); return true;
    case kSlideTimer:
        KillTimer(h, kSlideTimer);
        if (tucked_ && !slidOut_) {
            slidOut_ = true;
            Relayout(); // hides the windows; the render thread rests
            WatchNear(); // nothing to answer now: let it fall back
        }
        return true;
    case kPointerTimer: WatchPointer(); return true;
    case kNearTimer: WatchNear(); return true;
    default: return false;
    }
}

void Bar::HandleButtonUp(UINT m, WPARAM w, POINT pt)
{
    if (dragging_) {
        // Dropped well above or below the bar: cancel, like the system taskbar.
        const int mid = int((scene_.bar.top + scene_.bar.bottom) / 2);
        EndDrag(std::abs(pt.y - mid) <= int(scene_.bar.bottom - scene_.bar.top));
        ReleaseCapture();
        POINT cur;
        GetCursorPos(&cur);
        if (WindowFromPoint(cur) != input_) { // released off the bar: no hover is left
            tracking_ = false;
            SetRegion(false);
            scene_.hoverKey.clear();
            scene_.pointerX = scene_.pointerY = -1;
            Publish();
        }
        return;
    }
    const std::wstring key = HitTest(pt);
    const bool same = !pressed_.empty() && key == pressed_;
    pressed_.clear();
    pressedLeft_ = false;
    scene_.pressedKey.clear();
    ReleaseCapture();
    Publish();
    if (!same)
        return;
    // Shift+click starts a new instance, as on the Windows taskbar.
    const bool launch = m == WM_MBUTTONUP || (w & MK_SHIFT);
    if (IsEntry(key)) {
        if (m == WM_LBUTTONUP)
            ClickEntry(key);
    } else if (launch ? bool(onLaunch) : bool(onActivate)) {
        // Starting an app (or another instance) hops its icon, as in the macOS Dock.
        auto t = std::ranges::find_if(items_, [&](const TaskItem& i) { return i.key == key; });
        if (t != items_.end() && (launch || t->windows.empty()))
            Bounce(key);
        (launch ? onLaunch : onActivate)(key);
    }
}

void Bar::HandleCaptureChanged(HWND h, LPARAM l)
{
    if (reinterpret_cast<HWND>(l) != h && dragging_) {
        EndDrag(false); // capture taken away (e.g. a system menu opened)
        SetRegion(false);
    } else if (reinterpret_cast<HWND>(l) != h && (!pressed_.empty() || pressedLeft_)) {
        pressed_.clear();
        pressedLeft_ = false;
        scene_.pressedKey.clear();
        Publish();
    }
}

void Bar::HandleMouseWheel(HWND h, WPARAM w, LPARAM l)
{
    onInteract();
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    const int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
    const std::wstring over = notches ? HitTest(LayoutPoint(p)) : std::wstring();
    if (over == kStatusKey && onStatusWheel)
        onStatusWheel(notches);
}

void Bar::HandleRightButtonUp(HWND h, POINT pt)
{
    onInteract();
    if (dragging_)
        return;
    const std::wstring key = HitTest(pt);
    // Menus open on the inner side of the bar (beyond icons the pointer magnified), without the tooltip.
    HideTooltip();
    KillTimer(h, kTooltipTimer);
    const POINT screen = EdgeFrame{edge_, window_}.ToArea(POINT{pt.x, 0});
    if (!key.empty() && !IsEntry(key) && onItemMenu)
        onItemMenu(key, screen);
    else if (key == kStartKey && onStartMenu) // the logo: Windows' Start button menu (Win+X)
        onStartMenu(screen);
    else if (onContextMenu)
        onContextMenu(screen);
}

LRESULT Bar::OnInput(HWND h, UINT m, WPARAM w, LPARAM l)
{
    const POINT pt = LayoutPoint({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
    if (m == AppBarNotice() && AppBarNotice()) {
        if (w == ABN_POSCHANGED) { // another app bar or the taskbar moved: claim the strip again
            appBarWant_ = RECT{};
            Relayout();
        }
        return 0;
    }
    switch (m) {
    case WM_BAR_LAYOUT: {
        layoutPending_ = false;
        const HMONITOR beforeMon = hmonitor_;
        const RECT beforeRect = monitor_;
        const float beforeScale = scale_;
        const BarEdge beforeEdge = edge_;
        Relayout();
        if (beforeMon && (hmonitor_ != beforeMon || !EqualRect(&monitor_, &beforeRect) || scale_ != beforeScale || edge_ != beforeEdge))
            GuardStacking(L"display");
        return 0;
    }
    case WM_MOUSEACTIVATE: return HandleMouseActivate(h, l);
    case WM_CLOSE:
        if (onCloseRequest)
            onCloseRequest();
        return 0;
    case WM_MOUSEMOVE: HandleMouseMove(h, w, pt); return 0;
    case WM_TIMER:
        if (HandleInputTimer(h, w))
            return 0;
        break;
    case WM_MOUSELEAVE:
        tracking_ = false;
        HideTooltip();
        if (!dragging_) {
            SetRegion(false);
            scene_.hoverKey.clear(), scene_.pressedKey.clear(), scene_.pointerX = scene_.pointerY = -1;
            pressed_.clear(), pressedLeft_ = false;
            Publish();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
        HideTooltip();
        if (onPreviewClose)
            onPreviewClose(); // a press on the bar: the preview gives way
        pressed_ = HitTest(pt);
        pressedLeft_ = m == WM_LBUTTONDOWN;
        pressPt_ = pt;
        scene_.pressedKey = pressed_;
        if (m == WM_LBUTTONDOWN) { // the glass gives way under the press
            scene_.rippleX = float(pt.x);
            scene_.rippleY = float(pt.y);
            scene_.rippleMs = ClockMs();
        }
        SetCapture(h);
        Publish();
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP: HandleButtonUp(m, w, pt); return 0;
    case WM_CAPTURECHANGED: HandleCaptureChanged(h, l); return 0;
    case WM_MOUSEWHEEL: HandleMouseWheel(h, w, l); return 0;
    case WM_RBUTTONUP: HandleRightButtonUp(h, pt); return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void Bar::BeginDrag(POINT pt)
{
    int from = -1;
    for (int i = 0; i < int(scene_.items.size()); ++i)
        if (scene_.items[size_t(i)].key == pressed_)
            from = i;
    if (from < 0)
        return;
    const auto& plate = scene_.items[size_t(from)].plate;
    dragging_ = true;
    dragKey_ = pressed_;
    dragInsert_ = from;
    grabDx_ = pressPt_.x - int((plate.left + plate.right) / 2);
    dragX_ = pt.x;
    scene_.pressedKey.clear();
    HideTooltip();
    Relayout();
}

void Bar::UpdateDrag(POINT pt)
{
    dragX_ = pt.x;
    // Insertion point: the slot whose centre is nearest to the dragged plate's.
    const float centre = float(pt.x - grabDx_);
    const int slot = int(std::lround((centre - float(appsLeft_) - float(plate_) / 2) / float(pitch_)));
    dragInsert_ = std::clamp(slot, 0, std::max(shown_ - 1, 0));
    Relayout();
}

void Bar::EndDrag(bool commit)
{
    if (!dragging_)
        return;
    dragging_ = false;
    const std::wstring key = dragKey_;
    dragKey_.clear(), pressed_.clear(), pressedLeft_ = false, scene_.pressedKey.clear();
    int from = -1;
    for (int i = 0; i < shown_ && i < int(items_.size()); ++i)
        if (items_[size_t(i)].key == key)
            from = i;
    const bool moved = commit && from >= 0 && dragInsert_ >= 0 && dragInsert_ != from && dragInsert_ < int(items_.size());
    if (moved) {
        // Show the new order right away; the model confirms it a moment later
        // with the same order, which then needs no relayout.
        TaskItem t = std::move(items_[size_t(from)]);
        items_.erase(items_.begin() + from);
        items_.insert(items_.begin() + dragInsert_, std::move(t));
        auto shared = std::ranges::find_if(allItems_, [&](const TaskItem& item) { return item.key == key; });
        TaskItem movedItem = std::move(*shared);
        allItems_.erase(shared);
        const std::wstring before = dragInsert_ + 1 < int(items_.size()) ? items_[size_t(dragInsert_) + 1].key : L"";
        auto insertion = std::ranges::find_if(allItems_, [&](const TaskItem& item) { return item.key == before; });
        allItems_.insert(insertion, std::move(movedItem));
    }
    scene_.hoverKey = moved ? key : scene_.hoverKey;
    Relayout(); // the dropped icon slides from the pointer into its slot
    if (moved && onReorder)
        onReorder(key, dragInsert_);
}

std::wstring Bar::FileDragOver(POINT screen)
{
    onInteract();
    ScreenToClient(input_, &screen);
    std::wstring key = HitTest(LayoutPoint(screen));
    if (IsEntry(key))
        key.clear();
    if (key != fileHoverKey_) {
        fileHoverKey_ = key;
        scene_.hoverKey = key;
        KillTimer(input_, kFileHoverTimer);
        if (!key.empty())
            SetTimer(input_, kFileHoverTimer, kFileHoverDelayMs, nullptr);
        Publish();
    }
    return key;
}

void Bar::FileDragLeave()
{
    KillTimer(input_, kFileHoverTimer);
    fileHoverKey_.clear();
    scene_.hoverKey.clear();
    Publish();
}

void Bar::WatchPointer()
{
    if (!covered_ || !visible_) {
        KillTimer(input_, kPointerTimer);
        return;
    }
    POINT p{};
    if (!GetCursorPos(&p))
        return;
    if (!revealed_) {
        if (InEdgeSummonZone(edge_, p, monitor_) && !FullScreenInFront(hmonitor_)) {
            revealed_ = true;
            leftAt_ = 0;
            UpdateAutoHide();
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (InEdgeRevealZone(edge_, p, window_, monitor_, scene_.bar) || dragging_ || (keepShown && keepShown()) || now < summonGraceUntil_) {
        leftAt_ = 0;
        return;
    }
    if (!leftAt_)
        leftAt_ = now;
    else if (now - leftAt_ >= kLeaveMs) {
        revealed_ = false;
        leftAt_ = 0;
        UpdateAutoHide();
    }
}

void Bar::WatchNear(bool publish)
{
    const bool available = visible_ && !tucked_ && !slidOut_;
    const bool watching = available && scene_.magnify > 1.001f && (scene_.earlyRise || scene_.earlyMagnify);
    POINT p{};
    if ((watching || scene_.pointerX >= 0) && (!GetCursorPos(&p) || !ScreenToClient(input_, &p)))
        return;
    p = LayoutPoint(p);
    const bool changed = near_.Sample(scene_, {float(p.x), float(p.y)}, settings_.nearEnter, settings_.nearLeave, available);
    nearX_ = scene_.nearX, nearRise_ = scene_.nearRise, nearMagnify_ = scene_.nearMagnify;
    if (changed && publish)
        Publish();
}

void Bar::UpdateNearWatch()
{
    const bool wanted = settings_.magnify > 1.001f && ((settings_.hoverRise && settings_.riseNear) || settings_.magnifyNear);
    if (visible_ && !tucked_ && !slidOut_ && wanted)
        SetTimer(input_, kNearTimer, kNearMs, nullptr);
    else {
        KillTimer(input_, kNearTimer);
        near_.Reset();
        if (nearX_ >= 0 || nearRise_ || nearMagnify_) {
            nearX_ = scene_.nearX = -1;
            nearRise_ = nearMagnify_ = scene_.nearRise = scene_.nearMagnify = false;
            Publish();
        }
    }
}

void Bar::ShowTooltip()
{
    const std::wstring& key = scene_.hoverKey;
    auto hit = std::ranges::find_if(hits_, [&](const Hit& h) { return h.key == key; });
    const int rise = scene_.magnify > 1.001f ? rise_ : 0;
    const POINT tip =
        hit != hits_.end() ? EdgeFrame{edge_, window_}.ToArea(POINT{(hit->rect.left + hit->rect.right) / 2, int(scene_.bar.top) - rise}) : POINT{};
    // An app with windows: its windows, previewed, instead of the tooltip (as the Windows taskbar).
    if (onPreview && hit != hits_.end() && !IsEntry(key)) {
        auto item = std::ranges::find_if(items_, [&](const TaskItem& t) { return t.key == key; });
        if (item != items_.end() && !item->windows.empty()) {
            tooltip_->Hide(false);
            previewShown_ = true;
            onPreview(key, item->windows, tip);
            return;
        }
    }
    if (previewShown_) { // its last window went: back to the tooltip
        previewShown_ = false;
        if (onPreviewClose)
            onPreviewClose();
    }
    std::wstring text = TooltipText(key);
    if (hit == hits_.end() || text.empty()) {
        tooltip_->Hide(false);
        return;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hmonitor_, &mi);
    const int margin = Px(8, scale_);
    const float maxTextW = std::min(320.f, float(mi.rcWork.right - mi.rcWork.left - 2 * margin) / scale_ - 2 * kTooltipPadX);
    winrt::com_ptr<IDWriteTextFormat> format;
    winrt::com_ptr<IDWriteTextLayout> layout;
    // UiTextFormat/UiTextLayout take physical pixels: the tooltip's size is
    // DIP, so measure at the monitor's scale and divide the metrics back.
    if (FAILED(UiTextFormat(dwrite_.get(), kTooltipFont * scale_, DWRITE_FONT_WEIGHT_MEDIUM, format.put())))
        return;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (FAILED(UiTextLayout(dwrite_.get(), text, format.get(), maxTextW * scale_, 1000 * scale_, layout.put())))
        return;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    tooltipText_ = std::move(text);
    tooltipSize_ = D2D1::SizeF(std::min(maxTextW, std::ceil(metrics.widthIncludingTrailingWhitespace / scale_)) + 2 * kTooltipPadX,
                               std::max(kTooltipMinH, std::ceil(metrics.height / scale_) + 2 * kTooltipPadY));
    const int w = Px(tooltipSize_.width, scale_), h = Px(tooltipSize_.height, scale_);
    const int minX = int(mi.rcWork.left) + margin, maxX = std::max(minX, int(mi.rcWork.right) - margin - w);
    const int minY = int(mi.rcWork.top) + margin, maxY = std::max(minY, int(mi.rcWork.bottom) - margin - h);
    int left = std::clamp(int(tip.x) - w / 2, minX, maxX), top = std::clamp(int(tip.y) - h / 2, minY, maxY);
    if (edge_ == BarEdge::Bottom)
        top = std::max(minY, int(tip.y) - h - margin);
    else if (edge_ == BarEdge::Top)
        top = std::min(maxY, int(tip.y) + margin);
    else if (edge_ == BarEdge::Left)
        left = std::min(maxX, int(tip.x) + margin);
    else
        left = std::max(minX, int(tip.x) - w - margin);
    const bool tail = edge_ == BarEdge::Bottom;
    const RECT rect{left, top, left + w, top + h + (tail ? Px(kTooltipTailH, scale_) : 0)};
    const float radius = std::min(16.f, tooltipSize_.height / 2);
    tooltip_->SetRadius(radius);
    if (tail) {
        tooltipTailX_ = std::clamp(float(tip.x - left) / scale_, 0.f, tooltipSize_.width);
        ShapeDip label{D2D1::RectF(0, 0, tooltipSize_.width, tooltipSize_.height), radius};
        label.tailX = tooltipTailX_, label.tailHalfWidth = kTooltipTailHalfW, label.tailHeight = kTooltipTailH, label.tailInset = kTooltipTailInset;
        tooltip_->SetShapes({label});
    } else {
        tooltip_->SetShapes({});
    }
    POINT anchor{tip.x, rect.bottom}; // it grows out of the side facing the icon
    switch (edge_) {
    case BarEdge::Top: anchor.y = rect.top; break;
    case BarEdge::Left: anchor = {rect.left, tip.y}; break;
    case BarEdge::Right: anchor = {rect.right, tip.y}; break;
    case BarEdge::Bottom: break;
    }
    if (tooltip_->Visible()) {
        tooltip_->Move(rect);
        tooltip_->Invalidate();
    } else {
        tooltip_->Show(rect, false, anchor);
    }
}

void Bar::HideTooltip()
{
    KillTimer(input_, kTooltipTimer);
    if (previewShown_) {
        previewShown_ = false;
        if (onPreviewLeave)
            onPreviewLeave();
    }
    tooltip_->Hide();
}

void Bar::UpdateTooltips()
{
    if (tooltip_->Visible() || previewShown_)
        ShowTooltip(); // e.g. the window count changed while hovering
}

void Bar::SetVisible(bool visible)
{
    const bool changed = visible_ != visible;
    visible_ = visible;
    if (!visible_)
        UpdateCover();
    Relayout();
    if (visible_)
        UpdateCover();
    UpdateNearWatch();
    if (changed && onTuckChanged)
        onTuckChanged();
}

} // namespace atrium
