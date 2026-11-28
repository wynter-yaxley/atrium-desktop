// Shell item and executable icon loading: shortcuts, versioned app/ launchers,
// Atrium's own executables and HICON-to-Image conversion.
#pragma once

#include "icon_image.h"

#include <windows.h>

#include <string>
#include <vector>

struct IWICImagingFactory;

namespace atrium {

// Icon of a shell item (a path or "shell:AppsFolder\\<id>") through
// IShellItemImageFactory, square, premultiplied. Blocking; call it off the UI
// thread. COM must be initialised on the calling thread.
// thumbnail: the picture itself (an image file) rather than its icon.
bool LoadShellImage(const std::wstring& parsingName, int sizePx, IconImage& out, bool thumbnail = false);
// Loads an icon from a path and resource index (e.g. from PKEY_AppUserModel_RelaunchIconResource).
bool LoadIconResourceImage(const std::wstring& path, int index, int sizePx, IconImage& out);
// Atrium's own programs (Atrium.exe, Atrium.Settings.exe): their icon read from the file
// itself, not from the shell's icon cache or Explorer's tray snapshot, which keep the icon an
// earlier build had at the same path. False for any other program.
bool LoadAtriumIcon(const std::wstring& exePath, int sizePx, IconImage& out);
// Resolves a shortcut and a versioned client's app/ launcher without starting it.
std::wstring AppExecutable(const std::wstring& path);
// An icon's picture, premultiplied BGRA.
bool HIconToImage(HICON icon, IconImage& out);
// Scales a premultiplied image into a size x size square (aspect kept, centred).
IconImage FitToSquare(const IconImage& src, int size);
// Crops a premultiplied image to a centred square and scales it to size x size (cover).
IconImage CropToSquare(const IconImage& src, int size);
// Pull-decodes encoded image bytes into a centred square targetSize x targetSize premultiplied BGRA IconImage via WIC.
bool DecodeWicSquareImage(IWICImagingFactory* wic, std::vector<uint8_t>& bytes, int targetSize, IconImage& out);
// True when `path` is a PE/icon file with at least one embedded icon resource.
bool HasEmbeddedExeIcon(const std::wstring& path);
// True when `img` is a square icon whose 4 corners and outer perimeter are opaque (alpha >= 240).
bool IsFullBleedSquareIcon(const IconImage& img);
// Applies a G3 continuous-corner alpha mask when `img` is a full-bleed square icon; leaves other icons untouched.
IconImage ApplySquircleMaskIfFullBleed(const IconImage& img);
inline bool IsLink(const std::wstring& path) { return path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".lnk") == 0; }

} // namespace atrium
