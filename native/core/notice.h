// A notification as every source delivers it and the cards show it.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace atrium {

struct Notice {
    uint64_t id = 0;
    std::wstring source;     // "test", "atrium", "windows"
    std::wstring app;        // sender's display name
    std::wstring appId;      // sender's AUMID, used to activate it
    std::wstring title, body;
    std::wstring action;     // "uri:<target>", "app:<aumid>" or empty
    uint32_t externalId = 0; // the listener's id (de-duplication)
    std::vector<uint32_t> externalIds; // all merged listener ids (kept until all are withdrawn)
    ULONGLONG time = 0;      // GetTickCount64 of the latest update
    int count = 1;           // merged notices
    bool unread = true;
    DWORD pid = 0;                 // sender process id when known (multi-instance isolation)
    std::wstring trayHostKey;      // hosted tray icon key when originated from a tray flash
};

} // namespace atrium
