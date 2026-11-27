// Bar stacking policy: decides which foreign topmost windows the bar stays behind.
#pragma once

#include <windows.h>

#include <string_view>

namespace atrium {

// True when a foreign topmost window overlapping the bar is a menu, tooltip,
// shell surface, or click-through overlay that the bar must stay behind.
bool ShouldIgnoreTopmostCover(std::wstring_view cls, LONG_PTR style, LONG_PTR exStyle, bool isForeground, bool ownerIsForeground);

// True when a window's visual bounds overlap the bar's resting overlap zone.
bool WindowOverlapsBar(const RECT& windowBounds, const RECT& barZone);

// True when a topmost non-click-through horizontal bar overlaps `barZone` in meeting toolbar dimensions.
bool IsMeetingShareToolbarBounds(LONG_PTR exStyle, const RECT& bounds, const RECT& barZone, float scale);

// True when an executable stem belongs to a meeting/screen-share app with a floating share toolbar.
bool IsMeetingShareExeStem(std::wstring_view exeStem);

} // namespace atrium
