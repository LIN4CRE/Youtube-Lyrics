// Youtube-Lyrics - native settings window.
//
// The tray menu can toggle everything the app does, but a dozen entries with
// "cycle this value" behaviour are not discoverable for a first-time user.
// This is an ordinary Win32 window, drawn with the renderer's palette and the
// same Segoe UI family, that lists every persisted preference with a plain
// language label and an immediately visible value.
//
// It deliberately contains no framework, no property sheet and no installer:
// the controls are owner-drawn so they match the four themes, and every change
// is written straight into the App::Config the tray menu and hotkeys use.
#include "settings.h"

#include "app.h"
#include "update.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace ytl {
namespace {

constexpr wchar_t kSettingsClass[] = L"YoutubeLyricsSettings";
constexpr wchar_t kControlClass[] = L"YoutubeLyricsSettingsControl";

// The window's own palette, mirroring the four themes in src/win/render.cc so
// the dialog and the display can never drift apart.
struct Palette {
  COLORREF background;
  COLORREF panel;
  COLORREF text;
  COLORREF muted;
  COLORREF accent;
  COLORREF border;
};

Palette palette_for(int theme_index) {
  Palette p{};
  switch (theme_index) {
    case 1:  // Aurora
      p = {RGB(7, 13, 20), RGB(15, 32, 44), RGB(234, 247, 249),
           RGB(150, 190, 200), RGB(92, 200, 216), RGB(30, 58, 74),};
      break;
    case 2:  // Ember
      p = {RGB(18, 12, 8), RGB(34, 20, 10), RGB(255, 241, 227),
           RGB(206, 172, 146), RGB(224, 122, 60), RGB(74, 45, 27),};
      break;
    case 3:  // Mono
      p = {RGB(11, 11, 11), RGB(23, 23, 23), RGB(245, 242, 236),
           RGB(176, 174, 168), RGB(237, 234, 227), RGB(52, 52, 52),};
      break;
    default:  // Midnight
      p = {RGB(11, 10, 15), RGB(24, 22, 30), RGB(243, 239, 230),
           RGB(160, 154, 142), RGB(245, 165, 36), RGB(53, 50, 61),};
      break;
  }
  return p;
}

// ------------------------------------------------------------------- controls

enum class ControlKind { Button, Check };

struct ControlState {
  int id = 0;
  ControlKind kind = ControlKind::Button;
  std::wstring label;
  bool checked = false;
  bool enabled = true;
  bool hover = false;
  bool pressed = false;
  RECT rect{};
  HWND hwnd = nullptr;
};

struct SettingsWindow {
  App* app = nullptr;
  HWND hwnd = nullptr;
  HFONT body = nullptr;
  HFONT value = nullptr;
  HFONT heading = nullptr;
  HFONT small = nullptr;
  HFONT section = nullptr;
  HBRUSH background = nullptr;
  HBRUSH panel = nullptr;
  RECT close_rect{};
  bool close_hot = false;
  bool close_pressed = false;
  int pressing_id = -1;
};

SettingsWindow g_settings;
std::vector<ControlState>& controls() {
  static std::vector<ControlState> list;
  return list;
}

ControlState* find_control(int id) {
  for (ControlState& c : controls()) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

HFONT make_font(int px, int weight, const wchar_t* family = L"Segoe UI") {
  LOGFONTW lf{};
  lf.lfHeight = -px;
  lf.lfWeight = weight;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcscpy_s(lf.lfFaceName, family);
  return CreateFontIndirectW(&lf);
}

void track_leave(HWND hwnd) {
  TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
  TrackMouseEvent(&track);
}

void toggle_state(ControlState& c) {
  if (c.kind == ControlKind::Check) c.checked = !c.checked;
  SendMessageW(g_settings.hwnd, WM_COMMAND,
               MAKEWPARAM(c.id, BN_CLICKED), reinterpret_cast<LPARAM>(c.hwnd));
}

LRESULT CALLBACK control_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  ControlState* state =
      reinterpret_cast<ControlState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
      state = static_cast<ControlState*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      if (state) state->hwnd = hwnd;
      break;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT client{};
      GetClientRect(hwnd, &client);
      const Palette p = palette_for(g_settings.app ? g_settings.app->cfg.theme : 0);
      // The parent paints the panel; this control draws only its own chrome.
      if (!state) {
        EndPaint(hwnd, &ps);
        return 0;
      }
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      HBRUSH bg = CreateSolidBrush(p.panel);
      FillRect(mem, &client, bg);
      DeleteObject(bg);
      SetBkMode(mem, TRANSPARENT);
      SelectObject(mem, g_settings.body);

      const bool active = state->enabled;
      RECT r{0, 0, client.right, client.bottom};

      if (state->kind == ControlKind::Check) {
        // A small square with an amber tick: the same "lit line" idea at
        // control scale, and legible in every theme.
        const int box = 16;
        const int top = (client.bottom - box) / 2;
        RECT mark{1, top, 1 + box, top + box};
        HBRUSH fill = CreateSolidBrush(state->checked ? p.accent : p.background);
        FillRect(mem, &mark, fill);
        DeleteObject(fill);
        HPEN edge = CreatePen(PS_SOLID, 1, active ? p.border : p.panel);
        HGDIOBJ old_pen = SelectObject(mem, edge);
        HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, mark.left, mark.top, mark.right, mark.bottom);
        SelectObject(mem, old_pen);
        SelectObject(mem, old_brush);
        DeleteObject(edge);
        if (state->checked) {
          HPEN tick = CreatePen(PS_SOLID, 2, p.background);
          HGDIOBJ old_tick = SelectObject(mem, tick);
          MoveToEx(mem, mark.left + 4, top + 8, nullptr);
          LineTo(mem, mark.left + 7, top + 11);
          LineTo(mem, mark.left + 12, top + 5);
          SelectObject(mem, old_tick);
          DeleteObject(tick);
        }
        RECT label{mark.right + 9, 0, client.right, client.bottom};
        SetTextColor(mem, active ? p.text : p.muted);
        DrawTextW(mem, state->label.c_str(), -1, &label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (GetFocus() == hwnd) {
          // Tab traversal is real (IsDialogMessage in the message loop), so the
          // keyboard position has to be visible.
          RECT ring{0, 0, client.right - 1, client.bottom - 1};
          HGDIOBJ old_b = SelectObject(mem, GetStockObject(NULL_BRUSH));
          HPEN focus_pen = CreatePen(PS_DOT, 1, p.accent);
          HGDIOBJ old_p = SelectObject(mem, focus_pen);
          Rectangle(mem, ring.left, ring.top, ring.right, ring.bottom);
          SelectObject(mem, old_p);
          SelectObject(mem, old_b);
          DeleteObject(focus_pen);
        }
      } else {
        const bool lit = active && (state->hover || state->pressed);
        HBRUSH fill = CreateSolidBrush(state->pressed ? p.background : p.panel);
        FillRect(mem, &r, fill);
        DeleteObject(fill);
        HPEN edge = CreatePen(PS_SOLID, 1, lit ? p.accent : p.border);
        HGDIOBJ old_pen = SelectObject(mem, edge);
        HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, 0, 0, client.right, client.bottom);
        SelectObject(mem, old_pen);
        SelectObject(mem, old_brush);
        DeleteObject(edge);
        SetTextColor(mem, !active ? p.muted : (lit ? p.accent : p.text));
        DrawTextW(mem, state->label.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (GetFocus() == hwnd) {
          HPEN focus_pen = CreatePen(PS_SOLID, 2, p.accent);
          HGDIOBJ old_p = SelectObject(mem, focus_pen);
          HGDIOBJ old_b = SelectObject(mem, GetStockObject(NULL_BRUSH));
          Rectangle(mem, 2, 2, client.right - 2, client.bottom - 2);
          SelectObject(mem, old_p);
          SelectObject(mem, old_b);
          DeleteObject(focus_pen);
        }
      }

      BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (state && !state->hover) {
        state->hover = true;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      track_leave(hwnd);
      return 0;
    case WM_MOUSELEAVE:
      if (state && (state->hover || state->pressed)) {
        state->hover = false;
        state->pressed = false;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (state && state->enabled) {
        state->pressed = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
      if (state && state->enabled && state->pressed) {
        state->pressed = false;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        toggle_state(*state);
      }
      return 0;
    case WM_KEYDOWN:
      if (state && state->enabled && (wParam == VK_SPACE || wParam == VK_RETURN)) {
        toggle_state(*state);
        return 0;
      }
      break;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_GETFONT:
      return reinterpret_cast<LRESULT>(g_settings.body);
    case WM_NCDESTROY:
      if (state) state->hwnd = nullptr;
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ensure_control_class(HINSTANCE inst) {
  static bool registered = false;
  if (registered) return true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = control_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kControlClass;
  registered = RegisterClassExW(&wc) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  return registered;
}

ControlState& add_control(SettingsWindow& s, int id, ControlKind kind,
                          const std::wstring& label) {
  ControlState state;
  state.id = id;
  state.kind = kind;
  state.label = label;
  controls().push_back(state);
  ControlState& stored = controls().back();
  HWND created = CreateWindowExW(
      0, kControlClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
      s.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), s.app->inst,
      &stored);
  stored.hwnd = created;
  return stored;
}

void place(int id, int x, int y, int w, int h) {
  ControlState* c = find_control(id);
  if (!c || !c->hwnd) return;
  SetWindowPos(c->hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  c->rect = RECT{x, y, x + w, y + h};
}

void set_label(int id, const std::wstring& text) {
  ControlState* c = find_control(id);
  if (!c) return;
  c->label = text;
  if (c->hwnd) InvalidateRect(c->hwnd, nullptr, FALSE);
}

void set_checked(int id, bool checked) {
  ControlState* c = find_control(id);
  if (!c || c->checked == checked) return;
  c->checked = checked;
  if (c->hwnd) InvalidateRect(c->hwnd, nullptr, FALSE);
}

void set_enabled(int id, bool enabled) {
  ControlState* c = find_control(id);
  if (!c || c->enabled == enabled) return;
  c->enabled = enabled;
  if (c->hwnd) {
    EnableWindow(c->hwnd, enabled ? TRUE : FALSE);
    InvalidateRect(c->hwnd, nullptr, FALSE);
  }
}

// --------------------------------------------------------------------- layout

constexpr int kIdOffset = 2000;
constexpr int kIdOffsetMinus = 2007;
constexpr int kIdFont = 2001;
constexpr int kIdWipe = 2002;
constexpr int kIdResetOffset = 2003;
constexpr int kIdOpenLyrics = 2004;
constexpr int kIdClearCache = 2005;
constexpr int kIdCheckUpdates = 2006;
constexpr int kIdDiagnostics = 2008;
constexpr int kIdModeWallpaper = 2010;
constexpr int kIdFullscreen = 2011;
constexpr int kIdTopmost = 2012;
constexpr int kIdTheme = 2013;
constexpr int kIdWallpaperIndex = 2014;
constexpr int kIdRotation = 2015;
constexpr int kIdInterval = 2016;
constexpr int kIdRandomStart = 2017;
constexpr int kIdDim = 2018;
constexpr int kIdCover = 2019;
constexpr int kIdResetDisplay = 2020;
constexpr int kIdModeWindow = 2021;
constexpr int kIdDesktopRestore = 2022;
constexpr int kIdShowTrack = 2023;
constexpr int kIdDimPaused = 2024;
constexpr int kIdDesktopScene = 2025;
constexpr int kIdMonitors = 2026;
constexpr int kIdFade = 2027;
constexpr int kIdShadow = 2028;
constexpr int kIdAutostart = 2029;
constexpr int kIdClose = 2100;

// One table drives the controls, their labels and their vertical rhythm. They
// used to be positioned by two hand-counted sequences, which is how a label
// ended up one line above its control and the DISPLAY column ran under the
// footer. Each column now has its own cursor, and a row may declare that it
// shares the previous row's line (a value sitting beside its checkbox).
struct RowSpec {
  int id;             // control id, or 0 for a label-only / sub-heading row
  int column;         // 0 = LYRICS, 1 = DISPLAY
  int kind;           // kLabelOnly, kSubHead, kCheck, kButton, kValue
  const wchar_t* text;
  bool same_line;     // draw on the previous row's baseline
};

constexpr int kTop = 124;
constexpr int kRowH = 24;
constexpr int kGap = 8;
constexpr int kGroupGap = 10;     // space before a sub-heading
constexpr int kLabelX = 34;
constexpr int kControlX = 200;    // where a column's controls start
constexpr int kColumnGap = 34;
constexpr int kLabelOnly = 0;
constexpr int kSubHead = 1;
constexpr int kCheck = 2;
constexpr int kButton = 3;
constexpr int kValue = 4;

const RowSpec kRows[] = {
    {0, 0, kLabelOnly, L"Sync, all songs", false},
    {kIdOffsetMinus, 0, kButton, nullptr, true},
    {kIdOffset, 0, kButton, nullptr, true},
    {0, 0, kLabelOnly, L"Text size", false},
    {kIdFont, 0, kValue, nullptr, true},
    {kIdWipe, 0, kCheck, nullptr, false},
    {kIdShowTrack, 0, kCheck, nullptr, false},
    {kIdDimPaused, 0, kCheck, nullptr, false},
    {0, 0, kLabelOnly, L"Fade speed", false},
    {kIdFade, 0, kValue, nullptr, true},
    {0, 0, kLabelOnly, L"Text shadow", false},
    {kIdShadow, 0, kValue, nullptr, true},
    {0, 0, kSubHead, L"LOCAL LYRICS AND UPDATES", false},
    {kIdOpenLyrics, 0, kButton, nullptr, false},
    {kIdClearCache, 0, kButton, nullptr, false},
    {kIdCheckUpdates, 0, kButton, nullptr, false},
    {kIdDiagnostics, 0, kButton, nullptr, false},
    {kIdResetOffset, 0, kButton, nullptr, false},
    {kIdAutostart, 0, kCheck, nullptr, false},

    {kIdModeWallpaper, 1, kCheck, nullptr, false},
    {0, 1, kLabelOnly, nullptr, false},
    {kIdDesktopRestore, 1, kButton, nullptr, false},
    {0, 1, kLabelOnly, L"Show on", false},
    {kIdMonitors, 1, kValue, nullptr, true},
    {kIdFullscreen, 1, kCheck, nullptr, false},
    {kIdModeWindow, 1, kCheck, nullptr, false},
    {kIdTopmost, 1, kCheck, nullptr, false},
    {kIdDesktopScene, 1, kCheck, nullptr, false},
    {kIdCover, 1, kCheck, nullptr, false},
    {0, 1, kLabelOnly, L"Theme", false},
    {kIdTheme, 1, kValue, nullptr, true},
    {0, 1, kLabelOnly, L"Cinematic wallpaper", false},
    {kIdWallpaperIndex, 1, kValue, nullptr, true},
    {kIdRotation, 1, kCheck, nullptr, false},
    {kIdInterval, 1, kValue, nullptr, true},
    {kIdRandomStart, 1, kCheck, nullptr, false},
    {0, 1, kLabelOnly, L"Background darkness", false},
    {kIdDim, 1, kValue, nullptr, true},
    {kIdResetDisplay, 1, kButton, nullptr, false},
};

constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

int column_width(int w) { return (w - kLabelX * 2 - kColumnGap) / 2; }
int column_x(int column, int w) { return kLabelX + column * (column_width(w) + kColumnGap); }

// Y top of every row. Shared by layout() and paint(), so a label can never be
// drawn somewhere its control is not. Each column keeps its own cursor, and a
// `same_line` row reuses the previous row's top instead of advancing.
int row_top(int index) {
  int cursor[2] = {kTop, kTop};
  int last[2] = {kTop, kTop};
  int top = kTop;
  for (int i = 0; i <= index; ++i) {
    const RowSpec& spec = kRows[i];
    const int column = spec.column;
    if (spec.same_line) {
      top = last[column];
    } else {
      top = cursor[column];
      if (spec.kind == kSubHead) top += kGroupGap;
    }
    last[column] = top;
    cursor[column] = top + kRowH + kGap;
  }
  return top;
}

// The dialog is sized from the table, not from a guessed constant. Returns the
// client height needed for every row plus the header and footer.
int content_height() {
  const int bottom = row_top(kRowCount - 1) + kRowH;
  // room for the footer rule, the hint line and the close button, plus 8px of
  // breathing space under the last row.
  return bottom + 16 + 78;
}

void layout(SettingsWindow& s, int w, int h) {
  const int width = column_width(w);
  const int value_w = std::min(170, width - kControlX);

  for (int i = 0; i < kRowCount; ++i) {
    const RowSpec& spec = kRows[i];
    if (spec.kind == kLabelOnly || spec.kind == kSubHead) continue;
    const int x = column_x(spec.column, w);
    const int y = row_top(i);
    if (spec.same_line) {
      // Second half of a shared row: the value sits to the right of the
      // checkbox, which layout() narrows so the two cannot overlap.
      place(spec.id, x + kControlX, y, std::min(116, width - kControlX), kRowH);
    } else if (spec.kind == kCheck) {
      const bool shares = i + 1 < kRowCount && kRows[i + 1].same_line;
      place(spec.id, x, y, shares ? kControlX - 12 : width - 20, kRowH);
    } else if (spec.kind == kValue) {
      place(spec.id, x + kControlX, y, value_w, kRowH);
    } else {
      place(spec.id, x, y, std::min(200, width - 20), kRowH);
    }
  }

  const RECT close{w - 152, h - 62, w - 30, h - 22};
  s.close_rect = close;
}

void refresh_values(SettingsWindow& s) {
  Config& cfg = s.app->cfg;
  wchar_t buf[96];
  std::swprintf(buf, 96, L"%+.2f s", cfg.offset_ms / 1000.0);
  // Both nudge buttons keep their own caption; the value is painted by the
  // window next to them, so there is exactly one place it can come from.
  set_label(kIdOffsetMinus, L"\u22120.25 s");
  set_label(kIdOffset, L"+0.25 s");
  std::swprintf(buf, 96, L"%.0f%%", (cfg.font_scale - 1.0) * 100.0 + 100.0);
  set_label(kIdFont, buf);
  set_checked(kIdWipe, cfg.wipe);
  // Three modes, exactly one ticked.
  set_checked(kIdModeWallpaper, cfg.mode() == DisplayMode::Desktop);
  set_checked(kIdFullscreen, cfg.mode() == DisplayMode::Fullscreen);
  set_checked(kIdModeWindow, cfg.mode() == DisplayMode::Window);
  set_checked(kIdShowTrack, cfg.show_track);
  set_checked(kIdDimPaused, cfg.dim_paused);
  set_checked(kIdDesktopScene, cfg.desktop_scene);
  set_checked(kIdAutostart, autostart_enabled());
  set_label(kIdFade, cfg.fade_ms <= 350 ? L"Fast" : (cfg.fade_ms >= 1000 ? L"Slow" : L"Normal"));
  set_label(kIdShadow, cfg.shadow <= 0 ? L"Off" : (cfg.shadow >= 2 ? L"Strong" : L"Soft"));
  {
    const MonitorChoice choice = parse_monitor_choice(cfg.desktop_monitors);
    std::wstring label = L"All displays";
    if (choice.primary_only) label = L"Main display only";
    else if (!choice.all) label = L"Display " + utf8_to_wide(format_monitor_choice(choice));
    set_label(kIdMonitors, label.c_str());
  }
  {
    set_label(kIdDesktopRestore,
              cfg.mode() != DisplayMode::Desktop
                  ? L"Restore desktop mode"
                  : (desktop_layer_attached(*s.app) ? L"Check attachment again"
                                                    : L"Try attaching again"));
  }
  set_checked(kIdTopmost, cfg.topmost && cfg.mode() != DisplayMode::Desktop);
  set_checked(kIdCover, cfg.cover);
  // The renderer owns the theme table; a second list here was in a different
  // order and named every theme wrongly.
  set_label(kIdTheme, theme_name(cfg.theme));
  // Ask the renderer for the name: the settings window previously carried its
  // own list in a different order and its own modulus, so it showed the wrong
  // scene and could not represent a fourth choice at all.
  set_label(kIdWallpaperIndex, wallpaper_name(cfg.wallpaper_index));
  set_checked(kIdRotation, cfg.wallpaper_auto_rotate);
  std::swprintf(buf, 96, L"every %d min", cfg.wallpaper_interval_minutes);
  set_label(kIdInterval, buf);
  set_checked(kIdRandomStart, cfg.wallpaper_random_start);
  std::swprintf(buf, 96, L"%d%%", cfg.wallpaper_dim);
  set_label(kIdDim, buf);
  // Always-on-top cannot apply to the desktop layer; disable rather than offer
  // a switch that silently does nothing.
  set_enabled(kIdTopmost, cfg.mode() != DisplayMode::Desktop);
  set_enabled(kIdInterval, cfg.wallpaper_auto_rotate);
  InvalidateRect(s.hwnd, nullptr, TRUE);
}

void repaint_all(SettingsWindow& s) {
  InvalidateRect(s.hwnd, nullptr, TRUE);
  for (ControlState& c : controls()) {
    if (c.hwnd) InvalidateRect(c.hwnd, nullptr, TRUE);
  }
}

void commit(SettingsWindow& s, bool rebuild) {
  s.app->cfg.save();
  if (rebuild) {
    s.app->rebuild_layout(true);
    s.app->invalidate();
  }
  refresh_values(s);
  repaint_all(s);
}

// --------------------------------------------------------------------- paint

void draw_mark(HDC dc, int x, int y, int size) {
  // The brand mark at dialog scale: paper tile, margin rule, three lines with
  // the middle one lit. Kept in proportion with scripts/brand.py.
  HBRUSH paper = CreateSolidBrush(RGB(243, 239, 230));
  HPEN none = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
  HGDIOBJ old_brush = SelectObject(dc, paper);
  HGDIOBJ old_pen = SelectObject(dc, none);
  RoundRect(dc, x, y, x + size, y + size, size / 4, size / 4);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(paper);
  DeleteObject(none);

  auto bar = [&](int bx, int by, int bw, int bh, COLORREF colour) {
    HBRUSH b = CreateSolidBrush(colour);
    HGDIOBJ old = SelectObject(dc, b);
    HGDIOBJ old_p = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, bx, by, bx + bw, by + bh, bh, bh);
    SelectObject(dc, old);
    SelectObject(dc, old_p);
    DeleteObject(b);
  };
  const int ink = RGB(11, 10, 15);
  bar(x + (size * 134) / 1000, y + (size * 215) / 1000, size / 45,
      (size * 570) / 1000, ink);
  bar(x + (size * 240) / 1000, y + (size * 314) / 1000, (size * 340) / 1000,
      (size * 75) / 1000, RGB(96, 91, 84));
  bar(x + (size * 240) / 1000, y + (size * 448) / 1000, (size * 420) / 1000,
      (size * 125) / 1000, RGB(245, 165, 36));
  bar(x + (size * 660) / 1000, y + (size * 448) / 1000, (size * 160) / 1000,
      (size * 125) / 1000, RGB(150, 146, 138));
  bar(x + (size * 240) / 1000, y + (size * 632) / 1000, (size * 265) / 1000,
      (size * 75) / 1000, RGB(96, 91, 84));
}

void paint(SettingsWindow& s, HWND hwnd) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT client{};
  GetClientRect(hwnd, &client);
  const int w = client.right, h = client.bottom;
  if (w <= 0 || h <= 0) {
    EndPaint(hwnd, &ps);
    return;
  }
  const Palette p = palette_for(s.app->cfg.theme);

  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  HBRUSH bg = CreateSolidBrush(p.background);
  FillRect(mem, &client, bg);
  DeleteObject(bg);
  SetBkMode(mem, TRANSPARENT);

  // Header: the mark, the title, and one line of orientation.
  draw_mark(mem, 30, 24, 40);
  SelectObject(mem, s.heading);
  SetTextColor(mem, p.text);
  RECT title{84, 20, w - 30, 52};
  DrawTextW(mem, L"Settings", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  SelectObject(mem, s.small);
  SetTextColor(mem, p.muted);
  RECT subtitle{84, 50, w - 30, 74};
  DrawTextW(mem,
            L"Changes apply immediately and are saved automatically.",
            -1, &subtitle, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

  // Column rule + section headings.
  RECT split{w / 2, 100, w / 2 + 1, h - 86};
  HBRUSH border = CreateSolidBrush(p.border);
  FillRect(mem, &split, border);
  DeleteObject(border);

  SelectObject(mem, s.section);
  SetTextColor(mem, p.accent);
  RECT left_head{34, 96, w / 2 - 20, 118};
  DrawTextW(mem, L"LYRICS", -1, &left_head, DT_LEFT | DT_SINGLELINE);
  RECT right_head{w / 2 + 30, 96, w - 30, 118};
  DrawTextW(mem, L"DISPLAY", -1, &right_head, DT_LEFT | DT_SINGLELINE);

  // Labels are painted here, not as windows, so they always match the client.
  // They are driven by the same row table as the controls.
  SelectObject(mem, s.body);
  SetTextColor(mem, p.text);
  auto label = [&](const wchar_t* text, int x, int y, int cw, bool muted = false) {
    SetTextColor(mem, muted ? p.muted : p.text);
    RECT r{x, y, x + cw, y + kRowH};
    DrawTextW(mem, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  };
  const int column_w_paint = column_width(w);
  for (int i = 0; i < kRowCount; ++i) {
    const RowSpec& spec = kRows[i];
    const int x = column_x(spec.column, w);
    const int y = row_top(i);
    if (spec.kind == kSubHead) {
      SelectObject(mem, s.section);
      SetTextColor(mem, p.muted);
      RECT r{x, y, x + column_w_paint, y + kRowH};
      DrawTextW(mem, spec.text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      SelectObject(mem, s.body);
      continue;
    }
    if (spec.kind == kLabelOnly && !spec.text) {
      // The desktop attachment line: what is true now, not what was asked for.
      const std::wstring status =
          s.app->cfg.mode() == DisplayMode::Desktop
              ? utf8_to_wide(desktop_layer_status(*s.app))
              : std::wstring(L"Desktop mode is off: lyrics are in a window");
      const bool trouble = s.app->cfg.mode() == DisplayMode::Desktop &&
                           desktop_layer_trouble(*s.app);
      SelectObject(mem, s.small);
      SetTextColor(mem, trouble ? RGB(240, 170, 90) : p.muted);
      RECT r{x + 28, y, x + column_w_paint, y + kRowH};
      DrawTextW(mem, status.c_str(), -1, &r,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      SelectObject(mem, s.body);
      continue;
    }
    if (spec.kind == kLabelOnly) {
      label(spec.text, x, y, kControlX - 12);
      continue;
    }
    if (spec.same_line) {
      // The nudge pair shares the "Sync offset" line; the current value is
      // painted after the pair so the row explains itself.
      if (spec.id == kIdOffset) {
        // The current offset is painted right after the row label, in the gap
        // before the buttons: there is no space to its right inside the column,
        // and it must not spill into DISPLAY.
        wchar_t offset_text[32];
        std::swprintf(offset_text, 32, L"%+.2f s", s.app->cfg.offset_ms / 1000.0);
        label(offset_text, x + 96, y, kControlX - 104, true);
      }
    }
  }

  // Footer.
  RECT footer_rule{30, h - 78, w - 30, h - 77};
  border = CreateSolidBrush(p.border);
  FillRect(mem, &footer_rule, border);
  DeleteObject(border);
  SelectObject(mem, s.small);
  SetTextColor(mem, p.muted);
  RECT hint{30, h - 70, w - 180, h - 40};
  DrawTextW(mem,
            L"Log, lyric cache and your own .lrc files: %LOCALAPPDATA%\\YouTubeLyrics",
            -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  // Close button.
  const bool hot = s.close_hot || s.close_pressed;
  HBRUSH button = CreateSolidBrush(p.panel);
  FillRect(mem, &s.close_rect, button);
  DeleteObject(button);
  HPEN edge = CreatePen(PS_SOLID, 1, hot ? p.accent : p.border);
  HGDIOBJ old_pen = SelectObject(mem, edge);
  HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
  Rectangle(mem, s.close_rect.left, s.close_rect.top, s.close_rect.right + 1,
            s.close_rect.bottom + 1);
  SelectObject(mem, old_pen);
  SelectObject(mem, old_brush);
  DeleteObject(edge);
  SelectObject(mem, s.body);
  SetTextColor(mem, hot ? p.accent : p.text);
  RECT close_text = s.close_rect;
  DrawTextW(mem, L"Close", -1, &close_text,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(hwnd, &ps);
}

// ------------------------------------------------------------------ commands

void handle_command(SettingsWindow& s, int id) {
  App& app = *s.app;
  switch (id) {
    case kIdOffset:
      app.nudge_offset(250);
      refresh_values(s);
      return;
    case kIdOffsetMinus:
      app.nudge_offset(-250);
      refresh_values(s);
      return;
    case kIdFont:
      app.set_font_scale(app.cfg.font_scale >= 1.55 ? 0.70
                                                    : app.cfg.font_scale + 0.1);
      refresh_values(s);
      return;
    case kIdResetOffset:
      app.reset_offsets();
      refresh_values(s);
      return;
    case kIdDiagnostics:
      app.export_diagnostics();
      return;
    case kIdOpenLyrics: {
      const std::wstring folder = utf8_to_wide(appdata_dir() + "\\lyrics");
      if (!CreateDirectoryW(folder.c_str(), nullptr) &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        app.show_toast("Could not create the local lyrics folder.");
      } else if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                     s.hwnd, L"open", folder.c_str(), nullptr, nullptr,
                     SW_SHOWNORMAL)) <= 32) {
        app.show_toast("Could not open the local lyrics folder.");
      }
      return;
    }
    case kIdClearCache: {
      const int removed = lyrics_clear_cache();
      if (removed < 0) {
        app.show_toast("Could not clear the automatic lyrics cache.");
      } else if (removed == 0) {
        app.show_toast("The automatic lyrics cache is already empty.");
      } else {
        app.show_toast("Cleared " + std::to_string(removed) + " cached sheets; "
                       "press R in the display to reload.");
      }
      return;
    }
    case kIdCheckUpdates:
      app.show_toast("Checking for updates…", 2200);
      update_request_check(app, true);
      return;
    case kIdModeWallpaper:
    case kIdFullscreen:
    case kIdModeWindow: {
      // One choice of three. Picking the mode that is already active changes
      // nothing (refresh_values puts its tick back).
      const DisplayMode want = id == kIdModeWallpaper
                                   ? DisplayMode::Desktop
                                   : (id == kIdFullscreen ? DisplayMode::Fullscreen
                                                          : DisplayMode::Window);
      if (want != app.cfg.mode()) app.set_display_mode(want);
      refresh_values(s);
      repaint_all(s);
      return;
    }
    case kIdDesktopRestore: {
      // The one-click way back to the wallpaper, and the manual retry when
      // Explorer would not take the layer.
      if (app.cfg.mode() != DisplayMode::Desktop) {
        app.set_display_mode(DisplayMode::Desktop);
      } else {
        desktop_layer_retry(app);
      }
      refresh_values(s);
      repaint_all(s);
      return;
    }
    case kIdMonitors: {
      // All -> main only -> display 1 -> display 2 ... -> all. The layer
      // notices the new choice on its next check and rebuilds itself.
      const int count = std::max(1, desktop_layer_display_count());
      const MonitorChoice now_choice = parse_monitor_choice(app.cfg.desktop_monitors);
      std::string next = "all";
      if (now_choice.all) {
        next = "primary";
      } else if (now_choice.primary_only) {
        next = count > 1 ? "1" : "all";
      } else if (now_choice.numbers.size() == 1 && now_choice.numbers[0] < count) {
        next = std::to_string(now_choice.numbers[0] + 1);
      }
      app.cfg.desktop_monitors = next;
      app.cfg.save();
      desktop_layer_tick(app, true);
      refresh_values(s);
      repaint_all(s);
      return;
    }
    case kIdFade:
      app.cfg.fade_ms = app.cfg.fade_ms <= 350 ? 600 : (app.cfg.fade_ms >= 1000 ? 300 : 1200);
      break;
    case kIdShadow:
      app.cfg.shadow = (app.cfg.shadow + 1) % 3;
      break;
    case kIdAutostart: {
      const bool want = find_control(kIdAutostart)->checked;
      if (!set_autostart(want)) app.show_toast("Windows did not allow the startup entry to be changed");
      refresh_values(s);
      repaint_all(s);
      return;
    }
    case kIdShowTrack:
      app.cfg.show_track = find_control(kIdShowTrack)->checked;
      break;
    case kIdDimPaused:
      app.cfg.dim_paused = find_control(kIdDimPaused)->checked;
      break;
    case kIdDesktopScene:
      app.cfg.desktop_scene = find_control(kIdDesktopScene)->checked;
      app.view.bg_theme = -1;
      break;
    case kIdTopmost: {
      const bool want = find_control(kIdTopmost)->checked;
      if (want != app.cfg.topmost) app.toggle_topmost();
      refresh_values(s);
      repaint_all(s);
      return;
    }
    case kIdCover:
      app.cfg.cover = find_control(kIdCover)->checked;
      app.view.bg_theme = -1;
      refresh_art(app);
      break;
    case kIdTheme:
      app.cfg.theme = (app.cfg.theme + 1) % 4;
      app.view.bg_theme = -1;
      break;
    case kIdWallpaperIndex:
      app.next_wallpaper();
      break;
    case kIdRotation:
      app.cfg.wallpaper_auto_rotate = find_control(kIdRotation)->checked;
      app.view.wallpaper_changed_at = app.now_ms();
      break;
    case kIdInterval:
      app.cycle_wallpaper_interval();
      break;
    case kIdDim:
      app.cycle_wallpaper_dim();
      break;
    case kIdRandomStart:
      app.cfg.wallpaper_random_start = find_control(kIdRandomStart)->checked;
      break;
    case kIdWipe:
      app.cfg.wipe = find_control(kIdWipe)->checked;
      break;
    case kIdResetDisplay: {
      // One predictable way back to the shipped look, instead of asking someone
      // to hunt through the tray menu for each switch.
      const Config defaults;
      app.cfg.offset_ms = defaults.offset_ms;
      app.cfg.font_scale = defaults.font_scale;
      app.cfg.wipe = defaults.wipe;
      app.cfg.cover = defaults.cover;
      app.cfg.theme = defaults.theme;
      app.cfg.topmost = defaults.topmost;
      app.cfg.wallpaper_dim = defaults.wallpaper_dim;
      app.cfg.wallpaper_auto_rotate = defaults.wallpaper_auto_rotate;
      app.cfg.wallpaper_interval_minutes = defaults.wallpaper_interval_minutes;
      app.cfg.wallpaper_random_start = defaults.wallpaper_random_start;
      app.view.bg_theme = -1;
      app.cfg.wallpaper_index = defaults.wallpaper_index;
      app.cfg.fullscreen = defaults.fullscreen;
      app.cfg.desktop_scene = defaults.desktop_scene;
      app.cfg.show_track = defaults.show_track;
      app.cfg.dim_paused = defaults.dim_paused;
      app.cfg.fade_ms = defaults.fade_ms;
      app.cfg.shadow = defaults.shadow;
      app.cfg.desktop_monitors = defaults.desktop_monitors;
      app.view.bg_wallpaper = -1;
      app.view.wallpaper_changed_at = app.now_ms();
      app.set_display_mode(DisplayMode::Desktop);
      app.apply_display_mode();
      refresh_values(s);
      repaint_all(s);
      return;
    }
    case kIdClose:
      DestroyWindow(s.hwnd);
      return;
    default:
      return;
  }
  commit(s, true);
}

LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  SettingsWindow& s = g_settings;
  switch (msg) {
    case WM_CREATE: {
      const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
      s.app = static_cast<App*>(create->lpCreateParams);
      s.hwnd = hwnd;
      const Palette p = palette_for(s.app->cfg.theme);
      s.background = CreateSolidBrush(p.background);
      s.panel = CreateSolidBrush(p.panel);
      s.body = make_font(15, FW_NORMAL);
      s.heading = make_font(26, FW_SEMIBOLD);
      s.small = make_font(13, FW_NORMAL);
      s.section = make_font(12, FW_BOLD);

      controls().clear();
      controls().reserve(24);
      add_control(s, kIdOffset, ControlKind::Button, L"Later +0.25 s");
      add_control(s, kIdOffsetMinus, ControlKind::Button, L"Earlier \u22120.25 s");
      add_control(s, kIdFont, ControlKind::Button, L"100%");
      add_control(s, kIdWipe, ControlKind::Check, L"Karaoke wipe on the active line");
      add_control(s, kIdResetOffset, ControlKind::Button, L"Reset sync correction");
      add_control(s, kIdOpenLyrics, ControlKind::Button, L"Open lyrics folder");
      add_control(s, kIdClearCache, ControlKind::Button, L"Clear lyrics cache");
      add_control(s, kIdCheckUpdates, ControlKind::Button, L"Check for updates");
      add_control(s, kIdDiagnostics, ControlKind::Button, L"Save diagnostic report");
      add_control(s, kIdModeWallpaper, ControlKind::Check,
                  L"Desktop wallpaper (behind the icons)");
      add_control(s, kIdDesktopRestore, ControlKind::Button, L"Restore desktop mode");
      add_control(s, kIdFullscreen, ControlKind::Check, L"Full screen (in front)");
      add_control(s, kIdModeWindow, ControlKind::Check, L"Ordinary window");
      add_control(s, kIdMonitors, ControlKind::Button, L"All displays");
      add_control(s, kIdFade, ControlKind::Button, L"Normal");
      add_control(s, kIdShadow, ControlKind::Button, L"Soft");
      add_control(s, kIdAutostart, ControlKind::Check, L"Start with Windows");
      add_control(s, kIdShowTrack, ControlKind::Check, L"Show artist and title");
      add_control(s, kIdDimPaused, ControlKind::Check, L"Dim the lyrics while paused");
      add_control(s, kIdDesktopScene, ControlKind::Check,
                  L"Cinematic scene instead of my wallpaper");
      add_control(s, kIdTopmost, ControlKind::Check, L"Above other windows");
      add_control(s, kIdCover, ControlKind::Check, L"Album-art tint");
      add_control(s, kIdTheme, ControlKind::Button, L"Midnight");
      add_control(s, kIdWallpaperIndex, ControlKind::Button, L"Rainfall District");
      add_control(s, kIdRotation, ControlKind::Check, L"Rotate automatically");
      add_control(s, kIdInterval, ControlKind::Button, L"30 min");
      add_control(s, kIdRandomStart, ControlKind::Check, L"A different scene each launch");
      add_control(s, kIdDim, ControlKind::Button, L"12%");
      add_control(s, kIdResetDisplay, ControlKind::Button, L"Reset display settings");
      refresh_values(s);
      return 0;
    }
    case WM_SIZE:
      layout(s, LOWORD(lParam), HIWORD(lParam));
      return 0;
    case WM_ERASEBKGND:
      return 1;  // fully painted in WM_PAINT
    case WM_PAINT:
      paint(s, hwnd);
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (HIWORD(wParam) == BN_CLICKED) {
        handle_command(s, id);
        return 0;
      }
      break;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const bool hot = PtInRect(&s.close_rect, pt) != 0;
      if (hot != s.close_hot) {
        s.close_hot = hot;
        InvalidateRect(hwnd, &s.close_rect, FALSE);
      }
      track_leave(hwnd);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (s.close_hot || s.close_pressed) {
        s.close_hot = false;
        s.close_pressed = false;
        InvalidateRect(hwnd, &s.close_rect, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (PtInRect(&s.close_rect, pt)) s.close_pressed = true;
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const bool hit = PtInRect(&s.close_rect, pt) != 0;
      if (s.close_pressed) {
        s.close_pressed = false;
        if (hit) {
          DestroyWindow(hwnd);
          return 0;
        }
        InvalidateRect(hwnd, &s.close_rect, FALSE);
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_NCDESTROY:
      if (s.app) s.app->settings_window = nullptr;
      controls().clear();
      if (s.body) DeleteObject(s.body);
      if (s.heading) DeleteObject(s.heading);
      if (s.small) DeleteObject(s.small);
      if (s.section) DeleteObject(s.section);
      if (s.background) DeleteObject(s.background);
      if (s.panel) DeleteObject(s.panel);
      s = SettingsWindow();
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ensure_window_class(App& app) {
  static bool registered = false;
  if (registered) return true;
  if (!ensure_control_class(app.inst)) return false;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = settings_proc;
  wc.hInstance = app.inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kSettingsClass;
  wc.hIcon = LoadIconW(app.inst, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  registered = RegisterClassExW(&wc) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  return registered;
}

}  // namespace

void settings_show(App& app) {
  if (app.settings_window && IsWindow(app.settings_window)) {
    ShowWindow(app.settings_window, SW_SHOWNORMAL);
    SetForegroundWindow(app.settings_window);
    return;
  }
  if (!ensure_window_class(app)) {
    app.show_toast("Could not open the settings window.");
    return;
  }

  // The dialog is laid out in fixed pixels. On a high-DPI display that made it
  // a small window with small text, so it is created in a DPI-virtualised
  // context: Windows scales the whole window to the monitor, and GDI text in
  // it stays sharp. Unavailable before Windows 10 1607, where this is skipped.
  using SetThreadContextFn = HANDLE(WINAPI*)(HANDLE);
  SetThreadContextFn set_thread_context = nullptr;
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    set_thread_context = reinterpret_cast<SetThreadContextFn>(reinterpret_cast<void*>(
        GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
  }
  HANDLE previous_context = nullptr;
  if (set_thread_context) {
    previous_context = set_thread_context(reinterpret_cast<HANDLE>(-5));  // UNAWARE_GDISCALED
    if (!previous_context)
      previous_context = set_thread_context(reinterpret_cast<HANDLE>(-1));  // UNAWARE
  }
  struct RestoreContext {
    SetThreadContextFn fn;
    HANDLE previous;
    ~RestoreContext() {
      if (fn && previous) fn(previous);
    }
  } restore_context{set_thread_context, previous_context};

  const int width = 820;
  // +39 for a standard caption and border, so the client matches the table.
  const int height = content_height() + 39;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int x = std::max<int>(work.left + 16,
                              work.left + ((work.right - work.left) - width) / 2);
  const int y = std::max<int>(work.top + 16,
                              work.top + ((work.bottom - work.top) - height) / 2);
  HWND hwnd = CreateWindowExW(
      WS_EX_APPWINDOW, kSettingsClass, L"Youtube-Lyrics settings",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, width,
      height, nullptr, nullptr, app.inst, &app);
  if (!hwnd) {
    app.show_toast("Could not open the settings window.");
    return;
  }
  // A dark title bar keeps the frame from flashing white against the client.
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark,
                        sizeof(dark));
  app.settings_window = hwnd;
  ShowWindow(hwnd, SW_SHOWNORMAL);
  SetForegroundWindow(hwnd);
}

void settings_refresh(App& app) {
  if (!app.settings_window || !IsWindow(app.settings_window)) return;
  refresh_values(g_settings);
  repaint_all(g_settings);
}

}  // namespace ytl
