using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

/// <summary>Stage Manager inspector options and gesture reference.</summary>
public sealed partial class StageInspector : UserControl
{
    private Func<BarSettings>? _settings;
    private Func<bool>? _isLoading;
    private Action<bool, bool>? _onChanged;
    private bool _settingSide;

    public StageInspector()
    {
        InitializeComponent();
    }

    internal void Bind(Func<BarSettings> settings, Func<bool> isLoading, Action<bool, bool> onChanged)
    {
        _settings = settings;
        _isLoading = isLoading;
        _onChanged = onChanged;
    }

    internal void Load(BarSettings s)
    {
        StageSwitch.IsOn = s.StageManager;
        (s.StageOnRight ? StageRight : StageLeft).IsChecked = true;
        (s.StageAutoHide ? StageAutoHide : StageAlways).IsChecked = true;
        StageMinimizeSwitch.IsOn = s.StageMinimize;
        StageDragMergeSwitch.IsOn = s.StageDragMerge;
        StageCloseButtonSwitch.IsOn = s.StageCloseButton;
        StageScrollWrapSwitch.IsOn = s.StageScrollWrap;
        StageAvoidSwitch.IsOn = s.StageAvoid;
    }

    internal void SetSide(bool isRight)
    {
        _settingSide = true;
        (isRight ? StageRight : StageLeft).IsChecked = true;
        _settingSide = false;
    }

    private void Apply(Action<BarSettings> update, bool refreshPreview = false, bool syncWidgets = false)
    {
        if (_settingSide || _isLoading?.Invoke() != false || _settings == null) return;
        update(_settings());
        _onChanged?.Invoke(refreshPreview, syncWidgets);
    }

    private void OnStageToggled(object sender, RoutedEventArgs e) =>
        Apply(s => s.StageManager = StageSwitch.IsOn, refreshPreview: true, syncWidgets: StageSwitch.IsOn);

    private void OnStageSideChecked(object sender, RoutedEventArgs e)
    {
        if (sender is not RadioButton { Tag: string tag }) return;
        Apply(s => s.StageOnRight = tag == "right", refreshPreview: true, syncWidgets: _settings?.Invoke().StageManager == true);
    }

    private void OnStageModeChecked(object sender, RoutedEventArgs e)
    {
        if (sender is not RadioButton { Tag: string tag }) return;
        Apply(s => s.StageAutoHide = tag == "hover", refreshPreview: true);
    }

    private void OnStageMinimizeToggled(object sender, RoutedEventArgs e) =>
        Apply(s => s.StageMinimize = StageMinimizeSwitch.IsOn);

    private void OnStageDragMergeToggled(object sender, RoutedEventArgs e) =>
        Apply(s => s.StageDragMerge = StageDragMergeSwitch.IsOn);

    private void OnStageAvoidToggled(object sender, RoutedEventArgs e) =>
        Apply(s => s.StageAvoid = StageAvoidSwitch.IsOn);

    private void OnStageCloseButtonToggled(object sender, RoutedEventArgs e) =>
        Apply(s => s.StageCloseButton = StageCloseButtonSwitch.IsOn);

    private void OnStageScrollWrapToggled(object sender, RoutedEventArgs e) =>
        Apply(s => s.StageScrollWrap = StageScrollWrapSwitch.IsOn);
}
