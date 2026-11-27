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

bool WinKey::OnKey(void* context, DWORD vk, DWORD scan, bool extended, bool down, bool injected, ULONG_PTR extraInfo)
{
    auto* self = static_cast<WinKey*>(context);
    if (!self || extraInfo == kAtriumSyntheticKey || (injected && IsWindowsKey(vk)))
        return false;
    WinKeyState& state = self->state_;
    const bool winAsync = state.key != 0 && (vk != state.key || down) && (GetAsyncKeyState(int(state.key)) & 0x8000) != 0;
    const bool freshWin = IsWindowsKey(vk) && down && (!state.key || LostWindowsKey(state, vk, down, winAsync));
    const bool startOpen = self->startMenuOpen_.load(std::memory_order_acquire);
    const bool elevated = freshWin && !startOpen && WindowHasHigherIntegrity(GetForegroundWindow());
    const bool modified = (freshWin || (down && state.key && vk >= '0' && vk <= '9')) &&
                          ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) != 0;
    const WinKeyAction action = StepWinKey(state, vk, down, winAsync, elevated, modified, injected, self->alone_, self->numbers_);
    if (action.maskRelease) {
        // Keep the placeholder and release in a single batch. If Windows blocks
        // SendInput (for example after a focus change), pass the physical release.
        constexpr DWORD kMenuMaskKey = 0xE8;
        INPUT keys[] = {Key(kMenuMaskKey, 0, false), Key(kMenuMaskKey, 0, false), Key(vk, scan, extended)};
        keys[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        keys[2].ki.dwFlags |= KEYEVENTF_KEYUP;
        if (SendInput(3, keys, sizeof(INPUT)) != 3)
            return false;
    }
    if (action.notify)
        PostMessageW(self->notify_, self->msg_, action.notifyParam, 0);
    return action.swallow || action.maskRelease;
}

} // namespace atrium
