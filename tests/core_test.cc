// Unit tests for the portable core. Build & run on any platform:
//   g++ -std=c++17 -O1 -Wall src/core/core.cc tests/core_test.cc -o /tmp/core_test && /tmp/core_test
#include <cmath>
#include <cstdio>
#include <cstring>
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

  // Enhanced LRC word tags and hours field.
  CHECK(lrc_parse("[00:00:05.50]<00:00:05.50>Hello <00:00:06.00>world", lines));
  CHECK_EQ(lines.size(), static_cast<size_t>(1));
  CHECK_EQ(lines[0].t_ms, static_cast<int64_t>(5500));
  CHECK_STR(lines[0].text, "Hello world");

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

int main() {
  test_utf8();
  test_json();
  test_lrc();
  test_clean_track();
  test_candidates();
  test_layout();
  test_misc();
  test_playback_simulation();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
