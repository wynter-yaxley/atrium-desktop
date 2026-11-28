using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;

namespace Atrium.Settings;

// The pages of the bar's newer entries: the now-playing entry (page 15), the folder stack (page 16)
// and Launchpad (page 17). Each page is built here when the window first asks
// for it (ExtrasPage, ExtrasSide), its navigation item with it; its controls read the settings each
// time the page shows. The previews are module scenes: the card, and the stack as it opens.
public sealed partial class MainWindow
{
    private const int NowPlayingPage = 15, StacksPage = 16, LaunchpadPage = 17;
    private static readonly string[] MotionTags = ["off", "subtle", "normal", "vivid"];
    private static readonly int[] StackMaxima = [8, 12, 16, 24];

    private StackPanel? _npMain, _npSide, _stMain, _stSide, _lpMain, _lpSide;
    private Border? _npStage, _stStage, _lpStage;
    private Canvas? _npCanvas, _stCanvas, _lpCanvas;
    private ModuleScene? _npScene, _stScene, _lpScene;
    private ToggleTile? _launchpadSwitch, _npSwitch, _npPausedSwitch, _stSwitch;
    private SegmentTrack? _lpLayout, _lpMaterial, _npMotion, _npRhythm, _stLayout, _stMax;
    private StackPanel? _stList, _stFileList;
    private TextBlock? _stCount, _stFileCount;
    private Button? _lpIconClear;
    private Image? _lpIconImage;
    private TextBlock? _lpIconGlyph, _lpIconNote;

    private StackPanel ExtrasPage(int page)
    {
        BuildExtras();
        return page switch { NowPlayingPage => _npMain!, StacksPage => _stMain!, _ => _lpMain! };
    }

    private StackPanel ExtrasSide(int page)
    {
        BuildExtras();
        return page switch { NowPlayingPage => _npSide!, StacksPage => _stSide!, _ => _lpSide! };
    }

    private ModuleScene? ExtrasScene(int page) => page switch
    {
        NowPlayingPage => _npScene ??= new NowPlayingScene(this, _npStage!, _npCanvas!),
        StacksPage => _stScene ??= new StacksScene(this, _stStage!, _stCanvas!),
        LaunchpadPage => _lpScene ??= new LaunchpadScene(this, _lpStage!, _lpCanvas!),
        _ => null,
    };

    // The bar pages' scenes and these pages' (Scenes in MainWindow.Modules.cs).
    private IEnumerable<ModuleScene> WithExtras(ModuleScene?[] scenes) => scenes.Append(_npScene).Append(_stScene).Append(_lpScene).OfType<ModuleScene>();

    // ---- Building ----

    private void BuildExtras()
    {
        if (_npMain != null) return;
        (_npMain, _npStage, _npCanvas) = StagePage();
        (_stMain, _stStage, _stCanvas) = StagePage();
        (_lpMain, _lpStage, _lpCanvas) = StagePage();
        int at = CenterStack.Children.IndexOf(MainIme) + 1;
        CenterStack.Children.Insert(at, _lpMain);
        CenterStack.Children.Insert(at, _stMain);
        CenterStack.Children.Insert(at, _npMain);
        _npSide = BuildNowPlayingSide();
        _stSide = BuildStacksSide();
        _stMain.Children.Add(BuildStacksGroup(isFile: false));
        _stMain.Children.Add(BuildStacksGroup(isFile: true));
        _npMain.Children.Add(BuildPlayers());
        _launchpadSwitch = Toggle("Show on the taskbar", "Show Launchpad on the taskbar",
            "An entry next to the start logo opens every app, page by page, in one large window of glass, as on macOS.", (s, on) => s.ShowLaunchpad = on);
        _lpMaterial = Segments("launchpadmaterial", [("Follow the taskbar", "bar"), ("Original color", "tint")],
            tag => CurrentSettings.LaunchpadMaterial = tag);
        _lpLayout = Segments("launchpadlayout", [("Windowed", "window"), ("Full-screen", "fullscreen")],
            tag => CurrentSettings.LaunchpadLayout = tag);
        _lpSide = Side("Launchpad", _launchpadSwitch, BuildLaunchpadIconRow(), Row("Display mode", _lpLayout), Row("Material", _lpMaterial));
        InspectorStack.Children.Add(_npSide);
        InspectorStack.Children.Add(_stSide);
        InspectorStack.Children.Add(_lpSide);
        // Each page reads the settings as it shows (they may have changed elsewhere since).
        _npSide.RegisterPropertyChangedCallback(UIElement.VisibilityProperty, (_, _) => { if (_npSide.Visibility == Visibility.Visible) LoadExtras(); });
        _stSide.RegisterPropertyChangedCallback(UIElement.VisibilityProperty, (_, _) => { if (_stSide.Visibility == Visibility.Visible) LoadExtras(); });
        AddNavItem(NowPlayingPage, "", "Now Playing", "12");
        AddNavItem(StacksPage, "", "File Stacks", "10"); // the bar's order: the stack sits left of the tray
        _lpSide.RegisterPropertyChangedCallback(UIElement.VisibilityProperty, (_, _) => { if (_lpSide.Visibility == Visibility.Visible) LoadExtras(); });
        // The bar's order, left to right: the start menu, Launchpad, the stack, then the tray's side.
        AddNavItem(LaunchpadPage, "\uF0E2", "Launchpad", "10");
    }

    // A page's picture: a stage the size of the other bar pages', its scene drawn on the canvas.
    private (StackPanel, Border, Canvas) StagePage()
    {
        var canvas = new Canvas { HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
        var stage = new Border
        {
            Height = 520,
            CornerRadius = new CornerRadius(28),
            Background = new SolidColorBrush(Color.FromArgb(0xFF, 0xCB, 0xD3, 0xDD)),
            Child = new Grid
            {
                Children =
                {
                    canvas,
                    new Border
                    {
                        CornerRadius = new CornerRadius(28), BorderThickness = new Thickness(1),
                        BorderBrush = new SolidColorBrush(Color.FromArgb(0x1F, 0, 0, 0)), IsHitTestVisible = false,
                    },
                },
            },
        };
        stage.SizeChanged += OnModuleStageSizeChanged;
        return (new StackPanel { Visibility = Visibility.Collapsed, Spacing = 10, Children = { stage } }, stage, canvas);
    }

    // Beside the bar's other parts in the navigation, after the item tagged `after`.
    private void AddNavItem(int page, string glyph, string text, string after)
    {
        string localized = Loc.T(text);
        var item = new ListViewItem
        {
            Tag = page.ToString(System.Globalization.CultureInfo.InvariantCulture),
            Content = new NavLabel { Glyph = glyph, Text = localized, Sub = true },
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(item, localized);
        int index = Nav.Items.IndexOf(Nav.Items.OfType<ListViewItem>().FirstOrDefault(i => i.Tag as string == after));
        Nav.Items.Insert(index < 0 ? Nav.Items.Count : index + 1, item);
    }

    private StackPanel BuildNowPlayingSide()
    {
        _npSwitch = Toggle("Show on the taskbar", "Show now playing",
            "Right of the status entry, while a music or video app is playing. Its five bars move with the sound.", (s, on) => s.ShowNowPlaying = on);
        _npPausedSwitch = Toggle("Keep while paused", "Keep now playing while paused", "", (s, on) => s.NowPlayingShowPaused = on);
        _npMotion = Segments("nowplayingmotion", [("Off", "off"), ("Subtle", "subtle"), ("Standard", "normal"), ("Vivid", "vivid")],
            tag => CurrentSettings.NowPlayingMotion = tag);
        _npRhythm = Segments("nowplayingrhythm", [("Follows the music", "live"), ("Steady animation", "loop")],
            tag => CurrentSettings.NowPlayingRhythm = tag);
        return Side("Now Playing", _npSwitch, _npPausedSwitch, Row("Bars' rhythm", _npRhythm), Row("Bars' motion", _npMotion));
    }

    private StackPanel BuildStacksSide()
    {
        _stSwitch = Toggle("Show on the taskbar", "Show file stacks",
            "Left of the tray: a click fans out the folders added here, a click on one opens it.", (s, on) => s.ShowFolderStacks = on);
        _stLayout = Segments("stacklayout", [("Fan", "fan"), ("List", "column")], tag => CurrentSettings.FolderStackLayout = tag);
        var invariant = System.Globalization.CultureInfo.InvariantCulture;
        _stMax = Segments("stackmax", [.. StackMaxima.Select(n => (n.ToString(System.Globalization.CultureInfo.CurrentCulture), n.ToString(invariant)))],
            tag => CurrentSettings.FolderStackMax = int.Parse(tag, System.Globalization.CultureInfo.InvariantCulture));
        return Side("File Stacks", _stSwitch, Row("Opens as", _stLayout), Row("Shows at most", _stMax));
    }

    // The Launchpad entry's icon: the built-in mark, or a picture file of the user's.
    private StackPanel BuildLaunchpadIconRow()
    {
        var res = Application.Current.Resources;
        _lpIconGlyph = new TextBlock
        {
            Text = "\uF0E2",
            FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
            FontSize = 15,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        _lpIconImage = new Image { Stretch = Stretch.Uniform, Visibility = Visibility.Collapsed };
        var preview = new Grid { Width = 28, Height = 28, Children = { _lpIconGlyph, _lpIconImage } };
        var choose = new Button { Content = Loc.T("Choose image…") };
        choose.Click += (_, _) => PickLaunchpadIcon();
        _lpIconClear = new Button
        {
            Style = (Style)res["IconButtonStyle"],
            Content = new FontIcon { Glyph = "\uE7A7", FontSize = 12 },
            Visibility = Visibility.Collapsed,
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(_lpIconClear, Loc.T("Use the built-in icon"));
        ToolTipService.SetToolTip(_lpIconClear, Loc.T("Use the built-in icon"));
        _lpIconClear.Click += (_, _) =>
        {
            CurrentSettings.LaunchpadIcon = "";
            ShowLaunchpadIcon("");
            Changed();
        };
        _lpIconNote = new TextBlock
        {
            Style = (Style)res["FootnoteStyle"],
            TextWrapping = TextWrapping.NoWrap,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var row = new StackPanel { Style = (Style)res["FlatRowStyle"] };
        row.Children.Add(new TextBlock { Text = Loc.T("Icon"), Style = (Style)res["RowTitleStyle"] });
        row.Children.Add(new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { preview, choose, _lpIconClear } });
        row.Children.Add(_lpIconNote);
        return row;
    }

    // The row's picture and note follow the setting; the bar previews follow with them (BarFace).
    private void ShowLaunchpadIcon(string path)
    {
        if (_lpIconImage == null || _lpIconGlyph == null || _lpIconNote == null || _lpIconClear == null) return;
        var bitmap = LaunchpadIcons.Preview(path);
        _lpIconImage.Source = bitmap;
        _lpIconImage.Visibility = bitmap != null ? Visibility.Visible : Visibility.Collapsed;
        _lpIconGlyph.Visibility = bitmap == null ? Visibility.Visible : Visibility.Collapsed;
        _lpIconClear.Visibility = path.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        string note = path.Length > 0 ? path : Loc.T("Built-in icon");
        _lpIconNote.Text = note;
        ToolTipService.SetToolTip(_lpIconNote, note);
    }

    private void PickLaunchpadIcon()
    {
        if (_updatesClosed) return;
        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        string? path = NativeFileDialog.PickPicture(hwnd, Loc.T("Images"), App.LogCrash);
        if (path == null || _updatesClosed) return;
        if (LaunchpadIcons.Preview(path) == null)
        {
            ShowMessage(InfoBarSeverity.Warning, Loc.T("That file could not be read as an image."), path);
            return;
        }
        CurrentSettings.LaunchpadIcon = path;
        ShowLaunchpadIcon(path);
        Changed();
    }

    // The folders and files, under the picture as the taskbar's pinned apps are under theirs: a
    // title with the count and the buttons that add, then the list in a group.
    private StackPanel BuildStacksGroup(bool isFile)
    {
        var res = Application.Current.Resources;
        var list = new StackPanel { Padding = new Thickness(0, 4, 0, 4) };
        var count = new TextBlock { Style = (Style)res["FootnoteStyle"], VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) };
        if (isFile) (_stFileList, _stFileCount) = (list, count);
        else (_stList, _stCount) = (list, count);
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            HorizontalAlignment = HorizontalAlignment.Right,
            Children = { count },
        };
        if (!isFile)
        {
            var places = new MenuFlyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedRight };
            foreach (var (name, path) in KnownStackFolders())
            {
                if (path.Length == 0) continue;
                var item = new MenuFlyoutItem { Text = Loc.T(name) };
                item.Click += (_, _) => AddStackPaths([path], isFile: false);
                places.Items.Add(item);
            }
            buttons.Children.Add(new DropDownButton { Content = Loc.T("Common folders"), Flyout = places });
        }
        var add = new Button { Content = isFile ? Loc.T("Add files…") : Loc.T("Add folder…"), Style = (Style)res["PrimaryButtonStyle"] };
        add.Click += async (_, _) => await PickStackAsync(isFile);
        buttons.Children.Add(add);
        var title = new TextBlock
        {
            Text = isFile ? Loc.T("Files") : Loc.T("Folders"),
            Style = (Style)res["SectionStyle"],
            Margin = new Thickness(4, 0, 0, 0),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var header = new Grid { Margin = new Thickness(0, 0, 0, 10), Children = { title, buttons } };
        var group = new Border { Style = (Style)res["GroupStyle"], Child = list };
        return new StackPanel { Margin = new Thickness(0, 16, 0, 0), Children = { header, group } };
    }

    private static (string Name, string Path)[] KnownStackFolders()
    {
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return
        [
            ("Desktop", Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)),
            ("Downloads", home.Length > 0 ? Path.Combine(home, "Downloads") : ""),
            ("Documents", Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)),
            ("Pictures", Environment.GetFolderPath(Environment.SpecialFolder.MyPictures)),
            ("Music", Environment.GetFolderPath(Environment.SpecialFolder.MyMusic)),
        ];
    }

    // ---- The controls ----

    private ToggleTile Toggle(string title, string name, string note, Action<BarSettings, bool> set)
    {
        var tile = new ToggleTile { Title = Loc.T(title), SwitchName = Loc.T(name), Flat = true };
        if (note.Length > 0) tile.Note = Loc.T(note);
        tile.Toggled += (_, _) =>
        {
            if (_loading) return;
            set(CurrentSettings, tile.IsOn);
            Changed();
        };
        return tile;
    }

    private SegmentTrack Segments(string group, (string Label, string Tag)[] items, Action<string> set)
    {
        var track = new SegmentTrack();
        foreach (var (label, tag) in items)
        {
            var button = new RadioButton { GroupName = group, Content = Loc.T(label), Tag = tag, Style = (Style)Application.Current.Resources["SegmentStyle"] };
            button.Checked += (_, _) =>
            {
                if (_loading) return;
                set(tag);
                Changed();
            };
            track.Children.Add(button);
        }
        return track;
    }

    private static void Check(SegmentTrack? track, string tag)
    {
        if (track == null) return;
        foreach (var button in track.Children.OfType<RadioButton>()) button.IsChecked = button.Tag as string == tag;
    }

    private static StackPanel Row(string title, FrameworkElement control) => new()
    {
        Style = (Style)Application.Current.Resources["FlatRowStyle"],
        Children = { new TextBlock { Text = Loc.T(title), Style = (Style)Application.Current.Resources["RowTitleStyle"] }, control },
    };

    private static StackPanel Side(string title, params FrameworkElement[] rows)
    {
        var inner = new StackPanel { Children = { new TextBlock { Text = Loc.T(title), Style = (Style)Application.Current.Resources["SectionStyle"] } } };
        foreach (var row in rows) inner.Children.Add(row);
        return new StackPanel { Style = (Style)Application.Current.Resources["InspectorPageStyle"], Visibility = Visibility.Collapsed, Children = { inner } };
    }

    // The controls from the settings, without counting as an edit.
    private void LoadExtras()
    {
        bool was = _loading;
        _loading = true;
        var s = CurrentSettings;
        _launchpadSwitch?.IsOn = s.ShowLaunchpad;
        Check(_lpLayout, s.LaunchpadLayout == "fullscreen" ? "fullscreen" : "window");
        Check(_lpMaterial, s.LaunchpadMaterial == "tint" ? "tint" : "bar");
        ShowLaunchpadIcon(s.LaunchpadIcon);
        _npSwitch?.IsOn = s.ShowNowPlaying;
        _npPausedSwitch?.IsOn = s.NowPlayingShowPaused;
        Check(_npMotion, MotionTags.Contains(s.NowPlayingMotion) ? s.NowPlayingMotion : "normal");
        Check(_npRhythm, s.NowPlayingRhythm == "loop" ? "loop" : "live");
        RefreshPlayers();
        _stSwitch?.IsOn = s.ShowFolderStacks;
        Check(_stLayout, s.FolderStackLayout == "column" ? "column" : "fan");
        int max = StackMaxima.MinBy(n => Math.Abs(n - s.FolderStackMax));
        Check(_stMax, max.ToString(System.Globalization.CultureInfo.InvariantCulture));
        FillStackList(isFile: false);
        FillStackList(isFile: true);
        _loading = was;
    }

    // ---- The stack's folders and files ----

    private List<string> StackItems(bool isFile) => isFile ? CurrentSettings.FolderStackFiles : CurrentSettings.FolderStacks;

    private void FillStackList(bool isFile)
    {
        var list = isFile ? _stFileList : _stList;
        if (list == null) return;
        list.Children.Clear();
        var items = StackItems(isFile);
        var count = isFile ? _stFileCount : _stCount;
        count?.Text = items.Count == 0 ? "" : Loc.F("{0} of 32", items.Count);
        if (items.Count == 0)
        {
            var res = Application.Current.Resources;
            list.Children.Add(new StackPanel
            {
                Padding = new Thickness(24, 36, 24, 36),
                Spacing = 6,
                HorizontalAlignment = HorizontalAlignment.Center,
                Children =
                {
                    new TextBlock
                    {
                        Text = isFile ? Loc.T("No files yet") : Loc.T("No folders yet"),
                        Style = (Style)res["TitleStyle"],
                        HorizontalAlignment = HorizontalAlignment.Center,
                    },
                    new TextBlock
                    {
                        Text = isFile
                            ? Loc.T("Choose documents or other files with Add files….")
                            : Loc.T("Choose one with Add folder…, or pick a common folder such as Downloads."),
                        Style = (Style)res["RowNoteStyle"], TextAlignment = TextAlignment.Center, MaxWidth = 360,
                    },
                },
            });
            return;
        }
        for (int i = 0; i < items.Count; i++) list.Children.Add(StackRow(i, items[i], items.Count, isFile));
    }

    internal static bool IsStackFile(string path, bool defaultIsFile)
    {
        if (File.Exists(path)) return true;
        if (Directory.Exists(path)) return false;
        return defaultIsFile;
    }

    internal static string StackItemName(string path, bool isFile)
    {
        if (isFile)
        {
            string stem = Path.GetFileNameWithoutExtension(path);
            if (stem.Length > 0) return stem;
            string file = Path.GetFileName(path);
            return file.Length > 0 ? file : path;
        }
        string trimmed = path.TrimEnd('\\', '/');
        string name = Path.GetFileName(trimmed);
        return name.Length > 0 ? name : (trimmed.Length > 0 ? trimmed : path);
    }

    internal void SetStackItemIcon(string path, bool isFile, Image picture, TextBlock glyph)
    {
        glyph.Text = isFile ? "\U0001F4C4" : "\U0001F4C1";
        if (!isFile || path.Length == 0)
        {
            picture.Tag = null;
            picture.Visibility = Visibility.Collapsed;
            glyph.Visibility = Visibility.Visible;
            return;
        }
        picture.Tag = path;
        if (_iconCache.TryGetValue(path, out var cached))
        {
            picture.Source = cached;
            picture.Visibility = Visibility.Visible;
            glyph.Visibility = Visibility.Collapsed;
            return;
        }
        picture.Visibility = Visibility.Collapsed;
        glyph.Visibility = Visibility.Visible;
        _ = LoadStackFileIconAsync(path, picture, glyph);
    }

    private async Task LoadStackFileIconAsync(string path, Image picture, TextBlock glyph)
    {
        if (!_iconCache.TryGetValue(path, out var source))
        {
            var img = await ShellIcons.LoadAsync(path, 96);
            if (img != null)
            {
                var bmp = new WriteableBitmap(img.Width, img.Height);
                using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(img.Bgra, 0, img.Bgra.Length);
                bmp.Invalidate();
                source = bmp;
                _iconCache[path] = source;
            }
        }
        if (source != null && picture.Tag as string == path)
        {
            picture.Source = source;
            picture.Visibility = Visibility.Visible;
            glyph.Visibility = Visibility.Collapsed;
        }
    }

    // A folder or file as a pinned app's row: its icon, its name over its path, and up, down and remove.
    private Grid StackRow(int index, string path, int count, bool isFile)
    {
        var res = Application.Current.Resources;
        var row = new Grid { Padding = new Thickness(14, 8, 10, 8), ColumnSpacing = 14, MinHeight = 60 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        bool showAsFile = IsStackFile(path, isFile);
        var symbol = new TextBlock
        {
            FontFamily = new FontFamily("Segoe UI Emoji"),
            FontSize = 28,
            TextAlignment = TextAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var picture = new Image { Width = 32, Height = 32, Stretch = Stretch.Uniform, Visibility = Visibility.Collapsed };
        var icon = new Grid { Width = 40, Height = 40, VerticalAlignment = VerticalAlignment.Center, Children = { symbol, picture } };
        SetStackItemIcon(path, showAsFile, picture, symbol);
        string name = StackItemName(path, showAsFile);
        var text = new StackPanel
        {
            VerticalAlignment = VerticalAlignment.Center,
            Spacing = 1,
            Children =
            {
                new TextBlock
                {
                    Text = name.Length > 0 ? name : path, Style = (Style)res["RowTitleStyle"],
                    TextTrimming = TextTrimming.CharacterEllipsis, TextWrapping = TextWrapping.NoWrap,
                },
                new TextBlock
                {
                    Text = path, Style = (Style)res["FootnoteStyle"], TextTrimming = TextTrimming.CharacterEllipsis, TextWrapping = TextWrapping.NoWrap,
                },
            },
        };
        ToolTipService.SetToolTip(text, path);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        foreach (var (glyph, label, move, size) in new[] { ("\uE70E", "Move up", -1, 13), ("\uE70D", "Move down", 1, 13), ("\uE711", "Remove", 0, 12) })
        {
            var button = new Button
            {
                Style = (Style)res["IconButtonStyle"],
                Content = new FontIcon { Glyph = glyph, FontSize = size },
                IsEnabled = move == 0 || (index + move >= 0 && index + move < count),
            };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(button, Loc.T(label));
            ToolTipService.SetToolTip(button, Loc.T(label));
            button.Click += (_, _) => EditStack(index, move, isFile);
            buttons.Children.Add(button);
        }
        Grid.SetColumn(text, 1);
        Grid.SetColumn(buttons, 2);
        row.Children.Add(icon);
        row.Children.Add(text);
        row.Children.Add(buttons);
        return row;
    }

    // move: -1 up, 1 down, 0 remove.
    private void EditStack(int index, int move, bool isFile)
    {
        var items = StackItems(isFile);
        if (index < 0 || index >= items.Count) return;
        if (move == 0) items.RemoveAt(index);
        else if (index + move >= 0 && index + move < items.Count) (items[index], items[index + move]) = (items[index + move], items[index]);
        FillStackList(isFile);
        Changed();
    }

    private static string NormalizeStackPath(string path, bool isFile)
    {
        string trimmed = path.Trim();
        if (trimmed.Length == 0 || isFile) return trimmed;
        string? root = Path.GetPathRoot(trimmed);
        if (root != null)
        {
            while (trimmed.Length > root.Length && (trimmed[^1] == '\\' || trimmed[^1] == '/'))
                trimmed = trimmed[..^1];
        }
        return trimmed;
    }

    private void AddStackPaths(IEnumerable<string> paths, bool isFile)
    {
        var items = StackItems(isFile);
        bool added = false;
        foreach (string raw in paths)
        {
            string normalized = NormalizeStackPath(raw, isFile);
            if (normalized.Length == 0 || !Path.IsPathFullyQualified(normalized)) continue;
            bool known = items.Any(f => string.Equals(NormalizeStackPath(f, isFile), normalized, StringComparison.OrdinalIgnoreCase));
            if (items.Count >= 32 || known) continue;
            items.Add(normalized);
            added = true;
        }
        if (!added) return;
        if (!CurrentSettings.ShowFolderStacks)
        {
            CurrentSettings.ShowFolderStacks = true; // an entry added is a stack wanted
            LoadExtras();
        }
        FillStackList(isFile);
        Changed();
    }

    private Task PickStackAsync(bool isFile)
    {
        if (_updatesClosed) return Task.CompletedTask;
        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        if (isFile)
        {
            string[] picked = NativeFileDialog.PickFiles(hwnd, Loc.T("Files"), App.LogCrash);
            if (picked.Length > 0 && !_updatesClosed)
                AddStackPaths(picked, isFile: true);
        }
        else
        {
            string? folder = NativeFileDialog.PickFolder(hwnd, App.LogCrash);
            if (folder != null && !_updatesClosed)
                AddStackPaths([folder], isFile: false);
        }
        return Task.CompletedTask;
    }
}
