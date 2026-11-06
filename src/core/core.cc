// Youtube-Lyrics - portable core logic (see core.h).
#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <set>

namespace ytl {

// ---------------------------------------------------------------- text utils

std::wstring utf8_to_wide(const std::string& s) {
  std::wstring out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    size_t extra = 0;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    } else {
      // Broken byte: emit U+FFFD and move on.
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    if (i + extra >= s.size()) {
      out.push_back(0xFFFD);
      break;
    }
    bool ok = true;
    for (size_t k = 1; k <= extra; ++k) {
      unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    i += extra + 1;
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
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
      uint32_t lo = static_cast<uint32_t>(s[i + 1]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
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
  if (v && (v->is_number() || v->is_string())) {
    if (v->is_number()) return v->num;
    return std::atof(v->str.c_str());
  }
  return def;
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
    while (p < end) {
      unsigned char c = static_cast<unsigned char>(*p);
      if (c == '"') {
        ++p;
        return true;
      }
      if (c == '\\') {
        ++p;
        if (p >= end) return false;
        char esc = *p++;
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
            if (end - p < 4) return false;
            uint32_t cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = p[k];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(h - 'A' + 10);
              else return false;
            }
            p += 4;
            // Surrogate pair?
            if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' &&
                p[1] == 'u') {
              uint32_t lo = 0;
              bool ok = true;
              for (int k = 0; k < 4; ++k) {
                char h = p[2 + k];
                lo <<= 4;
                if (h >= '0' && h <= '9') lo |= static_cast<uint32_t>(h - '0');
                else if (h >= 'a' && h <= 'f') lo |= static_cast<uint32_t>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') lo |= static_cast<uint32_t>(h - 'A' + 10);
                else { ok = false; break; }
              }
              if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
              }
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
    // Number
    const char* start = p;
    if (p < end && (*p == '-' || *p == '+')) ++p;
    bool any = false;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' ||
                       *p == 'E' || *p == '-' || *p == '+')) {
      any = true;
      ++p;
    }
    if (!any) return false;
    v.type = JType::Number;
    v.num = std::atof(std::string(start, p - start).c_str());
    return true;
  }
};

}  // namespace

bool json_parse(const std::string& src, JValue& out) {
  // Reset first: callers reuse JValue instances for successive responses.
  out = JValue();
  JsonParser jp{src.data(), src.data() + src.size()};
  jp.skip_ws();
  if (!jp.parse_value(out, 0)) return false;
  return true;
}

// ------------------------------------------------------------------- lyrics

namespace {

// Parses "mm:ss", "mm:ss.xx", "mm:ss.xxx", "hh:mm:ss.xx" and bare seconds.
// Timestamps are ambiguous by nature, so this splits on ':' first and treats a
// trailing ".xxx" (or ":xxx" in the 2-field legacy form) as a fraction.
bool parse_timestamp(const std::string& s, size_t pos, int64_t* out_ms,
                     size_t* consumed) {
  size_t i = pos;
  std::vector<long> fields;
  long cur = 0;
  bool cur_has_digits = false;
  long frac = -1;
  size_t frac_digits = 0;

  while (i < s.size()) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      if (frac >= 0) {  // digits after the fraction separator
        if (frac_digits < 3) {
          frac = frac * 10 + (c - '0');
          ++frac_digits;
        }
        ++i;
        continue;
      }
      cur = cur * 10 + (c - '0');
      cur_has_digits = true;
      ++i;
      continue;
    }
    if (c == ':' && frac < 0) {
      if (!cur_has_digits) return false;
      fields.push_back(cur);
      cur = 0;
      cur_has_digits = false;
      ++i;
      continue;
    }
    if (c == '.' && frac < 0) {
      if (!cur_has_digits) return false;
      frac = 0;
      frac_digits = 0;
      ++i;
      continue;
    }
    break;
  }
  if (cur_has_digits) fields.push_back(cur);
  if (fields.empty()) return false;

  double seconds = 0.0;
  if (fields.size() == 1) {
    seconds = static_cast<double>(fields[0]);
  } else if (fields.size() == 2) {
    seconds = static_cast<double>(fields[0]) * 60.0 + static_cast<double>(fields[1]);
  } else {
    seconds = static_cast<double>(fields[0]) * 3600.0 +
              static_cast<double>(fields[1]) * 60.0 +
              static_cast<double>(fields[2]);
  }
  double frac_ms = 0.0;
  if (frac >= 0) {
    if (frac_digits == 1) frac_ms = static_cast<double>(frac) * 100.0;
    else if (frac_digits == 2) frac_ms = static_cast<double>(frac) * 10.0;
    else frac_ms = static_cast<double>(frac);
  }
  *out_ms = static_cast<int64_t>(llround(seconds * 1000.0 + frac_ms));
  *consumed = i - pos;
  return true;
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

    // Collect leading [..] tags.
    std::vector<int64_t> stamps;
    size_t i = 0;
    while (i < raw.size() && raw[i] == '[') {
      size_t close = raw.find(']', i);
      if (close == std::string::npos) break;
      std::string tag = raw.substr(i + 1, close - i - 1);
      int64_t ms = 0;
      size_t used = 0;
      if (parse_timestamp(tag, 0, &ms, &used) && used == tag.size()) {
        stamps.push_back(ms);
      } else if (starts_with(to_lower_ascii(tag), "offset:")) {
        offset_ms = std::atoll(tag.c_str() + 7);
      }
      i = close + 1;
    }
    if (stamps.empty()) continue;  // metadata or stray text

    std::string text = raw.substr(i);
    // Drop inline word timings like <00:12.34>.
    std::string cleaned;
    cleaned.reserve(text.size());
    for (size_t k = 0; k < text.size();) {
      if (text[k] == '<') {
        size_t close = text.find('>', k);
        if (close != std::string::npos && text.find(':', k) < close) {
          k = close + 1;
          continue;
        }
      }
      cleaned.push_back(text[k]);
      ++k;
    }
    text = squish(cleaned);

    for (int64_t ms : stamps) lines.push_back({ms, text});
    if (nl >= lrc.size()) break;
  }

  if (lines.empty()) return false;

  for (auto& l : lines) {
    l.t_ms += offset_ms;
    if (l.t_ms < 0) l.t_ms = 0;
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
      "prod.", "prod by", "from the", "soundtrack", "ost", "pt.", "part"};
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
  return squish(s);
}

}  // namespace

TrackInfo clean_track(TrackInfo t) {
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

  t.title = strip_trailing_noise(strip_noise_blocks(t.title));
  // YouTube often formats as "Artist - Title" in the plain page title.
  t.title = trim(t.title);

  // If we still have a leftover "Title - Artist" and no artist, split it.
  if (t.artist.empty()) {
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
  return t;
}

std::string track_key(const TrackInfo& t) {
  std::string k = squish_and_lower(t.title) + "|" + squish_and_lower(t.artist);
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
  if (!search_results.is_array()) {
    // A single object works too.
    if (search_results.is_object()) {
      const JValue* synced = search_results.find("syncedLyrics");
      if (synced && synced->is_string() && !trim(synced->str).empty()) {
        if (score_out) *score_out = 10.0;
        return &search_results;
      }
    }
    return nullptr;
  }
  for (const auto& rec : search_results.arr) {
    if (!rec.is_object()) continue;
    bool has_synced = false, has_plain = false;
    if (const JValue* s = rec.find("syncedLyrics")) {
      has_synced = s->is_string() && !trim(s->str).empty();
    }
    if (const JValue* s = rec.find("plainLyrics")) {
      has_plain = s->is_string() && !trim(s->str).empty();
    }
    if (!has_synced && !has_plain) continue;

    double title_sim = text_similarity(want.title, rec.get_str("trackName"));
    double artist_sim = text_similarity(want.artist, rec.get_str("artistName"));
    if (want.artist.empty()) artist_sim = 0.6;
    double score = title_sim * 4.0 + artist_sim * 3.0;
    if (has_synced) score += 2.5;
    double dur = rec.get_num("duration", 0);
    if (want.duration_s > 0 && dur > 0) {
      double delta = std::fabs(dur - want.duration_s);
      if (delta <= 2.0) score += 2.0;
      else if (delta <= 6.0) score += 0.8;
      else if (delta > 25.0) score -= 2.0;
    }
    if (score > best_score) {
      best_score = score;
      best = &rec;
    }
  }
  if (score_out) *score_out = best_score;
  if (best_score < 4.0) return nullptr;
  return best;
}

bool parse_lrclib_payload(const std::string& body, const TrackInfo& want,
                          Lyrics& out) {
  JValue root;
  if (!json_parse(body, root)) return false;

  const JValue* rec = nullptr;
  if (root.is_array()) {
    double score = 0;
    rec = best_candidate(root, want, &score);
    if (!rec) return false;
  } else if (root.is_object()) {
    if (root.find("syncedLyrics") == nullptr && root.find("plainLyrics") == nullptr) {
      return false;
    }
    rec = &root;
  }
  if (!rec) return false;

  out = Lyrics();
  out.source = "LRCLIB";
  out.title = rec->get_str("trackName", want.title.c_str());
  out.artist = rec->get_str("artistName", want.artist.c_str());
  out.album = rec->get_str("albumName");

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
  std::vector<DispLine> out;
  if (lines.empty()) return out;

  // Wrap one lyric line into chunks that fit max_w.
  auto wrap = [&](const std::string& text, std::vector<std::string>& chunks) {
    chunks.clear();
    if (text.empty()) {
      chunks.push_back("");
      return;
    }
    if (max_w <= 1 || !measure) {
      chunks.push_back(text);
      return;
    }
    std::string cur;
    std::string word;
    auto flush_word = [&]() {
      if (word.empty()) return;
      std::string candidate = cur.empty() ? word : cur + " " + word;
      if (measure(candidate, px) <= max_w || cur.empty()) {
        cur = candidate;
      } else {
        chunks.push_back(cur);
        cur = word;
      }
      word.clear();
    };
    for (char c : text) {
      if (c == ' ') {
        flush_word();
      } else {
        word.push_back(c);
      }
    }
    flush_word();
    if (!cur.empty()) chunks.push_back(cur);
    if (chunks.empty()) chunks.push_back(text);
  };

  std::vector<std::string> chunks;
  for (size_t i = 0; i < lines.size(); ++i) {
    wrap(lines[i].text, chunks);
    int64_t end = (i + 1 < lines.size()) ? lines[i + 1].t_ms : lines[i].t_ms + 6000;
    for (size_t k = 0; k < chunks.size(); ++k) {
      DispLine dl;
      dl.start_ms = lines[i].t_ms;
      dl.end_ms = end;
      dl.text = chunks[k];
      dl.cont = (k > 0);
      out.push_back(std::move(dl));
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
  size_t idx = lo - 1;
  // Wrapped continuations share the start time: report the first chunk.
  while (idx > 0 && lines[idx].cont && lines[idx - 1].start_ms == lines[idx].start_ms) {
    --idx;
  }
  return static_cast<int>(idx);
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
    pos += (now_ms - anchor_at_ms);
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
