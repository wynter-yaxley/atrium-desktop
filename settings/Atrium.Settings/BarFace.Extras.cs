using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

// The three extra entries as BarExtrasPainter draws them: Launchpad's page of apps, the now-playing
// entry's five bars (held mid-swing, as if a song played) and the folder stack's two folders.
internal sealed partial class BarFace
{
    // MusicBars: the silhouette (kShape) over the shortest bar (kRest), here at 60 % of its reach.
    private static readonly double[] BarShape = [0.58, 0.86, 1, 0.80, 0.52];

    // DrawStacks in a unit square: the front folder with its tab, the edge of the one behind.
    private const string FrontFolder =
        "F0 M0,0.41 L0,0.93 Q0,1 0.07,1 L0.79,1 Q0.86,1 0.86,0.93 L0.86,0.49 Q0.86,0.42 0.79,0.42 L0.43,0.42 L0.34,0.26 L0.07,0.26 Q0,0.26 0,0.33 Z";
    private const string BackFolder = "F0 M0.30,0.12 L0.93,0.12 Q1,0.12 1,0.19 L1,0.84";

    private readonly Canvas _launchpad = new() { IsHitTestVisible = false, Width = 1, Height = 1 }, _music = new() { IsHitTestVisible = false };
    private readonly Image _launchpadImage = new() { IsHitTestVisible = false, Stretch = Stretch.Uniform };
    private string _launchpadIcon = ""; // the file the last layout drew (Layout)
    private readonly Rectangle[] _bars = new Rectangle[5];
    private readonly Path _folder = Folder(FrontFolder), _folderBack = Folder(BackFolder);
    private readonly Path _apps = new() { IsHitTestVisible = false };
    private double _appsStroke = -1; // the stroke _apps' outline was laid out for
    private readonly SolidColorBrush _extrasInkBrush = new(), _folderBrush = new(), _folderBackBrush = new();
    private readonly ScaleTransform _launchpadScale = new(), _folderScale = new(), _folderBackScale = new();

    private void AddExtras(Canvas canvas)
    {
        _launchpad.RenderTransform = _launchpadScale;
        _folder.RenderTransform = _folderScale;
        _folderBack.RenderTransform = _folderBackScale;
        _apps.Stroke = _extrasInkBrush;
        _launchpad.Children.Add(_apps);
        for (int i = 0; i < _bars.Length; i++)
        {
            _bars[i] = new Rectangle { IsHitTestVisible = false, Fill = _extrasInkBrush };
            _music.Children.Add(_bars[i]);
        }
        _folder.Stroke = _folderBrush;
        _folderBack.Stroke = _folderBackBrush;
        foreach (var e in new UIElement[] { _launchpad, _launchpadImage, _music, _folderBack, _folder })
            canvas.Children.Add(e);
    }

    private void LayoutExtras(MainWindow.BarGeometry l, Func<double, (double X, double Y, double Size)> item, Color ink, string launchpadIcon)
    {
        _extrasInkBrush.Color = ink;

        _launchpad.Visibility = Visibility.Collapsed;
        _launchpadImage.Visibility = Visibility.Collapsed;
        if (l.LaunchX >= 0)
        {
            // The icon square: the user's picture fills it as an app icon does, else the built-in mark at 80 %.
            var (x, y, size) = item(l.LaunchX);
            double s = size * 0.80;
            var picture = size > 0 ? LaunchpadIcons.Preview(launchpadIcon) : null;
            if (picture != null)
            {
                _launchpadImage.Source = picture;
                _launchpadImage.Width = _launchpadImage.Height = size;
                Canvas.SetLeft(_launchpadImage, x);
                Canvas.SetTop(_launchpadImage, y);
                _launchpadImage.Visibility = Visibility.Visible;
            }
            else if (s > 0)
            {
                // A stroke of 6.5 % of the box.
                _launchpad.Visibility = Visibility.Visible;
                double thickness = Math.Max(1, s * 0.065) / s;
                if (thickness != _appsStroke)
                {
                    _apps.Data = (Geometry)XamlBindingHelper.ConvertValue(typeof(Geometry), LaunchpadMarkup(thickness));
                    _appsStroke = thickness;
                }
                _apps.StrokeThickness = thickness;
                _launchpadScale.ScaleX = _launchpadScale.ScaleY = s;
                Canvas.SetLeft(_launchpad, x + (size - s) / 2);
                Canvas.SetTop(_launchpad, y + (size - s) / 2);
            }
        }

        _music.Visibility = l.MusicX < 0 ? Visibility.Collapsed : Visibility.Visible;
        if (l.MusicX >= 0)
        {
            var (x, y, size) = item(l.MusicX);
            double side = size * 0.80, bar = side * 0.11, gap = side * 0.0925;
            double left = x + size / 2 - (5 * bar + 4 * gap) / 2, cy = y + size / 2;
            for (int i = 0; i < _bars.Length; i++)
            {
                double h = Math.Max(bar, side * (0.22 + 0.78 * 0.6 * BarShape[i]));
                var b = _bars[i];
                b.Width = bar;
                b.Height = h;
                b.RadiusX = b.RadiusY = bar / 2;
                Canvas.SetLeft(b, left + i * (bar + gap));
                Canvas.SetTop(b, cy - h / 2);
            }
        }

        _folder.Visibility = _folderBack.Visibility = Visibility.Collapsed;
        if (l.StacksX >= 0)
        {
            // 74 % of the icon square, a stroke of 7.5 % of it; the folder behind at 55 % ink.
            var (x, y, size) = item(l.StacksX);
            double s = size * 0.74;
            if (s > 0)
            {
                _folder.Visibility = _folderBack.Visibility = Visibility.Visible;
                _folderBrush.Color = ink;
                _folderBackBrush.Color = Color.FromArgb((byte)(ink.A * 0.55), ink.R, ink.G, ink.B);
                double thickness = Math.Max(1, s * 0.075) / s;
                foreach (var (p, scale) in new[] { (_folder, _folderScale), (_folderBack, _folderBackScale) })
                {
                    p.StrokeThickness = thickness;
                    scale.ScaleX = scale.ScaleY = s;
                    Canvas.SetLeft(p, x + (size - s) / 2);
                    Canvas.SetTop(p, y + (size - s) / 2);
                }
            }
        }
    }

    // DrawLaunchpad in a unit square: four squares in outline, the whole's four corners sharp, the others round.
    private static string LaunchpadMarkup(double stroke)
    {
        const double Gap = 0.12, Pane = (1 - Gap) / 2;
        double round = Pane * 0.32 - stroke / 2;
        var markup = new System.Text.StringBuilder("F0");
        string P(double v) => v.ToString("0.####", System.Globalization.CultureInfo.InvariantCulture);
        for (int i = 0; i < 4; i++)
        {
            int col = i % 2, row = i / 2, sharp = row == 0 ? col : 3 - col;
            double x0 = col * (Pane + Gap) + stroke / 2, y0 = row * (Pane + Gap) + stroke / 2, x1 = x0 + Pane - stroke, y1 = y0 + Pane - stroke;
            double[] r = [round, round, round, round];
            r[sharp] = 0;
            string Arc(double radius, double x, double y) => radius > 0 ? $" A{P(radius)},{P(radius)} 0 0 1 {P(x)},{P(y)}" : "";
            markup.Append($" M{P(x0 + r[0])},{P(y0)} L{P(x1 - r[1])},{P(y0)}").Append(Arc(r[1], x1, y0 + r[1]))
                  .Append($" L{P(x1)},{P(y1 - r[2])}").Append(Arc(r[2], x1 - r[2], y1))
                  .Append($" L{P(x0 + r[3])},{P(y1)}").Append(Arc(r[3], x0, y1 - r[3]))
                  .Append($" L{P(x0)},{P(y0 + r[0])}").Append(Arc(r[0], x0 + r[0], y0)).Append(" Z");
        }
        return markup.ToString();
    }

    private static Path Folder(string markup) => new()
    {
        IsHitTestVisible = false,
        Data = (Geometry)XamlBindingHelper.ConvertValue(typeof(Geometry), markup),
        StrokeStartLineCap = PenLineCap.Round,
        StrokeEndLineCap = PenLineCap.Round,
        StrokeLineJoin = PenLineJoin.Round,
    };
}
