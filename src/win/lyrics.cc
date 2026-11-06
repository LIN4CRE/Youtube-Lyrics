// Youtube-Lyrics - lyrics lookup (LRCLIB) + on-disk cache + worker thread.
#include "app.h"

#include <winhttp.h>

#include <cstdio>
#include <cstring>

namespace ytl {

// LRCLIB asks API clients to identify themselves.
static const wchar_t* kUserAgent =
    L"YoutubeLyrics/1.1 (https://github.com/LIN4CRE/Youtube-Lyrics)";

// ---------------------------------------------------------------------------
// Minimal blocking HTTP GET (WinHTTP). Runs on the lyrics worker thread only.
// ---------------------------------------------------------------------------
std::string http_get(const std::wstring& url, int timeout_ms, int* status_out,
                     std::string* err_out) {
  std::string body;
  if (status_out) *status_out = 0;
  if (err_out) err_out->clear();

  auto fail = [&](const char* what, DWORD code) {
    if (err_out) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s (error %lu)", what,
                    static_cast<unsigned long>(code));
      *err_out = buf;
    }
    return std::string();
  };

  HINTERNET session = WinHttpOpen(
      kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session) return fail("WinHttpOpen", GetLastError());

  WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms * 2);

  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwHostNameLength = static_cast<DWORD>(-1);
  uc.dwUrlPathLength = static_cast<DWORD>(-1);
  uc.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
    DWORD e = GetLastError();
    WinHttpCloseHandle(session);
    return fail("WinHttpCrackUrl", e);
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.dwExtraInfoLength > 0) {
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  }
  bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

  HINTERNET conn = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
  if (!conn) {
    DWORD e = GetLastError();
    WinHttpCloseHandle(session);
    return fail("WinHttpConnect", e);
  }
  HINTERNET req = WinHttpOpenRequest(
      conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
  if (!req) {
    DWORD e = GetLastError();
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return fail("WinHttpOpenRequest", e);
  }

  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  WinHttpSetOption(req, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                   sizeof(protocols));
  WinHttpAddRequestHeaders(req, L"Accept: application/json", static_cast<DWORD>(-1),
                           WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    DWORD e = GetLastError();
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return fail("WinHttpSendRequest", e);
  }
  if (!WinHttpReceiveResponse(req, nullptr)) {
    DWORD e = GetLastError();
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return fail("WinHttpReceiveResponse", e);
  }

  DWORD status = 0, len = sizeof(status);
  WinHttpQueryHeaders(req,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                      WINHTTP_NO_HEADER_INDEX);
  if (status_out) *status_out = static_cast<int>(status);

  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail)) break;
    if (avail == 0) break;
    if (body.size() > (8u << 20)) break;  // sanity cap
    char buf[8192];
    DWORD want = avail < sizeof(buf) ? avail : sizeof(buf);
    DWORD got = 0;
    if (!WinHttpReadData(req, buf, want, &got) || got == 0) break;
    body.append(buf, got);
  }

  WinHttpCloseHandle(req);
  WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return body;
}


namespace {



App* g_app_for_lyrics = nullptr;

HANDLE g_thread = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_wake = nullptr;

struct Job {
  int id = 0;
  TrackInfo track;
  bool force = false;
};

std::mutex g_job_mu;
bool g_has_job = false;
int g_job_id = 0;
TrackInfo g_job_track;
bool g_job_force = false;

std::mutex g_result_mu;
struct Result {
  int id = 0;
  LyricsState state = LyricsState::Idle;
  Lyrics lyrics;
  std::string msg;
};
Result g_result;

bool ensure_cache_dir() {
  static int state = 0;  // 0 unknown, 1 ok, -1 failed
  if (state != 0) return state == 1;
  std::string dir = appdata_dir();
  if (dir.empty()) return !(state = -1);
  std::string cache = dir + "\\cache";
  CreateDirectoryW(utf8_to_wide(cache).c_str(), nullptr);
  state = 1;
  return true;
}

std::string cache_path(const TrackInfo& t) {
  return appdata_dir() + "\\cache\\" + fnv_hex(track_key(t)) + ".txt";
}

std::string serialize_lyrics(const Lyrics& l) {
  std::string s = "YTLY1\n";
  s += "title=" + l.title + "\n";
  s += "artist=" + l.artist + "\n";
  s += "album=" + l.album + "\n";
  s += "source=" + l.source + "\n";
  s += std::string("synced=") + (l.synced ? "1" : "0") + "\n";
  s += "--lrc--\n";
  for (const auto& line : l.lines) {
    char stamp[32];
    long long ms = static_cast<long long>(line.t_ms);
    std::snprintf(stamp, sizeof(stamp), "[%02lld:%02lld.%02lld]",
                  ms / 60000, (ms / 1000) % 60, (ms % 1000) / 10);
    s += stamp;
    s += line.text;
    s += "\n";
  }
  s += "--plain--\n";
  s += l.plain;
  s += "\n";
  return s;
}

bool parse_cache(const std::string& data, Lyrics& out) {
  if (!starts_with(data, "YTLY1\n")) return false;
  out = Lyrics();
  size_t pos = 6;
  bool synced = false;
  std::string lrc_section, plain_section;
  size_t lrc_at = data.find("--lrc--\n");
  size_t plain_at = data.find("--plain--\n");
  if (lrc_at == std::string::npos || plain_at == std::string::npos) return false;

  // header key=value block
  size_t line_end = lrc_at;
  while (pos < line_end) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos || nl > line_end) break;
    std::string kv = data.substr(pos, nl - pos);
    pos = nl + 1;
    size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "title") out.title = v;
    else if (k == "artist") out.artist = v;
    else if (k == "album") out.album = v;
    else if (k == "source") out.source = v;
    else if (k == "synced") synced = (v == "1");
  }
  lrc_section = data.substr(lrc_at + 8, plain_at - (lrc_at + 8));
  plain_section = data.substr(plain_at + 10);

  if (synced) {
    if (!lrc_parse(lrc_section, out.lines) || out.lines.empty()) return false;
    out.synced = true;
    return true;
  }
  out.synced = false;
  out.plain = plain_section;
  while (!out.plain.empty() && (out.plain.back() == '\n' || out.plain.back() == '\r'))
    out.plain.pop_back();
  return !out.plain.empty();
}

std::string build_get_url(const TrackInfo& t) {
  std::string url = "https://lrclib.net/api/get?";
  url += "artist_name=" + url_encode(t.artist);
  url += "&track_name=" + url_encode(t.title);
  if (!t.album.empty()) url += "&album_name=" + url_encode(t.album);
  if (t.duration_s >= 5) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "&duration=%d",
                  static_cast<int>(t.duration_s + 0.5));
    url += buf;
  }
  return url;
}

std::string build_search_url(const TrackInfo& t) {
  std::string url = "https://lrclib.net/api/search?";
  url += "track_name=" + url_encode(t.title);
  if (!t.artist.empty()) url += "&artist_name=" + url_encode(t.artist);
  return url;
}

std::string build_query_url(const TrackInfo& t) {
  std::string q = t.artist.empty() ? t.title : (t.artist + " " + t.title);
  return "https://lrclib.net/api/search?q=" + url_encode(q);
}

// One attempt: fetch `url`, parse, and store the lyrics on success.
bool try_fetch(const TrackInfo& track, const std::string& url, Lyrics& out,
               std::string& why) {
  int status = 0;
  std::string err;
  std::string body = http_get(utf8_to_wide(url), 12000, &status, &err);
  if (!err.empty()) {
    why = err;
    logf("lyrics: %s -> transport error: %s", url.c_str(), err.c_str());
    return false;
  }
  logf("lyrics: %s -> HTTP %d (%zu bytes)", url.c_str(), status, body.size());
  if (status != 200) {
    why = (status == 404) ? "No lyrics in the database for this track."
                          : ("Lookup failed (HTTP " + std::to_string(status) + ").");
    return false;
  }
  Lyrics parsed;
  if (!parse_lrclib_payload(body, track, parsed) || parsed.empty()) {
    why = "No usable lyrics in the database for this track.";
    return false;
  }
  out = parsed;
  if (out.title.empty()) out.title = track.title;
  if (out.artist.empty()) out.artist = track.artist;
  return true;
}

LyricsState do_lookup(const TrackInfo& track, bool force, Lyrics& out,
                      std::string& msg) {
  if (!force && ensure_cache_dir()) {
    std::string cached = read_file(cache_path(track));
    Lyrics l;
    if (!cached.empty() && parse_cache(cached, l)) {
      logf("lyrics: cache hit for %s", track_key(track).c_str());
      out = l;
      return LyricsState::Ready;
    }
  }

  std::string why;
  if (try_fetch(track, build_get_url(track), out, why)) return LyricsState::Ready;
  // Exact title+artist match failed; fall back to fuzzy search.
  if (try_fetch(track, build_search_url(track), out, why)) return LyricsState::Ready;
  if (!track.artist.empty() &&
      try_fetch(track, build_query_url(track), out, why)) {
    return LyricsState::Ready;
  }

  msg = why.empty() ? "No lyrics found." : why;
  return LyricsState::NotFound;
}

DWORD WINAPI thread_main(LPVOID) {
  while (true) {
    HANDLE waits[2] = {g_stop, g_wake};
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0 + 1) break;  // stop signalled

    Job job;
    {
      std::lock_guard<std::mutex> lock(g_job_mu);
      if (!g_has_job) continue;
      job.id = g_job_id;
      job.track = g_job_track;
      job.force = g_job_force;
      g_has_job = false;
    }

    Lyrics lyr;
    std::string msg;
    LyricsState state = do_lookup(job.track, job.force, lyr, msg);
    if (state == LyricsState::Ready && ensure_cache_dir()) {
      write_file_atomic(cache_path(job.track), serialize_lyrics(lyr));
    }

    {
      std::lock_guard<std::mutex> lock(g_result_mu);
      g_result.id = job.id;
      g_result.state = state;
      g_result.lyrics = lyr;
      g_result.msg = msg;
    }
    if (g_app_for_lyrics) PostMessageW(g_app_for_lyrics->hwnd, WM_APP_LYRICS, 0, 0);
  }
  return 0;
}

}  // namespace

void lyrics_enqueue(const TrackInfo& track, int req_id, bool force) {
  {
    std::lock_guard<std::mutex> lock(g_job_mu);
    g_job_id = req_id;
    g_job_track = track;
    g_job_force = force;
    g_has_job = true;
  }
  if (g_wake) SetEvent(g_wake);
}

void lyrics_start(App& app) {
  g_app_for_lyrics = &app;
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_thread = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr);
}

void lyrics_stop() {
  if (g_stop) SetEvent(g_stop);
  if (g_thread) {
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  if (g_stop) CloseHandle(g_stop);
  if (g_wake) CloseHandle(g_wake);
  g_stop = g_thread = g_wake = nullptr;
}

// Reads a finished lookup. Returns false when there is nothing new for `req_id`.
bool lyrics_take_result(int req_id, LyricsState* state, Lyrics* lyrics,
                        std::string* msg) {
  std::lock_guard<std::mutex> lock(g_result_mu);
  if (g_result.id != req_id || req_id == 0) return false;
  *state = g_result.state;
  if (lyrics) *lyrics = g_result.lyrics;
  if (msg) *msg = g_result.msg;
  return true;
}

}  // namespace ytl
