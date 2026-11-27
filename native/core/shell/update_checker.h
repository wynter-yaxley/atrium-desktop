// Periodic release checks belong to the resident process, not the settings window.
#pragma once

#include "notice.h"

#include <windows.h>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <string>

namespace atrium {

class UpdateChecker {
public:
    UpdateChecker(HWND notify, UINT message, bool autoCheck = true);
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;
    // Enables or disables automatic checks (startup, power-resume, periodic timer).
    void SetAutoCheck(bool on);
    // Triggers an immediate release check regardless of SetAutoCheck(false).
    void CheckNow();
    // Wakes the background checker on system resume when automatic checks are enabled.
    void OnPowerResume();
    // UI thread; completion or next startup. `show` gets the update card, or nothing to clear it.
    void NotifyPrepared(const std::function<void(std::optional<Notice> card, bool silent)>& show);

private:
    void Run();
    HWND notify_;
    UINT message_;
    HANDLE stop_;
    HANDLE wake_;
    std::wstring cache_;
    std::mutex mutex_;
    bool autoCheck_;
    bool manualRequested_ = false;
    std::thread worker_;
};

} // namespace atrium
