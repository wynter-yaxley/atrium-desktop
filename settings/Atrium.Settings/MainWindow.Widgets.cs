using System.Numerics;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace Atrium.Settings;

// The widget gallery (D43): every widget the core can show, as a small
// picture of it on a piece of glass over the specimen photo; a click puts one on the desktop.
public sealed partial class MainWindow
{
    private sealed record WidgetChoice(string Kind, string Size, string Name, string Note, string Category);

    // The gallery's groups, in order; a segmented control above shows all of them or one.
    private static readonly string[] WidgetCategoryNames = ["Clock", "Weather", "Calendar", "Productivity", "Photos", "System"];

    private static WidgetChoice[] WidgetChoices =>
    [
        new("clock", "small", Loc.T("Clock"), Loc.T("Local time face"), "Clock"),
        new("clock", "medium", Loc.T("World clock"), Loc.T("Four city faces"), "Clock"),
        new("digital", "small", Loc.T("Digital clock"), Loc.T("Time, weekday and date"), "Clock"),
        new("digital", "medium", Loc.T("Digital clock"), Loc.T("Three more cities"), "Clock"),
        new("weather", "small", Loc.T("Weather"), Loc.T("Temperature and conditions"), "Weather"),
        new("weather", "medium", Loc.T("Weather"), Loc.T("Hourly forecast added"), "Weather"),
        new("weather", "large", Loc.T("Weather"), Loc.T("Five-day forecast added"), "Weather"),
        new("calendar", "small", Loc.T("Calendar"), Loc.T("Today and lunar date"), "Calendar"),
        new("calendar", "medium", Loc.T("Calendar"), Loc.T("Month calendar added"), "Calendar"),
        new("calendar", "large", Loc.T("Lunar month calendar"), Loc.T("Lunar date or festival under each day"), "Calendar"),
        new("notes", "small", Loc.T("Notes"), Loc.T("Click to type"), "Productivity"),
        new("reminders", "medium", Loc.T("Reminders"), Loc.T("Check off completed items"), "Productivity"),
        new("photos", "small", Loc.T("Photos"), Loc.T("One from your Pictures folder"), "Photos"),
        new("photos", "medium", Loc.T("Photos"), Loc.T("Changes every 10 minutes"), "Photos"),
        new("battery", "small", Loc.T("Battery"), Loc.T("Battery ring"), "System"),
        new("system", "small", Loc.T("System monitor"), Loc.T("CPU, memory and disk"), "System"),
        new("system", "medium", Loc.T("System monitor"), Loc.T("Three rings"), "System"),
        new("bluetooth", "small", Loc.T("Bluetooth devices"), Loc.T("Four device batteries"), "System"),
        new("bluetooth", "medium", Loc.T("Bluetooth devices"), Loc.T("Name and battery"), "System"),
        new("network", "small", Loc.T("Network"), Loc.T("Down and up rates, with the last minute"), "System"),
        new("network", "medium", Loc.T("Network"), Loc.T("Rates, the last minute and this session's traffic"), "System"),
    ];

    private readonly Dictionary<string, FrameworkElement> _widgetSections = [];

    // Each group under its name; the control above narrows the gallery to one group.
    private void BuildWidgetGallery()
    {
        foreach (string category in WidgetCategoryNames)
        {
            var grid = new ResponsiveGrid { MinCellWidth = 180, Gap = 12 };
            foreach (var choice in WidgetChoices.Where(c => c.Category == category))
                grid.Children.Add(GalleryCard(choice));
            var section = new StackPanel { Spacing = 8 };
            section.Children.Add(new TextBlock
            {
                Text = Loc.T(category),
                Style = (Style)Application.Current.Resources["RowTitleStyle"],
                Margin = new Thickness(4, 0, 0, 0),
            });
            section.Children.Add(grid);
            _widgetSections[category] = section;
            WidgetGallery.Children.Add(section);
        }
    }

    private void OnWidgetCategoryChecked(object sender, RoutedEventArgs e)
    {
        if (_updatesClosed || _session.Closed || sender is not RadioButton { Tag: string category }) return;
        foreach (var (name, section) in _widgetSections)
            section.Visibility = category == "All" || category == name ? Visibility.Visible : Visibility.Collapsed;
    }

    // The gallery only adds while the widgets are on.
    private void UpdateGallery()
    {
        if (_updatesClosed || _session.Closed) return;
        bool on = SideWidgets.WidgetsOn;
        WidgetGallery.Opacity = on ? 1 : 0.45;
        WidgetGallery.IsHitTestVisible = on;
        // Only why the gallery does not answer: the widgets are off.
        WidgetHint.Text = Loc.T("Turn on Desktop widgets first.");
        WidgetHint.Visibility = on ? Visibility.Collapsed : Visibility.Visible;
    }

    // A gallery card: the widget on a piece of the bar's glass over the picture, rendered like the hero.
    private sealed record GalleryPreview(WidgetChoice Choice, Border Stage, Viewbox Face, Canvas Viewport)
    {
        public bool? DarkInk;
        public long Stamp;
        public string? Material;
        public BarSettings? RenderedMaterial;
        public int RenderedGen = -1;
    }

    private readonly List<GalleryPreview> _galleryStages = [];
    private readonly Queue<Action> _galleryApplyQueue = [];
    private bool _galleryApplyScheduled, _addingWidget;

    private Button GalleryCard(WidgetChoice c)
    {
        var stage = new Border { Height = 118, CornerRadius = new CornerRadius(14), Background = PhotoBrush() };
        var face = new Viewbox { Stretch = Stretch.Fill };
        // The bitmap and the face share the last completed viewport. While a
        // resize render is pending, XAML stretches both by the same amount.
        var holder = new Canvas { Width = 1, Height = 1, IsHitTestVisible = false, Children = { face } };
        stage.Child = new Viewbox { Stretch = Stretch.Fill, Child = holder };
        var preview = new GalleryPreview(c, stage, face, holder);
        _galleryStages.Add(preview);
        stage.SizeChanged += (_, _) => RenderGalleryCard(preview);
        var name = new TextBlock
        {
            Text = c.Name + " · " + Loc.T(c.Size switch { "small" => "Small", "medium" => "Medium", _ => "Large" }),
            Style = (Style)Application.Current.Resources["RowTitleStyle"],
            Margin = new Thickness(4, 0, 4, 2),
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var body = new StackPanel { Spacing = 10, Children = { stage, name } };
        var button = new Button
        {
            Content = body,
            Padding = new Thickness(10),
            CornerRadius = new CornerRadius(24),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Background = Tokens.Brush("CardBrush"),
            BorderBrush = Tokens.Brush("CardStrokeBrush"),
            BorderThickness = new Thickness(1),
            // Pointer over: the card rises a little.
            TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(180) }
        };
        button.PointerEntered += (_, _) => button.Translation = new Vector3(0, -2, 0);
        button.PointerExited += (_, _) => button.Translation = Vector3.Zero;
        AutomationProperties.SetName(button, c.Size == "small" ? Loc.F("Add {0} (small)", c.Name)
            : c.Size == "medium" ? Loc.F("Add {0} (medium)", c.Name) : Loc.F("Add {0} (large)", c.Name));
        ToolTipService.SetToolTip(button, c.Note);
        button.Click += (_, _) => OnWidgetCardClick(button, c);
        return button;
    }

    private async void OnWidgetCardClick(Button button, WidgetChoice choice)
    {
        if (_addingWidget || _tilesStopped || _updatesClosed || _session.Closed) return;
        if (string.IsNullOrEmpty(choice.Kind) || string.IsNullOrEmpty(choice.Size)) return;
        _addingWidget = true;
        button.IsEnabled = false;
        try
        {
            string? error = await _client.AddWidgetAsync(choice.Kind, choice.Size);
            if (_tilesStopped || _updatesClosed || _session.Closed) return;
            WidgetHint.Text = error == null
                ? Loc.F("Added “{0}” to the desktop.", choice.Name)
                : error.Contains("room", StringComparison.OrdinalIgnoreCase)
                    ? Loc.T("No room on the desktop: remove or shrink a widget first.")
                    : Loc.F("Not added: {0}", error);
            WidgetHint.Visibility = Visibility.Visible;
        }
        catch (Exception ex)
        {
            App.LogCrash(ex);
            System.Diagnostics.Debug.WriteLine("widget add failed: " + ex);
            if (!_tilesStopped && !_updatesClosed && !_session.Closed)
            {
                try
                {
                    WidgetHint.Text = Loc.F("Not added: {0}", ex.Message);
                    WidgetHint.Visibility = Visibility.Visible;
                }
                catch (Exception uiEx)
                {
                    System.Diagnostics.Debug.WriteLine("widget hint update failed: " + uiEx);
                }
            }
        }
        finally
        {
            _addingWidget = false;
            if (!_tilesStopped && !_updatesClosed && !_session.Closed)
            {
                try { button.IsEnabled = true; }
                catch (Exception uiEx)
                {
                    System.Diagnostics.Debug.WriteLine("widget button restore failed: " + uiEx);
                }
            }
        }
    }

    // The gallery again when the picture or the material changed since it was drawn.
    private void RefreshGallery()
    {
        if (_tilesStopped || _updatesClosed || _session.Closed) return;
        foreach (var preview in _galleryStages)
        {
            if (preview.RenderedMaterial != null &&
                preview.RenderedMaterial.SameMaterial(CurrentSettings) &&
                preview.RenderedMaterial.WidgetMaterial == CurrentSettings.WidgetMaterial &&
                preview.RenderedGen == Photo.Generation)
            {
                continue;
            }
            RenderGalleryCard(preview);
        }
    }

    private void RenderGalleryCard(GalleryPreview preview)
    {
        if (_tilesStopped || _updatesClosed || _session.Closed) return;
        var (c, stage, face, viewport) = preview;
        double sw = stage.ActualWidth, sh = stage.ActualHeight;
        if (!double.IsFinite(sw) || !double.IsFinite(sh) || sw <= 28 || sh <= 28) return;
        // The widget fitted into the card, 14 DIP of air around.
        var (w, h) = WidgetFaces.Size(c.Size);
        if (w <= 0 || h <= 0) return;
        double k = Math.Min((sw - 28) / w, (sh - 28) / h);
        if (!double.IsFinite(k) || k <= 0) return;
        preview.RenderedMaterial = CurrentSettings.Clone();
        preview.RenderedGen = Photo.Generation;
        var tile = new GlassTile((sw - w * k) / 2, (sh - h * k) / 2, w * k, h * k, 22 * k);
        int mine = 0;
        mine = PostTiles(stage, sw, sh, k, [tile], null, frame => EnqueueGalleryApply(() =>
        {
            if (_tilesStopped || _updatesClosed || _session.Closed ||
                !_tileRequests.TryGetValue(stage, out int current) || current != mine)
            {
                return;
            }
            if (frame.Lumas.Length == 0 || tile.W <= 0 || tile.H <= 0) return;
            frame.Apply(stage);
            viewport.Width = sw;
            viewport.Height = sh;
            bool dark = frame.Lumas[0] > 0.5f;
            long stamp = WidgetFaces.Stamp(c.Kind);
            string material = CurrentSettings.WidgetMaterial;
            if (preview.DarkInk != dark || preview.Stamp != stamp || preview.Material != material)
            {
                face.Child = WidgetFaces.Face(c.Kind, c.Size, dark, PhotoBrush(), material);
                ContinuousCorners.Install(face);
                preview.DarkInk = dark;
                preview.Stamp = stamp;
                preview.Material = material;
            }
            face.Width = tile.W;
            face.Height = tile.H;
            Canvas.SetLeft(face, tile.X);
            Canvas.SetTop(face, tile.Y);
        }));
    }

    private void EnqueueGalleryApply(Action apply)
    {
        if (_tilesStopped || _updatesClosed || _session.Closed) return;
        _galleryApplyQueue.Enqueue(apply);
        ScheduleGalleryDrain();
    }

    private void ScheduleGalleryDrain()
    {
        if (_galleryApplyScheduled || _tilesStopped || _updatesClosed || _session.Closed || _galleryApplyQueue.Count == 0) return;
        _galleryApplyScheduled = DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, DrainGalleryApplies);
    }

    private void DrainGalleryApplies()
    {
        _galleryApplyScheduled = false;
        if (_tilesStopped || _updatesClosed || _session.Closed)
        {
            _galleryApplyQueue.Clear();
            return;
        }
        int processed = 0;
        while (processed < 4 && _galleryApplyQueue.Count > 0 && !_tilesStopped && !_updatesClosed && !_session.Closed)
        {
            var apply = _galleryApplyQueue.Dequeue();
            try
            {
                apply();
            }
            catch (Exception ex)
            {
                App.LogCrash(ex);
                System.Diagnostics.Debug.WriteLine("gallery apply failed: " + ex);
            }
            processed++;
        }
        ScheduleGalleryDrain();
    }

    // ---- The widgets' glass on the CPU: the picture, the tiles of glass, their mean brightness ----

    // One drain for all tile previews, with at most two tiles rendering at once. Pending requests
    // replace only the same target; equal viewports share a backdrop. No full-size map or obsolete
    // photo is kept alive after the batch.
    private sealed record TileJob(object Target, int Id, double W, double H, double K, double Rs,
                                  IReadOnlyList<GlassTile> Tiles, GlassTile? Bar, BarSettings Material,
                                  Lazy<Photo> Photo, Action<TileFrame> Done, bool Overlays, bool Scaled);
    private readonly Lock _tileLock = new();
    private readonly Dictionary<object, TileJob> _pendingTiles = [];
    private bool _tilesBusy, _tilesStopped;
    private readonly Dictionary<object, int> _tileRequests = [];

    // A rendered stage: its pixels (BGRA), and each tile's brightness.
    private sealed record TileFrame(byte[] Px, int PW, int PH, double Rs, float[] Lumas, GlassOverlay[]? Overlays = null)
    {
        public WriteableBitmap Overlay(int index)
        {
            if (Overlays == null || (uint)index >= (uint)Overlays.Length)
                throw new ArgumentOutOfRangeException(nameof(index));
            var image = Overlays[index];
            if (image.Width <= 0 || image.Height <= 0)
                throw new InvalidOperationException("Tile overlay has empty bounds.");
            var bmp = new WriteableBitmap(image.Width, image.Height);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(image.Pixels);
            bmp.Invalidate();
            return bmp;
        }
        public void Apply(Border stage)
        {
            if (PW <= 0 || PH <= 0 || Px.Length == 0) return;
            var brush = ContinuousCorners.Original(stage) as ImageBrush;
            var bmp = brush?.ImageSource as WriteableBitmap;
            if (bmp == null || bmp.PixelWidth != PW || bmp.PixelHeight != PH)
            {
                bmp = new WriteableBitmap(PW, PH);
                stage.Background = new ImageBrush { ImageSource = bmp, Stretch = Stretch.Fill };
            }
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(Px, 0, Px.Length);
            bmp.Invalidate();
        }
    }

    private int PostTiles(object target, double w, double h, double k, IReadOnlyList<GlassTile> tiles, GlassTile? bar,
                          Action<TileFrame> done, bool overlays = false, bool scaled = false)
    {
        if (_tilesStopped || _updatesClosed || _session.Closed ||
            !double.IsFinite(w) || !double.IsFinite(h) || !double.IsFinite(k) ||
            w <= 0 || h <= 0 || k <= 0 || tiles.Count == 0)
        {
            return 0;
        }
        double rs = Math.Clamp(Content?.XamlRoot?.RasterizationScale ?? 1, 1, 2);
        if (!double.IsFinite(rs) || rs <= 0) rs = 1;
        int mine = _tileRequests[target] = (_tileRequests.TryGetValue(target, out int n) ? n : 0) + 1;
        bool startWorker = false;
        lock (_tileLock)
        {
            if (_tilesStopped) return mine;
            _pendingTiles[target] = new TileJob(target, mine, w, h, k, rs, tiles, bar, CurrentSettings.Clone(), Photo.Capture(), done, overlays, scaled);
            if (!_tilesBusy)
            {
                _tilesBusy = true;
                startWorker = true;
            }
        }
        if (startWorker)
        {
            _ = Task.Run(RenderTiles).ContinueWith(t =>
            {
                if (t.Exception != null)
                {
                    App.LogCrash(t.Exception);
                    System.Diagnostics.Debug.WriteLine("tile render worker failed: " + t.Exception);
                }
            }, TaskScheduler.Default);
        }
        return mine;
    }

    private void StopTiles()
    {
        lock (_tileLock) { _tilesStopped = true; _pendingTiles.Clear(); }
        if (DispatcherQueue.HasThreadAccess)
        {
            _galleryApplyQueue.Clear();
            _galleryApplyScheduled = false;
        }
        else
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                _galleryApplyQueue.Clear();
                _galleryApplyScheduled = false;
            });
        }
    }

    private void RenderTiles()
    {
        Backdrop? bg = null;
        (double W, double H, double Rs, Lazy<Photo> Photo)? backdrop = null;
        bool completed = false;
        try
        {
            while (true)
            {
                TileJob[] jobs;
                lock (_tileLock)
                {
                    if (_tilesStopped || _pendingTiles.Count == 0)
                    {
                        _tilesBusy = false;
                        completed = true;
                        return;
                    }
                    jobs = [.. _pendingTiles.Values];
                    _pendingTiles.Clear();
                }
                foreach (var group in jobs.GroupBy(j => (j.W, j.H, j.Rs, j.Photo)))
                {
                    var key = group.Key;
                    int pw = (int)Math.Round(key.W * key.Rs), ph = (int)Math.Round(key.H * key.Rs);
                    if (pw <= 0 || ph <= 0) continue;
                    try
                    {
                        if (bg == null || backdrop != key)
                        {
                            bg = Backdrop.Create(pw, ph, key.W, key.H, 0, 0, key.Rs, key.Photo.Value);
                            backdrop = key;
                        }
                        Parallel.ForEach(group, new ParallelOptions { MaxDegreeOfParallelism = 2 }, job =>
                        {
                            lock (_tileLock)
                                if (_tilesStopped || _pendingTiles.ContainsKey(job.Target)) return;
                            TileFrame frame;
                            if (job.Overlays)
                            {
                                // Scaled: the widget map, whose picture comes along (its cards move over it).
                                var cards = GlassRenderer.CardOverlays(bg, job.Material, job.Rs, job.Tiles, job.Scaled ? job.K : 0);
                                frame = new TileFrame(job.Scaled ? bg.Base : [], pw, ph, job.Rs, [.. cards.Select(c => c.Luma)], cards);
                            }
                            else
                            {
                                byte[] px = new byte[bg.Base.Length];
                                float[] lumas = GlassRenderer.DrawTiles(bg, px, job.Material, job.K, job.Rs, job.Tiles, job.Bar);
                                frame = new TileFrame(px, pw, ph, job.Rs, lumas);
                            }
                            DispatcherQueue.TryEnqueue(() =>
                            {
                                if (_tilesStopped || _updatesClosed || _session.Closed) return;
                                if (!_tileRequests.TryGetValue(job.Target, out int current) || current != job.Id) return;
                                try
                                {
                                    job.Done(frame);
                                }
                                catch (Exception ex)
                                {
                                    App.LogCrash(ex);
                                    System.Diagnostics.Debug.WriteLine("tile preview apply failed: " + ex);
                                }
                            });
                        });
                    }
                    catch (Exception e)
                    {
                        System.Diagnostics.Debug.WriteLine("tile preview render failed: " + e);
                    }
                }
            }
        }
        finally
        {
            if (!completed)
            {
                lock (_tileLock) { _tilesBusy = false; }
            }
        }
    }
}
