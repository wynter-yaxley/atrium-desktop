using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace Atrium.Settings;

// A row of a list of apps under a preview (the notification sources, the apps Stage Manager
// leaves alone): the app's icon, found from its program, and its name.
public abstract partial class AppRowVm : INotifyPropertyChanged
{
    public string Title { get; set { field = value; Raise(nameof(Title)); } } = "";
    public string Glyph { get; protected init; } = "\uecaa";
    public ImageSource? Icon
    {
        get; set
        {
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Icon)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(FallbackVisibility)));
        }
    }
    public Visibility FallbackVisibility => Icon == null ? Visibility.Visible : Visibility.Collapsed;
    internal string IconSource { get; set; } = ""; // a parsing name for ShellIcons; empty: found from Programs, else the glyph
    internal string Bundled { get; init; } = ""; // an icon of Assets\Apps (assets/apps), shown whether or not the app is installed
    internal string[] Programs { get; init; } = []; // program file names to look for
    internal string Folder { get; init; } = ""; // a game's folder: its icon is its program's
    internal bool Describe { get; init; } // the title is the program's description, once found

    public event PropertyChangedEventHandler? PropertyChanged;
    protected void Raise(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

// A notification source: one chat app, or the other apps' Windows notifications; some with an option of their
// own beside the switch (WeChat's official accounts), to tick only while the source is on.
public sealed partial class SourceVm : AppRowVm
{
    private readonly Func<BarSettings, bool> _get;
    private readonly Action<BarSettings, bool> _set;
    private readonly MainWindow _owner;

    internal SourceVm(MainWindow owner, string title, string note, string glyph, string[] programs, Func<BarSettings, bool> get, Action<BarSettings, bool> set)
    {
        _owner = owner;
        Title = Loc.T(title);
        string resolvedNote = string.IsNullOrWhiteSpace(note) ? "" : Loc.T(note);
        Note = string.IsNullOrWhiteSpace(resolvedNote) ? null : resolvedNote;
        Glyph = glyph;
        Programs = programs;
        _get = get;
        _set = set;
    }

    public string? Note { get; }
    public bool IsOn
    {
        get => _get(_owner.CurrentSettings);
        set
        {
            if (value == _get(_owner.CurrentSettings)) return;
            _set(_owner.CurrentSettings, value);
            _owner.SourceChanged();
            Raise(nameof(IsOn)); // the option beside it follows
        }
    }

    // The option beside the switch, if the source has one.
    internal Func<BarSettings, bool>? OptionGet { get; init; }
    internal Action<BarSettings, bool>? OptionSet { get; init; }
    public string OptionTitle { get; init; } = "";
    public string? OptionNote { get; init => field = string.IsNullOrWhiteSpace(value) ? null : value; }
    public Visibility OptionVisibility => OptionGet == null ? Visibility.Collapsed : Visibility.Visible;
    public bool OptionOn
    {
        get => OptionGet?.Invoke(_owner.CurrentSettings) ?? false;
        set
        {
            if (OptionSet == null || value == OptionOn) return;
            OptionSet(_owner.CurrentSettings, value);
            _owner.SourceChanged();
        }
    }
    internal void Refresh()
    {
        Raise(nameof(IsOn));
        Raise(nameof(OptionOn));
    }
}

// An app Stage Manager leaves alone: one entry of stageExcluded (or the Snipping Tool's three).
public sealed partial class ExcludedVm : AppRowVm
{
    private readonly MainWindow _owner;

    internal ExcludedVm(MainWindow owner, string title, string icon, string[] programs, IReadOnlyList<string> entries, bool named = false)
    {
        _owner = owner;
        Title = title;
        IconSource = icon;
        Programs = programs;
        Folder = entries.Count == 1 && GameFinder.IsFolder(entries[0]) ? entries[0] : "";
        Describe = !named && Folder.Length == 0 && (programs.Length > 0 || icon.EndsWith(".exe", StringComparison.OrdinalIgnoreCase));
        Entries = entries;
        RemoveName = Loc.F("Remove {0}", title);
    }

    internal IReadOnlyList<string> Entries { get; }
    public string RemoveName { get; }
    public void Remove() => _owner.RemoveExcluded(this);
}

public sealed partial class MainWindow
{
    private readonly ObservableCollection<SourceVm> _sources = [];
    private readonly ObservableCollection<ExcludedVm> _excluded = [];
    private static readonly Dictionary<string, string> ProgramPaths = new(StringComparer.OrdinalIgnoreCase);
    // The Windows Snipping Tool, one row for its entries (the packaged app and its two programs).
    private const string SnippingToolIcon = @"shell:AppsFolder\Microsoft.ScreenSketch_8wekyb3d8bbwe!App";

    internal BarSettings CurrentSettings => _session.Draft;

    private SliderTile? _notifyDurationTile;

    private void InitSources()
    {
        _notifyDurationTile = new SliderTile { Flat = true };
        _notifyDurationTile.Configure(Loc.T("Popup duration"), 3, 31, 1, "s", Loc.T("Always"));
        _notifyDurationTile.UserValueChanged += v =>
        {
            if (_loading) return;
            if (Math.Round(v) >= 31)
            {
                CurrentSettings.NotifyAlways = true;
            }
            else
            {
                CurrentSettings.NotifyAlways = false;
                CurrentSettings.NotifyTimeout = Math.Round(v);
            }
            Changed();
        };
        _notifyDurationTile.Engaged += on => OnEngage(Loc.T("Popup duration"), on);
        NotifyRows.Children.Insert(0, _notifyDurationTile);
        _sources.Add(new SourceVm(this, "WeChat",
            "Reads WeChat's chat list while its window is open or minimized; closed to the tray, a card only says there is a new message. Muted chats are ignored.",
            "\ue8bd", ["Weixin.exe", "WeChat.exe"], s => s.NotifyWeChat, (s, v) => s.NotifyWeChat = v)
        {
            Bundled = "wechat",
            OptionTitle = Loc.T("Block Official Accounts"),
            OptionNote = Loc.T(
                "No cards for the Official Accounts, Subscriptions and Service Accounts rows of WeChat's chat list. A service account listed as a chat of its own still shows."),
            OptionGet = s => s.WeChatBlockOfficial,
            OptionSet = (s, v) => s.WeChatBlockOfficial = v,
        });
        _sources.Add(new SourceVm(this, "QQ", "QQ's Windows notifications; when it flashes for a message without one, a card says there is a new message.",
            "\ue8bd", ["QQ.exe"], s => s.NotifyQQ, (s, v) => s.NotifyQQ = v)
        { Bundled = "qq" });
        _sources.Add(new SourceVm(this, "Feishu",
            "Reads the text of Feishu's popups; when it flashes for a message without one, a card says there is a new message.",
            "\ue8bd", ["Feishu.exe", "Lark.exe"], s => s.NotifyFeishu, (s, v) => s.NotifyFeishu = v)
        { Bundled = "feishu" });
        _sources.Add(new SourceVm(this, "WeCom", "When WeCom flashes for a new message, a card says so.",
            "\ue8bd", ["WXWork.exe", "WeCom.exe", "WXWorkLocal.exe", "WXWorkLocalPro.exe"], s => s.NotifyWeCom, (s, v) => s.NotifyWeCom = v)
        { Bundled = "wecom" });
        _sources.Add(new SourceVm(this, "DingTalk", "When DingTalk flashes for a new message, a card says so.",
            "\ue8bd", ["DingTalk.exe"], s => s.NotifyDingTalk, (s, v) => s.NotifyDingTalk = v)
        { Bundled = "dingtalk" });
        _sources.Add(new SourceVm(this, "Other apps", "Apps that send Windows notifications. Allow notification access in Windows privacy settings.",
            "\uea8f", [], s => s.NotifyListener, (s, v) => s.NotifyListener = v));
        SourcesList.ItemsSource = _sources;
        SourcesLimit.Text = Loc.T(
            "Limited by what WeChat lets other apps read: chat names and text are read only after WeChat's window has been opened once (until then a card only says there is a new message), and only from the chats near the top of its list.");
        foreach (var vm in _sources) _ = LoadRowIconAsync(vm);
        ExcludedList.ItemsSource = _excluded;
    }

    private void RefreshSources()
    {
        _notifyDurationTile?.SetValue(CurrentSettings.NotifyAlways ? 31 : CurrentSettings.NotifyTimeout);
        foreach (var vm in _sources) vm.Refresh();
    }

    internal void SourceChanged()
    {
        if (_loading) return;
        Changed();
    }

    private void RebuildExcluded()
    {
        var entries = CurrentSettings.StageExcluded;
        if (_excluded.SelectMany(vm => vm.Entries).SequenceEqual(entries, StringComparer.OrdinalIgnoreCase))
        {
            RefreshExcluded();
            return;
        }
        _excluded.Clear();
        var snipping = entries.Where(e => BarSettings.StageExcludedDefaults.Contains(e, StringComparer.OrdinalIgnoreCase)).ToList();
        bool snippingAdded = false;
        foreach (string entry in entries)
        {
            if (snipping.Contains(entry, StringComparer.OrdinalIgnoreCase))
            {
                if (snippingAdded) continue;
                snippingAdded = true;
                _excluded.Add(new ExcludedVm(this, Loc.T("Snipping Tool"), SnippingToolIcon, [], snipping));
                continue;
            }
            bool program = entry.EndsWith(".exe", StringComparison.OrdinalIgnoreCase), path = entry.Contains('\\') && !GameFinder.IsFolder(entry);
            _excluded.Add(new ExcludedVm(this, ExcludedTitle(entry), path ? entry : "", program && !path ? [entry] : [], [entry]));
        }
        foreach (var vm in _excluded) _ = LoadRowIconAsync(vm);
        RefreshExcluded();
    }

    private void RefreshExcluded()
    {
        bool empty = _excluded.Count == 0;
        ExcludedEmpty.Visibility = empty ? Visibility.Visible : Visibility.Collapsed;
        ExcludedList.Visibility = empty ? Visibility.Collapsed : Visibility.Visible;
        bool defaults = CurrentSettings.StageExcluded.SequenceEqual(BarSettings.StageExcludedDefaults, StringComparer.OrdinalIgnoreCase);
        ExcludedDefaultsButton.Visibility = defaults ? Visibility.Collapsed : Visibility.Visible;
    }

    private void SyncExcluded()
    {
        CurrentSettings.StageExcluded = [.. _excluded.SelectMany(vm => vm.Entries).Distinct(StringComparer.OrdinalIgnoreCase)];
        RefreshExcluded();
    }

    internal void RemoveExcluded(ExcludedVm vm)
    {
        if (!_excluded.Remove(vm)) return;
        SyncExcluded();
        Changed();
    }

    private async void OnAddExcluded(object sender, RoutedEventArgs e)
    {
        var file = await PickProgramAsync();
        if (file == null) return;
        // By its program's file name: still the same app after it updates into another folder.
        string exe = ShellIcons.ExecutablePath(file.Path);
        if (!exe.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) return;
        string entry = Path.GetFileName(exe).ToLowerInvariant();
        if (CurrentSettings.StageExcluded.Contains(entry, StringComparer.OrdinalIgnoreCase)) return;
        lock (ProgramPaths) ProgramPaths[entry] = exe;
        var vm = new ExcludedVm(this, ExcludedTitle(entry), exe, [], [entry]);
        _excluded.Add(vm);
        _ = LoadRowIconAsync(vm);
        SyncExcluded();
        ExcludedList.ScrollIntoView(vm);
        Changed();
    }

    // The installed games, listed for the user to choose which Stage Manager leaves alone.
    private async void OnFindGames(object sender, RoutedEventArgs e)
    {
        var button = (Button)sender;
        button.IsEnabled = false;
        List<GameFinder.Game> found;
        try { found = await Task.Run(GameFinder.Find); }
        finally { button.IsEnabled = true; }
        found.RemoveAll(g => CurrentSettings.StageExcluded.Contains(g.Entry, StringComparer.OrdinalIgnoreCase));
        var rows = new List<(CheckBox Box, GameFinder.Game Game)>();
        var list = new StackPanel { Spacing = 2, MinWidth = 360 };
        foreach (var game in found)
        {
            var image = new Image { Width = 28, Height = 28 };
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
            row.Children.Add(image);
            row.Children.Add(new TextBlock { Text = game.Title, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
            var box = new CheckBox { IsChecked = true, Content = row, Padding = new Thickness(10, 4, 0, 4) };
            list.Children.Add(box);
            rows.Add((box, game));
            if (game.Icon.Length > 0) _ = SetIconAsync(image, game.Icon);
        }
        var dialog = new ContentDialog
        {
            Title = Loc.T(found.Count > 0 ? "Games found" : "No games found"),
            Content = found.Count > 0 ? new ScrollViewer { Content = list, MaxHeight = 420 } : null,
            PrimaryButtonText = found.Count > 0 ? Loc.T("Exclude selected") : "",
            CloseButtonText = Loc.T(found.Count > 0 ? "Cancel" : "OK"),
            DefaultButton = found.Count > 0 ? ContentDialogButton.Primary : ContentDialogButton.Close,
        };
        if (await AskAsync(dialog) != ContentDialogResult.Primary) return;
        ExcludedVm? last = null;
        foreach (var (box, game) in rows)
        {
            if (box.IsChecked != true || CurrentSettings.StageExcluded.Contains(game.Entry, StringComparer.OrdinalIgnoreCase)) continue;
            last = new ExcludedVm(this, game.Title, game.Icon, [], [game.Entry], named: true);
            _excluded.Add(last);
            _ = LoadRowIconAsync(last);
            SyncExcluded();
        }
        if (last == null) return;
        ExcludedList.ScrollIntoView(last);
        Changed();
    }

    private async Task SetIconAsync(Image image, string source)
    {
        if (await IconFor(source) is ImageSource icon) image.Source = icon;
    }

    private void OnStageExcludedDefaults(object sender, RoutedEventArgs e)
    {
        CurrentSettings.StageExcluded = [.. BarSettings.StageExcludedDefaults];
        RebuildExcluded();
        Changed();
    }

    // An entry's name until its program is found: the file name without .exe (a package prefix without its _).
    private static string ExcludedTitle(string entry)
    {
        string name = entry.TrimEnd('_', '\\');
        int slash = name.LastIndexOfAny(['\\', '/']);
        if (slash >= 0) name = name[(slash + 1)..];
        return name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name[..^4] : name;
    }

    private static string Description(string path)
    {
        try { return System.Diagnostics.FileVersionInfo.GetVersionInfo(path).FileDescription?.Trim() ?? ""; }
        catch { return ""; }
    }

    // Where a program is: running now, else behind a start menu shortcut, else App Paths. Empty when
    // none is found (the row shows a glyph).
    private static string Program(params string[] files)
    {
        foreach (string file in files)
        {
            lock (ProgramPaths)
                if (ProgramPaths.TryGetValue(file, out string? known)) return known;
            string found = FindProgram(file);
            lock (ProgramPaths) ProgramPaths[file] = found;
            if (found.Length > 0) return found;
        }
        return "";
    }

    private static string FindProgram(string file)
    {
        string stem = Path.GetFileNameWithoutExtension(file);
        foreach (var p in System.Diagnostics.Process.GetProcessesByName(stem))
        {
            try { if (p.MainModule?.FileName is string path && path.Length > 0) return path; }
            catch { }
            finally { p.Dispose(); }
        }
        foreach (var folder in new[] { Environment.SpecialFolder.CommonPrograms, Environment.SpecialFolder.Programs })
        {
            try
            {
                foreach (string link in Directory.EnumerateFiles(Environment.GetFolderPath(folder), "*.lnk", SearchOption.AllDirectories))
                {
                    string target = ShellIcons.ExecutablePath(link);
                    if (string.Equals(Path.GetFileName(target), file, StringComparison.OrdinalIgnoreCase)) return target;
                }
            }
            catch { }
        }
        foreach (var root in new[] { Microsoft.Win32.Registry.CurrentUser, Microsoft.Win32.Registry.LocalMachine })
        {
            try
            {
                using var key = root.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\App Paths\" + file);
                if (key?.GetValue(null) is string path && File.Exists(path.Trim('"'))) return path.Trim('"');
            }
            catch { }
        }
        return "";
    }

    // Off the UI thread: finding the program reads the start menu's shortcuts.
    private async Task LoadRowIconAsync(AppRowVm vm)
    {
        if (vm.Bundled.Length > 0)
        {
            vm.Icon = new BitmapImage(new Uri(Path.Combine(AppContext.BaseDirectory, "Assets", "Apps", vm.Bundled + ".png"))) { DecodePixelWidth = 96 };
            return;
        }
        string source = vm.IconSource;
        if (source.Length == 0 && vm.Programs.Length > 0)
            source = vm.IconSource = await Task.Run(() => Program(vm.Programs));
        if (source.Length == 0 && vm.Folder.Length > 0)
            source = vm.IconSource = await Task.Run(() => GameFinder.MainProgram(vm.Folder));
        if (source.Length == 0) return;
        if (vm.Describe && source.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
        {
            string description = await Task.Run(() => Description(source));
            if (description.Length > 0) vm.Title = description;
        }
        var image = await IconFor(source);
        if (image == null && vm.Programs.Length > 0)
        {
            string fallback = await Task.Run(() => Program(vm.Programs));
            if (fallback.Length > 0 && !string.Equals(fallback, source, StringComparison.OrdinalIgnoreCase))
            {
                source = vm.IconSource = fallback;
                if (vm.Describe && source.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                {
                    string description = await Task.Run(() => Description(source));
                    if (description.Length > 0) vm.Title = description;
                }
                image = await IconFor(source);
            }
        }
        if (image != null) vm.Icon = image;
    }

    private async Task<ImageSource?> IconFor(string source)
    {
        if (_rowIcons.TryGetValue(source, out var image)) return image;
        var img = await ShellIcons.LoadAsync(source, 96);
        if (img == null) return null;
        var bmp = new WriteableBitmap(img.Width, img.Height);
        using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(img.Bgra, 0, img.Bgra.Length);
        bmp.Invalidate();
        return _rowIcons[source] = bmp;
    }

    private string _notifyCorner = "topRight";

    private void OnNotifyPlacementChecked(object sender, RoutedEventArgs e)
    {
        bool corner = (string)((RadioButton)sender).Tag == "corner";
        NotifyCornerRow.Visibility = corner ? Visibility.Visible : Visibility.Collapsed;
        if (_loading) return;
        // A corner: the one chosen last (top right at first, as before there were four).
        CurrentSettings.NotifyPlacement = corner ? _notifyCorner : "dock";
        _notifyEntrancePending = true;
        Changed();
    }

    private void OnNotifyCornerChecked(object sender, RoutedEventArgs e)
    {
        _notifyCorner = (string)((RadioButton)sender).Tag;
        if (_loading || CurrentSettings.NotifyPlacement == _notifyCorner) return;
        CurrentSettings.NotifyPlacement = _notifyCorner;
        _notifyEntrancePending = true;
        Changed();
    }

    private readonly Dictionary<string, ImageSource> _rowIcons = new(StringComparer.OrdinalIgnoreCase);
}
