// Minimal UI objects isolate Loc.cs's traversal without opening an application.
using Microsoft.UI.Xaml;

namespace Microsoft.UI.Xaml { public class DependencyObject { } }
namespace Microsoft.UI.Xaml.Documents
{
    public class Inline : DependencyObject { }
    public class InlineCollection : List<Inline> { }
    public class Run : Inline { public string Text { get; set; } = ""; }
    public class Span : Inline { public InlineCollection Inlines { get; } = []; }
}
namespace Microsoft.UI.Xaml.Controls
{
    public class TextBlock : DependencyObject { public string Text { get; set; } = ""; public Documents.InlineCollection Inlines { get; } = []; }
    public class ContentControl : DependencyObject { public object? Content { get; set; } }
    public class UserControl : DependencyObject { public object? Content { get; set; } }
    public class ComboBoxItem : ContentControl { }
    public class ToggleSwitch : DependencyObject
    {
        public object? Header { get; set; }
        public object? OnContent { get; set; }
        public object? OffContent { get; set; }
    }
    public class ComboBox : DependencyObject
    {
        public object? Header { get; set; }
        public string PlaceholderText { get; set; } = "";
        public List<object> Items { get; } = [];
    }
    public class TextBox : DependencyObject { public object? Header { get; set; } public string PlaceholderText { get; set; } = ""; }
    public class MenuFlyoutItem : DependencyObject { public string Text { get; set; } = ""; }
    public class MenuFlyoutSubItem : DependencyObject { public string Text { get; set; } = ""; }
    public static class ToolTipService
    {
        public static object? GetToolTip(DependencyObject d) { _ = d; return null; }
        public static void SetToolTip(DependencyObject d, object? o) { _ = d; _ = o; }
    }
}
namespace Microsoft.UI.Xaml.Media
{
    public static class VisualTreeHelper
    {
        public static int GetChildrenCount(DependencyObject d) { _ = d; return 0; }
        public static DependencyObject GetChild(DependencyObject d, int i) => throw new NotSupportedException();
    }
}
namespace Microsoft.UI.Xaml.Automation
{
    public static class AutomationProperties
    {
        public static string GetName(DependencyObject d) { _ = d; return ""; }
        public static void SetName(DependencyObject d, string s) { _ = d; _ = s; }
    }
}
namespace Atrium.Settings
{
    public static class CoreClient { public static string SettingsPath => "unused-review-fixture.json"; }
    public class ToggleTile : Microsoft.UI.Xaml.Controls.UserControl
    {
        public string Title { get; set; } = "";
        public string Note { get; set; } = "";
        public string SwitchName { get; set; } = "";
    }
    public class NavLabel : DependencyObject { public string Text { get; set; } = ""; }
}
