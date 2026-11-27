using System.Numerics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace Atrium.Settings;

// The control centre's optional modules (controlModules), as the widget gallery: a card for each, a click
// adds it to the control centre (at the end) or takes it away again.
public sealed partial class MainWindow
{
    // ControlCenter's kSpecs (native/core/ui/control_modules.cpp).
    private static string ModuleGlyph(string id) => id switch
    {
        "stage" => "\uE7C4",
        "energysaver" => "\uEC0A",
        "darkmode" => "\uE708",
        "hotspot" => "\uE88A",
        "dnd" => "\uE7ED",
        "airplane" => "\uE709",
        _ => "\uE88A",
    };

    private static string ModuleTitle(string id) => id switch
    {
        "stage" => "Stage Manager",
        "energysaver" => "Energy saver",
        "darkmode" => "Dark mode",
        "hotspot" => "Mobile hotspot",
        "dnd" => "Do Not Disturb",
        "airplane" => "Airplane mode",
        _ => "Mobile hotspot",
    };

    private static string ModuleNote(string id) => id switch
    {
        "stage" => "Turn Stage Manager on or off",
        "energysaver" => "Save battery",
        "darkmode" => "Windows and apps in dark colours",
        "hotspot" => "Share this PC's internet",
        "dnd" => "Silence notification popups",
        "airplane" => "Turn off all wireless radios",
        _ => "Share this PC's internet",
    };

    private sealed record ModuleCardHandles(
        Button Button,
        Ellipse Disc,
        FontIcon Glyph,
        TextBlock Title,
        TextBlock State,
        FontIcon Badge);

    private readonly Dictionary<string, ModuleCardHandles> _moduleCards = [];

    private void BuildModuleGallery()
    {
        _moduleCards.Clear();
        ModuleGallery.Children.Clear();
        foreach (string id in BarSettings.ControlModuleIds)
            ModuleGallery.Children.Add(CreateModuleCard(id));
    }

    private Button CreateModuleCard(string id)
    {
        var disc = new Ellipse { Width = 36, Height = 36 };
        var glyph = new FontIcon { Glyph = ModuleGlyph(id), FontSize = 15 };
        var mark = new Grid { Width = 36, Height = 36, VerticalAlignment = VerticalAlignment.Center, Children = { disc, glyph } };
        var title = new TextBlock { Style = (Style)Application.Current.Resources["RowTitleStyle"] };
        var state = new TextBlock
        {
            Style = (Style)Application.Current.Resources["FootnoteStyle"],
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var words = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center, Children = { title, state } };
        var badge = new FontIcon { FontSize = 13, VerticalAlignment = VerticalAlignment.Center };
        var body = new Grid { ColumnSpacing = 12 };
        body.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        body.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(words, 1);
        Grid.SetColumn(badge, 2);
        body.Children.Add(mark);
        body.Children.Add(words);
        body.Children.Add(badge);
        var button = new Button
        {
            Content = body,
            Padding = new Thickness(14, 12, 14, 12),
            CornerRadius = new CornerRadius(22),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Background = Tokens.Brush("CardBrush"),
            TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(180) },
        };
        button.PointerEntered += (_, _) => button.Translation = new Vector3(0, -2, 0);
        button.PointerExited += (_, _) => button.Translation = Vector3.Zero;
        button.Click += (_, _) => ToggleModule(id);
        _moduleCards[id] = new ModuleCardHandles(button, disc, glyph, title, state, badge);
        UpdateModuleCard(id);
        return button;
    }

    private void UpdateModuleCard(string id)
    {
        if (!_moduleCards.TryGetValue(id, out var card)) return;
        bool added = CurrentSettings.ControlModules.Contains(id);
        card.Disc.Fill = Tokens.Brush(added ? "AccentBrush" : "FillBrush");
        card.Glyph.Foreground = added ? new SolidColorBrush(Microsoft.UI.Colors.White) : Tokens.Brush("InkBrush");
        card.Title.Text = Loc.T(ModuleTitle(id));
        card.State.Text = added ? Loc.T("In the control centre") : Loc.T(ModuleNote(id));
        card.Badge.Glyph = added ? "\uE73E" : "\uE710";
        card.Button.Background = Tokens.Brush("CardBrush");
        card.Button.BorderBrush = Tokens.Brush(added ? "AccentBrush" : "CardStrokeBrush");
        card.Button.BorderThickness = new Thickness(added ? 1.5 : 1);
        AutomationProperties.SetName(card.Button,
            added ? Loc.F("Remove {0} from the control centre", card.Title.Text) : Loc.F("Add {0} to the control centre", card.Title.Text));
    }

    private void RefreshModuleGallery()
    {
        foreach (string id in _moduleCards.Keys)
            UpdateModuleCard(id);
    }

    private void ToggleModule(string id)
    {
        var modules = CurrentSettings.ControlModules;
        if (!modules.Remove(id)) modules.Add(id);
        Changed();
        UpdateModuleCard(id);
    }
}
