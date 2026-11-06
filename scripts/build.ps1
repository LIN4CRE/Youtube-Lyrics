# Builds dist\YoutubeLyrics.exe on Windows.
#   Requires: Zig on PATH (https://ziglang.org/download) and Python 3 + Pillow.
#   Usage:  powershell -ExecutionPolicy Bypass -File scripts\build.ps1
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$zig = if ($env:ZIG) { $env:ZIG } else { 'zig' }
$python = if ($env:PYTHON) { $env:PYTHON } else { 'python' }

New-Item -ItemType Directory -Force -Path dist, .build | Out-Null

# 1. Embed the PowerShell helper as a C++ raw string literal.
& $python -c @"
src = open('src/win/smtc.ps1', encoding='utf-8').read()
assert ')YTLYSPLIT"' not in src
out = ('// Generated - do not edit.\n#pragma once\nnamespace ytl {\nstatic const char kSmtcScript[] = R\"YTLYSPLIT('
       + src + ')YTLYSPLIT\";\n}\n')
open('.build/smtc_script.h', 'w', encoding='utf-8').write(out)
"@

# 2. Icon + version resources.
$res = @()
try {
    & $python scripts/make_icon.py .build
    & $zig rc /i .build src/win/app.rc .build/app.res
    if ($LASTEXITCODE -eq 0) { $res = @('.build/app.res') }
} catch { Write-Host 'note: building without an embedded icon' }

# 3. Cross/native compile (Zig drives the linker either way).
& $zig c++ -target x86_64-windows-gnu -std=c++17 -O2 `
    -DUNICODE -D_UNICODE `
    -municode -Wl,--subsystem,windows -Wl,--gc-sections `
    -fno-exceptions -fno-rtti -fno-strict-aliasing `
    -I.build -o dist/YoutubeLyrics.exe `
    src/core/core.cc src/win/app.cc src/win/render.cc src/win/media.cc src/win/lyrics.cc `
    @res `
    -lgdiplus -lgdi32 -luser32 -lole32 -luuid -lshell32 -lshlwapi -ldwmapi `
    -lwinhttp -lcomdlg32 -ladvapi32
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
Remove-Item -Force -ErrorAction SilentlyContinue dist/core.lib
Write-Host "built dist\YoutubeLyrics.exe"
