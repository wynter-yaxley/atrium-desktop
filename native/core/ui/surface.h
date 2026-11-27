// A popup window (the start menu and the notification panels). Two
// composition layers: the glass below, drawn by the bar's render thread with
// the bar's material (GlassHost, D25), and the content above, drawn here with
// Direct2D into a DirectComposition swap chain with premultiplied alpha. Lives
// on the UI thread; paints synchronously when invalidated and runs frame
// animations from a timer. The window is the content rectangle plus a margin
// for the glass's shadow; callers work in content coordinates only.
#pragma once

#include "icon_image.h"
#include "render/glass_panel.h"
#include "ui/popup_theme.h"

#include <windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <winrt/base.h>

#include <functional>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace atrium {

// A piece of glass in a popup: content coordinates, DIP.
struct ShapeDip {
    D2D1_RECT_F rect;
    float radius;
    float fade = 0; // 0 = as is, 1 = gone
    float rotate = 0; // radians, clockwise about its centre
    // Melting parts instead of one rectangle (GlassShape::parts), DIP; rect is their bounds.
    struct Part {
        D2D1_RECT_F rect;
        float radii[4]; // top left, top right, bottom right, bottom left
    };
    std::vector<Part> parts;
    float melt = 0; // DIP
    float partMix = 0; // 0: the parts, 1: rect with radius
    float tailX = 0, tailHalfWidth = 0, tailHeight = 0, tailInset = 0; // optional Dock label pointer, DIP
};

class Surface;

// Drawing helpers bound to one surface's device context.
class Canvas {
public:
    Canvas(Surface& surface, ID2D1DeviceContext* d2d, float scale) : scale(scale), s_(surface), d2d_(d2d) {}
    float scale; // px per DIP; every coordinate below is in DIP
    bool panel = false; // the glass is not there (yet): paint a plain panel instead
    double now = 0;     // ClockMs() of this frame
    Theme theme;        // ink for this frame
    float inkDark = 0;  // the theme's amount of dark ink (it fades, InkFade)
    bool halo = true;   // Text and Glyph draw the theme's halo under the ink
    // Ink region by region (WM_GLASS_INK): Text and Glyph in the theme's ink
    // take the ink the glass under them needs, fading as it changes.
    InkField ink;

    // Asks for another frame (an animation is still running).
    void Continue();
    // The amount of dark ink (0..1) the glass under `r` takes now, region by region as Text does.
    float DarkAt(D2D1_RECT_F r) const;
    void Callout(D2D1_RECT_F body, float radius, float tipX, float halfWidth, float tailHeight, float tailInset, D2D1_COLOR_F color);
    // Draws what follows faded, scaled and turned (clockwise, radians) around (cx, cy), moved by
    // (dx, dy); DIP. Groups nest.
    void BeginGroup(float opacity, float k, float cx, float cy, float dx = 0, float dy = 0, float turn = 0);
    void EndGroup();
    // What follows is one mark of several parts (an icon): its ink is chosen once, for `r` as a whole,
    // so it never turns dark in one part and light in another. Until EndInkArea; DIP.
    void BeginInkArea(D2D1_RECT_F r) { inkArea_ = r; }
    void EndInkArea() { inkArea_.reset(); }

    D2D1_RECT_F Px(D2D1_RECT_F r) const;
    void Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c);
    void Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1);
    void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width = 1);
    // A line with round ends (a clock's hand).
    void Capsule(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width);
    void Circle(float cx, float cy, float radius, D2D1_COLOR_F c);
    // A rounded rectangle filled from `a` to `b`, top to bottom (or left to right).
    void Gradient(D2D1_RECT_F r, float radius, D2D1_COLOR_F a, D2D1_COLOR_F b, bool vertical = true);
    // A crescent: the disc (cx, cy, radius) less the disc of `cut` moved by (dx, dy).
    void Crescent(float cx, float cy, float radius, float dx, float dy, float cut, D2D1_COLOR_F c);
    // A filled polygon; `round` > 0 rounds its corners, growing it by that much on every side.
    void Polygon(std::span<const D2D1_POINT_2F> points, D2D1_COLOR_F c, float round = 0);
    // A circular arc with round ends, clockwise from a0 to a1 (degrees from the right, y down).
    void Arc(float cx, float cy, float radius, float a0, float a1, D2D1_COLOR_F c, float width);
    // Text in `r`; align: 0 left, 1 centre, 2 right. One line, trimmed with an ellipsis unless wrap.
    void Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, int align = 0,
              bool wrap = false);
    void Glyph(const std::wstring& g, D2D1_RECT_F r, float size, D2D1_COLOR_F c);
    // The iPhone's Wi-Fi symbol (wifi_glyph) centred on (cx, cy), `radius` DIP high; `lit` steps 0..3 in `c`, the rest faint.
    void WifiFan(float cx, float cy, float radius, int lit, D2D1_COLOR_F c);
    float TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL);
    // The caret before character `pos` of `s` laid out as Text(s, r, size, c, w, 0, true) lays it out (DIP).
    D2D1_RECT_F Caret(const std::wstring& s, D2D1_RECT_F r, float size, DWRITE_FONT_WEIGHT w, size_t pos);
    // An icon by a stable cache key; `image` is only read the first time.
    void Image(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, float opacity = 1);
    void ImageCircle(const std::wstring& key, const IconImage& image, D2D1_RECT_F r);
    // Part of an image (`source`, px of the image) drawn onto the quadrilateral `quad` (DIP: its corners
    // for the source's top left, top right, bottom right, bottom left), cached as Image caches it.
    void ImageQuad(const std::wstring& key, const IconImage& image, D2D1_RECT_F source, const std::array<D2D1_POINT_2F, 4>& quad, float opacity = 1);
    // An image as Image draws it into `r`, then through `perspective`: a 4x4 transform in px of the
    // content (what Px gives), before the group's transform.
    void ImagePerspective(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, const D2D1_MATRIX_4X4_F& perspective, float opacity = 1);
    bool Cached(const std::wstring& key) const;
    // A press ripple: a soft disc growing from (x, y) inside the rounded
    // rectangle r, at progress t (0..1).
    void Ripple(D2D1_RECT_F r, float radius, float x, float y, float t, D2D1_COLOR_F c);
    void Clip(D2D1_RECT_F r);
    void Unclip();
    // What is drawn between the two, blurred by `radius` (DIP, the standard deviation; 0: sharp).
    // No nesting; clips pushed inside are popped inside.
    void BeginBlur();
    void EndBlur(float radius);
    // What is drawn between the two fades along the line `from` to `to` (DIP): `stops` give the
    // opacity (their colour's alpha) at positions 0..1 of it. May sit inside a blur.
    void BeginFade(D2D1_POINT_2F from, D2D1_POINT_2F to, std::span<const D2D1_GRADIENT_STOP> stops, D2D1_RECT_F contentBounds = D2D1::InfiniteRect());
    void EndFade();
    // Apply the analytic glass outline to the content already drawn.
    void MaskRounded(D2D1_RECT_F r, float radius);
    ID2D1DeviceContext* D2D() const { return d2d_; }

private:
    ID2D1Bitmap* Bitmap(const std::wstring& key, const IconImage& image); // cached under key
    IDWriteTextFormat* Format(float px, DWRITE_FONT_WEIGHT w, bool glyph, int align, bool wrap);
    D2D1_COLOR_F InkAt(D2D1_RECT_F r, D2D1_COLOR_F c, D2D1_COLOR_F& haloInk) const; // c, and its halo, for what is under r
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
    Surface& s_;
    ID2D1DeviceContext* d2d_;
    float alpha_ = 1; // opacity of the current group
    std::vector<std::pair<D2D1_MATRIX_3X2_F, float>> groups_;
    std::optional<D2D1_RECT_F> inkArea_;
    winrt::com_ptr<ID2D1Image> blurTarget_;
    winrt::com_ptr<ID2D1CommandList> blurList_;
};

class Surface {
public:
    // paint: draws the content (DIP coordinates, the window is width x height DIP).
    // message: window messages before default handling; return true if handled (result in `result`).
    // insetDip: the margin around the content for the glass's shadow.
    Surface(HINSTANCE instance, const wchar_t* title, bool activatable, float insetDip = 12, bool desktopLayer = false, bool clickThrough = false);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    std::function<void(Canvas&)> paint;
    // Mouse messages arrive in content coordinates.
    std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> message;
    // A press on the shadow margin, outside the content, or Alt+F4: the owner
    // closes (none: Alt+F4 does nothing, as on a widget).
    std::function<void()> onDismiss;
    std::function<void()> onReady;  // the glass is on screen (or the plain panel stands in)

    // The bar draws the glass layers; development screenshots keep popups capturable.
    static void SetGlassHost(GlassHost* host);
    static void SetCapturable(bool capturable);
    // Whether popups may show in screenshots: true while the bar's backdrop
    // (Desktop Duplication) leaves them out by itself. UI thread.
    static void SetCaptureVisible(bool visible);
    // Never in screenshots (notification cards with their text hidden).
    void SetPrivate(bool on);

    // Shows the content at `rect` (screen px), popping out of `anchor` (a
    // screen point); activatable surfaces take the foreground.
    void Show(RECT rect, bool activate, POINT anchor, bool pop = true); // pop: false appears in place (a hand-over from another popup)
    void Move(RECT rect, bool keepZOrder = false);
    // Only `dip` (content coordinates) and the shadow margin round it are on screen and take
    // clicks, which elsewhere reach the windows below; an empty rectangle: the whole window. A
    // popup whose pieces change size inside a window that does not (the start menu).
    void SetRegion(D2D1_RECT_F dip);
    // Nothing of the window on screen or taking clicks until SetRegion again: a layer kept ready, out of the way.
    void SetRegionNone();
    // Keep backing buffers stable during a resize; {0,0} releases the reserve.
    // Input bounds and glass shape keep their actual size.
    void ReserveResize(D2D1_SIZE_F sizeDip);
    // Plays the closing animation, then hides the window. Visible() is false at once.
    void Hide(bool animate = true);
    bool Visible() const { return visible_; }
    // Input and content changes paint immediately; continuing animation frames use the frame pump.
    void Invalidate();
    HWND Hwnd() const { return hwnd_; }
    float Scale() const { return scale_; }
    RECT Rect() const { return rect_; } // the content, screen px
    // Corner radius of the glass shape (DIP), matching the bar's.
    void SetRadius(float dip);
    // No glass at all: the content alone, transparent where nothing is drawn (Stage Manager's
    // strip, a widget with its own opaque face). May change while shown: the glass layer goes or comes.
    void SetGlassless(bool on);
    // Separate pieces of glass (content DIP), back to front; empty: the whole
    // content with the radius. Published to the glass when they change.
    void SetShapes(const std::vector<ShapeDip>& shapes, bool animating = false);
    // A press at (x, y) DIP in content coordinates: the glass ripples from there.
    void Ripple(float x, float y);
    POINT ScreenToContent(POINT screen) const { return POINT{screen.x - rect_.left, screen.y - rect_.top}; }
    POINT ContentToScreen(POINT content) const { return POINT{content.x + rect_.left, content.y + rect_.top}; }
    // Drops an image Canvas::Image cached under `key` (it is replaced by another).
    void ForgetImage(const std::wstring& key) { bitmaps_.erase(key); }

private:
    friend class Canvas;
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    bool HandleMouse(UINT m, LPARAM& l);
    void Paint();      // the frame now, unless one is being drawn: then the one after
    void PaintFrame();
    bool PrepareSwapTarget(int w, int h);
    void SetupCanvasInk(Canvas& c);
    void Present();
    bool EnsureDevice();
    void ResetDevice();
    void RecoverDevice();
    void RetryFrame();
    void Place(RECT rect, bool keepZOrder = false); // texture = content + margin; desktop HWND stays stationary
    void ApplyPlacement(bool keepZOrder = false);
    void ApplyRegion();
    void PublishPanel();
    void StartGlassWait();
    POINT PopOrigin(POINT a) const
    {
        const RECT& r = desktopLayer_ ? nativeWindow_ : window_;
        return {a.x - r.left, a.y - r.top};
    }
    void SetInk(bool dark, const InkGrid& grid); // WM_GLASS_INK: the inks start fading there
    void FinishHide();
    int InsetPx() const;
    D2D1_SIZE_U BufferSize() const;

    HWND hwnd_ = nullptr;
    bool activatable_;
    bool desktopLayer_ = false;
    bool clickThrough_ = false;
    bool glassless_ = false;
    float insetDip_;
    bool visible_ = false;
    bool hiding_ = false;    // closing animation running
    bool firstPaint_ = false;
    bool glassReady_ = false;
    bool frameWanted_ = false;
    unsigned glassGen_ = 0;  // of the panel last published
    unsigned glassShown_ = 0; // GlassPanel::shown
    bool glassPending_ = false; // published since the last content frame
    bool presentPending_ = false; // one content buffer awaiting its glass; input stays responsive
    bool painting_ = false;       // inside PaintFrame: a paint callback's message may ask for another
    std::optional<RECT> nextResize_;
    bool releaseResize_ = false;
    unsigned presentGen_ = 0;
    double paintMs_ = 0;
    // An animation's frames, logged when it ends (smoothness checks).
    struct Burst {
        double start = 0, last = 0, maxGap = 0, maxPaint = 0;
        int frames = 0;
    } burst_;
    double shownMs_ = 0;
    RECT rect_{};            // content, screen px
    RECT window_{};          // texture rectangle including shadow, screen px
    RECT nativeWindow_{};    // desktop host: monitor bounds, with a tight window region
    RECT presentedWindow_{}; // last content frame, used only to protect in-flight pixels from clipping
    RECT inputRegion_{};
    D2D1_RECT_F region_{}; // SetRegion
    bool regionNone_ = false; // SetRegionNone
    float scale_ = 1;
    float radius_ = 20;
    D2D1_SIZE_F resizeReserve_{};
    std::vector<ShapeDip> shapes_;
    bool shapesAnimating_ = false;
    bool darkInk_ = false;
    InkFade inkAll_;               // the theme's ink, fading
    InkGrid inkGrid_;              // the ink region by region, as published
    std::vector<InkFade> inkCells_; // and each cell's fade
    POINT anchor_{};
    unsigned pop_ = 0;
    bool popOpen_ = true;
    float rippleX_ = 0, rippleY_ = 0;
    double rippleMs_ = 0;
    bool private_ = false;
    void ApplyCaptureExclusion();
    int bufW_ = 0, bufH_ = 0;
    // One D3D11/D2D/DirectComposition device for all popups (a device per
    // window costs tens of MB and dozens of driver threads); per window only
    // the swap chain, its composition target and a device context, released
    // while the window is hidden.
    struct Shared;
    static std::shared_ptr<Shared> SharedDevice();
    std::shared_ptr<Shared> shared_;
    winrt::com_ptr<IDXGISwapChain1> swap_;
    winrt::com_ptr<ID2D1Bitmap1> backTarget_; // the back buffer for D2D (buffer 0 is always the one to draw in)
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual> visual_;
    winrt::com_ptr<ID2D1DeviceContext> d2d_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;
    winrt::com_ptr<ID2D1Bitmap> calloutMask_; // one current label mask, only allocated for the plain fallback
    std::array<float, 7> calloutMaskKey_{};
    winrt::com_ptr<ID2D1Effect> blur_; // Canvas::EndBlur; of the current device context only
    int failedFrames_ = 0;             // in a row, each on a fresh device (RetryFrame)
    winrt::com_ptr<ID2D1CommandList> roundedMask_;
    D2D1_RECT_F roundedMaskRect_{};
    float roundedMaskRadius_ = 0;
    std::map<std::tuple<int, int, bool, int, bool>, winrt::com_ptr<IDWriteTextFormat>> formats_;
    std::map<std::wstring, winrt::com_ptr<ID2D1Bitmap>> bitmaps_;
};

} // namespace atrium
