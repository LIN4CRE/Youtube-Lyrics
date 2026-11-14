// Youtube-Lyrics automatic updates: only the project site and official stable
// GitHub Releases are trusted.
#include "update.h"

#include "app.h"
#include "version.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <new>
#include <mutex>
#include <string>
#include <vector>

namespace ytl {
namespace {

enum class JobKind { None, Check, Download };
struct Job {
  JobKind kind = JobKind::None;
  bool manual = false;
  UpdateRelease release;
};

HANDLE g_thread = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_wake_event = nullptr;
HANDLE g_cancel_event = nullptr;
HWND g_app_window = nullptr;
std::mutex g_job_mutex;
std::mutex g_request_mutex;
Job g_job;
// Last thing the updater did, for the diagnostic report.
std::mutex g_status_mutex;
std::string g_status = "no update check has run in this session";

void set_status(const std::string& text) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char stamp[16];
  std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
  std::lock_guard<std::mutex> lock(g_status_mutex);
  g_status = stamp + text;
}

bool g_download_queued_or_running = false;
bool g_check_queued_or_running = false;
HINTERNET g_active_request = nullptr;

constexpr uint64_t kMaxReleaseMetadata = 128u << 10;
constexpr uint64_t kMaxExecutableBytes = 64u << 20;
constexpr int kMaxRedirects = 4;

bool event_signalled(HANDLE event) {
  return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

bool stopping() { return event_signalled(g_stop_event); }
bool download_cancelled() {
  return stopping() || event_signalled(g_cancel_event);
}

template <typename T>
void post_owned(UINT message, T* payload, WPARAM wparam = 0) {
  if (!g_app_window ||
      !PostMessageW(g_app_window, message, wparam,
                    reinterpret_cast<LPARAM>(payload)))
    delete payload;
}

void post_error(const std::string& text, bool visible) {
  post_owned(WM_APP_UPDATE_ERROR, new std::string(text), visible ? 1 : 0);
}

std::wstring quote_arg(const std::wstring& value) {
  std::wstring out = L"\"";
  for (wchar_t ch : value) {
    if (ch == L'"') out += L'\\';
    out += ch;
  }
  out += L'"';
  return out;
}

struct ParsedUrl {
  std::wstring host;
  std::wstring path;
  std::wstring extra;
  INTERNET_PORT port = 0;
  bool https = false;
};

bool parse_url(const std::string& text, ParsedUrl& out) {
  const std::wstring url = utf8_to_wide(text);
  if (url.empty()) return false;
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  parts.dwUserNameLength = static_cast<DWORD>(-1);
  parts.dwPasswordLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
    return false;
  if (parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0) return false;
  out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
  std::transform(out.host.begin(), out.host.end(), out.host.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength)
    out.extra.assign(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  out.port = parts.nPort;
  out.https = parts.nScheme == INTERNET_SCHEME_HTTPS;
  return out.https && out.port == INTERNET_DEFAULT_HTTPS_PORT;
}

bool allowed_redirect_url(const std::string& url) {
  ParsedUrl parsed;
  if (!parse_url(url, parsed)) return false;
  return parsed.host == L"github.com" ||
         parsed.host == L"release-assets.githubusercontent.com" ||
         parsed.host == L"objects.githubusercontent.com";
}

bool release_asset_url(const std::string& url, const std::string& tag,
                       const char* asset_name) {
  ParsedUrl parsed;
  if (!parse_url(url, parsed) || parsed.host != L"github.com" ||
      !parsed.extra.empty())
    return false;
  const std::wstring expected = L"/LIN4CRE/Youtube-Lyrics/releases/download/" +
                                utf8_to_wide(tag) + L"/" +
                                utf8_to_wide(asset_name);
  return parsed.path == expected;
}

// The project site publishes the same three files beside the download page.
// It is a second official source so that updates do not depend on GitHub
// Releases being published.
const wchar_t kSiteHost[] = L"youtube-lyrics-seven.vercel.app";
const char kSiteBase[] = "https://youtube-lyrics-seven.vercel.app/downloads/";

bool site_asset_url(const std::string& url, const char* asset_name) {
  ParsedUrl parsed;
  if (!parse_url(url, parsed) || parsed.host != kSiteHost || !parsed.extra.empty())
    return false;
  return parsed.path == L"/downloads/" + utf8_to_wide(asset_name);
}

void set_active_request(HINTERNET request) {
  std::lock_guard<std::mutex> lock(g_request_mutex);
  g_active_request = request;
}

void close_active_request(HINTERNET request) {
  bool close = false;
  {
    std::lock_guard<std::mutex> lock(g_request_mutex);
    if (g_active_request == request) {
      g_active_request = nullptr;
      close = true;
    }
  }
  if (close && request) WinHttpCloseHandle(request);
}

void cancel_current_request() {
  HINTERNET request = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_request_mutex);
    request = g_active_request;
    g_active_request = nullptr;
  }
  if (request) WinHttpCloseHandle(request);
}

std::string win_error(const char* step, DWORD code) {
  char message[192];
  std::snprintf(message, sizeof(message), "%s (Windows error %lu)", step,
                static_cast<unsigned long>(code));
  return message;
}

struct Response {
  int status = 0;
  std::string location;
  std::string body;
  uint64_t bytes_written = 0;
  uint64_t content_length = 0;
  bool has_content_length = false;
};

bool request_once(const std::string& url, const wchar_t* accept,
                  uint64_t max_bytes, HANDLE output_file, Response& response,
                  std::string& error) {
  ParsedUrl parsed;
  if (!parse_url(url, parsed)) {
    error = "The update server returned an invalid or non-HTTPS URL.";
    return false;
  }

  HINTERNET session = WinHttpOpen(
      L"YoutubeLyrics/" YTL_VERSION_WIDE L" (+https://github.com/LIN4CRE/Youtube-Lyrics)",
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    error = win_error("Could not initialize secure update networking", GetLastError());
    return false;
  }
  WinHttpSetTimeouts(session, 7000, 7000, 12000, 30000);

  HINTERNET connection = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
  if (!connection) {
    const DWORD code = GetLastError();
    WinHttpCloseHandle(session);
    error = win_error("Could not connect to GitHub", code);
    return false;
  }
  std::wstring path = parsed.path.empty() ? L"/" : parsed.path;
  path += parsed.extra;
  HINTERNET request = WinHttpOpenRequest(
      connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) {
    const DWORD code = GetLastError();
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    error = win_error("Could not open the GitHub update request", code);
    return false;
  }

  DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                   sizeof(redirect_policy));
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  WinHttpSetOption(request, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                   sizeof(protocols));
  std::wstring headers = L"Accept: ";
  headers += accept;
  headers += L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
  WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1),
                           WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
  set_active_request(request);
  if (stopping() || (output_file != INVALID_HANDLE_VALUE && download_cancelled())) {
    close_active_request(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    error = "Update operation cancelled.";
    return false;
  }

  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    const DWORD code = GetLastError();
    close_active_request(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    error = win_error("GitHub did not complete the update request", code);
    return false;
  }

  DWORD status = 0;
  DWORD status_bytes = sizeof(status);
  if (!WinHttpQueryHeaders(request,
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_bytes,
                           WINHTTP_NO_HEADER_INDEX)) {
    const DWORD code = GetLastError();
    close_active_request(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    error = win_error("Could not read GitHub's response status", code);
    return false;
  }
  response.status = static_cast<int>(status);
  if (status >= 300 && status < 400) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                        WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes >= sizeof(wchar_t)) {
      std::vector<wchar_t> location(bytes / sizeof(wchar_t) + 1, L'\0');
      if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                              WINHTTP_HEADER_NAME_BY_INDEX, location.data(), &bytes,
                              WINHTTP_NO_HEADER_INDEX)) {
        response.location = wide_to_utf8(location.data());
      }
    }
    close_active_request(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return true;
  }
  if (status < 200 || status >= 300) {
    close_active_request(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    error = "GitHub returned HTTP " + std::to_string(status) + ".";
    return false;
  }

  DWORD content_length = 0;
  DWORD length_bytes = sizeof(content_length);
  if (WinHttpQueryHeaders(request,
                          WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &content_length,
                          &length_bytes, WINHTTP_NO_HEADER_INDEX)) {
    response.content_length = content_length;
    response.has_content_length = true;
    if (response.content_length > max_bytes) {
      close_active_request(request);
      WinHttpCloseHandle(connection);
      WinHttpCloseHandle(session);
      error = "The GitHub update asset exceeds the safe size limit.";
      return false;
    }
  }

  bool ok = true;
  for (;;) {
    if (output_file != INVALID_HANDLE_VALUE && download_cancelled()) {
      error = "Update download cancelled.";
      ok = false;
      break;
    }
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) {
      error = win_error("Could not read the GitHub response", GetLastError());
      ok = false;
      break;
    }
    if (available == 0) break;
    if (response.bytes_written > max_bytes || available > max_bytes - response.bytes_written) {
      error = "The GitHub response exceeds the safe size limit.";
      ok = false;
      break;
    }
    char buffer[32768];
    const DWORD wanted = std::min<DWORD>(available, sizeof(buffer));
    DWORD received = 0;
    if (!WinHttpReadData(request, buffer, wanted, &received) || received == 0) {
      error = win_error("The GitHub download was interrupted", GetLastError());
      ok = false;
      break;
    }
    if (output_file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      if (!WriteFile(output_file, buffer, received, &written, nullptr) || written != received) {
        error = win_error("Could not save the downloaded update", GetLastError());
        ok = false;
        break;
      }
    } else {
      response.body.append(buffer, received);
    }
    response.bytes_written += received;
    if (response.bytes_written > max_bytes) {
      error = "The GitHub response exceeds the safe size limit.";
      ok = false;
      break;
    }
    if (output_file != INVALID_HANDLE_VALUE && g_app_window &&
        response.has_content_length && response.content_length > 0) {
      const int percent = static_cast<int>(std::min<uint64_t>(
          100, (response.bytes_written * 100) / response.content_length));
      PostMessageW(g_app_window, WM_APP_UPDATE_PROGRESS, percent, 0);
    }
  }

  if (ok && response.has_content_length &&
      response.bytes_written != response.content_length) {
    error = "The downloaded file ended before the advertised size.";
    ok = false;
  }
  close_active_request(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return ok;
}

bool get_url(const std::string& initial_url, const wchar_t* accept,
             uint64_t max_bytes, std::string& body, std::string& error) {
  std::string url = initial_url;
  for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
    Response response;
    if (!request_once(url, accept, max_bytes, INVALID_HANDLE_VALUE, response, error))
      return false;
    if (response.status >= 200 && response.status < 300) {
      body = std::move(response.body);
      return true;
    }
    if (response.status < 300 || response.status >= 400 || response.location.empty() ||
        redirect == kMaxRedirects || !allowed_redirect_url(response.location)) {
      error = "GitHub returned an unsupported update redirect.";
      return false;
    }
    url = response.location;
  }
  error = "Too many redirects while checking for updates.";
  return false;
}

bool get_asset(const std::string& initial_url, const std::string& path,
               uint64_t expected_size, std::string& error) {
  HANDLE file = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = win_error("Could not create the update download file", GetLastError());
    return false;
  }

  std::string url = initial_url;
  bool ok = false;
  uint64_t downloaded = 0;
  for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
    Response response;
    if (!request_once(url, L"application/octet-stream", kMaxExecutableBytes,
                      file, response, error))
      break;
    if (response.status >= 200 && response.status < 300) {
      downloaded = response.bytes_written;
      ok = true;
      break;
    }
    if (response.status < 300 || response.status >= 400 || response.location.empty() ||
        redirect == kMaxRedirects || !allowed_redirect_url(response.location)) {
      error = "GitHub returned an unsupported executable download redirect.";
      break;
    }
    url = response.location;
  }

  if (ok && expected_size && downloaded != expected_size) {
    error = "The GitHub release asset size does not match its release metadata.";
    ok = false;
  }
  if (ok && !FlushFileBuffers(file)) {
    error = win_error("Could not flush the update download to disk", GetLastError());
    ok = false;
  }
  CloseHandle(file);
  if (!ok) DeleteFileW(utf8_to_wide(path).c_str());
  return ok;
}

bool valid_hex_sha256(const std::string& value) {
  if (value.size() != 64) return false;
  for (unsigned char c : value) {
    if (!std::isxdigit(c)) return false;
  }
  return true;
}


std::string json_string(const JValue& root, const char* key) {
  return root.get_str(key);
}

bool json_true(const JValue& root, const char* key) {
  const JValue* value = root.find(key);
  return value && value->type == JType::Bool && value->b;
}

uint64_t json_u64(const JValue& root, const char* key) {
  const JValue* value = root.find(key);
  if (!value || !value->is_number() || !std::isfinite(value->num) ||
      value->num < 0 || value->num > static_cast<double>(kMaxExecutableBytes) ||
      std::floor(value->num) != value->num)
    return 0;
  return static_cast<uint64_t>(value->num);
}

// Reads the release published with the project site. Returns true when the
// site answered with valid metadata; `newer` says whether it is an update.
bool check_site(UpdateRelease& release, bool& newer, std::string& error) {
  newer = false;
  const std::string base = kSiteBase;
  std::string metadata_body;
  if (!get_url(base + "release.json", L"application/json", kMaxReleaseMetadata,
               metadata_body, error))
    return false;
  ReleaseMetadata metadata;
  if (!parse_release_metadata(metadata_body, "LIN4CRE/Youtube-Lyrics", metadata) ||
      metadata.size_bytes < (128u << 10) || metadata.size_bytes > kMaxExecutableBytes) {
    error = "The project site returned invalid release metadata.";
    return false;
  }
  StableVersion site_version, current_version;
  if (!parse_stable_version(metadata.version, site_version) ||
      !parse_stable_version(YTL_VERSION_STRING, current_version)) {
    error = "The project site release has an unsupported semantic version.";
    return false;
  }
  if (compare_stable_versions(site_version, current_version) <= 0) {
    release.version = YTL_VERSION_STRING;
    return true;
  }
  std::string checksum_body, sidecar_hash;
  if (!get_url(base + "YoutubeLyrics.exe.sha256", L"text/plain", 4096, checksum_body,
               error))
    return false;
  if (!parse_sha256_sidecar(checksum_body, sidecar_hash) ||
      sidecar_hash != metadata.sha256) {
    error = "The site checksum file does not match release.json.";
    return false;
  }
  release.version = metadata.version;
  release.tag = metadata.tag;
  release.asset_url = base + "YoutubeLyrics.exe";
  release.asset_size = metadata.size_bytes;
  release.sha256 = metadata.sha256;
  newer = true;
  return true;
}

bool check_github(UpdateRelease& release, std::string& error);

bool check_latest(UpdateRelease& release, std::string& error) {
  // The project site first; GitHub Releases second. An update found by either
  // is offered, and a failure of one does not hide the other.
  UpdateRelease site;
  bool site_newer = false;
  std::string site_error;
  const bool site_ok = check_site(site, site_newer, site_error);
  if (site_ok && site_newer) {
    release = site;
    return true;
  }
  UpdateRelease hub;
  std::string hub_error;
  if (check_github(hub, hub_error)) {
    release = hub;
    return true;
  }
  if (site_ok) {
    release = site;
    return true;
  }
  error = site_error.empty() ? hub_error : site_error;
  return false;
}

bool check_github(UpdateRelease& release, std::string& error) {
  const std::string api_url =
      "https://api.github.com/repos/LIN4CRE/Youtube-Lyrics/releases/latest";
  std::string response;
  if (!get_url(api_url, L"application/vnd.github+json", kMaxReleaseMetadata,
               response, error))
    return false;

  JValue root;
  if (!json_parse(response, root) || !root.is_object()) {
    error = "GitHub returned invalid release metadata.";
    return false;
  }
  if (json_true(root, "draft") || json_true(root, "prerelease")) {
    error = "The latest GitHub release is not a stable release.";
    return false;
  }
  const std::string tag = json_string(root, "tag_name");
  StableVersion release_version, current_version;
  if (!parse_stable_version(tag, release_version) ||
      !parse_stable_version(YTL_VERSION_STRING, current_version)) {
    error = "The latest GitHub release has an unsupported semantic version.";
    return false;
  }
  if (compare_stable_versions(release_version, current_version) <= 0) {
    release.version = YTL_VERSION_STRING;
    return true;
  }

  const JValue* assets = root.find("assets");
  if (!assets || !assets->is_array()) {
    error = "The latest GitHub release has no downloadable assets.";
    return false;
  }
  std::string exe_url, metadata_url, checksum_url;
  uint64_t exe_size = 0;
  for (const JValue& asset : assets->arr) {
    if (!asset.is_object()) continue;
    const std::string name = asset.get_str("name");
    const std::string url = asset.get_str("browser_download_url");
    if (name == "YoutubeLyrics.exe") {
      exe_url = url;
      exe_size = json_u64(asset, "size");
    } else if (name == "release.json") {
      metadata_url = url;
    } else if (name == "YoutubeLyrics.exe.sha256") {
      checksum_url = url;
    }
  }
  if (exe_url.empty() || metadata_url.empty() || checksum_url.empty() ||
      exe_size < (128u << 10) || exe_size > kMaxExecutableBytes ||
      !release_asset_url(exe_url, tag, "YoutubeLyrics.exe") ||
      !release_asset_url(metadata_url, tag, "release.json") ||
      !release_asset_url(checksum_url, tag, "YoutubeLyrics.exe.sha256")) {
    error = "The latest stable release is missing a trusted Windows x64 asset.";
    return false;
  }

  std::string metadata_body;
  if (!get_url(metadata_url, L"application/json", kMaxReleaseMetadata,
               metadata_body, error))
    return false;
  ReleaseMetadata metadata;
  if (!parse_release_metadata(metadata_body, "LIN4CRE/Youtube-Lyrics", metadata) ||
      metadata.tag != tag || metadata.size_bytes != exe_size) {
    error = "The release version, asset, architecture, or checksum metadata is invalid.";
    return false;
  }
  const std::string version = metadata.version;
  const std::string metadata_hash = metadata.sha256;
  std::string checksum_body;
  if (!get_url(checksum_url, L"text/plain", 4096, checksum_body, error)) return false;
  std::string sidecar_hash;
  if (!parse_sha256_sidecar(checksum_body, sidecar_hash) ||
      sidecar_hash != metadata_hash) {
    error = "The release checksum file does not match release.json.";
    return false;
  }

  release.version = version;
  release.tag = tag;
  release.asset_url = exe_url;
  release.asset_size = exe_size;
  release.sha256 = metadata_hash;
  return true;
}

bool sha256_file(const std::wstring& path, std::string& out) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  PUCHAR object = nullptr;
  bool ok = false;
  DWORD object_size = 0, bytes = 0, digest_size = 0;
  HANDLE file = INVALID_HANDLE_VALUE;

  NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                 nullptr, 0);
  if (status < 0) goto cleanup;
  status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                             reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                             &bytes, 0);
  if (status < 0 || object_size == 0 || object_size > (1u << 20)) goto cleanup;
  status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                             reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size),
                             &bytes, 0);
  if (status < 0 || digest_size != 32) goto cleanup;
  object = new (std::nothrow) UCHAR[object_size];
  if (!object) goto cleanup;
  status = BCryptCreateHash(algorithm, &hash, object, object_size, nullptr, 0, 0);
  if (status < 0) goto cleanup;
  file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) goto cleanup;
  for (;;) {
    UCHAR buffer[65536];
    DWORD got = 0;
    if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) goto cleanup;
    if (got == 0) break;
    status = BCryptHashData(hash, buffer, got, 0);
    if (status < 0) goto cleanup;
  }
  {
    UCHAR digest[32];
    status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (status < 0) goto cleanup;
    static const char hex[] = "0123456789abcdef";
    out.clear();
    out.reserve(64);
    for (UCHAR byte : digest) {
      out.push_back(hex[byte >> 4]);
      out.push_back(hex[byte & 0x0F]);
    }
  }
  ok = true;

cleanup:
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (hash) BCryptDestroyHash(hash);
  if (object) delete[] object;
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}

bool valid_x64_pe(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= (128 << 10) &&
            size.QuadPart <= static_cast<LONGLONG>(kMaxExecutableBytes);
  BYTE dos[64]{};
  DWORD got = 0;
  if (!ok || !ReadFile(file, dos, sizeof(dos), &got, nullptr) || got != sizeof(dos) ||
      dos[0] != 'M' || dos[1] != 'Z') {
    CloseHandle(file);
    return false;
  }
  const DWORD pe_offset = static_cast<DWORD>(dos[0x3C]) |
                          (static_cast<DWORD>(dos[0x3D]) << 8) |
                          (static_cast<DWORD>(dos[0x3E]) << 16) |
                          (static_cast<DWORD>(dos[0x3F]) << 24);
  if (pe_offset > static_cast<DWORD>(size.QuadPart) - 6 || pe_offset > (1u << 20)) {
    CloseHandle(file);
    return false;
  }
  LARGE_INTEGER seek{};
  seek.QuadPart = pe_offset;
  if (!SetFilePointerEx(file, seek, nullptr, FILE_BEGIN)) {
    CloseHandle(file);
    return false;
  }
  BYTE header[6]{};
  got = 0;
  ok = ReadFile(file, header, sizeof(header), &got, nullptr) && got == sizeof(header) &&
       header[0] == 'P' && header[1] == 'E' && header[2] == 0 && header[3] == 0 &&
       header[4] == 0x64 && header[5] == 0x86;
  CloseHandle(file);
  return ok;
}

bool download_release(const UpdateRelease& release, UpdateStage& stage,
                     std::string& error) {
  if ((!release_asset_url(release.asset_url, release.tag, "YoutubeLyrics.exe") &&
       !site_asset_url(release.asset_url, "YoutubeLyrics.exe")) ||
      !valid_hex_sha256(release.sha256) || release.asset_size < (128u << 10) ||
      release.asset_size > kMaxExecutableBytes) {
    error = "The update package failed release-source validation.";
    return false;
  }
  if (download_cancelled()) {
    error = "Update download cancelled.";
    return false;
  }
  const std::wstring dir = utf8_to_wide(appdata_dir() + "\\updates");
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    error = win_error("Could not create the local update folder", GetLastError());
    return false;
  }
  const std::wstring filename = L"YoutubeLyrics-" + utf8_to_wide(release.version) + L".exe";
  const std::wstring part = dir + L"\\" + filename + L".part";
  const std::wstring complete = dir + L"\\" + filename;
  const std::string part_utf8 = wide_to_utf8(part);
  if (!get_asset(release.asset_url, part_utf8, release.asset_size, error)) return false;

  std::string actual_hash;
  if (!sha256_file(part, actual_hash) || actual_hash != release.sha256) {
    DeleteFileW(part.c_str());
    error = "The downloaded update's SHA-256 checksum did not match. The existing app was left untouched.";
    return false;
  }
  if (!valid_x64_pe(part)) {
    DeleteFileW(part.c_str());
    error = "The downloaded update is not a valid 64-bit Windows executable.";
    return false;
  }
  if (!MoveFileExW(part.c_str(), complete.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(part.c_str());
    error = win_error("Could not prepare the verified update", GetLastError());
    return false;
  }
  stage.path = wide_to_utf8(complete);
  stage.version = release.version;
  stage.sha256 = release.sha256;
  return true;
}

bool get_release(UpdateRelease& release, std::string& error) {
  if (stopping()) return false;
  return check_latest(release, error);
}

void do_check(bool manual) {
  UpdateRelease release;
  std::string error;
  if (!get_release(release, error)) {
    if (!stopping()) {
      set_status("check failed: " + error);
      // An automatic check stays silent on screen, but not in the log: a
      // machine that can never reach the update server should be diagnosable.
      logf("update: %s check failed: %s", manual ? "manual" : "automatic", error.c_str());
    }
    if (!stopping() && manual) post_error(error, true);
    return;
  }
  if (release.asset_url.empty()) {
    set_status("up to date (" YTL_VERSION_STRING " is the newest stable release)");
    if (manual && g_app_window) PostMessageW(g_app_window, WM_APP_UPDATE_NONE, 1, 0);
    return;
  }
  set_status("version " + release.version + " is available (" +
             std::to_string(release.asset_size) + " bytes, from " +
             (site_asset_url(release.asset_url, "YoutubeLyrics.exe") ? "the project site"
                                                                     : "GitHub Releases") +
             ")");
  logf("update: version %s is available", release.version.c_str());
  post_owned(WM_APP_UPDATE_AVAILABLE,
             new UpdateRelease(std::move(release)), manual ? 1 : 0);
}

void do_download(const UpdateRelease& release) {
  PostMessageW(g_app_window, WM_APP_UPDATE_PROGRESS, 1000, 0);
  UpdateStage stage;
  std::string error;
  if (!download_release(release, stage, error)) {
    if (!stopping()) {
      set_status("download of " + release.version + " failed: " + error);
      logf("update: download failed: %s", error.c_str());
      post_error(error, true);
    }
    return;
  }
  set_status("version " + release.version + " downloaded and verified");
  logf("update: version %s downloaded and verified", release.version.c_str());
  post_owned(WM_APP_UPDATE_READY, new UpdateStage(std::move(stage)));
}

DWORD WINAPI update_thread_main(LPVOID) {
  HANDLE handles[2] = {g_stop_event, g_wake_event};
  for (;;) {
    const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
    if (wait != WAIT_OBJECT_0 + 1) continue;

    Job job;
    {
      std::lock_guard<std::mutex> lock(g_job_mutex);
      job = std::move(g_job);
      g_job = Job();
    }
    if (job.kind == JobKind::Check) {
      do_check(job.manual);
      std::lock_guard<std::mutex> lock(g_job_mutex);
      g_check_queued_or_running = false;
    } else if (job.kind == JobKind::Download) {
      do_download(job.release);
      std::lock_guard<std::mutex> lock(g_job_mutex);
      g_download_queued_or_running = false;
    }
    if (stopping()) break;
  }
  return 0;
}

const char kUpdateScript[] = R"YTLYUPDATE(
param(
  [Parameter(Mandatory=$true)][string]$Target,
  [Parameter(Mandatory=$true)][string]$Stage,
  [Parameter(Mandatory=$true)][string]$ExpectedHash,
  [Parameter(Mandatory=$true)][int]$ParentPid
)
$ErrorActionPreference = 'Stop'
$backup = $Target + '.ytl-backup'
$replacement = $Target + '.ytl-new'
$dir = Split-Path -Parent $Target
try {
  $parent = Get-Process -Id $ParentPid -ErrorAction SilentlyContinue
  if ($null -ne $parent) {
    # Bounded: if the running copy never exits, nothing has been touched yet,
    # so give up cleanly instead of waiting for ever as a hidden process.
    if (-not $parent.WaitForExit(60000)) {
      Remove-Item -LiteralPath $Stage -Force -ErrorAction SilentlyContinue
      exit 2
    }
  }
  $sourceHash = (Get-FileHash -LiteralPath $Stage -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($sourceHash -ne $ExpectedHash.ToLowerInvariant()) { throw 'Staged update checksum mismatch.' }
  Copy-Item -LiteralPath $Stage -Destination $replacement -Force
  $copyHash = (Get-FileHash -LiteralPath $replacement -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($copyHash -ne $ExpectedHash.ToLowerInvariant()) { throw 'Copied update checksum mismatch.' }
  if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force }
  [System.IO.File]::Replace($replacement, $Target, $backup, $true)
  $newProcess = Start-Process -FilePath $Target -ArgumentList '--updated' -WorkingDirectory $dir -PassThru
  Start-Sleep -Milliseconds 1800
  if ($newProcess.HasExited) { throw 'Updated application exited during startup.' }
  Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $Stage -Force -ErrorAction SilentlyContinue
  exit 0
} catch {
  Remove-Item -LiteralPath $replacement -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $backup) {
    Remove-Item -LiteralPath $Target -Force -ErrorAction SilentlyContinue
    Move-Item -LiteralPath $backup -Destination $Target -Force
  }
  Remove-Item -LiteralPath $Stage -Force -ErrorAction SilentlyContinue
  if (Test-Path -LiteralPath $Target) {
    Start-Process -FilePath $Target -ArgumentList '--update-failed' -WorkingDirectory $dir
  }
  exit 1
}
)YTLYUPDATE";

}  // namespace

std::string update_status_text() {
  std::lock_guard<std::mutex> lock(g_status_mutex);
  return g_status;
}

// An update that was interrupted (power loss, a killed helper) can leave a
// partial download, an unused staged copy, or the replacement helper's
// ".ytl-new"/".ytl-backup" files beside the executable. This process is
// running, so the executable in place is good and none of them are needed.
void update_cleanup_leftovers(bool helper_may_be_running) {
  int removed = 0;
  const std::wstring dir = utf8_to_wide(appdata_dir() + "\\updates");
  WIN32_FIND_DATAW entry{};
  HANDLE search = FindFirstFileW((dir + L"\\YoutubeLyrics-*").c_str(), &entry);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (DeleteFileW((dir + L"\\" + entry.cFileName).c_str())) ++removed;
    } while (FindNextFileW(search, &entry));
    FindClose(search);
  }
  wchar_t self[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  // Straight after a replacement the helper is still watching this process
  // start, and the backup is what it would roll back to.
  if (!helper_may_be_running && length > 0 && length < MAX_PATH) {
    for (const wchar_t* suffix : {L".ytl-new", L".ytl-backup"}) {
      const std::wstring path = std::wstring(self) + suffix;
      if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
          DeleteFileW(path.c_str()))
        ++removed;
    }
  }
  if (removed > 0) logf("update: removed %d leftover file(s) from an earlier update", removed);
}

void update_start(App& app, bool automatic_check, bool launched_by_update_helper) {
  if (g_thread) return;
  update_cleanup_leftovers(launched_by_update_helper);
  g_app_window = app.hwnd;
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_stop_event || !g_wake_event || !g_cancel_event) {
    logf("update: could not create worker events");
    if (g_stop_event) CloseHandle(g_stop_event);
    if (g_wake_event) CloseHandle(g_wake_event);
    if (g_cancel_event) CloseHandle(g_cancel_event);
    g_stop_event = g_wake_event = g_cancel_event = nullptr;
    return;
  }
  g_thread = CreateThread(nullptr, 0, update_thread_main, nullptr, 0, nullptr);
  if (!g_thread) {
    logf("update: could not start the update worker (error %lu)",
         static_cast<unsigned long>(GetLastError()));
    CloseHandle(g_stop_event);
    CloseHandle(g_wake_event);
    CloseHandle(g_cancel_event);
    g_stop_event = g_wake_event = g_cancel_event = nullptr;
    return;
  }
  if (automatic_check) update_request_check(app, false);
}

void update_request_check(App& app, bool manual) {
  if (!g_thread || stopping()) {
    if (manual) app.show_toast("Update checking is unavailable right now.");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    if (g_job.kind != JobKind::None || g_check_queued_or_running ||
        g_download_queued_or_running) {
      if (manual) app.show_toast("An update operation is already queued. Try again shortly.");
      return;
    }
    g_check_queued_or_running = true;
    g_job.kind = JobKind::Check;
    g_job.manual = manual;
  }
  SetEvent(g_wake_event);
}

void update_request_download(App& app, const UpdateRelease& release) {
  if (!g_thread || stopping()) {
    post_error("Update installation is unavailable because the worker has stopped.", true);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_job_mutex);
    if (g_download_queued_or_running || g_check_queued_or_running ||
        g_job.kind != JobKind::None) {
      app.show_toast("Another update operation is already running.");
      return;
    }
    g_download_queued_or_running = true;
    g_job.kind = JobKind::Download;
    g_job.release = release;
  }
  ResetEvent(g_cancel_event);
  SetEvent(g_wake_event);
}

void update_cancel_download() {
  if (g_cancel_event) SetEvent(g_cancel_event);
  cancel_current_request();
}

bool update_apply_staged(App& app, const UpdateStage& stage, std::string* error) {
  if (stage.path.empty() || !valid_hex_sha256(stage.sha256) ||
      !valid_x64_pe(utf8_to_wide(stage.path))) {
    if (error) *error = "The verified update file is missing or invalid.";
    return false;
  }
  std::string actual_hash;
  if (!sha256_file(utf8_to_wide(stage.path), actual_hash) ||
      actual_hash != stage.sha256) {
    if (error) *error = "The staged update failed its final SHA-256 check.";
    return false;
  }

  const std::wstring appdata = utf8_to_wide(appdata_dir());
  const std::wstring stage_path = utf8_to_wide(stage.path);
  if (stage_path.size() <= appdata.size() ||
      _wcsnicmp(stage_path.c_str(), appdata.c_str(), appdata.size()) != 0 ||
      (stage_path[appdata.size()] != L'\\' && stage_path[appdata.size()] != L'/')) {
    if (error) *error = "The update was not staged inside Youtube-Lyrics' local update folder.";
    return false;
  }

  wchar_t target_buffer[MAX_PATH] = {};
  const DWORD target_len = GetModuleFileNameW(nullptr, target_buffer, MAX_PATH);
  if (target_len == 0 || target_len >= MAX_PATH) {
    if (error) *error = win_error("Could not locate the running executable", GetLastError());
    return false;
  }
  const std::wstring script_path = appdata + L"\\apply-update.ps1";
  if (!write_file_atomic(wide_to_utf8(script_path), kUpdateScript)) {
    if (error) *error = "Could not prepare the local update helper script.";
    return false;
  }

  wchar_t system_dir[MAX_PATH] = {};
  const UINT system_len = GetSystemDirectoryW(system_dir, MAX_PATH);
  if (system_len == 0 || system_len >= MAX_PATH) {
    if (error) *error = win_error("Could not locate Windows PowerShell", GetLastError());
    return false;
  }
  const std::wstring powershell = std::wstring(system_dir) +
      L"\\WindowsPowerShell\\v1.0\\powershell.exe";
  std::wstring command = quote_arg(powershell) +
      L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File " +
      quote_arg(script_path) + L" -Target " + quote_arg(target_buffer) +
      L" -Stage " + quote_arg(stage_path) + L" -ExpectedHash " +
      quote_arg(utf8_to_wide(stage.sha256)) + L" -ParentPid " +
      std::to_wstring(GetCurrentProcessId());
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(powershell.c_str(), mutable_command.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                      appdata.c_str(), &startup, &process)) {
    if (error) *error = win_error("Could not start the local updater", GetLastError());
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

void update_stop() {
  if (!g_thread) return;
  SetEvent(g_stop_event);
  if (g_cancel_event) SetEvent(g_cancel_event);
  cancel_current_request();
  SetEvent(g_wake_event);
  WaitForSingleObject(g_thread, INFINITE);
  CloseHandle(g_thread);
  CloseHandle(g_stop_event);
  CloseHandle(g_wake_event);
  CloseHandle(g_cancel_event);
  g_thread = nullptr;
  g_stop_event = g_wake_event = g_cancel_event = nullptr;
  g_app_window = nullptr;
  std::lock_guard<std::mutex> lock(g_job_mutex);
  g_job = Job();
  g_download_queued_or_running = false;
  g_check_queued_or_running = false;
}

}  // namespace ytl
