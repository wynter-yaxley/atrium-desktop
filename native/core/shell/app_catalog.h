// All installed apps, as the Windows start menu lists them (the shell's
// AppsFolder: Store apps and start menu shortcuts), and the apps pinned to the
// Windows start menu, in its order. Enumerated and iconified on a worker
// thread; the UI thread reads snapshots.
#pragma once

#include "icon_image.h"
#include "pinned_app.h"
#include "shell/app_icon_cache.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct StartPinGroup;

struct CatalogApp {
    std::wstring name;    // display name
    std::wstring id;      // parsing name inside AppsFolder (AUMID or known-folder path), or a shortcut's full path
    std::wstring aumid;   // System.AppUserModel.ID, may be empty
    std::wstring target;  // link target exe, may be empty
    std::wstring args;    // the link's arguments (a browser's --app=..., a profile)
    int64_t installed = 0; // when it came (FILETIME ticks): a package's install, a shortcut's creation; 0 unknown
    bool system = false;   // Windows' own tools/drivers, placed in the System apps folder
    bool microsoft = false; // built-in Microsoft user apps, placed in the Microsoft apps folder
};

// A folder of the Windows start menu's All apps list (a folder of shortcuts under Start Menu\Programs,
// "Git", "7-Zip"), by its shown name, with its apps; Windows' own folders and uninstallers left out.
struct CatalogFolder {
    std::wstring name;
    std::vector<CatalogApp> apps;
    bool folder = true; // in StartPinGroups(): false for unnamed group (top-level tiles)
};

enum class VisibleIconsOwner { Start, Launchpad };

class AppCatalog {
public:
    // Posts `readyMsg` to `notify` when the list or an icon becomes available.
    AppCatalog(HWND notify, UINT readyMsg);
    ~AppCatalog();
    AppCatalog(const AppCatalog&) = delete;
    AppCatalog& operator=(const AppCatalog&) = delete;

    void Refresh(bool forceLayout = false); // enumerate again (asynchronous)
    bool Loaded() const;
    std::vector<CatalogApp> Apps() const; // sorted by name (user locale)
    // Pinned to the Windows start menu (not the taskbar: those are the bar's pins).
    std::vector<CatalogApp> StartPins() const;
    // Windows 10 start menu groups/folders (empty on Windows 11).
    std::vector<CatalogFolder> StartPinGroups() const;
    bool StartMenuUsesXml() const;
    // The All apps list's folders holding two apps or more (read with the apps).
    std::vector<CatalogFolder> StartFolders() const;
    IconImage Icon(const std::wstring& id, int px); // empty until loaded
    // The start pins' icons at `px` are read after every refresh, so the start
    // menu's first opening shows them at once (requests for icons on screen go first).
    void PrefetchStartIcons(int px);
    // The apps of the start menu's own arrangement (start.json): read ahead and kept like the pins'.
    void SetStartIds(std::vector<std::wstring> ids);
    void SetVisibleIcons(VisibleIconsOwner owner, const std::vector<std::pair<std::wstring, int>>& icons);
    // Icons read ahead for adjacent pages (`px`) and protected from eviction until the next turn.
    void SetAheadIcons(const std::vector<std::wstring>& ids, int px);
    // Background prefetch (Launchpad): queued behind visible and adjacent icons, subject to cache eviction.
    void PrefetchIcons(const std::vector<std::wstring>& ids, int px);
    // Every one of these icons read (or found missing): asks for those not yet asked for.
    bool IconsReady(const std::vector<std::wstring>& ids, int px);

    struct PreparedQuery {
        std::wstring folded;
        bool asciiAlnum = false;
        bool pathMatch = false;
    };

    static bool Launch(const CatalogApp& app);
    // Desktop apps only (a packaged app has no exe to elevate or to show): run elevated (the
    // UAC prompt), and the file to select in File Explorer (the exe, else the shortcut); empty: none.
    static bool LaunchAsAdmin(const CatalogApp& app);
    static std::wstring FileLocation(const CatalogApp& app);
    static PinnedApp PinFor(const CatalogApp& app);
    // Looks up the Start Menu / AppsFolder shell target for an app by AUMID, executable, or pin path.
    static std::wstring IconTargetFor(const std::wstring& aumid, const std::wstring& exePath, const std::wstring& pinPath);
    static bool HasPendingInitialRefresh();
    // Case-insensitive match of `query` against the name (and words in it), or the start of
    // its initials (NameInitials: "wx" finds 微信).
    static PreparedQuery PrepareQuery(const std::wstring& query);
    static bool MatchesPrepared(const CatalogApp& app, const PreparedQuery& query);
    static bool Matches(const CatalogApp& app, const std::wstring& query);
    // An app to show among apps (Launchpad): not an uninstaller, nor a shortcut to a document, a web
    // page, a help or a folder, nor one of the shell's own places.
    static bool IsLaunchable(const CatalogApp& app);

private:
    void Worker();
    HWND notify_;
    UINT msg_;
    HANDLE stopEvent_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<CatalogApp> apps_, startPins_;
    std::vector<CatalogFolder> startPinGroups_, startFolders_;
    // Worker only: the exported layout and the layout file's time it is from.
    std::vector<std::pair<std::wstring, std::wstring>> layout_; // key (packagedAppId | desktopAppLink), value
    std::vector<StartPinGroup> xmlLayout_;
    FILETIME layoutTime_{};
    bool layoutLoaded_ = false;
    bool loaded_ = false, refresh_ = false, forceLayout_ = false, stop_ = false;
    void QueuePrefetch(); // under mutex_
    void PruneIcons(bool sweepStale = false); // under mutex_
    void RefreshStartLayout(bool force); // worker only
    std::vector<CatalogApp> ResolveStartLayoutPins(const std::vector<CatalogApp>& apps, std::vector<CatalogFolder>& pinGroups) const;
    void RunRefresh(bool forceLayout);
    void RunIconJob(const IconKey& job);
    AppIconCache cache_;
    std::thread thread_;
};

// Internal helpers shared between app_catalog.cpp, app_catalog_stamp.cpp, and start_pins.cpp.
std::wstring ExpandKnownEnvFolders(const std::wstring& value);
void CollectStartLinks(const std::wstring& dir, std::vector<std::pair<std::wstring, std::wstring>>& links, int depth = 0);
std::vector<CatalogFolder> ReadStartMenuFolders(const std::vector<CatalogApp>& apps);
bool IsSystemStartFolder(const std::wstring& foldedName);
bool IsToolStartFolder(const std::wstring& foldedName);
bool IsStartupStartFolder(const std::wstring& foldedName);
bool NotAnAppShortcut(const CatalogApp& app);
void SortCatalogApps(std::vector<CatalogApp>& apps);
bool IsCatalogTargetMissing(const std::wstring& path);
void StampInstalledApps(std::vector<CatalogApp>& apps);

} // namespace atrium
