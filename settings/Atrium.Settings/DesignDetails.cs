using System.Diagnostics;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The design language page's technical details, parameter tables, motion curves and interactive
// curve cards.
public sealed partial class MainWindow
{
    private static class Motion
    {
        public static double EaseOut(double t)
        {
            t = Math.Clamp(t, 0, 1);
            double u = 1 - t;
            return 1 - u * u * u;
        }

        public static double EaseOutBack(double t, double s)
        {
            t = Math.Clamp(t, 0, 1) - 1;
            return 1 + (s + 1) * t * t * t + s * t * t;
        }

        // A spring from 0 at rest towards 1, at time t (closed form, as motion::Spring::Step).
        public static double Spring(double t, double response, double damping)
        {
            double w0 = 2 * Math.PI / response, z = damping, y;
            if (Math.Abs(z - 1) < 1e-3)
            {
                y = -Math.Exp(-w0 * t) * (1 + w0 * t);
            }
            else if (z < 1)
            {
                double a = z * w0, wd = w0 * Math.Sqrt(1 - z * z);
                y = -Math.Exp(-a * t) * (Math.Cos(wd * t) + a / wd * Math.Sin(wd * t));
            }
            else
            {
                double q = w0 * Math.Sqrt(z * z - 1), r1 = -z * w0 + q, r2 = -z * w0 - q, c2 = r1 / (r2 - r1);
                y = (-1 - c2) * Math.Exp(r1 * t) + c2 * Math.Exp(r2 * t);
            }
            return 1 + y;
        }

        public static double Approach(double t, double rate) => 1 - Math.Exp(-rate * t);

        // The launch hop (render/bar_painter.cpp): sin², no speed at either end.
        public static double Hop(double t, double seconds)
        {
            if (t <= 0 || t >= seconds) return 0;
            double s = Math.Sin(Math.PI * t / seconds);
            return s * s;
        }
    }

    private sealed record Curve(string Name, string Spec, string Formula, string Use, double Seconds, Func<double, double> At);

    private static Curve[] Curves =>
    [
        new(Loc.T("kPop · Spring"), Loc.T("Duration 0.36 s · Bounce 0.38"),
            Loc.T("Closed-form spring, damping ratio 0.62; split into 12 Hermite segments for DirectComposition"),
            Loc.T("Popup appears: scale 0.92 → 1, peaks at 1.0067"), 0.6, t => Motion.Spring(t, 0.36, 0.62)),
        new(Loc.T("kSmooth · Spring"), Loc.T("Duration 0.48 s · Bounce 0.23"), Loc.T("Closed-form spring, damping ratio 0.77"),
            Loc.T("Status ring: battery, volume and brightness"), 0.8, t => Motion.Spring(t, 0.48, 0.77)),
        new(Loc.T("Stack · Critically damped spring"), Loc.T("Duration 0.60 s · Bounce 0"), Loc.T("Closed-form spring, damping ratio 1"),
            Loc.T("Scrolling through notifications snaps to one"), 1.0, t => Motion.Spring(t, 0.60, 1.0)),
        new(Loc.T("EaseOut · Cubic ease-out"), "1 − (1 − t)³ · 320 ms", "1 − (1 − t)³",
            Loc.T("Page changes, fades, staggered lists, ink shifts"), 0.32, t => Motion.EaseOut(t / 0.32)),
        new(Loc.T("EaseOutBack · Back ease-out"), "s = 1.5 · 260 ms", "1 + (s + 1)(t − 1)³ + s(t − 1)²",
            Loc.T("Start menu tiles appearing"), 0.26, t => Motion.EaseOutBack(t / 0.26, 1.5)),
        new(Loc.T("Approach · Exponential approach"), Loc.T("In 20/s · Out 11/s"),
            Loc.T("1 − e^(−rate · t), continuing from the current value each frame"),
            Loc.T("Hover and press plates"), 0.3, t => Motion.Approach(t, 20)),
        new(Loc.T("Launch · One small hop"), Loc.T("sin² · 480 ms · 0.32 × icon height"),
            Loc.T("sin²(πt / 0.48); no velocity jump at takeoff or landing"),
            Loc.T("Opening a taskbar app: one hop only"), 0.48, t => Motion.Hop(t, 0.48)),
    ];

    // The type ramp (DESIGN.md §1.2): name, size, MiSans file, its weight, macOS counterpart, where.
    private static (string Name, double Size, string Font, string Weight, string Mac, string Use)[] TypeRamp =>
    [
        ("Large Title", 30, "TitleFont", "Semibold 520", "Large Title · 26", Loc.T("Page titles, product name")),
        ("Title", 20, "TitleFont", "Semibold 520", "Title 1 · 22", Loc.T("Empty-state titles, calendar month")),
        ("Value", 15, "EmphasisFont", "Demibold 450", "—", Loc.T("Slider values (tabular figures)")),
        ("Row", 15, "TextFont", "Medium 380", "Body · 13", Loc.T("Settings row titles")),
        ("Body", 13.5, "TextFont", "Medium 380", "Body · 13", Loc.T("Panel body, notification content")),
        ("Section", 13, "EmphasisFont", "Demibold 450", "—", Loc.T("Group headings")),
        ("Note", 13, "TextFont", "Medium 380", "Callout · 12", Loc.T("Notes under a row")),
        ("Footnote", 12, "TextFont", "Medium 380", "Footnote · 10", Loc.T("Footnotes; glass rises in Demibold")),
    ];

    // Logical width from, typical screens, bar height, icon, bottom margin, magnification.
    private static (double From, string Screens, int Height, int Icon, int Margin, double Magnify)[] SizeClasses =>
    [
        (0, Loc.T("1366×768 @100%, 1920×1080 @150%"), 52, 26, 8, 1.5),
        (1280, Loc.T("1920×1080 @125%, 2560×1600 @175%, 3200×2000 @200%"), 60, 30, 10, 1.7),
        (1680, Loc.T("1920×1080 @100%, 2560×1440 @125%"), 64, 32, 12, 1.7),
        (2400, Loc.T("2560×1440 @100%, 3840×2160 @150%"), 68, 34, 14, 1.6),
    ];

    private static (string Element, string Radius, string Rule)[] Corners =>
    [
        (Loc.T("Bar"), Loc.T("cornerRadius, default 30"), Loc.T("At most half the bar height: a capsule by default")),
        (Loc.T("Popup panel"), Loc.T("Same as the bar, 8 to height / 2"), Loc.T("Popup and bar share one material and one shape")),
        (Loc.T("Modules inside a panel"), Loc.T("min(panel radius, 22)"), Loc.T("A very round panel does not turn its modules into capsules")),
        (Loc.T("Tray icon plate"), "clamp(R − m − 2, 6, 12)", Loc.T("Concentric: outer radius minus the inset")),
        (Loc.T("Desktop widget"), Loc.T("22 (cell 164)"), Loc.T("About 13% of the side, as macOS widgets")),
        (Loc.T("Capsule control"), Loc.T("Height / 2"), Loc.T("G3 continuous ends; round knobs stay circular")),
        (Loc.T("Hover and press plates"), "8–12", Loc.T("At most the container radius minus the inset")),
    ];

    private readonly List<(double Start, Curve Curve, Action<double> Show)> _playing = [];
    private bool _designTicking;
    private static readonly Stopwatch DesignClock = Stopwatch.StartNew();

    // The details are built when they are opened, and again with the current settings each time the page shows.
    private void RefreshDesignDetails(bool opening = false)
    {
        if (_designDetails == null || (!opening && _designDetails.Visibility != Visibility.Visible)) return;
        _designDetails.Children.Clear();
        BuildDesignDetails(_designDetails);
        ContinuousCorners.Install(_designDetails);
    }

    private void BuildDesignDetails(StackPanel into)
    {
        into.Children.Add(Section(Loc.T("Motion parameters"),
            Loc.T("Springs are given Apple's way: duration and bounce (= 1 − damping ratio). Popup animations play in DirectComposition; the rest are computed per frame by the render thread on the composition clock."),
            Table([Loc.T("Curve"), Loc.T("Parameters"), Loc.T("Form"), Loc.T("Used for")], [1.2, 1.2, 2.0, 1.6],
                Curves.Select(c => new[] { c.Name, c.Spec, c.Formula, c.Use }))));
        var liquid = BarSettings.Liquid();
        var frosted = BarSettings.Frosted();
        string P(double v) => $"{v * 100:0}%";
        string Hex(uint c) => "#" + c.ToString("X6");
        into.Children.Add(Section(Loc.T("Material parameters"),
            Loc.T("Two presets and the current settings. The bar, popups and widgets share one shader (glass.hlsl) and one parameter set."),
            Table([Loc.T("Parameters"), Loc.T("Liquid glass"), Loc.T("Frosted glass"), Loc.T("Current")], [1.4, 1.0, 1.0, 1.0],
            [
                [Loc.T("Transparency"), P(liquid.Transparency), P(frosted.Transparency), P(CurrentSettings.Transparency)],
                [Loc.T("Blur"), P(liquid.Blur), P(frosted.Blur), P(CurrentSettings.Blur)],
                [Loc.T("Refraction"), $"{liquid.Refraction:0} px", $"{frosted.Refraction:0} px", $"{CurrentSettings.Refraction:0} px"],
                [Loc.T("Refraction edge"), $"{liquid.Edge:0} px", $"{frosted.Edge:0} px", $"{CurrentSettings.Edge:0} px"],
                [Loc.T("Dispersion"), P(liquid.Dispersion), P(frosted.Dispersion), P(CurrentSettings.Dispersion)],
                [Loc.T("Highlight"), P(liquid.Highlight), P(frosted.Highlight), P(CurrentSettings.Highlight)],
                [Loc.T("Shadow"), P(liquid.Shadow), P(frosted.Shadow), P(CurrentSettings.Shadow)],
                [Loc.T("Tint"), Hex(liquid.Tint), Hex(frosted.Tint), Hex(CurrentSettings.Tint)],
            ])));
        into.Children.Add(Section(Loc.T("Typography parameters"),
            Loc.T("Weights are never synthesized: the style picks a font file. MiSans's weights sit one step below their names: Medium 380 as regular, Demibold 450 as medium, Semibold 520 as semibold."),
            TypeCard(true)));
        into.Children.Add(Section(Loc.T("Corner radius"),
            Loc.T("Every outline resolves as G3: curvature and its rate are zero where straight edges meet. Cards 24, buttons take half their height; circles keep constant curvature."),
            Table([Loc.T("Element"), Loc.T("Corner radius"), Loc.T("Rule")], [1.1, 1.3, 2.2], Corners.Select(c => new[] { c.Element, c.Radius, c.Rule }))));
        into.Children.Add(RadiusCard());
        into.Children.Add(Section(Loc.T("Spacing"), null,
            Table([Loc.T("Name"), Loc.T("Value"), Loc.T("Use")], [0.6, 0.5, 2.6], Spacing.Select(s => new[] { s.Name, s.Value.ToString(), s.Use }))));
        into.Children.Add(Section(Loc.T("Dimensions"),
            Loc.T("Resolution becomes logical width (pixels ÷ scale), then the bar's size is chosen. Icons are half the bar height; the bar is about 6% of the logical screen height."),
            SizeCard()));
        into.Children.Add(Section(Loc.T("Ink"),
            Loc.T("Decided by the brightness behind the glass: above 0.50 dark ink, below 0.42 light ink, with no switch in between; a 450 ms ease-out crossfade on change. Ink has three opacity levels."),
            InkCard()));
        var link = new HyperlinkButton
        {
            Content = Loc.T("Full specification: docs/DESIGN.md"),
            NavigateUri = new Uri("https://github.com/wynter-yaxley/atrium-desktop/blob/main/docs/DESIGN.md"),
            Padding = new Thickness(0),
        };
        into.Children.Add(link);
    }

    // A plain table on a card: a header in footnote ink, rows split by hairlines, numbers tabular.
    private static Border Table(string[] head, double[] widths, IEnumerable<string[]> rows)
    {
        var g = new Grid { ColumnSpacing = 16 };
        foreach (double w in widths) g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(w, GridUnitType.Star) });
        int r = 0;
        void Row(string[] cells, bool header)
        {
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            if (!header && r > 1)
            {
                var line = new Rectangle { Height = 1, Fill = Tokens.Brush("SeparatorBrush"), VerticalAlignment = VerticalAlignment.Top };
                Grid.SetRow(line, r);
                Grid.SetColumnSpan(line, head.Length);
                g.Children.Add(line);
            }
            for (int c = 0; c < cells.Length && c < head.Length; c++)
            {
                var t = Tabular(Text(cells[c], header ? "FootnoteStyle" : c == 0 ? "RowTitleStyle" : "RowNoteStyle"));
                if (!header && c == 0) t.FontSize = 14;
                t.TextWrapping = TextWrapping.Wrap;
                t.Margin = new Thickness(0, header ? 0 : 9, 0, header ? 6 : 9);
                Grid.SetRow(t, r);
                Grid.SetColumn(t, c);
                g.Children.Add(t);
            }
            r++;
        }
        Row(head, true);
        foreach (string[] row in rows) Row(row, false);
        return Card(g);
    }

    // ---- Motion ----

    private ResponsiveGrid CurveGrid()
    {
        var grid = new ResponsiveGrid { MinCellWidth = 280, Gap = 16 };
        foreach (var c in Curves) grid.Children.Add(CurveCard(c));
        return grid;
    }

    // ---- Type: the samples (and in the details, the numbers) ----

    private static Border TypeCard(bool detailed)
    {
        var rows = new StackPanel { Spacing = 0 };
        for (int i = 0; i < TypeRamp.Length; i++)
        {
            var (name, size, font, weight, mac, use) = TypeRamp[i];
            var g = new Grid { ColumnSpacing = 16, Padding = new Thickness(0, 10, 0, 10) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(detailed ? 170 : 120) });
            if (detailed) g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(200) });
            var sample = new TextBlock
            {
                Text = detailed ? name : Loc.T("Under the glass, content first Aa 0123"),
                FontFamily = (FontFamily)Application.Current.Resources[font],
                FontSize = detailed ? 15 : size,
                VerticalAlignment = VerticalAlignment.Center,
                TextTrimming = TextTrimming.CharacterEllipsis,
            };
            var spec = Tabular(Text(detailed ? $"{size} · {weight}" : name, "RowNoteStyle"));
            spec.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(spec, 1);
            g.Children.Add(sample);
            g.Children.Add(spec);
            if (detailed)
            {
                var where = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                where.Children.Add(Text("macOS " + mac, "FootnoteStyle"));
                where.Children.Add(Text(use, "RowNoteStyle"));
                Grid.SetColumn(where, 2);
                g.Children.Add(where);
            }
            if (i > 0) rows.Children.Add(new Rectangle { Height = 1, Fill = Tokens.Brush("SeparatorBrush") });
            rows.Children.Add(g);
        }
        return Card(rows);
    }

    private Border CurveCard(Curve c)
    {
        // Drawn again at the card's width: the curve, its 0 and 1 lines, a cursor on it, and below
        // the same motion on a moving chip, the way it looks on screen.
        const double H = 96, Y0 = 82, Y1 = 22, Chip = 22;
        double w = 240;
        double X(double t) => 4 + (w - 8) * t / c.Seconds;
        double Y(double v) => Y0 + (Y1 - Y0) * v;

        var plot = new Canvas { Height = H };
        var lines = new[] { new Line { Style = StyleOf("GlyphFaintStyle"), StrokeDashArray = [2, 3] },
                            new Line { Style = StyleOf("GlyphFaintStyle"), StrokeDashArray = [2, 3] } };
        var line = new Polyline { Style = StyleOf("GlyphAccentStyle"), Points = [] };
        var cursor = new Ellipse { Width = 8, Height = 8, Style = StyleOf("GlyphAccentFillStyle") };
        foreach (var l in lines) plot.Children.Add(l);
        plot.Children.Add(line);
        plot.Children.Add(cursor);

        var track = new Grid { Height = 22 };
        track.Children.Add(new Border { CornerRadius = new CornerRadius(11), Background = Tokens.Brush("FillBrush") });
        var lane = new Canvas();
        var chip = new Rectangle { Width = Chip, Height = 14, RadiusX = 7, RadiusY = 7, Style = StyleOf("GlyphAccentFillStyle") };
        Canvas.SetTop(chip, 4);
        lane.Children.Add(chip);
        track.Children.Add(lane);

        double shown = c.Seconds;
        void Show(double t)
        {
            shown = Math.Min(t, c.Seconds);
            double v = c.At(shown);
            Canvas.SetLeft(cursor, X(shown) - 4);
            Canvas.SetTop(cursor, Y(v) - 4);
            Canvas.SetLeft(chip, 4 + (w - Chip - 8) * v);
        }
        void Draw(double width)
        {
            w = Math.Max(120, width);
            for (int k = 0; k < 2; k++)
            {
                lines[k].X2 = w;
                lines[k].Y1 = lines[k].Y2 = Y(k);
            }
            line.Points.Clear();
            for (int i = 0; i <= 120; i++)
            {
                double t = c.Seconds * i / 120;
                line.Points.Add(new Point(X(t), Y(c.At(t))));
            }
            Show(shown);
        }
        plot.SizeChanged += (_, e) => Draw(e.NewSize.Width);
        Draw(w);

        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(Text(c.Name, "RowTitleStyle"));
        var spec = Text(c.Spec, "RowNoteStyle");
        Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(spec, FontNumeralAlignment.Tabular);
        text.Children.Add(spec);
        text.Children.Add(Text(c.Use, "RowNoteStyle"));

        var body = new StackPanel { Spacing = 10 };
        body.Children.Add(text);
        body.Children.Add(plot);
        body.Children.Add(track);
        var card = Card(body);
        AutomationProperties.SetName(card, c.Name);
        card.PointerPressed += (_, _) => Play(c, Show);
        return card;
    }

    private void Play(Curve c, Action<double> show)
    {
        _playing.RemoveAll(p => p.Show == show);
        _playing.Add((DesignClock.Elapsed.TotalSeconds, c, show));
        show(0);
        if (_designTicking) return;
        _designTicking = true;
        CompositionTarget.Rendering += OnDesignFrame;
    }

    private void OnDesignFrame(object? sender, object e)
    {
        double now = DesignClock.Elapsed.TotalSeconds;
        for (int i = _playing.Count - 1; i >= 0; i--)
        {
            var (start, c, show) = _playing[i];
            show(now - start);
            if (now - start >= c.Seconds) _playing.RemoveAt(i);
        }
        if (_playing.Count > 0) return;
        CompositionTarget.Rendering -= OnDesignFrame;
        _designTicking = false;
    }

    private void StopDesignAnimations()
    {
        if (!_designTicking) return;
        CompositionTarget.Rendering -= OnDesignFrame;
        _designTicking = false;
        foreach (var (_, curve, show) in _playing) show(curve.Seconds);
        _playing.Clear();
    }

    // ---- Corners ----

    private static Border RadiusCard()
    {
        // Concentric: the bar's capsule radius, a 12 inset, the inner radius that keeps the gap even.
        const double R = 30, P = 12, Ww = 240, Hh = 132;
        var demo = new Canvas { Width = Ww, Height = Hh };
        var outer = new Rectangle { Width = Ww, Height = Hh, RadiusX = R, RadiusY = R, Style = StyleOf("GlyphInkStyle") };
        var inner = new Rectangle { Width = Ww - 2 * P, Height = Hh - 2 * P, RadiusX = R - P, RadiusY = R - P, Style = StyleOf("GlyphAccentStyle") };
        Canvas.SetLeft(inner, P);
        Canvas.SetTop(inner, P);
        demo.Children.Add(outer);
        demo.Children.Add(inner);
        var label = Text(Loc.F("R {0} − inset {1} = r {2}", R, P, R - P), "RowTitleStyle");
        label.Width = Ww - 2 * P;
        label.TextAlignment = TextAlignment.Center;
        Canvas.SetLeft(label, P);
        Canvas.SetTop(label, Hh / 2 - 10);
        demo.Children.Add(label);
        var left = new StackPanel { Spacing = 10, HorizontalAlignment = HorizontalAlignment.Center };
        left.Children.Add(demo);
        var why = Text(Loc.T("Inner radii shrink with the inset; straight-edge spacing stays even."), "RowNoteStyle");
        why.HorizontalAlignment = HorizontalAlignment.Center;
        left.Children.Add(why);

        var ladder = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 20, VerticalAlignment = VerticalAlignment.Center };
        (double R, string Use)[] steps = [(6, Loc.T("Min")), (10, Loc.T("Hover plate")), (22, Loc.T("Module · Widget")), (30, Loc.T("Bar and popup"))];
        foreach (var (r, use) in steps)
        {
            var s = new StackPanel { Spacing = 6, HorizontalAlignment = HorizontalAlignment.Center };
            // The bar's radius is half its height: drawn as the capsule it makes.
            s.Children.Add(new Rectangle
            {
                Width = r >= 30 ? 100 : 60,
                Height = 60,
                RadiusX = r,
                RadiusY = r,
                Style = StyleOf("GlyphInkStyle"),
                HorizontalAlignment = HorizontalAlignment.Center,
            });
            var v = Text(r.ToString(), "RowTitleStyle");
            v.HorizontalAlignment = HorizontalAlignment.Center;
            s.Children.Add(v);
            var u = Text(use, "FootnoteStyle");
            u.HorizontalAlignment = HorizontalAlignment.Center;
            s.Children.Add(u);
            ladder.Children.Add(s);
        }
        var grid = new ResponsiveGrid { MinCellWidth = 300, Gap = 24 };
        grid.Children.Add(left);
        grid.Children.Add(ladder);
        return Card(grid);
    }

    // ---- Sizes ----

    private Border SizeCard()
    {
        // This screen: physical pixels of the window's display over its scale.
        double scale = Content.XamlRoot?.RasterizationScale ?? 1;
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).OuterBounds;
        double logical = area.Width / scale;
        int mine = 0;
        for (int i = 0; i < SizeClasses.Length; i++)
            if (logical >= SizeClasses[i].From) mine = i;

        var g = new Grid { ColumnSpacing = 12, RowSpacing = 0 };
        foreach (double w in new[] { 1.1, 2.6, 0.7, 0.7, 0.7, 0.8 })
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(w, GridUnitType.Star) });
        string[] head = [Loc.T("Logical width"), Loc.T("Typical screens"), Loc.T("Bar height"), Loc.T("Icon"), Loc.T("Bottom margin"), Loc.T("Magnification")];
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (int c = 0; c < head.Length; c++)
        {
            var t = Text(head[c], "FootnoteStyle");
            t.Margin = new Thickness(c == 0 ? 10 : 0, 0, 0, 8);
            Grid.SetColumn(t, c);
            g.Children.Add(t);
        }
        for (int i = 0; i < SizeClasses.Length; i++)
        {
            var (from, screens, height, icon, margin, magnify) = SizeClasses[i];
            int row = i + 1;
            g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            if (i == mine)
            {
                var band = new Border { CornerRadius = new CornerRadius(8), Background = Tokens.Brush("FillBrush") };
                Grid.SetRow(band, row);
                Grid.SetColumnSpan(band, head.Length);
                g.Children.Add(band);
            }
            string range = i + 1 < SizeClasses.Length ? $"{(from == 0 ? "< " : from + "–")}{SizeClasses[i + 1].From - (from == 0 ? 0 : 1)}" : $"≥ {from}";
            string[] cells = [range, screens, height.ToString(), icon.ToString(), margin.ToString(), magnify.ToString("0.0")];
            for (int c = 0; c < cells.Length; c++)
            {
                var t = Text(cells[c], c == 0 ? "RowTitleStyle" : "RowNoteStyle");
                t.FontSize = c == 0 ? 13 : 12;
                t.Margin = new Thickness(c == 0 ? 10 : 0, 9, c == cells.Length - 1 ? 10 : 0, 9);
                t.VerticalAlignment = VerticalAlignment.Center;
                Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(t, FontNumeralAlignment.Tabular);
                Grid.SetRow(t, row);
                Grid.SetColumn(t, c);
                g.Children.Add(t);
            }
        }
        var (_, _, suggestedHeight, suggestedIcon, _, _) = SizeClasses[mine];
        var here = Text(Loc.F(
            "This screen: {0} × {1} pixels, scale {2:0}% → logical width {3:0}. Suggested bar height {4}, icons {5}; currently bar height {6:0}, icons {7:0}.",
            area.Width, area.Height, scale * 100, logical, suggestedHeight, suggestedIcon, CurrentSettings.Height, CurrentSettings.IconSize), "RowNoteStyle");
        here.Margin = new Thickness(10, 12, 10, 0);
        var s = new StackPanel();
        s.Children.Add(g);
        s.Children.Add(here);
        return Card(s);
    }

    // ---- Ink ----

    private static ResponsiveGrid InkCard()
    {
        static Border Panel(Color bg, Color ink, (string Name, double A)[] steps, Color warn, string title)
        {
            var s = new StackPanel { Spacing = 6 };
            var head = new TextBlock { Text = title, FontSize = 11, Foreground = new SolidColorBrush(Color.FromArgb(150, ink.R, ink.G, ink.B)) };
            s.Children.Add(head);
            foreach (var (name, a) in steps)
            {
                s.Children.Add(new TextBlock
                {
                    Text = $"{name} · {a * 100:0}%",
                    FontSize = 13.5,
                    FontFamily = (FontFamily)Application.Current.Resources[name == Loc.T("Primary text") ? "EmphasisFont" : "TextFont"],
                    Foreground = new SolidColorBrush(Color.FromArgb((byte)Math.Round(a * 255), ink.R, ink.G, ink.B)),
                });
            }

            var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 6, 0, 0) };
            foreach (var (c, label) in new[] { (Color.FromArgb(255, 0x0A, 0x84, 0xFF), "#0A84FF"), (warn, $"#{warn.R:X2}{warn.G:X2}{warn.B:X2}") })
            {
                var chip = new Border
                {
                    CornerRadius = new CornerRadius(10),
                    Padding = new Thickness(10, 3, 10, 4),
                    Background = new SolidColorBrush(c),
                    Child = new TextBlock { Text = label, FontSize = 11, Foreground = new SolidColorBrush(Colors.White) }
                };
                chips.Children.Add(chip);
            }
            s.Children.Add(chips);
            return new Border { Background = new SolidColorBrush(bg), CornerRadius = new CornerRadius(12), Padding = new Thickness(18, 14, 18, 16), Child = s };
        }
        var grid = new ResponsiveGrid { MinCellWidth = 280, Gap = 16, StretchLastRow = true };
        grid.Children.Add(Panel(Color.FromArgb(255, 0x24, 0x2A, 0x3A), Colors.White,
            [(Loc.T("Primary text"), 0.96), (Loc.T("Secondary text"), 0.76), (Loc.T("Tertiary text"), 0.56)],
            Color.FromArgb(255, 0xFF, 0x45, 0x3A), Loc.T("Light ink · dark backdrop")));
        grid.Children.Add(Panel(Color.FromArgb(255, 0xEC, 0xEE, 0xF3), Color.FromArgb(255, 0x11, 0x13, 0x17),
            [(Loc.T("Primary text"), 0.92), (Loc.T("Secondary text"), 0.66), (Loc.T("Tertiary text"), 0.46)],
            Color.FromArgb(255, 0xE0, 0x35, 0x2B), Loc.T("Dark ink · light backdrop")));
        return grid;
    }
}
