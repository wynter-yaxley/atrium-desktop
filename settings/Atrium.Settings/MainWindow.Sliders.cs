namespace Atrium.Settings;

// The Material page's sliders: their group, UI range, and the mapping between the stored value and
// the slider value (percentages are stored as 0..1). Split from MainWindow.xaml.cs by its size.
public sealed partial class MainWindow
{
    private sealed record SliderDef(string Group, string Title, double Min, double Max, double Step, string Unit,
        Func<BarSettings, double> Get, Action<BarSettings, double> Set, bool Material);

    private static SliderDef[] Sliders =>
    [
        new("clarity", Loc.T("Transparency"), 0, 100, 1, "%",
            s => s.Transparency * 100, (s, v) => s.Transparency = v / 100, true),
        new("clarity", Loc.T("Blur"), 0, 100, 1, "%",
            s => s.Blur * 100, (s, v) => s.Blur = v / 100, true),
        new("edge", Loc.T("Refraction"), 0, 60, 1, "px",
            s => s.Refraction, (s, v) => s.Refraction = v, true),
        new("edge", Loc.T("Refraction edge"), 1, 80, 1, "px",
            s => s.Edge, (s, v) => s.Edge = v, true),
        new("edge", Loc.T("Dispersion"), 0, 50, 1, "%",
            s => s.Dispersion * 100, (s, v) => s.Dispersion = v / 100, true),
        // The rim's coverage transition, and how much wider it grows over light content: the two
        // knobs for a soft edge on white pages and a crisp one over dark wallpaper.
        new("edge", Loc.T("Rim softness"), 80, 300, 5, "%",
            s => s.EdgeSoftness * 100, (s, v) => s.EdgeSoftness = v / 100, true),
        new("edge", Loc.T("Rim over light"), 0, 150, 5, "%",
            s => s.EdgeLightBoost * 100, (s, v) => s.EdgeLightBoost = v / 100, true),
        new("light", Loc.T("Highlight"), 0, 100, 1, "%",
            s => s.Highlight * 100, (s, v) => s.Highlight = v / 100, true),
        new("light", Loc.T("Shadow"), 0, 100, 1, "%",
            s => s.Shadow * 100, (s, v) => s.Shadow = v / 100, true),

        new("size", Loc.T("Height"), 32, 120, 1, "px",
            s => s.Height, (s, v) => s.Height = v, false),
        new("size", Loc.T("Corner radius"), 0, 80, 1, "px",
            s => s.CornerRadius, (s, v) => s.CornerRadius = v, false),
        new("dock", Loc.T("Icon size"), 16, 64, 1, "px",
            s => s.IconSize, (s, v) => s.IconSize = v, false),
        new("width", Loc.T("Maximum width"), 300, 2400, 10, "px",
            s => s.Width, (s, v) => s.Width = v, false),
        new("dock", Loc.T("Edge spacing"), 0, 200, 1, "px",
            s => s.Margin, (s, v) => s.Margin = v, false),
        new("dock", Loc.T("Hover magnification"), 100, 200, 5, "%",
            s => s.Magnify * 100, (s, v) => s.Magnify = v / 100, false),

        // The band above the bar that answers the pointer ahead of time (riseNear, magnifyNear).
        new("near", Loc.T("Enter distance"), 2, 160, 2, "px",
            s => s.NearEnter, (s, v) => s.NearEnter = v, false),
        new("near", Loc.T("Leave distance"), 2, 200, 2, "px",
            s => s.NearLeave, (s, v) => s.NearLeave = v, false),

    ];
}
