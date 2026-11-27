using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.UI;

namespace Atrium.Settings;

// Administrator privileges card in Settings -> System, core connection/lifecycle, and external settings sync.
public sealed partial class MainWindow
{
    private TextBlock? _adminStatus;
    private Button? _adminButton;
    private bool _adminBuilt;

    private void EnsureAdminCard()
    {
        if (!_adminBuilt)
        {
            _adminBuilt = true;
            var header = new TextBlock
            {
                Text = Loc.T("Administrator privileges"),
                Style = (Style)Application.Current.Resources["SectionStyle"],
            };
            var title = new TextBlock
            {
                Text = Loc.T("Run as administrator"),
                Style = (Style)Application.Current.Resources["RowTitleStyle"],
            };
            var note = new TextBlock
            {
                Text = Loc.T("Manage elevated apps in Stage Manager, previews and tray. Prompts UAC; Explorer cannot drag onto an elevated taskbar."),
                Style = (Style)Application.Current.Resources["RowNoteStyle"],
                TextWrapping = TextWrapping.Wrap,
            };
            _adminStatus = new TextBlock
            {
                Style = (Style)Application.Current.Resources["RowNoteStyle"],
                Margin = new Thickness(0, 4, 0, 0),
            };
            _adminButton = new Button
            {
                VerticalAlignment = VerticalAlignment.Center,
                MinWidth = 140,
            };
            _adminButton.Click += OnRestartAsAdminClick;

            var left = new StackPanel { Spacing = 4, Children = { title, note, _adminStatus } };
            var grid = new Grid { ColumnSpacing = 16 };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(left, 0);
            Grid.SetColumn(_adminButton, 1);
            grid.Children.Add(left);
            grid.Children.Add(_adminButton);

            var card = new Border
            {
                Style = (Style)Application.Current.Resources["TileStyle"],
                Child = grid,
            };
            MainSystem.Sections.Children.Add(new StackPanel { Children = { header, card } });
        }
        RefreshAdminCard();
    }

    private void DetachCoreProcess()
    {
        _coreProcess?.Dispose();
        _coreProcess = null;
    }

    private void RefreshAdminCard()
    {
        if (_adminStatus == null || _adminButton == null) return;
        bool running = _client.Connected;
        bool elevated = running && _client.CoreElevated;
        bool canRun = CoreClient.CoreExe() != null && !_coreChanging;
        _adminStatus.Text = elevated ? Loc.T("Running as administrator")
            : running ? Loc.T("Running as standard user")
            : Loc.T("Atrium is not running");
        _adminButton.Content = elevated ? Loc.T("Running as administrator")
            : running ? Loc.T("Restart as administrator")
            : Loc.T("Start as administrator");
        _adminButton.IsEnabled = canRun && !elevated;
    }

    private async Task<bool> WaitForCoreExitAsync(string exe, bool sentPipeQuit)
    {
        if (sentPipeQuit)
        {
            for (int i = 0; i < 40; i++)
            {
                if (_coreProcess == null || _coreProcess.HasExited) return true;
                await Task.Delay(150);
            }
        }
        using var quit = Process.Start(new ProcessStartInfo(exe, "--exit") { UseShellExecute = false, CreateNoWindow = true })
            ?? throw new InvalidOperationException(Loc.T("Could not send the quit request."));
        using var waitTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        try { await quit.WaitForExitAsync(waitTimeout.Token); }
        catch (OperationCanceledException) { throw new TimeoutException(Loc.T("Atrium has not quit yet. Try again later.")); }
        return quit.ExitCode == 0;
    }

    private async void OnRestartAsAdminClick(object sender, RoutedEventArgs e)
    {
        if (_coreChanging || (_client.Connected && _client.CoreElevated)) return;
        string? exe = CoreClient.CoreExe();
        if (exe == null) return;
        if (_session.HasUnsavedChanges && !await SaveAsync()) return;

        _coreChanging = true;
        RefreshAdminCard();
        CoreSwitch.IsEnabled = false;
        HideStatus();
        string? previousId = _client.InstanceId;
        bool wasConnected = _client.Connected;
        try
        {
            using var launched = Process.Start(CoreClient.CoreStartInfo(exe, runAsAdmin: true, replace: true));
            if (wasConnected) _client.Disconnect();
            bool reconnected = false;
            for (int i = 0; i < 66 && !_updatesClosed; i++)
            {
                if (await _client.ConnectAsync(150))
                {
                    await SyncConnectedStateAsync();
                    if (_client.CoreElevated || (_client.InstanceId != null && _client.InstanceId != previousId))
                    {
                        reconnected = true;
                        break;
                    }
                    _client.Disconnect();
                }
                await Task.Delay(150);
            }
            if (!reconnected)
            {
                ShowMessage(InfoBarSeverity.Warning, Loc.T("Could not start as administrator"),
                    Loc.T("Atrium restarted, but the settings window could not reconnect yet. Check the status indicator."));
            }
            else if (_client.CoreElevated)
            {
                WatchCore();
                ShowMessage(InfoBarSeverity.Success, Loc.T("Administrator mode active"),
                    Loc.T("Atrium is now running with administrator privileges."));
            }
        }
        catch (System.ComponentModel.Win32Exception error) when (error.NativeErrorCode == 1223)
        {
            // UAC cancelled by the user: leave the running instance undisturbed.
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            ShowMessage(InfoBarSeverity.Error, Loc.T("Could not start as administrator"), error.Message);
        }
        finally
        {
            FinishCoreChange(watch: true);
            RefreshAdminCard();
        }
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
        if (!_startupUpdateChecked)
        {
            _startupUpdateChecked = true;
            if (!CoreClient.Development)
            {
                if (_session.Saved.AutoUpdate)
                    _ = CheckForUpdatesAsync(isAuto: true);
                else
                    _ = RefreshDownloadAsync();
            }
        }
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
        RefreshAdminCard();
    }

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
                if (_coreProcess != process)
                    return;
                _client.Disconnect(coreStopped: true);
                _coreProcess.Dispose();
                _coreProcess = null;
                ShowStatus(false);
            });
            process.EnableRaisingEvents = true;
        }
        catch (System.ComponentModel.Win32Exception) { DetachCoreProcess(); }
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
                bool sentQuit = _client.Connected && await _client.QuitCoreAsync();
                _client.Disconnect();
                WatchCore();
                if (!await WaitForCoreExitAsync(exe, sentQuit))
                    throw new InvalidOperationException(Loc.T("Atrium has not quit yet. Try again later."));
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
}
