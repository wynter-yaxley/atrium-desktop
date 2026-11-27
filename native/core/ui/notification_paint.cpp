#include "ui/notifications.h"

#include "clock.h"
#include "localization.h"
#include "log.h"
#include "motion.h"
#include "shell/displays.h"
#include "ui/notification_layout.h"

#include <algorithm>
#include <cmath>

namespace atrium {
namespace {

constexpr float kW = 384, kGap = 10, kPeek = 10, kPill = 44;
constexpr double kMakeRoomMs = 170; // a corner card waits this long for the others to move aside
constexpr double kRippleMs = 520;
// The stack under the wheel: a notch pushes it this fast (cards per second);
// the push dies away (per second), and below kStackCoast a spring
// (stiffness, critically damped) settles it on a card. Past either end it
// gives at most kStackOverrun of speed, so it bounces back a little.
constexpr float kStackPush = 6.5f, kStackFriction = 5.5f, kStackCoast = 2.5f, kStackSpring = 110, kStackOverrun = 8;
constexpr float kStackLift = 24; // DIP a passed card rises over the stack's peeking edges as it goes
// A touchpad (steps finer than a notch) turns the stack by hand: one notch's
// worth is one card, and it settles on the nearest card this long after the last step.
constexpr ULONGLONG kStackFollowMs = 90;

// Moves v towards target at `rate` per second; true while still moving.
bool Glide(float& v, float target, float dt, float rate, float eps)
{
    v = motion::Approach(v, target, dt, rate);
    if (std::fabs(target - v) < eps) {
        v = target;
        return false;
    }
    return true;
}

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

std::wstring Ago(ULONGLONG t)
{
    ULONGLONG s = (GetTickCount64() - t) / 1000;
    if (s < 60)
        return Tr(L"Just now"); // 刚刚
    if (s < 3600)
        return TrFormat(L"%d min ago", int(s / 60));
    return TrFormat(L"%d h ago", int(s / 3600));
}

void SyncToastRegion(HWND hwnd, int& curTop, int& curBottom, const RECT& rr, int regionTop, bool clipBottom, float bottom, float scale)
{
    if (regionTop == 0)
        regionTop = -1;
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr))
        return;
    const int inset = ((wr.right - wr.left) - (rr.right - rr.left)) / 2;
    const int regionBottom = clipBottom ? std::min(int(wr.bottom - wr.top), int(std::ceil(bottom * scale)) + 2 * inset) : -1;
    if (regionTop == curTop && regionBottom == curBottom)
        return;
    HRGN rgn = regionTop > 0 || regionBottom >= 0 ?
                   CreateRectRgn(0, std::max(0, regionTop), wr.right - wr.left, regionBottom >= 0 ? regionBottom : wr.bottom - wr.top) :
                   nullptr;
    if (SetWindowRgn(hwnd, rgn, FALSE)) {
        curTop = regionTop;
        curBottom = regionBottom;
    } else if (rgn) {
        DeleteObject(rgn);
    }
}

void ClearRegion(HWND hwnd, int& regionTop, int& regionBottom)
{
    SetWindowRgn(hwnd, nullptr, FALSE);
    regionTop = regionBottom = -1;
}

struct CardTarget {
    float y = 0, inset = 0, alpha = 1, content = 1;
};

CardTarget ComputeCardTarget(size_t i, bool stack, bool dockTop, bool column, bool bottom, bool fromTop, float listTop, float listH, float card, float scroll,
                             float stackPos)
{
    if (!stack) {
        const float y = column && bottom ? listTop + listH - card - float(i) * (card + kGap) + scroll :
                        fromTop          ? listTop + notification::CardTop(i, card, kGap, scroll) :
                                           listH - card - float(i) * (card + kGap) + scroll;
        return {y};
    }
    const float rel = float(i) - stackPos;
    if (rel >= 0) {
        const float k = std::min(rel, 2.f);
        return {dockTop ? listTop + kPeek * k : listH - card - kPeek * k, 12.f * k, std::clamp(3.f - rel, 0.f, 1.f),
                motion::EaseOut(std::clamp(1.f - rel * 1.6f, 0.f, 1.f))};
    }
    const float u = std::min(-rel, 1.f);
    return {dockTop ? listTop + (2 * kPeek + kStackLift) * u : listH - card - (2 * kPeek + kStackLift) * u, 28.f * u, (1 - u) * (1 - u),
            1 - motion::EaseOut(std::clamp(u * 2.f, 0.f, 1.f))};
}

std::vector<size_t> StackDrawOrder(size_t count, bool stack, float stackPos)
{
    std::vector<size_t> order;
    order.reserve(count);
    auto passed = [&](size_t i, bool under) { return stack && float(i) < stackPos - 0.001f && (stackPos - float(i) >= 0.5f) == under; };
    for (size_t i = 0; i < count; ++i)
        if (passed(i, true))
            order.push_back(i);
    for (size_t i = count; i-- > 0;)
        if (!stack || float(i) >= stackPos - 0.001f)
            order.push_back(i);
    for (size_t i = count; i-- > 0;)
        if (passed(i, false))
            order.push_back(i);
    return order;
}

} // namespace

float Notifications::CardHeight() const { return 16 + 16 + 6 + 20 + 4 + 36 + 14; }

float Notifications::CardRadius() const { return std::min(radius_, CardHeight() / 2); }

void Notifications::CommitSurfaceRect(Mode mode, View& v, RECT r, float scale, POINT origin, bool animateShow, bool handoff)
{
    if (!v.surface->Visible()) {
        ResetView(v);
        if (handoff) {
            const float dy = float(handoffRect_.top - r.top) / scale;
            for (auto [id, a] : handoff_) {
                a.y += dy;
                a.content = id == handoffFront_ ? 1.f : 0.f;
                v.cards[id] = a;
            }
            handoff_.clear();
            handoffPending_ = false;
        }
        if (v.regionTop != -1 || v.regionBottom != -1)
            ClearRegion(v.surface->Hwnd(), v.regionTop, v.regionBottom);
        v.surface->Show(r, mode == Mode::Center, origin, animateShow);
        return;
    }
    const RECT old = v.surface->Rect();
    if (!EqualRect(&old, &r))
        ClearRegion(v.surface->Hwnd(), v.regionTop, v.regionBottom);
    if (!corner_) {
        const float dy = float(old.top - r.top) / scale;
        if (dy != 0)
            for (auto& [id, a] : v.cards)
                a.y += dy;
    }
    v.surface->Move(r);
    v.surface->Invalidate();
}

void Notifications::LayoutCorner(Mode mode, View& v, size_t shownCount)
{
    MONITORINFO primary{sizeof(primary)};
    const HMONITOR main = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(main, &primary);
    RECT work = primary.rcWork;
    if (RECT bar = barRect ? barRect() : RECT{}; !IsRectEmpty(&bar) && MonitorFromRect(&bar, MONITOR_DEFAULTTONULL) == main) {
        const BarEdge e = EdgeOf(bar);
        if (e == BarEdge::Top && !bottom_)
            work.top = std::min(work.bottom - 1, std::max(work.top, bar.bottom));
        else if (e == BarEdge::Left && left_)
            work.left = std::min(work.right - 1, std::max(work.left, bar.right));
        else if (e == BarEdge::Right && !left_)
            work.right = std::max(work.left + 1, std::min(work.right, bar.left));
        else if (e == BarEdge::Bottom && bottom_)
            work.bottom = std::max(work.top + 1, std::min(work.bottom, bar.top));
    }
    const float scale = PopupScale(work);
    const auto layout =
        notification::CornerLayout(work, scale, kW, CardHeight(), kGap, kPopupGapDip, shownCount, ShowControls(mode) ? kPill + kGap : 0, left_, bottom_);
    v.contentH = layout.contentHeight, v.viewH = layout.viewHeight, v.contentTop = layout.contentTop;
    v.scrollTarget = std::clamp(v.scrollTarget, 0.f, std::max(0.f, v.contentH - v.viewH));
    v.scroll = std::clamp(v.scroll, 0.f, std::max(0.f, v.contentH - v.viewH));
    const POINT at{left_ ? layout.rect.left : layout.rect.right, bottom_ ? layout.rect.bottom : layout.rect.top};
    CommitSurfaceRect(mode, v, layout.rect, scale, at, mode == Mode::Center, false);
}

void Notifications::LayoutAnchored(Mode mode, View& v, size_t shownCount)
{
    const RECT bell = bellRect ? bellRect() : RECT{}, bar = barRect ? barRect() : bell;
    barEdge_ = EdgeOf(bar);
    const bool fromTop = barEdge_ == BarEdge::Top;
    const float s = PopupScale(bar), card = CardHeight(), pill = ShowControls(mode) ? kPill + kGap : 0;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    float content = 0;
    if (shownCount && (mode == Mode::Popup || (!expanded_ && shownCount > 1)))
        content = card + kPeek * float(std::min<size_t>(2, shownCount - 1));
    else if (shownCount)
        content = float(shownCount) * card + float(shownCount - 1) * kGap;
    v.contentTop = fromTop ? pill : 0;
    const int gap = int(std::lround(kPopupGapDip * s)), margin = int(std::lround(8 * s)), minH = int(std::lround((card + pill) * s));
    int anchorY = fromTop ? int(bar.bottom) + gap : int(bar.top) - gap;
    if (!fromTop && Vertical(barEdge_))
        anchorY = std::clamp(int(bar.bottom), int(wa.top) + margin + minH, int(wa.bottom) - margin);
    const float maxContent = float(fromTop ? wa.bottom - margin - anchorY : anchorY - wa.top - margin) / s - pill;
    v.contentH = content;
    v.viewH = mode == Mode::Popup ? std::min(card + 2 * kPeek + kStackLift, std::max(card, maxContent)) : std::max(card, maxContent);
    v.scrollTarget = std::clamp(v.scrollTarget, 0.f, std::max(0.f, v.contentH - v.viewH));
    v.scroll = std::clamp(v.scroll, 0.f, std::max(0.f, v.contentH - v.viewH));
    const int w = int(std::lround(kW * s)), h = int(std::lround((v.viewH + pill) * s));
    int left = std::clamp(int(bar.right), int(wa.left) + margin + w, int(wa.right) - margin) - w;
    if (barEdge_ == BarEdge::Left)
        left = std::min(int(wa.right) - margin - w, int(bar.right) + gap);
    else if (barEdge_ == BarEdge::Right)
        left = std::max(int(wa.left) + margin, int(bar.left) - gap - w);
    const int top = fromTop ? anchorY : std::max(int(wa.top) + margin, anchorY - h);
    const RECT r{left, top, left + w, fromTop ? std::min(int(wa.bottom) - margin, anchorY + h) : anchorY};
    const bool handoff = mode == Mode::Center && handoffPending_;
    CommitSurfaceRect(mode, v, r, s, PopupOrigin(bar, r, bell), !handoff, handoff);
}

float Notifications::Ease(View& v, Canvas& c, const std::wstring& id, bool on, float dt)
{
    float& g = v.glow[id];
    if (Glide(g, on ? 1.f : 0.f, dt, on ? 20.f : 11.f, 0.01f))
        c.Continue();
    return g;
}

void Notifications::PushStack(View& v, size_t n, int wheelDelta)
{
    if (n < 2)
        return;
    const float last = float(n - 1);
    if (std::abs(wheelDelta) < WHEEL_DELTA) {
        float step = -float(wheelDelta) / WHEEL_DELTA;
        if (v.stackPos + step < 0 || v.stackPos + step > last)
            step *= 0.35f; // past the end it gives only a little
        v.stackPos = std::clamp(v.stackPos + step, -0.3f, last + 0.3f);
        v.stackVel = 0;
        v.stackGoal = -1;
        v.stackMoving = true;
        v.stackFollow = GetTickCount64();
        v.surface->Invalidate();
        return;
    }
    v.stackFollow = 0;
    if (!v.stackMoving || v.stackGoal >= 0)
        v.stackFrom = int(std::lround(std::clamp(v.stackPos, 0.f, last)));
    float push = -float(wheelDelta) / WHEEL_DELTA * kStackPush;
    if ((push < 0 && v.stackPos <= 0.01f) || (push > 0 && v.stackPos >= last - 0.01f))
        push *= 0.35f; // past the end it gives only a little
    if ((push > 0) != (v.stackVel > 0))
        v.stackVel = 0; // turning back stops the coast first
    v.stackVel += push;
    v.stackGoal = -1;
    v.stackMoving = true;
    v.surface->Invalidate();
}

bool Notifications::StepStack(View& v, size_t n, float dtMs)
{
    if (!v.stackMoving)
        return false;
    const float last = float(n ? n - 1 : 0);
    const float t = std::clamp(dtMs, 0.f, 50.f) / 1000.f;
    if (v.stackFollow) {
        if (GetTickCount64() - v.stackFollow < kStackFollowMs)
            return true; // still under the fingers
        v.stackFollow = 0;
        v.stackGoal = std::clamp(std::round(v.stackPos), 0.f, last);
    }
    const bool out = v.stackPos < 0 || v.stackPos > last;
    if (v.stackGoal < 0 && !out && std::fabs(v.stackVel) > kStackCoast) {
        // Coasting: the wheel's push dies away.
        v.stackVel *= std::exp(-kStackFriction * t);
        v.stackPos += v.stackVel * t;
        return true;
    }
    if (v.stackGoal < 0) {
        // The card it would come to rest on; one notch moves at least one card.
        float goal = std::round(v.stackPos + v.stackVel / kStackFriction);
        if (int(goal) == v.stackFrom && std::fabs(v.stackVel) > 0.01f)
            goal += v.stackVel > 0 ? 1.f : -1.f;
        v.stackGoal = std::clamp(goal, 0.f, last);
        if ((v.stackGoal == 0 && v.stackVel < 0) || (v.stackGoal == last && v.stackVel > 0))
            v.stackVel = std::clamp(v.stackVel, -kStackOverrun, kStackOverrun); // into the end: a small bounce
    }
    v.stackGoal = std::min(v.stackGoal, last); // cards went meanwhile
    // Settling: a critically damped spring (in small steps, steady at any frame rate).
    const float k = kStackSpring, d = 2 * std::sqrt(kStackSpring);
    int steps = std::max(1, int(std::ceil(t / 0.004f)));
    float h = t / float(steps);
    for (int i = 0; i < steps; ++i) {
        v.stackVel += (k * (v.stackGoal - v.stackPos) - d * v.stackVel) * h;
        v.stackPos += v.stackVel * h;
    }
    if (std::fabs(v.stackGoal - v.stackPos) < 0.002f && std::fabs(v.stackVel) < 0.02f) {
        v.stackPos = v.stackGoal;
        v.stackVel = 0;
        v.stackMoving = false;
        Log(L"notify: stack settled on card %d of %zu", int(v.stackGoal) + 1, n);
        v.stackGoal = -1;
        return false;
    }
    return true;
}

void Notifications::ShiftStack(Mode mode, size_t index, int by)
{
    View& v = ViewOf(mode);
    if (corner_)
        return;
    if (!v.surface->Visible())
        return;
    int front = int(std::lround(v.stackPos));
    if (by > 0) {
        // A new card at the front: a stack turned to older ones comes back to show it.
        if (v.stackPos > 0.001f || v.stackMoving) {
            v.stackPos += 1;
            v.stackVel = 0;
            v.stackGoal = 0;
            v.stackMoving = true;
            v.stackFollow = 0;
        }
    } else if (int(index) < front) {
        v.stackPos -= 1; // a newer card went: the one in front stays
        if (v.stackGoal > 0)
            v.stackGoal -= 1;
    }
    v.surface->Invalidate();
}

// A card's content over its own piece of glass. Cards behind the newest one
// in the stack show only their glass's edge.
void Notifications::PaintCard(Canvas& c, const Notice& n, D2D1_RECT_F r, int index, View& v, bool full, float alpha, bool hideBody)
{
    if (!full)
        return;
    const Theme& t = c.theme;
    const float cardR = CardRadius();
    std::wstring cardId = L"card:" + std::to_wstring(index), closeId = L"close:" + std::to_wstring(index);
    bool hot = v.hover == cardId || v.hover == closeId;
    bool pressed = v.pressed == cardId || (index == v.front && v.pressed == L"stack");
    float glow = Ease(v, c, cardId, hot, float(std::clamp(c.now - v.lastPaint, 0.0, 100.0)));
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    // A pressed card gives way a little, like the glass under it.
    c.BeginGroup(alpha, pressed ? 0.975f : 1 + 0.008f * glow, cx, cy);
    if (c.panel)
        c.Fill(r, cardR, t.panel);
    if (glow > 0)
        c.Fill(r, cardR, D2D1::ColorF(t.hover.r, t.hover.g, t.hover.b, t.hover.a * 0.6f * glow));
    if (v.rippleId == cardId && c.now - v.rippleMs < kRippleMs) {
        c.Ripple(r, cardR, v.rippleX, v.rippleY, float((c.now - v.rippleMs) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
        c.Continue();
    }
    const float x = r.left, y = r.top, w = r.right - r.left;
    float tx = x + 20, ty = y + 16;
    if (n.unread)
        c.Circle(tx + 3, ty + 8, 3.5f, t.accent);
    float nameX = tx + (n.unread ? 12 : 0);
    std::wstring app = n.app.empty() ? L"Atrium" : n.app;
    if (n.count > 1)
        app = TrFormat(L"%s  ·  %d messages", app.c_str(), n.count);
    c.Text(app, D2D1::RectF(nameX, ty, x + w - 170, ty + 16), 12, t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::wstring when = Ago(n.time);
    if (v.stacked && v.count > 1 && index == v.front) // which card of the stack is in front (the wheel turns it)
        when = std::to_wstring(index + 1) + L"/" + std::to_wstring(v.count) + L"  ·  " + when;
    c.Text(when, D2D1::RectF(x + w - 170, ty, x + w - 50, ty + 16), 12, t.faint, DWRITE_FONT_WEIGHT_NORMAL, 2);
    c.Text(n.title, D2D1::RectF(tx, ty + 22, x + w - 20, ty + 42), 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::wstring body = hideBody ? Tr(L"Content hidden") : n.body;
    c.Text(body, D2D1::RectF(tx, ty + 46, x + w - 20, ty + 46 + 36), 13, hideBody ? t.faint : t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    // Dismiss: a round button in the top-right corner, shown with the pointer on the card.
    D2D1_RECT_F close = D2D1::RectF(x + w - 44, y + 10, x + w - 16, y + 38);
    if (glow > 0.01f) {
        c.Fill(close, 14, D2D1::ColorF(t.hover.r, t.hover.g, t.hover.b, (v.hover == closeId ? t.press.a * 1.6f : t.hover.a * 1.2f) * glow));
        c.Glyph(L"\uE711", close, 10, D2D1::ColorF(t.text.r, t.text.g, t.text.b, t.text.a * glow));
    }
    c.EndGroup();
    v.hits.push_back({close, closeId});
}

struct Notifications::PaintFrame {
    float w = 0, dt = 16, listH = 0, listTop = 0, listBottom = 0, top = 0, bottom = 0;
    float card = 0, cardR = 0;
    bool column = false, dockTop = false, fromTop = false, stack = false, moving = false, hideBody = false;
    std::vector<ShapeDip> shapes;
    std::vector<D2D1_RECT_F> rects;
    std::vector<float> alphas;
};

void Notifications::StepSingleCard(View& v, Canvas& c, size_t i, const Notice& n, PaintFrame& f)
{
    const CardTarget ct = ComputeCardTarget(i, f.stack, f.dockTop, f.column, bottom_, f.fromTop, f.listTop, f.listH, f.card, v.scroll, v.stackPos);
    const bool others = !v.cards.empty();
    CardAnim& a = v.cards[n.id];
    if (!a.placed) {
        a = {ct.y + (!f.column ? (f.dockTop ? -18.f : 18.f) : 0.f), ct.inset, 0, 1, true};
        a.x = f.column ? (left_ ? -f.w : f.w) : 0;
        if (f.column && others)
            a.enterAt = c.now + kMakeRoomMs;
    }
    bool cardMoving = false;
    if (f.column && c.now < a.enterAt)
        cardMoving = true;
    else if (f.stack && v.stackMoving)
        a.y = ct.y, a.inset = ct.inset, a.alpha = ct.alpha, a.content = ct.content;
    else {
        cardMoving = Glide(a.y, ct.y, f.dt, 16, 0.2f);
        cardMoving = Glide(a.inset, ct.inset, f.dt, 16, 0.1f) || cardMoving;
        cardMoving = Glide(a.alpha, ct.alpha, f.dt, 12, 0.01f) || cardMoving;
        cardMoving = Glide(a.content, ct.content, f.dt, 9, 0.01f) || cardMoving;
        cardMoving = Glide(a.x, 0, f.dt, 16, 0.2f) || cardMoving;
    }
    float alpha = a.alpha, dx = a.x, shrink = 0;
    if (auto l = leaving_.find(n.id); l != leaving_.end()) {
        const float e = motion::EaseOut(float(std::max(0.0, c.now - l->second) / kLeaveMs));
        dx += (f.column && left_ ? -36 : 36) * e, shrink = 0.07f * e, alpha *= (1 - e) * (1 - e), cardMoving = true;
    }
    if (cardMoving)
        c.Continue();
    f.moving |= cardMoving;
    const float sx = (f.w - 2 * a.inset) * shrink / 2, sy = f.card * shrink / 2;
    f.rects[i] = D2D1::RectF(a.inset + dx + sx, a.y + sy, f.w - a.inset + dx - sx, a.y + f.card - sy);
    f.alphas[i] = alpha;
    if (alpha > 0.02f) {
        f.top = std::min(f.top, std::max(0.f, f.rects[i].top));
        if (f.rects[i].top < f.listBottom && f.rects[i].bottom > f.listTop)
            f.bottom = std::max(f.bottom, std::min(f.listBottom, f.rects[i].bottom));
    }
}

void Notifications::StepCards(View& v, Canvas& c, const std::vector<const Notice*>& shown, PaintFrame& f)
{
    f.moving = Glide(v.scroll, v.scrollTarget, f.dt, 18, 0.2f);
    if (f.stack) {
        const float last = float(shown.empty() ? 0 : shown.size() - 1);
        if (!v.stackMoving && v.stackPos > last + 0.001f)
            v.stackGoal = last, v.stackMoving = true;
        f.moving |= StepStack(v, shown.size(), f.dt);
        v.front = int(std::lround(std::clamp(v.stackPos, 0.f, last)));
    } else {
        v.stackPos = v.stackVel = 0, v.stackMoving = false, v.front = 0;
    }
    v.stacked = f.stack, v.count = shown.size();
    if (f.moving)
        c.Continue();
    f.rects.resize(shown.size()), f.alphas.resize(shown.size());
    for (size_t i = 0; i < shown.size(); ++i)
        StepSingleCard(v, c, i, *shown[i], f);
    for (auto it = v.cards.begin(); it != v.cards.end();)
        it = std::ranges::any_of(shown, [&](const Notice* n) { return n->id == it->first; }) ? std::next(it) : v.cards.erase(it);
}

void Notifications::PaintCardLayer(View& v, Canvas& c, const std::vector<const Notice*>& shown, PaintFrame& f)
{
    c.Clip(D2D1::RectF(-1, f.listTop, f.w + 1, f.listBottom));
    for (size_t i : StackDrawOrder(shown.size(), f.stack, v.stackPos)) {
        const D2D1_RECT_F r = f.rects[i];
        if (f.alphas[i] <= 0.02f || r.bottom < f.listTop || r.top > f.listBottom)
            continue;
        const float k = 0.96f + 0.04f * f.alphas[i], hw = (r.right - r.left) / 2 * k, hh = (r.bottom - r.top) / 2 * k;
        const float mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2;
        const D2D1_RECT_F g = D2D1::RectF(mx - hw, std::max(f.listTop, my - hh), mx + hw, std::min(f.listBottom, my + hh));
        if (g.bottom - g.top > 8)
            f.shapes.push_back({g, f.cardR, 1 - std::clamp(f.alphas[i], 0.f, 1.f)});
        const bool full = !f.stack || v.cards[shown[i]->id].content > 0.01f;
        const size_t hitsBefore = v.hits.size();
        PaintCard(c, *shown[i], r, int(i), v, full, f.alphas[i] * v.cards[shown[i]->id].content, f.hideBody);
        if (!f.stack)
            v.hits.push_back({r, L"card:" + std::to_wstring(i)});
        for (size_t h = hitsBefore; h < v.hits.size(); ++h) {
            v.hits[h].rect.top = std::max(v.hits[h].rect.top, f.listTop);
            v.hits[h].rect.bottom = std::min(v.hits[h].rect.bottom, f.listBottom);
        }
    }
    c.Unclip();
    if (f.stack && !shown.empty())
        v.hits.push_back({D2D1::RectF(0, f.listTop, f.w, f.listBottom), shown.size() > 1 ? L"stack" : L"card:0"});
}

void Notifications::PaintControlsPill(Mode mode, View& v, Canvas& c, size_t shownCount, PaintFrame& f)
{
    if (!ShowControls(mode))
        return;
    const Theme& t = c.theme;
    const float pillTop = f.fromTop ? 0 : f.listTop + f.listH + kGap;
    const D2D1_RECT_F pill = D2D1::RectF(0, pillTop, f.w, pillTop + kPill);
    f.top = std::min(f.top, pill.top), f.bottom = std::max(f.bottom, pill.bottom);
    f.shapes.push_back({pill, kPill / 2});
    if (c.panel)
        c.Fill(pill, kPill / 2, t.panel);
    const size_t live = LiveCount(mode, shownCount);
    const std::wstring label = live ? TrFormat(L"Notifications  ·  %d", int(live)) : Tr(L"No new notifications");
    c.Text(label, D2D1::RectF(pill.left + 20, pill.top, f.w / 2 + 40, pill.bottom), 13.5f, live ? t.text : t.subtext,
           live ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    float x = pill.right - 6;
    auto button = [&](const std::wstring& id, const std::wstring& text) {
        const float bw = c.TextWidth(text, 12) + 28;
        const D2D1_RECT_F b = D2D1::RectF(x - bw, pill.top + 6, x, pill.bottom - 6);
        const float g = Ease(v, c, id, v.hover == id, f.dt);
        c.Fill(b, (b.bottom - b.top) / 2, Alpha(v.pressed == id ? t.press : t.hover, 0.45f + 0.55f * g));
        c.Text(text, b, 12, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
        v.hits.push_back({b, id});
        x = b.left - 6;
    };
    if (live)
        button(L"clear", Tr(L"Clear All"));
    if (CanCollapse(mode, f.column) && expanded_ && shownCount > 1)
        button(L"collapse", Tr(L"Collapse"));
}

void Notifications::Paint(Mode mode, Canvas& c)
{
    View& v = ViewOf(mode);
    const RECT rr = v.surface->Rect();
    v.hits.clear();
    auto shown = Shown(mode);
    PaintFrame f;
    f.w = float(rr.right - rr.left) / c.scale;
    f.dt = v.lastPaint > 0 ? float(std::clamp(c.now - v.lastPaint, 0.0, 100.0)) : 16.f;
    f.listH = v.viewH, f.listTop = v.contentTop, f.listBottom = f.listTop + f.listH, f.top = f.listBottom;
    f.card = CardHeight(), f.cardR = CardRadius();
    f.column = corner_, f.dockTop = !corner_ && barEdge_ == BarEdge::Top, f.fromTop = (corner_ && !bottom_) || f.dockTop;
    f.stack = !corner_ && (mode == Mode::Popup || (!expanded_ && shown.size() > 1));
    f.hideBody = !shown.empty() && BodyHidden();
    StepCards(v, c, shown, f);
    PaintCardLayer(v, c, shown, f);
    PaintControlsPill(mode, v, c, shown.size(), f);
    if (f.shapes.empty())
        f.shapes.push_back({D2D1::RectF(f.w / 2, f.listTop, f.w / 2 + 1, f.listTop + 1), 0, 1.f});
    v.surface->SetShapes(f.shapes, f.moving);
    v.lastPaint = c.now;
    const int regionTop = f.fromTop || (!f.column && f.moving) ? -1 : int(std::lround(f.top * c.scale));
    SyncToastRegion(v.surface->Hwnd(), v.regionTop, v.regionBottom, rr, regionTop, f.fromTop && !(f.dockTop && f.moving), f.bottom, c.scale);
}

} // namespace atrium
