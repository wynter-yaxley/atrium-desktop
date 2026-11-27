using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace Atrium.Settings;

// One card of the pinned-apps list.
public sealed partial class PinVm : INotifyPropertyChanged
{
    private readonly MainWindow _owner;
    private string? _resolvedPath;

    internal PinVm(MainWindow owner, PinnedApp pin)
    {
        _owner = owner;
        Pin = pin;
        Title = MainWindow.PinTitle(pin);
        UpName = Loc.F("Move up {0}", Title);
        DownName = Loc.F("Move down {0}", Title);
        RemoveName = Loc.F("Remove {0}", Title);
        LaunchName = Loc.F("Launch mode {0}", Title);
    }

    internal PinnedApp Pin { get; private set; }
    public string Title { get; }
    public string UpName { get; }
    public string DownName { get; }
    public string RemoveName { get; }
    public string LaunchName { get; }
    public int LaunchMode
    {
        get => Pin.AllowsMultipleWindows(_resolvedPath) ? 1 : 0;
        set
        {
            if (value < 0 || value == 1 == Pin.AllowsMultipleWindows(_resolvedPath)) return;
            Pin = Pin with { MultipleWindows = value == 1 };
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(LaunchMode)));
            _owner.ChangePinLaunch();
        }
    }

    public bool CanUp { get; private set => Set(ref field, value, nameof(CanUp)); }
    public bool CanDown { get; private set => Set(ref field, value, nameof(CanDown)); }
    public ImageSource? Icon
    {
        get;
        set { if (Set(ref field, value, nameof(Icon))) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(FallbackVisibility))); }
    }
    public Visibility FallbackVisibility => Icon == null ? Visibility.Visible : Visibility.Collapsed;

    internal void SetResolvedPath(string path)
    {
        int before = LaunchMode;
        _resolvedPath = path;
        if (LaunchMode != before) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(LaunchMode)));
    }

    internal void SetPosition(int index, int count)
    {
        CanUp = index > 0;
        CanDown = index < count - 1;
    }

    public void MoveUp() => _owner.MovePin(this, -1);
    public void MoveDown() => _owner.MovePin(this, 1);
    public void Remove() => _owner.RemovePin(this);

    public event PropertyChangedEventHandler? PropertyChanged;

    private bool Set<T>(ref T field, T value, string name)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }
}

public sealed partial class MainWindow
{
    private readonly ObservableCollection<PinVm> _pinVms = [];
    private readonly Dictionary<string, ImageSource?> _iconCache = new(StringComparer.OrdinalIgnoreCase);
    private bool _pinOverflowShown;

    internal static string PinTitle(PinnedApp p) =>
        p.Name.Length > 0 ? p.Name : p.Path.Length > 0 ? Path.GetFileNameWithoutExtension(p.Path) : p.Aumid;

    internal void ShowPinOverflow()
    {
        var overflow = _session.OverflowPins;
        int total = _session.Draft.Pins?.Count ?? _pinVms.Count;
        const int PreviewCount = 3;
        string names = string.Join(Loc.T(", "), overflow.Take(PreviewCount).Select(PinTitle));
        string body = overflow.Count > PreviewCount
            ? Loc.F(
                "Pinned apps now total {0} / 64. Cannot save: {1} (+{2} more). Remove the extra apps to save, or click Revert to discard all unsaved changes.",
                total, names, overflow.Count - PreviewCount)
            : Loc.F(
                "Pinned apps now total {0} / 64. Cannot save: {1}. Remove the extra apps to save, or click Revert to discard all unsaved changes.",
                total, names);
        ShowMessage(InfoBarSeverity.Warning, Loc.T("Too many pinned apps"), body);
    }

    private void InitPins()
    {
        MainDock.List.ItemsSource = _pinVms;
        MainDock.ImportPinsClicked += OnImportPins;
        MainDock.AddPinClicked += OnAddPin;
        MainDock.PinsDragCompleted += OnPinsDragCompleted;
        MainDock.WindowsContextMenuToggled += OnWindowsContextMenuToggled;
    }

    private void RebuildPins()
    {
        if (_pinVms.Select(vm => vm.Pin).SequenceEqual(CurrentSettings.Pins ?? Enumerable.Empty<PinnedApp>()))
        {
            RefreshPins();
            return;
        }
        _pinVms.Clear();
        foreach (var pin in CurrentSettings.Pins ?? [])
        {
            var vm = new PinVm(this, pin);
            _pinVms.Add(vm);
            _ = LoadIconAsync(vm);
        }
        RefreshPins();
    }

    private void RefreshPins()
    {
        var names = _pinVms.Select(vm => ShellIcons.ParsingName(vm.Pin)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (string? key in _iconCache.Keys.Where(key => !names.Contains(key)).ToArray()) _iconCache.Remove(key);
        _previewPins = [.. _pinVms.Take(MaxPreviewApps - 1)];
        for (int i = 0; i < _pinVms.Count; i++) _pinVms[i].SetPosition(i, _pinVms.Count);
        bool empty = _pinVms.Count == 0;
        MainDock.EmptyPanel.Visibility = empty ? Visibility.Visible : Visibility.Collapsed;
        MainDock.List.Visibility = empty ? Visibility.Collapsed : Visibility.Visible;
        MainDock.CountText.Text = $"{_pinVms.Count} / {SettingsSession.MaxPins}";
        if (_session.HasPinOverflow)
        {
            ShowPinOverflow();
            _pinOverflowShown = true;
        }
        else if (_pinOverflowShown)
        {
            _pinOverflowShown = false;
            Status.IsOpen = false;
        }
    }

    // The collection is the order the user sees; the settings follow it.
    private void SyncPins()
    {
        CurrentSettings.Pins = [.. _pinVms.Select(v => v.Pin)];
        RefreshPins();
    }

    private async Task LoadIconAsync(PinVm vm)
    {
        if (vm.Pin.MultipleWindows == null && vm.Pin.Path.EndsWith(".lnk", StringComparison.OrdinalIgnoreCase))
        {
            string target = await Task.Run(() => ShellIcons.ExecutablePath(vm.Pin.Path));
            if (!_pinVms.Contains(vm)) return;
            vm.SetResolvedPath(target);
        }
        string name = ShellIcons.ParsingName(vm.Pin);
        if (!_iconCache.TryGetValue(name, out var source))
        {
            var img = await ShellIcons.LoadAsync(name, 96); // 32 DIP at up to 300 % scale
            if (!_pinVms.Contains(vm)) return;
            if (img != null)
            {
                var bmp = new WriteableBitmap(img.Width, img.Height);
                using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(img.Bgra, 0, img.Bgra.Length);
                bmp.Invalidate();
                source = bmp;
            }
            if (source != null) _iconCache[name] = source;
        }
        if (source != null) vm.Icon = source;
    }

    internal void MovePin(PinVm vm, int delta)
    {
        int index = _pinVms.IndexOf(vm), to = index + delta;
        if (index < 0 || to < 0 || to >= _pinVms.Count) return;
        _pinVms.Move(index, to);
        SyncPins();
        FocusPinButton(vm, delta < 0 ? Loc.T("Move up") : Loc.T("Move down"));
        Changed();
    }

    internal void ChangePinLaunch()
    {
        SyncPins();
        Changed();
    }

    internal void RemovePin(PinVm vm)
    {
        int index = _pinVms.IndexOf(vm);
        if (index < 0) return;
        _pinVms.RemoveAt(index);
        SyncPins();
        if (_pinVms.Count > 0) FocusPinButton(_pinVms[Math.Min(index, _pinVms.Count - 1)], Loc.T("Remove"));
        Changed();
    }

    // Keyboard users keep their place after a move or removal.
    private void FocusPinButton(PinVm vm, string verb)
    {
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            if (MainDock.List.ContainerFromItem(vm) is not DependencyObject container) return;
            var buttons = new List<Button>();
            Collect(container, buttons);
            var target = buttons.FirstOrDefault(b => b.IsEnabled && AutomationProperties.GetName(b).StartsWith(verb)) ??
                buttons.FirstOrDefault(b => b.IsEnabled);
            target?.Focus(FocusState.Programmatic);
        });
    }

    private static void Collect(DependencyObject root, List<Button> into)
    {
        for (int i = 0, n = VisualTreeHelper.GetChildrenCount(root); i < n; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is Button b) into.Add(b);
            Collect(child, into);
        }
    }

    private void OnWindowsContextMenuToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        CurrentSettings.WindowsContextMenu = MainDock.ContextMenuSwitch.IsOn;
        Changed();
    }

    // Drag and drop reordered the collection; write the new order back.
    private void OnPinsDragCompleted(ListViewBase sender, DragItemsCompletedEventArgs args)
    {
        var order = _pinVms.Select(v => v.Pin).ToList();
        if (CurrentSettings.Pins != null && order.SequenceEqual(CurrentSettings.Pins))
        {
            RefreshPins();
            return;
        }
        SyncPins();
        Changed();
    }

    private async void OnAddPin(object sender, RoutedEventArgs e)
    {
        string? path = await PickProgramAsync();
        if (path == null) return;
        if (_pinVms.Count >= SettingsSession.MaxPins)
        {
            ShowMessage(InfoBarSeverity.Warning, Loc.T("Pinned apps limit reached"), Loc.T("You can pin at most 64 apps."));
            return;
        }
        var vm = new PinVm(this, new PinnedApp(Path.GetFileNameWithoutExtension(path), path, "", ""));
        _pinVms.Add(vm);
        _ = LoadIconAsync(vm);
        SyncPins();
        FocusPinButton(vm, Loc.T("Remove"));
        MainDock.List.ScrollIntoView(vm);
        Changed();
    }

    private async void OnImportPins(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            Title = Loc.T("Import Windows taskbar pins?"),
            Content = Loc.T("The current pinned list will be replaced by the apps pinned to the Windows taskbar."),
            PrimaryButtonText = Loc.T("Replace"),
            CloseButtonText = Loc.T("Cancel"),
            DefaultButton = ContentDialogButton.Close,
        };
        if (await AskAsync(dialog) != ContentDialogResult.Primary) return;
        CurrentSettings.Pins = CoreClient.ImportTaskbarPins();
        RebuildPins();
        Changed();
    }
}
