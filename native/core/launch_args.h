// Command-line token parsing for Atrium.exe and launch argument formatting for Atrium.Settings.exe.
#pragma once

#include <windows.h>

#include <cerrno>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace atrium {

struct CoreLaunchOptions {
    std::wstring readyEvent;
    bool uninstall = false;
    bool exitRequested = false;
    bool debugCapturable = false;
    bool replace = false;
    bool guard = false;
    DWORD guardPid = 0;
};

inline CoreLaunchOptions ParseCoreLaunchOptions(const std::vector<std::wstring>& args)
{
    CoreLaunchOptions options;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        if (arg == L"--data-dir") {
            if (i + 1 < args.size())
                ++i;
            continue;
        }
        if (arg == L"--ready-event") {
            if (i + 1 < args.size()) {
                const std::wstring& value = args[++i];
                if (std::wstring_view(value).starts_with(L"Local\\Atrium.Update.Core."))
                    options.readyEvent = value;
            }
            continue;
        }
        if (arg == L"--guard") {
            options.guard = true;
            options.guardPid = 0;
            if (i + 1 < args.size()) {
                const wchar_t* s = args[i + 1].c_str();
                if (s[0] >= L'0' && s[0] <= L'9') {
                    wchar_t* end = nullptr;
                    errno = 0;
                    const unsigned long val = std::wcstoul(s, &end, 10);
                    if (errno == 0 && end != s && *end == L'\0' && val > 0 && val <= (std::numeric_limits<DWORD>::max)()) {
                        options.guardPid = static_cast<DWORD>(val);
                        ++i;
                    }
                }
            }
            continue;
        }
        if (arg == L"--exit")
            options.exitRequested = true;
        else if (arg == L"--uninstall" && args.size() == 2)
            options.uninstall = true;
        else if (arg == L"--debug-capturable")
            options.debugCapturable = true;
        else if (arg == L"--replace")
            options.replace = true;
    }
    return options;
}

inline std::wstring FormatSettingsLaunchArgs(const wchar_t* page, const std::wstring& devDataDirArgs)
{
    std::wstring_view extra(devDataDirArgs);
    while (!extra.empty() && extra.front() == L' ')
        extra.remove_prefix(1);
    while (!extra.empty() && extra.back() == L' ')
        extra.remove_suffix(1);
    std::wstring result;
    if (page && *page) {
        result = L"--page ";
        result += page;
    }
    if (!extra.empty()) {
        if (!result.empty())
            result += L' ';
        result += extra;
    }
    return result;
}

} // namespace atrium
