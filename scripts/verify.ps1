# Youtube-Lyrics - one-command local verification on Windows.
#
#   powershell -ExecutionPolicy Bypass -File scripts\verify.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -SkipBuild
#
# Does, in order, and writes one consolidated report:
#   1. checks the build dependencies (Zig, Python 3, Pillow)
#   2. builds dist\YoutubeLyrics.exe            (scripts\build.ps1)
#   3. runs the portable core tests             (tests\core_test.cc)
#   4. runs the release, build and settings-layout tests and the docs checks
#   5. runs the program's own self-test         (--self-test)
#   6. runs the desktop attachment test         (--desktop-test)
#
# -SkipBuild verifies the dist\YoutubeLyrics.exe that is already there (for
# example the one committed on the branch); steps 1-3 are then reported as
# SKIP, never as PASS.
#
# Exit code: 0 every step passed; 1 a step failed; 2 nothing failed but at
# least one step could not be carried out. Only 0 counts for a release.
#
# Step 6 puts a test card on the real desktop for a few seconds. Minimise your
# windows first (Win+D), or its drawing checks will be reported as SKIP.
#
# What this script cannot do: watch real music playing, click desktop icons,
# restart Explorer, or judge how anything looks. Those are the manual matrix in
# engineering\WINDOWS_TEST_MATRIX.md, and a release needs both.
param(
    [switch]$SkipBuild,
    [switch]$SkipDesktop,
    [string]$ReportDir = ''
)

$ErrorActionPreference = 'Continue'
Set-Location (Join-Path $PSScriptRoot '..')
$root = (Get-Location).Path
if (-not $ReportDir) { $ReportDir = Join-Path $root '.build\verify' }
New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null

$zig = if ($env:ZIG) { $env:ZIG } else { 'zig' }
$python = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
$exe = Join-Path $root 'dist\YoutubeLyrics.exe'

$results = New-Object System.Collections.Generic.List[object]
function Add-Result([string]$name, [string]$status, [string]$detail) {
    $results.Add([pscustomobject]@{ Step = $name; Status = $status; Detail = $detail })
    $colour = switch ($status) { 'PASS' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Write-Host ("{0,-5} {1}: {2}" -f $status, $name, $detail) -ForegroundColor $colour
}

function Test-Tool([string]$command, [string[]]$arguments) {
    try {
        $null = & $command @arguments 2>&1
        return ($LASTEXITCODE -eq 0)
    } catch { return $false }
}

# Runs a command, keeps its output in a log file, returns the exit code.
function Invoke-Logged([string]$log, [scriptblock]$action) {
    $path = Join-Path $ReportDir $log
    & $action *> $path
    return $LASTEXITCODE
}

# Runs the GUI executable with a switch and waits for it (it has no console).
function Invoke-App([string[]]$arguments, [int]$timeoutSeconds) {
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        try { $process.Kill() } catch { }
        return $null
    }
    return $process.ExitCode
}

# ---------------------------------------------------------------- 1. tools
$haveZig = Test-Tool $zig @('version')
$havePython = Test-Tool $python @('--version')
$havePillow = $havePython -and (Test-Tool $python @('-c', 'import PIL'))
$haveBash = Test-Tool 'bash' @('--version')

if ($SkipBuild) {
    Add-Result 'dependencies' 'SKIP' '-SkipBuild: the build tools were not needed'
} elseif ($haveZig -and $havePython -and $havePillow) {
    Add-Result 'dependencies' 'PASS' "Zig $(& $zig version), $(& $python --version 2>&1), Pillow present"
} else {
    $missing = @()
    if (-not $haveZig) { $missing += 'Zig (https://ziglang.org/download, or set $env:ZIG)' }
    if (-not $havePython) { $missing += 'Python 3 (or set $env:PYTHON)' }
    if ($havePython -and -not $havePillow) { $missing += "Pillow ('pip install Pillow')" }
    Add-Result 'dependencies' 'FAIL' ('missing: ' + ($missing -join '; '))
}

# ---------------------------------------------------------------- 2. build
if ($SkipBuild) {
    Add-Result 'build' 'SKIP' '-SkipBuild: using the existing dist\YoutubeLyrics.exe'
} elseif (-not ($haveZig -and $havePython -and $havePillow)) {
    Add-Result 'build' 'FAIL' 'not attempted: build dependencies are missing'
} else {
    $code = Invoke-Logged 'build.log' { powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 }
    if ($code -eq 0) { Add-Result 'build' 'PASS' 'scripts\build.ps1 (see build.log)' }
    else { Add-Result 'build' 'FAIL' "scripts\build.ps1 exited $code (see build.log)" }
}

$haveExe = Test-Path $exe
$exeHash = ''
if ($haveExe) { $exeHash = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant() }

# ----------------------------------------------------------- 3. core tests
if (-not $haveZig) {
    Add-Result 'core-tests' 'SKIP' 'Zig is not available to compile tests\core_test.cc'
} else {
    New-Item -ItemType Directory -Force -Path .build | Out-Null
    $code = Invoke-Logged 'core-tests.log' {
        & $zig c++ -std=c++17 -O1 -Wall -Wextra src/core/core.cc tests/core_test.cc -o .build/core_test.exe
        if ($LASTEXITCODE -eq 0) { & .\.build\core_test.exe }
    }
    $tail = (Get-Content (Join-Path $ReportDir 'core-tests.log') -Tail 1 -ErrorAction SilentlyContinue)
    if ($code -eq 0) { Add-Result 'core-tests' 'PASS' "$tail" }
    else { Add-Result 'core-tests' 'FAIL' "exit $code (see core-tests.log) $tail" }
}

# ------------------------------------------- 4. python tests and doc checks
$pythonSteps = @(
    @{ Name = 'build-tests'; Args = @('tests/build_test.py'); NeedsBash = $false },
    @{ Name = 'settings-layout'; Args = @('tests/settings_layout_test.py'); NeedsBash = $false },
    @{ Name = 'docs-check'; Args = @('scripts/check_docs.py'); NeedsBash = $false },
    @{ Name = 'release-facts'; Args = @('scripts/release_facts.py', '--check'); NeedsBash = $false },
    @{ Name = 'release-tests'; Args = @('tests/release_test.py'); NeedsBash = $true }
)
foreach ($step in $pythonSteps) {
    if (-not $havePython) {
        Add-Result $step.Name 'SKIP' 'Python 3 is not available'
    } elseif ($step.NeedsBash -and -not $haveBash) {
        Add-Result $step.Name 'SKIP' 'needs bash (Git for Windows) for the publish-script tests'
    } else {
        $stepArgs = $step.Args
        $code = Invoke-Logged "$($step.Name).log" { & $python @stepArgs }
        if ($code -eq 0) { Add-Result $step.Name 'PASS' "python $($stepArgs -join ' ')" }
        else { Add-Result $step.Name 'FAIL' "exit $code (see $($step.Name).log)" }
    }
}

# ------------------------------------------------------------ 5. self-test
$selfReport = Join-Path $ReportDir 'self-test.txt'
if (-not $haveExe) {
    Add-Result 'self-test' 'FAIL' 'dist\YoutubeLyrics.exe does not exist'
} else {
    Remove-Item $selfReport -ErrorAction SilentlyContinue
    $code = Invoke-App @('--self-test', "`"$selfReport`"") 180
    $text = if (Test-Path $selfReport) { Get-Content $selfReport -Raw -Encoding UTF8 } else { '' }
    if ($null -eq $code) { Add-Result 'self-test' 'FAIL' 'did not finish within 180 s' }
    elseif (-not $text) { Add-Result 'self-test' 'FAIL' "wrote no result file (exit $code)" }
    elseif ($code -eq 0 -and $text -match 'RESULT: PASS' -and $text -notmatch '(?m)^FAIL  ') {
        Add-Result 'self-test' 'PASS' 'see self-test.txt'
    } else { Add-Result 'self-test' 'FAIL' "exit $code; see self-test.txt" }
}

# --------------------------------------------------------- 6. desktop test
$desktopReport = Join-Path $ReportDir 'desktop-test.txt'
if ($SkipDesktop) {
    Add-Result 'desktop-test' 'SKIP' '-SkipDesktop was given'
} elseif (-not $haveExe) {
    Add-Result 'desktop-test' 'FAIL' 'dist\YoutubeLyrics.exe does not exist'
} elseif (Get-Process -Name 'YoutubeLyrics' -ErrorAction SilentlyContinue) {
    Add-Result 'desktop-test' 'SKIP' 'Youtube-Lyrics is running; quit it from the tray and run again'
} else {
    Remove-Item $desktopReport -ErrorAction SilentlyContinue
    $code = Invoke-App @('--desktop-test', "`"$desktopReport`"") 120
    $text = if (Test-Path $desktopReport) { Get-Content $desktopReport -Raw -Encoding UTF8 } else { '' }
    $summary = ''
    if ($text -match '(?m)^(\d+ passed, \d+ failed, \d+ skipped)') { $summary = $Matches[1] }
    if ($null -eq $code) { Add-Result 'desktop-test' 'FAIL' 'did not finish within 120 s' }
    elseif (-not $text) { Add-Result 'desktop-test' 'FAIL' "wrote no result file (exit $code)" }
    elseif ($code -eq 0 -and $text -match '(?m)^RESULT: PASS') {
        Add-Result 'desktop-test' 'PASS' "$summary; see desktop-test.txt and desktop-test.png (window relationships only, not appearance)"
    } elseif ($code -eq 2) {
        Add-Result 'desktop-test' 'SKIP' "$summary; some checks could not be made (minimise windows with Win+D and run again); see desktop-test.txt"
    } else { Add-Result 'desktop-test' 'FAIL' "$summary; exit $code; see desktop-test.txt" }
}

# ------------------------------------------------------------------ report
$failed = @($results | Where-Object { $_.Status -eq 'FAIL' }).Count
$skipped = @($results | Where-Object { $_.Status -eq 'SKIP' }).Count
$overall = if ($failed) { 'FAIL' } elseif ($skipped) { 'INCOMPLETE' } else { 'PASS' }

$os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
$ubr = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue).UBR
$displays = @()
try {
    Add-Type -AssemblyName System.Windows.Forms
    $displays = [System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
        "{0}x{1} at ({2},{3}){4}" -f $_.Bounds.Width, $_.Bounds.Height, $_.Bounds.X, $_.Bounds.Y, $(if ($_.Primary) { ' primary' } else { '' })
    }
} catch { }
$commit = ''
try { $commit = (& git rev-parse HEAD 2>$null) } catch { }

$lines = @()
$lines += 'Youtube-Lyrics local verification report'
$lines += "Date:        $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
$lines += "Version:     $((Get-Content VERSION -Raw).Trim())  (channel: $((Get-Content RELEASE_CHANNEL -Raw).Trim()))"
$lines += "Commit:      $commit"
$lines += "Executable:  $exe"
$lines += "SHA-256:     $exeHash"
if ($os) { $lines += "Windows:     $($os.Caption) $($os.Version).$ubr ($($os.OSArchitecture))" }
$lines += "Displays:    $($displays -join '; ')  (logical pixels as PowerShell sees them)"
$lines += ''
foreach ($r in $results) { $lines += ("{0,-5} {1,-16} {2}" -f $r.Status, $r.Step, $r.Detail) }
$lines += ''
$lines += "RESULT: $overall  ($failed failed, $skipped skipped)"
$lines += ''
$lines += 'This report covers automated checks only. It does not show that lyrics look'
$lines += 'right, follow real music, or stay behind icons and windows in daily use; that'
$lines += 'is the manual matrix in engineering\WINDOWS_TEST_MATRIX.md.'
$reportPath = Join-Path $ReportDir 'verification-report.txt'
$lines | Set-Content -Encoding UTF8 $reportPath

Write-Host ''
Write-Host "RESULT: $overall  ($failed failed, $skipped skipped)" -ForegroundColor $(if ($failed) { 'Red' } elseif ($skipped) { 'Yellow' } else { 'Green' })
Write-Host "Report and logs: $ReportDir"
if ($failed) { exit 1 }
if ($skipped) { exit 2 }
exit 0
