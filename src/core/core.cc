// Youtube-Lyrics - portable core logic (see core.h).
#include "core.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <set>

namespace ytl {

// ---------------------------------------------------------------- text utils

static bool decode_utf8_char(const std::string& s, size_t i, uint32_t* cp_out,
                             size_t* length_out) {
  if (i >= s.size()) return false;
  const unsigned char first = static_cast<unsigned char>(s[i]);
  uint32_t cp = 0;
  size_t length = 0;
  if (first < 0x80) {
    cp = first;
    length = 1;
  } else if (first >= 0xC2 && first <= 0xDF) {
    cp = first & 0x1F;
    length = 2;
  } else if (first >= 0xE0 && first <= 0xEF) {
    cp = first & 0x0F;
    length = 3;
  } else if (first >= 0xF0 && first <= 0xF4) {
    cp = first & 0x07;
    length = 4;
  } else {
    return false;
  }
  if (i + length > s.size()) return false;
  for (size_t k = 1; k < length; ++k) {
    const unsigned char next = static_cast<unsigned char>(s[i + k]);
    if ((next & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (next & 0x3F);
  }
  if ((length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000) ||
      (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    return false;
  }
  *cp_out = cp;
  *length_out = length;
  return true;
}

static bool valid_utf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    size_t length = 0;
    if (!decode_utf8_char(s, i, &cp, &length)) return false;
    i += length;
  }
  return true;
}

std::wstring utf8_to_wide(const std::string& s) {
  std::wstring out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    size_t length = 0;
    if (!decode_utf8_char(s, i, &cp, &length)) {
      // Replace malformed, overlong, surrogate, and out-of-range sequences.
      out.push_back(static_cast<wchar_t>(0xFFFD));
      ++i;
      continue;
    }
    i += length;
    if (cp > 0xFFFF) {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<wchar_t>(cp));
    }
  }
  return out;
}

static void append_utf8(std::string& out, uint32_t cp) {
  if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
  if (cp > 0x10FFFF) cp = 0xFFFD;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string wide_to_utf8(const std::wstring& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t cp = static_cast<uint32_t>(s[i]);
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i + 1 < s.size()) {
        const uint32_t lo = static_cast<uint32_t>(s[i + 1]);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          ++i;
        } else {
          cp = 0xFFFD;
        }
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    append_utf8(out, cp);
  }
  return out;
}

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && static_cast<unsigned char>(s[b]) <= ' ') ++b;
  while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') --e;
  return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const char* prefix) {
  size_t n = std::strlen(prefix);
  return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

bool ends_with(const std::string& s, const char* suffix) {
  size_t n = std::strlen(suffix);
  return s.size() >= n && std::memcmp(s.data() + s.size() - n, suffix, n) == 0;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  std::string out;
  size_t pos = 0;
  while (true) {
    size_t hit = s.find(from, pos);
    if (hit == std::string::npos) {
      out.append(s, pos, std::string::npos);
      break;
    }
    out.append(s, pos, hit - pos);
    out.append(to);
    pos = hit + from.size();
  }
  return out;
}

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                 c == '~';
    if (plain) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

std::string squish(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool space = false;
  for (unsigned char c : s) {
    if (c <= ' ') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out.push_back(' ');
    space = false;
    out.push_back(static_cast<char>(c));
  }
  return out;
}

// ---------------------------------------------------------------------- JSON

const JValue* JValue::find(const char* key) const {
  if (type != JType::Object) return nullptr;
  for (const auto& kv : obj) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string JValue::get_str(const char* key, const char* def) const {
  const JValue* v = find(key);
  if (v && v->is_string()) return v->str;
  return def;
}

double JValue::get_num(const char* key, double def) const {
  const JValue* v = find(key);
  if (!v) return def;
  if (v->is_number()) return std::isfinite(v->num) ? v->num : def;
  if (v->is_string()) {
    const std::string value = trim(v->str);
    if (value.empty()) return def;
    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    if (end != value.c_str() && end && *end == '\0' && std::isfinite(number))
      return number;
  }
  return def;
}

bool parse_stable_version(const std::string& text, StableVersion& out) {
  size_t i = 0;
  if (!text.empty() && text[0] == 'v') i = 1;
  StableVersion parsed;
  uint64_t* parts[3] = {&parsed.major, &parsed.minor, &parsed.patch};
  for (int part = 0; part < 3; ++part) {
    const size_t start = i;
    if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
    uint64_t value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      const unsigned digit = static_cast<unsigned>(text[i] - '0');
      if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
      value = value * 10 + digit;
      ++i;
    }
    if (i - start > 1 && text[start] == '0') return false;
    *parts[part] = value;
    if (part < 2) {
      if (i >= text.size() || text[i] != '.') return false;
      ++i;
    }
  }
  if (i != text.size()) return false;
  out = parsed;
  return true;
}

int compare_stable_versions(const StableVersion& a, const StableVersion& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

bool parse_release_metadata(const std::string& json,
                            const std::string& expected_repository,
                            ReleaseMetadata& out) {
  JValue root;
  if (!json_parse(json, root) || !root.is_object() || expected_repository.empty() ||
      root.get_num("schema_version", 0) != 1.0)
    return false;

  ReleaseMetadata parsed;
  parsed.repository = root.get_str("repository");
  parsed.tag = root.get_str("tag");
  parsed.version = root.get_str("version");
  parsed.channel = root.get_str("channel");
  parsed.architecture = root.get_str("architecture");
  parsed.asset = root.get_str("asset");
  parsed.sha256 = to_lower_ascii(root.get_str("sha256"));
  const JValue* size_value = root.find("size_bytes");
  if (!size_value || !size_value->is_number() || !std::isfinite(size_value->num) ||
      size_value->num < 1 || size_value->num > (64u << 20) ||
      std::floor(size_value->num) != size_value->num)
    return false;
  parsed.size_bytes = static_cast<uint64_t>(size_value->num);

  StableVersion tag_version, manifest_version;
  if (parsed.tag.size() < 2 || parsed.tag[0] != 'v' ||
      !parse_stable_version(parsed.tag.substr(1), tag_version) ||
      !parse_stable_version(parsed.version, manifest_version) ||
      compare_stable_versions(tag_version, manifest_version) != 0 ||
      parsed.repository != expected_repository || parsed.channel != "stable" ||
      parsed.architecture != "x64" || parsed.asset != "YoutubeLyrics.exe" ||
      parsed.sha256.size() != 64)
    return false;
  for (unsigned char c : parsed.sha256) {
    if (!std::isxdigit(c)) return false;
  }
  out = std::move(parsed);
  return true;
}

bool parse_sha256_sidecar(const std::string& text, std::string& sha256) {
  const std::string value = trim(text);
  if (value.size() < 64) return false;
  const std::string hash = to_lower_ascii(value.substr(0, 64));
  for (unsigned char c : hash) {
    if (!std::isxdigit(c)) return false;
  }
  if (value.size() > 64) {
    if (value[64] != ' ' && value[64] != '\t' && value[64] != '\r' &&
        value[64] != '\n')
      return false;
    std::string name = trim(value.substr(64));
    if (!name.empty() && name[0] == '*') name.erase(0, 1);
    if (!name.empty() && name != "YoutubeLyrics.exe") return false;
  }
  sha256 = hash;
  return true;
}

namespace {

struct JsonParser {
  const char* p;
  const char* end;

  void skip_ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  }

  bool literal(const char* text) {
    size_t n = std::strlen(text);
    if (static_cast<size_t>(end - p) < n) return false;
    if (std::memcmp(p, text, n) != 0) return false;
    p += n;
    return true;
  }

  bool parse_string(std::string& out) {
    if (p >= end || *p != '"') return false;
    ++p;
    out.clear();
    auto read_hex4 = [&](uint32_t* value) {
      if (end - p < 4) return false;
      uint32_t result = 0;
      for (int k = 0; k < 4; ++k) {
        const char h = p[k];
        result <<= 4;
        if (h >= '0' && h <= '9') result |= static_cast<uint32_t>(h - '0');
        else if (h >= 'a' && h <= 'f') result |= static_cast<uint32_t>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') result |= static_cast<uint32_t>(h - 'A' + 10);
        else return false;
      }
      p += 4;
      *value = result;
      return true;
    };
    while (p < end) {
      const unsigned char c = static_cast<unsigned char>(*p);
      if (c == '"') {
        ++p;
        return true;
      }
      if (c < 0x20) return false;
      if (c == '\\') {
        ++p;
        if (p >= end) return false;
        const char esc = *p++;
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!read_hex4(&cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
              p += 2;
              uint32_t low = 0;
              if (!read_hex4(&low) || low < 0xDC00 || low > 0xDFFF) return false;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              return false;
            }
            append_utf8(out, cp);
            break;
          }
          default: return false;
        }
      } else {
        out.push_back(static_cast<char>(c));
        ++p;
      }
    }
    return false;
  }

  bool parse_value(JValue& v, int depth) {
    if (depth > 64) return false;
    skip_ws();
    if (p >= end) return false;
    char c = *p;
    if (c == '{') {
      ++p;
      v.type = JType::Object;
      v.obj.clear();
      v.arr.clear();
      skip_ws();
      if (p < end && *p == '}') {
        ++p;
        return true;
      }
      while (p < end) {
        skip_ws();
        std::string key;
        if (!parse_string(key)) return false;
        skip_ws();
        if (p >= end || *p != ':') return false;
        ++p;
        JValue child;
        if (!parse_value(child, depth + 1)) return false;
        v.obj.emplace_back(std::move(key), std::move(child));
        skip_ws();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == '}') {
          ++p;
          return true;
        }
        return false;
      }
      return false;
    }
    if (c == '[') {
      ++p;
      v.type = JType::Array;
      v.arr.clear();
      v.obj.clear();
      skip_ws();
      if (p < end && *p == ']') {
        ++p;
        return true;
      }
      while (p < end) {
        JValue child;
        if (!parse_value(child, depth + 1)) return false;
        v.arr.push_back(std::move(child));
        skip_ws();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == ']') {
          ++p;
          return true;
        }
        return false;
      }
      return false;
    }
    if (c == '"') {
      v.type = JType::String;
      return parse_string(v.str);
    }
    if (literal("null")) {
      v.type = JType::Null;
      return true;
    }
    if (literal("true")) {
      v.type = JType::Bool;
      v.b = true;
      return true;
    }
    if (literal("false")) {
      v.type = JType::Bool;
      v.b = false;
      return true;
    }
    // Strict JSON number grammar: no leading plus, leading zeros, or
    // incomplete fraction/exponent are accepted.
    const char* start = p;
    if (p < end && *p == '-') ++p;
    if (p >= end) return false;
    if (*p == '0') {
      ++p;
      if (p < end && *p >= '0' && *p <= '9') return false;
    } else if (*p >= '1' && *p <= '9') {
      while (p < end && *p >= '0' && *p <= '9') ++p;
    } else {
      return false;
    }
    if (p < end && *p == '.') {
      ++p;
      if (p >= end || *p < '0' || *p > '9') return false;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      ++p;
      if (p < end && (*p == '+' || *p == '-')) ++p;
      if (p >= end || *p < '0' || *p > '9') return false;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    const std::string token(start, p - start);
    char* parsed_end = nullptr;
    const double number = std::strtod(token.c_str(), &parsed_end);
    if (parsed_end != token.c_str() + token.size() || !std::isfinite(number))
      return false;
    v.type = JType::Number;
    v.num = number;
    return true;
  }
};

}  // namespace

bool json_parse(const std::string& src, JValue& out) {
  // Reset first: callers reuse JValue instances for successive responses.
  out = JValue();
  if (!valid_utf8(src)) return false;
  JsonParser jp{src.data(), src.data() + src.size()};
  jp.skip_ws();
  if (!jp.parse_value(out, 0)) {
    out = JValue();
    return false;
  }
  jp.skip_ws();
  if (jp.p != jp.end) {
    out = JValue();
    return false;
  }
  return true;
}

// ------------------------------------------------------------------- lyrics

namespace {

// Parses bare seconds, mm:ss[.fraction], or hh:mm:ss[.fraction].
// Fractions are interpreted as one to three decimal places (100 ms, 10 ms,
// or 1 ms respectively); malformed and overflowing timestamps are rejected.
bool parse_timestamp(const std::string& s, size_t pos, int64_t* out_ms,
                     size_t* consumed) {
  size_t i = pos;
  std::vector<int64_t> fields;
  fields.reserve(3);
  int64_t current = 0;
  bool has_digits = false;
  bool in_fraction = false;
  int64_t fraction = 0;
  size_t fraction_digits = 0;
  const int64_t max_value = std::numeric_limits<int64_t>::max();

  while (i < s.size()) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      const int digit = c - '0';
      if (in_fraction) {
        if (fraction_digits == 3) return false;
        fraction = fraction * 10 + digit;
        ++fraction_digits;
      } else {
        if (current > (max_value - digit) / 10) return false;
        current = current * 10 + digit;
        has_digits = true;
      }
      ++i;
      continue;
    }
    if (c == ':' && !in_fraction) {
      if (!has_digits || fields.size() >= 2) return false;
      fields.push_back(current);
      current = 0;
      has_digits = false;
      ++i;
      continue;
    }
    if (c == '.' && !in_fraction) {
      if (!has_digits) return false;
      in_fraction = true;
      ++i;
      continue;
    }
    break;
  }
  if (!has_digits || (in_fraction && fraction_digits == 0)) return false;
  fields.push_back(current);
  if (fields.empty() || fields.size() > 3) return false;

  const uint64_t max_seconds = static_cast<uint64_t>(max_value / 1000);
  uint64_t seconds = 0;
  auto add_seconds = [&](uint64_t value, uint64_t multiplier) {
    if (value > max_seconds / multiplier) return false;
    const uint64_t part = value * multiplier;
    if (seconds > max_seconds - part) return false;
    seconds += part;
    return true;
  };
  if (fields.size() == 1) {
    if (!add_seconds(static_cast<uint64_t>(fields[0]), 1)) return false;
  } else if (fields.size() == 2) {
    if (fields[1] >= 60 || !add_seconds(static_cast<uint64_t>(fields[0]), 60) ||
        !add_seconds(static_cast<uint64_t>(fields[1]), 1)) return false;
  } else {
    if (fields[1] >= 60 || fields[2] >= 60 ||
        !add_seconds(static_cast<uint64_t>(fields[0]), 3600) ||
        !add_seconds(static_cast<uint64_t>(fields[1]), 60) ||
        !add_seconds(static_cast<uint64_t>(fields[2]), 1)) return false;
  }
  int64_t fraction_ms = fraction;
  if (fraction_digits == 1) fraction_ms *= 100;
  else if (fraction_digits == 2) fraction_ms *= 10;
  if (seconds > static_cast<uint64_t>((max_value - fraction_ms) / 1000))
    return false;
  *out_ms = static_cast<int64_t>(seconds * 1000u) + fraction_ms;
  *consumed = i - pos;
  return true;
}


struct RawLyricPart {
  int64_t t_ms = 0;
  size_t begin = 0;
  size_t end = 0;
  size_t normalized_begin = 0;
  size_t normalized_end = 0;
  bool explicit_time = false;
};

bool parse_inline_lyrics(const std::string& input, int64_t line_start_ms,
                         std::string* plain,
                         std::vector<LyricSegment>* segments) {
  std::string raw_text;
  std::vector<RawLyricPart> parts;
  size_t cursor = 0;
  int64_t part_time = line_start_ms;
  bool explicit_time = false;
  bool saw_tag = false;
  bool saw_timed_text = false;

  for (size_t i = 0; i < input.size();) {
    if (input[i] == '<') {
      const size_t close = input.find('>', i + 1);
      if (close != std::string::npos) {
        const std::string tag = input.substr(i + 1, close - i - 1);
        int64_t word_ms = 0;
        size_t used = 0;
        if (parse_timestamp(tag, 0, &word_ms, &used) && used == tag.size()) {
          if (i > cursor) {
            const size_t begin = raw_text.size();
            raw_text.append(input, cursor, i - cursor);
            parts.push_back({part_time, begin, raw_text.size(), 0, 0, explicit_time});
            if (explicit_time) saw_timed_text = true;
          }
          saw_tag = true;
          explicit_time = true;
          part_time = word_ms;
          cursor = close + 1;
          i = close + 1;
          continue;
        }
      }
    }
    ++i;
  }
  if (cursor < input.size()) {
    const size_t begin = raw_text.size();
    raw_text.append(input, cursor, std::string::npos);
    parts.push_back({part_time, begin, raw_text.size(), 0, 0, explicit_time});
    if (explicit_time) saw_timed_text = true;
  }

  *plain = squish(raw_text);
  segments->clear();
  if (!saw_tag || !saw_timed_text || plain->empty()) return false;

  // Map each raw part boundary through the same whitespace normalization as
  // `squish`, in one pass so even a long imported line stays linear-time.
  size_t begin_index = 0;
  size_t end_index = 0;
  size_t normalized_size = 0;
  bool have_text = false;
  bool pending_space = false;
  for (size_t i = 0; i <= raw_text.size(); ++i) {
    while (begin_index < parts.size() && parts[begin_index].begin == i) {
      parts[begin_index].normalized_begin = normalized_size;
      ++begin_index;
    }
    while (end_index < parts.size() && parts[end_index].end == i) {
      parts[end_index].normalized_end = normalized_size;
      ++end_index;
    }
    if (i == raw_text.size()) break;
    const unsigned char c = static_cast<unsigned char>(raw_text[i]);
    if (c <= ' ') {
      if (have_text) pending_space = true;
    } else {
      if (pending_space) {
        ++normalized_size;
        pending_space = false;
      }
      ++normalized_size;
      have_text = true;
    }
  }
  // Parts are in text order. A decreasing time or a tag inside a UTF-8
  // codepoint makes segment interpolation ambiguous, so retain the lyric text
  // but fall back to line timing.
  auto is_utf8_boundary = [&](size_t index) {
    return index == 0 || index >= plain->size() ||
           (static_cast<unsigned char>((*plain)[index]) & 0xC0) != 0x80;
  };
  int64_t previous_time = -1;
  for (const RawLyricPart& part : parts) {
    if (part.normalized_end <= part.normalized_begin) continue;
    if (part.t_ms < previous_time ||
        !is_utf8_boundary(part.normalized_begin) ||
        !is_utf8_boundary(part.normalized_end)) {
      segments->clear();
      return false;
    }
    previous_time = part.t_ms;
    segments->push_back({part.t_ms, part.normalized_begin, part.normalized_end});
  }
  return !segments->empty();
}

int64_t add_time_saturated(int64_t value, int64_t delta) {
  const int64_t max_value = std::numeric_limits<int64_t>::max();
  const int64_t min_value = std::numeric_limits<int64_t>::min();
  if (delta > 0 && value > max_value - delta) return max_value;
  if (delta < 0 && value < min_value - delta) return min_value;
  return value + delta;
}

}  // namespace

bool lrc_parse(const std::string& lrc, std::vector<LyricLine>& out) {
  out.clear();
  int64_t offset_ms = 0;
  std::vector<LyricLine> lines;

  size_t pos = 0;
  while (pos <= lrc.size()) {
    size_t nl = lrc.find('\n', pos);
    if (nl == std::string::npos) nl = lrc.size();
    std::string raw = lrc.substr(pos, nl - pos);
    pos = nl + 1;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    if (trim(raw).empty()) {
      if (nl >= lrc.size()) break;
      continue;
    }

    // Collect leading timestamps and metadata. Metadata offsets are applied
    // after all lines are parsed, regardless of where the tag appears.
    std::vector<int64_t> stamps;
    size_t i = 0;
    while (i < raw.size() && raw[i] == '[') {
      const size_t close = raw.find(']', i);
      if (close == std::string::npos) break;
      const std::string tag = raw.substr(i + 1, close - i - 1);
      int64_t ms = 0;
      size_t used = 0;
      if (parse_timestamp(tag, 0, &ms, &used) && used == tag.size()) {
        stamps.push_back(ms);
      } else if (starts_with(to_lower_ascii(tag), "offset:")) {
        const std::string value = trim(tag.substr(7));
        if (!value.empty()) {
          errno = 0;
          char* end = nullptr;
          const long long parsed = std::strtoll(value.c_str(), &end, 10);
          if (errno != ERANGE && end != value.c_str() && end && *end == '\0')
            offset_ms = static_cast<int64_t>(parsed);
        }
      }
      i = close + 1;
    }
    if (stamps.empty()) {
      if (nl >= lrc.size()) break;
      continue;  // metadata or stray text
    }

    std::string text;
    std::vector<LyricSegment> segments;
    parse_inline_lyrics(raw.substr(i), stamps.front(), &text, &segments);
    const int64_t base_stamp = stamps.front();
    for (int64_t stamp : stamps) {
      const int64_t shift = stamp - base_stamp;
      LyricLine line;
      line.t_ms = stamp;
      line.text = text;
      line.segments = segments;
      for (LyricSegment& segment : line.segments)
        segment.t_ms = add_time_saturated(segment.t_ms, shift);
      lines.push_back(std::move(line));
    }
    if (nl >= lrc.size()) break;
  }

  if (lines.empty()) return false;

  for (auto& line : lines) {
    line.t_ms = add_time_saturated(line.t_ms, offset_ms);
    if (line.t_ms < 0) line.t_ms = 0;
    for (LyricSegment& segment : line.segments) {
      segment.t_ms = add_time_saturated(segment.t_ms, offset_ms);
      if (segment.t_ms < 0) segment.t_ms = 0;
    }
  }
  std::stable_sort(lines.begin(), lines.end(),
                   [](const LyricLine& a, const LyricLine& b) {
                     return a.t_ms < b.t_ms;
                   });
  out = std::move(lines);
  return true;
}

std::string lyrics_to_plain(const std::vector<LyricLine>& lines) {
  std::string out;
  bool last_blank = true;
  for (const auto& l : lines) {
    if (l.text.empty()) {
      if (!last_blank && !out.empty()) out.push_back('\n');
      last_blank = true;
      continue;
    }
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    out += l.text;
    out.push_back('\n');
    last_blank = false;
  }
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}


std::string lyrics_to_lrc(const std::vector<LyricLine>& lines) {
  auto stamp = [](int64_t value, char open, char close) {
    if (value < 0) value = 0;
    const long long ms = static_cast<long long>(value);
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%c%02lld:%02lld.%03lld%c", open,
                  ms / 60000, (ms / 1000) % 60, ms % 1000, close);
    return std::string(buffer);
  };

  std::string out;
  for (const LyricLine& line : lines) {
    out += stamp(line.t_ms, '[', ']');
    size_t cursor = 0;
    for (const LyricSegment& segment : line.segments) {
      const size_t begin = std::min(segment.begin, line.text.size());
      const size_t end = std::min(segment.end, line.text.size());
      if (end <= begin || begin < cursor) continue;
      out.append(line.text, cursor, begin - cursor);
      out += stamp(segment.t_ms, '<', '>');
      out.append(line.text, begin, end - begin);
      cursor = end;
    }
    out.append(line.text, cursor, std::string::npos);
    out.push_back('\n');
  }
  return out;
}

// ----------------------------------------------------------- track matching

std::string squish_and_lower(const std::string& s) {
  return to_lower_ascii(squish(s));
}

namespace {

bool is_noise_block(const std::string& block) {
  static const char* kNoise[] = {
      "official", "video", "audio", "lyric", "lyrics", "visualizer", "visualiser",
      "remaster", "remastered", "hd", "hq", "4k", "8k", "mv", "m/v", "live",
      "performance", "explicit", "clean", "version", "edit", "radio", "extended",
      "audio only", "with lyrics", "color coded", "colour coded", "sped up",
      "slowed", "reverb", "instrumental", "karaoke", "full song", "music video",
      "official music video", "official audio", "topic", "album", "single",
      "mono", "stereo", "deluxe", "bonus", "feat", "ft", "featuring", "prod",
      "prod.", "prod by", "from the", "soundtrack", "ost", "pt.", "part",
      "mashup", "lyrics video", "official video", "slowed + reverb", "slowed and reverb",
      "bass boosted", "nightcore", "tiktok", "extended mix", "original mix", "radio edit",
      "club mix", "vip", "remix", "soundtrack", "from", "intro", "outro"};
  std::string b = squish_and_lower(block);
  if (b.empty()) return true;
  for (const char* noise : kNoise) {
    if (b == noise) return true;
    if (starts_with(b, noise) &&
        (b.size() == std::strlen(noise) || !std::isalnum(static_cast<unsigned char>(
                                                 b[std::strlen(noise)])))) {
      return true;
    }
    if (std::strstr(b.c_str(), noise) && b.size() <= std::strlen(noise) + 18) {
      return true;
    }
  }
  return false;
}

// Removes "(...)" / "[...]" / "{...}" blocks that only carry noise.
std::string strip_noise_blocks(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    char c = in[i];
    if (c == '(' || c == '[' || c == '{') {
      char closer = (c == '(') ? ')' : (c == '[' ? ']' : '}');
      size_t close = in.find(closer, i + 1);
      if (close != std::string::npos) {
        std::string block = in.substr(i + 1, close - i - 1);
        if (is_noise_block(block)) {
          i = close + 1;
          continue;
        }
      }
    }
    out.push_back(c);
    ++i;
  }
  return squish(out);
}

// Strips trailing "| something" and "- something" segments that are noise.
std::string strip_trailing_noise(const std::string& in) {
  std::string s = in;
  bool changed = true;
  while (changed && !s.empty()) {
    changed = false;
    size_t best = std::string::npos;
    size_t best_len = 0;
    for (const char* sep : {" | ", " - ", " – ", " — ", "/"}) {
      size_t found = s.rfind(sep);
      if (found != std::string::npos && found > 0) {
        std::string tail = s.substr(found + std::strlen(sep));
        if (is_noise_block(tail)) {
          if (best == std::string::npos || found > best) {
            best = found;
            best_len = std::strlen(sep);
          }
        }
      }
    }
    if (best != std::string::npos && best_len > 0) {
      s = trim(s.substr(0, best));
      changed = true;
    }
  }
  // Also strip trailing standalone noise tokens (e.g. "4K", "8K", "HD")
  bool stripped_word = true;
  while (stripped_word && !s.empty()) {
    stripped_word = false;
    size_t last_space = s.rfind(' ');
    if (last_space != std::string::npos && last_space + 1 < s.size()) {
      std::string last_word = s.substr(last_space + 1);
      if (is_noise_block(last_word)) {
        s = trim(s.substr(0, last_space));
        stripped_word = true;
      }
    }
  }
  return squish(s);
}

}  // namespace

TrackInfo clean_track(TrackInfo t, bool infer_artist_from_title) {
  t.title = trim(t.title);
  t.artist = trim(t.artist);
  t.album = trim(t.album);

  // "Artist - Topic" is what YouTube auto-generated channels are called.
  if (ends_with(squish_and_lower(t.artist), "- topic")) {
    t.artist = trim(t.artist.substr(0, t.artist.size() - 7));
    t.artist = trim(t.artist);
    if (!t.artist.empty() && t.artist.back() == '-') t.artist.pop_back();
    if (ends_with(t.artist, "\xE2\x80\x93") || ends_with(t.artist, "\xE2\x80\x94"))
      t.artist = t.artist.substr(0, t.artist.size() - 3);
    t.artist = trim(t.artist);
  }
  if (ends_with(to_lower_ascii(t.artist), "vevo")) {
    t.artist = trim(t.artist.substr(0, t.artist.size() - 4));
  }
  t.artist = strip_trailing_noise(squish(t.artist));

  // Remember the un-cleaned title before the edition markers are stripped, so
  // a live/remix/instrumental session is still identifiably that recording.
  if (t.raw_title.empty()) t.raw_title = t.title;
  t.title = strip_trailing_noise(strip_noise_blocks(t.title));
  // YouTube often formats as "Artist - Title" in the plain page title.
  t.title = trim(t.title);

  // If we still have a leftover "Title - Artist" and no artist, split it.
  if (infer_artist_from_title && t.artist.empty()) {
    size_t dash = t.title.find(" - ");
    if (dash != std::string::npos && dash > 0 && dash + 3 < t.title.size()) {
      std::string lhs = trim(t.title.substr(0, dash));
      std::string rhs = trim(t.title.substr(dash + 3));
      if (!lhs.empty() && !rhs.empty() && rhs.size() < 60) {
        // Heuristic: YouTube Music tab titles read "Title - Artist".
        t.title = lhs;
        t.artist = rhs;
      }
    }
  }
  if (t.title.empty()) t.title = trim(t.artist);
  t.album = squish(t.album);
  t.raw_title = squish(t.raw_title);
  return t;
}

TrackVariant TrackInfo::variant() const {
  return detect_variant(raw_title.empty() ? title : raw_title);
}

const char* track_variant_name(TrackVariant variant) {
  switch (variant) {
    case TrackVariant::Live: return "live";
    case TrackVariant::Remix: return "remix";
    case TrackVariant::Instrumental: return "instrumental";
    case TrackVariant::Karaoke: return "karaoke";
    case TrackVariant::Studio:
    default: return "studio";
  }
}

namespace {

// Loudness/length hints that identify an edition without naming it.
bool has_any(const std::string& haystack, std::initializer_list<const char*> needles) {
  for (const char* needle : needles) {
    if (haystack.find(needle) != std::string::npos) return true;
  }
  return false;
}

// Matches a whole word inside `text`, so "live" does not fire on "delivery".
bool has_word(const std::string& text, const char* word) {
  const size_t n = std::strlen(word);
  if (n == 0) return false;
  for (size_t pos = text.find(word); pos != std::string::npos;
       pos = text.find(word, pos + 1)) {
    const bool left_ok = pos == 0 ||
                         !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
    const bool right_ok = pos + n >= text.size() ||
                          !std::isalnum(static_cast<unsigned char>(text[pos + n]));
    if (left_ok && right_ok) return true;
  }
  return false;
}

}  // namespace

TrackVariant detect_variant(const std::string& raw_title) {
  const std::string t = squish_and_lower(raw_title);
  if (t.empty()) return TrackVariant::Studio;
  // Karaoke is detected first: a karaoke backing track is often also labelled
  // "instrumental version", and it must not be shown as the sung song.
  if (has_word(t, "karaoke") || has_any(t, {"backing track", "sing along version"}))
    return TrackVariant::Karaoke;
  if (has_word(t, "instrumental") || has_any(t, {"no vocals", "off vocal",
                                                "minus one", "\xe3\x82\xab\xe3\x83\xa9\xe3\x82\xaa\xe3\x82\xb1"}))
    return TrackVariant::Instrumental;
  if (has_any(t, {"remix", "remixed", "bootleg", "flip", "rework", "mashup",
                  "medley", "nightcore", "sped up", "slowed", "reverb",
                  "bass boosted", "club mix", "extended mix", "vip mix"}))
    return TrackVariant::Remix;
  if (has_word(t, "live") || has_any(t, {"in concert", "unplugged", "session",
                                         "en vivo", "ao vivo", "acoustic version"}))
    return TrackVariant::Live;
  return TrackVariant::Studio;
}

bool variant_compatible(TrackVariant want, const std::string& candidate_title,
                        const std::string& candidate_album) {
  if (want == TrackVariant::Studio) return true;
  // The provider's title is the primary marker; its album ("... (Live)") is a
  // secondary one that LRCLIB often carries when the title is plain.
  const TrackVariant title_variant = detect_variant(candidate_title);
  if (title_variant == want) return true;
  if (candidate_album.empty()) return false;
  return detect_variant(candidate_album) == want;
}

std::string track_key(const TrackInfo& t) {
  std::string k = squish_and_lower(t.title) + "|" + squish_and_lower(t.artist);
  const TrackVariant variant = t.variant();
  if (variant != TrackVariant::Studio) {
    k += "|";
    k += track_variant_name(variant);
  }
  // Duration is rounded to whole seconds, so two players disagreeing by a few
  // hundred milliseconds still share one cache entry and one lyric sheet.
  if (t.duration_s > 0) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "|%d", static_cast<int>(std::lround(t.duration_s)));
    k += buf;
  }
  return k;
}

std::string track_label(const TrackInfo& t) {
  if (t.artist.empty()) return t.title;
  if (t.title.empty()) return t.artist;
  return t.artist + " \xE2\x80\x94 " + t.title;  // em dash
}

bool same_track(const TrackInfo& a, const TrackInfo& b) {
  if (a.title.empty() || b.title.empty()) return false;
  if (text_similarity(a.title, b.title) < 0.80) return false;
  // An artist that is present on both sides must agree. A missing artist means
  // the player withdrew its metadata mid-session, which should not count as a
  // new song.
  if (!a.artist.empty() && !b.artist.empty() &&
      text_similarity(a.artist, b.artist) < 0.72) {
    return false;
  }
  // Contradictory durations mean a different song that happens to share a
  // title (very common with "Intro", "Interlude", "Home").
  if (a.duration_s > 5 && b.duration_s > 5 &&
      std::fabs(a.duration_s - b.duration_s) > 8.0) {
    return false;
  }
  // A live take, remix, instrumental or karaoke marker means a different
  // recording. Two studio observations are always compatible.
  const TrackVariant va = a.variant();
  const TrackVariant vb = b.variant();
  if (va != vb) return false;
  return true;
}

bool is_watchable_app(const std::string& app_id, const TrackInfo& t) {
  const std::string id = to_lower_ascii(app_id);
  static const char* kBrowsers[] = {"chrome",  "msedge", "brave",  "firefox",
                                    "opera",   "vivaldi", "chromium", "arc"};
  static const char* kPlayers[] = {"spotify", "vlc", "wmplayer", "foobar",
                                   "music.ui", "itunes", "tidal", "deezer",
                                   "amazon", "plex", "musicbee", "aimp", "mpc",
                                   "applemusic", "youtube", "podcast"};
  // Windows shell / system surfaces that report a session but are not music.
  static const char* kUI[] = {"shellexperiencehost", "searchhost",
                              "startmenuexperiencehost", "systemsettings"};

  for (const char* ui : kUI) {
    if (id.find(ui) != std::string::npos) return false;
  }
  if (t.title.empty()) return false;
  // Empty titles that are really just a container name, or obvious non-music.
  const std::string title = to_lower_ascii(t.title);
  if (title == "unknown" || title == "no title" || title == "advertisement" ||
      title == "audio") {
    return false;
  }
  if (t.duration_s > 0 && t.duration_s < 25) return false;  // stingers, ads, UI

  const bool browser = [&] {
    for (const char* b : kBrowsers) {
      if (id.find(b) != std::string::npos) return true;
    }
    return false;
  }();
  const bool player = [&] {
    for (const char* b : kPlayers) {
      if (id.find(b) != std::string::npos) return true;
    }
    return false;
  }();
  // An unknown app is allowed only if it reports a real song length; that is
  // the signal that separates a music session from a game or a video.
  return browser || player || t.duration_s >= 45;
}

double token_containment(const std::string& a, const std::string& b) {
  std::string x = squish_and_lower(a);
  std::string y = squish_and_lower(b);
  if (x.empty() || y.empty()) return 0.0;
  if (x == y) return 1.0;
  auto tokens = [](const std::string& s) {
    std::set<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
      bool alnum = std::isalnum(c) != 0 || c >= 0x80;
      if (alnum) {
        cur.push_back(static_cast<char>(c));
      } else if (!cur.empty()) {
        out.insert(cur);
        cur.clear();
      }
    }
    if (!cur.empty()) out.insert(cur);
    return out;
  };
  std::set<std::string> ta = tokens(x), tb = tokens(y);
  if (ta.empty() || tb.empty()) return 0.0;
  size_t inter = 0;
  for (const auto& s : ta) {
    if (tb.count(s)) ++inter;
  }
  return static_cast<double>(inter) /
         static_cast<double>(std::min(ta.size(), tb.size()));
}

double text_similarity(const std::string& a, const std::string& b) {
  std::string x = squish_and_lower(a);
  std::string y = squish_and_lower(b);
  if (x.empty() || y.empty()) return 0.0;
  if (x == y) return 1.0;
  // Token overlap (Jaccard-ish).
  auto tokens = [](const std::string& s) {
    std::set<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
      bool alnum = std::isalnum(c) != 0 || c >= 0x80;
      if (alnum) {
        cur.push_back(static_cast<char>(c));
      } else if (!cur.empty()) {
        out.insert(cur);
        cur.clear();
      }
    }
    if (!cur.empty()) out.insert(cur);
    return out;
  };
  std::set<std::string> ta = tokens(x), tb = tokens(y);
  if (ta.empty() || tb.empty()) return 0.0;
  size_t inter = 0;
  for (const auto& s : ta) {
    if (tb.count(s)) ++inter;
  }
  double jaccard = static_cast<double>(inter) /
                   static_cast<double>(ta.size() + tb.size() - inter);
  double containment = 0.0;
  if (x.find(y) != std::string::npos || y.find(x) != std::string::npos) {
    containment = 0.85;
  }
  return std::max(jaccard, containment * std::min(1.0, 1.0));
}

const JValue* best_candidate(const JValue& search_results, const TrackInfo& want,
                             double* score_out) {
  const JValue* best = nullptr;
  double best_score = -1.0;
  std::vector<const JValue*> candidates;
  if (search_results.is_array()) {
    candidates.reserve(search_results.arr.size());
    for (const auto& value : search_results.arr) candidates.push_back(&value);
  } else if (search_results.is_object()) {
    candidates.push_back(&search_results);
  } else {
    return nullptr;
  }

  for (const JValue* candidate : candidates) {
    if (!candidate || !candidate->is_object()) continue;
    const JValue& rec = *candidate;
    bool has_synced = false, has_plain = false;
    if (const JValue* s = rec.find("syncedLyrics")) {
      has_synced = s->is_string() && !trim(s->str).empty();
    }
    if (const JValue* s = rec.find("plainLyrics")) {
      has_plain = s->is_string() && !trim(s->str).empty();
    }
    if (!has_synced && !has_plain) continue;

    // Clean provider titles with the same conservative rules as media titles,
    // but never infer an artist from a title string. Title similarity is a hard
    // gate: a query for one track must not be won by another track from the
    // same artist merely because its duration happens to match.
    const double duration = rec.get_num("duration", 0);
    const std::string candidate_raw_title = rec.get_str("trackName");
    const std::string candidate_album = rec.get_str("albumName");
    TrackInfo candidate_track = clean_track(
        TrackInfo{candidate_raw_title, rec.get_str("artistName"), candidate_album,
                  duration, std::string()},
        false);
    const double title_sim = text_similarity(want.title, candidate_track.title);
    const double title_cont = token_containment(want.title, candidate_track.title);
    if (title_sim < 0.70 && title_cont < 0.80) continue;

    const double artist_sim = text_similarity(want.artist, candidate_track.artist);
    const double artist_cont = token_containment(want.artist, candidate_track.artist);
    const bool same_artist = artist_sim >= 0.65 || artist_cont >= 0.75;
    if (!want.artist.empty() && !candidate_track.artist.empty() && !same_artist) {
      continue;
    }
    // Edition gate. A live take, remix, instrumental or karaoke backing track
    // is a different recording with its own timings; showing the studio sheet
    // for it is a wrong answer even when the title and artist agree. When a
    // session is the plain studio song we stay permissive, because providers
    // routinely omit edition markers.
    const TrackVariant want_variant = want.variant();
    if (want_variant != TrackVariant::Studio &&
        !variant_compatible(want_variant, candidate_raw_title, candidate_album)) {
      continue;
    }
    // With no artist metadata, a title alone requires duration match to avoid same-title covers
    if (want.artist.empty()) {
      if (want.duration_s <= 5 || duration <= 5 ||
          std::fabs(duration - want.duration_s) > 8.0) {
        continue;
      }
    }

    double score = title_sim * 5.0 + artist_sim * 4.0;
    if (title_cont >= 0.90) score += 1.0;
    if (artist_cont >= 0.90) score += 0.8;
    if (has_synced) score += 2.5;

    if (want.duration_s > 0 && duration > 0) {
      const double delta = std::fabs(duration - want.duration_s);
      if (delta <= 2.0) score += 3.5;
      else if (delta <= 5.0) score += 2.2;
      else if (delta <= 8.0) score += 0.6;
      else if (delta > 30.0) score -= 4.0;
      else score -= 1.5;
    }
    if (score > best_score) {
      best_score = score;
      best = &rec;
    }
  }
  if (score_out) *score_out = best_score;
  // Better to show nothing than the wrong words.
  if (best_score < 6.5) return nullptr;
  return best;
}

bool parse_lyrics_ovh_payload(const std::string& body, Lyrics& out) {
  JValue root;
  if (!json_parse(body, root) || !root.is_object()) return false;
  std::string text = root.get_str("lyrics");
  if (trim(text).empty()) return false;
  // The service answers "No lyrics found" with HTTP 200 for misses.
  if (to_lower_ascii(trim(text)).find("no lyrics found") == 0) return false;
  out = Lyrics();
  out.synced = false;
  out.plain = text;
  out.source = "lyrics.ovh";
  return true;
}

std::vector<LyricLine> pace_plain_lyrics(const std::string& plain, int64_t lead_ms,
                                         int64_t per_line_ms) {
  std::vector<LyricLine> out;
  if (per_line_ms < 500) per_line_ms = 500;
  int64_t t = lead_ms < 0 ? 0 : lead_ms;
  size_t pos = 0;
  while (pos <= plain.size()) {
    size_t nl = plain.find('\n', pos);
    if (nl == std::string::npos) nl = plain.size();
    std::string line = plain.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    line = trim(line);
    // Blank lines become an empty spacer row rather than a timed lyric.
    out.push_back({t, line});
    t += per_line_ms;
    if (nl >= plain.size()) break;
    pos = nl + 1;
  }
  return out;
}

namespace {

std::string normalize_import_text(const std::string& input) {
  if (input.size() >= 3 &&
      static_cast<unsigned char>(input[0]) == 0xEF &&
      static_cast<unsigned char>(input[1]) == 0xBB &&
      static_cast<unsigned char>(input[2]) == 0xBF) {
    const std::string utf8 = input.substr(3);
    return valid_utf8(utf8) ? utf8 : wide_to_utf8(utf8_to_wide(utf8));
  }
  if (input.size() >= 2 &&
      ((static_cast<unsigned char>(input[0]) == 0xFF &&
        static_cast<unsigned char>(input[1]) == 0xFE) ||
       (static_cast<unsigned char>(input[0]) == 0xFE &&
        static_cast<unsigned char>(input[1]) == 0xFF))) {
    const bool little_endian = static_cast<unsigned char>(input[0]) == 0xFF;
    std::wstring wide_text;
    wide_text.reserve((input.size() - 2) / 2 + 1);
    size_t i = 2;
    for (; i + 1 < input.size(); i += 2) {
      const uint16_t first = static_cast<unsigned char>(input[i]);
      const uint16_t second = static_cast<unsigned char>(input[i + 1]);
      const uint16_t unit = little_endian
                                ? static_cast<uint16_t>(first | (second << 8))
                                : static_cast<uint16_t>((first << 8) | second);
      wide_text.push_back(static_cast<wchar_t>(unit));
    }
    if (i < input.size()) wide_text.push_back(static_cast<wchar_t>(0xFFFD));
    return wide_to_utf8(wide_text);
  }
  return valid_utf8(input) ? input : wide_to_utf8(utf8_to_wide(input));
}

}  // namespace

bool parse_local_lyrics(const std::string& text, Lyrics& out) {
  const std::string normalized = normalize_import_text(text);
  if (trim(normalized).empty()) return false;
  out = Lyrics();
  std::vector<LyricLine> lines;
  if (lrc_parse(normalized, lines) && !lines.empty()) {
    out.synced = true;
    out.lines = std::move(lines);
    out.source = "local file";
    return true;
  }
  out.synced = false;
  out.plain = normalized;
  out.source = "local file";
  return true;
}

bool parse_lrclib_payload(const std::string& body, const TrackInfo& want,
                          Lyrics& out) {
  JValue root;
  if (!json_parse(body, root)) return false;

  double score = 0;
  const JValue* rec = best_candidate(root, want, &score);
  if (!rec) return false;

  out = Lyrics();
  out.source = "LRCLIB";
  out.title = rec->get_str("trackName", want.title.c_str());
  out.artist = rec->get_str("artistName", want.artist.c_str());
  out.album = rec->get_str("albumName");
  const TrackVariant matched = detect_variant(out.title);
  if (matched != TrackVariant::Studio) out.variant = track_variant_name(matched);

  std::string synced = rec->get_str("syncedLyrics");
  if (!trim(synced).empty()) {
    std::vector<LyricLine> lines;
    if (lrc_parse(synced, lines)) {
      out.synced = true;
      out.lines = std::move(lines);
      return true;
    }
  }
  std::string plain = rec->get_str("plainLyrics");
  if (!trim(plain).empty()) {
    out.synced = false;
    out.plain = plain;
    return true;
  }
  return false;
}

// ------------------------------------------------------------------- layout

std::vector<DispLine> build_display_lines(const std::vector<LyricLine>& lines,
                                          double max_w, double px,
                                          const MeasureFn& measure) {
  struct Chunk {
    size_t begin = 0;
    size_t end = 0;
    std::string text;
  };

  std::vector<DispLine> out;
  if (lines.empty()) return out;

  auto next_codepoint = [](const std::string& text, size_t i) {
    if (i >= text.size()) return text.size();
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t length = 1;
    if (c >= 0xC2 && c <= 0xDF) length = 2;
    else if (c >= 0xE0 && c <= 0xEF) length = 3;
    else if (c >= 0xF0 && c <= 0xF4) length = 4;
    if (i + length > text.size()) return i + 1;
    for (size_t k = 1; k < length; ++k) {
      if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return i + 1;
    }
    return i + length;
  };

  auto wrap = [&](const std::string& text, std::vector<Chunk>& chunks) {
    chunks.clear();
    if (text.empty()) {
      chunks.push_back({0, 0, ""});
      return;
    }
    if (max_w <= 1 || !measure) {
      chunks.push_back({0, text.size(), text});
      return;
    }

    Chunk current;
    bool have_current = false;
    auto flush_current = [&]() {
      if (have_current) chunks.push_back(std::move(current));
      current = Chunk();
      have_current = false;
    };
    auto add_long_token = [&](size_t begin, size_t end) {
      size_t part_begin = begin;
      size_t cursor = begin;
      while (cursor < end) {
        const size_t next = next_codepoint(text, cursor);
        const std::string candidate = text.substr(part_begin, next - part_begin);
        if (measure(candidate, px) > max_w && cursor > part_begin) {
          chunks.push_back({part_begin, cursor, text.substr(part_begin, cursor - part_begin)});
          part_begin = cursor;
        }
        cursor = next;
      }
      if (part_begin < end)
        chunks.push_back({part_begin, end, text.substr(part_begin, end - part_begin)});
    };

    for (size_t pos = 0; pos < text.size();) {
      while (pos < text.size() && text[pos] == ' ') ++pos;
      if (pos >= text.size()) break;
      const size_t word_begin = pos;
      while (pos < text.size() && text[pos] != ' ') ++pos;
      const size_t word_end = pos;
      const std::string word = text.substr(word_begin, word_end - word_begin);

      if (measure(word, px) > max_w) {
        flush_current();
        add_long_token(word_begin, word_end);
        continue;
      }
      if (!have_current) {
        current = {word_begin, word_end, word};
        have_current = true;
        continue;
      }
      const std::string candidate = text.substr(current.begin, word_end - current.begin);
      if (measure(candidate, px) <= max_w) {
        current.end = word_end;
        current.text = candidate;
      } else {
        flush_current();
        current = {word_begin, word_end, word};
        have_current = true;
      }
    }
    flush_current();
    if (chunks.empty()) chunks.push_back({0, text.size(), text});
  };

  std::vector<Chunk> chunks;
  for (size_t i = 0; i < lines.size(); ++i) {
    wrap(lines[i].text, chunks);
    const int64_t end = (i + 1 < lines.size())
                            ? lines[i + 1].t_ms
                            : add_time_saturated(lines[i].t_ms, 6000);
    for (size_t k = 0; k < chunks.size(); ++k) {
      const Chunk& chunk = chunks[k];
      DispLine display;
      display.start_ms = lines[i].t_ms;
      display.end_ms = end;
      display.text = chunk.text;
      display.cont = (k > 0);
      for (const LyricSegment& segment : lines[i].segments) {
        const size_t begin = std::max(segment.begin, chunk.begin);
        const size_t finish = std::min(segment.end, chunk.end);
        if (finish > begin) {
          display.segments.push_back(
              {segment.t_ms, begin - chunk.begin, finish - chunk.begin});
        }
      }
      out.push_back(std::move(display));
    }
  }
  return out;
}

int active_index(const std::vector<DispLine>& lines, int64_t pos_ms) {
  if (lines.empty()) return -1;
  if (pos_ms < lines.front().start_ms) return -1;
  // Binary search: last line whose start <= pos.
  size_t lo = 0, hi = lines.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (lines[mid].start_ms <= pos_ms) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return -1;
  size_t group_end = lo - 1;
  size_t group_begin = group_end;
  while (group_begin > 0 && lines[group_begin].cont &&
         lines[group_begin - 1].start_ms == lines[group_begin].start_ms) {
    --group_begin;
  }

  // Ordinary timed lines keep the first wrapped row active. With genuine
  // Enhanced LRC segments, advance the active row to the chunk containing the
  // latest segment marker reached so long, wrapped lines remain navigable.
  size_t selected = group_begin;
  int64_t latest_segment_ms = -1;
  for (size_t row = group_begin; row <= group_end; ++row) {
    for (const LyricSegment& segment : lines[row].segments) {
      if (segment.t_ms <= pos_ms && segment.t_ms > latest_segment_ms) {
        latest_segment_ms = segment.t_ms;
        selected = row;
      }
    }
  }
  return static_cast<int>(selected);
}

double line_progress(const std::vector<DispLine>& lines, int index, int64_t pos_ms) {
  if (index < 0 || index >= static_cast<int>(lines.size())) return 0.0;
  const DispLine& l = lines[index];
  int64_t span = l.end_ms - l.start_ms;
  if (span <= 0) return 1.0;
  // Instrumental gaps shouldn't stretch the wipe across the whole silence.
  const int64_t kMaxSpan = 9000;
  if (span > kMaxSpan) span = kMaxSpan;
  double p = static_cast<double>(pos_ms - l.start_ms) / static_cast<double>(span);
  if (p < 0) p = 0;
  if (p > 1) p = 1;
  return p;
}

LineMetrics compute_line_metrics(const std::vector<DispLine>& lines, double box_h,
                                 int64_t gap_ms, double gap_extra_px) {
  LineMetrics m;
  m.top.resize(lines.size());
  m.height.resize(lines.size());
  double y = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0 && !lines[i].text.empty() &&
        lines[i].start_ms - lines[i - 1].start_ms >= gap_ms) {
      y += gap_extra_px;
    }
    m.top[i] = y;
    m.height[i] = box_h;
    y += box_h;
  }
  m.total = y;
  return m;
}

double compute_scroll_target(const LineMetrics& metrics, int active, int viewport_h,
                             double anchor) {
  if (metrics.total <= 0 || viewport_h <= 0) return 0;
  if (active < 0 || active >= static_cast<int>(metrics.top.size())) {
    return std::max(0.0, (viewport_h - metrics.total) * 0.5);
  }
  double want = viewport_h * anchor -
                (metrics.top[active] + metrics.height[active] * 0.5);
  double min_scroll = viewport_h * 0.72 - metrics.total;
  double max_scroll = viewport_h * (anchor - 0.02);
  if (min_scroll > max_scroll) min_scroll = max_scroll;
  return std::min(max_scroll, std::max(min_scroll, want));
}

double ease_towards(double current, double target, double dt_s, double response) {
  if (response <= 0) return target;
  double alpha = 1.0 - std::exp(-dt_s / response);
  if (alpha < 0) alpha = 0;
  if (alpha > 1) alpha = 1;
  return current + (target - current) * alpha;
}

int64_t interpolate_pos(int64_t anchor_ms, int64_t anchor_at_ms, int64_t now_ms,
                        bool playing, int64_t duration_ms) {
  int64_t pos = anchor_ms;
  if (playing && now_ms > anchor_at_ms) {
    // Subtract in unsigned space so extreme signed anchors cannot overflow.
    const uint64_t elapsed = static_cast<uint64_t>(now_ms) -
                             static_cast<uint64_t>(anchor_at_ms);
    const int64_t max_value = std::numeric_limits<int64_t>::max();
    if (elapsed > static_cast<uint64_t>(max_value) ||
        pos > max_value - static_cast<int64_t>(elapsed)) {
      pos = max_value;
    } else {
      pos += static_cast<int64_t>(elapsed);
    }
  }
  if (pos < 0) pos = 0;
  if (duration_ms > 0 && pos > duration_ms) pos = duration_ms;
  return pos;
}

std::string format_time(int64_t ms) {
  if (ms < 0) ms = 0;
  int64_t total_s = ms / 1000;
  int64_t m = total_s / 60;
  int64_t s = total_s % 60;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(m),
                static_cast<long long>(s));
  return buf;
}

}  // namespace ytl
