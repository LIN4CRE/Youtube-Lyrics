// Youtube-Lyrics - window, tray, hotkeys, orchestration.
#include "app.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <knownfolders.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

namespace ytl {
namespace {

using namespace Gdiplus;

const wchar_t* kClassName = L"YoutubeLyricsWindow";
const wchar_t* kAppName = L"Youtube-Lyrics";
const UINT_PTR kFrameTimer = 1;
const int kHotkeyToggle = 1;
const int kHotkeyWallpaper = 2;
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
      REAL rw = s * (small ? 0.052f : 0.022f);
      tile(path, rx, s * 0.215f, rw, s * 0.570f, rw * 0.5f);
      Gdiplus::SolidBrush ink(Color(small ? 170 : 96, 11, 10, 15));
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
        Gdiplus::SolidBrush unsung(Color(86, 11, 10, 15));
        g.FillPath(&unsung, &path);
        tile(path, x0, y, w * 0.68f, h, h * 0.5f);
        Gdiplus::SolidBrush amber(Color(255, 245, 165, 36));
        g.FillPath(&amber, &path);
      } else {
        Gdiplus::SolidBrush ink(Color(small ? 170 : 104, 11, 10, 15));
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

std::vector<LyricLine> synthesise_plain_lines(const std::string& plain) {
  std::vector<LyricLine> out;
  int64_t t = 0;
  size_t pos = 0;
  while (pos <= plain.size()) {
    size_t nl = plain.find('\n', pos);
    if (nl == std::string::npos) nl = plain.size();
    std::string line = plain.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back({t, trim(line)});
    t += 3000;
    if (nl >= plain.size()) break;
    pos = nl + 1;
  }
  return out;
}

void reset_document(App& app) {
  app.lyrics = Lyrics();
  app.disp.clear();
  app.pending_plain_lines.clear();
  app.view.line_top.clear();
  app.view.line_h.clear();
  app.view.total_h = 0;
  app.view.scroll = 0;
  app.active = -1;
}

void update_position_tracker(App& app, const MediaState& observed) {
  const bool was_playing = app.active_media.playing;
  app.active_media = observed;
  app.active_media.track = app.active_track;
  const std::string key = track_key(app.active_track);

  if (!observed.playing) {
    g_pos.key = key;
    g_pos.track = app.active_track;
    g_pos.reported = observed.pos_ms;
    g_pos.last_change = observed.at_ms;
    g_pos.free_anchor = observed.at_ms;
    g_pos.free_running = false;
    return;
  }

  const bool resumed = !was_playing;
  const bool new_recording = g_pos.key.empty() || !same_track(g_pos.track, app.active_track);
  if (new_recording || resumed) {
    g_pos.key = key;
    g_pos.track = app.active_track;
    g_pos.reported = observed.pos_ms;
    g_pos.last_change = observed.at_ms;
    g_pos.free_anchor = observed.at_ms;
    g_pos.free_running = false;
  } else {
    g_pos.key = key;
    g_pos.track = app.active_track;
    if (std::llabs(observed.pos_ms - g_pos.reported) > 150) {
      g_pos.reported = observed.pos_ms;
      g_pos.last_change = observed.at_ms;
      g_pos.free_running = false;
    } else if (!g_pos.free_running && observed.at_ms - g_pos.last_change > 2600 &&
               observed.pos_ms < 1500) {
      g_pos.free_running = true;
      g_pos.free_anchor = g_pos.last_change;
      logf("media: player reports no playhead, running free clock");
    }
  }

  if (g_pos.free_running) {
    app.active_media.at_ms = g_pos.free_anchor;
    app.active_media.pos_ms = 0;
  }
}

void on_media(App& app) {
  MediaState m;
  {
    std::lock_guard<std::mutex> lock(app.mu);
    m = app.media;
  }
  if (!m.raw.empty()) app.helper_heard = true;
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

      const bool confirmed = app.pending_seen >= 2 &&
                             now - app.pending_since >= 300;
      if (confirmed) {
        TrackInfo next = app.pending_track;
        if (next.title.empty()) next = m.track;
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
          logf("media: now playing '%s' (%s) dur=%.1f",
               track_label(app.active_track).c_str(), app.current_key.c_str(),
               m.duration_s);
          lyrics_enqueue(app.active_track, ++app.lyrics_req_id, false);
        }
        update_position_tracker(app, m);
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
  if (!worker) return TRUE;
  *reinterpret_cast<HWND*>(param) = worker;
  return FALSE;
}

HWND find_desktop_host() {
  // Ask Explorer to create the WorkerW surface behind the desktop icons (the
  // same documented-in-practice technique used by live wallpaper apps).
  HWND progman = FindWindowW(L"Progman", nullptr);
  if (progman) {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &ignored);
  }
  HWND host = nullptr;
  EnumWindows(find_workerw_after_desktop, reinterpret_cast<LPARAM>(&host));
  return host;
}

void apply_desktop_mode(App& app);

void apply_fullscreen(App& app) {
  if (!app.hwnd) return;
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  HMONITOR mon = MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST);
  GetMonitorInfoW(mon, &mi);

  if (!app.windowed) {
    SetWindowLongPtrW(app.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(app.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
  } else {
    SetWindowLongPtrW(app.hwnd, GWL_STYLE,
                      WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
    RECT r = app.windowed_rect;
    const int mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
    const int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (r.right - r.left < 400 || r.bottom - r.top < 320 ||
        r.right - r.left >= mon_w * 9 / 10 ||
        r.bottom - r.top >= mon_h * 9 / 10) {
      int w = mon_w * 3 / 4;
      int h = mon_h * 3 / 4;
      r = {mi.rcMonitor.left + (mon_w - w) / 2,
           mi.rcMonitor.top + (mon_h - h) / 2,
           mi.rcMonitor.left + (mon_w - w) / 2 + w,
           mi.rcMonitor.top + (mon_h - h) / 2 + h};
    }
    SetWindowPos(app.hwnd, HWND_TOP, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }

  app.hidden_to_tray = false;
  apply_desktop_mode(app);
  const bool desktop = app.cfg.wallpaper && !app.windowed;
  ShowWindow(app.hwnd, desktop ? SW_SHOWNOACTIVATE : SW_SHOW);
  if (!desktop) SetForegroundWindow(app.hwnd);
  invalidate_window(app);
}

// Places the display in the actual WorkerW desktop layer, behind desktop icons
// but above the wallpaper. When WorkerW is unavailable, fall back to the bottom
// of the top-level z-order; the tray toggle remains available either way.
void apply_desktop_mode(App& app) {
  if (!app.hwnd) return;
  const bool want_wallpaper = app.cfg.wallpaper && !app.windowed;

  if (want_wallpaper) {
    if (!app.desktop_parented) {
      HWND host = find_desktop_host();
      if (host) {
        SetLastError(0);
        SetParent(app.hwnd, host);
        if (GetLastError() == 0) {
          app.desktop_host = host;
          app.desktop_parented = true;
        }
      }
    }

    LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
    ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT;
    ex &= ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, ex);
    SetLayeredWindowAttributes(app.hwnd, 0, 255, LWA_ALPHA);

    if (app.desktop_parented && IsWindow(app.desktop_host)) {
      LONG_PTR style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
      style &= ~WS_POPUP;
      style |= WS_CHILD | WS_VISIBLE;
      SetWindowLongPtrW(app.hwnd, GWL_STYLE, style);
      RECT r{};
      GetClientRect(app.desktop_host, &r);
      SetWindowPos(app.hwnd, HWND_TOP, 0, 0, r.right - r.left, r.bottom - r.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
      logf("window: attached to desktop WorkerW %p", app.desktop_host);
    } else {
      // Graceful fallback for Windows configurations where Explorer does not
      // expose a WorkerW host.
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
      LONG_PTR style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
      style &= ~WS_CHILD;
      style |= WS_POPUP | WS_VISIBLE;
      SetWindowLongPtrW(app.hwnd, GWL_STYLE, style);
      SetWindowPos(app.hwnd, HWND_BOTTOM, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
      logf("window: WorkerW unavailable; using bottom z-order fallback");
    }
  } else {
    if (app.desktop_parented) {
      SetLastError(0);
      SetParent(app.hwnd, nullptr);
      app.desktop_parented = false;
      app.desktop_host = nullptr;
    }

    LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
    ex &= ~(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
            WS_EX_TRANSPARENT);
    ex |= WS_EX_APPWINDOW;
    SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, ex);

    if (!app.windowed) {
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
      LONG_PTR style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
      style &= ~WS_CHILD;
      style |= WS_POPUP | WS_VISIBLE;
      SetWindowLongPtrW(app.hwnd, GWL_STYLE, style);
      SetWindowPos(app.hwnd, app.cfg.topmost ? HWND_TOPMOST : HWND_TOP,
                   mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
      LONG_PTR style = GetWindowLongPtrW(app.hwnd, GWL_STYLE);
      style &= ~(WS_CHILD | WS_POPUP);
      style |= WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN;
      SetWindowLongPtrW(app.hwnd, GWL_STYLE, style);
      SetWindowPos(app.hwnd, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
    }
  }
  logf("window: desktop mode %s", want_wallpaper ? "on" : "off");
}

void toggle_wallpaper_mode(App& app) {
  app.cfg.wallpaper = !app.cfg.wallpaper;
  if (app.cfg.wallpaper) {
    // Wallpaper mode is a full-screen desktop surface, not a floating window.
    app.windowed = false;
    app.cfg.fullscreen = true;
    app.cfg.topmost = false;
  }
  app.cfg.save();
  apply_fullscreen(app);
  app.rebuild_layout(true);
  app.show_toast(app.cfg.wallpaper
                     ? "Desktop background on · Ctrl+Alt+W to switch off"
                     : "Desktop background off");
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

void remove_tray_icon(App& app) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = app.hwnd;
  nid.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void show_tray_menu(App& app) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, L"Show lyrics\tCtrl+Alt+L");
  AppendMenuW(menu, MF_STRING | (app.windowed ? 0 : MF_CHECKED), 2,
              L"Full screen\tF11");
  AppendMenuW(menu, MF_STRING | (app.cfg.wallpaper ? MF_CHECKED : 0), 7,
              L"Desktop background (clicks pass through)\tW");
  AppendMenuW(menu, MF_STRING | (app.cfg.topmost ? MF_CHECKED : 0), 3,
              L"Always on top\tA");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 4, L"Reload lyrics\tR");
  AppendMenuW(menu, MF_STRING | (app.view.help ? MF_CHECKED : 0), 5,
              L"Keyboard shortcuts\tF1");
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
      app.toggle_fullscreen();
      break;
    case 3:
      app.toggle_topmost();
      break;
    case 7:
      toggle_wallpaper_mode(app);
      break;
    case 4:
      app.request_lyrics(app.current_track(), true);
      invalidate_window(app);
      break;
    case 5:
      app.toggle_help();
      break;
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
  int64_t now = app.now_ms();
  double dt = app.view.last_frame ? (now - app.view.last_frame) / 1000.0 : 0.016;
  app.view.last_frame = now;
  if (dt > 0.25) dt = 0.25;

  RECT rc{};
  GetClientRect(app.hwnd, &rc);
  int h = rc.bottom - rc.top;

  int64_t pos = app.current_pos_ms();
  int new_active = app.lyrics.synced ? active_index(app.disp, pos) : -1;
  if (new_active != app.active) {
    app.active = new_active;
  }

  double target = scroll_target(app, h);
  app.view.scroll = ease_towards(app.view.scroll, target, dt, 0.13);
  if (std::fabs(target - app.view.scroll) < 0.4) app.view.scroll = target;
  bool animating = std::fabs(target - app.view.scroll) > 0.4;

  // Repaint while playing (smooth karaoke), while animating, and occasionally
  // when idle so the clock stays honest.
  static int idle_ticks = 0;
  // The resting wallpaper has a listening pulse, so idle is not static: repaint
  // at a low rate while nothing is playing instead of going completely still.
  bool repaint = animating || app.view.toast_until > now || app.active_media.playing;
  // Idle wallpaper: repaint about six times a second - enough for the listening
  // pulse to breathe, cheap enough to leave on a desktop all day.
  if (!repaint && app.lstate == LyricsState::Idle) {
    if (now - app.view.last_pulse_paint >= 160) {
      app.view.last_pulse_paint = now;
      repaint = true;
    }
  }
  if (!repaint) {
    if (++idle_ticks >= 12) {
      idle_ticks = 0;
      repaint = true;
    }
  } else {
    idle_ticks = 0;
  }
  if (repaint) invalidate_window(app);

  // Hide the mouse pointer when the display is left alone in full screen.
  if (!app.view.cursor_hidden && !app.windowed && !app.view.help &&
      now - app.view.last_mouse > 2500 && app.view.last_mouse > 0) {
    ShowCursor(FALSE);
    app.view.cursor_hidden = true;
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
  std::fclose(f);
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
  std::string out;
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ |
                                                          FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return out;
  char buf[8192];
  DWORD got = 0;
  while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
    out.append(buf, got);
  }
  CloseHandle(h);
  return out;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tmp";
  HANDLE h = CreateFileW(widen(tmp).c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written,
                      nullptr) != 0;
  CloseHandle(h);
  if (!ok) return false;
  return MoveFileExW(widen(tmp).c_str(), widen(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING) != 0;
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
    if (k == "offset_ms") offset_ms = atoi(v.c_str());
    else if (k == "font_scale") font_scale = atof(v.c_str());
    else if (k == "topmost") topmost = (v == "1");
    else if (k == "cover") cover = (v == "1");
    else if (k == "wipe") wipe = (v == "1");
    else if (k == "theme") theme = atoi(v.c_str());
    else if (k == "fullscreen") fullscreen = (v == "1");
    else if (k == "wallpaper") wallpaper = (v == "1");
  }
  if (font_scale < 0.55 || font_scale > 2.2) font_scale = 1.0;
  if (theme < 0 || theme >= 4) theme = 0;
  if (wallpaper) fullscreen = true;
}

void Config::save() const {
  char buf[512];
  std::string data = "offset_ms=" + std::to_string(offset_ms) + "\n";
  std::snprintf(buf, sizeof(buf), "font_scale=%.3f\n", font_scale);
  data += buf;
  data += std::string("topmost=") + (topmost ? "1" : "0") + "\n";
  data += std::string("cover=") + (cover ? "1" : "0") + "\n";
  data += std::string("wipe=") + (wipe ? "1" : "0") + "\n";
  data += "theme=" + std::to_string(theme) + "\n";
  data += std::string("fullscreen=") + (fullscreen ? "1" : "0") + "\n";
  data += std::string("wallpaper=") + (wallpaper ? "1" : "0") + "\n";
  write_file_atomic(appdata_dir() + "\\config.ini", data);
}

// ----------------------------------------------------------------- App methods

int64_t App::current_pos_ms() const {
  const MediaState& m = active_media;  // committed UI-thread snapshot
  int64_t dur = static_cast<int64_t>(m.duration_s * 1000.0);
  int64_t pos = interpolate_pos(m.pos_ms, m.at_ms, now_ms(), m.playing, dur);
  pos += cfg.offset_ms;
  if (pos < 0) pos = 0;
  if (dur > 0 && pos > dur) pos = dur;
  return pos;
}

double App::current_duration_s() const { return active_media.duration_s; }

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
  if (lstate == LyricsState::Ready && !lyrics.synced) {
    // Untimed lyrics: pace the rows across the song's real length so the sheet
    // still scrolls with playback instead of sitting frozen on one page. Two
    // seconds of lead-in keeps the first line off screen at 0:00.
    const double dur = current_duration_s();
    const size_t row_count = std::max<size_t>(1, synthesise_plain_lines(lyrics.plain).size());
    int64_t per_line = 4000;
    if (dur > 20) {
      per_line = static_cast<int64_t>(dur * 1000.0 * 0.92) /
                 static_cast<int64_t>(row_count);
      per_line = std::max<int64_t>(1500, std::min<int64_t>(9000, per_line));
    }
    pending_plain_lines = pace_plain_lyrics(lyrics.plain, 2000, per_line);
    if (!pending_plain_lines.empty() && lyrics.lines.empty()) {
      // Give the plain sheet timestamps too, so the active-line spotlight and
      // the scroll anchor work exactly as they do for synced lyrics.
      lyrics.lines = pending_plain_lines;
      lyrics.synced = true;
    }
  }
  relayout(*this, w, h);
  if (!lyrics.synced) active = -1;
  (void)force;
  view.bg_theme = -1;  // force background rebuild (theme/art may have changed)
}

void App::relayout_if_needed() { rebuild_layout(false); }

void App::invalidate() { invalidate_window(*this); }

void App::toggle_fullscreen() {
  windowed = !windowed;
  cfg.fullscreen = !windowed;
  cfg.save();
  apply_fullscreen(*this);
  rebuild_layout(true);
}

void App::toggle_topmost() {
  if (cfg.wallpaper) {
    show_toast("Always-on-top is unavailable in desktop mode");
    return;
  }
  cfg.topmost = !cfg.topmost;
  cfg.save();
  SetWindowPos(hwnd, cfg.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE);
  show_toast(cfg.topmost ? "Always on top: on" : "Always on top: off");
  invalidate();
}

void App::toggle_theme() {
  cfg.theme = (cfg.theme + 1) % 4;
  cfg.save();
  view.bg_theme = -1;
  std::string name = wide_to_utf8(theme_name(cfg.theme));
  show_toast("Theme: " + name);
  invalidate();
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
}

void App::show_toast(const std::string& text, int ms) {
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
  apply_desktop_mode(*this);
  const bool desktop = cfg.wallpaper && !windowed;
  ShowWindow(hwnd, desktop ? SW_SHOWNOACTIVATE : SW_SHOW);
  if (!desktop) SetForegroundWindow(hwnd);
  if (!windowed && !desktop_parented) {
    SetWindowPos(hwnd, cfg.topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  invalidate();
}

void App::hide_to_tray() {
  hidden_to_tray = true;
  ShowWindow(hwnd, SW_HIDE);
  show_toast("Hidden — Ctrl+Alt+L brings it back");
}

void App::create_tray_icon() { add_tray_icon(*this); }

void App::destroy_view_resources() {
  if (view.mem_dc) {
    SelectObject(view.mem_dc, view.mem_old);
    DeleteObject(view.mem_bmp);
    DeleteDC(view.mem_dc);
    view.mem_dc = nullptr;
    view.mem_bmp = nullptr;
  }
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
      if (!app.windowed) {
        app.toggle_fullscreen();
      } else {
        app.hide_to_tray();
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
    case 'C':
      app.cfg.cover = !app.cfg.cover;
      app.cfg.save();
      app.view.bg_theme = -1;
      refresh_art(app);
      app.show_toast(app.cfg.cover ? "Cover art background: on"
                                   : "Cover art background: off");
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
    add_tray_icon(*app);
    if (app->cfg.wallpaper && !app->windowed) {
      app->desktop_parented = false;
      app->desktop_host = nullptr;
      apply_desktop_mode(*app);
    }
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
        invalidate_window(*app);
      }
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
    case WM_HOTKEY:
      if (wParam == kHotkeyToggle) {
        if (app->hidden_to_tray || !IsWindowVisible(hwnd)) {
          app->show_window();
        } else {
          app->hide_to_tray();
        }
      } else if (wParam == kHotkeyWallpaper) {
        toggle_wallpaper_mode(*app);
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
    case WM_MOUSEMOVE:
      app->view.last_mouse = app->now_ms();
      if (app->view.cursor_hidden) {
        ShowCursor(TRUE);
        app->view.cursor_hidden = false;
      }
      return 0;
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
      if (!app->quitting) {
        app->hide_to_tray();
        return 0;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      remove_tray_icon(*app);
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION:
      app->cfg.save();
      break;
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

void enable_dark_titlebar(HWND hwnd) {
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark,
                        sizeof(dark));
}

}  // namespace

}  // namespace ytl

// ------------------------------------------------------------------- WinMain

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show_cmd) {
  using namespace ytl;

  set_dpi_awareness();

  GdiplusStartupInput gdi_input;
  ULONG_PTR gdi_token = 0;
  GdiplusStartup(&gdi_token, &gdi_input, nullptr);

  App app;
  app.inst = inst;
  g_app = &app;

  // Single instance: focus the running copy instead of starting a second one.
  app.single_instance =
      CreateMutexW(nullptr, TRUE, L"Local\\YoutubeLyrics_SingleInstance_v1");
  if (app.single_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = FindWindowW(kClassName, nullptr);
    if (existing) {
      // The running instance decides whether it may activate; in wallpaper mode
      // a second launch must not pull it above the desktop layer.
      PostMessageW(existing, WM_APP_SHOW_EXISTING, 0, 0);
    }
    GdiplusShutdown(gdi_token);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  app.cfg.load();

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

  app.windowed = !app.cfg.fullscreen;
  DWORD style = app.windowed ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE)
                             : (WS_POPUP | WS_VISIBLE);
  int win_w = mi.rcMonitor.right - mi.rcMonitor.left;
  int win_h = mi.rcMonitor.bottom - mi.rcMonitor.top;

  // In desktop-background mode the window is a tool window that never activates,
  // so it can live behind everything without appearing in alt-tab.
  const DWORD ex_style = app.cfg.wallpaper
                             ? (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)
                             : WS_EX_APPWINDOW;
  app.hwnd = CreateWindowExW(
      ex_style, kClassName, kAppName, style, mi.rcMonitor.left,
      mi.rcMonitor.top, app.windowed ? win_w * 3 / 4 : win_w,
      app.windowed ? win_h * 3 / 4 : win_h, nullptr, nullptr, inst, nullptr);
  if (!app.hwnd) {
    MessageBoxW(nullptr, L"Could not create the lyrics window.", kAppName,
                MB_ICONERROR);
    GdiplusShutdown(gdi_token);
    return 1;
  }

  enable_dark_titlebar(app.hwnd);
  GetWindowRect(app.hwnd, &app.windowed_rect);
  if (app.windowed) {
    int w = win_w * 3 / 4, h = win_h * 3 / 4;
    SetWindowPos(app.hwnd, nullptr, mi.rcMonitor.left + (win_w - w) / 2,
                 mi.rcMonitor.top + (win_h - h) / 2, w, h, SWP_NOZORDER);
  }

  apply_desktop_mode(app);
  const bool desktop_start = app.cfg.wallpaper && !app.windowed;
  ShowWindow(app.hwnd, desktop_start ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
  UpdateWindow(app.hwnd);
  if (!desktop_start) SetForegroundWindow(app.hwnd);

  app.taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
  app.create_tray_icon();
  if (!RegisterHotKey(app.hwnd, kHotkeyToggle, MOD_CONTROL | MOD_ALT, 'L')) {
    logf("hotkey Ctrl+Alt+L could not be registered (another app owns it)");
  }
  if (!RegisterHotKey(app.hwnd, kHotkeyWallpaper, MOD_CONTROL | MOD_ALT, 'W')) {
    logf("hotkey Ctrl+Alt+W could not be registered (another app owns it)");
  }

  app.started_at = app.now_ms();
  app.view.last_frame = app.now_ms();
  app.view.last_mouse = 0;
  app.view.hint_until = app.now_ms() + 9000;

  media_start(app);
  lyrics_start(app);
  app.rebuild_layout(true);
  SetTimer(app.hwnd, kFrameTimer, 16, nullptr);

  logf("started (windowed=%d, monitor %dx%d)", app.windowed ? 1 : 0, win_w, win_h);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (msg.message == WM_APP_SHOW_EXISTING) {
      app.quitting = false;
      app.show_window();
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
