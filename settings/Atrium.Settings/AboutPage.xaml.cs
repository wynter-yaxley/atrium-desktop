using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

/// <summary>Application metadata, software update status, and settings file path.</summary>
public sealed partial class AboutPage : UserControl
{
    public AboutPage()
    {
        InitializeComponent();
    }

    internal Image LogoImage => AboutLogo;
    internal TextBlock VersionLabel => VersionText;
    internal TextBlock PathLabel => PathText;
    internal TextBlock UpdateStatusText => UpdateStatus;
    internal ProgressRing UpdateProgressRing => UpdateProgress;
    internal Button CheckButton => CheckUpdateButton;
    internal StackPanel ReleaseLinksPanel => ReleaseLinks;
    internal Button InstallButton => InstallRelease;
    internal HyperlinkButton ReleaseNotesLink => ReleaseNotes;
    internal ToggleTile AutoUpdateToggle => AutoUpdateSwitch;

    internal event RoutedEventHandler? CheckForUpdatesClicked;
    internal event RoutedEventHandler? InstallReleaseClicked;
    internal event RoutedEventHandler? AutoUpdateToggled;

    internal void Load(BarSettings s) => AutoUpdateSwitch.IsOn = s.AutoUpdate;

    private void OnCheckForUpdates(object sender, RoutedEventArgs e) => CheckForUpdatesClicked?.Invoke(sender, e);
    private void OnInstallRelease(object sender, RoutedEventArgs e) => InstallReleaseClicked?.Invoke(sender, e);
    private void OnAutoUpdateToggled(object sender, RoutedEventArgs e) => AutoUpdateToggled?.Invoke(sender, e);
}
