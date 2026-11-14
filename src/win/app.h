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

// Three embedded cinematic stills plus one procedural scene that is drawn at
// runtime, so the fourth choice costs no bytes in the executable.
constexpr int kWallpaperCount = 4;

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
  // When the player last wrote its timeline (Unix ms), 0 when it never says.
  // Two tracks sharing one stamp means the second has no position of its own
  // yet: the reported position still belongs to the song before it.
  int64_t timeline_stamp = 0;
  // The helper could not extrapolate a believable position from the timeline.
  bool timeline_stale = false;
};

// Where the lyrics are shown. Exactly one of these is in effect at a time.
enum class DisplayMode {
  Desktop,     // behind the desktop icons, click-through, never focused
  Fullscreen,  // a real foreground full-screen window on one monitor
  Window,      // an ordinary resizable window
};

// ---------------------------------------------------------------------- config

struct Config {
  int version = 1;            // written so future migrations can be explicit
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
  // Desktop mode draws the lyrics over the user's own Windows wallpaper. A
  // bundled scene is painted behind them only when this is switched on.
  bool desktop_scene = false;
  bool show_track = true;   // discreet artist / title / progress line
  bool dim_paused = true;   // lyrics are a little fainter while paused
  // Displays the desktop lyrics appear on: "all", "primary" or "1,3".
  std::string desktop_monitors = "all";
  int fade_ms = 600;        // desktop fade-in time; fade-out is 1.5x this
  int shadow = 1;           // dark edge behind desktop text: 0 off, 1 soft, 2 strong
  // Last size and position of the ordinary window (0 x 0 = never saved).
  int win_x = 0, win_y = 0, win_w = 0, win_h = 0;

  DisplayMode mode() const {
    if (wallpaper) return DisplayMode::Desktop;
    return fullscreen ? DisplayMode::Fullscreen : DisplayMode::Window;
  }

  void load();
  void save() const;
};

// ------------------------------------------------------------------- view state
// Everything the painter needs that is not "document" data.

struct View {
  // background cache (gradient + cover art), rebuilt only when inputs change
  Gdiplus::Bitmap* bg = nullptr;
  Gdiplus::CachedBitmap* bg_cached = nullptr;  // device-format copy of `bg`
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
  std::map<int, double> line_activity;
  double smooth_fill = 0.0;
  int smooth_fill_line = -1;
  int64_t last_wipe_at = 0;  // real elapsed time for the karaoke wipe smoother

  // transient UI
  std::string toast;
  int64_t toast_until = 0;
  bool help = false;
  bool help_suppressed_hint = false;
  int64_t hint_until = 0;
  bool cursor_hidden = false;
  int64_t last_mouse = 0;
  POINT last_mouse_pt{-1, -1};
  int64_t last_pulse_paint = 0;

  // True only while render_frame() is drawing the desktop layer with per-pixel
  // transparency: nothing opaque is painted behind the text, ClearType is off
  // (it needs an opaque background) and text gets a soft dark halo.
  bool layer_transparent = false;
};

// ------------------------------------------------------------- desktop layer
// The window that really sits in Explorer's desktop surface. It is separate
// from the main window on purpose: a child of an Explorer window is destroyed
// when Explorer exits, and the main window owns the tray icon, hotkeys and
// timers, which must survive that.

// One layer window: one display.
struct DesktopSurface {
  HWND hwnd = nullptr;
  RECT monitor{};            // the display, screen coordinates
  int number = 0;            // 1-based, left to right then top to bottom
  bool primary = false;
  int w = 0, h = 0;
  UINT dpi = 0;
  bool below_icons = false;  // z-order relative to the icon layer confirmed
  bool visible = false;
  // 32-bit premultiplied back buffer (only allocated when needed)
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ old_bmp = nullptr;
  void* bits = nullptr;
  int buf_w = 0, buf_h = 0;
};

struct DesktopLayer {
  DesktopMachine machine;    // the only place the attachment state lives
  HWND host = nullptr;       // Explorer window the layers are children of
  HWND icons = nullptr;      // SHELLDLL_DefView, the icon layer
  std::string strategy;      // which shell layout was found
  std::string detail;        // why the last attempt failed, in plain words
  DWORD last_error = 0;
  std::vector<DesktopSurface> surfaces;
  int reference = -1;        // surface the lyrics are laid out for
  int w = 0, h = 0;          // its size
  int monitors_total = 0;
  std::string signature;     // display arrangement the surfaces were built for
  int64_t last_check_at = 0;
  bool closing = false;      // we are destroying the windows ourselves
  bool lost_pending = false; // a window was destroyed from outside
  int presents = 0;
  int present_failures = 0;
  int consecutive_draw_failures = 0;
  DWORD present_error = 0;
  double frame_ms_total = 0.0, frame_ms_max = 0.0, frame_ms_last = 0.0;
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
  // A lookup that failed because the network or the provider was down is
  // retried by itself a couple of times instead of leaving "not found" up.
  int64_t lyrics_retry_at = 0;
  int lyrics_retries = 0;

  // Sync corrections remembered per song (offsets.txt), and the one that
  // applies to the track on screen. cfg.offset_ms is the all-songs baseline.
  OffsetStore song_offsets;
  std::string song_offset_key;
  int song_offset_ms = 0;

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

  // Desktop wallpaper mode.
  DesktopLayer desktop;
  // 0..1 opacity of the desktop layer: fades in when music with lyrics is
  // playing and out when it stops, so an idle desktop shows nothing at all.
  double presence = 0.0;
  // The lyric sheet was withdrawn (song changed, session ended) while it was on
  // the desktop: its last frame is faded out as it stands, never re-rendered.
  bool fade_frozen = false;
  int64_t paused_since = 0;
  HWND update_progress_hwnd = nullptr;
  HWND update_progress_bar = nullptr;
  HWND update_progress_label = nullptr;
  std::string update_prompted_version;
  bool windowed = false;      // currently a normal window (not full screen)
  RECT windowed_rect{};
  bool hidden_to_tray = false;
  bool tray_hint_shown = false;     // "still running in the tray" balloon
  bool quitting = false;
  HANDLE single_instance = nullptr;
  HWND settings_window = nullptr;   // native settings dialog, when open
  bool first_run = false;           // no config file existed at startup

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
  // The one place a display mode is chosen: updates the config, re-parents and
  // restyles the window, and brings a foreground mode to the front.
  void set_display_mode(DisplayMode mode);
  void toggle_fullscreen();
  void toggle_topmost();
  void toggle_theme();
  void next_wallpaper();
  void toggle_wallpaper_rotation();
  void cycle_wallpaper_interval();
  void cycle_wallpaper_dim();
  void toggle_random_wallpaper_start();
  void nudge_offset(int delta_ms);        // all songs (Settings)
  void nudge_song_offset(int delta_ms);   // the song on screen (arrow keys)
  void reset_offsets();                   // this song and the baseline
  void load_song_offset();                // after the track on screen changes
  int total_offset_ms() const;            // baseline + this song, clamped
  void export_diagnostics();
  void set_font_scale(double scale);
  void show_toast(const std::string& text, int ms = 1800);
  void toggle_help();
  void open_settings();
  // Applies cfg.wallpaper/cfg.fullscreen/cfg.topmost to the live window in one
  // place, so the settings dialog, the tray menu and the hotkeys all converge.
  void apply_display_mode();
  void show_window();
  void hide_to_tray();
  // Client size of whatever the lyrics are drawn on: the desktop layer in
  // desktop mode, the main window otherwise.
  SIZE surface_size() const;

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
void trim_font_cache(App& app);
const wchar_t* theme_name(int index);
const wchar_t* wallpaper_name(int index);

// Implemented in media.cc
bool media_start(App& app);      // spawns the helper thread
bool media_stop();               // false if the listener did not stop in time
// Listener health for the diagnostic report.
struct MediaHelperStatus {
  int starts = 0;            // times the PowerShell helper was started
  int start_failures = 0;    // times it could not be started at all
  int lines = 0;             // usable reports received
  int bad_lines = 0;         // reports that could not be parsed
  bool running = false;
  int64_t ms_since_last_line = -1;  // -1 = nothing received yet
};
MediaHelperStatus media_helper_status();
// Parses one JSON line from the helper into `out`. Returns false when the line
// is not usable.
bool media_parse_line(const std::string& line, MediaState& out, int64_t now);

// Implemented in lyrics.cc
void lyrics_start(App& app);
bool lyrics_stop();              // false if the worker did not stop in time
void lyrics_enqueue(const TrackInfo& track, int req_id, bool force);
bool lyrics_take_result(int req_id, LyricsState* state, Lyrics* lyrics, std::string* msg);
int lyrics_clear_cache();  // returns deleted file count, or -1 on failure
// For the diagnostic report: the last lookups and what each provider answered,
// and the size of the automatic cache.
std::vector<std::string> lyrics_recent_lookups();
bool lyrics_cache_stats(int* files, uint64_t* bytes);

// Implemented in diag.cc
// Plain-text description of what the app is seeing. `with_connectivity` also
// contacts each provider and the update sources (slow; command line only).
std::string build_diagnostic_report(App& app, bool with_connectivity);
bool write_diagnostic_report(const std::string& report, const std::string& path);
// Writes a report into the data folder, copies it to the clipboard and shows
// it in Explorer.
enum class ReportExport {
  Saved,          // file written, copied to the clipboard
  ClipboardOnly,  // the file could not be written; the text is on the clipboard
  Failed,         // neither worked
};
ReportExport export_diagnostic_report(App& app, std::string* saved_path);
// Headless checks of everything that can be checked without a window or a
// user: file access, configuration, the media listener, cancelling a network
// request, and report redaction. Writes its findings to `path` and returns
// true when every check passed.
bool run_self_test(App& app, const std::string& path);

// Implemented in app.cc, used by the other modules
void notify_from_tray(App& app, const wchar_t* title, const wchar_t* text);

// Implemented in desktop.cc
// Creates the layer window inside Explorer's desktop surface and verifies it.
// Updates app.desktop (state, detail, strategy) either way.
void desktop_layer_enable(App& app);    // desktop mode selected
void desktop_layer_disable(App& app);   // another mode, or shutting down
// Health check, display-arrangement check and due retries. Cheap; called from
// the frame timer. `force` skips the two-second throttle.
void desktop_layer_tick(App& app, bool force);
void desktop_layer_shell_restarted(App& app);
void desktop_layer_display_changed(App& app);
void desktop_layer_power_resumed(App& app);
void desktop_layer_retry(App& app);     // the user asked
// True only when verified layer windows exist. Never derived from settings.
bool desktop_layer_attached(const App& app);
bool desktop_layer_trouble(const App& app);
// Renders the current frame and shows it at `alpha` (0-255) on every chosen
// display. False when nothing could be drawn.
bool desktop_layer_present(App& app, int alpha);
void desktop_layer_hide(App& app);
int desktop_layer_display_count();
// Shows the frame that is already on the layer at a new opacity, without
// rendering. Used to fade the previous song's last frame away.
bool desktop_layer_refade(App& app, int alpha);

// "Start with Windows": a per-user Run entry (no administrator rights, no
// service). The registry is the only record; nothing is cached in config.ini.
bool autostart_enabled();
bool set_autostart(bool on);
void autostart_refresh_path();
std::string desktop_layer_status(const App& app);
std::string desktop_layer_report(const App& app);
// "--desktop-test": attaches a test card to the desktop for a few seconds on
// this machine, checks parent, z-order, focus and click-through, saves a
// screenshot next to `path`, and writes what it found. True if all passed.
int run_desktop_test(App& app, const std::string& path);
void logf(const char* fmt, ...);
// Deliberately fails in release mode instead of shipping a silently missing
// asset: used for the initial window and tray icon paths.
std::string appdata_dir();          // %LOCALAPPDATA%\YouTubeLyrics, created on demand
std::string exe_dir();
std::string read_file(const std::string& path);
bool write_file_atomic(const std::string& path, const std::string& data);
std::string fnv_hex(const std::string& s);
std::string http_get(const std::wstring& url, int timeout_ms, int* status,
                     std::string* err);

}  // namespace ytl
