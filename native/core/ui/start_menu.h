// Atrium's own start menu (opened from the logo on the bar, or by the Windows
// key: winKeyStart), a launcher of separate pieces of the bar's glass: a panel
// with the search field and the tiles, and below it a capsule shaped like the
// bar with the account, network, volume, battery, the system tray and power.
// The power choices open in a third piece of glass, an open folder in another
// over the tiles.
//
// The tiles are the apps pinned to the Windows start menu until the user
// arranges them (shell/start_layout.h): dragged to another place or page, onto
// another app (a folder of the two) or into a folder. Folders open in a small
// card, take a name and give their apps back. The grip in the panel's top
// right corner (or the menu of the panel) picks one of four sizes; the
// window keeps the largest that fits and the pieces of glass spring to the
// size inside it. Typing searches all apps and folders, by name or by
// initials ("wx" for 微信); the button next to the field lists them all from
// A to Z, with the letters down the left to jump through the list.
// No recommendations, recent files or file search. Tiles lift under the
// pointer, presses ripple.
#pragma once

#include "motion.h"
#include "shell/app_catalog.h"
#include "shell/start_layout.h"
#include "shell/system_status.h"
#include "shell/task_model.h"
#include "ui/glass_menu.h"
#include "ui/surface.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

class StartMenu {
public:
    StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog);
    ~StartMenu();
    StartMenu(const StartMenu&) = delete;
    StartMenu& operator=(const StartMenu&) = delete;

    // barRect: the bar's glass shape on screen; the menu opens above its left
    // end, out of `logo` (the logo's rectangle).
    // previous: the window to give the focus back to when closed without launching.
    void Open(RECT barRect, RECT logo, HWND previous);
    void Close(bool launched);
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(bool listChanged = true); // an icon or the catalog changed
    void SetStatus(const SystemStatus& status);
    static int TileIconPx(float scale); // the largest icon size (AppCatalog::PrefetchStartIcons); every size draws from it
    int IconPx() const;                 // the same, in pixels on this monitor: what an icon is read at
    HWND Hwnd() const { return surface_.Hwnd(); }
    // Corner radius (DIP), the bar's; the panel uses it, the capsule is round like the bar.
    void SetRadius(float dip)
    {
        radius_ = dip;
        surface_.SetRadius(dip);
    }
    // Height of the bar (DIP): the status capsule has the same.
    void SetBarHeight(float dip) { barH_ = dip; }
    // The tray's button in the capsule (setting startTray).
    void SetShowTray(bool on) { showTray_ = on; }
    // The network, volume and battery buttons in the capsule (setting startStatus).
    void SetShowStatus(bool on) { showStatus_ = on; }
    // In the middle of the screen rather than above the bar's left end (setting startCentered).
    void SetCentered(bool on) { centered_ = on; }
    // The menu's size and its icons' size (settings startSize and startIcon: small,
    // medium, large, xlarge; empty leaves the menu as it is).
    void SetPreferredSize(const std::wstring& name);
    void SetPreferredIcon(const std::wstring& name);
    std::wstring SizeName() const;
    std::wstring IconSizeName() const;
    std::function<void(const std::wstring& name)> onSize;  // the user picked another size in the menu
    std::function<void(const std::wstring& name)> onIcon;  // or another icon size

    std::function<void(const PinnedApp&)> onPin;            // pin to the bar (the dock)
    std::function<void(const std::wstring& what)> onSystem; // "network", "volume", "battery", "tray", "account"
    std::function<void()> onStatusRequest;
    std::function<void(bool open)> onOpenChanged;           // notifies WinKey of Start menu open state
    void SetPowerNotify(HWND host, UINT message)
    {
        powerNotifyHost_ = host;
        powerNoticeMsg_ = message;
    }
    GlassMenu* menu = nullptr;                               // a tile's menu (main's)

private:
    static constexpr UINT_PTR kStatusTimer = 1, kDragTimer = 2, kCaretTimer = 3;
    static constexpr UINT WM_START_MOUSE_DOWN = WM_APP + 0x60;
    static constexpr float kPowerRowH = 44, kPowerPad = 12;
    static constexpr motion::Spring kFolderClose{0.30f, 1.f};
    struct Hit {
        D2D1_RECT_F rect; // DIP, content coordinates
        std::wstring id;
    };
    // A tile of the grid: an app, or a folder of apps.
    struct Tile {
        std::wstring key; // the app's id, or the folder's
        std::wstring name;
        CatalogApp app;
        std::vector<CatalogApp> apps; // a folder's
        bool folder = false;
    };
    // A line of the search results or of the A-Z list.
    struct Row {
        wchar_t letter = 0; // a heading of the list (no app)
        int folder = -1;    // a folder: its tile
        CatalogApp app;
    };
    struct PowerRow {
        const wchar_t* glyph;
        std::wstring name;
        std::optional<PowerAction> action; // empty: lock
    };
    // A tile carried by the pointer: from the grid, or out of the open folder.
    struct Drag {
        bool armed = false, on = false;
        bool fromFolder = false; // one of the open folder's apps
        bool inFolder = false;   // still over the folder (its apps make room), else over the grid
        int from = -1;           // its tile, or its place in the folder
        int folderTile = -1;     // the folder it came out of
        std::wstring key;
        float x = 0, y = 0;      // the pointer (DIP)
        float grabX = 0, grabY = 0; // the pointer within the tile's cell
        int insert = -1;         // where the others make room (display slot)
        int merge = -1;          // the tile it would go into (a folder of the two)
        int pendingSlot = -1;    // the slot the pointer rests on, before the others move
        double pendingMs = 0, edgeMs = 0;
        int edge = 0;            // resting at the grid's left (-1) or right (1) edge: turns the page
    };

    void Paint(Canvas& c);
    bool StepPanelSize(Canvas& c, D2D1_SIZE_F target);
    void UpdateSurfaceRegion(D2D1_SIZE_F target, bool resizing);
    void PaintFolderCard(Canvas& c, std::vector<ShapeDip>& shapes, float dt, float& open, D2D1_RECT_F& card, bool& animating);
    void StepPowerCard(Canvas& c, float dt, bool& animating);
    void PaintPowerCard(Canvas& c, const Theme& t, std::vector<ShapeDip>& shapes);
    bool DismissPowerCard();
    void PaintSearch(Canvas& c, const Theme& t);
    void PaintTiles(Canvas& c, const Theme& t, float shown);
    void PaintTileNaming(Canvas& c, const Theme& t, const Tile& tile, D2D1_RECT_F r, D2D1_RECT_F lr);
    // A tile in `r`: its icon (a folder: its first apps, small) and name. target: a carried app would go into it.
    void PaintTileFace(Canvas& c, const Theme& t, const Tile& tile, D2D1_RECT_F r, const std::wstring& id, bool selected, bool target);
    void DrawIcon(Canvas& c, const std::wstring& id, int px, D2D1_RECT_F r, float radius, const std::wstring& name = L"");
    void PaintHoverTip(Canvas& c, const Theme& t);
    void PaintRowHeading(Canvas& c, const Theme& t, D2D1_RECT_F r, wchar_t letter);
    void PaintRowItem(Canvas& c, const Theme& t, const Row& row, D2D1_RECT_F r, size_t index, bool firstIsDefault, int rowPx);
    void PaintRows(Canvas& c, const Theme& t, const std::vector<Row>& rows, float shown);
    void PaintIndex(Canvas& c, const Theme& t); // the A-Z list's letters, and the one being picked
    void PaintFolderTitle(Canvas& c, const Theme& t, const Tile& f, D2D1_RECT_F full);
    void PaintFolderApps(Canvas& c, const Theme& t, const Tile& f, D2D1_RECT_F full);
    void PaintFolder(Canvas& c, const Theme& t, D2D1_RECT_F card, float shown);
    void PaintDragged(Canvas& c, const Theme& t);
    void PaintGrip(Canvas& c, const Theme& t);
    void PaintShelfButton(Canvas& c, const Theme& t, float& x, float cy, float bh, const std::wstring& id, const std::wstring& glyph, const std::wstring& label,
                          bool dim);
    void PaintShelfAccount(Canvas& c, const Theme& t, float left, float maxRight, float cy, float bh, const D2D1_RECT_F& shelf);
    void PaintShelf(Canvas& c, const Theme& t);
    void PaintPower(Canvas& c, const Theme& t);
    void PaintDots(Canvas& c, const Theme& t, int pages, float current, float y, const wchar_t* prefix);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    void HandleExternalMouseDown(LPARAM l);
    void RemoveMouseWatcher();
    bool HandleMouseMove(WPARAM w, POINT pt);
    bool HandleLeftDown(POINT pt, POINT content);
    bool HandleLeftUp(POINT pt);
    bool HandleMouseWheel(UINT m, WPARAM w, POINT pt);
    bool HandleKeyDown(WPARAM w);
    bool HandleChar(WPARAM w, LRESULT& result);
    void HandleEscapeKey();
    void HandleReturnKey();

    // The tiles, the rows.
    void Rebuild();
    void BuildDefaultTiles();
    void BuildCustomTiles(const std::map<std::wstring, const CatalogApp*>& known);
    void SyncWindowsPins();            // tiles_ from the layout (or the Windows pins) and the catalog
    std::vector<Row> Results() const; // search matches, best first
    std::vector<Row> AllApps() const; // the A-Z list with its headings
    int PerPage() const { return cols_ * rows_; }
    int Pages() const;
    int FolderPages() const;
    void SetPage(int page);
    // Changing the arrangement: the Windows pins copied in first, saved after.
    void Arrange(const std::function<void(std::vector<StartTile>&)>& change);
    // The sync button: what is pinned to the Windows start menu, and the All apps list's folders
    // with their apps, join the arrangement (nothing the user arranged moves or goes).
    void OpenSyncMenu();
    void SyncFromWindows(bool pins, bool folders, bool refresh = true);
    void PinToStart(const CatalogApp& app);
    bool OnStart(const std::wstring& id) const;
    bool Hidden(const std::wstring& id) const;
    void SetHidden(const CatalogApp& app, bool hidden);
    void ShowHidden(); // the A-Z list at its hidden apps

    // Geometry (DIP). The menu's own coordinates: the panel's top left corner is
    // 0, 0; in the window it sits OffsetY() lower, on the window's bottom edge.
    float W() const; // the panel and the capsule, as shown (springing to a new size)
    float H() const;
    // The size shown: a tile's icon, its cell (the name under the icon), and the grid they make.
    float IconSize() const;
    float CellW() const;
    float CellH() const;
    float LabelSize() const; // the name under the icon, in step with it
    float LabelH() const;    // the room that name takes in the cell
    D2D1_SIZE_F SizeFor(int grid, int icons) const; // the tables in the .cpp
    D2D1_SIZE_F Size() const;                       // the window's: the largest grid that fits
    D2D1_SIZE_F PanelSize() const;                  // the panel's, inside it: the size chosen
    bool Fits(int grid, int icons) const;
    void UpdateRoom();                              // the largest grid that fits, at these icons
    float OffsetY() const;
    LONG OffsetPx() const;
    // Centred, the window (the largest size that fits) has the panel in its middle.
    float OffsetX() const;
    LONG OffsetXPx() const;
    PopupAlign Align() const { return centered_ ? PopupAlign::Center : PopupAlign::Left; }
    void SetSize(int grid, int icons); // the menu's size, and its icons'; the pieces spring to them
    D2D1_RECT_F MainRect() const;
    D2D1_RECT_F ShelfRect() const;
    D2D1_RECT_F PowerRect() const; // the power card when fully open
    float PowerRadius() const;     // its corners, concentric with the panel's
    D2D1_RECT_F SearchRect() const;
    D2D1_RECT_F AllButton() const; // "全部应用" / "返回"
    D2D1_RECT_F SyncButton() const; // beside it: sync with the Windows start menu
    D2D1_RECT_F FolderRect() const; // the open folder's card
    float ListBottom() const;
    float GridX() const;
    D2D1_RECT_F Cell(int slot) const; // a grid slot, its page laid out side by side (x + page * W)
    int FolderApps() const;
    int FolderCols() const;
    int FolderRows() const;
    D2D1_RECT_F FolderCell(int slot) const; // likewise in the folder's card
    float ButtonSize() const;      // round buttons in the capsule
    float Covered(const D2D1_RECT_F& r) const; // 1, or less while the power card covers r
    D2D1_RECT_F IndexRect() const; // the A-Z letters down the list's left side
    bool Indexed() const { return allApps_ && query_.empty(); }

    // Folders.
    void OpenFolder(int tile);
    void CloseFolder();
    void StartNaming(const std::wstring& key = L"");
    void FinishNaming(bool keep);

    // Dragging tiles; resizing.
    std::vector<int> Order() const;       // the grid's slots: tile indices, -1 the gap
    std::vector<int> FolderOrder() const; // the folder's
    void BeginDrag();
    void UpdateDrag(POINT client);
    void UpdateDragHover(int cols, int rows, float x0, float top, int page, double now);
    void EndDrag(bool commit);
    void CommitDrag(const Drag& d, const std::wstring& appId, const std::wstring& targetKey, const std::wstring& folderKey, bool targetIsFolder,
                    const std::vector<std::wstring>& keys);
    void CommitFolderMerge(const std::wstring& appId, const std::wstring& targetKey, bool targetIsFolder);
    void DragTick(); // the pointer resting: the others make room, the page turns
    void Resize(POINT screen);
    void EndResize();
    void ScrubTo(float y); // a letter of the index: the list glides to it
    wchar_t TopLetter() const; // the letter of the list's rows at the top

    void Activate(const std::wstring& id);
    void Launch(const CatalogApp& app);
    void ContextMenu(const std::wstring& id, POINT screen);
    std::vector<MenuEntry> BuildContextEntries(const std::wstring& id, CatalogApp& app, std::wstring& folderKey) const;
    void RunContextCommand(int cmd, const CatalogApp& app, const std::wstring& folderKey, const std::wstring& openKey);
    static std::optional<StartTile> TakeTile(std::vector<StartTile>& tiles, const std::wstring& id);
    static int ParseIndex(const std::wstring& id, size_t prefix);
    void RunPower(int index);
    std::wstring HitAt(POINT pt) const;
    const std::vector<Row>& RowsShown() const { return query_.empty() ? list_ : results_; }
    bool Listing() const { return !query_.empty() || allApps_; }
    void MoveSelection(int dx, int dy);
    void EnsureVisible();
    void PlaceIme();
    bool ImeMessage(UINT m, WPARAM w, LPARAM l, LRESULT& result); // the search's composition, inline
    bool TimerMessage(WPARAM w);
    bool NamingChar(wchar_t ch); // a key typed into a folder's name
    void Restage(); // the content changed: tiles slide in again
    void LoadAccount();
    float Hover(Canvas& c, const std::wstring& id, bool on); // eased 0..1, per element

    Surface surface_;
    TaskModel& tasks_;
    AppCatalog& catalog_;
    StartLayout layout_;
    int cols_ = 0, rows_ = 0;   // the grid shown (layout_.size, unless the screen is too small)
    int grid_ = 0, room_ = 0;   // the menu's size; the largest that fits (the window's)
    float panelW_ = 0, panelV_ = 0, mainH_ = 0, mainV_ = 0; // the panel's width and height, springing (DIP, DIP/s)
    std::vector<CatalogApp> apps_, pins_;
    std::vector<CatalogFolder> pinGroups_;
    struct {
        bool on = false, pins = false, folders = false;
    } pendingSync_;
    std::vector<Tile> tiles_;
    std::vector<Row> results_, list_;
    std::vector<std::pair<std::wstring, int>> visibleIcons_, askedIcons_;
    std::wstring query_;
    std::wstring composition_;
    bool searchFocused_ = false;
    float caretX_ = 0;
    bool allApps_ = false; // the A-Z list
    int page_ = 0;
    float pageShown_ = 0;               // displayed page, gliding to page_
    float wheelAcc_ = 0;
    float scroll_ = 0, scrollTarget_ = 0, contentH_ = 0;
    int sel_ = -1;
    std::wstring hover_, pressed_;
    std::vector<Hit> hits_;
    SystemStatus status_;
    HWND previous_ = nullptr;
    RECT bar_{};
    int mouseWatcherToken_ = 0;
    bool menuOpen_ = false;
    bool tracking_ = false;
    bool stale_ = false;    // the catalog changed while a tile was carried
    ULONGLONG lastClosed_ = 0;
    float radius_ = 30, barH_ = 60;
    bool showTray_ = true, showStatus_ = true, centered_ = false;
    // The open folder.
    int folder_ = -1;
    bool folderClosing_ = false;
    double folderMs_ = 0;               // opened (its apps pop up from then)
    float folderGrow_ = 0, folderV_ = 0; // the card: 0 its tile .. 1 open, springing
    D2D1_RECT_F folderFrom_{};          // its tile's icon, where the card grows from
    int folderPage_ = 0;
    float folderPageShown_ = 0;
    bool naming_ = false;
    std::wstring nameDraft_, namingKey_, namingHit_;
    D2D1_RECT_F namingRect_{};
    std::wstring tipText_;
    D2D1_RECT_F tipRect_{};
    // Dragging and resizing.
    Drag drag_;
    POINT pressPt_{};
    std::map<std::wstring, D2D1_POINT_2F> tilePos_; // where each tile is drawn, gliding to its slot
    bool sizing_ = false;
    POINT sizeFrom_{};
    int sizeStart_ = 0;
    // Picking a letter of the index.
    bool scrubbing_ = false;
    wchar_t scrubLetter_ = 0;
    float scrubY_ = 0, hintY_ = 0;
    // Animation.
    std::map<std::wstring, motion::Follow> hoverAnim_;
    double stageMs_ = 0;    // content (re)appeared: tiles slide in staggered
    double lastPaint_ = 0;
    float dt_ = 16;         // this frame's step (ms)
    std::wstring rippleId_;
    float rippleX_ = 0, rippleY_ = 0;
    double rippleMs_ = 0;
    std::vector<PowerRow> power_;
    HWND powerNotifyHost_ = nullptr;
    UINT powerNoticeMsg_ = 0;
    bool powerOpen_ = false;
    int powerSel_ = -1;
    float powerGrow_ = 0, powerV_ = 0; // the card's spring: 0 folded into the corner, 1 open
    // Account shown on the shelf.
    std::wstring userName_;
    IconImage avatar_;
    bool private_ = false; // ATRIUM_PRIVACY: no account or network names
};

} // namespace atrium
