using System.Numerics;
using System.Text.Json.Nodes;
using Microsoft.UI;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The workspace previews other than the bar: the shape's numbers, the notification cards as they
// pop up, and the desktop's widget layout (read from the core's widgets.json, laid out by its rules).
public sealed partial class MainWindow
{
    // ---- Shape: the geometry, read out ----

    private readonly List<(TextBlock Value, TextBlock Note)> _readout = [];

    private void UpdateReadout()
    {
        if (_page != 1) return;
        if (_readout.Count == 0)
        {
            foreach (string? name in new[] { Loc.T("Height"), Loc.T("Corner radius"), Loc.T("Width"), Loc.T("Outline") })
            {
                var value = new TextBlock { Style = StyleOf("TitleStyle") };
                Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(value, FontNumeralAlignment.Tabular);
                var note = Text("", "FootnoteStyle");
                var cell = new StackPanel { Spacing = 2 };
                cell.Children.Add(Text(name, "RowNoteStyle"));
                cell.Children.Add(value);
                cell.Children.Add(note);
                ShapeReadout.Children.Add(cell);
                _readout.Add((value, note));
            }
        }

        var s = CurrentSettings;
        double r = Math.Min(s.CornerRadius, s.Height / 2);
        bool capsule = s.CornerRadius >= s.Height / 2;
        _readout[0].Value.Text = $"{s.Height:0} px";
        _readout[0].Note.Text = Loc.F("Icon {0:0} px", s.IconSize);
        _readout[1].Value.Text = $"{r:0} px";
        _readout[1].Note.Text = s.CornerRadius > r ? Loc.F("Set to {0:0}, at most half the height", s.CornerRadius)
            : capsule ? Loc.T("G3 continuous capsule outline") : Loc.T("G3 continuous corners");
        _readout[2].Value.Text = s.AutoWidth ? Loc.T("Auto") : $"{s.Width:0} px";
        _readout[2].Note.Text = s.AutoWidth ? Loc.F("Up to {0:0} px", s.Width) : Loc.T("Fixed width");
        _readout[3].Value.Text = capsule ? Loc.T("Capsule") : r < 1 ? Loc.T("Square") : Loc.T("Rounded rectangle");
        _readout[3].Note.Text = Loc.F("{0:0} px from the screen edge", s.Margin);
    }

    // Notification previews follow the selected live layout and its existing motion curves.

    private const double CardW = 384, CardH = 112, CardPeek = 10, CardInset = 12;
    private bool _notifyDark, _notifyBackDark, _notifyPreviewReady, _notifyPreviewRendered;
    private bool _notifyEntrancePending;

    private void ShowNotifyPreview(bool animate)
    {
        if (!_windowLoaded) return;
        if (ContinuousCorners.Original(NotifyStage) is not ImageBrush) NotifyStage.Background = PhotoBrush();
        var s = CurrentSettings;
        double radius = Math.Min(Math.Clamp(s.CornerRadius, 8, Math.Max(8, s.Height / 2)), CardH / 2);
        double w = NotifyStage.ActualWidth, h = NotifyStage.ActualHeight;
        if (w < CardW + 48 || h < CardH + 60) { w = Math.Max(w, CardW + 48); h = Math.Max(h, CardH + 60); }
        var (corner, left, bottom) = NotifyCorner(s.NotifyPlacement);
        // Over the bar, right-aligned with it (the bell's popup); or a corner of the screen, the
        // newest card at the corner and the one before it beside it in the column.
        // At a corner the bar is shown smaller: the column has the room beside and above it.
        // At a corner the picture is the whole screen, smaller: the cards at the bar's scale, so that
        // where they stand (which corner, over the bar or at the top) shows at a glance.
        var bar = ShowNotifyBar(w, h, corner);
        double k = corner ? bar.S : 1;
        _notifyCardScale = k;
        double cw = CardW * k, ch = CardH * k, peek = CardPeek * k, edge = 18;
        double barRight = Math.Min(bar.BarX + bar.BarW, w - 16);
        double bottomY = bar.BarY - 12 * bar.S - ch;
        var front = corner ? new GlassTile(left ? edge : w - edge - cw, bottom ? bottomY : edge, cw, ch, radius * k)
            : new GlassTile(barRight - CardW, bottomY, CardW, CardH, radius);
        var back = corner ? new GlassTile(front.X, bottom ? front.Y - peek - ch : front.Y + ch + peek, cw, ch, radius * k)
            : new GlassTile(front.X + CardInset, front.Y - CardPeek, CardW - 2 * CardInset, CardH, radius);
        ScaleNotifyText(NotifyFrontText, k);
        ScaleNotifyText(NotifyBackText, k);

        NotifyCards.Width = w;
        NotifyCards.Height = h;
        NotifyPreviewClip.Rect = new Rect(0, 0, w, h);
        if (!_notifyPreviewReady)
        {
            ElementCompositionPreview.GetElementVisual(NotifyCards).Opacity = 0;
            _notifyPreviewReady = true;
        }
        var backCard = NotifyBackCard;
        var frontCard = NotifyFrontCard;
        const double Pad = GlassSpec.InsetDip;
        backCard.Width = back.W + 2 * Pad; backCard.Height = back.H + 2 * Pad;
        frontCard.Width = front.W + 2 * Pad; frontCard.Height = front.H + 2 * Pad;
        UpdateNotifyText(s);
        NotifyBackText.Visibility = corner ? Visibility.Visible : Visibility.Collapsed;
        Canvas.SetLeft(backCard, back.X - Pad); Canvas.SetTop(backCard, back.Y - Pad);
        Canvas.SetLeft(frontCard, front.X - Pad); Canvas.SetTop(frontCard, front.Y - Pad);
        _notifyEntrancePending |= animate;
        if (_notifyEntrancePending && _notifyPreviewRendered)
        {
            // Unseen until their glass for the new place has come; the entrance then shows them (else
            // they would stand there a moment with the glass of where they were).
            var cards = ElementCompositionPreview.GetElementVisual(NotifyCards);
            cards.StopAnimation("Opacity");
            cards.Opacity = 0;
        }
        string caption = s.NotifyAlways
            ? corner
                ? s.NotifyHideBody ? "Main screen corner · new notifications slide in · stays until dismissed · content hidden"
                                   : "Main screen corner · new notifications slide in · stays until dismissed"
                : s.NotifyHideBody ? "Follow the taskbar · stays until dismissed · content hidden" : "Follow the taskbar · stays until dismissed"
            : corner
                ? s.NotifyHideBody ? "Main screen corner · new notifications slide in · hides after {0:0} s · content hidden"
                                   : "Main screen corner · new notifications slide in · hides after {0:0} s"
                : s.NotifyHideBody ? "Follow the taskbar · hides after {0:0} s · content hidden" : "Follow the taskbar · hides after {0:0} s";
        NotifyCaption.Text = s.NotifyAlways ? Loc.T(caption) : Loc.F(caption, s.NotifyTimeout);

        // Their glass: rendered over the picture where they rest; each card carries its own piece.
        PostTiles(NotifyCards, w, h, 1, [back, front], null, f =>
        {
            NotifyBackImage.Source = f.Overlay(0);
            NotifyFrontImage.Source = f.Overlay(1);
            _notifyPreviewRendered = true;
            bool dark = _notifyDark ? f.Lumas[1] >= 0.42f : f.Lumas[1] > 0.50f;
            bool backDark = _notifyBackDark ? f.Lumas[0] >= 0.42f : f.Lumas[0] > 0.50f;
            if (dark != _notifyDark || backDark != _notifyBackDark)
            {
                _notifyDark = dark;
                _notifyBackDark = backDark;
                UpdateNotifyText(s);
            }
            if (_notifyEntrancePending)
            {
                _notifyEntrancePending = false;
                ReplayNotify();
            }
            else
            {
                ElementCompositionPreview.GetElementVisual(NotifyCards).Opacity = 1;
            }
        }, overlays: true);
    }

    private BarFace? _notifyFace;
    private double _notifyBarScale; // the scale of the bar shown (0: none yet)
    private const double SmallNotifyBar = 0.55; // the whole screen at a corner, in the picture's height
    private double _notifyCardScale = 1;
    private readonly Dictionary<FrameworkElement, (double Left, double Top, double Width, double Height, double Font)> _notifyTextAt = [];

    // A card's text at the cards' scale: places, widths and sizes (a transform would be cut by its layout).
    private void ScaleNotifyText(Canvas text, double k)
    {
        foreach (var child in text.Children.OfType<FrameworkElement>().Prepend(text))
        {
            if (!_notifyTextAt.TryGetValue(child, out var at))
                _notifyTextAt[child] = at = (Canvas.GetLeft(child), Canvas.GetTop(child), child.Width, child.Height, child is TextBlock t ? t.FontSize : 0);
            if (child != text)
            {
                Canvas.SetLeft(child, at.Left * k);
                Canvas.SetTop(child, at.Top * k);
            }
            if (!double.IsNaN(at.Width)) child.Width = at.Width * k;
            if (!double.IsNaN(at.Height)) child.Height = at.Height * k;
            if (child is TextBlock block && at.Font > 0) block.FontSize = Math.Max(1.0, at.Font * k);
        }
    }

    private void ResizeNotifyBar(BarGeometry bar, float from)
    {
        var visual = ElementCompositionPreview.GetElementVisual(NotifyBar);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)(bar.BarX + bar.BarW / 2), (float)(bar.BarY + bar.BarH), 0); // its bottom stays
        var scale = compositor.CreateVector3KeyFrameAnimation();
        var ease = compositor.CreateCubicBezierEasingFunction(new Vector2(0.2f, 0f), new Vector2(0f, 1f));
        scale.InsertKeyFrame(0, new Vector3(from, from, 1));
        scale.InsertKeyFrame(1, Vector3.One, ease);
        scale.Duration = TimeSpan.FromMilliseconds(380);
        visual.StartAnimation("Scale", scale);
    }
    private readonly Image _notifyBarGlass = new() { Stretch = Stretch.Fill, IsHitTestVisible = false };

    // The bar at the bottom of the notifications' picture, as on the pages of its parts.
    private BarGeometry ShowNotifyBar(double w, double h, bool small)
    {
        if (_notifyFace == null)
        {
            NotifyBar.Children.Add(_notifyBarGlass);
            _notifyFace = new BarFace(NotifyBar, MaxPreviewApps);
            foreach (var icon in _notifyFace.Icons)
                icon.SetBinding(Image.SourceProperty, new Microsoft.UI.Xaml.Data.Binding { Path = new PropertyPath(nameof(PinVm.Icon)) });
        }
        NotifyBar.Width = w;
        NotifyBar.Height = h;
        NotifyBar.Clip = new RectangleGeometry { Rect = new Rect(0, 0, w, h) };
        double scale = Math.Min(1.0, (w - 48) / (HeroReferenceW + 2 * GlassSpec.InsetDip)) * (small ? SmallNotifyBar : 1);
        var bar = BarLayout(CurrentSettings, w, h, scale, false);
        var tile = new GlassTile(bar.BarX, bar.BarY, bar.BarW, bar.BarH, bar.Radius);
        PostTiles(NotifyBar, w, h, scale, [tile], null, f =>
        {
            var glass = f.Overlays![0];
            _notifyBarGlass.Source = f.Overlay(0);
            _notifyBarGlass.Width = glass.Width / f.Rs;
            _notifyBarGlass.Height = glass.Height / f.Rs;
            Canvas.SetLeft(_notifyBarGlass, tile.X + tile.W / 2 - _notifyBarGlass.Width / 2);
            Canvas.SetTop(_notifyBarGlass, tile.Y + tile.H / 2 - _notifyBarGlass.Height / 2);
            bool dark = CurrentSettings.TextColor == "dark" || (CurrentSettings.TextColor == "auto" && glass.Luma > 0.5f);
            int running = CurrentSettings.ShowRunning && bar.N > _previewPins.Length ? _previewPins.Length : -1;
            for (int i = 0; i < MaxPreviewApps; i++)
            {
                var vm = i < _previewPins.Length ? _previewPins[i] : _previewRunning;
                if (_notifyFace.Icons[i].DataContext != vm) _notifyFace.Icons[i].DataContext = vm;
            }
            var clock = PreviewClock.Now;
            _notifyFace.SetClock(clock.ToString(CurrentSettings.ShowClockSeconds ? "H:mm:ss" : "H:mm"), clock.ToString("yyyy/M/d"));
            _notifyFace.Layout(bar, dark, bar.N, running, 2, 1, 0, 0, StatusLook.Current(), CurrentSettings.LaunchpadIcon);
            // Its new size has come: it grows or shrinks there from the size it had, on the
            // compositor (one picture, scaled: nothing is drawn again meanwhile).
            if (_notifyBarScale > 0 && Math.Abs(_notifyBarScale - scale) > 0.001)
                ResizeNotifyBar(bar, (float)(_notifyBarScale / scale));
            _notifyBarScale = scale;
        }, overlays: true, scaled: true);
        return bar;
    }

    private void UpdateNotifyText(BarSettings settings)
    {
        NotifyFrontText.Margin = NotifyBackText.Margin = new Thickness(GlassSpec.InsetDip);
        NotifyFrontBody.Text = settings.NotifyHideBody ? Loc.T("Content hidden") : Loc.T("Is tomorrow's time set? I'm free all morning, or after 3 pm.");
        NotifyBackBody.Text = settings.NotifyHideBody ? Loc.T("Content hidden") : Loc.T("3 pm, project review.");
        SetNotifyInk(_notifyDark, settings.NotifyHideBody, NotifyFrontApp, NotifyFrontTime, NotifyFrontTitle, NotifyFrontBody);
        SetNotifyInk(_notifyBackDark, settings.NotifyHideBody, NotifyBackApp, NotifyBackTime, NotifyBackTitle, NotifyBackBody);
    }

    // PaintCard: the app (12 semibold, secondary ink) after the unread dot, when (12, faint) on the right,
    // the title (14 semibold), the body (13, secondary ink, two lines) or "content hidden".
    private static void SetNotifyInk(bool dark, bool hideBody, TextBlock app, TextBlock time, TextBlock title, TextBlock body)
    {
        var ink = dark ? Color.FromArgb(255, 0x11, 0x13, 0x17) : Colors.White;
        SolidColorBrush A(double a) => new(Color.FromArgb((byte)Math.Round(255 * a), ink.R, ink.G, ink.B));
        var text = A(dark ? 0.92 : 0.96);
        var sub = A(dark ? 0.66 : 0.76);
        var faint = A(dark ? 0.46 : 0.56);
        app.Foreground = sub;
        time.Foreground = faint;
        title.Foreground = text;
        body.Foreground = hideBody ? faint : sub;
    }

    private void OnNotifyStageSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_windowLoaded && _page == 3 &&
            Math.Abs(e.NewSize.Width - e.PreviousSize.Width) + Math.Abs(e.NewSize.Height - e.PreviousSize.Height) > 0.5)
        {
            ShowNotifyPreview(false);
        }
    }

    private void OnReplayNotify(object sender, RoutedEventArgs e) => ReplayNotify();

    private void ReplayNotify()
    {
        if (!_notifyPreviewRendered)
        {
            _notifyEntrancePending = true;
            return;
        }
        if (NotifyCorner(CurrentSettings.NotifyPlacement) is (true, bool left, bool bottom))
        {
            var visual = ElementCompositionPreview.GetElementVisual(NotifyCards);
            visual.StopAnimation("Scale");
            visual.StopAnimation("Opacity");
            visual.Scale = Vector3.One;
            visual.Opacity = 1;
            // The card before moves aside for the new one, which comes in from the screen's edge.
            float k = (float)_notifyCardScale, step = (float)(CardH + CardPeek) * k;
            // The new one waits until the one before has left its place, else it would pass over it.
            AnimateNotifyCard(NotifyBackCard, new Vector3(0, bottom ? step : -step, 0), false);
            AnimateNotifyCard(NotifyFrontCard, new Vector3((left ? -1 : 1) * (float)CardW * k, 0, 0), true, MakeRoomSeconds);
            return;
        }
        // The default popup still opens from the taskbar bell.
        var anchor = new Vector2((float)(Canvas.GetLeft(NotifyFrontCard) + NotifyFrontCard.Width - GlassSpec.InsetDip - CurrentSettings.Height / 2 - 4),
                                 (float)(Canvas.GetTop(NotifyFrontCard) + NotifyFrontCard.Height - GlassSpec.InsetDip));
        ReplayNotification(NotifyCards, anchor, NotifyBackCard, NotifyFrontCard);
    }

    // PopVisual (glass_renderer.cpp) plus each new card's Glide (notifications.cpp).
    // The live notification code remains the authority; these previews use its equations.
    private static void ReplayNotification(FrameworkElement surface, Vector2 anchor, params FrameworkElement[] cards)
    {
        var visual = ElementCompositionPreview.GetElementVisual(surface);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3(anchor, 0);
        // Every replay is a new entrance, including clicks during the previous one.
        // Keep the existing card/glass buffers; replace only their animations.
        visual.StopAnimation("Scale");
        visual.StopAnimation("Opacity");
        visual.Scale = new Vector3(0.92f, 0.92f, 1);
        visual.Opacity = 0;
        var scale = compositor.CreateVector3KeyFrameAnimation();
        scale.Duration = TimeSpan.FromSeconds(0.6);
        scale.InsertKeyFrame(0, new Vector3(0.92f, 0.92f, 1));
        NotificationFrames(compositor, 0.6, PopSample, (time, value, ease) =>
            scale.InsertKeyFrame(time, new Vector3((float)(0.92 + 0.08 * value), (float)(0.92 + 0.08 * value), 1), ease));
        scale.InsertKeyFrame(1, Vector3.One);
        visual.StartAnimation("Scale", scale);

        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromSeconds(0.16);
        fade.InsertKeyFrame(0, 0);
        fade.InsertKeyFrame(1, 1, compositor.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
        visual.StartAnimation("Opacity", fade);

        foreach (var card in cards)
            AnimateNotifyCard(card, new Vector3(0, 18, 0), true);
    }

    private static (bool Corner, bool Left, bool Bottom) NotifyCorner(string placement) =>
        (BarSettings.NotifyCorners.Contains(placement), placement.EndsWith("Left"), placement.StartsWith("bottom"));

    private const double MakeRoomSeconds = 0.17; // kMakeRoomMs of the native cards

    private static void AnimateNotifyCard(FrameworkElement card, Vector3 offset, bool fade, double delay = 0)
    {
        ElementCompositionPreview.SetIsTranslationEnabled(card, true);
        var content = ElementCompositionPreview.GetElementVisual(card);
        var compositor = content.Compositor;
        content.StopAnimation("Translation");
        content.StopAnimation("Opacity");
        content.Properties.InsertVector3("Translation", offset);
        content.Opacity = fade ? 0 : 1;
        var move = compositor.CreateVector3KeyFrameAnimation();
        double moveTime = Math.Log(offset.Length() / 0.2) / 16; // the native card's Glide
        move.Duration = TimeSpan.FromSeconds(moveTime);
        move.InsertKeyFrame(0, offset);
        NotificationFrames(compositor, moveTime, t => ApproachSample(t, 16), (time, value, ease) =>
            move.InsertKeyFrame(time, offset * (float)(1 - value), ease));
        move.InsertKeyFrame(1, Vector3.Zero);
        move.DelayTime = TimeSpan.FromSeconds(delay);
        move.DelayBehavior = AnimationDelayBehavior.SetInitialValueBeforeDelay;
        content.StartAnimation("Translation", move);
        if (!fade) return;

        var opacity = compositor.CreateScalarKeyFrameAnimation();
        double fadeTime = Math.Log(1 / 0.01) / 12; // Glide(alpha, 1, dt, 12, 0.01)
        opacity.Duration = TimeSpan.FromSeconds(fadeTime);
        opacity.InsertKeyFrame(0, 0);
        NotificationFrames(compositor, fadeTime, t => ApproachSample(t, 12), (time, value, ease) =>
            opacity.InsertKeyFrame(time, (float)value, ease));
        opacity.InsertKeyFrame(1, 1);
        opacity.DelayTime = TimeSpan.FromSeconds(delay);
        opacity.DelayBehavior = AnimationDelayBehavior.SetInitialValueBeforeDelay;
        content.StartAnimation("Opacity", opacity);
    }

    private static (double Value, double Velocity) PopSample(double time)
    {
        const double Response = 0.36, Damping = 0.62; // motion::kPop
        double w = 2 * Math.PI / Response, wd = w * Math.Sqrt(1 - Damping * Damping);
        return (Motion.Spring(time, Response, Damping), Math.Exp(-Damping * w * time) * w * w / wd * Math.Sin(wd * time));
    }

    private static (double Value, double Velocity) ApproachSample(double time, double rate)
        => (1 - Math.Exp(-rate * time), rate * Math.Exp(-rate * time));

    // As SpringTo in glass_renderer.cpp: 12 Hermite segments, preserving both
    // position and velocity. Bezier X is linear time; Y is the same cubic.
    private static void NotificationFrames(Compositor compositor, double seconds,
        Func<double, (double Value, double Velocity)> sample, Action<float, double, CompositionEasingFunction> insert)
    {
        var previous = sample(0);
        double previousTime = 0;
        for (int i = 1; i <= 12; i++)
        {
            // The native animations snap to their target at End / Glide's epsilon.
            float progress = i == 12 ? 0.99999f : i / 12f;
            double time = progress * seconds;
            var next = sample(time);
            double delta = next.Value - previous.Value, span = time - previousTime;
            var ease = compositor.CreateCubicBezierEasingFunction(
                new Vector2(1f / 3, (float)(span * previous.Velocity / (3 * delta))),
                new Vector2(2f / 3, (float)(1 - span * next.Velocity / (3 * delta))));
            insert(progress, next.Value, ease);
            previous = next;
            previousTime = time;
        }
    }

    // ---- Widgets: the desktop, scaled down, with the widgets where the core puts them ----

    // The core's grid (native/core/ui/desktop_widgets.cpp): cells 164, gaps 16, margins 24, a row's room for the bar.
    private const double WCell = 164, WGap = 16, WMargin = 24;
    private FileSystemWatcher? _widgetWatcher;

    private static string WidgetsPath => Path.Combine(Path.GetDirectoryName(CoreClient.SettingsPath)!, "widgets.json");

    private void OnWidgetStageSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_windowLoaded && _page == 4 && Math.Abs(e.NewSize.Width - e.PreviousSize.Width) > 0.5) ShowWidgetMap();
    }

    // A widget on the map: where it is in the core's grid, its card (its glass and its face), and
    // what the face was drawn for.
    private sealed class MapCard
    {
        public required (string Kind, string Size, int Col, int Row) Cell;
        public required Grid View;
        public required Image Glass;
        public required Viewbox Face;
        public GlassTile Tile;
        public bool Dark;
        public long Stamp;
        public string Material = "";
        public string Cities = "";
    }

    private readonly List<MapCard> _mapCards = [];
    private Image? _mapBar;
    private bool _mapShown; // the first layout appears as it is; later changes animate

    // The new layout against the shown one: the same widget in the same place stays; one of the same
    // kind elsewhere (moved, resized) glides there; the others pop in or shrink away, as on the desktop.
    private void PlaceMapCards(List<GlassTile> tiles, List<(string Kind, string Size, IReadOnlyList<string> Cities)> faces,
                               List<(string Kind, string Size, int Col, int Row)> cells, TileFrame frame, double pad)
    {
        var old = new List<MapCard>(_mapCards);
        var next = new MapCard?[tiles.Count];
        for (int i = 0; i < tiles.Count; i++)
            if (old.FirstOrDefault(c => c.Cell == cells[i]) is { } same) { next[i] = same; old.Remove(same); }
        var moved = new HashSet<MapCard>();
        for (int i = 0; i < tiles.Count; i++)
        {
            if (next[i] != null) continue;
            var cell = cells[i];
            var other = old.Where(c => c.Cell.Kind == cell.Kind)
                           .OrderBy(c => Math.Abs(c.Cell.Col - cell.Col) + Math.Abs(c.Cell.Row - cell.Row)).FirstOrDefault();
            if (other == null) continue;
            next[i] = other;
            old.Remove(other);
            moved.Add(other);
        }
        foreach (var gone in old) MapCardLeave(gone);
        _mapCards.Clear();
        for (int i = 0; i < tiles.Count; i++)
        {
            var t = tiles[i];
            var (kind, size, cities) = faces[i];
            var card = next[i];
            bool added = card == null;
            if (card == null)
            {
                var glass = new Image { Stretch = Stretch.Fill };
                var face = new Viewbox { Stretch = Stretch.Fill };
                card = new MapCard { Cell = cells[i], View = new Grid { IsHitTestVisible = false, Children = { glass, face } }, Glass = glass, Face = face };
                WidgetMap.Children.Insert(Math.Min(i, WidgetMap.Children.Count), card.View);
            }
            var before = card.Tile;
            card.Cell = cells[i];
            bool dark = frame.Lumas[i] > 0.5f;
            long stamp = WidgetFaces.Stamp(kind);
            string material = CurrentSettings.WidgetMaterial;
            string cityList = string.Join("|", cities);
            if (added || moved.Contains(card) || card.Dark != dark || card.Stamp != stamp || card.Material != material || card.Cities != cityList)
            {
                card.Face.Child = WidgetFaces.Face(kind, size, dark, PhotoBrush(), material, cities);
                card.Cities = cityList;
                ContinuousCorners.Install(card.Face);
                card.Dark = dark;
                card.Stamp = stamp;
                card.Material = material;
            }
            card.Glass.Source = frame.Overlay(i);
            card.Tile = t;
            card.View.Width = t.W + 2 * pad;
            card.View.Height = t.H + 2 * pad;
            card.Face.Width = t.W;
            card.Face.Height = t.H;
            card.Face.Margin = new Thickness(pad);
            card.Face.HorizontalAlignment = HorizontalAlignment.Left;
            card.Face.VerticalAlignment = VerticalAlignment.Top;
            Canvas.SetLeft(card.View, t.X - pad);
            Canvas.SetTop(card.View, t.Y - pad);
            if (_mapShown && added) MapCardPop(card.View);
            else if (_mapShown && moved.Contains(card)) MapCardGlide(card.View, before, t, pad);
            _mapCards.Add(card);
        }
        _mapShown = true;
    }

    // A new widget: the pop of a popup (PopVisual), growing from its middle.
    private static void MapCardPop(FrameworkElement view)
    {
        var visual = ElementCompositionPreview.GetElementVisual(view);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)view.Width / 2, (float)view.Height / 2, 0);
        var scale = compositor.CreateVector3KeyFrameAnimation();
        scale.Duration = TimeSpan.FromSeconds(0.6);
        scale.InsertKeyFrame(0, new Vector3(0.8f, 0.8f, 1));
        NotificationFrames(compositor, 0.6, PopSample, (time, value, ease) =>
            scale.InsertKeyFrame(time, new Vector3((float)(0.8 + 0.2 * value), (float)(0.8 + 0.2 * value), 1), ease));
        scale.InsertKeyFrame(1, Vector3.One);
        visual.StartAnimation("Scale", scale);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromSeconds(0.2);
        fade.InsertKeyFrame(0, 0);
        fade.InsertKeyFrame(1, 1, compositor.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
        visual.StartAnimation("Opacity", fade);
    }

    // A removed widget shrinks and fades, then its card goes.
    private void MapCardLeave(MapCard card)
    {
        var view = card.View;
        var visual = ElementCompositionPreview.GetElementVisual(view);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)view.Width / 2, (float)view.Height / 2, 0);
        var ease = compositor.CreateCubicBezierEasingFunction(new Vector2(0.4f, 0), new Vector2(1, 1));
        var batch = compositor.CreateScopedBatch(CompositionBatchTypes.Animation);
        var scale = compositor.CreateVector3KeyFrameAnimation();
        scale.Duration = TimeSpan.FromSeconds(0.22);
        scale.InsertKeyFrame(1, new Vector3(0.8f, 0.8f, 1), ease);
        visual.StartAnimation("Scale", scale);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromSeconds(0.22);
        fade.InsertKeyFrame(1, 0, ease);
        visual.StartAnimation("Opacity", fade);
        batch.End();
        batch.Completed += (_, _) => WidgetMap.Children.Remove(view);
    }

    // A moved or resized widget glides from where it was: its card is laid out at the new place and
    // size, then moved and scaled back to the old one, and springs to rest.
    private static void MapCardGlide(FrameworkElement view, GlassTile from, GlassTile to, double pad)
    {
        if (from.W <= 0 || to.W <= 0) return;
        ElementCompositionPreview.SetIsTranslationEnabled(view, true);
        var visual = ElementCompositionPreview.GetElementVisual(view);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)pad, (float)pad, 0); // the widget's corner, not the shadow's
        float dx = (float)(from.X - to.X);
        float dy = (float)(from.Y - to.Y);
        float sx = (float)(from.W / to.W);
        float sy = (float)(from.H / to.H);
        var move = compositor.CreateVector3KeyFrameAnimation();
        var scale = compositor.CreateVector3KeyFrameAnimation();
        move.Duration = scale.Duration = TimeSpan.FromSeconds(0.6);
        move.InsertKeyFrame(0, new Vector3(dx, dy, 0));
        scale.InsertKeyFrame(0, new Vector3(sx, sy, 1));
        NotificationFrames(compositor, 0.6, PopSample, (time, value, ease) =>
        {
            float rest = (float)(1 - value);
            move.InsertKeyFrame(time, new Vector3(dx * rest, dy * rest, 0), ease);
            scale.InsertKeyFrame(time, new Vector3(1 + (sx - 1) * rest, 1 + (sy - 1) * rest, 1), ease);
        });
        move.InsertKeyFrame(1, Vector3.Zero);
        scale.InsertKeyFrame(1, Vector3.One);
        visual.StartAnimation("Translation", move);
        visual.StartAnimation("Scale", scale);
    }

    private void ShowWidgetMap()
    {
        if (!_windowLoaded) return;
        if (_widgetWatcher == null && Directory.Exists(Path.GetDirectoryName(WidgetsPath)))
        {
            // The core rewrites the file when a widget is moved, resized, added or removed.
            _widgetWatcher = new FileSystemWatcher(Path.GetDirectoryName(WidgetsPath)!, "widgets.json")
            {
                NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size,
                EnableRaisingEvents = true,
            };
            void OnChanged(object _1, FileSystemEventArgs _2) => DispatcherQueue.TryEnqueue(() => { if (_page == 4) ShowWidgetMap(); });
            _widgetWatcher.Changed += OnChanged;
            _widgetWatcher.Created += OnChanged;
            _widgetWatcher.Renamed += (_, _) => DispatcherQueue.TryEnqueue(() => { if (_page == 4) ShowWidgetMap(); });
        }

        // Match the selected desktop's grid, including mixed scaling.
        var displays = DesktopDisplay.Read();
        var display = displays.FirstOrDefault(d => d.Id == CurrentSettings.DesktopDisplay)
            ?? displays.FirstOrDefault(d => d.Primary)
            ?? displays.FirstOrDefault();
        double workW = display is { WorkWidth: > 0, WorkHeight: > 0, Scale: > 0 } ? display.WorkWidth / display.Scale : 1920;
        double workH = display is { WorkWidth: > 0, WorkHeight: > 0, Scale: > 0 } ? display.WorkHeight / display.Scale : 1032;
        int cols = Math.Max(1, (int)((workW - 2 * WMargin + WGap) / (WCell + WGap)));
        int rows = Math.Max(1, (int)((workH - 2 * WMargin - 90 + WGap) / (WCell + WGap)));
        double stageW = WidgetStage.ActualWidth > 0 ? WidgetStage.ActualWidth : 760;
        WidgetStage.Height = Math.Clamp(Math.Round(stageW * workH / workW), 220, 560);
        double k = stageW / workW, kh = WidgetStage.Height / workH;
        if (ContinuousCorners.Original(WidgetStage) is not ImageBrush) WidgetStage.Background = PhotoBrush();

        // The tiles where the core puts them, then the bar at the bottom for scale; all glass.
        var tiles = new List<GlassTile>();
        var faces = new List<(string Kind, string Size, IReadOnlyList<string> Cities)>();
        var cells = new List<(string Kind, string Size, int Col, int Row)>();
        foreach (var (kind, size, col, row, cities) in ReadWidgets())
        {
            int wc = size == "small" ? 1 : 2, wr = size == "large" ? 2 : 1;
            if (col < 0 || row < 0 || col + wc > cols || row + wr > rows) continue; // the core keeps it aside on this screen
            double w = wc * WCell + (wc - 1) * WGap, h = wr * WCell + (wr - 1) * WGap;
            int visualCol = CurrentSettings.WidgetsOnRight ? cols - col - wc : col;
            bool fromRight = 2 * visualCol + wc >= cols;
            double left = fromRight ? workW - WMargin - (cols - visualCol - wc) * (WCell + WGap) - w : WMargin + visualCol * (WCell + WGap);
            double top = WMargin + row * (WCell + WGap);
            tiles.Add(new GlassTile(left * k, top * kh, w * k, h * kh, 22 * Math.Min(k, kh)));
            faces.Add((kind, size, cities));
            cells.Add((kind, size, col, row));
        }
        double barW = 560 * k, barH = Math.Max(4, CurrentSettings.Height * kh);
        var bar = new GlassTile((stageW - barW) / 2, WidgetStage.Height - barH - Math.Max(2, CurrentSettings.Margin * kh), barW, barH, barH / 2);
        int shown = tiles.Count;
        double stageH = WidgetStage.Height, kk = Math.Min(k, kh);
        int photoGeneration = Photo.Generation;
        var placed = new List<(string Kind, string Size, int Col, int Row)>(cells);
        // Each widget carries its own piece of glass, so that it can come, go and move over the picture.
        PostTiles(WidgetMap, stageW, stageH, kk, [.. tiles, bar], null, frame =>
        {
            frame.Apply(WidgetStage);
            // Match the bitmap's completed viewport during subsequent resizes.
            WidgetMap.Width = stageW;
            WidgetMap.Height = stageH;
            WidgetMap.Clip = new RectangleGeometry { Rect = new Rect(0, 0, stageW, stageH) };
            double pad = Math.Round(GlassSpec.InsetDip * kk * frame.Rs) / frame.Rs;
            PlaceMapCards(tiles, faces, placed, frame, pad);
            _mapBar ??= new Image { Stretch = Stretch.Fill, IsHitTestVisible = false };
            if (_mapBar.Parent == null) WidgetMap.Children.Add(_mapBar);
            _mapBar.Source = frame.Overlay(tiles.Count);
            _mapBar.Width = bar.W + 2 * pad;
            _mapBar.Height = bar.H + 2 * pad;
            Canvas.SetLeft(_mapBar, bar.X - pad);
            Canvas.SetTop(_mapBar, bar.Y - pad);
            Ready("widgets", photoGeneration);
        }, overlays: true, scaled: true);

        WidgetMapCaption.Text = !CurrentSettings.Widgets ? Loc.T("Desktop widgets are off")
            : shown == 0 ? Loc.T("No widgets yet")
            : Loc.F("{0} widget(s)", shown);
        WidgetMap.Opacity = CurrentSettings.Widgets ? 1 : 0.5;
    }

    private static IEnumerable<(string Kind, string Size, int Col, int Row, IReadOnlyList<string> Cities)> ReadWidgets()
    {
        JsonNode? root = null;
        try { root = JsonNode.Parse(File.ReadAllText(WidgetsPath)); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        catch (System.Text.Json.JsonException) { }
        if (root?["widgets"] is not JsonArray list) yield break;
        foreach (var e in list)
        {
            if (e is not JsonObject o) continue;
            string kind = (string?)o["kind"] ?? "", size = (string?)o["size"] ?? "small";
            int col = (int?)(double?)o["col"] ?? 0, row = (int?)(double?)o["row"] ?? 0;
            IReadOnlyList<string> cities = o["cities"] is JsonArray a ? [.. a.Select(c => c is JsonValue v && v.TryGetValue(out string? s) ? s : "")] : [];
            if (kind.Length > 0) yield return (kind, size, col, row, cities);
        }
    }

    private void OnHideBodyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.NotifyHideBody = HideBodySwitch.IsOn;
        Changed();
    }

    private void OnNotifyFullscreenToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.NotifyFullscreen = NotifyFullscreenSwitch.IsOn;
        Changed();
    }
}
