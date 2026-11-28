using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace Atrium.Settings;

internal enum NativeFileDialogPreset
{
    Program,
    Files,
    Folder,
    Picture,
}

[Flags]
internal enum NativeFileDialogOptions : uint
{
    None = 0,
    NoChangeDir = 0x00000008,
    PickFolders = 0x00000020,
    ForceFileSystem = 0x00000040,
    AllowMultiselect = 0x00000200,
    PathMustExist = 0x00000800,
    FileMustExist = 0x00001000,
    NoDereferenceLinks = 0x00100000,
}

// Native Win32 STA COM IFileOpenDialog wrapper so file and folder pickers work both elevated and unelevated.
internal static class NativeFileDialog
{
    internal const int ErrorCancelledHResult = unchecked((int)0x800704C7);
    private const uint ClsctxInprocServer = 0x1;
    private const uint CoinitApartmentThreaded = 0x2;
    private const int RpcEChangedMode = unchecked((int)0x80010106);
    private const uint SigdnFileSysPath = 0x80058000;
    private static readonly Guid ClsidFileOpenDialog = new("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7");
    private static int _dialogOpen;

    internal static bool IsCancelledHResult(int hr) => hr == ErrorCancelledHResult;

    internal static NativeFileDialogOptions OptionsFor(NativeFileDialogPreset preset) => preset switch
    {
        NativeFileDialogPreset.Program => NativeFileDialogOptions.ForceFileSystem
            | NativeFileDialogOptions.FileMustExist
            | NativeFileDialogOptions.NoDereferenceLinks
            | NativeFileDialogOptions.NoChangeDir,
        NativeFileDialogPreset.Files => NativeFileDialogOptions.ForceFileSystem
            | NativeFileDialogOptions.FileMustExist
            | NativeFileDialogOptions.AllowMultiselect
            | NativeFileDialogOptions.NoChangeDir,
        NativeFileDialogPreset.Folder => NativeFileDialogOptions.PickFolders
            | NativeFileDialogOptions.ForceFileSystem
            | NativeFileDialogOptions.PathMustExist
            | NativeFileDialogOptions.NoChangeDir,
        NativeFileDialogPreset.Picture => NativeFileDialogOptions.ForceFileSystem
            | NativeFileDialogOptions.FileMustExist
            | NativeFileDialogOptions.NoChangeDir,
        _ => throw new ArgumentOutOfRangeException(nameof(preset), preset, null),
    };

    internal static (string Name, string Spec)[] FiltersFor(NativeFileDialogPreset preset, string? filterLabel = null) => preset switch
    {
        NativeFileDialogPreset.Program => [(string.IsNullOrWhiteSpace(filterLabel) ? "Apps" : filterLabel, "*.exe;*.lnk")],
        NativeFileDialogPreset.Files => [(string.IsNullOrWhiteSpace(filterLabel) ? "Files" : filterLabel, "*.*")],
        NativeFileDialogPreset.Folder => [],
        NativeFileDialogPreset.Picture => [(string.IsNullOrWhiteSpace(filterLabel) ? "Images" : filterLabel, PictureSpec)],
        _ => throw new ArgumentOutOfRangeException(nameof(preset), preset, null),
    };

    // The picture formats WIC reads out of the box (native/core/shell/shell_icon.cpp: LoadPictureIcon).
    private const string PictureSpec = "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.ico;*.webp;*.tif;*.tiff";

    [SupportedOSPlatform("windows")]
    internal static string? PickProgram(IntPtr ownerHwnd, string? filterLabel = null, Action<Exception>? onError = null)
    {
        string[] paths = ShowDialog(ownerHwnd, NativeFileDialogPreset.Program, filterLabel, onError);
        return paths.Length > 0 ? paths[0] : null;
    }

    // One picture file (the Launchpad entry's icon): null when cancelled.
    [SupportedOSPlatform("windows")]
    internal static string? PickPicture(IntPtr ownerHwnd, string? filterLabel = null, Action<Exception>? onError = null)
    {
        string[] paths = ShowDialog(ownerHwnd, NativeFileDialogPreset.Picture, filterLabel, onError);
        return paths.Length > 0 ? paths[0] : null;
    }

    [SupportedOSPlatform("windows")]
    internal static string[] PickFiles(IntPtr ownerHwnd, string? filterLabel = null, Action<Exception>? onError = null) =>
        ShowDialog(ownerHwnd, NativeFileDialogPreset.Files, filterLabel, onError);

    [SupportedOSPlatform("windows")]
    internal static string? PickFolder(IntPtr ownerHwnd, Action<Exception>? onError = null)
    {
        string[] paths = ShowDialog(ownerHwnd, NativeFileDialogPreset.Folder, null, onError);
        return paths.Length > 0 ? paths[0] : null;
    }

    [SupportedOSPlatform("windows")]
    private static string[] ShowDialog(IntPtr ownerHwnd, NativeFileDialogPreset preset, string? filterLabel, Action<Exception>? onError)
    {
        if (Interlocked.CompareExchange(ref _dialogOpen, 1, 0) != 0)
            return [];

        try
        {
            int coHr = CoInitializeEx(IntPtr.Zero, CoinitApartmentThreaded);
            if (coHr is < 0 and not RpcEChangedMode)
            {
                ReportHResult(coHr, onError);
                return [];
            }

            try
            {
                return RunDialog(ownerHwnd, preset, filterLabel, onError);
            }
            catch (COMException ex) when (IsCancelledHResult(ex.HResult))
            {
                return [];
            }
            catch (COMException ex)
            {
                ReportException(ex, onError);
                return [];
            }
            finally
            {
                if (coHr >= 0) CoUninitialize();
            }
        }
        finally
        {
            Interlocked.Exchange(ref _dialogOpen, 0);
        }
    }

    [SupportedOSPlatform("windows")]
    private static string[] RunDialog(IntPtr ownerHwnd, NativeFileDialogPreset preset, string? filterLabel, Action<Exception>? onError)
    {
        var iid = typeof(IFileOpenDialog).GUID;
        int hr = CoCreateInstance(ClsidFileOpenDialog, IntPtr.Zero, ClsctxInprocServer, iid, out var dialog);
        if (hr < 0 || dialog == null)
        {
            ReportHResult(hr, onError);
            return [];
        }

        try
        {
            hr = dialog.SetOptions(OptionsFor(preset));
            if (hr < 0)
            {
                ReportHResult(hr, onError);
                return [];
            }

            var filters = FiltersFor(preset, filterLabel);
            if (filters.Length > 0)
            {
                FilterSpec[] specs = [.. filters.Select(f => new FilterSpec(f.Name, f.Spec))];
                hr = dialog.SetFileTypes((uint)specs.Length, specs);
                if (hr < 0)
                {
                    ReportHResult(hr, onError);
                    return [];
                }
                _ = dialog.SetFileTypeIndex(1);
            }

            hr = dialog.Show(ownerHwnd);
            if (IsCancelledHResult(hr)) return [];
            if (hr < 0)
            {
                ReportHResult(hr, onError);
                return [];
            }

            return preset == NativeFileDialogPreset.Files
                ? ReadMultiplePaths(dialog, onError)
                : ReadSinglePath(dialog, onError);
        }
        finally
        {
            Marshal.FinalReleaseComObject(dialog);
        }
    }

    [SupportedOSPlatform("windows")]
    private static string[] ReadSinglePath(IFileOpenDialog dialog, Action<Exception>? onError)
    {
        int hr = dialog.GetResult(out var item);
        if (hr < 0 || item == null)
        {
            ReportHResult(hr, onError);
            return [];
        }

        try
        {
            string? path = ReadShellItemPath(item, onError);
            return path is { Length: > 0 } ? [path] : [];
        }
        finally
        {
            Marshal.FinalReleaseComObject(item);
        }
    }

    [SupportedOSPlatform("windows")]
    private static string[] ReadMultiplePaths(IFileOpenDialog dialog, Action<Exception>? onError)
    {
        int hr = dialog.GetResults(out var items);
        if (hr < 0 || items == null)
        {
            ReportHResult(hr, onError);
            return [];
        }

        try
        {
            hr = items.GetCount(out uint count);
            if (hr < 0)
            {
                ReportHResult(hr, onError);
                return [];
            }

            var paths = new List<string>((int)count);
            for (uint i = 0; i < count; i++)
            {
                hr = items.GetItemAt(i, out var item);
                if (hr < 0 || item == null)
                {
                    ReportHResult(hr, onError);
                    continue;
                }

                try
                {
                    string? path = ReadShellItemPath(item, onError);
                    if (path is { Length: > 0 }) paths.Add(path);
                }
                finally
                {
                    Marshal.FinalReleaseComObject(item);
                }
            }
            return [.. paths];
        }
        finally
        {
            Marshal.FinalReleaseComObject(items);
        }
    }

    [SupportedOSPlatform("windows")]
    private static string? ReadShellItemPath(IShellItem item, Action<Exception>? onError)
    {
        int hr = item.GetDisplayName(SigdnFileSysPath, out IntPtr pszPath);
        if (hr < 0 || pszPath == IntPtr.Zero)
        {
            ReportHResult(hr, onError);
            return null;
        }

        try
        {
            return Marshal.PtrToStringUni(pszPath);
        }
        finally
        {
            Marshal.FreeCoTaskMem(pszPath);
        }
    }

    private static void ReportHResult(int hr, Action<Exception>? onError)
    {
        if (hr >= 0) hr = unchecked((int)0x80004005); // E_FAIL when a COM call returns null with S_OK
        var ex = Marshal.GetExceptionForHR(hr) ?? new InvalidOperationException($"NativeFileDialog failed with HRESULT 0x{hr:X8}");
        ReportException(ex, onError);
    }

    private static void ReportException(Exception ex, Action<Exception>? onError)
    {
        Trace.TraceError("NativeFileDialog failed: 0x{0:X8} ({1})", ex.HResult, ex.Message);
        onError?.Invoke(ex);
    }

    [DllImport("ole32.dll")]
    private static extern int CoInitializeEx(IntPtr reserved, uint coInit);

    [DllImport("ole32.dll")]
    private static extern void CoUninitialize();

    [DllImport("ole32.dll")]
    private static extern int CoCreateInstance(
        in Guid rclsid,
        IntPtr pUnkOuter,
        uint dwClsContext,
        in Guid riid,
        [MarshalAs(UnmanagedType.Interface)] out IFileOpenDialog? ppv);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private readonly struct FilterSpec(string name, string spec)
    {
        [MarshalAs(UnmanagedType.LPWStr)]
        public readonly string Name = name;

        [MarshalAs(UnmanagedType.LPWStr)]
        public readonly string Spec = spec;
    }

    [ComImport, Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItem
    {
        [PreserveSig] int BindToHandler(IntPtr pbc, in Guid bhid, in Guid riid, out IntPtr ppv);
        [PreserveSig] int GetParent(out IShellItem? ppsi);
        [PreserveSig] int GetDisplayName(uint sigdnName, out IntPtr ppszName);
        [PreserveSig] int GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
        [PreserveSig] int Compare(IShellItem psi, uint hint, out int piOrder);
    }

    [ComImport, Guid("b63ea76d-1f85-456f-a19c-48159efa858b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItemArray
    {
        [PreserveSig] int BindToHandler(IntPtr pbc, in Guid bhid, in Guid riid, out IntPtr ppv);
        [PreserveSig] int GetPropertyStore(int flags, in Guid riid, out IntPtr ppv);
        [PreserveSig] int GetPropertyDescriptionList(IntPtr keyType, in Guid riid, out IntPtr ppv);
        [PreserveSig] int GetAttributes(int attribFlags, uint sfgaoMask, out uint psfgaoAttribs);
        [PreserveSig] int GetCount(out uint pdwNumItems);
        [PreserveSig] int GetItemAt(uint dwIndex, out IShellItem? ppsi);
        [PreserveSig] int EnumItems(out IntPtr ppenumShellItems);
    }

    [ComImport, Guid("d57c7288-d4ad-4768-be02-9d969532d960"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IFileOpenDialog
    {
        // IModalWindow
        [PreserveSig] int Show(IntPtr hwndOwner);
        // IFileDialog
        [PreserveSig] int SetFileTypes(uint cFileTypes, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] FilterSpec[] rgFilterSpec);
        [PreserveSig] int SetFileTypeIndex(uint iFileType);
        [PreserveSig] int GetFileTypeIndex(out uint piFileType);
        [PreserveSig] int Advise(IntPtr pfde, out uint pdwCookie);
        [PreserveSig] int Unadvise(uint dwCookie);
        [PreserveSig] int SetOptions(NativeFileDialogOptions fos);
        [PreserveSig] int GetOptions(out NativeFileDialogOptions pfos);
        [PreserveSig] int SetDefaultFolder(IShellItem psi);
        [PreserveSig] int SetFolder(IShellItem psi);
        [PreserveSig] int GetFolder(out IShellItem? ppsi);
        [PreserveSig] int GetCurrentSelection(out IShellItem? ppsi);
        [PreserveSig] int SetFileName([MarshalAs(UnmanagedType.LPWStr)] string pszName);
        [PreserveSig] int GetFileName(out IntPtr pszName);
        [PreserveSig] int SetTitle([MarshalAs(UnmanagedType.LPWStr)] string pszTitle);
        [PreserveSig] int SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string pszText);
        [PreserveSig] int SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
        [PreserveSig] int GetResult(out IShellItem? ppsi);
        [PreserveSig] int AddPlace(IShellItem psi, int fdap);
        [PreserveSig] int SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string pszDefaultExtension);
        [PreserveSig] int Close(int hr);
        [PreserveSig] int SetClientGuid(in Guid guid);
        [PreserveSig] int ClearClientData();
        [PreserveSig] int SetFilter(IntPtr pFilter);
        // IFileOpenDialog
        [PreserveSig] int GetResults(out IShellItemArray? ppenum);
        [PreserveSig] int GetSelectedItems(out IShellItemArray? ppsai);
    }
}
