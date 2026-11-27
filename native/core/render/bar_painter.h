// Draws the bar's content over the glass with Direct2D: app icons with soft
// shadows, the dock-style magnification wave under the pointer, launch
// bounces, running dots, a separator and the clock. The base geometry arrives
// precomputed in BarScene (window pixels); magnification only displaces and
// scales it, so hit testing on the base layout stays exact (D25).
// On any edge the scene is laid out as at the bottom (BarEdge): EdgeFrame{edge, window}
// places each finished rect on the real window, upright.
#pragma once

#include "../bar_edge.h"
#include "../icon_image.h"
#include "../status.h"
#include "bar_extras_painter.h"
#include "glass_panel.h"
#include "hover_wave.h"
#include "ime_glyph.h"

#include <d2d1_3.h>
#include <dwrite_3.h>
#include <winrt/base.h>

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace atrium {

struct Readback;
struct Settings;

struct SceneItem {
    std::wstring key;
    D2D1_RECT_F icon{};        // icon square, window px
    D2D1_RECT_F plate{};       // hover/active plate around the icon, same centre
    std::shared_ptr<const IconImage> image; // may be null while loading
    uint64_t imageId = 0;      // identity of `image` for the bitmap cache
    bool running = false;
    bool active = false;
    bool attention = false;
    std::wstring glyph;        // Segoe Fluent Icons glyph instead of an image (bar entries)
    int badge = 0;             // > 0: count badge on the icon (unread notifications)
    double bounceMs = 0;       // ClockMs() when a launch bounce started; 0 = none
};

struct BarScene {
    BarEdge edge = BarEdge::Bottom; // rects and points below are in the layout space of the bottom edge
    D2D1_RECT_F bar{};         // the glass shape inside the window (its width eases to this, D27)
    float windowW = 0;         // the window's width: a new one means new coordinates (nothing slides)
    float windowH = 0;         // windowW x windowH: the layout space; on a side edge the window is windowH wide
    bool tucked = false;       // auto-hide: the bar slides down off the screen
    float scale = 1;
    std::vector<SceneItem> items;  // apps only (drag and drop works on these)
    std::vector<SceneItem> extras; // Atrium logo (#start), overflow (#more), tray (#tray), status (#status), notifications (#notify)
    SystemStatus status;           // drawn by the #status entry
    bool statusIcons = false;      // as the Windows taskbar's three icons rather than the ring (statusStyle)
    // The folder stack entry (#stacks): a filled macOS-blue folder (true) or one outline in the ink.
    bool folderStackColor = true;
    // The input method entry (#ime): the foreground window's input (中/あ/가, a/A), fading in over
    // the glyph before it since imeMs (ClockMs).
    std::wstring imeGlyph, imePrevGlyph;
    std::shared_ptr<const MusicFeed> music; // the now-playing bars' level (Bar::SetMusic); null without the entry
    double imeMs = 0;
    float separatorX = -1;     // < 0: none
    D2D1_RECT_F clock{};       // empty: no clock
    float timePx = 0, datePx = 0;
    float plateRadius = 0;     // concentric with the bar's corners
    std::wstring hoverKey;     // item (or L"#clock") under the pointer
    std::wstring pressedKey;
    std::wstring dragKey;      // item being dragged: follows the pointer, drawn on top
    // Magnification: icons near the pointer grow up to `magnify` times, over a
    // wave `magnifyRange` px wide on each side; the glass stretches and rises with it.
    float magnify = 1;         // 1 = off
    float magnifyRange = 1;
    float magnifyLift = 0;     // glass rise at full magnification, px
    float pointerX = -1;       // window px; < 0 = the pointer is not in the input region
    float pointerY = -1;
    bool earlyRise = false, earlyMagnify = false; // above the glass, these channels obey the early-response state
    // The pointer just above the bar (settings riseNear and magnifyNear): where it is
    // along the bar, whether the glass rises to it and whether the icons magnify.
    float nearX = -1;          // window px; < 0 = not above the bar
    bool nearRise = false;
    bool nearMagnify = false;
    // An app asking for attention (SceneItem::attention): its plate lit (attentionLight), its
    // icon hopping a few times (attentionBounce).
    bool alertLight = true;
    bool alertBounce = false;
    // Press ripple in the glass (window px), started at ClockMs() `rippleMs`.
    float rippleX = 0, rippleY = 0;
    double rippleMs = 0;
};

// What the glass needs each frame: the stretched shape, local rise and ripple.
struct BarFrame {
    D2D1_RECT_F shape{};
    float waveX = 0, waveWidth = 0, waveHeight = 0; // local glass rise under the pointer
    float rippleX = 0, rippleY = 0, rippleR = 0, rippleA = 0;
    float slideY = 0;    // auto-hide: how far the bar is down (px; the content and the glass move together)
};

// Ink colour per region: near-black on light glass, white on dark glass; the
// amount of dark ink, 0..1, in between while it fades (InkFade).
struct Ink {
    float darkClock = 0;
    float darkApps = 0;
};

struct BarAnimation {
    bool glass = false;
    bool body = false;
    bool music = false;
    bool Any() const { return glass || body || music; }
    bool MusicOnly() const { return music && !glass && !body; }
};

class BarPainter {
public:
    BarPainter(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite);
    // Auto-hide: going down eases in and out; coming up decelerates with a
    // slight overshoot.
    static constexpr double kSlideOutMs = 300, kSlideInMs = 420;

    // Advances animations to `nowMs` (ClockMs); reports which channels are still moving.
    BarAnimation Animate(const BarScene& scene, double nowMs);
    // Each running dot follows only the backdrop at its displayed position.
    bool UpdateDotInk(const BarScene& scene, const Readback& backdrop, const Settings& settings, double nowMs);
    const BarFrame& Frame() const { return frame_; }
    // Draws the content. It is rendered into a cached layer and only redrawn
    // when `contentKey` changes or a body/music animation runs; glass-only
    // presents just composite the cached layer.
    void Draw(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date, const std::wstring& contentKey,
              BarAnimation anim, bool forceBody = false);

private:
    void DrawContent(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date);
    ID2D1Bitmap1* BitmapFor(ID2D1DeviceContext2* d2d, const SceneItem& item);
    IDWriteTextFormat* Format(float px, DWRITE_FONT_WEIGHT weight, const wchar_t* family = nullptr); // nullptr: the interface face
    bool AnimateSlide(const BarScene& scene, double nowMs);
    bool AnimateMusic(const BarScene& scene, double dt);
    void StepWave(const BarScene& scene, double dt, BarAnimation& anim);
    bool StepSingleItemMotion(const BarScene& scene, const std::wstring& key, const SceneItem* item, double nowMs, double dt, bool snap);
    bool StepItemMotions(const BarScene& scene, double nowMs, double dt, bool snap);
    bool PruneLeavingItems(const BarScene& scene, double dt, bool snap);
    bool StepRipple(const BarScene& scene, double nowMs);
    bool TryDrawMusicClip(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& contentKey, ID2D1Image* target, D2D1_SIZE_U size);
    // The status icon in `r` (icon height): a ring open at the bottom with the
    // battery's level (the volume without a battery) round the Wi-Fi fan (or
    // wired / offline); while the volume or the brightness changes the ring
    // shows it and a speaker or a sun takes the fan's place (StatusMotion).
    void DrawStatus(ID2D1DeviceContext2* d2d, const SystemStatus& st, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, bool darkInk);
    // The same as the Windows taskbar shows it: the network, the speaker and the battery side by
    // side in `r` (its height the icons'), redrawn in the bar's ink.
    void DrawStatusIcons(ID2D1DeviceContext2* d2d, const SystemStatus& st, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, bool darkInk);
    void DrawStatusSpeakerIcon(ID2D1DeviceContext2* d2d, ID2D1Factory* factory, ID2D1StrokeStyle* round, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, float l,
                               float mid, float h, float stroke, const SystemStatus& st);
    void DrawStatusBatteryIcon(ID2D1DeviceContext2* d2d, ID2D1Factory* factory, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, bool darkInk, float bx0,
                               float mid, float h, float stroke, const SystemStatus& st);
    float MapX(float u) const; // base x -> displayed x under the magnification

    struct Motion {
        float hover = 0;   // 0..1
        float press = 0;   // 0..1
        float appear = 0;  // 0..1, fades new items in
        float lift = 0;    // 0..1, while dragged
        float x = 0;       // displayed plate centre; slides to the layout position
        bool placed = false;
        bool inScene = false;
        float hop = 0;       // current bounce height, px
        float dot = 0;       // 0..1, the running dot
        float alert = 0;     // 0..1, the app asks for attention (its plate lit, as the system taskbar's button)
        double alertSince = -1; // when it started asking (the first seconds pulse)
        bool darkDot = false;
        InkFade dotInk;
        bool leaving = false; // the app went away: `ghost` shrinks and fades out where it was
        SceneItem ghost;
    };
    D2D1_ELLIPSE RunningDot(const BarScene& scene, const SceneItem& item, const Motion& motion) const;
    void DrawItem(ID2D1DeviceContext2* d2d, const BarScene& scene, const SceneItem& it, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, D2D1_COLOR_F wash);
    winrt::com_ptr<IDWriteFactory3> dwrite_;
    winrt::com_ptr<ID2D1SolidColorBrush> contentBrush_;
    winrt::com_ptr<ID2D1Effect> shadow_;
    BarExtrasPainter extras_;
    ImeGlyphCache imeCache_;
    MusicBars music_;
    // Device-independent geometries scoped to the painter and invalidated if the factory changes.
    struct StatusGeometryCache {
        struct ArcEntry {
            winrt::com_ptr<ID2D1PathGeometry> path;
            float sweep = -1;
        };
        struct BatteryEntry {
            float h = 0, stroke = 0;
            int pct = -1;
            winrt::com_ptr<ID2D1PathGeometry> cut;
            winrt::com_ptr<ID2D1PathGeometry> rest;
            uint64_t lastUsed = 0;
        };
        winrt::com_ptr<ID2D1Factory> factory;
        ArcEntry ringTrack, ringLevel;
        winrt::com_ptr<ID2D1PathGeometry> chargingBolt;
        winrt::com_ptr<ID2D1PathGeometry> speakerCone;
        winrt::com_ptr<ID2D1PathGeometry> speakerWave;
        std::vector<BatteryEntry> batteryCharging;
        uint64_t stamp = 0;

        void SyncFactory(ID2D1Factory* f)
        {
            if (factory.get() == f)
                return;
            ringTrack = {};
            ringLevel = {};
            chargingBolt = nullptr;
            speakerCone = nullptr;
            speakerWave = nullptr;
            batteryCharging.clear();
            factory.copy_from(f);
        }
    } statusGeo_;
    std::map<std::wstring, Motion> motion_;
    std::map<uint64_t, winrt::com_ptr<ID2D1Bitmap1>> bitmaps_;
    std::vector<uint64_t> usedBitmaps_; // scratch storage for one DrawContent
    std::map<std::tuple<int, int, std::wstring>, winrt::com_ptr<IDWriteTextFormat>> formats_;
    double lastMs_ = 0;
    BarEdge lastEdge_ = BarEdge::Bottom, layerEdge_ = BarEdge::Bottom;
    float lastWindowW_ = 0, lastTop_ = 0, lastBottom_ = 0;
    // Displayed bar edges, separator and clock centre: they slide when the bar's width changes.
    float barL_ = 0, barR_ = 0, sepX_ = -1, clockX_ = 0;
    std::map<std::wstring, SceneItem> lastApps_; // the apps of the previous frame (for their exit)
    HoverWave wave_;
    // The status ring: its fill springs to the battery, the volume or the
    // brightness. A changed level shows for a while (the latest change wins);
    // `volumeMix` and `brightMix` (0..1) fade the speaker and the sun in.
    struct StatusMotion {
        bool seen = false;
        int volume = 0, brightness = -1;
        bool muted = false;
        double volumeUntilMs = 0, brightUntilMs = 0;
        float volumeMix = 0, brightMix = 0;
        float fill = 0, speed = 0;
    } statusMotion_;
    // Auto-hide: 0 up, 1 down (below 0 while overshooting on the way up).
    float slide_ = 0, slideFrom_ = 0;
    float imeFade_ = 1; // the input method glyph's crossfade, 0..1
    bool slideDown_ = false;
    double slideStartMs_ = 0;
    BarFrame frame_;
    winrt::com_ptr<ID2D1Bitmap1> layer_;
    std::wstring layerKey_;
};

} // namespace atrium
