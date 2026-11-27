// Atrium: the resident process. Owns the glass bar, its popups, the desktop
// widgets, the tray icon and the settings pipe. Explorer's taskbar stays
// unless the user hides it (setting hideTaskbar, shell/system_chrome.h; D37).

#include "app_handlers.h"
#include "app_launch.h"
#include "app_runtime.h"
#include "crash_dump.h"
#include "host_messages.h"
#include "launch_args.h"
#include "localization.h"
#include "log.h"
#include "render/capture.h"
#include "resource.h"
#include "settings.h"
#include "settings_server.h"
#include "shell/app_registration.h"
#include "shell/system_chrome.h"
#include "shell/task_model.h"
#include "shell/tray_model.h"
#include "shell/update_checker.h"

#include <windows.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>

#include <winrt/Windows.Foundation.h>

#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace atrium;

namespace {

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_app.tasks && g_app.tasks->HandleMessage(m, w, l))
        return 0;
    if (m == g_app.taskbarCreated && m != 0) {
        HandleTaskbarCreated();
        return 0;
    }
    LRESULT result = 0;
    if (HandleUiAndStatusMessage(h, m, w, l, result))
        return result;
    if (HandleSystemAndLifecycleMessage(h, m, w, l, result))
        return result;
    return DefWindowProcW(h, m, w, l);
}

void CreateHostWindow(HINSTANCE instance)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = kHostClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ATRIUM));
    RegisterClassExW(&wc);
    g_app.host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Atrium", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g_app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_app.host, g_app.taskbarCreated, MSGFLT_ALLOW, nullptr);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetLanguage(L"auto");
    if (PWSTR profile = nullptr; SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) {
        SetCurrentDirectoryW(profile);
        CoTaskMemFree(profile);
    }
    int argumentCount = 0;
    auto* startupArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::vector<std::wstring> args(startupArguments, startupArguments + (startupArguments ? argumentCount : 0));
    LocalFree(static_cast<HLOCAL>(startupArguments));
    const atrium::CoreLaunchOptions launch = atrium::ParseCoreLaunchOptions(args);
    g_app.readyEvent = launch.readyEvent;
    if (const std::optional<int> code = HandleLaunchCommand(launch))
        return *code;
    int instanceExit = 0;
    HANDLE mutex = AcquireInstanceMutex(instanceExit, launch.replace);
    if (!mutex)
        return instanceExit;

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    SetCurrentProcessExplicitAppUserModelID(kAppId);
    g_app.instance = instance;
    Log(L"core: starting");
    if (LowIntegrity())
        Log(L"core: running at low integrity (Atrium.exe has a Low label?): settings cannot be saved, the Windows key is left alone");
    RegisterApplication(ExeDir());
    InstallCrashHandlers(&SystemChrome::EmergencyRestore);
    g_app.chrome = std::make_unique<SystemChrome>();

    CreateHostWindow(instance);

    Settings settings = LoadSettings();
    SetLanguage(settings.language);
    std::thread startupThread = StartLogonUpdate(settings);
    if (!settings.pins) {
        settings.pins = TaskModel::ImportWindowsPins();
        Log(L"core: imported %zu Windows taskbar pins", settings.pins->size());
    }
    g_app.capturable = launch.debugCapturable;
    SetRemoteVisible(settings.remoteVisible);
    if (settings.remoteVisible)
        Log(L"core: remote-control mode on: Atrium shows in every capture, the glass shows the wallpaper");

    InitializeSubsystems(instance, settings);
    AddTrayIcon();
    UpdateStatus();
    g_app.brightnessNotify = RegisterPowerSettingNotification(g_app.host, &kBrightnessSetting, DEVICE_NOTIFY_WINDOW_HANDLE);
    DEV_BROADCAST_DEVICEINTERFACE_W volumeInterface{sizeof(DEV_BROADCAST_DEVICEINTERFACE_W)};
    volumeInterface.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    volumeInterface.dbcc_classguid = kVolumeInterface;
    g_app.usbNotify = RegisterDeviceNotificationW(g_app.host, &volumeInterface, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!g_app.usbNotify)
        Log(L"usb: could not register for volume notifications (%lu)", GetLastError());
    SetTimer(g_app.host, kStatusTimer, kStatusMs, nullptr);
    g_app.tray->Refresh();
    if (!g_app.readyEvent.empty()) {
        SetTimer(g_app.host, kReadyTimer, 50, nullptr);
        PostMessageW(g_app.host, WM_ATRIUM_SERVER_READY, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ShutdownApp(mutex, startupThread);
    return 0;
}
