using System.Numerics;
using Microsoft.Graphics.Canvas.Geometry;
using Microsoft.Graphics.Canvas.UI.Xaml;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Atrium.Settings;

// The quiet banner over the workspace: the project's card material rather than the platform's
// InfoBar colours, and it closes itself after ten seconds.
public sealed partial class MainWindow
{
    private const double StatusSeconds = 10;
    private Border? _statusCard;
    private TextBlock? _statusGlyph, _statusTitle, _statusMessage;
    private DispatcherQueueTimer? _statusTimer, _statusRingTimer;
    private CanvasControl? _statusRing;
    private DateTime _statusShownAt;

    private void ShowMessage(InfoBarSeverity severity, string title, string message)
    {
        var card = EnsureStatusCard();
        var (glyph, color) = severity switch
        {
            InfoBarSeverity.Success => ("\uE73E", "#34C759"),
            InfoBarSeverity.Warning => ("\uE7BA", "#FF9500"),
            InfoBarSeverity.Error => ("\uEA39", "#FF3B30"),
            _ => ("\uE946", "#0A84FF"),
        };
        _statusGlyph!.Text = glyph;
        _statusGlyph.Foreground = new SolidColorBrush(HexColor(color));
        _statusTitle!.Text = title;
        _statusMessage!.Text = message;
        card.Visibility = Visibility.Visible;
        // Restarting the timer is the reset: the newest message decides how long the card stays, and
        // the ring around the close button follows the same countdown.
        _statusShownAt = DateTime.UtcNow;
        _statusTimer!.Stop();
        _statusTimer.Start();
        _statusRingTimer ??= CreateRingTimer();
        _statusRingTimer.Start();
        _statusRing?.Invalidate();
    }

    private void HideStatus()
    {
        _statusTimer?.Stop();
        _statusRingTimer?.Stop();
        _statusCard?.Visibility = Visibility.Collapsed;
    }

    private Border EnsureStatusCard()
    {
        if (_statusCard != null)
            return _statusCard;
        var res = Application.Current.Resources;
        _statusTimer = DispatcherQueue.CreateTimer();
        _statusTimer.Interval = TimeSpan.FromSeconds(StatusSeconds);
        _statusTimer.IsRepeating = false;
        _statusTimer.Tick += (_, _) => HideStatus();
        _statusGlyph = new TextBlock
        {
            FontFamily = new FontFamily("Segoe Fluent Icons"),
            FontSize = 15,
            VerticalAlignment = VerticalAlignment.Center,
        };
        _statusTitle = new TextBlock { Style = (Style)res["RowTitleStyle"] };
        _statusMessage = new TextBlock { Style = (Style)res["RowNoteStyle"], TextWrapping = TextWrapping.Wrap };
        var text = new StackPanel { Spacing = 2, Children = { _statusTitle, _statusMessage } };
        _statusRing = new CanvasControl { Width = 26, Height = 26, ClearColor = Colors.Transparent };
        _statusRing.Draw += OnStatusRingDraw;
        var cross = new TextBlock
        {
            Text = "\uE711",
            FontFamily = new FontFamily("Segoe Fluent Icons"),
            FontSize = 12,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            IsHitTestVisible = false,
        };
        var close = new Grid
        {
            Width = 26,
            Height = 26,
            VerticalAlignment = VerticalAlignment.Center,
            Background = new SolidColorBrush(Colors.Transparent),
            Children = { _statusRing, cross },
        };
        close.PointerPressed += (_, _) => HideStatus();
        var grid = new Grid { ColumnSpacing = 10 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(_statusGlyph, 0);
        Grid.SetColumn(text, 1);
        Grid.SetColumn(close, 2);
        grid.Children.Add(_statusGlyph);
        grid.Children.Add(text);
        grid.Children.Add(close);
        Brush cardFill = ThemeBrush("CardBrush"), cardEdge = ThemeBrush("CardStrokeBrush");
        _statusCard = new Border
        {
            Child = grid,
            Visibility = Visibility.Collapsed,
            Padding = new Thickness(14, 10, 8, 10),
            CornerRadius = new CornerRadius(16),
            BorderThickness = new Thickness(1),
            Background = cardFill,
            BorderBrush = cardEdge,
        };
        ContinuousCorners.SetEnabled(_statusCard, true);
        ContinuousCorners.SetPaint(_statusCard, cardFill);
        ContinuousCorners.SetEdge(_statusCard, cardEdge);
        StatusHost.Children.Add(_statusCard);
        return _statusCard;
    }

    // The card's own colours follow the window's theme, as every tile does.
    private static Brush ThemeBrush(string key)
    {
        string theme = Application.Current.RequestedTheme == ApplicationTheme.Dark ? "Dark" : "Light";
        var dictionaries = Application.Current.Resources.ThemeDictionaries;
        return dictionaries[theme] is ResourceDictionary dictionary && dictionary.ContainsKey(key)
            ? (Brush)dictionary[key]
            : new SolidColorBrush(Colors.Transparent);
    }

    private DispatcherQueueTimer CreateRingTimer()
    {
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(33);
        timer.IsRepeating = true;
        timer.Tick += (_, _) => _statusRing?.Invalidate();
        return timer;
    }

    // The close button's ring: a faint track and a coloured arc that shrinks as the card ages.
    private void OnStatusRingDraw(CanvasControl sender, CanvasDrawEventArgs args)
    {
        var ds = args.DrawingSession;
        float size = (float)sender.ActualWidth;
        if (size <= 0) return;
        var center = new Vector2(size / 2, size / 2);
        float radius = size / 2 - 1.25f;
        const float Stroke = 1.5f;
        ds.DrawCircle(center, radius, RingTrack(), Stroke);
        float share = RemainingShare();
        if (share <= 0.002f) return;
        float start = -MathF.PI / 2;
        var from = center + new Vector2(MathF.Cos(start), MathF.Sin(start)) * radius;
        using var path = new CanvasPathBuilder(ds);
        path.BeginFigure(from);
        path.AddArc(center, radius, radius, start, share * MathF.Tau);
        path.EndFigure(CanvasFigureLoop.Open);
        using var arc = CanvasGeometry.CreatePath(path);
        ds.DrawGeometry(arc, RingAccent(), Stroke);
    }

    private float RemainingShare() =>
        (float)Math.Clamp(1 - (DateTime.UtcNow - _statusShownAt).TotalSeconds / StatusSeconds, 0, 1);

    private static Color RingTrack() => ThemeBrush("InkTertiaryBrush") is SolidColorBrush ink
        ? Color.FromArgb(64, ink.Color.R, ink.Color.G, ink.Color.B)
        : Color.FromArgb(64, 0, 0, 0);

    private Color RingAccent() => _statusGlyph?.Foreground is SolidColorBrush glyph ? glyph.Color : Colors.White;

    private static Color HexColor(string hex) => Color.FromArgb(
        255,
        Convert.ToByte(hex.Substring(1, 2), 16),
        Convert.ToByte(hex.Substring(3, 2), 16),
        Convert.ToByte(hex.Substring(5, 2), 16));
}
