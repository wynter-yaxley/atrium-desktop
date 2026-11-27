// Host window message-group dispatchers and messenger/shell event handlers.
#pragma once

#include "shell/messengers.h"

#include <windows.h>

#include <string>

namespace atrium {

bool MessengerOn(Messenger m);
void MessengerFlashed(const std::wstring& program, DWORD pid = 0, const std::wstring& hostKey = {});
void HandleTaskbarCreated();
bool HandleUiAndStatusMessage(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT& result);
bool HandleSystemAndLifecycleMessage(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT& result);

} // namespace atrium
