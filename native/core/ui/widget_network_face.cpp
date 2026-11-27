#include "ui/widget_faces_internal.h"

#include "localization.h"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

// A rate as a person reads it: "12.4" beside "MB/s". MiSans' figures are tabular, so the digits
// keep their width and the unit does not jump about from second to second.
void RateParts(double bps, std::wstring& value, std::wstring& unit)
{
    wchar_t b[24] = {};
    const double v = std::max(0.0, bps);
    if (v < 1024.0) {
        swprintf_s(b, L"%d", int(std::lround(v)));
        unit = L"B/s";
    } else if (v < 1024.0 * 1024.0) {
        swprintf_s(b, L"%.1f", v / 1024.0);
        unit = L"KB/s";
    } else if (v < 1024.0 * 1024.0 * 1024.0) {
        swprintf_s(b, L"%.1f", v / (1024.0 * 1024.0));
        unit = L"MB/s";
    } else {
        swprintf_s(b, L"%.1f", v / (1024.0 * 1024.0 * 1024.0));
        unit = L"GB/s";
    }
    value = b;
}

// A count of bytes: "1.24 GB", "86.0 MB", "512 KB".
std::wstring BytesText(double bytes)
{
    wchar_t b[32] = {};
    if (bytes < 1024.0)
        swprintf_s(b, L"%d B", int(std::lround(bytes)));
    else if (bytes < 1024.0 * 1024.0)
        swprintf_s(b, L"%.0f KB", bytes / 1024.0);
    else if (bytes < 1024.0 * 1024.0 * 1024.0)
        swprintf_s(b, L"%.1f MB", bytes / (1024.0 * 1024.0));
    else
        swprintf_s(b, L"%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return b;
}

// The speed the link negotiated: "1.0 Gbps", "866 Mbps"; nothing where Windows does not say (a VPN).
std::wstring LinkText(double mbps)
{
    if (mbps <= 0)
        return {};
    wchar_t b[24] = {};
    if (mbps >= 1000.0)
        swprintf_s(b, L"%.1f Gbps", mbps / 1000.0);
    else
        swprintf_s(b, L"%.0f Mbps", mbps);
    return b;
}

// A rate's arrow: a shaft and two head strokes, down for what comes in, up for what goes out.
void RateArrow(Canvas& c, float cx, float cy, float r, bool down, D2D1_COLOR_F ink)
{
    const float w = std::max(1.2f, r * 0.34f);
    const float tip = down ? cy + r : cy - r, tail = down ? cy - r : cy + r;
    const float head = down ? tip - r * 0.6f : tip + r * 0.6f;
    c.Capsule(cx, tail, cx, tip, ink, w);
    c.Capsule(cx, tip, cx - r * 0.62f, head, ink, w);
    c.Capsule(cx, tip, cx + r * 0.62f, head, ink, w);
}

// The top of the graph: the fastest second of the window, rounded up to 1, 2 or 5 times a power of
// ten. An idle link keeps a floor, so its few hundred bytes do not fill the chart.
float GraphTop(const std::vector<NetworkRate>& h)
{
    float peak = 0;
    for (const NetworkRate& r : h)
        peak = std::max({peak, r.down, r.up});
    if (peak <= 16384.f)
        return 16384.f;
    const float e = std::pow(10.f, std::floor(std::log10(peak)));
    const float m = peak / e;
    return (m <= 1.f ? 1.f : m <= 2.f ? 2.f : m <= 5.f ? 5.f : 10.f) * e;
}

// One series of the graph: a line of capsules with the newest second on the right edge, so the
// minute fills in from the right instead of two points stretching across the whole width.
void GraphLine(Canvas& c, const std::vector<NetworkRate>& h, bool out, float l, float t, float r, float b, float top, D2D1_COLOR_F ink)
{
    const size_t n = h.size();
    if (n < 2)
        return;
    auto y = [&](const NetworkRate& s) { return b - (b - t) * std::min(std::max(out ? s.up : s.down, 0.f), top) / top; };
    const float step = (r - l) / float(kNetworkPoints - 1), x0 = r - step * float(n - 1);
    for (size_t i = 1; i < n; ++i)
        c.Capsule(x0 + step * float(i - 1), y(h[i - 1]), x0 + step * float(i), y(h[i]), ink, 1.6f);
    c.Circle(r, y(h[n - 1]), 1.8f, ink);
}

// A rate: its arrow, then the value at `font` with the unit on the same baseline. MiSans' line
// box is 1.326 em and its baseline 1.044 em down, so the unit is centred that much lower.
void PaintRateLine(Canvas& c, const WidgetPal& p, float fw, float pad, float top, float bottom, double bps, bool out, float arrow, float font, float unitSize,
                   D2D1_COLOR_F seriesInk)
{
    std::wstring value, unit;
    RateParts(bps, value, unit);
    const float cy = (top + bottom) / 2, x0 = pad + 2;
    RateArrow(c, x0 + arrow, cy, arrow, !out, seriesInk);
    const float x = x0 + 2 * arrow + 9;
    c.Text(value, D2D1::RectF(x, top, fw - pad, bottom), font, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float tw = c.TextWidth(value, font, DWRITE_FONT_WEIGHT_SEMI_BOLD), shift = 0.381f * (font - unitSize);
    c.Text(unit, D2D1::RectF(x + tw + 4, top + shift, x + tw + 4 + unitSize * 5, bottom + shift), unitSize, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

} // namespace

// The link's traffic: what comes in and what goes out, each with its arrow, the last minute of both
// under them, and the adapter with the speed it negotiated. The medium one adds this session's totals.
void PaintNetworkFace(Canvas& c, WidgetSize size, const WidgetWorld& w, const WidgetPal& p, float fw, float fh)
{
    const NetworkMonitor* n = w.network;
    const bool compact = size == WidgetSize::Small; // not `small`: rpcndr.h defines that as char
    const float pad = compact ? kWidgetPad : 16;
    // Dusk is monochrome, the accents too: the two series then differ by weight, not by hue.
    const bool mono = p.icon == WidgetInk::Mono;
    const D2D1_COLOR_F down = mono ? WidgetAlpha(p.text, 0.95f) : p.blue;
    const D2D1_COLOR_F up = mono ? WidgetAlpha(p.text, 0.5f) : p.orange;
    c.Text(Tr(L"Network"), D2D1::RectF(pad + 1, pad - 2, fw - pad - 40, pad + 16), compact ? 12.5f : 13.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!n || !n->Linked()) {
        c.Glyph(L"\uE774", D2D1::RectF(0, fh / 2 - 48, fw, fh / 2 - 14), 24, p.sub);
        c.Text(Tr(L"No network available"), D2D1::RectF(pad, fh / 2 - 6, fw - pad, fh / 2 + 14), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        return;
    }
    if (!compact) {
        const std::wstring speed = LinkText(n->LinkMbps());
        c.Text(speed.empty() ? n->Name() : n->Name() + L" · " + speed, D2D1::RectF(fw / 2 - 60, pad - 1, fw - pad - 1, pad + 16), 11, p.sub,
               DWRITE_FONT_WEIGHT_NORMAL, 2);
    }
    const std::vector<NetworkRate>& history = n->History();
    const float top = GraphTop(history);
    if (compact) {
        PaintRateLine(c, p, fw, pad, 32, 60, n->DownBps(), false, 6.5f, 20, 10.5f, down);
        PaintRateLine(c, p, fw, pad, 62, 84, n->UpBps(), true, 5, 14.5f, 9.5f, up);
        c.Line(pad, 126, fw - pad, 126, p.track);
        GraphLine(c, history, false, pad, 90, fw - pad, 126, top, down);
        GraphLine(c, history, true, pad, 90, fw - pad, 126, top, up);
        c.Text(n->Name(), D2D1::RectF(pad + 1, fh - pad - 14, fw - pad, fh - pad + 1), 10, p.sub);
        return;
    }
    PaintRateLine(c, p, fw, pad, 34, 72, n->DownBps(), false, 8, 32, 12.5f, down);
    PaintRateLine(c, p, fw, pad, 74, 96, n->UpBps(), true, 6, 18, 11, up);
    c.Line(pad, 136, fw - pad, 136, p.track);
    GraphLine(c, history, false, pad, 102, fw - pad, 136, top, down);
    GraphLine(c, history, true, pad, 102, fw - pad, 136, top, up);
    c.Text(TrFormat(L"Received %s · sent %s", BytesText(n->DownBytes()).c_str(), BytesText(n->UpBytes()).c_str()), D2D1::RectF(pad, 140, fw - pad, 154), 10,
           p.sub);
}

} // namespace atrium
