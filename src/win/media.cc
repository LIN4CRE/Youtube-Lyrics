// Youtube-Lyrics - "what is playing" listener.
//
// Spawns a long lived Windows PowerShell helper (smtc.ps1, embedded in the exe)
// that reports the current media session as JSON lines, one per ~400 ms. The
// thread below reads that pipe and turns each line into a MediaState snapshot.
#include "app.h"
#include "smtc_script.h"

#include <cstdio>
#include <cstring>

namespace ytl {
namespace {

HANDLE g_thread = nullptr;
HANDLE g_stop = nullptr;
DWORD g_thread_id = 0;

struct ChildProc {
  HANDLE process = nullptr;
  HANDLE out_read = nullptr;
  std::string pending;  // partial line buffer
  int64_t started_at = 0;
  int64_t last_output_at = 0;
};

void close_child(ChildProc& c) {
  if (c.process) {
    TerminateProcess(c.process, 0);
    CloseHandle(c.process);
    c.process = nullptr;
  }
  if (c.out_read) {
    CloseHandle(c.out_read);
    c.out_read = nullptr;
  }
  c.pending.clear();
}

bool ensure_script_file() {
  static bool done = false;
  static bool ok = false;
  if (done) return ok;
  done = true;
  std::string dir = appdata_dir();
  if (dir.empty()) return ok = false;
  std::string path = dir + "\\smtc.ps1";
  std::string existing = read_file(path);
  if (existing != kSmtcScript) {
    ok = write_file_atomic(path, kSmtcScript);
  } else {
    ok = true;
  }
  return ok;
}

bool spawn_helper(ChildProc& c) {
  c.pending.clear();
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE out_read = nullptr, out_write = nullptr;
  if (!CreatePipe(&out_read, &out_write, &sa, 1 << 16)) return false;
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, 0, nullptr);
  HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

  wchar_t sysdir[MAX_PATH] = {0};
  GetSystemDirectoryW(sysdir, MAX_PATH);
  std::wstring ps = std::wstring(sysdir) +
                    L"\\WindowsPowerShell\\v1.0\\powershell.exe";

  std::wstring script_path = utf8_to_wide(appdata_dir() + "\\smtc.ps1");
  wchar_t cmd[2048];
  swprintf(cmd, 2048,
           L"\"%ls\" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
           L"-WindowStyle Hidden -File \"%ls\" -ParentPid %lu",
           ps.c_str(), script_path.c_str(),
           static_cast<unsigned long>(GetCurrentProcessId()));

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = out_write;
  si.hStdError = (nul != INVALID_HANDLE_VALUE) ? nul : out_write;
  si.hStdInput = (nul_in != INVALID_HANDLE_VALUE) ? nul_in : out_write;

  PROCESS_INFORMATION pi{};
  DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  BOOL started = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, flags, nullptr,
                                nullptr, &si, &pi);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
  CloseHandle(out_write);
  if (!started) {
    CloseHandle(out_read);
    logf("media: failed to start powershell (err %lu)", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  c.process = pi.hProcess;
  c.out_read = out_read;
  c.started_at = static_cast<int64_t>(GetTickCount64());
  c.last_output_at = c.started_at;
  logf("media: helper started (pid %lu)", static_cast<unsigned long>(pi.dwProcessId));
  return true;
}

// Handles one JSON line from the helper.
void handle_line(App& app, const std::string& line) {
  MediaState fresh;
  int64_t now = static_cast<int64_t>(GetTickCount64());
  if (!media_parse_line(line, fresh, now)) {
    logf("media: unparsable line (%zu bytes)", line.size());
    return;
  }
  {
    std::lock_guard<std::mutex> lock(app.mu);
    // Keep the previous position anchor if this poll is identical in every
    // respect but the reported position, so smoothing stays stable.
    app.media = fresh;
  }
  PostMessageW(app.hwnd, WM_APP_MEDIA, 0, 0);
}

DWORD WINAPI thread_main(LPVOID param) {
  App* app = static_cast<App*>(param);
  ChildProc child;
  DWORD last_restart = 0;

  while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
    if (!child.process) {
      DWORD now = GetTickCount();
      if (last_restart && now - last_restart < 3000) {
        Sleep(500);
        continue;
      }
      last_restart = now;
      if (!spawn_helper(child)) {
        Sleep(2000);
        continue;
      }
    }

    // Read whatever is available without blocking, so we can honour stop events.
    DWORD avail = 0;
    if (!PeekNamedPipe(child.out_read, nullptr, 0, nullptr, &avail, nullptr)) {
      // Pipe broke: helper died.
      logf("media: pipe closed, restarting helper");
      close_child(child);
      Sleep(800);
      continue;
    }
    if (avail > 0) {
      char buf[8192];
      DWORD got = 0;
      DWORD want = avail < sizeof(buf) ? avail : sizeof(buf);
      if (ReadFile(child.out_read, buf, want, &got, nullptr) && got > 0) {
        child.last_output_at = static_cast<int64_t>(GetTickCount64());
        child.pending.append(buf, got);
        size_t nl;
        while ((nl = child.pending.find('\n')) != std::string::npos) {
          std::string line = child.pending.substr(0, nl);
          child.pending.erase(0, nl + 1);
          while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
          if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
              static_cast<unsigned char>(line[1]) == 0xBB &&
              static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
          }
          if (!line.empty()) handle_line(*app, line);
        }
        if (child.pending.size() > 1 << 20) child.pending.clear();
      }
    } else {
      Sleep(30);
    }

    // Watchdog: helper silent for a long time -> restart it.
    int64_t now = static_cast<int64_t>(GetTickCount64());
    if (now - child.last_output_at > 12000) {
      logf("media: helper silent for 12s, restarting");
      close_child(child);
      Sleep(500);
    }
  }

  close_child(child);
  logf("media: thread exit");
  return 0;
}

}  // namespace

bool media_parse_line(const std::string& line, MediaState& out, int64_t now) {
  JValue v;
  if (!json_parse(line, v) || !v.is_object()) return false;

  out = MediaState();
  out.at_ms = now;
  out.raw = line;

  if (v.get_num("ok", 0) < 0.5) {
    out.have = false;
    out.status = "error";
    return true;
  }

  std::string status = v.get_str("status");
  out.status = status;
  out.playing = (status == "playing");
  std::string title = v.get_str("title");
  std::string artist = v.get_str("artist");

  if (status == "none" || (title.empty() && status != "paused")) {
    out.have = false;
    return true;
  }

  TrackInfo t;
  t.title = title;
  t.artist = artist;
  t.album = v.get_str("album");
  t.duration_s = v.get_num("dur", 0);
  out.track = clean_track(t);
  out.duration_s = t.duration_s;
  out.pos_ms = static_cast<int64_t>(v.get_num("pos", 0) * 1000.0);
  out.art_path = v.get_str("art");
  out.app = v.get_str("app");
  out.key = track_key(out.track);
  out.have = !out.track.title.empty();
  return true;
}

bool media_start(App& app) {
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_stop) return false;
  if (!ensure_script_file()) {
    logf("media: could not write helper script");
  }
  g_thread = CreateThread(nullptr, 0, thread_main, &app, 0, &g_thread_id);
  return g_thread != nullptr;
}

void media_stop() {
  if (g_stop) SetEvent(g_stop);
  if (g_thread) {
    WaitForSingleObject(g_thread, 4000);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  if (g_stop) {
    CloseHandle(g_stop);
    g_stop = nullptr;
  }
}

}  // namespace ytl
