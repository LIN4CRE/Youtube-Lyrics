// Youtube-Lyrics - diagnostic report.
//
// One plain-text file that answers "what is the app seeing and why is it
// showing this?": versions, display layout, the media session, the lyric
// lookup trail, the cache, the updater and the recent log. It is written only
// when the user asks for it and is never sent anywhere by this program.
#include "app.h"
#include "update.h"
#include "version.h"

#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ytl {
namespace {

std::string line(const char* label, const std::string& value) {
  std::string out = "  ";
  out += label;
  out += ": ";
  while (out.size() < 26) out.push_back(' ');
  out += value.empty() ? std::string("(none)") : value;
  out += "\r\n";
  return out;
}

std::string yes_no(bool value) { return value ? "yes" : "no"; }

std::string fmt(const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return buffer;
}

std::string windows_version() {
  // RtlGetVersion reports the real build; GetVersionEx answers according to
  // the application manifest and is deprecated.
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  OSVERSIONINFOEXW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  RtlGetVersionFn get = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(
                                    GetProcAddress(ntdll, "RtlGetVersion")))
                              : nullptr;
  if (!get || get(&info) != 0) return "unknown";
  const char* family = "Windows";
  if (info.dwMajorVersion == 10 && info.wProductType == VER_NT_WORKSTATION)
    family = info.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
  else if (info.dwMajorVersion == 10)
    family = "Windows Server";
  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  const char* arch = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64
                         ? "x64"
                         : (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64
                                ? "ARM64 (running the x64 build)"
                                : "other");
  return fmt("%s, version %lu.%lu build %lu, %s", family,
             static_cast<unsigned long>(info.dwMajorVersion),
             static_cast<unsigned long>(info.dwMinorVersion),
             static_cast<unsigned long>(info.dwBuildNumber), arch);
}

struct MonitorList {
  std::string text;
  int count = 0;
};

BOOL CALLBACK add_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
  auto* list = reinterpret_cast<MonitorList*>(param);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return TRUE;
  unsigned dpi = 0;
  // Per-monitor DPI needs shcore (Windows 8.1+); loaded on demand so the
  // program still starts where it is missing.
  if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
    using GetDpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    auto get = reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(
        GetProcAddress(shcore, "GetDpiForMonitor")));
    UINT x = 0, y = 0;
    if (get && SUCCEEDED(get(monitor, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) dpi = x;
    FreeLibrary(shcore);
  }
  ++list->count;
  list->text += fmt("  monitor %d: %ldx%ld at (%ld,%ld)%s", list->count,
                    info.rcMonitor.right - info.rcMonitor.left,
                    info.rcMonitor.bottom - info.rcMonitor.top, info.rcMonitor.left,
                    info.rcMonitor.top,
                    (info.dwFlags & MONITORINFOF_PRIMARY) ? ", primary" : "");
  if (dpi) list->text += fmt(", %u DPI (%u%%)", dpi, dpi * 100 / 96);
  list->text += "\r\n";
  return TRUE;
}

const char* mode_name(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::Desktop: return "on the desktop (behind windows)";
    case DisplayMode::Fullscreen: return "full screen (in front)";
    case DisplayMode::Window:
    default: return "window";
  }
}

const char* state_name(LyricsState state) {
  switch (state) {
    case LyricsState::Loading: return "looking for lyrics";
    case LyricsState::Ready: return "lyrics shown";
    case LyricsState::NotFound: return "no lyrics found";
    case LyricsState::Error: return "lookup failed";
    case LyricsState::Idle:
    default: return "idle (no track)";
  }
}

std::string timing_mode(const App& app) {
  if (app.lstate != LyricsState::Ready) return "not applicable";
  if (app.lyrics.synced) {
    bool enhanced = false;
    for (const LyricLine& l : app.lyrics.lines) {
      if (!l.segments.empty()) enhanced = true;
    }
    return enhanced ? "timed by the source, word level (Enhanced LRC)"
                    : "timed by the source, line level (LRC)";
  }
  return app.timing_estimated
             ? "untimed words; scroll position is an estimate, not source timing"
             : "untimed words, shown as a static sheet";
}

std::string track_text(const TrackInfo& t) {
  if (t.title.empty()) return std::string();
  std::string out = track_label(t);
  if (!t.album.empty()) out += "  [album: " + t.album + "]";
  out += t.duration_s > 0
             ? "  [" + format_time(static_cast<int64_t>(t.duration_s * 1000.0)) + "]"
             : "  [length not reported]";
  return out;
}

int count_files(const std::string& directory) {
  WIN32_FIND_DATAW entry{};
  HANDLE search = FindFirstFileW(utf8_to_wide(directory + "\\*").c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) return 0;
  int count = 0;
  do {
    if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++count;
  } while (FindNextFileW(search, &entry));
  FindClose(search);
  return count;
}

// Last `max_lines` lines of the log on disk. The file also covers earlier
// sessions, which is what matters after a crash or a failed start.
std::string log_tail(size_t max_lines) {
  const std::string text = read_file(appdata_dir() + "\\ytlyrics.log");
  if (text.empty()) return "  (the log is empty)\r\n";
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string one = text.substr(pos, end - pos);
    while (!one.empty() && one.back() == '\r') one.pop_back();
    if (!one.empty()) lines.push_back(one);
    pos = end + 1;
  }
  std::string out;
  const size_t first = lines.size() > max_lines ? lines.size() - max_lines : 0;
  for (size_t i = first; i < lines.size(); ++i) {
    if (lines[i].size() > 400) lines[i] = lines[i].substr(0, 400) + " ...";
    out += "  " + lines[i] + "\r\n";
  }
  return out;
}

std::string probe(const char* name, const wchar_t* url) {
  int status = 0;
  std::string error;
  const ULONGLONG started = GetTickCount64();
  const std::string body = http_get(url, 8000, &status, &error);
  const unsigned long elapsed = static_cast<unsigned long>(GetTickCount64() - started);
  if (!error.empty()) return line(name, fmt("FAILED after %lu ms: ", elapsed) + error);
  return line(name, fmt("HTTP %d, %zu bytes, %lu ms", status, body.size(), elapsed));
}

std::string environment_value(const wchar_t* name) {
  wchar_t buffer[512] = {};
  const DWORD length = GetEnvironmentVariableW(name, buffer, 512);
  if (length == 0 || length >= 512) return std::string();
  return wide_to_utf8(buffer);
}

}  // namespace

std::string build_diagnostic_report(App& app, bool with_connectivity) {
  std::string r;
  SYSTEMTIME now{};
  GetLocalTime(&now);
  r += "Youtube-Lyrics diagnostic report\r\n";
  r += fmt("Created %04d-%02d-%02d %02d:%02d:%02d (local time)\r\n", now.wYear, now.wMonth,
           now.wDay, now.wHour, now.wMinute, now.wSecond);
  r += "\r\n";
  r += "PRIVACY: read this before sharing the file.\r\n";
  r += "  - It was written on this computer because you asked for it. This program\r\n";
  r += "    never sends it anywhere.\r\n";
  r += "  - It DOES contain: the titles and artists of the songs played recently,\r\n";
  r += "    the name of the player app, the lyric searches made for them, your\r\n";
  r += "    Windows version, monitor sizes and this program's settings and log.\r\n";
  r += "  - It does NOT contain audio, lyrics text, browsing history, your Windows\r\n";
  r += "    account name (replaced by a placeholder) or any password or token\r\n";
  r += "    (this program has none; anything that looks like one is masked).\r\n";
  r += "  - Delete any line you would rather not share. The file is plain text.\r\n";

  r += "\r\nAPPLICATION\r\n";
  r += line("Version", YTL_VERSION_STRING);
  r += line("Windows", windows_version());
  r += line("Running for", app.started_at > 0
                               ? fmt("%lld s", static_cast<long long>(
                                                   (app.now_ms() - app.started_at) / 1000))
                               : std::string("not started (report made from the command line)"));
  r += line("Data folder", appdata_dir());
  r += line("PowerShell helper host",
            yes_no(GetFileAttributesW((utf8_to_wide(environment_value(L"SystemRoot")) +
                                       L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe")
                                          .c_str()) != INVALID_FILE_ATTRIBUTES) +
                " (Windows PowerShell 5.1 present)");

  r += "\r\nDISPLAY\r\n";
  r += line("Mode", mode_name(app.cfg.mode()));
  MonitorList monitors;
  EnumDisplayMonitors(nullptr, nullptr, add_monitor, reinterpret_cast<LPARAM>(&monitors));
  r += line("Monitors", std::to_string(monitors.count));
  r += monitors.text;
  if (app.hwnd && IsWindow(app.hwnd)) {
    RECT rect{};
    GetWindowRect(app.hwnd, &rect);
    r += line("Window", fmt("%ldx%ld at (%ld,%ld), %s%s", rect.right - rect.left,
                            rect.bottom - rect.top, rect.left, rect.top,
                            IsWindowVisible(app.hwnd) ? "visible" : "not visible",
                            app.hidden_to_tray ? ", hidden to the tray" : ""));
    r += line("Window styles",
              fmt("0x%08lX / 0x%08lX",
                  static_cast<unsigned long>(GetWindowLongPtrW(app.hwnd, GWL_STYLE)),
                  static_cast<unsigned long>(GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE))));
    if (app.cfg.mode() == DisplayMode::Desktop) {
      r += line("Desktop layer",
                app.desktop_parented
                    ? "attached to Explorer's WorkerW (behind the desktop icons)"
                    : "WorkerW not available; using the bottom-of-z-order fallback");
    }
  } else {
    r += line("Window", "none (report made from the command line)");
    r += line("Desktop layer host",
              desktop_host_available() ? "Explorer's WorkerW was found"
                                       : "Explorer's WorkerW was not found");
  }
  r += line("Theme / text size", fmt("%d / %.0f%%", app.cfg.theme, app.cfg.font_scale * 100.0));
  r += line("Always on top", yes_no(app.cfg.topmost));
  r += line("Background", fmt("scene %d, darkness %d%%, rotate %s every %d min",
                              app.cfg.wallpaper_index, app.cfg.wallpaper_dim,
                              app.cfg.wallpaper_auto_rotate ? "on" : "off",
                              app.cfg.wallpaper_interval_minutes));

  r += "\r\nMEDIA SESSION\r\n";
  MediaState latest;
  {
    std::lock_guard<std::mutex> lock(app.mu);
    latest = app.media;
  }
  const MediaHelperStatus helper = media_helper_status();
  r += line("Listener",
            helper.starts == 0 && helper.start_failures == 0
                ? std::string("not started in this process")
                : fmt("%s; started %d time(s), %d start failure(s), %d report(s), "
                      "%d unreadable",
                      helper.running ? "running" : "not running", helper.starts,
                      helper.start_failures, helper.lines, helper.bad_lines));
  if (helper.ms_since_last_line >= 0)
    r += line("Last report", fmt("%lld ms ago", static_cast<long long>(
                                                    helper.ms_since_last_line)));
  r += line("Helper has reported", yes_no(app.helper_heard));
  r += line("Session present", yes_no(app.session_present));
  r += line("Player", app.active_media.app.empty() ? latest.app : app.active_media.app);
  r += line("Status", latest.status);
  r += line("Track shown", track_text(app.active_track));
  r += line("Track last reported", track_text(latest.track));
  if (!app.pending_track.title.empty())
    r += line("Track being confirmed", track_text(app.pending_track));
  if (!app.active_track.title.empty()) {
    r += line("Edition", track_variant_name(app.active_track.variant()));
    r += line("Position", format_time(app.current_pos_ms()) +
                              (app.active_media.playing ? " (playing)" : " (paused)"));
    r += line("Player timeline",
              latest.timeline_stamp == 0
                  ? "no 'last updated' time; the position is run from a local clock"
                  : (latest.timeline_stale ? "reported, but older than the track"
                                           : "reported and current"));
  }

  r += "\r\nLYRICS\r\n";
  r += line("State", state_name(app.lstate));
  if (!app.lmessage.empty()) r += line("Detail", app.lmessage);
  if (app.lstate == LyricsState::Ready) {
    r += line("Source", app.lyrics.source);
    r += line("Matched as", track_text(TrackInfo{app.lyrics.title, app.lyrics.artist,
                                                 app.lyrics.album, 0.0, std::string()}));
    r += line("Lines", std::to_string(app.lyrics.synced ? app.lyrics.lines.size()
                                                        : app.pending_plain_lines.size()));
    r += line("Requests used", std::to_string(app.lyrics.providers_tried));
  }
  r += line("Timing mode", timing_mode(app));
  r += line("Sync correction",
            fmt("%+.2f s for all songs, %+.2f s for this song", app.cfg.offset_ms / 1000.0,
                app.song_offset_ms / 1000.0));
  r += line("Songs with a correction", std::to_string(app.song_offsets.entries.size()));
  if (app.lyrics_retry_at != 0)
    r += line("Automatic retry", fmt("attempt %d scheduled", app.lyrics_retries));

  r += "\r\nRECENT LOOKUPS (newest last)\r\n";
  const std::vector<std::string> lookups = lyrics_recent_lookups();
  if (lookups.empty()) r += "  (none in this session)\r\n";
  for (const std::string& entry : lookups) r += "  " + entry + "\r\n";

  r += "\r\nCACHE\r\n";
  int files = 0;
  uint64_t bytes = 0;
  if (lyrics_cache_stats(&files, &bytes)) {
    r += line("Automatic cache", fmt("%d sheet(s), %llu KiB", files,
                                     static_cast<unsigned long long>((bytes + 1023) / 1024)));
  } else {
    r += line("Automatic cache", "could not be read");
  }
  r += line("Local lyric files", std::to_string(count_files(appdata_dir() + "\\lyrics")));

  r += "\r\nUPDATES\r\n";
  r += line("Installed", YTL_VERSION_STRING);
  r += line("Last activity", update_status_text());

  if (with_connectivity) {
    r += "\r\nCONNECTIVITY (checked while writing this report)\r\n";
    r += probe("LRCLIB", L"https://lrclib.net/api/search?track_name=Yesterday&artist_name=The%20Beatles");
    r += probe("lyrics.ovh", L"https://api.lyrics.ovh/v1/The%20Beatles/Yesterday");
    r += probe("Project site", L"https://youtube-lyrics-seven.vercel.app/downloads/release.json");
    r += probe("GitHub Releases",
               L"https://api.github.com/repos/LIN4CRE/Youtube-Lyrics/releases/latest");
  }

  r += "\r\nRECENT LOG (newest last)\r\n";
  r += log_tail(80);

  const std::string profile = environment_value(L"USERPROFILE");
  const std::string user = environment_value(L"USERNAME");
  return redact_report(r, profile, user);
}

bool write_diagnostic_report(const std::string& report, const std::string& path) {
  // A UTF-8 byte-order mark, so Notepad on older Windows 10 builds shows
  // non-English titles correctly.
  return write_file_atomic(path, std::string("\xEF\xBB\xBF") + report);
}

namespace {

bool copy_to_clipboard(HWND owner, const std::string& text) {
  const std::wstring wide = utf8_to_wide(text);
  // Another program can hold the clipboard for a moment; try a few times.
  bool opened = false;
  for (int attempt = 0; attempt < 5 && !opened; ++attempt) {
    opened = OpenClipboard(owner) != 0;
    if (!opened) Sleep(30);
  }
  if (!opened) return false;
  bool copied = false;
  EmptyClipboard();
  const size_t size = (wide.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size)) {
    if (void* target = GlobalLock(memory)) {
      std::memcpy(target, wide.c_str(), size);
      GlobalUnlock(memory);
      // On success the clipboard owns the memory.
      if (SetClipboardData(CF_UNICODETEXT, memory)) copied = true;
      else GlobalFree(memory);
    } else {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
  return copied;
}

}  // namespace

ReportExport export_diagnostic_report(App& app, std::string* saved_path) {
  const std::string report = build_diagnostic_report(app, false);
  const bool copied = copy_to_clipboard(app.hwnd, report);

  const std::string directory = appdata_dir() + "\\diagnostics";
  bool saved = false;
  std::string path;
  if (CreateDirectoryW(utf8_to_wide(directory).c_str(), nullptr) ||
      GetLastError() == ERROR_ALREADY_EXISTS) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    path = directory + fmt("\\YoutubeLyrics-diagnostics-%04d%02d%02d-%02d%02d%02d.txt",
                           now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                           now.wSecond);
    saved = write_diagnostic_report(report, path);
  }
  if (!saved) {
    logf("diagnostics: could not write the report file (error %lu)",
         static_cast<unsigned long>(GetLastError()));
    return copied ? ReportExport::ClipboardOnly : ReportExport::Failed;
  }
  if (saved_path) *saved_path = path;
  // Show the file in Explorer, selected.
  const std::wstring arguments = L"/select,\"" + utf8_to_wide(path) + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
  logf("diagnostics: report saved%s", copied ? "" : " (clipboard unavailable)");
  return ReportExport::Saved;
}

// ------------------------------------------------------------------ self-test

bool run_self_test(App& app, const std::string& path) {
  std::string out = "Youtube-Lyrics self-test, version " YTL_VERSION_STRING "\r\n";
  out += "Checks that need no window and no user. A pass here does not show that\r\n";
  out += "the display, tray, hotkeys or desktop mode work.\r\n\r\n";
  bool all_ok = true;
  auto record = [&](const char* name, bool ok, const std::string& detail) {
    out += std::string(ok ? "PASS  " : "FAIL  ") + name + ": " + detail + "\r\n";
    if (!ok) all_ok = false;
  };

  // 1. Platform detection.
  const std::string windows = windows_version();
  record("windows-version", windows != "unknown", windows);
  MonitorList monitors;
  EnumDisplayMonitors(nullptr, nullptr, add_monitor, reinterpret_cast<LPARAM>(&monitors));
  record("monitors", monitors.count >= 1, std::to_string(monitors.count) + " found");

  // 2. The data folder can be created, written, replaced, read and cleaned up.
  {
    const std::string dir = appdata_dir();
    const std::string probe_path = dir + "\\self-test-probe.txt";
    const std::string first = "first \xE2\x99\xAB";      // non-ASCII content
    const std::string second = "second, replacing the first";
    const bool wrote = write_file_atomic(probe_path, first);
    const bool read_first = read_file(probe_path) == first;
    const bool replaced = write_file_atomic(probe_path, second);
    const bool read_second = read_file(probe_path) == second;
    const bool removed = DeleteFileW(utf8_to_wide(probe_path).c_str()) != 0;
    const bool temp_gone =
        GetFileAttributesW(utf8_to_wide(probe_path + ".tmp").c_str()) == INVALID_FILE_ATTRIBUTES;
    record("data-folder", wrote && read_first && replaced && read_second && removed && temp_gone,
           fmt("write %d, read %d, replace %d, reread %d, delete %d, no temp left %d", wrote,
               read_first, replaced, read_second, removed, temp_gone));
  }

  // 3. Settings were loaded (by the caller) and are within range. Nothing is
  //    saved: a self-test must not alter the user's configuration.
  record("settings", app.cfg.font_scale >= 0.3 && app.cfg.font_scale <= 4.0 &&
                         app.cfg.offset_ms >= -60000 && app.cfg.offset_ms <= 60000 &&
                         app.cfg.wallpaper_dim >= 0 && app.cfg.wallpaper_dim <= 100,
         fmt("mode %s, text %.0f%%, sync %+d ms, darkness %d%%", mode_name(app.cfg.mode()),
             app.cfg.font_scale * 100.0, app.cfg.offset_ms, app.cfg.wallpaper_dim));

  // 4. The media listener: the PowerShell helper starts, reports at least
  //    once, and the listener thread stops when asked.
  {
    const ULONGLONG started = GetTickCount64();
    const bool launched = media_start(app);
    MediaHelperStatus status;
    while (launched && GetTickCount64() - started < 60000) {
      status = media_helper_status();
      if (status.lines > 0) break;
      Sleep(100);
    }
    const ULONGLONG first_report_ms = GetTickCount64() - started;
    MediaState seen;
    {
      std::lock_guard<std::mutex> lock(app.mu);
      seen = app.media;
    }
    const ULONGLONG stop_started = GetTickCount64();
    const bool stopped = media_stop();
    const ULONGLONG stop_ms = GetTickCount64() - stop_started;
    record("media-listener-start", launched && status.lines > 0,
           status.lines > 0
               ? fmt("first report after %llu ms; session status '%s'",
                     static_cast<unsigned long long>(first_report_ms),
                     seen.status.empty() ? "none" : seen.status.c_str())
               : fmt("no report within 60 s (helper starts %d, start failures %d)",
                     status.starts, status.start_failures));
    record("media-listener-stop", stopped,
           fmt("stopped in %llu ms", static_cast<unsigned long long>(stop_ms)));
  }

  // 5. Leaving while a lyric request is on the wire: the worker must stop
  //    promptly whether or not the network answers.
  {
    lyrics_start(app);
    TrackInfo track;
    // A name no provider has, so that nothing is ever added to the lyric cache.
    track.title = "Self Test Zqxvj Kwpfh";
    track.artist = "Youtube Lyrics Self Test";
    track.duration_s = 125.0;
    lyrics_enqueue(track, 1, true);
    Sleep(250);  // long enough for the request to be sent, too short to finish reliably
    const ULONGLONG stop_started = GetTickCount64();
    const bool stopped = lyrics_stop();
    const ULONGLONG stop_ms = GetTickCount64() - stop_started;
    record("exit-during-lookup", stopped && stop_ms < 5000,
           fmt("worker stopped in %llu ms", static_cast<unsigned long long>(stop_ms)));
  }

  // 6. The report builds, names this version, and does not carry the account
  //    name or the profile path.
  const std::string report = build_diagnostic_report(app, false);
  {
    const std::string user = environment_value(L"USERNAME");
    const std::string profile = environment_value(L"USERPROFILE");
    const bool has_version = report.find(YTL_VERSION_STRING) != std::string::npos;
    const bool no_profile = profile.size() < 4 || report.find(profile) == std::string::npos;
    const bool no_user_path =
        user.size() < 2 || (report.find("\\Users\\" + user) == std::string::npos &&
                            report.find("/Users/" + user) == std::string::npos);
    const std::string planted =
        redact_report("C:\\Users\\" + (user.empty() ? std::string("someone") : user) +
                          "\\x?access_token=PLANTED-SECRET-123",
                      profile, user);
    const bool planted_hidden = planted.find("PLANTED-SECRET-123") == std::string::npos &&
                                (user.size() < 2 || planted.find(user) == std::string::npos);
    record("report-redaction", has_version && no_profile && no_user_path && planted_hidden,
           fmt("version present %d, profile path absent %d, account path absent %d, "
               "planted credential masked %d",
               has_version, no_profile, no_user_path, planted_hidden));
  }

  out += std::string("\r\nRESULT: ") + (all_ok ? "PASS" : "FAIL") + "\r\n\r\n";
  out += "---- diagnostic report produced during the self-test ----\r\n";
  out += report;
  const bool written = write_diagnostic_report(out, path);
  return all_ok && written;
}

}  // namespace ytl
