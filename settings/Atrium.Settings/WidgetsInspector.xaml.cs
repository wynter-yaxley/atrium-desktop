using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

/// <summary>Desktop widgets inspector options.</summary>
public sealed partial class WidgetsInspector : UserControl
{
    private Func<BarSettings>? _settings;
    private Func<bool>? _isLoading;
    private Action? _onWidgetsToggled;
    private Action? _onSideChanged;
    private Action? _onMaterialChanged;
    private Action? _onChanged;
    private bool _settingSide;

    public WidgetsInspector()
    {
        InitializeComponent();
    }

    internal bool WidgetsOn => WidgetsSwitch.IsOn;

    internal void Bind(
        Func<BarSettings> settings,
        Func<bool> isLoading,
        Action onWidgetsToggled,
        Action onSideChanged,
        Action onMaterialChanged,
        Action onChanged)
    {
        _settings = settings;
        _isLoading = isLoading;
        _onWidgetsToggled = onWidgetsToggled;
        _onSideChanged = onSideChanged;
        _onMaterialChanged = onMaterialChanged;
        _onChanged = onChanged;
    }

    internal void Load(BarSettings s)
    {
        WidgetsSwitch.IsOn = s.Widgets;
        (s.WidgetsOnRight ? WidgetsRight : WidgetsLeft).IsChecked = true;
        (s.WidgetBackdrop == "wallpaper" ? BackdropWallpaper : BackdropDesktop).IsChecked = true;
        (s.WidgetMaterial switch { "dusk" => MaterialDusk, "color" => MaterialColor, _ => MaterialGlass }).IsChecked = true;
    }

    internal void SetSide(bool isRight)
    {
        _settingSide = true;
        (isRight ? WidgetsRight : WidgetsLeft).IsChecked = true;
        _settingSide = false;
    }

    private void OnWidgetsToggled(object sender, RoutedEventArgs e)
    {
        if (_settings != null && _isLoading?.Invoke() == false)
            _settings().Widgets = WidgetsSwitch.IsOn;
        _onWidgetsToggled?.Invoke();
    }

    private void OnWidgetAlignmentChecked(object sender, RoutedEventArgs e)
    {
        if (_settingSide || _isLoading?.Invoke() != false || _settings == null || sender is not RadioButton { Tag: string tag }) return;
        _settings().WidgetsOnRight = tag == "right";
        _onSideChanged?.Invoke();
    }

    private void OnWidgetMaterialChecked(object sender, RoutedEventArgs e)
    {
        if (_isLoading?.Invoke() != false || _settings == null || sender is not RadioButton { Tag: string tag }) return;
        var s = _settings();
        if (tag == s.WidgetMaterial) return;
        s.WidgetMaterial = tag;
        _onMaterialChanged?.Invoke();
    }

    private void OnWidgetBackdropChecked(object sender, RoutedEventArgs e)
    {
        if (_isLoading?.Invoke() != false || _settings == null || sender is not RadioButton { Tag: string tag }) return;
        _settings().WidgetBackdrop = tag;
        _onChanged?.Invoke();
    }
}
