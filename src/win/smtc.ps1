# Youtube-Lyrics media helper.
#
# Polls the Windows "now playing" session - the same information behind the
# volume-key overlay - and writes one compact JSON line per tick to stdout.
# That is how the app knows what YouTube Music (or Spotify, or any other player)
# is doing while it sits in the background.
#
# Runs under Windows PowerShell 5.1, which every Windows 10/11 install has.
param([int]$ParentPid = 0)

$ErrorActionPreference = 'Stop'

try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch { }

function Write-Line([string]$s) {
    [Console]::Out.WriteLine($s)
    [Console]::Out.Flush()
}

function Emit-Obj($h) {
    try { Write-Line (ConvertTo-Json -InputObject $h -Compress -Depth 4) }
    catch { Write-Line '{"ok":0,"status":"error","msg":"json"}' }
}

function Emit-Error([string]$where, [string]$what) {
    if ($what) { $what = $what -replace '[\r\n"]', ' ' }
    Emit-Obj ([ordered]@{ ok = 0; status = 'error'; where = $where; msg = $what })
}

# --- WinRT plumbing ---------------------------------------------------------
try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null
} catch {
    Emit-Error 'runtime' 'System.Runtime.WindowsRuntime is missing'
    exit 1
}

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and
        $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    })[0]

if ($null -eq $asTaskGeneric) {
    Emit-Error 'runtime' 'AsTask helper not found'
    exit 1
}

# Turns a WinRT IAsyncOperation into a blocking call.
function Await($op, $type) {
    $task = $asTaskGeneric.MakeGenericMethod($type).Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    return $task.Result
}

# Type handles for Await(): the ContentType hint makes PowerShell resolve these
# WinRT types without needing an assembly reference.
$tMgr    = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
$tProps  = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties, Windows.Media.Control, ContentType = WindowsRuntime]
$tStream = [Windows.Storage.Streams.IRandomAccessStreamWithContentType, Windows.Storage.Streams, ContentType = WindowsRuntime]
$null = $tProps, $tStream

if ($null -eq $tMgr) {
    Emit-Error 'winrt' 'Windows.Media.Control is unavailable'
    exit 1
}

# --- cover art --------------------------------------------------------------
$artDir = Join-Path $env:LOCALAPPDATA 'YouTubeLyrics\art'
try {
    New-Item -ItemType Directory -Force -Path $artDir | Out-Null
    # Keep the cover-art cache from growing forever.
    Get-ChildItem -LiteralPath $artDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-14) } |
        Remove-Item -Force -ErrorAction SilentlyContinue
} catch { }

function Get-ArtPath($props, [string]$key) {
    try {
        if ($null -eq $props -or $null -eq $props.Thumbnail) { return '' }
        $md5 = [System.Security.Cryptography.MD5]::Create()
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($key)
        $hash = [System.BitConverter]::ToString($md5.ComputeHash($bytes)).Replace('-', '').ToLower()
        $md5.Dispose()
        $file = Join-Path $artDir ($hash.Substring(0, 20) + '.jpg')
        if (Test-Path -LiteralPath $file) { return $file }

        $stream = Await ($props.Thumbnail.OpenReadAsync()) $tStream
        if ($null -eq $stream) { return '' }
        $size = [uint32]$stream.Size
        if ($size -le 0 -or $size -gt 12000000) { return '' }

        $reader = [Windows.Storage.Streams.DataReader]::new($stream)
        Await ($reader.LoadAsync($size)) ([uint32]) | Out-Null
        $buffer = New-Object byte[] $size
        $reader.ReadBytes($buffer)
        try { $reader.Dispose() } catch { }

        $tmp = $file + '.tmp'
        [System.IO.File]::WriteAllBytes($tmp, $buffer)
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
        Move-Item -LiteralPath $tmp -Destination $file -Force
        return $file
    } catch {
        return ''
    }
}

# --- main loop --------------------------------------------------------------
$manager = $null
$consecutiveErrors = 0

while ($true) {
    if ($ParentPid -gt 0) {
        if ($null -eq (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue)) { break }
    }

    try {
        if ($null -eq $manager) {
            $manager = Await ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]::RequestAsync()) $tMgr
        }
        if ($null -eq $manager) { throw 'no session manager' }

        $sessions = $manager.GetSessions()
        $chosen = $null
        $firstPlaying = $null
        $firstAny = $null

        foreach ($s in $sessions) {
            if ($null -eq $firstAny) { $firstAny = $s }
            $info = $null
            try { $info = $s.GetPlaybackInfo() } catch { continue }
            if ($null -eq $info) { continue }
            $isPlaying = ($info.PlaybackStatus.ToString() -eq 'Playing')
            if ($isPlaying) {
                if ($null -eq $firstPlaying) { $firstPlaying = $s }
                $id = [string]$s.SourceAppUserModelId
                if ($id -match '(?i)(chrome|msedge|brave|firefox|opera|vivaldi|chromium|spotify)') {
                    $chosen = $s
                    break
                }
            }
        }
        if ($null -eq $chosen) {
            if ($null -ne $firstPlaying) { $chosen = $firstPlaying }
            elseif ($null -ne $firstAny) { $chosen = $firstAny }
        }

        if ($null -eq $chosen) {
            Emit-Obj ([ordered]@{ ok = 1; status = 'none'; title = ''; artist = ''; album = ''; pos = 0; dur = 0; app = ''; art = '' })
            $consecutiveErrors = 0
            Start-Sleep -Milliseconds 500
            continue
        }

        $status = ''
        $title = ''
        $artist = ''
        $album = ''
        $pos = 0.0
        $dur = 0.0

        try { $status = [string]$chosen.GetPlaybackInfo().PlaybackStatus } catch { }
        if ($status -eq '') { $status = 'Unknown' }

        $props = $null
        try { $props = Await ($chosen.TryGetMediaPropertiesAsync()) $tProps } catch { $props = $null }
        if ($null -ne $props) {
            try {
                $title = [string]$props.Title
                $artist = [string]$props.Artist
                $album = [string]$props.AlbumTitle
            } catch { }
        }

        try {
            $tl = $chosen.GetTimelineProperties()
            $start = $tl.StartTime.TotalSeconds
            $pos = $tl.Position.TotalSeconds - $start
            $dur = $tl.EndTime.TotalSeconds - $start
            if ($status -eq 'Playing') {
                $delta = ([DateTimeOffset]::UtcNow - $tl.LastUpdatedTime).TotalSeconds
                if ($delta -gt 0 -and $delta -lt 60) { $pos += $delta }
            }
        } catch { }

        if ($null -eq $pos -or $pos -lt 0) { $pos = 0.0 }
        if ($null -eq $dur -or $dur -lt 0) { $dur = 0.0 }

        $art = ''
        if ($title -ne '') {
            $art = Get-ArtPath $props ($title + '|' + $artist + '|' + $album)
        }

        Emit-Obj ([ordered]@{
                ok     = 1
                status = $status.ToLower()
                title  = $title
                artist = $artist
                album  = $album
                pos    = [math]::Round([double]$pos, 3)
                dur    = [math]::Round([double]$dur, 3)
                app    = [string]$chosen.SourceAppUserModelId
                art    = $art
            })
        $consecutiveErrors = 0
    } catch {
        $consecutiveErrors++
        Emit-Error 'loop' ([string]$_.Exception.Message)
        $manager = $null
        if ($consecutiveErrors -gt 30) { break }
        Start-Sleep -Milliseconds 1200
        continue
    }

    Start-Sleep -Milliseconds 400
}
