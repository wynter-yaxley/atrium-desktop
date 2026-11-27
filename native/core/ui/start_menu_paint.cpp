// Start menu Direct2D painting: panel, search, tiles, A-Z list, folder card, and status capsule.
#include "ui/start_menu.h"

#include "bar_edge.h"
#include "localization.h"
#include "motion.h"
#include "render/wifi_glyph.h"
#include "ui/start_menu_metrics.h"
#include "ui/start_menu_sort.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace atrium {

namespace {

using namespace start_menu_detail;

} // namespace

bool StartMenu::StepPanelSize(Canvas& c, D2D1_SIZE_F target)
{
    const float targetH = target.height - kGap - barH_;
    if (panelW_ == target.width && mainH_ == targetH && panelV_ == 0 && mainV_ == 0)
        return false;
    kResize.Step(panelW_, panelV_, target.width, dt_ / 1000);
    kResize.Step(mainH_, mainV_, targetH, dt_ / 1000);
    if (std::fabs(panelW_ - target.width) < 0.25f && std::fabs(mainH_ - targetH) < 0.25f && std::fabs(panelV_) < 2 && std::fabs(mainV_) < 2) {
        panelW_ = target.width;
        mainH_ = targetH;
        panelV_ = mainV_ = 0;
        return false;
    }
    c.Continue();
    return true;
}

void StartMenu::UpdateSurfaceRegion(D2D1_SIZE_F target, bool resizing)
{
    // Only the menu's pieces are on screen and take clicks (with room for the frame still on its way).
    const BarEdge edge = EdgeOf(bar_);
    const float winW = float(surface_.Rect().right - surface_.Rect().left) / surface_.Scale();
    const float winH = float(surface_.Rect().bottom - surface_.Rect().top) / surface_.Scale(), slack = resizing ? 8.f : 0.f;
    const float rw = std::max(W(), target.width) + slack, rh = std::max(H(), target.height) + slack;
    float left = 0.f, top = 0.f;
    if (edge == BarEdge::Right)
        left = std::max(0.f, winW - rw);
    else if (!Vertical(edge) && centered_)
        left = std::max(0.f, (winW - rw) / 2);
    if (edge == BarEdge::Bottom)
        top = std::max(0.f, winH - rh);
    else if (Vertical(edge) && centered_)
        top = std::max(0.f, (winH - rh) / 2);
    surface_.SetRegion(D2D1::RectF(left, top, std::min(winW, left + rw), std::min(winH, top + rh)));
}

void StartMenu::PaintFolderCard(Canvas& c, std::vector<ShapeDip>& shapes, float dt, float& open, D2D1_RECT_F& card, bool& animating)
{
    if (folder_ < 0)
        return;
    // The card springs out of the folder's tile and back into it; turning round
    // half-way keeps its speed.
    const float goal = folderClosing_ ? 0.f : 1.f;
    (folderClosing_ ? kFolderClose : kFolderOpen).Step(folderGrow_, folderV_, goal, dt / 1000);
    const bool rest = std::fabs(folderGrow_ - goal) < 0.002f && std::fabs(folderV_) < 0.02f;
    if (rest) {
        folderGrow_ = goal;
        folderV_ = 0;
    }
    if (folderClosing_ && (rest || folderGrow_ < 0.01f)) {
        folder_ = -1;
        folderClosing_ = false;
        folderGrow_ = folderV_ = 0;
        return;
    }
    open = std::clamp(folderGrow_, 0.f, 1.f);
    card = Mix(folderFrom_, FolderRect(), folderGrow_);
    const float cardR = std::max(12.f, radius_ - 14); // concentric with the panel
    shapes.push_back({card, Mix(12.f, cardR, open)});
    if (!rest) {
        c.Continue();
        animating = true;
    }
}

void StartMenu::Paint(Canvas& c)
{
    visibleIcons_.clear(), tipText_.clear();
    const Theme& t = c.theme;
    const double now = c.now;
    hits_.clear();
    const double gap = now - lastPaint_;
    dt_ = (lastPaint_ > 0 && gap > 0.0 && gap <= 50.0) ? float(gap) : 16.f;
    lastPaint_ = now;
    auto glide = [&](float& v, float target, float rate, float eps) {
        v = motion::Approach(v, target, dt_, rate);
        if (std::fabs(target - v) < eps)
            v = target;
        else
            c.Continue();
    };
    glide(scroll_, scrollTarget_, 16, 0.3f);
    glide(pageShown_, float(page_), 14, 0.002f);
    glide(folderPageShown_, float(folderPage_), 14, 0.002f);
    // Another size: the panel and the capsule spring to it, from the bottom left corner.
    // The window is the largest grid that fits (Size), so either size may grow
    // inside it; only the panel springs.
    const D2D1_SIZE_F target = PanelSize();
    const bool resizing = StepPanelSize(c, target);
    UpdateSurfaceRegion(target, resizing);

    const float offX = OffsetX(), offY = OffsetY();
    c.BeginGroup(1, 1, 0, 0, offX, offY);

    // The pieces of glass: the panel, the capsule (round like the bar) and,
    // while open, the folder's card over the tiles and the power card growing
    // up out of the capsule.
    const D2D1_RECT_F main = MainRect(), shelf = ShelfRect();
    std::vector<ShapeDip> shapes{{main, radius_}, {shelf, (shelf.bottom - shelf.top) / 2}};
    bool animating = false;
    float open = 0;
    D2D1_RECT_F card{};
    PaintFolderCard(c, shapes, dt_, open, card, animating);
    StepPowerCard(c, dt_, animating);
    if (c.panel) // no glass (yet): plain panels keep the text readable
        for (const ShapeDip& sh : shapes)
            c.Fill(sh.rect, sh.radius, t.panel);

    PaintSearch(c, t);
    // What the folder's card covers stays, blurred and dimmed under it.
    const float under = 1 - 0.45f * open;
    if (open > 0.01f)
        c.BeginBlur();
    if (Listing()) {
        PaintRows(c, t, RowsShown(), under);
        if (Indexed())
            PaintIndex(c, t);
    } else {
        PaintTiles(c, t, under);
    }
    if (open > 0.01f)
        c.EndBlur(6 * open);
    if (folder_ >= 0)
        PaintFolder(c, t, card, open);
    PaintHoverTip(c, t);
    PaintShelf(c, t);
    PaintGrip(c, t);
    if (drag_.on)
        PaintDragged(c, t);
    if (visibleIcons_ != askedIcons_)
        askedIcons_ = visibleIcons_, catalog_.SetVisibleIcons(VisibleIconsOwner::Start, visibleIcons_);
    PaintPowerCard(c, t, shapes);
    c.EndGroup();
    for (ShapeDip& sh : shapes)
        sh.rect.left += offX, sh.rect.right += offX, sh.rect.top += offY, sh.rect.bottom += offY;
    surface_.SetShapes(shapes, animating || resizing);
}

// Search: an outlined capsule at the top of the panel; typing anywhere goes
// here. Beside it the way to all the apps, A to Z (and back).
void StartMenu::PaintSearch(Canvas& c, const Theme& t)
{
    D2D1_RECT_F search = SearchRect();
    const float r = kSearchH / 2;
    float sh = Hover(c, L"search", hover_ == L"search" || !query_.empty());
    c.Fill(search, r, Alpha(t.card, 0.6f + 0.4f * sh));
    c.Stroke(search, r, query_.empty() ? t.border : Alpha(t.accent, 0.85f), query_.empty() ? 1.f : 1.5f);
    c.Glyph(L"\uE721", D2D1::RectF(search.left + 10, search.top, search.left + 44, search.bottom), 16, t.subtext);
    D2D1_RECT_F field = D2D1::RectF(search.left + 46, search.top, search.right - 48, search.bottom);
    const float maxW = field.right - field.left - 2;
    caretX_ = field.left + std::min(c.TextWidth(query_ + composition_, 15), maxW) + 1;
    const bool empty = query_.empty() && composition_.empty();
    c.Text(empty ? Tr(L"Search apps and folders") : query_ + composition_, field, 15, empty ? t.faint : t.text);
    if (!composition_.empty()) {
        const float x0 = field.left + std::min(c.TextWidth(query_, 15), maxW);
        if (caretX_ - 1 > x0)
            c.Line(x0, (field.top + field.bottom) / 2 + 10, caretX_ - 1, (field.top + field.bottom) / 2 + 10, t.text, 1);
    }
    const UINT blink = GetCaretBlinkTime();
    if (!naming_ && searchFocused_ && (blink == INFINITE || (ULONGLONG(c.now) / std::max(1u, blink)) % 2 == 0))
        c.Line(caretX_, search.top + 12, caretX_, search.bottom - 12, Alpha(t.accent, 0.9f), 1.5f);
    hits_.push_back({search, L"search"});
    if (!query_.empty()) {
        D2D1_RECT_F clear = D2D1::RectF(search.right - 40, search.top + 6, search.right - 8, search.bottom - 6);
        float hv = Hover(c, L"clear", hover_ == L"clear");
        c.Fill(clear, 16, Alpha(pressed_ == L"clear" ? t.press : t.hover, 0.5f + 0.5f * hv));
        c.Glyph(L"\uE711", clear, 10, t.text);
        hits_.push_back({clear, L"clear"});
    }

    const D2D1_RECT_F b = AllButton();
    const bool back = allApps_ && query_.empty();
    const std::wstring label = back ? Tr(L"Back") : Tr(L"All apps");
    float hv = Hover(c, L"allapps", hover_ == L"allapps");
    c.Fill(b, r, Alpha(pressed_ == L"allapps" ? t.press : t.hover, 0.45f + 0.55f * hv));
    const float tw = c.TextWidth(label, 13), x = (b.left + b.right - (16 + 6 + tw)) / 2;
    c.Glyph(back ? L"\uE72B" : L"\uE71D", D2D1::RectF(x, b.top, x + 16, b.bottom), 14, t.text);
    c.Text(label, D2D1::RectF(x + 22, b.top, b.right - 4, b.bottom), 13, t.text);
    hits_.push_back({b, L"allapps"});

    const D2D1_RECT_F sync = SyncButton();
    float sv = Hover(c, L"sync", hover_ == L"sync");
    c.Fill(sync, r, Alpha(pressed_ == L"sync" ? t.press : t.hover, 0.45f + 0.55f * sv));
    c.Glyph(L"\uE895", sync, 15, t.text);
    hits_.push_back({sync, L"sync"});
}

// `px` is always the largest icon size (IconPx): one bitmap per app serves
// every icon size, drawn into `r` at the size asked for. Changing the size
// therefore never reads an icon file or waits on the catalog.
void StartMenu::DrawIcon(Canvas& c, const std::wstring& id, int px, D2D1_RECT_F r, float radius, const std::wstring& name)
{
    static const IconImage kNone;
    visibleIcons_.emplace_back(id, px);
    std::wstring cacheKey = L"c|" + id + L"|" + std::to_wstring(px);
    bool cached = c.Cached(cacheKey);
    IconImage fetched;
    if (!cached)
        fetched = catalog_.Icon(id, px);
    const IconImage& icon = cached ? kNone : fetched;
    if (cached || !icon.Empty()) {
        c.Image(cacheKey, icon, r);
        return;
    }
    c.Fill(r, radius, c.theme.hover);
    for (wchar_t ch : name)
        if (!iswspace(ch)) {
            c.Text(std::wstring(1, iswalpha(ch) ? wchar_t(towupper(ch)) : ch), r, std::max(9.f, (r.bottom - r.top) * 0.45f), c.theme.text,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            break;
        }
}

void StartMenu::PaintTileNaming(Canvas& c, const Theme& t, const Tile& tile, D2D1_RECT_F r, D2D1_RECT_F lr)
{
    const float cx = (r.left + r.right) / 2;
    const D2D1_RECT_F field = D2D1::RectF(r.left, lr.top - 1, r.right, std::min(r.bottom, lr.top + LabelH() + 4));
    namingRect_ = field;
    c.Fill(field, 7, Alpha(t.card, 1));
    c.Stroke(field, 7, Alpha(t.accent, 0.85f), 1.5f);
    const std::wstring& shown = nameDraft_.empty() ? tile.app.name : nameDraft_;
    const float tw = std::min(c.TextWidth(shown, LabelSize()), field.right - field.left - 8);
    c.Text(shown, field, LabelSize(), nameDraft_.empty() ? t.faint : t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
    const float caretX = nameDraft_.empty() ? cx - tw / 2 : cx + tw / 2 + 1;
    c.Line(caretX, field.top + 3, caretX, field.bottom - 3, Alpha(t.accent, 0.9f), 1.5f);
}

void StartMenu::PaintTileFace(Canvas& c, const Theme& t, const Tile& tile, D2D1_RECT_F r, const std::wstring& id, bool selected, bool target)
{
    const double now = c.now;
    float hv = Hover(c, id, hover_ == id || selected);
    bool pressed = pressed_ == id && !drag_.on;
    D2D1_RECT_F plate = D2D1::RectF(r.left + 2, r.top + 1, r.right - 2, r.bottom - 1);
    if (hv > 0.001f)
        c.Fill(plate, 16, selected && hover_ != id ? Alpha(t.accent, 0.22f * hv) : Alpha(t.hover, hv));
    if (pressed)
        c.Fill(plate, 16, t.press);
    if (rippleId_ == id && now - rippleMs_ < kRippleMs) {
        c.Ripple(plate, 16, rippleX_, rippleY_, float((now - rippleMs_) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
        c.Continue();
    }
    // A carried app over it: it opens up to take it (a folder of the two).
    float into = Hover(c, id + L"#into", target);
    const float icon = IconSize(), ch = r.bottom - r.top;
    // The icon and the name under it, as one block, centred in the cell. The
    // name grows with the icon, so the block keeps its proportions.
    const float labelH = LabelH(), block = icon + 4 + labelH;
    float cx = (r.left + r.right) / 2, iy = r.top + std::max(6.f, (ch - block) / 2);
    D2D1_RECT_F ir = D2D1::RectF(cx - icon / 2, iy, cx + icon / 2, iy + icon);
    c.BeginGroup(1, pressed ? 0.94f : 1 + 0.08f * hv + 0.1f * into, cx, iy + icon, 0, -2 * hv);
    if (into > 0.01f) {
        D2D1_RECT_F ring = D2D1::RectF(ir.left - 5, ir.top - 5, ir.right + 5, ir.bottom + 5);
        c.Fill(ring, 15, Alpha(t.hover, into));
        c.Stroke(ring, 15, Alpha(t.accent, 0.8f * into), 1.5f);
    }
    if (tile.folder) {
        // A folder: a plate with its first four apps, each a quarter of the cover.
        // Their icons are the tiles' own, read ahead (SetStartIds): scaled down,
        // not fetched again.
        c.Fill(ir, 11, t.cardHot);
        const float gap = 3, m = (icon - gap) / 2 - 2, pad = (icon - 2 * m - gap) / 2;
        const int px = IconPx();
        for (size_t k = 0; k < std::min<size_t>(4, tile.apps.size()); ++k) {
            const size_t row = k / 2;
            float x = ir.left + pad + float(k % 2) * (m + gap), y = ir.top + pad + float(row) * (m + gap);
            DrawIcon(c, tile.apps[k].id, px, D2D1::RectF(x, y, x + m, y + m), 4, tile.apps[k].name);
        }
    } else {
        DrawIcon(c, tile.app.id, IconPx(), ir, 10, tile.name);
    }
    c.EndGroup();
    const D2D1_RECT_F lr = D2D1::RectF(r.left + 2, ir.bottom + 4, r.right - 2, r.bottom + 2);
    if (naming_ && id == namingHit_) {
        PaintTileNaming(c, t, tile, r, lr);
        return;
    }
    const bool twoLines = 2.f * (c.Caret(L"A", lr, LabelSize(), DWRITE_FONT_WEIGHT_NORMAL, 0).bottom - lr.top) <= (lr.bottom - lr.top) + 0.5f;
    c.Text(tile.name, lr, LabelSize(), t.text, DWRITE_FONT_WEIGHT_NORMAL, 1, twoLines);
    if (hover_ == id && id != L"#carried" && (folder_ < 0 || id.starts_with(L"fapp:")) && c.TextWidth(tile.name, LabelSize()) > (lr.right - lr.left) &&
        (!twoLines || c.Caret(tile.name, lr, LabelSize(), DWRITE_FONT_WEIGHT_NORMAL, tile.name.size()).bottom > lr.bottom + 0.5f))
        tipText_ = tile.name, tipRect_ = lr;
}

// The tiles, cols x rows per page; pages slide sideways. Tiles glide to new
// places (a carried tile, another size); `shown` fades them under the folder's card.
void StartMenu::PaintTiles(Canvas& c, const Theme& t, float shown)
{
    const float w = W();
    const double now = c.now;
    const std::vector<int> order = Order();
    if (order.empty()) {
        std::wstring msg = catalog_.Loaded() ? Tr(L"Nothing is pinned yet. Right-click an app in the search results or All apps to pin it.") : Tr(L"Reading…");
        c.Text(msg, D2D1::RectF(kSearchX, kGridTop + 60, w - kSearchX, kGridTop + 90), 13, Alpha(t.subtext, shown), DWRITE_FONT_WEIGHT_NORMAL, 1);
        return;
    }
    const int per = PerPage();
    const float cw = CellW(), ch = CellH();
    c.Clip(D2D1::RectF(0, kListTop - 4, w, std::min(kGridTop + float(rows_) * ch, MainRect().bottom - kGridFoot) + 4));
    for (int slot = 0; slot < int(order.size()); ++slot) {
        const int i = order[size_t(slot)];
        if (i < 0)
            continue;
        const Tile& tile = tiles_[size_t(i)];
        const D2D1_RECT_F home = Cell(slot);
        auto& p = tilePos_.try_emplace(tile.key, D2D1::Point2F(home.left, home.top)).first->second;
        for (auto [v, target] : {std::pair{&p.x, home.left}, std::pair{&p.y, home.top}}) {
            *v = motion::Approach(*v, target, dt_, kGlideRate);
            if (std::fabs(target - *v) < 0.3f)
                *v = target;
            else
                c.Continue();
        }
        const float x = p.x - pageShown_ * w, y = p.y;
        if (x + cw < 0 || x > w)
            continue;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + cw, y + ch);
        float e = motion::EaseOut(float((now - stageMs_ - kStaggerMs * std::min(slot % per, 18)) / kStageMs));
        if (e < 1)
            c.Continue();
        float vis = Covered(r) * shown;
        if (vis <= 0.01f)
            continue;
        c.BeginGroup(e * vis, 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 0, 10 * (1 - e));
        const std::wstring id = L"tile:" + std::to_wstring(i);
        PaintTileFace(c, t, tile, r, id, i == sel_ && folder_ < 0, drag_.on && drag_.merge == i);
        c.EndGroup();
        if (folder_ < 0 && !drag_.on && slot / per == page_)
            hits_.push_back({r, id});
    }
    c.Unclip();
    if (shown > 0.01f)
        PaintDots(c, t, Pages(), pageShown_, MainRect().bottom - 26, L"page:");
}

// Page dots; the current one is drawn out into a short capsule.
void StartMenu::PaintDots(Canvas& c, const Theme& t, int pages, float current, float y, const wchar_t* prefix)
{
    if (pages <= 1)
        return;
    const float dot = 6, gapX = 8;
    float total = 0;
    std::vector<float> widths(static_cast<size_t>(pages));
    for (int p = 0; p < pages; ++p) {
        float on = std::max(0.f, 1 - std::fabs(current - float(p)));
        widths[size_t(p)] = dot + 12 * on;
        total += widths[size_t(p)] + (p ? gapX : 0);
    }
    float x = (W() - total) / 2;
    for (int p = 0; p < pages; ++p) {
        float on = std::max(0.f, 1 - std::fabs(current - float(p)));
        std::wstring id = prefix + std::to_wstring(p);
        float hv = Hover(c, id, hover_ == id);
        D2D1_RECT_F d = D2D1::RectF(x, y - dot / 2, x + widths[size_t(p)], y + dot / 2);
        D2D1_COLOR_F col = t.faint;
        col.a = t.faint.a + (t.text.a - t.faint.a) * std::max(on, 0.5f * hv);
        c.Fill(d, dot / 2, col);
        hits_.push_back({D2D1::RectF(d.left - gapX / 2, y - 10, d.right + gapX / 2, y + 10), id});
        x = d.right + gapX;
    }
}

void StartMenu::PaintRowHeading(Canvas& c, const Theme& t, D2D1_RECT_F r, wchar_t letter)
{
    // A letter's heading (or the hidden apps').
    const std::wstring label = letter == kStartMenuHiddenHead ? Tr(L"Hidden") : std::wstring(1, letter);
    const float tw = std::max(c.TextWidth(label, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD), 10.f);
    D2D1_RECT_F chip = D2D1::RectF(r.left + 6, r.top + 6, r.left + 6 + tw + 20, r.bottom - 4);
    c.Fill(chip, (chip.bottom - chip.top) / 2, Alpha(t.hover, 0.35f));
    c.Text(label, chip, 13, t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
}

void StartMenu::PaintRowItem(Canvas& c, const Theme& t, const Row& row, D2D1_RECT_F r, size_t index, bool firstIsDefault, int rowPx)
{
    const double now = c.now;
    const std::wstring id = L"row:" + std::to_wstring(index);
    const bool selected = int(index) == sel_ || (firstIsDefault && index == 0);
    float hv = Hover(c, id, hover_ == id || selected);
    D2D1_RECT_F plate = D2D1::RectF(r.left, r.top + 2, r.right, r.bottom - 2);
    const float pr = (plate.bottom - plate.top) / 2;
    if (hv > 0.001f)
        c.Fill(plate, pr, selected && hover_ != id ? Alpha(t.accent, 0.22f * hv) : Alpha(t.hover, hv));
    if (pressed_ == id)
        c.Fill(plate, pr, t.press);
    if (rippleId_ == id && now - rippleMs_ < kRippleMs) {
        c.Ripple(plate, pr, rippleX_, rippleY_, float((now - rippleMs_) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
        c.Continue();
    }
    float iy = (r.top + r.bottom) / 2 - kRowIcon / 2;
    D2D1_RECT_F ir = D2D1::RectF(r.left + 14, iy, r.left + 14 + kRowIcon, iy + kRowIcon);
    c.BeginGroup(1, pressed_ == id ? 0.94f : 1 + 0.06f * hv, (ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2);
    std::wstring name, note;
    if (row.folder >= 0) {
        const Tile& f = tiles_[size_t(row.folder)];
        c.Fill(ir, 8, t.cardHot);
        const float m = 10, gap = 2, pad = (kRowIcon - 2 * m - gap) / 2;
        const int px = int(std::lround(m * c.scale * 1.1f));
        for (size_t k = 0; k < std::min<size_t>(4, f.apps.size()); ++k) {
            const size_t line = k / 2;
            float x = ir.left + pad + float(k % 2) * (m + gap), yy = ir.top + pad + float(line) * (m + gap);
            DrawIcon(c, f.apps[k].id, px, D2D1::RectF(x, yy, x + m, yy + m), 3, f.apps[k].name);
        }
        name = f.name;
        note = TrFormat(L"%d apps", int(f.apps.size()));
    } else {
        DrawIcon(c, row.app.id, rowPx, ir, 7, row.app.name);
        name = row.app.name;
    }
    c.EndGroup();
    c.Text(name, D2D1::RectF(r.left + 56, r.top, r.right - 100, r.bottom), 14, t.text);
    if (selected && !query_.empty())
        note = Tr(L"Enter to open");
    if (!note.empty())
        c.Text(note, D2D1::RectF(r.right - 110, r.top, r.right - 18, r.bottom), 12, t.faint, DWRITE_FONT_WEIGHT_NORMAL, 2);
}

// Rows: the search results (folders, then apps; the first or the selected one
// opens with Enter), or every app from A to Z under its letter's heading.
void StartMenu::PaintRows(Canvas& c, const Theme& t, const std::vector<Row>& rows, float shown)
{
    const float w = W(), bottom = ListBottom();
    const double now = c.now;
    contentH_ = 0;
    for (const Row& row : rows)
        contentH_ += row.letter ? kHeadH : kRowH;
    const float viewH = bottom - kListTop;
    scrollTarget_ = std::clamp(scrollTarget_, 0.f, std::max(0.f, contentH_ - viewH));
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, contentH_ - viewH));
    if (rows.empty()) {
        std::wstring msg = !query_.empty() ? Tr(L"No apps found") : catalog_.Loaded() ? Tr(L"No apps") : Tr(L"Reading…");
        c.Text(msg, D2D1::RectF(kSearchX, kGridTop + 60, w - kSearchX, kGridTop + 90), 13, Alpha(t.subtext, shown), DWRITE_FONT_WEIGHT_NORMAL, 1);
        return;
    }
    if (shown <= 0.01f)
        return;
    const int rowPx = int(std::lround(kRowIcon * c.scale * 1.1f));
    const bool firstIsDefault = !query_.empty() && sel_ < 0;
    c.Clip(D2D1::RectF(0, kListTop, w, bottom));
    int staged = 0;
    float y = kListTop - scroll_;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const float h = row.letter ? kHeadH : kRowH;
        D2D1_RECT_F r = D2D1::RectF(Indexed() ? IndexRect().right + 6 : 16, y, w - 16, y + h);
        y += h;
        if (r.bottom < kListTop || r.top > bottom)
            continue;
        float e = motion::EaseOut(float((now - stageMs_ - kStaggerMs * std::min(staged++, 8)) / kStageMs));
        if (e < 1)
            c.Continue();
        float visible = Covered(r) * shown;
        if (visible <= 0.01f)
            continue;
        c.BeginGroup(e * visible, 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 0, 8 * (1 - e));
        if (row.letter) {
            PaintRowHeading(c, t, r, row.letter);
            c.EndGroup();
            continue;
        }
        PaintRowItem(c, t, row, r, i, firstIsDefault, rowPx);
        c.EndGroup();
        hits_.push_back({r, L"row:" + std::to_wstring(i)});
    }
    c.Unclip();
    if (contentH_ > viewH + 1) {
        float th = std::max(28.f, viewH * viewH / contentH_);
        float ty = kListTop + (viewH - th) * (scroll_ / (contentH_ - viewH));
        c.Fill(D2D1::RectF(w - 9, ty, w - 5, ty + th), 2, Alpha(t.faint, shown));
    }
}

// The A-Z list's letters down its left side: those with apps under them in
// the ink, the others faint, the one at the top of the list in the accent.
// Pressed or dragged along, the list glides to the letter, and the letter
// shows large beside the pointer. Too little room for all of them: every
// other one is a dot (it can still be picked).
void StartMenu::PaintIndex(Canvas& c, const Theme& t)
{
    std::set<wchar_t> present;
    for (const Row& row : list_)
        if (row.letter)
            present.insert(row.letter);
    const D2D1_RECT_F bar = IndexRect();
    const int n = int(std::size(kLetters)) - 1;
    const float step = (bar.bottom - bar.top) / float(n), cx = (bar.left + bar.right) / 2;
    const bool compact = step < 12;
    const wchar_t current = scrubbing_ ? scrubLetter_ : TopLetter();
    float hv = Hover(c, L"azbar", hover_ == L"azbar" || scrubbing_);
    if (hv > 0.001f)
        c.Fill(bar, kIndexW / 2, Alpha(t.hover, 0.7f * hv));
    for (int i = 0; i < n; ++i) {
        const wchar_t letter = kLetters[i];
        const float y = bar.top + step * (float(i) + 0.5f);
        const D2D1_COLOR_F ink = letter == current ? t.accent : present.contains(letter) ? t.subtext : t.faint;
        if (compact && i % 2 && letter != current) {
            c.Circle(cx, y, 1.4f, ink);
            continue;
        }
        c.Text(std::wstring(1, letter), D2D1::RectF(bar.left, y - 8, bar.right, y + 8), compact ? 9.5f : 10.5f, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    hits_.push_back({bar, L"azbar"});

    // The letter picked, large, following the pointer.
    const float hint = Hover(c, L"#hint", scrubbing_);
    if (hint <= 0.01f || !scrubLetter_)
        return;
    const float y = std::clamp(scrubY_, kListTop + kHint / 2, ListBottom() - kHint / 2);
    hintY_ = hint < 0.05f ? y : motion::Approach(hintY_, y, dt_, 30);
    if (std::fabs(hintY_ - y) > 0.3f)
        c.Continue();
    const D2D1_RECT_F b = D2D1::RectF(bar.right + 12, hintY_ - kHint / 2, bar.right + 12 + kHint, hintY_ + kHint / 2);
    c.BeginGroup(hint, 0.8f + 0.2f * hint, b.left, (b.top + b.bottom) / 2);
    c.Fill(b, 18, Alpha(t.accent, 0.94f));
    bool halo = c.halo;
    c.halo = false;
    c.Text(std::wstring(1, scrubLetter_), b, 26, t.accentText, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.halo = halo;
    c.EndGroup();
}

void StartMenu::PaintFolderTitle(Canvas& c, const Theme& t, const Tile& f, D2D1_RECT_F full)
{
    // The name; while it is being edited, a field with a caret.
    D2D1_RECT_F title = D2D1::RectF(full.left + 24, full.top + 12, full.right - 24, full.top + 46);
    const std::wstring shownName = naming_ ? nameDraft_ : f.name;
    const float tw = std::min(c.TextWidth(shownName.empty() ? f.name : shownName, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD), title.right - title.left - 40);
    const float cx = (title.left + title.right) / 2;
    D2D1_RECT_F field = D2D1::RectF(cx - tw / 2 - 18, title.top, cx + tw / 2 + 18, title.bottom);
    if (naming_) {
        field = D2D1::RectF(std::max(title.left, cx - std::max(tw / 2 + 18, 90.f)), title.top, std::min(title.right, cx + std::max(tw / 2 + 18, 90.f)),
                            title.bottom);
        c.Fill(field, 17, Alpha(t.card, 1));
        c.Stroke(field, 17, Alpha(t.accent, 0.85f), 1.5f);
        if (nameDraft_.empty())
            c.Text(f.name, title, 17, t.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        else
            c.Text(nameDraft_, title, 17, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        const float caretX = nameDraft_.empty() ? cx - tw / 2 : cx + tw / 2 + 1;
        c.Line(caretX, title.top + 8, caretX, title.bottom - 8, Alpha(t.accent, 0.9f), 1.5f);
    } else {
        float hv = Hover(c, L"ftitle", hover_ == L"ftitle");
        if (hv > 0.001f)
            c.Fill(field, 17, Alpha(t.hover, hv));
        c.Text(f.name, title, 17, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    hits_.push_back({field, L"ftitle"});
}

void StartMenu::PaintFolderApps(Canvas& c, const Theme& t, const Tile& f, D2D1_RECT_F full)
{
    const std::vector<int> order = FolderOrder();
    const int per = FolderCols() * FolderRows();
    const float pageW = full.right - full.left;
    c.Clip(D2D1::RectF(full.left + 2, full.top + kFolderHead - 6, full.right - 2, full.bottom - kFolderFoot + 6));
    for (int slot = 0; slot < int(order.size()); ++slot) {
        const int i = order[size_t(slot)];
        if (i < 0)
            continue;
        const CatalogApp& app = f.apps[size_t(i)];
        const D2D1_RECT_F home = FolderCell(slot);
        auto& p = tilePos_.try_emplace(L"f|" + app.id, D2D1::Point2F(home.left, home.top)).first->second;
        for (auto [v, target] : {std::pair{&p.x, home.left}, std::pair{&p.y, home.top}}) {
            *v = motion::Approach(*v, target, dt_, kGlideRate);
            if (std::fabs(target - *v) < 0.3f)
                *v = target;
            else
                c.Continue();
        }
        const float x = p.x - folderPageShown_ * pageW;
        if (x + CellW() < full.left || x > full.right)
            continue;
        D2D1_RECT_F r = D2D1::RectF(x, p.y, x + CellW(), p.y + CellH());
        const std::wstring id = L"fapp:" + std::to_wstring(i);
        // Opening, the apps pop up one after another.
        const float e = folderClosing_ ? 1.f : float((c.now - folderMs_ - 70 - kFolderAppStaggerMs * std::min(slot % per, 12)) / kFolderAppMs);
        if (e < 1)
            c.Continue();
        c.BeginGroup(motion::EaseOut(e * 1.4f), 0.6f + 0.4f * motion::EaseOutBack(e, 1.6f), (r.left + r.right) / 2, (r.top + r.bottom) / 2);
        PaintTileFace(c, t, Tile{app.id, app.name, app, {}, false}, r, id, i == sel_, false);
        c.EndGroup();
        if (!drag_.on && !folderClosing_ && slot / per == folderPage_)
            hits_.push_back({r, id});
    }
    c.Unclip();
}

// The open folder: its name (click to rename) and its apps, pages of them.
void StartMenu::PaintFolder(Canvas& c, const Theme& t, D2D1_RECT_F card, float shown)
{
    if (folder_ < 0)
        return;
    const Tile& f = tiles_[size_t(folder_)];
    if (!folderClosing_)
        hits_.push_back({card, L"fcard"});
    const float a = std::clamp((shown - 0.3f) / 0.7f, 0.f, 1.f);
    if (a <= 0.01f)
        return;
    const D2D1_RECT_F full = FolderRect();
    c.BeginGroup(a, 0.94f + 0.06f * a, (full.left + full.right) / 2, (full.top + full.bottom) / 2); // settles in with the card
    PaintFolderTitle(c, t, f, full);
    PaintFolderApps(c, t, f, full);
    const int pages = FolderPages();
    if (pages > 1) {
        // The dots under the card's apps (centred on the panel, as the card is).
        PaintDots(c, t, pages, folderPageShown_, full.bottom - 16, L"fpage:");
    }
    c.EndGroup();
}

// The carried tile, over everything, a little larger.
void StartMenu::PaintDragged(Canvas& c, const Theme& t)
{
    Tile tile;
    if (drag_.fromFolder) {
        if (drag_.folderTile < 0 || drag_.folderTile >= int(tiles_.size()) || drag_.from >= int(tiles_[size_t(drag_.folderTile)].apps.size()))
            return;
        const CatalogApp& app = tiles_[size_t(drag_.folderTile)].apps[size_t(drag_.from)];
        tile = {app.id, app.name, app, {}, false};
    } else {
        if (drag_.from < 0 || drag_.from >= int(tiles_.size()))
            return;
        tile = tiles_[size_t(drag_.from)];
    }
    D2D1_RECT_F r = D2D1::RectF(drag_.x - drag_.grabX, drag_.y - drag_.grabY, drag_.x - drag_.grabX + CellW(), drag_.y - drag_.grabY + CellH());
    c.BeginGroup(0.95f, 1.08f, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
    PaintTileFace(c, t, tile, r, L"#carried", false, false);
    c.EndGroup();
}

// The grip that sizes the grid: an arc in the panel's top right corner,
// concentric with it (as the widgets' grips), faint while the pointer is on
// the menu and clear on the grip itself.
void StartMenu::PaintGrip(Canvas& c, const Theme& t)
{
    const D2D1_RECT_F m = MainRect();
    const float inMenu = Hover(c, L"#menu", tracking_ || sizing_), on = Hover(c, L"resize", hover_ == L"resize" || sizing_);
    const float a = std::max(0.4f * inMenu, on);
    const BarEdge edge = EdgeOf(bar_);
    const bool left = edge == BarEdge::Right, bottom = edge != BarEdge::Bottom;
    hits_.push_back(
        {D2D1::RectF(left ? m.left : m.right - kGrip, bottom ? m.bottom - kGrip : m.top, left ? m.left + kGrip : m.right, bottom ? m.bottom : m.top + kGrip),
         L"resize"});
    if (a <= 0.01f)
        return;
    const float r = std::max(radius_, 18.f);
    float a0 = -80.f;
    if (left)
        a0 = 100.f;
    else if (bottom)
        a0 = 10.f;
    c.Arc(left ? m.left + r : m.right - r, bottom ? m.bottom - r : m.top + r, r - 6, a0, a0 + 70.f, Alpha(t.subtext, a), 3.5f);
}

void StartMenu::PaintShelfButton(Canvas& c, const Theme& t, float& x, float cy, float bh, const std::wstring& id, const std::wstring& glyph,
                                 const std::wstring& label, bool dim)
{
    const float tw = label.empty() ? 0 : c.TextWidth(label, 12) + 4;
    const float bw = label.empty() ? bh : 12 + 20 + 4 + tw + 12;
    const D2D1_RECT_F b = D2D1::RectF(x - bw, cy - bh / 2, x, cy + bh / 2);
    const float hv = Hover(c, id, hover_ == id || (id == L"power" && powerOpen_));
    if (hv > 0)
        c.Fill(b, bh / 2, Alpha(pressed_ == id ? t.press : t.hover, hv));
    const D2D1_RECT_F g = label.empty() ? b : D2D1::RectF(b.left + 10, b.top, b.left + 34, b.bottom);
    if (id == L"network" && status_.connected && status_.wifi)
        c.WifiFan((g.left + g.right) / 2, cy, 12, WifiSteps(status_.signal), t.text);
    else
        c.Glyph(glyph, g, 16, dim ? t.faint : t.text);
    if (!label.empty())
        c.Text(label, D2D1::RectF(b.left + 36, b.top, b.right - 8, b.bottom), 12, dim ? t.faint : t.subtext);
    hits_.push_back({b, id});
    x = b.left - 2;
}

void StartMenu::PaintShelfAccount(Canvas& c, const Theme& t, float left, float maxRight, float cy, float bh, const D2D1_RECT_F& shelf)
{
    const std::wstring name = userName_.empty() ? Tr(L"User") : userName_;
    const float nw = std::clamp(c.TextWidth(name, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD), 0.f, std::max(0.f, maxRight - left - 70));
    const D2D1_RECT_F acct = D2D1::RectF(left, cy - bh / 2, left + 8 + 28 + 10 + nw + 16, cy + bh / 2);
    const float ah = Hover(c, L"account", hover_ == L"account");
    if (ah > 0)
        c.Fill(acct, bh / 2, Alpha(pressed_ == L"account" ? t.press : t.hover, ah));
    const D2D1_RECT_F pic = D2D1::RectF(acct.left + 8, cy - 14, acct.left + 36, cy + 14);
    if (!avatar_.Empty()) {
        c.ImageCircle(L"avatar", avatar_, pic);
    } else {
        c.Circle((pic.left + pic.right) / 2, cy, 14, Alpha(t.accent, 0.9f));
        const bool halo = c.halo;
        c.halo = false;
        c.Text(name.substr(0, 1), pic, 13, t.accentText, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.halo = halo;
    }
    c.Text(name, D2D1::RectF(pic.right + 10, shelf.top, pic.right + 10 + nw + 2, shelf.bottom), 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hits_.push_back({acct, L"account"});
}

// The capsule: the account on the left; network, volume, battery (unless turned
// off: startStatus), the tray (startTray) and power on the right, round buttons like the bar's.
void StartMenu::PaintShelf(Canvas& c, const Theme& t)
{
    const D2D1_RECT_F shelf = ShelfRect();
    const float cy = (shelf.top + shelf.bottom) / 2, bh = ButtonSize();
    const float edge = (shelf.bottom - shelf.top - bh) / 2; // concentric with the capsule

    // Right side, built from the right edge.
    float x = shelf.right - edge;
    PaintShelfButton(c, t, x, cy, bh, L"power", L"\uE7E8", L"", false);
    if (showTray_) {
        c.Line(x - 4, cy - 12, x - 4, cy + 12, t.divider);
        x -= 8;
        PaintShelfButton(c, t, x, cy, bh, L"tray", L"\uE70E", L"", false);
    }
    if (showStatus_ && status_.battery) {
        const int level = std::clamp(status_.batteryPercent / 10, 0, 10);
        const std::wstring glyph(1, wchar_t((status_.charging ? 0xEBAB : 0xEBA0) + level));
        PaintShelfButton(c, t, x, cy, bh, L"battery", glyph, std::to_wstring(status_.batteryPercent) + L"%", false);
    }
    if (showStatus_ && status_.audio) {
        const wchar_t* volGlyph = status_.muted ? L"\uE74F" : status_.volume < 34 ? L"\uE993" : status_.volume < 67 ? L"\uE994" : L"\uE995";
        PaintShelfButton(c, t, x, cy, bh, L"volume", volGlyph, L"", status_.muted);
    }
    if (showStatus_) {
        const wchar_t* netGlyph = !status_.connected ? L"\uEB55" : status_.wifi ? L"\uE701" : L"\uE839";
        PaintShelfButton(c, t, x, cy, bh, L"network", netGlyph, L"", !status_.connected);
    }

    // Account, in what is left.
    PaintShelfAccount(c, t, shelf.left + edge, x, cy, bh, shelf);
}

} // namespace atrium
