using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace Atrium.Settings;

/// <summary>Pinned taskbar apps list and context-menu preference.</summary>
public sealed partial class DockPinsPage : UserControl
{
    public DockPinsPage()
    {
        InitializeComponent();
    }

    internal TextBlock CountText => PinCount;
    internal ListView List => PinsList;
    internal StackPanel EmptyPanel => PinsEmpty;
    internal ToggleTile ContextMenuSwitch => WindowsContextMenuSwitch;

    internal event RoutedEventHandler? ImportPinsClicked;
    internal event RoutedEventHandler? AddPinClicked;
    internal event TypedEventHandler<ListViewBase, DragItemsCompletedEventArgs>? PinsDragCompleted;
    internal event RoutedEventHandler? WindowsContextMenuToggled;

    private void OnImportPins(object sender, RoutedEventArgs e) => ImportPinsClicked?.Invoke(sender, e);
    private void OnAddPin(object sender, RoutedEventArgs e) => AddPinClicked?.Invoke(sender, e);

    private void OnPinsDragCompleted(ListViewBase sender, DragItemsCompletedEventArgs args) =>
        PinsDragCompleted?.Invoke(sender, args);

    private void OnWindowsContextMenuToggled(object sender, RoutedEventArgs e) =>
        WindowsContextMenuToggled?.Invoke(sender, e);

    private void OnLocalizeTemplate(object sender, RoutedEventArgs e)
    {
        if (sender is DependencyObject root) Loc.Localize(root);
    }
}
