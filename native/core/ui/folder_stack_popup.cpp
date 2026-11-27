#include "ui/folder_stack_popup.h"

#include "clock.h"
#include "localization.h"
#include "render/ui_font.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr float kLabelFont = 13, kRowFont = 13.5f;
constexpr double kOpenMs = 340, kStaggerMs = 22;     // each folder's flight out, and the next one's start
constexpr double kLabelDelayMs = 70, kLabelMs = 200; // a name's capsule, after its icon
constexpr double kCloseMs = 170, kCloseStaggerMs = 12;
constexpr double kRowMs = 240, kRowStaggerMs = 14;
constexpr UINT_PTR kCloseTimer = 0x5354;
constexpr int kManage = -2;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
D2D1_POINT_2F Centre(const D2D1_RECT_F& r) { return D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2); }
D2D1_RECT_F Around(D2D1_POINT_2F c, float half) { return D2D1::RectF(c.x - half, c.y - half, c.x + half, c.y + half); }
D2D1_RECT_F Moved(D2D1_RECT_F r, float dx, float dy) { return D2D1::RectF(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy); }

// The empty stack's card and its button, in the column's first two rows.
D2D1_RECT_F ManageButton(const StackPlacement& p, float scale)
{
    const float w = float(p.window.right - p.window.left) / scale, h = float(p.window.bottom - p.window.top) / scale;
    return D2D1::RectF(w / 2 - 96, h - 18 - 30, w / 2 + 96, h - 18);
}

} // namespace

FolderStackPopup::FolderStackPopup(HINSTANCE instance, FolderStackModel& model) : surface_(instance, L"Atrium Stacks", true), model_(model)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(); };
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.put()));
}

void FolderStackPopup::Open(RECT entry, RECT bar)
{
    entry_ = entry;
    bar_ = bar;
    folders_ = model_.Snapshot();
    if (folders_.size() > maxVisible_)
        folders_.resize(maxVisible_);
    KillTimer(surface_.Hwnd(), kCloseTimer);
    closing_ = false;
    hover_ = pressed_ = sel_ = -1;
    hoverAnim_.clear();
    openedMs_ = ClockMs();
    Place(true);
}

void FolderStackPopup::Close()
{
    if (!surface_.Visible() || closing_)
        return;
    lastClosed_ = GetTickCount64();
    pressed_ = hover_ = sel_ = -1;
    if (!place_.fan || folders_.empty()) {
        surface_.Hide();
        return;
    }
    // The fan gathers back into the entry, then the window goes.
    closing_ = true;
    closedMs_ = ClockMs();
    const double total = kCloseMs + kCloseStaggerMs * double(place_.items.size());
    SetTimer(surface_.Hwnd(), kCloseTimer, UINT(total) + 20, nullptr);
    surface_.Invalidate();
}

void FolderStackPopup::Refresh()
{
    if (!surface_.Visible() || closing_)
        return;
    std::vector<StackFolder> next = model_.Snapshot();
    if (next.size() > maxVisible_)
        next.resize(maxVisible_);
    const bool same = next.size() == folders_.size() && std::ranges::equal(next, folders_, [](const StackFolder& a, const StackFolder& b) {
                          return a.path == b.path && a.name == b.name && a.isFile == b.isFile;
                      });
    for (const auto& f : folders_)
        surface_.ForgetImage(L"stack|" + f.path);
    folders_ = std::move(next);
    if (!same) {
        pressed_ = hover_ = sel_ = -1;
        Place(false); // other folders or names: other places (icons alone just repaint)
    }
    surface_.Invalidate();
}

void FolderStackPopup::Place(bool show)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&bar_, MONITOR_DEFAULTTONEAREST), &mi);
    const float scale = PopupScale(bar_);
    const BarEdge edge = EdgeOf(bar_);
    if (folders_.empty()) {
        place_ = ColumnPlacement(bar_, entry_, mi.rcWork, scale, edge, 2);
        place_.items.clear();
    } else if (fan_) {
        std::vector<float> widths;
        widths.reserve(folders_.size());
        for (const auto& f : folders_)
            widths.push_back(dwrite_ ? MeasureUiText(dwrite_.get(), f.name, kLabelFont, DWRITE_FONT_WEIGHT_NORMAL) : 120.f);
        place_ = FanPlacement(bar_, entry_, mi.rcWork, scale, edge, widths);
    } else {
        place_ = ColumnPlacement(bar_, entry_, mi.rcWork, scale, edge, folders_.size());
    }
    const bool fan = place_.fan && !folders_.empty();
    if (!fan)
        surface_.SetShapes({}); // one card: the whole content
    const POINT from{(entry_.left + entry_.right) / 2, (entry_.top + entry_.bottom) / 2};
    if (show)
        surface_.Show(place_.window, true, from, !fan); // the fan unfolds by itself
    else
        surface_.Move(place_.window);
    if (fan)
        PublishFanGlass(ClockMs());
}

float FolderStackPopup::Unfold(size_t i, double now, float& opacity) const
{
    if (closing_) {
        // Back into the entry, the farthest first, quickening as it goes.
        const size_t n = place_.items.size();
        const float t = std::clamp(float((now - closedMs_ - kCloseStaggerMs * double(n - 1 - i)) / kCloseMs), 0.f, 1.f);
        opacity = std::clamp((1 - t) * 2.5f, 0.f, 1.f);
        return 1 - t * t;
    }
    const float t = std::clamp(float((now - openedMs_ - kStaggerMs * double(i)) / kOpenMs), 0.f, 1.f);
    opacity = std::clamp(t * 3, 0.f, 1.f);
    return motion::EaseOutBack(t, 1.25f);
}

// Each name's capsule of glass, following its icon out of the entry and fading in just after it.
void FolderStackPopup::PublishFanGlass(double now)
{
    std::vector<ShapeDip> shapes;
    bool moving = false;
    for (size_t i = 0; i < place_.items.size(); ++i) {
        float opacity = 0;
        const float e = Unfold(i, now, opacity);
        const auto& item = place_.items[i];
        const D2D1_POINT_2F target = Centre(item.icon);
        const float dx = (place_.origin.x - target.x) * (1 - e), dy = (place_.origin.y - target.y) * (1 - e);
        float shown = closing_ ? opacity : std::clamp(float((now - openedMs_ - kStaggerMs * double(i) - kLabelDelayMs) / kLabelMs), 0.f, 1.f);
        shown = motion::EaseOut(shown);
        moving = moving || shown < 1 || (closing_ && opacity > 0) || std::abs(e - 1) > 0.001f;
        ShapeDip glass{Moved(item.label, dx, dy), (item.label.bottom - item.label.top) / 2};
        glass.fade = 1 - shown;
        glass.rotate = item.tilt * std::min(e, 1.f); // levels out on the way in
        shapes.push_back(glass);
    }
    surface_.SetShapes(shapes, moving);
}

float FolderStackPopup::Hover(Canvas& c, int id, bool on)
{
    auto& f = hoverAnim_[id];
    const float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

void FolderStackPopup::Paint(Canvas& c)
{
    if (folders_.empty())
        PaintEmpty(c);
    else if (place_.fan)
        PaintFan(c);
    else
        PaintColumn(c);
}

void FolderStackPopup::PaintIcon(Canvas& c, size_t i, D2D1_RECT_F r, float opacity)
{
    const StackFolder& f = folders_[i];
    const float fade = opacity * (f.missing ? 0.45f : 1.f);
    if (f.icon.width > 0) {
        c.Image(L"stack|" + f.path, f.icon, r, fade);
        return;
    }
    c.BeginGroup(fade, 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
    c.Glyph(f.isFile ? L"\uE7C3" : L"\uE8B7", r, (r.bottom - r.top) * 0.8f, c.theme.text); // Document / Folder
    c.EndGroup();
}

void FolderStackPopup::PaintFan(Canvas& c)
{
    const Theme& t = c.theme;
    PublishFanGlass(c.now);
    bool moving = false;
    for (size_t i = 0; i < place_.items.size(); ++i) {
        float opacity = 0;
        const float e = Unfold(i, c.now, opacity);
        moving = moving || std::abs(e - 1) > 0.001f || (closing_ && opacity > 0);
        if (opacity <= 0.003f)
            continue;
        const auto& item = place_.items[i];
        const D2D1_POINT_2F target = Centre(item.icon);
        const float dx = (place_.origin.x - target.x) * (1 - e), dy = (place_.origin.y - target.y) * (1 - e);
        const float hv = Hover(c, int(i), hover_ == int(i) || sel_ == int(i));
        // The name: over its capsule (painted plain while the glass is not there), lit under the pointer.
        const D2D1_RECT_F pill = Moved(item.label, dx, dy);
        const float radius = (pill.bottom - pill.top) / 2;
        float shown = closing_ ? opacity : std::clamp(float((c.now - openedMs_ - kStaggerMs * double(i) - kLabelDelayMs) / kLabelMs), 0.f, 1.f);
        shown = motion::EaseOut(shown);
        moving = moving || shown < 1;
        c.BeginGroup(shown, 1, Centre(pill).x, Centre(pill).y, 0, 0, item.tilt * std::min(e, 1.f));
        if (c.panel)
            c.Fill(pill, radius, t.panel);
        if (hv > 0.001f)
            c.Fill(pill, radius, Alpha(pressed_ == int(i) ? t.press : t.hover, hv));
        const float pad = kStackLabelPadX - 2;
        c.Text(folders_[i].name, D2D1::RectF(pill.left + pad, pill.top, pill.right - pad, pill.bottom), kLabelFont, folders_[i].missing ? t.subtext : t.text,
               DWRITE_FONT_WEIGHT_NORMAL, 1);
        c.EndGroup();
        // The icon: out of the entry small, full size in place; a little larger under the pointer;
        // tilted with its name.
        const float k = (0.35f + 0.65f * std::min(e, 1.f)) * (1 + 0.12f * hv) * (pressed_ == int(i) ? 0.94f : 1.f);
        const D2D1_POINT_2F at = D2D1::Point2F(target.x + dx, target.y + dy);
        c.BeginGroup(1, 1, at.x, at.y, 0, 0, item.tilt * std::min(e, 1.f));
        PaintIcon(c, i, Around(at, kStackFanIcon / 2 * k), opacity);
        c.EndGroup();
    }
    if (moving)
        c.Continue();
}

void FolderStackPopup::PaintColumn(Canvas& c)
{
    const Theme& t = c.theme;
    const float w = float(place_.window.right - place_.window.left) / c.scale, h = float(place_.window.bottom - place_.window.top) / c.scale;
    if (c.panel)
        c.Fill(D2D1::RectF(0, 0, w, h), radius_, t.panel);
    const float plateR = std::clamp(radius_ - kStackColumnPad - 2, 6.f, 12.f); // concentric with the card's corners
    for (size_t i = 0; i < place_.items.size(); ++i) {
        const auto& item = place_.items[i];
        const float e = motion::EaseOut(float((c.now - openedMs_ - kRowStaggerMs * double(i)) / kRowMs));
        if (e < 1)
            c.Continue();
        const D2D1_POINT_2F mid = Centre(item.label);
        c.BeginGroup(e, 1, mid.x, mid.y, 0, 6 * (1 - e));
        const float hv = Hover(c, int(i), hover_ == int(i) || sel_ == int(i));
        const D2D1_RECT_F plate = D2D1::RectF(item.label.left + 2, item.label.top + 2, item.label.right - 2, item.label.bottom - 2);
        if (hv > 0.001f)
            c.Fill(plate, plateR, Alpha(t.hover, hv));
        if (pressed_ == int(i))
            c.Fill(plate, plateR, t.press);
        PaintIcon(c, i, item.icon, 1);
        c.Text(folders_[i].name, D2D1::RectF(item.icon.right + 12, item.label.top, item.label.right - 12, item.label.bottom), kRowFont,
               folders_[i].missing ? t.subtext : t.text);
        c.EndGroup();
    }
}

void FolderStackPopup::PaintEmpty(Canvas& c)
{
    const Theme& t = c.theme;
    const float w = float(place_.window.right - place_.window.left) / c.scale;
    if (c.panel)
        c.Fill(D2D1::RectF(0, 0, w, float(place_.window.bottom - place_.window.top) / c.scale), radius_, t.panel);
    c.Text(Tr(L"No folders in the stack yet"), D2D1::RectF(16, 18, w - 16, 40), kRowFont, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
    const D2D1_RECT_F b = ManageButton(place_, c.scale);
    const float hv = Hover(c, kManage, hover_ == kManage);
    c.Fill(b, (b.bottom - b.top) / 2, Alpha(pressed_ == kManage ? t.press : t.hover, 0.55f + 0.45f * hv));
    c.Text(Tr(L"Add folders in Settings"), b, 12.5f, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
}

int FolderStackPopup::HitAt(POINT pt) const
{
    const float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    if (folders_.empty())
        return In(ManageButton(place_, s), x, y) ? kManage : -1;
    for (size_t i = 0; i < place_.items.size(); ++i) {
        const auto& item = place_.items[i];
        // The capsule is tilted: the point turned back about its centre.
        const D2D1_POINT_2F o = Centre(item.label);
        const float cs = std::cos(item.tilt), sn = std::sin(item.tilt);
        const float lx = o.x + (x - o.x) * cs + (y - o.y) * sn, ly = o.y - (x - o.x) * sn + (y - o.y) * cs;
        if (In(item.label, lx, ly) || In(item.icon, x, y))
            return int(i);
    }
    return -1;
}

void FolderStackPopup::Activate(size_t i, bool newWindow)
{
    if (i >= folders_.size())
        return;
    model_.OpenFolder(folders_[i].path, newWindow, folders_[i].isFile);
    Close();
}

bool FolderStackPopup::Message(UINT m, WPARAM w, LPARAM l, LRESULT&)
{
    const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_TIMER:
        if (w != kCloseTimer)
            return false;
        KillTimer(surface_.Hwnd(), kCloseTimer);
        closing_ = false;
        surface_.Hide(false); // the fan has gathered: nothing left to animate
        return true;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !pinned_)
            Close();
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        const int h = closing_ ? -1 : HitAt(pt);
        if (h != hover_) {
            hover_ = h;
            sel_ = -1; // the pointer takes over from the keys
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        hover_ = pressed_ = -1;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
        pressed_ = closing_ ? -1 : HitAt(pt);
        if (pressed_ >= 0 && !place_.fan)
            surface_.Ripple(float(pt.x) / surface_.Scale(), float(pt.y) / surface_.Scale());
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP: {
        if (closing_) {
            pressed_ = -1;
            return true;
        }
        const int h = HitAt(pt);
        const bool same = h == pressed_ && h != -1;
        pressed_ = -1;
        surface_.Invalidate();
        if (same && h == kManage) {
            Close();
            if (onManageFolders)
                onManageFolders();
        } else if (same) {
            Activate(size_t(h), m == WM_MBUTTONUP);
        }
        return true;
    }
    case WM_KEYDOWN: return Key(w);
    default: return false;
    }
}

bool FolderStackPopup::Key(WPARAM key)
{
    if (closing_)
        return false;
    const int n = int(place_.items.size());
    // Up the fan or the column away from a bottom bar, down it from a top one.
    const int away = EdgeOf(bar_) == BarEdge::Top ? VK_DOWN : VK_UP, toward = away == VK_UP ? VK_DOWN : VK_UP;
    if (key == VK_ESCAPE)
        Close();
    else if ((key == WPARAM(away) || key == WPARAM(toward)) && n > 0)
        sel_ = std::clamp(sel_ < 0 ? 0 : sel_ + (key == WPARAM(away) ? 1 : -1), 0, n - 1);
    else if (key == VK_RETURN && sel_ >= 0)
        Activate(size_t(sel_), false);
    else
        return false;
    surface_.Invalidate();
    return true;
}

} // namespace atrium
