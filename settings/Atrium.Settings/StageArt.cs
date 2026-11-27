using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// Stage Manager page artwork: app icons on a 64 x 64 grid and miniature app windows drawn in code
// (also exported as PNGs by MainWindow.StageArt.cs).
public sealed partial class MainWindow
{
    // ---- Icons: on a 64 x 64 grid, after the apps' own ----

    private FrameworkElement StageIcon(int app, double size)
    {
        if (_stageArt.TryGetValue($"icon-{app}", out var shot))
            return new Image { Source = shot, Width = size, Height = size, Stretch = Stretch.Fill, IsHitTestVisible = false };
        return StageIconLive(app, size);
    }

    private static Viewbox StageIconLive(int app, double size)
    {
        var c = new Canvas { Width = 64, Height = 64 };
        void Put(UIElement e, double x, double y)
        {
            Canvas.SetLeft(e, x);
            Canvas.SetTop(e, y);
            c.Children.Add(e);
        }
        Border Plate(Brush fill) => new()
        {
            Width = 64,
            Height = 64,
            CornerRadius = new CornerRadius(14.5),
            Background = fill,
            BorderBrush = new SolidColorBrush(Color.FromArgb(30, 0, 0, 0)),
            BorderThickness = new Thickness(0.6),
        };
        switch (app)
        {
            case 0: // 备忘录: ruled paper under a yellow band
                {
                    Put(Plate(Vertical(Rgb(0xFFFFFF), Rgb(0xF1F1F4))), 0, 0);
                    Put(new Border
                    {
                        Width = 64,
                        Height = 21,
                        CornerRadius = new CornerRadius(14.5, 14.5, 0, 0),
                        Background = Vertical(Rgb(0xFFE14D), Rgb(0xFFC300)),
                    }, 0, 0);
                    Put(new Line
                    {
                        X1 = 7,
                        X2 = 57,
                        Y1 = 0,
                        Y2 = 0,
                        Stroke = new SolidColorBrush(Rgb(0xD9A600)),
                        StrokeThickness = 1.4,
                        StrokeDashArray = [1, 1.6]
                    }, 0, 21.5);
                    foreach (double y in new[] { 32.0, 41, 50 })
                        Put(new Rectangle { Width = 46, Height = 1.3, Fill = new SolidColorBrush(Rgb(0xD5D5DB)) }, 9, y);
                    Put(new Rectangle
                    {
                        Width = 30,
                        Height = 3,
                        RadiusX = 1.5,
                        RadiusY = 1.5,
                        Fill = new SolidColorBrush(Rgb(0x8E8E93)),
                        Opacity = 0.55,
                    }, 10, 27);
                    Put(new Rectangle
                    {
                        Width = 22,
                        Height = 3,
                        RadiusX = 1.5,
                        RadiusY = 1.5,
                        Fill = new SolidColorBrush(Rgb(0x8E8E93)),
                        Opacity = 0.55,
                    }, 10, 36);
                    break;
                }
            case 1: // 邮件: a white envelope on blue
                {
                    Put(Plate(Vertical(Rgb(0x5AC8FA), Rgb(0x1468F0))), 0, 0);
                    Put(new Border
                    {
                        Width = 42,
                        Height = 29,
                        CornerRadius = new CornerRadius(4.5),
                        Background = Vertical(Rgb(0xFFFFFF), Rgb(0xE9F1FF)),
                    }, 11, 18);
                    Put(new Polyline
                    {
                        Points = { new Point(12.5, 20), new Point(32, 35), new Point(51.5, 20) },
                        Stroke = new SolidColorBrush(Rgb(0x8DBBF5)),
                        StrokeThickness = 2.4,
                        StrokeLineJoin = PenLineJoin.Round,
                        StrokeStartLineCap = PenLineCap.Round,
                        StrokeEndLineCap = PenLineCap.Round,
                    }, 0, 0);
                    break;
                }
            case 2: // 日历: the weekday in red over the day of the month
                {
                    Put(Plate(Vertical(Rgb(0xFFFFFF), Rgb(0xF3F3F6))), 0, 0);
                    Put(new TextBlock
                    {
                        Text = Loc.T("Wed"),
                        Width = 64,
                        TextAlignment = TextAlignment.Center,
                        FontSize = 10.5,
                        FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                        Foreground = new SolidColorBrush(Rgb(0xFF3B30)),
                        IsTextScaleFactorEnabled = false,
                    }, 0, 6);
                    Put(new TextBlock
                    {
                        Text = "14",
                        Width = 64,
                        TextAlignment = TextAlignment.Center,
                        FontSize = 31,
                        FontWeight = Microsoft.UI.Text.FontWeights.Light,
                        Foreground = new SolidColorBrush(Rgb(0x1C1C1E)),
                        IsTextScaleFactorEnabled = false,
                    }, 0, 17);
                    break;
                }
            case 3: // 照片: a flower of eight translucent petals
                {
                    Put(Plate(new SolidColorBrush(Colors.White)), 0, 0);
                    uint[] petals = [0xFFCC00, 0xFF9500, 0xFF3B30, 0xFF2D55, 0xAF52DE, 0x007AFF, 0x5AC8FA, 0x34C759];
                    for (int i = 0; i < petals.Length; i++)
                    {
                        Put(new Ellipse
                        {
                            Width = 13,
                            Height = 24,
                            Fill = new SolidColorBrush(Rgb(petals[i], 0xC8)),
                            RenderTransform = new RotateTransform { Angle = i * 45, CenterX = 6.5, CenterY = 25 },
                        }, 25.5, 7);
                    }

                    break;
                }
            default: // 终端: a prompt on a dark screen
                {
                    Put(Plate(Vertical(Rgb(0x56565B), Rgb(0x2A2A2D))), 0, 0);
                    Put(new Border
                    {
                        Width = 52,
                        Height = 44,
                        CornerRadius = new CornerRadius(7),
                        Background = Vertical(Rgb(0x141416), Rgb(0x050506)),
                        BorderBrush = new SolidColorBrush(Color.FromArgb(70, 255, 255, 255)),
                        BorderThickness = new Thickness(0.8),
                    }, 6, 9);
                    Put(new Polyline
                    {
                        Points = { new Point(14, 21), new Point(21, 27.5), new Point(14, 34) },
                        Stroke = new SolidColorBrush(Rgb(0xF2F2F7)),
                        StrokeThickness = 3,
                        StrokeLineJoin = PenLineJoin.Round,
                        StrokeStartLineCap = PenLineCap.Round,
                        StrokeEndLineCap = PenLineCap.Round,
                    }, 0, 0);
                    Put(new Rectangle { Width = 12, Height = 3, RadiusX = 1.5, RadiusY = 1.5, Fill = new SolidColorBrush(Rgb(0xF2F2F7)) }, 24, 32.5);
                    Put(new Border { Width = 64, Height = 5, Background = new SolidColorBrush(Color.FromArgb(0, 0, 0, 0)) }, 0, 0);
                    break;
                }
        }
        return new Viewbox { Width = size, Height = size, Child = c, IsHitTestVisible = false };
    }

    private static Color Rgb(uint rgb, byte a = 255) => Color.FromArgb(a, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

    private static LinearGradientBrush Vertical(Color top, Color bottom)
    {
        var b = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
        b.GradientStops.Add(new GradientStop { Color = top, Offset = 0 });
        b.GradientStops.Add(new GradientStop { Color = bottom, Offset = 1 });
        return b;
    }

    // ---- Windows ----

    // An app's window, drawn at 320 x 200 and scaled: the title bar with the app's icon, then what
    // the app shows.
    private FrameworkElement StageWindow(StageApp app, double w, double h, double radius)
    {
        if (_stageArt.TryGetValue($"window-{Array.IndexOf(StageApps, app)}", out var shot))
            return new Image { Source = shot, Width = w, Height = h, Stretch = Stretch.Fill, IsHitTestVisible = false };
        return StageWindowLive(app, w, h, radius);
    }

    private static Viewbox StageWindowLive(StageApp app, double w, double h, double radius)
    {
        int index = Array.IndexOf(StageApps, app);
        var bg = app.Dark ? Rgb(0x1C1C1E) : Colors.White;
        var faint = app.Dark ? Color.FromArgb(110, 255, 255, 255) : Rgb(0x8E8E93);
        var body = new Grid { Width = 320, Height = 200 };
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(26) });
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var titleBar = new Grid { Padding = new Thickness(9, 0, 10, 0), Background = new SolidColorBrush(app.Dark ? Rgb(0x2A2A2D) : Rgb(0xF6F6F8)) };
        var name = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        name.Children.Add(StageIconLive(index, 14));
        name.Children.Add(new TextBlock
        {
            Text = Loc.T(app.Name),
            FontSize = 10,
            Foreground = new SolidColorBrush(faint),
            IsTextScaleFactorEnabled = false,
            VerticalAlignment = VerticalAlignment.Center,
        });
        titleBar.Children.Add(name);
        titleBar.Children.Add(new TextBlock
        {
            Text = "\uE921   \uE922   \uE8BB",
            FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
            FontSize = 8,
            Foreground = new SolidColorBrush(faint),
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center,
            IsTextScaleFactorEnabled = false,
        });
        body.Children.Add(titleBar);
        var main = StageContent(index);
        Grid.SetRow(main, 1);
        body.Children.Add(main);
        var frame = new Border
        {
            Width = 320,
            Height = 200,
            CornerRadius = new CornerRadius(radius * 320 / w),
            Background = new SolidColorBrush(bg),
            BorderBrush = new SolidColorBrush(Color.FromArgb(40, 0, 0, 0)),
            BorderThickness = new Thickness(1),
            Child = body,
        };
        return new Viewbox { Width = w, Height = h, Stretch = Stretch.Fill, Child = frame, IsHitTestVisible = false };
    }

    private static TextBlock StageText(string text, double size) => new()
    {
        Text = text,
        FontSize = size,
        FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        Foreground = new SolidColorBrush(Rgb(0x1C1C1E)),
        IsTextScaleFactorEnabled = false,
    };

    private static Border Bar(double w, double h, Color c, double r = 3) =>
        new()
        {
            Width = w,
            Height = h,
            CornerRadius = new CornerRadius(r),
            Background = new SolidColorBrush(c),
            HorizontalAlignment = HorizontalAlignment.Left,
        };

    // What each app shows under its title bar (174 DIP high).
    private static FrameworkElement StageContent(int app)
    {
        var line = Rgb(0xE3E4E8);
        switch (app)
        {
            case 0: // 备忘录: the notes, one picked, beside the open one
                {
                    var g = new Grid { ColumnSpacing = 0 };
                    g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(104) });
                    g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    var list = new StackPanel { Spacing = 6, Padding = new Thickness(8, 8, 8, 0), Background = new SolidColorBrush(Rgb(0xFAFAF7)) };
                    for (int i = 0; i < 4; i++)
                    {
                        var row = new StackPanel
                        {
                            Spacing = 4,
                            Padding = new Thickness(6, 5, 6, 5),
                            CornerRadius = new CornerRadius(5),
                            Background = new SolidColorBrush(i == 0 ? Rgb(0xFFE38A) : Colors.Transparent)
                        };
                        row.Children.Add(Bar(60 - 8 * (i % 2), 5, Rgb(0x8E8E93)));
                        row.Children.Add(Bar(44 + 6 * (i % 3), 4, Rgb(0xA0A0A5)));
                        list.Children.Add(row);
                    }
                    g.Children.Add(list);
                    var paper = new StackPanel { Spacing = 9, Padding = new Thickness(16, 12, 16, 0) };
                    paper.Children.Add(StageText(Loc.T("Weekend plans"), 13));
                    foreach (double lw in new[] { 0.95, 0.8, 0.9, 0.6, 0.85, 0.4 }) paper.Children.Add(Bar(176 * lw, 5, line));
                    Grid.SetColumn(paper, 1);
                    g.Children.Add(paper);
                    return g;
                }
            case 1: // 邮件: mailboxes, messages, the one open
                {
                    var g = new Grid();
                    g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(56) });
                    g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(112) });
                    g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    var boxes = new StackPanel { Spacing = 9, Padding = new Thickness(8, 10, 6, 0), Background = new SolidColorBrush(Rgb(0xEEF4FF)) };
                    foreach (double bw in new[] { 34.0, 28, 38, 24 }) boxes.Children.Add(Bar(bw, 5, Rgb(0x9DB9E8)));
                    g.Children.Add(boxes);
                    var mails = new StackPanel { Spacing = 0 };
                    for (int i = 0; i < 4; i++)
                    {
                        var row = new Grid
                        {
                            Padding = new Thickness(7, 6, 6, 6),
                            ColumnSpacing = 6,
                            Background = new SolidColorBrush(i == 0 ? Rgb(0x1E7BFF) : Colors.Transparent),
                        };
                        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
                        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                        uint[] avatars = [0xFF9500, 0x34C759, 0xAF52DE, 0x5AC8FA];
                        row.Children.Add(new Ellipse { Width = 16, Height = 16, Fill = new SolidColorBrush(Rgb(avatars[i])) });
                        var text = new StackPanel { Spacing = 3 };
                        text.Children.Add(Bar(52, 4, i == 0 ? Colors.White : Rgb(0x6E6E73)));
                        text.Children.Add(Bar(70, 3, i == 0 ? Color.FromArgb(170, 255, 255, 255) : Rgb(0xB0B0B5)));
                        Grid.SetColumn(text, 1);
                        row.Children.Add(text);
                        mails.Children.Add(row);
                    }
                    Grid.SetColumn(mails, 1);
                    g.Children.Add(mails);
                    var read = new StackPanel { Spacing = 8, Padding = new Thickness(12, 10, 12, 0) };
                    read.Children.Add(StageText(Loc.T("Quarterly review"), 11));
                    foreach (double lw in new[] { 0.9, 0.95, 0.7, 0.85, 0.5 }) read.Children.Add(Bar(128 * lw, 4, line));
                    Grid.SetColumn(read, 2);
                    g.Children.Add(read);
                    return g;
                }
            case 2: // 日历: the month, today in red, a few events
                {
                    var g = new Grid { Padding = new Thickness(12, 8, 12, 10), RowSpacing = 3, ColumnSpacing = 3 };
                    g.RowDefinitions.Add(new RowDefinition { Height = new GridLength(18) });
                    for (int r = 0; r < 5; r++) g.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
                    for (int col = 0; col < 7; col++) g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    var month = new TextBlock
                    {
                        Text = Loc.T("Nov"),
                        FontSize = 11,
                        FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                        Foreground = new SolidColorBrush(Rgb(0x1C1C1E)),
                        IsTextScaleFactorEnabled = false,
                    };
                    Grid.SetColumnSpan(month, 7);
                    g.Children.Add(month);
                    for (int d = 0; d < 35; d++)
                    {
                        int day = d - 5;
                        var cell = new Grid { CornerRadius = new CornerRadius(3), Background = new SolidColorBrush(Rgb(0xF5F5F7)) };
                        if (day is >= 1 and <= 30)
                        {
                            bool today = day == 14;
                            var num = new Border
                            {
                                Width = 11,
                                Height = 11,
                                CornerRadius = new CornerRadius(5.5),
                                HorizontalAlignment = HorizontalAlignment.Right,
                                VerticalAlignment = VerticalAlignment.Top,
                                Margin = new Thickness(0, 2, 2, 0),
                                Background = new SolidColorBrush(today ? Rgb(0xFF3B30) : Colors.Transparent),
                                Child = new TextBlock
                                {
                                    Text = day.ToString(),
                                    FontSize = 6,
                                    HorizontalAlignment = HorizontalAlignment.Center,
                                    VerticalAlignment = VerticalAlignment.Center,
                                    Foreground = new SolidColorBrush(today ? Colors.White : Rgb(0x6E6E73)),
                                    IsTextScaleFactorEnabled = false
                                }
                            };
                            cell.Children.Add(num);
                            uint? ev = day switch { 3 => 0x34C759, 11 => 0x1E7BFF, 14 => 0xFF9500, 19 => 0xAF52DE, 24 => 0x1E7BFF, _ => null };
                            if (ev is uint color)
                            {
                                cell.Children.Add(new Border
                                {
                                    Height = 4,
                                    CornerRadius = new CornerRadius(2),
                                    Margin = new Thickness(3, 0, 3, 3),
                                    VerticalAlignment = VerticalAlignment.Bottom,
                                    Background = new SolidColorBrush(Rgb(color))
                                });
                            }
                        }
                        Grid.SetRow(cell, 1 + d / 7);
                        Grid.SetColumn(cell, d % 7);
                        g.Children.Add(cell);
                    }
                    return g;
                }
            case 3: // 照片: a grid of pictures
                {
                    var g = new Grid { Padding = new Thickness(8), RowSpacing = 4, ColumnSpacing = 4 };
                    for (int r = 0; r < 3; r++) g.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
                    for (int col = 0; col < 5; col++) g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                    (uint Top, uint Bottom)[] shots =
                    [
                        (0x7FC8F8, 0xF6D365), (0x2E8B57, 0xA8E063), (0xFF7E5F, 0xFEB47B), (0x355C7D, 0xC06C84), (0x89F7FE, 0x66A6FF),
                        (0xF7971E, 0xFFD200), (0x56AB2F, 0xA8E063), (0x4B79A1, 0x283E51), (0xEE9CA7, 0xFFDDE1), (0x00B4DB, 0x0083B0),
                        (0xF953C6, 0xB91D73), (0x99F2C8, 0x1F4037), (0xFFB75E, 0xED8F03), (0xA1C4FD, 0xC2E9FB), (0x6A3093, 0xA044FF),
                    ];
                    for (int i = 0; i < shots.Length; i++)
                    {
                        var tile = new Border { CornerRadius = new CornerRadius(3), Background = Vertical(Rgb(shots[i].Top), Rgb(shots[i].Bottom)) };
                        Grid.SetRow(tile, i / 5);
                        Grid.SetColumn(tile, i % 5);
                        g.Children.Add(tile);
                    }
                    return g;
                }
            default: // 终端: a session
                {
                    var lines = new StackPanel { Spacing = 7, Padding = new Thickness(12, 10, 12, 0) };
                    var green = Rgb(0x32D74B, 220);
                    var white = Color.FromArgb(150, 255, 255, 255);
                    (double W, Color C, double Indent)[] rows =
                    [
                        (70, green, 0), (180, white, 10), (150, white, 10), (90, green, 0), (210, white, 10), (120, white, 10), (40, green, 0),
                    ];
                    foreach (var (lw, c, indent) in rows)
                    {
                        var b = Bar(lw, 5, c, 2);
                        b.Margin = new Thickness(indent, 0, 0, 0);
                        lines.Children.Add(b);
                    }
                    return lines;
                }
        }
    }
}
