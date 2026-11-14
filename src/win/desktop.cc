// Youtube-Lyrics - the desktop wallpaper layer.
//
// Desktop mode is a small, separate window that lives inside Explorer's
// desktop surface: above the wallpaper, below the icons. It is drawn with
// per-pixel transparency, so the user's own wallpaper shows through and only
// the lyrics are added to it.
//
// Three facts shape this file:
//
//  * The shell has more than one layout. Until Windows 11 24H2 the icons live
//    in one top-level window and the wallpaper in a "WorkerW" behind it. From
//    24H2 both are children of Progman. Each needs a different parent and a
//    different place in the z-order, so the layout is detected, recorded and
//    reported rather than assumed.
//
//  * A child of an Explorer window dies with Explorer. That is why this is not
//    the main window: the tray icon, hotkeys and timers must survive, notice
//    the loss and attach a new layer when the shell comes back.
//
//  * Nothing here can be proved by compiling it. Every attempt records what
//    was found, what Windows answered and what was verified afterwards, and
//    "--desktop-test" runs the same code on a real machine and writes that
//    down together with a screenshot.
#include "app.h"
#include "version.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace ytl {
namespace {

constexpr wchar_t kLayerClass[] = L"YoutubeLyricsDesktopLayer";

App* g_layer_app = nullptr;

std::string text(const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return buffer;
}

std::string class_of(HWND hwnd) {
  if (!hwnd) return "(none)";
  if (!IsWindow(hwnd)) return "(gone)";
  wchar_t name[128] = L"";
  GetClassNameW(hwnd, name, 128);
  return wide_to_utf8(name);
}

std::string describe(HWND hwnd) {
  if (!hwnd) return "(none)";
  if (!IsWindow(hwnd)) return text("%p (no longer a window)", static_cast<void*>(hwnd));
  RECT r{};
  GetWindowRect(hwnd, &r);
  return text("%p %s, %ldx%ld at (%ld,%ld), %s, style 0x%08lX, ex 0x%08lX",
              static_cast<void*>(hwnd), class_of(hwnd).c_str(), r.right - r.left,
              r.bottom - r.top, r.left, r.top, IsWindowVisible(hwnd) ? "visible" : "hidden",
              static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
              static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
}

// --------------------------------------------------------- shell discovery

struct ShellDesktop {
  HWND progman = nullptr;
  HWND icons = nullptr;          // SHELLDLL_DefView
  HWND icons_parent = nullptr;   // Progman, or a top-level WorkerW
  HWND worker_child = nullptr;   // WorkerW that is a child of Progman (24H2+)
  HWND worker_behind = nullptr;  // top-level WorkerW after the icon window
  HWND host = nullptr;           // where the layer must be created
  HWND insert_after = nullptr;   // sibling to sit directly beneath, or null
  const char* strategy = "none";
  std::string why_not;
};

BOOL CALLBACK find_icon_window(HWND top, LPARAM param) {
  HWND view = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
  if (!view) return TRUE;
  auto* shell = reinterpret_cast<ShellDesktop*>(param);
  shell->icons = view;
  shell->icons_parent = top;
  return FALSE;
}

void look(ShellDesktop& shell) {
  shell.icons = nullptr;
  shell.icons_parent = nullptr;
  shell.worker_child = nullptr;
  shell.worker_behind = nullptr;
  shell.icons = FindWindowExW(shell.progman, nullptr, L"SHELLDLL_DefView", nullptr);
  if (shell.icons) {
    shell.icons_parent = shell.progman;
  } else {
    EnumWindows(find_icon_window, reinterpret_cast<LPARAM>(&shell));
  }
  shell.worker_child = FindWindowExW(shell.progman, nullptr, L"WorkerW", nullptr);
  if (shell.icons_parent && shell.icons_parent != shell.progman) {
    // The WorkerW that follows the icon window in the top-level z-order is the
    // one Explorer draws the wallpaper into.
    shell.worker_behind = FindWindowExW(nullptr, shell.icons_parent, L"WorkerW", nullptr);
  }
}

// `spawn` asks Explorer to create its wallpaper worker first. That message is
// undocumented but is what every wallpaper program relies on; it is harmless
// when the worker already exists.
ShellDesktop find_shell_desktop(bool spawn) {
  ShellDesktop shell;
  shell.progman = FindWindowW(L"Progman", nullptr);
  if (!shell.progman) {
    shell.why_not = "Explorer's desktop window (Progman) was not found; is Explorer running?";
    return shell;
  }
  look(shell);
  const bool have_worker = shell.worker_child || shell.worker_behind;
  if (spawn && !have_worker) {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(shell.progman, 0x052C, 0xD, 0x1, SMTO_ABORTIFHUNG, 1000, &ignored);
    look(shell);
    if (!shell.worker_child && !shell.worker_behind) {
      // The form older builds answer to.
      SendMessageTimeoutW(shell.progman, 0x052C, 0, 0, SMTO_ABORTIFHUNG, 1000, &ignored);
      for (int attempt = 0; attempt < 5; ++attempt) {
        look(shell);
        if (shell.worker_child || shell.worker_behind) break;
        Sleep(40);
      }
    }
  }
  if (!shell.icons) {
    shell.why_not = "the desktop icon layer (SHELLDLL_DefView) was not found";
    return shell;
  }

  if (shell.icons_parent == shell.progman && shell.worker_child) {
    // Windows 11 24H2 and later: icons and wallpaper are both children of
    // Progman. The layer goes between them.
    shell.host = shell.progman;
    shell.insert_after = shell.icons;
    shell.strategy = "between the icon layer and the wallpaper inside Progman "
                     "(Windows 11 24H2 layout)";
  } else if (shell.worker_behind) {
    // Windows 10 and Windows 11 before 24H2.
    shell.host = shell.worker_behind;
    shell.insert_after = nullptr;
    shell.strategy = "inside the WorkerW behind the icon window (classic layout)";
  } else if (shell.icons_parent == shell.progman) {
    // Explorer never split the desktop: Progman draws the wallpaper itself and
    // holds the icons. A child placed under the icon layer is still above the
    // wallpaper.
    shell.host = shell.progman;
    shell.insert_after = shell.icons;
    shell.strategy = "under the icon layer inside Progman (no WorkerW was created)";
  } else {
    shell.why_not = "the icon layer is in a separate window but no WorkerW follows it";
  }
  return shell;
}

// True when `layer` comes after `icons` among the children of their parent,
// i.e. it is drawn underneath the icons.
bool is_below_sibling(HWND layer, HWND icons) {
  if (!layer || !icons) return false;
  if (GetAncestor(layer, GA_PARENT) != GetAncestor(icons, GA_PARENT)) return false;
  int guard = 0;
  for (HWND next = GetWindow(icons, GW_HWNDNEXT); next && guard < 4096;
       next = GetWindow(next, GW_HWNDNEXT), ++guard) {
    if (next == layer) return true;
  }
  return false;
}

// ------------------------------------------------------------ DPI contexts
// SetParent/CreateWindow across processes misbehaves when parent and child
// were created under different DPI awareness. The layer is therefore created
// under the awareness of the Explorer window that will hold it. The functions
// are Windows 10 1607+, so they are looked up at run time.

using DpiContext = HANDLE;

struct DpiApi {
  DpiContext(WINAPI* window_context)(HWND) = nullptr;
  DpiContext(WINAPI* set_thread)(DpiContext) = nullptr;
  int(WINAPI* awareness)(DpiContext) = nullptr;
  UINT(WINAPI* window_dpi)(HWND) = nullptr;
};

const DpiApi& dpi_api() {
  static DpiApi api;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
      api.window_context = reinterpret_cast<DpiContext(WINAPI*)(HWND)>(
          reinterpret_cast<void*>(GetProcAddress(user32, "GetWindowDpiAwarenessContext")));
      api.set_thread = reinterpret_cast<DpiContext(WINAPI*)(DpiContext)>(
          reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
      api.awareness = reinterpret_cast<int(WINAPI*)(DpiContext)>(
          reinterpret_cast<void*>(GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext")));
      api.window_dpi = reinterpret_cast<UINT(WINAPI*)(HWND)>(
          reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
    }
  }
  return api;
}

// -1 unknown, 0 unaware, 1 system, 2 per-monitor.
int awareness_of(HWND hwnd) {
  const DpiApi& api = dpi_api();
  if (!hwnd || !api.window_context || !api.awareness) return -1;
  return api.awareness(api.window_context(hwnd));
}

const char* awareness_name(int value) {
  switch (value) {
    case 0: return "unaware";
    case 1: return "system";
    case 2: return "per-monitor";
    default: return "unknown";
  }
}

// ----------------------------------------------------------------- monitors

struct MonitorInfo {
  RECT rect{};
  bool primary = false;
};

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info)) {
    MonitorInfo m;
    m.rect = info.rcMonitor;
    m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    reinterpret_cast<std::vector<MonitorInfo>*>(param)->push_back(m);
  }
  return TRUE;
}

// Active displays, numbered the way a person would: left to right, then top
// to bottom. The numbering is by position, so it does not change when Windows
// re-enumerates the same arrangement in a different order.
std::vector<MonitorInfo> list_monitors() {
  std::vector<MonitorInfo> monitors;
  EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&monitors));
  std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
    if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
    return a.rect.top < b.rect.top;
  });
  return monitors;
}

int primary_index(const std::vector<MonitorInfo>& monitors) {
  for (size_t i = 0; i < monitors.size(); ++i)
    if (monitors[i].primary) return static_cast<int>(i);
  return 0;
}

// Everything that, if it changes, means the layer windows are in the wrong
// place: the arrangement of displays and which of them were asked for.
std::string layout_signature(const std::vector<MonitorInfo>& monitors, const std::string& choice) {
  std::string s = choice + "|";
  for (const MonitorInfo& m : monitors) {
    s += text("%ld,%ld,%ld,%ld%s;", m.rect.left, m.rect.top, m.rect.right, m.rect.bottom,
              m.primary ? "p" : "");
  }
  return s;
}

// --------------------------------------------------------------- the window

DesktopSurface* surface_of(DesktopLayer& d, HWND hwnd) {
  for (DesktopSurface& s : d.surfaces)
    if (s.hwnd == hwnd) return &s;
  return nullptr;
}

LRESULT CALLBACK layer_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;   // every click belongs to the desktop
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      // The layer is drawn with UpdateLayeredWindow and has nothing to paint.
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCDESTROY: {
      App* app = g_layer_app;
      if (app) {
        DesktopLayer& d = app->desktop;
        if (DesktopSurface* s = surface_of(d, hwnd)) {
          s->hwnd = nullptr;
          s->visible = false;
          // Destroyed from outside: Explorer exited, restarted, or rebuilt the
          // desktop. Noted here, acted on by the next tick; the main window is
          // untouched.
          if (!d.closing) d.lost_pending = true;
        }
      }
      break;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ensure_layer_class(HINSTANCE inst) {
  static bool registered = false;
  if (registered) return true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = layer_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kLayerClass;
  registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  return registered;
}

void free_buffer(DesktopSurface& s) {
  if (s.dc) {
    SelectObject(s.dc, s.old_bmp);
    DeleteDC(s.dc);
  }
  if (s.bmp) DeleteObject(s.bmp);
  s.dc = nullptr;
  s.bmp = nullptr;
  s.old_bmp = nullptr;
  s.bits = nullptr;
  s.buf_w = s.buf_h = 0;
}

// 32-bit top-down premultiplied buffer the size of the surface.
bool ensure_buffer(DesktopSurface& s) {
  if (s.dc && s.buf_w == s.w && s.buf_h == s.h) return true;
  free_buffer(s);
  if (s.w <= 0 || s.h <= 0) return false;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = s.w;
  info.bmiHeader.biHeight = -s.h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  HDC screen = GetDC(nullptr);
  s.dc = CreateCompatibleDC(screen);
  s.bmp = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &s.bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!s.dc || !s.bmp || !s.bits) {
    free_buffer(s);
    return false;
  }
  s.old_bmp = SelectObject(s.dc, s.bmp);
  s.buf_w = s.w;
  s.buf_h = s.h;
  return true;
}

// Screen rectangle -> client coordinates of `host`.
bool to_host(HWND host, const RECT& screen, RECT* client) {
  POINT corners[2] = {{screen.left, screen.top}, {screen.right, screen.bottom}};
  SetLastError(ERROR_SUCCESS);
  if (MapWindowPoints(nullptr, host, corners, 2) == 0 && GetLastError() != ERROR_SUCCESS)
    return false;
  *client = RECT{corners[0].x, corners[0].y, corners[1].x, corners[1].y};
  return true;
}

void destroy_surfaces(DesktopLayer& d) {
  d.closing = true;
  for (DesktopSurface& s : d.surfaces) {
    if (s.hwnd) DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    free_buffer(s);
  }
  d.closing = false;
  d.surfaces.clear();
  d.reference = -1;
  d.lost_pending = false;
}

// Creates one verified layer window per chosen display. All or nothing:
// returns false with d.detail / d.last_error set and no windows left behind.
bool create_surfaces(HINSTANCE inst, DesktopLayer& d, const std::string& choice_text) {
  destroy_surfaces(d);
  const ShellDesktop shell = find_shell_desktop(true);
  d.strategy = shell.strategy;
  d.host = shell.host;
  d.icons = shell.icons;
  d.last_error = 0;
  if (!shell.host) {
    d.detail = shell.why_not;
    return false;
  }
  if (!ensure_layer_class(inst)) {
    d.last_error = GetLastError();
    d.detail = text("the layer window class could not be registered (error %lu)",
                    static_cast<unsigned long>(d.last_error));
    return false;
  }
  const std::vector<MonitorInfo> monitors = list_monitors();
  d.monitors_total = static_cast<int>(monitors.size());
  if (monitors.empty()) {
    d.detail = "Windows reports no active display";
    return false;
  }
  const std::vector<int> chosen =
      select_monitors(parse_monitor_choice(choice_text), d.monitors_total, primary_index(monitors));

  // Create the children under the same DPI awareness as their Explorer parent.
  const DpiApi& api = dpi_api();
  DpiContext previous = nullptr;
  if (api.window_context && api.set_thread) {
    if (DpiContext host_context = api.window_context(shell.host))
      previous = api.set_thread(host_context);
  }
  bool ok = true;
  for (int index : chosen) {
    const MonitorInfo& monitor = monitors[static_cast<size_t>(index)];
    RECT place{};
    if (!to_host(shell.host, monitor.rect, &place)) {
      d.last_error = GetLastError();
      d.detail = text("display %d could not be mapped into the desktop surface (error %lu)",
                      index + 1, static_cast<unsigned long>(d.last_error));
      ok = false;
      break;
    }
    const int w = place.right - place.left, h = place.bottom - place.top;
    if (w < 64 || h < 64) {
      d.detail = text("display %d has no usable area inside the desktop surface", index + 1);
      ok = false;
      break;
    }
    SetLastError(ERROR_SUCCESS);
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kLayerClass,
                                L"", WS_CHILD, place.left, place.top, w, h, shell.host, nullptr,
                                inst, nullptr);
    if (!hwnd) {
      d.last_error = GetLastError();
      d.detail = text("Windows refused to create a window inside the desktop surface (error %lu)",
                      static_cast<unsigned long>(d.last_error));
      ok = false;
      break;
    }
    DesktopSurface s;
    s.hwnd = hwnd;
    s.monitor = monitor.rect;
    s.number = index + 1;
    s.primary = monitor.primary;
    s.w = w;
    s.h = h;
    d.surfaces.push_back(s);
    DesktopSurface& made = d.surfaces.back();

    // Directly beneath the icon layer when they share a parent, otherwise on
    // top inside the wallpaper worker.
    SetWindowPos(hwnd, shell.insert_after ? shell.insert_after : HWND_TOP, place.left, place.top,
                 w, h, SWP_NOACTIVATE);

    // Verify instead of trusting the return values.
    if (GetAncestor(hwnd, GA_PARENT) != shell.host) {
      d.detail = "a window was created but is not a child of the desktop surface";
      ok = false;
      break;
    }
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((ex & WS_EX_LAYERED) == 0 || (ex & WS_EX_TRANSPARENT) == 0 || (style & WS_CHILD) == 0) {
      d.detail = text("Windows changed the layer's styles (style 0x%08lX, ex 0x%08lX)",
                      static_cast<unsigned long>(style), static_cast<unsigned long>(ex));
      ok = false;
      break;
    }
    if (shell.insert_after) {
      made.below_icons = is_below_sibling(hwnd, shell.icons);
      if (!made.below_icons) {
        // In front of the icons is exactly what this mode must never be.
        d.detail = "the layer could not be ordered beneath the desktop icons";
        ok = false;
        break;
      }
    } else {
      // Classic layout: the whole host window is behind the icon window.
      made.below_icons = shell.worker_behind == shell.host;
    }
    if (api.window_dpi) made.dpi = api.window_dpi(hwnd);
  }
  if (previous && api.set_thread) api.set_thread(previous);
  if (!ok || d.surfaces.empty()) {
    if (ok) d.detail = "no display was selected";
    destroy_surfaces(d);
    return false;
  }

  // The lyrics are laid out once, for the largest chosen display; smaller
  // displays show that frame scaled down, never up.
  d.reference = 0;
  for (size_t i = 1; i < d.surfaces.size(); ++i) {
    const DesktopSurface& s = d.surfaces[i];
    const DesktopSurface& best = d.surfaces[static_cast<size_t>(d.reference)];
    const long long area = 1LL * s.w * s.h, best_area = 1LL * best.w * best.h;
    if (area > best_area || (area == best_area && s.primary)) d.reference = static_cast<int>(i);
  }
  d.w = d.surfaces[static_cast<size_t>(d.reference)].w;
  d.h = d.surfaces[static_cast<size_t>(d.reference)].h;
  d.signature = layout_signature(monitors, choice_text);
  d.detail.clear();
  return true;
}

bool push(DesktopSurface& s, HDC source, int alpha, DWORD* error) {
  SIZE size{s.w, s.h};
  POINT origin{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = static_cast<BYTE>(std::max(0, std::min(255, alpha)));
  blend.AlphaFormat = AC_SRC_ALPHA;
  SetLastError(ERROR_SUCCESS);
  if (!UpdateLayeredWindow(s.hwnd, nullptr, nullptr, &size, source, &origin, 0, &blend,
                           ULW_ALPHA)) {
    *error = GetLastError();
    return false;
  }
  if (!s.visible) {
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    s.visible = true;
  }
  return true;
}

// What a display is cleared to before the scaled frame is drawn on it: the
// same even veil render_frame() lays over the wallpaper.
Gdiplus::Color backdrop(const App& app) {
  if (app.cfg.desktop_scene) return Gdiplus::Color(255, 0, 0, 0);
  const int dim = std::max(0, std::min(60, app.cfg.wallpaper_dim));
  return Gdiplus::Color(static_cast<BYTE>(dim * 255 / 100), 0, 0, 0);
}

void announce_failure(App& app) {
  if (!app.hwnd) return;
  notify_from_tray(app, L"Lyrics could not be placed on the desktop",
                   L"Youtube-Lyrics is running but cannot draw on the desktop wallpaper layer, "
                   L"so it is showing nothing. It will keep trying. Right-click this icon for "
                   L"Settings, which shows the reason, or choose a window mode.");
}

void try_attach(App& app);

// Reports an event to the state machine and carries out what it asks for.
void dispatch(App& app, DesktopEvent event) {
  DesktopLayer& d = app.desktop;
  const DesktopPhase before = d.machine.phase;
  const DesktopAction action = desktop_machine_step(d.machine, event, app.now_ms());
  if (action.release_layer) destroy_surfaces(d);
  if (before != d.machine.phase) {
    logf("desktop: %s -> %s%s%s", desktop_phase_name(before), desktop_phase_name(d.machine.phase),
         d.detail.empty() ? "" : ": ", d.detail.c_str());
  }
  if (action.announce_failure) announce_failure(app);
  if (action.attempt_now) try_attach(app);
}

void try_attach(App& app) {
  DesktopLayer& d = app.desktop;
  g_layer_app = &app;
  if (!create_surfaces(app.inst, d, app.cfg.desktop_monitors)) {
    dispatch(app, DesktopEvent::AttachFailed);
    return;
  }
  d.consecutive_draw_failures = 0;
  dispatch(app, DesktopEvent::AttachSucceeded);
  if (d.machine.phase != DesktopPhase::Attached) {
    // The machine did not accept the success (the mode was switched off
    // meanwhile); leave nothing behind.
    destroy_surfaces(d);
    return;
  }
  d.last_check_at = app.now_ms();
  logf("desktop: %d layer(s) on %d display(s); %s; host %s; layout for %dx%d; host DPI "
       "awareness %s, layer %s",
       static_cast<int>(d.surfaces.size()), d.monitors_total, d.strategy.c_str(),
       class_of(d.host).c_str(), d.w, d.h, awareness_name(awareness_of(d.host)),
       awareness_name(awareness_of(d.surfaces[0].hwnd)));
  app.view.bg_theme = -1;
  app.on_resize(d.w, d.h);
  app.rebuild_layout(true);
}

std::string surface_list(const DesktopLayer& d) {
  if (d.surfaces.size() == 1 && d.monitors_total <= 1) return std::string();
  std::string s;
  for (const DesktopSurface& surface : d.surfaces) {
    if (!s.empty()) s += ", ";
    s += std::to_string(surface.number);
  }
  return text(" on display%s %s of %d", d.surfaces.size() == 1 ? "" : "s", s.c_str(),
              d.monitors_total);
}

}  // namespace

// ------------------------------------------------------------------ public

void desktop_layer_enable(App& app) { dispatch(app, DesktopEvent::Enable); }

void desktop_layer_disable(App& app) {
  dispatch(app, DesktopEvent::Disable);
  DesktopLayer& d = app.desktop;
  destroy_surfaces(d);
  d.host = nullptr;
  d.icons = nullptr;
  d.detail.clear();
  d.w = d.h = 0;
  app.presence = 0.0;
}

void desktop_layer_shell_restarted(App& app) {
  app.desktop.detail = "Explorer restarted";
  dispatch(app, DesktopEvent::ShellRestarted);
}

void desktop_layer_display_changed(App& app) { dispatch(app, DesktopEvent::DisplayChanged); }

void desktop_layer_power_resumed(App& app) { dispatch(app, DesktopEvent::PowerResumed); }

void desktop_layer_retry(App& app) { dispatch(app, DesktopEvent::ManualRetry); }

bool desktop_layer_attached(const App& app) {
  return desktop_machine_may_draw(app.desktop.machine) && !app.desktop.surfaces.empty();
}

bool desktop_layer_trouble(const App& app) {
  return desktop_machine_in_trouble(app.desktop.machine);
}

void desktop_layer_hide(App& app) {
  for (DesktopSurface& s : app.desktop.surfaces) {
    if (s.hwnd && s.visible) {
      ShowWindow(s.hwnd, SW_HIDE);
      s.visible = false;
    }
  }
}

void desktop_layer_tick(App& app, bool force) {
  DesktopLayer& d = app.desktop;
  if (d.machine.phase == DesktopPhase::Off) return;
  const int64_t now = app.now_ms();

  if (d.machine.phase == DesktopPhase::Attached) {
    if (d.lost_pending) {
      d.detail = "the desktop surface was closed by Explorer";
      dispatch(app, DesktopEvent::SurfaceLost);
    } else if (force || now - d.last_check_at >= 2000) {
      d.last_check_at = now;
      bool alive = IsWindow(d.host) != 0;
      for (const DesktopSurface& s : d.surfaces)
        alive = alive && s.hwnd && IsWindow(s.hwnd) && GetAncestor(s.hwnd, GA_PARENT) == d.host;
      if (!alive) {
        d.detail = "the desktop surface changed underneath the layer";
        dispatch(app, DesktopEvent::SurfaceLost);
      } else if (layout_signature(list_monitors(), app.cfg.desktop_monitors) != d.signature) {
        // A display was added, removed, moved, resized or rescaled, or a
        // different set of displays was chosen. Rebuild rather than patch.
        d.detail = "the display arrangement changed";
        dispatch(app, DesktopEvent::DisplayChanged);
      } else if (d.icons && IsWindow(d.icons) && GetAncestor(d.icons, GA_PARENT) == d.host) {
        // Explorer re-stacks its children when the wallpaper changes; make
        // sure every layer is still under the icons.
        for (DesktopSurface& s : d.surfaces) {
          if (!is_below_sibling(s.hwnd, d.icons)) {
            SetWindowPos(s.hwnd, d.icons, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          }
          s.below_icons = is_below_sibling(s.hwnd, d.icons);
          if (!s.below_icons) {
            d.detail = "the layer could not be kept beneath the desktop icons";
            dispatch(app, DesktopEvent::SurfaceLost);
            break;
          }
        }
      }
    }
  }

  if (desktop_machine_attempt_due(d.machine, app.now_ms())) try_attach(app);
}

bool desktop_layer_present(App& app, int alpha) {
  DesktopLayer& d = app.desktop;
  if (!desktop_layer_attached(app) || d.reference < 0) return false;
  DesktopSurface& ref = d.surfaces[static_cast<size_t>(d.reference)];
  DWORD error = 0;
  bool ok = ensure_buffer(ref);
  if (!ok) error = GetLastError();

  LARGE_INTEGER t0{}, t1{}, freq{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  if (ok) {
    Gdiplus::Bitmap frame(ref.buf_w, ref.buf_h, ref.buf_w * 4, PixelFormat32bppPARGB,
                          static_cast<BYTE*>(ref.bits));
    {
      Gdiplus::Graphics g(&frame);
      app.view.layer_transparent = true;
      render_frame(app, g, ref.buf_w, ref.buf_h);
      app.view.layer_transparent = false;
    }
    for (DesktopSurface& s : d.surfaces) {
      if (!ok) break;
      if (!s.hwnd) { ok = false; break; }
      if (s.w == ref.w && s.h == ref.h) {
        ok = push(s, ref.dc, alpha, &error);
        continue;
      }
      // A display of another size: the same frame, scaled to fit and centred.
      if (!ensure_buffer(s)) { ok = false; error = GetLastError(); break; }
      {
        Gdiplus::Bitmap target(s.buf_w, s.buf_h, s.buf_w * 4, PixelFormat32bppPARGB,
                               static_cast<BYTE*>(s.bits));
        Gdiplus::Graphics g(&target);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.Clear(backdrop(app));
        g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const double fit = std::min(static_cast<double>(s.w) / ref.w,
                                    static_cast<double>(s.h) / ref.h);
        const double cover = std::max(static_cast<double>(s.w) / ref.w,
                                      static_cast<double>(s.h) / ref.h);
        // A scene has to fill the display; lyrics alone must never be cropped.
        const double scale = app.cfg.desktop_scene ? cover : fit;
        const Gdiplus::REAL dw = static_cast<Gdiplus::REAL>(ref.w * scale);
        const Gdiplus::REAL dh = static_cast<Gdiplus::REAL>(ref.h * scale);
        g.DrawImage(&frame, Gdiplus::RectF((s.w - dw) / 2, (s.h - dh) / 2, dw, dh), 0, 0,
                    static_cast<Gdiplus::REAL>(ref.w), static_cast<Gdiplus::REAL>(ref.h),
                    Gdiplus::UnitPixel);
      }
      ok = push(s, s.dc, alpha, &error);
    }
  }
  QueryPerformanceCounter(&t1);

  if (ok) {
    const double ms = freq.QuadPart > 0
                          ? (t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)
                          : 0.0;
    ++d.presents;
    d.frame_ms_total += ms;
    d.frame_ms_max = std::max(d.frame_ms_max, ms);
    d.frame_ms_last = ms;
    d.consecutive_draw_failures = 0;
    return true;
  }

  ++d.present_failures;
  d.present_error = error;
  if (++d.consecutive_draw_failures >= 5) {
    // Windows will not draw the transparent layer. There is no substitute
    // picture: the layer is removed and the failure is reported.
    d.detail = text("Windows refused to draw the transparent layer (error %lu)",
                    static_cast<unsigned long>(error));
    logf("desktop: %s", d.detail.c_str());
    dispatch(app, DesktopEvent::DrawRefused);
  }
  return false;
}

bool desktop_layer_refade(App& app, int alpha) {
  DesktopLayer& d = app.desktop;
  if (!desktop_layer_attached(app) || d.reference < 0) return false;
  const DesktopSurface& ref = d.surfaces[static_cast<size_t>(d.reference)];
  if (!ref.dc) return false;  // nothing was ever drawn
  bool ok = true;
  DWORD error = 0;
  for (DesktopSurface& s : d.surfaces) {
    if (!s.hwnd || !s.visible) continue;
    HDC source = (s.w == ref.w && s.h == ref.h) ? ref.dc : s.dc;
    if (!source) continue;
    ok = push(s, source, alpha, &error) && ok;
  }
  return ok;
}

std::string desktop_layer_status(const App& app) {
  const DesktopLayer& d = app.desktop;
  const std::string why = d.detail.empty() ? std::string() : " (" + d.detail + ")";
  switch (d.machine.phase) {
    case DesktopPhase::Attached: {
      std::string s = "Attached beneath the desktop icons" + surface_list(d);
      if (d.machine.restores > 0) s += "; restored after Explorer restarted";
      if (d.consecutive_draw_failures > 0) s += "; drawing is failing";
      return s;
    }
    case DesktopPhase::Attaching:
      return d.machine.attempts > 0 ? "Not attached; trying again" + why : "Attaching\xE2\x80\xA6";
    case DesktopPhase::Restoring:
      return "Explorer restarted; attaching again" + (d.machine.attempts > 0 ? why : "");
    case DesktopPhase::Recovering:
      return "Display or power change; rebuilding" + (d.machine.attempts > 0 ? why : "");
    case DesktopPhase::Failed:
      return "Not working, nothing is shown: " +
             (d.detail.empty() ? std::string("unknown reason") : d.detail);
    default:
      return "Not in use (another mode is selected)";
  }
}

std::string desktop_layer_report(const App& app) {
  const DesktopLayer& d = app.desktop;
  auto row = [](const char* name, const std::string& value) {
    return text("  %-26s %s\r\n", (std::string(name) + ":").c_str(), value.c_str());
  };
  std::string r;
  r += row("Attachment state", desktop_phase_name(d.machine.phase));
  r += row("Status shown to the user", desktop_layer_status(app));
  if (!d.detail.empty()) r += row("Detail", d.detail);
  if (d.last_error) r += row("Last Windows error", std::to_string(d.last_error));
  r += row("Failed attempts / restores / recoveries",
           text("%d / %d / %d", d.machine.attempts, d.machine.restores, d.machine.recoveries));
  if (!d.strategy.empty()) r += row("Placement", d.strategy);
  r += row("Displays chosen", app.cfg.desktop_monitors);

  const std::vector<MonitorInfo> monitors = list_monitors();
  for (size_t i = 0; i < monitors.size(); ++i) {
    const RECT& m = monitors[i].rect;
    r += row(text("Display %d", static_cast<int>(i + 1)).c_str(),
             text("%ldx%ld at (%ld,%ld)%s", m.right - m.left, m.bottom - m.top, m.left, m.top,
                  monitors[i].primary ? ", primary" : ""));
  }

  if (d.surfaces.empty()) r += row("Layer windows", "none");
  for (const DesktopSurface& s : d.surfaces) {
    const std::string name = text("Layer, display %d", s.number);
    if (!s.hwnd || !IsWindow(s.hwnd)) {
      r += row(name.c_str(), "window is gone");
      continue;
    }
    r += row(name.c_str(), describe(s.hwnd));
    const HWND parent = GetAncestor(s.hwnd, GA_PARENT);
    r += row("  actual parent", describe(parent));
    r += row("  parent is the chosen host", parent == d.host ? "yes" : "NO");
    r += row("  below the icon layer", s.below_icons ? "confirmed" : "not confirmed");
    const LONG_PTR ex = GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
    const LONG_PTR style = GetWindowLongPtrW(s.hwnd, GWL_STYLE);
    r += row("  child/layered/click-through/no-activate",
             text("%s / %s / %s / %s", (style & WS_CHILD) ? "yes" : "NO",
                  (ex & WS_EX_LAYERED) ? "yes" : "NO", (ex & WS_EX_TRANSPARENT) ? "yes" : "NO",
                  (ex & WS_EX_NOACTIVATE) ? "yes" : "NO"));
    r += row("  caption / border / taskbar button",
             text("%s / %s / %s", (style & WS_CAPTION) == WS_CAPTION ? "YES" : "none",
                  (style & (WS_BORDER | WS_THICKFRAME)) ? "YES" : "none",
                  (ex & WS_EX_APPWINDOW) ? "YES" : "none (child window)"));
    r += row("  size / display",
             text("%dx%d / %ldx%ld at (%ld,%ld)%s", s.w, s.h, s.monitor.right - s.monitor.left,
                  s.monitor.bottom - s.monitor.top, s.monitor.left, s.monitor.top,
                  &s == &d.surfaces[static_cast<size_t>(std::max(0, d.reference))]
                      ? ", layout reference" : ", shows the reference frame scaled"));
    r += row("  DPI / awareness host / layer",
             text("%u / %s / %s", s.dpi, awareness_name(awareness_of(d.host)),
                  awareness_name(awareness_of(s.hwnd))));
    r += row("  shown now", s.visible ? "yes" : "no (idle)");
  }
  r += row("Drawing", "per-pixel transparency only (no opaque fallback)");
  r += row("Frames shown / failed",
           text("%d / %d%s", d.presents, d.present_failures,
                d.present_error ? text(" (last error %lu)",
                                       static_cast<unsigned long>(d.present_error)).c_str()
                                : ""));
  if (d.presents > 0) {
    r += row("Frame time avg / max / last",
             text("%.2f / %.2f / %.2f ms (render and present, all displays)",
                  d.frame_ms_total / d.presents, d.frame_ms_max, d.frame_ms_last));
  }
  r += row("Opacity now", text("%.0f%%", app.presence * 100.0));

  // What the shell looks like right now, without changing anything.
  const ShellDesktop shell = find_shell_desktop(false);
  r += row("Progman", describe(shell.progman));
  r += row("Icon layer (DefView)", describe(shell.icons));
  r += row("Icon layer parent", describe(shell.icons_parent));
  r += row("WorkerW inside Progman", describe(shell.worker_child));
  r += row("WorkerW behind icons", describe(shell.worker_behind));
  r += row("Placement available now",
           shell.host ? std::string(shell.strategy) : "none: " + shell.why_not);
  r += row("Virtual screen",
           text("%dx%d at (%d,%d), %d monitor(s)", GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_CYVIRTUALSCREEN), GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CMONITORS)));
  return r;
}

int desktop_layer_display_count() { return static_cast<int>(list_monitors().size()); }

// ------------------------------------------------------------ desktop test

namespace {

int png_encoder(CLSID* clsid) {
  UINT count = 0, bytes = 0;
  Gdiplus::GetImageEncodersSize(&count, &bytes);
  if (bytes == 0) return -1;
  std::vector<BYTE> storage(bytes);
  auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
  Gdiplus::GetImageEncoders(count, bytes, codecs);
  for (UINT i = 0; i < count; ++i) {
    if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
      *clsid = codecs[i].Clsid;
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Saves `area` of the screen. On failure `why` says which step failed.
bool save_screenshot(const RECT& area, const std::wstring& path, std::string* why) {
  const int w = area.right - area.left, h = area.bottom - area.top;
  if (w <= 0 || h <= 0) { *why = "the capture area is empty"; return false; }
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  bool saved = false;
  if (!screen || !mem || !bmp) {
    *why = text("no capture surface (error %lu)", static_cast<unsigned long>(GetLastError()));
  } else {
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL copied = BitBlt(mem, 0, 0, w, h, screen, area.left, area.top, SRCCOPY | CAPTUREBLT);
    const DWORD copy_error = GetLastError();
    SelectObject(mem, old);
    if (!copied) {
      *why = text("the screen could not be copied (error %lu)",
                  static_cast<unsigned long>(copy_error));
    } else {
      CLSID png{};
      if (png_encoder(&png) < 0) {
        *why = "no PNG encoder is installed";
      } else {
        Gdiplus::Bitmap image(bmp, nullptr);
        const Gdiplus::Status status = image.Save(path.c_str(), &png, nullptr);
        saved = status == Gdiplus::Ok;
        if (!saved) *why = text("the PNG could not be written (GDI+ status %d)", status);
      }
    }
  }
  if (bmp) DeleteObject(bmp);
  if (mem) DeleteDC(mem);
  if (screen) ReleaseDC(nullptr, screen);
  return saved;
}

void pump(int milliseconds) {
  const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
  while (GetTickCount64() < until) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(15);
  }
}

// Is the bare desktop (wallpaper or icon view) what is on screen at `p`? Only
// then does a screen pixel there say anything about the layer.
bool desktop_exposed_at(POINT p, const DesktopLayer& d) {
  HWND hit = WindowFromPoint(p);
  for (int depth = 0; hit && depth < 8; ++depth) {
    const std::string cls = class_of(hit);
    if (hit == d.host || cls == "Progman" || cls == "WorkerW" || cls == "SHELLDLL_DefView")
      return true;
    hit = GetAncestor(hit, GA_PARENT);
  }
  return false;
}

bool near_colour(COLORREF a, COLORREF b, int tolerance) {
  return std::abs(GetRValue(a) - GetRValue(b)) <= tolerance &&
         std::abs(GetGValue(a) - GetGValue(b)) <= tolerance &&
         std::abs(GetBValue(a) - GetBValue(b)) <= tolerance;
}

}  // namespace

// Exit code: 0 every check passed, 1 a check failed or the report could not be
// written, 2 nothing failed but at least one check could not be carried out.
int run_desktop_test(App& app, const std::string& path) {
  DesktopLayer& d = app.desktop;
  std::string out = "Youtube-Lyrics desktop integration test, version " YTL_VERSION_STRING "\r\n";
  out += "Runs the real attachment code on this computer for a few seconds. A card\r\n";
  out += "reading \"Youtube-Lyrics desktop test\" should appear on the wallpaper of each\r\n";
  out += "chosen display, BEHIND the desktop icons, and then vanish.\r\n\r\n";
  out += "For the drawing checks the desktop must be visible: minimise your windows\r\n";
  out += "(Win+D) before running. A check that could not be carried out is marked\r\n";
  out += "SKIP and is not counted as a pass.\r\n\r\n";
  int failed = 0, skipped = 0, passed = 0;
  auto record = [&](const char* name, int result, const std::string& detail) {
    // result: 1 pass, 0 fail, -1 skipped
    out += std::string(result > 0 ? "PASS  " : (result == 0 ? "FAIL  " : "SKIP  ")) + name + ": " +
           detail + "\r\n";
    if (result > 0) ++passed; else if (result == 0) ++failed; else ++skipped;
  };
  auto skip_rest = [&](std::initializer_list<const char*> names, const char* why) {
    for (const char* name : names) record(name, -1, why);
  };

  const HWND foreground_before = GetForegroundWindow();
  const std::vector<MonitorInfo> monitors = list_monitors();

  // 1. Shell discovery, reported before anything is created.
  const ShellDesktop shell = find_shell_desktop(true);
  record("shell-host", shell.host ? 1 : 0,
         shell.host ? std::string(shell.strategy) + "; host " + class_of(shell.host)
                    : shell.why_not);

  // 2. Attachment through the same state machine the program uses.
  desktop_layer_enable(app);
  for (int attempt = 0; attempt < 12 && !desktop_layer_attached(app); ++attempt) {
    pump(450);
    desktop_layer_tick(app, false);
    if (d.machine.phase == DesktopPhase::Failed) break;
  }
  const bool attached = desktop_layer_attached(app);
  record("attach", attached ? 1 : 0,
         attached ? text("%d layer window(s), state: %s", static_cast<int>(d.surfaces.size()),
                         desktop_phase_name(d.machine.phase))
                  : std::string(desktop_phase_name(d.machine.phase)) + ": " + d.detail);

  std::string screenshot_path = path;
  {
    const size_t dot = screenshot_path.find_last_of('.');
    const size_t slash = screenshot_path.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
      screenshot_path.erase(dot);
    screenshot_path += ".png";
  }

  if (!attached) {
    skip_rest({"parent", "styles", "below-icons", "monitor-bounds", "dpi", "draw", "visible",
               "transparent-drawing", "click-through", "no-focus", "screenshot"},
              "not attached, so there was nothing to measure");
  } else {
    // 3. Relationships and styles, read back from Windows for every layer.
    bool parent_ok = true, styles_ok = true, below_ok = true, bounds_ok = true;
    std::string parent_text, styles_text, bounds_text, dpi_text;
    for (const DesktopSurface& s : d.surfaces) {
      const HWND parent = GetAncestor(s.hwnd, GA_PARENT);
      parent_ok = parent_ok && parent == d.host && parent == shell.host;
      parent_text += text("display %d: %s; ", s.number, class_of(parent).c_str());
      const LONG_PTR style = GetWindowLongPtrW(s.hwnd, GWL_STYLE);
      const LONG_PTR ex = GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
      const bool good = (style & WS_CHILD) && !(style & (WS_CAPTION | WS_THICKFRAME | WS_SYSMENU)) &&
                        (ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT) &&
                        (ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW);
      styles_ok = styles_ok && good;
      styles_text += text("display %d: 0x%08lX / 0x%08lX; ", s.number,
                          static_cast<unsigned long>(style), static_cast<unsigned long>(ex));
      below_ok = below_ok && s.below_icons;
      RECT actual{};
      GetWindowRect(s.hwnd, &actual);
      const bool same = EqualRect(&actual, &s.monitor) != 0;
      bounds_ok = bounds_ok && same;
      bounds_text += text("display %d: layer %ldx%ld at (%ld,%ld), display %ldx%ld at (%ld,%ld); ",
                          s.number, actual.right - actual.left, actual.bottom - actual.top,
                          actual.left, actual.top, s.monitor.right - s.monitor.left,
                          s.monitor.bottom - s.monitor.top, s.monitor.left, s.monitor.top);
      dpi_text += text("display %d: layer DPI %u; ", s.number, s.dpi);
    }
    record("parent", parent_ok ? 1 : 0, parent_text + "expected " + class_of(shell.host));
    record("styles", styles_ok ? 1 : 0,
           styles_text + "need child, layered, click-through, no-activate; no caption, frame "
                         "or taskbar button");
    if (shell.insert_after) {
      record("below-icons", below_ok ? 1 : 0,
             below_ok ? "every layer follows the icon view among its siblings"
                      : "a layer is not ordered after the icon view");
    } else {
      record("below-icons", below_ok ? 1 : 0,
             "classic layout: the layers are inside the WorkerW that follows the icon window "
             "(order of the two top-level windows as found, not re-measured)");
    }
    record("monitor-bounds", bounds_ok ? 1 : 0, bounds_text);
    const int own = awareness_of(d.surfaces[0].hwnd), host = awareness_of(d.host);
    if (own < 0 || host < 0) {
      record("dpi", -1, dpi_text + "this Windows version cannot report DPI awareness");
    } else {
      // What matters is that layer and host agree and that the layer covers
      // its display in real pixels, which monitor-bounds measured.
      record("dpi", own == host && bounds_ok ? 1 : 0,
             dpi_text + text("awareness host %s, layer %s", awareness_name(host),
                             awareness_name(own)));
    }

    // 4. Drawing. Sample the screen before the card is shown.
    struct Sample { POINT p; COLORREF before; bool usable; bool inside_card; };
    std::vector<Sample> samples;
    const COLORREF card_colour = RGB(24, 96, 200);
    HDC screen = GetDC(nullptr);
    for (const DesktopSurface& s : d.surfaces) {
      const int mw = s.monitor.right - s.monitor.left, mh = s.monitor.bottom - s.monitor.top;
      for (int i = 0; i < 9; ++i) {
        // Inside the opaque swatch (a band across the middle of the card)...
        POINT in{s.monitor.left + mw * (30 + i * 5) / 100, s.monitor.top + mh * 50 / 100};
        // ...and in the part of the layer that is left fully transparent.
        POINT outp{s.monitor.left + mw * (10 + i * 10) / 100, s.monitor.top + mh * 12 / 100};
        samples.push_back({in, GetPixel(screen, in.x, in.y), desktop_exposed_at(in, d), true});
        samples.push_back({outp, GetPixel(screen, outp.x, outp.y), desktop_exposed_at(outp, d),
                           false});
      }
    }

    bool drawn = true;
    DWORD draw_error = 0;
    for (DesktopSurface& s : d.surfaces) {
      if (!ensure_buffer(s)) { drawn = false; draw_error = GetLastError(); break; }
      {
        Gdiplus::Bitmap target(s.buf_w, s.buf_h, s.buf_w * 4, PixelFormat32bppPARGB,
                               static_cast<BYTE*>(s.bits));
        Gdiplus::Graphics g(&target);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const Gdiplus::REAL cw = s.buf_w * 0.56f, ch = s.buf_h * 0.2f;
        const Gdiplus::RectF card((s.buf_w - cw) / 2, (s.buf_h - ch) / 2, cw, ch);
        Gdiplus::SolidBrush back(Gdiplus::Color(200, 12, 12, 18));
        g.FillRectangle(&back, card);
        // A fully opaque band that the screen samples can recognise.
        Gdiplus::SolidBrush swatch(Gdiplus::Color(255, GetRValue(card_colour),
                                                  GetGValue(card_colour), GetBValue(card_colour)));
        g.FillRectangle(&swatch, Gdiplus::RectF(card.X, s.buf_h * 0.5f - ch * 0.08f, cw,
                                                ch * 0.16f));
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font font(&family, std::max(14.0f, s.buf_h * 0.026f), Gdiplus::FontStyleBold,
                           Gdiplus::UnitPixel);
        Gdiplus::SolidBrush ink(Gdiplus::Color(255, 245, 245, 245));
        Gdiplus::StringFormat centre;
        centre.SetAlignment(Gdiplus::StringAlignmentCenter);
        const std::wstring label = utf8_to_wide(
            text("Youtube-Lyrics desktop test - display %d\nicons should be in front of this card",
                 s.number));
        g.DrawString(label.c_str(), -1, &font,
                     Gdiplus::RectF(card.X, card.Y + ch * 0.06f, cw, ch * 0.36f), &centre, &ink);
      }
      if (!push(s, s.dc, 255, &draw_error)) { drawn = false; break; }
    }
    record("draw", drawn ? 1 : 0,
           drawn ? "UpdateLayeredWindow accepted a per-pixel-alpha frame for every layer"
                 : text("drawing failed (error %lu)", static_cast<unsigned long>(draw_error)));
    pump(2000);

    bool alive = !d.surfaces.empty();
    for (const DesktopSurface& s : d.surfaces) alive = alive && s.hwnd && IsWindow(s.hwnd);
    if (!alive || !drawn) {
      if (!alive) record("visible", 0, "a layer window was destroyed during the test");
      else record("visible", -1, "nothing was drawn");
      skip_rest({"transparent-drawing", "click-through"}, "no drawn layer to measure");
    } else {
      bool visible = true;
      for (const DesktopSurface& s : d.surfaces) visible = visible && IsWindowVisible(s.hwnd);
      record("visible", visible ? 1 : 0,
             "IsWindowVisible for every layer (says the window is shown, not that nothing "
             "covers it)");

      // Screen pixels: the swatch must have appeared, the transparent part
      // must not have changed. Only points where the desktop is exposed count.
      int in_usable = 0, in_hit = 0, out_usable = 0, out_same = 0;
      for (const Sample& sample : samples) {
        if (!sample.usable || sample.before == CLR_INVALID) continue;
        const COLORREF now_colour = GetPixel(screen, sample.p.x, sample.p.y);
        if (now_colour == CLR_INVALID) continue;
        if (sample.inside_card) {
          ++in_usable;
          if (near_colour(now_colour, card_colour, 10)) ++in_hit;
        } else {
          ++out_usable;
          if (near_colour(now_colour, sample.before, 6)) ++out_same;
        }
      }
      const std::string counts =
          text("opaque band seen at %d of %d exposed points; transparent area unchanged at %d "
               "of %d", in_hit, in_usable, out_same, out_usable);
      if (in_usable < 5 || out_usable < 5) {
        record("transparent-drawing", -1,
               counts + ". Too little of the desktop was exposed to measure: minimise all "
                        "windows (Win+D) and run again");
      } else {
        // Icons may sit on a few points and a slideshow wallpaper may change
        // a few, so a clear majority is required rather than all.
        const bool ok = in_hit * 3 >= in_usable * 2 && out_same * 3 >= out_usable * 2;
        record("transparent-drawing", ok ? 1 : 0,
               counts + (ok ? "" : ". The card did not reach the screen, or the layer is not "
                                   "transparent (an animated wallpaper can also cause this)"));
      }

      bool through = true;
      std::string hit_text;
      for (const DesktopSurface& s : d.surfaces) {
        const POINT centre{(s.monitor.left + s.monitor.right) / 2,
                           (s.monitor.top + s.monitor.bottom) / 2};
        const HWND hit = WindowFromPoint(centre);
        through = through && surface_of(d, hit) == nullptr;
        hit_text += text("display %d: %s; ", s.number, class_of(hit).c_str());
      }
      record("click-through", through ? 1 : 0,
             hit_text + "window that hit-testing finds at the centre of each card (never the "
                        "layer). No click was sent and no icon was touched");
    }
    ReleaseDC(nullptr, screen);

    record("no-focus",
           GetForegroundWindow() == foreground_before && surface_of(d, GetForegroundWindow()) ==
                                                             nullptr && GetFocus() == nullptr
               ? 1 : 0,
           "the foreground window is the same as before the test and this process holds no "
           "keyboard focus");

    RECT everything{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    std::string why;
    const bool shot = save_screenshot(everything, utf8_to_wide(screenshot_path), &why);
    record("screenshot", shot ? 1 : 0,
           shot ? "saved to " + screenshot_path +
                      " (all displays, including any open windows; look before sharing)"
                : "NOT saved: " + why);
    pump(1200);
  }

  out += text("\r\n%d passed, %d failed, %d skipped\r\n", passed, failed, skipped);
  out += std::string("RESULT: ") +
         (failed ? "FAIL" : (skipped ? "INCOMPLETE (nothing failed, but not everything could "
                                       "be checked)" : "PASS")) + "\r\n\r\n";
  out += "WHAT THIS TEST DOES NOT SHOW\r\n";
  out += "  - That the card really looked right and was behind the icons: look at the\r\n";
  out += "    screenshot and the screen yourself.\r\n";
  out += "  - That desktop icons can be clicked, dragged and right-clicked through lyrics.\r\n";
  out += "  - That every application window covers the lyrics.\r\n";
  out += "  - A normal start of the program, real music, Explorer restarts, sleep, or\r\n";
  out += "    display changes.\r\n";
  out += "  These are in the manual matrix, engineering/WINDOWS_TEST_MATRIX.md.\r\n\r\n";
  out += "---- desktop integration details ----\r\n";
  out += text("  %-26s %d\r\n", "Displays found:", static_cast<int>(monitors.size()));
  out += desktop_layer_report(app);
  desktop_layer_disable(app);

  const std::wstring wide_path = utf8_to_wide(path);
  HANDLE file = CreateFileW(wide_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  bool written = false;
  if (file != INVALID_HANDLE_VALUE) {
    const std::string bom = "\xEF\xBB\xBF";
    DWORD done = 0;
    written = WriteFile(file, bom.data(), static_cast<DWORD>(bom.size()), &done, nullptr) &&
              WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &done, nullptr) &&
              done == out.size();
    CloseHandle(file);
  }
  if (failed || !written) return 1;
  return skipped ? 2 : 0;
}

}  // namespace ytl
