// Youtube-Lyrics - lyrics lookup (LRCLIB) + on-disk cache + worker thread.
#include "app.h"
#include "version.h"

#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>

namespace ytl {

// LRCLIB asks API clients to identify themselves. The version comes from
// VERSION so it can never drift from the published build.
static const wchar_t* kUserAgent =
    L"YoutubeLyrics/" YTL_VERSION_WIDE L" (https://github.com/LIN4CRE/Youtube-Lyrics)";

namespace {
std::mutex g_http_handle_mu;
HINTERNET g_active_http_request = nullptr;
bool g_http_cancelled = false;

void cancel_active_http_request() {
  HINTERNET request = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_http_handle_mu);
    g_http_cancelled = true;
    request = g_active_http_request;
    g_active_http_request = nullptr;
  }
  // Closing the active WinHTTP request cancels an in-flight synchronous call.
  if (request) WinHttpCloseHandle(request);
}

// Aborts the request in flight without latching the "shutting down" flag: used
// when a newer track supersedes the lookup that is currently on the wire.
void abort_active_http_request() {
  HINTERNET request = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_http_handle_mu);
    request = g_active_http_request;
    g_active_http_request = nullptr;
  }
  if (request) WinHttpCloseHandle(request);
}

void reset_http_cancellation() {
  std::lock_guard<std::mutex> lock(g_http_handle_mu);
  g_http_cancelled = false;
}

bool http_cancel_requested() {
  std::lock_guard<std::mutex> lock(g_http_handle_mu);
  return g_http_cancelled;
}
}  // namespace

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

  // Keep lookups responsive when a provider is offline. The chain has fallbacks,
  // so one stalled host must not freeze the display for a minute.
  timeout_ms = std::max(1000, std::min(timeout_ms, 30000));
  WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms * 2);

  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwHostNameLength = static_cast<DWORD>(-1);
  uc.dwUrlPathLength = static_cast<DWORD>(-1);
  uc.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
    const DWORD error = GetLastError();
    WinHttpCloseHandle(session);
    return fail("WinHttpCrackUrl", error);
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.dwExtraInfoLength > 0) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

  HINTERNET connection = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
  if (!connection) {
    const DWORD error = GetLastError();
    WinHttpCloseHandle(session);
    return fail("WinHttpConnect", error);
  }
  HINTERNET request = WinHttpOpenRequest(
      connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
  if (!request) {
    const DWORD error = GetLastError();
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return fail("WinHttpOpenRequest", error);
  }

  {
    std::lock_guard<std::mutex> lock(g_http_handle_mu);
    if (g_http_cancelled) {
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connection);
      WinHttpCloseHandle(session);
      return fail("HTTP request cancelled", ERROR_OPERATION_ABORTED);
    }
    g_active_http_request = request;
  }
  auto close_request = [&]() {
    bool should_close = false;
    {
      std::lock_guard<std::mutex> lock(g_http_handle_mu);
      if (g_active_http_request == request) {
        g_active_http_request = nullptr;
        should_close = true;
      }
    }
    // The stop path may already have closed this handle.
    if (should_close) WinHttpCloseHandle(request);
  };
  auto close_handles = [&]() {
    close_request();
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
  };

  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  WinHttpSetOption(request, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                   sizeof(protocols));
  WinHttpAddRequestHeaders(request, L"Accept: application/json",
                           static_cast<DWORD>(-1),
                           WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    const DWORD error = GetLastError();
    close_handles();
    return fail("WinHttpSendRequest", error);
  }
  if (!WinHttpReceiveResponse(request, nullptr)) {
    const DWORD error = GetLastError();
    close_handles();
    return fail("WinHttpReceiveResponse", error);
  }

  DWORD status = 0;
  DWORD length = sizeof(status);
  if (!WinHttpQueryHeaders(request,
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
                           WINHTTP_NO_HEADER_INDEX)) {
    const DWORD error = GetLastError();
    close_handles();
    return fail("WinHttpQueryHeaders", error);
  }
  if (status_out) *status_out = static_cast<int>(status);

  constexpr size_t kMaxResponseBytes = 8u << 20;
  DWORD read_error = ERROR_SUCCESS;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) {
      read_error = GetLastError();
      break;
    }
    if (available == 0) break;
    if (body.size() >= kMaxResponseBytes) {
      read_error = ERROR_FILE_TOO_LARGE;
      break;
    }
    char buffer[8192];
    const size_t remaining = kMaxResponseBytes - body.size();
    const DWORD wanted = static_cast<DWORD>(std::min<size_t>(
        std::min<size_t>(available, sizeof(buffer)), remaining));
    DWORD received = 0;
    if (!WinHttpReadData(request, buffer, wanted, &received) || received == 0) {
      read_error = GetLastError();
      if (read_error == ERROR_SUCCESS) read_error = ERROR_INVALID_DATA;
      break;
    }
    body.append(buffer, received);
  }

  close_handles();
  if (read_error != ERROR_SUCCESS) return fail("WinHTTP response read", read_error);
  return body;
}

namespace {

App* g_app_for_lyrics = nullptr;
std::mutex g_lyrics_app_mu;

HANDLE g_thread = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_wake = nullptr;

struct Job {
  int id = 0;
  TrackInfo track;
  bool force = false;
};

std::mutex g_job_mu;
// Newest request id handed to lyrics_enqueue(). A lookup that is no longer the
// newest gives up between provider attempts instead of holding the only worker
// for a song that has already finished.
std::atomic<int> g_latest_job_id{0};
bool g_has_job = false;
int g_job_id = 0;
TrackInfo g_job_track;
bool g_job_force = false;

std::mutex g_result_mu;
std::mutex g_cache_mu;
struct Result {
  int id = 0;
  LyricsState state = LyricsState::Idle;
  Lyrics lyrics;
  std::string msg;
};
Result g_result;

// Recent lookup activity, kept in memory for the diagnostic report.
std::mutex g_trace_mu;
std::deque<std::string> g_trace;

void trace(const std::string& text) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char stamp[16];
  std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
  std::lock_guard<std::mutex> lock(g_trace_mu);
  g_trace.push_back(stamp + text);
  while (g_trace.size() > 80) g_trace.pop_front();
}

// What a provider URL asks for, without the query (for the lookup trace).
const char* request_kind(const std::string& url) {
  if (url.find("lyrics.ovh") != std::string::npos) return "lyrics.ovh exact";
  if (url.find("/api/get") != std::string::npos) return "LRCLIB exact";
  if (url.find("track_name=") != std::string::npos) return "LRCLIB title+artist search";
  return "LRCLIB text search";
}

// Days since a file was last written, or -1 when that cannot be read.
int file_age_days(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(utf8_to_wide(path).c_str(), GetFileExInfoStandard, &data))
    return -1;
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER a, b;
  a.LowPart = data.ftLastWriteTime.dwLowDateTime;
  a.HighPart = data.ftLastWriteTime.dwHighDateTime;
  b.LowPart = now.dwLowDateTime;
  b.HighPart = now.dwHighDateTime;
  if (b.QuadPart <= a.QuadPart) return 0;
  return static_cast<int>((b.QuadPart - a.QuadPart) / 10000000ULL / 86400ULL);
}

bool ensure_cache_dir() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, []() {
    const std::string dir = appdata_dir();
    if (dir.empty()) return;
    const std::string cache = dir + "\\cache";
    if (CreateDirectoryW(utf8_to_wide(cache).c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
      ok = true;
    }
  });
  return ok;
}
std::string cache_path(const TrackInfo& t) {
  return appdata_dir() + "\\cache\\" + fnv_hex(track_key(t)) + ".txt";
}

std::string serialize_lyrics(const Lyrics& l) {
  std::string s = "YTLY2\n";
  s += "title=" + l.title + "\n";
  s += "artist=" + l.artist + "\n";
  s += "album=" + l.album + "\n";
  s += "source=" + l.source + "\n";
  s += "variant=" + l.variant + "\n";
  s += std::string("synced=") + (l.synced ? "1" : "0") + "\n";
  s += "--lrc--\n";
  s += lyrics_to_lrc(l.lines);
  s += "--plain--\n";
  s += l.plain;
  s += "\n";
  return s;
}

bool parse_cache(const std::string& data, Lyrics& out) {
  // "YTLY1" sheets were stored by builds whose matching accepted look-alike
  // titles and other cuts of a song; they are ignored and fetched again.
  if (!starts_with(data, "YTLY2\n")) return false;
  out = Lyrics();
  size_t pos = 6;
  bool synced = false;
  std::string lrc_section, plain_section;
  size_t lrc_at = data.find("--lrc--\n");
  size_t plain_at = data.find("--plain--\n");
  if (lrc_at == std::string::npos || plain_at == std::string::npos ||
      plain_at < lrc_at + 8) return false;

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
    else if (k == "variant") out.variant = v;
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

namespace {

// The edition word LRCLIB itself uses in track titles, appended to queries so a
// live or remix session asks for the recording it is actually playing. The
// studio variant asks with the plain title.
const char* variant_suffix(TrackVariant variant) {
  switch (variant) {
    case TrackVariant::Live: return " (Live)";
    case TrackVariant::Remix: return " (Remix)";
    case TrackVariant::Instrumental: return " (Instrumental)";
    case TrackVariant::Karaoke: return " (Karaoke)";
    case TrackVariant::Studio:
    default: return "";
  }
}

std::string variant_query_title(const TrackInfo& t) {
  return t.title + variant_suffix(t.variant());
}

}  // namespace

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

std::string build_variant_get_url(const TrackInfo& t) {
  if (t.variant() == TrackVariant::Studio) return build_get_url(t);
  TrackInfo variant = t;
  variant.title = variant_query_title(t);
  return build_get_url(variant);
}

std::string build_search_url(const TrackInfo& t) {
  std::string url = "https://lrclib.net/api/search?";
  url += "track_name=" + url_encode(t.title);
  if (!t.artist.empty()) url += "&artist_name=" + url_encode(t.artist);
  return url;
}

std::string build_variant_search_url(const TrackInfo& t) {
  if (t.variant() == TrackVariant::Studio) return build_search_url(t);
  TrackInfo variant = t;
  variant.title = variant_query_title(t);
  return build_search_url(variant);
}

// Where a hand-written override lives: %LOCALAPPDATA%\YouTubeLyrics\lyrics\
// <Artist> - <Title>.lrc (or .txt). Anything dropped there wins over the network.
std::string manual_path(const TrackInfo& t) {
  std::string dir = appdata_dir() + "\\lyrics";
  std::string base = t.artist.empty() ? t.title : (t.artist + " - " + t.title);
  const TrackVariant variant = t.variant();
  if (variant != TrackVariant::Studio) base += std::string(" (") + track_variant_name(variant) + ")";
  for (char& c : base) {
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
        c == '|' || c == '?' || c == '*') {
      c = '_';
    }
  }
  std::string leaf = base + ".lrc";
  std::string p = dir + "\\" + leaf;
  if (!read_file(p).empty()) return p;
  return dir + "\\" + base + ".txt";
}

std::string build_ovh_url(const TrackInfo& t) {
  return "https://api.lyrics.ovh/v1/" + url_encode(t.artist) + "/" +
         url_encode(t.title);
}

std::string build_title_only_url(const TrackInfo& t) {
  return "https://lrclib.net/api/search?track_name=" + url_encode(t.title);
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
  std::string body = http_get(utf8_to_wide(url), 8000, &status, &err);
  if (!err.empty()) {
    why = "Lyrics provider transport error: " + err;
    logf("lyrics: %s -> transport error: %s", url.c_str(), err.c_str());
    return false;
  }
  logf("lyrics: %s -> HTTP %d (%zu bytes)", url.c_str(), status, body.size());
  if (status != 200) {
    if (status == 404) {
      why = "No lyrics in the database for this track.";
    } else if (status == 429 || (status >= 500 && status <= 599)) {
      why = "Provider is temporarily rate-limited or unavailable; try again later.";
    } else {
      why = "Lookup failed (HTTP " + std::to_string(status) + ").";
    }
    return false;
  }
  Lyrics parsed;
  const bool ovh = url.find("lyrics.ovh") != std::string::npos;
  const bool ok = ovh ? parse_lyrics_ovh_payload(body, track, parsed)
                      : parse_lrclib_payload(body, track, parsed);
  if (!ok || parsed.empty()) {
    why = "No usable lyrics in the database for this track.";
    return false;
  }
  if (!ovh) {
    // Record which catalogue answered, so the display can say so honestly.
    parsed.source = "LRCLIB";
  }
  out = parsed;
  if (out.title.empty()) out.title = track.title;
  if (out.artist.empty()) out.artist = track.artist;
  return true;
}

// A sheet is only reused on a later attempt when it is still the right song:
// the same recording, the same edition, and (when both sides report one) a
// comparable length. This is what stops a rejection earlier in the chain from
// being undone by a looser query later.
bool sheet_fits(const Lyrics& sheet, const TrackInfo& want) {
  if (sheet.empty()) return false;
  // The lyrics carry the provider's own title/artist, not the session's.
  TrackInfo found{sheet.title, sheet.artist, sheet.album, 0.0, std::string()};
  if (!found.title.empty() && text_similarity(found.title, want.title) < 0.62 &&
      token_containment(found.title, want.title) < 0.70) {
    return false;
  }
  if (want.variant() != TrackVariant::Studio &&
      !variant_compatible(want.variant(), sheet.title, sheet.album)) {
    return false;
  }
  // Timestamps that run past the end of this recording are for another cut.
  if (!lyrics_fit_duration(sheet, want.duration_s)) return false;
  return true;
}

LyricsState do_lookup(const TrackInfo& track, bool force, int job_id, Lyrics& out,
                      std::string& msg, bool& cacheable) {
  cacheable = true;
  trace("lookup: " + track_label(track) +
        (track.duration_s > 0 ? " (" + format_time(static_cast<int64_t>(
                                           track.duration_s * 1000.0)) + ")"
                              : std::string(" (length unknown)")) +
        ", edition " + track_variant_name(track.variant()) +
        (force ? ", forced" : ""));
  // A lyric sheet the user dropped in by hand always wins, including over a
  // previously cached automatic result.
  {
    std::string manual = read_file(manual_path(track));
    Lyrics l;
    if (!manual.empty() && parse_local_lyrics(manual, l)) {
      trace(std::string("  local file -> used (") + (l.synced ? "timed" : "untimed") + ")");
      logf("lyrics: using local override for %s", track_key(track).c_str());
      l.title = track.title;
      l.artist = track.artist;
      out = l;
      return LyricsState::Ready;
    }
  }

  Lyrics stale_untimed;        // an old untimed cache entry, kept as a fallback
  bool have_stale_untimed = false;
  if (!force && ensure_cache_dir()) {
    std::string cached;
    {
      std::lock_guard<std::mutex> lock(g_cache_mu);
      cached = read_file(cache_path(track));
    }
    Lyrics l;
    if (!cached.empty() && parse_cache(cached, l)) {
      // A cached sheet is re-checked against the track it is about to be shown
      // for; a stale or mismatched entry is dropped rather than trusted.
      // An untimed sheet is the fallback answer. Catalogues gain timed lyrics
      // all the time, so an old untimed entry is looked up again instead of
      // pinning the song to the fallback for ever.
      constexpr int kUntimedCacheDays = 7;
      const int age = l.synced ? 0 : file_age_days(cache_path(track));
      if (!sheet_fits(l, track)) {
        trace("  cache -> entry no longer fits this recording; looking again");
        logf("lyrics: cached sheet no longer fits %s; refetching",
             track_key(track).c_str());
      } else if (!l.synced && age >= kUntimedCacheDays) {
        trace("  cache -> untimed entry is " + std::to_string(age) +
              " days old; looking for timed lyrics again");
        stale_untimed = l;
        have_stale_untimed = true;
      } else {
        trace(std::string("  cache -> hit (") + (l.synced ? "timed" : "untimed") +
              ", from " + (l.source.empty() ? "unknown source" : l.source) + ")");
        logf("lyrics: cache hit for %s", track_key(track).c_str());
        out = l;
        return LyricsState::Ready;
      }
    }
  }

  std::string why;
  // A network or provider failure is the reason worth reporting (and the one
  // the display retries on); a later "not in the database" must not hide it.
  std::string transient_why;
  std::set<std::string> asked;  // the same URL is never requested twice
  std::string plain_fallback;   // kept in case nothing timed turns up
  Lyrics plain_lyrics;
  int tried = 0;
  bool lrclib_blocked = false;
  // A bounded chain: the split-title fallback can generate dozens of queries,
  // and a track that needs more than this many is not going to be found.
  constexpr int kMaxProviderAttempts = 12;
  // ...and a wall-clock budget, so one slow provider cannot keep the display
  // on "Looking for lyrics" for minutes.
  const ULONGLONG started = GetTickCount64();
  constexpr ULONGLONG kLookupBudgetMs = 30000;

  auto attempt = [&](const TrackInfo& t, const std::string& url, const char* name) {
    if (http_cancel_requested()) return false;
    if (g_latest_job_id.load() != job_id) return false;  // superseded
    if (GetTickCount64() - started > kLookupBudgetMs) return false;
    const bool lrclib = url.find("lrclib.net") != std::string::npos;
    if (lrclib && lrclib_blocked) return false;
    // The last attempt is kept for the independent fallback provider, so a
    // long chain of LRCLIB searches cannot crowd it out.
    if (tried >= (lrclib ? kMaxProviderAttempts - 1 : kMaxProviderAttempts)) return false;
    if (!asked.insert(url).second) return false;
    ++tried;
    Lyrics l;
    std::string detail;
    if (!try_fetch(t, url, l, detail)) {
      if (!detail.empty()) why = detail;
      const bool transient =
          detail.find("transport error") != std::string::npos ||
          detail.find("rate-limited or unavailable") != std::string::npos;
      if (transient && transient_why.empty()) transient_why = detail;
      if (lrclib && transient) lrclib_blocked = true;
      trace(std::string("  ") + request_kind(url) + " -> " +
            (detail.empty() ? "no result" : detail));
      return false;
    }
    // Timed lyrics for the wrong edition are a worse answer than no lyrics:
    // fall back to "not found" rather than putting the studio sheet under a
    // live recording.
    if (!sheet_fits(l, track)) {
      logf("lyrics: rejected '%s' from %s (wrong edition or length)",
           track_label(TrackInfo{l.title, l.artist, l.album, 0, std::string()}).c_str(),
           name);
      why = "No lyrics found for this recording.";
      trace(std::string("  ") + request_kind(url) +
            " -> rejected: wrong edition, title or length");
      return false;
    }
    if (l.synced && !l.empty()) {
      l.providers_tried = tried;
      out = l;
      trace(std::string("  ") + request_kind(url) + " -> accepted, timed (" +
            std::to_string(l.lines.size()) + " lines)");
      return true;
    }
    trace(std::string("  ") + request_kind(url) +
          " -> untimed words only; still looking for timed lyrics");
    // Untimed lyrics are still useful, but only once every timed source has
    // been tried - a scrolling sheet beats no sheet, and beats a static one.
    if (plain_fallback.empty() && !l.empty()) {
      plain_fallback = name;
      plain_lyrics = l;
    }
    return false;
  };

  // 1. Exact match (fastest, most precise). For a live/remix/instrumental
  //    session, ask for that edition first so the plain studio sheet cannot win
  //    the race on a title-only search.
  if (attempt(track, build_get_url(track), "LRCLIB")) return LyricsState::Ready;
  if (attempt(track, build_variant_get_url(track), "LRCLIB")) return LyricsState::Ready;
  // 2. Structured search - title + artist, then free text.
  if (attempt(track, build_search_url(track), "LRCLIB")) return LyricsState::Ready;
  if (attempt(track, build_variant_search_url(track), "LRCLIB")) return LyricsState::Ready;
  if (!track.artist.empty() && attempt(track, build_query_url(track), "LRCLIB")) {
    return LyricsState::Ready;
  }
  // 3. Title-only LRCLIB search; candidates still pass best_candidate()'s
  //    title/artist/duration checks before any lyrics are shown.
  if (attempt(track, build_title_only_url(track), "LRCLIB")) return LyricsState::Ready;

  // 3b. Split title search: YouTube titles are frequently formatted with separators,
  // e.g. "Artist - Title" or "Remixer — Title — Original Artist | Channel"
  std::vector<std::string> parts;
  std::string rem = track.title;
  while (!rem.empty()) {
    size_t best_sep = std::string::npos;
    size_t sep_len = 0;
    for (const char* sep : {" | ", " - ", " \xE2\x80\x93 ", " \xE2\x80\x94 ", " / ", " X ", " x ", " vs ", " vs. ", " VS "}) {
      size_t found = rem.find(sep);
      if (found != std::string::npos && (best_sep == std::string::npos || found < best_sep)) {
        best_sep = found;
        sep_len = std::strlen(sep);
      }
    }
    if (best_sep != std::string::npos) {
      std::string seg = trim(rem.substr(0, best_sep));
      if (!seg.empty()) parts.push_back(seg);
      rem = trim(rem.substr(best_sep + sep_len));
    } else {
      std::string seg = trim(rem);
      if (!seg.empty()) parts.push_back(seg);
      break;
    }
  }

  if (parts.size() >= 2) {
    for (size_t i = 0; i < parts.size(); ++i) {
      for (size_t j = 0; j < parts.size(); ++j) {
        if (i == j) continue;
        TrackInfo cand = clean_track(TrackInfo{parts[j], parts[i], track.album,
                                               track.duration_s, track.raw_title},
                                     false);
        if (!cand.title.empty() && !cand.artist.empty()) {
          if (attempt(cand, build_search_url(cand), "LRCLIB")) return LyricsState::Ready;
          if (attempt(cand, build_query_url(cand), "LRCLIB")) return LyricsState::Ready;
        }
      }
      TrackInfo t_only = clean_track(TrackInfo{parts[i], track.artist, track.album,
                                               track.duration_s, track.raw_title},
                                     false);
      if (!t_only.title.empty()) {
        if (attempt(t_only, build_search_url(t_only), "LRCLIB")) return LyricsState::Ready;
        if (attempt(t_only, build_query_url(t_only), "LRCLIB")) return LyricsState::Ready;
      }
      TrackInfo t_bare = clean_track(TrackInfo{parts[i], "", track.album,
                                               track.duration_s, track.raw_title},
                                     false);
      if (!t_bare.title.empty()) {
        if (attempt(t_bare, build_title_only_url(t_bare), "LRCLIB")) return LyricsState::Ready;
      }
    }
  }

  // 4. Final, lower-confidence fallback: lyrics.ovh is queried by exact
  //    artist/title but does not return identity metadata for independent checks.
  if (!track.artist.empty() && attempt(track, build_ovh_url(track), "lyrics.ovh")) {
    return LyricsState::Ready;
  }

  // An untimed sheet found while LRCLIB was unreachable (or the time budget ran
  // out) is shown, but not cached: the next play should try for timed lyrics
  // again instead of being stuck with the fallback for ever.
  if (lrclib_blocked || GetTickCount64() - started > kLookupBudgetMs)
    cacheable = false;
  if (plain_fallback.empty() && have_stale_untimed) {
    // Nothing better turned up: the words already on disk are still the answer.
    plain_fallback = stale_untimed.source.empty() ? "the cache" : stale_untimed.source;
    plain_lyrics = stale_untimed;
  }
  if (!plain_fallback.empty()) {
    plain_lyrics.providers_tried = tried;
    if (plain_lyrics.title.empty()) plain_lyrics.title = track.title;
    if (plain_lyrics.artist.empty()) plain_lyrics.artist = track.artist;
    logf("lyrics: falling back to untimed lyrics from %s", plain_fallback.c_str());
    trace("  result: untimed lyrics from " + plain_fallback +
          (cacheable ? "" : " (not cached: a provider was unreachable)"));
    out = plain_lyrics;
    return LyricsState::Ready;
  }

  msg = !transient_why.empty() ? transient_why : (why.empty() ? "No lyrics found." : why);
  trace("  result: none after " + std::to_string(tried) + " request(s): " + msg);
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
    bool cacheable = true;
    LyricsState state = do_lookup(job.track, job.force, job.id, lyr, msg, cacheable);
    // A newer track arrived while this one was on the wire: its result would
    // be ignored by the UI anyway, and must not be cached from a cut-short run.
    if (g_latest_job_id.load() != job.id) continue;
    // Manual overrides are re-read on every lookup; never leave a stale copy
    // in the automatic cache if the user later removes that local file.
    if (state == LyricsState::Ready && cacheable && lyr.source != "local file") {
      std::lock_guard<std::mutex> lock(g_cache_mu);
      if (ensure_cache_dir())
        write_file_atomic(cache_path(job.track), serialize_lyrics(lyr));
    }

    {
      std::lock_guard<std::mutex> lock(g_result_mu);
      g_result.id = job.id;
      g_result.state = state;
      g_result.lyrics = lyr;
      g_result.msg = msg;
    }
    {
      std::lock_guard<std::mutex> lock(g_lyrics_app_mu);
      if (g_app_for_lyrics)
        PostMessageW(g_app_for_lyrics->hwnd, WM_APP_LYRICS, 0, 0);
    }
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
    g_latest_job_id.store(req_id);
  }
  // Whatever is on the wire is for an older track now.
  abort_active_http_request();
  if (g_wake) SetEvent(g_wake);
}

void lyrics_start(App& app) {
  reset_http_cancellation();
  {
    std::lock_guard<std::mutex> lock(g_lyrics_app_mu);
    g_app_for_lyrics = &app;
  }
  g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g_stop || !g_wake) {
    logf("lyrics: unable to create worker events");
    if (g_stop) CloseHandle(g_stop);
    if (g_wake) CloseHandle(g_wake);
    g_stop = g_wake = nullptr;
    std::lock_guard<std::mutex> lock(g_lyrics_app_mu);
    g_app_for_lyrics = nullptr;
    return;
  }
  g_thread = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr);
  if (!g_thread) {
    logf("lyrics: unable to start worker thread");
    CloseHandle(g_stop);
    CloseHandle(g_wake);
    g_stop = g_wake = nullptr;
    std::lock_guard<std::mutex> lock(g_lyrics_app_mu);
    g_app_for_lyrics = nullptr;
  }
}

void lyrics_stop() {
  {
    std::lock_guard<std::mutex> lock(g_lyrics_app_mu);
    g_app_for_lyrics = nullptr;
  }
  cancel_active_http_request();
  if (g_stop) SetEvent(g_stop);
  if (g_thread) {
    // The active WinHTTP request is cancelled above; wait for worker cleanup
    // before closing its event handles or allowing App storage to disappear.
    const DWORD wait = WaitForSingleObject(g_thread, 5000);
    if (wait != WAIT_OBJECT_0) {
      // Keep the stop/wake handles alive for the worker. The App pointer is
      // already detached, and process teardown will reclaim these handles.
      logf("lyrics: worker did not stop within five seconds (wait=%lu)",
           static_cast<unsigned long>(wait));
      return;
    }
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  if (g_stop) CloseHandle(g_stop);
  if (g_wake) CloseHandle(g_wake);
  g_stop = g_wake = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_job_mu);
    g_has_job = false;
  }
}
std::vector<std::string> lyrics_recent_lookups() {
  std::lock_guard<std::mutex> lock(g_trace_mu);
  return std::vector<std::string>(g_trace.begin(), g_trace.end());
}

bool lyrics_cache_stats(int* files, uint64_t* bytes) {
  *files = 0;
  *bytes = 0;
  const std::string directory = appdata_dir() + "\\cache";
  std::lock_guard<std::mutex> lock(g_cache_mu);
  WIN32_FIND_DATAW entry{};
  HANDLE search = FindFirstFileW(utf8_to_wide(directory + "\\*").c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
  }
  do {
    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    ++*files;
    *bytes += (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
  } while (FindNextFileW(search, &entry));
  FindClose(search);
  return true;
}

int lyrics_clear_cache() {
  if (!ensure_cache_dir()) return -1;
  std::lock_guard<std::mutex> lock(g_cache_mu);
  const std::string directory = appdata_dir() + "\\cache";
  WIN32_FIND_DATAW entry{};
  const std::wstring pattern = utf8_to_wide(directory + "\\*");
  HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? 0 : -1;
  }

  int removed = 0;
  bool failed = false;
  do {
    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring path = utf8_to_wide(directory + "\\") + entry.cFileName;
    if (DeleteFileW(path.c_str())) {
      ++removed;
    } else {
      failed = true;
      logf("lyrics: unable to clear cache file (error %lu)",
           static_cast<unsigned long>(GetLastError()));
    }
  } while (FindNextFileW(search, &entry));
  const DWORD error = GetLastError();
  FindClose(search);
  if (error != ERROR_NO_MORE_FILES) failed = true;
  return failed ? -1 : removed;
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
