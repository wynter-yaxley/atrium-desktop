#include "shell/media_players.h"

#include "shell/shell_launch.h"

#include <tlhelp32.h>

#include <algorithm>
#include <array>

namespace atrium {

namespace {

// Ctrl+Alt+key, as a press of the keys (the player's global shortcut takes it from anywhere).
bool SendChord(WORD key)
{
    std::array<INPUT, 6> in{};
    const std::array<WORD, 3> keys{VK_CONTROL, VK_MENU, key};
    for (size_t i = 0; i < 3; ++i) {
        in[i].type = in[5 - i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = in[5 - i].ki.wVk = keys[i];
        in[5 - i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    return SendInput(UINT(in.size()), in.data(), sizeof(INPUT)) == in.size();
}

// The full path of the running process `exe`; empty if none runs.
std::wstring RunningImage(const wchar_t* exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return {};
    std::wstring path;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && path.empty(); ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe) != 0)
            continue;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD size = DWORD(std::size(buf));
            if (QueryFullProcessImageNameW(process, 0, buf, &size))
                path.assign(buf, size);
            CloseHandle(process);
        }
    }
    CloseHandle(snap);
    return path;
}

} // namespace

bool LikeTrack(MediaPlayer p)
{
    switch (p) {
    case MediaPlayer::Netease: return SendChord('L');
    default: return false;
    }
}

bool ToggleLyrics(MediaPlayer p)
{
    switch (p) {
    case MediaPlayer::Netease: return SendChord('D');
    case MediaPlayer::QqMusic: return SendChord('W');
    default: return false;
    }
}

bool QqMusicSeek(int64_t positionUs)
{
    const std::wstring exe = RunningImage(L"QQMusic.exe");
    if (exe.empty())
        return false;
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? std::wstring{} : exe.substr(0, slash);
    LaunchRequest req;
    req.target = exe;
    req.parameters = L"/seekto " + std::to_wstring(std::max<int64_t>(0, (positionUs + 500000) / 1000000));
    req.workingDirectory = dir;
    req.kind = LaunchTargetKind::Executable;
    req.privilege = LaunchPrivilege::ShellDefault;
    return ShellLaunch(req).outcome == LaunchOutcome::Succeeded;
}

} // namespace atrium
