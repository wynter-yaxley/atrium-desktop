using System.Runtime.InteropServices;
using System.Security.Principal;
using Microsoft.UI.Xaml;
using Microsoft.Win32.SafeHandles;

namespace Atrium.Settings;

public partial class App : Application
{
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);
    [StructLayout(LayoutKind.Sequential)]
    private struct SecurityAttributes { public uint Length; public IntPtr SecurityDescriptor; public bool InheritHandle; }
    [StructLayout(LayoutKind.Sequential)]
    private struct ChangeFilterStruct { public uint Size; public uint ExtStatus; }

    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetProp(IntPtr hwnd, string name);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int MessageBox(IntPtr hwnd, string text, string title, uint type);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool ChangeWindowMessageFilterEx(IntPtr hwnd, uint message, uint action, ref ChangeFilterStruct changeFilterStruct);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(string sddl, uint revision, out IntPtr sd, out uint size);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateMutexW(ref SecurityAttributes sa, bool initialOwner, string name);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern IntPtr LocalFree(IntPtr hMem);
    internal const string WindowProp = "Atrium.Settings.Window";
    internal const uint OpenPageMessage = 0x8000 + 31;
    internal const uint CloseForUninstallMessage = 0x8000 + 32;

    public static string WindowTitle => Loc.T("Atrium Settings");
    internal static MainWindow? CurrentWindow { get; private set; }
    private static BarSettings? _pendingCurrent, _pendingSaved;
    private static Mutex? _instance;

    // ATRIUM_THEME=dark|light (development): that theme whatever Windows' is, in an instance of its own
    // (the other theme looked at beside the window in use).
    internal static readonly string? ThemeOverride =
        Environment.GetEnvironmentVariable("ATRIUM_THEME") is var theme and ("dark" or "light") ? theme : null;

    public App()
    {
        if (ThemeOverride != null)
            RequestedTheme = ThemeOverride == "dark" ? ApplicationTheme.Dark : ApplicationTheme.Light;
        InitializeComponent();
        // A crash leaves its exception in %LOCALAPPDATA%\Atrium\logs\settings.log (WinUI itself reports only 0xc000027b).
        UnhandledException += (_, e) => LogCrash(e.Exception);
    }

    public static void LogCrash(Exception e)
    {
        try
        {
            string dir = Path.Combine(CoreClient.DataDirectory, "logs");
            Directory.CreateDirectory(dir);
            File.AppendAllText(Path.Combine(dir, "settings.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {e}\n");
        }
        catch (ArgumentException) { }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        Loc.Load(); // also before the single-instance lookup: the title follows the language
        // Single instance: a second launch (tray menu, double click) brings the
        // existing window to the front instead of opening another one.
        string mutexName = @"Local\Atrium.Settings" + (ThemeOverride == null ? "" : "." + ThemeOverride);
        if (!TryAcquireInstanceMutex(mutexName, out _instance))
        {
            nint other = FindSettingsWindow();
            if (other != IntPtr.Zero)
            {
                _ = GetWindowThreadProcessId(other, out uint pid);
                bool same = string.Equals(CoreClient.TryGetProcessPath((int)pid), Environment.ProcessPath, StringComparison.OrdinalIgnoreCase);
                if (!same)
                {
                    _ = MessageBox(IntPtr.Zero,
                        Loc.T("Atrium Settings is running from another location. Close that window, then open the settings in this folder."),
                        "Atrium", 0x40);
                    Exit();
                    return;
                }
                ShowWindow(other, 9 /* SW_RESTORE */);
                SetForegroundWindow(other);
                if (PageFromArgs(Environment.GetCommandLineArgs()) is int page)
                    PostMessage(other, OpenPageMessage, (UIntPtr)page, IntPtr.Zero);
            }
            Exit();
            return;
        }
        try
        {
            CurrentWindow = new MainWindow();
            AttachWindowLifecycle(CurrentWindow);
            CurrentWindow.Activate();
        }
        catch (Exception e)
        {
            LogCrash(e);
            throw;
        }
    }

    // A language change re-creates the window: every XAML string is loaded
    // again in the new language. The snapshots keep edits made while the save
    // was on its way, so the new window can show them as unsaved.
    internal static void ReloadWindow(int page, BarSettings? current = null, BarSettings? saved = null)
    {
        _pendingCurrent = current?.Clone();
        _pendingSaved = saved?.Clone();
        var old = CurrentWindow;
        old?.ReleaseForReload();
        var window = new MainWindow(page);
        AttachWindowLifecycle(window);
        CurrentWindow = window;
        window.Activate();
        old?.Close();
    }

    // The new window takes the pending snapshots after reading the core's
    // saved settings; they were still unsaved when the old window closed.
    internal static (BarSettings? Current, BarSettings? Saved) TakePendingState()
    {
        var state = (_pendingCurrent, _pendingSaved);
        _pendingCurrent = _pendingSaved = null;
        return state;
    }

    internal static int? PageFromArgs(string[] args)
    {
        int i = Array.IndexOf(args, "--page");
        if (i < 0 || i + 1 >= args.Length) return null;
        return args[i + 1] switch
        {
            "material" => 0,
            "shape" => 1,
            "dock" => 2,
            "notify" => 3,
            "widgets" => 4,
            "stage" => 5,
            "displays" => 6,
            "system" => 7,
            "design" => 8,
            "about" => 9,
            "start" => 10,
            "tray" => 11,
            "status" => 12,
            "clock" => 13,
            "nowplaying" => 15,
            "stacks" => 16,
            "launchpad" => 17,
            "ime" => 14,
            _ => null,
        };
    }

    private static nint FindSettingsWindow()
    {
        nint found = IntPtr.Zero;
        EnumWindows((hwnd, _) =>
        {
            if (GetProp(hwnd, WindowProp) == IntPtr.Zero) return true;
            found = hwnd;
            return false;
        }, IntPtr.Zero);
        return found;
    }

    private static bool TryAcquireInstanceMutex(string name, out Mutex? mutex)
    {
        mutex = null;
        string? sid = WindowsIdentity.GetCurrent().User?.Value;
        if (!string.IsNullOrEmpty(sid))
        {
            // Least-privilege same-user DACL plus Medium integrity label so Medium and Elevated launches coordinate cleanly.
            string sddl = $"D:P(A;;0x001F0001;;;{sid})(A;;0x001F0001;;;SY)(A;;0x001F0001;;;BA)S:(ML;;NW;;;ME)";
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, out IntPtr sd, out _))
            {
                try
                {
                    var sa = new SecurityAttributes
                    {
                        Length = (uint)Marshal.SizeOf<SecurityAttributes>(),
                        SecurityDescriptor = sd,
                        InheritHandle = false,
                    };
                    IntPtr handle = CreateMutexW(ref sa, true, name);
                    int err = Marshal.GetLastWin32Error();
                    if (handle == IntPtr.Zero) return false;
                    if (err == 183 /* ERROR_ALREADY_EXISTS */) { CloseHandle(handle); return false; }
                    var sw = new SafeWaitHandle(handle, ownsHandle: true);
                    mutex = new Mutex(false) { SafeWaitHandle = sw };
                    return true;
                }
                finally { LocalFree(sd); }
            }
        }
        try
        {
            var fallback = new Mutex(true, name, out bool created);
            if (created) { mutex = fallback; return true; }
            fallback.Dispose();
            return false;
        }
        catch (UnauthorizedAccessException) { return false; }
    }

    private static void AttachWindowLifecycle(Window window)
    {
        window.Closed += (sender, _) =>
        {
            if (ReferenceEquals(sender, CurrentWindow))
            {
                _instance?.Dispose();
                _instance = null;
            }
        };
        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
        if (hwnd == IntPtr.Zero) return;
        const uint MsgFltAllow = 1;
        var filter = new ChangeFilterStruct { Size = (uint)Marshal.SizeOf<ChangeFilterStruct>() };
        _ = ChangeWindowMessageFilterEx(hwnd, OpenPageMessage, MsgFltAllow, ref filter);
        _ = ChangeWindowMessageFilterEx(hwnd, CloseForUninstallMessage, MsgFltAllow, ref filter);
    }

    internal static void SignalStartupReady(bool coreReady)
    {
        string[] command = Environment.GetCommandLineArgs();
        if (command.Contains("--require-core-ready") && !coreReady) return;
        int index = Array.IndexOf(command, "--ready-event");
        if (index < 0 || index + 1 >= command.Length || !command[index + 1].StartsWith(@"Local\Atrium.Update.Settings.", StringComparison.Ordinal)) return;
        try
        {
            using var ready = EventWaitHandle.OpenExisting(command[index + 1]);
            ready.Set();
        }
        catch (WaitHandleCannotBeOpenedException) { }
    }
}
