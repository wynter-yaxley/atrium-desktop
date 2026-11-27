using System.Numerics;
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorRect { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo { public int Size; public MonitorRect Bounds, Work; public uint Flags; }

    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromRect(IntPtr rect, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW")] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool SetProp(IntPtr hwnd, string name, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr RemoveProp(IntPtr hwnd, string name);
    private delegate IntPtr SubclassProc(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam, UIntPtr id, UIntPtr data);
    [DllImport("comctl32.dll")] private static extern bool SetWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id, UIntPtr data);
    [DllImport("comctl32.dll")] private static extern bool RemoveWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id);
    [DllImport("comctl32.dll")] private static extern IntPtr DefSubclassProc(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam);
    private readonly SubclassProc? _updateMessage;

    private void UpdateWindowMinimumSize(uint dpi = 0, IntPtr suggestedRect = 0)
    {
        if (AppWindow.Presenter is not OverlappedPresenter presenter) return;
        if (dpi == 0) dpi = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this));
        double scale = (dpi > 0 ? dpi : 96) / 96.0;
        int minWidth = (int)(980 * scale), minHeight = (int)(680 * scale);
        int workW = 0, workH = 0;
        if (suggestedRect != IntPtr.Zero)
        {
            nint monitor = MonitorFromRect(suggestedRect, 2 /* MONITOR_DEFAULTTONEAREST */);
            var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
            if (monitor != IntPtr.Zero && GetMonitorInfo(monitor, ref info))
            {
                workW = info.Work.Right - info.Work.Left;
                workH = info.Work.Bottom - info.Work.Top;
            }
        }
        else if (DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary) is { } area)
        {
            workW = area.WorkArea.Width;
            workH = area.WorkArea.Height;
        }
        if (workW > 0 && workH > 0)
        {
            minWidth = Math.Min(minWidth, workW);
            minHeight = Math.Min(minHeight, workH);
        }
        presenter.PreferredMinimumWidth = minWidth;
        presenter.PreferredMinimumHeight = minHeight;
    }

    // ---- Pages ----

    // The navigation's items name their pages (Tag): the bar's parts sit under it, numbered after the others.
    private void OnNavChanged(object sender, SelectionChangedEventArgs e)
    {
        if (Nav.SelectedItem is ListViewItem { Tag: string tag } && int.TryParse(tag, out int page)) ShowPage(page);
    }

    private void SelectPage(int page)
    {
        foreach (object? item in Nav.Items)
            if (item is ListViewItem { Tag: string tag } li && tag == page.ToString()) { Nav.SelectedItem = li; return; }
    }

    // A page is its workspace part (under the title, with the live preview on the first three) and its
    // inspector part; pages without parameters of their own (system, design, about) take the full width.
    private void ShowPage(int index)
    {
        if (_page == 8 && index != 8) StopDesignAnimations();
        _page = index;
        for (int i = 0; i < _mains.Length; i++)
        {
            _mains[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
            if (_sides[i] is { } side) side.Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
        }
        PageTitle.Text = PageTitles[index];
        HeroBlock.Visibility = index < 3 ? Visibility.Visible : Visibility.Collapsed;
        LoupeToggle.Visibility = index == 0 ? Visibility.Visible : Visibility.Collapsed;
        Loupe.Visibility = index == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        // The design page reads wider than a settings page.
        CenterStack.MaxWidth = _mains[index] == DesignStack ? 1280 : 1120;
        ArrangeInspector();
        if (!_windowLoaded) return;
        ActivatePageAfterLoad(index);
    }

    private void ActivateCurrentPageAfterLoad() => ActivatePageAfterLoad(_page);

    private void RefreshLoadedPreviews(bool fromLoad)
    {
        if (!_windowLoaded) return;
        UpdateHero();
        UpdateCustomSwatch();
        if (_page == 3) ShowNotifyPreview(false);
        if (fromLoad && _page == 4) { ShowWidgetMap(); RefreshGallery(); }
        if (_page == 5)
        {
            if (fromLoad) ShowStagePreview(false);
            else StageRenderGlass(StageCanvas.Width, StageCanvas.Height);
        }
        if (_page >= 10) ShowModulePreview(_page);
    }

    private void ActivatePageAfterLoad(int index)
    {
        if (_mains[index] == DesignStack)
        {
            BuildDesignPage();
            RefreshDesignDetails(); // the "current" column follows the settings
        }
        if (_mains[index] == MainWidgets)
        {
            if (WidgetGallery.Children.Count == 0) BuildWidgetGallery(); // built when first shown
            else RefreshGallery();
            ShowWidgetMap();
        }
        if (_mains[index] == MainNotify) ShowNotifyPreview(true);
        if (_mains[index] == MainStatus)
        {
            if (ModuleGallery.Children.Count == 0) BuildModuleGallery();
            else RefreshModuleGallery();
        }
        if (_mains[index] == MainStage) ShowStagePreview(true);
        if (_mains[index] == MainDisplays) RefreshDisplays();
        if (_mains[index] == MainSystem) EnsureAdminCard();
        if (index >= 10) EnterModulePage(index);
        else LeaveModulePages();
        ContinuousCorners.Install(_mains[index]);
        // Pages and the inspector are created/reparented lazily; translate the
        // one being shown now, so a language switch does not wait for a restart.
        Loc.Localize(_mains[index]);
        if (_sides[index] is { } pageSide) Loc.Localize(pageSide);
        Loc.Localize(InspectorStack);
        Loc.Localize(FoldedInspector);
        CenterScroll.ChangeView(null, 0, null, true);
        InspectorScroll.ChangeView(null, 0, null, true);
        if (index < 3) UpdateHero();
        if (index == 0) UpdateCustomSwatch();
        AnimateIn(_mains[index]);
        if (_sides[index] is { } shown) AnimateIn(shown);
    }

    // The inspector: its own column when there is room for the workspace beside it, else folded
    // under the workspace (the groups move, keeping their state).
    private const double FoldBelow = 900;

    private void OnContentSizeChanged(object sender, SizeChangedEventArgs e)
    {
        bool folded = e.NewSize.Width < FoldBelow;
        if (folded == _folded && _mains.Length > 0) return;
        _folded = folded;
        if (folded)
        {
            InspectorScroll.Content = null;
            InspectorStack.Margin = new Thickness(0, 8, 0, 0);
            FoldedInspector.Child = InspectorStack;
        }
        else
        {
            FoldedInspector.Child = null;
            InspectorStack.ClearValue(FrameworkElement.MarginProperty);
            InspectorScroll.Content = InspectorStack;
        }
        ArrangeInspector();
    }

    private void ArrangeInspector()
    {
        bool has = _sides.Length > _page && _sides[_page] != null;
        InspectorPane.Visibility = has && !_folded ? Visibility.Visible : Visibility.Collapsed;
        FoldedInspector.Visibility = has && _folded ? Visibility.Visible : Visibility.Collapsed;
    }

    // Every page uses the viewport width, including pages with small intrinsic content.
    private void OnWorkspaceSizeChanged(object sender, SizeChangedEventArgs e) =>
        CenterStack.Width = Math.Max(0, e.NewSize.Width - CenterStack.Margin.Left - CenterStack.Margin.Right);

    // The previews keep a natural proportion as the workspace widens.
    private void OnCenterSizeChanged(object sender, SizeChangedEventArgs e)
    {
        double w = e.NewSize.Width;
        HeroFrame.Height = Math.Clamp(Math.Round(w * 0.52), 280, 580);
        NotifyStage.Height = Math.Clamp(Math.Round(w * 0.42), 260, 440);
    }

    private void OnLoupeToggled(object sender, RoutedEventArgs e)
    {
        Loupe.Visibility = _page == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        if (Loupe.Visibility == Visibility.Collapsed)
        {
            _loupeBrush.ImageSource = null;
            _loupeBmp = null;
        }
        UpdateHero();
    }

    // Fade and rise a few pixels; the transitions are attached one tick later so the start values stick.
    private void AnimateIn(UIElement e)
    {
        e.OpacityTransition = null;
        e.TranslationTransition = null;
        e.Opacity = 0;
        e.Translation = new Vector3(0, 10, 0);
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(240) };
            e.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(300) };
            e.Opacity = 1;
            e.Translation = Vector3.Zero;
        });
    }

    // The sidebar is a virtualizing list; its entries are set directly so they
    // follow the language even before the first list container is realized.
    private void LocalizeNav()
    {
        foreach (object? entry in Nav.Items)
        {
            if (entry is not ListViewItem item) continue;
            if (item.Content is NavLabel label) label.Text = Loc.T(label.Text);
            string name = AutomationProperties.GetName(item);
            if (!string.IsNullOrEmpty(name)) AutomationProperties.SetName(item, Loc.T(name));
        }
    }

    // A template's elements are created after the window: localize each one as it loads.
    private void OnLocalizeTemplate(object sender, RoutedEventArgs e)
    {
        if (sender is DependencyObject root) Loc.Localize(root);
    }

    private async void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        args.Cancel = true;
        await CloseRequestedAsync();
    }

    // Window.Close does not raise AppWindow.Closing. Both the title-bar close
    // and update button explicitly use the same save/exit path before calling it.
    private async Task CloseRequestedAsync()
    {
        if (_closePending) return;
        if (_coreChanging) { _closeAfterCore = true; return; }
        if (_dialog != null) // a close while a dialog is up dismisses the dialog
        {
            _dialog.Hide();
            return;
        }
        _closePending = true;
        try
        {
            if (_session.HasUnsavedChanges)
            {
                var dialog = new ContentDialog
                {
                    Title = Loc.T("Save changes?"),
                    Content = Loc.T("Unsaved changes will be discarded when this window closes."),
                    PrimaryButtonText = Loc.T("Save"),
                    SecondaryButtonText = Loc.T("Don't save"),
                    CloseButtonText = Loc.T("Cancel"),
                    DefaultButton = ContentDialogButton.Primary,
                };
                var result = await AskAsync(dialog);
                if (result == ContentDialogResult.None) { _restartUpdate = null; return; }
                if (result == ContentDialogResult.Primary && !await SaveAsync()) { _restartUpdate = null; return; }
            }
            if (_restartUpdate != null)
            {
                try { ReleaseInstaller.Restart(_restartUpdate, _client.Connected); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
                {
                    _restartUpdate = null;
                    MainAbout.UpdateStatusText.Text = error is UnauthorizedAccessException
                        ? Loc.T("The installation folder is not writable; the update was not installed")
                        : Loc.F("Could not restart for the update: {0}", error.Message);
                    return;
                }
            }
            _client.Dispose(); // the core reverts any unsaved preview on disconnect
            Close();
        }
        finally { _closePending = false; }
    }
}
