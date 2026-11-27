// The bar's own entries that are drawn rather than read from an app: the start logo, the
// Launchpad's page of apps, the now-playing bars and the folder stack. Each fills `r`, the entry's icon
// square in the window's coordinates, upright on every edge. Render thread.
#pragma once

#include "music_bars.h"

#include <d2d1_3.h>
#include <winrt/base.h>

#include <string>

namespace atrium {

class BarExtrasPainter {
public:
    // The entry `key` if it is one of these (#start, #launchpad, #music, #stacks), faded to
    // `opacity`; false for any other key (nothing drawn).
    bool Draw(const std::wstring& key, ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, float opacity,
              const MusicBars& bars, bool folderStackColor);
    // The Windows logo, as four panes in Windows 11's light-to-deep blue.
    void DrawLogo(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, float opacity);
    // Launchpad: nine small apps, three by three, in outline, in the bar's ink.
    static void DrawLaunchpad(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink);
    // Now playing: five round-ended bars in the bar's ink, as tall as `bars` has them.
    static void DrawMusic(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, const MusicBars& bars, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink);
    // Folder stacks: the colorful folder (a macOS-blue fill as large as the apps). The ink outline
    // is the settings' own folder glyph (U+E8B7), drawn by the bar's glyph path.
    void DrawStacks(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink);

private:
    winrt::com_ptr<ID2D1LinearGradientBrush> logoBlue_; // the logo's panes and their lit edge
    winrt::com_ptr<ID2D1SolidColorBrush> logoSheen_;
    winrt::com_ptr<ID2D1LinearGradientBrush> folderBlue_; // the stack folder's fill
};

} // namespace atrium
