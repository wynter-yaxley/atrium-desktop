using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The hero specimen: the glass bar rendered on the CPU (GlassPreview.cs) over the specimen photo
// (Photo.cs), with real shell icons and taskbar entries laid out by the same rules as the real bar, a loupe on
// the right end of the bar, and dimension marks for the parameter that is being edited. The photo
// is uploaded once per size; a frame repaints only the glass, as a transparent layer over it.
public sealed partial class MainWindow
{
    // The loupe: the bar's top right corner at 5x, the whole corner and the bell beside it.
    private const double LoupeZoom = 5;
    // The bar's width the preview is scaled for: a default bar (seven apps and the clock) at full size
    // once the preview is this wide (DIP), smaller below it.
    private const double HeroReferenceW = 860;

    // The bar in hero DIP (real DIP * S). Lefts are base positions; Map() is the magnification wave
    // (BarPainter::MapX: the local scale 1 + K * (1 + cos(pi x)) / 2 over Range, integrated from the
    // pointer at Centre), BarX/BarW the stretched glass the wave makes.
    internal sealed record BarGeometry(
        double S, double BaseX, double BarX, double BarY, double BarW, double BarH, double Radius, double MaxW,
        int N, double Icon, double P, double PlateR, double AppsLeft, double Pitch, double PlateY,
        bool Clock, double ClockX, double ClockW, double TimePx, double DatePx, double SepX,
        double StartX, double TrayX, double ImeX, double StatusX, double BellX,
        double K, double Centre, double Range,
        double LoupeX0, double LoupeY0, double LoupeW, double LoupeH, double StatusExtra = 0, double DeskX = -1,
        double LaunchX = -1, double MusicX = -1, double StacksX = -1)
    {
        public double PlateX(int i) => BaseX + (AppsLeft + i * Pitch) * S;

        public double Map(double u)
        {
            if (K <= 0) return u;
            double x = (u - Centre) / Range;
            double area = x >= 1 ? 0.5 : x <= -1 ? -0.5 : x / 2 + Math.Sin(Math.PI * x) / (2 * Math.PI);
            return u + K * Range * area;
        }
    }

    private sealed record RenderJob(BarSettings S, BarGeometry L, double W, double H, double Rs, bool Loupe, Lazy<Photo> Photo, int PhotoGeneration);

    private const int MaxPreviewApps = 8;
    private PinVm _previewRunning = null!;
    private PinVm[] _previewPins = [];

    private double _heroW, _heroH;
    private readonly ImageBrush _heroBrush = new() { Stretch = Stretch.Fill };
    private readonly ImageBrush _loupeBrush = new() { Stretch = Stretch.Fill };
    private readonly Image _glass = new() { Stretch = Stretch.Fill, IsHitTestVisible = false };
    private WriteableBitmap? _photoBmp, _glassBmp, _loupeBmp;
    private Backdrop? _photoPending; // a new backdrop for the hero's background (Interlocked)
    private volatile FrameBuffers? _heroFrames, _loupeFrames;
    private LatestWorker<RenderJob>? _renderWorker;
    private LatestWorker<BarSettings>? _swatchWorker;
    private sealed record SwatchPhoto(int Generation, Lazy<Photo> Photo);
    private LatestWorker<SwatchPhoto>? _fixedSwatchWorker;
    private int _uiPending;
    private float _luma = 0.3f;
    private bool _darkInk; // the bar's ink over the preview, with the core's hysteresis
    private string? _engaged;
    private bool _heroSized;

    // Render-thread caches.
    private Backdrop? _mainBg, _loupeBg;
    private string _mainKey = "", _loupeKey = "";

    private BarFace _face = null!, _loupeFace = null!;
    private readonly List<UIElement> _annotations = [];

    private void InitHero()
    {
        HeroFrame.Background = _heroBrush;
        Loupe.Background = _loupeBrush;
        _renderWorker = new LatestWorker<RenderJob>(RenderFrame);
        _swatchWorker = new LatestWorker<BarSettings>(job => RenderSwatch(job, SwatchCustom));
        _previewRunning = new PinVm(this, new PinnedApp("Atrium", Environment.ProcessPath!, "", ""))
        {
            Icon = new BitmapImage(new Uri(System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "atrium-256.png"))) { DecodePixelWidth = 96 },
        };

        HeroCanvas.Children.Add(_glass);
        _face = new BarFace(HeroCanvas, MaxPreviewApps);
        _loupeFace = new BarFace(LoupeCanvas, MaxPreviewApps);
        for (int i = 0; i < MaxPreviewApps; i++)
        {
            foreach (var icon in new[] { _face.Icons[i], _loupeFace.Icons[i] })
                icon.SetBinding(Image.SourceProperty, new Microsoft.UI.Xaml.Data.Binding { Path = new PropertyPath(nameof(PinVm.Icon)) });
        }

        // The two presets are fixed swatches; the custom one follows the current material.
        var liquid = BarSettings.Liquid();
        var frosted = BarSettings.Frosted();
        _fixedSwatchWorker = new LatestWorker<SwatchPhoto>(photo =>
        {
            RenderSwatch(liquid, SwatchLiquid, photo);
            RenderSwatch(frosted, SwatchFrosted, photo);
        });
        _fixedSwatchWorker.Post(new SwatchPhoto(Photo.Generation, Photo.Capture()));
        // A new picture: the swatches again, over it.
        PhotoChanged += () =>
        {
            _fixedSwatchWorker.Post(new SwatchPhoto(Photo.Generation, Photo.Capture()));
            _swatchSettings = CurrentSettings.Clone();
            _swatchWorker?.Post(_swatchSettings);
        };
    }

    private double RasterScale => Math.Clamp(HeroFrame.XamlRoot?.RasterizationScale ?? 1.0, 1.0, 2.0);

    private void OnHeroSizeChanged(object sender, SizeChangedEventArgs e)
    {
        _heroW = e.NewSize.Width;
        _heroH = e.NewSize.Height;
        HeroCanvas.Clip = new RectangleGeometry { Rect = new Rect(0, 0, _heroW, _heroH) };
        if (!_heroSized && HeroFrame.XamlRoot != null)
        {
            _heroSized = true;
            HeroFrame.XamlRoot.Changed += (_, _) => UpdateHero();
        }
        UpdateHero();
    }

    // A card was focused or let go: remember which one so the hero can point at it.
    private void OnEngage(string title, bool on)
    {
        _engaged = on ? title : (_engaged == title ? null : _engaged);
        bool edge = _engaged == Loc.T("Refraction") || _engaged == Loc.T("Refraction edge") || _engaged == Loc.T("Dispersion") ||
                    _engaged == Loc.T("Highlight") || _engaged == Loc.T("Corner radius") || _engaged == Loc.T("Shadow");
        Loupe.BorderBrush = edge ? Tokens.Brush("AccentBrush") : new SolidColorBrush(Color.FromArgb(179, 255, 255, 255));
        UpdateHero();
    }

    // ---- Layout: the same rules as the real bar, scaled to the specimen ----

    private BarGeometry? ComputeLayout(BarSettings s)
    {
        if (_heroW < 80 || _heroH < 80) return null;
        return BarLayout(s, _heroW, _heroH, Math.Min(1.0, (_heroW - 48) / (HeroReferenceW + 2 * GlassSpec.InsetDip)), true);
    }

    // The bar at scale `scale` at the bottom of a view of viewW x viewH; `wave`: as if the pointer
    // rested on the second app (else no magnification).
    private BarGeometry BarLayout(BarSettings s, double viewW, double viewH, double scale, bool wave)
    {
        double h = s.Height, i = Math.Clamp(s.IconSize, 12, h - 16);
        double p = Math.Max(i + 4, Math.Min(h - 12, Math.Round(i * 1.55)));
        double v = Math.Floor((h - p) / 2), pad = v + 4, gap = Math.Max(2, Math.Round(s.IconSpacing));
        double timePx = h * 0.25, datePx = h * 0.19, sepGap = 8;
        double clockW = s.ShowClock ? Math.Max(timePx * (s.ShowClockSeconds ? 4.8 : 2.75), datePx * 5.7) + 22 : 0;
        double deskW = s.ShowDesktop ? Math.Max(10, Math.Round(i * 0.45)) : 0; // the Show Desktop slot
        int rightCount = (s.ShowTray ? 1 : 0) + (s.ImeStatus ? 1 : 0) + (s.ShowStatus ? 1 : 0) +
            (s.ShowNotifications ? 1 : 0) + (s.ShowClock ? 1 : 0) + (s.ShowDesktop ? 1 : 0) + (s.ShowNowPlaying ? 1 : 0) + (s.ShowFolderStacks ? 1 : 0);
        double statusExtra = s.StatusStyle == "icons" ? Math.Round(i * 1.3) : 0; // three icons: about an icon wider
        double rightW = (rightCount - (s.ShowClock ? 1 : 0) - (s.ShowDesktop ? 1 : 0)) * p +
            (s.ShowStatus ? statusExtra : 0) + clockW + deskW + Math.Max(0, rightCount - 1) * gap;
        double launch = s.ShowLaunchpad ? p + gap : 0; // Launchpad, one slot right of the logo
        double Content(int n) => Math.Max(2 * h,
            2 * pad + p + 2 * gap + launch + n * p + Math.Max(0, n - 1) * gap + (rightCount > 0 ? 2 * sepGap + rightW : 0));
        double mag = Math.Clamp(s.Magnify, 1, 2), pitch = p + gap;
        double stretch = mag > 1.001 ? Math.Ceiling((mag - 1) * pitch * 2.3) : 0;
        double maxW = Math.Max(2 * h, Math.Min(s.Width, (viewW - 16) / scale - stretch));
        int n = _previewPins.Length + (s.ShowRunning ? 1 : 0);
        while (n > 0 && Content(n) > maxW) n--;
        double barW = s.AutoWidth ? Math.Min(Content(n), maxW) : maxW;

        double cursor = barW - pad;
        double Slot(double width, bool on)
        {
            if (!on) return -1;
            cursor -= width;
            double x = cursor;
            cursor -= gap;
            return x;
        }
        double deskLeft = Slot(deskW, s.ShowDesktop);
        double bellLeft = Slot(p, s.ShowNotifications), clockLeft = Slot(clockW, s.ShowClock), musicLeft = Slot(p, s.ShowNowPlaying);
        double statusLeft = Slot(p + statusExtra, s.ShowStatus), imeLeft = Slot(p, s.ImeStatus), trayLeft = Slot(p, s.ShowTray);
        double stacksLeft = Slot(p, s.ShowFolderStacks);
        double rightLeft = rightCount > 0 ? cursor + gap : barW - pad;
        double appsMin = pad + p + 2 * gap + launch;
        double appsLeft = appsMin + Math.Max(0, (rightLeft - (rightCount > 0 ? 2 * sepGap : 0) - appsMin - (n * p + Math.Max(0, n - 1) * gap)) / 2);
        double reff = Math.Min(s.CornerRadius, Math.Min(h, barW) / 2);

        double bx = (viewW - barW * scale) / 2;
        double by = Math.Max(10, viewH - s.Margin * scale - h * scale);
        double X(double left) => left < 0 ? -1 : bx + left * scale;

        // As if the pointer rested on the bar, just above the second app: the icons take the wave
        // with 栏上方提前放大图标 (magnifyNear); the glass rises on its own switch below.
        double k = wave && mag > 1.001 && s.MagnifyNear ? mag - 1 : 0, range = pitch * 2.3 * scale;
        double centre = n > 1 ? bx + (appsLeft + pitch) * scale + p * scale / 2 : bx + (pad + p / 2) * scale;
        double Map(double u)
        {
            if (k <= 0) return u;
            double x = (u - centre) / range;
            return u + k * range * (x >= 1 ? 0.5 : x <= -1 ? -0.5 : x / 2 + Math.Sin(Math.PI * x) / (2 * Math.PI));
        }
        double left = Map(bx), right = Map(bx + barW * scale);

        // The loupe's view: the whole top right corner and the bell, 10 DIP of air around.
        double lw = Math.Max(64, reff + 34), lh = Math.Max(56, reff + 26);
        return new BarGeometry(scale, bx, left, by, right - left, h * scale, reff * scale, maxW * scale,
            n, i * scale, p * scale, Math.Max(4, Math.Min(p / 2, reff - v)) * scale, appsLeft, pitch, by + v * scale,
            clockW > 0, X(clockLeft), clockW * scale, timePx * scale, datePx * scale, rightCount > 0 ? bx + (rightLeft - sepGap) * scale : -1,
            bx + pad * scale, X(trayLeft), X(imeLeft), X(statusLeft), X(bellLeft),
            k, centre, range,
            right - (lw - 10) * scale, by - 10 * scale, lw * scale, lh * scale, statusExtra * scale,
            deskLeft < 0 ? -1 : bx + (deskLeft + deskW / 2) * scale,
            s.ShowLaunchpad ? X(pad + p + gap) : -1, X(musicLeft), X(stacksLeft));
    }

    private void UpdateHero()
    {
        UpdateReadout();
        if (HeroBlock.Visibility != Visibility.Visible || _renderWorker == null) return;
        var bar = ComputeLayout(CurrentSettings);
        if (bar == null) return;
        bool loupe = Loupe.Visibility == Visibility.Visible;
        if (loupe)
        {
            Loupe.Width = Math.Round(bar.LoupeW * LoupeZoom);
            Loupe.Height = Math.Round(bar.LoupeH * LoupeZoom);
            LoupeCanvas.Clip = new RectangleGeometry { Rect = new Rect(0, 0, Loupe.Width, Loupe.Height) };
        }
        PositionTaskbar(bar, loupe);
        DrawAnnotations(bar);
        _renderWorker.Post(new RenderJob(CurrentSettings.Clone(), bar, _heroW, _heroH, RasterScale, loupe, Photo.Capture(), Photo.Generation));
    }

    private bool DarkInk() => CurrentSettings.TextColor == "dark" || (CurrentSettings.TextColor == "auto" && _darkInk);

    private void PositionTaskbar(BarGeometry bar, bool loupe)
    {
        int running = CurrentSettings.ShowRunning && bar.N > _previewPins.Length ? _previewPins.Length : -1;
        for (int i = 0; i < MaxPreviewApps; i++)
        {
            var vm = i < _previewPins.Length ? _previewPins[i] : _previewRunning;
            if (_face.Icons[i].DataContext != vm) _face.Icons[i].DataContext = vm;
            if (_loupeFace.Icons[i].DataContext != vm) _loupeFace.Icons[i].DataContext = vm;
        }
        var clock = PreviewClock.Now;
        string time = clock.ToString(CurrentSettings.ShowClockSeconds ? "H:mm:ss" : "H:mm"), date = clock.ToString("yyyy/M/d");
        _face.SetClock(time, date);
        var status = StatusLook.Current();
        _face.Layout(bar, DarkInk(), bar.N, running, CurrentSettings.Magnify, 1, 0, 0, status, CurrentSettings.LaunchpadIcon);
        if (loupe)
        {
            _loupeFace.SetClock(time, date);
            _loupeFace.Layout(bar, DarkInk(), bar.N, running, CurrentSettings.Magnify, LoupeZoom, bar.LoupeX0, bar.LoupeY0, status,
                CurrentSettings.LaunchpadIcon);
        }
    }

    // ---- Dimension marks for the card in use ----

    private void DrawAnnotations(BarGeometry bar)
    {
        foreach (var a in _annotations) HeroCanvas.Children.Remove(a);
        _annotations.Clear();
        if (_engaged == null) return;
        var white = new SolidColorBrush(Color.FromArgb(235, 255, 255, 255));
        void Line(double x1, double y1, double x2, double y2, bool dashed = false)
        {
            var l = new Line
            {
                X1 = x1,
                Y1 = y1,
                X2 = x2,
                Y2 = y2,
                Stroke = white,
                StrokeThickness = 1.5,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round,
                IsHitTestVisible = false,
            };
            if (dashed) l.StrokeDashArray = [3, 3];
            _annotations.Add(l);
            HeroCanvas.Children.Add(l);
        }
        void Chip(string text, double cx, double top)
        {
            var chip = new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(235, 255, 255, 255)),
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(7, 2, 7, 3),
                Child = new TextBlock
                {
                    Text = text,
                    FontSize = 11,
                    FontFamily = (FontFamily)Application.Current.Resources["EmphasisFont"],
                    Foreground = new SolidColorBrush(Color.FromArgb(255, 22, 24, 29)),
                },
                IsHitTestVisible = false,
            };
            Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment((TextBlock)chip.Child, FontNumeralAlignment.Tabular);
            chip.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            Canvas.SetLeft(chip, Math.Clamp(cx - chip.DesiredSize.Width / 2, 4, Math.Max(4, _heroW - chip.DesiredSize.Width - 4)));
            Canvas.SetTop(chip, Math.Clamp(top, 4, Math.Max(4, _heroH - chip.DesiredSize.Height - 4)));
            _annotations.Add(chip);
            HeroCanvas.Children.Add(chip);
        }

        double barBottom = bar.BarY + bar.BarH, barRight = bar.BarX + bar.BarW;
        if (_engaged == Loc.T("Height"))
        {
            double x = bar.BarX - 14;
            Line(x, bar.BarY, x, barBottom); Line(x - 4, bar.BarY, x + 4, bar.BarY); Line(x - 4, barBottom, x + 4, barBottom);
            Chip($"{Math.Round(CurrentSettings.Height)} px", x - 30, bar.BarY + bar.BarH / 2 - 10);
        }
        else if (_engaged == Loc.T("Edge spacing"))
        {
            double x = bar.BarX + bar.BarW * 0.5;
            Line(x, barBottom, x, _heroH); Line(x - 4, barBottom, x + 4, barBottom); Line(x - 4, _heroH - 1, x + 4, _heroH - 1);
            Chip($"{Math.Round(CurrentSettings.Margin)} px", x + 34, (barBottom + _heroH) / 2 - 10);
        }
        else if (_engaged == Loc.T("Maximum width"))
        {
            double y = bar.BarY - 12, x0 = (_heroW - bar.MaxW) / 2, x1 = x0 + bar.MaxW;
            Line(x0, y, x1, y, dashed: true); Line(x0, y - 4, x0, y + 4); Line(x1, y - 4, x1, y + 4);
            Chip($"{Math.Round(CurrentSettings.Width)} px", _heroW / 2, y - 24);
        }
        else if (_engaged == Loc.T("Enter distance") || _engaged == Loc.T("Leave distance"))
        {
            // The band above the bar that answers a pointer coming down onto it (栏上方提前拱起 /
            // 提前放大图标): a dashed line at the enter distance, and the leave one while it is set.
            double x0 = bar.BarX + 8, x1 = Math.Max(x0 + 1, barRight - 8);
            double enterY = bar.BarY - CurrentSettings.NearEnter * bar.S, leaveY = bar.BarY - CurrentSettings.NearLeave * bar.S;
            Line(x0, enterY, x1, enterY, dashed: true);
            Chip(Loc.F("Enter {0} px", Math.Round(CurrentSettings.NearEnter)), x0 + 44, enterY - 22);
            if (_engaged == Loc.T("Leave distance"))
            {
                Line(x0, leaveY, x1, leaveY, dashed: true);
                Chip(Loc.F("Leave {0} px", Math.Round(CurrentSettings.NearLeave)), x0 + 44, leaveY - 22);
            }
        }
        else if (_engaged == Loc.T("Corner radius"))
        {
            // The circle the corner is an arc of, and its radius.
            double r = Math.Max(bar.Radius, 2);
            var circle = new Ellipse
            {
                Width = 2 * r,
                Height = 2 * r,
                Stroke = white,
                StrokeThickness = 1.25,
                StrokeDashArray = [3, 3],
                IsHitTestVisible = false,
            };
            Canvas.SetLeft(circle, bar.BarX);
            Canvas.SetTop(circle, bar.BarY);
            _annotations.Add(circle);
            HeroCanvas.Children.Add(circle);
            Line(bar.BarX + r, bar.BarY + r, bar.BarX + r - r * 0.7071, bar.BarY + r - r * 0.7071);
            Chip($"R {Math.Round(Math.Min(CurrentSettings.CornerRadius, CurrentSettings.Height / 2))} px", bar.BarX + r, bar.BarY - 30);
        }
        else if (_engaged == Loc.T("Icon size"))
        {
            if (bar.N == 0) return;
            double x = bar.PlateX(0) + bar.P / 2 - bar.Icon / 2 - 4, y0 = bar.PlateY + bar.P / 2 - bar.Icon / 2, y1 = y0 + bar.Icon;
            Line(x, y0, x, y1); Line(x - 3, y0, x + 3, y0); Line(x - 3, y1, x + 3, y1);
            Chip($"{Math.Round(CurrentSettings.IconSize)} px", bar.PlateX(0) + bar.P / 2, bar.BarY - 28);
        }
        else if (_engaged == Loc.T("Icon spacing"))
        {
            // The air between the first two plates: exactly what the setting changes (the plates
            // stay the same size, and the whole bar follows their new pitch).
            if (bar.N < 2) return;
            double x0 = bar.PlateX(0) + bar.P, x1 = bar.PlateX(1), y = bar.BarY - 12;
            Line(x0, y, x1, y); Line(x0, y - 4, x0, y + 4); Line(x1, y - 4, x1, y + 4);
            Chip($"{Math.Round(CurrentSettings.IconSpacing)} px", (x0 + x1) / 2, y - 24);
        }
    }

    // ---- Rendering (thread pool) ----

    private void RenderFrame(RenderJob j)
    {
        int pw = (int)Math.Round(j.W * j.Rs), ph = (int)Math.Round(j.H * j.Rs);
        if (pw < 8 || ph < 8) return;
        var bar = j.L;

        // Main view: the photo once per size, then the glass alone, over it.
        string key = $"{pw}x{ph}/{j.W:F1}x{j.H:F1}/{j.PhotoGeneration}";
        if (_mainBg == null || key != _mainKey)
        {
            _mainBg = Backdrop.Create(pw, ph, j.W, j.H, 0, 0, j.Rs, j.Photo.Value);
            _mainKey = key;
            _heroPhotoGen = j.PhotoGeneration;
            Interlocked.Exchange(ref _photoPending, _mainBg);
        }
        // Keep the specimen and loupe in step with the two early-answer switches: the glass
        // rises with 跟随鼠标拱起 + 栏上方提前拱起 (hoverRise + riseNear), whether or not the
        // icons take the wave with it.
        double waveRise = j.S.HoverRise && j.S.RiseNear ? Math.Clamp(j.S.Magnify, 1, 2) - 1 : 0;
        GlassSpec Spec(double ppd) => GlassSpec.For(j.S, bar.S * ppd) with
        {
            WaveX = (float)((bar.Centre - bar.BarX - bar.BarW / 2) * ppd),
            WaveWidth = (float)(bar.Range * (1 + bar.K / 2) * ppd),
            WaveHeight = (float)(bar.Icon * waveRise * ppd),
        };
        var spec = Spec(j.Rs);
        int inset = (int)spec.Inset;
        int rise = spec.RiseInset;
        int bx = (int)Math.Round(bar.BarX * j.Rs), by = (int)Math.Round(bar.BarY * j.Rs);
        int bw = (int)Math.Round(bar.BarW * j.Rs), bh = (int)Math.Round(bar.BarH * j.Rs);
        int gw = bw + 2 * inset, gh = bh + 2 * inset + rise;
        var frames = _heroFrames;
        if (frames == null || frames.Width != gw || frames.Height != gh)
            _heroFrames = frames = new FrameBuffers(gw, gh);
        Array.Clear(frames.Back);
        GlassRenderer.DrawOverlay(_mainBg, frames.Back, bx - inset, by - inset - rise, gw, gh, spec);
        float luma = GlassRenderer.MeanLuma(frames.Back, gw, inset + Math.Clamp(bh / 2, 0, bw - 1), inset + rise + bh / 4,
            inset + Math.Clamp(bw - bh / 2, 1, bw), inset + rise + Math.Max(bh * 3 / 4, bh / 4 + 1));
        frames.Publish(luma, (bx - inset) / j.Rs, (by - inset - rise) / j.Rs, j.Rs);

        // Only the material page with the loupe enabled needs its second render.
        int lw = j.Loupe ? (int)Math.Round(Math.Round(bar.LoupeW * LoupeZoom) * j.Rs) : 0;
        int lh = j.Loupe ? (int)Math.Round(Math.Round(bar.LoupeH * LoupeZoom) * j.Rs) : 0;
        if (j.Loupe)
        {
            double ppd = LoupeZoom * j.Rs;
            string lkey = $"{lw}x{lh}/{j.W:F1}x{j.H:F1}/{bar.LoupeX0:F2},{bar.LoupeY0:F2}/{j.PhotoGeneration}";
            if (_loupeBg == null || lkey != _loupeKey)
            {
                _loupeBg = Backdrop.Create(lw, lh, j.W, j.H, bar.LoupeX0, bar.LoupeY0, ppd, j.Photo.Value);
                _loupeKey = lkey;
            }
            var lframes = _loupeFrames;
            if (lframes == null || lframes.Width != lw || lframes.Height != lh)
                _loupeFrames = lframes = new FrameBuffers(lw, lh);
            Buffer.BlockCopy(_loupeBg.Base, 0, lframes.Back, 0, _loupeBg.Base.Length);
            var lspec = Spec(ppd);
            int linset = (int)lspec.Inset;
            int lbx = (int)Math.Round((bar.BarX - bar.LoupeX0) * ppd), lby = (int)Math.Round((bar.BarY - bar.LoupeY0) * ppd);
            int lbw = (int)Math.Round(bar.BarW * ppd), lbh = (int)Math.Round(bar.BarH * ppd);
            GlassRenderer.Draw(_loupeBg, lframes.Back, lbx - linset, lby - linset - lspec.RiseInset,
                lbw + 2 * linset, lbh + 2 * linset + lspec.RiseInset, lspec);
            lframes.Publish(luma);
        }
        else
        {
            _loupeBg = null;
            _loupeFrames = null;
            _loupeKey = "";
        }

        if (Interlocked.Exchange(ref _uiPending, 1) == 0)
            DispatcherQueue.TryEnqueue(ApplyFrames);
    }

    // ---- UI thread: frames to bitmaps ----

    private void ApplyFrames()
    {
        Interlocked.Exchange(ref _uiPending, 0);
        if (Interlocked.Exchange(ref _photoPending, null) is { } bg)
        {
            _photoBmp = new WriteableBitmap(bg.Width, bg.Height);
            using (var stream = _photoBmp.PixelBuffer.AsStream()) stream.Write(bg.Base, 0, bg.Base.Length);
            _photoBmp.Invalidate();
            _heroBrush.ImageSource = _photoBmp;
        }
        if (_heroFrames is { } hf)
        {
            if (_glassBmp == null || _glassBmp.PixelWidth != hf.Width || _glassBmp.PixelHeight != hf.Height)
            {
                _glassBmp = new WriteableBitmap(hf.Width, hf.Height);
                _glass.Source = _glassBmp;
            }
            FrameBuffers.Placement at;
            using (var stream = _glassBmp.PixelBuffer.AsStream()) at = hf.WriteFront(stream);
            _glassBmp.Invalidate();
            _glass.Width = hf.Width / at.Scale;
            _glass.Height = hf.Height / at.Scale;
            Canvas.SetLeft(_glass, at.X);
            Canvas.SetTop(_glass, at.Y);
            if (Math.Abs(at.Luma - _luma) > 0.001f)
            {
                _luma = at.Luma;
                // kDarkInkAbove / kLightInkBelow (bar.cpp): no flicker at the threshold.
                bool dark = _darkInk ? _luma >= 0.42f : _luma > 0.50f;
                if (dark != _darkInk)
                {
                    _darkInk = dark;
                    if (CurrentSettings.TextColor == "auto") UpdateHero();
                }
            }
        }
        if (Loupe.Visibility == Visibility.Visible && _loupeFrames is { } lf) Upload(lf, ref _loupeBmp, _loupeBrush);
        if (_heroPhotoGen == Photo.Generation && _heroFrames != null) Ready("hero", _heroPhotoGen);
    }

    private static void Upload(FrameBuffers f, ref WriteableBitmap? bmp, ImageBrush brush)
    {
        if (bmp == null || bmp.PixelWidth != f.Width || bmp.PixelHeight != f.Height)
        {
            bmp = new WriteableBitmap(f.Width, f.Height);
            brush.ImageSource = bmp;
        }
        using (var stream = bmp.PixelBuffer.AsStream()) f.WriteFront(stream);
        bmp.Invalidate();
    }

    // ---- Preset swatches: the same renderer on a small canvas ----

    private const double SwatchW = 240, SwatchH = 72;
    private static (int Gen, Backdrop Bg)? _swatchBg;
    private static readonly Lock SwatchLock = new();

    private BarSettings? _swatchSettings;

    private void UpdateCustomSwatch()
    {
        if (_page != 0 || (_swatchSettings?.SameMaterial(CurrentSettings) ?? false)) return;
        _swatchSettings = CurrentSettings.Clone();
        _swatchWorker?.Post(_swatchSettings);
    }

    // Swatches are tiny; rendering them at 2x keeps them crisp up to 200 % scale.
    private void RenderSwatch(BarSettings material, Border target, SwatchPhoto? snapshot = null)
    {
        const double Rs = 2;
        int w = (int)Math.Round(SwatchW * Rs), h = (int)Math.Round(SwatchH * Rs);
        Backdrop bg;
        snapshot ??= new SwatchPhoto(Photo.Generation, Photo.Capture());
        int gen = snapshot.Generation;
        var photo = snapshot.Photo;
        if (gen != Photo.Generation) return;
        lock (SwatchLock)
        {
            if (_swatchBg?.Gen != gen) _swatchBg = (gen, Backdrop.Create(w, h, SwatchW, SwatchH, 0, 0, Rs, photo.Value));
            bg = _swatchBg.Value.Bg;
        }
        // A 60 DIP capsule at 60 %, centred.
        var s = material.Clone();
        s.Height = 60;
        s.CornerRadius = 30;
        var spec = GlassSpec.For(s, 0.6 * Rs);
        int inset = (int)spec.Inset;
        int bw = (int)Math.Round(170 * Rs), bh = (int)Math.Round(36 * Rs);
        int bx = (w - bw) / 2, by = (h - bh) / 2;
        byte[] pixels = new byte[bg.Base.Length];
        Buffer.BlockCopy(bg.Base, 0, pixels, 0, pixels.Length);
        GlassRenderer.Draw(bg, pixels, bx - inset, by - inset, bw + 2 * inset, bh + 2 * inset, spec);
        DispatcherQueue.TryEnqueue(() =>
        {
            if (gen != Photo.Generation || _updatesClosed) return;
            var bmp = new WriteableBitmap(w, h);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(pixels, 0, pixels.Length);
            bmp.Invalidate();
            target.Background = new ImageBrush { ImageSource = bmp, Stretch = Stretch.UniformToFill };
        });
    }
}
