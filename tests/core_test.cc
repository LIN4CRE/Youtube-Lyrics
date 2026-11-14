// Unit tests for the portable core. Build & run on any platform:
//   g++ -std=c++17 -O1 -Wall src/core/core.cc tests/core_test.cc -o /tmp/core_test && /tmp/core_test
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../src/core/core.h"

using namespace ytl;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    ++g_checks;                                                              \
    auto _a = (a);                                                           \
    auto _b = (b);                                                           \
    if (!(_a == _b)) {                                                       \
      ++g_failures;                                                          \
      std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
    }                                                                        \
  } while (0)

#define CHECK_STR(a, b)                                                      \
  do {                                                                       \
    ++g_checks;                                                              \
    std::string _a = (a);                                                    \
    std::string _b = (b);                                                    \
    if (_a != _b) {                                                          \
      ++g_failures;                                                          \
      std::printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,      \
                  _a.c_str(), _b.c_str());                                   \
    }                                                                        \
  } while (0)

static void test_utf8() {
  CHECK_STR(wide_to_utf8(utf8_to_wide("Héllo — 世界 🎵")), "Héllo — 世界 🎵");
  CHECK_STR(wide_to_utf8(utf8_to_wide("")), "");
  // U+1F3B5 (🎵) round trips through surrogate pairs.
  CHECK_EQ(utf8_to_wide("🎵").size(), static_cast<size_t>(2));
  std::string malformed;
  malformed.push_back(static_cast<char>(0xC0));  // overlong lead
  malformed.push_back(static_cast<char>(0xAF));
  CHECK_STR(wide_to_utf8(utf8_to_wide(malformed)),
            "\xEF\xBF\xBD\xEF\xBF\xBD");
  CHECK_STR(wide_to_utf8(std::wstring(1, static_cast<wchar_t>(0xD800))),
            "\xEF\xBF\xBD");
  CHECK_STR(url_encode("Linkin Park & Friends"), "Linkin%20Park%20%26%20Friends");
}

static void test_json() {
  JValue v;
  CHECK(json_parse("{\"a\":1,\"b\":\"x\\u00e9\\n\",\"c\":[1,2,{\"d\":true}],"
                   "\"e\":null,\"f\":-2.5e2}",
                   v));
  CHECK_EQ(v.get_num("a"), 1.0);
  CHECK_STR(v.get_str("b"), "xé\n");
  CHECK_EQ(v.get_num("f"), -250.0);
  const JValue* c = v.find("c");
  CHECK(c && c->is_array() && c->arr.size() == 3);
  const JValue* d = c->arr[2].find("d");
  CHECK(d && d->b);
  CHECK(v.find("e") && v.find("e")->is_null());
  CHECK(v.find("zzz") == nullptr);

  // Realistic LRCLIB shape, with embedded newlines in syncedLyrics.
  std::string body =
      "{\"id\":3396224,\"trackName\":\"In the End\",\"artistName\":\"Linkin Park\","
      "\"albumName\":\"Hybrid Theory\",\"duration\":216.0,"
      "\"instrumental\":false,\"plainLyrics\":\"It starts with one\","
      "\"syncedLyrics\":\"[00:12.34] It starts with one\\n[00:15.00] thing\"}";
  CHECK(json_parse(body, v));
  CHECK_STR(v.get_str("trackName"), "In the End");
  CHECK_EQ(v.get_num("duration"), 216.0);
  CHECK(v.get_str("syncedLyrics").find('\n') != std::string::npos);

  CHECK(!json_parse("{\"broken\":", v));
  CHECK(!json_parse("", v));
  CHECK(json_parse("\"\\uD83C\\uDFB5\"", v));
  CHECK_STR(v.str, "🎵");

  const char* malformed_json[] = {
      "01", "-01", "+1", ".1", "1.", "1e", "1e+", "1e9999",
      "[1,]", "{\"x\":01}", "null trailing", "truefalse",
      "\"\\uD800\"", "\"\\uDC00\""};
  for (const char* sample : malformed_json) {
    CHECK(!json_parse(sample, v));
    CHECK(v.is_null());  // failed parsing never leaks a partial object
  }
  std::string raw_control = "\"line";
  raw_control.push_back('\n');
  raw_control += "break\"";
  CHECK(!json_parse(raw_control, v));
  std::string invalid_utf8 = "\"";
  invalid_utf8.push_back(static_cast<char>(0xC0));
  invalid_utf8 += "\"";
  CHECK(!json_parse(invalid_utf8, v));
}

static void test_semantic_versions() {
  StableVersion current, tag, higher, equal;
  CHECK(parse_stable_version("1.3.1", current));
  CHECK(parse_stable_version("v1.4.0", tag));
  CHECK(parse_stable_version("1.3.1", equal));
  CHECK_EQ(compare_stable_versions(tag, current), 1);
  CHECK_EQ(compare_stable_versions(current, tag), -1);
  CHECK_EQ(compare_stable_versions(current, equal), 0);
  CHECK(parse_stable_version("2.0.0", higher));
  CHECK_EQ(compare_stable_versions(higher, tag), 1);
  CHECK(!parse_stable_version("", current));
  CHECK(!parse_stable_version("1.4", current));
  CHECK(!parse_stable_version("1.4.0-rc.1", current));
  CHECK(!parse_stable_version("1.4.0+build", current));
  CHECK(!parse_stable_version("01.4.0", current));
  CHECK(!parse_stable_version("1.04.0", current));
  CHECK(!parse_stable_version("1.4.00", current));
  CHECK(!parse_stable_version("1.4.0junk", current));
  CHECK(!parse_stable_version("18446744073709551616.0.0", current));
}

static void test_release_metadata() {
  const std::string hash(64, 'a');
  const std::string body =
      R"JSON({"schema_version":1,"repository":"LIN4CRE/Youtube-Lyrics","tag":"v1.4.0","version":"1.4.0","channel":"stable","architecture":"x64","asset":"YoutubeLyrics.exe","size_bytes":567808,"sha256":")JSON" + hash + R"JSON("})JSON";
  ReleaseMetadata metadata;
  CHECK(parse_release_metadata(body, "LIN4CRE/Youtube-Lyrics", metadata));
  CHECK_STR(metadata.tag, "v1.4.0");
  CHECK_STR(metadata.version, "1.4.0");
  CHECK_EQ(metadata.size_bytes, static_cast<uint64_t>(567808));
  CHECK_STR(metadata.sha256, hash);
  CHECK(!parse_release_metadata(body, "other/repository", metadata));
  std::string bad = body;
  const size_t tag = bad.find("v1.4.0");
  bad.replace(tag, 6, "v1.4.1");
  CHECK(!parse_release_metadata(bad, "LIN4CRE/Youtube-Lyrics", metadata));
  bad = body;
  const size_t arch = bad.find("x64");
  bad.replace(arch, 3, "arm");
  CHECK(!parse_release_metadata(bad, "LIN4CRE/Youtube-Lyrics", metadata));

  std::string parsed_hash;
  CHECK(parse_sha256_sidecar(hash + "  YoutubeLyrics.exe\n", parsed_hash));
  CHECK_STR(parsed_hash, hash);
  CHECK(parse_sha256_sidecar(hash + " *YoutubeLyrics.exe\r\n", parsed_hash));
  CHECK(!parse_sha256_sidecar(hash + "  other.exe\n", parsed_hash));
  CHECK(!parse_sha256_sidecar(std::string(63, 'a'), parsed_hash));
}

static void test_lrc() {
  std::vector<LyricLine> lines;
  std::string lrc =
      "[ar:Linkin Park]\r\n[ti:In the End]\r\n[offset:-250]\r\n"
      "[00:12.34]It starts with one\r\n"
      "[00:15.10]thing\r\n"
      "[00:17.5][00:20.00]Chorus line\r\n"
      "\r\n"
      "[01:02.123]  spaced   out  \r\n";
  CHECK(lrc_parse(lrc, lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(5));
  CHECK_EQ(lines[0].t_ms, static_cast<int64_t>(12340 - 250));
  CHECK_STR(lines[0].text, "It starts with one");
  CHECK_EQ(lines[1].t_ms, static_cast<int64_t>(15100 - 250));
  CHECK_STR(lines[4].text, "spaced out");
  CHECK_EQ(lines[4].t_ms, static_cast<int64_t>(62123 - 250));
  // Multiple stamps on one line -> two entries, sorted afterwards.
  CHECK_EQ(lines[2].t_ms, static_cast<int64_t>(17500 - 250));
  CHECK_EQ(lines[3].t_ms, static_cast<int64_t>(20000 - 250));

  // Genuine Enhanced LRC markers survive parsing with byte ranges.
  CHECK(lrc_parse("[00:00:05.50]<00:00:05.50>Hello <00:00:06.00>world", lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(1));
  CHECK_EQ(lines[0].t_ms, static_cast<int64_t>(5500));
  CHECK_STR(lines[0].text, "Hello world");
  CHECK_EQ(lines[0].segments.size(), static_cast<size_t>(2));
  CHECK_EQ(lines[0].segments[0].t_ms, static_cast<int64_t>(5500));
  CHECK_EQ(lines[0].segments[0].begin, static_cast<size_t>(0));
  CHECK_EQ(lines[0].segments[0].end, static_cast<size_t>(5));
  CHECK_EQ(lines[0].segments[1].t_ms, static_cast<int64_t>(6000));
  CHECK_EQ(lines[0].segments[1].begin, static_cast<size_t>(5));
  CHECK_EQ(lines[0].segments[1].end, static_cast<size_t>(11));
  const std::string cached_lrc = lyrics_to_lrc(lines);
  CHECK(cached_lrc.find("<00:06.000>") != std::string::npos);
  std::vector<LyricLine> round_trip;
  CHECK(lrc_parse(cached_lrc, round_trip));
  CHECK_EQ(round_trip.size(), lines.size());
  CHECK_STR(round_trip[0].text, lines[0].text);
  CHECK_EQ(round_trip[0].t_ms, lines[0].t_ms);
  CHECK_EQ(round_trip[0].segments.size(), lines[0].segments.size());
  CHECK_EQ(round_trip[0].segments[1].t_ms, lines[0].segments[1].t_ms);
  CHECK_EQ(round_trip[0].segments[1].begin, lines[0].segments[1].begin);
  CHECK_EQ(round_trip[0].segments[1].end, lines[0].segments[1].end);

  // Repeated lines shift their inline markers; line offsets shift both layers.
  CHECK(lrc_parse("[offset:-250][00:01.000][00:05.000]"
                  "<00:01.000>hello <00:01.500>world", lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(2));
  CHECK_EQ(lines[0].t_ms, static_cast<int64_t>(750));
  CHECK_EQ(lines[0].segments[0].t_ms, static_cast<int64_t>(750));
  CHECK_EQ(lines[0].segments[1].t_ms, static_cast<int64_t>(1250));
  CHECK_EQ(lines[1].t_ms, static_cast<int64_t>(4750));
  CHECK_EQ(lines[1].segments[0].t_ms, static_cast<int64_t>(4750));
  CHECK_EQ(lines[1].segments[1].t_ms, static_cast<int64_t>(5250));

  // Malformed timing tags stay visible as text and do not imply word sync.
  CHECK(lrc_parse("[00:01.00]Hello <00:02.1234>world", lines));
  CHECK(lines[0].segments.empty());
  CHECK_STR(lines[0].text, "Hello <00:02.1234>world");
  std::string split_utf8_tag = "[00:01.00]";
  split_utf8_tag.push_back(static_cast<char>(0xE4));
  split_utf8_tag += "<00:01.20>";
  split_utf8_tag.push_back(static_cast<char>(0xB8));
  split_utf8_tag.push_back(static_cast<char>(0xAD));
  CHECK(lrc_parse(split_utf8_tag, lines));
  CHECK_STR(lines[0].text, "中");
  CHECK(lines[0].segments.empty());
  CHECK(!lrc_parse("[00:60.00]invalid seconds", lines));
  CHECK(!lrc_parse("[999999999999999999999999:00]overflow", lines));
  CHECK(lrc_parse("[1:02:03.004]hours form", lines));
  CHECK_EQ(lines[0].t_ms, static_cast<int64_t>(3723004));

  // Not lyrics at all.
  CHECK(!lrc_parse("just some prose\nno timestamps here", lines));
  CHECK(lines.empty());

  // Plain conversion keeps stanza breaks.
  std::vector<LyricLine> src = {{0, "one"}, {1000, "two"}, {2000, ""},
                                {3000, "three"}};
  CHECK_STR(lyrics_to_plain(src), "one\ntwo\n\nthree");
}

static void test_clean_track() {
  {
    TrackInfo t{"In the End (Official Video)", "Linkin Park - Topic", "", 216};
    TrackInfo c = clean_track(t);
    CHECK_STR(c.title, "In the End");
    CHECK_STR(c.artist, "Linkin Park");
  }
  {
    TrackInfo t{"Numb [Official Music Video] | HD", "Linkin Park", "", 185};
    TrackInfo c = clean_track(t);
    CHECK_STR(c.title, "Numb");
  }
  {
    TrackInfo t{"In the End (feat. Someone)", "Linkin Park", "", 0};
    CHECK_STR(clean_track(t).title, "In the End");
  }
  {
    // YouTube plain title form when the artist is missing.
    TrackInfo t{"In the End - Linkin Park", "", "", 0};
    TrackInfo c = clean_track(t);
    CHECK_STR(c.title, "In the End");
    CHECK_STR(c.artist, "Linkin Park");
    // The live media path does not invent an artist from an ambiguous title.
    TrackInfo conservative = clean_track(t, false);
    CHECK_STR(conservative.title, "In the End - Linkin Park");
    CHECK(conservative.artist.empty());
  }
  {
    TrackInfo t{"Somebody That I Used To Know", "Gotye", "", 0};
    CHECK_STR(clean_track(t).title, "Somebody That I Used To Know");
  }
  {
    TrackInfo t{"Bohemian Rhapsody (Remastered 2011)", "QueenVEVO", "", 0};
    TrackInfo c = clean_track(t);
    CHECK_STR(c.title, "Bohemian Rhapsody");
    CHECK_STR(c.artist, "Queen");
  }
  {
    // Em dash in the artist must survive.
    TrackInfo t{"Song", "Artist – Topic", "", 0};
    CHECK_STR(clean_track(t).artist, "Artist");
  }
}

static void test_candidates() {
  std::string search =
      "[{\"trackName\":\"In the End\",\"artistName\":\"Linkin Park\","
      "\"duration\":216,\"syncedLyrics\":\"[00:12.34] It starts with one\","
      "\"plainLyrics\":\"x\"},"
      "{\"trackName\":\"In the End\",\"artistName\":\"Some Cover Band\","
      "\"duration\":300,\"syncedLyrics\":null,\"plainLyrics\":\"cover\"}]";
  JValue v;
  CHECK(json_parse(search, v));
  TrackInfo want{"in the end", "Linkin Park", "", 216.4};
  double score = 0;
  const JValue* best = best_candidate(v, want, &score);
  CHECK(best != nullptr);
  CHECK_STR(best->get_str("artistName"), "Linkin Park");
  CHECK(score > 8.0);

  // A completely unrelated result set should be rejected.
  std::string other =
      "[{\"trackName\":\"Despacito\",\"artistName\":\"Luis Fonsi\","
      "\"duration\":230,\"syncedLyrics\":\"[00:01.00] y\"}]";
  CHECK(json_parse(other, v));
  CHECK(best_candidate(v, want, &score) == nullptr);

  // /api/get style single object.
  std::string get =
      "{\"trackName\":\"In the End\",\"artistName\":\"Linkin Park\","
      "\"duration\":216,\"syncedLyrics\":\"[00:12.34] It starts with one\\n"
      "[00:15.10] thing\"}";
  Lyrics out;
  CHECK(parse_lrclib_payload(get, want, out));
  CHECK(out.synced);
  CHECK_EQ(out.lines.size(), static_cast<size_t>(2));
  CHECK_STR(out.title, "In the End");

  // Unsynced fallback.
  std::string plain =
      "{\"trackName\":\"In the End\",\"artistName\":\"Linkin Park\","
      "\"syncedLyrics\":null,\"plainLyrics\":\"It starts with one\"}";
  CHECK(parse_lrclib_payload(plain, want, out));
  CHECK(!out.synced);
  CHECK_STR(out.plain, "It starts with one");

  // 404 body.
  CHECK(!parse_lrclib_payload("{\"code\":404,\"name\":\"TrackNotFound\"}", want, out));
}

// A deterministic fake font: 0.5em average advance.
static double fake_measure(const std::string& s, double px) {
  return static_cast<double>(s.size()) * 0.5 * px;
}

static void test_layout() {
  std::vector<LyricLine> lines = {{1000, "Hello"}, {2000, "World"},
                                  {3000, "a very long line that must wrap across the screen width"}};
  auto disp = build_display_lines(lines, 200, 20, fake_measure);
  CHECK(disp.size() >= 4);
  CHECK_STR(disp[0].text, "Hello");
  CHECK_EQ(disp[0].start_ms, static_cast<int64_t>(1000));
  CHECK_EQ(disp[0].end_ms, static_cast<int64_t>(2000));
  CHECK_EQ(disp[3].cont, true);

  // Unbroken words and CJK text wrap at UTF-8 codepoint boundaries.
  auto unbroken = build_display_lines({{0, "abcdefghij"}}, 20, 10, fake_measure);
  CHECK(unbroken.size() >= 3);
  std::string joined;
  for (const auto& row : unbroken) {
    CHECK(fake_measure(row.text, 10) <= 20);
    joined += row.text;
  }
  CHECK_STR(joined, "abcdefghij");
  auto cjk = build_display_lines({{0, "世界"}}, 16, 10, fake_measure);
  CHECK_EQ(cjk.size(), static_cast<size_t>(2));
  CHECK_EQ(cjk[0].text.size(), static_cast<size_t>(3));
  CHECK_EQ(cjk[1].text.size(), static_cast<size_t>(3));
  CHECK_STR(cjk[0].text + cjk[1].text, "世界");

  // Enhanced timing ranges remain attached when their line wraps.
  LyricLine timed{0, "hello world", {{0, 0, 5}, {500, 5, 11}}};
  auto timed_rows = build_display_lines({timed}, 30, 10, fake_measure);
  CHECK_EQ(timed_rows.size(), static_cast<size_t>(2));
  CHECK_EQ(timed_rows[0].segments.size(), static_cast<size_t>(1));
  CHECK_EQ(timed_rows[0].segments[0].begin, static_cast<size_t>(0));
  CHECK_EQ(timed_rows[0].segments[0].end, static_cast<size_t>(5));
  CHECK_EQ(timed_rows[1].segments.size(), static_cast<size_t>(1));
  CHECK_EQ(timed_rows[1].segments[0].begin, static_cast<size_t>(0));
  CHECK_EQ(timed_rows[1].segments[0].end, static_cast<size_t>(5));
  CHECK_EQ(active_index(timed_rows, 0), 0);
  CHECK_EQ(active_index(timed_rows, 499), 0);
  CHECK_EQ(active_index(timed_rows, 500), 1);

  // Active line selection.
  CHECK_EQ(active_index(disp, 0), -1);
  CHECK_EQ(active_index(disp, 1000), 0);
  CHECK_EQ(active_index(disp, 1999), 0);
  CHECK_EQ(active_index(disp, 2000), 1);
  CHECK_EQ(active_index(disp, 999999), 2);  // lands on the first chunk of the group
  CHECK_EQ(active_index({}, 5000), -1);

  // Progress through a line.
  std::vector<DispLine> two = {{0, 4000, "a"}, {4000, 10000, "b"}};
  CHECK_EQ(line_progress(two, 0, 0), 0.0);
  CHECK_EQ(line_progress(two, 0, 2000), 0.5);
  CHECK_EQ(line_progress(two, 0, 9999), 1.0);
  CHECK_EQ(line_progress(two, -1, 100), 0.0);
  // Long instrumental gap is capped.
  std::vector<DispLine> gap = {{0, 60000, "a"}};
  CHECK(line_progress(gap, 0, 9000) > 0.99);

  // Smoothing converges and never overshoots.
  double v = 0;
  for (int i = 0; i < 120; ++i) v = ease_towards(v, 100, 1.0 / 60.0, 0.12);
  CHECK(v > 99.0 && v <= 100.0001);
  CHECK_EQ(ease_towards(5, 9, 0.016, 0), 9.0);

  // Position interpolation.
  CHECK_EQ(interpolate_pos(1000, 5000, 6500, true, 200000), 2500);
  CHECK_EQ(interpolate_pos(1000, 5000, 6500, false, 200000), 1000);
  CHECK_EQ(interpolate_pos(199000, 5000, 6500, true, 200000), 200000);  // clamped
  CHECK_EQ(interpolate_pos(-50, 5000, 6500, false, 0), 0);
  CHECK_EQ(interpolate_pos(0, std::numeric_limits<int64_t>::min(),
                           std::numeric_limits<int64_t>::max(), true, 0),
           std::numeric_limits<int64_t>::max());
}

static void test_misc() {
  CHECK_STR(format_time(0), "0:00");
  CHECK_STR(format_time(216340), "3:36");
  CHECK_STR(format_time(-5), "0:00");
  CHECK_STR(track_label(TrackInfo{"In the End", "Linkin Park", "", 0}),
            "Linkin Park — In the End");
  CHECK_STR(squish("  a   b\n c "), "a b c");
  CHECK(text_similarity("In the End", "in the end") > 0.99);
  CHECK(text_similarity("In the End", "Numb") < 0.2);
  CHECK(text_similarity("", "x") == 0.0);
  CHECK_STR(track_key(TrackInfo{"In the End", "Linkin Park", "", 216.4}),
            "in the end|linkin park|216");
}


// End-to-end simulation: a realistic LRC played from 0 to the end, checking the
// invariants the renderer relies on (monotonic highlight, sane scroll, visible
// rows on screen at 1080p).
static void test_playback_simulation() {
  std::string lrc;
  const char* lines[] = {
      "[00:06.40](It starts with one)",
      "[00:12.34]One thing, I don't know why",
      "[00:15.10]It doesn't even matter how hard you try",
      "[00:18.20]Keep that in mind",
      "[00:20.00]I designed this rhyme",
      "[00:22.40]To explain in due time",
      "[00:25.00]All I know",
      "[00:27.60]Time is a valuable thing",
      "[00:30.10]Watch it fly by as the pendulum swings",
      "[00:33.00]Watch it count down to the end of the day",
      "[00:36.40]The clock ticks life away",
      "[00:39.00]It's so unreal",
      "[00:41.20]Didn't look out below",
      "[00:44.00]Watch the time go right out the window",
      "[01:10.00]I tried so hard and got so far",
      "[01:14.50]But in the end, it doesn't even matter",
  };
  for (const char* l : lines) {
    lrc += l;
    lrc += "\n";
  }
  std::vector<LyricLine> parsed;
  CHECK(lrc_parse(lrc, parsed));
  CHECK_EQ(parsed.size(), static_cast<size_t>(16));

  const int viewport_h = 1080;
  const double base = 45.0;  // what relayout() picks for 1080p
  auto disp = build_display_lines(parsed, 1400, base, fake_measure);
  CHECK_EQ(disp.size(), parsed.size());  // nothing needs wrapping at this width

  LineMetrics metrics = compute_line_metrics(disp, base * 1.5, 3000, base * 0.55);
  CHECK(metrics.top.size() >= disp.size());
  CHECK(metrics.total > 0);

  // Walk the whole song at 100 ms resolution.
  int last_active = -1;
  int changes = 0;
  for (int64_t t = 0; t <= 80000; t += 100) {
    int a = active_index(disp, t);
    if (a != last_active) {
      ++changes;
      CHECK(a >= last_active);  // never goes backwards during linear playback
      last_active = a;
    }
    double scroll = compute_scroll_target(metrics, a, viewport_h);
    // The active row must be on screen, and the sheet must never fly away.
    if (a >= 0) {
      double y = metrics.top[a] + scroll;
      CHECK(y > -metrics.height[a]);
      CHECK(y < viewport_h);
    }
    CHECK(scroll <= viewport_h * 0.45);
    CHECK(scroll >= viewport_h * 0.72 - metrics.total - 0.001);
  }
  CHECK_EQ(changes, 16);  // -1 plus one transition per lyric line
  CHECK_EQ(last_active, 15);

  // Smoothing must converge, and never overshoot the clamp window.
  double scroll = 0;
  for (int frame = 0; frame < 90; ++frame) {  // 1.5 s at 60 fps
    scroll = ease_towards(scroll, compute_scroll_target(metrics, 8, viewport_h),
                          1.0 / 60.0, 0.13);
  }
  CHECK(std::fabs(scroll - compute_scroll_target(metrics, 8, viewport_h)) < 1.0);

  // A short sheet is vertically centred rather than pinned to the top.
  std::vector<LyricLine> few = {{0, "one"}, {4000, "two"}};
  auto disp2 = build_display_lines(few, 1400, base, fake_measure);
  LineMetrics m2 = compute_line_metrics(disp2, base * 1.5, 3000, 25);
  double centred = compute_scroll_target(m2, -1, viewport_h);
  CHECK(std::fabs(centred - (viewport_h - m2.total) * 0.5) < 0.001);

  // Instrumental gaps add breathing room.
  std::vector<LyricLine> gapped = {{0, "a"}, {1000, "b"}, {30000, "c"}};
  auto disp3 = build_display_lines(gapped, 1400, base, fake_measure);
  LineMetrics m3 = compute_line_metrics(disp3, 60, 3000, 25);
  CHECK(m3.top[2] - m3.top[1] > 60 + 24);
  CHECK(m3.top[1] - m3.top[0] == 60);
}


// Regression: a stale media session that is still reporting the previous song
// must not be mistaken for the track that is playing now, and a search result
// for a cover must never win just because it is the only synced candidate.
static void test_wrong_song_guards() {
  // Same recording, different capitalisation / punctuation is the same track.
  TrackInfo a{"In the End", "Linkin Park", "Hybrid Theory", 216.0};
  TrackInfo b{"in the end", "LINKIN PARK", "", 216.4};
  CHECK(same_track(a, b));

  // A different recording with a colliding title is not.
  TrackInfo c{"In the End", "Linkin Park", "", 260.0};
  CHECK(!same_track(a, c));
  TrackInfo d{"In the End", "Some Cover Band", "", 216.0};
  CHECK(!same_track(a, d));

  // A player that withdraws its artist mid-session has not changed the song.
  TrackInfo e{"In the End", "", "", 216.0};
  CHECK(same_track(a, e));

  // Short-lived non-music sessions are never watched.
  TrackInfo ad{"Advertisement", "Spotify", "", 20.0};
  CHECK(!is_watchable_app("Spotify.exe", ad));
  CHECK(!is_watchable_app("Chrome", TrackInfo{"Intro", "Band", "", 18.0}));
  CHECK(!is_watchable_app("ShellExperienceHost", TrackInfo{"Settings", "", "", 0}));
  CHECK(is_watchable_app("Chrome", TrackInfo{"In the End", "Linkin Park", "", 216.0}));
  CHECK(is_watchable_app("Spotify.exe", TrackInfo{"In the End", "Linkin Park", "", 0}));
  // An unknown app is trusted only when it reports a real song length.
  CHECK(!is_watchable_app("SomeGame.exe", TrackInfo{"Theme", "Composer", "", 40.0}));
  CHECK(is_watchable_app("SomeGame.exe", TrackInfo{"Theme", "Composer", "", 180.0}));

  // Candidate ranking: an unrelated artist with the right title must be
  // rejected even when it is the only synced result in the list.
  JValue v;
  std::string covers =
      "[{\"trackName\":\"In the End\",\"artistName\":\"Karaoke Kings\","
      "\"duration\":216,\"syncedLyrics\":\"[00:12.00] wrong words\"},"
      "{\"trackName\":\"In the End\",\"artistName\":\"Linkin Park\","
      "\"duration\":215,\"syncedLyrics\":\"[00:12.00] It starts with one\"}]";
  CHECK(json_parse(covers, v));
  const JValue* best = best_candidate(v, a, nullptr);
  CHECK(best != nullptr);
  CHECK_STR(best->get_str("artistName"), "Linkin Park");

  // Right title, right artist, but the wrong length: a live version. A 40 s
  // discrepancy must lose to a studio cut that matches.
  std::string versions =
      "[{\"trackName\":\"In the End\",\"artistName\":\"Linkin Park\","
      "\"duration\":258,\"syncedLyrics\":\"[00:12.00] live\"},"
      "{\"trackName\":\"In the End\",\"artistName\":\"Linkin Park\","
      "\"duration\":216,\"syncedLyrics\":\"[00:12.00] studio\"}]";
  CHECK(json_parse(versions, v));
  best = best_candidate(v, a, nullptr);
  CHECK(best != nullptr);
  CHECK_EQ(static_cast<int>(best->get_num("duration")), 216);

  // A different title from the same artist must not win on a matching duration.
  std::string same_artist_wrong_title =
      "[{\"trackName\":\"Numb\",\"artistName\":\"Linkin Park\","
      "\"duration\":216,\"syncedLyrics\":\"[00:12.00] wrong song\"}]";
  CHECK(json_parse(same_artist_wrong_title, v));
  CHECK(best_candidate(v, a, nullptr) == nullptr);

  // Even the exact-match endpoint is validated against the requested track;
  // a malformed or stale response cannot bypass the search candidate gate.
  Lyrics rejected;
  CHECK(!parse_lrclib_payload(
      "{\"trackName\":\"Numb\",\"artistName\":\"Linkin Park\","
      "\"duration\":216,\"syncedLyrics\":\"[00:12.00] wrong song\"}",
      a, rejected));

  // If the media source omitted the artist, exact title alone is not enough;
  // require matching duration to avoid a same-title song from another artist.
  TrackInfo artist_missing{"In the End", "", "", 216.0};
  std::string title_only_wrong_length =
      "[{\"trackName\":\"In the End\",\"artistName\":\"Other Artist\","
      "\"duration\":258,\"syncedLyrics\":\"[00:12.00] other song\"}]";
  CHECK(json_parse(title_only_wrong_length, v));
  CHECK(best_candidate(v, artist_missing, nullptr) == nullptr);
  std::string title_only_right_length =
      "[{\"trackName\":\"In the End\",\"artistName\":\"Linkin Park\","
      "\"duration\":216,\"syncedLyrics\":\"[00:12.00] same recording\"}]";
  CHECK(json_parse(title_only_right_length, v));
  CHECK(best_candidate(v, artist_missing, nullptr) != nullptr);
  artist_missing.duration_s = 0;
  CHECK(best_candidate(v, artist_missing, nullptr) == nullptr);

  // Only an artist mismatch available -> refuse rather than guess.
  std::string only_cover =
      "[{\"trackName\":\"In the End\",\"artistName\":\"Karaoke Kings\","
      "\"duration\":216,\"syncedLyrics\":\"[00:12.00] wrong words\"}]";
  CHECK(json_parse(only_cover, v));
  CHECK(best_candidate(v, a, nullptr) == nullptr);
}

// The wider net: a second provider, pacing for untimed lyrics, and a local
// override file.
static void test_extra_providers() {
  Lyrics out;
  CHECK(parse_lyrics_ovh_payload("{\"lyrics\":\"first line\\nsecond line\"}", out));
  CHECK(!out.synced);
  CHECK_STR(out.source, "lyrics.ovh");
  CHECK(out.plain.find("second line") != std::string::npos);
  // The service reports misses in-band with HTTP 200.
  CHECK(!parse_lyrics_ovh_payload("{\"lyrics\":\"No lyrics found\"}", out));
  CHECK(!parse_lyrics_ovh_payload("{\"error\":\"nope\"}", out));
  CHECK(!parse_lyrics_ovh_payload("not json", out));

  // Untimed lyrics are paced across the track so the view still scrolls.
  auto paced = pace_plain_lyrics("one\n\ntwo\nthree", 2000, 4000);
  CHECK_EQ(paced.size(), static_cast<size_t>(4));
  CHECK_EQ(paced[0].t_ms, static_cast<int64_t>(2000));
  CHECK(paced[1].text.empty());
  CHECK_EQ(paced[3].t_ms, static_cast<int64_t>(14000));

  // A hand-written .lrc or .txt overrides whatever the lookup returned.
  CHECK(parse_local_lyrics("[00:01.00]hello\n[00:05.00]world", out));
  CHECK(out.synced);
  CHECK_EQ(out.lines.size(), static_cast<size_t>(2));
  CHECK_STR(out.source, "local file");
  CHECK(parse_local_lyrics("just words\nmore words", out));
  CHECK(!out.synced);
  CHECK(!parse_local_lyrics("   \n  ", out));

  // Local lyric files are commonly saved with a BOM by Windows editors.
  std::string utf8_bom;
  utf8_bom.push_back(static_cast<char>(0xEF));
  utf8_bom.push_back(static_cast<char>(0xBB));
  utf8_bom.push_back(static_cast<char>(0xBF));
  utf8_bom += "[00:01.00]hello";
  CHECK(parse_local_lyrics(utf8_bom, out));
  CHECK(out.synced);
  CHECK_STR(out.lines[0].text, "hello");

  std::string utf16le;
  utf16le.push_back(static_cast<char>(0xFF));
  utf16le.push_back(static_cast<char>(0xFE));
  for (unsigned char c : std::string("Hello\nworld")) {
    utf16le.push_back(static_cast<char>(c));
    utf16le.push_back('\0');
  }
  CHECK(parse_local_lyrics(utf16le, out));
  CHECK(!out.synced);
  CHECK_STR(out.plain, "Hello\nworld");
}

static void test_enhanced_matching() {
  // Test token containment
  CHECK(token_containment("Gladiator (from 'The Greatest')", "Gladiator") >= 0.99);
  CHECK(token_containment("Wyclef Jean and Kofi Black", "Wyclef Jean") >= 0.99);
  CHECK(token_containment("In the End", "Numb") == 0.0);

  // Test trailing 4K stripping
  TrackInfo t1{"Messy X Children (Mashup) 4K", "Liam Conner", "", 152};
  TrackInfo c1 = clean_track(t1);
  CHECK_STR(c1.title, "Messy X Children");

  // Test best_candidate with subtitle / featured artist
  JValue v;
  std::string res =
      "[{\"trackName\":\"Gladiator\",\"artistName\":\"Wyclef Jean\","
      "\"duration\":91,\"syncedLyrics\":\"[00:10.00] lyrics line\"}]";
  CHECK(json_parse(res, v));
  TrackInfo want{"Gladiator (from \"The Greatest\")", "Wyclef Jean and Kofi Black", "", 91};
  double score = 0;
  const JValue* best = best_candidate(v, want, &score);
  CHECK(best != nullptr);
}


// Live/remix/instrumental editions are different recordings: they must not be
// identified, cached or matched as if they were the studio track.
static void test_track_variants() {
  CHECK_EQ(detect_variant("In the End"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("In the End (Official Video)"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("Numb (Live at Wembley)"), TrackVariant::Live);
  CHECK_EQ(detect_variant("Song - Live"), TrackVariant::Live);
  CHECK_EQ(detect_variant("Song (MTV Unplugged)"), TrackVariant::Live);
  CHECK_EQ(detect_variant("Song (Club Mix)"), TrackVariant::Remix);
  CHECK_EQ(detect_variant("Song (Slowed + Reverb)"), TrackVariant::Remix);
  CHECK_EQ(detect_variant("Song (Instrumental)"), TrackVariant::Instrumental);
  CHECK_EQ(detect_variant("Song (Karaoke Version)"), TrackVariant::Karaoke);
  // "delivery" and "olive" contain the letters of "live" but not the word.
  CHECK_EQ(detect_variant("Special Delivery"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("Olive Branch"), TrackVariant::Studio);
  CHECK_STR(track_variant_name(TrackVariant::Live), "live");
  CHECK_STR(track_variant_name(TrackVariant::Studio), "studio");

  // The studio edition accepts anything; a marked edition needs a marker.
  CHECK(variant_compatible(TrackVariant::Studio, "Song", ""));
  CHECK(variant_compatible(TrackVariant::Live, "Song (Live)", ""));
  CHECK(variant_compatible(TrackVariant::Live, "Song", "Live at Leeds"));
  CHECK(!variant_compatible(TrackVariant::Live, "Song", ""));
  CHECK(!variant_compatible(TrackVariant::Live, "Song (Remix)", ""));
  CHECK(!variant_compatible(TrackVariant::Instrumental, "Song", ""));

  // Caching identity and "did the track change" both include the edition.
  TrackInfo studio{"Song", "Artist", "", 200, "Song"};
  TrackInfo live{"Song", "Artist", "", 260, "Song (Live)"};
  CHECK(studio.variant() == TrackVariant::Studio);
  CHECK(live.variant() == TrackVariant::Live);
  CHECK(track_key(studio) != track_key(live));
  CHECK(!same_track(studio, live));
  CHECK(same_track(studio, TrackInfo{"Song", "Artist", "", 200, "Song"}));
  // A provider sheet for the wrong edition is rejected before display.
  const std::string studio_sheet =
      "[{\"trackName\":\"Song\",\"artistName\":\"Artist\",\"duration\":200,"
      "\"syncedLyrics\":\"[00:01.00] one\\n[00:05.00] two\"}]";
  JValue search;
  CHECK(json_parse(studio_sheet, search));
  double score = 0;
  CHECK(best_candidate(search, studio, &score) != nullptr);
  CHECK(best_candidate(search, live, &score) == nullptr);
  const std::string live_sheet =
      "[{\"trackName\":\"Song (Live)\",\"artistName\":\"Artist\",\"duration\":260,"
      "\"syncedLyrics\":\"[00:01.00] one\\n[00:05.00] two\"}]";
  CHECK(json_parse(live_sheet, search));
  const JValue* matched = best_candidate(search, live, &score);
  CHECK(matched != nullptr);
  Lyrics parsed;
  CHECK(parse_lrclib_payload(live_sheet, live, parsed));
  CHECK_STR(parsed.variant, "live");
}

// 1.6.0: a sheet for a different song, or a different-length recording of the
// same song, must never be shown as timed karaoke.
static void test_wrong_sheet_rejection() {
  // Real words at the end of a title are part of the title.
  CHECK_STR(clean_track(TrackInfo{"Car Radio", "Twenty One Pilots", "", 0}).title, "Car Radio");
  CHECK_STR(clean_track(TrackInfo{"Come Clean", "Hilary Duff", "", 0}).title, "Come Clean");
  CHECK_STR(clean_track(TrackInfo{"Numb 4K", "Linkin Park", "", 0}).title, "Numb");

  // "Home" is not "Homecoming": containment counts whole words only.
  CHECK(text_similarity("Home", "Homecoming") < 0.6);
  CHECK(text_similarity("In the End", "in the end") > 0.99);
  CHECK(same_title_text("In The End", "in the end!"));
  CHECK(!same_title_text("Home", "Homecoming"));

  // A timed sheet has to fit the recording it is shown against.
  Lyrics sheet;
  sheet.synced = true;
  for (int i = 0; i < 40; ++i) sheet.lines.push_back(LyricLine{i * 5000, "line", {}});
  CHECK(lyrics_fit_duration(sheet, 200.0));   // last line at 195 s
  CHECK(!lyrics_fit_duration(sheet, 150.0));  // runs 45 s past the end
  CHECK(!lyrics_fit_duration(sheet, 900.0));  // stops in the first quarter
  CHECK(lyrics_fit_duration(sheet, 0.0));     // unknown length: nothing to compare

  auto one = [](const char* title, const char* artist, int duration) {
    return std::string("[{\"trackName\":\"") + title + "\",\"artistName\":\"" + artist +
           "\",\"duration\":" + std::to_string(duration) +
           ",\"syncedLyrics\":\"[00:01.00] a\\n[00:02.00] b\",\"plainLyrics\":\"a\\nb\"}]";
  };
  JValue v;
  double score = 0;
  bool trusted = false;
  TrackInfo want{"In the End", "Linkin Park", "", 216.0};

  CHECK(json_parse(one("In the End", "Linkin Park", 217), v));
  CHECK(best_candidate(v, want, &score, &trusted) != nullptr);
  CHECK(trusted);

  // Same song, 30 s longer (a live or extended cut): words yes, timing no.
  CHECK(json_parse(one("In the End", "Linkin Park", 246), v));
  trusted = true;
  CHECK(best_candidate(v, want, &score, &trusted) != nullptr);
  CHECK(!trusted);

  // More than a minute out is a different recording altogether.
  CHECK(json_parse(one("In the End", "Linkin Park", 300), v));
  CHECK(best_candidate(v, want, &score, &trusted) == nullptr);

  // A near-miss title is only accepted when the length agrees closely.
  CHECK(json_parse(one("In the End Part Two", "Linkin Park", 226), v));
  CHECK(best_candidate(v, want, &score, &trusted) == nullptr);

  // No artist to check against: the length has to agree.
  TrackInfo anonymous{"In the End", "", "", 216.0};
  CHECK(json_parse(one("In the End", "Someone Else", 240), v));
  CHECK(best_candidate(v, anonymous, &score, &trusted) == nullptr);

  // lyrics.ovh states which song it answered with; a different one is refused.
  Lyrics out;
  CHECK(!parse_lyrics_ovh_payload(
      "{\"lyrics\":\"Paroles de la chanson Despacito par Luis Fonsi\\nline one\\nline two\"}",
      want, out));
  CHECK(parse_lyrics_ovh_payload(
      "{\"lyrics\":\"Paroles de la chanson In the End par Linkin Park\\nIt starts with one\\nthing\"}",
      want, out));
  CHECK(out.plain.find("Paroles") == std::string::npos);
  CHECK(out.plain.find("It starts with one") != std::string::npos);

  // An answer far longer than any song is a scraped page, not lyrics.
  std::string huge = "{\"lyrics\":\"";
  for (int i = 0; i < 450; ++i) huge += "another line\\n";
  huge += "\"}";
  CHECK(!parse_lyrics_ovh_payload(huge, want, out));
}

// 1.7.0: edition words are only edition notes where an edition is written.
static void test_edition_context() {
  // Ordinary title words must not turn a studio song into another edition.
  CHECK_EQ(detect_variant("Live Forever"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("Oasis - Live Forever (Official Video)"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("Live and Let Die"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("Obsession"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("Flip Flop"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("Reverberation"), TrackVariant::Studio);
  CHECK_EQ(detect_variant("Slowed Down Heart"), TrackVariant::Studio);
  // ...while written edition notes still count.
  CHECK_EQ(detect_variant("Live Forever (Live at Knebworth)"), TrackVariant::Live);
  CHECK_EQ(detect_variant("Hello - Live at the Royal Albert Hall"), TrackVariant::Live);
  CHECK_EQ(detect_variant("Song [Live]"), TrackVariant::Live);
  CHECK_EQ(detect_variant("Song (BBC Radio 1 Session)"), TrackVariant::Live);
  CHECK_EQ(detect_variant("Song (Slowed)"), TrackVariant::Remix);
  CHECK_EQ(detect_variant("Song (DJ Flip)"), TrackVariant::Remix);
  CHECK_EQ(detect_variant("Song Remix"), TrackVariant::Remix);
  CHECK_EQ(detect_variant("Song slowed + reverb"), TrackVariant::Remix);
  // Albums are named after the edition, so any edition word counts there.
  CHECK_EQ(detect_variant("Live in Paris", true), TrackVariant::Live);
  CHECK_EQ(detect_variant("Live in Paris"), TrackVariant::Studio);
  CHECK(variant_compatible(TrackVariant::Live, "Song", "Live in Paris"));

  // A studio song whose name contains an edition word keeps a plain identity.
  TrackInfo forever = clean_track(TrackInfo{"Live Forever", "Oasis", "", 276});
  CHECK(forever.variant() == TrackVariant::Studio);
  CHECK(track_key(forever).find("|live") == std::string::npos);

  // Bracketed subtitles are kept unless they are a whole-word noise note.
  CHECK_STR(clean_track(TrackInfo{"Song (Lost)", "A", "", 0}).title, "Song (Lost)");
  CHECK_STR(clean_track(TrackInfo{"Song (After Hours)", "A", "", 0}).title, "Song (After Hours)");
  CHECK_STR(clean_track(TrackInfo{"Song (Alive)", "A", "", 0}).title, "Song (Alive)");
  CHECK_STR(clean_track(TrackInfo{"Song (Official Lyric Video)", "A", "", 0}).title, "Song");
  CHECK_STR(clean_track(TrackInfo{"Song (2011 Remaster)", "A", "", 0}).title, "Song");
  CHECK_STR(clean_track(TrackInfo{"Song (feat. Someone)", "A", "", 0}).title, "Song");
  CHECK_STR(clean_track(TrackInfo{"Song [HD]", "A", "", 0}).title, "Song");
}

static void test_offset_store() {
  OffsetStore store;
  CHECK_EQ(store.get("a|b"), 0);
  store.set("a|b", 250);
  store.set("c|d", -750);
  CHECK_EQ(store.get("a|b"), 250);
  CHECK_EQ(store.get("c|d"), -750);
  // Round trip.
  const std::string text = serialize_offset_store(store);
  OffsetStore loaded = parse_offset_store(text);
  CHECK_EQ(loaded.entries.size(), static_cast<size_t>(2));
  CHECK_EQ(loaded.get("a|b"), 250);
  CHECK_EQ(loaded.get("c|d"), -750);
  // Zero forgets; values are clamped; the newest entry is last.
  loaded.set("a|b", 0);
  CHECK_EQ(loaded.entries.size(), static_cast<size_t>(1));
  loaded.set("e", 999999);
  CHECK_EQ(loaded.get("e"), OffsetStore::kMaxOffsetMs);
  loaded.set("c|d", -500);
  CHECK_STR(loaded.entries.back().first, "c|d");
  // The table is bounded and drops the oldest songs first.
  OffsetStore full;
  for (int i = 0; i < 450; ++i) full.set("song" + std::to_string(i), 250);
  CHECK_EQ(full.entries.size(), OffsetStore::kMaxEntries);
  CHECK_EQ(full.get("song0"), 0);
  CHECK_EQ(full.get("song449"), 250);
  // Damaged files are read as far as they make sense, never trusted blindly.
  OffsetStore damaged = parse_offset_store(
      "250\tgood|one\n\nnot a line\nabc\tbad\n\t\n-\tbad\n99999999\thuge\n-250\tgood|two\r\n");
  CHECK_EQ(damaged.get("good|one"), 250);
  CHECK_EQ(damaged.get("good|two"), -250);
  CHECK_EQ(damaged.get("bad"), 0);
  CHECK_EQ(damaged.get("huge"), OffsetStore::kMaxOffsetMs);
  // Keys cannot break the line format.
  OffsetStore odd;
  odd.set("a\tb\nc", 250);
  CHECK_EQ(parse_offset_store(serialize_offset_store(odd)).entries.size(),
           static_cast<size_t>(1));

  // The preference key ignores the length a particular player reports.
  TrackInfo a{"Song", "Artist", "", 200.2, "Song"};
  TrackInfo b{"Song", "Artist", "", 203.9, "Song"};
  TrackInfo live{"Song", "Artist", "", 200, "Song (Live)"};
  CHECK_STR(track_preference_key(a), track_preference_key(b));
  CHECK(track_preference_key(a) != track_preference_key(live));
}

static void test_report_redaction() {
  CHECK_STR(replace_all_ci("C:\\Users\\Jo\\x and c:\\users\\jo\\y", "C:\\Users\\Jo", "~"),
            "~\\x and ~\\y");
  CHECK_STR(replace_all_ci("abc", "", "x"), "abc");
  const std::string report =
      "log: C:\\Users\\Jo Bloggs\\AppData\\Local\\YouTubeLyrics\\ytlyrics.log\n"
      "url: https://example.test/a?track=Hello&token=abc123&x=1\n"
      "hdr: Authorization: Bearer zzz.yyy\n"
      "title: Key Largo\n";
  const std::string clean = redact_report(report, "C:\\Users\\Jo Bloggs", "Jo Bloggs");
  CHECK(clean.find("Jo Bloggs") == std::string::npos);
  CHECK(clean.find("%USERPROFILE%\\AppData\\Local\\YouTubeLyrics") != std::string::npos);
  CHECK(clean.find("abc123") == std::string::npos);
  CHECK(clean.find("zzz.yyy") == std::string::npos);
  CHECK(clean.find("track=Hello") != std::string::npos);
  CHECK(clean.find("x=1") != std::string::npos);
  CHECK(clean.find("Key Largo") != std::string::npos);
}

int main() {
  test_utf8();
  test_json();
  test_semantic_versions();
  test_release_metadata();
  test_lrc();
  test_clean_track();
  test_candidates();
  test_layout();
  test_misc();
  test_playback_simulation();
  test_wrong_song_guards();
  test_extra_providers();
  test_enhanced_matching();
  test_track_variants();
  test_wrong_sheet_rejection();
  test_edition_context();
  test_offset_store();
  test_report_redaction();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
