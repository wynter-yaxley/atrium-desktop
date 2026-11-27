#include "settings.h"
#include "shell/logon_task.h"
#include "shell/system_chrome.h"
#include "ui/stage_manager.h"

#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct TempDir {
    fs::path path;
    explicit TempDir(const wchar_t* prefix)
    {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        path = fs::path(base) / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::error_code ec;
        fs::create_directories(path, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string ReadBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool WriteBytes(const fs::path& p, const std::string& data)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(data.data(), std::streamsize(data.size()));
    return out.good();
}

std::vector<fs::path> ListFiles(const fs::path& dir)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!ec && entry.is_regular_file(ec))
            out.push_back(entry.path());
    }
    return out;
}

bool HasTmpFiles(const fs::path& dir)
{
    return std::ranges::any_of(ListFiles(dir), [](const fs::path& p) { return p.extension() == L".tmp"; });
}

std::vector<fs::path> ListCorruptBackups(const fs::path& dir)
{
    std::vector<fs::path> out;
    for (const auto& p : ListFiles(dir)) {
        const std::wstring name = p.filename().wstring();
        if (name.starts_with(L"settings.corrupt.") && name.ends_with(L".json"))
            out.push_back(p);
    }
    return out;
}

void CheckWriteTextFile(const fs::path& root)
{
    const fs::path dir = root / L"write";
    fs::create_directories(dir);
    const fs::path target = dir / L"sample.txt";
    Check(atrium::WriteTextFile(target.wstring(), L"hello \u4e2d\u6587 1"), "WriteTextFile writes initial UTF-8 text");
    Check(atrium::WriteTextFile(target.wstring(), L"hello \u4e2d\u6587 2"), "WriteTextFile overwrites atomically");
    Check(atrium::ReadTextFile(target.wstring()) == L"hello \u4e2d\u6587 2", "ReadTextFile reads overwritten UTF-8 text");
    Check(!HasTmpFiles(dir), "WriteTextFile leaves no .tmp files on success");

    const fs::path dirTarget = dir / L"occupied_dir";
    fs::create_directories(dirTarget);
    SetLastError(ERROR_SUCCESS);
    Check(!atrium::WriteTextFile(dirTarget.wstring(), L"fail"), "WriteTextFile fails when MoveFileExW target is an existing directory");
    Check(GetLastError() != ERROR_SUCCESS, "WriteTextFile preserves Win32 error code on MoveFileExW failure");
    Check(!HasTmpFiles(dir), "WriteTextFile cleans up .tmp when MoveFileExW fails");
}

void CheckSettingsCorruptAndFuture(const fs::path& root)
{
    const fs::path dir = root / L"settings";
    fs::create_directories(dir);
    const fs::path cfg = dir / L"settings.json";

    const std::string corruptBytes = "{broken-first";
    Check(WriteBytes(cfg, corruptBytes), "write corrupt fixture");
    atrium::SettingsFileState state = atrium::SettingsFileState::Missing;
    bool backupOk = false;
    atrium::Settings loaded = atrium::LoadSettingsFrom(cfg.wstring(), &state, &backupOk);
    Check(state == atrium::SettingsFileState::Corrupt && backupOk, "LoadSettingsFrom detects corrupt JSON and backs it up");
    Check(loaded.height == atrium::Settings{}.height, "LoadSettingsFrom returns default settings for corrupt JSON");
    Check(ReadBytes(cfg) == corruptBytes, "LoadSettingsFrom keeps original corrupt file until overwrite");
    const auto backups1 = ListCorruptBackups(dir);
    Check(backups1.size() == 1 && ReadBytes(backups1[0]) == corruptBytes, "corrupt backup preserves exact original bytes");
    loaded = atrium::LoadSettingsFrom(cfg.wstring(), &state, &backupOk);
    Check(state == atrium::SettingsFileState::Corrupt && backupOk && ListCorruptBackups(dir).size() == 1,
          "repeated LoadSettingsFrom on unchanged corrupt file deduplicates backup");

    atrium::Settings modified;
    modified.height = 72;
    std::wstring error;
    for (int i = 0; i < 4; ++i) {
        const std::string payload = "{broken-round-" + std::to_string(i);
        Check(WriteBytes(cfg, payload), "write corrupt file before SaveSettingsTo");
        bool startupChanged = false;
        Check(atrium::SaveSettingsTo(cfg.wstring(), modified, &error, {}, &startupChanged) && !startupChanged,
              "SaveSettingsTo backs up corrupt file and saves");
        Check(error.empty(), "SaveSettingsTo leaves error empty on success");
    }
    const auto backupsN = ListCorruptBackups(dir);
    Check(backupsN.size() == 3, "corrupt backups are capped at 3 files in the directory");
    Check(atrium::LoadSettingsFrom(cfg.wstring(), &state).height == 72 && state == atrium::SettingsFileState::Valid, "saved valid settings reload cleanly");

    atrium::Settings startupFlipped = modified;
    startupFlipped.startOnLogin = !modified.startOnLogin;
    bool startupChanged = false;
    Check(atrium::SaveSettingsTo(cfg.wstring(), startupFlipped, &error, {}, &startupChanged) && startupChanged,
          "SaveSettingsTo reports startupChanged when startOnLogin changes");
    Check(atrium::SaveSettingsTo(cfg.wstring(), startupFlipped, &error, {}, &startupChanged) && !startupChanged,
          "SaveSettingsTo clears startupChanged when startOnLogin is unchanged");
    atrium::Settings startupModeFlipped = modified;
    startupModeFlipped.startOnLogin = true;
    startupModeFlipped.startupMode = L"run";
    Check(atrium::SaveSettingsTo(cfg.wstring(), startupModeFlipped, &error, {}, &startupChanged), "SaveSettingsTo baseline for startupMode change");
    startupModeFlipped.startupMode = L"task";
    Check(atrium::SaveSettingsTo(cfg.wstring(), startupModeFlipped, &error, {}, &startupChanged) && startupChanged,
          "SaveSettingsTo reports startupChanged when startupMode changes while startOnLogin is true");
    Check(atrium::SaveSettingsTo(cfg.wstring(), startupModeFlipped, &error, {}, &startupChanged) && !startupChanged,
          "SaveSettingsTo clears startupChanged when startupMode is unchanged");
    atrium::Settings disabledModeFlipped = startupModeFlipped;
    disabledModeFlipped.startOnLogin = false;
    Check(atrium::SaveSettingsTo(cfg.wstring(), disabledModeFlipped, &error, {}, &startupChanged) && startupChanged,
          "SaveSettingsTo reports startupChanged when disabling startOnLogin");
    disabledModeFlipped.startupMode = L"run";
    Check(atrium::SaveSettingsTo(cfg.wstring(), disabledModeFlipped, &error, {}, &startupChanged) && !startupChanged,
          "SaveSettingsTo ignores startupMode changes while startOnLogin is false");

    const std::string lockedCorrupt = "{broken-backup-must-fail";
    Check(WriteBytes(cfg, lockedCorrupt), "write corrupt file for backup failure test");
    Check(!atrium::SaveSettingsTo(cfg.wstring(), modified, &error, [](const std::wstring&) { return false; }),
          "SaveSettingsTo refuses overwrite when corrupt backup fails");
    Check(!error.empty(), "SaveSettingsTo sets clear error when corrupt backup fails");
    Check(ReadBytes(cfg) == lockedCorrupt, "SaveSettingsTo preserves corrupt original bytes when backup fails");

    const std::string futureJson = R"({"version":99,"height":80,"futureNested":{"k":"v"}})";
    Check(WriteBytes(cfg, futureJson), "write future-version fixture");
    loaded = atrium::LoadSettingsFrom(cfg.wstring(), &state, &backupOk);
    Check(state == atrium::SettingsFileState::FutureVersion && loaded.height == 80, "LoadSettingsFrom reads compatible fields from future version read-only");
    modified.height = 44;
    Check(!atrium::SaveSettingsTo(cfg.wstring(), modified, &error), "SaveSettingsTo refuses to overwrite future version");
    Check(!error.empty(), "SaveSettingsTo sets clear error when refusing future version");
    Check(ReadBytes(cfg) == futureJson, "SaveSettingsTo preserves future-version file byte-for-byte");
    Check(!atrium::BackupCorruptSettings((dir / L"missing.json").wstring(), 3), "BackupCorruptSettings fails on missing file");
    Check(!HasTmpFiles(dir), "settings tests leave no .tmp files");
}

void CheckStageState()
{
    std::wstring saved;
    int calls = 0;
    bool failNext = true;
    auto writer = [&](const std::wstring& text) {
        ++calls;
        if (failNext)
            return false;
        return !text.empty();
    };
    const std::wstring payload = LR"({"parked":[{"hwnd":1,"key":"a.exe|1"}],"sets":[["a.exe|1"]]})";
    Check(!atrium::SaveStageState(payload, saved, writer), "SaveStageState returns false when writer fails");
    Check(saved.empty() && calls == 1, "SaveStageState does not advance saved_ on failure");
    failNext = false;
    Check(atrium::SaveStageState(payload, saved, writer), "SaveStageState retries identical content after failure");
    Check(saved == payload && calls == 2, "SaveStageState advances saved_ after retry succeeds");
    Check(atrium::SaveStageState(payload, saved, writer) && calls == 2, "SaveStageState skips unchanged content after success");
    const std::wstring emptyState = LR"({"parked":[],"sets":[]})";
    Check(atrium::SaveStageState(emptyState, saved, writer) && saved == emptyState && calls == 3, "SaveStageState writes empty restored state");
    failNext = true;
    Check(!atrium::SaveStageState(payload, saved, writer) && !atrium::SaveStageState(L"other", saved, writer),
          "consecutive SaveStageState failures do not wedge");
    failNext = false;
    Check(atrium::SaveStageState(payload, saved, writer) && saved == payload && calls == 6, "SaveStageState recovers cleanly after consecutive failures");
}

void CheckSystemChromeTaskbar()
{
    using namespace atrium;
    std::vector<std::string> events;
    bool saveShouldSucceed = false;
    bool setAutoHideShouldSucceed = true;
    int setAutoHideCount = 0;
    int hideCount = 0;
    int showCount = 0;
    int watchCount = 0;
    int stopWatchCount = 0;
    ChromeRecord lastRecord;
    TaskbarChromeState state;
    TaskbarChromeOps ops{
        .queryAutoHide = []() -> std::optional<bool> { return false; },
        .setAutoHide =
            [&](bool on) {
                ++setAutoHideCount;
                events.emplace_back(on ? "setAutoHide(true)" : "setAutoHide(false)");
                return setAutoHideShouldSucceed;
            },
        .hasShellTaskbar = [] { return true; },
        .hideTaskbars =
            [&] {
                ++hideCount;
                events.emplace_back("hideTaskbars");
            },
        .showTaskbars =
            [&] {
                ++showCount;
                events.emplace_back("showTaskbars");
            },
        .watch =
            [&] {
                ++watchCount;
                events.emplace_back("watch");
            },
        .stopWatch =
            [&] {
                ++stopWatchCount;
                events.emplace_back("stopWatch");
            },
        .scheduleRefit = [&] { events.emplace_back("scheduleRefit"); },
        .setHidden = [&](bool h) { events.emplace_back(h ? "setHidden(true)" : "setHidden(false)"); },
        .saveRecord =
            [&](const ChromeRecord& rec) {
                lastRecord = rec;
                events.emplace_back("saveRecord");
                return saveShouldSucceed;
            },
    };

    ApplyTaskbarHidden(true, state, {}, ops);
    Check(!state.hidden && !state.autoHideOurs, "hide aborted when SaveRecord fails");
    Check(setAutoHideCount == 0 && hideCount == 0 && watchCount == 0, "no taskbar or auto-hide mutation when SaveRecord fails");

    events.clear();
    saveShouldSucceed = true;
    ApplyTaskbarHidden(true, state, {}, ops);
    Check(state.hidden && state.autoHideOurs, "hide succeeds when SaveRecord succeeds");
    Check(lastRecord.taskbarHidden && lastRecord.autoHide, "SaveRecord received intended taskbar + autoHide record");
    const auto saveIt = std::ranges::find(events, "saveRecord");
    const auto autoIt = std::ranges::find(events, "setAutoHide(true)");
    const auto hideIt = std::ranges::find(events, "hideTaskbars");
    Check(saveIt != events.end() && autoIt != events.end() && hideIt != events.end() && saveIt < autoIt && saveIt < hideIt,
          "SaveRecord executes strictly before SetAutoHide and HideTaskbars");

    events.clear();
    saveShouldSucceed = false;
    ApplyTaskbarHidden(false, state, {}, ops);
    Check(!state.hidden && !state.autoHideOurs, "restore clears flags even when SaveRecord fails");
    Check(stopWatchCount == 1 && showCount == 1 && setAutoHideCount == 2,
          "restore runs stopWatch, showTaskbars and SetAutoHide(false) despite SaveRecord failure");

    TaskbarChromeState rollbackState;
    events.clear();
    saveShouldSucceed = true;
    setAutoHideShouldSucceed = false;
    ApplyTaskbarHidden(true, rollbackState, {}, ops);
    Check(rollbackState.hidden && !rollbackState.autoHideOurs && !lastRecord.autoHide, "failed SetAutoHide rolls back autoHide claim in system.json");
}

void CheckSystemChromeBanners()
{
    using namespace atrium;
    std::vector<std::string> events;
    bool saveShouldSucceed = false;
    bool setQuietShouldSucceed = true;
    std::wstring currentQuiet = kQuietProfileOff;
    bool autoPlayShown = true;
    int setQuietCount = 0;
    int setAutoPlayCount = 0;
    ChromeRecord lastRecord;
    BannerChromeState state;
    BannerChromeOps ops{
        .quietProfile = [&] { return currentQuiet; },
        .setQuietProfile =
            [&](const wchar_t* profile) {
                ++setQuietCount;
                events.emplace_back("setQuietProfile");
                if (!setQuietShouldSucceed)
                    return false;
                currentQuiet = profile;
                return true;
            },
        .autoPlayBannerShown = [&] { return autoPlayShown; },
        .setAutoPlayBanner =
            [&](bool shown) {
                ++setAutoPlayCount;
                autoPlayShown = shown;
                events.emplace_back("setAutoPlayBanner");
                return true;
            },
        .saveRecord =
            [&](const ChromeRecord& rec) {
                lastRecord = rec;
                events.emplace_back("saveRecord");
                return saveShouldSucceed;
            },
    };

    ApplyBannersHidden(true, state, {}, ops);
    Check(!state.bannersHidden && !state.quietOurs && !state.quietReady && !state.autoPlayBannerOurs && !state.autoPlayBannerReady,
          "banner hide aborted when SaveRecord fails");
    Check(setQuietCount == 0 && setAutoPlayCount == 0 && currentQuiet == kQuietProfileOff && autoPlayShown,
          "no DND or AutoPlay registry mutation when SaveRecord fails");

    events.clear();
    saveShouldSucceed = true;
    ApplyBannersHidden(true, state, {}, ops);
    Check(state.bannersHidden && state.quietOurs && state.quietReady && state.autoPlayBannerOurs && state.autoPlayBannerReady,
          "banner hide succeeds when SaveRecord succeeds");
    Check(lastRecord.quiet && lastRecord.quietRestore == kQuietProfileOff && lastRecord.autoPlayBanner, "SaveRecord received intended DND + AutoPlay record");
    const auto saveIt = std::ranges::find(events, "saveRecord");
    const auto quietIt = std::ranges::find(events, "setQuietProfile");
    const auto autoPlayIt = std::ranges::find(events, "setAutoPlayBanner");
    Check(saveIt != events.end() && quietIt != events.end() && autoPlayIt != events.end() && saveIt < quietIt && saveIt < autoPlayIt,
          "SaveRecord executes strictly before SetQuietProfile and SetAutoPlayBanner");

    setQuietShouldSucceed = false;
    ApplyBannersHidden(false, state, {}, ops);
    Check(!state.bannersHidden && state.quietOurs && state.quietBefore == kQuietProfileOff && lastRecord.quiet && lastRecord.quietRestore == kQuietProfileOff &&
              !state.autoPlayBannerOurs && autoPlayShown,
          "failed DND restore keeps quietOurs and system.json quiet recovery record");
    setQuietShouldSucceed = true;
    ApplyBannersHidden(false, state, {}, ops);
    Check(!state.quietOurs && state.quietBefore.empty() && currentQuiet == kQuietProfileOff && !lastRecord.quiet,
          "subsequent retry completes DND restore and clears quiet record");

    ApplyBannersHidden(true, state, {}, ops);
    currentQuiet.clear();
    ApplyBannersHidden(false, state, {}, ops);
    Check(state.quietOurs && lastRecord.quiet && lastRecord.quietRestore == kQuietProfileOff,
          "unavailable QuietHours service keeps quietOurs and recovery record for retry");
    currentQuiet = kQuietProfile;
    saveShouldSucceed = false;
    ApplyBannersHidden(false, state, {}, ops);
    Check(!state.bannersHidden && !state.quietOurs && !state.quietReady && !state.autoPlayBannerOurs && !state.autoPlayBannerReady,
          "banner restore clears flags even when SaveRecord fails");
    Check(currentQuiet == kQuietProfileOff && autoPlayShown, "banner restore reverts DND and AutoPlay despite SaveRecord failure");
}

bool JsonFlag(const winrt::Windows::Data::Json::JsonObject& o, const wchar_t* key)
{
    return o.HasKey(key) && o.GetNamedValue(key).ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean && o.GetNamedBoolean(key);
}

std::wstring JsonText(const winrt::Windows::Data::Json::JsonObject& o, const wchar_t* key)
{
    if (o.HasKey(key) && o.GetNamedValue(key).ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
        return std::wstring(o.GetNamedString(key));
    return {};
}

bool ParseJsonFile(const fs::path& p, winrt::Windows::Data::Json::JsonObject& out)
{
    const std::wstring raw = atrium::ReadTextFile(p.wstring());
    return !raw.empty() && winrt::Windows::Data::Json::JsonObject::TryParse(raw, out);
}

void CheckSaveChromeRecord(const fs::path& root)
{
    const fs::path dir = root / L"chrome_record";
    fs::create_directories(dir);
    const fs::path sysPath = dir / L"system.json";
    atrium::ChromeRecord rec{true, true, true, atrium::kQuietProfileOff, true};
    Check(atrium::SaveChromeRecord(sysPath.wstring(), rec), "SaveChromeRecord writes non-empty system.json");
    Check(fs::exists(sysPath), "system.json exists after SaveChromeRecord");
    Check(atrium::SaveChromeRecord(sysPath.wstring(), atrium::ChromeRecord{}), "SaveChromeRecord deletes empty system.json");
    Check(!fs::exists(sysPath), "system.json removed when ChromeRecord is Empty()");
    Check(!HasTmpFiles(dir), "SaveChromeRecord leaves no .tmp files");
}

void CheckUndoLeftoverRetry(const fs::path& root)
{
    using namespace atrium;
    const fs::path dir = root / L"undo_leftover";
    fs::create_directories(dir);
    const fs::path sysPath = dir / L"system.json";
    const std::wstring priorQuiet = L"Microsoft.QuietHoursProfile.PriorityOnly";
    std::vector<std::string> events;
    bool autoHideSucceeds = true;
    std::wstring currentQuiet;
    bool setQuietSucceeds = false;
    bool autoPlaySucceeds = false;
    int autoHideCalls = 0;
    int setQuietCalls = 0;
    int autoPlayCalls = 0;
    std::wstring restoredQuiet;
    UndoLeftoverOps ops{
        .showTaskbars = [&] { events.emplace_back("showTaskbars"); },
        .setAutoHide =
            [&](bool on) {
                ++autoHideCalls;
                events.emplace_back(on ? "setAutoHide(true)" : "setAutoHide(false)");
                return autoHideSucceeds;
            },
        .quietProfile = [&] { return currentQuiet; },
        .setQuietProfile =
            [&](const wchar_t* profile) {
                ++setQuietCalls;
                events.emplace_back("setQuietProfile");
                if (!setQuietSucceeds)
                    return false;
                restoredQuiet = profile;
                currentQuiet = profile;
                return true;
            },
        .setAutoPlayBanner =
            [&](bool shown) {
                ++autoPlayCalls;
                events.emplace_back(shown ? "setAutoPlay(true)" : "setAutoPlay(false)");
                return autoPlaySucceeds;
            },
        .saveRecord = [&](const ChromeRecord& rec) { return SaveChromeRecord(sysPath.wstring(), rec); },
    };

    // Scenario A: multi-pass partial clear & retry on same system.json
    Check(SaveChromeRecord(sysPath.wstring(), {true, true, true, priorQuiet, true}), "seed system.json for Scenario A");
    Check(ApplyUndoLeftover(sysPath.wstring(), ops), "Scenario A pass 1 returns true");
    Check(!events.empty() && events.front() == "showTaskbars", "pass 1 shows taskbars first before autoHide/autoPlay");
    Check(autoHideCalls == 1 && setQuietCalls == 0 && autoPlayCalls == 1, "pass 1 skips setQuietProfile when service unavailable");
    Check(fs::exists(sysPath), "system.json kept after pass 1 failures");
    winrt::Windows::Data::Json::JsonObject parsed{nullptr};
    Check(ParseJsonFile(sysPath, parsed), "parse system.json after pass 1");
    Check(!JsonFlag(parsed, L"taskbarHidden") && !JsonFlag(parsed, L"autoHide") && JsonFlag(parsed, L"quiet") &&
              JsonText(parsed, L"quietRestore") == priorQuiet && JsonFlag(parsed, L"autoPlayBanner"),
          "pass 1 clears taskbarHidden/autoHide and keeps only quiet + autoPlayBanner");

    events.clear();
    currentQuiet = kQuietProfile;
    autoPlaySucceeds = true;
    Check(ApplyUndoLeftover(sysPath.wstring(), ops), "Scenario A pass 2 returns true");
    Check(autoHideCalls == 1 && setQuietCalls == 1 && autoPlayCalls == 2, "pass 2 retries only failed quiet and autoPlay");
    Check(fs::exists(sysPath), "system.json still kept after pass 2 DND failure");
    Check(ParseJsonFile(sysPath, parsed), "parse system.json after pass 2");
    Check(!JsonFlag(parsed, L"taskbarHidden") && !JsonFlag(parsed, L"autoHide") && JsonFlag(parsed, L"quiet") &&
              JsonText(parsed, L"quietRestore") == priorQuiet && !JsonFlag(parsed, L"autoPlayBanner"),
          "pass 2 clears autoPlayBanner and keeps only failed quiet record");

    setQuietSucceeds = true;
    Check(ApplyUndoLeftover(sysPath.wstring(), ops), "Scenario A pass 3 completes recovery");
    Check(autoPlayCalls == 2 && restoredQuiet == priorQuiet && !fs::exists(sysPath),
          "pass 3 restores prior quiet profile and deletes system.json once all items clear");
    Check(!ApplyUndoLeftover(sysPath.wstring(), ops), "pass 4 returns false when system.json is gone");

    // Scenario B: autoHide failure retry + user-overridden DND profile cleared
    autoHideCalls = 0;
    setQuietCalls = 0;
    autoHideSucceeds = false;
    currentQuiet = priorQuiet;
    Check(SaveChromeRecord(sysPath.wstring(), {true, true, true, kQuietProfileOff, false}), "seed system.json for Scenario B");
    Check(ApplyUndoLeftover(sysPath.wstring(), ops), "Scenario B pass 1 returns true");
    Check(autoHideCalls == 1 && setQuietCalls == 0 && fs::exists(sysPath),
          "Scenario B pass 1 skips setQuietProfile for user-chosen DND and keeps failed autoHide");
    Check(ParseJsonFile(sysPath, parsed) && JsonFlag(parsed, L"autoHide") && !JsonFlag(parsed, L"quiet"),
          "Scenario B pass 1 retains autoHide and clears user-changed quiet");
    autoHideSucceeds = true;
    Check(ApplyUndoLeftover(sysPath.wstring(), ops) && autoHideCalls == 2 && !fs::exists(sysPath), "Scenario B pass 2 clears autoHide and deletes system.json");
    Check(!HasTmpFiles(dir), "UndoLeftover retry tests leave no .tmp files");
}

void CheckStartupHandoffFromLeftover(const fs::path& root)
{
    using namespace atrium;
    const fs::path dir = root / L"startup_handoff";
    fs::create_directories(dir);
    const fs::path sysPath = dir / L"system.json";
    const std::wstring priorQuiet = L"Microsoft.QuietHoursProfile.PriorityOnly";

    bool autoHideSucceeds = false;
    bool autoPlayShown = false;
    std::wstring currentQuiet;
    int setAutoHideCalls = 0;

    UndoLeftoverOps undoOps{
        .showTaskbars = [] {},
        .setAutoHide = [&](bool) { return ++setAutoHideCalls, autoHideSucceeds; },
        .quietProfile = [&] { return currentQuiet; },
        .setQuietProfile = [&](const wchar_t*) { return false; },
        .setAutoPlayBanner = [&](bool) { return false; },
        .saveRecord = [&](const ChromeRecord& rec) { return SaveChromeRecord(sysPath.wstring(), rec); },
    };

    Check(SaveChromeRecord(sysPath.wstring(), {true, true, true, priorQuiet, true}), "seed system.json for startup handoff");
    ChromeRecord leftover{};
    Check(ApplyUndoLeftover(sysPath.wstring(), undoOps, &leftover), "partial UndoLeftover outputs remaining record");
    Check(!leftover.taskbarHidden && leftover.autoHide && leftover.quiet && leftover.quietRestore == priorQuiet && leftover.autoPlayBanner,
          "leftover captures un-restored autoHide, quiet, quietRestore, autoPlayBanner");

    TaskbarChromeState tbState{false, leftover.autoHide};
    BannerChromeState bnState{false, false, leftover.quiet, leftover.quietRestore, leftover.autoPlayBanner, false};
    TaskbarChromeOps tbOps{
        .queryAutoHide = []() -> std::optional<bool> { return true; },
        .setAutoHide = [&](bool) { return ++setAutoHideCalls, autoHideSucceeds; },
        .hasShellTaskbar = [] { return true; },
        .hideTaskbars = [] {},
        .showTaskbars = [] {},
        .watch = [] {},
        .stopWatch = [] {},
        .scheduleRefit = [] {},
        .setHidden = [](bool) {},
        .saveRecord = [&](const ChromeRecord& rec) { return SaveChromeRecord(sysPath.wstring(), rec); },
    };
    BannerChromeOps bnOps{
        .quietProfile = [&] { return currentQuiet; },
        .setQuietProfile =
            [&](const wchar_t* profile) {
                currentQuiet = profile;
                return true;
            },
        .autoPlayBannerShown = [&] { return autoPlayShown; },
        .setAutoPlayBanner =
            [&](bool shown) {
                autoPlayShown = shown;
                return true;
            },
        .saveRecord = [&](const ChromeRecord& rec) { return SaveChromeRecord(sysPath.wstring(), rec); },
    };

    // Failure A: SetTaskbarHidden(true) must preserve adopted quiet/autoPlay recovery records in system.json
    ChromeRecord adoptedBanner{false, false, bnState.quietOurs, bnState.quietBefore, bnState.autoPlayBannerOurs};
    ApplyTaskbarHidden(true, tbState, adoptedBanner, tbOps);
    winrt::Windows::Data::Json::JsonObject parsed{nullptr};
    Check(ParseJsonFile(sysPath, parsed) && JsonFlag(parsed, L"taskbarHidden") && JsonFlag(parsed, L"autoHide") && JsonFlag(parsed, L"quiet") &&
              JsonText(parsed, L"quietRestore") == priorQuiet && JsonFlag(parsed, L"autoPlayBanner"),
          "SetTaskbarHidden preserves leftover quiet, quietRestore and autoPlayBanner");

    // Failure B: SetBannersHidden(true) -> (false) preserves and restores priorQuiet instead of treating AlarmsOnly as user-owned
    currentQuiet = kQuietProfile;
    ApplyBannersHidden(true, bnState, {tbState.hidden, tbState.autoHideOurs, false, {}, false}, bnOps);
    Check(bnState.quietReady && bnState.quietOurs && bnState.quietBefore == priorQuiet && bnState.autoPlayBannerReady && bnState.autoPlayBannerOurs,
          "SetBannersHidden(true) preserves leftover quietBefore and autoPlay ownership");
    ApplyBannersHidden(false, bnState, {false, false, false, {}, false}, bnOps);
    Check(currentQuiet == priorQuiet && autoPlayShown && !bnState.quietOurs && bnState.quietBefore.empty() && !bnState.autoPlayBannerOurs &&
              !fs::exists(sysPath),
          "SetBannersHidden(false) restores priorQuiet/AutoPlay and cleans system.json");

    // Failure C: ApplyTaskbarHidden(false) must retry autoHide when hidden==false but autoHideOurs==true
    TaskbarChromeState tbRetry{false, true};
    autoHideSucceeds = true;
    const int beforeCalls = setAutoHideCalls;
    ApplyTaskbarHidden(false, tbRetry, {}, tbOps);
    Check(setAutoHideCalls == beforeCalls + 1 && !tbRetry.autoHideOurs && !fs::exists(sysPath),
          "ApplyTaskbarHidden(false) retries leftover autoHide when hidden==false");
    Check(!HasTmpFiles(dir), "startup handoff test leaves no .tmp files");
}

void CheckLogonTaskDecisions()
{
    using namespace atrium;
    const uint8_t enabled2[]{0x02, 0x00};
    const uint8_t enabled6[]{0x06, 0x00};
    const uint8_t disabled3[]{0x03, 0x00};
    const uint8_t disabled7[]{0x07, 0x00};
    Check(!IsStartupApprovedDisabled(nullptr, 0), "StartupApproved empty slice is enabled");
    Check(!IsStartupApprovedDisabled(enabled2, sizeof(enabled2)) && !IsStartupApprovedDisabled(enabled6, sizeof(enabled6)),
          "StartupApproved even first byte is enabled");
    Check(IsStartupApprovedDisabled(disabled3, sizeof(disabled3)) && IsStartupApprovedDisabled(disabled7, sizeof(disabled7)),
          "StartupApproved odd first byte is disabled");

    Check(!ShouldSyncStartupOnLaunch(true, true, true, false), "ShouldSyncStartupOnLaunch skips development build");
    Check(!ShouldSyncStartupOnLaunch(false, false, true, false), "ShouldSyncStartupOnLaunch skips unregistered install");
    Check(!ShouldSyncStartupOnLaunch(false, true, false, false), "ShouldSyncStartupOnLaunch skips when startOnLogin is off");
    Check(!ShouldSyncStartupOnLaunch(false, true, true, true), "ShouldSyncStartupOnLaunch skips when StartupApproved is off");
    Check(ShouldSyncStartupOnLaunch(false, true, true, false), "ShouldSyncStartupOnLaunch syncs registered release build");

    const std::wstring exe = LR"(C:\Program Files\Atrium\Atrium.exe)";
    const std::wstring exeAltCase = LR"(c:\program files\atrium\atrium.exe)";
    const std::wstring otherExe = LR"(D:\Portable\Atrium\Atrium.exe)";
    const std::wstring cmd = L"\"" + exe + L"\"";
    const std::wstring cmdAltCase = L"\"" + exeAltCase + L"\"";
    const std::wstring otherCmd = L"\"" + otherExe + L"\"";

    auto samePlan = [](const StartupPlan& a, const StartupPlan& b) {
        return a.ok == b.ok && a.removeTask == b.removeTask && a.writeRun == b.writeRun && a.deleteRun == b.deleteRun && a.createTask == b.createTask;
    };

    Check(ParseStartupMode(L"task") == StartupMode::Task && ParseStartupMode(L"run") == StartupMode::Run && ParseStartupMode(L"other") == StartupMode::Run,
          "ParseStartupMode maps task and falls back to Run");

    Check(samePlan(PlanStartupChange({exe, exeAltCase, {}, false, true}, true), {true, true, true, false, false}),
          "enable migrates own legacy task and writes Run");
    Check(samePlan(PlanStartupChange({exe, exeAltCase, cmd, true, true}, true), {true, true, false, false, false}),
          "enable removes own legacy task and skips identical Run");
    Check(samePlan(PlanStartupChange({exe, otherExe, {}, false, true}, true), {true, true, true, false, false}),
          "enable removes other install task and writes Run");
    Check(samePlan(PlanStartupChange({exe, {}, cmd, true, true}, true), {true, false, false, false, false}), "enable is a no-op when Run already matches");
    Check(samePlan(PlanStartupChange({exe, {}, otherCmd, true, true}, true), {true, false, true, false, false}),
          "enable updates stale Run entry to current exe");

    Check(samePlan(PlanStartupChange({exe, {}, {}, false, true}, true, StartupMode::Task), {true, false, false, false, true}),
          "task mode creates logon task when neither entry exists");
    Check(samePlan(PlanStartupChange({exe, {}, cmd, true, true}, true, StartupMode::Task), {true, false, false, true, true}),
          "task mode migrates Run entry to logon task");
    Check(samePlan(PlanStartupChange({exe, exeAltCase, {}, false, true}, true, StartupMode::Task), {true, false, false, false, false}),
          "task mode is a no-op when own task already exists");
    Check(samePlan(PlanStartupChange({exe, otherExe, cmd, true, true}, true, StartupMode::Task), {true, false, false, true, true}),
          "task mode replaces other install task and removes Run");
    Check(samePlan(PlanStartupChange({exe, {}, {}, false, false}, true, StartupMode::Task), {false, false, false, false, false}),
          "task mode refuses when Run registry read fails");

    Check(samePlan(PlanStartupChange({exe, exeAltCase, cmdAltCase, true, true}, false), {true, true, false, true, false}),
          "disable removes own task and own Run (case-insensitive)");
    Check(samePlan(PlanStartupChange({exe, exeAltCase, {}, false, true}, false), {true, true, false, false, false}),
          "disable removes only own task when Run is absent");
    Check(samePlan(PlanStartupChange({exe, {}, cmdAltCase, true, true}, false), {true, false, false, true, false}),
          "disable removes only own Run when task is absent");
    Check(samePlan(PlanStartupChange({exe, {}, {}, false, true}, false), {true, false, false, false, false}), "disable succeeds when neither entry exists");
    Check(samePlan(PlanStartupChange({exe, {}, otherCmd, true, true}, false), {false, false, false, false, false}),
          "disable refuses when Run belongs to another install");
    Check(samePlan(PlanStartupChange({exe, otherExe, cmd, true, true}, false), {false, false, false, false, false}),
          "disable refuses when task belongs to another install");
    Check(samePlan(PlanStartupChange({exe, exe, {}, false, false}, false), {false, false, false, false, false}),
          "disable refuses when Run registry read fails");
    Check(samePlan(PlanStartupChange({exe, {}, {}, false, true, false}, true, StartupMode::Run), {false, false, false, false, false}),
          "Run mode refuses when Task Scheduler query fails");
    Check(samePlan(PlanStartupChange({exe, {}, {}, false, true, false}, true, StartupMode::Task), {false, false, false, false, false}),
          "Task mode refuses when Task Scheduler query fails");
    Check(samePlan(PlanStartupChange({exe, {}, cmd, true, true, false}, false), {false, false, false, false, false}),
          "disable refuses when Task Scheduler query fails");
}

} // namespace

int main()
{
    winrt::init_apartment();
    TempDir temp(L"atrium-persist-");
    CheckWriteTextFile(temp.path);
    CheckSettingsCorruptAndFuture(temp.path);
    CheckStageState();
    CheckSystemChromeTaskbar();
    CheckSystemChromeBanners();
    CheckSaveChromeRecord(temp.path);
    CheckUndoLeftoverRetry(temp.path);
    CheckStartupHandoffFromLeftover(temp.path);
    CheckLogonTaskDecisions();
    if (g_failures) {
        std::fprintf(stderr, "%d persistence recovery check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("persistence recovery checks passed");
    return 0;
}
