using System.Net;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI.Xaml;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    // ---- Updates ----

    private ReleaseInfo? _latestRelease;
    private ReleaseInstaller.Prepared? _preparedRelease, _restartUpdate;
    private ReleaseInstaller.TransferStatus? _transferStatus;
    private DispatcherTimer? _downloadTimer;
    private bool _downloadRefreshBusy, _queueingDownload, _updatesClosed, _startupUpdateChecked;

    private void InitUpdates()
    {
        MainAbout.CheckForUpdatesClicked += OnCheckForUpdates;
        MainAbout.InstallReleaseClicked += OnInstallRelease;
        MainAbout.AutoUpdateToggled += OnAutoUpdateToggled;
    }

    private void OnAutoUpdateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.AutoUpdate = MainAbout.AutoUpdateToggle.IsOn;
        Changed();
    }

    private void ShowInstallAction()
    {
        MainAbout.InstallButton.Content = _preparedRelease != null ? Loc.T("Restart and finish update")
            : _transferStatus is { CanRetry: false } || _queueingDownload ? Loc.T("Downloading in background") : Loc.T("Retry download");
        MainAbout.InstallButton.IsEnabled = _updateCheck == null && !_queueingDownload
            && (_preparedRelease != null || _transferStatus?.CanRetry != false);
        MainAbout.InstallButton.Visibility = _preparedRelease != null || _transferStatus != null || _latestRelease?.Download != null
            && _latestRelease.Version > typeof(MainWindow).Assembly.GetName().Version!
            ? Visibility.Visible : Visibility.Collapsed;
    }

    private void ObserveDownload()
    {
        if (_downloadTimer == null)
        {
            _downloadTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _downloadTimer.Tick += async (_, _) => await RefreshDownloadAsync();
        }
        _downloadTimer.Start();
    }

    private async Task RefreshDownloadAsync()
    {
        if (_downloadRefreshBusy || _updatesClosed) return;
        _downloadRefreshBusy = true;
        try
        {
            var state = await Task.Run(() => (Transfer: ReleaseInstaller.ReadTransferStatus(),
                Ready: ReleaseInstaller.ReadPreparedRecovering(typeof(MainWindow).Assembly.GetName().Version!)));
            if (_updatesClosed) return;
            _preparedRelease = state.Ready;
            _transferStatus = state.Transfer;
            if (_preparedRelease != null)
            {
                MainAbout.UpdateStatusText.Text = Loc.T("Update ready; restart to install");
                UpdateAvailableButton.Visibility = MainAbout.ReleaseLinksPanel.Visibility = Visibility.Visible;
                MainAbout.ReleaseNotesLink.NavigateUri ??= new Uri("https://github.com/wynter-yaxley/atrium-desktop/releases/latest");
                _downloadTimer?.Stop();
            }
            else if (_transferStatus != null)
            {
                MainAbout.UpdateStatusText.Text = _transferStatus.Text;
                UpdateAvailableButton.Visibility = MainAbout.ReleaseLinksPanel.Visibility = Visibility.Visible;
                MainAbout.ReleaseNotesLink.NavigateUri ??= new Uri("https://github.com/wynter-yaxley/atrium-desktop/releases/latest");
                if (_transferStatus.CanRetry) _downloadTimer?.Stop();
                else ObserveDownload();
            }
            else
            {
                _downloadTimer?.Stop();
            }

            ShowInstallAction();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or COMException or JsonException)
        {
            if (!_updatesClosed) MainAbout.UpdateStatusText.Text = Loc.F("Could not read download progress: {0}", error.Message);
            _downloadTimer?.Stop();
        }
        finally { _downloadRefreshBusy = false; }
    }

    private async Task QueueDownloadAsync(ReleaseInfo? release, bool isAuto = false)
    {
        if (_queueingDownload) return;
        if (isAuto && (!_session.Saved.AutoUpdate || _updatesClosed)) return;
        _queueingDownload = true;
        ShowInstallAction();
        try
        {
            using var worker = release != null ? ReleaseInstaller.StartBackgroundDownload(release) : ReleaseInstaller.ResumeBackgroundDownload();
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(45));
            await worker.WaitForExitAsync(cts.Token); // closing this window never cancels the transfer
            if (_updatesClosed) return;
            await RefreshDownloadAsync();
            if (worker.ExitCode != 0 && _transferStatus == null) MainAbout.UpdateStatusText.Text = Loc.T("Could not start the background download. Try again.");
        }
        catch (OperationCanceledException)
        {
            if (!_updatesClosed) MainAbout.UpdateStatusText.Text = Loc.T("Could not start the background download. Try again.");
        }
        catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception)
        { if (!_updatesClosed) MainAbout.UpdateStatusText.Text = Loc.F("Could not start the background download: {0}", error.Message); }
        finally { _queueingDownload = false; if (!_updatesClosed) ShowInstallAction(); }
    }

    private async void OnInstallRelease(object sender, RoutedEventArgs e)
    {
        if (_updateCheck != null || _queueingDownload) return;
        if (_preparedRelease != null)
        {
            _restartUpdate = _preparedRelease;
            await CloseRequestedAsync();
            return;
        }
        if (_latestRelease?.Download == null && _transferStatus == null) return;
        await QueueDownloadAsync(_latestRelease);
    }

    private void OnOpenUpdate(object sender, RoutedEventArgs e) => SelectPage(9);

    private async void OnCheckForUpdates(object sender, RoutedEventArgs e) => await CheckForUpdatesAsync();

    private async Task CheckForUpdatesAsync(bool isAuto = false)
    {
        if (_updateCheck != null) return;
        using var cancel = new CancellationTokenSource();
        _updateCheck = cancel;
        MainAbout.CheckButton.IsEnabled = false;
        MainAbout.UpdateProgressRing.Visibility = Visibility.Visible;
        MainAbout.UpdateProgressRing.IsActive = true;
        MainAbout.ReleaseLinksPanel.Visibility = Visibility.Collapsed;
        MainAbout.UpdateStatusText.Text = Loc.T("Checking…");
        string? failText = null;
        try
        {
            failText = ReleaseInstaller.ReadFailure() is { } failure ? Loc.F("Update not installed: {0}", failure) : null;
            await RefreshDownloadAsync();
            if (cancel.IsCancellationRequested) return;
            if (_preparedRelease != null)
            {
                UpdateAvailableButton.Visibility = MainAbout.ReleaseLinksPanel.Visibility = Visibility.Visible;
                MainAbout.UpdateStatusText.Text = failText ?? Loc.T("Update ready; restart to install");
                MainAbout.ReleaseNotesLink.NavigateUri = new Uri("https://github.com/wynter-yaxley/atrium-desktop/releases/latest");
                ShowInstallAction();
                return;
            }
            if (failText != null) UpdateAvailableButton.Visibility = Visibility.Visible;
            else if (_transferStatus is { CanRetry: false }) await QueueDownloadAsync(null, isAuto);
            var release = await ReleaseInfo.FetchLatestAsync(cancel.Token);
            if (cancel.IsCancellationRequested) return;
            if (release == null)
            {
                UpdateAvailableButton.Visibility = failText != null ? Visibility.Visible : Visibility.Collapsed;
                MainAbout.UpdateStatusText.Text = failText ?? Loc.T("No stable release yet");
                return;
            }
            var current = typeof(MainWindow).Assembly.GetName().Version!;
            int comparison = release.Version.CompareTo(current);
            UpdateAvailableButton.Visibility = comparison > 0 || failText != null ? Visibility.Visible : Visibility.Collapsed;
            MainAbout.UpdateStatusText.Text = failText ?? (comparison > 0 ? Loc.F("Update available: {0}", release.Tag)
                : comparison == 0 ? Loc.T("You're up to date") : Loc.F("This version is newer than the latest stable release {0}", release.Tag));
            MainAbout.ReleaseNotesLink.NavigateUri = release.Page;
            _latestRelease = release;
            ShowInstallAction();
            MainAbout.ReleaseLinksPanel.Visibility = Visibility.Visible;
            if (failText == null && comparison > 0 && release.Download != null) await QueueDownloadAsync(release, isAuto);
            else if (failText == null && _transferStatus != null) MainAbout.UpdateStatusText.Text = _transferStatus.Text;
        }
        catch (Exception error) when (error is HttpRequestException or OperationCanceledException
            or JsonException or FormatException or InvalidOperationException or KeyNotFoundException or IOException or UnauthorizedAccessException)
        {
            if (cancel.IsCancellationRequested) return; // the window closed while checking
            MainAbout.UpdateStatusText.Text = failText ?? _transferStatus?.Text ?? (error switch
            {
                OperationCanceledException => Loc.T("Connection timed out. Try again."),
                HttpRequestException { StatusCode: HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests }
                    => Loc.T("GitHub is not responding. Try again later."),
                HttpRequestException => Loc.T("Could not connect to GitHub. Try again."),
                IOException or UnauthorizedAccessException => Loc.F("Could not prepare the update: {0}", error.Message),
                _ => Loc.T("Could not read release information. Try again later."),
            });
        }
        finally
        {
            _updateCheck = null;
            if (!cancel.IsCancellationRequested)
            {
                MainAbout.UpdateProgressRing.IsActive = false;
                MainAbout.UpdateProgressRing.Visibility = Visibility.Collapsed;
                MainAbout.CheckButton.IsEnabled = true;
                ShowInstallAction();
            }
        }
    }
}
