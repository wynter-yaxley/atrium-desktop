using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

/// <summary>Language, Windows startup, and shell integration controls.</summary>
public sealed partial class SystemPage : UserControl
{
    private Func<BarSettings>? _settings;
    private Func<bool>? _isLoading;
    private Action? _onChanged;
    private bool _populating;

    public SystemPage()
    {
        InitializeComponent();
    }

    internal Panel Sections => RootStack;

    internal void Bind(Func<BarSettings> settings, Func<bool> isLoading, Action onChanged)
    {
        _settings = settings;
        _isLoading = isLoading;
        _onChanged = onChanged;
        PopulateLanguagePicker();
        StartupSwitch.IsEnabled = !CoreClient.Development;
        StartupMethodPicker.IsEnabled = !CoreClient.Development;
        if (CoreClient.Development)
        {
            string tip = Loc.T("Development builds do not change Windows startup.");
            ToolTipService.SetToolTip(StartupSwitch, tip);
            ToolTipService.SetToolTip(StartupMethodPicker, tip);
        }
    }

    internal void Load(BarSettings s)
    {
        HideTaskbarSwitch.IsOn = s.HideTaskbar;
        GenieSwitch.IsOn = s.GenieMinimize;
        PeekSwitch.IsOn = s.WindowPeek;
        HideBannersSwitch.IsOn = s.HideBanners;
        WinKeySwitch.IsOn = s.WinKeyStart;
        WinNumbersSwitch.IsOn = s.WinNumbers;
        RemoteVisibleSwitch.IsOn = s.RemoteVisible;
        StartupSwitch.IsOn = s.StartOnLogin;
        UpdateStartupMethodState(s);

        string currentLanguage = s.Language == "zh" ? "zh-Hans" : s.Language;
        LanguagePicker.SelectedIndex = 0;
        for (int i = 0; i < LanguagePicker.Items.Count; i++)
        {
            if (LanguagePicker.Items[i] is ComboBoxItem item && (item.Tag as string) == currentLanguage)
                LanguagePicker.SelectedIndex = i;
        }
    }

    private void UpdateStartupMethodState(BarSettings s)
    {
        string mode = s.StartupMode == "task" ? "task" : "run";
        StartupMethodPicker.SelectedIndex = mode == "task" ? 1 : 0;
        StartupMethodPicker.IsEnabled = !CoreClient.Development && s.StartOnLogin;
    }

    // The picker follows locales/manifest.json: adding a language file and a
    // manifest entry is enough; no code change here.
    private void PopulateLanguagePicker()
    {
        _populating = true;
        LanguagePicker.Items.Clear();
        LanguagePicker.Items.Add(new ComboBoxItem { Content = Loc.T("Follow system"), Tag = "auto" });
        foreach (var (tag, name) in Loc.Languages)
            LanguagePicker.Items.Add(new ComboBoxItem { Content = name, Tag = tag });
        _populating = false;
    }

    private void Apply(Action<BarSettings> update)
    {
        if (_populating || _isLoading?.Invoke() != false || _settings == null) return;
        update(_settings());
        _onChanged?.Invoke();
    }

    private void OnLanguageChanged(object sender, SelectionChangedEventArgs e) =>
        Apply(s => s.Language = (LanguagePicker.SelectedItem as ComboBoxItem)?.Tag as string ?? "auto");

    private void OnStartupToggled(object sender, RoutedEventArgs e)
    {
        Apply(s =>
        {
            s.StartOnLogin = StartupSwitch.IsOn;
            StartupMethodPicker.IsEnabled = !CoreClient.Development && s.StartOnLogin;
        });
    }

    private void OnStartupMethodChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_populating || _isLoading?.Invoke() != false || _settings == null) return;
        if (StartupMethodPicker.SelectedItem is not ComboBoxItem { Tag: string raw }) return;
        string mode = raw == "task" ? "task" : "run";
        if (string.Equals(_settings().StartupMode, mode, StringComparison.Ordinal)) return;
        Apply(s => s.StartupMode = mode);
    }

    private void OnHideTaskbarToggled(object sender, RoutedEventArgs e) => Apply(s => s.HideTaskbar = HideTaskbarSwitch.IsOn);
    private void OnGenieToggled(object sender, RoutedEventArgs e) => Apply(s => s.GenieMinimize = GenieSwitch.IsOn);
    private void OnPeekToggled(object sender, RoutedEventArgs e) => Apply(s => s.WindowPeek = PeekSwitch.IsOn);
    private void OnHideBannersToggled(object sender, RoutedEventArgs e) => Apply(s => s.HideBanners = HideBannersSwitch.IsOn);
    private void OnWinKeyToggled(object sender, RoutedEventArgs e) => Apply(s => s.WinKeyStart = WinKeySwitch.IsOn);
    private void OnWinNumbersToggled(object sender, RoutedEventArgs e) => Apply(s => s.WinNumbers = WinNumbersSwitch.IsOn);
    private void OnRemoteVisibleToggled(object sender, RoutedEventArgs e) => Apply(s => s.RemoteVisible = RemoteVisibleSwitch.IsOn);
}
