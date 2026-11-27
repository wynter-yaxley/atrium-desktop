#include "bar_painter.h"
#include "bar_painter_shared.h"
#include "continuous_corners.h"
#include "ime_glyph.h"
#include "round_stroke.h"
#include "ui_font.h"
#include "wifi_glyph.h"
#include "../motion.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace atrium {

namespace {

constexpr double kAlertPulseMs = 4000, kAlertPeriodMs = 1000;

D2D1_COLOR_F FadeColor(D2D1_COLOR_F c, float a)
{
    c.a *= a;
    return c;
}

void DrawAppBadge(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite, IDWriteTextFormat* f, ID2D1SolidColorBrush* brush, const BarScene& scene,
                  const D2D1_RECT_F& iconArea, float size, int count, float appear)
{
    const float s = scene.scale;
    const float h = std::max(13.f * s, size * 0.44f);
    const std::wstring n = count >= 100 ? L"99+" : std::to_wstring(count);
    float w = h;
    if (count >= 10) {
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite->CreateTextLayout(n.c_str(), UINT32(n.size()), f, 200.f, h, layout.put()))) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(layout->GetMetrics(&tm)))
                w = std::max(h, tm.width + h * 0.50f);
        }
    }
    const float right = iconArea.right + h * 0.20f, top = iconArea.top - h * 0.30f;
    D2D1_RECT_F badge = D2D1::RectF(right - w, top, right, top + h);
    D2D1_MATRIX_3X2_F xform{};
    d2d->GetTransform(&xform);
    if (xform._11 > 0.01f && xform._22 > 0.01f) {
        const float areaW = Vertical(scene.edge) ? scene.windowH : scene.windowW;
        const float areaH = Vertical(scene.edge) ? scene.windowW : scene.windowH;
        const float pad = 1.f * s;
        const float tx0 = badge.left * xform._11 + xform._31;
        const float ty0 = badge.top * xform._22 + xform._32;
        const float tx1 = badge.right * xform._11 + xform._31;
        const float ty1 = badge.bottom * xform._22 + xform._32;
        float dx = 0, dy = 0;
        if (tx0 < pad)
            dx = pad - tx0;
        else if (tx1 > areaW - pad)
            dx = (areaW - pad) - tx1;
        if (ty0 < pad)
            dy = pad - ty0;
        else if (ty1 > areaH - pad)
            dy = (areaH - pad) - ty1;
        badge.left += dx / xform._11;
        badge.right += dx / xform._11;
        badge.top += dy / xform._22;
        badge.bottom += dy / xform._22;
    }
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(badge, h / 2, h / 2);
    brush->SetColor(D2D1::ColorF(0xFF3B30, appear));
    d2d->FillRoundedRectangle(rr, brush);
    brush->SetColor(D2D1::ColorF(0, 0, 0, 0.18f * appear));
    d2d->DrawRoundedRectangle(rr, brush, 0.75f * s);
    brush->SetColor(D2D1::ColorF(1, 1, 1, appear));
    DrawUiText(d2d, dwrite, n, f, badge, brush);
}

void DrawNotifyBadge(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite, IDWriteTextFormat* f, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& iconArea, float d,
                     int count)
{
    const float bxr = iconArea.right - d * 0.35f, by = iconArea.top + d * 0.15f;
    brush->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1.0f));
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bxr, by), d / 2, d / 2), brush);
    const std::wstring n = count > 9 ? L"9+" : std::to_wstring(count);
    if (f) {
        brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
        DrawUiText(d2d, dwrite, n, f, D2D1::RectF(bxr - d, by - d / 2, bxr + d, by + d / 2), brush);
    }
}

void DrawStatusArc(ID2D1DeviceContext2* d2d, ID2D1Factory* factory, ID2D1StrokeStyle* round, ID2D1SolidColorBrush* brush, float ox, float oy, float rad,
                   float a0, float sweep, D2D1_COLOR_F c, float width, winrt::com_ptr<ID2D1PathGeometry>& cachedPath, float& cachedSweep)
{
    if (sweep <= 0.01f || c.a <= 0.003f)
        return;
    constexpr float kDeg = std::numbers::pi_v<float> / 180.f;
    if (!cachedPath || cachedSweep != sweep) {
        cachedPath = nullptr;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(cachedPath.put())) || FAILED(cachedPath->Open(sink.put())))
            return;
        const float a1 = a0 + sweep;
        sink->BeginFigure(D2D1::Point2F(std::cos(a0 * kDeg), std::sin(a0 * kDeg)), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(std::cos(a1 * kDeg), std::sin(a1 * kDeg)), D2D1::SizeF(1, 1), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        cachedSweep = sweep;
    }
    brush->SetColor(c);
    D2D1_MATRIX_3X2_F previous;
    d2d->GetTransform(&previous);
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(rad, rad) * D2D1::Matrix3x2F::Translation(ox, oy) * previous);
    d2d->DrawGeometry(cachedPath.get(), brush, width / rad, round);
    d2d->SetTransform(previous);
}

void DrawChargingBolt(ID2D1DeviceContext2* d2d, ID2D1Factory* factory, ID2D1SolidColorBrush* brush, float cx, float cy, float h, D2D1_COLOR_F ink, float alpha,
                      winrt::com_ptr<ID2D1PathGeometry>& cached)
{
    if (alpha <= 0.003f)
        return;
    if (!cached) {
        winrt::com_ptr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(cached.put())) || FAILED(cached->Open(sink.put())))
            return;
        const D2D1_POINT_2F pts[] = {{0.18f, -0.5f}, {-0.4f, 0.08f}, {-0.02f, 0.08f}, {-0.18f, 0.5f}, {0.4f, -0.08f}, {0.02f, -0.08f}};
        auto at = [](D2D1_POINT_2F p) { return D2D1::Point2F(p.x * 0.17f, 0.44f + p.y * 0.26f); };
        sink->BeginFigure(at(pts[0]), D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < 6; ++i)
            sink->AddLine(at(pts[i]));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
    }
    D2D1_MATRIX_3X2_F previous;
    d2d->GetTransform(&previous);
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(h, h) * D2D1::Matrix3x2F::Translation(cx, cy) * previous);
    brush->SetColor(FadeColor(ink, alpha));
    d2d->FillGeometry(cached.get(), brush);
    d2d->SetTransform(previous);
}

void DrawStatusSpeakerIcon(ID2D1DeviceContext2* d2d, ID2D1Factory* factory, ID2D1StrokeStyle* round, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, float l,
                           float mid, float h, float stroke, const SystemStatus& st)
{
    winrt::com_ptr<ID2D1PathGeometry> cone;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(factory->CreatePathGeometry(cone.put())) && SUCCEEDED(cone->Open(sink.put()))) {
        const D2D1_POINT_2F pts[] = {{l, mid - 0.10f * h},
                                     {l + 0.11f * h, mid - 0.10f * h},
                                     {l + 0.25f * h, mid - 0.22f * h},
                                     {l + 0.25f * h, mid + 0.22f * h},
                                     {l + 0.11f * h, mid + 0.10f * h},
                                     {l, mid + 0.10f * h}};
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 1; i < 6; ++i)
            sink->AddLine(pts[i]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        brush->SetColor(FadeColor(ink, st.muted ? 0.55f : 1.f));
        d2d->FillGeometry(cone.get(), brush);
        d2d->DrawGeometry(cone.get(), brush, stroke * 0.6f, round);
    }
    const float ox = l + 0.25f * h;
    if (st.muted) {
        const float c = ox + 0.19f * h, k = 0.08f * h;
        brush->SetColor(ink);
        d2d->DrawLine(D2D1::Point2F(c - k, mid - k), D2D1::Point2F(c + k, mid + k), brush, stroke, round);
        d2d->DrawLine(D2D1::Point2F(c - k, mid + k), D2D1::Point2F(c + k, mid - k), brush, stroke, round);
        return;
    }
    const int waves = st.volume <= 0 ? 0 : st.volume < 34 ? 1 : st.volume < 67 ? 2 : 3;
    for (int i = 0; i < 3; ++i) {
        const float rad = (0.11f + 0.105f * float(i)) * h, a = 0.75f;
        winrt::com_ptr<ID2D1PathGeometry> arc;
        winrt::com_ptr<ID2D1GeometrySink> s2;
        if (FAILED(factory->CreatePathGeometry(arc.put())) || FAILED(arc->Open(s2.put())))
            break;
        s2->BeginFigure(D2D1::Point2F(ox + rad * std::cos(-a), mid + rad * std::sin(-a)), D2D1_FIGURE_BEGIN_HOLLOW);
        s2->AddArc(D2D1::ArcSegment(D2D1::Point2F(ox + rad * std::cos(a), mid + rad * std::sin(a)), D2D1::SizeF(rad, rad), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                    D2D1_ARC_SIZE_SMALL));
        s2->EndFigure(D2D1_FIGURE_END_OPEN);
        s2->Close();
        brush->SetColor(i < waves ? ink : FadeColor(ink, 0.24f));
        d2d->DrawGeometry(arc.get(), brush, stroke, round);
    }
}

void DrawStatusBatteryIcon(ID2D1DeviceContext2* d2d, ID2D1Factory* factory, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, bool darkInk, float bx0, float mid,
                           float h, float stroke, const SystemStatus& st)
{
    const float bw = 0.58f * h, bh = 0.36f * h, rad = 0.09f * h;
    const D2D1_RECT_F body = D2D1::RectF(bx0 + stroke / 2, mid - bh / 2, bx0 + bw, mid + bh / 2);
    brush->SetColor(FadeColor(ink, 0.9f));
    FillContinuousRect(d2d, D2D1::RectF(body.right + 0.03f * h, mid - 0.075f * h, body.right + 0.08f * h, mid + 0.075f * h), 0.02f * h, brush);
    d2d->DrawRoundedRectangle(D2D1::RoundedRect(body, rad, rad), brush, stroke);
    const float pad = stroke + 0.035f * h, level = std::clamp(float(st.batteryPercent) / 100.f, 0.f, 1.f);
    const D2D1_RECT_F inner = D2D1::RectF(body.left + pad, body.top + pad, body.right - pad, body.bottom - pad);
    const bool low = !st.charging && st.batteryPercent >= 0 && st.batteryPercent <= 20;
    D2D1_COLOR_F fill = low ? D2D1::ColorF(darkInk ? 0xE0352B : 0xFF453A) : ink;
    D2D1_RECT_F lit = inner;
    lit.right = inner.left + std::max(0.06f * h, (inner.right - inner.left) * level);
    winrt::com_ptr<ID2D1RoundedRectangleGeometry> levelShape;
    factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(lit, 0.035f * h, 0.035f * h), levelShape.put());
    winrt::com_ptr<ID2D1PathGeometry> bolt;
    if (st.charging) {
        winrt::com_ptr<ID2D1GeometrySink> s3;
        if (SUCCEEDED(factory->CreatePathGeometry(bolt.put())) && SUCCEEDED(bolt->Open(s3.put()))) {
            const float bx = (body.left + body.right) / 2, k = bh * 0.62f;
            const D2D1_POINT_2F pts[] = {{0.18f, -0.5f}, {-0.4f, 0.08f}, {-0.02f, 0.08f}, {-0.18f, 0.5f}, {0.4f, -0.08f}, {0.02f, -0.08f}};
            s3->BeginFigure(D2D1::Point2F(bx + pts[0].x * k, mid + pts[0].y * k * 1.5f), D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i < 6; ++i)
                s3->AddLine(D2D1::Point2F(bx + pts[i].x * k, mid + pts[i].y * k * 1.5f));
            s3->EndFigure(D2D1_FIGURE_END_CLOSED);
            s3->Close();
        } else {
            bolt = nullptr;
        }
    }
    brush->SetColor(fill);
    if (bolt && levelShape) {
        auto combine = [&](ID2D1Geometry* a, ID2D1Geometry* b) {
            winrt::com_ptr<ID2D1PathGeometry> out;
            winrt::com_ptr<ID2D1GeometrySink> s4;
            if (FAILED(factory->CreatePathGeometry(out.put())) || FAILED(out->Open(s4.put())))
                return winrt::com_ptr<ID2D1PathGeometry>{};
            a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, s4.get());
            s4->Close();
            return out;
        };
        if (auto cut = combine(levelShape.get(), bolt.get()))
            d2d->FillGeometry(cut.get(), brush);
        if (auto rest = combine(bolt.get(), levelShape.get())) {
            brush->SetColor(ink);
            d2d->FillGeometry(rest.get(), brush);
        }
    } else if (levelShape) {
        d2d->FillGeometry(levelShape.get(), brush);
    }
}

void DrawItemPlates(ID2D1DeviceContext2* d2d, const BarScene& scene, ID2D1SolidColorBrush* brush, D2D1_COLOR_F wash, const D2D1_RECT_F& plateArea,
                    D2D1_POINT_2F plateAnchor, D2D1_POINT_2F plateShift, float grow, bool magnifying, float hover, float lift, float alert, float appear,
                    double alertSince, double lastMs)
{
    if (alert > 0.001f) {
        float pulse = 0;
        if (alertSince >= 0 && lastMs - alertSince < kAlertPulseMs)
            pulse = 0.5f - 0.5f * std::cos(2 * std::numbers::pi_v<float> * float((lastMs - alertSince) / kAlertPeriodMs));
        const float clear = magnifying ? std::clamp(1.f - (grow - 1.f) * 8.f, 0.f, 1.f) : 1.f;
        brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, (0.34f + 0.22f * pulse) * alert * appear * clear));
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(grow, grow, plateAnchor) * D2D1::Matrix3x2F::Translation(plateShift.x, plateShift.y));
        FillContinuousRect(d2d, plateArea, scene.plateRadius, brush);
    }
    const float plateA = (magnifying ? 0.f : hover) + 0.4f * lift;
    if (plateA > 0.001f) {
        D2D1_COLOR_F c = wash;
        c.a *= std::min(plateA, 1.6f);
        brush->SetColor(c);
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(grow, grow, plateAnchor) * D2D1::Matrix3x2F::Translation(plateShift.x, plateShift.y));
        FillContinuousRect(d2d, plateArea, scene.plateRadius, brush);
    }
}

} // namespace

void BarPainter::DrawItem(ID2D1DeviceContext2* d2d, const BarScene& scene, const SceneItem& it, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink,
                          D2D1_COLOR_F wash)
{
    const float s = scene.scale;
    const Motion& m = motion_[it.key];
    const bool magnifying = scene.magnify > 1.001f;
    const float dx = m.placed ? m.x - (it.plate.left + it.plate.right) / 2 : 0;
    const D2D1_RECT_F plate = D2D1::RectF(it.plate.left + dx, it.plate.top, it.plate.right + dx, it.plate.bottom);
    const D2D1_RECT_F icon = D2D1::RectF(it.icon.left + dx, it.icon.top, it.icon.right + dx, it.icon.bottom);
    const float size = icon.right - icon.left, bx = (icon.left + icon.right) / 2, bottom = icon.bottom;
    const float grow = wave_.Scale(bx, std::max(1.f, plate.right - plate.left)), cx = MapX(bx);
    const float hover = motion::EaseOut(m.hover), lift = motion::EaseOut(m.lift);
    const EdgeFrame frame = BarFrameFor(scene);
    const D2D1_RECT_F plateArea = frame.ToArea(plate), iconArea = frame.ToArea(icon);
    const D2D1_POINT_2F plateAnchor = frame.ToArea(D2D1::Point2F(bx, plate.bottom));
    const D2D1_POINT_2F plateShift = BarVectorToArea(frame, cx - bx, 0);

    DrawItemPlates(d2d, scene, brush, wash, plateArea, plateAnchor, plateShift, grow, magnifying, hover, lift, m.alert, m.appear, m.alertSince, lastMs_);

    const float k = grow * (magnifying ? 1.f : 1 + 0.06f * hover) * (1 + 0.08f * lift) * (1 - 0.08f * m.press) * (0.85f + 0.15f * m.appear) *
                    (m.leaving ? 0.45f + 0.55f * m.appear : 1.f);
    const float up = m.hop * 0.32f * size + (magnifying ? 0.f : 1.5f * s * hover) + 2.0f * s * lift;
    const D2D1_POINT_2F iconAnchor = frame.ToArea(D2D1::Point2F(bx, bottom));
    const D2D1_POINT_2F iconShift = BarVectorToArea(frame, cx - bx, -up);
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(k, k, iconAnchor) * D2D1::Matrix3x2F::Translation(iconShift.x, iconShift.y));
    if (it.key == L"#ime") {
        IDWriteTextFormat* f = Format((iconArea.bottom - iconArea.top) * 0.64f, DWRITE_FONT_WEIGHT_NORMAL);
        const float e = motion::EaseOut(imeFade_);
        DrawImeGlyph(d2d, dwrite_.get(), f, scene.imePrevGlyph, iconArea, brush, ink, (1 - e) * m.appear, &imeCache_);
        DrawImeGlyph(d2d, dwrite_.get(), f, scene.imeGlyph, iconArea, brush, ink, e * m.appear, &imeCache_);
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
        return;
    }
    if (it.key == L"#status") {
        const bool darkInk = ink.r < 0.5f;
        if (scene.statusIcons)
            DrawStatusIcons(d2d, scene.status, iconArea, brush, ink, darkInk);
        else
            DrawStatus(d2d, scene.status, iconArea, brush, ink, darkInk);
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
        return;
    }
    if (it.key == L"#desktop") {
        D2D1_COLOR_F c = ink;
        c.a = ((ink.r < 0.5f ? 0.18f : 0.30f) + 0.35f * hover) * m.appear;
        brush->SetColor(c);
        const float h = (scene.bar.bottom - scene.bar.top) * 0.46f, mid = (scene.bar.top + scene.bar.bottom) / 2, x = (icon.left + icon.right) / 2;
        d2d->SetTransform(D2D1::Matrix3x2F::Translation(plateShift.x, plateShift.y));
        d2d->FillRectangle(frame.ToArea(D2D1::RectF(x - 0.5f * s, mid - h / 2, x + 0.5f * s, mid + h / 2)), brush);
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
        return;
    }
    if (extras_.Draw(it.key, d2d, iconArea, brush, ink, m.appear, music_, scene.folderStackColor)) {
    } else if (!it.glyph.empty()) {
        // The stack's folder is the settings' own symbol, drawn as large as an app icon; the other
        // entries keep the smaller bar size.
        const float glyphSize = size * (it.key == L"#stacks" ? 0.95f : 0.62f);
        if (IDWriteTextFormat* f = Format(glyphSize, DWRITE_FONT_WEIGHT_NORMAL, IconFontFamily())) {
            brush->SetColor(ink);
            d2d->DrawText(it.glyph.c_str(), UINT32(it.glyph.size()), f, iconArea, brush);
        }
    } else if (ID2D1Bitmap1* bmp = BitmapFor(d2d, it)) {
        usedBitmaps_.push_back(it.imageId);
        shadow_->SetInput(0, bmp);
        shadow_->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, (1.6f + 1.6f * lift + 1.2f * m.hop) * s);
        shadow_->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, (0.38f + 0.12f * lift) * m.appear));
        const auto px = bmp->GetPixelSize();
        D2D1_MATRIX_3X2_F t;
        d2d->GetTransform(&t);
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(size / float(px.width), size / float(px.height)) *
                          D2D1::Matrix3x2F::Translation(iconArea.left, iconArea.top + (1.2f + 1.5f * lift) * s) * t);
        d2d->DrawImage(shadow_.get(), D2D1_INTERPOLATION_MODE_LINEAR);
        d2d->SetTransform(t);
        d2d->DrawBitmap(bmp, iconArea, m.appear, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    } else {
        D2D1_COLOR_F c = wash;
        c.a *= 1.4f * m.appear;
        brush->SetColor(c);
        FillContinuousRect(d2d, iconArea, size * 0.22f, brush);
    }
    if (it.badge > 0) {
        if (it.key == L"#notify") {
            const float d = std::max(9.f * s, size * 0.46f);
            DrawNotifyBadge(d2d, dwrite_.get(), Format(d * 0.62f, DWRITE_FONT_WEIGHT_SEMI_BOLD), brush, iconArea, d, it.badge);
        } else {
            const float h = std::max(13.f * s, size * 0.44f);
            if (IDWriteTextFormat* f = Format(h * 0.64f, DWRITE_FONT_WEIGHT_SEMI_BOLD))
                DrawAppBadge(d2d, dwrite_.get(), f, brush, scene, iconArea, size, it.badge, m.appear);
        }
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());

    if (m.dot > 0.01f && lift < 0.99f) {
        const bool lit = it.attention && scene.alertLight;
        D2D1_COLOR_F c = lit ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f) : BarInkColor(m.dotInk.Value(lastMs_));
        if (!it.active && !lit)
            c.a *= 0.45f;
        c.a *= (1 - lift) * m.appear * m.dot;
        brush->SetColor(c);
        auto dot = RunningDot(scene, it, m);
        dot.point = frame.ToArea(dot.point);
        d2d->FillEllipse(dot, brush);
    }
}

void BarPainter::DrawStatus(ID2D1DeviceContext2* d2d, const SystemStatus& st, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, bool darkInk)
{
    const float h = std::min(r.right - r.left, r.bottom - r.top), cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const StatusMotion& sm = statusMotion_;
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    ID2D1StrokeStyle* round = RoundStroke(d2d);
    auto connection = [&](float size, float alpha) {
        if (alpha <= 0.003f)
            return;
        if (st.connected && !st.wifi) {
            if (IDWriteTextFormat* f = Format(size * 0.62f, DWRITE_FONT_WEIGHT_NORMAL, IconFontFamily())) {
                brush->SetColor(FadeColor(ink, alpha));
                d2d->DrawText(L"\uE839", 1, f, D2D1::RectF(cx - size, cy - size / 2, cx + size, cy + size / 2), brush);
            }
            return;
        }
        const int lit = !st.connected ? -1 : WifiSteps(st.signal);
        DrawWifiFan(d2d, brush, cx, cy, size * 0.62f, lit, FadeColor(ink, alpha), FadeColor(ink, (lit < 0 ? 0.32f : 0.24f) * alpha));
    };

    const bool ring = st.battery || st.audio || sm.brightMix > 0.003f;
    if (!ring) {
        connection(h, 1);
        return;
    }
    constexpr float kGap = 64, kStart = 90 + kGap / 2, kSweep = 360 - kGap;
    const float rad = h * 0.44f, stroke = std::max(1.5f, h * 0.07f);
    const float shown = std::min(1.f, sm.volumeMix + sm.brightMix);
    const float mix = st.battery ? sm.volumeMix : 1.f - sm.brightMix;
    const bool low = st.battery && !st.charging && st.batteryPercent >= 0 && st.batteryPercent <= 20;
    D2D1_COLOR_F level = ink;
    if (low) {
        const D2D1_COLOR_F red = D2D1::ColorF(darkInk ? 0xE0352B : 0xFF453A);
        const float k = 1 - shown;
        level = D2D1::ColorF(red.r + (ink.r - red.r) * (1 - k), red.g + (ink.g - red.g) * (1 - k), red.b + (ink.b - red.b) * (1 - k), ink.a);
    }
    const bool silent = st.muted && mix > 0.5f;
    DrawStatusArc(d2d, factory.get(), round, brush, cx, cy, rad, kStart, kSweep, FadeColor(ink, silent ? 0.16f : 0.24f), stroke, ringTrack_.path,
                  ringTrack_.sweep);
    DrawStatusArc(d2d, factory.get(), round, brush, cx, cy, rad, kStart, kSweep * std::clamp(sm.fill, 0.f, 1.f), level, stroke, ringLevel_.path,
                  ringLevel_.sweep);
    DrawChargingBolt(d2d, factory.get(), brush, cx, cy, h, ink, st.battery && st.charging ? 1 - shown : 0, chargingBolt_);

    const float speaker = st.battery ? sm.volumeMix : 0.f;
    connection(h * 0.6f, 1 - std::min(1.f, speaker + sm.brightMix));
    auto symbol = [&](const wchar_t* glyph, float alpha) {
        if (alpha <= 0.003f)
            return;
        if (IDWriteTextFormat* f = Format(h * 0.40f, DWRITE_FONT_WEIGHT_NORMAL, IconFontFamily())) {
            brush->SetColor(FadeColor(ink, alpha));
            d2d->DrawText(glyph, 1, f, D2D1::RectF(cx - h / 2, cy - h / 2, cx + h / 2, cy + h / 2), brush);
        }
    };
    const wchar_t* volumeGlyph = st.muted || st.volume == 0 ? L"\uE74F" : st.volume < 34 ? L"\uE993" : st.volume < 67 ? L"\uE994" : L"\uE995";
    symbol(volumeGlyph, speaker);
    symbol(L"\uE706", sm.brightMix);

    const float gapSpeaker = !st.battery && st.audio ? 1 - sm.brightMix : 0.f;
    if (gapSpeaker > 0.003f)
        if (IDWriteTextFormat* f = Format(h * 0.22f, DWRITE_FONT_WEIGHT_NORMAL, IconFontFamily())) {
            brush->SetColor(FadeColor(ink, gapSpeaker));
            const float gy = cy + rad;
            d2d->DrawText(volumeGlyph, 1, f, D2D1::RectF(cx - h / 4, gy - h / 4, cx + h / 4, gy + h / 4), brush);
        }
}

void BarPainter::DrawStatusIcons(ID2D1DeviceContext2* d2d, const SystemStatus& st, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink, bool darkInk)
{
    const bool vertical = r.bottom - r.top > r.right - r.left;
    const float h = vertical ? r.right - r.left : r.bottom - r.top, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    ID2D1StrokeStyle* round = RoundStroke(d2d);
    const float stroke = std::max(1.f, h * 0.075f);
    const float netW = 0.62f * h, speakerW = 0.60f * h, batteryW = 0.68f * h, gap = 0.18f * h, stepY = 0.76f * h;
    float x = cx - (netW + speakerW + (st.battery ? batteryW + gap : 0) + (st.audio ? 0 : -speakerW - gap) + gap) / 2;
    float y = cy - float((st.audio ? 1 : 0) + (st.battery ? 1 : 0)) * stepY / 2;

    {
        const float nx = vertical ? cx - netW / 2 : x, ny = vertical ? y : cy;
        if (st.connected && !st.wifi) {
            if (IDWriteTextFormat* f = Format(h * 0.62f, DWRITE_FONT_WEIGHT_NORMAL, IconFontFamily())) {
                brush->SetColor(ink);
                d2d->DrawText(L"\uE839", 1, f, D2D1::RectF(nx - h, ny - h / 2, nx + netW + h, ny + h / 2), brush);
            }
        } else {
            const int lit = !st.connected ? -1 : WifiSteps(st.signal);
            DrawWifiFan(d2d, brush, nx + netW / 2, ny, h * 0.46f, lit, ink, FadeColor(ink, lit < 0 ? 0.32f : 0.24f));
        }
        x += netW + gap;
        y += stepY;
    }

    if (st.audio) {
        DrawStatusSpeakerIcon(d2d, factory.get(), round, brush, ink, vertical ? cx - speakerW / 2 : x, vertical ? y : cy, h, stroke, st);
        x += speakerW + gap;
        y += stepY;
    }

    if (st.battery)
        DrawStatusBatteryIcon(d2d, factory.get(), brush, ink, darkInk, vertical ? cx - batteryW / 2 : x, vertical ? y : cy, h, stroke, st);
}

} // namespace atrium
