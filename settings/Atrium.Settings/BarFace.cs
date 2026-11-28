using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

// What the bar draws on its glass, as XAML over the preview's rendered glass: the same marks,
// sizes and inks as BarPainter (native/core/render/bar_painter.cpp) — the Windows logo's four
// panes, the app icons growing in the magnification wave, the running dots, the separator, the
// tray chevron, the status ring, the clock with its halo, the bell and its badge. One BarFace
// draws the preview at 1x, another the loupe's close-up.
internal sealed partial class BarFace
{
    [StructLayout(LayoutKind.Sequential)]
    private struct PowerStatus { public byte AcLine, BatteryFlag, Percent, SaverStatus; public int LifeTime, FullLifeTime; }
    [DllImport("kernel32.dll")] private static extern bool GetSystemPowerStatus(out PowerStatus status);

    // The bar's inks (BarPainter::DrawContent): white 96 % on dark glass, near-black 92 % on light.
    public static readonly Color LightInk = Color.FromArgb(245, 255, 255, 255), DarkInk = Color.FromArgb(235, 18, 18, 23);

    private readonly Canvas _logo = new() { IsHitTestVisible = false };
    private readonly Rectangle[] _logoPanes = new Rectangle[4];
    private readonly List<Ellipse> _dots = [];
    private readonly Border _plate = new() { IsHitTestVisible = false };
    private readonly Rectangle _sep = new() { IsHitTestVisible = false }, _desk = new() { IsHitTestVisible = false };
    private readonly FontIcon _tray = Glyph("\uE70E"), _bell = Glyph("\uEA8F");
    private readonly TextBlock _ime = Ime(), _imePrev = Ime();
    private readonly Path _imePath = new() { IsHitTestVisible = false }, _imePrevPath = new() { IsHitTestVisible = false };
    private readonly Border _badge = new() { IsHitTestVisible = false, Background = new SolidColorBrush(Color.FromArgb(255, 10, 133, 255)) };
    private readonly TextBlock _badgeText = new()
    {
        Text = "3",
        Foreground = new SolidColorBrush(Colors.White),
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
        TextAlignment = TextAlignment.Center
    };
    private readonly StatusGlyph _status = new();
    private readonly StatusIcons _statusIcons = new();
    private readonly TextBlock _time = Clock(true), _date = Clock(false), _timeHalo = Clock(true), _dateHalo = Clock(false);
    private readonly SolidColorBrush _inkBrush = new(), _plateBrush = new(), _dotBrush = new(), _sepBrush = new(), _haloBrush = new();
    private (MainWindow.BarGeometry Geometry, bool Dark, int Apps, int Running, double Magnify, double Zoom, double X, double Y,
             StatusLook Status)? _layout;
    private (double X, double Y, double Size, bool Icons, Color Ink)? _statusAt;
    private (double X, double Y, double Size, Color Ink)? _imeAt;
    private (string Glyph, string Prev, double Fade) _imeState = ("中", "", 1);

    public BarFace(Canvas canvas, int maxIcons)
    {
        _badgeText.FontFamily = (FontFamily)Application.Current.Resources["TitleFont"];
        _badge.Child = _badgeText;
        _plate.Background = _plateBrush;
        for (int i = 0; i < _logoPanes.Length; ++i)
        {
            var pane = new Rectangle { IsHitTestVisible = false, Fill = LogoBrush() };
            ContinuousCorners.SetEnabled(pane, true);
            _logoPanes[i] = pane;
            _logo.Children.Add(pane);
        }
        canvas.Children.Add(_plate);
        canvas.Children.Add(_logo);
        AddExtras(canvas);
        for (int i = 0; i < maxIcons; i++)
        {
            var icon = new Image { Stretch = Stretch.Uniform, IsHitTestVisible = false };
            Icons.Add(icon);
            canvas.Children.Add(icon);
            var dot = new Ellipse { IsHitTestVisible = false };
            _dots.Add(dot);
            canvas.Children.Add(dot);
        }
        foreach (var e in new UIElement[]
        {
            _sep, _desk, _tray, _imePrev, _imePrevPath, _ime, _imePath, _status, _statusIcons, _timeHalo, _dateHalo, _time, _date, _bell, _badge,
        })
        {
            canvas.Children.Add(e);
        }
    }

    public List<Image> Icons { get; } = [];

    // The status entry in another state (the status page's demonstration), where Layout put it.
    public void ShowStatus(StatusLook status)
    {
        if (_statusAt is not var (x, y, size, icons, ink)) return;
        if (icons) _statusIcons.Update(size, ink, status);
        else _status.Update(size, ink, status);
        Canvas.SetLeft(icons ? _statusIcons : _status, x);
        Canvas.SetTop(icons ? _statusIcons : _status, y);
    }

    public void ShowIme(string glyph, string prev = "", double fade = 1)
    {
        _imeState = (glyph, prev, fade);
        if (_imeAt is not var (x, y, size, ink)) return;
        PlaceIme(_imePrev, _imePrevPath, prev, x, y, size, StatusGlyph.Fade(ink, 1 - fade));
        PlaceIme(_ime, _imePath, glyph, x, y, size, StatusGlyph.Fade(ink, fade));
    }

    // Known letters come from Noto Sans CJK SC Regular outlines (em units centred on the ink); any other
    // falls back to TextFont centred in the square.
    private static void PlaceIme(TextBlock t, Path p, string glyph, double x, double y, double size, Color color)
    {
        if (color.A < 1 || string.IsNullOrEmpty(glyph))
        {
            t.Visibility = p.Visibility = Visibility.Collapsed;
            return;
        }
        if (ImeGlyphData.PathFor(glyph) is string markup)
        {
            t.Visibility = Visibility.Collapsed;
            p.Visibility = Visibility.Visible;
            if (!Equals(p.Tag, glyph))
            {
                p.Tag = glyph;
                p.Data = (Geometry)XamlBindingHelper.ConvertValue(typeof(Geometry), "F1 " + markup);
            }
            double em = Math.Max(1, size * 0.64);
            if (p.ReadLocalValue(Shape.FillProperty) is SolidColorBrush brush) brush.Color = color;
            else p.Fill = new SolidColorBrush(color);
            if (p.ReadLocalValue(UIElement.RenderTransformProperty) is ScaleTransform scale) { scale.ScaleX = scale.ScaleY = em; }
            else { p.RenderTransform = new ScaleTransform { ScaleX = em, ScaleY = em }; }
            Canvas.SetLeft(p, x + size / 2);
            Canvas.SetTop(p, y + size / 2);
            return;
        }
        p.Visibility = Visibility.Collapsed;
        StatusGlyph.Place(t, glyph, x, y, size, size * 0.64, color);
    }

    private static TextBlock Ime() => new()
    {
        Text = "中",
        Foreground = new SolidColorBrush(Colors.Transparent),
        TextAlignment = TextAlignment.Center,
        IsHitTestVisible = false,
        IsTextScaleFactorEnabled = false,
        TextWrapping = TextWrapping.NoWrap,
        FontFamily = (FontFamily)Application.Current.Resources["TextFont"],
    };

    private static FontIcon Glyph(string glyph) => new()
    {
        Glyph = glyph,
        IsHitTestVisible = false,
        FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
    };

    // Time: Semibold; date: the regular face (MiSans Medium). Figures tabular.
    private static TextBlock Clock(bool time)
    {
        var t = new TextBlock
        {
            TextAlignment = TextAlignment.Center,
            IsHitTestVisible = false,
            IsTextScaleFactorEnabled = false,
            TextWrapping = TextWrapping.NoWrap,
            FontFamily = (FontFamily)Application.Current.Resources[time ? "TitleFont" : "TextFont"],
            LineStackingStrategy = LineStackingStrategy.BlockLineHeight,
            Text = time ? "10:24" : "2026/11/3",
        };
        Typography.SetNumeralAlignment(t, FontNumeralAlignment.Tabular);
        return t;
    }

    public void SetClock(string time, string date)
    {
        _time.Text = _timeHalo.Text = time;
        _date.Text = _dateHalo.Text = date;
    }

    // DrawLogo: four panes, Windows 11's light-to-deep blue across the whole logo.
    private static LinearGradientBrush LogoBrush()
    {
        var g = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 1) };
        g.GradientStops.Add(new GradientStop { Color = Color.FromArgb(255, 0x5C, 0xCB, 0xFF), Offset = 0 });
        g.GradientStops.Add(new GradientStop { Color = Color.FromArgb(255, 0x1A, 0x9B, 0xF0), Offset = 0.5 });
        g.GradientStops.Add(new GradientStop { Color = Color.FromArgb(255, 0x00, 0x60, 0xC8), Offset = 1 });
        return g;
    }

    // The preview's state of the system: the real battery, Wi-Fi at full strength.
    public static (double Fill, bool Charging, bool Low) Battery()
    {
        if (!GetSystemPowerStatus(out var p) || (p.BatteryFlag & 128) != 0 || p.Percent > 100) return (0.6, false, false);
        bool charging = p.AcLine == 1;
        return (p.Percent / 100.0, charging, !charging && p.Percent <= 20);
    }

    // A computer without a battery (a desktop): the ring is the volume, with a speaker in its gap.
    public static bool HasBattery() => GetSystemPowerStatus(out var p) && (p.BatteryFlag & 128) == 0 && p.Percent <= 100;

    // Lays everything out for layout l (hero DIP), seen at `zoom` from origin (ox, oy).
    public void Layout(MainWindow.BarGeometry l, bool darkInk, int shownApps, int runningIndex, double magnify, double zoom, double ox, double oy,
                       StatusLook status, string launchpadIcon)
    {
        // Material edits change the rendered glass, not the marks on it. Keep their geometries
        // and brushes until their layout, ink or status actually changes; clocks update separately.
        var layout = (l, darkInk, shownApps, runningIndex, magnify, zoom, ox, oy, status);
        if (_layout == layout && _launchpadIcon == launchpadIcon) return;
        _layout = layout;
        _launchpadIcon = launchpadIcon;
        var inkColor = darkInk ? DarkInk : LightInk;
        _inkBrush.Color = inkColor;
        var ink = _inkBrush;
        double X(double u) => (l.Map(u) - ox) * zoom;
        double Y(double v) => (v - oy) * zoom;
        double iconTop = l.PlateY + (l.P - l.Icon) / 2, iconBottom = iconTop + l.Icon, barBottom = l.BarY + l.BarH;

        // One entry: its displayed square (left, top, size) under the wave, grown from its bottom.
        (double X, double Y, double Size) Item(double plateLeft)
        {
            double bx = plateLeft + l.P / 2;
            double grow = (l.Map(bx + l.P / 2) - l.Map(bx - l.P / 2)) / l.P;
            double size = l.Icon * grow * zoom;
            return (X(bx) - size / 2, Y(iconBottom) - size, size);
        }

        // The hover plate, only without the magnification (concentric with the bar).
        bool plate = shownApps > 1 && magnify <= 1.001;
        _plate.Visibility = plate ? Visibility.Visible : Visibility.Collapsed;
        if (plate)
        {
            _plateBrush.Color = darkInk ? Color.FromArgb(26, 0, 0, 0) : Color.FromArgb(51, 255, 255, 255);
            _plate.Width = _plate.Height = l.P * zoom;
            _plate.CornerRadius = new CornerRadius(l.PlateR * zoom);
            Canvas.SetLeft(_plate, X(l.PlateX(1)));
            Canvas.SetTop(_plate, Y(l.PlateY));
        }

        // The logo: 80 % of the icon square, a gap of 8 %, corners of 10 % of a pane.
        {
            var (x, y, size) = Item(l.StartX);
            double w = size * 0.80, gap = w * 0.08, pane = (w - gap) / 2, r = pane * 0.10;
            _logo.Width = _logo.Height = w;
            for (int i = 0; i < 4; i++)
            {
                var part = _logoPanes[i];
                double px = i % 2 * (pane + gap), py = i / 2 * (pane + gap);
                part.Width = part.Height = pane;
                part.RadiusX = part.RadiusY = r;
                var gradient = (LinearGradientBrush)ContinuousCorners.Original(part)!;
                gradient.StartPoint = new Point(-px / pane, -py / pane);
                gradient.EndPoint = new Point((w - px) / pane, (w - py) / pane);
                Canvas.SetLeft(part, px);
                Canvas.SetTop(part, py);
            }
            Canvas.SetLeft(_logo, x + (size - w) / 2);
            Canvas.SetTop(_logo, y + (size - w) / 2);
        }

        _dotBrush.Color = Color.FromArgb((byte)(inkColor.A * 0.45), inkColor.R, inkColor.G, inkColor.B);
        for (int i = 0; i < Icons.Count; i++)
        {
            bool shown = i < shownApps;
            Icons[i].Visibility = shown ? Visibility.Visible : Visibility.Collapsed;
            _dots[i].Visibility = shown && i == runningIndex ? Visibility.Visible : Visibility.Collapsed;
            if (!shown) continue;
            var (x, y, size) = Item(l.PlateX(i));
            Icons[i].Width = Icons[i].Height = size;
            Canvas.SetLeft(Icons[i], x);
            Canvas.SetTop(Icons[i], y);
            // Running: a 1.9 DIP dot in the gap under the icon, 45 % ink (not the foreground app).
            double r = 1.9 * l.S * zoom;
            _dots[i].Width = _dots[i].Height = 2 * r;
            _dots[i].Fill = _dotBrush;
            Canvas.SetLeft(_dots[i], X(l.PlateX(i) + l.P / 2) - r);
            Canvas.SetTop(_dots[i], Y((iconBottom + barBottom) / 2) - r);
        }

        _sepBrush.Color = Color.FromArgb((byte)(255 * (darkInk ? 0.18 : 0.30)), inkColor.R, inkColor.G, inkColor.B);
        // The separator: 46 % of the bar's height, 30 % (light) or 18 % (dark) ink.
        _sep.Visibility = l.SepX >= 0 ? Visibility.Visible : Visibility.Collapsed;
        if (l.SepX >= 0)
        {
            double h = l.BarH * 0.46;
            _sep.Width = Math.Max(1, l.S * zoom);
            _sep.Height = h * zoom;
            _sep.Fill = _sepBrush;
            Canvas.SetLeft(_sep, X(l.SepX) - _sep.Width / 2);
            Canvas.SetTop(_sep, Y(l.BarY + (l.BarH - h) / 2));
        }

        // Show Desktop: a line like the separator at the bar's right end.
        _desk.Visibility = l.DeskX >= 0 ? Visibility.Visible : Visibility.Collapsed;
        if (l.DeskX >= 0)
        {
            double h = l.BarH * 0.46;
            _desk.Width = Math.Max(1, l.S * zoom);
            _desk.Height = h * zoom;
            _desk.Fill = _sepBrush;
            Canvas.SetLeft(_desk, X(l.DeskX) - _desk.Width / 2);
            Canvas.SetTop(_desk, Y(l.BarY + (l.BarH - h) / 2));
        }

        // Glyph entries: Segoe Fluent Icons at 62 % of the icon square.
        void Entry(FontIcon view, double plateLeft)
        {
            view.Visibility = plateLeft < 0 ? Visibility.Collapsed : Visibility.Visible;
            if (plateLeft < 0) return;
            var (x, y, size) = Item(plateLeft);
            view.Width = view.Height = size;
            view.FontSize = Math.Max(4, size * 0.62);
            view.Foreground = ink;
            Canvas.SetLeft(view, x);
            Canvas.SetTop(view, y);
        }
        Entry(_tray, l.TrayX);
        Entry(_bell, l.BellX);
        LayoutExtras(l, Item, inkColor, launchpadIcon);

        _ime.Visibility = _imePrev.Visibility = _imePath.Visibility = _imePrevPath.Visibility = Visibility.Collapsed;
        _imeAt = null;
        if (l.ImeX >= 0)
        {
            var (x, y, size) = Item(l.ImeX);
            _imeAt = (x, y, size, inkColor);
            ShowIme(_imeState.Glyph, _imeState.Prev, _imeState.Fade);
        }

        // The bell's count: an accent disc, max(9, 46 % of the icon) across, on its upper right.
        _badge.Visibility = l.BellX < 0 ? Visibility.Collapsed : Visibility.Visible;
        if (l.BellX >= 0)
        {
            var (x, y, size) = Item(l.BellX);
            double d = Math.Max(9 * l.S * zoom, size * 0.46);
            _badge.Width = _badge.Height = d;
            _badge.CornerRadius = new CornerRadius(d / 2);
            _badgeText.FontSize = Math.Max(3, d * 0.62);
            Canvas.SetLeft(_badge, x + size - d * 0.35 - d / 2);
            Canvas.SetTop(_badge, y + d * 0.15 - d / 2);
        }

        bool icons = l.StatusExtra > 0;
        _status.Visibility = l.StatusX < 0 || icons ? Visibility.Collapsed : Visibility.Visible;
        _statusIcons.Visibility = l.StatusX < 0 || !icons ? Visibility.Collapsed : Visibility.Visible;
        _statusAt = null;
        if (l.StatusX >= 0)
        {
            // The three icons are centred on the wider entry (an icon square drawn past its sides).
            var (x, y, size) = Item(l.StatusX + l.StatusExtra / 2);
            _statusAt = (x, y, size, icons, inkColor);
            ShowStatus(status);
        }

        // The clock: moves with the stretched bar, does not grow; time 25 %, date 19 % of the bar's
        // height, line heights 1.22 and 1.25, centred on the bar; a halo just below (32 % black or 35 % white).
        bool clock = l.Clock;
        foreach (var t in new[] { _time, _date, _timeHalo, _dateHalo }) t.Visibility = clock ? Visibility.Visible : Visibility.Collapsed;
        if (clock)
        {
            double lineT = l.TimePx * 1.22, lineD = l.DatePx * 1.25;
            double cx = X(l.ClockX + l.ClockW / 2), w = Math.Max(l.ClockW, l.TimePx * 4) * zoom;
            double top = Y(l.BarY + l.BarH / 2 - (lineT + lineD) / 2);
            double halo = Math.Max(0.6 * l.S, 0.5) * zoom;
            _haloBrush.Color = darkInk ? Color.FromArgb(89, 255, 255, 255) : Color.FromArgb(82, 0, 0, 0);
            var haloBrush = _haloBrush;
            void Place(TextBlock t, double size, double line, double y, Brush brush)
            {
                t.FontSize = Math.Max(1, size * zoom);
                t.LineHeight = line * zoom;
                t.Width = w;
                t.Foreground = brush;
                Canvas.SetLeft(t, cx - w / 2);
                Canvas.SetTop(t, y);
            }
            Place(_timeHalo, l.TimePx, lineT, top + halo, haloBrush);
            Place(_dateHalo, l.DatePx, lineD, top + lineT * zoom + halo, haloBrush);
            Place(_time, l.TimePx, lineT, top, ink);
            Place(_date, l.DatePx, lineD, top + lineT * zoom, ink);
        }
    }
}

// The height of a line of text in a TextBlock's font: what DWrite centres in a box (Canvas::Text's
// centred paragraphs), so marks sit where the core's do.
internal static class TextMetrics
{
    private static readonly Dictionary<(string, double, int), double> Lines = [];

    public static double Line(TextBlock t)
    {
        double size = Math.Round(t.FontSize * 4.0) / 4.0;
        if (size <= 0) size = t.FontSize;
        var key = (t.FontFamily.Source, size, (int)t.FontWeight.Weight);
        if (Lines.TryGetValue(key, out double h)) return h;
        if (Lines.Count > 256) Lines.Clear(); // sizes in a glide are all different: bounded, not a cache of every one
        var probe = new TextBlock
        {
            FontFamily = t.FontFamily,
            FontSize = size,
            FontWeight = t.FontWeight,
            Text = "Ag",
            IsTextScaleFactorEnabled = false,
        };
        probe.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return Lines[key] = probe.DesiredSize.Height;
    }
}

// What the status entry shows (SystemStatus and BarPainter's StatusMotion): the battery, the volume, the
// connection, and how far a changed volume or brightness has taken the ring over (0..1 each).
internal sealed record StatusLook(double Fill, int Percent, bool Battery, bool Charging, int Volume = 50, bool Muted = false, int Lit = 3,
                                  bool Connected = true, bool Wifi = true, double VolumeMix = 0, double BrightMix = 0)
{
    public bool Low => Battery && !Charging && Percent >= 0 && Percent <= 20;

    // The computer's own battery (none on a desktop: the ring is the volume), Wi-Fi at full strength.
    public static StatusLook Current()
    {
        var (fill, charging, _) = BarFace.Battery();
        bool battery = BarFace.HasBattery();
        return battery ? new StatusLook(fill, (int)Math.Round(fill * 100), true, charging) : new StatusLook(0.5, -1, false, false);
    }
}

// The status ring (BarPainter::DrawStatus) in an h x h square: a ring open by 64 degrees at the
// bottom, filled clockwise from the gap's left end; a bolt in the gap while charging (a small speaker
// where there is no battery, the ring then the volume); in the middle the iPhone's Wi-Fi fan
// (wifi_glyph.cpp), or the speaker or the sun while a changed level shows on the ring.
internal sealed partial class StatusGlyph : Canvas
{
    private readonly Path _track = Stroke(), _level = Stroke(), _bolt = new() { IsHitTestVisible = false };
    private readonly WifiFan _fan = new();
    private readonly TextBlock _wired = Symbol(), _speaker = Symbol(), _sun = Symbol(), _gapSpeaker = Symbol();
    private readonly SolidColorBrush _trackBrush = new(), _levelBrush = new(), _boltBrush = new();

    public StatusGlyph()
    {
        IsHitTestVisible = false;
        foreach (var e in new UIElement[] { _track, _level, _bolt, _fan, _wired, _speaker, _sun, _gapSpeaker }) Children.Add(e);
    }

    internal static Path Stroke() => new() { IsHitTestVisible = false, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round };

    internal static TextBlock Symbol() => new()
    {
        IsHitTestVisible = false,
        Foreground = new SolidColorBrush(Colors.Transparent),
        FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
        TextAlignment = TextAlignment.Center,
        IsTextScaleFactorEnabled = false,
    };

    // A glyph centred in the w x w square at (x, y).
    internal static void Place(TextBlock t, string glyph, double x, double y, double w, double size, Color color)
    {
        t.Visibility = color.A < 1 ? Visibility.Collapsed : Visibility.Visible;
        t.Text = glyph;
        t.FontSize = Math.Max(1, size);
        t.Width = w;
        if (t.ReadLocalValue(TextBlock.ForegroundProperty) is SolidColorBrush brush) brush.Color = color;
        else t.Foreground = new SolidColorBrush(color);
        SetLeft(t, x);
        SetTop(t, y + (w - TextMetrics.Line(t)) / 2);
    }

    internal static Point At(double cx, double cy, double r, double deg) =>
        new(cx + r * Math.Cos(deg * Math.PI / 180), cy + r * Math.Sin(deg * Math.PI / 180));

    internal static PathGeometry Arc(double cx, double cy, double r, double from, double sweep)
    {
        var f = new PathFigure { StartPoint = At(cx, cy, r, from), IsClosed = false, IsFilled = false };
        f.Segments.Add(new ArcSegment
        {
            Point = At(cx, cy, r, from + sweep),
            Size = new Size(r, r),
            SweepDirection = SweepDirection.Clockwise,
            IsLargeArc = sweep > 180,
        });
        return new PathGeometry { Figures = { f } };
    }

    internal static Color Fade(Color c, double a) => Color.FromArgb((byte)Math.Round(c.A * Math.Clamp(a, 0, 1)), c.R, c.G, c.B);

    internal static Color Red(Color ink) => ink.R < 128 ? Color.FromArgb(255, 0xE0, 0x35, 0x2B) : Color.FromArgb(255, 0xFF, 0x45, 0x3A);

    internal static string VolumeGlyph(StatusLook s) =>
        s.Muted || s.Volume == 0 ? "\uE74F" : s.Volume < 34 ? "\uE993" : s.Volume < 67 ? "\uE994" : "\uE995";

    // The bolt (BarPainter's chargingBolt_): six points in a unit square.
    internal static readonly (double X, double Y)[] BoltPoints = [(0.18, -0.5), (-0.4, 0.08), (-0.02, 0.08), (-0.18, 0.5), (0.4, -0.08), (0.02, -0.08)];

    public void Update(double h, Color ink, StatusLook s)
    {
        Width = Height = h;
        double cx = h / 2, cy = h / 2;
        // Inside: the connection (the fan, the wired glyph), at 60 % of the square.
        void Connection(double size, double alpha)
        {
            bool wired = s.Connected && !s.Wifi;
            _fan.Visibility = !wired && alpha > 0.003 ? Visibility.Visible : Visibility.Collapsed;
            _wired.Visibility = Visibility.Collapsed;
            if (alpha <= 0.003) return;
            if (wired)
            {
                Place(_wired, "\uE839", cx - size, cy - size / 2, 2 * size, size * 0.62, Fade(ink, alpha));
            }
            else
            {
                int lit = !s.Connected ? -1 : s.Lit;
                _fan.Update(cx, cy, size * 0.62, lit, Fade(ink, alpha), Fade(ink, (lit < 0 ? 0.32 : 0.24) * alpha));
            }
        }
        // The ring (the preview's computer has a playback device, so there always is one).
        const double Gap = 64, Start = 90 + Gap / 2, Sweep = 360 - Gap;
        double rad = h * 0.44, stroke = Math.Max(0.75, h * 0.07);
        double shown = Math.Min(1, s.VolumeMix + s.BrightMix);
        double mix = s.Battery ? s.VolumeMix : 1 - s.BrightMix; // 1: the ring shows the volume
        var level = ink;
        if (s.Low)
        {
            var red = Red(ink);
            level = Color.FromArgb(ink.A, (byte)(red.R + (ink.R - red.R) * shown),
                (byte)(red.G + (ink.G - red.G) * shown), (byte)(red.B + (ink.B - red.B) * shown));
        }
        bool silent = s.Muted && mix > 0.5;
        _track.Data = Arc(cx, cy, rad, Start, Sweep);
        _trackBrush.Color = Fade(ink, silent ? 0.16 : 0.24);
        _track.Stroke = _trackBrush;
        _track.StrokeThickness = stroke;
        double fill = Math.Clamp(s.Fill, 0, 1);
        _level.Visibility = fill > 0.0005 ? Visibility.Visible : Visibility.Collapsed;
        _level.Data = Arc(cx, cy, rad, Start, Math.Max(0.01, Sweep * Math.Min(fill, 0.9999)));
        _levelBrush.Color = level;
        _level.Stroke = _levelBrush;
        _level.StrokeThickness = stroke;

        // In the gap: the bolt while charging (fading out while a level shows).
        double bolt = s.Battery && s.Charging ? 1 - shown : 0;
        _bolt.Visibility = bolt > 0.003 ? Visibility.Visible : Visibility.Collapsed;
        if (bolt > 0.003)
        {
            Point P((double X, double Y) p) => new(cx + p.X * 0.17 * h, cy + (0.44 + p.Y * 0.26) * h);
            var f = new PathFigure { IsClosed = true, IsFilled = true, StartPoint = P(BoltPoints[0]) };
            for (int i = 1; i < BoltPoints.Length; i++) f.Segments.Add(new LineSegment { Point = P(BoltPoints[i]) });
            _bolt.Data = new PathGeometry { Figures = { f } };
            _boltBrush.Color = Fade(ink, bolt);
            _bolt.Fill = _boltBrush;
        }

        // Inside: the connection, or the speaker or the sun while a level shows.
        double speaker = s.Battery ? s.VolumeMix : 0;
        Connection(h * 0.6, 1 - Math.Min(1, speaker + s.BrightMix));
        Place(_speaker, VolumeGlyph(s), 0, 0, h, h * 0.40, Fade(ink, speaker));
        Place(_sun, "\uE706", 0, 0, h, h * 0.40, Fade(ink, s.BrightMix));
        // No battery: the ring is the volume, a small speaker in its gap (gone while the brightness shows).
        double gap = !s.Battery ? 1 - s.BrightMix : 0;
        Place(_gapSpeaker, VolumeGlyph(s), cx - h / 4, cy + rad - h / 4, h / 2, h * 0.22, Fade(ink, gap));
    }
}

// The iPhone's Wi-Fi fan (wifi_glyph.cpp): a wedge and two arcs about a point half a radius below
// the centre, as many lit as the signal (-1: offline, all faint).
internal sealed partial class WifiFan : Canvas
{
    private readonly Path _wedge = new() { IsHitTestVisible = false, StrokeLineJoin = PenLineJoin.Round };
    private readonly Path _arc1 = StatusGlyph.Stroke(), _arc2 = StatusGlyph.Stroke();
    private readonly SolidColorBrush _wedgeBrush = new(), _arc1Brush = new(), _arc2Brush = new();

    public WifiFan()
    {
        IsHitTestVisible = false;
        _wedge.Fill = _wedge.Stroke = _wedgeBrush;
        _arc1.Stroke = _arc1Brush;
        _arc2.Stroke = _arc2Brush;
        Children.Add(_wedge);
        Children.Add(_arc1);
        Children.Add(_arc2);
    }

    public void Update(double cx, double cy, double radius, int lit, Color on, Color off)
    {
        const double Span = 78, From = 270 - Span / 2;
        double oy = cy + 0.5 * radius, r0 = 0.37 * radius;
        var wedge = new PathFigure { StartPoint = new Point(cx, oy), IsClosed = true, IsFilled = true };
        wedge.Segments.Add(new LineSegment { Point = StatusGlyph.At(cx, oy, r0, From) });
        wedge.Segments.Add(new ArcSegment
        {
            Point = StatusGlyph.At(cx, oy, r0, From + Span),
            Size = new Size(r0, r0),
            SweepDirection = SweepDirection.Clockwise,
        });
        _wedge.Data = new PathGeometry { Figures = { wedge } };
        _wedgeBrush.Color = lit >= 1 ? on : off;
        _wedge.StrokeThickness = 0.19 * 0.3 * radius;
        _arc1.Data = StatusGlyph.Arc(cx, oy, 0.595 * radius, From, Span);
        _arc1Brush.Color = lit >= 2 ? on : off;
        _arc1.StrokeThickness = 0.19 * radius;
        _arc2.Data = StatusGlyph.Arc(cx, oy, 0.905 * radius, From, Span);
        _arc2Brush.Color = lit >= 3 ? on : off;
        _arc2.StrokeThickness = 0.19 * radius;
    }
}

// The status entry as the Windows taskbar shows it (BarPainter::DrawStatusIcons), centred on an
// h x h square and wider than it: the network, the speaker with a wave for each third of the
// volume, the battery with its level (a bolt cut out of it while charging).
internal sealed partial class StatusIcons : Canvas
{
    private readonly WifiFan _fan = new();
    private readonly TextBlock _wired = StatusGlyph.Symbol();
    private readonly Path _cone = new() { IsHitTestVisible = false, StrokeLineJoin = PenLineJoin.Round };
    private readonly Path[] _waves = [StatusGlyph.Stroke(), StatusGlyph.Stroke(), StatusGlyph.Stroke()];
    private readonly Path _cross = StatusGlyph.Stroke();
    private readonly Path _body = new() { IsHitTestVisible = false }, _nub = new() { IsHitTestVisible = false }, _fill = new() { IsHitTestVisible = false };
    private readonly SolidColorBrush _onBrush = new(), _coneBrush = new(), _waveOffBrush = new(), _outlineBrush = new(), _lowBrush = new();

    public StatusIcons()
    {
        IsHitTestVisible = false;
        _cone.Fill = _cone.Stroke = _coneBrush;
        _cross.Stroke = _onBrush;
        _body.Stroke = _nub.Fill = _outlineBrush;
        foreach (var e in new UIElement[] { _fan, _wired, _cone, _waves[0], _waves[1], _waves[2], _cross, _body, _nub, _fill }) Children.Add(e);
    }

    private static PathFigure Figure(ReadOnlySpan<Point> pts)
    {
        var f = new PathFigure { StartPoint = pts[0], IsClosed = true, IsFilled = true };
        for (int i = 1; i < pts.Length; i++) f.Segments.Add(new LineSegment { Point = pts[i] });
        return f;
    }

    public void Update(double h, Color ink, StatusLook s)
    {
        Width = Height = h;
        double cy = h / 2, stroke = Math.Max(0.75, h * 0.075);
        double netW = 0.62 * h, speakerW = 0.60 * h, batteryW = 0.68 * h, gap = 0.18 * h;
        double x = h / 2 - (netW + speakerW + (s.Battery ? batteryW + gap : 0) + gap) / 2;
        _onBrush.Color = ink;
        var on = _onBrush;

        // The network: the fan lit by the signal, the wired glyph, or a faint fan offline.
        bool wired = s.Connected && !s.Wifi;
        _fan.Visibility = wired ? Visibility.Collapsed : Visibility.Visible;
        _wired.Visibility = wired ? Visibility.Visible : Visibility.Collapsed;
        if (wired)
        {
            StatusGlyph.Place(_wired, "\uE839", x - h, cy - h / 2, netW + 2 * h, h * 0.62, ink);
        }
        else
        {
            int lit = !s.Connected ? -1 : s.Lit;
            _fan.Update(x + netW / 2, cy, h * 0.46, lit, ink, StatusGlyph.Fade(ink, lit < 0 ? 0.32 : 0.24));
        }
        x += netW + gap;

        // The speaker: a cone, a wave for each third of the volume (the others faint); muted, a cross.
        _cone.Data = new PathGeometry
        {
            Figures =
            {
                Figure(
                [
                    new Point(x, cy - 0.10 * h), new Point(x + 0.11 * h, cy - 0.10 * h), new Point(x + 0.25 * h, cy - 0.22 * h),
                    new Point(x + 0.25 * h, cy + 0.22 * h), new Point(x + 0.11 * h, cy + 0.10 * h), new Point(x, cy + 0.10 * h),
                ]),
            },
        };
        _coneBrush.Color = StatusGlyph.Fade(ink, s.Muted ? 0.55 : 1);
        _cone.StrokeThickness = stroke * 0.6;
        double ox = x + 0.25 * h;
        int waves = s.Volume <= 0 ? 0 : s.Volume < 34 ? 1 : s.Volume < 67 ? 2 : 3;
        _waveOffBrush.Color = StatusGlyph.Fade(ink, 0.24);
        for (int i = 0; i < 3; i++)
        {
            _waves[i].Visibility = s.Muted ? Visibility.Collapsed : Visibility.Visible;
            double r = (0.11 + 0.105 * i) * h;
            _waves[i].Data = StatusGlyph.Arc(ox, cy, r, -43, 86);
            _waves[i].Stroke = i < waves ? on : _waveOffBrush;
            _waves[i].StrokeThickness = stroke;
        }
        _cross.Visibility = s.Muted ? Visibility.Visible : Visibility.Collapsed;
        if (s.Muted)
        {
            double c = ox + 0.19 * h, k = 0.08 * h;
            var g = new PathGeometry();
            foreach (var (a, b) in new[] { (new Point(c - k, cy - k), new Point(c + k, cy + k)), (new Point(c - k, cy + k), new Point(c + k, cy - k)) })
            {
                var f = new PathFigure { StartPoint = a, IsClosed = false, IsFilled = false };
                f.Segments.Add(new LineSegment { Point = b });
                g.Figures.Add(f);
            }
            _cross.Data = g;
            _cross.StrokeThickness = stroke;
        }
        x += speakerW + gap;

        // The battery: an outline and its nub, the level inside (red when low), a bolt cut out of it
        // while charging (and drawn where there is no level).
        foreach (var p in new[] { _body, _nub, _fill }) p.Visibility = s.Battery ? Visibility.Visible : Visibility.Collapsed;
        if (!s.Battery) return;
        double bw = 0.58 * h, bh = 0.36 * h, rad = 0.09 * h;
        var body = new Rect(x + stroke / 2, cy - bh / 2, bw - stroke / 2, bh);
        _outlineBrush.Color = StatusGlyph.Fade(ink, 0.9);
        _body.Data = RoundRect(body, rad);
        _body.StrokeThickness = stroke;
        _nub.Data = RoundRect(new Rect(body.Right + 0.03 * h, cy - 0.075 * h, 0.05 * h, 0.15 * h), 0.02 * h);
        double pad = stroke + 0.035 * h, level = Math.Clamp(s.Percent / 100.0, 0, 1);
        var inner = new Rect(body.Left + pad, body.Top + pad, Math.Max(0, body.Width - 2 * pad), Math.Max(0, body.Height - 2 * pad));
        var levelRect = new Rect(inner.Left, inner.Top, Math.Max(0.06 * h, inner.Width * level), inner.Height);
        var shape = new GeometryGroup { FillRule = FillRule.EvenOdd };
        shape.Children.Add(RoundRect(levelRect, 0.035 * h));
        if (s.Charging)
        {
            double bx = (body.Left + body.Right) / 2, k = bh * 0.62;
            shape.Children.Add(new PathGeometry
            {
                Figures = { Figure([.. StatusGlyph.BoltPoints.Select(p => new Point(bx + p.X * k, cy + p.Y * k * 1.5))]) },
            });
        }
        _fill.Data = shape;
        if (s.Low)
        {
            _lowBrush.Color = StatusGlyph.Red(ink);
            _fill.Fill = _lowBrush;
        }
        else
        {
            _fill.Fill = on;
        }
    }

    private static PathGeometry RoundRect(Rect r, double radius)
    {
        radius = Math.Min(radius, Math.Min(r.Width, r.Height) / 2);
        var f = new PathFigure { StartPoint = new Point(r.Left + radius, r.Top), IsClosed = true, IsFilled = true };
        void Corner(double x, double y) => f.Segments.Add(
            new ArcSegment { Point = new Point(x, y), Size = new Size(radius, radius), SweepDirection = SweepDirection.Clockwise });
        f.Segments.Add(new LineSegment { Point = new Point(r.Right - radius, r.Top) });
        Corner(r.Right, r.Top + radius);
        f.Segments.Add(new LineSegment { Point = new Point(r.Right, r.Bottom - radius) });
        Corner(r.Right - radius, r.Bottom);
        f.Segments.Add(new LineSegment { Point = new Point(r.Left + radius, r.Bottom) });
        Corner(r.Left, r.Bottom - radius);
        f.Segments.Add(new LineSegment { Point = new Point(r.Left, r.Top + radius) });
        Corner(r.Left + radius, r.Top);
        return new PathGeometry { Figures = { f } };
    }
}
