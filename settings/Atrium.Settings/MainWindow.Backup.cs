using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace Atrium.Settings;

// Settings backup: one file with every setting, for moving a configuration to another computer.
// The start menu's tiles and the widgets' layout live in their own files and stay behind.
public sealed partial class MainWindow
{
    private async void OnExportSettings(object sender, RoutedEventArgs e)
    {
        if (_dialog != null) return;
        var picker = new FileSavePicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        picker.FileTypeChoices.Add("JSON", [".json"]);
        picker.SuggestedFileName = $"Atrium-settings-{DateTime.Now:yyyyMMdd}";
        StorageFile? file;
        try { file = await picker.PickSaveFileAsync(); }
        catch (COMException)
        {
            ShowPickerUnavailable();
            return;
        }
        if (file == null) return;
        try
        {
            string text = CurrentSettings.ToJson().ToJsonString(new JsonSerializerOptions { WriteIndented = true });
            await FileIO.WriteTextAsync(file, text);
            ShowMessage(InfoBarSeverity.Success, Loc.T("Settings exported"), file.Name);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            ShowMessage(InfoBarSeverity.Error, Loc.T("Could not write the file"), error.Message);
        }
    }

    private async void OnImportSettings(object sender, RoutedEventArgs e)
    {
        if (_dialog != null) return;
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        picker.FileTypeFilter.Add(".json");
        StorageFile? file;
        try { file = await picker.PickSingleFileAsync(); }
        catch (COMException)
        {
            ShowPickerUnavailable();
            return;
        }
        if (file == null) return;
        BarSettings imported;
        try
        {
            string text = await FileIO.ReadTextAsync(file);
            if (JsonNode.Parse(text) is not JsonObject parsed || !LooksLikeSettings(parsed))
                throw new FormatException();
            imported = BarSettings.FromJson(parsed, new());
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or FormatException or InvalidOperationException)
        {
            ShowMessage(InfoBarSeverity.Error, Loc.T("That file is not an Atrium settings file"), file.Name);
            return;
        }
        var dialog = new ContentDialog
        {
            Title = Loc.T("Import settings?"),
            Content = Loc.T("Every setting is replaced with the file's. The start menu and widget layouts are not included."),
            PrimaryButtonText = Loc.T("Import"),
            CloseButtonText = Loc.T("Cancel"),
            DefaultButton = ContentDialogButton.Primary,
        };
        if (await AskAsync(dialog) != ContentDialogResult.Primary) return;
        _session.Import(imported);
        LoadIntoControls();
        Changed();
        if (await SaveAsync())
            ShowMessage(InfoBarSeverity.Success, Loc.T("Settings imported"), file.Name);
        else
            ShowMessage(InfoBarSeverity.Warning, Loc.T("Imported, but not saved"), Loc.T("Save the settings from the title bar to keep them."));
    }

    private void ShowPickerUnavailable() =>
        ShowMessage(InfoBarSeverity.Warning, Loc.T("The file picker is unavailable"),
            Loc.T("Windows does not show it to apps run as administrator. Open the settings without administrator rights."));

    // A file that has none of the settings' own keys is not one of ours (a photo, another app's json).
    private static bool LooksLikeSettings(JsonObject o) =>
        o.ContainsKey("version") || o.ContainsKey("transparency") || o.ContainsKey("pins") || o.ContainsKey("showClock");

    // The Backup group is built here rather than in XAML: the page's markup stays under its line
    // budget, and its buttons call the pickers above anyway.
    private void BuildBackupSection()
    {
        var res = Application.Current.Resources;
        var title = new TextBlock { Text = Loc.T("Backup"), Style = (Style)res["SectionStyle"] };
        var heading = new TextBlock { Text = Loc.T("Settings backup"), Style = (Style)res["RowTitleStyle"] };
        var note = new TextBlock
        {
            Text = Loc.T("Move your settings to another computer: export them to a file here, then import the file there."),
            Style = (Style)res["RowNoteStyle"],
            TextWrapping = TextWrapping.Wrap,
        };
        var export = new Button { Content = Loc.T("Export settings…") };
        export.Click += OnExportSettings;
        var import = new Button { Content = Loc.T("Import settings…") };
        import.Click += OnImportSettings;
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, Children = { export, import } };
        var body = new StackPanel { Spacing = 12, Children = { heading, note, buttons } };
        var section = new StackPanel { Spacing = 10 };
        section.Children.Add(title);
        section.Children.Add(new Border { Style = (Style)res["TileStyle"], Child = body });
        MainSystem.Children.Add(section);
    }
}
