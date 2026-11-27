// Launchpad's persisted layout, %LOCALAPPDATA%\Atrium\launchpad.json:
// root order (apps, "#microsoft", "#system", "folder:<n>"), user folders, and hidden apps.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace atrium {

struct LaunchpadFolder {
    std::wstring id;                // "folder:<n>"
    std::wstring name;
    std::vector<std::wstring> apps; // catalog app IDs in order
};

struct LaunchpadLayout {
    std::vector<std::wstring> order;
    std::vector<LaunchpadFolder> folders;
    std::vector<std::wstring> hidden;
    std::wstring NewFolderId() const;
    void Clamp();
};

LaunchpadLayout LoadLaunchpadLayout();
bool SaveLaunchpadLayout(const LaunchpadLayout& layout);
bool ParseLaunchpadLayoutJson(std::wstring_view text, LaunchpadLayout& layout);
std::wstring SerializeLaunchpadLayoutJson(const LaunchpadLayout& layout);

} // namespace atrium
