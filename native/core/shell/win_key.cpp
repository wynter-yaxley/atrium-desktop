#include "shell/win_key.h"

#include "log.h"
#include "shell/keyboard_hook.h"
#include "shell/process_integrity.h"

namespace atrium {

namespace {

INPUT Key(DWORD vk, DWORD scan, bool extended)
{
    INPUT i{};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = WORD(vk);
    i.ki.wScan = WORD(scan);
    i.ki.dwFlags = extended ? KEYEVENTF_EXTENDEDKEY : 0;
    i.ki.dwExtraInfo = kAtriumSyntheticKey;
    return i;
}

} // namespace

WinKey::WinKey(HWND notify, UINT msg, bool alone, bool numbers)
    : notify_(notify),
      msg_(msg),
      alone_(alone),
      numbers_(numbers),
      keyToken_(KeyboardHook::Instance().Add(&WinKey::OnKey, this))
{
    Log(L"winkey: watching the Windows key (%s%s%s)", alone_ ? L"start menu" : L"", alone_ && numbers_ ? L", " : L"", numbers_ ? L"Win + digits" : L"");
}

WinKey::~WinKey() { KeyboardHook::Instance().Remove(keyToken_); }

void WinKey::SetStartMenuOpen(bool open) { startMenuOpen_.store(open, std::memory_order_release); }

bool WinKey::OnKey(void* context, DWORD vk, DWORD scan, bool extended, bool down, bool /*injected*/, ULONG_PTR extraInfo)
{
    WinKey* self = static_cast<WinKey*>(context);
    if (!self || extraInfo == kAtriumSyntheticKey)
        return false; // Atrium's own replay: let it through
    WinKeyState& s = self->state_;
    const bool win = vk == VK_LWIN || vk == VK_RWIN;
    const bool winAsync = s.held && s.passed && (vk != s.key || down) && (GetAsyncKeyState(int(s.key)) & 0x8000) != 0;
    const ULONGLONG now = GetTickCount64();
    const bool willReset = s.held && s.passed && (vk != s.key || down) && !winAsync;
    const bool freshWin = win && down && (!s.held || willReset || (!s.passed && now - s.lastDownMs > 2000));
    const bool startOpen = self->startMenuOpen_.load(std::memory_order_acquire);
    const bool elevated = (freshWin && !startOpen) ? WindowHasHigherIntegrity(GetForegroundWindow()) : false;
    const bool mod =
        !win && down && s.held && !s.passed && ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) != 0;
    const WinKeyAction action = StepWinKey(s, vk, scan, extended, down, now, winAsync, elevated, mod, self->alone_, self->numbers_);
    if (action.notify)
        PostMessageW(self->notify_, self->msg_, action.notifyParam, 0);
    if (action.replayAlone) {
        INPUT keys[2] = {Key(action.winVk, action.winScan, action.winExtended), Key(action.winVk, action.winScan, action.winExtended)};
        keys[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, keys, sizeof(INPUT));
    } else if (action.replayCombo) {
        INPUT keys[2] = {Key(action.winVk, action.winScan, action.winExtended), Key(vk, scan, extended)};
        SendInput(2, keys, sizeof(INPUT));
    }
    return action.swallow;
}

} // namespace atrium
