// The bar's three newer entries behind one door: Launchpad (next to the start logo), the
// now-playing entry with its card (right of the status entry, only while a player has a media
// session) and the folder stack (left of the tray). It owns their models and popups, and a window of
// its own for their workers' messages; main.cpp hands it the settings, each bar to configure, the
// catalog's news and the moments every popup closes.
#pragma once

#include "render/music_bars.h"
#include "shell/folder_stacks.h"
#include "shell/media_session.h"
#include "ui/folder_stack_popup.h"
#include "ui/launchpad.h"
#include "ui/now_playing_card.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

class AppCatalog;
class Bar;
struct Settings;

class ExtrasHost {
public:
    using ForEachBar = std::function<void(const std::function<void(Bar&)>&)>;
    // forBars: every bar now on screen.
    ExtrasHost(HINSTANCE instance, AppCatalog& catalog, ForEachBar forBars);
    ~ExtrasHost();
    ExtrasHost(const ExtrasHost&) = delete;
    ExtrasHost& operator=(const ExtrasHost&) = delete;

    // The settings as applied; radius: the popups' corner (the bar's, as the other popups get it).
    void Apply(const Settings& s, float radius);
    void ConfigureBar(Bar& bar);
    void OnBarRetiring(const Bar* bar);
    void OnCatalogChanged(bool listChanged = true);
    void ClosePopups();
    bool AnyPopupVisible() const;
    void Shutdown(); // before the bars go: the media watcher's thread ends

    std::function<void()> onPanelsChanged;                   // a popup opened (the notification cards step aside)
    std::function<void(const wchar_t* page)> onOpenSettings; // the empty stack's button
    void SetAppMenu(decltype(Launchpad::onAppMenu) menu, GlassMenu* popup = nullptr)
    {
        launchpad_.onAppMenu = std::move(menu);
        launchpad_.menu = popup;
    } // Launchpad's right click & folder/background menus

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    static HWND CreateHost(HINSTANCE instance, ExtrasHost* self);
    void Click(Bar& bar, const std::wstring& key, RECT entry);
    void OnMedia(const MediaSnapshot& snapshot);
    std::wstring MusicTip() const; // the entry's tooltip: the track
    void UpdateSampling();   // the player's level is read only while a bar shows it
    void WakeMusic();        // wakes bar render threads and the card on audio or rhythm changes
    void PrewarmLaunchpad(); // every app's icon, for the main display

    ForEachBar forBars_;
    HWND host_; // message-only: the media watcher's and the stack's news
    std::shared_ptr<MusicFeed> feed_;
    std::shared_ptr<std::atomic<bool>> wakeQueued_{std::make_shared<std::atomic<bool>>(false)};
    Launchpad launchpad_;
    NowPlayingCard card_;
    FolderStackModel stackModel_;
    FolderStackPopup stacks_;
    std::unique_ptr<MediaSessionWatcher> media_;
    MediaSnapshot last_;
    bool musicOn_ = false, musicShown_ = false, stacksOn_ = false, launchpadOn_ = false;
    bool liveRhythm_ = false, stacksCached_ = false;
    std::optional<bool> samplingActive_;
    std::vector<std::wstring> lastFolderStacks_;
    std::vector<std::wstring> lastFolderStackFiles_;
    Bar* lastBar_ = nullptr;
};

} // namespace atrium
