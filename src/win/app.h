// Youtube-Lyrics - Windows GUI layer shared state.
#pragma once

#include <windows.h>
#include <gdiplus.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../core/core.h"
#include "resource.h"

namespace ytl {

// Custom window messages.
enum : UINT {
  WM_APP_MEDIA = WM_APP + 1,   // media thread: new song state available
  WM_APP_LYRICS = WM_APP + 2,  // lyrics thread: lookup finished
  WM_APP_TRAY = WM_APP + 3,    // tray icon callback
  WM_APP_UPDATE_AVAILABLE = WM_APP + 4,
  WM_APP_UPDATE_PROGRESS = WM_APP + 5,
  WM_APP_UPDATE_READY = WM_APP + 6,
  WM_APP_UPDATE_ERROR = WM_APP + 7,
  WM_APP_UPDATE_NONE = WM_APP + 8,
};

constexpr int kWallpaperCount = 3;

enum class LyricsState { Idle, Loading, Ready, NotFound, Error };

// ---------------------------------------------------------------- media state

struct MediaState {
  bool have = false;      // a media session exists
  bool playing = false;   // it is playing right now
  std::string status;     // raw SMTC status ("playing"/"paused"/"none"/...)
  TrackInfo track;        // cleaned title/artist/album
  double duration_s = 0.0;
  int64_t pos_ms = 0;     // reported position when the poll arrived
  int64_t at_ms = 0;      // GetTickCount64() when the poll arrived
  std::string key;        // track_key(track)
  std::string art_path;   // cover-art file written by the helper (may be empty)
  std::string app;        // source app id (chrome.exe, Spotify.exe, ...)
  std::string raw;        // raw JSON, shown in the help overlay for debugging
};

// ---------------------------------------------------------------------- config

struct Config {
  int offset_ms = 0;          // manual sync nudge
  double font_scale = 1.0;    // text size multiplier
  bool topmost = false;
  bool cover = true;          // cover art background
  bool wipe = true;           // karaoke wipe on the active line
  int theme = 0;              // index into the theme table
  bool fullscreen = true;     // start in full screen
  // Desktop-wallpaper mode: the window sits on the desktop layer behind every
  // other window, ignores mouse input entirely and never takes focus, so it can
  // stay up permanently without getting in the way.
  bool wallpaper = true;
  int wallpaper_index = 0;
  bool wallpaper_random_start = true;
  bool wallpaper_auto_rotate = true;
  int wallpaper_interval_minutes = 30;
  int wallpaper_dim = 12;  // percent, subtle by default

  void load();
  void save() const;
};

// ------------------------------------------------------------------- view state
// Everything the painter needs that is not "document" data.

struct View {
  // background cache (gradient + cover art), rebuilt only when inputs change
  Gdiplus::Bitmap* bg = nullptr;
  int bg_w = 0;
  int bg_h = 0;
  std::string bg_art;
  int bg_theme = -1;
  int bg_wallpaper = -1;
  int bg_dim = -1;
  int64_t wallpaper_changed_at = 0;

  // cover art image
  Gdiplus::Image* art = nullptr;
  std::string art_path;

  // One selected embedded wallpaper; keep its stream alive for GDI+ and never
  // cache all decoded backgrounds at once.
  Gdiplus::Image* wallpaper_image = nullptr;
  IStream* wallpaper_stream = nullptr;
  int wallpaper_loaded_index = -1;

  // font cache, keyed by (pixels, weight)
  std::map<std::pair<int, int>, Gdiplus::Font*> fonts;
  Gdiplus::FontFamily* family = nullptr;

  // layout: per display-line vertical metrics
  std::vector<double> line_top;
  std::vector<double> line_h;
  double total_h = 0;
  int disp_rows = 0;
  double lay_w = 0, lay_h = 0, lay_scale = 0;
  double base_px = 0, act_px = 0, col_w = 0;

  // cached back buffer (avoids reallocating an 8 MB bitmap every frame)
  HDC mem_dc = nullptr;
  HBITMAP mem_bmp = nullptr;
  HGDIOBJ mem_old = nullptr;
  int mem_w = 0;
  int mem_h = 0;

  // animation
  double scroll = 0;
  int64_t last_frame = 0;
  int last_active = -1;

  // transient UI
  std::string toast;
  int64_t toast_until = 0;
  bool help = false;
  bool help_suppressed_hint = false;
  int64_t hint_until = 0;
  bool cursor_hidden = false;
  int64_t last_mouse = 0;
  int64_t last_pulse_paint = 0;
};

// --------------------------------------------------------------------- the app

struct App {
  HINSTANCE inst = nullptr;
  HWND hwnd = nullptr;
  UINT taskbar_created_msg = 0;

  Config cfg;

  std::mutex mu;   // guards `media` (the latest raw helper observation)
  MediaState media;

  // UI-thread-owned committed media state. Unlike `media`, this survives a
  // transient blank/none poll while SMTC tears down or switches sessions.
  MediaState active_media;
  TrackInfo active_track;

  // lyrics document (UI thread owns it)
  Lyrics lyrics;
  LyricsState lstate = LyricsState::Idle;
  std::string lmessage;  // detail for NotFound / Error
  int lyrics_req_id = 0; // id of the in-flight lookup

  // laid out display lines
  std::vector<DispLine> disp;
  std::vector<LyricLine> pending_plain_lines;  // estimated rows for untimed text
  bool timing_estimated = false;  // display pacing, never source synchronization
  int active = -1;
  std::string current_key;  // track currently being displayed

  View view;

  bool helper_heard = false;  // has the media helper reported anything yet?
  bool session_present = false;  // current SMTC observation is watchable
  int64_t started_at = 0;

  // Track-change debounce. A stale session can report one song for a single
  // poll while another app is starting; only commit after the same track has
  // been seen twice in a row, so the display never flickers to another song.
  std::string pending_key;
  TrackInfo pending_track;
  int pending_seen = 0;
  int64_t pending_since = 0;
  int64_t missing_since = 0;

  // Parent HWND for true WorkerW wallpaper mode (behind desktop icons).
  HWND desktop_host = nullptr;
  bool desktop_parented = false;
  HWND update_progress_hwnd = nullptr;
  HWND update_progress_bar = nullptr;
  HWND update_progress_label = nullptr;
  std::string update_prompted_version;
  bool windowed = false;      // currently a normal window (not full screen)
  RECT windowed_rect{};
  bool hidden_to_tray = false;
  bool quitting = false;
  HANDLE single_instance = nullptr;

  // ---- helpers ------------------------------------------------------------
  int64_t now_ms() const { return static_cast<int64_t>(GetTickCount64()); }

  // Current playback position, extrapolated from the last media poll and with
  // the user's manual offset applied. 0 when nothing is playing.
  int64_t current_pos_ms() const;
  double current_duration_s() const;

  // Track whose lyrics have been committed after media-session validation.
  TrackInfo current_track() const;

  void request_lyrics(const TrackInfo& track, bool force);
  void apply_lyrics(int req_id);
  void rebuild_layout(bool force = false);
  void relayout_if_needed();
  void invalidate();

  // view toggles
  void toggle_fullscreen();
  void toggle_topmost();
  void toggle_theme();
  void next_wallpaper();
  void toggle_wallpaper_rotation();
  void cycle_wallpaper_interval();
  void cycle_wallpaper_dim();
  void toggle_random_wallpaper_start();
  void nudge_offset(int delta_ms);
  void set_font_scale(double scale);
  void show_toast(const std::string& text, int ms = 1800);
  void toggle_help();
  void show_window();
  void hide_to_tray();

  // painting / icons
  void paint(HDC dc);
  void on_resize(int w, int h);
  void create_tray_icon();
  void destroy_view_resources();
};

// Implemented in render.cc
void render_frame(App& app, Gdiplus::Graphics& g, int w, int h);
void ensure_background(App& app, int w, int h);
void relayout(App& app, int w, int h);
void refresh_art(App& app);
double scroll_target(const App& app, int h);
Gdiplus::Font* get_font(App& app, int px, int weight);
const wchar_t* theme_name(int index);
const wchar_t* wallpaper_name(int index);

// Implemented in media.cc
bool media_start(App& app);      // spawns the helper thread
void media_stop();
// Parses one JSON line from the helper into `out`. Returns false when the line
// is not usable.
bool media_parse_line(const std::string& line, MediaState& out, int64_t now);

// Implemented in lyrics.cc
void lyrics_start(App& app);
void lyrics_stop();
void lyrics_enqueue(const TrackInfo& track, int req_id, bool force);
bool lyrics_take_result(int req_id, LyricsState* state, Lyrics* lyrics, std::string* msg);
int lyrics_clear_cache();  // returns deleted file count, or -1 on failure

// Implemented in app.cc, used by the other modules
void logf(const char* fmt, ...);
std::string appdata_dir();          // %LOCALAPPDATA%\YouTubeLyrics, created on demand
std::string exe_dir();
std::string read_file(const std::string& path);
bool write_file_atomic(const std::string& path, const std::string& data);
std::string fnv_hex(const std::string& s);
std::string http_get(const std::wstring& url, int timeout_ms, int* status,
                     std::string* err);

}  // namespace ytl
