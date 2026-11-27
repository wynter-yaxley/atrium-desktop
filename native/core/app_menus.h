// Context menus, jump-list actions, pin helpers, and taskbar reorder for main.cpp.
#pragma once

#include "bar.h"
#include "pinned_app.h"
#include "shell/app_catalog.h"
#include "shell/task_model.h"
#include "ui/glass_menu.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

class SettingsServer;
class StageManager;

struct AppMenuEnv {
    HWND host = nullptr;
    GlassMenu* menu = nullptr;
    TaskModel* tasks = nullptr;
    SettingsServer* server = nullptr;
    StageManager* stage = nullptr;
    std::function<Bar&()> activeBar;
    std::function<void(const wchar_t* page)> openSettings;
    std::function<bool()> barOn;
    std::function<bool()> widgetsOn;
    std::function<void(bool)> setBar;
    std::function<void(bool)> setWidgets;
    std::function<void(bool)> setStage;
};

struct AppMenuTarget {
    std::wstring key;
    std::wstring name;
    bool pinned = false, multipleWindows = false, canTerminate = false;
    int pinIndex = -1;
    std::vector<HWND> windows;
    PinnedApp pin;
    std::wstring pinTarget;
    std::wstring location;
    std::function<void()> launch;
};

MenuPlace BarMenuPlace(const Bar& bar, bool centered);
void LaunchShellAsync(std::wstring verb, std::wstring file, std::wstring args = {}, std::wstring dir = {});
bool PinsEqual(const PinnedApp& a, const PinnedApp& b);
bool AddUniquePin(std::vector<PinnedApp>& pins, const PinnedApp& pin);
bool UnpinMatchingPin(std::vector<PinnedApp>& pins, int pinIndex, const std::optional<PinnedApp>& target);

void ShowBarContextMenu(const AppMenuEnv& env, POINT pt);
void ShowStartContextMenu(const AppMenuEnv& env, POINT pt);
void ShowAppMenu(const AppMenuEnv& env, const AppMenuTarget& target, POINT pt, MenuPlace place, std::function<void(bool)> done = nullptr,
                 std::vector<AppMenuExtra> extras = {});
void ShowTaskItemMenu(const AppMenuEnv& env, const std::wstring& key, POINT pt);
void ShowCatalogAppMenu(const AppMenuEnv& env, const CatalogApp& app, POINT pt, std::function<void(bool)> done, std::vector<AppMenuExtra> extras = {});
void ShowOverflowAppsMenu(const AppMenuEnv& env, const std::vector<std::wstring>& keys, RECT anchor);
void ReorderBarTask(const AppMenuEnv& env, const std::wstring& key, int to);

} // namespace atrium
