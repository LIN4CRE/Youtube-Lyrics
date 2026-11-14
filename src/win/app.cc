// Youtube-Lyrics - window, tray, hotkeys, orchestration.
#include "app.h"
#include "settings.h"
#include "update.h"
#include "version.h"

#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <knownfolders.h>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace ytl {
namespace {

using namespace Gdiplus;

const wchar_t* kClassName = L"YoutubeLyricsWindow";
const wchar_t* kAppName = L"Youtube-Lyrics";
const wchar_t* kAppVersion = YTL_VERSION_WIDE;
const wchar_t* kProjectUrl = L"https://github.com/LIN4CRE/Youtube-Lyrics";
const wchar_t* kSupportUrl = L"https://paypal.me/delinacre16";
const UINT_PTR kFrameTimer = 1;
const int kHotkeyToggle = 1;
const int kHotkeyWallpaper = 2;
const int kHotkeySettings = 3;
const int kHotkeyFullscreen = 4;
const UINT kTrayId = 1;
const UINT WM_APP_SHOW_EXISTING = WM_APP + 9;

App* g_app = nullptr;
std::mutex g_log_mu;
HICON g_tray_icon = nullptr;

// Position tracking used to keep lyrics in sync even for players that never
// report a timeline (some browsers). See note in README.
struct PosTracker {
  std::string key;
  TrackInfo track;
  int64_t reported = -1;
  int64_t last_change = 0;
  int64_t free_anchor = 0;
  bool free_running = false;
  // Timeline bookkeeping, see MediaState::timeline_stamp.
  int64_t stamp = 0;             // stamp of the most recent observation
  int64_t inherited_stamp = 0;   // stamp the previous track left behind
  int64_t inherited_reported = 0;  // ...and the position it left behind
  bool await_fresh = false;      // new track whose timeline is not written yet
  int64_t track_started_at = 0;  // tick when this track was first seen
};
PosTracker g_pos;

// ---------------------------------------------------------------- utilities

std::wstring widen(const std::string& s) { return utf8_to_wide(s); }


std::string temp_log_path() { return appdata_dir() + "\\ytlyrics.log"; }

// ------------------------------------------------------------- tray and icon

HBITMAP make_icon_bitmap(int size) {
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = size;
  bi.bV5Height = -size;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;

  HDC screen = GetDC(nullptr);
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi),
                                 DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!dib) return nullptr;

  // Paint into an ARGB surface first so we get real anti-aliased alpha.
  Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
  {
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.Clear(Color(0, 0, 0, 0));
    g.SetCompositingMode(CompositingModeSourceOver);

    // The brand mark: a lyric sheet on a paper tile. Kept numerically in step
    // with draw_mark() in scripts/brand.py, which draws the same figure for
    // the exe icon and the README artwork.
    const bool small = size <= 48;
    const REAL s = static_cast<REAL>(size);

    // GraphicsPath is not copyable, so the outline is written into a path the
    // caller owns.
    auto tile = [&](Gdiplus::GraphicsPath& p, REAL x, REAL y, REAL w, REAL h,
                    REAL radius) {
      REAL d = radius * 2;
      if (d > w) d = w;
      if (d > h) d = h;
      p.Reset();
      p.AddArc(x, y, d, d, 180, 90);
      p.AddArc(x + w - d, y, d, d, 270, 90);
      p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
      p.AddArc(x, y + h - d, d, d, 90, 90);
      p.CloseFigure();
    };

    Gdiplus::GraphicsPath path;
    tile(path, 0, 0, s, s, s * 0.235f);
    Gdiplus::SolidBrush paper(Color(255, 243, 239, 230));
    g.FillPath(&paper, &path);

    // The margin rule every line of the song hangs from.
    {
      REAL rx = s * (small ? 0.150f : 0.134f);
      REAL rw = s * (small ? 0.052f : 0.024f);
      tile(path, rx, s * 0.215f, rw, s * 0.570f, rw * 0.5f);
      Gdiplus::SolidBrush ink(Color(small ? 168 : 128, 11, 10, 15));
      g.FillPath(&ink, &path);
    }

    // Three lines of text; the middle one is the line being sung, part amber.
    struct Bar { REAL width, centre_y, height; bool lit; };
    const REAL thin = s * (small ? 0.120f : 0.075f);
    const REAL mid = s * (small ? 0.175f : 0.125f);
    const Bar bars[3] = {
        {small ? 0.350f : 0.340f, 0.345f, thin, false},
        {small ? 0.580f : 0.600f, 0.500f, mid, true},
        {small ? 0.265f : 0.265f, 0.655f, thin, false},
    };
    const REAL x0 = s * (small ? 0.268f : 0.240f);
    for (int i = 0; i < 3; ++i) {
      REAL w = s * bars[i].width;
      REAL h = bars[i].height;
      REAL y = s * bars[i].centre_y - h * 0.5f;
      tile(path, x0, y, w, h, h * 0.5f);
      if (bars[i].lit) {
        Gdiplus::SolidBrush unsung(Color(132, 11, 10, 15));
        g.FillPath(&unsung, &path);
        tile(path, x0, y, w * 0.66f, h, h * 0.5f);
        Gdiplus::SolidBrush amber(Color(255, 245, 165, 36));
        g.FillPath(&amber, &path);
      } else {
        Gdiplus::SolidBrush ink(Color(small ? 190 : 150, 11, 10, 15));
        g.FillPath(&ink, &path);
      }
    }
  }

  Gdiplus::Rect lock(0, 0, size, size);
  Gdiplus::BitmapData bd{};
  if (bmp.LockBits(&lock, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
    BYTE* src = static_cast<BYTE*>(bd.Scan0);
    BYTE* dst = static_cast<BYTE*>(bits);
    for (int y = 0; y < size; ++y) {
      const BYTE* srow = src + static_cast<size_t>(y) * bd.Stride;
      BYTE* drow = dst + static_cast<size_t>(y) * size * 4;
      for (int x = 0; x < size; ++x) {
        BYTE a = srow[x * 4 + 3];
        // premultiply for CreateIconIndirect
        drow[x * 4 + 0] = static_cast<BYTE>(srow[x * 4 + 0] * a / 255);
        drow[x * 4 + 1] = static_cast<BYTE>(srow[x * 4 + 1] * a / 255);
        drow[x * 4 + 2] = static_cast<BYTE>(srow[x * 4 + 2] * a / 255);
        drow[x * 4 + 3] = a;
      }
    }
    bmp.UnlockBits(&bd);
  }
  return dib;
}

HICON make_app_icon(int size) {
  HBITMAP color = make_icon_bitmap(size);
  if (!color) return LoadIconW(nullptr, IDI_APPLICATION);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = color;
  ii.hbmMask = mask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

// ------------------------------------------------------------------ app bits

void invalidate_window(App& app) {
  if (app.hwnd) InvalidateRect(app.hwnd, nullptr, FALSE);
}

void reset_document(App& app) {
  app.lyrics = Lyrics();
  app.disp.clear();
  app.pending_plain_lines.clear();
  app.timing_estimated = false;
  app.view.line_top.clear();
  app.view.line_h.clear();
  app.view.total_h = 0;
  app.view.scroll = 0;
  app.view.line_activity.clear();
  app.view.smooth_fill = 0.0;
  app.view.smooth_fill_line = -1;
  app.active = -1;
}

// Commits one media observation as the playback clock. `first_seen_at` is the
// tick at which a newly confirmed track was first reported (0 otherwise).
void update_position_tracker(App& app, const MediaState& observed,
                             int64_t first_seen_at = 0) {
  const bool was_playing = app.active_media.playing;
  const bool had_track = !g_pos.key.empty();
  const bool new_recording = !had_track || !same_track(g_pos.track, app.active_track);
  const int64_t previous_stamp = g_pos.stamp;
  const int64_t previous_reported = g_pos.reported;

  app.active_media = observed;
  app.active_media.track = app.active_track;
  g_pos.key = track_key(app.active_track);
  g_pos.track = app.active_track;
  g_pos.stamp = observed.timeline_stamp;

  if (new_recording) {
    g_pos.reported = observed.pos_ms;
    g_pos.last_change = observed.at_ms;
    g_pos.free_anchor = observed.at_ms;
    g_pos.free_running = false;
    g_pos.track_started_at = first_seen_at > 0 ? first_seen_at : observed.at_ms;
    g_pos.inherited_stamp = previous_stamp;
    // Browsers change the title first and rewrite the timeline a moment (or
    // many seconds) later. Until they do, the reported position is the *end
    // of the previous song*, which is what made a new track open on its last
    // lines. A timeline that still carries the old stamp, or that the helper
    // flagged as older than the track, is not this track's position.
    // "Inherited" also requires the position to be impossible for a track that
    // has only just appeared; a player that rewrites the timeline *before* the
    // title is already reporting the right thing.
    const int64_t existed_for = std::max<int64_t>(0, observed.at_ms - g_pos.track_started_at);
    const bool implausible = observed.pos_ms > existed_for + 4000;
    const bool same_stamp = observed.timeline_stamp != 0 &&
                            observed.timeline_stamp == previous_stamp;
    const bool same_raw = observed.timeline_stamp == 0 &&
                          std::llabs(observed.pos_ms - previous_reported) <= 150;
    g_pos.inherited_reported = observed.pos_ms;
    g_pos.await_fresh = had_track && observed.playing &&
                        (observed.timeline_stale ||
                         (implausible && (same_stamp || same_raw)));
    if (g_pos.await_fresh)
      logf("media: timeline not updated for the new track yet; counting from 0");
  }

  if (g_pos.await_fresh) {
    const bool fresh =
        !observed.timeline_stale &&
        (observed.timeline_stamp != 0
             ? observed.timeline_stamp != g_pos.inherited_stamp
             : std::llabs(observed.pos_ms - g_pos.inherited_reported) > 150);
    if (fresh) {
      g_pos.await_fresh = false;
      g_pos.reported = observed.pos_ms;
      g_pos.last_change = observed.at_ms;
    } else {
      // Count from the moment the track appeared. Paused before any real
      // timeline arrived: hold what has elapsed so far.
      const int64_t elapsed = std::max<int64_t>(0, observed.at_ms - g_pos.track_started_at);
      if (observed.playing) {
        app.active_media.pos_ms = 0;
        app.active_media.at_ms = g_pos.track_started_at;
      } else {
        app.active_media.pos_ms = was_playing ? elapsed : 0;
        app.active_media.at_ms = observed.at_ms;
        g_pos.await_fresh = false;
      }
      return;
    }
  }

  if (!observed.playing) {
    g_pos.reported = observed.pos_ms;
    g_pos.last_change = observed.at_ms;
    g_pos.free_anchor = observed.at_ms;
    g_pos.free_running = false;
    return;
  }

  const bool resumed = !was_playing;
  if (new_recording || resumed) {
    g_pos.reported = observed.pos_ms;
    g_pos.last_change = observed.at_ms;
    g_pos.free_anchor = observed.at_ms;
    g_pos.free_running = false;
  } else {
    if (std::llabs(observed.pos_ms - g_pos.reported) > 150) {
      g_pos.reported = observed.pos_ms;
      g_pos.last_change = observed.at_ms;
      g_pos.free_running = false;
    } else if (!g_pos.free_running && observed.at_ms - g_pos.last_change > 5000 &&
               observed.pos_ms < 1500) {
      g_pos.free_running = true;
      g_pos.free_anchor = g_pos.last_change;
      logf("media: player reports no playhead, running free clock");
    }
  }

  if (g_pos.free_running) {
    app.active_media.at_ms = g_pos.free_anchor;
    app.active_media.pos_ms = 0;
  } else if (observed.timeline_stamp == 0) {
    // No "last updated" time from this player, so the helper cannot
    // extrapolate for us. Run the clock from when the reported value last
    // moved; re-anchoring to an unchanged value on every poll froze the
    // lyrics in place.
    app.active_media.pos_ms = g_pos.reported;
    app.active_media.at_ms = g_pos.last_change;
  }
}

void on_media(App& app) {
  MediaState m;
  {
    std::lock_guard<std::mutex> lock(app.mu);
    m = app.media;
  }
  if (!m.raw.empty()) app.helper_heard = true;
  // A different app's *paused* session is not what is being listened to. It is
  // handled exactly like "no session": the current sheet freezes and then
  // clears, instead of being replaced by a song from hours ago.
  if (m.have && !m.playing && !app.active_track.title.empty() &&
      !app.active_media.app.empty() &&
      to_lower_ascii(m.app) != to_lower_ascii(app.active_media.app) &&
      !same_track(app.active_track, m.track)) {
    m.have = false;
  }
  app.session_present = m.have && !m.track.title.empty();
  const int64_t now = m.at_ms > 0 ? m.at_ms : app.now_ms();

  if (m.have && !m.track.title.empty()) {
    app.missing_since = 0;

    // A brief withdrawal of artist/duration metadata is the same observation,
    // not a new track. Keep known-good fields rather than degrading the active
    // identity until the next SMTC poll.
    if (!app.active_track.title.empty() && same_track(app.active_track, m.track)) {
      TrackInfo refined = m.track;
      if (m.art_path.empty()) m.art_path = app.active_media.art_path;
      if (refined.artist.empty()) refined.artist = app.active_track.artist;
      if (refined.album.empty()) refined.album = app.active_track.album;
      if (refined.duration_s <= 0) refined.duration_s = app.active_track.duration_s;
      m.duration_s = refined.duration_s;
      app.active_track = clean_track(refined, false);
      app.current_key = track_key(app.active_track);
      app.pending_key.clear();
      app.pending_track = TrackInfo();
      app.pending_seen = 0;
      update_position_tracker(app, m);
    } else {
      // Require the same, credible track in consecutive polls before replacing
      // the sheet. A stale SMTC session or a one-poll title glitch cannot make
      // us fetch lyrics for the wrong song.
      const bool same_pending = !app.pending_track.title.empty() &&
                                same_track(app.pending_track, m.track);
      if (!same_pending) {
        app.pending_track = m.track;
        app.pending_key = m.key;
        app.pending_seen = 1;
        app.pending_since = now;
        logf("media: candidate '%s' (%s)", track_label(m.track).c_str(),
             m.key.c_str());
      } else {
        ++app.pending_seen;
        if (app.pending_track.artist.empty() && !m.track.artist.empty())
          app.pending_track = m.track;
      }

      bool confirmed = app.pending_seen >= 2 && now - app.pending_since >= 300;
      const bool replacing = !app.active_track.title.empty();
      // A session that is merely *paused* never replaces what is on screen
      // unless it is the same player (pause, then skip). A different app that
      // happens to be paused in the background is not what the user is
      // listening to.
      if (confirmed && replacing && !m.playing && !app.active_media.app.empty() &&
          to_lower_ascii(m.app) != to_lower_ascii(app.active_media.app)) {
        confirmed = false;  // unreachable after the filter above; kept as a guard
      }
      // The title changes before the duration does. A "new" song that still
      // has exactly the old song's length is given a moment to settle, so the
      // lookup is not made (and cached) with the wrong duration.
      if (confirmed && replacing && m.playing && m.duration_s > 5 &&
          std::fabs(m.duration_s - app.active_media.duration_s) < 0.35 &&
          now - app.pending_since < 1500) {
        confirmed = false;
      }
      if (confirmed) {
        TrackInfo next = app.pending_track;
        if (next.title.empty()) next = m.track;
        // The newest poll carries the settled duration and album.
        if (m.track.duration_s > 0) next.duration_s = m.track.duration_s;
        if (next.album.empty()) next.album = m.track.album;
        const int64_t first_seen_at = app.pending_since;
        const bool different = app.active_track.title.empty() ||
                               !same_track(app.active_track, next);
        app.active_track = clean_track(next, false);
        app.current_key = track_key(app.active_track);
        app.pending_key.clear();
        app.pending_track = TrackInfo();
        app.pending_seen = 0;

        if (different) {
          // Starting a distinct song invalidates the old lyric sheet immediately;
          // never leave Linkin Park words on screen while a new lookup runs.
          reset_document(app);
          app.lstate = LyricsState::Loading;
          app.lmessage.clear();
          app.lyrics_retries = 0;
          app.lyrics_retry_at = 0;
          logf("media: now playing '%s' (%s) dur=%.1f",
               track_label(app.active_track).c_str(), app.current_key.c_str(),
               m.duration_s);
          lyrics_enqueue(app.active_track, ++app.lyrics_req_id, false);
        }
        update_position_tracker(app, m, different ? first_seen_at : 0);
      }
    }
  } else {
    app.pending_key.clear();
    app.pending_track = TrackInfo();
    app.pending_seen = 0;

    if (app.missing_since == 0) app.missing_since = now;
    // Freeze at the last credible position as soon as the session vanishes.
    // The active song and lyrics survive short SMTC gaps; they are cleared only
    // after three seconds with no watchable session.
    if (!app.active_track.title.empty() && app.active_media.playing) {
      const int64_t duration = static_cast<int64_t>(app.active_media.duration_s * 1000.0);
      int64_t frozen = interpolate_pos(app.active_media.pos_ms, app.active_media.at_ms,
                                        now, true, duration);
      frozen -= app.cfg.offset_ms;
      if (frozen < 0) frozen = 0;
      app.active_media.pos_ms = frozen;
      app.active_media.at_ms = now;
      app.active_media.playing = false;
    }

    if (now - app.missing_since >= 3000 && !app.active_track.title.empty()) {
      logf("media: session gone for 3s; clearing '%s'",
           track_label(app.active_track).c_str());
      app.active_track = TrackInfo();
      app.active_media = MediaState();
      app.current_key.clear();
      app.missing_since = 0;
      ++app.lyrics_req_id;  // ignore any lookup that finishes after the session
      app.lstate = LyricsState::Idle;
      app.lmessage.clear();
      app.lyrics_retry_at = 0;
      reset_document(app);
      g_pos = PosTracker();
    }
  }

  refresh_art(app);
  invalidate_window(app);
}

void on_lyrics(App& app) {
  LyricsState state;
  Lyrics lyrics;
  std::string msg;
  if (!lyrics_take_result(app.lyrics_req_id, &state, &lyrics, &msg)) return;

  app.lstate = state;
  app.lmessage = msg;
  if (state == LyricsState::Ready) {
    app.lyrics = lyrics;
    logf("lyrics: %s (%s, %zu lines)", lyrics.synced ? "synced" : "unsynced",
         track_label(TrackInfo{lyrics.title, lyrics.artist, lyrics.album, 0}).c_str(), lyrics.lines.size());
  } else {
    app.lyrics = Lyrics();
    logf("lyrics: lookup failed: %s", msg.c_str());
    const bool transient = msg.find("transport error") != std::string::npos ||
                           msg.find("unavailable") != std::string::npos;
    if (transient && app.lyrics_retries < 3) {
      ++app.lyrics_retries;
      app.lyrics_retry_at = app.now_ms() + 12000 * app.lyrics_retries;
    }
  }
  app.rebuild_layout(true);
  app.view.scroll = scroll_target(app, app.view.lay_h > 0
                                           ? static_cast<int>(app.view.lay_h)
                                           : 800);
  invalidate_window(app);
}

// ---------------------------------------------------------------- fullscreen

BOOL CALLBACK find_workerw_after_desktop(HWND top, LPARAM param) {
  HWND def_view = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
  if (!def_view) return TRUE;
  HWND worker = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
  if (worker) {
    *reinterpret_cast<HWND*>(param) = worker;
    return FALSE;
  }
  return TRUE;
}

HWND find_desktop_host() {
  HWND progman = FindWindowW(L"Progman", nullptr);
  if (!progman) return nullptr;

  DWORD_PTR ignored = 0;
  // Send 0x052C with 0, 0 to spawn the WorkerW behind desktop icons.
  // Note: Do not send 0x0000000D because 0x0000000D destroys WorkerW.
  SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &ignored);

  // Modern Windows 11 check (Build 22000+, 24H2 / 26xxx):
  // Progman contains SHELLDLL_DefView and WorkerW as direct children!
  HWND def_view = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
  if (def_view) {
    HWND worker = FindWindowExW(progman, def_view, L"WorkerW", nullptr);
    if (!worker) worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    if (worker && IsWindow(worker)) return worker;
  }

  // Windows 10 check:
  // SHELLDLL_DefView is inside a top-level window (WorkerW or Progman),
  // and the target WorkerW is the top-level sibling directly behind it.
  HWND host = nullptr;
  for (int attempt = 0; attempt < 6; ++attempt) {
    EnumWindows(find_workerw_after_desktop, reinterpret_cast<LPARAM>(&host));
    if (host && IsWindow(host)) return host;
    Sleep(50);
  }
  return host;
}

void apply_desktop_mode(App& app);

void enable_dark_titlebar(HWND hwnd) {
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark,
                        sizeof(dark));
}

bool is_desktop_styled(HWND hwnd) {
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  return (style & WS_CHILD) != 0 || (ex & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) != 0;
}

// Remembers where the ordinary window is, so leaving and re-entering window
// mode (and the next launch) puts it back where the user left it.
void remember_window_rect(App& app) {
  if (!app.hwnd || !IsWindowVisible(app.hwnd)) return;
  const LONG_PTR style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
  if ((style & WS_CHILD) != 0 || (style & WS_CAPTION) != WS_CAPTION) return;
  RECT r{};
  if (IsZoomed(app.hwnd) || IsIconic(app.hwnd)) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(app.hwnd, &placement)) return;
    r = placement.rcNormalPosition;
  } else if (!GetWindowRect(app.hwnd, &r)) {
    return;
  }
  if (r.right - r.left < 200 || r.bottom - r.top < 150) return;
  app.windowed_rect = r;
  app.cfg.win_x = r.left;
  app.cfg.win_y = r.top;
  app.cfg.win_w = r.right - r.left;
  app.cfg.win_h = r.bottom - r.top;
}

// A window rectangle that is guaranteed to be reachable: sensible size, and on
// a monitor that still exists.
RECT usable_window_rect(const App& app, const MONITORINFO& fallback_monitor) {
  RECT r = app.windowed_rect;
  bool ok = r.right - r.left >= 480 && r.bottom - r.top >= 320;
  if (ok) {
    HMONITOR monitor = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
      ok = false;
    } else {
      // The title bar must be on screen, or the window cannot be moved back.
      RECT title{r.left + 40, r.top, r.right - 40, r.top + 30};
      RECT overlap{};
      if (!IntersectRect(&overlap, &title, &info.rcWork)) ok = false;
      // A "window" that fills the monitor is a leftover full-screen rectangle.
      if (r.right - r.left >= info.rcMonitor.right - info.rcMonitor.left &&
          r.bottom - r.top >= info.rcMonitor.bottom - info.rcMonitor.top) {
        ok = false;
      }
    }
  }
  if (ok) return r;
  const RECT& work = fallback_monitor.rcWork;
  const int mon_w = work.right - work.left;
  const int mon_h = work.bottom - work.top;
  const int w = std::max(480, mon_w * 3 / 4);
  const int h = std::max(320, mon_h * 3 / 4);
  const int x = work.left + (mon_w - w) / 2;
  const int y = work.top + (mon_h - h) / 2;
  return RECT{x, y, x + w, y + h};
}

// Puts the window into the mode the config asks for. This is the only function
// that restyles, re-parents or repositions the main window.
//
// The two foreground modes used to be reached by patching styles on top of
// whatever the desktop layer had left behind (WS_CHILD of Explorer's WorkerW,
// click-through, never activated, no taskbar button). Leaving the desktop layer
// is now explicit and happens first, so "Full screen" really is a focused
// window in front of everything else.
void apply_window_mode(App& app, bool activate = true) {
  if (!app.hwnd) return;
  const DisplayMode mode = app.cfg.mode();
  const bool was_desktop = is_desktop_styled(app.hwnd);
  if (!was_desktop) remember_window_rect(app);
  app.windowed = mode == DisplayMode::Window;
  app.hidden_to_tray = false;
  // A cursor hidden by full-screen idling must not stay hidden elsewhere.
  app.view.cursor_hidden = false;
  app.view.last_mouse = app.now_ms();

  if (mode == DisplayMode::Desktop) {
    if (!was_desktop) {
      // Hide while the taskbar button and the styles change, so the window is
      // never seen half-converted in front of other applications.
      ShowWindow(app.hwnd, SW_HIDE);
      if (IsZoomed(app.hwnd) || IsIconic(app.hwnd)) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(app.hwnd, &placement)) {
          placement.showCmd = SW_HIDE;
          placement.flags = 0;
          SetWindowPlacement(app.hwnd, &placement);
        }
      }
      SetWindowLongPtrW(app.hwnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN);
    }
    apply_desktop_mode(app);
    ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);
    invalidate_window(app);
    return;
  }

  // Where should a foreground window appear? Coming from the desktop layer the
  // window spans every monitor, so use the one the user is working on.
  HMONITOR monitor = nullptr;
  if (was_desktop) {
    POINT cursor{};
    GetCursorPos(&cursor);
    monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  } else {
    monitor = MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST);
  }
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi)) {
    mi.rcMonitor = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    mi.rcWork = mi.rcMonitor;
  }

  if (was_desktop) {
    ShowWindow(app.hwnd, SW_HIDE);
    if (GetParent(app.hwnd) != nullptr ||
        (GetWindowLongPtrW(app.hwnd, GWL_STYLE) & WS_CHILD) != 0) {
      SetLastError(ERROR_SUCCESS);
      if (!SetParent(app.hwnd, nullptr) && GetLastError() != ERROR_SUCCESS) {
        logf("window: could not detach from the desktop layer (error %lu)",
             static_cast<unsigned long>(GetLastError()));
      }
    }
    app.desktop_parented = false;
    app.desktop_host = nullptr;
  } else if (IsZoomed(app.hwnd) || IsIconic(app.hwnd)) {
    // Styles are about to change; a maximised or minimised state would be
    // carried into the new mode and leave the window the wrong size.
    ShowWindow(app.hwnd, SW_RESTORE);
  }

  LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
  ex &= ~(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT);
  ex |= WS_EX_APPWINDOW;
  SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, ex);

  const LONG_PTR visible = GetWindowLongPtrW(app.hwnd, GWL_STYLE) & WS_VISIBLE;
  const HWND order = app.cfg.topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
  if (mode == DisplayMode::Fullscreen) {
    SetWindowLongPtrW(app.hwnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN | visible);
    SetWindowPos(app.hwnd, order, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
  } else {
    const RECT r = usable_window_rect(app, mi);
    SetWindowLongPtrW(app.hwnd, GWL_STYLE,
                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | visible);
    SetWindowPos(app.hwnd, order, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
    enable_dark_titlebar(app.hwnd);
  }

  ShowWindow(app.hwnd, activate ? SW_SHOW : SW_SHOWNA);
  if (activate) {
    // A foreground mode must actually take the keyboard, or F11/Esc and every
    // other key would go to whichever app was in front before.
    SetWindowPos(app.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(app.hwnd);
    SetFocus(app.hwnd);
  }
  logf("window: %s", mode == DisplayMode::Fullscreen ? "full screen" : "window");
  invalidate_window(app);
}

// Places the display in the actual WorkerW desktop layer, behind desktop icons
// but above the wallpaper. When WorkerW is unavailable, fall back to the bottom
// of the top-level z-order; the tray toggle remains available either way.
void apply_desktop_mode(App& app) {
  if (!app.hwnd) return;
  if (app.cfg.mode() != DisplayMode::Desktop) return;
  if (app.hidden_to_tray) return;  // hidden on purpose; do not re-show it
  // This runs again on every retry and display change; only a change of
  // outcome is worth a log line.
  static int last_logged = -1;
  {
    // A wallpaper is never topmost. WorkerW is a shell-owned desktop surface
    // below SHELLDLL_DefView (the icon view), not a transparent foreground HWND.
    if (!app.desktop_parented) {
      SetWindowPos(app.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    app.cfg.topmost = false;

    if (app.desktop_parented &&
        (!IsWindow(app.desktop_host) || GetParent(app.hwnd) != app.desktop_host)) {
      SetLastError(ERROR_SUCCESS);
      SetParent(app.hwnd, nullptr);
      app.desktop_parented = false;
      app.desktop_host = nullptr;
    }
    if (!app.desktop_parented) {
      const HWND host = find_desktop_host();
      if (host && IsWindow(host)) {
        // A child window must have WS_CHILD style before SetParent across processes.
        LONG_PTR child_style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
        child_style &= ~WS_POPUP;
        child_style |= WS_CHILD | WS_VISIBLE;
        SetWindowLongPtrW(app.hwnd, GWL_STYLE, child_style);

        SetLastError(ERROR_SUCCESS);
        const HWND previous = SetParent(app.hwnd, host);
        const DWORD error = GetLastError();
        if (previous || error == ERROR_SUCCESS || GetParent(app.hwnd) == host) {
          app.desktop_host = host;
          app.desktop_parented = true;
        } else {
          logf("window: SetParent to WorkerW failed (error %lu)",
               static_cast<unsigned long>(error));
        }
      }
    }

    LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
    ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT;
    ex &= ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, ex);

    if (app.desktop_parented && IsWindow(app.desktop_host)) {
      SetLayeredWindowAttributes(app.hwnd, 0, 255, LWA_ALPHA);
      LONG_PTR style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
      style &= ~WS_POPUP;
      style |= WS_CHILD | WS_VISIBLE;
      SetWindowLongPtrW(app.hwnd, GWL_STYLE, style);
      RECT r{};
      if (!GetClientRect(app.desktop_host, &r) || r.right - r.left < 8 ||
          r.bottom - r.top < 8) {
        logf("window: WorkerW has no usable client area");
        return;
      }
      // The WorkerW client rectangle is used instead of the primary monitor's
      // dimensions, so Explorer's virtual-desktop surface can cover all displays.
      SetWindowPos(app.hwnd, HWND_BOTTOM, 0, 0, r.right - r.left, r.bottom - r.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
      if (last_logged != 1) {
        last_logged = 1;
        logf("window: attached to desktop WorkerW %p (%ldx%ld)", app.desktop_host,
             r.right - r.left, r.bottom - r.top);
      }
    } else {
      // Safe fallback: transparent bottom-z-order layer so desktop icons are NOT obscured.
      LONG_PTR style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
      style &= ~WS_CHILD;
      style |= WS_POPUP | WS_VISIBLE;
      SetWindowLongPtrW(app.hwnd, GWL_STYLE, style);
      SetLayeredWindowAttributes(app.hwnd, RGB(16, 16, 24), 255, LWA_COLORKEY);

      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
      SetWindowPos(app.hwnd, HWND_BOTTOM, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
      if (last_logged != 0) {
        last_logged = 0;
        logf("window: WorkerW unavailable; safe transparent bottom-z-order fallback");
      }
    }
  }
}

void toggle_wallpaper_mode(App& app) {
  // Off returns to whichever foreground mode was last used.
  if (app.cfg.wallpaper) {
    app.set_display_mode(app.cfg.fullscreen ? DisplayMode::Fullscreen
                                            : DisplayMode::Window);
    app.show_toast("Desktop background off");
  } else {
    app.set_display_mode(DisplayMode::Desktop);
    app.show_toast("Desktop background on \xC2\xB7 Ctrl+Alt+W to switch off");
  }
}

// --------------------------------------------------------------- tray plumbing

void add_tray_icon(App& app) {
  if (!g_tray_icon) g_tray_icon = make_app_icon(32);
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  nid.hIcon = g_tray_icon ? g_tray_icon : LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(nid.szTip, L"Youtube-Lyrics");
  Shell_NotifyIconW(NIM_ADD, &nid);
  nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

// A balloon from the tray icon. Used for things that must be said while the
// main window is hidden and therefore cannot show a toast of its own.
void notify_from_tray(App& app, const wchar_t* title, const wchar_t* text) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
  wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(nid.szInfo, text, _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void remove_tray_icon(App& app) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

constexpr wchar_t kUpdateProgressClass[] = L"YoutubeLyricsUpdateProgress";
constexpr int kUpdateProgressBarId = 3101;
constexpr int kUpdateProgressLabelId = 3102;
constexpr int kUpdateProgressCancelId = 3103;

LRESULT CALLBACK update_progress_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam) {
  App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    app = static_cast<App*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    if (app) app->update_progress_hwnd = hwnd;
  }
  switch (msg) {
    case WM_COMMAND:
      if (LOWORD(wParam) == kUpdateProgressCancelId) {
        update_cancel_download();
        if (app && app->update_progress_label)
          SetWindowTextW(app->update_progress_label, L"Cancelling safely…");
        EnableWindow(reinterpret_cast<HWND>(lParam), FALSE);
        return 0;
      }
      break;
    case WM_CLOSE:
      update_cancel_download();
      if (app && app->update_progress_label)
        SetWindowTextW(app->update_progress_label, L"Cancelling safely…");
      return 0;
    case WM_NCDESTROY:
      if (app) {
        app->update_progress_hwnd = nullptr;
        app->update_progress_bar = nullptr;
        app->update_progress_label = nullptr;
      }
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void show_update_progress(App& app) {
  if (app.update_progress_hwnd) {
    ShowWindow(app.update_progress_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(app.update_progress_hwnd);
    return;
  }
  INITCOMMONCONTROLSEX controls{};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&controls);

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = update_progress_proc;
    wc.hInstance = app.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kUpdateProgressClass;
    registered = RegisterClassExW(&wc) != 0 ||
                 GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }
  if (!registered) return;

  const int width = 440, height = 164;
  const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
  const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
  HWND dialog = CreateWindowExW(
      WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW, kUpdateProgressClass,
      L"Updating Youtube-Lyrics", WS_CAPTION | WS_SYSMENU,
      x, y, width, height, nullptr, nullptr, app.inst, &app);
  if (!dialog) return;

  app.update_progress_label = CreateWindowExW(
      0, L"STATIC", L"Downloading and verifying the official release…",
      WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, 394, 24, dialog,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateProgressLabelId)),
      app.inst, nullptr);
  app.update_progress_bar = CreateWindowExW(
      0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
      20, 57, 394, 22, dialog,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateProgressBarId)),
      app.inst, nullptr);
  HWND cancel = CreateWindowExW(
      0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      width - 112, 96, 92, 28, dialog,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUpdateProgressCancelId)),
      app.inst, nullptr);
  if (app.update_progress_bar)
    SendMessageW(app.update_progress_bar, PBM_SETRANGE32, 0, 100);
  if (cancel) SetFocus(cancel);
  ShowWindow(dialog, SW_SHOWNORMAL);
  UpdateWindow(dialog);
  SetForegroundWindow(dialog);
}

void close_update_progress(App& app) {
  if (app.update_progress_hwnd) DestroyWindow(app.update_progress_hwnd);
}

void show_tray_menu(App& app) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, L"Show lyrics\tCtrl+Alt+L");
  AppendMenuW(menu, MF_STRING, 19, L"Settings…\tCtrl+Alt+S");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  // The three display modes are one choice, shown as one radio group.
  const DisplayMode mode = app.cfg.mode();
  AppendMenuW(menu, MF_STRING, 7,
              L"On the desktop, behind windows\tCtrl+Alt+W");
  AppendMenuW(menu, MF_STRING, 2, L"Full screen, in front\tCtrl+Alt+F");
  AppendMenuW(menu, MF_STRING, 20, L"In a window");
  CheckMenuRadioItem(menu, 0, 0, 0, MF_BYCOMMAND);
  const UINT mode_item = mode == DisplayMode::Desktop
                             ? 7u
                             : (mode == DisplayMode::Fullscreen ? 2u : 20u);
  MENUITEMINFOW radio{};
  radio.cbSize = sizeof(radio);
  radio.fMask = MIIM_FTYPE | MIIM_STATE;
  radio.fType = MFT_RADIOCHECK;
  radio.fState = MFS_CHECKED;
  SetMenuItemInfoW(menu, mode_item, FALSE, &radio);
  AppendMenuW(menu, MF_STRING | (app.cfg.topmost ? MF_CHECKED : 0) |
                        (mode == DisplayMode::Desktop ? MF_GRAYED : 0),
              3, L"Always on top\tA");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 13, L"Next cinematic wallpaper\tB");
  AppendMenuW(menu, MF_STRING | (app.cfg.wallpaper_auto_rotate ? MF_CHECKED : 0),
              14, L"Automatic wallpaper rotation");
  AppendMenuW(menu, MF_STRING, 15,
              (std::wstring(L"Rotation interval: ") +
               std::to_wstring(app.cfg.wallpaper_interval_minutes) + L" min").c_str());
  AppendMenuW(menu, MF_STRING, 16,
              (std::wstring(L"Background darkness: ") +
               std::to_wstring(app.cfg.wallpaper_dim) + L"%").c_str());
  AppendMenuW(menu, MF_STRING | (app.cfg.wallpaper_random_start ? MF_CHECKED : 0),
              17, L"Random wallpaper on launch");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 4, L"Reload lyrics\tR");
  AppendMenuW(menu, MF_STRING, 10, L"Reset sync offset");
  AppendMenuW(menu, MF_STRING | (app.view.help ? MF_CHECKED : 0), 5,
              L"Keyboard shortcuts\tF1");
  AppendMenuW(menu, MF_STRING, 8, L"Open local lyrics folder");
  AppendMenuW(menu, MF_STRING, 9, L"Clear automatic lyrics cache");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 18, L"Check for updates");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 11, L"About Youtube-Lyrics");
  AppendMenuW(menu, MF_STRING, 12, L"Help out via PayPal");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 6, L"Quit");
  SetMenuDefaultItem(menu, 1, FALSE);

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(app.hwnd);
  int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                           app.hwnd, nullptr);
  DestroyMenu(menu);

  switch (cmd) {
    case 1:
      app.show_window();
      break;
    case 2:
      app.set_display_mode(DisplayMode::Fullscreen);
      break;
    case 20:
      app.set_display_mode(DisplayMode::Window);
      break;
    case 3:
      app.toggle_topmost();
      break;
    case 7:
      app.set_display_mode(DisplayMode::Desktop);
      break;
    case 13:
      app.next_wallpaper();
      break;
    case 14:
      app.toggle_wallpaper_rotation();
      break;
    case 15:
      app.cycle_wallpaper_interval();
      break;
    case 16:
      app.cycle_wallpaper_dim();
      break;
    case 17:
      app.toggle_random_wallpaper_start();
      break;
    case 4:
      app.request_lyrics(app.current_track(), true);
      invalidate_window(app);
      break;
    case 5:
      app.toggle_help();
      break;
    case 8: {
      const std::wstring folder = widen(appdata_dir() + "\\lyrics");
      if (!CreateDirectoryW(folder.c_str(), nullptr) &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        app.show_toast("Could not create the local lyrics folder.");
      } else {
        const HINSTANCE result = ShellExecuteW(app.hwnd, L"open", folder.c_str(),
                                               nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
          app.show_toast("Could not open the local lyrics folder.");
      }
      break;
    }
    case 18:
      app.show_toast("Checking for updates…", 2200);
      update_request_check(app, true);
      break;
    case 19:
      app.open_settings();
      break;
    case 10:
      app.cfg.offset_ms = 0;
      app.cfg.save();
      app.show_toast("Sync offset reset");
      invalidate_window(app);
      break;
    case 9: {
      const int removed = lyrics_clear_cache();
      if (removed < 0) {
        app.show_toast("Could not clear the automatic lyrics cache.");
      } else if (removed == 0) {
        app.show_toast("Automatic lyrics cache is already empty.");
      } else {
        app.show_toast("Cleared " + std::to_string(removed) +
                       " cached sheets; press R to reload.");
      }
      break;
    }
    case 11: {
      const std::wstring about =
          std::wstring(L"Youtube-Lyrics ") + kAppVersion +
          L"\n\nA free, open-source Windows desktop lyrics display that follows "
          L"Windows media sessions.\n\n"
          L"No audio is recorded and no telemetry is sent. Lyric lookups use "
          L"LRCLIB and lyrics.ovh.\n\n"
          L"MIT licence\nProject: " + std::wstring(kProjectUrl) +
          L"\nHelp out: paypal.me/delinacre16";
      MessageBoxW(app.hwnd, about.c_str(), L"About Youtube-Lyrics",
                  MB_OK | MB_ICONINFORMATION);
      break;
    }
    case 12: {
      const HINSTANCE result = ShellExecuteW(app.hwnd, L"open", kSupportUrl,
                                             nullptr, nullptr, SW_SHOWNORMAL);
      if (reinterpret_cast<INT_PTR>(result) <= 32)
        app.show_toast("Could not open the PayPal support page.");
      break;
    }
    case 6:
      app.quitting = true;
      PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------------ frame loop

void on_timer(App& app) {
  const int64_t now = app.now_ms();
  if (app.hidden_to_tray) {
    app.view.last_frame = now;
    SetTimer(app.hwnd, kFrameTimer, 1000, nullptr);
    return;
  }

  // Explorer creates its WorkerW lazily, so a failed attach is retried: quickly
  // at first, then rarely. Each attempt blocks this thread for up to a second,
  // and retrying every two seconds for ever made the whole display stutter on
  // machines where the desktop layer is simply not available.
  static int64_t last_workerw_retry = 0;
  static int workerw_attempts = 0;
  if (app.cfg.mode() != DisplayMode::Desktop || app.desktop_parented) {
    workerw_attempts = 0;
  } else if (now - last_workerw_retry > (workerw_attempts < 5 ? 2000 : 60000)) {
    last_workerw_retry = now;
    ++workerw_attempts;
    apply_desktop_mode(app);
  }

  // Automatic second try after a network or provider failure.
  if (app.lyrics_retry_at != 0 && now >= app.lyrics_retry_at) {
    app.lyrics_retry_at = 0;
    if (app.lstate == LyricsState::NotFound && !app.active_track.title.empty()) {
      logf("lyrics: retrying after a transient failure");
      app.request_lyrics(app.active_track, true);
    }
  }

  // Rotation only advances while the desktop layer is actually in use. A timer
  // that kept cycling the saved index in window mode would silently change the
  // scene the user next sees on the desktop.
  const int64_t interval =
      static_cast<int64_t>(std::max(5, app.cfg.wallpaper_interval_minutes)) * 60 * 1000;
  if (app.view.wallpaper_changed_at == 0) app.view.wallpaper_changed_at = now;
  if (app.cfg.wallpaper && app.cfg.wallpaper_auto_rotate &&
      now - app.view.wallpaper_changed_at >= interval) {
    app.cfg.wallpaper_index = (app.cfg.wallpaper_index + 1) % kWallpaperCount;
    app.view.wallpaper_changed_at = now;
    app.view.bg_wallpaper = -1;
    app.cfg.save();
    settings_refresh(app);
    invalidate_window(app);
  }

  double dt = app.view.last_frame ? (now - app.view.last_frame) / 1000.0 : 0.016;
  app.view.last_frame = now;
  if (dt > 0.25) dt = 0.25;

  RECT rc{};
  GetClientRect(app.hwnd, &rc);
  const int h = rc.bottom - rc.top;
  const int64_t pos = app.current_pos_ms();
  const int new_active = (app.lyrics.synced || app.timing_estimated)
                             ? active_index(app.disp, pos)
                             : -1;
  // A seek or a new sheet moves the active line a long way; gliding there
  // through dozens of lines looks broken, so jump instead.
  const bool jumped = new_active != app.active &&
                      (app.active < 0 || new_active < 0 ||
                       std::abs(new_active - app.active) > 6);
  if (new_active != app.active) app.active = new_active;

  // Smooth line-to-line activity transitions. A wrapped line is several display
  // rows sharing one timestamp, and they light together as one sung line.
  bool line_animating = false;
  if (!app.disp.empty() && (app.lyrics.synced || app.timing_estimated)) {
    const int count = static_cast<int>(app.disp.size());
    int group_begin = app.active;
    if (group_begin >= 0 && group_begin < count) {
      while (group_begin > 0 && app.disp[group_begin].cont &&
             app.disp[group_begin - 1].start_ms == app.disp[group_begin].start_ms) {
        --group_begin;
      }
    }
    for (int i = 0; i < count; ++i) {
      bool in_active_group = i == app.active;
      if (!in_active_group && app.active >= 0 && i >= group_begin && i <= app.active &&
          app.disp[i].start_ms == app.disp[app.active].start_ms) {
        in_active_group = true;
      }
      const double target_act = in_active_group ? 1.0 : 0.0;
      double cur_act = 0.0;
      auto it = app.view.line_activity.find(i);
      if (it != app.view.line_activity.end()) cur_act = it->second;
      if (cur_act != target_act) {
        double next_act = ease_towards(cur_act, target_act, dt, 0.14);
        if (std::abs(next_act - target_act) < 0.005) next_act = target_act;
        if (next_act > 0.001) {
          app.view.line_activity[i] = next_act;
          if (next_act < 0.999) line_animating = true;
        } else {
          app.view.line_activity.erase(i);
        }
      }
    }
  }

  const double target = scroll_target(app, h);
  if (jumped && std::fabs(target - app.view.scroll) > h * 0.9) app.view.scroll = target;
  app.view.scroll = ease_towards(app.view.scroll, target, dt, 0.13);
  if (std::fabs(target - app.view.scroll) < 0.4) app.view.scroll = target;
  const bool animating = std::fabs(target - app.view.scroll) > 0.4 || line_animating;
  const bool lyric_progress = app.session_present && app.active_media.playing &&
                              app.lstate == LyricsState::Ready;
  // The idle "listening" ring breathes, and the start-up hint fades out.
  const bool idle_pulse = app.cfg.mode() != DisplayMode::Desktop &&
                          app.lstate == LyricsState::Idle;
  const bool rapid_frame = animating || app.view.toast_until > now || lyric_progress;

  // Static wallpaper idles at one cheap timer tick per second; only lyric motion,
  // progress, or a toast requests a repaint. Automatic rotation is independent.
  const bool minimized = IsIconic(app.hwnd) != 0;
  const UINT frame_interval = minimized ? 1000u : (rapid_frame ? 16u : (idle_pulse ? 66u : 500u));
  SetTimer(app.hwnd, kFrameTimer, frame_interval, nullptr);
  if (!minimized) invalidate_window(app);

  // Hide the pointer over an idle full-screen display. This is done through
  // WM_SETCURSOR rather than ShowCursor(), whose counter is per-thread and so
  // also hid the pointer over the settings window.
  if (!app.view.cursor_hidden && app.cfg.mode() == DisplayMode::Fullscreen &&
      !app.view.help && now - app.view.last_mouse > 2500 &&
      app.view.last_mouse > 0 && GetForegroundWindow() == app.hwnd) {
    app.view.cursor_hidden = true;
    POINT pt{};
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) == app.hwnd) SetCursor(nullptr);
  }
}

}  // namespace

// ---------------------------------------------------------------- log / files

void logf(const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(g_log_mu);
  std::string path = temp_log_path();
  if (path.empty()) return;
  FILE* f = _wfopen(widen(path).c_str(), L"a");
  if (!f) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(f, fmt, args);
  va_end(args);
  std::fputc('\n', f);
  const long size = std::ftell(f);
  std::fclose(f);
  // Keep the log bounded: one previous file, about a megabyte each.
  if (size > (1 << 20)) {
    const std::wstring current = widen(path);
    const std::wstring previous = widen(path + ".old");
    MoveFileExW(current.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
}

std::string exe_dir() {
  wchar_t path[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path);
  size_t slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos) p = p.substr(0, slash);
  return wide_to_utf8(p);
}

std::string appdata_dir() {
  static std::string cached;
  static std::once_flag once;
  std::call_once(once, []() {
    std::wstring dir;
    const wchar_t* env = _wgetenv(L"LOCALAPPDATA");
    if (env && *env) dir = env;
    if (dir.empty()) dir = widen(exe_dir());
    dir += L"\\YouTubeLyrics";
    CreateDirectoryW(dir.c_str(), nullptr);
    cached = wide_to_utf8(dir);
  });
  return cached;
}

std::string read_file(const std::string& path) {
  constexpr uint64_t kMaxFileBytes = 16u << 20;
  std::string out;
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ |
                                                          FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return out;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
      static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes) {
    CloseHandle(h);
    return out;
  }
  out.reserve(static_cast<size_t>(size.QuadPart));
  char buf[8192];
  DWORD got = 0;
  bool ok = true;
  for (;;) {
    got = 0;
    if (!ReadFile(h, buf, sizeof(buf), &got, nullptr)) {
      ok = false;
      break;
    }
    if (got == 0) break;
    out.append(buf, got);
    if (out.size() > kMaxFileBytes) {
      ok = false;
      break;
    }
  }
  CloseHandle(h);
  if (!ok) out.clear();
  return out;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
  if (data.size() > MAXDWORD) return false;
  const std::string tmp = path + ".tmp";
  const std::wstring tmp_wide = widen(tmp);
  const std::wstring path_wide = widen(path);
  HANDLE h = CreateFileW(tmp_wide.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written,
                      nullptr) != 0 && written == data.size();
  if (ok) ok = FlushFileBuffers(h) != 0;
  CloseHandle(h);
  if (!ok) {
    DeleteFileW(tmp_wide.c_str());
    return false;
  }
  if (!MoveFileExW(tmp_wide.c_str(), path_wide.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp_wide.c_str());
    return false;
  }
  return true;
}

std::string fnv_hex(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

// ------------------------------------------------------------------- config

namespace {

bool parse_config_integer(const std::string& value, int64_t* result) {
  if (value.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(value.c_str(), &end, 10);
  if (errno == ERANGE || end == value.c_str() || !end || *end != '\0') return false;
  *result = static_cast<int64_t>(parsed);
  return true;
}

bool parse_config_real(const std::string& value, double* result) {
  if (value.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (errno == ERANGE || end == value.c_str() || !end || *end != '\0' ||
      !std::isfinite(parsed)) return false;
  *result = parsed;
  return true;
}

}  // namespace

void Config::load() {
  std::string data = read_file(appdata_dir() + "\\config.ini");
  size_t pos = 0;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    std::string line = data.substr(pos, nl - pos);
    pos = nl + 1;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq));
    std::string v = trim(line.substr(eq + 1));
    int64_t integer = 0;
    if (k == "version" && parse_config_integer(v, &integer) &&
        integer >= 1 && integer <= 100) {
      version = static_cast<int>(integer);
    } else if (k == "offset_ms" && parse_config_integer(v, &integer)) {
      offset_ms = static_cast<int>(std::max<int64_t>(-60000,
                                                     std::min<int64_t>(60000, integer)));
    } else if (k == "font_scale") {
      double real = 0.0;
      if (parse_config_real(v, &real)) font_scale = real;
    } else if (k == "topmost") topmost = (v == "1");
    else if (k == "cover") cover = (v == "1");
    else if (k == "wipe") wipe = (v == "1");
    else if (k == "theme" && parse_config_integer(v, &integer)) {
      theme = (integer >= 0 && integer < 4) ? static_cast<int>(integer) : 0;
    } else if (k == "fullscreen") fullscreen = (v == "1");
    else if (k == "wallpaper") wallpaper = (v == "1");
    else if (k == "wallpaper_index" && parse_config_integer(v, &integer))
      wallpaper_index = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(kWallpaperCount - 1, integer)));
    else if (k == "wallpaper_random_start") wallpaper_random_start = (v == "1");
    else if (k == "wallpaper_auto_rotate") wallpaper_auto_rotate = (v == "1");
    else if (k == "wallpaper_interval_minutes" && parse_config_integer(v, &integer))
      wallpaper_interval_minutes = static_cast<int>(std::max<int64_t>(5, std::min<int64_t>(240, integer)));
    else if (k == "wallpaper_dim" && parse_config_integer(v, &integer))
      wallpaper_dim = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(60, integer)));
    else if (k == "win_x" && parse_config_integer(v, &integer))
      win_x = static_cast<int>(std::max<int64_t>(-100000, std::min<int64_t>(100000, integer)));
    else if (k == "win_y" && parse_config_integer(v, &integer))
      win_y = static_cast<int>(std::max<int64_t>(-100000, std::min<int64_t>(100000, integer)));
    else if (k == "win_w" && parse_config_integer(v, &integer))
      win_w = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(100000, integer)));
    else if (k == "win_h" && parse_config_integer(v, &integer))
      win_h = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(100000, integer)));
  }
  if (!std::isfinite(font_scale) || font_scale < 0.55 || font_scale > 2.2)
    font_scale = 1.0;
  if (theme < 0 || theme >= 4) theme = 0;
  if (wallpaper_index < 0 || wallpaper_index >= kWallpaperCount) wallpaper_index = 0;
  if (wallpaper_interval_minutes != 15 && wallpaper_interval_minutes != 30 &&
      wallpaper_interval_minutes != 60) wallpaper_interval_minutes = 30;
  if (wallpaper_dim < 0 || wallpaper_dim > 60) wallpaper_dim = 12;
  // Settings files written before 1.6.0 predate the larger default text and
  // the "wrap instead of shrink" layout. A size someone turned up to fight the
  // old tiny text would now be enormous, so it is brought back into range once.
  if (version < 2) {
    if (font_scale > 1.3) font_scale = 1.3;
    version = 2;
  }
  // `fullscreen` is deliberately left alone while `wallpaper` is on: it is the
  // foreground mode to return to when the desktop layer is switched off.
}
void Config::save() const {
  char buf[512];
  std::string data = "version=" + std::to_string(version) + "\n";
  data += "offset_ms=" + std::to_string(offset_ms) + "\n";
  std::snprintf(buf, sizeof(buf), "font_scale=%.3f\n", font_scale);
  data += buf;
  data += std::string("topmost=") + (topmost ? "1" : "0") + "\n";
  data += std::string("cover=") + (cover ? "1" : "0") + "\n";
  data += std::string("wipe=") + (wipe ? "1" : "0") + "\n";
  data += "theme=" + std::to_string(theme) + "\n";
  data += std::string("fullscreen=") + (fullscreen ? "1" : "0") + "\n";
  data += std::string("wallpaper=") + (wallpaper ? "1" : "0") + "\n";
  data += "wallpaper_index=" + std::to_string(wallpaper_index) + "\n";
  data += std::string("wallpaper_random_start=") + (wallpaper_random_start ? "1" : "0") + "\n";
  data += std::string("wallpaper_auto_rotate=") + (wallpaper_auto_rotate ? "1" : "0") + "\n";
  data += "wallpaper_interval_minutes=" + std::to_string(wallpaper_interval_minutes) + "\n";
  data += "wallpaper_dim=" + std::to_string(wallpaper_dim) + "\n";
  data += "win_x=" + std::to_string(win_x) + "\n";
  data += "win_y=" + std::to_string(win_y) + "\n";
  data += "win_w=" + std::to_string(win_w) + "\n";
  data += "win_h=" + std::to_string(win_h) + "\n";
  write_file_atomic(appdata_dir() + "\\config.ini", data);
}

// ----------------------------------------------------------------- App methods

int64_t App::current_pos_ms() const {
  const MediaState& m = active_media;  // committed UI-thread snapshot
  const double duration_s = current_duration_s();
  const int64_t duration_ms = duration_s > 0.0
                                  ? static_cast<int64_t>(duration_s * 1000.0)
                                  : 0;
  int64_t pos = interpolate_pos(m.pos_ms, m.at_ms, now_ms(), m.playing,
                                duration_ms);
  if (cfg.offset_ms > 0 &&
      pos > std::numeric_limits<int64_t>::max() - cfg.offset_ms) {
    pos = std::numeric_limits<int64_t>::max();
  } else {
    pos += cfg.offset_ms;
  }
  if (pos < 0) pos = 0;
  if (duration_ms > 0 && pos > duration_ms) pos = duration_ms;
  return pos;
}

double App::current_duration_s() const {
  const double duration = active_media.duration_s;
  if (!std::isfinite(duration) || duration <= 0.0) return 0.0;
  return std::min(duration, 24.0 * 60.0 * 60.0);
}

TrackInfo App::current_track() const { return active_track; }

void App::request_lyrics(const TrackInfo& track, bool force) {
  if (track.title.empty()) return;
  lyrics_req_id++;
  lstate = LyricsState::Loading;
  lmessage.clear();
  lyrics_enqueue(track, lyrics_req_id, force);
  invalidate_window(*this);
}

void App::rebuild_layout(bool force) {
  RECT rc{};
  if (hwnd) GetClientRect(hwnd, &rc);
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w < 10 || h < 10) {
    w = 1280;
    h = 800;
  }
  pending_plain_lines.clear();
  timing_estimated = false;
  if (lstate == LyricsState::Ready && !lyrics.synced) {
    // Untimed lyrics may be paced for navigation, but these estimates never
    // change Lyrics::synced and never drive a karaoke wipe.
    const double duration = current_duration_s();
    size_t row_count = 1;
    for (char c : lyrics.plain) {
      if (c == '\n') ++row_count;
    }
    int64_t per_line = 4000;
    if (std::isfinite(duration) && duration > 20) {
      const double bounded_duration = std::min(duration, 24.0 * 60.0 * 60.0);
      const double estimated = bounded_duration * 1000.0 * 0.92 /
                               static_cast<double>(std::max<size_t>(1, row_count));
      per_line = static_cast<int64_t>(estimated);
      per_line = std::max<int64_t>(1500, std::min<int64_t>(9000, per_line));
    }
    pending_plain_lines = pace_plain_lyrics(lyrics.plain, 2000, per_line);
    timing_estimated = !pending_plain_lines.empty();
  }
  relayout(*this, w, h);
  if (!lyrics.synced && !timing_estimated) active = -1;
  (void)force;
  view.bg_theme = -1;  // force background rebuild (theme/art may have changed)
}

void App::relayout_if_needed() { rebuild_layout(false); }

void App::invalidate() { invalidate_window(*this); }

void App::set_display_mode(DisplayMode mode) {
  const DisplayMode before = cfg.mode();
  cfg.wallpaper = mode == DisplayMode::Desktop;
  if (mode == DisplayMode::Fullscreen) cfg.fullscreen = true;
  if (mode == DisplayMode::Window) cfg.fullscreen = false;
  if (cfg.wallpaper) cfg.topmost = false;
  apply_window_mode(*this);
  cfg.save();
  rebuild_layout(true);
  settings_refresh(*this);
  // Settings stays usable on top of a full-screen display it just created.
  if (settings_window && IsWindow(settings_window) && IsWindowVisible(settings_window)) {
    SetWindowPos(settings_window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(settings_window);
  }
  if (before != mode) {
    // Says where the lyrics went, and how to get out again: the desktop layer
    // has no keyboard, so its way back has to be a global shortcut.
    view.hint_until = now_ms() + 6000;
  }
  invalidate();
}

// F11, double-click and Ctrl+Alt+F: full screen from anywhere, and back to a
// window from full screen. It no longer "toggles" into the desktop layer,
// which looked exactly like the window vanishing.
void App::toggle_fullscreen() {
  set_display_mode(cfg.mode() == DisplayMode::Fullscreen ? DisplayMode::Window
                                                         : DisplayMode::Fullscreen);
}

void App::apply_display_mode() { set_display_mode(cfg.mode()); }

void App::open_settings() { settings_show(*this); }

void App::toggle_topmost() {
  if (cfg.wallpaper && !windowed) {
    show_toast("Always-on-top is unavailable in desktop mode");
    settings_refresh(*this);
    return;
  }
  cfg.topmost = !cfg.topmost;
  cfg.save();
  SetWindowPos(hwnd, cfg.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE);
  show_toast(cfg.topmost ? "Always on top: on" : "Always on top: off");
  invalidate();
  settings_refresh(*this);
}

void App::toggle_theme() {
  cfg.theme = (cfg.theme + 1) % 4;
  cfg.save();
  view.bg_theme = -1;
  std::string name = wide_to_utf8(theme_name(cfg.theme));
  show_toast("Theme: " + name);
  invalidate();
  settings_refresh(*this);
}

void App::next_wallpaper() {
  cfg.wallpaper_index = (cfg.wallpaper_index + 1) % kWallpaperCount;
  cfg.save();
  view.wallpaper_changed_at = now_ms();
  view.bg_wallpaper = -1;
  show_toast("Wallpaper: " + wide_to_utf8(wallpaper_name(cfg.wallpaper_index)));
  invalidate();
  settings_refresh(*this);
}

void App::toggle_wallpaper_rotation() {
  cfg.wallpaper_auto_rotate = !cfg.wallpaper_auto_rotate;
  view.wallpaper_changed_at = now_ms();
  cfg.save();
  show_toast(cfg.wallpaper_auto_rotate ? "Wallpaper rotation: on"
                                       : "Wallpaper rotation: off");
  settings_refresh(*this);
}

void App::cycle_wallpaper_interval() {
  if (cfg.wallpaper_interval_minutes == 15)
    cfg.wallpaper_interval_minutes = 30;
  else if (cfg.wallpaper_interval_minutes == 30)
    cfg.wallpaper_interval_minutes = 60;
  else
    cfg.wallpaper_interval_minutes = 15;
  view.wallpaper_changed_at = now_ms();
  cfg.save();
  show_toast("Wallpaper changes every " +
             std::to_string(cfg.wallpaper_interval_minutes) + " minutes");
  settings_refresh(*this);
}

void App::cycle_wallpaper_dim() {
  if (cfg.wallpaper_dim == 0)
    cfg.wallpaper_dim = 12;
  else if (cfg.wallpaper_dim == 12)
    cfg.wallpaper_dim = 24;
  else if (cfg.wallpaper_dim == 24)
    cfg.wallpaper_dim = 36;
  else
    cfg.wallpaper_dim = 0;
  cfg.save();
  view.bg_dim = -1;
  show_toast("Background darkness: " + std::to_string(cfg.wallpaper_dim) + "%");
  invalidate();
  settings_refresh(*this);
}

void App::toggle_random_wallpaper_start() {
  cfg.wallpaper_random_start = !cfg.wallpaper_random_start;
  cfg.save();
  show_toast(cfg.wallpaper_random_start ? "Random wallpaper on launch: on"
                                        : "Random wallpaper on launch: off");
  settings_refresh(*this);
}

void App::nudge_offset(int delta_ms) {
  cfg.offset_ms += delta_ms;
  if (cfg.offset_ms > 60000) cfg.offset_ms = 60000;
  if (cfg.offset_ms < -60000) cfg.offset_ms = -60000;
  cfg.save();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Sync %+.2f s", cfg.offset_ms / 1000.0);
  show_toast(buf);
  invalidate();
  settings_refresh(*this);
}

void App::set_font_scale(double scale) {
  if (scale < 0.6) scale = 0.6;
  if (scale > 2.0) scale = 2.0;
  cfg.font_scale = scale;
  cfg.save();
  rebuild_layout(true);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Text size %.0f%%",
                (cfg.font_scale - 1.0) * 100.0 + 100.0);
  show_toast(buf);
  invalidate();
  settings_refresh(*this);
}

void App::show_toast(const std::string& text, int ms) {
  if (!hwnd || quitting) return;
  view.toast = text;
  view.toast_until = now_ms() + ms;
  invalidate();
}

void App::toggle_help() {
  view.help = !view.help;
  invalidate();
}

void App::show_window() {
  hidden_to_tray = false;
  if (hwnd) SetTimer(hwnd, kFrameTimer, 16, nullptr);
  apply_window_mode(*this);
  invalidate();
}

void App::hide_to_tray() {
  if (cfg.mode() != DisplayMode::Desktop) remember_window_rect(*this);
  hidden_to_tray = true;
  ShowWindow(hwnd, SW_HIDE);
  cfg.save();
  // The window is gone, so a toast inside it would never be seen. Say it from
  // the tray instead, once per run: closing the window does not quit the app.
  if (!tray_hint_shown && !quitting) {
    tray_hint_shown = true;
    notify_from_tray(*this, L"Youtube-Lyrics is still running",
                     L"Click this icon or press Ctrl+Alt+L to bring the lyrics "
                     L"back. Right-click it for settings or to quit.");
  }
}

void App::create_tray_icon() { add_tray_icon(*this); }

void App::destroy_view_resources() {
  if (settings_window && IsWindow(settings_window)) DestroyWindow(settings_window);
  settings_window = nullptr;
  if (view.mem_dc) {
    SelectObject(view.mem_dc, view.mem_old);
    DeleteObject(view.mem_bmp);
    DeleteDC(view.mem_dc);
    view.mem_dc = nullptr;
    view.mem_bmp = nullptr;
  }
  delete view.bg_cached;
  view.bg_cached = nullptr;
  delete view.bg;
  view.bg = nullptr;
  delete view.art;
  view.art = nullptr;
  delete view.wallpaper_image;
  view.wallpaper_image = nullptr;
  if (view.wallpaper_stream) {
    view.wallpaper_stream->Release();
    view.wallpaper_stream = nullptr;
  }
  for (auto& entry : view.fonts) delete entry.second;
  view.fonts.clear();
}

// ------------------------------------------------------------------ painting

void App::paint(HDC dc) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return;

  if (!view.mem_dc || view.mem_w != w || view.mem_h != h) {
    if (view.mem_dc) {
      SelectObject(view.mem_dc, view.mem_old);
      DeleteObject(view.mem_bmp);
      DeleteDC(view.mem_dc);
      view.mem_dc = nullptr;
      view.mem_bmp = nullptr;
    }
    view.mem_dc = CreateCompatibleDC(dc);
    view.mem_bmp = CreateCompatibleBitmap(dc, w, h);
    if (!view.mem_dc || !view.mem_bmp) return;
    view.mem_old = SelectObject(view.mem_dc, view.mem_bmp);
    view.mem_w = w;
    view.mem_h = h;
  }

  {
    Gdiplus::Graphics g(view.mem_dc);
    render_frame(*this, g, w, h);
  }
  BitBlt(dc, 0, 0, w, h, view.mem_dc, 0, 0, SRCCOPY);
}

void App::on_resize(int w, int h) { relayout(*this, w, h); }

// ---------------------------------------------------------------- window proc

namespace {

void handle_key(App& app, WPARAM key) {
  switch (key) {
    case VK_F11:
      app.toggle_fullscreen();
      break;
    case VK_ESCAPE:
      // Esc closes the innermost thing: the help panel, then full screen. It
      // never hides an ordinary window: that looked like the app had died.
      if (app.view.help) {
        app.toggle_help();
      } else if (app.cfg.mode() == DisplayMode::Fullscreen) {
        app.set_display_mode(DisplayMode::Window);
      }
      break;
    case 'A':
      app.toggle_topmost();
      break;
    case 'W':
      toggle_wallpaper_mode(app);
      break;
    case 'T':
      app.toggle_theme();
      break;
    case 'B':
      app.next_wallpaper();
      break;
    case 'C':
      app.cfg.cover = !app.cfg.cover;
      app.cfg.save();
      app.view.bg_theme = -1;
      refresh_art(app);
      app.show_toast(app.cfg.cover ? "Subtle cover-art tint: on"
                                   : "Subtle cover-art tint: off");
      app.invalidate();
      break;
    case 'K':
      app.cfg.wipe = !app.cfg.wipe;
      app.cfg.save();
      app.show_toast(app.cfg.wipe ? "Karaoke wipe: on" : "Karaoke wipe: off");
      app.invalidate();
      break;
    case 'R':
      app.request_lyrics(app.current_track(), true);
      app.show_toast("Searching for lyrics…");
      break;
    case 'F':
      app.cfg.font_scale = 1.0;
      app.set_font_scale(1.0);
      break;
    case VK_UP:
      app.set_font_scale(app.cfg.font_scale + 0.07);
      break;
    case VK_DOWN:
      app.set_font_scale(app.cfg.font_scale - 0.07);
      break;
    case VK_LEFT:
      app.nudge_offset(-250);
      break;
    case VK_RIGHT:
      app.nudge_offset(250);
      break;
    case VK_F1:
    case 'H':
      app.toggle_help();
      break;
    case 'S':
      app.open_settings();
      break;
    case VK_OEM_PLUS:
    case VK_ADD:
      app.set_font_scale(app.cfg.font_scale + 0.07);
      break;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
      app.set_font_scale(app.cfg.font_scale - 0.07);
      break;
    case VK_OEM_4:  // '[' — quieter background
      app.cycle_wallpaper_dim();
      break;
    case 'Q':
      app.quitting = true;
      PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
      break;
    default:
      break;
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  App* app = g_app;
  if (!app) return DefWindowProcW(hwnd, msg, wParam, lParam);

  if (msg == app->taskbar_created_msg && app->taskbar_created_msg != 0) {
    // Explorer restarted: the tray icon belonged to the old shell, and any
    // WorkerW parenting is gone with it.
    logf("shell: TaskbarCreated, restoring tray icon and desktop layer");
    add_tray_icon(*app);
    if (app->cfg.mode() == DisplayMode::Desktop) {
      app->desktop_parented = false;
      app->desktop_host = nullptr;
      apply_desktop_mode(*app);
    }
    return 0;
  }
  if (msg == WM_APP_SHOW_EXISTING) {
    // A second launch. Handled here rather than in the message loop so it also
    // arrives while a menu or message box is running its own loop.
    app->quitting = false;
    app->show_window();
    // On the desktop layer there is nothing to bring to the front, and a
    // launch that appears to do nothing is the worst answer: open Settings.
    if (app->cfg.mode() == DisplayMode::Desktop) app->open_settings();
    return 0;
  }

  switch (msg) {
    case WM_NCHITTEST:
      if (app->cfg.wallpaper && !app->windowed) return HTTRANSPARENT;
      break;
    case WM_MOUSEACTIVATE:
      if (app->cfg.wallpaper && !app->windowed) return MA_NOACTIVATE;
      break;
    case WM_CREATE:
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      app->paint(dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) {
        app->on_resize(LOWORD(lParam), HIWORD(lParam));
        // Keep the active line where it was instead of sliding to it again.
        app->view.scroll = scroll_target(*app, HIWORD(lParam));
        invalidate_window(*app);
      }
      return 0;
    case WM_EXITSIZEMOVE:
      if (app->cfg.mode() == DisplayMode::Window) {
        remember_window_rect(*app);
        app->cfg.save();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lParam) == HTCLIENT && app->view.cursor_hidden &&
          app->cfg.mode() == DisplayMode::Fullscreen) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    case WM_DPICHANGED: {
      const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
      if (suggested && app->windowed) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      } else if (app->cfg.mode() == DisplayMode::Desktop) {
        apply_desktop_mode(*app);
      } else if (!app->windowed) {
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
          SetWindowPos(hwnd, nullptr, info.rcMonitor.left, info.rcMonitor.top,
                       info.rcMonitor.right - info.rcMonitor.left,
                       info.rcMonitor.bottom - info.rcMonitor.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
      app->view.bg_theme = -1;
      app->rebuild_layout(true);
      invalidate_window(*app);
      return 0;
    }
    case WM_DISPLAYCHANGE:
      if (app->hidden_to_tray) {
        // Nothing to move while hidden; show_window() re-applies the mode.
      } else if (app->cfg.mode() == DisplayMode::Desktop) {
        apply_desktop_mode(*app);
      } else if (app->cfg.mode() == DisplayMode::Fullscreen) {
        // Stay on the monitor the window is on; only snap to its new size.
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
          SetWindowPos(hwnd, nullptr, info.rcMonitor.left, info.rcMonitor.top,
                       info.rcMonitor.right - info.rcMonitor.left,
                       info.rcMonitor.bottom - info.rcMonitor.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
      } else if (!IsIconic(hwnd) && !IsZoomed(hwnd)) {
        // A monitor was unplugged: make sure the window is still reachable.
        RECT now_rect{};
        GetWindowRect(hwnd, &now_rect);
        app->windowed_rect = now_rect;
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info);
        const RECT r = usable_window_rect(*app, info);
        if (!EqualRect(&r, &now_rect)) {
          SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left,
                       r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
      app->view.bg_theme = -1;
      app->rebuild_layout(true);
      invalidate_window(*app);
      return 0;
    case WM_GETMINMAXINFO: {
      MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      mmi->ptMinTrackSize.x = 480;
      mmi->ptMinTrackSize.y = 320;
      return 0;
    }
    case WM_TIMER:
      if (wParam == kFrameTimer) {
        on_timer(*app);
        return 0;
      }
      break;
    case WM_APP_MEDIA:
      on_media(*app);
      return 0;
    case WM_APP_LYRICS:
      on_lyrics(*app);
      return 0;
    case WM_APP_UPDATE_AVAILABLE: {
      std::unique_ptr<UpdateRelease> release(
          reinterpret_cast<UpdateRelease*>(lParam));
      if (!release) return 0;
      const bool manual_check = wParam != 0;
      if (!manual_check && app->update_prompted_version == release->version) return 0;
      app->update_prompted_version = release->version;
      const std::wstring prompt =
          L"A new version of Youtube-Lyrics is available!\n\n"
          L"Installed version: " + widen(YTL_VERSION_STRING) +
          L"\nAvailable version: " + widen(release->version) +
          L"\n\nDownload and install this official release now?";
      const int answer = MessageBoxW(nullptr, prompt.c_str(),
                                     L"Youtube-Lyrics update",
                                     MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND);
      if (answer == IDYES) {
        show_update_progress(*app);
        update_request_download(*app, *release);
      }
      return 0;
    }
    case WM_APP_UPDATE_PROGRESS:
      if (app->update_progress_bar && app->update_progress_label) {
        if (wParam == 1000) {
          SendMessageW(app->update_progress_bar, PBM_SETMARQUEE, TRUE, 35);
          SetWindowTextW(app->update_progress_label,
                         L"Connecting securely to the official release…");
        } else {
          SendMessageW(app->update_progress_bar, PBM_SETMARQUEE, FALSE, 0);
          SendMessageW(app->update_progress_bar, PBM_SETPOS,
                       std::min<WPARAM>(100, wParam), 0);
          const std::wstring label = L"Downloading and verifying update… " +
                                     std::to_wstring(std::min<WPARAM>(100, wParam)) + L"%";
          SetWindowTextW(app->update_progress_label, label.c_str());
        }
      }
      return 0;
    case WM_APP_UPDATE_READY: {
      std::unique_ptr<UpdateStage> stage(reinterpret_cast<UpdateStage*>(lParam));
      close_update_progress(*app);
      if (!stage) return 0;
      std::string error;
      if (update_apply_staged(*app, *stage, &error)) {
        app->quitting = true;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
      } else {
        MessageBoxW(nullptr, widen(error).c_str(), L"Update could not be installed",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
      }
      return 0;
    }
    case WM_APP_UPDATE_ERROR: {
      std::unique_ptr<std::string> error(reinterpret_cast<std::string*>(lParam));
      close_update_progress(*app);
      if (error && wParam)
        MessageBoxW(nullptr, widen(*error).c_str(), L"Youtube-Lyrics update",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
      return 0;
    }
    case WM_APP_UPDATE_NONE:
      if (wParam) app->show_toast("You're using the latest stable version.", 2800);
      return 0;
    case WM_HOTKEY:
      if (wParam == kHotkeyToggle) {
        if (app->hidden_to_tray || !IsWindowVisible(hwnd)) {
          app->show_window();
        } else {
          app->hide_to_tray();
        }
      } else if (wParam == kHotkeyWallpaper) {
        toggle_wallpaper_mode(*app);
      } else if (wParam == kHotkeyFullscreen) {
        // Works from the desktop layer too, where the window has no keyboard.
        if (app->hidden_to_tray) app->hidden_to_tray = false;
        app->toggle_fullscreen();
      } else if (wParam == kHotkeySettings) {
        app->open_settings();
      }
      return 0;
    case WM_APP_TRAY: {
      UINT event = LOWORD(lParam);
      if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
        if (app->hidden_to_tray || !IsWindowVisible(hwnd)) {
          app->show_window();
        } else {
          app->hide_to_tray();
        }
      } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
        show_tray_menu(*app);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      // Windows also sends this when a window appears under a still pointer;
      // only real movement counts as the user being there.
      const POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
      if (pt.x == app->view.last_mouse_pt.x && pt.y == app->view.last_mouse_pt.y) return 0;
      app->view.last_mouse_pt = pt;
      app->view.last_mouse = app->now_ms();
      if (app->view.cursor_hidden) {
        app->view.cursor_hidden = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      }
      return 0;
    }
    case WM_KEYDOWN:
      handle_key(*app, wParam);
      return 0;
    case WM_LBUTTONDBLCLK:
      app->toggle_fullscreen();
      return 0;
    case WM_SYSCOMMAND:
      if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;  // no menu beep on Alt
      break;
    case WM_CLOSE:
      if (app->cfg.mode() == DisplayMode::Window) remember_window_rect(*app);
      if (!app->quitting) {
        app->hide_to_tray();
        return 0;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      UnregisterHotKey(hwnd, kHotkeyToggle);
      UnregisterHotKey(hwnd, kHotkeyWallpaper);
      UnregisterHotKey(hwnd, kHotkeySettings);
      UnregisterHotKey(hwnd, kHotkeyFullscreen);
      close_update_progress(*app);
      update_stop();
      remove_tray_icon(*app);
      PostQuitMessage(0);
      return 0;
    case WM_POWERBROADCAST:
      if (wParam == PBT_APMSUSPEND) {
        // Freeze the clock before the machine sleeps; the wall time between
        // suspend and resume must not be counted as playback.
        app->cfg.save();
        return TRUE;
      }
      if (wParam == PBT_APMRESUMEAUTOMATIC ||
          wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMECRITICAL) {
        MediaState& media = app->active_media;
        const int64_t now = app->now_ms();
        if (media.have) {
          const double duration = app->current_duration_s();
          const int64_t duration_ms = duration > 0.0
                                          ? static_cast<int64_t>(duration * 1000.0)
                                          : 0;
          const int64_t frozen_at = std::max(media.at_ms, app->view.last_frame);
          media.pos_ms = interpolate_pos(media.pos_ms, media.at_ms, frozen_at,
                                         media.playing, duration_ms);
          media.at_ms = now;
          media.playing = false;
        }
        app->view.last_frame = now;
        invalidate_window(*app);
      }
      return TRUE;
    case WM_ENDSESSION:
      // Windows is ending the session: save now, and let the normal teardown
      // path in wWinMain stop the worker threads.
      if (wParam) {
        app->quitting = true;
        app->cfg.save();
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void set_dpi_awareness() {
  using Fn = BOOL(WINAPI*)(HANDLE);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32) {
    auto set_ctx = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (set_ctx && set_ctx(reinterpret_cast<HANDLE>(-4))) return;  // PER_MONITOR_AWARE_V2
  }
  SetProcessDPIAware();
}

}  // namespace

}  // namespace ytl

// ------------------------------------------------------------------- WinMain

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, PWSTR command_line, int show_cmd) {
  using namespace ytl;

  std::wstring launch_flag = command_line ? command_line : L"";
  while (!launch_flag.empty() && (launch_flag.front() == L' ' || launch_flag.front() == L'\t'))
    launch_flag.erase(launch_flag.begin());
  while (!launch_flag.empty() && (launch_flag.back() == L' ' || launch_flag.back() == L'\t'))
    launch_flag.pop_back();
  const bool launched_after_update = launch_flag == L"--updated";
  const bool launched_after_update_failure = launch_flag == L"--update-failed";

  set_dpi_awareness();

  GdiplusStartupInput gdi_input;
  ULONG_PTR gdi_token = 0;
  GdiplusStartup(&gdi_token, &gdi_input, nullptr);

  App app;
  app.inst = inst;
  g_app = &app;

  // Single instance: ask the running copy to show itself instead of starting a
  // second one. SetLastError is cleared first because CreateMutexW only sets it
  // when the named object already exists, and a stale code would make a healthy
  // first launch think it was the second.
  SetLastError(ERROR_SUCCESS);
  app.single_instance =
      CreateMutexW(nullptr, FALSE, L"Local\\YoutubeLyrics_SingleInstance_v1");
  if (app.single_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = FindWindowW(kClassName, nullptr);
    if (existing) {
      // Hand our right to take the foreground to the running copy, so it can
      // actually come to the front instead of flashing in the taskbar.
      DWORD running_pid = 0;
      GetWindowThreadProcessId(existing, &running_pid);
      if (running_pid) AllowSetForegroundWindow(running_pid);
      // The running instance decides whether it may activate; in wallpaper mode
      // a second launch must not pull it above the desktop layer.
      PostMessageW(existing, WM_APP_SHOW_EXISTING, 0, 0);
    } else {
      // The class exists in no window: a previous process died without cleaning
      // up. Carry on rather than silently doing nothing.
      logf("startup: stale single-instance mutex, continuing");
    }
    if (app.single_instance) CloseHandle(app.single_instance);
    GdiplusShutdown(gdi_token);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // A config file that does not exist yet means this is the first launch: the
  // idle screen then explains itself once instead of only saying "Listening".
  app.first_run = read_file(appdata_dir() + "\\config.ini").empty();
  app.cfg.load();
  if (app.cfg.wallpaper_random_start && kWallpaperCount > 1) {
    const unsigned int seed = static_cast<unsigned int>(GetTickCount64()) ^
                              static_cast<unsigned int>(GetCurrentProcessId());
    const int step = 1 + static_cast<int>(seed % (kWallpaperCount - 1));
    app.cfg.wallpaper_index = (app.cfg.wallpaper_index + step) % kWallpaperCount;
  }
  app.view.wallpaper_changed_at = app.now_ms();
  app.cfg.save();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kClassName;
  wc.hIcon = make_app_icon(32);
  wc.hIconSm = wc.hIcon;
  RegisterClassExW(&wc);

  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);

  int win_w = mi.rcMonitor.right - mi.rcMonitor.left;
  int win_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  if (win_w < 320 || win_h < 240) {
    win_w = std::max(320, GetSystemMetrics(SM_CXSCREEN));
    win_h = std::max(240, GetSystemMetrics(SM_CYSCREEN));
  }

  // Created hidden as a plain pop-up; apply_window_mode() then gives it the
  // styles, parent, size and focus of whichever mode was saved. Doing that in
  // one place is what keeps start-up and a later mode switch identical.
  app.windowed = app.cfg.mode() == DisplayMode::Window;
  app.hwnd = CreateWindowExW(
      WS_EX_APPWINDOW, kClassName, kAppName, WS_POPUP | WS_CLIPCHILDREN,
      mi.rcMonitor.left, mi.rcMonitor.top, win_w, win_h, nullptr, nullptr, inst,
      nullptr);
  if (!app.hwnd) {
    MessageBoxW(nullptr, L"Could not create the lyrics window.", kAppName,
                MB_ICONERROR);
    GdiplusShutdown(gdi_token);
    return 1;
  }

  enable_dark_titlebar(app.hwnd);
  if (app.cfg.win_w >= 480 && app.cfg.win_h >= 320) {
    app.windowed_rect = RECT{app.cfg.win_x, app.cfg.win_y,
                             app.cfg.win_x + app.cfg.win_w,
                             app.cfg.win_y + app.cfg.win_h};
  }
  apply_window_mode(app);
  UpdateWindow(app.hwnd);

  app.taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
  app.create_tray_icon();
  if (launched_after_update) {
    app.show_toast("Updated successfully to version " YTL_VERSION_STRING, 5000);
  } else if (launched_after_update_failure) {
    app.show_toast("Update failed; the previous version is still installed.", 6000);
  }
  if (!RegisterHotKey(app.hwnd, kHotkeyToggle, MOD_CONTROL | MOD_ALT, 'L')) {
    logf("hotkey Ctrl+Alt+L could not be registered (another app owns it)");
  }
  if (!RegisterHotKey(app.hwnd, kHotkeyWallpaper, MOD_CONTROL | MOD_ALT, 'W')) {
    logf("hotkey Ctrl+Alt+W could not be registered (another app owns it)");
  }
  if (!RegisterHotKey(app.hwnd, kHotkeySettings, MOD_CONTROL | MOD_ALT, 'S')) {
    logf("hotkey Ctrl+Alt+S could not be registered (another app owns it)");
  }
  if (!RegisterHotKey(app.hwnd, kHotkeyFullscreen, MOD_CONTROL | MOD_ALT, 'F')) {
    logf("hotkey Ctrl+Alt+F could not be registered (another app owns it)");
  }

  app.started_at = app.now_ms();
  app.view.last_frame = app.now_ms();
  app.view.last_mouse = 0;
  // The desktop layer stays clean on an ordinary launch; the hint is shown
  // there only the first time, and whenever the mode is changed.
  app.view.hint_until = (app.cfg.mode() != DisplayMode::Desktop || app.first_run)
                            ? app.now_ms() + 9000
                            : 0;

  media_start(app);
  lyrics_start(app);
  update_start(app, !launched_after_update_failure);  // skip re-prompt after a failed replacement
  app.rebuild_layout(true);
  SetTimer(app.hwnd, kFrameTimer, 16, nullptr);

  logf("started %s (mode=%d, monitor %dx%d)", YTL_VERSION_STRING,
       static_cast<int>(app.cfg.mode()), win_w, win_h);

  MSG msg{};
  for (;;) {
    const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
    if (result == 0) break;  // WM_QUIT
    if (result == -1) {
      logf("GetMessageW failed (error %lu)",
           static_cast<unsigned long>(GetLastError()));
      break;
    }
    // The settings window is not a dialog box, so nothing gives Tab, arrows and
    // Escape to the focused owner-drawn control: without this the window could
    // only be driven with the mouse. IsDialogMessage handles that traversal.
    if (app.settings_window && IsWindow(app.settings_window) &&
        IsDialogMessageW(app.settings_window, &msg)) {
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  logf("shutting down");
  app.cfg.save();
  app.destroy_view_resources();
  media_stop();
  lyrics_stop();
  if (app.single_instance) CloseHandle(app.single_instance);
  CoUninitialize();
  GdiplusShutdown(gdi_token);
  return static_cast<int>(msg.wParam);
}
