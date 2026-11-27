using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.UI;

namespace Atrium.Settings;

public sealed partial class MainWindow : Window, IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorRect { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo { public int Size; public MonitorRect Bounds, Work; public uint Flags; }

    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromRect(IntPtr rect, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW")] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool SetProp(IntPtr hwnd, string name, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr RemoveProp(IntPtr hwnd, string name);
    private delegate IntPtr SubclassProc(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam, UIntPtr id, UIntPtr data);
    [DllImport("comctl32.dll")] private static extern bool SetWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id, UIntPtr data);
    [DllImport("comctl32.dll")] private static extern bool RemoveWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id);
    [DllImport("comctl32.dll")] private static extern IntPtr DefSubclassProc(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam);
    private readonly SubclassProc? _updateMessage;
    // The Material page's sliders and their SliderDef record: MainWindow.Sliders.cs.

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
            TitleLogo.Source = AboutLogo.Source = logo;
        });
        PopulateLanguagePicker();
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
        LoupeSwitch.IsOn = true;
        BuildRows();
        BuildBackupSection();
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
        PathText.Text = CoreClient.SettingsPath;
        StartupSwitch.IsEnabled = !CoreClient.Development;
        if (CoreClient.Development) ToolTipService.SetToolTip(StartupSwitch, Loc.T("Development builds do not change Windows startup."));
        var v = typeof(MainWindow).Assembly.GetName().Version;
        VersionText.Text = Loc.F("Version {0}", v?.ToString(3) ?? "1.0.0");
        ShowStatus(false);
        // `--page widgets` (the widgets' menu): open on that page.
        int page = startPage >= 0 ? startPage : App.PageFromArgs(Environment.GetCommandLineArgs()) ?? 0;
        SelectPage(page);
        _ = ConnectAndLoadAsync();
        if (!Environment.GetCommandLineArgs().Contains("--development")) _ = CheckForUpdatesAsync();
    }

    private void UpdateWindowMinimumSize(uint dpi = 0, IntPtr suggestedRect = 0)
    {
        if (AppWindow.Presenter is not OverlappedPresenter presenter) return;
        if (dpi == 0) dpi = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this));
        double scale = (dpi > 0 ? dpi : 96) / 96.0;
        int minWidth = (int)(980 * scale), minHeight = (int)(680 * scale);
        int workW = 0, workH = 0;
        if (suggestedRect != IntPtr.Zero)
        {
            nint monitor = MonitorFromRect(suggestedRect, 2 /* MONITOR_DEFAULTTONEAREST */);
            var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
            if (monitor != IntPtr.Zero && GetMonitorInfo(monitor, ref info))
            {
                workW = info.Work.Right - info.Work.Left;
                workH = info.Work.Bottom - info.Work.Top;
            }
        }
        else if (DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary) is { } area)
        {
            workW = area.WorkArea.Width;
            workH = area.WorkArea.Height;
        }
        if (workW > 0 && workH > 0)
        {
            minWidth = Math.Min(minWidth, workW);
            minHeight = Math.Min(minHeight, workH);
        }
        presenter.PreferredMinimumWidth = minWidth;
        presenter.PreferredMinimumHeight = minHeight;
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

    // ---- Connection ----

    private async Task ConnectAndLoadAsync()
    {
        try { await LoadFromCoreAsync(); }
        finally { FinishCoreChange(); }
    }

    private async Task LoadFromCoreAsync()
    {
        long draftBefore = _session.DraftVersion;
        await _client.ConnectAsync(800);
        var state = await _client.GetAsync();
        if (_updatesClosed || _session.Closed) return;
        var pending = App.TakePendingState();
        if (pending.Current != null && pending.Saved != null)
            _session.ApplyInitialState(state.Saved, state.Current, state.InstanceId, state.Revision, pending.Saved, pending.Current);
        else if (draftBefore != _session.DraftVersion)
            _session.ApplyReconnectSnapshot(state.Saved, state.Current, state.InstanceId, state.Revision, draftBefore);
        else
            _session.ApplyInitialState(state.Saved, state.Current, state.InstanceId, state.Revision);
        _coreChanging = false;
        WatchCore();
        WatchSettingsFile();
        ShowStatus(_client.Connected);
        LoadIntoControls();
        UpdateButtons();
        if (_session.HasUnsavedChanges && !_session.HasPinOverflow) SchedulePreview();
        if (Content is FrameworkElement { IsLoaded: true }) App.SignalStartupReady(_client.Connected && _client.StateConfirmed);
        else if (Content is FrameworkElement root) root.Loaded += (_, _) => App.SignalStartupReady(_client.Connected && _client.StateConfirmed);
        if (_reconnectTimer == null && !_updatesClosed)
        {
            _reconnectTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            _reconnectTimer.Tick += async (_, _) => await ReconnectAsync();
            _reconnectTimer.Start();
        }
    }

    private async Task SyncConnectedStateAsync()
    {
        long draftBefore = _session.DraftVersion;
        var state = await _client.GetAsync();
        if (_updatesClosed || _session.Closed || !state.FromCore) return;
        bool preview = _session.ApplyReconnectSnapshot(state.Saved, state.Current, state.InstanceId, state.Revision, draftBefore);
        LoadIntoControls();
        UpdateButtons();
        if (preview && !_session.HasPinOverflow) SchedulePreview();
    }

    private void FinishCoreChange(bool watch = false)
    {
        _coreChanging = false;
        if (_updatesClosed) return;
        if (watch) WatchCore();
        ShowStatus(_client.Connected);
        if (_closeAfterCore) { _closeAfterCore = false; _ = CloseRequestedAsync(); }
    }

    // The core started again on its own (an update, the guard after a crash, a launch from
    // elsewhere): this window connects to it by itself. Edits made meanwhile stay and are previewed.
    private DispatcherTimer? _reconnectTimer;
    private bool _reconnecting;

    private async Task ReconnectAsync()
    {
        if (_reconnecting || _coreChanging || _updatesClosed || _client.Connected) return;
        _reconnecting = true;
        try
        {
            if (!await _client.ConnectAsync(300) || _updatesClosed)
            {
                if (!_updatesClosed && !_coreChanging) ShowStatus(_client.Connected);
                return;
            }
            await SyncConnectedStateAsync();
            if (_updatesClosed) return;
            WatchCore();
            ShowStatus(_client.Connected);
        }
        finally { _reconnecting = false; }
    }

    private static readonly SolidColorBrush RunningBrush = new(Color.FromArgb(255, 76, 194, 106));
    private static readonly SolidColorBrush IdleBrush = new(Color.FromArgb(255, 150, 150, 156));

    private void ShowStatus(bool connected)
    {
        StatusDot.Fill = connected ? RunningBrush : IdleBrush;
        _updatingCoreSwitch = true;
        CoreSwitch.IsOn = connected || (_coreProcess != null && !_coreProcess.HasExited);
        _updatingCoreSwitch = false;
        CoreSwitch.IsEnabled = !_coreChanging && CoreClient.CoreExe() != null;
        if (!_coreChanging)
        {
            StatusMessage.Text = connected ? Loc.T("Running") : _client.ConnectionError ??
                (_coreProcess != null && !_coreProcess.HasExited
                    ? Loc.T("The core is still running; the settings connection closed")
                    : Loc.T("Closed; Windows' taskbar is back"));
        }
    }

    // The banner itself lives in MainWindow.Status.cs: the project's card material, closing itself.

    // WinUI shows one dialog at a time and none on a window that is closing: then the answer is None.
    private async Task<ContentDialogResult> AskAsync(ContentDialog dialog)
    {
        if (_dialog != null || Content.XamlRoot == null) return ContentDialogResult.None;
        dialog.Style = (Style)Application.Current.Resources["AtriumDialogStyle"];
        dialog.XamlRoot = Content.XamlRoot;
        _dialog = dialog;
        try { return await dialog.ShowAsync(); }
        finally { _dialog = null; }
    }

    // An app (.exe or .lnk) the user picks; null when cancelled. Windows shows no picker to an
    // unpackaged app run as administrator.
    private async Task<StorageFile?> PickProgramAsync()
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.ComputerFolder };
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        picker.FileTypeFilter.Add(".exe");
        picker.FileTypeFilter.Add(".lnk");
        try { return await picker.PickSingleFileAsync(); }
        catch (COMException)
        {
            ShowMessage(InfoBarSeverity.Warning, Loc.T("The file picker is unavailable"),
                Loc.T("Windows does not show it to apps run as administrator. Open the settings without administrator rights."));
            return null;
        }
    }

    // Follow the process exit event rather than polling the pipe while the settings window is idle.
    private void WatchCore()
    {
        if (_updatesClosed) return;
        if (!_client.Connected && _coreProcess != null && !_coreProcess.HasExited) return;
        _coreProcess?.Dispose();
        _coreProcess = null;
        if (!_client.Connected || _updatesClosed) return;
        try
        {
            var process = Process.GetProcessById(_client.ProcessId);
            _coreProcess = process;
            process.Exited += (_, _) => DispatcherQueue.TryEnqueue(() =>
            {
                if (_coreProcess != process) return;
                _client.Disconnect(coreStopped: true);
                _coreProcess.Dispose();
                _coreProcess = null;
                ShowStatus(false);
            });
            process.EnableRaisingEvents = true;
        }
        catch (ArgumentException) { _client.Disconnect(coreStopped: true); }
    }

    private async void OnCoreToggled(object sender, RoutedEventArgs e)
    {
        if (_updatingCoreSwitch || _coreChanging) return;
        bool start = CoreSwitch.IsOn;
        _coreChanging = true;
        CoreSwitch.IsEnabled = false;
        StatusMessage.Text = start ? Loc.T("Starting…") : Loc.T("Closing…");
        HideStatus();
        try
        {
            string exe = CoreClient.CoreExe() ?? throw new FileNotFoundException(Loc.T("Atrium.exe was not found"));
            if (start)
            {
                using var launched = Process.Start(CoreClient.CoreStartInfo(exe));
                if (!await _client.ConnectAsync(8000)) throw new TimeoutException(Loc.T("Could not connect to Atrium. Try again later."));
                await SyncConnectedStateAsync();
            }
            else
            {
                _client.Disconnect();
                WatchCore();
                using var quit = Process.Start(new ProcessStartInfo(exe, "--exit") { UseShellExecute = false, CreateNoWindow = true })
                    ?? throw new InvalidOperationException(Loc.T("Could not send the quit request."));
                using var waitTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
                try { await quit.WaitForExitAsync(waitTimeout.Token); }
                catch (OperationCanceledException) { throw new TimeoutException(Loc.T("Atrium has not quit yet. Try again later.")); }
                if (quit.ExitCode != 0) throw new InvalidOperationException(Loc.T("Atrium has not quit yet. Try again later."));
                _client.Disconnect(coreStopped: true);
            }
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or TimeoutException or IOException)
        {
            ShowMessage(InfoBarSeverity.Error, start ? Loc.T("Could not start") : Loc.T("Could not close"), error.Message);
        }
        finally { FinishCoreChange(watch: true); }
    }

    // ---- Changes made elsewhere ----

    // The core writes settings.json for changes made at the bar (its menu, the control centre):
    // what changed there comes into the controls, the edits made here meanwhile stay.
    private FileSystemWatcher? _settingsWatcher;
    private int _settingsReload;

    private void WatchSettingsFile()
    {
        string? dir = Path.GetDirectoryName(CoreClient.SettingsPath);
        if (_settingsWatcher != null || dir == null || !Directory.Exists(dir)) return;
        _settingsWatcher = new FileSystemWatcher(dir, "settings.json")
        {
            NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size,
            EnableRaisingEvents = true,
        };
        void OnChanged(object _1, FileSystemEventArgs _2) => DispatcherQueue.TryEnqueue(ReloadExternal);
        _settingsWatcher.Changed += OnChanged;
        _settingsWatcher.Created += OnChanged;
        _settingsWatcher.Renamed += (_, _) => DispatcherQueue.TryEnqueue(ReloadExternal);
    }

    private async void ReloadExternal()
    {
        int id = ++_settingsReload;
        await Task.Delay(120); // one reload for a burst of writes
        if (id != _settingsReload || _updatesClosed || _session.Closed || !_client.Connected) return;
        var state = await _client.GetAsync();
        if (id != _settingsReload || _updatesClosed || _session.Closed || !state.FromCore) return;
        if (!_session.ApplyExternalChange(state.Saved, state.Revision)) return;
        LoadIntoControls();
        UpdateButtons();
        if (_page == 4) ShowWidgetMap();
        if (_page == 5) ShowStagePreview(true);
        if (_session.HasUnsavedChanges && !_session.HasPinOverflow) SchedulePreview(); // the edits made here, over the new state
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
        WidgetsSwitch.IsOn = CurrentSettings.Widgets;
        (CurrentSettings.WidgetsOnRight ? WidgetsRight : WidgetsLeft).IsChecked = true;
        HideTaskbarSwitch.IsOn = CurrentSettings.HideTaskbar;
        GenieSwitch.IsOn = CurrentSettings.GenieMinimize;
        PeekSwitch.IsOn = CurrentSettings.WindowPeek;
        StageSwitch.IsOn = CurrentSettings.StageManager;
        (CurrentSettings.StageOnRight ? StageRight : StageLeft).IsChecked = true;
        (CurrentSettings.StageAutoHide ? StageAutoHide : StageAlways).IsChecked = true;
        StageMinimizeSwitch.IsOn = CurrentSettings.StageMinimize;
        StageDragMergeSwitch.IsOn = CurrentSettings.StageDragMerge;
        StageCloseButtonSwitch.IsOn = CurrentSettings.StageCloseButton;
        StageScrollWrapSwitch.IsOn = CurrentSettings.StageScrollWrap;
        StageAvoidSwitch.IsOn = CurrentSettings.StageAvoid;
        RebuildExcluded();
        (CurrentSettings.WidgetBackdrop == "wallpaper" ? BackdropWallpaper : BackdropDesktop).IsChecked = true;
        (CurrentSettings.WidgetMaterial switch { "dusk" => MaterialDusk, "color" => MaterialColor, _ => MaterialGlass }).IsChecked = true;
        HideBannersSwitch.IsOn = CurrentSettings.HideBanners;
        WinKeySwitch.IsOn = CurrentSettings.WinKeyStart;
        WinNumbersSwitch.IsOn = CurrentSettings.WinNumbers;
        RemoteVisibleSwitch.IsOn = CurrentSettings.RemoteVisible;
        (CurrentSettings.StatusStyle == "icons" ? StatusIcons : StatusRing).IsChecked = true;
        (CurrentSettings.AutoHide switch { "never" => HideNever, "always" => HideAlways, _ => HideFullscreen }).IsChecked = true;
        BarReserveSwitch.IsOn = CurrentSettings.BarReserve;
        BarFullscreenHideSwitch.IsOn = CurrentSettings.BarFullscreenHide;
        VisibleOptions.Visibility = CurrentSettings.AutoHide == "never" ? Visibility.Visible : Visibility.Collapsed;
        (CurrentSettings.BarEdge switch { "top" => EdgeTop, "left" => EdgeLeft, "right" => EdgeRight, _ => EdgeBottom }).IsChecked = true;
        StartupSwitch.IsOn = CurrentSettings.StartOnLogin;
        string currentLanguage = CurrentSettings.Language == "zh" ? "zh-Hans" : CurrentSettings.Language;
        LanguagePicker.SelectedIndex = 0;
        for (int i = 0; i < LanguagePicker.Items.Count; i++)
        {
            if (LanguagePicker.Items[i] is ComboBoxItem item && (item.Tag as string) == currentLanguage)
                LanguagePicker.SelectedIndex = i;
        }

        LoadDisplayControls();
        (CurrentSettings.TextColor switch { "light" => TextLight, "dark" => TextDark, _ => TextAuto }).IsChecked = true;
        UpdateGallery();
        RebuildPins();
        WindowsContextMenuSwitch.IsOn = CurrentSettings.WindowsContextMenu;
        TintPicker.Color = TintColor(CurrentSettings.Tint);
        ShowTint();
        _loading = false;
        RefreshLoadedPreviews(true);
    }

    private static Color TintColor(uint rgb) => Color.FromArgb(255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

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

    private void OnClockSecondsToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.ShowClockSeconds = ClockSecondsSwitch.IsOn;
        Changed();
    }

    private void OnBarContentToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.ShowTray = TraySwitch.IsOn;
        CurrentSettings.TrayHost = TrayHostSwitch.IsOn;
        CurrentSettings.StartTray = StartTraySwitch.IsOn;
        CurrentSettings.StartStatus = StartStatusSwitch.IsOn;
        CurrentSettings.ShowStatus = StatusSwitch.IsOn;
        CurrentSettings.ShowNotifications = NotificationsSwitch.IsOn;
        CurrentSettings.ShowDesktop = DesktopSwitch.IsOn;
        CurrentSettings.StartCentered = StartCenteredSwitch.IsOn;
        CurrentSettings.StartWithBar = StartWithBarSwitch.IsOn;
        CurrentSettings.AttentionLight = AttentionLightSwitch.IsOn;
        CurrentSettings.AttentionBounce = AttentionBounceSwitch.IsOn;
        CurrentSettings.BarBadges = BarBadgesSwitch.IsOn;
        Changed();
    }

    private void OnWidgetAlignmentChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.WidgetsOnRight = tag == "right";
        // With Stage Manager on, it keeps to the widgets' other side.
        if (CurrentSettings.StageManager) SetSide(v => CurrentSettings.StageOnRight = v, StageLeft, StageRight, !CurrentSettings.WidgetsOnRight);
        ShowWidgetMap();
        Changed();
    }

    private void SetSide(Action<bool> set, RadioButton left, RadioButton right, bool isRight)
    {
        set(isRight);
        _loading = true;
        (isRight ? right : left).IsChecked = true;
        _loading = false;
    }

    private void OnAutoWidthToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.AutoWidth = AutoWidthSwitch.IsOn;
        Changed();
    }

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

    private void OnRiseNearToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.RiseNear = RiseNearSwitch.IsOn;
        Changed();
    }

    private void OnMagnifyNearToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.MagnifyNear = MagnifyNearSwitch.IsOn;
        Changed();
    }

    private void OnRunningToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.ShowRunning = RunningSwitch.IsOn;
        Changed();
    }

    private void OnImeStatusToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.ImeStatus = ImeStatusSwitch.IsOn;
        ImeBubbleSwitch.IsEnabled = ImeStatusSwitch.IsOn; // the bubble announces what the status reads
        Changed();
    }

    private void OnImeBubbleToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.ImeBubble = ImeBubbleSwitch.IsOn;
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

    private void OnWidgetsToggled(object sender, RoutedEventArgs e)
    {
        UpdateGallery();
        if (_loading) return;
        CurrentSettings.Widgets = WidgetsSwitch.IsOn;
        Changed();
    }

    // The widgets' material: the map and the gallery show it at once, the core with the preview.
    private void OnWidgetMaterialChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag } || tag == CurrentSettings.WidgetMaterial) return;
        CurrentSettings.WidgetMaterial = tag;
        Changed();
        if (_page == 4) { ShowWidgetMap(); RefreshGallery(); }
    }

    private void OnWidgetBackdropChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.WidgetBackdrop = tag;
        Changed();
    }

    private void OnGenieToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.GenieMinimize = GenieSwitch.IsOn;
        Changed();
    }

    private void OnPeekToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.WindowPeek = PeekSwitch.IsOn;
        Changed();
    }

    private void OnHideTaskbarToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.HideTaskbar = HideTaskbarSwitch.IsOn;
        Changed();
    }

    private void OnStageToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.StageManager = StageSwitch.IsOn;
        // The widgets go to the other side.
        if (CurrentSettings.StageManager) SetSide(v => CurrentSettings.WidgetsOnRight = v, WidgetsLeft, WidgetsRight, !CurrentSettings.StageOnRight);
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }

    private void OnStageSideChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.StageOnRight = tag == "right";
        if (CurrentSettings.StageManager) SetSide(v => CurrentSettings.WidgetsOnRight = v, WidgetsLeft, WidgetsRight, !CurrentSettings.StageOnRight);
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }

    private void OnStageModeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.StageAutoHide = tag == "hover";
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }

    private void OnStartupToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.StartOnLogin = StartupSwitch.IsOn;
        Changed();
    }

    // The picker follows locales/manifest.json: adding a language file and a
    // manifest entry is enough; no code change here.
    private void PopulateLanguagePicker()
    {
        bool loading = _loading;
        _loading = true;
        LanguagePicker.Items.Clear();
        LanguagePicker.Items.Add(new ComboBoxItem { Content = Loc.T("Follow system"), Tag = "auto" });
        foreach (var (tag, name) in Loc.Languages)
            LanguagePicker.Items.Add(new ComboBoxItem { Content = name, Tag = tag });
        _loading = loading;
    }

    private void OnLanguageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.Language = (LanguagePicker.SelectedItem as ComboBoxItem)?.Tag as string ?? "auto";
        Changed();
    }

    private void OnHideBannersToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.HideBanners = HideBannersSwitch.IsOn;
        Changed();
    }

    private void OnStageAvoidToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.StageAvoid = StageAvoidSwitch.IsOn;
        Changed();
    }

    private void OnStageCloseButtonToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.StageCloseButton = StageCloseButtonSwitch.IsOn;
        Changed();
    }

    private void OnStageDragMergeToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.StageDragMerge = StageDragMergeSwitch.IsOn;
        Changed();
    }

    private void OnStageMinimizeToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.StageMinimize = StageMinimizeSwitch.IsOn;
        Changed();
    }

    private void OnWinKeyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.WinKeyStart = WinKeySwitch.IsOn;
        Changed();
    }

    private void OnStartSizeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.StartSize = tag;
        Changed();
    }

    private void OnStartIconSizeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.StartIcon = tag;
        Changed();
    }

    private void OnWinNumbersToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.WinNumbers = WinNumbersSwitch.IsOn;
        Changed();
    }

    private void OnRemoteVisibleToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.RemoteVisible = RemoteVisibleSwitch.IsOn;
        Changed();
    }

    private void OnStatusStyleChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.StatusStyle = tag;
        Changed();
    }

    private void OnAutoHideChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.AutoHide = tag;
        VisibleOptions.Visibility = tag == "never" ? Visibility.Visible : Visibility.Collapsed;
        Changed();
    }

    private void OnBarReserveToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.BarReserve = BarReserveSwitch.IsOn;
        Changed();
    }

    private void OnBarFullscreenHideToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.BarFullscreenHide = BarFullscreenHideSwitch.IsOn;
        Changed();
    }

    private void OnBarEdgeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.BarEdge = tag;
        Changed();
    }

    private void OnTextColorChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        CurrentSettings.TextColor = tag;
        Changed();
    }

    private void OnNotifyPlacementChecked(object sender, RoutedEventArgs e)
    {
        bool corner = (string)((RadioButton)sender).Tag == "corner";
        NotifyCornerRow.Visibility = corner ? Visibility.Visible : Visibility.Collapsed;
        if (_loading) return;
        // A corner: the one chosen last (top right at first, as before there were four).
        CurrentSettings.NotifyPlacement = corner ? _notifyCorner : "dock";
        _notifyEntrancePending = true;
        Changed();
    }

    private void OnNotifyCornerChecked(object sender, RoutedEventArgs e)
    {
        _notifyCorner = (string)((RadioButton)sender).Tag;
        if (_loading || CurrentSettings.NotifyPlacement == _notifyCorner) return;
        CurrentSettings.NotifyPlacement = _notifyCorner;
        _notifyEntrancePending = true;
        Changed();
    }

    private string _notifyCorner = "topRight";

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
        HideStatus();
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
        _session.Revert(); HideStatus();
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

    private async void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        args.Cancel = true;
        await CloseRequestedAsync();
    }

    // Window.Close does not raise AppWindow.Closing. Both the title-bar close
    // and update button explicitly use the same save/exit path before calling it.
    private async Task CloseRequestedAsync()
    {
        if (_closePending) return;
        if (_coreChanging) { _closeAfterCore = true; return; }
        if (_dialog != null) // a close while a dialog is up dismisses the dialog
        {
            _dialog.Hide();
            return;
        }
        _closePending = true;
        try
        {
            if (_session.HasUnsavedChanges)
            {
                var dialog = new ContentDialog
                {
                    Title = Loc.T("Save changes?"),
                    Content = Loc.T("Unsaved changes will be discarded when this window closes."),
                    PrimaryButtonText = Loc.T("Save"),
                    SecondaryButtonText = Loc.T("Don't save"),
                    CloseButtonText = Loc.T("Cancel"),
                    DefaultButton = ContentDialogButton.Primary,
                };
                var result = await AskAsync(dialog);
                if (result == ContentDialogResult.None) { _restartUpdate = null; return; }
                if (result == ContentDialogResult.Primary && !await SaveAsync()) { _restartUpdate = null; return; }
            }
            if (_restartUpdate != null)
            {
                try { ReleaseInstaller.Restart(_restartUpdate, _client.Connected); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
                {
                    _restartUpdate = null;
                    UpdateStatus.Text = error is UnauthorizedAccessException
                        ? Loc.T("The installation folder is not writable; the update was not installed")
                        : Loc.F("Could not restart for the update: {0}", error.Message);
                    return;
                }
            }
            _client.Dispose(); // the core reverts any unsaved preview on disconnect
            Close();
        }
        finally { _closePending = false; }
    }
}
