#include "ui/start_menu.h"

#include "bar_edge.h"
#include "clock.h"
#include "localization.h"
#include "log.h"
#include "shell/keyboard_hook.h"
#include "shell/shell_icon.h"
#include "ui/start_menu_metrics.h"

#include <imm.h>
#include <sddl.h>
#include <windowsx.h>
#define SECURITY_WIN32
#include <security.h>
#include <secext.h>

#include <algorithm>
#include <ranges>

namespace atrium {

namespace {

using namespace start_menu_detail;

} // namespace

StartMenu::StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog)
    : surface_(instance, L"Atrium Start", true),
      tasks_(tasks),
      catalog_(catalog),
      layout_(LoadStartLayout()),
      cols_(kStartGrids[layout_.size].cols),
      rows_(kStartGrids[layout_.size].rows),
      grid_(layout_.size)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(false); }; // a press next to the menu
    Log(L"start: %d x %d tiles, %.0f DIP icons, %s", cols_, rows_, kStartIcons[layout_.icon].icon,
        layout_.custom ? L"arranged by the user" : L"the Windows start menu's pins");
}

StartMenu::~StartMenu() { RemoveMouseWatcher(); }

void StartMenu::RemoveMouseWatcher()
{
    if (mouseWatcherToken_ > 0) {
        KeyboardHook::Instance().RemoveMouseDownWatcher(mouseWatcherToken_);
        mouseWatcherToken_ = 0;
    }
}

// The settings window's choices (settings.json's startSize and startIcon: small,
// medium, large, xlarge). Neither reads an icon: every icon is kept at the
// largest size and drawn smaller, so changing either never waits on a file.
void StartMenu::SetPreferredSize(const std::wstring& name)
{
    const int size = name == L"small" ? 0 : name == L"medium" ? 1 : name == L"large" ? 2 : name == L"xlarge" ? 3 : -1;
    if (size < 0 || size == layout_.size)
        return;
    layout_.size = size;
    SaveStartLayout(layout_);
    if (surface_.Visible())
        SetSize(layout_.size, layout_.icon);
    Log(L"start: size %s from the settings", name.c_str());
}

void StartMenu::SetPreferredIcon(const std::wstring& name)
{
    const int icon = name == L"small" ? 0 : name == L"medium" ? 1 : name == L"large" ? 2 : name == L"xlarge" ? 3 : -1;
    if (icon < 0 || icon == layout_.icon)
        return;
    layout_.icon = icon;
    SaveStartLayout(layout_);
    if (surface_.Visible())
        SetSize(layout_.size, layout_.icon);
    Log(L"start: icons %s from the settings", name.c_str());
}

std::wstring StartMenu::SizeName() const
{
    static const wchar_t* const kNames[] = {L"small", L"medium", L"large", L"xlarge"};
    return kNames[layout_.size];
}

std::wstring StartMenu::IconSizeName() const
{
    static const wchar_t* const kNames[] = {L"small", L"medium", L"large", L"xlarge"};
    return kNames[layout_.icon];
}

void StartMenu::LoadAccount()
{
    if (!userName_.empty())
        return;
    // Screenshots and screen recordings: ATRIUM_PRIVACY=1 shows neither the
    // account's name and picture nor the network's name.
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
    if (private_) {
        userName_ = Tr(L"User"); // 用户
        return;
    }
    wchar_t buf[256] = {};
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, buf, &n) && buf[0]) {
        userName_ = buf;
    } else {
        DWORD m = 256;
        if (GetUserNameW(buf, &m))
            userName_ = buf;
    }
    // The account picture Windows keeps for the user.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return;
    BYTE info[256];
    DWORD len = 0;
    LPWSTR sid = nullptr;
    if (GetTokenInformation(token, TokenUser, info, sizeof(info), &len) && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info)->User.Sid, &sid)) {
        std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\") + sid;
        for (const wchar_t* value : {L"Image192", L"Image240", L"Image96", L"Image448"}) {
            wchar_t path[MAX_PATH] = {};
            DWORD size = sizeof(path);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS &&
                GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && LoadShellImage(path, 96, avatar_, true))
                break;
        }
        LocalFree(sid);
    }
    CloseHandle(token);
}

// ---- Opening, closing, contents ----

void StartMenu::Open(RECT bar, RECT logo, HWND previous)
{
    previous_ = previous;
    bar_ = bar;
    query_.clear();
    composition_.clear();
    searchFocused_ = false;
    caretX_ = kSearchX + 46;
    results_.clear();
    allApps_ = scrubbing_ = false;
    page_ = 0;
    pageShown_ = 0;
    wheelAcc_ = 0;
    scroll_ = scrollTarget_ = 0;
    sel_ = -1;
    hover_.clear();
    pressed_.clear();
    powerOpen_ = false;
    powerGrow_ = powerV_ = 0;
    power_ = {{L"\uE708", Tr(L"Sleep"), PowerAction::Sleep}};
    if (const HibernateMenuOption hiber = DescribeHibernateMenu(QueryHibernateCapability()); hiber.visible)
        power_.push_back({L"\uE823", hiber.label, PowerAction::Hibernate});
    power_.push_back({L"\uE72C", Tr(L"Restart"), PowerAction::Restart});
    power_.push_back({L"\uE7E8", Tr(L"Shut down"), PowerAction::Shutdown});
    power_.push_back({L"\uE72E", Tr(L"Lock"), std::nullopt});
    folder_ = -1;
    folderClosing_ = naming_ = false;
    drag_ = {};
    sizing_ = false;
    tilePos_.clear();
    apps_ = catalog_.Apps();
    pins_ = catalog_.StartPins();
    pinGroups_ = catalog_.StartPinGroups();
    stale_ = false;
    Rebuild();
    catalog_.Refresh(); // new installs and pins since last time; the menu updates when ready
    if (onStatusRequest)
        onStatusRequest();
    LoadAccount();
    // The window has the largest size the work area has room for; the menu its own size, if it fits.
    UpdateRoom();
    SetSize(layout_.size, layout_.icon);
    const D2D1_SIZE_F z = PanelSize();
    panelW_ = z.width;
    mainH_ = z.height - kGap - barH_;
    panelV_ = mainV_ = 0;
    RECT r = PopupRect(bar_, Size(), Align());
    Restage();
    // Pops out of the logo.
    RemoveMouseWatcher();
    surface_.Show(r, true, PopupOrigin(bar_, r, logo));
    mouseWatcherToken_ = KeyboardHook::Instance().AddMouseDownWatcher(surface_.Hwnd(), WM_START_MOUSE_DOWN);
    if (onOpenChanged)
        onOpenChanged(true);
    SetTimer(surface_.Hwnd(), kStatusTimer, 2000, nullptr);
    if (const UINT blink = GetCaretBlinkTime(); blink != INFINITE)
        SetTimer(surface_.Hwnd(), kCaretTimer, std::max(1u, blink), nullptr);
    Log(L"start: opened (%zu apps, %zu tiles, %s)", apps_.size(), tiles_.size(), catalog_.Loaded() ? L"catalog ready" : L"catalog loading");
}

void StartMenu::Close(bool launched)
{
    if (!surface_.Visible())
        return;
    RemoveMouseWatcher();
    if (drag_.on || drag_.armed)
        EndDrag(false);
    if (sizing_)
        EndResize();
    FinishNaming(true);
    KillTimer(surface_.Hwnd(), kStatusTimer);
    KillTimer(surface_.Hwnd(), kCaretTimer);
    composition_.clear();
    searchFocused_ = false;
    catalog_.SetVisibleIcons(VisibleIconsOwner::Start, {});
    askedIcons_.clear();
    powerOpen_ = false;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
    if (menuOpen_ && menu && menu->Visible())
        menu->Close();
    if (onOpenChanged)
        onOpenChanged(false);
    // Give the keyboard back to where it was, unless an app was just started
    // (it takes the foreground itself).
    wchar_t cls[64] = {};
    if (!launched && previous_ && IsWindow(previous_)) {
        GetClassNameW(previous_, cls, 64);
        if (!SetForegroundWindow(previous_))
            SwitchToThisWindow(previous_, TRUE);
    }
    Log(L"start: closed%s%s%s", launched ? L" (launched)" : L"", cls[0] ? L", focus back to " : L"", cls);
}

void StartMenu::SetStatus(const SystemStatus& status)
{
    if (!(status_ == status)) {
        status_ = status;
        surface_.Invalidate();
    }
}

void StartMenu::Refresh(bool listChanged)
{
    if (!surface_.Visible())
        return;
    // Icons arrive one by one and the apps' windows change all the time: the
    // tiles are rebuilt only when the apps themselves changed.
    if (listChanged) {
        auto apps = catalog_.Apps();
        auto pins = catalog_.StartPins();
        auto pinGroups = catalog_.StartPinGroups();
        auto same = [](const std::vector<CatalogApp>& a, const std::vector<CatalogApp>& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](const CatalogApp& x, const CatalogApp& y) { return x.id == y.id && x.name == y.name; });
        };
        if (!same(apps, apps_) || !same(pins, pins_) || pinGroups.size() != pinGroups_.size() ||
            !std::equal(pinGroups.begin(), pinGroups.end(), pinGroups_.begin(),
                        [&](const CatalogFolder& x, const CatalogFolder& y) { return x.name == y.name && x.folder == y.folder && same(x.apps, y.apps); })) {
            if (drag_.on) {
                stale_ = true; // the carried tile's indices stay valid until it is put down
            } else {
                apps_ = std::move(apps);
                pins_ = std::move(pins);
                pinGroups_ = std::move(pinGroups);
                Rebuild();
            }
        }
    }
    if (pendingSync_.on && !drag_.on) {
        auto sync = pendingSync_;
        pendingSync_.on = false;
        SyncFromWindows(sync.pins, sync.folders, false);
    }
    surface_.Invalidate();
}

void StartMenu::Restage()
{
    stageMs_ = ClockMs();
    hoverAnim_.clear();
}

float StartMenu::Hover(Canvas& c, const std::wstring& id, bool on)
{
    auto& f = hoverAnim_[id];
    float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

std::wstring StartMenu::HitAt(POINT pt) const
{
    float s = surface_.Scale();
    float x = float(pt.x) / s, y = float(pt.y) / s;
    // Last painted is on top (the power card over the panel).
    for (const auto& h : std::ranges::reverse_view(hits_)) {
        if (!In(h.rect, x, y))
            continue;
        // Rows scrolled out of the list are not clickable.
        if ((h.id.starts_with(L"row:") || h.id.starts_with(L"head:")) && (y < kListTop || y > ListBottom()))
            continue;
        return h.id;
    }
    return {};
}

// ---- Actions ----

void StartMenu::Launch(const CatalogApp& app)
{
    AppCatalog::Launch(app);
    Close(true);
}

void StartMenu::RunPower(int index)
{
    if (index < 0 || index >= int(power_.size()))
        return;
    Close(true);
    if (!power_[size_t(index)].action) { // Windows' own lock
        if (onSystem)
            onSystem(L"lock");
        return;
    }
    RunPowerAction(*power_[size_t(index)].action, powerNotifyHost_, powerNoticeMsg_);
}

void StartMenu::Activate(const std::wstring& id)
{
    if (naming_ && id == namingHit_)
        return;
    if (id.starts_with(L"tile:")) {
        const int i = Index(id, 5);
        if (i >= 0 && i < int(tiles_.size())) {
            if (tiles_[size_t(i)].folder)
                OpenFolder(i);
            else
                Launch(tiles_[size_t(i)].app);
        }
    } else if (id.starts_with(L"fapp:")) {
        const int i = Index(id, 5);
        if (folder_ >= 0 && i >= 0 && i < int(tiles_[size_t(folder_)].apps.size()))
            Launch(tiles_[size_t(folder_)].apps[size_t(i)]);
    } else if (id.starts_with(L"row:")) {
        const auto& rows = RowsShown();
        const size_t i = size_t(Index(id, 4));
        if (i < rows.size() && !rows[i].letter) {
            if (rows[i].folder >= 0)
                OpenFolder(rows[i].folder);
            else
                Launch(rows[i].app);
        }
    } else if (id == L"sync") {
        OpenSyncMenu();
    } else if (id == L"allapps") {
        allApps_ = query_.empty() ? !allApps_ : true;
        query_.clear();
        composition_.clear();
        searchFocused_ = false;
        results_.clear();
        if (allApps_)
            list_ = AllApps();
        sel_ = -1;
        scroll_ = scrollTarget_ = 0;
        Restage();
        surface_.Invalidate();
    } else if (id.starts_with(L"page:")) {
        SetPage(Index(id, 5));
    } else if (id.starts_with(L"fpage:")) {
        folderPage_ = std::clamp(Index(id, 6), 0, FolderPages() - 1);
        surface_.Invalidate();
    } else if (id == L"ftitle") {
        StartNaming();
    } else if (id == L"fcard" || id == L"resize" || id == L"azbar" || id == L"search") {
        // The folder's card itself, the grip, the search field: nothing to do on a click.
    } else if (id == L"clear") {
        query_.clear();
        composition_.clear();
        searchFocused_ = false;
        results_.clear();
        sel_ = -1;
        Restage();
        surface_.Invalidate();
    } else if (id == L"power") {
        powerOpen_ = !powerOpen_;
        powerSel_ = -1;
        surface_.Invalidate();
    } else if (id.starts_with(L"pw:")) {
        RunPower(Index(id, 3));
    } else if (!id.empty() && onSystem) {
        Close(true);
        onSystem(id);
    }
}

void StartMenu::MoveSelection(int dx, int dy)
{
    if (folder_ >= 0 && !folderClosing_) {
        // The open folder's apps.
        const int n = int(tiles_[size_t(folder_)].apps.size()), cols = FolderCols(), per = cols * FolderRows();
        if (!n)
            return;
        int next = sel_ < 0 ? folderPage_ * per : sel_ + dx + dy * cols;
        sel_ = std::clamp(next, 0, n - 1);
        folderPage_ = sel_ / per;
        surface_.Invalidate();
        return;
    }
    if (Listing()) {
        // Rows; the headings are passed over. With nothing selected in the
        // results, the first row counts as selected.
        const auto& rows = RowsShown();
        std::vector<int> selectable;
        for (int i = 0; i < int(rows.size()); ++i)
            if (!rows[size_t(i)].letter)
                selectable.push_back(i);
        if (selectable.empty())
            return;
        int at = int(std::ranges::find(selectable, sel_) - selectable.begin());
        if (sel_ < 0)
            at = !query_.empty() ? 0 : -1;
        int step = dy + dx;
        at = std::clamp(at < 0 ? 0 : at + step, 0, int(selectable.size()) - 1);
        sel_ = selectable[size_t(at)];
        EnsureVisible();
        surface_.Invalidate();
        return;
    }
    const int n = int(tiles_.size()), cols = cols_;
    if (!n)
        return;
    int next;
    if (sel_ < 0)
        next = page_ * PerPage(); // the first tile of the page shown
    else if (int to = sel_ + dx + dy * cols; to >= 0 && to < n)
        next = to;
    else
        next = dx ? std::clamp(to, 0, n - 1) : sel_; // up/down past the grid stays
    sel_ = std::clamp(next, 0, n - 1);
    SetPage(sel_ / PerPage());
    surface_.Invalidate();
}

void StartMenu::EnsureVisible()
{
    if (sel_ < 0 || !Listing())
        return;
    const auto& rows = RowsShown();
    float top = 0;
    for (int i = 0; i < sel_ && i < int(rows.size()); ++i)
        top += rows[size_t(i)].letter ? kHeadH : kRowH;
    const float viewH = ListBottom() - kListTop;
    if (top < scrollTarget_)
        scrollTarget_ = top;
    else if (top + kRowH > scrollTarget_ + viewH)
        scrollTarget_ = top + kRowH - viewH;
}

void StartMenu::PlaceIme()
{
    float s = surface_.Scale();
    if (HIMC imc = ImmGetContext(surface_.Hwnd())) {
        // Window coordinates: the content starts after the shadow margin.
        RECT win{};
        GetWindowRect(surface_.Hwnd(), &win);
        POINT origin{surface_.Rect().left - win.left + OffsetXPx(), surface_.Rect().top - win.top + OffsetPx()};
        // Below the field being typed in: the search, or the folder's name.
        const D2D1_RECT_F f = FolderRect();
        const bool appNaming = naming_ && !namingKey_.starts_with(L"folder:");
        const float x = appNaming ? namingRect_.left : naming_ ? (f.left + f.right) / 2 - 60 : (caretX_ > 0 ? caretX_ : kSearchX + 46);
        const float y = appNaming ? namingRect_.bottom + 4 : naming_ ? f.top + 50 : kSearchTop + kSearchH + 4;
        const POINT pos{origin.x + LONG(x * s), origin.y + LONG(y * s)};
        COMPOSITIONFORM cf{CFS_POINT, pos, {}};
        ImmSetCompositionWindow(imc, &cf);
        CANDIDATEFORM cand{0, CFS_CANDIDATEPOS, pos, {}};
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(surface_.Hwnd(), imc);
    }
}

bool StartMenu::ImeMessage(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    if (naming_) {
        if (m == WM_IME_STARTCOMPOSITION)
            PlaceIme();
        return false;
    }
    switch (m) {
    case WM_IME_SETCONTEXT: result = DefWindowProcW(surface_.Hwnd(), m, w, l & ~LPARAM(ISC_SHOWUICOMPOSITIONWINDOW)); return true;
    case WM_IME_STARTCOMPOSITION:
        searchFocused_ = true;
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
            searchFocused_ = searchFocused_ || !composition_.empty();
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

bool StartMenu::TimerMessage(WPARAM w)
{
    if (w == kStatusTimer && onStatusRequest)
        onStatusRequest();
    else if (w == kDragTimer)
        DragTick();
    else if (w == kCaretTimer && searchFocused_ && !naming_)
        surface_.Invalidate();
    return w == kStatusTimer || w == kDragTimer || w == kCaretTimer;
}

bool StartMenu::NamingChar(wchar_t ch)
{
    if (ch == 27)
        FinishNaming(false);
    else if (ch == 13)
        FinishNaming(true);
    else if (ch == 8 && !nameDraft_.empty())
        nameDraft_.pop_back();
    else if (ch >= 32 && nameDraft_.size() < (namingKey_.starts_with(L"folder:") ? 24u : 64u))
        nameDraft_ += ch;
    surface_.Invalidate();
    return true;
}

void StartMenu::HandleExternalMouseDown(LPARAM l)
{
    if (!surface_.Visible() || drag_.on || sizing_ || scrubbing_)
        return;
    const POINT screenPt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if (menuOpen_ && menu && menu->Visible()) {
        RECT mr{};
        if (GetWindowRect(menu->Hwnd(), &mr) && PtInRect(&mr, screenPt))
            return;
    }
    const float s = surface_.Scale();
    const POINT content = surface_.ScreenToContent(screenPt);
    const float x = float(content.x - OffsetXPx()) / s;
    const float y = float(content.y - OffsetPx()) / s;
    if (In(MainRect(), x, y) || In(ShelfRect(), x, y))
        return;
    previous_ = nullptr; // outside click moves focus elsewhere; do not yank it back on close
    Close(false);
}

bool StartMenu::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    // Mouse positions in the menu's coordinates (the panel's corner, lower in the window).
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    const POINT content = pt;
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) {
        pt.x -= OffsetXPx();
        pt.y -= OffsetPx();
    }
    switch (m) {
    case WM_START_MOUSE_DOWN:
        HandleExternalMouseDown(l);
        result = 0;
        return true;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !menuOpen_ && surface_.Visible()) {
            wchar_t cls[64] = {};
            GetClassNameW(reinterpret_cast<HWND>(l), cls, 64);
            Log(L"start: deactivated by %s", cls);
            previous_ = nullptr; // the user clicked elsewhere: that window has the focus now
            Close(false);
        }
        return false;
    case WM_TIMER: return TimerMessage(w);
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && (sizing_ || hover_ == L"resize")) {
            const BarEdge edge = EdgeOf(bar_);
            SetCursor(LoadCursorW(nullptr, (edge == BarEdge::Right) == (edge != BarEdge::Bottom) ? IDC_SIZENESW : IDC_SIZENWSE));
            result = TRUE;
            return true;
        }
        return false;
    case WM_MOUSEMOVE: return HandleMouseMove(w, pt);
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (!drag_.on && !sizing_ && !scrubbing_) {
            hover_.clear();
            pressed_.clear();
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN: return HandleLeftDown(pt, content);
    case WM_LBUTTONUP: return HandleLeftUp(pt);
    case WM_CAPTURECHANGED:
        if (drag_.on || drag_.armed)
            EndDrag(false);
        if (sizing_)
            EndResize();
        if (scrubbing_) {
            scrubbing_ = false;
            surface_.Invalidate();
        }
        return false;
    case WM_RBUTTONUP:
        if (!drag_.on && !sizing_ && !scrubbing_)
            ContextMenu(HitAt(pt), surface_.ContentToScreen(content));
        return true;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: return HandleMouseWheel(m, w, pt);
    case WM_IME_SETCONTEXT:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION: return ImeMessage(m, w, l, result);
    case WM_KEYDOWN: return HandleKeyDown(w);
    case WM_CHAR: return HandleChar(w, result);
    default: return false;
    }
}

} // namespace atrium
