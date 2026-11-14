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

// --------------------------------------------------------------- the window

LRESULT CALLBACK layer_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;   // every click belongs to the desktop
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      // Only the opaque fallback is painted this way; the transparent layer
      // is pushed with UpdateLayeredWindow and never receives a paint.
      App* app = g_layer_app;
      if (app && app->desktop.hwnd == hwnd && app->desktop.opaque && app->desktop.dc) {
        BitBlt(dc, 0, 0, app->desktop.buf_w, app->desktop.buf_h, app->desktop.dc, 0, 0, SRCCOPY);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCDESTROY: {
      App* app = g_layer_app;
      if (app && app->desktop.hwnd == hwnd) {
        DesktopLayer& d = app->desktop;
        d.hwnd = nullptr;
        d.visible = false;
        d.below_icons = false;
        if (!d.closing) {
          // Destroyed from outside: Explorer exited, restarted, or rebuilt the
          // desktop. The main window is untouched and will attach again.
          d.lost = true;
          d.state = DesktopAttach::Retrying;
          d.detail = "the desktop surface was closed by Explorer; waiting for it to return";
          d.attempts = 0;
          d.next_retry_at = app->now_ms() + 1500;
          logf("desktop: the layer was destroyed with its Explorer parent; will re-attach");
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

void free_buffer(DesktopLayer& d) {
  if (d.dc) {
    SelectObject(d.dc, d.old_bmp);
    DeleteDC(d.dc);
  }
  if (d.bmp) DeleteObject(d.bmp);
  d.dc = nullptr;
  d.bmp = nullptr;
  d.old_bmp = nullptr;
  d.bits = nullptr;
  d.buf_w = d.buf_h = 0;
}

bool ensure_buffer(DesktopLayer& d) {
  if (d.dc && d.buf_w == d.w && d.buf_h == d.h) return true;
  free_buffer(d);
  if (d.w <= 0 || d.h <= 0) return false;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = d.w;
  info.bmiHeader.biHeight = -d.h;  // top-down, so row 0 is the top row
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  HDC screen = GetDC(nullptr);
  d.dc = CreateCompatibleDC(screen);
  d.bmp = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &d.bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!d.dc || !d.bmp || !d.bits) {
    free_buffer(d);
    return false;
  }
  d.old_bmp = SelectObject(d.dc, d.bmp);
  d.buf_w = d.w;
  d.buf_h = d.h;
  return true;
}

// The monitor the lyrics are shown on: the primary one. Spanning every
// monitor would put the centre line across the gap between two screens.
RECT target_monitor() {
  const POINT origin{0, 0};
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &info))
    return info.rcMonitor;
  return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
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

void destroy_layer(DesktopLayer& d) {
  if (d.hwnd) {
    d.closing = true;
    DestroyWindow(d.hwnd);
    d.closing = false;
  }
  d.hwnd = nullptr;
  d.visible = false;
  d.below_icons = false;
  d.last_alpha = -1;
}

// Creates and places the layer. Returns false with d.detail / d.last_error set.
bool create_layer(HINSTANCE inst, DesktopLayer& d) {
  const ShellDesktop shell = find_shell_desktop(true);
  d.strategy = shell.strategy;
  d.host = shell.host;
  d.icons = shell.icons;
  if (!shell.host) {
    d.last_error = 0;
    d.detail = shell.why_not;
    return false;
  }
  if (!ensure_layer_class(inst)) {
    d.last_error = GetLastError();
    d.detail = text("the layer window class could not be registered (error %lu)",
                    static_cast<unsigned long>(d.last_error));
    return false;
  }

  d.monitor = target_monitor();
  RECT place{};
  if (!to_host(shell.host, d.monitor, &place)) {
    d.last_error = GetLastError();
    d.detail = text("monitor coordinates could not be mapped into the desktop surface "
                    "(error %lu)", static_cast<unsigned long>(d.last_error));
    return false;
  }
  const int w = place.right - place.left, h = place.bottom - place.top;
  if (w < 64 || h < 64) {
    d.detail = "the desktop surface has no usable area";
    return false;
  }

  // Create the child under the same DPI awareness as its Explorer parent.
  const DpiApi& api = dpi_api();
  DpiContext previous = nullptr;
  if (api.window_context && api.set_thread) {
    if (DpiContext host_context = api.window_context(shell.host))
      previous = api.set_thread(host_context);
  }
  SetLastError(ERROR_SUCCESS);
  HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kLayerClass,
                              L"", WS_CHILD, place.left, place.top, w, h, shell.host, nullptr,
                              inst, nullptr);
  const DWORD create_error = GetLastError();
  if (previous && api.set_thread) api.set_thread(previous);
  if (!hwnd) {
    d.last_error = create_error;
    d.detail = text("Windows refused to create a window inside the desktop surface (error %lu)",
                    static_cast<unsigned long>(create_error));
    return false;
  }
  d.hwnd = hwnd;
  d.w = w;
  d.h = h;
  d.visible = false;
  d.last_alpha = -1;

  // Position in the z-order: directly beneath the icon layer when they share
  // a parent, otherwise on top inside the wallpaper worker.
  SetWindowPos(hwnd, shell.insert_after ? shell.insert_after : HWND_TOP, place.left, place.top, w,
               h, SWP_NOACTIVATE);

  // Verify instead of trusting the return values.
  if (GetAncestor(hwnd, GA_PARENT) != shell.host) {
    d.last_error = GetLastError();
    d.detail = "the window was created but is not a child of the desktop surface";
    destroy_layer(d);
    return false;
  }
  if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) {
    d.detail = "Windows removed the layered style from the desktop child window";
    destroy_layer(d);
    return false;
  }
  if (shell.insert_after) {
    d.below_icons = is_below_sibling(hwnd, shell.icons);
  } else {
    // Classic layout: the whole host window is behind the icon window. That
    // relationship was established when the host was found.
    d.below_icons = shell.worker_behind == shell.host;
  }
  if (d.opaque) SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
  d.last_error = 0;
  d.detail.clear();
  return true;
}

// Pushes the back buffer to the screen at `alpha`.
bool push(DesktopLayer& d, int alpha) {
  alpha = std::max(0, std::min(255, alpha));
  if (d.opaque) {
    if (alpha != d.last_alpha) SetLayeredWindowAttributes(d.hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
    InvalidateRect(d.hwnd, nullptr, FALSE);
  } else {
    SIZE size{d.w, d.h};
    POINT source{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = static_cast<BYTE>(alpha);
    blend.AlphaFormat = AC_SRC_ALPHA;
    SetLastError(ERROR_SUCCESS);
    if (!UpdateLayeredWindow(d.hwnd, nullptr, nullptr, &size, d.dc, &source, 0, &blend,
                             ULW_ALPHA)) {
      d.present_error = GetLastError();
      ++d.present_failures;
      return false;
    }
  }
  d.last_alpha = alpha;
  ++d.presents;
  if (!d.visible) {
    ShowWindow(d.hwnd, SW_SHOWNOACTIVATE);
    d.visible = true;
  }
  return true;
}

const char* state_word(DesktopAttach state) {
  switch (state) {
    case DesktopAttach::Attached: return "attached";
    case DesktopAttach::Restored: return "attached (restored after Explorer restarted)";
    case DesktopAttach::Retrying: return "retrying";
    case DesktopAttach::Failed: return "failed";
    default: return "not in use";
  }
}

bool attached(const DesktopLayer& d) {
  return d.state == DesktopAttach::Attached || d.state == DesktopAttach::Restored;
}

}  // namespace

// ------------------------------------------------------------------ public

bool desktop_layer_attach(App& app) {
  DesktopLayer& d = app.desktop;
  g_layer_app = &app;
  destroy_layer(d);
  ++d.attempts;
  const int64_t now = app.now_ms();
  if (!create_layer(app.inst, d)) {
    // Five quick attempts cover Explorer still starting; after that it is a
    // failure the user is told about, and retries become rare.
    const bool exhausted = d.attempts >= 5;
    const DesktopAttach before = d.state;
    d.state = exhausted ? DesktopAttach::Failed : DesktopAttach::Retrying;
    d.next_retry_at = now + (exhausted ? 30000 : 2000);
    if (before != d.state || d.attempts == 1) {
      logf("desktop: attach %s (attempt %d): %s", state_word(d.state), d.attempts,
           d.detail.c_str());
    }
    return false;
  }
  const bool restored = d.lost;
  d.lost = false;
  d.state = restored ? DesktopAttach::Restored : DesktopAttach::Attached;
  if (restored) ++d.restores;
  d.attached_at = now;
  d.next_retry_at = 0;
  d.attempts = 0;
  d.failure_notified = false;
  logf("desktop: %s; %s; host %s; layer %dx%d on monitor (%ld,%ld); below icons: %s; "
       "host DPI awareness %s, layer %s",
       state_word(d.state), d.strategy.c_str(), class_of(d.host).c_str(), d.w, d.h,
       d.monitor.left, d.monitor.top, d.below_icons ? "confirmed" : "not confirmed",
       awareness_name(awareness_of(d.host)), awareness_name(awareness_of(d.hwnd)));
  app.on_resize(d.w, d.h);
  app.rebuild_layout(true);
  return true;
}

void desktop_layer_detach(App& app) {
  DesktopLayer& d = app.desktop;
  destroy_layer(d);
  free_buffer(d);
  d.state = DesktopAttach::Inactive;
  d.host = nullptr;
  d.icons = nullptr;
  d.lost = false;
  d.attempts = 0;
  d.next_retry_at = 0;
  d.detail.clear();
  app.presence = 0.0;
}

void desktop_layer_hide(App& app) {
  DesktopLayer& d = app.desktop;
  if (d.hwnd && d.visible) {
    ShowWindow(d.hwnd, SW_HIDE);
    d.visible = false;
  }
}

void desktop_layer_shell_restarted(App& app) {
  DesktopLayer& d = app.desktop;
  if (d.state == DesktopAttach::Inactive) return;
  // The old Explorer's windows are gone or about to be; do not wait to find
  // out one at a time.
  destroy_layer(d);
  d.lost = true;
  d.attempts = 0;
  d.state = DesktopAttach::Retrying;
  d.detail = "Explorer restarted; attaching to the new desktop";
  d.next_retry_at = app.now_ms();
  desktop_layer_tick(app, true);
}

void desktop_layer_tick(App& app, bool force) {
  DesktopLayer& d = app.desktop;
  if (d.state == DesktopAttach::Inactive) return;
  const int64_t now = app.now_ms();

  if (attached(d)) {
    if (!force && now - d.last_check_at < 2000) return;
    d.last_check_at = now;
    const bool alive = d.hwnd && IsWindow(d.hwnd) && IsWindow(d.host) &&
                       GetAncestor(d.hwnd, GA_PARENT) == d.host;
    if (!alive) {
      logf("desktop: attachment lost (layer %s, host %s)", class_of(d.hwnd).c_str(),
           class_of(d.host).c_str());
      destroy_layer(d);
      d.lost = true;
      d.attempts = 0;
      d.state = DesktopAttach::Retrying;
      d.detail = "the desktop surface changed; attaching again";
      d.next_retry_at = now;
    } else {
      // Monitor moved, resized or rescaled?
      const RECT monitor = target_monitor();
      RECT place{};
      if (to_host(d.host, monitor, &place)) {
        RECT current{};
        GetWindowRect(d.hwnd, &current);
        POINT corner{current.left, current.top};
        MapWindowPoints(nullptr, d.host, &corner, 1);
        const int w = place.right - place.left, h = place.bottom - place.top;
        if (corner.x != place.left || corner.y != place.top || w != d.w || h != d.h) {
          SetWindowPos(d.hwnd, nullptr, place.left, place.top, w, h,
                       SWP_NOZORDER | SWP_NOACTIVATE);
          d.monitor = monitor;
          const bool resized = w != d.w || h != d.h;
          d.w = w;
          d.h = h;
          d.last_alpha = -1;
          logf("desktop: display changed; layer now %dx%d at (%ld,%ld)", w, h, monitor.left,
               monitor.top);
          if (resized) {
            app.view.bg_theme = -1;
            app.on_resize(w, h);
            app.rebuild_layout(true);
          }
        }
      }
      // Explorer re-stacks its children when the wallpaper changes; make sure
      // the layer is still under the icons.
      if (d.icons && IsWindow(d.icons) && GetAncestor(d.icons, GA_PARENT) == d.host) {
        if (!is_below_sibling(d.hwnd, d.icons)) {
          SetWindowPos(d.hwnd, d.icons, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        d.below_icons = is_below_sibling(d.hwnd, d.icons);
      }
      return;
    }
  }

  if (now < d.next_retry_at && !force) return;
  if (desktop_layer_attach(app)) return;
  if (d.state == DesktopAttach::Failed && !d.failure_notified && app.hwnd) {
    d.failure_notified = true;
    notify_from_tray(app, L"Lyrics could not be placed on the desktop",
                     L"Youtube-Lyrics is running but could not attach to the desktop "
                     L"wallpaper layer. It will keep trying. Right-click this icon for "
                     L"Settings, which shows the reason, or choose a window mode.");
  }
}

bool desktop_layer_present(App& app, int alpha) {
  DesktopLayer& d = app.desktop;
  if (!attached(d) || !d.hwnd) return false;
  if (!ensure_buffer(d)) {
    d.present_error = GetLastError();
    ++d.present_failures;
    return false;
  }
  {
    Gdiplus::Bitmap target(d.buf_w, d.buf_h, d.buf_w * 4, PixelFormat32bppPARGB,
                           static_cast<BYTE*>(d.bits));
    Gdiplus::Graphics g(&target);
    app.view.layer_transparent = !d.opaque;
    render_frame(app, g, d.buf_w, d.buf_h);
    app.view.layer_transparent = false;
  }
  if (push(d, alpha)) return true;

  // Per-pixel transparency was refused. Rather than show nothing, fall back
  // once to an opaque child that paints a scene, and say so in the status.
  if (!d.opaque && d.presents == 0 && d.present_failures >= 3) {
    logf("desktop: UpdateLayeredWindow failed (error %lu); using the opaque scene fallback",
         static_cast<unsigned long>(d.present_error));
    d.opaque = true;
    const bool was_lost = d.lost;
    destroy_layer(d);
    d.lost = was_lost;
    d.attempts = 0;
    d.next_retry_at = app.now_ms();
    d.state = DesktopAttach::Retrying;
    d.detail = "switching to opaque drawing";
  }
  return false;
}

std::string desktop_layer_status(const App& app) {
  const DesktopLayer& d = app.desktop;
  switch (d.state) {
    case DesktopAttach::Attached:
    case DesktopAttach::Restored: {
      std::string s = d.state == DesktopAttach::Restored
                          ? "Attached to the desktop (restored after Explorer restarted)"
                          : "Attached to the desktop";
      s += d.below_icons ? ", below the icons" : ", icon order not confirmed";
      if (d.opaque) s += "; transparency unavailable, showing a scene";
      else if (d.presents == 0 && d.present_failures > 0) s += "; drawing is failing";
      return s;
    }
    case DesktopAttach::Retrying:
      return "Not attached; retrying" +
             (d.detail.empty() ? std::string() : " (" + d.detail + ")");
    case DesktopAttach::Failed:
      return "Attachment failed: " + (d.detail.empty() ? std::string("unknown reason") : d.detail);
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
  r += row("Attachment state", state_word(d.state));
  r += row("Status shown to the user", desktop_layer_status(app));
  if (!d.detail.empty()) r += row("Detail", d.detail);
  if (d.last_error) r += row("Last Windows error", std::to_string(d.last_error));
  r += row("Attempts / restores", text("%d / %d", d.attempts, d.restores));
  if (!d.strategy.empty()) r += row("Placement", d.strategy);
  if (d.hwnd && IsWindow(d.hwnd)) {
    r += row("Layer window", describe(d.hwnd));
    const HWND parent = GetAncestor(d.hwnd, GA_PARENT);
    r += row("Actual parent", describe(parent));
    r += row("Parent is the chosen host", parent == d.host ? "yes" : "NO");
    r += row("Below the icon layer", d.below_icons ? "confirmed" : "not confirmed");
    const LONG_PTR ex = GetWindowLongPtrW(d.hwnd, GWL_EXSTYLE);
    const LONG_PTR style = GetWindowLongPtrW(d.hwnd, GWL_STYLE);
    r += row("Child / layered / click-through / no-activate",
             text("%s / %s / %s / %s", (style & WS_CHILD) ? "yes" : "NO",
                  (ex & WS_EX_LAYERED) ? "yes" : "NO", (ex & WS_EX_TRANSPARENT) ? "yes" : "NO",
                  (ex & WS_EX_NOACTIVATE) ? "yes" : "NO"));
    r += row("Caption / border / taskbar button",
             text("%s / %s / %s", (style & WS_CAPTION) == WS_CAPTION ? "YES" : "none",
                  (style & (WS_BORDER | WS_THICKFRAME)) ? "YES" : "none",
                  (ex & WS_EX_APPWINDOW) ? "YES" : "none (child window)"));
    r += row("Layer size", text("%dx%d", d.w, d.h));
    r += row("Target monitor", text("%ldx%ld at (%ld,%ld)", d.monitor.right - d.monitor.left,
                                    d.monitor.bottom - d.monitor.top, d.monitor.left,
                                    d.monitor.top));
    if (dpi_api().window_dpi)
      r += row("Layer DPI", std::to_string(dpi_api().window_dpi(d.hwnd)));
    r += row("DPI awareness host / layer",
             text("%s / %s", awareness_name(awareness_of(d.host)),
                  awareness_name(awareness_of(d.hwnd))));
    r += row("Drawing", d.opaque ? "opaque fallback (scene)" : "per-pixel transparency");
    r += row("Frames shown / failed",
             text("%d / %d%s", d.presents, d.present_failures,
                  d.present_error ? text(" (last error %lu)",
                                         static_cast<unsigned long>(d.present_error)).c_str()
                                  : ""));
    r += row("Currently shown", text("%s, opacity %.0f%%", d.visible ? "yes" : "no (idle)",
                                     app.presence * 100.0));
  } else {
    r += row("Layer window", "none");
  }

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

bool save_screenshot(const RECT& area, const std::wstring& path) {
  const int w = area.right - area.left, h = area.bottom - area.top;
  if (w <= 0 || h <= 0) return false;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  const BOOL copied = BitBlt(mem, 0, 0, w, h, screen, area.left, area.top, SRCCOPY | CAPTUREBLT);
  SelectObject(mem, old);
  bool saved = false;
  if (copied) {
    CLSID png{};
    if (png_encoder(&png) >= 0) {
      Gdiplus::Bitmap image(bmp, nullptr);
      saved = image.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }
  }
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
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

}  // namespace

bool run_desktop_test(App& app, const std::string& path) {
  DesktopLayer& d = app.desktop;
  std::string out = "Youtube-Lyrics desktop integration test, version " YTL_VERSION_STRING "\r\n";
  out += "Runs the real attachment code on this computer for a few seconds. A card\r\n";
  out += "reading \"Youtube-Lyrics desktop test\" should appear on the wallpaper,\r\n";
  out += "BEHIND the desktop icons, and then vanish. These checks describe window\r\n";
  out += "relationships; only your eyes (and the screenshot) show how it looks.\r\n\r\n";
  bool all_ok = true;
  auto record = [&](const char* name, bool ok, const std::string& detail) {
    out += std::string(ok ? "PASS  " : "FAIL  ") + name + ": " + detail + "\r\n";
    if (!ok) all_ok = false;
  };

  const HWND foreground_before = GetForegroundWindow();
  d.state = DesktopAttach::Retrying;
  bool ok = false;
  for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
    ok = desktop_layer_attach(app);
    if (!ok) Sleep(400);
  }
  record("attach", ok, ok ? d.strategy : d.detail);

  bool drawn = false;
  bool screenshot = false;
  std::string screenshot_path;
  if (ok) {
    record("parent", GetAncestor(d.hwnd, GA_PARENT) == d.host,
           "parent is " + class_of(GetAncestor(d.hwnd, GA_PARENT)));
    record("below-icons", d.below_icons,
           d.below_icons ? "the layer is ordered beneath the icon layer"
                         : "could not confirm the order relative to the icon layer");

    // Draw a test card straight into the layer's buffer.
    for (int pass = 0; pass < 2 && !drawn; ++pass) {
      if (!d.hwnd || !ensure_buffer(d)) break;
      {
        Gdiplus::Bitmap target(d.buf_w, d.buf_h, d.buf_w * 4, PixelFormat32bppPARGB,
                               static_cast<BYTE*>(d.bits));
        Gdiplus::Graphics g(&target);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(d.opaque ? Gdiplus::Color(255, 16, 16, 24) : Gdiplus::Color(0, 0, 0, 0));
        const Gdiplus::REAL cw = d.buf_w * 0.56f, ch = d.buf_h * 0.2f;
        const Gdiplus::RectF card((d.buf_w - cw) / 2, (d.buf_h - ch) / 2, cw, ch);
        Gdiplus::SolidBrush back(Gdiplus::Color(200, 12, 12, 18));
        g.FillRectangle(&back, card);
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font font(&family, std::max(16.0f, d.buf_h * 0.04f), Gdiplus::FontStyleBold,
                           Gdiplus::UnitPixel);
        Gdiplus::SolidBrush ink(Gdiplus::Color(255, 245, 165, 36));
        Gdiplus::StringFormat centre;
        centre.SetAlignment(Gdiplus::StringAlignmentCenter);
        centre.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(L"Youtube-Lyrics desktop test\nicons should be in front of this card", -1,
                     &font, card, &centre, &ink);
      }
      drawn = push(d, 255);
      if (!drawn && !d.opaque) {
        // Same fallback the app uses.
        d.opaque = true;
        desktop_layer_attach(app);
      }
    }
    record("draw", drawn,
           drawn ? std::string(d.opaque ? "opaque fallback" : "per-pixel transparency")
                 : text("drawing failed (error %lu)", static_cast<unsigned long>(d.present_error)));
    pump(2500);

    if (d.hwnd) {
      record("visible", IsWindowVisible(d.hwnd) != 0, "the layer window reports itself visible");
      const POINT centre{(d.monitor.left + d.monitor.right) / 2,
                         (d.monitor.top + d.monitor.bottom) / 2};
      const HWND hit = WindowFromPoint(centre);
      record("click-through", hit != d.hwnd,
             "a click at the centre of the card would go to " + class_of(hit));
      record("no-focus", GetForegroundWindow() == foreground_before &&
                             GetForegroundWindow() != d.hwnd,
             "the foreground window did not change");
      const LONG_PTR style = GetWindowLongPtrW(d.hwnd, GWL_STYLE);
      record("no-chrome", (style & (WS_CAPTION | WS_THICKFRAME | WS_SYSMENU)) == 0,
             text("style 0x%08lX", static_cast<unsigned long>(style)));
    } else {
      record("survived", false, "the layer window was destroyed during the test");
    }

    screenshot_path = path;
    const size_t dot = screenshot_path.find_last_of('.');
    const size_t slash = screenshot_path.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
      screenshot_path.erase(dot);
    screenshot_path += ".png";
    screenshot = save_screenshot(d.monitor, utf8_to_wide(screenshot_path));
    out += std::string(screenshot ? "NOTE  screenshot: saved to " + screenshot_path
                                  : "NOTE  screenshot: could not be saved") + "\r\n";
    if (screenshot) {
      out += "      It shows whatever was on the primary monitor, including any open\r\n";
      out += "      windows. Look at it before sharing it. It stays on this computer.\r\n";
    }
    pump(1500);
  }

  out += std::string("\r\nRESULT: ") + (all_ok ? "PASS" : "FAIL") + "\r\n";
  out += "A PASS means the window is where it should be. Whether the card was really\r\n";
  out += "behind the icons and looked right is for a person to confirm.\r\n\r\n";
  out += "---- desktop integration details ----\r\n";
  out += desktop_layer_report(app);
  desktop_layer_detach(app);

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
  return all_ok && written;
}

}  // namespace ytl
