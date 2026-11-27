// Command-line token parsing for Atrium.exe and launch argument formatting for Atrium.Settings.exe.
#pragma once

#include <windows.h>

#include <cerrno>
#include <cstdint>
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
    uint64_t guardGeneration = 0;
    bool desktopAgent = false;
    DWORD desktopAgentCorePid = 0;
    uint64_t desktopAgentGeneration = 0;
};

namespace detail {

inline bool TryParsePositiveDword(const std::wstring& token, DWORD& out)
{
    const wchar_t* s = token.c_str();
    if (s[0] < L'0' || s[0] > L'9')
        return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long val = std::wcstoul(s, &end, 10);
    if (errno == 0 && end != s && *end == L'\0' && val > 0 && val <= (std::numeric_limits<DWORD>::max)()) {
        out = static_cast<DWORD>(val);
        return true;
    }
    return false;
}

inline bool TryParseUint64(const std::wstring& token, uint64_t& out)
{
    const wchar_t* s = token.c_str();
    if (s[0] < L'0' || s[0] > L'9')
        return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long val = std::wcstoull(s, &end, 10);
    if (errno == 0 && end != s && *end == L'\0') {
        out = static_cast<uint64_t>(val);
        return true;
    }
    return false;
}

inline bool TryParseValueOption(const std::vector<std::wstring>& args, size_t& i, CoreLaunchOptions& options)
{
    const std::wstring& arg = args[i];
    if (arg == L"--data-dir") {
        if (i + 1 < args.size())
            ++i;
        return true;
    }
    if (arg == L"--ready-event") {
        if (i + 1 < args.size()) {
            const std::wstring& value = args[++i];
            if (std::wstring_view(value).starts_with(L"Local\\Atrium.Update.Core."))
                options.readyEvent = value;
        }
        return true;
    }
    if (arg == L"--guard" || arg == L"--desktop-agent") {
        const bool isGuard = (arg == L"--guard");
        (isGuard ? options.guard : options.desktopAgent) = true;
        DWORD& pidOut = isGuard ? options.guardPid : options.desktopAgentCorePid;
        pidOut = 0;
        if (i + 1 < args.size() && TryParsePositiveDword(args[i + 1], pidOut))
            ++i;
        return true;
    }
    if (arg == L"--guard-gen" || arg == L"--agent-gen") {
        uint64_t& genOut = (arg == L"--guard-gen") ? options.guardGeneration : options.desktopAgentGeneration;
        genOut = 0;
        if (i + 1 < args.size() && TryParseUint64(args[i + 1], genOut))
            ++i;
        return true;
    }
    return false;
}

} // namespace detail

inline CoreLaunchOptions ParseCoreLaunchOptions(const std::vector<std::wstring>& args)
{
    CoreLaunchOptions options;
    for (size_t i = 1; i < args.size(); ++i) {
        if (detail::TryParseValueOption(args, i, options))
            continue;
        const std::wstring& arg = args[i];
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
