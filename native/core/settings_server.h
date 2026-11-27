// Named-pipe endpoint for Atrium.Settings (docs/SETTINGS-PROTOCOL.md).
// One client at a time; previews only change memory, commit writes the file,
// and a client that disconnects without committing has its preview reverted.
#pragma once

#include "host_messages.h"
#include "settings.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace atrium {

std::wstring SettingsPipeName();

class SettingsServer {
public:
    SettingsServer(HWND host, const Settings& saved, std::wstring pipeName = {}, std::wstring settingsPath = {},
                   std::function<void(bool)> onAutoUpdateCommitted = {});
    ~SettingsServer();
    SettingsServer(const SettingsServer&) = delete;
    SettingsServer& operator=(const SettingsServer&) = delete;

    Settings Current();
    Settings Saved();
    bool Ready() const { return ready_.load(); }
    // A change made by the core itself (e.g. pinning from the bar's menu):
    // applied to the saved settings and to the current preview, then written.
    bool Modify(const std::function<void(Settings&)>& change);

private:
    void Run();
    void Serve(HANDLE pipe);
    std::string Handle(const std::string& request);
    void SetCurrent(const Settings& s);
    bool Save(const Settings& s, std::wstring* warning = nullptr, std::wstring* error = nullptr) const;
    std::wstring EffectiveSettingsPath() const;

    HWND host_;
    std::wstring pipeName_;
    std::wstring settingsPath_;
    std::wstring instanceId_;
    // txn_: one change at a time, from reading the settings to writing the
    // file (the pipe's thread and the UI thread's Modify); lock_ guards the fields.
    std::mutex txn_, lock_;
    Settings saved_, current_;
    std::function<void(bool)> onAutoUpdateCommitted_;
    uint64_t revision_ = 1;
    HANDLE quit_;
    std::atomic_bool ready_ = false;
    std::thread thread_;
};

} // namespace atrium
