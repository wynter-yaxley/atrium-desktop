#include "ui/widget_internal.h"

#include "localization.h"
#include "log.h"

#include <imm.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace atrium {

using namespace widget_detail;

namespace {

size_t StepBack(const std::wstring& text, size_t i)
{
    if (i >= 2 && IS_LOW_SURROGATE(text[i - 1]) && IS_HIGH_SURROGATE(text[i - 2]))
        return i - 2;
    return i ? i - 1 : 0;
}

size_t StepAhead(const std::wstring& text, size_t i)
{
    if (i + 1 < text.size() && IS_HIGH_SURROGATE(text[i]) && IS_LOW_SURROGATE(text[i + 1]))
        return i + 2;
    return std::min(i + 1, text.size());
}

} // namespace

int DesktopWidgets::GripAt(const Widget& w, POINT screen) const
{
    if (!w.surface || !Resizable(w.kind))
        return 0;
    RECT r = w.surface->Rect();
    int g = int(std::lround(kGrip * Scale()));
    if (screen.y < r.bottom - g || !PtInRect(&r, screen))
        return 0;
    return screen.x < r.left + g ? -1 : screen.x >= r.right - g ? 1 : 0;
}

// Dragging the grip: the size whose corner is nearest the pointer, among those the widget has
// and that fit where it stands; it glides there at once, as the sizes of the menu do.
void DesktopWidgets::DragSize(Widget& w, POINT screen)
{
    float s = Scale();
    const RECT& d = w.downRect; // the edges that stay: the top, and the side away from the grip
    float width = float(w.growLeft ? d.right - screen.x : screen.x - d.left) / s;
    float height = float(screen.y - d.top) / s;
    const float half = kWidgetCell + (kWidgetGap + kWidgetCell) / 2; // between one cell and two
    WidgetSize size = height > half ? WidgetSize::Large : width > half ? WidgetSize::Medium : WidgetSize::Small;
    if (size == WidgetSize::Large && !WidgetSupports(w.kind, size))
        size = WidgetSize::Medium;
    if (!WidgetSupports(w.kind, size))
        size = WidgetSize::Small;
    // The column that keeps the fixed side where it was (near the middle a size may be placed
    // from the other edge, CellRect): within half a cell, or the widget stays as it is.
    int col = -1;
    LONG best = LONG(std::lround((kWidgetCell + kWidgetGap) * s / 2));
    for (int c = std::max(0, w.downCol - 2); c <= w.downCol + 2; ++c) {
        if (!Fits(c, w.row, size, &w))
            continue;
        RECT r = CellRect(c, w.row, size);
        LONG off = std::abs(w.growLeft ? r.right - d.right : r.left - d.left);
        if (off <= best) {
            best = off;
            col = c;
        }
    }
    if (col < 0 || (size == w.size && col == w.col))
        return;
    w.size = size;
    w.col = col;
    Place(w, true);
    w.surface->Invalidate();
}

LRESULT DesktopWidgets::MouseActivate(const Widget& w, LPARAM lp) const
{
    if (!Editable(w.kind) || HIWORD(lp) != WM_LBUTTONDOWN || LOWORD(lp) != HTCLIENT || !w.surface)
        return MA_NOACTIVATE;
    POINT pt{};
    if (!GetCursorPos(&pt) || GripAt(w, pt) != 0)
        return MA_NOACTIVATE;
    if (w.kind == WidgetKind::Notes)
        return MA_ACTIVATE;
    ScreenToClient(w.surface->Hwnd(), &pt);
    const D2D1_POINT_2F p = D2D1::Point2F(float(pt.x) / w.surface->Scale(), float(pt.y) / w.surface->Scale());
    for (const WidgetHit& h : w.hits)
        if (h.id != L"caret" && p.x >= h.rect.left && p.x < h.rect.right && p.y >= h.rect.top && p.y < h.rect.bottom)
            return (h.id.starts_with(L"item:") || h.id == L"add") ? MA_ACTIVATE : MA_NOACTIVATE;
    return MA_NOACTIVATE;
}

bool DesktopWidgets::UpdateCursor(const Widget& w, LPARAM lp, LRESULT& r) const
{
    POINT p;
    GetCursorPos(&p);
    int grip = w.resizing ? (w.growLeft ? -1 : 1) : w.down ? 0 : GripAt(w, p);
    if (LOWORD(lp) != HTCLIENT || !grip)
        return false;
    SetCursor(LoadCursorW(nullptr, grip < 0 ? IDC_SIZENESW : IDC_SIZENWSE));
    r = TRUE;
    return true;
}

bool DesktopWidgets::OnWindowPosChanging(WINDOWPOS* pos)
{
    // Activation still gives editable widgets keyboard focus, without changing their layer.
    if (!zMoving_) {
        if (!(pos->flags & SWP_NOZORDER) && !restackPosted_) {
            restackPosted_ = true;
            PostMessageW(host_, WM_WIDGET_RESTACK, 0, 0);
        }
        pos->flags |= SWP_NOZORDER;
    }
    return false;
}

bool DesktopWidgets::OnMouseWheel(Widget& w, WPARAM wp) const
{
    if (w.kind != WidgetKind::Reminders)
        return false;
    w.content.scroll =
        std::clamp(w.content.scroll - float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * kReminderRow * 3, 0.f, ReminderScrollMax(w.content, w.size));
    w.surface->Invalidate();
    return true;
}

bool DesktopWidgets::OnMouseLeave(Widget& w) const
{
    w.tracking = false;
    if (w.hover && !w.down) {
        w.hover = false;
        w.surface->Invalidate();
    }
    return true;
}

bool DesktopWidgets::OnLButtonDown(Widget& w)
{
    w.down = true;
    w.dragging = false;
    GetCursorPos(&w.downAt);
    w.downRect = w.surface->Rect();
    int grip = GripAt(w, w.downAt);
    w.growLeft = grip < 0;
    w.resizing = grip != 0;
    w.downSize = w.size;
    w.downCol = w.col;
    if (w.resizing) {
        EndEdit(w);
        w.surface->ReserveResize(D2D1::SizeF(WidgetWidth(WidgetSupports(w.kind, WidgetSize::Medium) ? WidgetSize::Medium : WidgetSize::Small),
                                             WidgetHeight(WidgetSupports(w.kind, WidgetSize::Large) ? WidgetSize::Large : WidgetSize::Small)));
    }
    SetCapture(w.surface->Hwnd());
    return true;
}

bool DesktopWidgets::OnMouseMove(Widget& w)
{
    if (!w.tracking) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, w.surface->Hwnd(), 0};
        w.tracking = TrackMouseEvent(&t) != FALSE;
    }
    if (!w.hover) {
        w.hover = true;
        w.surface->Invalidate();
    }
    if (!w.down)
        return true;
    POINT p;
    GetCursorPos(&p);
    if (w.resizing) {
        DragSize(w, p);
        return true;
    }
    int dx = p.x - w.downAt.x, dy = p.y - w.downAt.y;
    if (!w.dragging && std::abs(dx) + std::abs(dy) > int(6 * Scale())) {
        w.dragging = true;
        w.glideMs = 0;
        EndEdit(w);
        Restack(); // dragging stays on the desktop, below open application windows
    }
    if (w.dragging)
        w.surface->Move(RECT{w.downRect.left + dx, w.downRect.top + dy, w.downRect.right + dx, w.downRect.bottom + dy});
    return true;
}

void DesktopWidgets::FinishResize(Widget& w)
{
    if (w.glideMs <= 0)
        w.surface->ReserveResize(D2D1::SizeF());
    if (w.size != w.downSize) {
        if (w.kind == WidgetKind::Photos)
            LoadPhoto(w, false); // the same photo, cropped for the new size
        Save();
        Log(L"widgets: %s resized to %s at %d,%d", NameOf(w.kind), SizeName(w.size), w.col, w.row);
    }
}

void DesktopWidgets::FinishDrag(Widget& w)
{
    // Snaps to the nearest free place of the grid and glides there.
    RECT cur = w.surface->Rect(), wa = WorkArea();
    float s = Scale();
    // Counted from the edge the widget was dropped nearer to, as CellRect places it.
    float col = (float(cur.left - wa.left) / s - kMargin) / (kWidgetCell + kWidgetGap);
    if (cur.left + cur.right > wa.left + wa.right)
        col = float(Cols() - WidgetCols(w.size)) - (float(wa.right - cur.right) / s - kMargin) / (kWidgetCell + kWidgetGap);
    if (onRight_)
        col = float(Cols() - WidgetCols(w.size)) - col;
    float row = (float(cur.top - wa.top) / s - kMargin) / (kWidgetCell + kWidgetGap);
    int c = w.col, rr = w.row;
    if (FreeCellNear(col, row, w.size, &w, c, rr)) {
        w.col = c;
        w.row = rr;
    }
    Place(w, true);
    Save();
    Restack();
    Log(L"widgets: %s moved to %d,%d", NameOf(w.kind), w.col, w.row);
}

bool DesktopWidgets::OnLButtonUpOrCapture(Widget& w, UINT m, LPARAM lp)
{
    if (!w.down)
        return m == WM_LBUTTONUP;
    w.down = false;
    bool dragged = w.dragging, resized = w.resizing;
    w.dragging = w.resizing = false;
    if (m == WM_LBUTTONUP && GetCapture() == w.surface->Hwnd())
        ReleaseCapture();
    POINT at;
    GetCursorPos(&at);
    if (RECT now = w.surface->Rect(); w.hover && !PtInRect(&now, at)) {
        w.hover = false; // let go outside it (its WM_MOUSELEAVE came while it was held)
        w.surface->Invalidate();
    }
    if (resized)
        FinishResize(w);
    else if (dragged)
        FinishDrag(w);
    else if (m == WM_LBUTTONUP) {
        if (Editable(w.kind))
            Click(w, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        else
            Activate(w);
    }
    return true;
}

bool DesktopWidgets::Message(Widget& w, UINT m, WPARAM wp, LPARAM lp, LRESULT& r)
{
    switch (m) {
    case WM_MOUSEACTIVATE: r = MouseActivate(w, lp); return true;
    case WM_WINDOWPOSCHANGING: return OnWindowPosChanging(reinterpret_cast<WINDOWPOS*>(lp));
    case WM_MOUSEWHEEL: return OnMouseWheel(w, wp);
    case WM_SETCURSOR: return UpdateCursor(w, lp, r);
    case WM_MOUSELEAVE: return OnMouseLeave(w);
    case WM_LBUTTONDOWN: return OnLButtonDown(w);
    case WM_MOUSEMOVE: return OnMouseMove(w);
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: return OnLButtonUpOrCapture(w, m, lp);
    case WM_RBUTTONUP: Menu(w); return true;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: return true;
    case WM_CHAR:
    case WM_KEYDOWN: return w.content.editing && Key(w, m, wp);
    case WM_KILLFOCUS: EndEdit(w); return false;
    case WM_IME_STARTCOMPOSITION: PlaceImeWindow(w); return false; // the input method's own window
    default: return false;
    }
}

void DesktopWidgets::Activate(Widget& w)
{
    POINT p;
    GetCursorPos(&p);
    POINT c = w.surface->ScreenToContent(p);
    float s = w.surface->Scale();
    w.surface->Ripple(float(c.x) / s, float(c.y) / s);
    std::wstring open;
    switch (w.kind) {
    case WidgetKind::Clock:
    case WidgetKind::Digital: open = L"ms-clock:"; break;
    case WidgetKind::Calendar:
        if (onCalendar)
            onCalendar();
        break;
    case WidgetKind::System: open = L"taskmgr.exe"; break;
    case WidgetKind::Battery: open = world_.status.battery ? L"ms-settings:batterysaver" : L"ms-settings:powersleep"; break;
    case WidgetKind::Bluetooth: open = L"ms-settings:bluetooth"; break; // Bluetooth & devices: each device with its battery
    case WidgetKind::Network: open = L"ms-settings:network-status"; break; // 网络和 Internet: the adapters, their speeds and their usage
    case WidgetKind::Photos:
        if (w.content.photo.empty())
            PickFolder(w);
        else
            open = w.content.photo;
        break;
    case WidgetKind::Weather:
        // Windows' weather app, else its web page.
        if (INT_PTR(ShellExecuteW(nullptr, L"open", L"msnweather:", nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            open = L"https://www.msn.cn/zh-cn/weather/forecast";
        break;
    default: break;
    }
    if (!open.empty())
        ShellExecuteW(nullptr, L"open", open.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Log(L"widgets: %s opened", NameOf(w.kind));
}

void DesktopWidgets::Click(Widget& w, POINT content)
{
    if (w.kind == WidgetKind::Notes) {
        StartEdit(w, -1);
        return;
    }
    float s = w.surface->Scale();
    D2D1_POINT_2F p = D2D1::Point2F(float(content.x) / s, float(content.y) / s);
    std::wstring id;
    for (const WidgetHit& h : w.hits)
        if (h.id != L"caret" && p.x >= h.rect.left && p.x < h.rect.right && p.y >= h.rect.top && p.y < h.rect.bottom) {
            id = h.id;
            break;
        }
    WidgetContent& c = w.content;
    if (id.starts_with(L"toggle:")) {
        size_t i = size_t(_wtoi(id.c_str() + 7));
        if (c.editing && c.item == int(i))
            EndEdit(w);
        if (i < c.items.size() && !c.items[i].text.empty()) {
            c.items[i].done = !c.items[i].done;
            SaveSoon();
        }
        w.surface->Invalidate();
    } else if (id.starts_with(L"item:")) {
        StartEdit(w, _wtoi(id.c_str() + 5));
    } else if (id == L"add") {
        EndEdit(w);
        c.items.push_back({});
        StartEdit(w, int(c.items.size()) - 1);
    } else {
        EndEdit(w);
    }
}

void DesktopWidgets::StartEdit(Widget& w, int item)
{
    WidgetContent& c = w.content;
    if (c.editing && c.item != item) {
        // Another reminder: an empty one being left goes, which may move this one up.
        if (w.kind == WidgetKind::Reminders && c.item >= 0 && c.item < item && c.items[size_t(c.item)].text.empty())
            --item;
        EndEdit(w);
    }
    if (w.kind == WidgetKind::Reminders && (item < 0 || item >= int(c.items.size())))
        return;
    c.editing = true;
    c.item = item;
    c.caret = w.kind == WidgetKind::Notes ? c.text.size() : c.items[size_t(item)].text.size();
    c.caretOn = true;
    if (w.kind == WidgetKind::Reminders) {
        float view = WidgetHeight(w.size) - kReminderTop - kReminderBottom;
        float top = float(item) * kReminderRow;
        c.scroll = std::clamp(c.scroll, std::max(0.f, top + kReminderRow - view), top);
    }
    SetTimer(host_, kCaretTimer, kCaretMs, nullptr);
    if (GetForegroundWindow() != w.surface->Hwnd())
        SetForegroundWindow(w.surface->Hwnd());
    SetFocus(w.surface->Hwnd());
    w.surface->Invalidate();
}

void DesktopWidgets::EndEdit(Widget& w)
{
    WidgetContent& c = w.content;
    if (!c.editing)
        return;
    c.editing = false;
    if (w.kind == WidgetKind::Reminders && c.item >= 0 && c.item < int(c.items.size()) && c.items[size_t(c.item)].text.empty())
        c.items.erase(c.items.begin() + c.item); // nothing was typed
    c.item = -1;
    KillTimer(host_, kSaveTimer);
    Save();
    if (w.surface)
        w.surface->Invalidate();
    PostMessageW(host_, WM_WIDGET_RESTACK, 0, 0); // back under the windows
}

void DesktopWidgets::Insert(Widget& w, const std::wstring& s)
{
    WidgetContent& c = w.content;
    std::wstring& text = w.kind == WidgetKind::Notes ? c.text : c.items[size_t(c.item)].text;
    const size_t limit = w.kind == WidgetKind::Notes ? kMaxNoteText : kMaxReminderText;
    std::wstring add = s;
    if (w.kind == WidgetKind::Reminders) // one line
        std::ranges::replace_if(add, [](wchar_t ch) { return ch == L'\r' || ch == L'\n'; }, L' ');
    else
        std::erase(add, L'\r');
    add = ClampText(std::move(add), text.size() < limit ? limit - text.size() : 0);
    c.caret = std::min(c.caret, text.size());
    text.insert(c.caret, add);
    c.caret += add.size();
}

bool DesktopWidgets::HandleKeyDown(Widget& w, std::wstring& text, WPARAM wp, bool& changed)
{
    size_t& at = w.content.caret;
    switch (wp) {
    case VK_LEFT: at = StepBack(text, at); return true;
    case VK_RIGHT: at = StepAhead(text, at); return true;
    case VK_HOME: {
        size_t nl = at ? text.rfind(L'\n', at - 1) : std::wstring::npos;
        at = nl == std::wstring::npos ? 0 : nl + 1;
        return true;
    }
    case VK_END: {
        size_t nl = text.find(L'\n', at);
        at = nl == std::wstring::npos ? text.size() : nl;
        return true;
    }
    case VK_DELETE:
        if (at < text.size()) {
            text.erase(at, StepAhead(text, at) - at);
            changed = true;
        }
        return true;
    default: return false; // WM_CHAR follows for the rest
    }
}

bool DesktopWidgets::PasteClipboard(Widget& w)
{
    bool changed = false;
    if (!OpenClipboard(w.surface->Hwnd()))
        return false;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
            Insert(w, p);
            GlobalUnlock(data);
            changed = true;
        }
    }
    CloseClipboard();
    return changed;
}

bool DesktopWidgets::HandleEnter(Widget& w, std::wstring& text, bool& changed)
{
    if (w.kind == WidgetKind::Notes) {
        Insert(w, L"\n");
        changed = true;
        return true;
    }
    if (text.empty()) {
        EndEdit(w);
        return false;
    }
    // The next reminder, right below.
    WidgetContent& c = w.content;
    int next = c.item + 1;
    c.items.insert(c.items.begin() + next, Reminder{});
    c.editing = false; // the one typed stays as it is
    SaveSoon();
    StartEdit(w, next);
    return false;
}

bool DesktopWidgets::HandleKeyChar(Widget& w, std::wstring& text, wchar_t ch, bool& changed)
{
    size_t& at = w.content.caret;
    if (ch == 27) { // Esc
        EndEdit(w);
        return false;
    }
    if (ch == L'\r')
        return HandleEnter(w, text, changed);
    if (ch == 8) { // Backspace
        if (at) {
            size_t from = StepBack(text, at);
            text.erase(from, at - from);
            at = from;
            changed = true;
        }
        return true;
    }
    if (ch == 22) { // Ctrl+V
        changed = PasteClipboard(w);
        return true;
    }
    if (ch >= 0x20 || ch == L'\t') {
        Insert(w, std::wstring(1, ch));
        changed = true;
        return true;
    }
    return false; // other control keys
}

bool DesktopWidgets::Key(Widget& w, UINT m, WPARAM wp)
{
    WidgetContent& c = w.content;
    if (w.kind == WidgetKind::Reminders && (c.item < 0 || c.item >= int(c.items.size())))
        return false;
    std::wstring& text = w.kind == WidgetKind::Notes ? c.text : c.items[size_t(c.item)].text;
    c.caret = std::min(c.caret, text.size());
    bool changed = false;
    if (m == WM_KEYDOWN) {
        if (!HandleKeyDown(w, text, wp, changed))
            return false;
    } else if (!HandleKeyChar(w, text, wchar_t(wp), changed)) {
        return true;
    }
    if (changed)
        SaveSoon();
    c.caretOn = true;
    SetTimer(host_, kCaretTimer, kCaretMs, nullptr); // the caret stays lit while typing
    w.surface->Invalidate();
    return true;
}

// The input method's composition and candidates next to the caret.
void DesktopWidgets::PlaceImeWindow(Widget& w)
{
    auto caret = std::ranges::find_if(w.hits, [](const WidgetHit& h) { return h.id == L"caret"; });
    if (caret == w.hits.end() || !w.surface)
        return;
    float s = w.surface->Scale();
    POINT p = w.surface->ContentToScreen(POINT{LONG(caret->rect.left * s), LONG(caret->rect.top * s)});
    ScreenToClient(w.surface->Hwnd(), &p);
    HIMC imc = ImmGetContext(w.surface->Hwnd());
    if (!imc)
        return;
    COMPOSITIONFORM cf{CFS_POINT, p, {}};
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand{0, CFS_CANDIDATEPOS, POINT{p.x, p.y + LONG((caret->rect.bottom - caret->rect.top) * s) + 4}, {}};
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(w.surface->Hwnd(), imc);
}

// A world clock's cities: which one to change, then the city in its place (both in menus at `at`).
void DesktopWidgets::PickCity(Widget& w, POINT at)
{
    if (!menu)
        return;
    const std::vector<std::wstring> cities = WidgetCities(w.kind, w.content.cities);
    std::vector<MenuEntry> slots{{Tr(L"Change which city"), 0, false, false, true}};
    for (size_t i = 0; i < cities.size(); ++i)
        slots.push_back({Tr(cities[i].c_str()), int(i) + 1});
    Widget* target = &w;
    menu->Open(std::move(slots), at, MenuPlace::Below, [this, target, at, cities](int slot) {
        if (slot <= 0 || !Has(target))
            return;
        std::vector<MenuEntry> list{{TrFormat(L"Change “%s” to", Tr(cities[size_t(slot - 1)].c_str())), 0, false, false, true}};
        const auto& all = WorldCities();
        for (size_t i = 0; i < all.size(); ++i)
            list.push_back({Tr(all[i].name), int(i) + 1, cities[size_t(slot - 1)] == all[i].name});
        menu->Open(std::move(list), at, MenuPlace::Below, [this, target, slot, cities](int pick) {
            if (pick <= 0 || !Has(target))
                return;
            Widget& wd = *target;
            std::vector<std::wstring> chosen = cities;
            chosen[size_t(slot - 1)] = WorldCities()[size_t(pick - 1)].name;
            wd.content.cities = std::move(chosen);
            Save();
            wd.surface->Invalidate();
            Log(L"widgets: a world clock shows %s", wd.content.cities[size_t(slot - 1)].c_str());
        });
    });
}

void DesktopWidgets::Menu(Widget& w)
{
    if (!menu)
        return;
    constexpr int kSmall = 1, kMedium = 2, kLarge = 3, kRemove = 4, kFolder = 5, kNextPhoto = 6, kClearDone = 7, kAdd = 8, kCities = 9;
    std::vector<MenuEntry> entries;
    auto size = [&](int id, WidgetSize s, const wchar_t* label) { entries.push_back({label, id, w.size == s, !WidgetSupports(w.kind, s)}); };
    size(kSmall, WidgetSize::Small, Tr(L"Small"));
    size(kMedium, WidgetSize::Medium, Tr(L"Medium"));
    size(kLarge, WidgetSize::Large, Tr(L"Large"));
    entries.push_back({});
    if (w.kind == WidgetKind::Photos) {
        entries.push_back({Tr(L"Choose a photo folder…"), kFolder});
        entries.push_back({Tr(L"Next photo"), kNextPhoto});
        entries.push_back({});
    }
    if (HasWorldClocks(w.kind, w.size)) {
        entries.push_back({Tr(L"Choose city…"), kCities});
        entries.push_back({});
    }
    if (w.kind == WidgetKind::Reminders) {
        bool anyDone = std::ranges::any_of(w.content.items, [](const Reminder& r) { return r.done; });
        entries.push_back({Tr(L"Clear completed"), kClearDone, false, !anyDone});
        entries.push_back({});
    }
    entries.push_back({Tr(L"Remove widget"), kRemove});
    entries.push_back({Tr(L"Add Widgets…"), kAdd});
    POINT p;
    GetCursorPos(&p);
    Widget* target = &w;
    menu->Open(std::move(entries), p, MenuPlace::Below, [this, target, p](int cmd) {
        if (cmd == kAdd) {
            if (onAddWidgets)
                onAddWidgets();
            return;
        }
        if (!Has(target))
            return; // gone while the menu was open
        Widget& wd = *target;
        switch (cmd) {
        case kSmall:
        case kMedium:
        case kLarge: Resize(wd, cmd == kSmall ? WidgetSize::Small : cmd == kMedium ? WidgetSize::Medium : WidgetSize::Large); break;
        case kFolder: PickFolder(wd); break;
        case kCities: PickCity(wd, p); break;
        case kNextPhoto: LoadPhoto(wd, true); break;
        case kClearDone:
            EndEdit(wd);
            std::erase_if(wd.content.items, [](const Reminder& r) { return r.done; });
            Save();
            wd.surface->Invalidate();
            break;
        case kRemove: Remove(wd); break; // wd is gone
        default: break;
        }
    });
}

} // namespace atrium
