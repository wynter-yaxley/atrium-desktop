$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$linter = Join-Path $project 'tools/lint.ps1'
$pwsh = (Get-Process -Id $PID).Path
$temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $temporary ('Atrium-lint-tests-' + [guid]::NewGuid().ToString('N'))
$expectedRoot = [IO.Path]::GetFullPath($fixtureRoot)
$null = New-Item -ItemType Directory -Path $fixtureRoot
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$utf8Bom = [Text.UTF8Encoding]::new($true)
$script:caseCount = 0

function Write-RepoFile([string]$root, [string]$rel, [string]$text, [bool]$crlf = $false, [bool]$bom = $false) {
    $full = Join-Path $root $rel
    $parent = Split-Path -Parent $full
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
    $norm = ($text -replace "`r`n", "`n").TrimEnd("`n") + "`n"
    if ($crlf) { $norm = $norm -replace "`n", "`r`n" }
    [IO.File]::WriteAllText($full, $norm, $(if ($bom) { $utf8Bom } else { $utf8NoBom }))
}

function Invoke-Git([string]$dir, [string[]]$gitArgs) {
    $out = & git -C $dir @gitArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($gitArgs -join ' ') failed in ${dir}:`n$($out -join "`n")" }
}

function New-TemplateRepo([string]$dir) {
    $null = New-Item -ItemType Directory -Path $dir
    Invoke-Git $dir @('init', '-q')
    Invoke-Git $dir @('config', 'user.name', 'LintTest')
    Invoke-Git $dir @('config', 'user.email', 'lint@test.local')
    Invoke-Git $dir @('config', 'core.autocrlf', 'false')
    Copy-Item -LiteralPath (Join-Path $project '.clang-format') -Destination (Join-Path $dir '.clang-format')
    Copy-Item -LiteralPath (Join-Path $project '.clang-tidy') -Destination (Join-Path $dir '.clang-tidy')
    Write-RepoFile $dir '.gitignore' '/native/build/'
    Write-RepoFile $dir 'native/CMakeLists.txt' "cmake_minimum_required(VERSION 3.20)`nproject(Atrium VERSION 2.7.0 LANGUAGES CXX)"
    Write-RepoFile $dir 'native/core/core.manifest' '<assemblyIdentity type="win32" name="Atrium" version="2.7.0.0" />'
    Write-RepoFile $dir 'settings/Atrium.Settings/Atrium.Settings.csproj' '<Project><PropertyGroup><Version>2.7.0</Version></PropertyGroup></Project>'
    Write-RepoFile $dir 'settings/Atrium.Settings/app.manifest' '<assemblyIdentity version="2.7.0.0" />'
    Write-RepoFile $dir 'tools/Uninstall.ps1' '# ok' $true $true
    Write-RepoFile $dir 'tools/Update.ps1' '# ok' $true $true
    Write-RepoFile $dir 'tools/lint-baseline.json' '{}'
    Invoke-Git $dir @('add', '-A'); Invoke-Git $dir @('commit', '-q', '-m', 'init')
    $dir
}

$templateDir = New-TemplateRepo (Join-Path $fixtureRoot '_template')
function New-FixtureRepo([string]$name) {
    $dir = Join-Path $fixtureRoot $name
    Copy-Item -LiteralPath $templateDir -Destination $dir -Recurse
    $dir
}

function Invoke-LintCase([string]$dir, [string[]]$lintArgs, [int]$expectedExit, [string[]]$expectPatterns = @()) {
    $script:caseCount++
    $resolved = [IO.Path]::GetFullPath($dir)
    if (-not $resolved.StartsWith($expectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to run linter outside fixture root: $resolved"
    }
    $out = (& $pwsh -NoProfile -File $linter -Root $dir @lintArgs 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne $expectedExit) {
        throw "Case #$($script:caseCount) ($dir) expected exit $expectedExit, got ${LASTEXITCODE}:`n$out"
    }
    foreach ($pat in $expectPatterns) {
        if ($pat -and $out -notmatch $pat) {
            throw "Case #$($script:caseCount) ($dir) missing pattern '$pat':`n$out"
        }
    }
}

function New-NestedBranchBody([string]$varName, [int]$depth, [int]$baseIndent) {
    $sb = [System.Text.StringBuilder]::new(); $b = ' ' * $baseIndent
    [void]$sb.AppendLine("${b}int acc = 0;")
    for ($d = 0; $d -lt $depth; $d++) { [void]$sb.AppendLine((' ' * ($baseIndent + 4 * $d)) + "if ($varName > $d) {") }
    [void]$sb.AppendLine((' ' * ($baseIndent + 4 * $depth)) + "acc += $varName;")
    for ($d = $depth - 1; $d -ge 0; $d--) { [void]$sb.AppendLine((' ' * ($baseIndent + 4 * $d)) + '}') }
    [void]$sb.Append("${b}return acc;")
    $sb.ToString()
}

try {
    # 1. Clean baseline
    Invoke-LintCase (New-FixtureRepo 'clean') @() 0 @('lint: clean')

    # 2. Text & BOM rules
    $r = New-FixtureRepo 'cpp-bom'; Write-RepoFile $r 'native/core/a.cpp' 'int kX = 1;' $false $true
    Invoke-LintCase $r @() 1 @(': bom:')
    $r = New-FixtureRepo 'uninstall-nobom'; Write-RepoFile $r 'tools/Uninstall.ps1' '# ok' $true $false
    Invoke-LintCase $r @() 1 @('Uninstall\.ps1: bom:')
    $r = New-FixtureRepo 'update-nobom'; Write-RepoFile $r 'tools/Update.ps1' '# ok' $true $false
    Invoke-LintCase $r @() 1 @('Update\.ps1: bom:')
    $r = New-FixtureRepo 'cpp-blank'; [IO.File]::WriteAllText((Join-Path $r 'native/core/a.cpp'), "int kX = 1;`n`n`n", $utf8NoBom)
    Invoke-LintCase $r @() 1 @(': blank-lines:')
    $r = New-FixtureRepo 'cpp-crlf'; Write-RepoFile $r 'native/core/a.cpp' 'int kX = 1;' $true $false
    Invoke-LintCase $r @() 1 @(': line-ending:')

    # 3. XAML rules
    $r = New-FixtureRepo 'xaml-ok'
    Write-RepoFile $r 'settings/Atrium.Settings/Main.xaml' "<Page>`n    <!--`n        <Grid>`n    -->`n    <Grid /> <!-- note -->`n</Page>"
    Invoke-LintCase $r @() 0 @('lint: clean')
    $r = New-FixtureRepo 'xaml-bad'; Write-RepoFile $r 'settings/Atrium.Settings/Main.xaml' "<Page>`n  <Grid />`n</Page>"
    Invoke-LintCase $r @() 1 @(': xaml-indent:')

    # 4. Comment/string stripping, banned rules & #define
    $r = New-FixtureRepo 'strip-pass'
    Write-RepoFile $r 'native/core/a.cpp' ("struct S {`n    S(const S&) = delete;`n};`n// delete ptr; new Foo;`n" +
        "// colour = newest), or nullopt.`n// NOLINTEND`nconst int kBig = 1'000'000;`n" +
        "const wchar_t* kWide = L`"delete item`";`nconst char* kRaw = R`"(new int(1);)`";")
    Invoke-LintCase $r @() 0 @('lint: clean')

    $nativeCases = @(
        @('native/core/a.cpp', 'void F() { auto* p = new int(1); }', ': raw-new:'),
        @('native/core/a.cpp', 'void F() { Foo(new int(1)); }', ': raw-new:'),
        @('native/core/a.cpp', 'void F(int* p) { delete(p); }', ': raw-delete:'),
        @('native/core/a.cpp', '#pragma comment(lib, "x.lib")', ': pragma-lib:'),
        @('native/core/a.cpp', '// return false;', ': commented-code:'),
        @('native/core/a.cpp', '// NOLINT', ': nolint:'),
        @('native/core/a.cpp', '// NOLINT(*): skip', ': nolint:'),
        @('native/core/a.cpp', '// NOLINT(readability-function-cognitive-complexity): too deep', ': nolint:'),
        @('native/core/a.cpp', '// NOLINTNEXTLINE(readability-function-size): too long', ': nolint:'),
        @('native/core/a.cpp', "#ifndef ATRIUM_DEVELOPMENT`n#define FOO 1`n#endif", ': define:'),
        @('native/core/a.cpp', "#ifndef ATRIUM_DEVELOPMENT`n#define BAD(x) (x)`n#endif", ': define:'),
        @('native/core/a.cpp', 'void F(HWND h) { SendMessageW(h, WM_USER, 0, 0); }', ': send-message:'),
        @('native/core/a.cpp', 'void F() { SHAppBarMessage(0, nullptr); }', ': appbar-message:'),
        @('native/core/a.cpp', 'void F() { Shell_NotifyIconGetRect(nullptr, nullptr); }', ': notify-icon-rect:'),
        @('native/core/shell/foo.cpp', 'void F(HWND w) { ShowWindow(w, 0); }', ': show-foreign-window:'),
        @('native/core/a.cpp', 'void F(DWORD a, DWORD b) { AttachThreadInput(a, b, TRUE); }', ': attach-thread-input:'),
        @('native/core/ui/foo.cpp', 'void F(HANDLE h) { WaitForSingleObject(h, INFINITE); }', ': ui-infinite-wait:'),
        @('native/core/ui/foo.cpp', 'void F() { FooAsync(1).get(); }', ': ui-winrt-blocking-get:'),
        @('native/core/ui/foo.cpp', 'void F() { std::thread([=] {}); }', ': thread-default-capture:'),
        @('native/core/a.cpp', 'void F(std::thread& t) { t.detach(); }', ': detach:'),
        @('native/core/a.cpp', "void F()`n{`n    try {`n        Foo();`n    } catch (const std::exception&) {`n    }`n}", ': try:'),
        @('native/core/a.cpp', "void F()`n{`n    try {`n        Foo();`n    } catch (...) {`n    }`n}", ': try:'),
        @('native/core/a.cpp', ("void F()`n{`n    try {`n        Foo();`n    } catch (const winrt::hresult_error&) {`n" +
            "    } catch (const std::exception&) {`n    }`n}"), ': try:'),
        @('native/core/a.cpp', ("void F()`n{`n    try {`n        Foo();`n    } catch (const std::exception&) {`n" +
            "    } catch (const winrt::hresult_error&) {`n    }`n}"), ': try:'),
        @('native/core/a.cpp', ("void F()`n{`n    try {`n        Foo();`n    } catch (const winrt::hresult_error&) {`n" +
            "    } catch (...) {`n    }`n}"), ': try:'),
        @('native/core/a.cpp', "void F()`n{`n    try {`n        Foo();`n    } catch (const winrt::hresult_error_code&) {`n    }`n}", ': try:'),
        @('native/core/a.cpp', "void F()`n{`n    try {`n        Foo();`n    }`n}", ': try:'),
        @('native/core/a.cpp', ("void F()`n{`n    try {`n        try {`n            Foo();`n        } catch (const std::exception&) {`n" +
            "        }`n    } catch (const winrt::hresult_error&) {`n    }`n}"), ': try:')
    )
    $r = New-FixtureRepo 'native-bans'
    foreach ($tc in $nativeCases) {
        Write-RepoFile $r $tc[0] $tc[1]
        Invoke-LintCase $r @() 1 @($tc[2])
        Remove-Item -LiteralPath (Join-Path $r $tc[0]) -Force
    }

    $r = New-FixtureRepo 'safety-pass'
    Write-RepoFile $r 'native/core/a.cpp' ("void F(HWND tip, HWND other, DWORD me, DWORD them)`n{`n" +
        "    SendMessageW(tip, TTM_GETDELAYTIME, 0, 0);`n    if (!IsHungAppWindow(other)) {`n" +
        "        AttachThreadInput(me, them, TRUE);`n    }`n}`n" +
        "void WinRtTries(bool flag)`n{`n" +
        "    try {`n        if (flag) {`n            Foo();`n        }`n" +
        "    } catch (const winrt::hresult_error& e) {`n        Log(e);`n    }`n" +
        "    try {`n        Foo();`n    } catch (winrt::hresult_error) {`n    }`n" +
        "    try {`n        Foo();`n    } catch (const hresult_error& e) {`n        Log(e);`n    }`n" +
        "    try {`n        Foo();`n    } catch (const winrt::hresult_error* p) {`n        Log(p);`n" +
        "    } catch (const winrt::hresult_error& e) {`n        Log(e);`n    }`n" +
        "    try {`n        try {`n            Foo();`n        } catch (const winrt::hresult_error&) {`n        }`n" +
        "    } catch (const winrt::hresult_error&) {`n    }`n}")
    Write-RepoFile $r 'native/core/shell/shell_taskbar.h' ("// Taskbar.`n#pragma once`ninline void Tb()`n{`n" +
        "    SHAppBarMessage(0, nullptr);`n    Shell_NotifyIconGetRect(nullptr, nullptr);`n}")
    Write-RepoFile $r 'native/core/settings.cpp' "#include <winrt/base.h>`nint kSettingsOk = 1;"
    Invoke-LintCase $r @() 0 @('lint: clean')

    $r = New-FixtureRepo 'try-mixed-count'
    Write-RepoFile $r 'native/core/a.cpp' ("void F()`n{`n" +
        "    try {`n        Foo();`n    } catch (const winrt::hresult_error&) {`n    }`n" +
        "    try {`n        Bar();`n    } catch (const std::exception&) {`n    }`n}")
    Write-RepoFile $r 'tools/lint-baseline.json' '{"core/a.cpp|try":1}'
    Invoke-LintCase $r @() 0 @('lint: clean')

    # 5. Layers, header-cycle, csharp-layer
    $hdr = "// H.`n#pragma once"
    $structCases = @(
        @('layer-render-ui', @{ 'native/core/ui/foo.h' = $hdr; 'native/core/render/bar.cpp' = '#include <ui/foo.h>' }, ': layer:'),
        @('layer-found-bar', @{ 'native/core/bar.h' = $hdr; 'native/core/settings.h' = "$hdr`n#include `"bar.h`"" }, ': layer:'),
        @('layer-found-cpp-shell', @{ 'native/core/shell/s.h' = $hdr; 'native/core/settings.cpp' = '#include "shell/s.h"' }, ': layer:'),
        @('layer-found-winrt', @{ 'native/core/settings.h' = "$hdr`n#include <winrt/base.h>" }, ': layer:'),
        @('header-cycle', @{ 'native/core/ui/a.h' = "$hdr`n#include `"b.h`""; 'native/core/ui/b.h' = "$hdr`n#include `"a.h`"" }, ': header-cycle:'),
        @('csharp-layer', @{ 'settings/Atrium.Settings/BarSettings.cs' = "using Microsoft.UI.Xaml;`npublic class BarSettings {}" }, ': csharp-layer:'),
        @('csharp-layer-gamefinder-parsers',
            @{ 'settings/Atrium.Settings/GameFinder.Parsers.cs' = "using Microsoft.UI.Xaml;`npublic static class GameFinderParsers {}" },
            ': csharp-layer:'),
        @('csharp-layer-release-validation',
            @{ 'settings/Atrium.Settings/ReleaseValidation.cs' = "using Microsoft.UI.Xaml;`npublic static class ReleaseValidation {}" },
            ': csharp-layer:')
    )
    foreach ($sc in $structCases) {
        $r = New-FixtureRepo $sc[0]
        foreach ($kv in $sc[1].GetEnumerator()) { Write-RepoFile $r $kv.Key $kv.Value }
        Invoke-LintCase $r @() 1 @($sc[2])
    }

    # 6. Baseline monotonicity & -UpdateBaseline / -SeedMetric guards
    $r = New-FixtureRepo 'base-rose'
    Write-RepoFile $r 'native/core/a.cpp' 'void F(std::thread& t) { t.detach(); }'
    Write-RepoFile $r 'tools/lint-baseline.json' '{"core/a.cpp|detach":1}'
    Invoke-Git $r @('add', '-A'); Invoke-Git $r @('commit', '-q', '-m', 'base')
    Write-RepoFile $r 'tools/lint-baseline.json' '{"core/a.cpp|detach":2}'
    Invoke-LintCase $r @('-Base', 'HEAD') 1 @(': baseline:.*rose')

    $r = New-FixtureRepo 'base-added'
    Write-RepoFile $r 'tools/lint-baseline.json' '{"core/a.cpp|detach":1}'
    Invoke-LintCase $r @('-Base', 'HEAD') 1 @(': baseline:.*added without a matching')

    $r = New-FixtureRepo 'base-nonint'
    Write-RepoFile $r 'tools/lint-baseline.json' '{"core/a.cpp|detach":"10"}'
    Invoke-LintCase $r @() 1 @(': baseline:.*positive integer')

    $r = New-FixtureRepo 'base-rename'
    Write-RepoFile $r 'tools/lint-baseline.json' '{"core/a.cpp|complexity|OldFn()":45}'
    Invoke-Git $r @('add', '-A'); Invoke-Git $r @('commit', '-q', '-m', 'base')
    Write-RepoFile $r 'tools/lint-baseline.json' '{"core/a.cpp|complexity|NewFn()":42}'
    Invoke-LintCase $r @('-Base', 'HEAD') 0 @('lint: clean')

    $r = New-FixtureRepo 'upd-guards'
    Write-RepoFile $r 'tools/lint-baseline.json' '{"core/a.cpp|detach":1}'
    Invoke-Git $r @('add', '-A'); Invoke-Git $r @('commit', '-q', '-m', 'base')
    $blPath = Join-Path $r 'tools/lint-baseline.json'; $before = [IO.File]::ReadAllBytes($blPath)
    Invoke-LintCase $r @('-UpdateBaseline', '-Tidy', '-Base', 'HEAD') 1 @('-UpdateBaseline requires -Tidy and cannot be used with -Base')
    Invoke-LintCase $r @('-UpdateBaseline') 1 @('-UpdateBaseline requires -Tidy')
    Invoke-LintCase $r @('-UpdateBaseline', '-Tidy', '-SeedMetric', 'detach') 1 @('already has entries in tools/lint-baseline\.json')
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($blPath)) -ne [Convert]::ToBase64String($before)) {
        throw 'Baseline file was modified by rejected -UpdateBaseline/-SeedMetric call'
    }

    # 7. -Base & -Paths integrity
    Invoke-LintCase (New-FixtureRepo 'bad-base') @('-Base', 'deadbeef') 1 @('deadbeef|failed|unknown')

    $r = New-FixtureRepo 'paths-glob'
    Write-RepoFile $r 'native/core/ui/sub.h' $hdr
    Invoke-LintCase $r @('-Base', 'HEAD', '-Paths', 'native/core/*.h') 1 @('native/core/ui/sub\.h: scope:')

    $r = New-FixtureRepo 'paths-del'
    Write-RepoFile $r 'native/core/tracked.cpp' 'int kT = 1;'
    Invoke-Git $r @('add', '-A'); Invoke-Git $r @('commit', '-q', '-m', 'tracked')
    Remove-Item -LiteralPath (Join-Path $r 'native/core/tracked.cpp')
    Write-RepoFile $r 'native/core/a.h' $hdr
    Invoke-LintCase $r @('-Base', 'HEAD', '-Paths', 'native/core/*.h') 1 @('native/core/tracked\.cpp: scope:')

    $r = New-FixtureRepo 'ver-outside-paths'
    Write-RepoFile $r 'settings/Atrium.Settings/Atrium.Settings.csproj' '<Project><PropertyGroup><Version>2.8.0</Version></PropertyGroup></Project>'
    Invoke-Git $r @('add', '-A'); Invoke-Git $r @('commit', '-q', '-m', 'ver')
    Write-RepoFile $r 'native/core/a.h' $hdr
    Invoke-LintCase $r @('-Base', 'HEAD', '-Paths', 'native/core/*.h') 1 @('Atrium\.Settings\.csproj: version:')

    # 8. End-to-end mini -Tidy (deps + overload + anonymous lambdas)
    $r = New-FixtureRepo 'mini-tidy'
    Write-RepoFile $r 'native/CMakeLists.txt' ("cmake_minimum_required(VERSION 3.20)`nproject(Atrium VERSION 2.7.0 LANGUAGES CXX)`n" +
        "set(CMAKE_CXX_STANDARD 20)`nadd_library(core STATIC core/unit.cpp)`ntarget_include_directories(core PRIVATE core)")
    Write-RepoFile $r 'native/core/leaf.h' "$hdr`ninline int LeafVal() { return 1; }"
    Write-RepoFile $r 'native/core/mid.h' "$hdr`n#include `"leaf.h`"`ninline int MidVal() { return LeafVal(); }"
    Write-RepoFile $r 'native/core/unit.cpp' ("#include `"mid.h`"`nnamespace {`nint Compute() { return MidVal(); }`n} // namespace`n" +
        "int wmain() { return Compute(); }")
    Invoke-Git $r @('add', '-A'); Invoke-Git $r @('commit', '-q', '-m', 'tidy-base')
    Invoke-LintCase $r @('-Tidy') 0 @('lint: clean')
    Invoke-LintCase $r @('-Tidy', '-BuildDir', 'native/build/tidy') 0 @('lint: clean')
    Invoke-LintCase $r @('-Tidy', '-BuildDir', 'nonexistent') 1 @('no compile_commands\.json')

    Write-RepoFile $r 'native/core/unit.cpp' ("#include `"mid.h`"`nnamespace {`nint Compute()`n{`n" +
        "    int v = MidVal();`n    char* q = (char*)&v;`n    return q != nullptr ? v : 0;`n}`n} // namespace`n" +
        "int wmain() { return Compute(); }")
    Invoke-Git $r @('add', '-A'); Invoke-Git $r @('commit', '-q', '-m', 'unit-warn')
    Write-RepoFile $r 'native/core/leaf.h' "$hdr`ninline int LeafVal() { return 2; }"
    Invoke-LintCase $r @('-Tidy', '-Base', 'HEAD') 1 @('unit\.cpp:\d+: cppcoreguidelines-pro-type-cstyle-cast:')

    $bA = New-NestedBranchBody 'val' 9 4; $bB = New-NestedBranchBody 'static_cast<int>(val)' 9 4; $bL = New-NestedBranchBody 'item' 9 12
    Write-RepoFile $r 'native/core/unit.cpp' ("#include `"mid.h`"`nnamespace {`nint Dup(int val)`n{`n$bA`n}`n" +
        "int Dup(double val)`n{`n$bB`n}`ntemplate<typename Fn> int Call(Fn fn) { return fn(1); }`n" +
        "int Host(int seed)`n{`n    int first = Call([](int item) {`n$bL`n    });`n    int second = Call([](int item) {`n$bL`n    });`n" +
        "    return first + second + seed;`n}`n} // namespace`nint wmain() { return MidVal() + Dup(1) + Dup(1.0) + Host(2); }")
    $tidyPats = @('complexity Dup\(int\)', 'complexity Dup\(double\)', 'complexity Host::\(lambda#1\)', 'complexity Host::\(lambda#2\)')
    Invoke-LintCase $r @('-Tidy') 1 $tidyPats
    Write-Host "Lint regressions passed ($($script:caseCount) cases)."
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if ($resolved -ne $expectedRoot -or -not $resolved.StartsWith($temporary, [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFileName($resolved).StartsWith('Atrium-lint-tests-')) { throw 'Unexpected test cleanup path' }
    if (Test-Path -LiteralPath $resolved) {
        Get-ChildItem -LiteralPath $resolved -Recurse -Force -File |
            Where-Object { $_.Attributes -band [IO.FileAttributes]::ReadOnly } |
            ForEach-Object { $_.Attributes = [IO.FileAttributes]::Normal }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
exit 0
