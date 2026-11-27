using System.Diagnostics;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Atrium.Settings;

// Administrator privileges card in Settings -> System, core connection/lifecycle, and external settings sync.
public sealed partial class MainWindow
{
    private TextBlock? _adminStatus, _adminNote;
    private Button? _adminButton;
    private bool _adminBuilt, _coreExitDenied, _systemClosedHooked;
    private CoreClient.NativeProcessWaitHandle? _coreProcessHandle;
    private RegisteredWaitHandle? _coreExitWait;
    private CancellationTokenSource? _lifecycleCts;

    private void HookSystemClosed()
    {
        if (_systemClosedHooked) return;
        _systemClosedHooked = true;
        Closed += (_, _) =>
        {
            _lifecycleCts?.Cancel();
            _lifecycleCts?.Dispose();
            _lifecycleCts = null;
            DetachCoreProcess();
        };
    }

    private void EnsureAdminCard()
    {
        HookSystemClosed();
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
            _adminNote = new TextBlock
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
            _adminButton.Click += OnAdminActionClick;

            var left = new StackPanel { Spacing = 4, Children = { title, _adminNote, _adminStatus } };
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
        _coreExitWait?.Unregister(null);
        _coreExitWait = null;
        _coreProcessHandle?.Dispose();
        _coreProcessHandle = null;
        _coreExitDenied = false;
    }

    private CorePrivilegeInfo CurrentPrivilegeInfo() =>
        new(_client.CoreElevated, _client.PrivilegeMode, _client.CanDeElevate);

    private void RefreshAdminCard()
    {
        if (_adminStatus == null || _adminNote == null || _adminButton == null) return;
        var model = CoreClient.GetAdminCardState(_client.Connected, CurrentPrivilegeInfo(), CoreClient.CoreExe() != null, _coreChanging);
        _adminStatus.Text = model.Status;
        _adminNote.Text = model.Note;
        _adminButton.Visibility = model.ButtonVisible ? Visibility.Visible : Visibility.Collapsed;
        _adminButton.Content = model.ButtonText ?? string.Empty;
        _adminButton.IsEnabled = model.ButtonEnabled;
    }

    private async Task<bool> WaitForCoreExitAsync(string exe, bool sentPipeQuit, int oldPid)
    {
        if (sentPipeQuit && _coreProcessHandle != null)
        {
            if (await CoreClient.WaitForProcessExitAsync(_coreProcessHandle, 6000)) return true;
        }
        else if (sentPipeQuit && _coreExitDenied && oldPid > 0)
        {
            for (int i = 0; i < 40; i++)
            {
                int? activePid = await _client.PingLifecyclePidAsync(300);
                if (activePid == null || activePid != oldPid) return true;
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

    private async void OnAdminActionClick(object sender, RoutedEventArgs e)
    {
        var model = CoreClient.GetAdminCardState(_client.Connected, CurrentPrivilegeInfo(), CoreClient.CoreExe() != null, _coreChanging);
        if (model.Action is AdminCardAction.StartAsAdmin or AdminCardAction.RestartAsAdmin)
            await RestartAsAdminUiAsync();
        else if (model.Action == AdminCardAction.RestartAsStandard)
            await RestartAsStandardUiAsync();
    }

    private async Task RestartAsStandardUiAsync()
    {
        if (_coreChanging || !_client.Connected || !_client.CoreElevated || !_client.CanDeElevate) return;
        if (_session.HasUnsavedChanges && !await SaveAsync()) return;

        HookSystemClosed();
        _coreChanging = true;
        RefreshAdminCard();
        CoreSwitch.IsEnabled = false;
        Status.IsOpen = false;
        _lifecycleCts?.Cancel();
        _lifecycleCts?.Dispose();
        _lifecycleCts = new CancellationTokenSource();
        try
        {
            var result = await _client.RestartAsStandardAsync(_lifecycleCts.Token);
            if (_updatesClosed) return;
            if (result.Reconnected) await SyncConnectedStateAsync();
            if (result.Succeeded)
            {
                WatchCore();
                ShowMessage(InfoBarSeverity.Success, Loc.T("Standard user mode active"),
                    Loc.T("Atrium is now running as a standard user."));
            }
            else if (result.Error != null)
            {
                ShowMessage(result.Reconnected ? InfoBarSeverity.Warning : InfoBarSeverity.Error,
                    Loc.T("Could not restart as standard user"), result.Error);
            }
        }
        finally
        {
            FinishCoreChange(watch: true);
            RefreshAdminCard();
        }
    }

    private async Task RestartAsAdminUiAsync()
    {
        if (_coreChanging || (_client.Connected && _client.CoreElevated)) return;
        string? exe = CoreClient.CoreExe();
        if (exe == null) return;
        if (_session.HasUnsavedChanges && !await SaveAsync()) return;

        _coreChanging = true;
        RefreshAdminCard();
        CoreSwitch.IsEnabled = false;
        Status.IsOpen = false;
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
        bool alive = CoreClient.IsProcessRunning(_coreProcessHandle);
        _updatingCoreSwitch = true;
        CoreSwitch.IsOn = connected || alive;
        _updatingCoreSwitch = false;
        CoreSwitch.IsEnabled = !_coreChanging && CoreClient.CoreExe() != null;
        if (!_coreChanging)
        {
            StatusMessage.Text = connected ? Loc.T("Running") : _client.ConnectionError ??
                (alive
                    ? Loc.T("The core is still running; the settings connection closed")
                    : Loc.T("Closed; Windows' taskbar is back"));
        }
        RefreshAdminCard();
    }

    private void ShowMessage(InfoBarSeverity severity, string title, string message)
    {
        Status.Severity = severity;
        Status.Title = title;
        Status.Message = message;
        Status.IsOpen = true;
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

    // An app (.exe or .lnk) the user picks; null when cancelled.
    private Task<string?> PickProgramAsync() =>
        Task.FromResult(NativeFileDialog.PickProgram(WinRT.Interop.WindowNative.GetWindowHandle(this), Loc.T("Apps"), App.LogCrash));

    // Follow the process exit event rather than polling the pipe while the settings window is idle.
    private void WatchCore()
    {
        HookSystemClosed();
        if (_updatesClosed) return;
        if (!_client.Connected && CoreClient.IsProcessRunning(_coreProcessHandle)) return;
        DetachCoreProcess();
        if (!_client.Connected || _updatesClosed) return;
        var handle = CoreClient.TryOpenProcessHandle(_client.ProcessId, out bool accessDenied);
        if (handle == null)
        {
            if (accessDenied) { _coreExitDenied = true; return; }
            _client.Disconnect(coreStopped: true);
            return;
        }
        _coreProcessHandle = handle;
        _coreExitWait = ThreadPool.RegisterWaitForSingleObject(handle, (_, _) => DispatcherQueue.TryEnqueue(() =>
        {
            if (!ReferenceEquals(_coreProcessHandle, handle)) return;
            _client.Disconnect(coreStopped: true);
            DetachCoreProcess();
            ShowStatus(false);
        }), null, Timeout.Infinite, executeOnlyOnce: true);
    }

    private async void OnCoreToggled(object sender, RoutedEventArgs e)
    {
        if (_updatingCoreSwitch || _coreChanging) return;
        bool start = CoreSwitch.IsOn;
        _coreChanging = true;
        CoreSwitch.IsEnabled = false;
        StatusMessage.Text = start ? Loc.T("Starting…") : Loc.T("Closing…");
        Status.IsOpen = false;
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
                int oldPid = _client.ProcessId;
                bool sentQuit = _client.Connected && await _client.QuitCoreAsync();
                _client.Disconnect();
                WatchCore();
                if (!await WaitForCoreExitAsync(exe, sentQuit, oldPid))
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
