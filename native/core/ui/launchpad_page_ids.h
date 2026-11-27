// Expands Launchpad page entries (including any folder preview) into catalog icon IDs.
#pragma once

#include "shell/app_catalog.h"
#include "ui/launchpad_model.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace atrium {

template<class LookupFn>
inline std::vector<std::wstring> ExpandLaunchpadPageIds(const std::vector<CatalogApp>& shown, int page, int perPage, LookupFn&& folderApps)
{
    std::vector<std::wstring> ids;
    const LaunchpadSlice slice = PageSlice(shown.size(), page, perPage);
    ids.reserve(slice.end - slice.begin);
    for (size_t i = slice.begin; i < slice.end; ++i) {
        if (const std::vector<CatalogApp>* items = folderApps(shown[i].id)) {
            for (size_t j = 0; j < std::min<size_t>(9, items->size()); ++j)
                ids.push_back((*items)[j].id);
        } else {
            ids.push_back(shown[i].id);
        }
    }
    return ids;
}

inline std::vector<std::wstring> ExpandLaunchpadPageIds(const std::vector<CatalogApp>& shown, const std::vector<CatalogApp>& system, int page, int perPage,
                                                        std::wstring_view folderId, const std::vector<CatalogApp>& microsoft = {},
                                                        std::wstring_view microsoftId = {})
{
    return ExpandLaunchpadPageIds(shown, page, perPage, [&](std::wstring_view id) -> const std::vector<CatalogApp>* {
        if (id == folderId)
            return &system;
        if (!microsoftId.empty() && id == microsoftId)
            return &microsoft;
        return nullptr;
    });
}

} // namespace atrium
