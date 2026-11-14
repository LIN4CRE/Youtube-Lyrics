// Youtube-Lyrics - portable core logic.
//
// Everything in this file is plain C++17 with no platform dependencies so it
// can be unit-tested on any machine (see tests/core_test.cc). The Windows GUI
// layer (src/win/*) only does drawing, input and I/O on top of this.
#pragma once

#include <cstddef>
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

// Returns false on malformed input, invalid UTF-8, or trailing non-whitespace.
bool json_parse(const std::string& src, JValue& out);

// Strict stable semantic versions used by the release updater. An optional
// leading 'v' is accepted for Git tags; prerelease/build suffixes are rejected.
struct StableVersion {
  uint64_t major = 0;
  uint64_t minor = 0;
  uint64_t patch = 0;
};
bool parse_stable_version(const std::string& text, StableVersion& out);
// Returns -1, 0, or 1 using semantic-version numeric precedence.
int compare_stable_versions(const StableVersion& a, const StableVersion& b);

struct ReleaseMetadata {
  std::string repository;
  std::string tag;
  std::string version;
  std::string channel;
  std::string architecture;
  std::string asset;
  std::string sha256;
  uint64_t size_bytes = 0;
};
// Parses the small release.json attached to stable GitHub Releases and rejects
// mismatched tag/version, architecture, asset name, repo, or checksum data.
bool parse_release_metadata(const std::string& json,
                            const std::string& expected_repository,
                            ReleaseMetadata& out);
// Parses the common `sha256  YoutubeLyrics.exe` sidecar format.
bool parse_sha256_sidecar(const std::string& text, std::string& sha256);

// ------------------------------------------------------------------- lyrics

struct LyricSegment {
  // Genuine Enhanced LRC timestamp for this segment, milliseconds from track start.
  int64_t t_ms = 0;
  // UTF-8 byte range within the owning line's normalized `text`.
  size_t begin = 0;
  size_t end = 0;
};

struct LyricLine {
  LyricLine() = default;
  LyricLine(int64_t start, std::string value,
            std::vector<LyricSegment> timed_segments = {})
      : t_ms(start), text(std::move(value)), segments(std::move(timed_segments)) {}

  // Start time of the line, milliseconds from the start of the track.
  int64_t t_ms = 0;
  // Line text, UTF-8.
  std::string text;
  // Present only when the source contains inline Enhanced LRC timestamps.
  std::vector<LyricSegment> segments;
};

struct Lyrics {
  // True when timestamps were available (karaoke view possible).
  bool synced = false;
  std::vector<LyricLine> lines;  // sorted by t_ms; may include blank spacer lines
  std::string plain;             // unsynced fallback text
  std::string source;            // e.g. "LRCLIB"
  int providers_tried = 0;       // services attempted for this track
  std::string title, artist, album;
  // Edition of the matched recording ("live", "remix", "instrumental", ...).
  // Shown in the status line so a live sheet is never silently presented as
  // the studio version.
  std::string variant;
  bool empty() const { return lines.empty() && plain.empty(); }
};

// Parses an LRC document. Supports [mm:ss.xx]/[hh:mm:ss.xxx] stamps, multiple
// stamps per line, [offset:+/-ms], and preserves genuine inline Enhanced LRC
// segment timestamps. Blank-text timestamps are kept as instrumental gaps.
bool lrc_parse(const std::string& lrc, std::vector<LyricLine>& out);

// Turns a synced lyric list into plain text (used when a service only gives
// unsynced text, or for the "plain" fallback view).
std::string lyrics_to_plain(const std::vector<LyricLine>& lines);
// Serializes line and genuine Enhanced LRC segment timestamps for local cache.
std::string lyrics_to_lrc(const std::vector<LyricLine>& lines);

// ----------------------------------------------------------- track matching

// Which edition of a song a media session is reporting. Two sessions for the
// same title and artist are only interchangeable when the variant matches:
// a live recording, a remix and the studio take have different timings and
// sometimes different words, so serving one's lyric sheet for another is a
// wrong answer even though the title looks right.
enum class TrackVariant { Studio, Live, Remix, Instrumental, Karaoke };

struct TrackInfo {
  TrackInfo() = default;
  // Positional form used throughout the tests and the media path; `raw` is the
  // player's original title when it differs from the cleaned one.
  TrackInfo(std::string value_title, std::string value_artist = std::string(),
            std::string value_album = std::string(), double value_duration = 0.0,
            std::string raw = std::string())
      : title(std::move(value_title)),
        artist(std::move(value_artist)),
        album(std::move(value_album)),
        duration_s(value_duration),
        raw_title(std::move(raw)) {}

  std::string title;
  std::string artist;
  std::string album;
  double duration_s = 0.0;  // 0 when unknown
  // Title exactly as the player reported it, before clean_track() removed the
  // "(Live)", "(Remix)" and similar markers. Kept so the edition can still be
  // identified after cleaning; empty when the caller never had a raw title.
  std::string raw_title;

  // Edition of the song, read from the raw (un-cleaned) title when present.
  TrackVariant variant() const;
};

// Reads a variant out of a raw (uncleaned) title, e.g. "Numb (Live at Wembley)"
// or "Song - Sped Up". Studio is the default.
TrackVariant detect_variant(const std::string& raw_title);
const char* track_variant_name(TrackVariant variant);

// True when a lyric record may be used for a session of this variant. Checks
// happen on the provider's own title/album text, which is usually the only
// edition marker LRCLIB gives us.
bool variant_compatible(TrackVariant want, const std::string& candidate_title,
                        const std::string& candidate_album);

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
// `*timing_trusted` is false when the words are very likely right but the
// record is for a cut of a different length, so its timestamps must not drive
// the display (the caller shows it as untimed text instead).
const JValue* best_candidate(const JValue& search_results, const TrackInfo& want,
                             double* score = nullptr,
                             bool* timing_trusted = nullptr);

// True when two titles are the same words, ignoring case and punctuation.
bool same_title_text(const std::string& a, const std::string& b);

// False when a timed sheet cannot belong to a recording of `duration_s`: its
// last line starts after the track ends, or it stops in the first third.
// Always true for untimed text and unknown durations.
bool lyrics_fit_duration(const Lyrics& lyrics, double duration_s);

// Reads a lyrics payload: either an LRCLIB /api/get object or the best entry of
// a /api/search array. Returns false when no usable lyrics were found.
bool parse_lrclib_payload(const std::string& body, const TrackInfo& want, Lyrics& out);

// lyrics.ovh /api/v1/{artist}/{title}: {"lyrics":"..."}. Always untimed.
bool parse_lyrics_ovh_payload(const std::string& body, Lyrics& out);
// As above, but also rejects an answer whose own credit line names a different
// song than `want`, and strips that credit line from the text.
bool parse_lyrics_ovh_payload(const std::string& body, const TrackInfo& want,
                              Lyrics& out);

// Plain (untimed) lyrics are turned into evenly paced display rows for the
// fallback view. `lead_ms` delays the first row and `per_line_ms` paces them.
std::vector<LyricLine> pace_plain_lyrics(const std::string& plain, int64_t lead_ms,
                                         int64_t per_line_ms);

// Normalizes a hand-written LRC or plain-text lyric document loaded from the
// user's local override folder.
bool parse_local_lyrics(const std::string& text, Lyrics& out);

// ------------------------------------------------------------------- layout

// Measures the width of a piece of text at `px` height. Injected so the layout
// code is testable without a font engine.
using MeasureFn = std::function<double(const std::string& utf8, double px)>;

struct DispLine {
  DispLine() = default;
  DispLine(int64_t start, int64_t end, std::string value, bool continuation = false,
           std::vector<LyricSegment> timed_segments = {})
      : start_ms(start), end_ms(end), text(std::move(value)), cont(continuation),
        segments(std::move(timed_segments)) {}

  int64_t start_ms = 0;
  int64_t end_ms = 0;
  std::string text;      // may be empty (instrumental gap)
  bool cont = false;     // true for wrapped continuation of previous line
  // Genuine inline timing, with byte ranges relative to this display row.
  std::vector<LyricSegment> segments;
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
// Token containment in [0,1] (measures whether one string's tokens are contained in the other).
double token_containment(const std::string& a, const std::string& b);

}  // namespace ytl
