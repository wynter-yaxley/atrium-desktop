param(
    [string]$ZipPath,
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [switch]$SmokeStart,
    [string]$EvidenceDirectory = '',
    [int]$UiaTimeoutMs = 10000,
    [string]$PipeName = '',
    [switch]$SkipRunningProcessCheck,
    [string]$BeforeSaveMarkerPath = '',
    [string]$BeforeSaveResumeEvent = '',
    [string]$HangUiaStep = ''
)
$ErrorActionPreference = 'Stop'
if ($UiaTimeoutMs -lt 100) { throw "UiaTimeoutMs must be >= 100 (got $UiaTimeoutMs)." }
if (!$ZipPath) {
    $candidates = @(Get-ChildItem -LiteralPath (Join-Path $Root 'dist') -Filter 'Atrium-*-win-x64.zip' -ErrorAction SilentlyContinue)
    if ($candidates.Count -ne 1) { throw "Expected 1 package zip in dist/, found $($candidates.Count)." }
    $ZipPath = $candidates[0].FullName
}
$ZipPath = [IO.Path]::GetFullPath($ZipPath)
if (!(Test-Path -LiteralPath $ZipPath -PathType Leaf)) { throw "Package zip not found: $ZipPath" }
$cmake = Get-Content -LiteralPath (Join-Path $Root 'native/CMakeLists.txt') -Raw
if ($cmake -notmatch 'VERSION\s+(\d+\.\d+\.\d+)') { throw 'Missing version in CMakeLists.txt.' }
$expectedVersion = $Matches[1]
$temp = Join-Path ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) ('Atrium-verify-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $temp)
    $pkg = Join-Path $temp 'Atrium'; if (!(Test-Path -LiteralPath $pkg -PathType Container)) { $pkg = $temp }
    $required = @('Atrium.exe', 'Settings\Atrium.Settings.exe', 'Settings\Atrium.Settings.dll',
        'Settings\Update.ps1', 'Uninstall.ps1', 'LICENSE', 'locales\manifest.json', 'package-manifest.json')
    foreach ($rel in $required) {
        if (!(Test-Path -LiteralPath (Join-Path $pkg $rel) -PathType Leaf)) { throw "Missing package file: $rel" }
    }
    $pkgFull = [IO.Path]::GetFullPath($pkg).TrimEnd('\')
    $pkgManifest = Get-Content -LiteralPath (Join-Path $pkgFull 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $pkgManifest -or (($pkgManifest.schema -isnot [int]) -and ($pkgManifest.schema -isnot [long])) -or
        $pkgManifest.schema -ne 1) { throw 'Invalid package-manifest.json schema.' }
    if ($pkgManifest.version -ne $expectedVersion) { throw "package-manifest.json version '$($pkgManifest.version)' != $expectedVersion." }
    $manifestFiles = @($pkgManifest.files)
    if ($manifestFiles.Count -eq 0) { throw 'package-manifest.json files list is empty.' }
    $listed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        foreach ($entry in $manifestFiles) {
            $rel = [string]$entry.path
            if ([string]::IsNullOrWhiteSpace($rel) -or [IO.Path]::IsPathRooted($rel) -or
                $rel.StartsWith('\') -or $rel.StartsWith('/') -or $rel.EndsWith('\') -or $rel.EndsWith('/')) {
                throw "Invalid manifest path: '$rel'"
            }
            $norm = $rel.Replace('/', '\')
            if ($norm.Split('\') | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }) {
                throw "Unsafe manifest path: '$rel'"
            }
            $full = [IO.Path]::GetFullPath((Join-Path $pkgFull $norm))
            if (!$full.StartsWith($pkgFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "Out-of-bounds manifest path: '$rel'"
            }
            if (!$listed.Add($norm)) { throw "Duplicate manifest path: '$rel'" }
            if (!(Test-Path -LiteralPath $full -PathType Leaf)) { throw "Missing file listed in package-manifest.json: $norm" }
            $expectedHash = [string]$entry.sha256
            if ($expectedHash -cnotmatch '^[0-9a-f]{64}$') { throw "Invalid SHA-256 in manifest for $norm" }
            $stream = [IO.File]::OpenRead($full)
            try { $actualHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
            finally { $stream.Dispose() }
            if ($actualHash -cne $expectedHash) { throw "SHA-256 mismatch for $norm" }
        }
    } finally {
        $sha.Dispose()
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $pkgFull -Recurse -File -Force)) {
        $rel = $file.FullName.Substring($pkgFull.Length + 1).Replace('/', '\')
        if ($rel -ieq 'package-manifest.json' -or $rel -ieq 'development.txt') { continue }
        if (!$listed.Contains($rel)) { throw "File not listed in package-manifest.json: $rel" }
    }
    foreach ($ps in @('Uninstall.ps1', 'Settings\Update.ps1')) {
        $bom = [IO.File]::ReadAllBytes((Join-Path $pkg $ps))
        if ($bom.Length -lt 3 -or $bom[0] -ne 0xEF -or $bom[1] -ne 0xBB -or $bom[2] -ne 0xBF) { throw "$ps is missing its UTF-8 BOM." }
    }
    $testLocales = @(Get-ChildItem -LiteralPath (Join-Path $pkg 'locales') -Filter 'x-*.json' -ErrorAction SilentlyContinue)
    if ($testLocales.Count -gt 0) { throw "Test locale files found in package: $($testLocales.Name -join ', ')" }
    $manifest = Get-Content -LiteralPath (Join-Path $pkg 'locales\manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($lang in @($manifest.languages)) { if ($lang.tag -like 'x-*') { throw "Test locale tag found in manifest: $($lang.tag)" } }
    foreach ($bin in @('Atrium.exe', 'Settings\Atrium.Settings.dll')) {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $pkg $bin))
        if ("$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart)" -ne $expectedVersion) { throw "$bin version != $expectedVersion." }
    }
    $exeBytes = (Get-Item -LiteralPath (Join-Path $pkg 'Atrium.exe')).Length
    if ($exeBytes -gt 6MB) { throw "Atrium.exe exceeds 6 MB limit: $exeBytes bytes." }
    $zipBytes = (Get-Item -LiteralPath $ZipPath).Length
    if ($zipBytes -gt 120MB) { throw "Package zip exceeds 120 MB limit: $zipBytes bytes." }
    if ($SmokeStart) {
        function Send-PipeRequest([System.IO.Pipes.NamedPipeClientStream]$pipe, [string]$json) {
            $req = [Text.Encoding]::UTF8.GetBytes($json)
            $pipe.Write($req, 0, $req.Length)
            $pipe.Flush()
            $ms = New-Object System.IO.MemoryStream
            try {
                $buf = New-Object byte[] 4096
                do {
                    $n = $pipe.Read($buf, 0, $buf.Length)
                    if ($n -eq 0) { throw 'Pipe closed unexpectedly while reading response.' }
                    $ms.Write($buf, 0, $n)
                } while (!$pipe.IsMessageComplete)
                [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
            } finally {
                $ms.Dispose()
            }
        }
        function Read-LogText([string]$path) {
            if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return '(missing)' }
            $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try {
                $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8, $true)
                try { $sr.ReadToEnd() } finally { $sr.Dispose() }
            } finally {
                $fs.Dispose()
            }
        }
        function Wait-Until([int]$timeoutMs, [scriptblock]$cond, [string]$failMessage) {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
                if (& $cond) { return }
                Start-Sleep -Milliseconds 100
            }
            if (& $cond) { return }
            throw $failMessage
        }
        function Format-ProcessArg([string]$arg) {
            if ($arg -match '[\s"]') { return '"' + $arg.Replace('"', '\"') + '"' }
            return $arg
        }
        if (!$SkipRunningProcessCheck) {
            $running = @(Get-Process -Name Atrium -ErrorAction SilentlyContinue)
            if ($running.Count -gt 0) {
                throw "Atrium is already running (PID: $($running.Id -join ', ')); stop it before -SmokeStart."
            }
            $runningSettings = @(Get-Process -Name 'Atrium.Settings' -ErrorAction SilentlyContinue)
            if ($runningSettings.Count -gt 0) {
                throw "Atrium.Settings is already running (PID: $($runningSettings.Id -join ', ')); stop it before -SmokeStart."
            }
        }
        if (!('AtriumVerify.NativeHost' -as [type])) {
            Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace AtriumVerify
{
    public static class NativeHost
    {
        private const int JobObjectExtendedLimitInformation = 9;
        private const uint JobObjectLimitKillOnJobClose = 0x00002000;
        private const uint WmClose = 0x0010;

        [StructLayout(LayoutKind.Sequential)]
        private struct IO_COUNTERS
        {
            public ulong ReadOperationCount;
            public ulong WriteOperationCount;
            public ulong OtherOperationCount;
            public ulong ReadTransferCount;
            public ulong WriteTransferCount;
            public ulong OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
        {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
            public IO_COUNTERS IoInfo;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern IntPtr CreateJobObject(IntPtr a, string n);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(IntPtr h, int c, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, uint len);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool AssignProcessToJobObject(IntPtr h, IntPtr p);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
        private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern int GetClassNameW(IntPtr hwnd, StringBuilder buf, int maxCount);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetProp(IntPtr hwnd, string name);
        [DllImport("user32.dll", ExactSpelling = true)]
        private static extern bool PostMessageW(IntPtr hwnd, uint msg, UIntPtr wparam, IntPtr lparam);

        public static IntPtr CreateKillOnCloseJob()
        {
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            if (job == IntPtr.Zero) throw new InvalidOperationException("CreateJobObject failed: " + Marshal.GetLastWin32Error());
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
            info.BasicLimitInformation.LimitFlags = JobObjectLimitKillOnJobClose;
            uint size = (uint)Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref info, size))
            {
                int err = Marshal.GetLastWin32Error();
                CloseHandle(job);
                throw new InvalidOperationException("SetInformationJobObject failed: " + err);
            }
            return job;
        }

        public static void AssignProcess(IntPtr job, IntPtr process)
        {
            if (job != IntPtr.Zero && process != IntPtr.Zero) AssignProcessToJobObject(job, process);
        }
        public static void CloseJob(IntPtr job) { if (job != IntPtr.Zero) CloseHandle(job); }
        public static bool PostClose(IntPtr hwnd) { return PostMessageW(hwnd, WmClose, UIntPtr.Zero, IntPtr.Zero); }
        public static bool PostTrayOpen(IntPtr hwnd) { return PostMessageW(hwnd, 0x8002, UIntPtr.Zero, new IntPtr(0x0400)); }

        public static IntPtr FindHostWindow(int pid)
        {
            IntPtr found = IntPtr.Zero;
            EnumWindows(delegate(IntPtr hwnd, IntPtr lparam)
            {
                uint wpid;
                GetWindowThreadProcessId(hwnd, out wpid);
                if (wpid != (uint)pid) return true;
                StringBuilder sb = new StringBuilder(64);
                GetClassNameW(hwnd, sb, sb.Capacity);
                if (sb.ToString() == "AtriumCoreHost" || GetProp(hwnd, "Atrium.Core.Host") != IntPtr.Zero) { found = hwnd; return false; }
                return true;
            }, IntPtr.Zero);
            return found;
        }
    }
}
'@
        }
        $csc = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
        if (!(Test-Path -LiteralPath $csc -PathType Leaf)) { throw "C# compiler not found: $csc" }
        $probeSource = Join-Path $temp 'UiaProbe.cs'
        $probeExe = Join-Path $temp 'UiaProbe.exe'
        $probeCode = @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace AtriumVerify
{
    internal static class UiaProbe
    {
        private const int TreeScopeDescendants = 4;
        private const int TogglePatternId = 10015;
        private const int InvokePatternId = 10000;
        private const int AutomationIdPropId = 30011;
        private const int IsInvokeAvailablePropId = 30031;
        private const int IsToggleAvailablePropId = 30041;

        private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetProp(IntPtr hwnd, string name);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);

        [ComImport, Guid("352ffba8-0973-437c-a61f-f64cafd81df9"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomationCondition { }

        [ComImport, Guid("fb377fbe-8ea6-46d5-9c73-6499642d3059"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomationInvokePattern { void Invoke(); }

        [ComImport, Guid("94cf8058-9b8d-4ab9-8bfd-4cd0a33c8c70"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomationTogglePattern { void Toggle(); int CurrentToggleState { get; } }

        [ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomationElement
        {
            void _0(); void _1();
            [return: MarshalAs(UnmanagedType.Interface)]
            IUIAutomationElement FindFirst(int scope, IUIAutomationCondition condition);
            void _3(); void _4(); void _5(); void _6(); void _7(); void _8(); void _9(); void _10();
            void _11(); void _12();
            [return: MarshalAs(UnmanagedType.IUnknown)]
            object GetCurrentPattern(int patternId);
            void _14(); void _15(); void _16(); void _17(); void _18(); void _19(); void _20();
            void _21(); void _22(); void _23(); void _24();
            int CurrentIsEnabled { get; }
        }

        [ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomation
        {
            void _0(); void _1(); void _2();
            [return: MarshalAs(UnmanagedType.Interface)]
            IUIAutomationElement ElementFromHandle(IntPtr hwnd);
            void _4(); void _5(); void _6(); void _7(); void _8(); void _9(); void _10();
            void _11(); void _12(); void _13(); void _14(); void _15(); void _16(); void _17(); void _18(); void _19();
            [return: MarshalAs(UnmanagedType.Interface)]
            IUIAutomationCondition CreatePropertyCondition(int propertyId, [MarshalAs(UnmanagedType.Struct)] object value);
            void _21();
            [return: MarshalAs(UnmanagedType.Interface)]
            IUIAutomationCondition CreateAndCondition(IUIAutomationCondition c1, IUIAutomationCondition c2);
        }

        private static bool TryFindById(IUIAutomation uia, IntPtr hwnd, string id, int patternPropId, out IUIAutomationElement el)
        {
            el = null;
            IUIAutomationElement root = uia.ElementFromHandle(hwnd);
            if (root == null) return false;
            IUIAutomationCondition c1 = uia.CreatePropertyCondition(AutomationIdPropId, id);
            IUIAutomationCondition c2 = uia.CreatePropertyCondition(patternPropId, true);
            IUIAutomationCondition and = uia.CreateAndCondition(c1, c2);
            el = root.FindFirst(TreeScopeDescendants, and);
            return true;
        }

        private static IUIAutomationElement FindById(IUIAutomation uia, IntPtr hwnd, string id, int patternPropId)
        {
            IUIAutomationElement el;
            TryFindById(uia, hwnd, id, patternPropId, out el);
            return el;
        }

        private static IntPtr FindWindowByProp(int pid, string propName)
        {
            IntPtr found = IntPtr.Zero;
            EnumWindows(delegate(IntPtr hwnd, IntPtr lparam)
            {
                if (GetProp(hwnd, propName) == IntPtr.Zero) return true;
                uint wpid;
                GetWindowThreadProcessId(hwnd, out wpid);
                if (wpid != (uint)pid) return true;
                found = hwnd;
                return false;
            }, IntPtr.Zero);
            return found;
        }

        [MTAThread]
        private static int Main(string[] args)
        {
            for (int i = 0; i < args.Length; i++)
            {
                if (string.Equals(args[i], "--hang", StringComparison.Ordinal))
                {
                    Thread.Sleep(Timeout.Infinite);
                    return 2;
                }
            }
            if (args.Length < 1) return 1;
            string cmd = args[0];
            if (string.Equals(cmd, "find-window", StringComparison.Ordinal) && args.Length >= 3)
            {
                int pid = int.Parse(args[1]);
                string prop = args[2];
                for (;;)
                {
                    IntPtr w = FindWindowByProp(pid, prop);
                    if (w != IntPtr.Zero)
                    {
                        Console.WriteLine(w.ToInt64().ToString());
                        return 0;
                    }
                    Thread.Sleep(100);
                }
            }
            Guid clsid = new Guid("E22AD333-B25F-460C-83D0-0581107395C9");
            IUIAutomation uia = (IUIAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(clsid));
            try
            {
                if (string.Equals(cmd, "wait-toggle", StringComparison.Ordinal) && args.Length >= 4)
                {
                    IntPtr hwnd = new IntPtr(long.Parse(args[1]));
                    string id = args[2];
                    int expected = int.Parse(args[3]);
                    for (;;)
                    {
                        try
                        {
                            IUIAutomationElement el = FindById(uia, hwnd, id, IsToggleAvailablePropId);
                            IUIAutomationTogglePattern pat = el == null ? null : el.GetCurrentPattern(TogglePatternId) as IUIAutomationTogglePattern;
                            if (pat != null && pat.CurrentToggleState == expected) return 0;
                        }
                        catch (COMException) { }
                        Thread.Sleep(100);
                    }
                }
                if (string.Equals(cmd, "toggle", StringComparison.Ordinal) && args.Length >= 3)
                {
                    IntPtr hwnd = new IntPtr(long.Parse(args[1]));
                    string id = args[2];
                    for (;;)
                    {
                        try
                        {
                            IUIAutomationElement el = FindById(uia, hwnd, id, IsToggleAvailablePropId);
                            IUIAutomationTogglePattern pat = el == null ? null : el.GetCurrentPattern(TogglePatternId) as IUIAutomationTogglePattern;
                            if (pat != null) { pat.Toggle(); return 0; }
                        }
                        catch (COMException) { }
                        Thread.Sleep(100);
                    }
                }
                if (string.Equals(cmd, "wait-toggle-enabled", StringComparison.Ordinal) && args.Length >= 5)
                {
                    IntPtr hwnd = new IntPtr(long.Parse(args[1]));
                    string toggleId = args[2];
                    int expected = int.Parse(args[3]);
                    string invokeId = args[4];
                    for (;;)
                    {
                        try
                        {
                            IUIAutomationElement tel = FindById(uia, hwnd, toggleId, IsToggleAvailablePropId);
                            IUIAutomationTogglePattern tpat = tel == null ? null : tel.GetCurrentPattern(TogglePatternId) as IUIAutomationTogglePattern;
                            IUIAutomationElement iel = FindById(uia, hwnd, invokeId, IsInvokeAvailablePropId);
                            if (tpat != null && tpat.CurrentToggleState == expected && iel != null && iel.CurrentIsEnabled != 0) return 0;
                        }
                        catch (COMException) { }
                        Thread.Sleep(100);
                    }
                }
                if (string.Equals(cmd, "invoke", StringComparison.Ordinal) && args.Length >= 3)
                {
                    IntPtr hwnd = new IntPtr(long.Parse(args[1]));
                    string id = args[2];
                    for (;;)
                    {
                        try
                        {
                            IUIAutomationElement el = FindById(uia, hwnd, id, IsInvokeAvailablePropId);
                            IUIAutomationInvokePattern pat = el == null ? null : el.GetCurrentPattern(InvokePatternId) as IUIAutomationInvokePattern;
                            if (pat != null) { pat.Invoke(); return 0; }
                        }
                        catch (COMException) { }
                        Thread.Sleep(100);
                    }
                }
                if (string.Equals(cmd, "wait-disabled", StringComparison.Ordinal) && args.Length >= 3)
                {
                    IntPtr hwnd = new IntPtr(long.Parse(args[1]));
                    string id = args[2];
                    for (;;)
                    {
                        try
                        {
                            IUIAutomationElement el;
                            if (TryFindById(uia, hwnd, id, IsInvokeAvailablePropId, out el) &&
                                (el == null || el.CurrentIsEnabled == 0)) return 0;
                        }
                        catch (COMException) { }
                        Thread.Sleep(100);
                    }
                }
                return 1;
            }
            finally
            {
                if (uia != null && Marshal.IsComObject(uia)) Marshal.ReleaseComObject(uia);
            }
        }
    }
}
'@
        [IO.File]::WriteAllText($probeSource, $probeCode, (New-Object Text.UTF8Encoding $false))
        $cscOut = & $csc /nologo /optimize+ /target:exe /reference:System.dll "/out:$probeExe" $probeSource 2>&1
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $probeExe -PathType Leaf)) {
            throw "Failed to compile UiaProbe.exe (exit $LASTEXITCODE): $($cscOut -join "`n")"
        }
        $smokeDataDir = [IO.Path]::GetFullPath((Join-Path $temp '--exit-case'))
        New-Item -ItemType Directory -Path $smokeDataDir -Force | Out-Null
        $smokeSettingsPath = [IO.Path]::GetFullPath((Join-Path $smokeDataDir 'settings.json'))
        $logPath = Join-Path $smokeDataDir 'logs\core.log'
        $settingsLogPath = Join-Path $smokeDataDir 'logs\settings.log'
        $core = Join-Path $pkgFull 'Atrium.exe'
        $settingsExe = Join-Path $pkgFull 'Settings\Atrium.Settings.exe'
        $eventName = 'Local\Atrium.Update.Core.' + [Guid]::NewGuid().ToString('N')
        $ready = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
        $beforeSaveHandle = $null
        if ($BeforeSaveResumeEvent) {
            $beforeSaveHandle = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $BeforeSaveResumeEvent)
        }
        $job = [IntPtr]::Zero
        $job = [AtriumVerify.NativeHost]::CreateKillOnCloseJob()
        $proc = $null
        $settingsProcs = New-Object 'System.Collections.Generic.List[Diagnostics.Process]'
        $settingsWaitHandles = New-Object 'System.Collections.Generic.List[System.Threading.EventWaitHandle]'
        function Add-JobProcess([IntPtr]$jobHandle, [Diagnostics.Process]$p) {
            try {
                if ($p -and !$p.HasExited) { [AtriumVerify.NativeHost]::AssignProcess($jobHandle, $p.Handle) }
            } catch [InvalidOperationException] { }
        }
        function Invoke-UiaStep([string]$StepName, [string]$ControlId, [IntPtr]$Hwnd, [string[]]$ProbeArgs) {
            $outPath = Join-Path $temp ($StepName + '.stdout.txt')
            $errPath = Join-Path $temp ($StepName + '.stderr.txt')
            if (Test-Path -LiteralPath $outPath) { Remove-Item -LiteralPath $outPath -Force }
            if (Test-Path -LiteralPath $errPath) { Remove-Item -LiteralPath $errPath -Force }
            $allArgs = @($ProbeArgs)
            if ($HangUiaStep -and [string]::Equals($HangUiaStep, $StepName, [StringComparison]::OrdinalIgnoreCase)) {
                $allArgs += '--hang'
            }
            $argLine = ($allArgs | ForEach-Object { Format-ProcessArg $_ }) -join ' '
            $stepSw = [Diagnostics.Stopwatch]::StartNew()
            $probe = Start-Process -FilePath $probeExe -ArgumentList $argLine `
                -RedirectStandardOutput $outPath -RedirectStandardError $errPath -WindowStyle Hidden -PassThru
            try {
                Add-JobProcess $job $probe
                if (!$probe.WaitForExit($UiaTimeoutMs)) {
                    $stepSw.Stop()
                    try { $probe.Kill() } catch { }
                    [void]$probe.WaitForExit(3000)
                    $hwndHex = '0x{0:X}' -f $Hwnd.ToInt64()
                    $elapsedMs = $stepSw.ElapsedMilliseconds
                    throw "UIA step '$StepName' timed out after ${UiaTimeoutMs}ms (elapsed=${elapsedMs}ms, control=$ControlId, hwnd=$hwndHex)."
                }
                if ($probe.ExitCode -ne 0) {
                    $errRaw = if (Test-Path -LiteralPath $errPath) { Get-Content -LiteralPath $errPath -Raw } else { '' }
                    $errText = if ($errRaw) { $errRaw.Trim() } else { '' }
                    $hwndHex = '0x{0:X}' -f $Hwnd.ToInt64()
                    throw "UIA step '$StepName' failed (exit=$($probe.ExitCode), control=$ControlId, hwnd=$hwndHex): $errText"
                }
                if (Test-Path -LiteralPath $outPath) {
                    $outRaw = Get-Content -LiteralPath $outPath -Raw
                    if ($outRaw) { return $outRaw.Trim() }
                }
                return ''
            } finally {
                $probe.Dispose()
            }
        }
        try {
            $coreArgs = "--development --data-dir $(Format-ProcessArg $smokeDataDir) --ready-event $eventName"
            if ($PipeName) { $coreArgs += " --pipe-name $(Format-ProcessArg $PipeName)" }
            $proc = Start-Process -FilePath $core -ArgumentList $coreArgs -WorkingDirectory $temp -WindowStyle Hidden -PassThru
            Add-JobProcess $job $proc
            $readyOk = $ready.WaitOne(15000)
            $exited = $proc.HasExited
            if (!$readyOk -or $exited) {
                throw "Atrium.exe failed smoke startup (ready=$readyOk, exited=$exited" + $(if ($exited) { ", exitCode=$($proc.ExitCode)" } else { '' }) + ').'
            }
            $effectivePipe = if ($PipeName) { $PipeName } else { "Atrium.Core.$([Diagnostics.Process]::GetCurrentProcess().SessionId)" }
            $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $effectivePipe, [System.IO.Pipes.PipeDirection]::InOut)
            try {
                $pipe.Connect(5000)
                $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
                $state = Send-PipeRequest $pipe '{"op":"get"}'
                if ($state.op -ne 'state' -or [int]$state.protocol -ne 1 -or
                    [string]::IsNullOrWhiteSpace([string]$state.instanceId) -or [int64]$state.revision -lt 1) {
                    throw "Unexpected get reply: $($state | ConvertTo-Json -Compress)"
                }
                $reportedPath = [string]$state.path
                if ([string]::IsNullOrWhiteSpace($reportedPath)) {
                    throw 'Core pipe get reply did not report settings path.'
                }
                $normReported = [IO.Path]::GetFullPath($reportedPath)
                if (![string]::Equals($normReported, $smokeSettingsPath, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "Core settings path isolation check failed: expected '$smokeSettingsPath', got '$normReported'."
                }
                $savedHeight = [double]$state.saved.height
                $previewHeight = if ($savedHeight -eq 72) { 64 } else { 72 }
                $previewReq = "{`"op`":`"preview`",`"settings`":{`"height`":$previewHeight}}"
                $prevReply = Send-PipeRequest $pipe $previewReq
                if ($prevReply.op -ne 'ok') { throw "Preview failed: $($prevReply | ConvertTo-Json -Compress)" }
                $during = Send-PipeRequest $pipe '{"op":"get"}'
                if ([double]$during.current.height -ne $previewHeight -or [double]$during.saved.height -ne $savedHeight) {
                    throw "Preview state mismatch: current=$($during.current.height), saved=$($during.saved.height)"
                }
            } finally {
                $pipe.Dispose()
            }
            Start-Sleep -Milliseconds 300
            $pipe2 = New-Object System.IO.Pipes.NamedPipeClientStream('.', $effectivePipe, [System.IO.Pipes.PipeDirection]::InOut)
            try {
                $pipe2.Connect(5000)
                $pipe2.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
                $after = Send-PipeRequest $pipe2 '{"op":"get"}'
                $expectedInstanceId = [string]$state.instanceId
                $expectedRev = [int64]$state.revision
                if ([double]$after.current.height -ne $savedHeight -or
                    [string]$after.instanceId -ne $expectedInstanceId -or [int64]$after.revision -ne $expectedRev) {
                    throw "Preview lease rollback mismatch: $($after | ConvertTo-Json -Compress)"
                }
                $conflictReq = @{
                    op = 'commit'
                    instanceId = $expectedInstanceId
                    expectedRevision = 0
                    settings = $state.saved
                } | ConvertTo-Json -Depth 16 -Compress
                $conflict = Send-PipeRequest $pipe2 $conflictReq
                if ($conflict.op -ne 'conflict' -or [string]$conflict.instanceId -ne $expectedInstanceId -or
                    [int64]$conflict.revision -ne $expectedRev -or $null -eq $conflict.saved) {
                    throw "Expected conflict at revision ${expectedRev}: $($conflict | ConvertTo-Json -Compress)"
                }
                $commitReq = @{
                    op = 'commit'
                    instanceId = $expectedInstanceId
                    expectedRevision = $expectedRev
                    settings = $state.saved
                } | ConvertTo-Json -Depth 16 -Compress
                $commitReply = Send-PipeRequest $pipe2 $commitReq
                if ($commitReply.op -ne 'ok' -or [string]$commitReply.instanceId -ne $expectedInstanceId -or
                    [int64]$commitReply.revision -ne ($expectedRev + 1)) {
                    throw "Expected commit ok at revision $($expectedRev + 1): $($commitReply | ConvertTo-Json -Compress)"
                }
            } finally {
                $pipe2.Dispose()
            }
            $sEvent1 = 'Local\Atrium.Update.Settings.' + [Guid]::NewGuid().ToString('N')
            $sReady1 = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $sEvent1)
            $settingsWaitHandles.Add($sReady1)
            $sArgs1 = "--development --data-dir $(Format-ProcessArg $smokeDataDir) --page clock --require-core-ready --ready-event $sEvent1"
            if ($PipeName) { $sArgs1 += " --pipe-name $(Format-ProcessArg $PipeName)" }
            $sProc1 = Start-Process -FilePath $settingsExe -ArgumentList $sArgs1 -WorkingDirectory $temp -PassThru
            $settingsProcs.Add($sProc1)
            Add-JobProcess $job $sProc1
            if (!$sReady1.WaitOne(20000) -or $sProc1.HasExited) { throw 'Atrium.Settings.exe failed first smoke startup.' }
            $hwnd1Text = Invoke-UiaStep 'first-find-window' 'Atrium.Settings.Window' ([IntPtr]::Zero) `
                @('find-window', "$($sProc1.Id)", 'Atrium.Settings.Window')
            $hwnd1 = [IntPtr]([int64]$hwnd1Text)
            [void](Invoke-UiaStep 'first-wait-toggle' 'ClockSecondsSwitch' $hwnd1 @('wait-toggle', "$($hwnd1.ToInt64())", 'ClockSecondsSwitch', '0'))
            [void](Invoke-UiaStep 'first-toggle' 'ClockSecondsSwitch' $hwnd1 @('toggle', "$($hwnd1.ToInt64())", 'ClockSecondsSwitch'))
            [void](Invoke-UiaStep 'first-wait-toggle-enabled' 'ClockSecondsSwitch,SaveButton' $hwnd1 `
                @('wait-toggle-enabled', "$($hwnd1.ToInt64())", 'ClockSecondsSwitch', '1', 'SaveButton'))
            if ($BeforeSaveMarkerPath) {
                $markerDir = Split-Path -Parent $BeforeSaveMarkerPath
                if ($markerDir -and !(Test-Path -LiteralPath $markerDir -PathType Container)) {
                    New-Item -ItemType Directory -Path $markerDir -Force | Out-Null
                }
                $markerTmp = $BeforeSaveMarkerPath + '.tmp'
                [IO.File]::WriteAllText($markerTmp, "$PID`n$temp", (New-Object Text.UTF8Encoding $false))
                if (Test-Path -LiteralPath $BeforeSaveMarkerPath) { Remove-Item -LiteralPath $BeforeSaveMarkerPath -Force }
                [IO.File]::Move($markerTmp, $BeforeSaveMarkerPath)
            }
            if ($beforeSaveHandle) { [void]$beforeSaveHandle.WaitOne() }
            [void](Invoke-UiaStep 'first-invoke' 'SaveButton' $hwnd1 @('invoke', "$($hwnd1.ToInt64())", 'SaveButton'))
            Wait-Until $UiaTimeoutMs {
                if (!(Test-Path -LiteralPath $smokeSettingsPath -PathType Leaf)) { return $false }
                try {
                    $disk = Get-Content -LiteralPath $smokeSettingsPath -Raw -Encoding UTF8 | ConvertFrom-Json
                    return ($disk.showClockSeconds -eq $true)
                } catch {
                    return $false
                }
            } "Saved settings.json missing showClockSeconds=true: $smokeSettingsPath"
            [void](Invoke-UiaStep 'first-wait-disabled' 'SaveButton' $hwnd1 @('wait-disabled', "$($hwnd1.ToInt64())", 'SaveButton'))
            if (![AtriumVerify.NativeHost]::PostClose($hwnd1)) { throw 'PostMessage(WM_CLOSE) failed on first Settings window.' }
            if (!$sProc1.WaitForExit(15000) -or $sProc1.ExitCode -ne 0) {
                throw "First Atrium.Settings.exe did not exit cleanly (exited=$($sProc1.HasExited), exitCode=$($sProc1.ExitCode))."
            }
            $pipe3 = New-Object System.IO.Pipes.NamedPipeClientStream('.', $effectivePipe, [System.IO.Pipes.PipeDirection]::InOut)
            try {
                $pipe3.Connect(5000)
                $pipe3.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
                $afterSave = Send-PipeRequest $pipe3 '{"op":"get"}'
                if ($afterSave.saved.showClockSeconds -ne $true -or $afterSave.current.showClockSeconds -ne $true) {
                    throw "Core pipe get mismatch after UI save: $($afterSave | ConvertTo-Json -Compress)"
                }
            } finally {
                $pipe3.Dispose()
            }
            $sEvent2 = 'Local\Atrium.Update.Settings.' + [Guid]::NewGuid().ToString('N')
            $sReady2 = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $sEvent2)
            $settingsWaitHandles.Add($sReady2)
            $sArgs2 = "--development --data-dir $(Format-ProcessArg $smokeDataDir) --page clock --require-core-ready --ready-event $sEvent2"
            if ($PipeName) { $sArgs2 += " --pipe-name $(Format-ProcessArg $PipeName)" }
            $sProc2 = Start-Process -FilePath $settingsExe -ArgumentList $sArgs2 -WorkingDirectory $temp -PassThru
            $settingsProcs.Add($sProc2)
            Add-JobProcess $job $sProc2
            if (!$sReady2.WaitOne(20000) -or $sProc2.HasExited) { throw 'Atrium.Settings.exe failed second smoke startup.' }
            $hwnd2Text = Invoke-UiaStep 'second-find-window' 'Atrium.Settings.Window' ([IntPtr]::Zero) `
                @('find-window', "$($sProc2.Id)", 'Atrium.Settings.Window')
            $hwnd2 = [IntPtr]([int64]$hwnd2Text)
            [void](Invoke-UiaStep 'second-wait-toggle' 'ClockSecondsSwitch' $hwnd2 @('wait-toggle', "$($hwnd2.ToInt64())", 'ClockSecondsSwitch', '1'))
            if (![AtriumVerify.NativeHost]::PostClose($hwnd2)) { throw 'PostMessage(WM_CLOSE) failed on second Settings window.' }
            if (!$sProc2.WaitForExit(15000) -or $sProc2.ExitCode -ne 0) {
                throw "Second Atrium.Settings.exe did not exit cleanly (exited=$($sProc2.HasExited), exitCode=$($sProc2.ExitCode))."
            }
            $seenSettings = [Collections.Generic.HashSet[int]]::new()
            foreach ($p in @(Get-Process -Name 'Atrium.Settings' -ErrorAction SilentlyContinue)) { [void]$seenSettings.Add($p.Id) }
            foreach ($sp in $settingsProcs) { [void]$seenSettings.Add($sp.Id) }
            $coreHost = [AtriumVerify.NativeHost]::FindHostWindow($proc.Id)
            if ($coreHost -eq [IntPtr]::Zero) { throw 'Core host window not found.' }
            if (![AtriumVerify.NativeHost]::PostTrayOpen($coreHost)) { throw 'PostTrayOpen failed.' }
            $tray = @{ Pid = 0; Cmd = '' }
            Wait-Until 15000 {
                foreach ($w in @(Get-CimInstance Win32_Process -Filter "Name='Atrium.Settings.exe'" -ErrorAction SilentlyContinue)) {
                    $wpid = [int]$w.ProcessId
                    if (!$seenSettings.Contains($wpid) -and
                        [string]::Equals([string]$w.ExecutablePath, $settingsExe, [StringComparison]::OrdinalIgnoreCase)) {
                        $tray.Pid = $wpid; $tray.Cmd = [string]$w.CommandLine; return $true
                    }
                }
                return $false
            } 'Atrium.Settings.exe did not launch from Core tray open.'
            $trayProc = Get-Process -Id $tray.Pid
            $settingsProcs.Add($trayProc)
            Add-JobProcess $job $trayProc
            foreach ($tok in @('--development', '--data-dir', $smokeDataDir)) {
                if ($tray.Cmd.IndexOf($tok, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
                    throw "Tray-launched Settings missing '$tok' in CommandLine: $($tray.Cmd)"
                }
            }
            $hwndTrayText = Invoke-UiaStep 'tray-find-window' 'Atrium.Settings.Window' ([IntPtr]::Zero) `
                @('find-window', "$($tray.Pid)", 'Atrium.Settings.Window')
            $hwndTray = [IntPtr]([int64]$hwndTrayText)
            if (![AtriumVerify.NativeHost]::PostClose($hwndTray)) { throw 'PostMessage(WM_CLOSE) failed on tray Settings window.' }
            if (!$trayProc.WaitForExit(15000) -or $trayProc.ExitCode -ne 0) {
                throw "Tray Atrium.Settings.exe did not exit cleanly (exited=$($trayProc.HasExited), exitCode=$($trayProc.ExitCode))."
            }
            $exitArgs = '--exit'
            if ($PipeName) { $exitArgs += " --pipe-name $(Format-ProcessArg $PipeName)" }
            $exit = Start-Process -FilePath $core -ArgumentList $exitArgs -WorkingDirectory $temp -WindowStyle Hidden -PassThru
            try {
                Add-JobProcess $job $exit
                $exitDone = $exit.WaitForExit(15000)
                $exitCode = if ($exitDone) { $exit.ExitCode } else { 'timeout' }
                if (!$exitDone -or $exitCode -ne 0 -or !$proc.WaitForExit(15000)) {
                    throw "Atrium.exe --exit failed (exitCode=$exitCode, exited=$($proc.HasExited))."
                }
            } finally {
                $exit.Dispose()
            }
            if (!(Test-Path -LiteralPath $logPath -PathType Leaf)) { throw "Missing core.log: $logPath" }
            $logText = Read-LogText $logPath
            if ($logText -notmatch 'core: starting' -or $logText -notmatch 'core: exited') {
                throw "core.log missing expected startup/exit lines.`nLog:`n$logText"
            }
            if ($logText -match 'render thread stuck') {
                throw "core.log reported render thread stuck.`nLog:`n$logText"
            }
        } catch {
            $coreText = Read-LogText $logPath
            $settingsText = Read-LogText $settingsLogPath
            throw "$($_.Exception.Message)`ncore.log:`n$coreText`nsettings.log:`n$settingsText"
        } finally {
            if ($job -ne [IntPtr]::Zero) { [AtriumVerify.NativeHost]::CloseJob($job) }
            foreach ($sp in $settingsProcs) {
                if (!$sp.HasExited) {
                    Stop-Process -Id $sp.Id -Force -ErrorAction SilentlyContinue
                    [void]$sp.WaitForExit(3000)
                }
                $sp.Dispose()
            }
            foreach ($wh in $settingsWaitHandles) { $wh.Dispose() }
            if ($proc -and !$proc.HasExited) {
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                [void]$proc.WaitForExit(3000)
            }
            if ($proc) { $proc.Dispose() }
            $pkgPrefix = $pkgFull + '\'
            foreach ($orphan in @(Get-Process -ErrorAction SilentlyContinue)) {
                try {
                    if ($orphan.Path -and $orphan.Path.StartsWith($pkgPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                        Stop-Process -Id $orphan.Id -Force -ErrorAction SilentlyContinue
                        [void]$orphan.WaitForExit(3000)
                    }
                } catch { }
            }
            if ($beforeSaveHandle) { $beforeSaveHandle.Dispose() }
            $ready.Dispose()
            if ($EvidenceDirectory) {
                New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
                foreach ($evLog in @($logPath, $settingsLogPath)) {
                    if (Test-Path -LiteralPath $evLog -PathType Leaf) {
                        Copy-Item -LiteralPath $evLog -Destination (Join-Path $EvidenceDirectory ([IO.Path]::GetFileName($evLog))) -Force
                    }
                }
            }
        }
    }
    Write-Host "Package verified: $ZipPath ($expectedVersion)"
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }
}
