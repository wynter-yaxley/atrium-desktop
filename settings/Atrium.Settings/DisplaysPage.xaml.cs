using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

/// <summary>Display topology and multi-monitor placement controls.</summary>
public sealed partial class DisplaysPage : UserControl
{
    public DisplaysPage()
    {
        InitializeComponent();
    }

    internal TextBlock SummaryText => DisplaySummary;
    internal ItemsControl ConnectedList => ConnectedDisplayList;
    internal RadioButton FollowRadio => DockDisplayFollow;
    internal RadioButton PrimaryRadio => DockDisplayPrimary;
    internal RadioButton FixedRadio => DockDisplayFixed;
    internal RadioButton AllRadio => DockDisplayAll;
    internal ComboBox DockPicker => DockDisplayPicker;
    internal ComboBox DesktopPicker => DesktopDisplayPicker;
    internal SliderTile DelayTile => DisplayDelay;
    internal ComboBox AppsPicker => TaskbarAppsPicker;
    internal ToggleTile RightPrimaryOnlyTile => TaskbarRightPrimaryOnlySwitch;
    internal TextBlock FallbackText => DisplayFallback;

    internal event RoutedEventHandler? RefreshClicked;
    internal event RoutedEventHandler? SystemDisplaysClicked;
    internal event RoutedEventHandler? DisplayModeChecked;
    internal event SelectionChangedEventHandler? DockDisplayChanged;
    internal event SelectionChangedEventHandler? DesktopDisplayChanged;
    internal event SelectionChangedEventHandler? TaskbarAppsChanged;
    internal event RoutedEventHandler? TaskbarRightPrimaryOnlyToggled;

    private void OnRefresh(object sender, RoutedEventArgs e) => RefreshClicked?.Invoke(sender, e);
    private void OnSystemDisplays(object sender, RoutedEventArgs e) => SystemDisplaysClicked?.Invoke(sender, e);
    private void OnDisplayMode(object sender, RoutedEventArgs e) => DisplayModeChecked?.Invoke(sender, e);
    private void OnDockDisplay(object sender, SelectionChangedEventArgs e) => DockDisplayChanged?.Invoke(sender, e);
    private void OnDesktopDisplay(object sender, SelectionChangedEventArgs e) => DesktopDisplayChanged?.Invoke(sender, e);
    private void OnTaskbarApps(object sender, SelectionChangedEventArgs e) => TaskbarAppsChanged?.Invoke(sender, e);
    private void OnTaskbarRightPrimaryOnly(object sender, RoutedEventArgs e) => TaskbarRightPrimaryOnlyToggled?.Invoke(sender, e);
}
