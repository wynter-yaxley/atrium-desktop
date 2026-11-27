#include "bar_extras_painter.h"

#include "continuous_corners.h"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

// The square centred in r, `k` of its shorter side.
D2D1_RECT_F Square(D2D1_RECT_F r, float k)
{
    const float w = std::min(r.right - r.left, r.bottom - r.top) * k, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    return D2D1::RectF(cx - w / 2, cy - w / 2, cx + w / 2, cy + w / 2);
}

// A closed rectangle with its own radius at each corner: top left, top right, bottom right, bottom left.
void AddRoundedSquare(ID2D1GeometrySink* sink, D2D1_RECT_F r, const float (&radii)[4])
{
    const auto arc = [&](float x, float y, float radius) {
        if (radius > 0)
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    };
    sink->BeginFigure(D2D1::Point2F(r.left + radii[0], r.top), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(D2D1::Point2F(r.right - radii[1], r.top));
    arc(r.right, r.top + radii[1], radii[1]);
    sink->AddLine(D2D1::Point2F(r.right, r.bottom - radii[2]));
    arc(r.right - radii[2], r.bottom, radii[2]);
    sink->AddLine(D2D1::Point2F(r.left + radii[3], r.bottom));
    arc(r.left, r.bottom - radii[3], radii[3]);
    sink->AddLine(D2D1::Point2F(r.left, r.top + radii[0]));
    arc(r.left + radii[0], r.top, radii[0]);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

} // namespace

bool BarExtrasPainter::Draw(const std::wstring& key, ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, float opacity,
                            const MusicBars& bars, bool folderStackColor)
{
    D2D1_COLOR_F faded = ink;
    faded.a *= opacity;
    if (key == L"#start")
        DrawLogo(d2d, r, opacity);
    else if (key == L"#launchpad")
        DrawLaunchpad(d2d, r, brush, faded);
    else if (key == L"#music")
        DrawMusic(d2d, r, bars, brush, faded);
    else if (key == L"#stacks") {
        if (!folderStackColor)
            return false; // the ink outline is the settings' own folder glyph, drawn by the glyph path
        DrawStacks(d2d, r, brush, faded);
    } else
        return false;
    return true;
}

void BarExtrasPainter::DrawLogo(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, float opacity)
{
    const D2D1_RECT_F box = Square(r, 0.80f);
    const float w = box.right - box.left, left = box.left, top = box.top;
    const float gap = w * 0.08f, pane = (w - gap) / 2, corner = pane * 0.10f;
    if (!logoBlue_) {
        D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(0x5CCBFF)}, {0.5f, D2D1::ColorF(0x1A9BF0)}, {1.f, D2D1::ColorF(0x0060C8)}};
        winrt::com_ptr<ID2D1GradientStopCollection> coll;
        if (FAILED(d2d->CreateGradientStopCollection(stops, 3, coll.put())) ||
            FAILED(d2d->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), coll.get(), logoBlue_.put())) ||
            FAILED(d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), logoSheen_.put()))) {
            logoBlue_ = nullptr;
            return;
        }
    }
    ID2D1LinearGradientBrush* grad = logoBlue_.get();
    grad->SetStartPoint(D2D1::Point2F(left, top));
    grad->SetEndPoint(D2D1::Point2F(left + w, top + w));
    grad->SetOpacity(opacity);
    ID2D1SolidColorBrush* sheen = logoSheen_.get();
    sheen->SetOpacity(opacity);
    for (int i = 0; i < 4; ++i) {
        const float x = left + float(i % 2) * (pane + gap), y = top + float(i >> 1) * (pane + gap);
        FillContinuousRect(d2d, D2D1::RectF(x, y, x + pane, y + pane), corner, grad);
        // A faint lit edge on top of each pane.
        d2d->FillRectangle(D2D1::RectF(x + corner, y, x + pane - corner, y + std::max(0.6f, pane * 0.06f)), sheen);
    }
}

void BarExtrasPainter::DrawLaunchpad(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink)
{
    // Four squares in outline, as large as the Windows logo: the four corners of the whole are sharp, every
    // other corner round.
    const D2D1_RECT_F box = Square(r, 0.80f);
    const float s = box.right - box.left, stroke = std::max(1.f, s * 0.065f), gap = s * 0.12f, pane = (s - gap) / 2;
    const float round = pane * 0.32f - stroke / 2;
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put())))
        return;
    for (int i = 0; i < 4; ++i) {
        const int col = i % 2, row = i / 2;
        const float x0 = box.left + float(col) * (pane + gap) + stroke / 2, y0 = box.top + float(row) * (pane + gap) + stroke / 2;
        const float x1 = x0 + pane - stroke, y1 = y0 + pane - stroke;
        // Top left, top right, bottom right, bottom left: the one at the whole's corner is sharp.
        const int sharp = row == 0 ? col : 3 - col;
        float radii[4] = {round, round, round, round};
        radii[sharp] = 0;
        AddRoundedSquare(sink.get(), D2D1::RectF(x0, y0, x1, y1), radii);
    }
    if (FAILED(sink->Close()))
        return;
    brush->SetColor(ink);
    d2d->DrawGeometry(path.get(), brush, stroke);
}

void BarExtrasPainter::DrawMusic(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, const MusicBars& bars, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink)
{
    const D2D1_RECT_F box = Square(r, 0.80f);
    const float side = box.right - box.left, bar = side * 0.11f, gap = side * 0.0925f;
    const float cy = (box.top + box.bottom) / 2;
    float x = (box.left + box.right) / 2 - (float(MusicBars::kCount) * bar + float(MusicBars::kCount - 1) * gap) / 2;
    brush->SetColor(ink);
    for (size_t i = 0; i < MusicBars::kCount; ++i, x += bar + gap) {
        const float h = std::max(bar, side * bars.Height(i));
        d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, cy - h / 2, x + bar, cy + h / 2), bar / 2, bar / 2), brush);
    }
}

void BarExtrasPainter::DrawStacks(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink)
{
    // The reference logo's folder, filled in macOS blue and as large as the apps beside it.
    const D2D1_RECT_F box = Square(r, 1.f);
    const float s = box.right - box.left;
    auto at = [&](float u, float v) { return D2D1::Point2F(box.left + u * s, box.top + v * s); };
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    winrt::com_ptr<ID2D1PathGeometry> folder;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(folder.put())) || FAILED(folder->Open(sink.put())))
        return;
    const float rt = 0.07f, rb = 0.12f; // the tab's and body's corners, as the reference logo
    sink->BeginFigure(at(0.02f, 0.12f + rt), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.02f, 0.12f), at(0.02f + rt, 0.12f)));
    sink->AddLine(at(0.44f - rt, 0.12f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.44f, 0.12f), at(0.44f, 0.12f + rt)));
    sink->AddLine(at(0.44f, 0.26f));
    sink->AddLine(at(0.98f - rb, 0.26f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.98f, 0.26f), at(0.98f, 0.26f + rb)));
    sink->AddLine(at(0.98f, 0.92f - rb));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.98f, 0.92f), at(0.98f - rb, 0.92f)));
    sink->AddLine(at(0.02f + rb, 0.92f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(0.02f, 0.92f), at(0.02f, 0.92f - rb)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return;
    if (!folderBlue_) {
        D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(0x8FD3FF)}, {0.55f, D2D1::ColorF(0x4A9BF5)}, {1.f, D2D1::ColorF(0x1F6FE0)}};
        winrt::com_ptr<ID2D1GradientStopCollection> coll;
        if (FAILED(d2d->CreateGradientStopCollection(stops, 3, coll.put())) ||
            FAILED(d2d->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), coll.get(), folderBlue_.put())))
            return;
    }
    ID2D1LinearGradientBrush* grad = folderBlue_.get();
    grad->SetStartPoint(at(0.06f, 0.20f));
    grad->SetEndPoint(at(0.94f, 0.92f));
    grad->SetOpacity(std::clamp(ink.a, 0.f, 1.f));
    d2d->FillGeometry(folder.get(), grad);
    // A lit top edge, as the apps' icons have on the glass.
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.20f * std::clamp(ink.a, 0.f, 1.f)));
    const float x0 = at(0.06f, 0).x, x1 = at(0.94f, 0).x, y0 = at(0, 0.28f).y, y1 = at(0, 0.36f).y;
    d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x1, y1), (y1 - y0) / 2, (y1 - y0) / 2), brush);
}

} // namespace atrium
