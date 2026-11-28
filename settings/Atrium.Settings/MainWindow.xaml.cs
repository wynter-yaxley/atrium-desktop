using System.Diagnostics;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;

namespace Atrium.Settings;

public sealed partial class MainWindow : Window, IDisposable
{
    // One slider tile: its group, UI range, and the mapping between the stored
    // value and the slider value (percentages are stored as 0..1).
    private sealed record SliderDef(string Group, string Title, double Min, double Max, double Step, string Unit,
        Func<BarSettings, double> Get, Action<BarSettings, double> Set, bool Material);

    private static SliderDef[] Sliders =>
    [
        new("clarity", Loc.T("Transparency"), 0, 100, 1, "%",
            s => s.Transparency * 100, (s, v) => s.Transparency = v / 100, true),
        new("clarity", Loc.T("Blur"), 0, 100, 1, "%",
            s => s.Blur * 100, (s, v) => s.Blur = v / 100, true),
        new("edge", Loc.T("Refraction"), 0, 60, 1, "px",
            s => s.Refraction, (s, v) => s.Refraction = v, true),
        new("edge", Loc.T("Refraction edge"), 1, 80, 1, "px",
            s => s.Edge, (s, v) => s.Edge = v, true),
        new("edge", Loc.T("Dispersion"), 0, 50, 1, "%",
            s => s.Dispersion * 100, (s, v) => s.Dispersion = v / 100, true),
        new("light", Loc.T("Highlight"), 0, 100, 1, "%",
            s => s.Highlight * 100, (s, v) => s.Highlight = v / 100, true),
        new("light", Loc.T("Shadow"), 0, 100, 1, "%",
            s => s.Shadow * 100, (s, v) => s.Shadow = v / 100, true),

        new("size", Loc.T("Height"), 32, 120, 1, "px",
            s => s.Height, (s, v) => s.Height = v, false),
        new("size", Loc.T("Corner radius"), 0, 80, 1, "px",
            s => s.CornerRadius, (s, v) => s.CornerRadius = v, false),
        new("dock", Loc.T("Icon size"), 16, 64, 1, "px",
            s => s.IconSize, (s, v) => s.IconSize = v, false),
        new("dock", Loc.T("Icon spacing"), 2, 40, 1, "px",
            s => s.IconSpacing, (s, v) => s.IconSpacing = v, false),
        new("width", Loc.T("Maximum width"), 300, 2400, 10, "px",
            s => s.Width, (s, v) => s.Width = v, false),
        new("dock", Loc.T("Edge spacing"), 0, 200, 1, "px",
            s => s.Margin, (s, v) => s.Margin = v, false),
        new("dock", Loc.T("Hover magnification"), 100, 200, 5, "%",
            s => s.Magnify * 100, (s, v) => s.Magnify = v / 100, false),

        // The band above the bar that answers the pointer ahead of time (riseNear, magnifyNear).
        new("near", Loc.T("Enter distance"), 2, 160, 2, "px",
            s => s.NearEnter, (s, v) => s.NearEnter = v, false),
        new("near", Loc.T("Leave distance"), 2, 200, 2, "px",
            s => s.NearLeave, (s, v) => s.NearLeave = v, false),
    ];

    private static string[] PageTitles =>
    [
        Loc.T("Material"), Loc.T("Shape"), Loc.T("Taskbar"), Loc.T("Notifications"), Loc.T("Widgets"),
        Loc.T("Stage Manager"), Loc.T("Displays"), Loc.T("System"), Loc.T("Design language"), Loc.T("About"),
        Loc.T("Start menu"), Loc.T("Tray"), Loc.T("Status bar"), Loc.T("Time"), Loc.T("Input method"), Loc.T("Now Playing"), Loc.T("File Stacks"),
        Loc.T("Launchpad"),
    ];

    private readonly CoreClient _client = new();
    private readonly SettingsSession _session = new();
    private readonly List<(SliderDef Def, SliderTile Tile)> _rows = [];
    private readonly List<(uint Rgb, Button Button, FrameworkElement Ring)> _tintButtons = [];
    private bool _loading;
    private bool _previewBusy, _previewPending;
    private Task<bool>? _saveTask;
    private bool _closePending, _closeAfterCore;
    private ContentDialog? _dialog; // the one shown (AskAsync)
    private readonly FrameworkElement[] _mains = [];
    private readonly FrameworkElement?[] _sides = [];
    private int _page;
    private bool _windowLoaded;
    private bool _folded; // a narrow window: the inspector's groups under the workspace
    private int _flashId;
    private bool _updatingCoreSwitch, _coreChanging = true;
    private Process? _coreProcess;
    private CancellationTokenSource? _updateCheck;

    public MainWindow(int startPage = -1)
    {
        InitializeComponent();
        AutomationProperties.SetAutomationId(SaveButton, "SaveButton");
        AutomationProperties.SetAutomationId(ClockSecondsSwitch.Switch, "ClockSecondsSwitch");
        // Windows 10 (#106): no Mica there, and a bitmap decoded during the first frame takes its XAML
        // renderer down; the logos come once the window is up.
        if (MicaController.IsSupported()) SystemBackdrop = new MicaBackdrop { Kind = MicaKind.Base };
        RootGrid.Loaded += (_, _) => DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            var logo = new BitmapImage(new Uri("ms-appx:///Assets/atrium-256.png"));
            TitleLogo.Source = MainAbout.LogoImage.Source = logo;
        });
        BindPages();
        Title = App.WindowTitle;
        Loc.Localize(RootGrid);
        // The inspector lives in its own ScrollViewer; localize it directly as
        // well, and again when a page (re)appears below.
        Loc.Localize(InspectorStack);
        Loc.Localize(FoldedInspector);
        LocalizeNav();
        RootGrid.Loaded += (_, _) =>
        {
            Loc.Localize(RootGrid);
            Loc.Localize(InspectorStack);
            ContinuousCorners.Install(RootGrid);
            LoadStageArt(); // the Stage Manager page's pictures, ready before it opens
#if ATRIUM_DEVELOPMENT
            ExportStageArtIfAsked();
#endif
            if (!_windowLoaded) { _windowLoaded = true; ActivateCurrentPageAfterLoad(); }
        };
        ExtendsContentIntoTitleBar = true;
        if (AppWindowTitleBar.IsCustomizationSupported())
            AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        SetTitleBar(AppTitleBar);
        InitTheme();
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "atrium.ico"));
        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        SetProp(hwnd, App.WindowProp, 1);
        uint dpi = GetDpiForWindow(hwnd);
        double scale = (dpi > 0 ? dpi : 96) / 96.0;
        // A roomy default (the preview is the centre of the window), never larger than the screen.
        if (DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary) is { } area)
        {
            var work = area.WorkArea;
            int width = Math.Min((int)(1440 * scale), work.Width * 92 / 100), height = Math.Min((int)(920 * scale), work.Height * 92 / 100);
            AppWindow.MoveAndResize(new Windows.Graphics.RectInt32(work.X + (work.Width - width) / 2, work.Y + (work.Height - height) / 2, width, height));
        }
        UpdateWindowMinimumSize();
        AppWindow.Closing += OnClosing;
        AppWindow.Changed += OnAppWindowChanged;
        VisibilityChanged += OnWindowVisibilityChanged;
        _updateMessage = (handle, message, wparam, lparam, id, data) =>
        {
            if (message == App.OpenPageMessage) { if (wparam.ToUInt64() < (ulong)_mains.Length) SelectPage((int)wparam.ToUInt64()); return IntPtr.Zero; }
            if (message == App.CloseForUninstallMessage) { _client.Dispose(); Close(); return IntPtr.Zero; }
            if (message == 0x02E0) UpdateWindowMinimumSize((uint)(wparam.ToUInt64() & 0xFFFF), lparam);
            if (message is 0x007E or 0x02E0 || (message == 0x001A && wparam.ToUInt64() == 0x002F))
                DispatcherQueue.TryEnqueue(() => { RefreshDisplays(); UpdateWindowMinimumSize(); }); // hotplug, DPI or work-area changed
            return DefSubclassProc(handle, message, wparam, lparam);
        };
        SetWindowSubclass(hwnd, _updateMessage, 1, UIntPtr.Zero);
        Closed += (_, _) =>
        {
            Dispose();
            RemoveProp(hwnd, App.WindowProp);
            RemoveWindowSubclass(hwnd, _updateMessage, 1);
            UnsubscribeTheme();
            _updateCheck?.Cancel();
            _updatesClosed = true;
            _session.Close();
            _downloadTimer?.Stop();
            _reconnectTimer?.Stop();
            StopDesignAnimations();
            _renderWorker?.Stop();
            _swatchWorker?.Stop();
            _fixedSwatchWorker?.Stop();
            _photoThawTimer?.Stop();
            ++_photoSelection;
            StopTiles();
            StopModules();
        };

        _mains =
        [
            MainMaterial, MainShape, MainDock, MainNotify, MainWidgets, MainStage, MainDisplays, MainSystem, DesignStack, MainAbout,
            MainStart, MainTray, MainStatus, MainClock, MainIme, ExtrasPage(NowPlayingPage), ExtrasPage(StacksPage), ExtrasPage(LaunchpadPage),
        ];
        _sides =
        [
            SideMaterial, SideShape, SideDock, SideNotify, SideWidgets, SideStage, null, null, null, null,
            SideStart, SideTray, SideStatus, SideClock, SideIme, ExtrasSide(NowPlayingPage), ExtrasSide(StacksPage), ExtrasSide(LaunchpadPage),
        ];
        InitDisplays();
        InitUpdates();
        LoupeSwitch.IsOn = true;
        BuildRows();
        BuildTintSwatches();
        InitPins();
        InitSources();
        InitHero();
        PhotoChanged += () =>
        {
            if (!_windowLoaded) return;
            if (_page == 4) { ShowWidgetMap(); RefreshGallery(); }
            if (_page == 3) ShowNotifyPreview(false);
            if (_page == 5) ShowStagePreview(false);
            if (_page >= 10) ShowModulePreview(_page);
        };
        MainAbout.PathLabel.Text = CoreClient.SettingsPath;
        var v = typeof(MainWindow).Assembly.GetName().Version;
        MainAbout.VersionLabel.Text = Loc.F("Version {0}", v?.ToString(3) ?? "1.0.0");
        ShowStatus(false);
        // `--page widgets` (the widgets' menu): open on that page.
        int page = startPage >= 0 ? startPage : App.PageFromArgs(Environment.GetCommandLineArgs()) ?? 0;
        SelectPage(page);
        _ = ConnectAndLoadAsync();
    }

    private void BindPages()
    {
        MainSystem.Bind(() => CurrentSettings, () => _loading, Changed);
        SideStage.Bind(() => CurrentSettings, () => _loading, (refreshPreview, syncWidgets) =>
        {
            if (syncWidgets)
            {
                CurrentSettings.WidgetsOnRight = !CurrentSettings.StageOnRight;
                SideWidgets.SetSide(CurrentSettings.WidgetsOnRight);
            }
            Changed();
            if (refreshPreview && _page == 5) ShowStagePreview(true);
        });
        SideWidgets.Bind(
            () => CurrentSettings,
            () => _loading,
            () => { UpdateGallery(); if (!_loading) Changed(); },
            () =>
            {
                if (CurrentSettings.StageManager)
                {
                    CurrentSettings.StageOnRight = !CurrentSettings.WidgetsOnRight;
                    SideStage.SetSide(CurrentSettings.StageOnRight);
                }
                ShowWidgetMap();
                Changed();
            },
            () => { Changed(); if (_page == 4) { ShowWidgetMap(); RefreshGallery(); } },
            Changed);
    }

    // A language switch builds the new settings window while this one is
    // still open; release the core pipe first so the new window can connect.
    internal void ReleaseForReload()
    {
        RemoveProp(WinRT.Interop.WindowNative.GetWindowHandle(this), App.WindowProp);
        _updatesClosed = true;
        UnsubscribeTheme();
        _session.Close();
        _updateCheck?.Cancel();
        _downloadTimer?.Stop();
        _settingsWatcher?.Dispose();
        _settingsWatcher = null;
        _widgetWatcher?.Dispose();
        _widgetWatcher = null;
        _client.Dispose();
    }

    public void Dispose()
    {
        _client.Dispose();
        _coreProcess?.Dispose();
        _widgetWatcher?.Dispose();
        _settingsWatcher?.Dispose();
    }

    // ---- Rows ----

    // Every slider is a tile in its group's grid: after the group's other tiles, or before them
    // where the sliders lead (the tint, the notification switch).
    private void BuildRows()
    {
        // The basic three are tiles in the workspace; the rest are plain rows in the inspector.
        var groups = new Dictionary<string, (Panel Panel, bool First, bool Flat)>
        {
            ["clarity"] = (ClarityTiles, true, false),
            ["edge"] = (EdgeRows, false, true),
            ["light"] = (LightRows, false, true),
            ["size"] = (SizeRows, false, true),
            ["width"] = (WidthRows, false, true),
            ["dock"] = (DockRows, false, true),
            ["notify"] = (NotifyRows, true, true),
            ["near"] = (NearRows, false, true),
        };
        var placed = new Dictionary<string, int>();
        foreach (var d in Sliders)
        {
            var (panel, first, flat) = groups[d.Group];
            var tile = new SliderTile { Flat = flat };
            tile.Configure(d.Title, d.Min, d.Max, d.Step, d.Unit);
            tile.UserValueChanged += v => OnSliderChanged(d, v);
            tile.Engaged += on => OnEngage(d.Title, on);
            _rows.Add((d, tile));
            int at = first ? placed.GetValueOrDefault(d.Group) : panel.Children.Count;
            panel.Children.Insert(at, tile);
            placed[d.Group] = at + 1;
        }
        AutoWidthSwitch.GotFocus += (_, _) => OnEngage(Loc.T("Maximum width"), true);
        AutoWidthSwitch.LostFocus += (_, _) => OnEngage(Loc.T("Maximum width"), false);
    }

    private static readonly (string Name, uint Rgb)[] TintChoices =
    [
        ("White", 0xFFFFFF), ("Warm white", 0xFFE6C8), ("Cool blue", 0xD3E6FF), ("Rose", 0xFFD0DC), ("Mint", 0xCCF4E0), ("Graphite", 0x2E3138),
    ];

    private void BuildTintSwatches()
    {
        foreach (var (name, rgb) in TintChoices)
        {
            var ring = new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = 28,
                Height = 28,
                Stroke = Tokens.Brush("AccentBrush"),
                StrokeThickness = 2,
                Opacity = 0,
                OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) }
            };
            var dot = new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = 22,
                Height = 22,
                Fill = new SolidColorBrush(TintColor(rgb)),
                Stroke = Tokens.Brush("CardStrokeBrush"),
                StrokeThickness = 1,
            };
            var button = new Button
            {
                Content = new Grid { Width = 28, Height = 28, Children = { dot, ring } },
                Style = (Style)Application.Current.Resources["IconButtonStyle"],
                Width = 32,
                Height = 32,
                CornerRadius = new CornerRadius(16),
            };
            AutomationProperties.SetName(button, Loc.F("Tint {0}", Loc.T(name)));
            ToolTipService.SetToolTip(button, Loc.T(name));
            uint captured = rgb;
            button.Click += (_, _) => SetTint(captured);
            _tintButtons.Add((rgb, button, ring));
            TintSwatches.Children.Add(button);
        }
    }

    private void SetTint(uint rgb)
    {
        CurrentSettings.Tint = rgb;
        MarkCustom();
        _loading = true;
        TintPicker.Color = TintColor(rgb);
        _loading = false;
        ShowTint();
        Changed();
    }

    private void ShowTint()
    {
        var c = TintColor(CurrentSettings.Tint);
        TintSwatch.Fill = new SolidColorBrush(c);
        TintHex.Text = "#" + CurrentSettings.Tint.ToString("X6", System.Globalization.CultureInfo.InvariantCulture);
        foreach (var (rgb, _, ring) in _tintButtons) ring.Opacity = rgb == CurrentSettings.Tint ? 1 : 0;
    }

    // ---- Controls <-> settings ----

    private void LoadIntoControls()
    {
        _loading = true;
        foreach (var (d, card) in _rows)
            card.SetValue(d.Get(CurrentSettings));
        (CurrentSettings.Preset switch { "liquid" => PresetLiquid, "frosted" => PresetFrosted, _ => PresetCustom }).IsChecked = true;
        (Photo.Selected switch
        { "wallpaper" => BgWallpaper, "paper" => BgPaper, "geometry" => BgGeometry, "layers" => BgLayers, _ => BgSpecimen }).IsChecked = true;
        ClockSwitch.IsOn = CurrentSettings.ShowClock;
        ClockSecondsSwitch.IsOn = CurrentSettings.ShowClockSeconds;
        UpdateClockOptions();
        TraySwitch.IsOn = CurrentSettings.ShowTray;
        TrayHostSwitch.IsOn = CurrentSettings.TrayHost;
        StartTraySwitch.IsOn = CurrentSettings.StartTray;
        StartStatusSwitch.IsOn = CurrentSettings.StartStatus;
        StartCenteredSwitch.IsOn = CurrentSettings.StartCentered;
        StartWithBarSwitch.IsOn = CurrentSettings.StartWithBar;
        (CurrentSettings.StartSize switch { "small" => StartSmall, "large" => StartLarge, "xlarge" => StartXLarge, _ => StartMedium }).IsChecked = true;
        (CurrentSettings.StartIcon switch
        { "small" => StartIconSmall, "large" => StartIconLarge, "xlarge" => StartIconXLarge, _ => StartIconMedium }).IsChecked = true;
        StatusSwitch.IsOn = CurrentSettings.ShowStatus;
        NotificationsSwitch.IsOn = CurrentSettings.ShowNotifications;
        DesktopSwitch.IsOn = CurrentSettings.ShowDesktop;
        AttentionLightSwitch.IsOn = CurrentSettings.AttentionLight;
        AttentionBounceSwitch.IsOn = CurrentSettings.AttentionBounce;
        BarBadgesSwitch.IsOn = CurrentSettings.BarBadges;
        AutoWidthSwitch.IsOn = CurrentSettings.AutoWidth;
        HoverRiseSwitch.IsOn = CurrentSettings.HoverRise;
        RiseNearSwitch.IsOn = CurrentSettings.RiseNear;
        MagnifyNearSwitch.IsOn = CurrentSettings.MagnifyNear;
        UpdateRiseNearDependent();
        RunningSwitch.IsOn = CurrentSettings.ShowRunning;
        ClickWindowPickerSwitch.IsOn = CurrentSettings.ClickWindowPicker;
        HideBodySwitch.IsOn = CurrentSettings.NotifyHideBody;
        NotifyFullscreenSwitch.IsOn = CurrentSettings.NotifyFullscreen;
        ImeStatusSwitch.IsOn = CurrentSettings.ImeStatus;
        ImeBubbleSwitch.IsOn = CurrentSettings.ImeBubble;
        ImeBubbleSwitch.IsEnabled = CurrentSettings.ImeStatus;
        bool notifyCorner = BarSettings.NotifyCorners.Contains(CurrentSettings.NotifyPlacement);
        if (notifyCorner) _notifyCorner = CurrentSettings.NotifyPlacement;
        (notifyCorner ? NotifyAtCorner : NotifyAtDock).IsChecked = true;
        NotifyCornerRow.Visibility = notifyCorner ? Visibility.Visible : Visibility.Collapsed;
        (_notifyCorner switch
        { "topLeft" => NotifyTopLeft, "bottomLeft" => NotifyBottomLeft, "bottomRight" => NotifyBottomRight, _ => NotifyTopRight }).IsChecked = true;
        RefreshSources();
        if (ModuleGallery.Children.Count > 0) RefreshModuleGallery(); // also when the page already shows (opened on it, before the settings came)
        LoadExtras();
        BarSwitch.IsOn = CurrentSettings.Bar;
        UpdateBarModule();
        SideWidgets.Load(CurrentSettings);
        SideStage.Load(CurrentSettings);
        RebuildExcluded();
        MainSystem.Load(CurrentSettings);
        MainAbout.Load(CurrentSettings);
        (CurrentSettings.StatusStyle == "icons" ? StatusIcons : StatusRing).IsChecked = true;
        (CurrentSettings.AutoHide switch { "never" => HideNever, "always" => HideAlways, _ => HideFullscreen }).IsChecked = true;
        BarDodgeSwitch.IsOn = CurrentSettings.BarDodge;
        BarDodgeSwitch.Visibility = CurrentSettings.AutoHide != "always" ? Visibility.Visible : Visibility.Collapsed;
        BarReserveSwitch.IsOn = CurrentSettings.BarReserve;
        BarFullscreenHideSwitch.IsOn = CurrentSettings.BarFullscreenHide;
        VisibleOptions.Visibility = CurrentSettings.AutoHide == "never" ? Visibility.Visible : Visibility.Collapsed;
        (CurrentSettings.BarEdge switch { "top" => EdgeTop, "left" => EdgeLeft, "right" => EdgeRight, _ => EdgeBottom }).IsChecked = true;
        LoadDisplayControls();
        (CurrentSettings.TextColor switch { "light" => TextLight, "dark" => TextDark, _ => TextAuto }).IsChecked = true;
        UpdateGallery();
        RebuildPins();
        MainDock.ContextMenuSwitch.IsOn = CurrentSettings.WindowsContextMenu;
        TintPicker.Color = TintColor(CurrentSettings.Tint);
        ShowTint();
        _loading = false;
        RefreshLoadedPreviews(true);
    }

    private static Color TintColor(uint rgb) => Color.FromArgb(255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

    private void Set(Action<BarSettings> apply)
    {
        if (_loading) return;
        apply(CurrentSettings);
        Changed();
    }

    private void OnSliderChanged(SliderDef d, double v)
    {
        if (_loading) return;
        d.Set(CurrentSettings, v);
        if (d.Material) MarkCustom();
        Changed();
    }

    private void MarkCustom()
    {
        CurrentSettings.Preset = "custom";
        bool was = _loading;
        _loading = true;
        PresetCustom.IsChecked = true;
        _loading = was;
    }

    private void OnPresetChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        if (tag == "liquid") CurrentSettings.ApplyMaterial(BarSettings.Liquid());
        else if (tag == "frosted") CurrentSettings.ApplyMaterial(BarSettings.Frosted());
        else CurrentSettings.Preset = "custom";
        LoadIntoControls();
        Changed();
    }

    private void OnTintChanged(ColorPicker sender, ColorChangedEventArgs args)
    {
        if (_loading) return;
        CurrentSettings.Tint = ((uint)args.NewColor.R << 16) | ((uint)args.NewColor.G << 8) | args.NewColor.B;
        MarkCustom();
        ShowTint();
        Changed();
    }

    private void OnClockToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.ShowClock = ClockSwitch.IsOn;
        UpdateClockOptions();
        Changed();
    }

    // Seconds only matter while the clock itself is shown.
    private void UpdateClockOptions()
    {
        ClockSecondsSwitch.IsEnabled = CurrentSettings.ShowClock;
        ClockSecondsSwitch.Opacity = CurrentSettings.ShowClock ? 1 : 0.45;
    }

    private void OnBarContentToggled(object sender, RoutedEventArgs e) => Set(s =>
    {
        s.ShowTray = TraySwitch.IsOn;
        s.TrayHost = TrayHostSwitch.IsOn;
        s.StartTray = StartTraySwitch.IsOn;
        s.StartStatus = StartStatusSwitch.IsOn;
        s.ShowStatus = StatusSwitch.IsOn;
        s.ShowNotifications = NotificationsSwitch.IsOn;
        s.ShowDesktop = DesktopSwitch.IsOn;
        s.StartCentered = StartCenteredSwitch.IsOn;
        s.StartWithBar = StartWithBarSwitch.IsOn;
        s.AttentionLight = AttentionLightSwitch.IsOn;
        s.AttentionBounce = AttentionBounceSwitch.IsOn;
        s.BarBadges = BarBadgesSwitch.IsOn;
    });

    private void OnHoverRiseToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.HoverRise = HoverRiseSwitch.IsOn;
        UpdateRiseNearDependent();
        Changed();
    }

    // The early rise only means something while the glass rises with the wave at all.
    private void UpdateRiseNearDependent()
    {
        RiseNearSwitch.Opacity = CurrentSettings.HoverRise ? 1 : 0.45;
        RiseNearSwitch.IsHitTestVisible = CurrentSettings.HoverRise;
    }

    private void OnImeStatusToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.ImeStatus = ImeStatusSwitch.IsOn;
        ImeBubbleSwitch.IsEnabled = ImeStatusSwitch.IsOn; // the bubble announces what the status reads
        Changed();
    }

    // The bar module: off, its own options and the notification cards have nothing to act on.
    private void OnBarToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.Bar = BarSwitch.IsOn;
        UpdateBarModule();
        Changed();
    }

    private void UpdateBarModule()
    {
        foreach (var part in new FrameworkElement[] { DockContent, DockLayout, SideNotify })
        {
            part.Opacity = CurrentSettings.Bar ? 1 : 0.45;
            part.IsHitTestVisible = CurrentSettings.Bar;
        }
    }

    private void OnAutoHideChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.AutoHide = tag;
        BarDodgeSwitch.Visibility = tag != "always" ? Visibility.Visible : Visibility.Collapsed;
        VisibleOptions.Visibility = tag == "never" ? Visibility.Visible : Visibility.Collapsed;
        Changed();
    }

    private void OnClockSecondsToggled(object s, RoutedEventArgs e) => Set(x => x.ShowClockSeconds = ClockSecondsSwitch.IsOn);
    private void OnAutoWidthToggled(object s, RoutedEventArgs e) => Set(x => x.AutoWidth = AutoWidthSwitch.IsOn);
    private void OnRiseNearToggled(object s, RoutedEventArgs e) => Set(x => x.RiseNear = RiseNearSwitch.IsOn);
    private void OnMagnifyNearToggled(object s, RoutedEventArgs e) => Set(x => x.MagnifyNear = MagnifyNearSwitch.IsOn);
    private void OnRunningToggled(object s, RoutedEventArgs e) => Set(x => x.ShowRunning = RunningSwitch.IsOn);
    private void OnClickWindowPickerToggled(object s, RoutedEventArgs e) => Set(x => x.ClickWindowPicker = ClickWindowPickerSwitch.IsOn);
    private void OnImeBubbleToggled(object s, RoutedEventArgs e) => Set(x => x.ImeBubble = ImeBubbleSwitch.IsOn);
    private void OnBarDodgeToggled(object s, RoutedEventArgs e) => Set(x => x.BarDodge = BarDodgeSwitch.IsOn);
    private void OnBarReserveToggled(object s, RoutedEventArgs e) => Set(x => x.BarReserve = BarReserveSwitch.IsOn);
    private void OnBarFullscreenHideToggled(object s, RoutedEventArgs e) => Set(x => x.BarFullscreenHide = BarFullscreenHideSwitch.IsOn);
    private void OnStartSizeChecked(object s, RoutedEventArgs e) { if (s is RadioButton { Tag: string t }) Set(x => x.StartSize = t); }
    private void OnStartIconSizeChecked(object s, RoutedEventArgs e) { if (s is RadioButton { Tag: string t }) Set(x => x.StartIcon = t); }
    private void OnStatusStyleChecked(object s, RoutedEventArgs e) { if (s is RadioButton { Tag: string t }) Set(x => x.StatusStyle = t); }
    private void OnBarEdgeChecked(object s, RoutedEventArgs e) { if (s is RadioButton { Tag: string t }) Set(x => x.BarEdge = t); }
    private void OnTextColorChecked(object s, RoutedEventArgs e) { if (s is RadioButton { Tag: string t }) Set(x => x.TextColor = t); }

    private void Changed()
    {
        _session.MarkEdited();
        UpdateButtons();
        RefreshLoadedPreviews(false);
        SchedulePreview();
    }

    // Title-bar actions appear while there is something to save, and briefly after a save.
    private void UpdateButtons()
    {
        bool dirty = _session.Initialized && _session.HasUnsavedChanges;
        bool canEdit = dirty && _saveTask == null;
        RevertButton.IsEnabled = canEdit; SaveButton.IsEnabled = canEdit && !_session.HasPinOverflow;
        _flashId++;
        DirtyText.Text = Loc.T("Unsaved");
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0xFF, 0x9F, 0x0A));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Visible;
        ShowSaveBar(dirty);
    }

    private void ShowSaveBar(bool on)
    {
        SaveBar.OpacityTransition ??= new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
        SaveBar.Opacity = on ? 1 : 0;
        SaveBar.IsHitTestVisible = on;
    }

    private async void FlashSaved()
    {
        int id = ++_flashId;
        DirtyText.Text = Loc.T("Saved");
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0x30, 0xD1, 0x58));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Collapsed;
        ShowSaveBar(true);
        await Task.Delay(1600);
        if (id == _flashId) ShowSaveBar(false);
    }

    // At most one preview in flight; the newest state is sent when it returns,
    // so dragging a slider never queues up stale frames.
    private async void SchedulePreview()
    {
        if (!_session.Initialized || !_client.Connected || _updatesClosed || _session.HasPinOverflow) return;
        if (_previewBusy) { _previewPending = true; return; }
        _previewBusy = true;
        do
        {
            _previewPending = false;
            if (!_session.Initialized || _session.HasPinOverflow) break;
            await _client.PreviewAsync(CurrentSettings.Clone());
            await Task.Delay(16);
        } while (_previewPending && _session.Initialized && !_updatesClosed && !_session.HasPinOverflow);
        _previewBusy = false;
        if (!_updatesClosed && !_client.Connected) ShowStatus(false);
    }

    private async void OnSave(object sender, RoutedEventArgs e) { if (_session.Initialized && _saveTask == null) await SaveAsync(); }

    private async Task<bool> SaveAsync()
    {
        if (!_session.Initialized) return false;
        if (_saveTask != null)
        {
            if (!await _saveTask) return false;
            if (!_session.HasUnsavedChanges) return true;
        }
        var task = SaveCoreAsync();
        _saveTask = task;
        UpdateButtons();
        try { return await task; }
        finally
        {
            if (ReferenceEquals(_saveTask, task)) _saveTask = null;
            if (!_updatesClosed && !_session.Closed)
            {
                RevertButton.IsEnabled = _session.Initialized && _session.HasUnsavedChanges;
                SaveButton.IsEnabled = _session.Initialized && _session.HasUnsavedChanges && !_session.HasPinOverflow;
            }
        }
    }

    private async Task<bool> SaveCoreAsync()
    {
        if (_session.HasPinOverflow) { ShowPinOverflow(); return false; }
        // What is saved is this snapshot: edits made while it is on its way stay unsaved.
        var sent = _session.Draft.Clone();
        bool languageChanged = sent.Language != _session.Saved.Language;
        var result = await _client.CommitAsync(sent.Clone(), _session.InstanceId, _session.Revision);
        if (_updatesClosed || _session.Closed) return false;
        if (result.IsConflict && result.ServerSaved != null)
        {
            _session.ApplyConflict(result.ServerSaved, result.Revision, result.InstanceId);
            LoadIntoControls();
            UpdateButtons();
            if (_session.HasPinOverflow) { ShowPinOverflow(); ShowStatus(_client.Connected); return false; }
            if (_session.HasUnsavedChanges)
            {
                SchedulePreview();
                ShowMessage(InfoBarSeverity.Warning, Loc.T("Settings changed elsewhere"),
                    Loc.T("The core settings changed before saving. Your unsubmitted edits were kept; review them and click Save again."));
                ShowStatus(_client.Connected);
                return false;
            }
        }
        else if (!result.Succeeded) { ShowMessage(InfoBarSeverity.Error, Loc.T("Could not save"), result.Error ?? "error"); return false; }
        else { _session.ApplyCommitSuccess(sent, result.InstanceId, result.Revision); }
        // The settings window's XAML is Chinese source text: re-create it in
        // the new language, carrying edits made while the save was in flight.
        if (languageChanged)
        {
            Loc.Apply(sent.Language);
            App.ReloadWindow(7, _session.Draft, _session.Saved);
            return true;
        }
        Status.IsOpen = false;
        UpdateButtons();
        if (result.Warning is { } warning)
            ShowMessage(InfoBarSeverity.Warning, Loc.T("Settings saved"), warning);
        else if (!_session.HasUnsavedChanges) FlashSaved(); // edits made meanwhile keep the capsule as it is
        ShowStatus(_client.Connected);
        return true;
    }

    private async void OnRevert(object sender, RoutedEventArgs e)
    {
        if (!_session.Initialized) return;
        _session.Revert(); Status.IsOpen = false;
        LoadIntoControls(); UpdateButtons();
        await _client.CancelAsync();
    }

    // A group's "restore defaults": its parameters (and what goes with them) back to the defaults.
    private void OnGroupReset(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string group }) return;
        var d = BarSettings.Liquid();
        foreach (var (def, _) in _rows)
            if (def.Group == group) def.Set(CurrentSettings, def.Get(d));
        if (group == "clarity") CurrentSettings.Tint = d.Tint;
        if (group == "width") CurrentSettings.AutoWidth = d.AutoWidth;
        if (group == "size")
        {
            CurrentSettings.HoverRise = d.HoverRise;
            CurrentSettings.RiseNear = d.RiseNear;
            CurrentSettings.MagnifyNear = d.MagnifyNear;
        }
        if (group == "notify")
        {
            CurrentSettings.NotifyTimeout = d.NotifyTimeout;
            CurrentSettings.NotifyAlways = d.NotifyAlways;
            CurrentSettings.NotifyHideBody = d.NotifyHideBody;
            CurrentSettings.NotifyFullscreen = d.NotifyFullscreen;
            CurrentSettings.NotifyPlacement = d.NotifyPlacement;
        }
        if (group is "clarity" or "edge" or "light")
        {
            CurrentSettings.Preset = CurrentSettings.SameMaterial(BarSettings.Liquid()) ? "liquid"
                : CurrentSettings.SameMaterial(BarSettings.Frosted()) ? "frosted" : "custom";
        }
        LoadIntoControls();
        Changed();
    }

    // The material back to the Liquid preset (layout, apps and sources stay as they are).
    private void OnDefaults(object sender, RoutedEventArgs e)
    {
        CurrentSettings.ApplyMaterial(BarSettings.Liquid());
        LoadIntoControls();
        Changed();
    }
}
