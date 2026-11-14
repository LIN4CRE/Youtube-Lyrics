# Builds dist\YoutubeLyrics.exe on Windows (native, or cross-compiled via Zig).
#   Requires: Zig on PATH (https://ziglang.org/download) and Python 3 + Pillow.
#   Usage:   powershell -ExecutionPolicy Bypass -File scripts\build.ps1
#
# This script is the Windows twin of scripts/build.sh and enforces the same
# gate. A build that cannot embed the icon and version resource, or whose
# finished executable fails verification, exits non-zero and leaves
# docs\downloads exactly as it was:
#   * the icon, version resource and resource compile are mandatory
#   * the PE is verified (x64, GUI subsystem, icon frames, embedded wallpapers,
#     version resource) before anything is published
#   * the public copy is replaced only after verification, and restored if the
#     copy or the checksum sidecar fails
#
# Set YTL_ALLOW_ICONLESS_BUILD=1 only for throwaway experiments; CI and the
# release workflow never use it.
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$zig = if ($env:ZIG) { $env:ZIG } else { 'zig' }
$python = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
$allowIconless = $env:YTL_ALLOW_ICONLESS_BUILD -eq '1'

$tempExe = '.build\YoutubeLyrics.exe'
$downloads = 'docs\downloads\YoutubeLyrics.exe'
$downloadsBackup = '.build\YoutubeLyrics.exe.previous'

function Fail([string]$message) {
    Write-Host "build failed: $message" -ForegroundColor Red
    exit 1
}

function Invoke-Checked([string]$what, [scriptblock]$action) {
    & $action
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit $LASTEXITCODE)" }
}

New-Item -ItemType Directory -Force -Path dist, .build | Out-Null

Invoke-Checked 'version generation' { & $python scripts/generate_version.py }

# 1. Embed the PowerShell media helper as a C++ raw string literal.
Invoke-Checked 'media-helper script generation' { & $python scripts/generate_smtc.py }

# 2. Application icon + version info -> COFF .res object. The icon is not
#    optional: an iconless executable used to be produced silently whenever the
#    resource compiler failed.
$res = @()
& $python -c 'import PIL' 2>$null
if ($LASTEXITCODE -ne 0) {
    if (-not $allowIconless) {
        Fail @"
Pillow is required to embed the application icon and version resource.
       Install it with 'pip install Pillow', or set YTL_ALLOW_ICONLESS_BUILD=1 for a
       throwaway build with no icon (never for a release).
"@
    }
    Write-Host 'note: Pillow not installed; YTL_ALLOW_ICONLESS_BUILD=1 set, continuing without resources' -ForegroundColor Yellow
} else {
    Invoke-Checked 'icon generation' { & $python scripts/make_icon.py .build }
    Invoke-Checked 'icon verification' { & $python scripts/make_icon.py --verify .build/app.ico }
    Remove-Item -Force -ErrorAction SilentlyContinue .build/app.res
    Invoke-Checked 'resource compilation' {
        & $zig rc /i .build /i src/win src/win/app.rc .build/app.res
    }
    if (-not (Test-Path .build/app.res) -or (Get-Item .build/app.res).Length -eq 0) {
        Fail 'resource compiler produced an empty object'
    }
    $res = @('.build/app.res')
}

# 3. Compile to a temporary name so a failed link can never truncate a working
#    dist\YoutubeLyrics.exe.
Remove-Item -Force -ErrorAction SilentlyContinue $tempExe
Invoke-Checked 'compile or link' {
    & $zig c++ -target x86_64-windows-gnu -std=c++17 -O2 `
        -DUNICODE -D_UNICODE `
        "-municode" "-Wl,--subsystem,windows" "-Wl,--gc-sections" `
        -fno-exceptions -fno-rtti -fno-strict-aliasing `
        -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type `
        -I.build -o $tempExe `
        src/core/core.cc src/win/app.cc src/win/render.cc src/win/media.cc `
        src/win/lyrics.cc src/win/update.cc src/win/settings.cc `
        @res `
        -lgdiplus -lgdi32 -luser32 -lole32 -luuid -lshell32 -lshlwapi -ldwmapi `
        -lwinhttp -lcomdlg32 -ladvapi32 -lbcrypt -lcomctl32
}
Remove-Item -Force -ErrorAction SilentlyContinue .build/core.lib, .build/YoutubeLyrics.pdb

# 3b. Remove the link time and the build GUID derived from it, so the same
#     source and toolchain always give the same bytes (and the same SHA-256).
Invoke-Checked 'executable normalization' { & $python scripts/normalize_pe.py $tempExe }

# 4. Verify the actual image, not the build log.
if ($res.Count -gt 0) {
    Invoke-Checked 'executable verification' {
        & $python scripts/verify_build.py $tempExe --ico .build/app.ico
    }
} else {
    Write-Host 'warning: building without an embedded icon (YTL_ALLOW_ICONLESS_BUILD=1)' -ForegroundColor Yellow
    Invoke-Checked 'PE sanity check' { & $python scripts/verify_build.py $tempExe --allow-missing-resources }
}

# 5. Publish. The public copy is replaced only now, and restored if the copy or
#    the checksum sidecar cannot be written.
$hash = (Get-FileHash $tempExe -Algorithm SHA256).Hash.ToLowerInvariant()
if (-not $hash) { Fail 'could not compute a SHA-256 checksum' }

$hadPrevious = Test-Path $downloads
if ($hadPrevious) { Copy-Item -Force $downloads $downloadsBackup }

try {
    Move-Item -Force $tempExe dist\YoutubeLyrics.exe
    Copy-Item -Force dist\YoutubeLyrics.exe $downloads
    Set-Content -NoNewline -Encoding ascii docs\downloads\YoutubeLyrics.exe.sha256 "$hash  YoutubeLyrics.exe`n"
} catch {
    if ($hadPrevious) {
        Copy-Item -Force $downloadsBackup $downloads
        Write-Host 'build failed: restored the previous docs\downloads copy' -ForegroundColor Red
    }
    throw
}

# Re-verify the published copy so "downloadable == verified" is a checked fact.
Invoke-Checked 'published copy verification' {
    & $python scripts/verify_build.py $downloads
}

# 6. Release manifest and the facts printed on the site and in the README are
#    derived from the published bytes, so one build leaves them all consistent.
if ($res.Count -gt 0) {
    Invoke-Checked 'release manifest' {
        & $python scripts/generate_release.py --exe $downloads --output-dir docs/downloads
    }
    Invoke-Checked 'release facts' { & $python scripts/release_facts.py }
}

Write-Host "built dist\YoutubeLyrics.exe ($((Get-Item dist\YoutubeLyrics.exe).Length) bytes)"
Write-Host "SHA-256: $hash"
Write-Host "published $downloads (byte-identical)"
