using System.Numerics;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The design language page, in two layers. First what Atrium looks and moves like, to look at and
// touch: the two glasses side by side, corners and curvature, type, spacing, colour, the dock and a
// notification, the curves. Then, behind "查看详细参数", the numbers: every one of them taken from
// the code (the curves use the core's own formulas, native/core/motion.h; the materials are
// BarSettings' presets; the colours are this app's resources).
public sealed partial class MainWindow
{
    // The spacing scale (DESIGN.md §4.1).
    private static (string Name, int Value, string Use)[] Spacing =>
    [
        ("2xs", 4, Loc.T("Icons and badges, tight elements")), ("xs", 8, Loc.T("Adjacent elements in a group")),
        ("s", 10, Loc.T("Cards inside a panel (kGap)")), ("m", 12, Loc.T("Popup and bar (kPopupGapDip)")),
        ("l", 14, Loc.T("Panel padding (kPad), glass shadow")), ("xl", 16, Loc.T("Gap between widgets")),
        ("2xl", 24, Loc.T("Widgets to the screen edge, between sections")),
    ];

    private bool _designBuilt;
    private StackPanel? _designDetails;

    private static Style StyleOf(string key) => (Style)Application.Current.Resources[key];

    private static TextBlock Text(string text, string style, double? size = null)
    {
        var t = new TextBlock { Text = text, Style = StyleOf(style) };
        if (size is double s) t.FontSize = s;
        return t;
    }

    private static TextBlock Tabular(TextBlock t)
    {
        Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(t, FontNumeralAlignment.Tabular);
        return t;
    }

    private static Border Card(UIElement child) => new() { Style = StyleOf("TileStyle"), Padding = new Thickness(20, 16, 20, 16), Child = child };

    private static StackPanel Section(string label, string? note, UIElement body)
    {
        var s = new StackPanel { Spacing = 12 };
        var title = Text(label, "TitleStyle");
        title.Margin = new Thickness(4, 0, 0, 0);
        s.Children.Add(title);
        if (note != null)
        {
            var n = Text(note, "RowNoteStyle");
            n.Margin = new Thickness(4, -4, 0, 2);
            n.MaxWidth = 760;
            n.HorizontalAlignment = HorizontalAlignment.Left;
            s.Children.Add(n);
        }
        s.Children.Add(body);
        return s;
    }

    private void BuildDesignPage()
    {
        if (_designBuilt) return;
        _designBuilt = true;
        // ---- First layer: to look at and to touch ----
        DesignStack.Children.Add(Principles());
        DesignStack.Children.Add(Section(Loc.T("Material"),
            Loc.T("One shader, two parameter sets: liquid glass refracts only at the rim and is almost clear in the middle; frosted glass blurs the backdrop into colour fields and hardly refracts. Drag the divider left and right to compare."),
            MaterialCompare()));
        DesignStack.Children.Add(Section(Loc.T("Geometry"), Loc.T("One G3 continuous corner system, keeping the large radii and capsule proportions."),
            GeometryCard()));
        DesignStack.Children.Add(Section(Loc.T("Typography"),
            Loc.T("Three MiSans weights. At most three sizes per screen; hierarchy first uses opacity, then size, then weight; all figures are tabular."),
            TypeCard(false)));
        DesignStack.Children.Add(Section(Loc.T("Spacing and colour"),
            Loc.T("One spacing scale; one accent colour, everything else neutral grey. Ink on glass follows the brightness behind it."),
            SpaceAndColour()));
        DesignStack.Children.Add(Section(Loc.T("Interaction"),
            Loc.T("Move the pointer over the taskbar: icons grow in a wave (the current magnification); click an icon to see the launch hop. Click a notification card to bring it back."),
            InteractionCard()));
        DesignStack.Children.Add(Section(Loc.T("Motion"),
            Loc.T("Animations can change direction from where they are and advance in real time; springs keep velocity, while the widgets' ease-out currently restarts. Click a card to play."),
            CurveGrid()));

        // ---- Second layer: the numbers ----
        var toggle = new Button { HorizontalAlignment = HorizontalAlignment.Left, Padding = new Thickness(16, 7, 14, 8) };
        var label = new TextBlock { Text = Loc.T("Show details") };
        var chevron = new FontIcon
        {
            Glyph = "\uE70D",
            FontSize = 11,
            Margin = new Thickness(10, 1, 0, 0),
            RenderTransformOrigin = new Point(0.5, 0.5),
            RenderTransform = new RotateTransform()
        };
        toggle.Content = new StackPanel { Orientation = Orientation.Horizontal, Children = { label, chevron } };
        AutomationProperties.SetName(toggle, Loc.T("Show details"));
        _designDetails = new StackPanel { Spacing = 36, Visibility = Visibility.Collapsed };
        toggle.Click += (_, _) =>
        {
            RefreshDesignDetails(true);
            bool open = _designDetails.Visibility != Visibility.Visible;
            _designDetails.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
            label.Text = open ? Loc.T("Hide details") : Loc.T("Show details");
            ((RotateTransform)chevron.RenderTransform).Angle = open ? 180 : 0;
            if (open) AnimateIn(_designDetails);
        };
        var foot = new StackPanel { Spacing = 8 };
        foot.Children.Add(Text(Loc.T("Technical details"), "TitleStyle"));
        var why = Text(
            Loc.T("Animation curves and durations, every material preset value, sizes and weights, corners and spacing: the same values Atrium runs with."),
            "RowNoteStyle");
        why.Margin = new Thickness(0, 0, 0, 6);
        foot.Children.Add(why);
        foot.Children.Add(toggle);
        foot.Margin = new Thickness(4, 0, 0, 0);
        DesignStack.Children.Add(foot);
        DesignStack.Children.Add(_designDetails);
    }

    private static StackPanel Principles()
    {
        var grid = new ResponsiveGrid { MinCellWidth = 240, Gap = 16, StretchLastRow = true };
        (string N, string Title, string Body)[] items =
        [
            ("01", Loc.T("Yield to content"),
                Loc.T("Glass carries content. Ink follows the brightness behind it; decoration stays within what keeps text legible.")),
            ("02", Loc.T("Motion behaves like real objects"),
                Loc.T("It can change direction mid-flight with continuous position; springs keep velocity, while the widgets' ease-out currently restarts.")),
            ("03", Loc.T("Same things, same treatment"), Loc.T("The same kind of element uses one curve, one radius and one type size.")),
        ];
        foreach (var (n, title, body) in items)
        {
            var s = new StackPanel { Spacing = 8 };
            var num = Text(n, "ValueStyle");
            num.HorizontalAlignment = HorizontalAlignment.Left;
            num.Foreground = Tokens.Brush("AccentBrush");
            s.Children.Add(num);
            s.Children.Add(Text(title, "RowTitleStyle"));
            s.Children.Add(Text(body, "RowNoteStyle"));
            grid.Children.Add(Card(s));
        }
        return Section(Loc.T("Principles"), null, grid);
    }

    // ---- Material: the two presets on one photo, split by a handle the pointer moves ----

    private Grid MaterialCompare()
    {
        const double H = 240;
        var frosted = new Border { CornerRadius = new CornerRadius(20), Background = PhotoBrush() };
        var liquid = new Border { CornerRadius = new CornerRadius(20), Background = PhotoBrush() };
        // Keep the split's XAML rectangle clip on a separate, unrounded host.
        // XAML otherwise replaces the same visual Clip used for the photo's
        // continuous boundary, and attempts to treat it as its own inset clip.
        var reveal = new Grid { Children = { liquid } };
        var split = new Rectangle { Width = 2, Fill = new SolidColorBrush(Color.FromArgb(230, 255, 255, 255)), HorizontalAlignment = HorizontalAlignment.Left };
        var knob = new Border
        {
            Width = 30,
            Height = 30,
            CornerRadius = new CornerRadius(15),
            Background = new SolidColorBrush(Color.FromArgb(240, 255, 255, 255)),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center,
            Child = new FontIcon { Glyph = "\uE76F", FontSize = 12, Foreground = new SolidColorBrush(Color.FromArgb(200, 0x1D, 0x1D, 0x1F)) },
            Shadow = new ThemeShadow(),
            Translation = new Vector3(0, 0, 8),
        };
        Border Tag(string text, HorizontalAlignment side) => new()
        {
            HorizontalAlignment = side,
            VerticalAlignment = VerticalAlignment.Top,
            Margin = new Thickness(14),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(8, 3, 8, 4),
            Background = new SolidColorBrush(Color.FromArgb(150, 0, 0, 0)),
            Child = new TextBlock { Text = text, FontSize = 12, Foreground = new SolidColorBrush(Colors.White) },
        };
        var stage = new Grid { Height = H, Background = new SolidColorBrush(Colors.Transparent) };
        stage.Children.Add(frosted);
        stage.Children.Add(reveal);
        stage.Children.Add(split);
        stage.Children.Add(knob);
        stage.Children.Add(Tag(Loc.T("Liquid glass"), HorizontalAlignment.Left));
        stage.Children.Add(Tag(Loc.T("Frosted glass"), HorizontalAlignment.Right));
        stage.Children.Add(new Border
        {
            CornerRadius = new CornerRadius(20),
            BorderThickness = new Thickness(1),
            BorderBrush = new SolidColorBrush(Color.FromArgb(31, 0, 0, 0)),
            IsHitTestVisible = false,
        });
        AutomationProperties.SetName(stage, Loc.T("Material comparison"));

        double at = 0.5, width = 0;
        void Place()
        {
            double x = Math.Round(width * at);
            reveal.Clip = new RectangleGeometry { Rect = new Rect(0, 0, Math.Max(0, x), H) };
            split.Translation = new Vector3((float)(x - 1), 0, 0);
            knob.Translation = new Vector3((float)(x - 15), 0, 8);
        }
        bool dragging = false;
        stage.PointerPressed += (_, e) =>
        {
            dragging = stage.CapturePointer(e.Pointer);
            at = Math.Clamp(e.GetCurrentPoint(stage).Position.X / Math.Max(1, width), 0.04, 0.96);
            Place();
        };
        stage.PointerMoved += (_, e) =>
        {
            if (!dragging) return;
            at = Math.Clamp(e.GetCurrentPoint(stage).Position.X / Math.Max(1, width), 0.04, 0.96);
            Place();
        };
        stage.PointerReleased += (_, e) => { dragging = false; stage.ReleasePointerCapture(e.Pointer); };
        stage.PointerCaptureLost += (_, _) => dragging = false;

        // Both renders at the stage's size, on the thread pool; redone when the width changes much,
        // and over a new picture when another is picked.
        double rendered = 0;
        int request = 0; // the newest render: an older one finishing later is dropped
        void Render()
        {
            rendered = width;
            double rs = Math.Clamp(stage.XamlRoot?.RasterizationScale ?? 1, 1, 2), w = width;
            int mine = ++request;
            int photoGeneration = Photo.Generation;
            var photo = Photo.Capture();
            Task.Run(() =>
            {
                var a = RenderMaterial(BarSettings.Liquid(), w, H, rs, photo.Value);
                var b = RenderMaterial(BarSettings.Frosted(), w, H, rs, photo.Value);
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (mine != request || photoGeneration != Photo.Generation) return;
                    liquid.Background = a.Brush();
                    frosted.Background = b.Brush();
                    Ready("design", photoGeneration);
                });
            });
        }
        stage.SizeChanged += (_, e) =>
        {
            width = e.NewSize.Width;
            Place();
            if (Math.Abs(width - rendered) >= 48) Render();
        };
        PhotoChanged += () => { if (width > 0) Render(); };
        _compareBuilt = true;
        return stage;
    }

    private sealed record Rendered(int W, int H, byte[] Pixels)
    {
        public ImageBrush Brush()
        {
            var bmp = new WriteableBitmap(W, H);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(Pixels, 0, Pixels.Length);
            bmp.Invalidate();
            return new ImageBrush { ImageSource = bmp, Stretch = Stretch.Fill };
        }
    }

    // A long capsule of the material across the photo, 84 DIP high (the bar at 1.4×).
    private static Rendered RenderMaterial(BarSettings material, double w, double h, double rs, Photo photo)
    {
        int pw = (int)Math.Round(w * rs), ph = (int)Math.Round(h * rs);
        var bg = Backdrop.Create(pw, ph, w, h, 0, 0, rs, photo);
        var s = material.Clone();
        s.Height = 60;
        s.CornerRadius = 30;
        double k = 1.4;
        var spec = GlassSpec.For(s, k * rs);
        int inset = (int)spec.Inset;
        int bw = (int)Math.Round((w - 80) * rs), bh = (int)Math.Round(60 * k * rs);
        int bx = (pw - bw) / 2, by = (ph - bh) / 2;
        byte[] pixels = new byte[bg.Base.Length];
        Buffer.BlockCopy(bg.Base, 0, pixels, 0, pixels.Length);
        GlassRenderer.Draw(bg, pixels, bx - inset, by - inset, bw + 2 * inset, bh + 2 * inset, spec);
        return new Rendered(pw, ph, pixels);
    }

    // ---- Geometry: the same continuous corner profile used by the application ----

    private static Border GeometryCard()
    {
        var outline = new Rectangle
        {
            Width = 176,
            Height = 176,
            RadiusX = 46,
            RadiusY = 46,
            Style = StyleOf("GlyphAccentStyle"),
            HorizontalAlignment = HorizontalAlignment.Center,
        };
        var face = new StackPanel { Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top };
        face.Children.Add(outline);
        var caption = Text(Loc.T("G3 large corners"), "RowTitleStyle");
        caption.HorizontalAlignment = HorizontalAlignment.Center;
        caption.MinHeight = caption.LineHeight = 22;
        face.Children.Add(caption);
        var grid = new ResponsiveGrid { MinCellWidth = 300, MaxColumns = 2, Gap = 28 };
        grid.Children.Add(face);
        grid.Children.Add(RadiusDemo());
        return Card(grid);
    }
    // The concentric rule in one picture: the bar's capsule radius, a 12 inset, the inner radius.
    private static StackPanel RadiusDemo()
    {
        const double R = 30, P = 12, Ww = 220, Hh = 176;
        var demo = new Canvas { Width = Ww, Height = Hh };
        demo.Children.Add(new Rectangle { Width = Ww, Height = Hh, RadiusX = R, RadiusY = R, Style = StyleOf("GlyphInkStyle") });
        var inner = new Rectangle { Width = Ww - 2 * P, Height = Hh - 2 * P, RadiusX = R - P, RadiusY = R - P, Style = StyleOf("GlyphAccentStyle") };
        Canvas.SetLeft(inner, P);
        Canvas.SetTop(inner, P);
        demo.Children.Add(inner);
        var label = Tabular(Text(Loc.F("R {0} − inset {1} = r {2}", R, P, R - P), "RowTitleStyle"));
        label.Width = Ww - 2 * P;
        label.TextAlignment = TextAlignment.Center;
        Canvas.SetLeft(label, P);
        Canvas.SetTop(label, Hh / 2 - 10);
        demo.Children.Add(label);
        var s = new StackPanel { Spacing = 12, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top };
        s.Children.Add(demo);
        var why = Text(Loc.T("Nesting: the inner radius shrinks with the inset, keeping the outline consistent."), "RowNoteStyle");
        why.HorizontalAlignment = HorizontalAlignment.Center;
        why.MinHeight = why.LineHeight = 22;
        s.Children.Add(why);
        return s;
    }

    // ---- Spacing and colour ----

    private static Border SpaceAndColour()
    {
        var space = new ResponsiveGrid { MinCellWidth = 280, MaxColumns = 1, Gap = 10, DistributeRows = true };
        foreach (var (name, value, _) in Spacing)
        {
            var row = new Grid { ColumnSpacing = 12 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(40) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.Children.Add(Tabular(Text(name, "RowNoteStyle")));
            var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            bar.Children.Add(new Rectangle
            {
                Width = value * 6,
                Height = 10,
                RadiusX = 3,
                RadiusY = 3,
                Fill = Tokens.Brush("AccentBrush"),
                Opacity = 0.35 + 0.65 * value / 24.0,
                VerticalAlignment = VerticalAlignment.Center,
            });
            bar.Children.Add(Tabular(Text(value.ToString(), "FootnoteStyle")));
            Grid.SetColumn(bar, 1);
            row.Children.Add(bar);
            space.Children.Add(row);
        }

        var colours = new ResponsiveGrid { MinCellWidth = 112, MaxColumns = 4, Gap = 12, DistributeRows = true };
        (string Key, string Name)[] tokens =
        [
            ("AccentBrush", Loc.T("Accent")), ("InkBrush", Loc.T("Primary text")),
            ("InkSecondaryBrush", Loc.T("Secondary text")), ("InkTertiaryBrush", Loc.T("Tertiary text")),
            ("FillBrush", Loc.T("Fill")), ("SeparatorBrush", Loc.T("Divider")), ("CardBrush", Loc.T("Card")), ("PageBrush", Loc.T("Page")),
        ];
        foreach (var (key, name) in tokens) colours.Children.Add(Chip(Tokens.Brush(key), name));
        // The few colours widgets use (WidgetFaces.cs, widget_faces.cpp), as on macOS.
        (Color C, string Name)[] accents =
        [
            (WidgetFaces.Green, Loc.T("Normal · Green")), (WidgetFaces.Orange, Loc.T("Reminder · Orange")),
            (WidgetFaces.Red, Loc.T("Warning · Red")), (WidgetFaces.Blue, Loc.T("Calendar · Blue")),
        ];
        foreach (var (c, name) in accents)
            colours.Children.Add(Chip(new SolidColorBrush(c), name));

        var grid = new ResponsiveGrid { MinCellWidth = 300, Gap = 28, StretchLastRow = true };
        grid.Children.Add(space);
        // Bring the outer colour rows in towards xs and xl, leaving room above and below.
        grid.Children.Add(new Border { Padding = new Thickness(0, 24, 0, 24), Child = colours });
        return Card(grid);
    }

    private static Grid Chip(Brush brush, string name)
    {
        var row = new Grid { ColumnSpacing = 10 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(28) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.Children.Add(new Border
        {
            Width = 28,
            Height = 28,
            VerticalAlignment = VerticalAlignment.Center,
            CornerRadius = new CornerRadius(8),
            Background = brush,
            BorderBrush = Tokens.Brush("CardStrokeBrush"),
            BorderThickness = new Thickness(1)
        });
        var labels = new StackPanel();
        labels.Children.Add(Text(name, "RowNoteStyle"));
        string hex = brush is SolidColorBrush b
            ? (b.Color.A == 255 ? $"#{b.Color.R:X2}{b.Color.G:X2}{b.Color.B:X2}" : $"#{b.Color.A:X2}{b.Color.R:X2}{b.Color.G:X2}{b.Color.B:X2}") : "";
        labels.Children.Add(Tabular(Text(hex, "FootnoteStyle")));
        Grid.SetColumn(labels, 1);
        row.Children.Add(labels);
        return row;
    }

    // ---- Interaction: a dock that magnifies under the pointer and hops once, a card that pops ----

    private ResponsiveGrid InteractionCard()
    {
        var grid = new ResponsiveGrid { MinCellWidth = 340, Gap = 16, StretchLastRow = true };
        grid.Children.Add(DockDemo());
        grid.Children.Add(PopDemo());
        return grid;
    }

    private static Border InteractionFrame(Grid stage, UIElement content)
    {
        var frame = Card(content);
        frame.Padding = new Thickness(16);
        // The border stroke is part of the inset too: the photo follows the same corner centre.
        double inset = frame.BorderThickness.Left + frame.Padding.Left;
        stage.CornerRadius = new CornerRadius(ContinuousCorners.RadiusOf(frame) - inset);
        return frame;
    }

    private Border DockDemo()
    {
        const double Icon = 34, Gap = 9, StageH = 190, BarH = 54;
        var stage = new Grid { Height = StageH, Background = PhotoBrush() };
        var lane = new Canvas { Height = StageH };
        stage.Children.Add(lane);
        var bar = new Border
        {
            Height = BarH,
            CornerRadius = new CornerRadius(BarH / 2),
            VerticalAlignment = VerticalAlignment.Bottom,
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 0, 0, 16),
            Background = new SolidColorBrush(Color.FromArgb(120, 255, 255, 255)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(170, 255, 255, 255)),
            BorderThickness = new Thickness(1),
        };
        stage.Children.Insert(0, bar);
        var apps = _previewPins.Append(_previewRunning).ToArray();
        int n = apps.Length;
        bar.Width = n * Icon + (n - 1) * Gap + 2 * 14;
        var holders = new List<(Border Holder, Image Icon)>();
        for (int i = 0; i < n; i++)
        {
            var icon = new Image
            {
                Width = Icon,
                Height = Icon,
                Stretch = Stretch.Uniform,
                DataContext = apps[i],
            };
            icon.SetBinding(Image.SourceProperty, new Microsoft.UI.Xaml.Data.Binding { Path = new PropertyPath(nameof(PinVm.Icon)) });
            var holder = new Border
            {
                Width = Icon,
                Height = Icon,
                Child = icon,
                CenterPoint = new Vector3((float)(Icon / 2), (float)Icon, 0)
            };
            Canvas.SetTop(holder, StageH - 16 - BarH / 2 - Icon / 2);
            lane.Children.Add(holder);
            holders.Add((holder, icon));
            var hop = new Curve("hop", "", "", "", 0.48, t => Motion.Hop(t, 0.48));
            void Show(double t) => icon.Translation = new Vector3(0, (float)(-Motion.Hop(t, 0.48) * 0.32 * Icon * 1.6), 0);
            holder.PointerPressed += (_, _) => Play(hop, Show);
            AutomationProperties.SetName(holder, apps[i].Title);
        }
        double width = 0, x0 = 0, row = n * Icon + (n - 1) * Gap;
        // As the bar does (BarPainter): the pointer drives the wave, its strength follows the pointer
        // in and out exponentially (20/s in, 11/s out), all on the frame clock with render transforms
        // only (no layout but the bar's width, no restarted transitions).
        double? pointer = null;
        double amount = 0, lastX = 0;
        long last = 0;
        bool ticking = false;
        void Apply()
        {
            double mag = Math.Max(1, CurrentSettings.Magnify), k = (mag - 1) * amount, range = (Icon + Gap) * 2.3;
            double centre = pointer ?? lastX;
            double Map(double u)
            {
                if (k <= 0) return u;
                double x = (u - centre) / range;
                return u + k * range * (x >= 1 ? 0.5 : x <= -1 ? -0.5 : x / 2 + Math.Sin(Math.PI * x) / (2 * Math.PI));
            }
            for (int i = 0; i < n; i++)
            {
                double left = x0 + i * (Icon + Gap), bx = left + Icon / 2;
                double grow = (Map(bx + Icon / 2) - Map(bx - Icon / 2)) / Icon;
                holders[i].Holder.Translation = new Vector3((float)(Map(bx) - bx), 0, 0);
                holders[i].Holder.Scale = new Vector3((float)grow, (float)grow, 1);
            }
            bar.Width = Map(x0 + row + 14) - Map(x0 - 14);
            bar.Translation = new Vector3((float)((Map(x0 - 14) + Map(x0 + row + 14)) / 2 - (x0 + row / 2)), 0, 0);
        }
        void Tick(object? sender, object e)
        {
            long now = Environment.TickCount64;
            double dt = Math.Clamp((now - last) / 1000.0, 0, 0.05);
            last = now;
            double target = pointer.HasValue ? 1 : 0, rate = pointer.HasValue ? 20 : 11;
            amount += (target - amount) * (1 - Math.Exp(-dt * rate));
            if (Math.Abs(target - amount) < 0.002)
            {
                amount = target;
                CompositionTarget.Rendering -= Tick;
                ticking = false;
            }
            Apply();
        }
        void Wake()
        {
            if (ticking) { Apply(); return; }
            ticking = true;
            last = Environment.TickCount64;
            CompositionTarget.Rendering += Tick;
        }
        stage.SizeChanged += (_, e) =>
        {
            width = e.NewSize.Width;
            x0 = (width - row) / 2;
            for (int i = 0; i < n; i++) Canvas.SetLeft(holders[i].Holder, x0 + i * (Icon + Gap));
            Apply();
        };
        stage.PointerMoved += (_, e) => { pointer = lastX = e.GetCurrentPoint(lane).Position.X; Wake(); };
        stage.PointerExited += (_, _) => { pointer = null; Wake(); };
        stage.Unloaded += (_, _) => { if (ticking) { CompositionTarget.Rendering -= Tick; ticking = false; } };

        var s = new StackPanel { Spacing = 10 };
        s.Children.Add(stage);
        s.Children.Add(Text(Loc.T("Hover magnification and launch hop"), "RowTitleStyle"));
        s.Children.Add(Text(
            Loc.T("Wave: (1 + cos πd) / 2, where d is the pointer's distance in icons (radius 2.3 icons); magnification comes from the Taskbar page."),
            "RowNoteStyle"));
        return InteractionFrame(stage, s);
    }

    private Border PopDemo()
    {
        var stage = new Grid { Height = 190, Background = PhotoBrush() };
        var card = new Border
        {
            Width = 300,
            Padding = new Thickness(14, 12, 16, 13),
            CornerRadius = new CornerRadius(22),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Background = new SolidColorBrush(Color.FromArgb(205, 250, 250, 252)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(150, 255, 255, 255)),
            BorderThickness = new Thickness(1),
        };
        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(new TextBlock
        {
            Text = Loc.T("Calendar · Now"),
            FontSize = 12,
            Foreground = new SolidColorBrush(Color.FromArgb(150, 0x1D, 0x1D, 0x1F)),
        });
        text.Children.Add(new TextBlock
        {
            Text = Loc.T("Product review"),
            FontSize = 14,
            FontFamily = (FontFamily)Application.Current.Resources["EmphasisFont"],
            Foreground = new SolidColorBrush(Color.FromArgb(240, 0x1D, 0x1D, 0x1F)),
        });
        text.Children.Add(new TextBlock
        {
            Text = Loc.T("Click me to show it again."),
            FontSize = 13,
            Foreground = new SolidColorBrush(Color.FromArgb(190, 0x1D, 0x1D, 0x1F)),
        });
        card.Child = text;
        var popup = new Grid { HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        popup.Children.Add(card);
        stage.Children.Add(popup);
        stage.PointerPressed += (_, _) => ReplayNotification(popup,
            new Vector2((float)(popup.ActualWidth - CurrentSettings.Height / 2 - 4), (float)popup.ActualHeight), card);
        AutomationProperties.SetName(stage, Loc.T("Notification popup example"));

        var s = new StackPanel { Spacing = 10 };
        s.Children.Add(stage);
        s.Children.Add(Text(Loc.T("Notification popup"), "RowTitleStyle"));
        s.Children.Add(Text(
            Loc.T("Spring kPop: from 0.92 to 1, overshooting only slightly (peak 1.0067), fading in over 160 ms. No back-and-forth bounce."),
            "RowNoteStyle"));
        return InteractionFrame(stage, s);
    }
}
