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
    # Bounded: a media session that never answers (a tab being closed, a player
    # that is hung) used to block here for ever, and with it every lyric.
    if (-not $task.Wait(8000)) { throw 'media session did not answer in time' }
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
    $stream = $null
    $reader = $null
    $tmp = $null
    try {
        if ($null -eq $props -or $null -eq $props.Thumbnail) { return '' }
        $md5 = [System.Security.Cryptography.MD5]::Create()
        try {
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($key)
            $hash = [System.BitConverter]::ToString($md5.ComputeHash($bytes)).Replace('-', '').ToLower()
        } finally {
            $md5.Dispose()
        }
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

        $tmp = $file + '.tmp'
        [System.IO.File]::WriteAllBytes($tmp, $buffer)
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
        Move-Item -LiteralPath $tmp -Destination $file -Force
        $tmp = $null
        return $file
    } catch {
        return ''
    } finally {
        if ($reader) { try { $reader.Dispose() } catch { } }
        if ($stream) { try { $stream.Dispose() } catch { } }
        if ($tmp -and (Test-Path -LiteralPath $tmp)) {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        }
    }
}

# --- main loop --------------------------------------------------------------
$manager = $null
$managerAt = [DateTime]::MinValue
$consecutiveErrors = 0
# The app whose session was last seen *playing*. A paused session is only ever
# reported for this app: Windows' "current session" can be a player that was
# paused hours ago, and following it put a long-forgotten song's lyrics on
# screen whenever the real player stopped between tracks.
$lastApp = ''

while ($true) {
    if ($ParentPid -gt 0) {
        if ($null -eq (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue)) { break }
    }

    try {
        # The manager is cheap to request; refreshing it periodically means a
        # stale COM object can never leave the listener deaf for the rest of
        # the session.
        if ($null -ne $manager -and ((Get-Date) - $managerAt).TotalSeconds -gt 120) { $manager = $null }
        if ($null -eq $manager) {
            $managerAt = Get-Date
            $manager = Await ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]::RequestAsync()) $tMgr
        }
        if ($null -eq $manager) { throw 'no session manager' }

        $sessions = $manager.GetSessions()

        # Rank actively playing sessions first; paused sessions are considered
        # only if Windows identifies one as its current music session below.
        $chosen = $null
        $firstPlaying = $null

        $musicRank = {
            param($id)
            $id = ([string]$id).ToLowerInvariant()
            if ($id -match 'spotify|applemusic|tidal|deezer|music\.ui|amazon') { return 3 }
            if ($id -match 'chrome|msedge|brave|firefox|opera|vivaldi|chromium') { return 2 }
            if ($id -match 'vlc|wmplayer|foobar|musicbee|aimp|mpc|plex|itunes') { return 1 }
            return 0
        }

        foreach ($s in $sessions) {
            $info = $null
            try { $info = $s.GetPlaybackInfo() } catch { continue }
            if ($null -eq $info) { continue }
            $state = [string]$info.PlaybackStatus
            if ($state -eq 'Playing') {
                if ($null -eq $firstPlaying) { $firstPlaying = $s }
                elseif ((& $musicRank $s.SourceAppUserModelId) -gt (& $musicRank $firstPlaying.SourceAppUserModelId)) {
                    $firstPlaying = $s
                }
            }
        }

        # Prefer Windows' current session when it is playing, then the best
        # ranked playing session. When nothing is playing, keep following the
        # app we were already showing while it is explicitly Paused, so the
        # sheet survives a pause. A paused session from any other app is never
        # adopted.
        try {
            $current = $manager.GetCurrentSession()
            if ($null -ne $current) {
                $currentInfo = $current.GetPlaybackInfo()
                $currentRank = & $musicRank $current.SourceAppUserModelId
                if ($null -ne $currentInfo -and $currentRank -gt 0 -and
                    ([string]$currentInfo.PlaybackStatus) -eq 'Playing') {
                    $chosen = $current
                }
            }
        } catch { }
        if ($null -eq $chosen -and $null -ne $firstPlaying) {
            $chosen = $firstPlaying
        }
        if ($null -ne $chosen) {
            $lastApp = [string]$chosen.SourceAppUserModelId
        } elseif ($lastApp -ne '') {
            foreach ($s in $sessions) {
                try {
                    if (([string]$s.SourceAppUserModelId) -ne $lastApp) { continue }
                    $info = $s.GetPlaybackInfo()
                    if ($null -ne $info -and ([string]$info.PlaybackStatus) -eq 'Paused') {
                        $chosen = $s
                        break
                    }
                } catch { }
            }
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
        $upd = [int64]0
        $stale = 0

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
            # LastUpdatedTime says when the player last wrote the timeline. Some
            # players never set it (it reads as the year 1601); extrapolating
            # from that pinned the position to the end of the song.
            $stamp = $tl.LastUpdatedTime
            if ($stamp.Year -ge 2015) {
                $upd = [int64]$stamp.ToUnixTimeMilliseconds()
                if ($status -eq 'Playing') {
                    $delta = ([DateTimeOffset]::UtcNow - $stamp).TotalSeconds
                    if ($delta -gt 0) {
                        if ($dur -gt 0 -and ($pos + $delta) -gt ($dur + 1.5)) {
                            # The timeline is older than the rest of the track:
                            # it belongs to a previous song or was never
                            # refreshed. Report it as stale, do not invent.
                            $stale = 1
                            $pos = $dur
                        } else {
                            $pos += $delta
                        }
                    }
                }
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
                upd    = $upd
                stale  = $stale
                tick   = [int64](([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()))
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

    Start-Sleep -Milliseconds 250
}
