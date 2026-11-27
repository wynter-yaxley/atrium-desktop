// Launchpad's input: the pointer (a press launches, an app pressed and moved lifts to be moved, as in the
// start menu (launchpad_drag.cpp), a sideways drag on the glass turns the pages, a press on the empty glass closes), the wheel, the keys and typing.
#include "ui/launchpad.h"

#include "clock.h"
#include "localization.h"

#include <windowsx.h>
#include <imm.h>

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr float kDragStart = 8; // DIP sideways before a press turns the pages
constexpr float kRubber = 0.35f; // past the first or last page, the sheet follows this much

} // namespace

bool Launchpad::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_TIMER:
        if (w == kCloseTimer)
            Finish();
        else if (w == kCaretTimer && (searchFocused_ || namingFolder_))
            surface_.Invalidate(); // the caret blinks
        else if (w == kDragTimer)
            DragTick();
        return w == kCloseTimer || w == kCaretTimer || w == kDragTimer;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !menuOpen_)
            Close(false);
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (down_) {
            Drag(pt);
            return true;
        }
        const int h = closing_ ? kGlass : HitAt(pt);
        const int tile = h >= 0 ? h : -1;
        if (tile != hover_) {
            hover_ = tile;
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ != -1) {
            hover_ = -1;
            surface_.Invalidate();
        }
        return true;
    case WM_LBUTTONDOWN:
        Press(pt);
        if (!closing_)
            SetCapture(surface_.Hwnd());
        return true;
    case WM_LBUTTONUP:
        Release(pt);
        if (GetCapture() == surface_.Hwnd())
            ReleaseCapture();
        return true;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(l) != surface_.Hwnd() && (down_ || dragging_ || pressed_ != -1)) {
            DropLifted();
            down_ = dragging_ = false;
            pressed_ = -1;
            SetPage(int(std::lround(pageShown_)));
        }
        return true;
    case WM_RBUTTONUP: AppMenu(pt); return true;
    case WM_MOUSEWHEEL: Wheel(GET_WHEEL_DELTA_WPARAM(w)); return true;
    case WM_MOUSEHWHEEL: Wheel(-GET_WHEEL_DELTA_WPARAM(w)); return true;
    case WM_KEYDOWN: return Key(w);
    case WM_CHAR:
        Type(wchar_t(w));
        result = 0;
        return true;
    case WM_IME_SETCONTEXT:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION: return ImeMessage(m, w, l, result);
    default: return false;
    }
}

bool Launchpad::ImeMessage(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    switch (m) {
    case WM_IME_SETCONTEXT: result = DefWindowProcW(surface_.Hwnd(), m, w, l & ~LPARAM(ISC_SHOWUICOMPOSITIONWINDOW)); return true;
    case WM_IME_STARTCOMPOSITION:
        searchFocused_ = !namingFolder_;
        surface_.Invalidate();
        PlaceIme();
        result = 0;
        return true;
    case WM_IME_COMPOSITION:
        if (l & GCS_COMPSTR) {
            if (HIMC imc = ImmGetContext(surface_.Hwnd())) {
                const LONG bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
                composition_.assign(bytes > 0 ? size_t(bytes) / sizeof(wchar_t) : 0, L'\0');
                if (bytes > 0) {
                    const LONG got = ImmGetCompositionStringW(imc, GCS_COMPSTR, composition_.data(), DWORD(bytes));
                    composition_.resize(got > 0 ? size_t(got) / sizeof(wchar_t) : 0);
                }
                ImmReleaseContext(surface_.Hwnd(), imc);
            }
            searchFocused_ = !namingFolder_ && (searchFocused_ || !composition_.empty());
        } else if (l == 0 || (l & GCS_RESULTSTR)) {
            composition_.clear();
        }
        surface_.Invalidate();
        PlaceIme();
        if (l & GCS_RESULTSTR)
            return false; // DefWindowProcW posts WM_CHAR for the committed text
        result = 0;
        return true;
    case WM_IME_ENDCOMPOSITION:
        composition_.clear();
        surface_.Invalidate();
        PlaceIme();
        result = 0;
        return true;
    default: return false;
    }
}

bool Launchpad::Key(WPARAM key)
{
    if (closing_)
        return false;
    if (namingFolder_) {
        if (key == VK_ESCAPE)
            FinishFolderNaming(false);
        else if (key == VK_RETURN)
            FinishFolderNaming(true);
        else if (key == VK_BACK && !nameDraft_.empty()) {
            nameDraft_.pop_back();
            surface_.Invalidate();
        }
        return true;
    }
    const int n = int(Shown().size());
    auto move = [&](int dx, int dy) {
        int page = page_;
        sel_ = MoveSelection(sel_, dx, dy, n, grid_.cols, grid_.rows, page);
        SetPage(page);
    };
    switch (key) {
    case VK_ESCAPE:
        if (!query_.empty()) {
            query_.clear(); // a search goes first, then the folder, then Launchpad
            composition_.clear();
            searchFocused_ = false;
            Filter();
        } else if (!openFolder_.empty()) {
            ShowFolder({});
        } else {
            Close(false);
        }
        break;
    case VK_F2: StartFolderNaming(); return true;
    case VK_LEFT: move(-1, 0); break;
    case VK_RIGHT: move(1, 0); break;
    case VK_UP: move(0, -1); break;
    case VK_DOWN: move(0, 1); break;
    case VK_PRIOR: SetPage(page_ - 1); break;
    case VK_NEXT: SetPage(page_ + 1); break;
    case VK_HOME: SetPage(0); break;
    case VK_END: SetPage(Pages() - 1); break;
    case VK_RETURN: Launch(sel_); break;
    case VK_BACK:
        if (query_.empty())
            return true;
        query_.pop_back();
        Filter();
        break;
    default: return false;
    }
    surface_.Invalidate();
    return true;
}

// Typing goes to the open folder's title while renaming, otherwise to the search.
void Launchpad::Type(wchar_t ch)
{
    if (closing_ || ch < 32 || ch == 127)
        return;
    if (namingFolder_) {
        if (nameDraft_.size() < 40) {
            nameDraft_ += ch;
            PlaceIme();
            surface_.Invalidate();
        }
        return;
    }
    if (query_.size() >= 64)
        return;
    searchFocused_ = true;
    query_ += ch;
    Filter();
    surface_.Invalidate();
    PlaceIme();
}

void Launchpad::Press(POINT pt)
{
    if (closing_)
        return;
    down_ = true;
    dragging_ = false;
    downPt_ = pt;
    dragFrom_ = pageShown_;
    dragSpeed_ = 0;
    dragMs_ = ClockMs();
    lastDragX_ = float(pt.x) / surface_.Scale();
    pressed_ = HitAt(pt);
    if (namingFolder_ && pressed_ != kFolderTitle)
        FinishFolderNaming(true);
    searchFocused_ = !namingFolder_ && pressed_ == kSearch;
    surface_.Invalidate();
}

void Launchpad::Drag(POINT pt)
{
    if (lifted_ >= 0) {
        MoveLifted(pt);
        return;
    }
    // An app pressed and moved past the system's drag distance lifts at once, as a start menu tile.
    if (!dragging_ && pressed_ >= 0 && Arranging()) {
        if (std::abs(pt.x - downPt_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - downPt_.y) > GetSystemMetrics(SM_CYDRAG)) {
            Lift();
            if (lifted_ >= 0)
                MoveLifted(pt);
        }
        return;
    }
    const float s = surface_.Scale(), x = float(pt.x) / s, dx = float(pt.x - downPt_.x) / s;
    if (!dragging_ && std::abs(dx) < kDragStart)
        return;
    if (!dragging_) {
        dragging_ = true;
        pressed_ = -1; // a drag launches nothing and closes nothing
    }
    // The sheet follows the pointer; past the ends it gives only a little, as a rubber band.
    const float raw = dragFrom_ - dx / w_, end = float(Pages() - 1);
    const float held = std::clamp(raw, 0.f, end);
    pageShown_ = held + std::clamp((raw - held) * kRubber, -0.45f, 0.45f);
    const double now = ClockMs();
    if (now > dragMs_) {
        const float speed = (x - lastDragX_) / w_ / float((now - dragMs_) / 1000.0);
        dragSpeed_ = dragSpeed_ * 0.6f + speed * 0.4f; // pointer pages/s, the last moves weighing most
    }
    dragMs_ = now;
    lastDragX_ = x;
    surface_.Invalidate();
}

void Launchpad::Release(POINT pt)
{
    if (!down_)
        return;
    down_ = false;
    if (lifted_ >= 0) {
        DropLifted();
        return;
    }
    const int was = pressed_;
    pressed_ = -1;
    if (dragging_) {
        dragging_ = false;
        const float moved = float(pt.x - downPt_.x) / surface_.Scale() / w_;
        const int basePage = std::clamp(int(std::lround(dragFrom_)), 0, Pages() - 1);
        SetPage(SettlePageDrag(basePage, Pages(), moved, ClockMs() - dragMs_ > 120 ? 0.f : dragSpeed_));
        pageSpeed_ = -dragSpeed_; // the glide goes on from the drag's pace
        return;
    }
    const int at = HitAt(pt);
    if (was >= 0 && at == was)
        Launch(was);
    else if (was == kFolderTitle && at == kFolderTitle && IsCustomFolder(openFolder_))
        StartFolderNaming();
    else if (was <= -2 && was > kFolderTitle && at == was)
        SetPage(-2 - was);
    else if (was == kGlass && at == kGlass && !openFolder_.empty() && query_.empty())
        ShowFolder({}); // the empty glass around a folder's apps closes the folder
    else if (was == kGlass && at == kGlass && full_)
        Close(false); // all of the screen: its empty glass is the outside
    surface_.Invalidate();
}

void Launchpad::OpenFolderMenu(const std::wstring& folderId, POINT screen)
{
    if (!menu || !IsCustomFolder(folderId))
        return;
    menuOpen_ = true;
    hover_ = -1;
    surface_.Invalidate();
    std::vector<MenuEntry> entries = {{Tr(L"Open"), 1}, {Tr(L"Rename"), 2}, {}, {Tr(L"Ungroup folder"), 3}};
    menu->Open(
        std::move(entries), screen, MenuPlace::Below,
        [this, folderId](int cmd) {
            if (cmd == 1)
                ShowFolder(folderId);
            else if (cmd == 2) {
                ShowFolder(folderId);
                StartFolderNaming();
            } else if (cmd == 3)
                UngroupFolder(folderId);
        },
        [this] {
            menuOpen_ = false;
            if (!surface_.Visible() || closing_)
                return;
            DWORD pid = 0;
            const HWND fg = GetForegroundWindow();
            if (fg)
                GetWindowThreadProcessId(fg, &pid);
            if (!fg || pid == GetCurrentProcessId())
                SetForegroundWindow(surface_.Hwnd());
            else
                Close(false);
        });
}

void Launchpad::OpenBackgroundMenu(POINT screen)
{
    if (!menu || layout_.hidden.empty())
        return;
    menuOpen_ = true;
    hover_ = -1;
    surface_.Invalidate();
    std::vector<MenuEntry> entries = {{Tr(L"Removed from Launchpad"), 0, false, false, true}};
    std::vector<std::wstring> hiddenIds;
    const size_t limit = std::min<size_t>(12, layout_.hidden.size());
    for (size_t i = 0; i < limit; ++i) {
        const std::wstring& id = layout_.hidden[i];
        auto it = std::ranges::find(apps_, id, &CatalogApp::id);
        hiddenIds.push_back(id);
        entries.push_back({it != apps_.end() ? it->name : id, 100 + int(i)});
    }
    entries.push_back({});
    entries.push_back({Tr(L"Restore all removed apps"), 1});
    menu->Open(
        std::move(entries), screen, MenuPlace::Below,
        [this, hiddenIds = std::move(hiddenIds)](int cmd) {
            if (cmd == 1)
                RestoreAllHidden();
            else if (cmd >= 100 && size_t(cmd - 100) < hiddenIds.size())
                RestoreApp(hiddenIds[size_t(cmd - 100)]);
        },
        [this] {
            menuOpen_ = false;
            if (!surface_.Visible() || closing_)
                return;
            DWORD pid = 0;
            const HWND fg = GetForegroundWindow();
            if (fg)
                GetWindowThreadProcessId(fg, &pid);
            if (!fg || pid == GetCurrentProcessId())
                SetForegroundWindow(surface_.Hwnd());
            else
                Close(false);
        });
}

void Launchpad::AppMenu(POINT pt)
{
    const int at = closing_ || down_ || lifted_ >= 0 ? kGlass : HitAt(pt);
    const POINT screen = surface_.ContentToScreen(pt);
    if (at == kGlass) {
        if (!closing_ && !down_ && lifted_ < 0 && query_.empty() && openFolder_.empty() && !layout_.hidden.empty())
            OpenBackgroundMenu(screen);
        return;
    }
    if (at < 0 || size_t(at) >= Shown().size())
        return;
    const CatalogApp app = Shown()[size_t(at)];
    if (IsCustomFolder(app.id)) {
        OpenFolderMenu(app.id, screen);
        return;
    }
    if (FolderApps(app.id) != nullptr || !onAppMenu)
        return;
    std::vector<AppMenuExtra> extras;
    if (IsCustomFolder(openFolder_))
        extras.push_back({Tr(L"Remove from folder"), [this, f = openFolder_, id = app.id] { RemoveAppFromFolder(f, id); }, true, true});
    extras.push_back({Tr(L"Remove from Launchpad"), [this, app] { HideApp(app); }, !IsCustomFolder(openFolder_), true});
    menuOpen_ = true;
    hover_ = -1;
    surface_.Invalidate();
    onAppMenu(
        app, screen,
        [this](bool picked) {
            menuOpen_ = false;
            if (!surface_.Visible() || closing_)
                return;
            // Back to Launchpad, unless a row was picked or the click that closed the menu went elsewhere.
            DWORD pid = 0;
            const HWND fg = GetForegroundWindow();
            if (fg)
                GetWindowThreadProcessId(fg, &pid);
            if (!picked && (!fg || pid == GetCurrentProcessId()))
                SetForegroundWindow(surface_.Hwnd());
            else
                Close(false);
        },
        std::move(extras));
}

void Launchpad::Wheel(int delta)
{
    if (closing_)
        return;
    wheel_ += delta;
    // A notch is a page; precision touchpads add up to one.
    while (wheel_ >= WHEEL_DELTA) {
        wheel_ -= WHEEL_DELTA;
        SetPage(page_ - 1);
        if (lifted_ >= 0)
            UpdateDragTarget();
    }
    while (wheel_ <= -WHEEL_DELTA) {
        wheel_ += WHEEL_DELTA;
        SetPage(page_ + 1);
        if (lifted_ >= 0)
            UpdateDragTarget();
    }
}

// What is under a content point (px): a tile's index, kSearch, -2 - n for page dot n, or kGlass.
int Launchpad::HitAt(POINT pt) const
{
    const float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    for (const auto& hit : hits_)
        if (x >= hit.rect.left && x < hit.rect.right && y >= hit.rect.top && y < hit.rect.bottom)
            return hit.index;
    return kGlass;
}

void Launchpad::SetPage(int page)
{
    const int next = std::clamp(page, 0, Pages() - 1);
    if (next != page_) {
        page_ = next;
        UpdatePageTargets(true);
    }
    surface_.Invalidate();
}

void Launchpad::Launch(int index)
{
    if (index < 0 || size_t(index) >= Shown().size())
        return;
    const CatalogApp app = Shown()[size_t(index)];
    if (FolderApps(app.id) != nullptr) {
        ShowFolder(app.id);
        return;
    }
    Close(true);
    AppCatalog::Launch(app);
}

// The IME's composition window under the search field's text.
void Launchpad::PlaceIme()
{
    const HWND hwnd = surface_.Hwnd();
    if (HIMC imc = ImmGetContext(hwnd)) {
        RECT win{};
        GetWindowRect(hwnd, &win);
        const RECT content = surface_.Rect();
        const D2D1_RECT_F field = namingFolder_ ? folderTitleRect_ : SearchRect();
        const float s = surface_.Scale();
        const float x = namingFolder_ ? (field.left + field.right) * 0.5f : caretX_ > 0 ? caretX_ : field.left + 36;
        const float y = field.bottom + 4;
        const POINT pos{content.left - win.left + LONG(x * s), content.top - win.top + LONG(y * s)};
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = pos;
        ImmSetCompositionWindow(imc, &cf);
        CANDIDATEFORM cand{0, CFS_CANDIDATEPOS, pos, {}};
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(hwnd, imc);
    }
}

} // namespace atrium
