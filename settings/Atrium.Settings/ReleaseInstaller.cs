using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.Json;

namespace Atrium.Settings;

// BITS owns downloads; its completion starts this executable in headless mode
// to validate/stage the package. Only the user's restart action replaces the app.
internal static class ReleaseInstaller
{
    internal sealed record Prepared(string Tag, string Version, string Directory);
    internal static string InstallDirectory => Path.GetDirectoryName(CoreClient.CoreExe()
        ?? throw new IOException(Loc.T("Could not find the Atrium program folder.")))!;
    internal static string CacheDirectory => Path.Combine(Path.GetDirectoryName(CoreClient.SettingsPath)!, "Updates",
        Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(InstallDirectory.ToUpperInvariant())))[..16]);
    private static string StatePath => Path.Combine(CacheDirectory, "ready.json");
    private static string DownloadPath => Path.Combine(CacheDirectory, "download.json");
    private static string WorkDirectory => Path.Combine(CacheDirectory, "package-background");
    internal static string ResultPath => Path.Combine(CacheDirectory, "result.json");
    private sealed record Download(ReleaseInfo Release, Guid Job, string? Error = null);
    internal sealed record TransferStatus(string Text, bool CanRetry = false);

    internal static Prepared? ReadPrepared(Version current) =>
        ReadPrepared(current, CacheDirectory, StatePath);

    internal static Prepared? ReadPrepared(
        Version current,
        string cacheDirectory,
        string statePath,
        Func<string, Version>? versionReader = null)
    {
        var ready = ReadJson<Prepared>(statePath);
        if (ready == null || !Version.TryParse(ready.Version, out var version) || version <= current) return null;
        string cacheRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(cacheDirectory)) + Path.DirectorySeparatorChar;
        if (!Path.GetFullPath(ready.Directory).StartsWith(cacheRoot, StringComparison.OrdinalIgnoreCase))
            throw new IOException(Loc.T("The update cache path is invalid. Download again."));
        ValidatePackage(ready.Directory, version, versionReader);
        return ready;
    }

    internal static Prepared? ReadPreparedRecovering(Version current) =>
        ReadPreparedRecovering(current, CacheDirectory, StatePath, WorkDirectory, App.LogCrash);

    internal static Prepared? ReadPreparedRecovering(
        Version current,
        string cacheDirectory,
        string statePath,
        string workDirectory,
        Action<Exception>? logError = null,
        Func<string, Version>? versionReader = null) =>
        ReleaseValidation.TryRecoverPrepared(
            () => ReadPrepared(current, cacheDirectory, statePath, versionReader), statePath, workDirectory, logError);

    internal static string? ReadFailure() =>
        ReleaseValidation.ReadFailureWithLock(ResultPath, @"Local\Atrium.Update." + Path.GetFileName(CacheDirectory), App.LogCrash);

    internal static Process StartBackgroundDownload(ReleaseInfo release)
    {
        var start = BackgroundStart("--queue-update");
        start.ArgumentList.Add(Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(JsonSerializer.Serialize(release))));
        return Process.Start(start) ?? throw new IOException(Loc.T("Could not start the background download. Please try again."));
    }

    internal static Process ResumeBackgroundDownload() => Process.Start(BackgroundStart("--resume-update"))
        ?? throw new IOException(Loc.T("Could not resume the background download. Try again."));

    private static ProcessStartInfo BackgroundStart(string command)
    {
        var start = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, CreateNoWindow = true };
        start.ArgumentList.Add(command);
        return start;
    }

    [SupportedOSPlatform("windows")]
    internal static TransferStatus? ReadTransferStatus()
    {
        var download = ReadDownload();
        if (download == null || download.Release.Version <= CurrentVersion) return null;
        if (download.Error != null) return new(Loc.T("Could not prepare the update. Try again."), true);
        using var job = BitsTransfer.Open(download.Job);
        if (job == null)
        {
            return File.Exists(Path.Combine(WorkDirectory, "release.zip"))
            ? new(Loc.T("Preparing the update…")) : new(Loc.T("The download did not finish. Try again."), true);
        }

        if (job.CurrentState == BitsTransfer.State.Error) return new(Loc.T("The background download failed. Try again."), true);
        if (job.CurrentState == BitsTransfer.State.Transferred) return new(Loc.T("Preparing the update…"));
        if (job.CurrentState == BitsTransfer.State.TransientError) return new(Loc.T("Waiting to resume the background download…"));
        var progress = job.CurrentProgress;
        return new(progress.Total is > 0 and < ulong.MaxValue
            ? Loc.F("Downloading in background… {0}%", Math.Min(100, progress.Transferred * 100 / progress.Total)) : Loc.T("Downloading in background…"));
    }

    private static Version CurrentVersion => typeof(ReleaseInstaller).Assembly.GetName().Version!;
    private static Download? ReadDownload() => ReadJson<Download>(DownloadPath);
    private static T? ReadJson<T>(string path)
    {
        try
        {
            // The worker atomically replaces/removes state as the UI reads progress.
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
            return JsonSerializer.Deserialize<T>(stream)
                ?? throw new JsonException(Loc.F("The update state is empty or incomplete: {0}", Path.GetFileName(path)));
        }
        catch (FileNotFoundException) { return default; }
        catch (DirectoryNotFoundException) { return default; }
    }
    private static void SaveJson<T>(string path, T value) => ReleaseValidation.SaveJsonAtomic(path, value);

    // Called before XAML starts. The mutex covers the short queue/finalize work,
    // not the transfer; closing any Atrium process never calls BITS.Cancel().
    [SupportedOSPlatform("windows")]
    internal static int RunBackground(string[] args)
    {
        Download? pending = null;
        string? cache = null;
        Mutex? mutex = null;
        bool acquired = false;
        try
        {
            cache = CacheDirectory;
            Directory.CreateDirectory(cache);
            mutex = new Mutex(false, @"Local\Atrium.Update." + Path.GetFileName(cache));
            try
            {
                if (!mutex.WaitOne(TimeSpan.FromSeconds(30)))
                    throw new IOException(Loc.T("Could not prepare the update. Try again."));
            }
            catch (AbandonedMutexException) { }
            acquired = true;
            // Read once. Error reporting must not depend on the same corrupt or
            // inaccessible state that brought us into the catch branch.
            pending = ReadDownload();
            if (args[0] == "--queue-update")
            {
                var release = JsonSerializer.Deserialize<ReleaseInfo>(Convert.FromBase64String(args[1]))
                    ?? throw new JsonException("Missing release.");
                Queue(release, pending);
            }
            else if (args[0] == "--resume-update")
            {
                if (pending != null) Queue(pending.Release, pending);
                else NotifyReady();
            }
            else
            {
                Finish(pending);
            }

            return 0;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or COMException or JsonException
            or FormatException or InvalidOperationException or ArgumentException or InvalidDataException or NotSupportedException)
        {
            App.LogCrash(error);
            if (cache != null)
            {
                try
                {
                    // A known transfer keeps its identity. Unknown/unreadable
                    // state is left intact, including any recoverable package.
                    SaveJson(Path.Combine(cache, "result.json"), new { error = Loc.F("The update did not finish: {0}", error.Message) });
                    File.WriteAllText(Path.Combine(cache, "worker-error.txt"), error.ToString());
                    if (pending != null) SaveJson(DownloadPath, pending with { Error = error.Message });
                }
                catch (Exception reporting) when (reporting is IOException or UnauthorizedAccessException)
                { App.LogCrash(new AggregateException(Loc.T("Could not write the update error state"), error, reporting)); }
            }
            return args[0] == "--finish-update" ? 0 : 1; // permanent failures wait for a retry, not callback loops
        }
        finally
        {
            if (acquired) mutex!.ReleaseMutex();
            mutex?.Dispose();
        }
    }

    [SupportedOSPlatform("windows")]
    private static void Queue(ReleaseInfo release, Download? pending)
    {
        if (release.Download == null || release.Version <= CurrentVersion) return;
        if (ReadPreparedRecovering(CurrentVersion) is { } ready &&
            Version.Parse(ready.Version) >= release.Version)
        { NotifyReady(); return; }
        if (pending != null && pending.Release.Version > release.Version) release = pending.Release;
        if (release.Download == null) return;
        if (pending != null && pending.Release.Version >= release.Version && pending.Error == null)
        {
            bool finish = false;
            using (var existing = BitsTransfer.Open(pending.Job))
            {
                if (existing != null)
                {
                    existing.OnTransferred(Environment.ProcessPath!);
                    finish = existing.CurrentState == BitsTransfer.State.Transferred;
                    if (!finish) { existing.Resume(); return; }
                }
            }
            if (finish || File.Exists(Path.Combine(WorkDirectory, "release.zip"))) { Finish(pending); return; }
        }
        if (pending != null)
        {
            using var previous = BitsTransfer.Open(pending.Job);
            previous?.Cancel(); // only this install's superseded or failed transfer
        }
        if (Directory.Exists(WorkDirectory)) Directory.Delete(WorkDirectory, true);
        File.Delete(StatePath);
        Directory.CreateDirectory(WorkDirectory);
        using var job = BitsTransfer.Create("Atrium update " + Path.GetFileName(CacheDirectory));
        try
        {
            SaveJson(DownloadPath, new Download(release, job.Id));
            job.Add(release.Download, Path.Combine(WorkDirectory, "release.zip"));
            job.OnTransferred(Environment.ProcessPath!);
            job.Resume();
        }
        catch { job.Cancel(); throw; }
    }

    [SupportedOSPlatform("windows")]
    private static void Finish(Download? pending)
    {
        if (pending == null) { NotifyReady(); return; }
        using (var job = BitsTransfer.Open(pending.Job))
        {
            if (job != null)
            {
                if (job.CurrentState == BitsTransfer.State.Error)
                { SaveJson(DownloadPath, pending with { Error = Loc.T("The background download failed") }); return; }
                if (job.CurrentState != BitsTransfer.State.Transferred) return;
                job.Complete();
            }
        }
        string zip = Path.Combine(WorkDirectory, "release.zip");
        try
        {
            ReleaseValidation.VerifyZipDigest(zip, pending.Release.Digest, LocalizeValidation);
        }
        catch (IOException)
        {
            File.Delete(zip); // a retry downloads it again
            throw;
        }
        string unpacked = Path.Combine(WorkDirectory, "unpacked");
        if (Directory.Exists(unpacked)) Directory.Delete(unpacked, true); // an interrupted extraction, not transfer data
        try
        {
            ReleaseValidation.ExtractZipSafely(zip, unpacked);
        }
        catch (InvalidDataException)
        {
            File.Delete(zip); // damaged: a retry downloads it again
            throw;
        }
        string root = File.Exists(Path.Combine(unpacked, "Atrium.exe")) ? unpacked : Path.Combine(unpacked, "Atrium");
        ValidatePackage(root, pending.Release.Version);
        SaveJson(StatePath, new Prepared(pending.Release.Tag, pending.Release.Version.ToString(), root));
        File.Delete(DownloadPath);
        File.Delete(zip);
        NotifyReady();
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindow(string cls, string? title);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint process);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, uint message, UIntPtr wp, IntPtr lp);

    [SupportedOSPlatform("windows")]
    internal static bool TryNotifyReadyViaPipe(string pipeName, string expectedCoreExe, int timeoutMs = 1000)
    {
        try
        {
            using var pipe = new NamedPipeClientStream(
                ".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous, TokenImpersonationLevel.Identification);
            pipe.Connect(timeoutMs);
            if (OperatingSystem.IsWindows())
                pipe.ReadMode = PipeTransmissionMode.Message;
            if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out uint serverPid) || serverPid == 0)
                return false;
            if (!string.Equals(CoreClient.TryGetProcessPath((int)serverPid), expectedCoreExe, StringComparison.OrdinalIgnoreCase))
                return false;
            byte[] payload = Encoding.UTF8.GetBytes("""{"cmd":"notify-update-ready"}""");
            using var cts = new CancellationTokenSource(timeoutMs);
            pipe.WriteAsync(payload, 0, payload.Length, cts.Token).GetAwaiter().GetResult();
            pipe.FlushAsync(cts.Token).GetAwaiter().GetResult();
            byte[] reply = new byte[1024];
            int read = pipe.ReadAsync(reply, 0, reply.Length, cts.Token).GetAwaiter().GetResult();
            if (read <= 0)
                return false;
            try
            {
                using var doc = JsonDocument.Parse(reply.AsMemory(0, read));
                var root = doc.RootElement;
                return root.ValueKind == JsonValueKind.Object
                    && root.TryGetProperty("ok", out var ok)
                    && ok.ValueKind == JsonValueKind.True
                    && root.TryGetProperty("status", out var status)
                    && status.ValueKind == JsonValueKind.String
                    && string.Equals(status.GetString(), "accepted", StringComparison.Ordinal);
            }
            catch (JsonException)
            {
                return false;
            }
        }
        catch (Exception e) when (e is TimeoutException or IOException or UnauthorizedAccessException
            or OperationCanceledException or PlatformNotSupportedException)
        {
            return false;
        }
    }

    [SupportedOSPlatform("windows")]
    private static void NotifyReady()
    {
        string? corePath = CoreClient.CoreExe();
        if (string.IsNullOrEmpty(corePath)) return;
        string lifecyclePipe = "Atrium.Lifecycle." + Process.GetCurrentProcess().SessionId;
        if (TryNotifyReadyViaPipe(lifecyclePipe, corePath)) return;
        IntPtr host = FindWindow("AtriumCoreHost", null);
        if (host == IntPtr.Zero || GetWindowThreadProcessId(host, out uint pid) == 0) return;
        if (string.Equals(CoreClient.TryGetProcessPath((int)pid), corePath, StringComparison.OrdinalIgnoreCase))
            PostMessage(host, 0x8000 + 11, UIntPtr.Zero, IntPtr.Zero);
    }

    private static void ValidatePackage(string directory, Version expected, Func<string, Version>? versionReader = null) =>
        ReleaseValidation.ValidatePackage(directory, expected, versionReader, LocalizeValidation);

    private static string LocalizeValidation(string key) => key switch
    {
        "The update package failed verification. Download again." => Loc.T("The update package failed verification. Download again."),
        "The update package is missing program files. Download again." => Loc.T("The update package is missing program files. Download again."),
        "The update package does not match the release. Download again." => Loc.T("The update package does not match the release. Download again."),
        _ => key,
    };

    internal static void Restart(Prepared ready, bool startCore)
    {
        ValidatePackage(ready.Directory, Version.Parse(ready.Version));
        // Copy the helper out of the install directory before replacing that directory.
        string script = Path.Combine(CacheDirectory, "Update.ps1");
        string packagedScript = Path.Combine(ready.Directory, "Settings", "Update.ps1");
        string sourceScript = File.Exists(packagedScript) ? packagedScript : Path.Combine(AppContext.BaseDirectory, "Update.ps1");
        // Windows PowerShell 5.1 needs a BOM to read the helper's Chinese messages.
        File.WriteAllText(script, File.ReadAllText(sourceScript), new System.Text.UTF8Encoding(true));
        var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
            "WindowsPowerShell", "v1.0", "powershell.exe"))
        { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = CacheDirectory };
        foreach (string arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
            "-InstallDirectory", InstallDirectory, "-PackageDirectory", ready.Directory, "-StatePath", StatePath,
            "-WaitForProcessId", Environment.ProcessId.ToString(), "-StartCore", startCore ? "1" : "0" })
        {
            start.ArgumentList.Add(arg);
        }
        // Check before closing any UI. Program Files installations need a writable
        // parent directory; do not stop the app and then discover access was denied.
        string probe = Path.Combine(Path.GetDirectoryName(InstallDirectory)!, ".atrium-update-" + Guid.NewGuid().ToString("N"));
        using (File.Create(probe)) { }
        File.Delete(probe);
        using var helper = Process.Start(start) ?? throw new IOException(Loc.T("Could not start the update. Try again."));
    }
}
