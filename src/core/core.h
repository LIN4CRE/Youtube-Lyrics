// Youtube-Lyrics - portable core logic.
//
// Everything in this file is plain C++17 with no platform dependencies so it
// can be unit-tested on any machine (see tests/core_test.cc). The Windows GUI
// layer (src/win/*) only does drawing, input and I/O on top of this.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ytl {

// ---------------------------------------------------------------- text utils

std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);
std::string to_lower_ascii(std::string s);
std::string trim(const std::string& s);
bool starts_with(const std::string& s, const char* prefix);
bool ends_with(const std::string& s, const char* suffix);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
// Percent-encode for use in a URL query value.
std::string url_encode(const std::string& s);
// Collapse runs of whitespace to single spaces, drop control chars.
std::string squish(const std::string& s);

// ---------------------------------------------------------------------- JSON

enum class JType { Null, Bool, Number, String, Array, Object };

struct JValue {
  JType type = JType::Null;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JValue> arr;
  std::vector<std::pair<std::string, JValue>> obj;

  bool is_null() const { return type == JType::Null; }
  bool is_string() const { return type == JType::String; }
  bool is_number() const { return type == JType::Number; }
  bool is_array() const { return type == JType::Array; }
  bool is_object() const { return type == JType::Object; }

  const JValue* find(const char* key) const;
  std::string get_str(const char* key, const char* def = "") const;
  double get_num(const char* key, double def = 0.0) const;
};

// Returns false on malformed input. Parsing is lenient about trailing garbage.
bool json_parse(const std::string& src, JValue& out);

// ------------------------------------------------------------------- lyrics

struct LyricLine {
  // Start time of the line, milliseconds from the start of the track.
  int64_t t_ms = 0;
  // Line text, UTF-8.
  std::string text;
};

struct Lyrics {
  // True when timestamps were available (karaoke view possible).
  bool synced = false;
  std::vector<LyricLine> lines;  // sorted by t_ms; may include blank spacer lines
  std::string plain;             // unsynced fallback text
  std::string source;            // e.g. "LRCLIB"
  int providers_tried = 0;       // services attempted for this track
  std::string title, artist, album;
  bool empty() const { return lines.empty() && plain.empty(); }
};

// Parses an LRC document. Supports [mm:ss.xx]/[mm:ss:xxx] stamps, multiple
// stamps per line, [offset:+/-ms] and inline <mm:ss.xx> word tags (dropped).
// Blank-text timestamps are kept, they act as instrumental gaps.
bool lrc_parse(const std::string& lrc, std::vector<LyricLine>& out);

// Turns a synced lyric list into plain text (used when a service only gives
// unsynced text, or for the "plain" fallback view).
std::string lyrics_to_plain(const std::vector<LyricLine>& lines);

// ----------------------------------------------------------- track matching

struct TrackInfo {
  std::string title;
  std::string artist;
  std::string album;
  double duration_s = 0.0;  // 0 when unknown
};

// Strips "Official Video", "(feat. X)", "- Topic", "VEVO", trailing "| ..."
// and similar noise that media apps put in their reported metadata.
TrackInfo clean_track(TrackInfo t, bool infer_artist_from_title = true);

// Stable identity for caching / "did the track change" checks.
std::string track_key(const TrackInfo& t);

// True when two observations describe the same recording: the same title and
// artist, and durations that do not contradict each other. Used to reject a
// stale media session that is still reporting the previous song.
bool same_track(const TrackInfo& a, const TrackInfo& b);

// Whether a track is worth watching for: music, not video, podcasts or UI
// sounds. Streams (no duration) count only when they come from a known player.
// Rejecting the rest is what stops the app from chasing a YouTube video's audio
// track or a game's now-playing widget.
bool is_watchable_app(const std::string& app_id, const TrackInfo& t);

// Human readable "Artist — Title".
std::string track_label(const TrackInfo& t);

// Picks the best record out of an LRCLIB /api/search array for `want`.
// Returns nullptr when nothing is remotely close. `*score` gets the score.
const JValue* best_candidate(const JValue& search_results, const TrackInfo& want,
                             double* score = nullptr);

// Reads a lyrics payload: either an LRCLIB /api/get object or the best entry of
// a /api/search array. Returns false when no usable lyrics were found.
bool parse_lrclib_payload(const std::string& body, const TrackInfo& want, Lyrics& out);

// lyrics.ovh /api/v1/{artist}/{title}: {"lyrics":"..."}. Always untimed.
bool parse_lyrics_ovh_payload(const std::string& body, Lyrics& out);

// Plain (untimed) lyrics are turned into evenly paced display rows for the
// fallback view. `lead_ms` delays the first row and `per_line_ms` paces them.
std::vector<LyricLine> pace_plain_lyrics(const std::string& plain, int64_t lead_ms,
                                         int64_t per_line_ms);

// Normalises a hand-written LRC or plain-text lyric document (the user can drop
// a .lrc next to a cached track to override the lookup).
bool parse_local_lyrics(const std::string& text, Lyrics& out);

// ------------------------------------------------------------------- layout

// Measures the width of a piece of text at `px` height. Injected so the layout
// code is testable without a font engine.
using MeasureFn = std::function<double(const std::string& utf8, double px)>;

struct DispLine {
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  std::string text;      // may be empty (instrumental gap)
  bool cont = false;     // true for wrapped continuation of previous line
};

// Wraps long lyric lines to `max_w`. `px_for_line` yields the font size a line
// is drawn at (the active line is bigger, so wrapping can depend on it; we pass
// the same size for every line to keep line breaks stable while scrolling).
std::vector<DispLine> build_display_lines(const std::vector<LyricLine>& lines,
                                          double max_w, double px,
                                          const MeasureFn& measure);

// Index of the line that is currently being sung (-1 before the first line).
int active_index(const std::vector<DispLine>& lines, int64_t pos_ms);

// Italic progress through the active line, 0..1. Very long gaps (instrumentals)
// are capped so the karaoke wipe does not crawl forever.
double line_progress(const std::vector<DispLine>& lines, int index, int64_t pos_ms);

// Vertical metrics for a laid-out lyric sheet.
struct LineMetrics {
  std::vector<double> top;     // y of each row's box, before scrolling
  std::vector<double> height;  // box height of each row
  double total = 0.0;
};

// Lays rows out top to bottom. `gap_ms`+ silences between lines get extra air,
// which is what makes a lyric sheet readable when there are instrumental breaks.
LineMetrics compute_line_metrics(const std::vector<DispLine>& lines,
                                 double box_h, int64_t gap_ms,
                                 double gap_extra_px);

// Scroll offset that places the active row slightly above centre. Clamped so a
// short sheet stays centred and the end of a long sheet is never overshot.
double compute_scroll_target(const LineMetrics& metrics, int active,
                             int viewport_h, double anchor = 0.42);

// Frame-rate independent exponential smoothing towards `target`.
double ease_towards(double current, double target, double dt_s, double response);

// Extrapolates playback position between media polls.
int64_t interpolate_pos(int64_t anchor_ms, int64_t anchor_at_ms, int64_t now_ms,
                        bool playing, int64_t duration_ms);

// Formats milliseconds as m:ss.
std::string format_time(int64_t ms);

// -------------------------------------------------------------------- misc

// Rough similarity in [0,1] used for ranking search results.
double text_similarity(const std::string& a, const std::string& b);

}  // namespace ytl
