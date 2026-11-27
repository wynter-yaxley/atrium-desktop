using System.Numerics;
using Microsoft.UI;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.System;
using Windows.UI;

namespace Atrium.Settings;

// Stage Manager's page: the desktop over the shared picture, the app in front in the middle and the
// others waiting at the side, their windows standing at an angle with their shadows and their apps'
// icons, as on the desktop (ui/stage_manager.cpp). A click on a card swaps it with the stage, Shift
// and a click adds it to the stage; the cards glide on the same spring (0.42 s, damping 0.86). Auto-
// hidden, the strip slides out while the pointer is at the preview's edge.
public sealed partial class MainWindow
{
    private sealed record StageApp(string Name, Color Accent, bool Dark);

    private static readonly StageApp[] StageApps =
    [
        new("Notes", Color.FromArgb(255, 0xFF, 0xC6, 0x0A), false),
        new("Mail", Color.FromArgb(255, 0x1E, 0x7B, 0xFF), false),
        new("Calendar", Color.FromArgb(255, 0xFF, 0x3B, 0x30), false),
        new("Photos", Color.FromArgb(255, 0xFF, 0x95, 0x00), false),
        new("Terminal", Color.FromArgb(255, 0x2C, 0x2C, 0x2E), true),
    ];

    // Preview DIP: the desktop's boxes (172 x 112, 22 from the edge, turned 22 degrees) at about 0.6.
    private const double StageBoxW = 100, StageBoxH = 64, StageSlotGap = 26, StageEdge = 16, StageMargin = 16;
    private const double StageTilt = 22, StageHoverTilt = 7;
    private const double StageBarH = 34, StageBarRoom = 56, StageWinGap = 30;

    private List<int> _stageFront = [0];
    private readonly List<List<int>> _stageStrip = [[1, 2], [3], [4]];
    private readonly Dictionary<string, Grid> _stageCards = [];
    private readonly Dictionary<string, Visual> _stageTilts = [];
    // The apps' windows and icons as pictures (assets/stage, made by --export-stage-art from the
    // elements below): one image instead of a hundred elements, with an exact outline for its shadow.
    private readonly Dictionary<string, BitmapImage> _stageArt = [];
    private Task? _stageArtLoading;
    private int _stageResize;
    private Grid? _stageCenter, _stageBar;
    private Canvas? _stageLayer; // the strip: slides out of the edge as a whole
    private Image? _stageBarGlass;
    private bool _stageBuilt, _stagePointerHooked;
    private bool _stageOut = true, _stageHidePending;
    private int _stageHideId;

    private static string StageKey(List<int> set) => string.Join(",", set);
    private double StageSide => CurrentSettings.StageOnRight ? -1 : 1; // +1: the strip on the left

    private void OnStagePreviewSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_page == 5 && Math.Abs(e.NewSize.Width - e.PreviousSize.Width) + Math.Abs(e.NewSize.Height - e.PreviousSize.Height) > 0.5)
            StageResized();
    }

    // Laid out again once the window has stopped changing size, not at every step of a drag.
    private async void StageResized()
    {
        int id = ++_stageResize;
        await Task.Delay(120);
        if (id == _stageResize && _page == 5) ShowStagePreview(false);
    }

    private static string StageArtPath(string name) => System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "Stage", name + ".png");

    // Read and decoded once, when the window opens: the page draws with them from its first frame.
    private Task LoadStageArt() => _stageArtLoading ??= LoadAll();

    private async Task LoadAll()
    {
        for (int a = 0; a < StageApps.Length; a++)
        {
            foreach (string name in new[] { $"window-{a}", $"icon-{a}" })
            {
                try
                {
                    var file = await Windows.Storage.StorageFile.GetFileFromPathAsync(StageArtPath(name));
                    using var stream = await file.OpenReadAsync();
                    var image = new BitmapImage();
                    await image.SetSourceAsync(stream);
                    _stageArt[name] = image;
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or FileNotFoundException
                    or System.Runtime.InteropServices.COMException)
                { }
            }
        }
    }

    private async void OnStageReplay(object sender, RoutedEventArgs e)
    {
        if (_stageStrip.Count == 0) return;
        if (!_stageOut)
        {
            // Auto-hidden: out of the edge first, as the pointer would bring it.
            StageReveal(true, true);
            await Task.Delay(420);
            StagePick(_stageStrip[0], false);
            await Task.Delay(1400);
            StageHideSoon();
            return;
        }
        StagePick(_stageStrip[0], false);
    }

    // Lays the desktop out again from the model; `animate`: the stage and the strip come in.
    private void ShowStagePreview(bool animate)
    {
        if (ContinuousCorners.Original(StagePreview) is not ImageBrush) StagePreview.Background = PhotoBrush();
        if (!_stagePointerHooked)
        {
            _stagePointerHooked = true;
            StagePreview.PointerMoved += OnStagePointerMoved;
            StagePreview.PointerExited += (_, _) => StageHideSoon();
        }
        double w = Math.Max(StagePreview.ActualWidth, 420), h = Math.Max(StagePreview.ActualHeight, 300);
        StageCanvas.Width = w;
        StageCanvas.Height = h;
        bool on = CurrentSettings.StageManager;
        string side = CurrentSettings.StageOnRight ? Loc.T("Right") : Loc.T("Left");
        string format = CurrentSettings.StageAutoHide
            ? "Rest the pointer at the preview's {0} edge to slide the cards out; click to bring one forward, Shift-click to add it to this group"
            : "Click a card on the {0} to bring it forward; Shift-click to bring it beside the window in front";
        StageCaption.Text = !on ? Loc.T("Stage Manager is off: windows stack on the desktop as usual") : Loc.F(format, side);
        StageReplay.IsEnabled = on;

        StageCanvas.Children.Clear();
        _stageCards.Clear();
        _stageTilts.Clear();
        if (_stageArtLoading is not { IsCompleted: true })
        {
            // The first time, a moment before the pictures are decoded.
            LoadStageArt().ContinueWith(_ => DispatcherQueue.TryEnqueue(() => { if (_page == 5) ShowStagePreview(animate); }), TaskScheduler.Default);
            return;
        }
        _stageBuilt = true;

        // The bar along the bottom, the apps on it.
        var bar = StageBarTile(w, h);
        _stageBarGlass = new Image { Stretch = Stretch.Fill };
        _stageBar = new Grid { Width = bar.W + 2 * GlassSpec.InsetDip, Height = bar.H + 2 * GlassSpec.InsetDip, IsHitTestVisible = false };
        _stageBar.Children.Add(_stageBarGlass);
        var icons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 9,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        for (int a = 0; a < StageApps.Length; a++) icons.Children.Add(StageIcon(a, 22));
        _stageBar.Children.Add(icons);
        Canvas.SetLeft(_stageBar, bar.X - GlassSpec.InsetDip);
        Canvas.SetTop(_stageBar, bar.Y - GlassSpec.InsetDip);
        StageCanvas.Children.Add(_stageBar);

        if (on)
        {
            _stageCenter = StageCenter(_stageFront, w, h);
            StageCanvas.Children.Add(_stageCenter);
            _stageLayer = new Canvas { Width = w, Height = h };
            ElementCompositionPreview.SetIsTranslationEnabled(_stageLayer, true);
            for (int i = 0; i < _stageStrip.Count; i++)
                _stageLayer.Children.Add(StageCard(_stageStrip[i], StageBox(i, _stageStrip.Count, w, h)));
            StageCanvas.Children.Add(_stageLayer);
            if (animate) StageAppear(_stageCenter, 0.96f);
            if (CurrentSettings.StageAutoHide)
            {
                StageReveal(false, false);
            }
            else
            {
                StageReveal(false, false);
                StageReveal(true, animate);
                if (!animate) StageReveal(true, false);
            }
        }
        else
        {
            // Off: every window where it was, one over the other.
            _stageLayer = null;
            _stageCenter = StageCascade(w, h);
            StageCanvas.Children.Add(_stageCenter);
            if (animate) StageAppear(_stageCenter, 0.98f);
        }
        StageRenderGlass(w, h);
    }

    private static GlassTile StageBarTile(double width, double height)
    {
        double w = Math.Min(width * 0.52, 480);
        return new GlassTile((width - w) / 2, height - 10 - StageBarH, w, StageBarH, StageBarH / 2);
    }

    private Rect StageBox(int i, int n, double w, double h)
    {
        double slot = StageBoxH + StageSlotGap, used = n * slot - StageSlotGap;
        double top = Math.Max(16, (h - StageBarRoom - used) / 2);
        double x = CurrentSettings.StageOnRight ? w - StageEdge - StageBoxW : StageEdge;
        return new Rect(x, top + i * slot, StageBoxW, StageBoxH);
    }

    // The room of the app in front: beside the strip, above the bar.
    private Rect StageRoom(double w, double h)
    {
        double strip = StageEdge + StageBoxW + StageWinGap;
        double left = CurrentSettings.StageOnRight ? StageWinGap : strip;
        double right = CurrentSettings.StageOnRight ? w - strip : w - StageWinGap;
        return new Rect(left, 22, Math.Max(40, right - left), Math.Max(40, h - StageBarRoom - 22 - 12));
    }

    // ---- The bar's glass (the strip has none) ----

    private void StageRenderGlass(double w, double h)
    {
        int photoGeneration = Photo.Generation;
        PostTiles(StageCanvas, w, h, 1, [StageBarTile(w, h)], null, f =>
        {
            _stageBarGlass?.Source = f.Overlay(0);
            Ready("stage", photoGeneration);
        }, overlays: true);
    }

    // ---- The strip ----

    // Out of the edge (or back into it): the cards slide and turn on the strip's spring.
    private void StageReveal(bool show, bool animate)
    {
        _stageOut = show;
        if (show) { _stageHideId++; _stageHidePending = false; }
        if (_stageLayer == null) return;
        var visual = ElementCompositionPreview.GetElementVisual(_stageLayer);
        float away = (float)((StageEdge + StageBoxW + 2 * StageMargin) * -StageSide);
        var to = new Vector3(show ? 0 : away, 0, 0);
        if (!animate)
        {
            visual.StopAnimation("Translation");
            visual.StopAnimation("Opacity");
            visual.Properties.InsertVector3("Translation", to);
            visual.Opacity = show ? 1 : 0;
            return;
        }
        var compositor = visual.Compositor;
        var slide = compositor.CreateSpringVector3Animation();
        slide.Target = "Translation";
        slide.FinalValue = to;
        slide.DampingRatio = show ? 0.82f : 1f;
        slide.Period = TimeSpan.FromSeconds((show ? 0.40 : 0.24) / (2 * Math.PI));
        visual.StartAnimation("Translation", slide);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromMilliseconds(show ? 180 : 220);
        fade.InsertKeyFrame(1, show ? 1 : 0);
        visual.StartAnimation("Opacity", fade);
    }

    private void OnStagePointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!CurrentSettings.StageManager || !CurrentSettings.StageAutoHide || _stageLayer == null) return;
        double x = e.GetCurrentPoint(StageCanvas).Position.X, w = StageCanvas.Width, reach = StageEdge + StageBoxW + StageMargin;
        bool atEdge = CurrentSettings.StageOnRight ? x >= w - 12 : x <= 12;
        bool inStrip = CurrentSettings.StageOnRight ? x >= w - reach : x <= reach;
        if (atEdge && !_stageOut) StageReveal(true, true);
        if (!_stageOut) return;
        if (inStrip) { _stageHideId++; _stageHidePending = false; }
        else if (!_stageHidePending)
        {
            StageHideSoon();
        }
    }

    // Auto-hidden: back into the edge once the pointer has been away for a moment.
    private async void StageHideSoon()
    {
        if (!_stageOut || !CurrentSettings.StageAutoHide || !CurrentSettings.StageManager) return;
        int id = ++_stageHideId;
        _stageHidePending = true;
        await Task.Delay(350);
        if (id != _stageHideId || !_stageOut || !CurrentSettings.StageAutoHide) return;
        _stageHidePending = false;
        StageReveal(false, true);
    }

    // A set's card: its windows (the most recent in front, the others a step inwards and up) turned
    // in perspective, the side towards the middle away; the apps' icons at the outer lower corner.
    private Grid StageCard(List<int> set, Rect box)
    {
        string key = StageKey(set);
        double m = StageMargin, side = StageSide;
        var tilt = new Grid { Width = box.Width, Height = box.Height, Margin = new Thickness(m), IsHitTestVisible = false };
        // Turned by the compositor, off the UI thread; the card's hit area stays where it is.
        var turn = ElementCompositionPreview.GetElementVisual(tilt);
        turn.RotationAxis = new Vector3(0, 1, 0);
        turn.CenterPoint = new Vector3((float)(box.Width / 2), (float)(box.Height / 2), 0);
        turn.RotationAngleInDegrees = (float)(side * StageTilt);
        var windows = new Canvas { Width = box.Width, Height = box.Height };
        int n = Math.Min(set.Count, 3);
        for (int j = n - 1; j >= 0; j--)
        {
            double k = Math.Pow(0.93, j), fw = Math.Min(box.Width, box.Height * 1.6) * k, fh = fw / 1.6;
            var w = Shadowed(StageWindow(StageApps[set[j]], fw, fh, 5), fw, fh, 5, 14, 0.34f, 4);
            Canvas.SetLeft(w, (box.Width - fw) / 2 + side * 6 * j);
            Canvas.SetTop(w, (box.Height - fh) / 2 - 6 * j);
            windows.Children.Add(w);
        }
        tilt.Children.Add(windows);
        var icons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = -5,
            FlowDirection = side > 0 ? FlowDirection.LeftToRight : FlowDirection.RightToLeft,
            HorizontalAlignment = side > 0 ? HorizontalAlignment.Left : HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(m - 9, 0, m - 9, m - 11),
            IsHitTestVisible = false,
        };
        foreach (int a in set.Take(3)) icons.Children.Add(Shadowed(StageIcon(a, 24), 24, 24, 5.5, 8, 0.32f, 2));
        // Seen from 340 DIP in front of the screen, as on the desktop (at the preview's scale), the eye
        // level with a point 0.45 of the way to the strip's middle: the cards above it lean down, those
        // below lean up.
        double middle = (StageCanvas.Height - StageBarRoom) / 2, cy = box.Y + box.Height / 2;
        var card = new Grid
        {
            Width = box.Width + 2 * m,
            Height = box.Height + 2 * m,
            Tag = key,
            Background = new SolidColorBrush(Colors.Transparent),
        };
        var eye = new Vector3((float)(card.Width / 2), (float)(card.Height / 2 + 0.45 * (middle - cy)), 0);
        var perspective = Matrix4x4.Identity;
        perspective.M34 = -1f / 200;
        ElementCompositionPreview.GetElementVisual(card).TransformMatrix = Matrix4x4.CreateTranslation(-eye) * perspective * Matrix4x4.CreateTranslation(eye);
        card.Children.Add(tilt);
        card.Children.Add(icons);
        Canvas.SetLeft(card, box.X - m);
        Canvas.SetTop(card, box.Y - m);
        ElementCompositionPreview.SetIsTranslationEnabled(card, true);
        // The pointer turns the windows most of the way to the eye.
        card.PointerEntered += (_, _) => StageTurn(turn, (float)(side * StageHoverTilt), 1.06f);
        card.PointerExited += (_, _) => StageTurn(turn, (float)(side * StageTilt), 1);
        card.PointerCanceled += (_, _) => StageTurn(turn, (float)(side * StageTilt), 1);
        card.PointerCaptureLost += (_, _) => StageTurn(turn, (float)(side * StageTilt), 1);
        card.PointerPressed += (_, e) =>
        {
            bool shift = (e.KeyModifiers & VirtualKeyModifiers.Shift) != 0;
            var picked = _stageStrip.FirstOrDefault(s => StageKey(s) == key);
            if (picked != null) StagePick(picked, shift);
            e.Handled = true;
        };
        _stageCards[key] = card;
        _stageTilts[key] = turn;
        return card;
    }

    // Turns the windows to `angle` and grows them to `scale`, from wherever they are now.
    private static void StageTurn(Visual turn, float angle, float scale)
    {
        var compositor = turn.Compositor;
        var ease = compositor.CreateCubicBezierEasingFunction(new Vector2(0.2f, 0.8f), new Vector2(0.3f, 1f));
        var rotate = compositor.CreateScalarKeyFrameAnimation();
        rotate.Duration = TimeSpan.FromMilliseconds(240);
        rotate.InsertKeyFrame(1, angle, ease);
        turn.StartAnimation("RotationAngleInDegrees", rotate);
        var grow = compositor.CreateVector3KeyFrameAnimation();
        grow.Duration = TimeSpan.FromMilliseconds(240);
        grow.InsertKeyFrame(1, new Vector3(scale, scale, 1), ease);
        turn.StartAnimation("Scale", grow);
    }

    // Fades out at once (the icons of a card that flies to the stage).
    private static void StageHide(UIElement element)
    {
        var visual = ElementCompositionPreview.GetElementVisual(element);
        var fade = visual.Compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromMilliseconds(120);
        fade.InsertKeyFrame(1, 0);
        visual.StartAnimation("Opacity", fade);
    }

    // `content` (w x h) over its soft shadow, `radius` its corners.
    private static Grid Shadowed(FrameworkElement content, double w, double h, double radius, float blur, float opacity, float drop)
    {
        var grid = new Grid { Width = w, Height = h, IsHitTestVisible = false };
        var host = new Canvas { Width = w, Height = h };
        grid.Children.Add(host);
        // A picture casts the shadow of its own outline; anything else that of a rounded rectangle
        // (under it, the same size, so none of it shows).
        CompositionBrush mask;
        if (content is Image image)
        {
            mask = image.GetAlphaMask();
        }
        else
        {
            var shape = new Rectangle { Width = w, Height = h, RadiusX = radius, RadiusY = radius, Fill = new SolidColorBrush(Colors.Black), Opacity = 0.01 };
            grid.Children.Add(shape);
            mask = shape.GetAlphaMask();
        }
        grid.Children.Add(content);
        var compositor = ElementCompositionPreview.GetElementVisual(host).Compositor;
        var shadow = compositor.CreateDropShadow();
        shadow.BlurRadius = blur;
        shadow.Opacity = opacity;
        shadow.Color = Colors.Black;
        shadow.Offset = new Vector3(0.5f, drop + 0.5f, 0);
        shadow.Mask = mask;
        var sprite = compositor.CreateSpriteVisual();
        sprite.Size = new Vector2((float)w, (float)h);
        sprite.Shadow = shadow;
        ElementCompositionPreview.SetElementChildVisual(host, sprite);
        return grid;
    }

    // The apps' windows and icons are built in StageArt.cs (with cached PNGs loaded in LoadAll).

    // The front window of the set in front, preview DIP.
    private Rect StageFront(int count, double w, double h)
    {
        var room = StageRoom(w, h);
        double step = 18, n = Math.Min(count, 3);
        double fw = Math.Min(room.Width - step * (n - 1), (room.Height - step * (n - 1)) * 1.6), fh = fw / 1.6;
        double x0 = (room.Width - fw - step * (n - 1)) / 2, y0 = (room.Height - fh - step * (n - 1)) / 2;
        return new Rect(room.X + x0 + step * (n - 1), room.Y + y0 + step * (n - 1), fw, fh);
    }

    // The set in front: its windows in the room beside the strip, the most recent in front.
    private Grid StageCenter(List<int> set, double width, double height)
    {
        var room = StageRoom(width, height);
        double step = 18, n = Math.Min(set.Count, 3);
        double fw = Math.Min(room.Width - step * (n - 1), (room.Height - step * (n - 1)) * 1.6), fh = fw / 1.6;
        var grid = new Grid { Width = room.Width, Height = room.Height, IsHitTestVisible = false };
        var canvas = new Canvas();
        double x0 = (room.Width - fw - step * (n - 1)) / 2, y0 = (room.Height - fh - step * (n - 1)) / 2;
        // Back to front: the first app of the set is the most recent.
        for (int j = (int)n - 1; j >= 0; j--)
        {
            var w = Shadowed(StageWindow(StageApps[set[j]], fw, fh, 8), fw, fh, 8, 26, 0.32f, 8);
            Canvas.SetLeft(w, x0 + step * ((int)n - 1 - j));
            Canvas.SetTop(w, y0 + step * ((int)n - 1 - j));
            canvas.Children.Add(w);
        }
        grid.Children.Add(canvas);
        Canvas.SetLeft(grid, room.X);
        Canvas.SetTop(grid, room.Y);
        return grid;
    }

    // Stage Manager off: every app's window in a loose cascade.
    private Grid StageCascade(double width, double height)
    {
        var room = new Rect(40, 22, Math.Max(40, width - 80), Math.Max(40, height - StageBarRoom - 34));
        int n = StageApps.Length;
        double step = 26, fw = Math.Min(room.Width - step * (n - 1), (room.Height - 12 * (n - 1)) * 1.6), fh = fw / 1.6;
        var grid = new Grid { Width = room.Width, Height = room.Height, IsHitTestVisible = false };
        var canvas = new Canvas();
        double x0 = (room.Width - fw - step * (n - 1)) / 2;
        for (int j = n - 1; j >= 0; j--)
        {
            var w = Shadowed(StageWindow(StageApps[j], fw, fh, 8), fw, fh, 8, 22, 0.26f, 6);
            Canvas.SetLeft(w, x0 + step * (n - 1 - j));
            Canvas.SetTop(w, 12 * (n - 1 - j));
            canvas.Children.Add(w);
        }
        grid.Children.Add(canvas);
        Canvas.SetLeft(grid, room.X);
        Canvas.SetTop(grid, room.Y);
        return grid;
    }

    // ---- The swap ----

    private void StagePick(List<int> picked, bool add)
    {
        if (!CurrentSettings.StageManager || !_stageBuilt || _stageLayer == null) return;
        double w = StageCanvas.Width, h = StageCanvas.Height;
        var oldFront = _stageFront;
        int at = _stageStrip.IndexOf(picked);
        if (at < 0) return;
        _stageStrip.RemoveAt(at);
        if (add)
        {
            _stageFront = [.. oldFront, .. picked];
        }
        else
        {
            _stageStrip.Insert(0, oldFront);
            _stageFront = picked;
        }

        // The stage's windows fly into the strip's top place; the picked card's fly to the middle, where
        // its windows stand once they land. Shift: the old windows stay, the new ones join them.
        // Everything new is built and laid out first; the animations start together after, on a
        // tree that is ready (none of their first frames goes to layout).
        var go = new List<Action>();
        var oldCenter = _stageCenter;
        _stageCenter = StageCenter(_stageFront, w, h);
        StageCanvas.Children.Insert(Math.Min(1, StageCanvas.Children.Count), _stageCenter);
        ElementCompositionPreview.GetElementVisual(_stageCenter).Opacity = 0;
        var room = StageRoom(w, h);
        var front = StageFront(_stageFront.Count, w, h);
        string oldKey = StageKey(oldFront);
        if (oldCenter != null)
        {
            if (add)
            {
                go.Add(() => StageLeave(oldCenter, StageCanvas, 1f, new Vector3((float)(room.Width / 2), (float)(room.Height / 2), 0)));
            }
            else
            {
                var from = StageFront(oldFront.Count, w, h);
                var to = StageBox(0, _stageStrip.Count, w, h);
                double fw = Math.Min(to.Width, to.Height * 1.6);
                float k = (float)(fw / from.Width);
                var pivot = new Vector3((float)(from.X + from.Width / 2 - room.X), (float)(from.Y + from.Height / 2 - room.Y), 0);
                // Turning as it goes, to the strip's angle, in the strip's perspective.
                if (oldCenter.Children.Count > 0)
                {
                    var perspective = Matrix4x4.Identity;
                    perspective.M34 = -k / 200;
                    ElementCompositionPreview.GetElementVisual(oldCenter).TransformMatrix =
                        Matrix4x4.CreateTranslation(-pivot) * perspective * Matrix4x4.CreateTranslation(pivot);
                    var inner = ElementCompositionPreview.GetElementVisual(oldCenter.Children[0]);
                    inner.RotationAxis = new Vector3(0, 1, 0);
                    inner.CenterPoint = pivot;
                    var turn = inner.Compositor.CreateScalarKeyFrameAnimation();
                    turn.Duration = TimeSpan.FromMilliseconds(460);
                    turn.InsertKeyFrame(1, (float)(StageSide * StageTilt),
                        inner.Compositor.CreateCubicBezierEasingFunction(new Vector2(0.65f, 0f), new Vector2(0.35f, 1f)));
                    go.Add(() => inner.StartAnimation("RotationAngleInDegrees", turn));
                }
                go.Add(() => StageFly(oldCenter, pivot, k,
                         new Vector3((float)(to.X + to.Width / 2 - from.X - from.Width / 2), (float)(to.Y + to.Height / 2 - from.Y - from.Height / 2), 0),
                         () => StageCanvas.Children.Remove(oldCenter)));
            }
        }

        string pickedKey = StageKey(picked);
        if (_stageCards.Remove(pickedKey, out var gone))
        {
            if (_stageTilts.Remove(pickedKey, out var tilt)) go.Add(() => StageTurn(tilt, 0, 1));
            if (gone.Children.Count > 1) go.Add(() => StageHide(gone.Children[1])); // the icons stay in the strip
            double bx = Canvas.GetLeft(gone) + StageMargin + StageBoxW / 2, by = Canvas.GetTop(gone) + StageMargin + StageBoxH / 2;
            double fw = Math.Min(StageBoxW, StageBoxH * 1.6);
            var center = _stageCenter;
            go.Add(() => StageFly(gone, new Vector3((float)(gone.Width / 2), (float)(gone.Height / 2), 0), (float)(front.Width / fw),
                     new Vector3((float)(front.X + front.Width / 2 - bx), (float)(front.Y + front.Height / 2 - by), 0),
                     () => _stageLayer?.Children.Remove(gone)));
            go.Add(() => StageFadeIn(center)); // under the picture as it lands
        }
        else
        {
            var center = _stageCenter;
            go.Add(() => StageShow(center));
        }

        // The strip: the others glide to their places; the stage's old set gets the top place, shown when its windows land.
        for (int i = 0; i < _stageStrip.Count; i++)
        {
            string key = StageKey(_stageStrip[i]);
            var box = StageBox(i, _stageStrip.Count, w, h);
            if (_stageCards.TryGetValue(key, out var card))
            {
                double fromY = Canvas.GetTop(card), toY = box.Y - StageMargin;
                Canvas.SetTop(card, toY);
                var glide = card;
                go.Add(() => StageGlide(glide, (float)(fromY - toY)));
            }
            else
            {
                card = StageCard(_stageStrip[i], box);
                _stageLayer.Children.Add(card);
                ElementCompositionPreview.GetElementVisual(card).Opacity = 0;
                var added = card;
                if (key != oldKey) go.Add(() => StageAppear(added, 0.9f));
                else go.Add(() => StageFadeIn(added)); // as the stage's windows land on it
            }
        }
        StageCanvas.UpdateLayout();
        foreach (var start in go) start();
    }

    // Scales about `center` to `scale` and moves by `move` (the flight between the stage and the
    // strip), then `done`.
    private static void StageFly(FrameworkElement element, Vector3 center, float scale, Vector3 move, Action done)
    {
        ElementCompositionPreview.SetIsTranslationEnabled(element, true);
        var visual = ElementCompositionPreview.GetElementVisual(element);
        var compositor = visual.Compositor;
        visual.CenterPoint = center;
        var ease = compositor.CreateCubicBezierEasingFunction(new Vector2(0.65f, 0f), new Vector2(0.35f, 1f));
        var batch = compositor.CreateScopedBatch(CompositionBatchTypes.Animation);
        var grow = compositor.CreateVector3KeyFrameAnimation();
        grow.Duration = TimeSpan.FromMilliseconds(460);
        grow.InsertKeyFrame(1, new Vector3(scale, scale, 1), ease);
        visual.StartAnimation("Scale", grow);
        // Whole on the way, fading into what stands where it lands.
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromMilliseconds(460);
        fade.InsertKeyFrame(0.7f, 1);
        fade.InsertKeyFrame(1, 0);
        visual.StartAnimation("Opacity", fade);
        var go = compositor.CreateVector3KeyFrameAnimation();
        go.Duration = TimeSpan.FromMilliseconds(460);
        go.InsertKeyFrame(1, move, ease);
        visual.StartAnimation("Translation", go);
        batch.End();
        batch.Completed += (_, _) => done();
    }

    // Fades in over the last part of a flight (what it lands on).
    private static void StageFadeIn(UIElement element)
    {
        var visual = ElementCompositionPreview.GetElementVisual(element);
        visual.Opacity = 0;
        var fade = visual.Compositor.CreateScalarKeyFrameAnimation();
        fade.DelayTime = TimeSpan.FromMilliseconds(300);
        fade.DelayBehavior = AnimationDelayBehavior.SetInitialValueBeforeDelay;
        fade.Duration = TimeSpan.FromMilliseconds(160);
        fade.InsertKeyFrame(0, 0);
        fade.InsertKeyFrame(1, 1);
        visual.StartAnimation("Opacity", fade);
    }

    // Shown at once (where a flight has just landed).
    private static void StageShow(UIElement element)
    {
        var visual = ElementCompositionPreview.GetElementVisual(element);
        visual.StopAnimation("Opacity");
        visual.Opacity = 1;
    }

    // From where it was to where it now stands, on the strip's spring.
    private static void StageGlide(UIElement element, float fromY)
    {
        if (Math.Abs(fromY) < 0.5f) return;
        var visual = ElementCompositionPreview.GetElementVisual(element);
        var compositor = visual.Compositor;
        visual.Properties.InsertVector3("Translation", new Vector3(0, fromY, 0));
        var spring = compositor.CreateSpringVector3Animation();
        spring.Target = "Translation";
        spring.FinalValue = Vector3.Zero;
        spring.DampingRatio = 0.86f;
        spring.Period = TimeSpan.FromSeconds(0.42 / (2 * Math.PI));
        visual.StartAnimation("Translation", spring);
    }

    // Fades in and grows from `scale` (a critically damped spring for the scale).
    private static void StageAppear(FrameworkElement element, float scale)
    {
        var visual = ElementCompositionPreview.GetElementVisual(element);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)(element.Width / 2), (float)(element.Height / 2), 0);
        visual.Scale = new Vector3(scale, scale, 1);
        visual.Opacity = 0;
        var grow = compositor.CreateSpringVector3Animation();
        grow.FinalValue = Vector3.One;
        grow.DampingRatio = 1f;
        grow.Period = TimeSpan.FromSeconds(0.30 / (2 * Math.PI));
        visual.StartAnimation("Scale", grow);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromMilliseconds(240);
        fade.InsertKeyFrame(1, 1, compositor.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
        visual.StartAnimation("Opacity", fade);
    }

    // Fades out and shrinks towards `center`, then leaves `parent`.
    private static void StageLeave(FrameworkElement element, Panel parent, float scale, Vector3 center)
    {
        var visual = ElementCompositionPreview.GetElementVisual(element);
        var compositor = visual.Compositor;
        visual.CenterPoint = center;
        var batch = compositor.CreateScopedBatch(CompositionBatchTypes.Animation);
        var shrink = compositor.CreateVector3KeyFrameAnimation();
        shrink.Duration = TimeSpan.FromMilliseconds(220);
        shrink.InsertKeyFrame(1, new Vector3(scale, scale, 1), compositor.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
        visual.StartAnimation("Scale", shrink);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromMilliseconds(220);
        fade.InsertKeyFrame(1, 0);
        visual.StartAnimation("Opacity", fade);
        batch.End();
        batch.Completed += (_, _) => parent.Children.Remove(element);
    }
}
