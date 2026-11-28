using System.Numerics;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

// The pages under the bar (the start menu, the tray, the status entry, the time): each shows the
// bar at the bottom of the picture and the popup that opens from it, laid out with the core's
// numbers and moving with its curves. The glass is rendered on the CPU (GlassPreview.cs) for the
// shapes as they are on this frame; the marks on it are laid out for the shapes the glass on
// screen was rendered for, so the two never drift apart while a shape springs to another size.
public sealed partial class MainWindow
{
    private ModuleScene? _startScene, _trayScene, _imeScene, _statusScene, _clockScene;
    private bool _scenesPaused; // the scenes stand still: nothing of theirs is on screen

    private ModuleScene? SceneFor(int page) => page switch
    {
        10 => _startScene ??= new StartScene(this),
        11 => _trayScene ??= new TrayScene(this),
        12 => _statusScene ??= new StatusScene(this),
        13 => _clockScene ??= new ClockScene(this),
        14 => _imeScene ??= new ImeScene(this),
        _ => ExtrasScene(page),
    };

    private IEnumerable<ModuleScene> Scenes() => WithExtras([_startScene, _trayScene, _imeScene, _statusScene, _clockScene]);

    // A page was opened: its scene comes in (the popup pops out of its entry), the others rest.
    private void EnterModulePage(int page)
    {
        var scene = SceneFor(page);
        foreach (var other in Scenes()) if (other != scene) other.Leave();
        scene?.Enter();
    }

    private void LeaveModulePages()
    {
        foreach (var scene in Scenes()) scene.Leave();
    }

    private void OnAppWindowChanged(AppWindow sender, AppWindowChangedEventArgs args)
    {
        if (args.DidPositionChange) UpdateWindowMinimumSize();
        UpdateScenesPause(!sender.IsVisible ? false : null);
    }

    private void OnWindowVisibilityChanged(object sender, WindowVisibilityChangedEventArgs args) => UpdateScenesPause(args.Visible);

    private void UpdateScenesPause(bool? visible = null)
    {
        bool paused = !(visible ?? AppWindow.IsVisible) || AppWindow.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized };
        if (paused == _scenesPaused) return;
        _scenesPaused = paused;
        foreach (var scene in Scenes()) { if (paused) scene.Pause(); else scene.Resume(); }
    }

    private void StopModules()
    {
        foreach (var scene in Scenes()) scene.Stop();
    }

    // The settings or the picture changed.
    private void ShowModulePreview(int page) => SceneFor(page)?.Refresh();

    private void OnModuleStageSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (Math.Abs(e.NewSize.Width - e.PreviousSize.Width) + Math.Abs(e.NewSize.Height - e.PreviousSize.Height) > 0.5) SceneFor(_page)?.Resize();
    }

    // The bar preview's apps whose icons are read.
    private ImageSource[] LoadedIcons() => [.. _previewPins.Select(p => p.Icon).OfType<ImageSource>()];

    // ---- Motion (native/core/motion.h) ----

    // motion::Spring: response (s) and damping ratio, stepped in closed form.
    private readonly record struct Spring(double Response, double Damping)
    {
        public void Step(ref double x, ref double v, double target, double dt)
        {
            double w0 = 2 * Math.PI / Math.Max(Response, 0.01), z = Math.Max(Damping, 0), y0 = x - target, v0 = v, y, vy;
            if (Math.Abs(z - 1) < 1e-3)
            {
                double e = Math.Exp(-w0 * dt), b = v0 + w0 * y0;
                y = e * (y0 + b * dt);
                vy = e * (v0 - w0 * dt * b);
            }
            else if (z < 1)
            {
                double a = z * w0, wd = w0 * Math.Sqrt(1 - z * z), e = Math.Exp(-a * dt), c = Math.Cos(wd * dt), s = Math.Sin(wd * dt);
                y = e * (y0 * c + (v0 + a * y0) / wd * s);
                vy = e * (v0 * c - (a * v0 + w0 * w0 * y0) / wd * s);
            }
            else
            {
                double q = w0 * Math.Sqrt(z * z - 1), r1 = -z * w0 + q, r2 = -z * w0 - q, c2 = (v0 - r1 * y0) / (r2 - r1), c1 = y0 - c2;
                double e1 = Math.Exp(r1 * dt), e2 = Math.Exp(r2 * dt);
                y = c1 * e1 + c2 * e2;
                vy = c1 * r1 * e1 + c2 * r2 * e2;
            }
            x = target + y;
            v = vy;
        }
    }

    private static readonly Spring ResizeSpring = new(0.40, 0.82); // StartMenu's kResize
    private static readonly Spring SmoothSpring = new(0.48, 0.77); // motion::kSmooth: the status ring's level

    // motion::Approach: closes the share `rate` of the distance per second, whatever the frame time.
    private static double Approach(double v, double target, double dtMs, double rate) => v + (target - v) * (1 - Math.Exp(-dtMs / 1000 * rate));

    // ---- The popups' ink (Theme::For) over light glass (dark ink) or dark glass ----

    private sealed class PanelInk
    {
        public readonly SolidColorBrush Text = new(), Sub = new(), Faint = new(), Hover = new(), Divider = new(), Card = new(), Border = new(),
                                        Wash45 = new(), Search = new(), Track = new(), Quiet = new(), Level = new(), OnFill = new(),
                                        Disabled = new(), DisabledTrack = new(), DisabledGlyph = new(), Chevron = new(), Ring = new();
        public bool? Dark;

        public static Color A(Color c, double a) => Color.FromArgb((byte)Math.Round(c.A * Math.Clamp(a, 0, 1)), c.R, c.G, c.B);

        public void Set(bool dark)
        {
            if (Dark == dark) return;
            Dark = dark;
            Color ink = dark ? Color.FromArgb(255, 0x11, 0x13, 0x17) : Colors.White, wash = dark ? Colors.Black : Colors.White;
            Text.Color = A(ink, dark ? 0.92 : 0.96);
            Sub.Color = A(ink, dark ? 0.66 : 0.76);
            Faint.Color = A(ink, dark ? 0.46 : 0.56);
            Hover.Color = A(wash, dark ? 0.10 : 0.20);
            Divider.Color = A(wash, dark ? 0.12 : 0.24);
            Card.Color = A(wash, dark ? 0.05 : 0.08);
            Border.Color = A(wash, dark ? 0.10 : 0.30);
            Wash45.Color = A(Hover.Color, 0.45);
            Search.Color = A(Card.Color, 0.6);
            Track.Color = A(Text.Color, 0.14);
            Quiet.Color = A(Text.Color, 0.16);
            Ring.Color = A(Text.Color, 0.18);
            Level.Color = A(Text.Color, 0.9);
            OnFill.Color = dark ? Color.FromArgb(247, 255, 255, 255) : Color.FromArgb(247, 18, 18, 23);
            Disabled.Color = A(Text.Color, 0.4);
            DisabledTrack.Color = A(Text.Color, 0.08);
            DisabledGlyph.Color = A(Text.Color, 0.35);
            Chevron.Color = A(Text.Color, 0.08);
        }
    }

    private static readonly SolidColorBrush AccentBrush = new(Color.FromArgb(255, 0x0A, 0x84, 0xFF));
    private static readonly SolidColorBrush WhiteInk = new(Color.FromArgb(250, 255, 255, 255));

    // ---- Marks: text and symbols at the core's sizes, in boxes as Canvas::Text places them ----

    private static TextBlock Words(double size, Brush brush, bool bold = false, TextAlignment align = TextAlignment.Left, bool wrap = false) => new()
    {
        FontSize = size,
        Foreground = brush,
        TextAlignment = align,
        IsTextScaleFactorEnabled = false,
        IsHitTestVisible = false,
        TextWrapping = wrap ? TextWrapping.Wrap : TextWrapping.NoWrap,
        TextTrimming = TextTrimming.CharacterEllipsis,
        MaxLines = wrap ? 2 : 1,
        FontFamily = (FontFamily)Application.Current.Resources[bold ? "TitleFont" : "TextFont"],
    };

    private static TextBlock Symbol(string glyph, double size, Brush brush) => new()
    {
        Text = glyph,
        FontSize = size,
        Foreground = brush,
        TextAlignment = TextAlignment.Center,
        IsTextScaleFactorEnabled = false,
        IsHitTestVisible = false,
        FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
    };

    // In the box (x, y, w, h): one line centred on its height (DWrite's centred paragraph); wrapping
    // text from its top.
    private static void Box(TextBlock t, double x, double y, double w, double h)
    {
        t.Width = Math.Max(0, w);
        Canvas.SetLeft(t, x);
        Canvas.SetTop(t, t.TextWrapping == TextWrapping.NoWrap ? y + (h - TextMetrics.Line(t)) / 2 : y);
    }

    private static Border Plate(Brush fill, double radius) => new() { Background = fill, CornerRadius = new CornerRadius(radius), IsHitTestVisible = false };

    private static void Put(FrameworkElement e, double x, double y, double w, double h)
    {
        e.Width = Math.Max(0, w);
        e.Height = Math.Max(0, h);
        Canvas.SetLeft(e, x);
        Canvas.SetTop(e, y);
    }

    private static readonly Dictionary<(string, double, bool), double> TextWidths = [];

    // The width of a line of text (Canvas::TextWidth).
    private static double TextWidth(string text, double size, bool bold = false)
    {
        if (TextWidths.TryGetValue((text, size, bold), out double w)) return w;
        var t = Words(size, AccentBrush, bold);
        t.Text = text;
        t.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return TextWidths[(text, size, bold)] = t.DesiredSize.Width;
    }

    // A tile's icon, drawn here: a rounded square in its colour, its symbol in white.
    private static Grid DrawnIcon(string glyph, uint top, uint bottom)
    {
        static Color Rgb(uint c) => Color.FromArgb(255, (byte)(c >> 16), (byte)(c >> 8), (byte)c);
        var fill = new LinearGradientBrush { StartPoint = new Point(0.5, 0), EndPoint = new Point(0.5, 1) };
        fill.GradientStops.Add(new GradientStop { Color = Rgb(top), Offset = 0 });
        fill.GradientStops.Add(new GradientStop { Color = Rgb(bottom), Offset = 1 });
        var g = new Grid { Width = 40, Height = 40, IsHitTestVisible = false };
        g.Children.Add(new Border { Background = fill, CornerRadius = new CornerRadius(10) });
        g.Children.Add(new FontIcon
        {
            Glyph = glyph,
            FontSize = 19,
            Foreground = WhiteInk,
            FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
        });
        return g;
    }

    private static Path RingPath(double width) =>
        new() { IsHitTestVisible = false, StrokeThickness = width, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round };

    // ---- A page's scene: the picture, the bar and the popup above it ----

    private sealed record GlassJob(int Id, double W, double H, double Rs, bool Low, double Scale, Lazy<Photo> Photo, int PhotoGeneration, BarSettings Material,
                                   GlassTile[] Tiles, object State);
    private sealed record GlassFrame(int Id, GlassTile[] Tiles, GlassOverlay[] Overlays, double Rs, bool Low, double Ms, object State);

    private abstract class ModuleScene
    {
        private const double BarGlideMs = 320, PopupGap = 12; // kPopupGapDip: a popup over the bar

        protected readonly MainWindow M;
        private readonly Border _stage;
        private readonly Canvas _root;
        private readonly Image _barGlass = new() { Stretch = Stretch.Fill, IsHitTestVisible = false };
        private readonly Canvas _barLayer = new() { IsHitTestVisible = false };
        protected readonly BarFace Face;
        private readonly Canvas _pop = new() { IsHitTestVisible = false };
        protected readonly Canvas Marks = new() { IsHitTestVisible = false };
        private readonly ScaleTransform _marksScale = new();
        private readonly List<Image> _glass = [];
        private readonly LatestWorker<GlassJob> _worker;

        protected double W, H, S;
        protected BarGeometry? Bar;
        private BarGeometry? _barFrom, _barTo;
        private double _barT = 1;
        private bool _barDark;
        protected bool Active { get; private set; }
        protected double Now { get; set; }
        protected bool Ticking { get; private set; }
        private bool _open, _popPending;
        private long _last;
        private int _posted, _applied;
        private string _postedKey = "";
        private bool _postedLow;
        // In motion the glass is rendered at a share of the resolution, smaller if a frame took long.
        private static readonly double[] MotionScales = [0.5, 0.35];
        private int _motion;
        private object? _shown;

        // Render thread: the pictures (sharp, and at the motion resolutions), the glass of the last
        // frame, and the bar's at the motion resolutions, ready before anything moves.
        private readonly Dictionary<string, Backdrop> _backdrops = [];
        private Dictionary<(GlassTile, GlassSpec, string), GlassOverlay> _cache = [], _warm = [];

        protected ModuleScene(MainWindow m, Border stage, Canvas root)
        {
            M = m;
            _stage = stage;
            _root = root;
            root.Children.Add(_barGlass);
            root.Children.Add(_barLayer);
            root.Children.Add(_pop);
            _pop.Children.Add(Marks);
            Face = new BarFace(_barLayer, MaxPreviewApps);
            foreach (var icon in Face.Icons) icon.SetBinding(Image.SourceProperty, new Binding { Path = new PropertyPath(nameof(PinVm.Icon)) });
            Marks.RenderTransform = _marksScale;
            _worker = new LatestWorker<GlassJob>(Render, (j, _) => M.DispatcherQueue.TryEnqueue(() => RenderFailed(j)));
        }

        // ---- What a scene says ----

        protected abstract bool IsOpen { get; }                        // the popup is there (its entry is on the bar)
        protected abstract Size PopupSize { get; }                     // its largest size (DIP), to fit the scene
        protected abstract (GlassTile[] Tiles, object State) Pieces(); // its glass on this frame, in the stage's DIP
        protected abstract Vector2 Anchor { get; }                     // where it pops out of
        protected abstract void Ink(int piece, bool dark);
        protected virtual bool Step(double dt) => false;               // its shapes move
        protected abstract bool Arrange(object state, double dt);     // lays its marks; true while they move
        protected virtual void Entered() { }
        protected virtual void Refreshed() { }
        protected virtual void PoppedIn() { }
        protected virtual void Left() { }
        protected virtual void Paused() { }
        protected virtual void Resumed() { }
        protected virtual StatusLook BarStatus() => StatusLook.Current();

        protected bool HasFrame => _shown != null;
        protected double BarHeight => M.CurrentSettings.Height;
        protected double Radius => Math.Clamp(M.CurrentSettings.CornerRadius, 8, Math.Max(8, M.CurrentSettings.Height / 2));
        protected double PopupBottom => Bar!.BarY - PopupGap * S;
        protected double BarRight => Math.Min(Bar!.BarX + Bar.BarW, W - 16); // PopupRect, right-aligned with the bar

        // The marks' origin (stage DIP); inside, real DIP (times `scale`, by default the scene's).
        protected void PlaceMarks(double x, double y, double? scale = null)
        {
            _marksScale.ScaleX = _marksScale.ScaleY = scale ?? S;
            Canvas.SetLeft(Marks, x);
            Canvas.SetTop(Marks, y);
        }

        // ---- The page comes and goes ----

        public void Enter()
        {
            Active = true;
            if (ContinuousCorners.Original(_stage) is not ImageBrush) _stage.Background = M.PhotoBrush();
            _open = IsOpen;
            var visual = ElementCompositionPreview.GetElementVisual(_pop);
            visual.StopAnimation("Scale");
            visual.StopAnimation("Opacity");
            visual.Opacity = 0;
            _popPending = _open;
            _shown = null;
            _postedKey = "";
            Entered();
            if (Fit()) Kick();
        }

        public void Leave() { if (!Active) return; Active = false; StopTicking(); Left(); }
        public void Stop() { Leave(); _worker.Stop(); }
        public void Resize() { if (!Active) return; _postedKey = ""; if (Fit()) Kick(); }

        public void Refresh()
        {
            if (!Active) return;
            if (ContinuousCorners.Original(_stage) is not ImageBrush) _stage.Background = M.PhotoBrush();
            if (Bar == null || Math.Abs(W - _stage.ActualWidth) > 0.5 || Math.Abs(H - _stage.ActualHeight) > 0.5 || Math.Abs(S - FitScale()) > 1e-6)
            {
                if (!Fit())
                {
                    _open = IsOpen;
                    _popPending = _open;
                    Refreshed();
                    return;
                }
            }
            else
            {
                var target = M.BarLayout(M.CurrentSettings, W, H, S, false);
                if (!HasFrame)
                {
                    (Bar, _barTo, _barT) = (target, target, 1); // nothing shown yet: no glide
                }
                else if (target != _barTo)
                {
                    _barFrom = Bar;
                    _barTo = target;
                    _barT = 0;
                }
            }
            bool open = IsOpen;
            if (open != _open)
            {
                _open = open;
                if (open) PopIn();
                else PopOut();
            }
            Refreshed();
            _postedKey = ""; // the material or the picture may have changed
            Kick();
        }

        private double FitScale()
        {
            var s = M.CurrentSettings;
            var need = PopupSize;
            double w = _stage.ActualWidth, h = _stage.ActualHeight;
            return Math.Min(1.0, Math.Min(Math.Min((w - 48) / (HeroReferenceW + 2 * GlassSpec.InsetDip), (w - 32) / need.Width),
                                          (h - 20) / (need.Height + PopupGap + s.Height + s.Margin)));
        }

        private bool Fit()
        {
            double w = _stage.ActualWidth, h = _stage.ActualHeight;
            if (w < 120 || h < 120) return false;
            W = w;
            H = h;
            S = FitScale();
            _root.Width = _pop.Width = w;
            _root.Height = _pop.Height = h;
            _root.Clip = new RectangleGeometry { Rect = new Rect(0, 0, w, h) };
            Bar = _barTo = M.BarLayout(M.CurrentSettings, w, h, S, false);
            _barFrom = null;
            _barT = 1;
            return true;
        }

        // ---- Frames ----

        protected void Kick()
        {
            if (!Active || Ticking || M._scenesPaused) return;
            Ticking = true;
            _last = Environment.TickCount64;
            CompositionTarget.Rendering += Tick;
        }

        // The window was minimized, hidden, or restored.
        public void Pause() { if (Active) { StopTicking(); Paused(); } }
        public void Resume() { if (Active && !M._scenesPaused) { Resumed(); Kick(); } }

        private void StopTicking()
        {
            if (!Ticking) return;
            Ticking = false;
            CompositionTarget.Rendering -= Tick;
        }

        private void Tick(object? sender, object e)
        {
            long t = Environment.TickCount64;
            double dt = Math.Clamp(t - _last, 0, 100);
            _last = t;
            Now = t;
            bool moving = false;
            if (_barT < 1 && _barFrom != null && _barTo != null)
            {
                _barT = Math.Min(1, _barT + dt / BarGlideMs);
                Bar = Lerp(_barFrom, _barTo, Motion.EaseOut(_barT));
                moving = true;
            }
            moving |= Step(dt);
            Post(moving);
            bool marks = _shown != null && Arrange(_shown, dt);
            if (!moving && !marks && _applied == _posted && !_postedLow) StopTicking();
        }

        private static GlassTile BarTile(BarGeometry bar) => new(bar.BarX, bar.BarY, bar.BarW, bar.BarH, bar.Radius);

        // The glass for this frame, unless it is the one already on its way; while shapes move at a
        // lower resolution (so it keeps up with the frames), sharp again once they rest.
        private void Post(bool moving)
        {
            if (Bar == null) return;
            var (tiles, state) = Pieces();
            var all = new GlassTile[tiles.Length + 1];
            all[0] = BarTile(Bar);
            tiles.CopyTo(all, 1);
            double rs = Math.Clamp(M.Content.XamlRoot?.RasterizationScale ?? 1, 1, 2);
            if (moving && !_postedLow) _motion = 1; // a motion starts on the smaller one: the first frames come at once
            double r = moving ? rs * MotionScales[_motion] : rs;
            string key = string.Join(";", all) + $"|{r}|{W}x{H}|{Photo.Generation}|{GlassSpec.For(M.CurrentSettings, S * rs)}";
            if (key == _postedKey) return;
            _postedKey = key;
            _postedLow = moving;
            _worker.Post(new GlassJob(++_posted, W, H, r, moving, S, Photo.Capture(), Photo.Generation, M.CurrentSettings.Clone(), all, (Bar, state)));
        }

        private void Render(GlassJob j)
        {
            long start = System.Diagnostics.Stopwatch.GetTimestamp();
            if (Picture(j, j.Rs) is not { } picture) { M.DispatcherQueue.TryEnqueue(() => RenderFailed(j)); return; }
            var (bg, key) = picture;
            var spec = GlassSpec.For(j.Material, j.Scale * j.Rs);
            var next = new Dictionary<(GlassTile, GlassSpec, string), GlassOverlay>();
            var overlays = new GlassOverlay[j.Tiles.Length];
            for (int i = 0; i < j.Tiles.Length; i++)
            {
                var k = (j.Tiles[i], spec, key);
                if (!_cache.TryGetValue(k, out var o) && !_warm.TryGetValue(k, out o))
                    o = GlassRenderer.CardOverlays(bg, j.Material, j.Rs, [j.Tiles[i]], j.Scale)[0];
                overlays[i] = next[k] = o;
            }
            _cache = next;
            var frame = new GlassFrame(j.Id, j.Tiles, overlays, j.Rs, j.Low, System.Diagnostics.Stopwatch.GetElapsedTime(start).TotalMilliseconds, j.State);
            M.DispatcherQueue.TryEnqueue(() => Apply(frame));

            // At rest: the pictures and the bar at the motion resolutions, so that a motion starts at once.
            if (j.Low) return;
            var warm = new Dictionary<(GlassTile, GlassSpec, string), GlassOverlay>();
            foreach (double m in MotionScales)
            {
                double r = j.Rs * m;
                if (Picture(j, r) is not { } low) continue;
                var k = (j.Tiles[0], GlassSpec.For(j.Material, j.Scale * r), low.Item2);
                warm[k] = _warm.TryGetValue(k, out var o) ? o : GlassRenderer.CardOverlays(low.Item1, j.Material, r, [j.Tiles[0]], j.Scale)[0];
            }
            _warm = warm;
        }

        private (Backdrop, string)? Picture(GlassJob j, double rs)
        {
            int pw = (int)Math.Round(j.W * rs), ph = (int)Math.Round(j.H * rs);
            if (pw < 8 || ph < 8) return null;
            string key = $"{pw}x{ph}/{j.W:F1}x{j.H:F1}/{j.PhotoGeneration}";
            if (!_backdrops.TryGetValue(key, out var bg))
            {
                if (_backdrops.Count > MotionScales.Length) _backdrops.Clear();
                bg = _backdrops[key] = Backdrop.Create(pw, ph, j.W, j.H, 0, 0, rs, j.Photo.Value);
            }
            return (bg, key);
        }

        private bool DarkInk(float luma) => M.CurrentSettings.TextColor == "dark" || (M.CurrentSettings.TextColor == "auto" && luma > 0.5f);

        private void Apply(GlassFrame f)
        {
            if (!Active || f.Id < _applied) return;
            _applied = f.Id;
            if (f.Low) _motion = _motion == 0 && f.Ms > 16 ? 1 : _motion == 1 && f.Ms < 7 ? 0 : _motion;
            Show(_barGlass, f.Tiles[0], f.Overlays[0], f.Rs);
            _barDark = DarkInk(f.Overlays[0].Luma);
            for (int i = 1; i < f.Tiles.Length; i++)
            {
                while (_glass.Count < i)
                {
                    var image = new Image { Stretch = Stretch.Fill, IsHitTestVisible = false };
                    _pop.Children.Insert(_glass.Count, image);
                    _glass.Add(image);
                }
                _glass[i - 1].Visibility = Visibility.Visible;
                Show(_glass[i - 1], f.Tiles[i], f.Overlays[i], f.Rs);
                Ink(i - 1, DarkInk(f.Overlays[i].Luma));
            }
            for (int i = f.Tiles.Length - 1; i < _glass.Count; i++)
                _glass[i].Visibility = Visibility.Collapsed;
            var (bar, state) = ((BarGeometry, object))f.State;
            LayoutBar(bar);
            _shown = state;
            Arrange(state, 0);
            if (_popPending) { _popPending = false; PopIn(); }
            Kick();
        }

        private void RenderFailed(GlassJob j)
        {
            if (!Active || j.Id <= _applied) return;
            _applied = j.Id;
            _postedLow = false;
            if (_shown == null) { _shown = (((BarGeometry, object))j.State).Item2; _popPending = false; }
            Kick();
        }

        private static void Show(Image image, GlassTile t, GlassOverlay o, double rs)
        {
            if (image.Source is not WriteableBitmap bmp || bmp.PixelWidth != o.Width || bmp.PixelHeight != o.Height)
                image.Source = bmp = new WriteableBitmap(o.Width, o.Height);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(o.Pixels, 0, o.Pixels.Length);
            bmp.Invalidate();
            double w = o.Width / rs, h = o.Height / rs;
            image.Width = w;
            image.Height = h;
            Canvas.SetLeft(image, t.X + t.W / 2 - w / 2);
            Canvas.SetTop(image, t.Y + t.H / 2 - h / 2);
            image.RenderTransform = t.Angle == 0 ? null : new RotateTransform { Angle = t.Angle, CenterX = w / 2, CenterY = h / 2 };
        }

        private void LayoutBar(BarGeometry bar)
        {
            int running = M.CurrentSettings.ShowRunning && bar.N > M._previewPins.Length ? M._previewPins.Length : -1;
            for (int i = 0; i < MaxPreviewApps; i++)
            {
                var vm = i < M._previewPins.Length ? M._previewPins[i] : M._previewRunning;
                if (Face.Icons[i].DataContext != vm) Face.Icons[i].DataContext = vm;
            }
            SetBarClock();
            // No pointer on the bar: no magnification, no hover plate.
            Face.Layout(bar, _barDark, bar.N, running, 2, 1, 0, 0, BarStatus(), M.CurrentSettings.LaunchpadIcon);
        }

        protected void SetBarClock()
        {
            var clock = PreviewClock.Now;
            Face.SetClock(clock.ToString(M.CurrentSettings.ShowClockSeconds ? "H:mm:ss" : "H:mm"), clock.ToString("yyyy/M/d"));
        }

        // The bar between two layouts: everything glides; an entry that comes or goes does at once.
        private static BarGeometry Lerp(BarGeometry a, BarGeometry b, double t)
        {
            double Mix(double x, double y) => x + (y - x) * t;
            double Slot(double x, double y) => x < 0 || y < 0 ? y : x + (y - x) * t;
            return b with
            {
                BaseX = Mix(a.BaseX, b.BaseX),
                BarX = Mix(a.BarX, b.BarX),
                BarY = Mix(a.BarY, b.BarY),
                BarW = Mix(a.BarW, b.BarW),
                BarH = Mix(a.BarH, b.BarH),
                Radius = Mix(a.Radius, b.Radius),
                Icon = Mix(a.Icon, b.Icon),
                P = Mix(a.P, b.P),
                PlateR = Mix(a.PlateR, b.PlateR),
                AppsLeft = Mix(a.AppsLeft, b.AppsLeft),
                Pitch = Mix(a.Pitch, b.Pitch),
                PlateY = Mix(a.PlateY, b.PlateY),
                ClockX = Slot(a.ClockX, b.ClockX),
                ClockW = Mix(a.ClockW, b.ClockW),
                SepX = Slot(a.SepX, b.SepX),
                StartX = Mix(a.StartX, b.StartX),
                TrayX = Slot(a.TrayX, b.TrayX),
                ImeX = Slot(a.ImeX, b.ImeX),
                StatusX = Slot(a.StatusX, b.StatusX),
                BellX = Slot(a.BellX, b.BellX),
                LaunchX = Slot(a.LaunchX, b.LaunchX),
                MusicX = Slot(a.MusicX, b.MusicX),
                StacksX = Slot(a.StacksX, b.StacksX),
                StatusExtra = Mix(a.StatusExtra, b.StatusExtra),
            };
        }

        protected void UpdateBarStatus(StatusLook look) => Face.ShowStatus(look);

        // ---- The popup pops out of its entry (PopVisual) and back ----

        private void PopIn()
        {
            var visual = ElementCompositionPreview.GetElementVisual(_pop);
            var c = visual.Compositor;
            visual.CenterPoint = new Vector3(Anchor, 0);
            visual.StopAnimation("Scale");
            visual.StopAnimation("Opacity");
            visual.Scale = new Vector3(0.92f, 0.92f, 1);
            visual.Opacity = 0;
            var scale = c.CreateVector3KeyFrameAnimation();
            scale.Duration = TimeSpan.FromSeconds(0.6);
            scale.InsertKeyFrame(0, new Vector3(0.92f, 0.92f, 1));
            NotificationFrames(c, 0.6, PopSample, (time, value, ease) =>
                scale.InsertKeyFrame(time, new Vector3((float)(0.92 + 0.08 * value), (float)(0.92 + 0.08 * value), 1), ease));
            scale.InsertKeyFrame(1, Vector3.One);
            visual.StartAnimation("Scale", scale);
            var fade = c.CreateScalarKeyFrameAnimation();
            fade.Duration = TimeSpan.FromSeconds(0.16);
            fade.InsertKeyFrame(0, 0);
            fade.InsertKeyFrame(1, 1, c.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
            visual.StartAnimation("Opacity", fade);
            Now = Environment.TickCount64;
            PoppedIn();
            Kick();
        }

        // The popup closes and opens again (a choice of where it opens): `between` runs while it is away.
        protected void Replay(Action between)
        {
            if (!_open || !HasFrame) { between(); return; }
            PopOut();
            var timer = M.DispatcherQueue.CreateTimer();
            timer.Interval = TimeSpan.FromMilliseconds(160);
            timer.IsRepeating = false;
            timer.Tick += (_, _) =>
            {
                if (M._updatesClosed) return;
                between();
                _postedKey = "";
                if (Active && _open) PopIn();
            };
            timer.Start();
        }

        private void PopOut()
        {
            var visual = ElementCompositionPreview.GetElementVisual(_pop);
            var c = visual.Compositor;
            visual.CenterPoint = new Vector3(Anchor, 0);
            visual.StopAnimation("Scale");
            visual.StopAnimation("Opacity");
            var ease = c.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1));
            var scale = c.CreateVector3KeyFrameAnimation();
            scale.Duration = TimeSpan.FromSeconds(0.13);
            scale.InsertKeyFrame(1, new Vector3(0.95f, 0.95f, 1), ease);
            visual.StartAnimation("Scale", scale);
            var fade = c.CreateScalarKeyFrameAnimation();
            fade.Duration = TimeSpan.FromSeconds(0.12);
            fade.InsertKeyFrame(1, 0, ease);
            visual.StartAnimation("Opacity", fade);
        }
    }

    // ---- The tray: its panel over the bar's right end ----

    private sealed class TrayScene : ModuleScene
    {
        private const double Cell = 40, IconSize = 20, Air = (Cell - IconSize) / 2, Split = 14, FooterH = 24;
        private const int Count = 13, ColsN = 5, RowsN = 3; // TrayPanel::Cols: 13 icons in rows of 5, 5 and 3
        private static readonly string[] Glyphs = ["\uE753", "\uE702", "\uEA18", "\uE88E", "\uE8BD", "\uE7F4"];
        private readonly PanelInk _ink = new();
        private readonly FrameworkElement?[] _items = new FrameworkElement?[Count];
        private readonly Rectangle _divider = new() { IsHitTestVisible = false };
        private readonly TextBlock _count, _system;
        private readonly Border _button;
        private double _opened = double.MaxValue;

        private sealed record Frame(double Left, double Top);

        public TrayScene(MainWindow m) : base(m, m.TrayStage, m.TrayCanvas)
        {
            _divider.Fill = _ink.Divider;
            _count = Words(12, _ink.Sub);
            _count.Text = Loc.F("{0} icons", Count);
            _button = Plate(_ink.Wash45, 12);
            _system = Words(11.5, _ink.Text, false, TextAlignment.Center);
            _system.Text = Loc.T("System tray");
            foreach (var e in new UIElement[] { _divider, _count, _button, _system }) Marks.Children.Add(e);
        }

        private double Margin => Math.Clamp(Radius * 0.65, 14, 22);
        private Size Panel => new(2 * (Margin - Air) + ColsN * Cell, Margin - Air + RowsN * Cell - Air + 2 * Split + FooterH + Margin);
        protected override bool IsOpen => M.CurrentSettings.ShowTray;
        protected override Size PopupSize => Panel;
        protected override Vector2 Anchor => new((float)(Bar!.TrayX >= 0 ? Bar.TrayX + Bar.P / 2 : BarRight), (float)PopupBottom);

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double left = BarRight - Panel.Width * S, top = PopupBottom - Panel.Height * S;
            return (new[] { new GlassTile(left, top, Panel.Width * S, Panel.Height * S, Radius * S) }, new Frame(left, top));
        }

        protected override void Ink(int piece, bool dark) => _ink.Set(dark);
        protected override void Entered() => _opened = double.MaxValue;
        protected override void PoppedIn() => _opened = Now;

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Top);
            double m = Margin, pad = m - Air, w = Panel.Width, h = Panel.Height;
            // The icons: the bar's apps (their own icons) between one-colour symbols in the panel's ink.
            var icons = M.LoadedIcons();
            bool moving = false;
            for (int i = 0; i < Count; i++)
            {
                bool app = i % 2 == 0 && icons.Length > 0;
                if (_items[i] == null || (_items[i] is Image) != app)
                {
                    if (_items[i] != null) Marks.Children.Remove(_items[i]);
                    _items[i] = app ? new Image { Stretch = Stretch.Uniform, IsHitTestVisible = false } : Symbol(Glyphs[i / 2 % Glyphs.Length], 16, _ink.Text);
                    Marks.Children.Add(_items[i]);
                }
                var item = _items[i]!;
                if (item is Image image) image.Source = icons[i / 2 % icons.Length];
                double cx = pad + i % ColsN * Cell + Cell / 2, cy = pad + i / ColsN * Cell + Cell / 2;
                // In one after another (kStageMs 240, kStaggerMs 14), rising 6 DIP.
                double e = Motion.EaseOut((Now - _opened - 14 * Math.Min(i, 18)) / 240);
                if (e < 1) moving = true;
                cy += 6 * (1 - e);
                if (item is TextBlock t) Box(t, cx - IconSize / 2, cy - IconSize / 2, IconSize, IconSize);
                else Put(item, cx - IconSize / 2, cy - IconSize / 2, IconSize, IconSize);
                item.Opacity = e;
            }
            // Below them: the count, and the system's own tray.
            double footTop = h - m - FooterH, divider = footTop - Split;
            Put(_divider, m, divider, w - 2 * m, 1);
            double bw = TextWidth(Loc.T("System tray"), 11.5) + 20;
            Box(_count, m, footTop, w - m - bw - 10 - m, FooterH);
            Put(_button, w - m - bw, footTop, bw, FooterH);
            Box(_system, w - m - bw, footTop, bw, FooterH);
            return moving;
        }
    }
}
