using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    private List<DesktopDisplay> _displays = [];

    private void InitDisplays()
    {
        MainDisplays.DelayTile.Configure(Loc.T("Cross-display dwell time"), 100, 2000, 50, "ms");
        MainDisplays.DelayTile.UserValueChanged += value =>
        {
            if (_loading) return;
            CurrentSettings.DockDisplayDelay = value;
            Changed();
        };
        MainDisplays.RefreshClicked += OnRefreshDisplays;
        MainDisplays.SystemDisplaysClicked += OnSystemDisplays;
        MainDisplays.DisplayModeChecked += OnDisplayModeChecked;
        MainDisplays.DockDisplayChanged += OnDockDisplayChanged;
        MainDisplays.DesktopDisplayChanged += OnDesktopDisplayChanged;
        MainDisplays.TaskbarAppsChanged += OnTaskbarAppsChanged;
        MainDisplays.TaskbarRightPrimaryOnlyToggled += OnTaskbarRightPrimaryOnlyToggled;
        RefreshDisplays();
    }

    private void RefreshDisplays()
    {
        bool loading = _loading;
        _loading = true;
        _displays = DesktopDisplay.Read();
        MainDisplays.ConnectedList.ItemsSource = _displays;
        MainDisplays.SummaryText.Text = Loc.F("{0} display(s) connected", _displays.Count);
        var choices = new List<DesktopDisplay> { new("", Loc.T("Follow the system primary display"), "", true) };
        choices.AddRange(_displays);
        // Keep an unplugged selection visible; reconnection restores it without resaving.
        foreach (string id in new[] { CurrentSettings.DockDisplay, CurrentSettings.DesktopDisplay }.Distinct())
        {
            if (id.Length > 0 && !choices.Any(d => d.Id == id))
                choices.Add(new(id, Loc.T("Disconnected display (using the primary until it returns)"), "", false));
        }

        MainDisplays.DockPicker.ItemsSource = choices;
        MainDisplays.DesktopPicker.ItemsSource = choices;
        MainDisplays.DockPicker.SelectedItem = choices.First(d => d.Id == CurrentSettings.DockDisplay);
        MainDisplays.DesktopPicker.SelectedItem = choices.First(d => d.Id == CurrentSettings.DesktopDisplay);
        MainDisplays.FallbackText.Text = new[] { CurrentSettings.DockDisplayMode == "fixed" ? CurrentSettings.DockDisplay : "", CurrentSettings.DesktopDisplay }
            .Any(id => id.Length > 0 && !_displays.Any(d => d.Id == id))
            ? Loc.T("The chosen display is disconnected; the primary is used and the choice returns when it reconnects.") : "";
        MainDisplays.FallbackText.Visibility = MainDisplays.FallbackText.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        _loading = loading;
    }

    private void LoadDisplayControls()
    {
        var mode = CurrentSettings.DockDisplayMode switch
        {
            "primary" => MainDisplays.PrimaryRadio,
            "fixed" => MainDisplays.FixedRadio,
            "all" => MainDisplays.AllRadio,
            _ => MainDisplays.FollowRadio,
        };
        mode.IsChecked = true;
        MainDisplays.DelayTile.SetValue(CurrentSettings.DockDisplayDelay);
        MainDisplays.AppsPicker.SelectedIndex = CurrentSettings.TaskbarApps switch { "primaryAndWindow" => 1, "window" => 2, _ => 0 };
        MainDisplays.RightPrimaryOnlyTile.IsOn = CurrentSettings.TaskbarRightPrimaryOnly;
        UpdateDisplayControls();
        RefreshDisplays();
    }

    private void UpdateDisplayControls()
    {
        MainDisplays.DockPicker.IsEnabled = CurrentSettings.DockDisplayMode == "fixed";
        MainDisplays.DelayTile.IsEnabled = CurrentSettings.DockDisplayMode == "follow";
    }

    private void OnDisplayModeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.DockDisplayMode = (string)((RadioButton)sender).Tag;
        UpdateDisplayControls();
        RefreshDisplays();
        Changed();
    }

    private void OnDockDisplayChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || e.AddedItems.Count == 0) return;
        CurrentSettings.DockDisplay = ((DesktopDisplay)e.AddedItems[0]).Id;
        Changed();
    }

    private void OnDesktopDisplayChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || e.AddedItems.Count == 0) return;
        CurrentSettings.DesktopDisplay = ((DesktopDisplay)e.AddedItems[0]).Id;
        Changed();
    }

    private void OnTaskbarAppsChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || MainDisplays.AppsPicker.SelectedIndex < 0) return;
        CurrentSettings.TaskbarApps = MainDisplays.AppsPicker.SelectedIndex switch { 1 => "primaryAndWindow", 2 => "window", _ => "all" };
        Changed();
    }

    private void OnTaskbarRightPrimaryOnlyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.TaskbarRightPrimaryOnly = MainDisplays.RightPrimaryOnlyTile.IsOn;
        Changed();
    }

    private void OnRefreshDisplays(object sender, RoutedEventArgs e) => RefreshDisplays();
    private void OnSystemDisplays(object sender, RoutedEventArgs e) => Process.Start(new ProcessStartInfo("ms-settings:display") { UseShellExecute = true });
}
