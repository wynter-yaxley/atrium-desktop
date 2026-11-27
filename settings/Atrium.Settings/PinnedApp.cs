namespace Atrium.Settings;

// One pinned app of the bar. Either Path (.lnk / .exe) or Aumid identifies it.
internal sealed record PinnedApp(string Name, string Path, string Args, string Aumid, bool? MultipleWindows = null)
{
    public bool AllowsMultipleWindows(string? resolvedPath = null)
    {
        if (MultipleWindows is bool multiple) return multiple;
        string exe = System.IO.Path.GetFileNameWithoutExtension(resolvedPath ?? Path).ToLowerInvariant();
        return exe is "explorer" or "chrome" or "msedge" or "firefox" or "brave" or "opera" or "vivaldi" or "chromium" ||
               Aumid.Equals("Microsoft.Windows.Explorer", StringComparison.OrdinalIgnoreCase);
    }
}
