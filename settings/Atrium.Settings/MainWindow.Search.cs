using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

// The sidebar's search: finds a setting by the text its page already shows, then opens that page
// and brings the element into view.
public sealed partial class MainWindow
{
    private sealed record SearchHit(int Page, string PageTitle, string Text, FrameworkElement? Target)
    {
        public override string ToString() => Target == null ? PageTitle : $"{PageTitle} › {Text}";
    }

    private void OnSettingsSearch(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args)
    {
        if (args.Reason != AutoSuggestionBoxTextChangeReason.UserInput) return;
        string query = sender.Text.Trim();
        sender.ItemsSource = query.Length == 0 ? null : SearchSettings(query);
    }

    private void OnSettingsSuggestionChosen(AutoSuggestBox sender, AutoSuggestBoxSuggestionChosenEventArgs args)
    {
        if (args.SelectedItem is not SearchHit hit) return;
        SelectPage(hit.Page);
    }

    private List<SearchHit> SearchSettings(string query)
    {
        var hits = new List<SearchHit>();
        for (int i = 0; i < _mains.Length && hits.Count < 40; i++)
        {
            string page = i < PageTitles.Length ? PageTitles[i] : "";
            if (page.Contains(query, StringComparison.CurrentCultureIgnoreCase))
                hits.Add(new SearchHit(i, page, page, null));
            CollectText(_mains[i], i, page, query, hits);
        }
        return hits;
    }

    // Every label, note and switch title of a page, wherever it sits.
    private static void CollectText(object root, int page, string pageTitle, string query, List<SearchHit> hits)
    {
        if (hits.Count >= 40) return;
        switch (root)
        {
            case ToggleTile tile when tile.Title.Contains(query, StringComparison.CurrentCultureIgnoreCase):
                hits.Add(new SearchHit(page, pageTitle, tile.Title, tile));
                return;
            case TextBlock text when !string.IsNullOrWhiteSpace(text.Text) &&
                                      text.Text.Contains(query, StringComparison.CurrentCultureIgnoreCase):
                hits.Add(new SearchHit(page, pageTitle, text.Text, text));
                return;
            case Panel panel:
                foreach (object child in panel.Children) CollectText(child, page, pageTitle, query, hits);
                break;
            case Border border:
                if (border.Child != null) CollectText(border.Child, page, pageTitle, query, hits);
                break;
            case ScrollViewer scroll:
                if (scroll.Content != null) CollectText(scroll.Content, page, pageTitle, query, hits);
                break;
            case ContentControl content when content.Content is not null and not string:
                CollectText(content.Content, page, pageTitle, query, hits);
                break;
        }
    }
}
