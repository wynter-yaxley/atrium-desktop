# Structural checks for the whole repository; any finding fails.
#   ./tools/lint.ps1                     text, native rules, ratchets, clang-format (the whole native tree)
#   ./tools/lint.ps1 -Tidy               also clang-tidy (.clang-tidy; a Ninja build in native/build/tidy)
#   ./tools/lint.ps1 -DotNet             also the C# projects, built with their style and quality rules
#   ./tools/lint.ps1 -Fix                format first (clang-format, dotnet format)
#   ./tools/lint.ps1 -Base origin/main   only the changed files for clang-tidy; -MaxLines budget
#   -Paths <globs>                       the change may touch only these, and answers only for them
#   -BuildDir <dir>                      reuse an existing Ninja build for -Tidy
#   ./tools/lint.ps1 -Tidy -UpdateBaseline  record the ratchets (only ever lower them)
#   -Root <tree>                         check another checkout with these rules
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$RulesRoot,
    [string]$BuildDir,
    [string]$Base,
    [string[]]$Paths,
    [int]$MaxLines = 0,
    [string[]]$SeedMetric,
    [switch]$Tidy,
    [switch]$DotNet,
    [switch]$Fix,
    [switch]$UpdateBaseline
)
$ErrorActionPreference = 'Stop'
$rules = if ($RulesRoot) { $RulesRoot } else { $Root }
$core = Join-Path $Root 'native/core'
$baselinePath = Join-Path $Root 'tools/lint-baseline.json'
$errors = [System.Collections.Generic.List[string]]::new()
$counts = [ordered]@{}
$Paths = @($Paths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$SeedMetric = @($SeedMetric | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($UpdateBaseline -and ($Base -or $Paths -or $MaxLines -or -not $Tidy)) {
    throw 'lint: -UpdateBaseline requires -Tidy and cannot be used with -Base, -Paths, or -MaxLines'
}
if ($SeedMetric -and -not $UpdateBaseline) {
    throw 'lint: -SeedMetric requires -UpdateBaseline -Tidy and cannot be used with -Base, -Paths, or -MaxLines'
}

function Rel([string]$path) { [IO.Path]::GetRelativePath($Root, $path).Replace('\', '/') }
function Fail([string]$where, [string]$rule, [string]$why) { $errors.Add("${where}: ${rule}: $why") }
function BaselineFile([string]$keyFile) { if ($keyFile -match '^(?:settings|tools)/') { $keyFile } else { "native/$keyFile" } }
function Get-KeyMetricTag([string]$key) {
    $p = $key.Split('|', 3)
    if ($p[1] -eq 'file-lines' -and $p[0] -match '^(settings|tools)/') { "$($Matches[1])/file-lines" } else { $p[1] }
}
function Read-Baseline([hashtable]$map) {
    foreach ($k in @($map.Keys)) {
        $v = $map[$k]
        if (($v -is [int] -or $v -is [long]) -and $v -gt 0 -and $v -le [int]::MaxValue) { $map[$k] = [int]$v }
        else { Fail 'tools/lint-baseline.json' 'baseline' "$k must be a positive integer"; $map[$k] = 0 }
    }
    $map
}
$baseline = Read-Baseline $(if (Test-Path $baselinePath) { Get-Content $baselinePath -Raw | ConvertFrom-Json -AsHashtable } else { @{} })
if ($SeedMetric) {
    $existingTags = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($k in $baseline.Keys) { [void]$existingTags.Add((Get-KeyMetricTag $k)) }
    foreach ($tag in $SeedMetric) {
        if ($existingTags.Contains($tag)) { throw "lint: -SeedMetric '$tag' already has entries in tools/lint-baseline.json" }
    }
}
function Tail([string[]]$lines, [int]$n = 15) {
    if (-not $lines -or -not $lines.Count) { return '(no output)' }
    ($lines | Select-Object -Last $n) -join "`n"
}
function Invoke-Tool([string]$Exe, [string[]]$ToolArgs, [switch]$AllowNonZero) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = -1
        $lines = @(& $Exe @ToolArgs 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } catch {
        throw "lint: $Exe failed: $_"
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($code -ne 0 -and -not $AllowNonZero) {
        throw "lint: $Exe $($ToolArgs -join ' ') failed (exit $code):`n$(Tail $lines)"
    }
    [pscustomobject]@{ ExitCode = $code; Lines = $lines }
}
function Test-Glob([string]$path, [string[]]$globs) {
    $p = $path.Replace('\', '/') -replace '^\./', ''
    foreach ($g in $globs) {
        $rx = '^'
        $i = 0
        $n = $g.Replace('\', '/') -replace '^\./', ''
        while ($i -lt $n.Length) {
            if ($i + 2 -lt $n.Length -and $n.Substring($i, 3) -eq '**/') { $rx += '(?:.+/)?'; $i += 3 }
            elseif ($i + 1 -lt $n.Length -and $n.Substring($i, 2) -eq '**') { $rx += '.*'; $i += 2 }
            elseif ($n[$i] -eq '*') { $rx += '[^/]*'; $i++ }
            elseif ($n[$i] -eq '?') { $rx += '[^/]'; $i++ }
            else { $rx += [regex]::Escape($n[$i]); $i++ }
        }
        if ($p -match "$rx`$") { return $true }
    }
    $false
}
# The LLVM tools of one major version: another clang-tidy brings other checks, another clang-format other layouts.
$llvmMajor = 19
function Llvm([string]$exe) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath 2>$null } else { $null }
    $candidates = @((Get-Command $exe -All -ErrorAction SilentlyContinue).Source) +
        $(if ($vs) { @("$vs\VC\Tools\Llvm\x64\bin\$exe") } else { @() })
    $candidates | Where-Object { $_ -and (Test-Path $_) -and (& $_ --version) -match "version $llvmMajor\." } | Select-Object -First 1
}

$diffStatus = @()
$diffNumstat = @()
$touchedFiles = @()
$fullRun = $false
if ($Base) {
    Invoke-Tool 'git' @('-C', $Root, 'rev-parse', '--verify', "$Base^{commit}") | Out-Null
    $mb = Invoke-Tool 'git' @('-C', $Root, 'merge-base', $Base, 'HEAD') -AllowNonZero
    if ($mb.ExitCode -ne 0 -or -not $mb.Lines) {
        throw "lint: git merge-base $Base HEAD failed (shallow clone or no common ancestor): $(Tail $mb.Lines)"
    }
    $mergeBase = @($mb.Lines)[0].Trim()
    $prevIndex = $env:GIT_INDEX_FILE
    $realIndex = if ($prevIndex) { $prevIndex } else {
        @((Invoke-Tool 'git' @('-C', $Root, 'rev-parse', '--path-format=absolute', '--git-path', 'index')).Lines)[0].Trim()
    }
    $tmpIndex = Join-Path ([IO.Path]::GetTempPath()) "atrium-lint-$([Guid]::NewGuid().ToString('N')).index"
    try {
        if (Test-Path $realIndex) { Copy-Item $realIndex $tmpIndex -Force }
        $env:GIT_INDEX_FILE = $tmpIndex
        $untracked = @((Invoke-Tool 'git' @('-C', $Root, '-c', 'core.quotepath=false', 'ls-files', '--others', '--exclude-standard')).Lines)
        if ($untracked) { Invoke-Tool 'git' (@('-C', $Root, 'add', '-N', '--') + $untracked) | Out-Null }
        $diffStatus = @((Invoke-Tool 'git' @('-C', $Root, '-c', 'core.quotepath=false', 'diff', '--name-status', '-M', $mergeBase, '--', '.')).Lines)
        $diffNumstat = @((Invoke-Tool 'git' @('-C', $Root, '-c', 'core.quotepath=false', 'diff', '--numstat', $mergeBase, '--', '.')).Lines)
    } finally {
        $env:GIT_INDEX_FILE = $prevIndex
        Remove-Item $tmpIndex, "$tmpIndex.lock" -ErrorAction SilentlyContinue
    }
    $touchedFiles = @($diffStatus | ForEach-Object {
        $parts = $_ -split "`t"
        if ($parts[0] -like 'R*') { $parts[1], $parts[2] } elseif ($parts[0] -like 'C*') { $parts[2] } else { $parts[1] }
    })
    $fullConfig = '.clang-tidy', '.clang-format', '.editorconfig', '.gitattributes', 'Directory.Build.props', 'global.json',
        'tools/lint.ps1', 'tools/lint-*.ps1', 'tools/lint-baseline.json', 'tools/check-localization.ps1',
        'native/CMakeLists.txt', 'native/**/CMakeLists.txt', 'native/CMakePresets.json', '.github/workflows/**'
    $fullRun = [bool]($touchedFiles | Where-Object { Test-Glob $_ $fullConfig } | Select-Object -First 1)
    if ($fullRun -and ($touchedFiles | Where-Object {
        Test-Glob $_ @('**/*.cs', '**/*.xaml', '**/*.csproj', '.editorconfig', 'Directory.Build.props', 'global.json')
    } | Select-Object -First 1)) { $DotNet = $true }
}

$files = @((Invoke-Tool 'git' @('-C', $Root, '-c', 'core.quotepath=false', 'ls-files', '--cached', '--others', '--exclude-standard')).Lines |
    Where-Object { Test-Path (Join-Path $Root $_) })
$native = @($files | Where-Object { $_ -match '^native/(core|tests)/.*\.(cpp|h|hlsl)$' })
$projects = @($files | Where-Object { $_ -match '\.csproj$' } | ForEach-Object { Join-Path $Root $_ })
$clangFormat = Llvm 'clang-format.exe'
$style = Join-Path $rules '.clang-format'
$sources = @($native | ForEach-Object { Join-Path $Root $_ })
if ($Fix) {
    if ($clangFormat -and $sources) {
        $rsp = [IO.Path]::GetTempFileName()
        try {
            [IO.File]::WriteAllLines($rsp, $sources)
            Invoke-Tool $clangFormat @('-i', "--style=file:$style", "--files=$rsp") | Out-Null
        } finally {
            Remove-Item $rsp -ErrorAction SilentlyContinue
        }
    }
    if ($DotNet) { $projects | ForEach-Object { Invoke-Tool 'dotnet' @('format', $_) | Out-Null } }
}

# --- Text: every source and configuration file ---------------------------------------------------------------
$fileLines = 800 # a file longer than this is split by what it does
$text = '\.(cpp|h|hlsl|cs|xaml|ps1|csproj|props|json|yml|md|cmake|txt|manifest|rc)$|(^|/)(CMakeLists\.txt|\.[a-z-]+)$'
$code = '\.(cpp|h|hlsl|cs|xaml|ps1)$'
foreach ($rel in $files | Where-Object { $_ -match $text -and $_ -notmatch '^tests/fixtures/' }) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $Root $rel))
    if (-not $bytes.Length) { continue }
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    if ($hasBom -and $rel -notmatch '\.ps1$') { Fail $rel 'bom' 'UTF-8 without a byte order mark' }
    if (-not $hasBom -and $rel -in @('tools/Uninstall.ps1', 'tools/Update.ps1')) {
        Fail $rel 'bom' 'UTF-8 with BOM required for PowerShell 5.1'
    }
    if ($bytes[-1] -ne 10) { Fail $rel 'final-newline' 'end the file with a newline' }
    $content = [Text.Encoding]::UTF8.GetString($bytes)
    $crlf = $rel -match '\.(ps1|sln)$'
    $lines = ($content -replace '\n\z', '').Split("`n")
    $blank = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $where = "${rel}:$($i + 1)"
        if ($line.EndsWith("`r")) {
            if (-not $crlf) { Fail $where 'line-ending' 'LF only' }
            $line = $line.Substring(0, $line.Length - 1)
        } elseif ($crlf) { Fail $where 'line-ending' 'CRLF for PowerShell' }
        if ($line.Contains("`t")) { Fail $where 'tab' 'indent with spaces' }
        if ($line -match '\s$' -and $rel -notmatch '\.md$') { Fail $where 'trailing-space' 'delete it' }
        # A line of one XAML attribute or one string (a sentence to translate) cannot be wrapped further.
        $unbreakable = ($rel.EndsWith('.xaml') -and $line -match '^\s*[\w:.]+="[^"]*"\s*/?>?\s*$') -or
            ($rel -match '\.(cs|cpp|h)$' -and $line -match '^\s*((Loc\.(?:T|F|Plural)|Tr(?:Format|Plural)?)\()?L?"(?:[^"\\]|\\.)*"\)?[,;)]*\s*$')
        if ($rel -match $code -and $line.Length -gt 160 -and -not $unbreakable) { Fail $where 'line-length' "$($line.Length) > 160 columns" }
        $blank = if ($line.Trim()) { 0 } else { $blank + 1 }
        if ($blank -eq 2 -and $rel -match $code) { Fail $where 'blank-lines' 'one blank line at most' }
    }
    if ($rel -match '^(?:settings/.*\.(?:cs|xaml)|tools/.*\.ps1)$' -and $lines.Count -gt $fileLines) {
        $counts["$rel|file-lines"] = $lines.Count
    }
}

# --- XAML: an element sits four spaces deeper than its parent; a start tag's other lines follow it ------------
foreach ($rel in $files | Where-Object { $_ -match '\.xaml$' }) {
    $lines = [IO.File]::ReadAllLines((Join-Path $Root $rel))
    $depth = 0
    $open = $false
    $inComment = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $raw = $lines[$i]
        $s = $raw.Trim()
        if ($inComment) {
            if ($s.Contains('-->')) { $inComment = $false }
            continue
        }
        if (-not $s.StartsWith('<!--')) {
            if ($s -match '<!--(?!.*-->)') { $inComment = $true; $s = ($s -replace '\s*<!--.*$', '').Trim() }
            else { $s = ($s -replace '\s*<!--.*?-->', '').Trim() }
        }
        if ($open) {
            if ($s.EndsWith('/>') -or $s -match '</[\w:.]+>$') { $open = $false } elseif ($s.EndsWith('>')) { $open = $false; $depth++ }
            continue
        }
        if (-not $s.StartsWith('<') -or $s.StartsWith('<?')) { continue }
        if ($s.StartsWith('</')) { $depth-- }
        $indent = $raw.Length - $raw.TrimStart(' ').Length
        if ($indent -ne 4 * $depth) { Fail "${rel}:$($i + 1)" 'xaml-indent' "indent $(4 * $depth) spaces, the element's depth" }
        if ($s.StartsWith('<!--')) {
            if (-not $s.Contains('-->')) { $inComment = $true }
            continue
        }
        if ($s.StartsWith('</') -or $s.EndsWith('/>') -or $s -match '</[\w:.]+>$') { continue }
        if ($s.EndsWith('>')) { $depth++ } else { $open = $true }
    }
}

# --- C# layer: pure logic/model files stay free of WinUI; LocalizationValidation compiles standalone ----------
$csLayer = 'BarSettings.cs', 'BitsTransfer.cs', 'CoreClient.cs', 'CornerProfile.cs', 'DesktopDisplay.cs', 'GameFinder.cs',
    'GameFinder.Parsers.cs', 'GlassPreview.cs', 'LocalizationValidation.cs', 'Photo.cs', 'PinnedApp.cs', 'ReleaseInfo.cs',
    'ReleaseInstaller.cs', 'ReleaseValidation.cs', 'SettingsSession.cs', 'ShellIcons.cs'
foreach ($rel in $files | Where-Object { $_ -match '^settings/Atrium\.Settings/.*\.cs$' }) {
    $name = [IO.Path]::GetFileName($rel)
    if ($csLayer -notcontains $name) { continue }
    $lines = [IO.File]::ReadAllLines((Join-Path $Root $rel))
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $codeLine = $lines[$i] -replace '//.*$', ''
        if ($codeLine -cmatch '\b(?:Microsoft\.UI|Microsoft\.Graphics|Windows\.UI)\b') {
            Fail "${rel}:$($i + 1)" 'csharp-layer' "$name may not reference WinUI/UI namespaces"
        } elseif ($name -eq 'LocalizationValidation.cs' -and (
            $codeLine -cmatch '^\s*(?:global\s+)?using\s+(?!var\b|\()(?:static\s+)?(?!(?:System\b|[A-Za-z_]\w*\s*=\s*System\b))' -or
            $codeLine -cmatch '\b(?:Microsoft|Windows)\.[A-Za-z_]')) {
            Fail "${rel}:$($i + 1)" 'csharp-layer' 'LocalizationValidation.cs may only use System namespaces'
        }
    }
    if ($name -eq 'LocalizationValidation.cs' -and (-not $Paths -or (Test-Glob $rel $Paths))) {
        $tmpDll = Join-Path ([IO.Path]::GetTempPath()) "atrium-loc-$([Guid]::NewGuid().ToString('N')).dll"
        try {
            $prelude = "#nullable enable`nglobal using System;`nglobal using System.Collections.Generic;`nglobal using System.Linq;`n"
            Add-Type -TypeDefinition ($prelude + ($lines -join "`n")) -OutputAssembly $tmpDll -ErrorAction Stop
        } catch {
            Fail $rel 'csharp-layer' "LocalizationValidation.cs must compile standalone: $($_.Exception.Message)"
        } finally {
            Remove-Item $tmpDll -ErrorAction SilentlyContinue
        }
    }
}

# --- Native: layers, header cycles, banned and ratcheted constructs -------------------------------------------
function Get-NativeCodeLines([string[]]$lines) {
    $out = [string[]]::new($lines.Count)
    $inComment = $false
    $rawEnd = $null
    $special = [char[]]@('/', '"', "'")
    $sb = [System.Text.StringBuilder]::new()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $len = $line.Length
        $pos = 0
        $null = $sb.Clear()
        while ($pos -lt $len) {
            if ($inComment) {
                $end = $line.IndexOf('*/', $pos, [StringComparison]::Ordinal)
                if ($end -lt 0) { $null = $sb.Append(' '); break }
                $null = $sb.Append(' '); $pos = $end + 2; $inComment = $false; continue
            }
            if ($null -ne $rawEnd) {
                $end = $line.IndexOf($rawEnd, $pos, [StringComparison]::Ordinal)
                if ($end -lt 0) { break }
                $pos = $end + $rawEnd.Length; $rawEnd = $null; continue
            }
            $hit = $line.IndexOfAny($special, $pos)
            if ($hit -lt 0) { $null = $sb.Append($line, $pos, $len - $pos); break }
            $ch = $line[$hit]
            if ($ch -eq '/') {
                if ($hit + 1 -lt $len -and $line[$hit + 1] -eq '/') { $null = $sb.Append($line, $pos, $hit - $pos); break }
                if ($hit + 1 -lt $len -and $line[$hit + 1] -eq '*') {
                    $null = $sb.Append($line, $pos, $hit - $pos).Append(' ')
                    $end = $line.IndexOf('*/', $hit + 2, [StringComparison]::Ordinal)
                    if ($end -lt 0) { $inComment = $true; break }
                    $pos = $end + 2; continue
                }
                $null = $sb.Append($line, $pos, $hit - $pos + 1); $pos = $hit + 1; continue
            }
            $seg = $line.Substring($pos, $hit - $pos)
            if ($ch -eq "'") {
                $hasPrefix = $seg -cmatch '(?:^|[^0-9A-Za-z_])(u8|[LuU])$'
                if ($hit -gt 0 -and $line[$hit - 1] -cmatch '[0-9A-Za-z_]' -and -not $hasPrefix) {
                    $null = $sb.Append($seg).Append("'"); $pos = $hit + 1; continue
                }
                if ($hasPrefix) { $seg = $seg.Substring(0, $seg.Length - $Matches[1].Length) }
                $null = $sb.Append($seg).Append("''")
                $j = $hit + 1
                while ($j -lt $len) { if ($line[$j] -eq '\') { $j += 2 } elseif ($line[$j] -eq "'") { $j++; break } else { $j++ } }
                $pos = [Math]::Min($j, $len); continue
            }
            if ($seg -cmatch '(?:^|[^0-9A-Za-z_])((?:u8|[LuU])?R)$') {
                $pfx = $Matches[1]
                if ($line.Substring($hit) -cmatch '^"([^()\\\s]{0,16})\(') {
                    $delim = $Matches[1]
                    $null = $sb.Append($seg, 0, $seg.Length - $pfx.Length).Append('""')
                    $rawEnd = ")$delim`""
                    $end = $line.IndexOf($rawEnd, $hit + 2 + $delim.Length, [StringComparison]::Ordinal)
                    if ($end -lt 0) { break }
                    $pos = $end + $rawEnd.Length; $rawEnd = $null; continue
                }
            }
            if ($seg -cmatch '(?:^|[^0-9A-Za-z_])(u8|[LuU])$') { $seg = $seg.Substring(0, $seg.Length - $Matches[1].Length) }
            $null = $sb.Append($seg).Append('""')
            $j = $hit + 1
            while ($j -lt $len) { if ($line[$j] -eq '\') { $j += 2 } elseif ($line[$j] -eq '"') { $j++; break } else { $j++ } }
            $pos = [Math]::Min($j, $len)
        }
        $out[$i] = $sb.ToString()
    }
    return ,$out
}

# Who may include whom. Root headers are either foundation (anyone) or composition (root only).
$foundation = 'bar_edge.h', 'clock.h', 'icon_image.h', 'localization.h', 'localization_format.h', 'log.h', 'motion.h', 'notice.h', 'posted.h', 'resource.h',
    'pinned_app.h', 'settings.h', 'stage_excluded.h', 'status.h', 'win32_path.h'
$layers = @{ render = @(); shell = @(); ui = @('render', 'shell') }
# Configuration macros set before the headers that read them; any other value is a constexpr.
$configMacros = 'SECURITY_WIN32', 'WIN32_LEAN_AND_MEAN', 'NOMINMAX', 'IDI_ATRIUM'
$rawBanned = [ordered]@{
    'todo'          = @('\b(TODO|FIXME|XXX|HACK)\b', 'open an issue instead')
    'include-guard' = @('^\s*#\s*ifndef\s+\w+_H\w*\s*$', 'use #pragma once')
    'nolint'        = @(('\bNOLINT(?:NEXTLINE|BEGIN)?\b(?:(?!\([\w.,-]+\): \S)' +
        '|\([^)]*(?:readability-function-cognitive-complexity|readability-function-size)[^)]*\))'),
        'name the check and the reason: NOLINT(check): why (no *, cognitive-complexity, or function-size)')
    'narration'     = @(('//\s*(Check|Get|Set|Call|Loop|Iterate|Increment|Decrement|Initialize|Create|Return|Update|Add|Remove|Handle)' +
        '\s+(if|the|a|an|whether|over|through)\b'), 'say why, not what the next line does')
}
$banned = [ordered]@{
    'null-macro' = @('\bNULL\b', 'use nullptr')
    'endl'       = @('std::endl', "use '\n'")
    'assert'     = @('\bassert\s*\(', 'handle the case or let it crash in a test')
    'using-std'  = @('using\s+namespace\s+std\b', 'qualify std::')
    'print'      = @('\b(w?printf|std::w?cout|std::w?cerr|OutputDebugString\w*)\s*[(<]', 'use Log (tests print their results)')
    'raw-new'    = @('(?<!\boperator\s)\bnew\s+(?:\([^)]*\)\s*)?[A-Za-z_:]', 'own it with std::unique_ptr / winrt::com_ptr')
    'raw-delete' = @('(?<!\boperator\s)\bdelete(?:\s*\[\s*\])?(?:\s+[A-Za-z_*&]|\s*\()', 'own it with std::unique_ptr / winrt::com_ptr')
    'pragma-lib' = @('#\s*pragma\s+comment\s*\(\s*lib\b', 'declare link libraries in native/core/CMakeLists.txt')
}
# Counted per file; a file may never hold more than its baseline.
$ratcheted = [ordered]@{
    'catch-all'      = @('catch\s*\(\.\.\.\)', 'catch what can be thrown, at the boundary that can act on it')
    'try'            = @('\btry\s*\{', 'exceptions belong at API boundaries (WinRT, std::stoi), not around internal calls')
    'sleep'          = @('\bSleep\s*\(', 'wait on an event or a timer')
    'goto'           = @('\bgoto\b', 'structure the loop')
    'detach'         = @('\.detach\s*\(', 'join threads on shutdown or manage lifetime explicitly')
    'commented-code' = @('^\s*//\s*([A-Za-z_][\w:]*(?:(?:\.|->)[A-Za-z_]\w*)*\s*(?:\([^;]*\)|[+\-*/]?=[^=][^;]*);|return\b[^;]*;|[{}])\s*$',
        'delete it; git keeps history')
}
$headerEdges = @{}
foreach ($rel in $native) { if ($rel -match '^native/core/.*\.h$') { $headerEdges[$rel.Substring(12)] = [System.Collections.Generic.List[string]]::new() } }

foreach ($rel in $native) {
    $path = Join-Path $Root $rel
    $lines = [IO.File]::ReadAllLines($path)
    $codeLines = Get-NativeCodeLines $lines
    $coreRel = if ($rel.StartsWith('native/core/')) { $rel.Substring(12) } else { '' }
    $from = if ($coreRel.Contains('/')) { $coreRel.Split('/')[0] } else { '' }
    $isCoreHeader = $coreRel -and $rel -match '\.h$'
    if ($rel -match '\.h$' -and $lines.Count -and $lines[0] -notmatch '^// ') { Fail "${rel}:1" 'header-comment' 'begin with what the header is for' }
    $ppStack = [System.Collections.Generic.List[string]]::new()
    $rsState = 0
    $rsDepth = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $codeLine = $codeLines[$i]
        $where = "${rel}:$($i + 1)"
        foreach ($rule in $rawBanned.Keys) { if ($line -cmatch $rawBanned[$rule][0]) { Fail $where $rule $rawBanned[$rule][1] } }
        foreach ($rule in $banned.Keys) {
            if ($codeLine -cmatch $banned[$rule][0] -and -not ($rule -eq 'print' -and ($rel -match '^native/tests/' -or $coreRel -eq 'log.cpp'))) {
                Fail $where $rule $banned[$rule][1]
            }
        }
        if ($line -match '^\s*using\s+namespace\b' -and $rel -match '\.h$') { Fail $where 'using-in-header' 'qualify names in headers' }
        if ($line -match '^\s*#\s*ifndef\s+(\w+)') { $ppStack.Add($(if ($Matches[1] -ne 'ATRIUM_DEVELOPMENT') { $Matches[1] } else { '' })) }
        elseif ($line -match '^\s*#\s*(?:if|ifdef)\b') { $ppStack.Add('') }
        elseif ($line -match '^\s*#\s*(?:else|elif)\b') { if ($ppStack.Count) { $ppStack[$ppStack.Count - 1] = '' } }
        elseif ($line -match '^\s*#\s*endif\b') { if ($ppStack.Count) { $ppStack.RemoveAt($ppStack.Count - 1) } }
        if ($line -match '^\s*#\s*define\s+(\w+)') {
            $macro = $Matches[1]
            $isConst = $line -cmatch '^\s*#\s*define\s+[A-Z][A-Z0-9_]*\b(?!\()'
            $guarded = $ppStack.Count -gt 0 -and $ppStack[-1]
            if (-not $isConst -or (-not $guarded -and $configMacros -notcontains $macro)) {
                Fail $where 'define' 'use constexpr (only uppercase constant macros under non-ATRIUM_DEVELOPMENT #ifndef)'
            }
        }
        if ($coreRel -and $line -match '^\s*#\s*include\s*([<"])([^>"]+)[>"]') {
            $inc = $Matches[2]
            if (-not $from -and $foundation -contains $coreRel -and $inc -match '^(?:winrt/|d3d|dcomp|uiautomation)') {
                Fail $where 'layer' "$coreRel may not include <$inc>"
            }
            $target = [IO.Path]::GetFullPath((Join-Path (Split-Path $path) $inc))
            if (-not (Test-Path $target)) { $target = [IO.Path]::GetFullPath((Join-Path $core $inc)) }
            if (Test-Path $target) {
                $to = [IO.Path]::GetRelativePath($core, $target).Replace('\', '/')
                if (-not $to.StartsWith('..') -and -not [IO.Path]::IsPathRooted($to)) {
                    if ($isCoreHeader -and $headerEdges.ContainsKey($to)) { [void]$headerEdges[$coreRel].Add($to) }
                    $toLayer = if ($to.Contains('/')) { $to.Split('/')[0] } else { '' }
                    if ($from) {
                        if (-not $(if ($toLayer) { $toLayer -eq $from -or $layers[$from] -contains $toLayer } else { $foundation -contains $to })) {
                            Fail $where 'layer' "$from/ may not include $to"
                        }
                    } elseif ($foundation -contains ($coreRel -replace '\.cpp$', '.h') -and ($toLayer -or $foundation -notcontains $to)) {
                        Fail $where 'layer' "$coreRel may not include $to"
                    }
                }
            }
        }
        if ($coreRel -eq 'bar.cpp') {
            if ($rsState -eq 0 -and $codeLine -cmatch '\bBar::RenderSession\s*\([^;]*$') { $rsState = 1 }
            if ($rsState -gt 0) {
                foreach ($ch in $codeLine.ToCharArray()) {
                    if ($ch -eq '{') { if ($rsState -eq 1) { $rsState = 2; $rsDepth = 1 } else { $rsDepth++ } }
                    elseif ($ch -eq '}' -and $rsState -eq 2) { $rsDepth--; if ($rsDepth -eq 0) { $rsState = 0; break } }
                }
            }
        }
        if ($coreRel) {
            foreach ($m in [regex]::Matches($codeLine, '\bSendMessage[AW]?\s*\(')) {
                $tail = $codeLine.Substring($m.Index + $m.Length)
                if ($i + 1 -lt $codeLines.Count) { $tail += ' ' + $codeLines[$i + 1] }
                if ($i + 2 -lt $codeLines.Count) { $tail += ' ' + $codeLines[$i + 2] }
                $pd = 1; $ok = $false
                for ($k = 0; $k -lt $tail.Length; $k++) {
                    $c = $tail[$k]
                    if ($c -eq '(') { $pd++ }
                    elseif ($c -eq ')') { $pd--; if ($pd -le 0) { break } }
                    elseif ($c -eq ',' -and $pd -eq 1) { $ok = $tail.Substring($k + 1) -cmatch '^\s*TTM_'; break }
                }
                if (-not $ok) {
                    Fail $where 'send-message' 'use SendMessageTimeoutW(..., SMTO_ABORTIFHUNG, ...) or PostMessageW (only TTM_* tooltip calls are exempt)'
                }
            }
            if ($coreRel -ne 'shell/shell_taskbar.h') {
                if ($codeLine -cmatch '\bSHAppBarMessage\s*\(') { Fail $where 'appbar-message' 'use AppBarMessage in shell/shell_taskbar.h' }
                if ($codeLine -cmatch '\bShell_NotifyIconGetRect\s*\(') { Fail $where 'notify-icon-rect' 'use ShellNotifyIconRect in shell/shell_taskbar.h' }
            }
            if ($from -eq 'shell' -and $codeLine -cmatch '\bShowWindow\s*\(') {
                Fail $where 'show-foreign-window' 'use ShowWindowAsync for windows owned by other processes'
            }
            if ($codeLine -cmatch '\bAttachThreadInput\s*\((?:[^()]|\([^()]*\))*,\s*(?:TRUE|true|1)\s*\)' -and
                ($codeLines[[Math]::Max(0, $i - 10)..$i] -join "`n") -cnotmatch '\bIsHungAppWindow\b') {
                Fail $where 'attach-thread-input' 'check IsHungAppWindow before AttachThreadInput(..., TRUE)'
            }
            if (($from -eq 'ui' -or $coreRel -eq 'main.cpp' -or ($coreRel -eq 'bar.cpp' -and $rsState -ne 2)) -and
                $codeLine -cmatch '\b(?:Msg)?WaitFor(?:Single|Multiple)Objects?(?:Ex)?\s*\([^;]*\bINFINITE\b') {
                Fail $where 'ui-infinite-wait' 'never wait with INFINITE on the UI thread'
            }
            if (($from -eq 'ui' -or $coreRel -in @('main.cpp', 'bar.cpp')) -and $codeLine -cmatch '\b\w*Async\([^;]*\)\.get\s*\(') {
                Fail $where 'ui-winrt-blocking-get' 'do not block the UI thread with .get() on WinRT async operations'
            }
            if ($codeLine -cmatch '\bstd::thread\s*\(\s*\[\s*[=&]\s*(?:,|\])') {
                Fail $where 'thread-default-capture' 'capture explicitly in std::thread lambdas; [=]/[&] captures this implicitly'
            }
        }
    }
    $key = $rel.Substring(7)
    foreach ($rule in $ratcheted.Keys) {
        if ($rule -eq 'detach' -and -not $coreRel) { continue }
        $targetLines = if ($rule -eq 'commented-code') { $lines } else { $codeLines }
        $n = @($targetLines | Where-Object { $_ -cmatch $ratcheted[$rule][0] }).Count
        if ($n) { $counts["$key|$rule"] = $n }
    }
    if ($lines.Count -gt $fileLines) { $counts["$key|file-lines"] = $lines.Count }
}
$cycleState = @{}
function Find-HeaderCycle([string]$u, [string[]]$stack) {
    $cycleState[$u] = 1
    foreach ($v in $headerEdges[$u]) {
        if ($cycleState[$v] -eq 1) {
            $idx = [Array]::IndexOf($stack, $v)
            $loop = $stack[$idx..($stack.Length - 1)]
            $desc = ($loop + @($v)) -join ' -> '
            foreach ($node in $loop) { Fail "native/core/$node" 'header-cycle' "include cycle: $desc" }
        } elseif (-not $cycleState.ContainsKey($v)) {
            Find-HeaderCycle $v ($stack + @($v))
        }
    }
    $cycleState[$u] = 2
}
foreach ($h in ($headerEdges.Keys | Sort-Object)) { if (-not $cycleState.ContainsKey($h)) { Find-HeaderCycle $h @($h) } }

# --- One version: the native project, the settings app and both manifests --------------------------------------
$versions = [ordered]@{
    'native/CMakeLists.txt'                        = 'project\(\w+ VERSION (\d+\.\d+\.\d+)'
    'settings/Atrium.Settings/Atrium.Settings.csproj' = '<Version>(\d+\.\d+\.\d+)</Version>'
    'native/core/core.manifest'                    = '<assemblyIdentity type="win32" name="Atrium" version="(\d+\.\d+\.\d+)\.0"'
    'settings/Atrium.Settings/app.manifest'        = '<assemblyIdentity version="(\d+\.\d+\.\d+)\.0"'
}
$found = foreach ($file in $versions.Keys) {
    $m = [regex]::Match([IO.File]::ReadAllText((Join-Path $Root $file)), $versions[$file])
    [pscustomobject]@{ File = $file; Version = if ($m.Success) { $m.Groups[1].Value } else { '?' } }
}
$expected = $found[0].Version
foreach ($f in $found | Where-Object Version -ne $expected) { Fail $f.File 'version' "$($f.Version), the project is $expected" }

# --- clang-format: the whole native tree ----------------------------------------------------------------------
if (-not $clangFormat) { $errors.Add("lint: clang-format $llvmMajor not found") }
elseif ($sources) {
    $rsp = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllLines($rsp, $sources)
        $res = Invoke-Tool $clangFormat @('--dry-run', '--Werror', "--style=file:$style", "--files=$rsp") -AllowNonZero
        $hits = 0
        foreach ($line in $res.Lines) {
            if ($line -match '^(.+?):(\d+):\d+: (?:warning|error): code should be clang-formatted') {
                $hits++
                Fail "$(Rel $Matches[1]):$($Matches[2])" 'format' 'run ./tools/lint.ps1 -Fix'
            }
        }
        if ($res.ExitCode -ne 0 -and -not $hits) {
            $errors.Add("tool-error: clang-format failed (exit $($res.ExitCode)): $(Tail $res.Lines)")
        }
    } finally {
        Remove-Item $rsp -ErrorAction SilentlyContinue
    }
}

# --- clang-tidy: .clang-tidy over a Ninja build of the native tree --------------------------------------------
$analysed = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
if ($Tidy) {
    . (Join-Path $PSScriptRoot 'lint-tidy.ps1')
    Invoke-TidyCheck -Root $Root -Rules $rules -BuildDir $BuildDir -Base $Base -FullRun $fullRun `
        -TouchedFiles $touchedFiles -Analysed $analysed -Errors $errors -Counts $counts -LlvmMajor $llvmMajor
}

# --- C#: the projects built with .editorconfig's style rules and the recommended analyzers as errors ----------
if ($DotNet) {
    foreach ($project in $projects) {
        $configs = if ($project -like '*Atrium.Settings.csproj') { @('', '-p:AtriumDevelopment=false') } else { @('') }
        foreach ($cfg in $configs) {
            $buildArgs = @('build', $project, '-c', 'Release', '-nologo') + $(if ($cfg) { @($cfg) } else { @() })
            $res = Invoke-Tool 'dotnet' $buildArgs -AllowNonZero
            $hits = 0
            $unparsedError = $false
            foreach ($line in $res.Lines) {
                if ($line -match '^\s*(.+?\.(?:cs|xaml))\((\d+),\d+\): (?:[\w]+\s+)?error (\w+): (.*?)( \[.*)?$') {
                    $hits++
                    Fail "$(Rel $Matches[1]):$($Matches[2])" $Matches[3] $Matches[4]
                } elseif ($line -match ': error | error [A-Z]+\d*:') {
                    $unparsedError = $true
                }
            }
            if ($res.ExitCode -ne 0 -and (-not $hits -or $unparsedError)) {
                $errors.Add("tool-error: dotnet build $(Rel $project) failed (exit $($res.ExitCode)): $(Tail $res.Lines)")
            }
        }
    }
}

# --- Ratchets ---------------------------------------------------------------------------------------------------
function Match-BaselineMoves([System.Collections.IDictionary]$oldMap, [System.Collections.IDictionary]$newMap) {
    $shrank = @{}
    $isShrank = {
        param([string]$keyFile)
        if ($shrank.Contains($keyFile)) { return $shrank[$keyFile] }
        $repoRel = BaselineFile $keyFile
        $full = Join-Path $Root $repoRel
        if (-not (Test-Path $full)) { return ($shrank[$keyFile] = $true) }
        $cur = [IO.File]::ReadAllLines($full).Count
        $rev = if ($Base) { $mergeBase } else { 'HEAD' }
        $res = Invoke-Tool 'git' @('-C', $Root, 'show', "${rev}:$repoRel") -AllowNonZero
        # A file added and split in the same change has no prior version here; it cannot count as shrunk.
        $old = if ($res.ExitCode -eq 0) { $res.Lines.Count }
            elseif ($oldMap.Contains("$keyFile|file-lines")) { [int]$oldMap["$keyFile|file-lines"] } else { 0 }
        $shrank[$keyFile] = ($cur -lt $old)
        $shrank[$keyFile]
    }
    $added = @($newMap.Keys | Where-Object { -not $oldMap.Contains($_) } | Sort-Object { -[int]$newMap[$_] }, { $_ })
    $removed = [System.Collections.Generic.List[string]]::new()
    foreach ($k in ($oldMap.Keys | Sort-Object)) { if (-not $newMap.Contains($k)) { $removed.Add($k) } }
    $matched = @{}
    foreach ($samePass in @($true, $false)) {
        foreach ($k in $added) {
            if ($matched.Contains($k)) { continue }
            $p = $k.Split('|', 3)
            if ($p.Count -lt 3) { continue }
            $best = -1
            for ($i = 0; $i -lt $removed.Count; $i++) {
                $rp = $removed[$i].Split('|', 3)
                if ($rp.Count -lt 3 -or $rp[1] -ne $p[1] -or [int]$newMap[$k] -gt [int]$oldMap[$removed[$i]]) { continue }
                $same = $rp[0] -eq $p[0]
                if ($same -ne $samePass) { continue }
                if (-not $same -and ($rp[2] -ne $p[2] -or $p[2] -in @('(lambda)', 'operator()') -or
                    $p[2] -match '(?:^|::)\(lambda#\d+\)$' -or -not (& $isShrank $rp[0]))) { continue }
                if ($best -lt 0 -or [int]$oldMap[$removed[$i]] -lt [int]$oldMap[$removed[$best]]) { $best = $i }
            }
            if ($best -ge 0) { $matched[$k] = $removed[$best]; $removed.RemoveAt($best) }
        }
    }
    $matched
}
if ($Base) {
    $tree = Invoke-Tool 'git' @('-C', $Root, 'ls-tree', $mergeBase, '--', 'tools/lint-baseline.json') -AllowNonZero
    $gitOk = $tree.ExitCode -eq 0
    $baseBaseline = @{}
    if (-not $gitOk) {
        $errors.Add("tool-error: git ls-tree $mergeBase failed (exit $($tree.ExitCode)): $(Tail $tree.Lines)")
    } elseif ($tree.Lines | Where-Object { $_ }) {
        $res = Invoke-Tool 'git' @('-C', $Root, 'show', "${mergeBase}:tools/lint-baseline.json") -AllowNonZero
        if ($res.ExitCode -ne 0) {
            $gitOk = $false
            $errors.Add("tool-error: git show ${mergeBase}:tools/lint-baseline.json failed (exit $($res.ExitCode)): $(Tail $res.Lines)")
        } elseif ($res.Lines) {
            $baseBaseline = Read-Baseline (($res.Lines -join "`n") | ConvertFrom-Json -AsHashtable)
        }
    }
    if ($gitOk) {
        $baseTags = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($k in $baseBaseline.Keys) { [void]$baseTags.Add((Get-KeyMetricTag $k)) }
        $lintChanged = [bool]($touchedFiles | Where-Object { Test-Glob $_ @('tools/lint.ps1', 'tools/lint-*.ps1') } | Select-Object -First 1)
        foreach ($k in $baseline.Keys) {
            if ($baseBaseline.Contains($k) -and [int]$baseline[$k] -gt [int]$baseBaseline[$k]) {
                Fail 'tools/lint-baseline.json' 'baseline' "$k rose ($($baseBaseline[$k]) -> $($baseline[$k]))"
            }
        }
        $baseMoves = Match-BaselineMoves $baseBaseline $baseline
        foreach ($k in $baseline.Keys) {
            if (-not $baseBaseline.Contains($k) -and -not $baseMoves.Contains($k)) {
                if ($lintChanged -and -not $baseTags.Contains((Get-KeyMetricTag $k))) { continue }
                Fail 'tools/lint-baseline.json' 'baseline' "$k added without a matching removed key"
            }
        }
    }
}
$updateMoves = if ($UpdateBaseline) { Match-BaselineMoves $baseline $counts } else { @{} }
$advice = @{ 'file-lines' = "split it by what it does (at most $fileLines lines)"; 'complexity' = 'simplify it (cognitive complexity 40 at most)'
    'function-lines' = 'split it (120 lines at most)' }
foreach ($key in $counts.Keys) {
    if ($UpdateBaseline -and ($updateMoves.Contains($key) -or ($SeedMetric -contains (Get-KeyMetricTag $key)))) { continue }
    $limit = if ($baseline.Contains($key)) { [int]$baseline[$key] } else { 0 }
    if ([int]$counts[$key] -gt $limit) {
        $parts = $key.Split('|')
        $why = if ($ratcheted.Contains($parts[1])) { $ratcheted[$parts[1]][1] } else { $advice[$parts[1]] }
        Fail (BaselineFile $parts[0]) "$($parts[1..($parts.Count - 1)] -join ' ')" "$($counts[$key]) > $limit; $why"
    }
}
if (-not $UpdateBaseline -and (-not $Paths -or (Test-Glob 'tools/lint-baseline.json' $Paths))) {
    foreach ($key in $baseline.Keys) {
        $tidyKey = $key -match '\|(complexity|function-lines)\|'
        if ($tidyKey -and -not $Tidy) { continue }
        if ($Base -and $tidyKey -and -not $fullRun) {
            $keyFile = $key.Split('|')[0]
            if ((Test-Path (Join-Path $Root (BaselineFile $keyFile))) -and -not $analysed.Contains($keyFile)) { continue }
        }
        if (-not $counts.Contains($key) -or [int]$counts[$key] -lt [int]$baseline[$key]) {
            $errors.Add("lint: $key is below its baseline; run ./tools/lint.ps1 -Tidy -UpdateBaseline")
        }
    }
}

# --- The change itself --------------------------------------------------------------------------------------------
if ($Base) {
    foreach ($line in $diffStatus) {
        $parts = $line -split "`t"
        $status = $parts[0]
        $checked = if ($status -like 'R*') { @($parts[1], $parts[2]) } elseif ($status -like 'C*') { @($parts[2]) } else { @($parts[1]) }
        foreach ($path in $checked) {
            if ($Paths -and -not (Test-Glob $path $Paths)) { Fail $path 'scope' "outside the allowed paths ($($Paths -join ', '))" }
        }
    }
    $numbers = $diffNumstat | ForEach-Object { ($_ -split "`t")[0..1] -replace '-', '0' }
    $total = ($numbers | ForEach-Object { [int]$_ } | Measure-Object -Sum).Sum
    if ($MaxLines -and $total -gt $MaxLines) { $errors.Add("diff: $total changed lines > budget $MaxLines") }
}

$problems = @($errors | Sort-Object -Unique)
# A scoped change answers for its own files (and its diff), never silencing tree-wide or tool errors.
if ($Paths) {
    $problems = @($problems | Where-Object {
        $file = if ($_ -match '^([^:]+?)(:\d+)?: ') { $Matches[1] } else { '' }
        -not $file -or $_ -match '^(lint|diff|tool-error): ' -or
            $_ -match ': (?:scope|version|baseline|header-cycle|clang-diagnostic-error|(?:CS|MC|WMC|RZ)\d+): ' -or (Test-Glob $file $Paths)
    })
}
$problems | ForEach-Object { Write-Output $_ }
if ($problems.Count) { Write-Output "lint: $($problems.Count) problem(s)"; exit 1 }
if ($UpdateBaseline) {
    $next = [ordered]@{}
    foreach ($key in $counts.Keys | Sort-Object) {
        if ($baseline.Contains($key)) { $next[$key] = [Math]::Min([int]$baseline[$key], [int]$counts[$key]) }
        elseif ($updateMoves.Contains($key) -or ($SeedMetric -contains (Get-KeyMetricTag $key))) { $next[$key] = [int]$counts[$key] }
    }
    [IO.File]::WriteAllText($baselinePath, ($next | ConvertTo-Json).Replace("`r`n", "`n") + "`n")
    "baseline: $($next.Count) entries"
    exit 0
}
Write-Output 'lint: clean'
