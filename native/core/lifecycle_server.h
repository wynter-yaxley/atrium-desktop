// Short-lived same-user named-pipe endpoint for Atrium.Lifecycle (ping, open-settings, quit, restart-standard).
#pragma once

#include "shell/process_integrity.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace atrium {

std::wstring LifecyclePipeName();

struct LifecycleCallbacks {
    std::function<void(const std::wstring& page)> onOpenSettings;
    std::function<void()> onQuit;
    std::function<bool()> onRestartStandard;
    std::function<PrivilegeMode()> privilegeModeProvider;
};

struct LifecycleReply {
    bool ok = false;
    bool pong = false;
    DWORD pid = 0;
    std::wstring privilegeMode;
    bool canDeElevate = false;
    std::wstring status;
    std::wstring error;
};

class LifecycleServer {
public:
    explicit LifecycleServer(HWND host, std::wstring pipeName = {}, LifecycleCallbacks callbacks = {});
    ~LifecycleServer();
    LifecycleServer(const LifecycleServer&) = delete;
    LifecycleServer& operator=(const LifecycleServer&) = delete;

    bool Ready() const { return ready_.load(); }
    const std::wstring& PipeName() const { return pipeName_; }

private:
    void Run();
    void Serve(HANDLE pipe);
    bool VerifyClientUser(HANDLE pipe) const;
    std::string Handle(const std::string& request);

    HWND host_;
    std::wstring pipeName_;
    LifecycleCallbacks callbacks_;
    HANDLE quit_;
    std::atomic_bool ready_ = false;
    std::thread thread_;
};

bool SendLifecycleCommand(const std::wstring& cmd, const std::wstring& page = {}, LifecycleReply* replyOut = nullptr, const std::wstring& pipeName = {},
                          DWORD timeoutMs = 2000);

} // namespace atrium
