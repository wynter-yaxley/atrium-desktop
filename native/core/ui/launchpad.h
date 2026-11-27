// Launchpad, as on macOS: the apps the user installed (and Windows' Settings), oldest first, then one
// folder holding Windows' own apps (the catalog's `system`), opened in place; a search finds every app.
// One window of the bar's glass (or that glass under a black veil: launchpadMaterial tint) in the middle
// of the display (or over the whole monitor, launchpadLayout fullscreen, where a click on the empty glass
// closes it), in pages of a grid (launchpad_model.h) with the page dots below and a search field
// above. It opens from its entry next to the start logo: the glass fades in and the apps settle from a
// little larger, all at once when every icon is read (a spinner meanwhile; they are read ahead at start),
// and it leaves the same way. Typing goes into the search (the input method's composition inline, the
// caret shown once the field is clicked or typed in); the wheel, a drag or the arrows turn the pages with
// a spring; a click launches, a right click shows the bar's menu for the app; Esc clears a search, then closes the folder, then Launchpad, as a click
// outside it does.
#pragma once

#include "motion.h"
#include "shell/app_catalog.h"
#include "shell/launchpad_order.h"
#include "ui/glass_menu.h"
#include "ui/launchpad_model.h"
#include "ui/surface.h"

#include <windows.h>

#include <functional>
#include <map>
#include <string_view>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

class Launchpad {
public:
    Launchpad(HINSTANCE instance, AppCatalog& catalog);

    // work: the display's work area (screen px), the window centred in it; previous: the window to
    // give the focus back to when closed without launching.
    void Open(RECT work, HWND previous);
    void SetRadius(float dip) { radius_ = dip; }
    void SetTint(bool tint) { tint_ = tint; }
    void SetFullscreen(bool on) { full_ = on; } // launchpadLayout fullscreen: the whole monitor, as macOS's
    void Close(bool launched);
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(bool listChanged = true); // the catalog changed: the apps, an icon
    // Reads every app's icon ahead, at the size a window on `work` would show them.
    void Prewarm(RECT work);
    HWND Hwnd() const { return surface_.Hwnd(); }
    GlassMenu* menu = nullptr;
    // A right click on an app: its menu at `screen` (the bar's, main.cpp); done(picked) once it has closed.
    std::function<void(const CatalogApp& app, POINT screen, std::function<void(bool picked)> done, std::vector<AppMenuExtra> extras)> onAppMenu;

private:
    static constexpr UINT_PTR kCloseTimer = 0x4C50, kCaretTimer = 0x4C51, kDragTimer = 0x4C53;
    static constexpr int kSearch = -1, kFolderTitle = -900, kGlass = -1000;
    struct Hit {
        D2D1_RECT_F rect;
        int index; // into Shown(), kSearch, kFolderTitle, or -2 - n for page dot n
    };

    // Painting (launchpad.cpp).
    void Paint(Canvas& c);
    void PaintSearch(Canvas& c, const Theme& t, float shown);
    void PaintFolderHeader(Canvas& c, const Theme& t, float shown);
    void PaintPage(Canvas& c, const Theme& t, int page, float dx);
    void PaintTile(Canvas& c, const Theme& t, const CatalogApp& app, int index, D2D1_RECT_F cell);
    void PaintDots(Canvas& c, const Theme& t);
    void PaintSpinner(Canvas& c, const Theme& t, float shown);
    void PaintIcon(Canvas& c, const Theme& t, const std::wstring& id, D2D1_RECT_F r); // an app's icon, a quiet square until read
    void PaintFolder(Canvas& c, const Theme& t, const std::vector<CatalogApp>& apps, D2D1_RECT_F r);
    const std::vector<CatalogApp>* FolderApps(std::wstring_view id) const;
    std::wstring FolderTitle(std::wstring_view id) const;
    static bool IsCustomFolder(std::wstring_view id) { return id.starts_with(L"folder:"); }
    bool Hidden(const std::wstring& id) const;
    void Split(); // tiles_, customFolders_, microsoft_ and system_ from apps_
    void ShowFolder(const std::wstring& id);
    std::vector<std::wstring> PageIconIds(int page) const;
    void UpdatePageTargets(bool prioritize);
    int IconPx(RECT work) const;
    void PublishGlass(double now);
    float Hover(Canvas& c, int id, bool on);
    D2D1_RECT_F Cell(int slot) const; // a slot of a page, its page at x 0
    D2D1_RECT_F SearchRect() const;
    float OpenAmount(double now) const; // 0 hidden .. 1 open, through the opening and the closing
    void Size(RECT work);               // w_, h_ and grid_ for a window on `work`
    static RECT Monitor(RECT work);     // the monitor `work` is on (fullscreen)
    std::vector<CatalogApp> LaunchableApps() const;
    float Radius() const;

    // Input (launchpad_input.cpp).
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool Key(WPARAM key);
    void Type(wchar_t ch);
    void Press(POINT pt);
    void Drag(POINT pt);
    void Release(POINT pt);
    void AppMenu(POINT pt); // a right click
    void OpenFolderMenu(const std::wstring& folderId, POINT screen);
    void OpenBackgroundMenu(POINT screen);
    void Wheel(int delta);
    int HitAt(POINT pt) const;
    void SetPage(int page);
    void Launch(int index);
    bool ImeMessage(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    // Arranging & custom folders (launchpad_drag.cpp).
    bool Arranging() const { return query_.empty() && (openFolder_.empty() || IsCustomFolder(openFolder_)); }
    std::vector<int> DragOrder() const;
    void Lift();
    void MoveLifted(POINT pt);
    void UpdateDragTarget();
    void DragTick();
    void DropLifted();
    void HideApp(const CatalogApp& app);
    void RestoreApp(const std::wstring& id);
    void RestoreAllHidden();
    void RemoveAppFromFolder(const std::wstring& folderId, const std::wstring& appId);
    void UngroupFolder(const std::wstring& folderId);
    void StartFolderNaming();
    void FinishFolderNaming(bool commit);
    void ApplyOrder(); // tiles_ in the user's order
    D2D1_RECT_F Glide(const std::wstring& id, D2D1_RECT_F cell, int page, float dx); // where a tile is drawn as it makes room
    void PaintLifted(Canvas& c, const Theme& t);
    void PlaceIme();

    void Finish();          // the closing has played: the window goes, the focus goes back
    void Filter();          // shown_ from apps_ and query_
    int Pages() const;
    const std::vector<CatalogApp>& Shown() const
    {
        if (!query_.empty())
            return shown_;
        if (const auto* folder = FolderApps(openFolder_))
            return *folder;
        return tiles_;
    }

    Surface surface_;
    AppCatalog& catalog_;
    static constexpr const wchar_t* kFolderId = L"#system";          // the system folder's tile among tiles_
    static constexpr const wchar_t* kMicrosoftFolderId = L"#microsoft"; // built-in Microsoft apps folder
    std::vector<CatalogApp> apps_, shown_;
    std::vector<CatalogApp> tiles_, microsoft_, system_; // root tiles, Microsoft folder, system folder
    std::map<std::wstring, std::vector<CatalogApp>> customFolders_;
    std::map<std::wstring, std::wstring> customFolderNames_;
    std::wstring openFolder_;
    bool namingFolder_ = false;
    std::wstring nameDraft_;
    D2D1_RECT_F folderTitleRect_{};
    std::wstring query_;
    std::wstring composition_;
    bool searchFocused_ = false;
    float caretX_ = 0;
    LaunchpadGrid grid_;
    float w_ = 0, h_ = 0; // the window, DIP
    float radius_ = 24;   // the bar's popups' corner
    bool tint_ = false;   // launchpadMaterial tint: a black veil over the glass, light ink
    bool full_ = false;   // launchpadLayout fullscreen
    int iconPx_ = 0;      // the icons' size on the display it opened on
    bool loading_ = false;  // icons still being read: a spinner instead of the apps
    double revealedMs_ = 0; // when the apps began to settle in
    int page_ = 0;
    float pageShown_ = 0, pageSpeed_ = 0; // the page on screen (fractional while it glides), pages/s
    int sel_ = -1, hover_ = -1, pressed_ = -1;
    // A press: on a tile (moved, it lifts) or the glass (dragged sideways past a few DIP, it turns the pages).
    bool down_ = false, dragging_ = false;
    POINT downPt_{};
    float dragFrom_ = 0;     // pageShown_ when the drag began
    float dragSpeed_ = 0;    // pages/s, of the last moves
    double dragMs_ = 0;
    float lastDragX_ = 0;
    int wheel_ = 0;          // wheel notches not yet turned into a page
    bool tracking_ = false;
    bool menuOpen_ = false; // an app's menu is up: the focus going to it does not close Launchpad
    bool stale_ = false;    // the catalog changed while an app was lifted
    HWND previous_ = nullptr;
    double openedMs_ = 0, closedMs_ = 0, lastPaint_ = 0;
    bool closing_ = false, launched_ = false;
    ULONGLONG lastClosed_ = 0;
    std::pair<int, std::vector<std::wstring>> prewarmed_; // icon px and page ids last read ahead
    std::vector<std::wstring> pageIconIds_;
    std::vector<Hit> hits_;
    std::vector<std::pair<std::wstring, int>> visibleIcons_, askedIcons_;
    std::map<int, motion::Follow> hoverAnim_;
    int lifted_ = -1; // index inside liftedFromFolder_ ? *FolderApps(dragFolderId_) : tiles_
    bool liftedFromFolder_ = false, dragInFolder_ = false;
    std::wstring dragFolderId_, liftedId_;
    CatalogApp liftedApp_;
    int dragInsert_ = 0, dragPendingSlot_ = -1, mergeTarget_ = -1, dragEdge_ = 0;
    double dragPendingMs_ = 0, dragEdgeMs_ = 0;
    D2D1_POINT_2F liftAt_{};         // the pointer, DIP
    LaunchpadLayout layout_;         // the user's layout, as read when it opened
    std::unordered_map<std::wstring, D2D1_POINT_2F> tilePos_; // the tiles' places on the sheet as they glide
    float glideMs_ = 0;              // this frame's time step for the places
    bool gliding_ = false;
};

} // namespace atrium
