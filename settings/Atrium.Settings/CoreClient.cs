using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.CSharp.RuntimeBinder;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace Atrium.Settings;

internal enum CommitStatus { Saved, Conflict, Error }

internal enum CorePrivilegeMode { Unknown, Standard, ElevatedSplit, ElevatedUnified, CrossAccountElevated }
internal enum AdminCardAction { StartAsAdmin, RestartAsAdmin, RestartAsStandard, None }
internal sealed record CorePrivilegeInfo(bool Elevated, CorePrivilegeMode Mode, bool CanDeElevate);
internal sealed record AdminCardState(string Status, string Note, string? ButtonText, AdminCardAction Action, bool ButtonVisible, bool ButtonEnabled);
internal sealed record LifecycleRestartResult(bool Succeeded, bool Reconnected, string? Error = null);

internal sealed record CoreState(BarSettings Saved, BarSettings Current, string? InstanceId, ulong Revision, bool FromCore);

internal sealed record CommitResult(
    CommitStatus Status,
    BarSettings? ServerSaved = null,
    string? InstanceId = null,
    ulong Revision = 0,
    string? Warning = null,
    string? Error = null)
{
    public bool Succeeded => Status == CommitStatus.Saved;
    public bool IsConflict => Status == CommitStatus.Conflict;
}

// Client of the Atrium.Core settings pipe (docs/SETTINGS-PROTOCOL.md). When the
// core is not running, settings are read from and written to the file directly.
internal sealed partial class CoreClient : IDisposable
{
    internal sealed partial class NativeProcessWaitHandle : WaitHandle
    {
        public NativeProcessWaitHandle(IntPtr handle) { SafeWaitHandle = new SafeWaitHandle(handle, ownsHandle: true); }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, uint processId);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, char[] exeName, ref uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();
    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr processHandle, uint desiredAccess, out IntPtr tokenHandle);
    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetTokenInformation(
        IntPtr tokenHandle, int tokenInformationClass, out uint tokenInformation, uint tokenInformationLength, out uint returnLength);

    internal static string? TryGetProcessPath(int pid) => pid > 0 ? TryGetProcessPath((uint)pid) : null;

    internal static string? TryGetProcessPath(uint pid)
    {
        const uint ProcessQueryLimitedInformation = 0x1000;
        IntPtr proc = OpenProcess(ProcessQueryLimitedInformation, false, pid);
        if (proc == IntPtr.Zero) return null;
        try
        {
            char[] buf = new char[32768];
            uint len = (uint)buf.Length;
            return QueryFullProcessImageNameW(proc, 0, buf, ref len) && len > 0 ? new string(buf, 0, (int)len) : null;
        }
        finally { CloseHandle(proc); }
    }

    internal static bool IsElevatedProcess
    {
        get
        {
            if (!OperatingSystem.IsWindows() || !OpenProcessToken(GetCurrentProcess(), 0x0008 /* TOKEN_QUERY */, out IntPtr token)) return false;
            try { return GetTokenInformation(token, 20 /* TokenElevation */, out uint elevated, sizeof(uint), out _) && elevated != 0; }
            finally { CloseHandle(token); }
        }
    }

    internal static NativeProcessWaitHandle? TryOpenProcessHandle(int pid, out bool accessDenied)
    {
        accessDenied = false;
        if (pid <= 0 || !OperatingSystem.IsWindows()) return null;
        IntPtr proc = OpenProcess(0x00101000u /* SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION */, false, (uint)pid);
        if (proc == IntPtr.Zero) { accessDenied = Marshal.GetLastWin32Error() == 5; return null; }
        return new NativeProcessWaitHandle(proc);
    }

    internal static bool IsProcessRunning(NativeProcessWaitHandle? handle) =>
        handle != null && !handle.SafeWaitHandle.IsInvalid && !handle.SafeWaitHandle.IsClosed &&
        GetExitCodeProcess(handle.SafeWaitHandle.DangerousGetHandle(), out uint code) && code == 259;

    internal static async Task<bool> WaitForProcessExitAsync(NativeProcessWaitHandle? handle, int timeoutMs, CancellationToken ct = default)
    {
        if (!IsProcessRunning(handle)) return true;
        var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var reg = ThreadPool.RegisterWaitForSingleObject(handle!, (_, timedOut) => tcs.TrySetResult(!timedOut), null, timeoutMs, true);
        using var ctr = ct.Register(() => tcs.TrySetCanceled(ct));
        try { return await tcs.Task; } finally { reg.Unregister(null); }
    }

    private readonly string _pipeName;
    private readonly string _lifecyclePipeName;
    private readonly string _settingsPath;
    private readonly string? _expectedServerExe;
    private readonly Func<BarSettings, string?>? _writeFileOverride;

    internal CoreClient(
        string? pipeName = null,
        string? settingsPath = null,
        string? expectedServerExe = null,
        Func<BarSettings, string?>? writeFileOverride = null,
        string? lifecyclePipeName = null)
    {
        int sessionId = Process.GetCurrentProcess().SessionId;
        _pipeName = pipeName ?? "Atrium.Core." + sessionId;
        _lifecyclePipeName = lifecyclePipeName ?? "Atrium.Lifecycle." + sessionId;
        _settingsPath = settingsPath ?? SettingsPath;
        _expectedServerExe = expectedServerExe;
        _writeFileOverride = writeFileOverride;
    }

    internal static bool TryParseDataDirArgument(string[] args, out string? dir, out bool specified)
    {
        dir = null;
        specified = false;
        for (int i = 0; i < args.Length; i++)
        {
            if (!string.Equals(args[i], "--data-dir", StringComparison.Ordinal)) continue;
            specified = true;
            if (i + 1 >= args.Length) { dir = null; return false; }
            string raw = args[++i];
            if (string.IsNullOrWhiteSpace(raw) || !Path.IsPathFullyQualified(raw)) { dir = null; return false; }
            if (raw.StartsWith(@"\\", StringComparison.Ordinal) &&
                !raw.StartsWith(@"\\?\", StringComparison.Ordinal) &&
                !raw.StartsWith(@"\\.\", StringComparison.Ordinal))
            {
                string trimmed = raw.TrimEnd('\\', '/');
                int slash = trimmed.IndexOfAny(['\\', '/'], 2);
                if (slash < 0 || slash + 1 >= trimmed.Length) { dir = null; return false; }
            }
            try { dir = Path.TrimEndingDirectorySeparator(Path.GetFullPath(raw)); }
            catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException) { dir = null; return false; }
        }
        return true;
    }

    public static string DataDirectory
    {
        get
        {
            string dir = Development
                ? TryParseDataDirArgument(Environment.GetCommandLineArgs(), out string? custom, out bool specified)
                    ? specified && custom != null ? custom : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Atrium")
                    : throw new ArgumentException("Invalid --data-dir argument; expected a fully qualified absolute path.", "--data-dir")
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Atrium");
            Directory.CreateDirectory(dir);
            return dir;
        }
    }

    public static string SettingsPath => Path.Combine(DataDirectory, "settings.json");

    private readonly SemaphoreSlim _io = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private CancellationTokenSource? _connection;
    private readonly byte[] _readBuffer = new byte[4096];
    private NamedPipeClientStream? _pipe;
    private bool _connectionLost;
    internal static bool Development
    {
        get
        {
#if ATRIUM_DEVELOPMENT
            return true;
#else
            string? exe = CoreExe();
            return Environment.GetCommandLineArgs().Contains("--development") ||
                (exe != null && File.Exists(Path.Combine(Path.GetDirectoryName(exe)!, "development.txt")));
#endif
        }
    }
    public string? LastSaveWarning { get; private set; }
    public string? ConnectionError { get; private set; }

    public bool Connected => _pipe is { IsConnected: true };
    public int ProcessId { get; private set; }
    public bool CoreElevated { get; private set; }
    public CorePrivilegeMode PrivilegeMode { get; private set; }
    public bool CanDeElevate { get; private set; }
    public bool StateConfirmed { get; private set; }
    public string? InstanceId { get; private set; }
    public ulong Revision { get; private set; }

    public static string? CoreExe()
    {
        foreach (string? p in new[] { Path.Combine(AppContext.BaseDirectory, "..", "Atrium.exe"), Path.Combine(AppContext.BaseDirectory, "Atrium.exe") })
            if (File.Exists(p)) return Path.GetFullPath(p);
        return null;
    }

    internal static ProcessStartInfo CoreStartInfo(
        string exe, string[]? args = null, bool? development = null, bool runAsAdmin = false, bool replace = false)
    {
        bool dev = development ?? Development;
        var list = new List<string>();
        if (replace) list.Add("--replace");
        if (dev && TryParseDataDirArgument(args ?? Environment.GetCommandLineArgs(), out string? custom, out bool specified) &&
            specified && custom != null)
        {
            list.Add("--development");
            list.Add("--data-dir");
            list.Add(custom);
        }
        if (runAsAdmin)
            return new ProcessStartInfo(exe) { UseShellExecute = true, Verb = "runas", Arguments = string.Join(" ", list.Select(QuoteArg)) };
        if (list.Count > 0)
        {
            var info = new ProcessStartInfo(exe) { UseShellExecute = false };
            foreach (string a in list) info.ArgumentList.Add(a);
            return info;
        }
        return new ProcessStartInfo(exe) { UseShellExecute = true };
    }

    private static string QuoteArg(string a)
    {
        if (a.Length > 0 && !a.Any(c => char.IsWhiteSpace(c) || c == '"')) return a;
        var sb = new System.Text.StringBuilder("\"");
        int bs = 0;
        foreach (char c in a)
        {
            if (c == '\\') { bs++; continue; }
            if (c == '"') { sb.Append('\\', bs * 2 + 1); sb.Append('"'); bs = 0; continue; }
            if (bs > 0) { sb.Append('\\', bs); bs = 0; }
            sb.Append(c);
        }
        if (bs > 0) sb.Append('\\', bs * 2);
        return sb.Append('"').ToString();
    }

    private async Task<(NamedPipeClientStream? Pipe, uint Pid, bool WrongLocation)> OpenVerifiedPipeAsync(
        string pipeName, int timeoutMs, CancellationToken ct)
    {
        var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        await pipe.ConnectAsync(timeoutMs, ct);
        if (OperatingSystem.IsWindows()) pipe.ReadMode = PipeTransmissionMode.Message;
        // The name is session-wide; only this distribution's executable owns our settings.
        if (!GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(), out uint pid)) { pipe.Dispose(); return (null, 0, false); }
        string? expected = _expectedServerExe ?? CoreExe();
        if (expected == null || !string.Equals(TryGetProcessPath(pid), expected, StringComparison.OrdinalIgnoreCase))
        { pipe.Dispose(); return (null, 0, true); }
        return (pipe, pid, false);
    }

    public async Task<bool> ConnectAsync(int timeoutMs)
    {
        Disconnect();
        NamedPipeClientStream? pipe = null;
        try
        {
            (pipe, uint pid, bool wrongLocation) = await OpenVerifiedPipeAsync(_pipeName, timeoutMs, _lifetime.Token);
            if (pipe == null)
            {
                if (wrongLocation)
                {
                    _connectionLost = true;
                    ConnectionError = Loc.T("Atrium is running from another location; the core in this folder was not connected.");
                }
                return false;
            }
            _lifetime.Token.ThrowIfCancellationRequested();
            _connection = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token);
            _pipe = pipe;
            ProcessId = (int)pid;
            _connectionLost = false;
            ConnectionError = null;
            return true;
        }
        catch (Exception e) when (e is TimeoutException or IOException or UnauthorizedAccessException
            or ArgumentException or InvalidOperationException or OperationCanceledException or System.ComponentModel.Win32Exception)
        {
            if (e is not TimeoutException and not OperationCanceledException)
            { _connectionLost = true; ConnectionError = Loc.F("Could not connect to the core in this folder: {0}", e.Message); }
            pipe?.Dispose();
            return false;
        }
    }

    public void Disconnect(bool coreStopped = false)
    {
        _connection?.Cancel(); // also wakes every request waiting for the semaphore
        _connection?.Dispose();
        _connection = null;
        _pipe?.Dispose();
        _pipe = null;
        ProcessId = 0;
        CoreElevated = false;
        PrivilegeMode = CorePrivilegeMode.Unknown;
        CanDeElevate = false;
        StateConfirmed = false;
        InstanceId = null;
        Revision = 0;
        if (coreStopped) { _connectionLost = false; ConnectionError = null; }
    }

    private async Task<JsonNode?> RequestAsync(JsonObject request)
    {
        var pipe = _pipe;
        var connection = _connection;
        if (pipe is not { IsConnected: true } || connection == null || _lifetime.IsCancellationRequested) return null;
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(connection.Token, _lifetime.Token);
        deadline.CancelAfter(TimeSpan.FromSeconds(5)); // includes queueing, writing and the reply
        bool entered = false;
        try
        {
            await _io.WaitAsync(deadline.Token);
            entered = true;
            deadline.Token.ThrowIfCancellationRequested();
            if (!ReferenceEquals(pipe, _pipe)) return null;
            byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(request);
            await pipe.WriteAsync(bytes, deadline.Token);
            await pipe.FlushAsync(deadline.Token);
            return await ReadPipeReplyAsync(pipe, _readBuffer, 1024 * 1024, deadline.Token);
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException or JsonException or OperationCanceledException)
        {
            if (ReferenceEquals(pipe, _pipe))
            {
                _connectionLost = true;
                ConnectionError = Loc.T("The core did not respond or the connection was lost; the running state is not confirmed.");
                Disconnect();
            }
            return null;
        }
        finally
        {
            if (entered) _io.Release();
        }
    }

    // State from the core, or from the file when it is not running.
    public async Task<CoreState> GetAsync()
    {
        var r = await RequestAsync(new JsonObject { ["op"] = "get" });
        if (r?["op"]?.GetValue<string>() == "state" && r is JsonObject obj)
        {
            StateConfirmed = true;
            var priv = ParsePrivilegeInfo(obj);
            CoreElevated = priv.Elevated;
            PrivilegeMode = priv.Mode;
            CanDeElevate = priv.CanDeElevate;
            InstanceId = ParseNonEmptyString(r["instanceId"]);
            Revision = ParseRevision(r["revision"]);
            return new CoreState(
                BarSettings.FromJson(r["saved"] as JsonObject, new()),
                BarSettings.FromJson(r["current"] as JsonObject, new()),
                InstanceId,
                Revision,
                true);
        }
        var file = await ReadFileSerializedAsync();
        return new CoreState(file, file.Clone(), null, 0, false);
    }

    public Task<string?> PreviewAsync(BarSettings s) => Send("preview", s);

    private static CommitResult ConnectionLostCommitError() => new(CommitStatus.Error,
        Error: Loc.T("The connection to the core was lost; the save result is not confirmed. Reconnect and check the saved settings."));

    public async Task<CommitResult> CommitAsync(BarSettings s, string? expectedInstanceId = null, ulong expectedRevision = 0)
    {
        LastSaveWarning = null;
        if (Connected)
            return await SendCommitAsync(s, expectedInstanceId ?? InstanceId, expectedRevision > 0 ? expectedRevision : Revision);
        if (_connectionLost || _lifetime.IsCancellationRequested) return ConnectionLostCommitError();
        var snapshot = s.Clone();
        bool entered = false;
        try
        {
            await _io.WaitAsync(_lifetime.Token);
            entered = true;
            if (_lifetime.IsCancellationRequested) return ConnectionLostCommitError();
            if (Connected)
            {
                _io.Release();
                entered = false;
                return await SendCommitAsync(snapshot, expectedInstanceId ?? InstanceId, expectedRevision > 0 ? expectedRevision : Revision);
            }
            LastSaveWarning = await Task.Run(() => _writeFileOverride != null ? _writeFileOverride(snapshot) : WriteFile(_settingsPath, snapshot));
            return new CommitResult(CommitStatus.Saved, Warning: LastSaveWarning);
        }
        catch (OperationCanceledException) { return ConnectionLostCommitError(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return new CommitResult(CommitStatus.Error, Error: e.Message); }
        finally { if (entered) _io.Release(); }
    }

    internal static CorePrivilegeInfo ParsePrivilegeInfo(JsonObject r)
    {
        bool elev = false;
        bool hasElevated = r["elevated"] is JsonValue ev && ev.TryGetValue(out elev);
        bool elevatedFlag = hasElevated && elev;
        var mode = ParseNonEmptyString(r["privilegeMode"]) switch
        {
            "standard" => CorePrivilegeMode.Standard,
            "elevated_split" => CorePrivilegeMode.ElevatedSplit,
            "elevated_unified" => CorePrivilegeMode.ElevatedUnified,
            "cross_account_elevated" => CorePrivilegeMode.CrossAccountElevated,
            "unknown" => CorePrivilegeMode.Unknown,
            null when hasElevated && !elevatedFlag => CorePrivilegeMode.Standard,
            _ => CorePrivilegeMode.Unknown,
        };
        bool elevated = elevatedFlag || mode is CorePrivilegeMode.ElevatedSplit
            or CorePrivilegeMode.ElevatedUnified or CorePrivilegeMode.CrossAccountElevated;
        bool canDeElevate = mode == CorePrivilegeMode.ElevatedSplit &&
            r["canDeElevate"] is JsonValue cv && cv.TryGetValue(out bool cde) && cde;
        return new CorePrivilegeInfo(elevated, mode, canDeElevate);
    }

    internal static AdminCardState GetAdminCardState(bool connected, CorePrivilegeInfo privilege, bool hasCoreExe, bool coreChanging)
    {
        string defaultNote = Loc.T("Manage elevated apps in Stage Manager, previews and tray. Prompts UAC; Explorer cannot drag onto an elevated taskbar.");
        bool enabled = hasCoreExe && !coreChanging;
        if (!connected)
            return new(Loc.T("Atrium is not running"), defaultNote, Loc.T("Start as administrator"), AdminCardAction.StartAsAdmin, true, enabled);
        if (!privilege.Elevated)
            return new(Loc.T("Running as standard user"), defaultNote, Loc.T("Restart as administrator"), AdminCardAction.RestartAsAdmin, true, enabled);
        if (privilege.CanDeElevate)
            return new(Loc.T("Running as administrator"), defaultNote, Loc.T("Restart as standard user"), AdminCardAction.RestartAsStandard, true, enabled);
        string unifiedNote = Loc.T("Your current Windows account or UAC policy runs apps elevated by default, so Atrium cannot restart as a standard user.");
        return new(Loc.T("Running as administrator"), unifiedNote, null, AdminCardAction.None, false, false);
    }

    private static async Task<JsonObject?> ReadPipeReplyAsync(NamedPipeClientStream pipe, byte[] buffer, int maxBytes, CancellationToken ct)
    {
        using var message = new MemoryStream();
        do
        {
            int n = await pipe.ReadAsync(buffer, ct);
            if (n == 0) throw new IOException("pipe closed");
            message.Write(buffer, 0, n);
            if (message.Length > maxBytes) throw new IOException(Loc.T("The core's reply was too large"));
        } while (!pipe.IsMessageComplete);
        return JsonNode.Parse(message.GetBuffer().AsSpan(0, (int)message.Length)) as JsonObject;
    }

    internal async Task<JsonObject?> RequestLifecycleAsync(JsonObject request, int timeoutMs = 1500, CancellationToken ct = default)
    {
        try
        {
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token, ct);
            linked.CancelAfter(timeoutMs);
            var (pipe, _, _) = await OpenVerifiedPipeAsync(_lifecyclePipeName, timeoutMs, linked.Token);
            if (pipe == null) return null;
            using (pipe)
            {
                await pipe.WriteAsync(JsonSerializer.SerializeToUtf8Bytes(request), linked.Token);
                await pipe.FlushAsync(linked.Token);
                return await ReadPipeReplyAsync(pipe, new byte[4096], 64 * 1024, linked.Token);
            }
        }
        catch (Exception e) when (e is TimeoutException or IOException or UnauthorizedAccessException
            or ArgumentException or InvalidOperationException or OperationCanceledException or JsonException)
        { return null; }
    }

    internal async Task<int?> PingLifecyclePidAsync(int timeoutMs = 400, CancellationToken ct = default)
    {
        var r = await RequestLifecycleAsync(new JsonObject { ["cmd"] = "ping" }, timeoutMs, ct);
        return r?["ok"] is JsonValue okv && okv.TryGetValue(out bool ok) && ok &&
               r["pid"] is JsonValue pv && pv.TryGetValue(out int pid) ? pid : null;
    }

    private async Task<bool> WaitForCoreStopAsync(NativeProcessWaitHandle? handle, int oldPid, int timeoutMs, CancellationToken ct)
    {
        if (handle != null && !await WaitForProcessExitAsync(handle, timeoutMs, ct)) return false;
        if (oldPid <= 0) return true;
        long deadline = Environment.TickCount64 + timeoutMs;
        while (!ct.IsCancellationRequested)
        {
            int? activePid = await PingLifecyclePidAsync(300, ct);
            if (activePid == null || activePid != oldPid) return true;
            if (Environment.TickCount64 >= deadline) return false;
            await Task.Delay(150, ct);
        }
        return false;
    }

    public async Task<LifecycleRestartResult> RestartAsStandardAsync(CancellationToken ct = default)
    {
        int oldPid = ProcessId;
        string? previousId = InstanceId;
        string? exe = TryGetProcessPath(oldPid) ?? _expectedServerExe ?? CoreExe();
        if (exe == null) return new(false, false, Loc.T("Atrium.exe was not found"));
        using var oldProc = TryOpenProcessHandle(oldPid, out _);
        try
        {
            if (!IsElevatedProcess)
            {
                var r = await RequestLifecycleAsync(new JsonObject { ["cmd"] = "quit" }, 1500, ct);
                bool sentQuit = r?["ok"] is JsonValue okv && okv.TryGetValue(out bool ok) && ok;
                if (!sentQuit && Connected) sentQuit = await QuitCoreAsync();
                Disconnect();
                if (sentQuit && !await WaitForCoreStopAsync(oldProc, oldPid, 8000, ct))
                    return new(false, false, Loc.T("Atrium has not quit yet. Try again later."));
                using var launched = Process.Start(CoreStartInfo(exe, replace: true));
            }
            else
            {
                var r = await RequestLifecycleAsync(new JsonObject { ["cmd"] = "restart-standard" }, 3000, ct);
                if (r?["ok"] is not JsonValue okv || !okv.TryGetValue(out bool ok) || !ok)
                {
                    return new(false, false, ParseNonEmptyString(r?["error"]) == "cannot_deelevate"
                        ? Loc.T("Atrium cannot restart as a standard user under the current Windows account or UAC policy.")
                        : Loc.T("Could not restart as standard user"));
                }
                Disconnect();
                await WaitForCoreStopAsync(oldProc, oldPid, 8000, ct);
            }
            for (int i = 0; i < 66 && !ct.IsCancellationRequested; i++)
            {
                if (await ConnectAsync(150))
                {
                    var state = await GetAsync();
                    if (state.FromCore && (!CoreElevated || (InstanceId != null && InstanceId != previousId)))
                        return new(!CoreElevated, true, CoreElevated ? Loc.T("Could not restart as standard user") : null);
                    Disconnect();
                }
                await Task.Delay(150, ct);
            }
            return new(false, false, Loc.T("Atrium restarted, but the settings window could not reconnect yet. Check the status indicator."));
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested) { return new(false, false); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        { return new(false, false, e.Message); }
    }

    public Task<string?> CancelAsync() => Send("cancel", null);

    public async Task<bool> QuitCoreAsync() =>
        (await RequestAsync(new JsonObject { ["op"] = "quit" }))?["op"]?.GetValue<string>() == "ok";

    // Puts a desktop widget in the first free place ("clock", "medium"); null on success.
    public async Task<string?> AddWidgetAsync(string kind, string size)
    {
        var r = await RequestAsync(new JsonObject { ["op"] = "add-widget", ["kind"] = kind, ["size"] = size });
        if (r == null) return Loc.T("Atrium is not running");
        return r["op"]?.GetValue<string>() == "ok" ? null : r["message"]?.GetValue<string>() ?? "error";
    }

    private async Task<CommitResult> SendCommitAsync(BarSettings s, string? expectedInstanceId, ulong expectedRevision)
    {
        var request = new JsonObject { ["op"] = "commit", ["settings"] = s.ToJson() };
        if (!string.IsNullOrEmpty(expectedInstanceId)) request["instanceId"] = expectedInstanceId;
        if (expectedRevision > 0) request["expectedRevision"] = expectedRevision;
        var r = await RequestAsync(request);
        if (r == null)
        {
            return new CommitResult(CommitStatus.Error,
                Error: Loc.T("The core did not reply in time or the connection closed; the result is not confirmed. Please reconnect."));
        }
        string? op = r["op"]?.GetValue<string>();
        if (op is "ok" or "conflict")
        {
            string? id = ParseNonEmptyString(r["instanceId"]) ?? InstanceId;
            ulong rev = ParseRevision(r["revision"]);
            if (op == "ok" && rev == 0) rev = Revision;
            InstanceId = id;
            Revision = rev;
            if (op == "ok")
            {
                LastSaveWarning = r["warning"]?.GetValue<string>();
                return new CommitResult(CommitStatus.Saved, InstanceId: id, Revision: rev, Warning: LastSaveWarning);
            }
            return new CommitResult(CommitStatus.Conflict, BarSettings.FromJson(r["saved"] as JsonObject, new()),
                id, rev, Error: r["message"]?.GetValue<string>() ?? "conflict");
        }
        return new CommitResult(CommitStatus.Error, Error: r["message"]?.GetValue<string>() ?? "error");
    }

    // Null on success, otherwise an error message.
    private async Task<string?> Send(string op, BarSettings? s)
    {
        var request = new JsonObject { ["op"] = op };
        if (s != null) request["settings"] = s.ToJson();
        bool connected = Connected;
        var r = await RequestAsync(request);
        // Not connected: nothing to preview. A connection lost on the way: not done.
        if (r == null)
        {
            return connected || _connectionLost
                ? Loc.T("The core did not reply in time or the connection closed; the result is not confirmed. Please reconnect.")
                : null;
        }
        return r["op"]?.GetValue<string>() == "ok" ? null : r["message"]?.GetValue<string>() ?? "error";
    }

    private async Task<BarSettings> ReadFileSerializedAsync()
    {
        bool entered = false;
        try
        {
            await _io.WaitAsync(_lifetime.Token);
            entered = true;
            return await Task.Run(() => ReadFile(_settingsPath));
        }
        catch (OperationCanceledException) { return new BarSettings(); }
        finally { if (entered) _io.Release(); }
    }

    private static string? ParseNonEmptyString(JsonNode? node) =>
        node is JsonValue v && v.TryGetValue(out string? s) && !string.IsNullOrEmpty(s) ? s : null;

    private static ulong ParseRevision(JsonNode? node)
    {
        if (node is not JsonValue v) return 0;
        if (v.TryGetValue(out ulong u)) return u;
        return v.TryGetValue(out double d) && double.IsFinite(d) && d >= 0 ? (ulong)d : 0;
    }

    private static BarSettings ReadFile(string path)
    {
        var s = SettingsPersistence.ReadSettings(path);
        // First run (no "pins" key yet): show what the core would import.
        s.Pins ??= ImportTaskbarPins();
        return s;
    }

    // The pins of the Windows taskbar, as the core imports them on first run.
    public static List<PinnedApp> ImportTaskbarPins()
    {
        var pins = new List<PinnedApp>();
        try
        {
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "Microsoft", "Internet Explorer", "Quick Launch", "User Pinned", "TaskBar");
            if (!Directory.Exists(dir)) return pins;
            foreach (string? f in Directory.EnumerateFiles(dir, "*.lnk").OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                pins.Add(new PinnedApp(Path.GetFileNameWithoutExtension(f), f, "", ""));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return pins.Count > 64 ? pins.GetRange(0, 64) : pins;
    }

    private static string FormatPersistenceError(string key, object?[] args) => key switch
    {
        "Could not save {0}: settings file uses a newer format version (v{1} > v{2}). Update Atrium before saving." =>
            Loc.F("Could not save {0}: settings file uses a newer format version (v{1} > v{2}). Update Atrium before saving.", args),
        "Could not back up corrupt settings file {0} before saving; original file was kept." =>
            Loc.F("Could not back up corrupt settings file {0} before saving; original file was kept.", args),
        _ => string.Format(System.Globalization.CultureInfo.InvariantCulture, key, args),
    };

    private static string? WriteFile(string settingsPath, BarSettings s)
    {
        bool startupChanged = SettingsPersistence.WriteSettings(settingsPath, s, FormatPersistenceError);
        if (!startupChanged || Development || !OperatingSystem.IsWindows()) return null;
        try
        {
            using var run = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run");
            string exe = CoreExe() ?? throw new FileNotFoundException(Loc.T("Atrium.exe was not found"));
            string command = $"\"{exe}\"";
            if (s.StartOnLogin)
            {
                if (s.StartupMode == "task")
                {
                    CreateLogonTask(exe);
                    run.DeleteValue("Atrium", throwOnMissingValue: false);
                }
                else
                {
                    run.SetValue("Atrium", command);
                    RemoveLogonTask(exe, strictOwner: false);
                }
            }
            else
            {
                if (run.GetValue("Atrium") is string owner && !string.Equals(owner, command, StringComparison.OrdinalIgnoreCase))
                    throw new IOException(Loc.T("That startup entry belongs to Atrium in another location"));
                RemoveLogonTask(exe, strictOwner: true);
                run.DeleteValue("Atrium", throwOnMissingValue: false);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            return Loc.F("Settings were saved, but Windows startup could not be changed: {0}. Toggle Start with Windows again, then save.", e.Message);
        }
        return null;
    }

    private static void ReleaseCom(params object?[] objects)
    {
        if (!OperatingSystem.IsWindows()) return;
        foreach (object? com in objects)
            if (com != null && Marshal.IsComObject(com)) Marshal.FinalReleaseComObject(com);
    }

    // Create or update the standard-user Task Scheduler logon task for this install.
    private static void CreateLogonTask(string exe)
    {
        if (!OperatingSystem.IsWindows()) return;
        string? sid = System.Security.Principal.WindowsIdentity.GetCurrent().User?.Value;
        if (string.IsNullOrEmpty(sid))
            throw new IOException("Could not determine the current user SID.");
        string name = "Atrium Startup-" + sid;
        dynamic? service = null, folder = null, definition = null, registrationInfo = null,
            principal = null, settings = null, idleSettings = null, triggers = null, trigger = null, actions = null, action = null;
        try
        {
            service = Activator.CreateInstance(Type.GetTypeFromProgID("Schedule.Service")!)!;
            service.Connect();
            folder = service.GetFolder("\\");
            definition = service.NewTask(0);
            registrationInfo = definition.RegistrationInfo;
            registrationInfo.Author = "Atrium";
            registrationInfo.Description = "Start Atrium when you sign in to Windows.";
            principal = definition.Principal;
            principal.UserId = sid;
            principal.LogonType = 3; // TASK_LOGON_INTERACTIVE_TOKEN
            principal.RunLevel = 0;  // TASK_RUNLEVEL_LUA
            settings = definition.Settings;
            settings.DisallowStartIfOnBatteries = false;
            settings.StopIfGoingOnBatteries = false;
            settings.ExecutionTimeLimit = "PT0S";
            settings.AllowDemandStart = true;
            settings.Enabled = true;
            settings.MultipleInstances = 2; // TASK_INSTANCES_IGNORE_NEW
            settings.Priority = 4;          // match normal interactive UI priority
            idleSettings = settings.IdleSettings;
            idleSettings.StopOnIdleEnd = false;
            idleSettings.RestartOnIdle = false;
            triggers = definition.Triggers;
            trigger = triggers.Create(9); // TASK_TRIGGER_LOGON
            trigger.UserId = sid;
            trigger.Enabled = true;
            actions = definition.Actions;
            action = actions.Create(0); // TASK_ACTION_EXEC
            action.Path = exe;
            action.WorkingDirectory = Path.GetDirectoryName(exe) ?? "";
            folder.RegisterTaskDefinition(name, definition, 6, null, null, 3);
        }
        catch (Exception e) when (e is RuntimeBinderException or COMException or ArgumentException)
        {
            throw new IOException(e.Message, e);
        }
        finally
        {
            ReleaseCom(action, actions, trigger, triggers, idleSettings, settings, principal, registrationInfo, definition, folder, service);
        }
    }

    // Remove any leftover Task Scheduler logon task for this install.
    private static void RemoveLogonTask(string exe, bool strictOwner)
    {
        if (!OperatingSystem.IsWindows()) return;
        string name = "Atrium Startup-" + System.Security.Principal.WindowsIdentity.GetCurrent().User?.Value;
        dynamic? service = null, folder = null, task = null, definition = null, actions = null, action = null;
        try
        {
            service = Activator.CreateInstance(Type.GetTypeFromProgID("Schedule.Service")!)!;
            service.Connect();
            folder = service.GetFolder("\\");
            try { task = folder.GetTask(name); }
            catch (COMException e) when (e.HResult == unchecked((int)0x80070002)) { return; } // no task
            if (strictOwner)
            {
                definition = task.Definition;
                actions = definition.Actions;
                if ((int)actions.Count < 1) return;
                action = actions.Item(1);
                string? command = (action.Path as string)?.Trim('"');
                if (!string.Equals(command, exe, StringComparison.OrdinalIgnoreCase))
                    throw new IOException(Loc.T("That startup entry belongs to Atrium in another location"));
            }
            folder.DeleteTask(name, 0);
        }
        catch (Exception e) when (e is RuntimeBinderException or COMException or ArgumentException)
        { throw new IOException(e.Message, e); }
        finally { ReleaseCom(action, actions, definition, task, folder, service); }
    }

    public void Dispose()
    {
        _lifetime.Cancel();
        Disconnect();
        // In-flight requests still release this semaphore after Disconnect completes their I/O.
    }
}
