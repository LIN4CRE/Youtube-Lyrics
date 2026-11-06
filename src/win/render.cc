// Youtube-Lyrics - all GDI+ painting.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

using namespace Gdiplus;

namespace ytl {
namespace {

struct Theme {
  ARGB bg_top;
  ARGB bg_bottom;
  ARGB accent;
  ARGB text;
  const wchar_t* name;
};

// The first entry is the brand theme and the default: ink and warm paper with
// one amber accent, the same five values the logo and the README are drawn in.
// The rest are genuinely different moods, not hue-rotations of each other.
const Theme kThemes[] = {
    {0xFF0B0A0F, 0xFF17151D, 0xFFF5A524, 0xFFF3EFE6, L"Midnight"},
    {0xFF070D14, 0xFF0E2230, 0xFF5CC8D8, 0xFFEAF7F9, L"Aurora"},
    {0xFF120C08, 0xFF241509, 0xFFE07A3C, 0xFFFFF1E3, L"Ember"},
    {0xFF0B0B0B, 0xFF171717, 0xFFEDEAE3, 0xFFF5F2EC, L"Mono"},
};
const int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));

const Theme& theme(int index) {
  if (index < 0 || index >= kThemeCount) index = 0;
  return kThemes[index];
}

// --- fonts ----------------------------------------------------------------
// Segoe UI ships on every Windows 10/11 install; the Semibold/Light variants
// are separate families in GDI+, which gives us weight control without LOGFONT.
FontFamily* family_for(int weight) {
  static std::map<int, FontFamily*> cache;
  int key = (weight >= 600) ? 600 : (weight <= 300 ? 300 : 400);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  const wchar_t* names[3];
  if (key == 600) {
    names[0] = L"Segoe UI Semibold";
    names[1] = L"Segoe UI";
    names[2] = L"Tahoma";
  } else if (key == 300) {
    names[0] = L"Segoe UI Light";
    names[1] = L"Segoe UI";
    names[2] = L"Tahoma";
  } else {
    names[0] = L"Segoe UI";
    names[1] = L"Tahoma";
    names[2] = L"Arial";
  }
  FontFamily* chosen = nullptr;
  for (const wchar_t* n : names) {
    FontFamily* f = new FontFamily(n);
    if (f->IsAvailable()) {
      chosen = f;
      break;
    }
    delete f;
  }
  if (!chosen) chosen = new FontFamily(L"Arial");
  cache[key] = chosen;
  return chosen;
}

Font* make_font(int px, int weight) {
  if (px < 6) px = 6;
  return new Font(family_for(weight), static_cast<REAL>(px), FontStyleRegular,
                  UnitPixel);
}

// Surface used only for text measurement. Deliberately leaked: it must outlive
// GdiplusShutdown(), which happens before static destructors run.
Graphics& measure_graphics() {
  static Bitmap* bmp = new Bitmap(8, 8, PixelFormat32bppRGB);
  static Graphics* g = new Graphics(bmp);
  return *g;
}

std::wstring wide(const std::string& s) { return utf8_to_wide(s); }

double measure_text(Graphics& g, const std::wstring& s, Font* font) {
  if (s.empty()) return 0.0;
  StringFormat fmt;
  fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
  RectF box(0, 0, 100000.0f, 100000.0f);
  RectF bounds;
  g.MeasureString(s.c_str(), static_cast<INT>(s.size()), font, box, &fmt, &bounds);
  return bounds.Width;
}

void draw_text(Graphics& g, const std::wstring& s, Font* font, const RectF& box,
               ARGB color, StringAlignment align = StringAlignmentCenter) {
  if (s.empty()) return;
  SolidBrush brush{Color(color)};
  StringFormat fmt;
  fmt.SetAlignment(align);
  fmt.SetLineAlignment(StringAlignmentCenter);
  fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
  g.DrawString(s.c_str(), static_cast<INT>(s.size()), font, box, &fmt, &brush);
}

void draw_rounded_rect(Graphics& g, const RectF& r, REAL radius, ARGB fill,
                       bool border, ARGB border_color) {
  GraphicsPath path;
  REAL d = radius * 2;
  path.AddArc(r.X, r.Y, d, d, 180, 90);
  path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  path.CloseFigure();
  if ((fill & 0xFF000000) != 0) {
    SolidBrush b{Color(fill)};
    g.FillPath(&b, &path);
  }
  if (border) {
    Pen p(Color(border_color), 1.0f);
    g.DrawPath(&p, &path);
  }
}

void glow(Graphics& g, double cx, double cy, double rx, double ry, ARGB center) {
  GraphicsPath p;
  p.AddEllipse(RectF(static_cast<REAL>(cx - rx), static_cast<REAL>(cy - ry),
                     static_cast<REAL>(rx * 2), static_cast<REAL>(ry * 2)));
  PathGradientBrush b(&p);
  b.SetCenterColor(Color(center));
  Color surround(0, 0, 0, 0);
  int n = 1;
  b.SetSurroundColors(&surround, &n);
  g.FillPath(&b, &p);
}

// Catmull-ish vertical scrim used to keep chrome text legible over artwork.
void scrim(Graphics& g, const RectF& r, Color from, Color to) {
  LinearGradientBrush b(r, from, to, LinearGradientModeVertical);
  g.FillRectangle(&b, r);
}

}  // namespace

const wchar_t* theme_name(int index) { return theme(index).name; }

const wchar_t* wallpaper_name(int index) {
  static const wchar_t* names[kWallpaperCount] = {
      L"Rainfall District", L"Midnight Range", L"Stillwood"};
  if (index < 0 || index >= kWallpaperCount) index = 0;
  return names[index];
}

Gdiplus::Font* get_font(App& app, int px, int weight) {
  auto key = std::make_pair(px, weight >= 600 ? 600 : (weight <= 300 ? 300 : 400));
  auto it = app.view.fonts.find(key);
  if (it != app.view.fonts.end()) return it->second;
  Font* f = make_font(px, weight);
  app.view.fonts[key] = f;
  return f;
}

// ------------------------------------------------------------------ artwork

void refresh_art(App& app) {
  View& v = app.view;
  std::string want = app.cfg.cover ? app.active_media.art_path : std::string();
  if (want == v.art_path) return;
  if (v.art) {
    delete v.art;
    v.art = nullptr;
  }
  v.art_path = want;
  if (want.empty()) return;
  Image* img = Image::FromFile(wide(want).c_str(), FALSE);
  if (img && img->GetLastStatus() == Ok && img->GetWidth() > 8) {
    v.art = img;
  } else {
    delete img;
    logf("render: could not load cover art %s", want.c_str());
  }
}

void load_wallpaper(App& app, int index) {
  View& v = app.view;
  if (index < 0 || index >= kWallpaperCount) index = 0;
  if (v.wallpaper_loaded_index == index) return;

  if (v.wallpaper_image) {
    delete v.wallpaper_image;
    v.wallpaper_image = nullptr;
  }
  if (v.wallpaper_stream) {
    v.wallpaper_stream->Release();
    v.wallpaper_stream = nullptr;
  }
  v.wallpaper_loaded_index = index;

  const int resource_ids[kWallpaperCount] = {
      IDR_WALLPAPER_CITY, IDR_WALLPAPER_MOUNTAIN, IDR_WALLPAPER_FOREST};
  HRSRC resource = FindResourceW(app.inst, MAKEINTRESOURCEW(resource_ids[index]), RT_RCDATA);
  if (!resource) return;
  HGLOBAL loaded = LoadResource(app.inst, resource);
  const DWORD size = SizeofResource(app.inst, resource);
  const void* bytes = loaded ? LockResource(loaded) : nullptr;
  if (!bytes || size == 0 || size > (2u << 20)) return;

  IStream* stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return;
  ULONG written = 0;
  if (FAILED(stream->Write(bytes, size, &written)) || written != size) {
    stream->Release();
    return;
  }
  LARGE_INTEGER start{};
  if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
    stream->Release();
    return;
  }
  Image* image = Image::FromStream(stream, FALSE);
  if (!image || image->GetLastStatus() != Ok || image->GetWidth() < 8 ||
      image->GetHeight() < 8) {
    delete image;
    stream->Release();
    return;
  }
  v.wallpaper_stream = stream;
  v.wallpaper_image = image;
  v.wallpaper_loaded_index = index;
}

// --------------------------------------------------------------- background

void ensure_background(App& app, int w, int h) {
  View& v = app.view;
  if (w < 8 || h < 8) return;
  load_wallpaper(app, app.cfg.wallpaper_index);
  const std::string art_sig = (app.cfg.cover && v.art) ? v.art_path : std::string();
  if (v.bg && v.bg_w == w && v.bg_h == h && v.bg_theme == app.cfg.theme &&
      v.bg_wallpaper == app.cfg.wallpaper_index && v.bg_dim == app.cfg.wallpaper_dim &&
      v.bg_art == art_sig) {
    return;
  }

  // Cap the cached background to about two megapixels. The display back buffer
  // remains native-resolution for crisp lyrics, but the cinematic still does
  // not allocate another full 4K/8K bitmap in memory.
  const double cache_scale = std::min(
      1.0, std::min(1920.0 / static_cast<double>(w),
                    1080.0 / static_cast<double>(h)));
  const int bw = std::max(1, static_cast<int>(std::lround(w * cache_scale)));
  const int bh = std::max(1, static_cast<int>(std::lround(h * cache_scale)));
  Bitmap* bg = new Bitmap(bw, bh, PixelFormat32bppRGB);
  if (!bg || bg->GetLastStatus() != Ok) {
    delete bg;
    return;
  }
  Graphics g(bg);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
  g.SetPixelOffsetMode(PixelOffsetModeHalf);
  const Theme& th = theme(app.cfg.theme);
  RectF full(0, 0, static_cast<REAL>(bw), static_cast<REAL>(bh));

  if (v.wallpaper_image) {
    const double iw = static_cast<double>(v.wallpaper_image->GetWidth());
    const double ih = static_cast<double>(v.wallpaper_image->GetHeight());
    const double scale = std::max(bw / iw, bh / ih);
    const double dw = iw * scale, dh = ih * scale;
    g.DrawImage(v.wallpaper_image,
                RectF(static_cast<REAL>((bw - dw) / 2),
                      static_cast<REAL>((bh - dh) / 2), static_cast<REAL>(dw),
                      static_cast<REAL>(dh)));
  } else {
    LinearGradientBrush base(full, Color(th.bg_top), Color(th.bg_bottom),
                             LinearGradientModeVertical);
    g.FillRectangle(&base, full);
  }

  if (app.cfg.cover && v.art) {
    // Cover art is an optional, low-opacity tint; the selected cinematic image
    // remains the permanent visual foundation rather than being replaced.
    const double iw = static_cast<double>(v.art->GetWidth());
    const double ih = static_cast<double>(v.art->GetHeight());
    const double scale = std::max(bw / iw, bh / ih);
    const double dw = iw * scale, dh = ih * scale;
    ColorMatrix matrix{};
    matrix.m[0][0] = 1.0f;
    matrix.m[1][1] = 1.0f;
    matrix.m[2][2] = 1.0f;
    matrix.m[3][3] = 0.16f;
    matrix.m[4][4] = 1.0f;
    ImageAttributes attributes;
    attributes.SetColorMatrix(&matrix, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
    g.DrawImage(v.art,
                RectF(static_cast<REAL>((bw - dw) / 2),
                      static_cast<REAL>((bh - dh) / 2), static_cast<REAL>(dw),
                      static_cast<REAL>(dh)),
                0, 0, static_cast<REAL>(iw), static_cast<REAL>(ih), UnitPixel,
                &attributes);
  }

  const int dim = std::max(0, std::min(60, app.cfg.wallpaper_dim));
  if (dim > 0) {
    SolidBrush dark(Color(static_cast<BYTE>(dim * 255 / 100), 0, 0, 0));
    g.FillRectangle(&dark, full);
  }

  // A restrained theme-coloured haze unifies the chosen art with the lyric
  // palette without turning it into a generic gradient wallpaper.
  const ARGB accent = th.accent;
  const int ar = (accent >> 16) & 0xFF, ag = (accent >> 8) & 0xFF,
            ab = accent & 0xFF;
  glow(g, bw * 0.18, bh * 0.16, bh * 0.42, bh * 0.32,
       (0x1A << 24) | (ar << 16) | (ag << 8) | ab);
  glow(g, bw * 0.86, bh * 0.88, bh * 0.36, bh * 0.30,
       (0x16 << 24) | (ar << 16) | (ag << 8) | ab);

  // Soft edge vignette and readability scrims, cached once per wallpaper/settings change.
  GraphicsPath vig_path;
  vig_path.AddEllipse(RectF(-static_cast<REAL>(bw) * 0.18f,
                            -static_cast<REAL>(bh) * 0.28f,
                            static_cast<REAL>(bw) * 1.36f,
                            static_cast<REAL>(bh) * 1.56f));
  PathGradientBrush vign(&vig_path);
  vign.SetCenterColor(Color(0, 0, 0, 0));
  Color surround(112, 0, 0, 0);
  int one = 1;
  vign.SetSurroundColors(&surround, &one);
  g.FillPath(&vign, &vig_path);
  scrim(g, RectF(0, 0, static_cast<REAL>(bw), bh * 0.15f), Color(82, 0, 0, 0),
        Color(0, 0, 0, 0));
  scrim(g, RectF(0, bh * 0.84f, static_cast<REAL>(bw), bh * 0.16f),
        Color(0, 0, 0, 0), Color(112, 0, 0, 0));

  if (v.bg) delete v.bg;
  v.bg = bg;
  v.bg_w = w;
  v.bg_h = h;
  v.bg_theme = app.cfg.theme;
  v.bg_wallpaper = app.cfg.wallpaper_index;
  v.bg_dim = app.cfg.wallpaper_dim;
  v.bg_art = art_sig;
}

// ------------------------------------------------------------------- layout

void relayout(App& app, int w, int h) {
  View& v = app.view;
  v.line_top.clear();
  v.line_h.clear();
  v.total_h = 0;
  v.disp_rows = 0;
  v.lay_w = w;
  v.lay_h = h;

  if (w < 40 || h < 40) return;
  if (app.lyrics.empty()) {
    app.disp.clear();
    return;
  }

  Graphics& mg = measure_graphics();
  const double col_w = std::min(w * 0.80, 1400.0);
  const double active_scale = 1.22;
  const double max_text_w = col_w / active_scale;

  const std::vector<LyricLine>& src =
      app.lyrics.synced ? app.lyrics.lines : app.pending_plain_lines;
  if (src.empty()) return;

  double base = std::min(std::max(h * 0.042, 16.0), 88.0) * app.cfg.font_scale;
  if (!app.lyrics.synced) base = std::min(base, h * 0.035);

  auto measure_at = [&](const std::string& text, double px) {
    Font* f = get_font(app, static_cast<int>(std::lround(px)), 400);
    return measure_text(mg, wide(text), f);
  };

  // Shrink to fit: wide lines first, then (for unsynced sheets) total height.
  // Wrapping is only used when shrinking would make the text unreadably small.
  for (int attempt = 0; attempt < 30; ++attempt) {
    double widest = 0;
    for (const auto& l : src) {
      if (l.text.empty()) continue;
      widest = std::max(widest, measure_at(l.text, base));
    }
    bool too_wide = widest > max_text_w;
    bool too_tall = false;
    double est_total = base * 1.5 * static_cast<double>(src.size());
    if (!app.lyrics.synced && est_total > h * 0.88) too_tall = true;

    if ((too_wide || too_tall) && base > 13.0) {
      double factor = 0.95;
      if (too_wide && widest > 0) factor = std::min(factor, max_text_w / widest);
      if (too_tall && est_total > 0) factor = std::min(factor, (h * 0.88) / est_total);
      base = std::max(13.0, base * std::max(0.82, factor));
      continue;
    }
    break;
  }

  auto measure_fn = [&](const std::string& text, double px) {
    return measure_at(text, px) + 6.0;
  };
  app.disp = build_display_lines(src, max_text_w, base, measure_fn);
  if (app.disp.empty()) return;

  v.base_px = base;
  v.act_px = base * active_scale;
  v.col_w = col_w;
  v.lay_scale = app.cfg.font_scale;

  LineMetrics metrics = compute_line_metrics(app.disp, base * 1.5, 3000, base * 0.55);
  v.line_top = metrics.top;
  v.line_h = metrics.height;
  v.total_h = metrics.total;
  v.disp_rows = static_cast<int>(app.disp.size());
  app.active = (app.lyrics.synced || app.timing_estimated)
                   ? active_index(app.disp, app.current_pos_ms())
                   : -1;
  v.last_active = app.active;
}

double scroll_target(const App& app, int h) {
  const View& v = app.view;
  LineMetrics m;
  m.top = v.line_top;
  m.height = v.line_h;
  m.total = v.total_h;
  return compute_scroll_target(m, app.active, h);
}

// ------------------------------------------------------------------ painting

namespace {

double timed_segment_fill_width(Graphics& g, const DispLine& line,
                                const std::string& utf8, Font* font,
                                int64_t position_ms) {
  double filled = 0.0;
  size_t cursor = 0;
  for (size_t i = 0; i < line.segments.size(); ++i) {
    const LyricSegment& segment = line.segments[i];
    const size_t begin = std::max(cursor, std::min(segment.begin, utf8.size()));
    const size_t end = std::min(segment.end, utf8.size());
    if (end <= begin) continue;
    if (position_ms < segment.t_ms) return filled;

    if (begin > cursor) {
      const std::wstring gap = wide(utf8.substr(cursor, begin - cursor));
      filled += measure_text(g, gap, font);
    }
    const std::wstring text = wide(utf8.substr(begin, end - begin));
    const double segment_width = measure_text(g, text, font);
    int64_t segment_end_ms = line.end_ms;
    if (i + 1 < line.segments.size())
      segment_end_ms = line.segments[i + 1].t_ms;
    if (segment_end_ms < segment.t_ms) segment_end_ms = segment.t_ms;

    if (segment_end_ms > segment.t_ms && position_ms < segment_end_ms) {
      const double progress = std::max(
          0.0, std::min(1.0, static_cast<double>(position_ms - segment.t_ms) /
                                static_cast<double>(segment_end_ms - segment.t_ms)));
      filled += segment_width * progress;
      return filled;
    }
    filled += segment_width;
    cursor = end;
  }
  if (cursor < utf8.size() && !line.segments.empty() &&
      position_ms >= line.segments.back().t_ms) {
    filled += measure_text(g, wide(utf8.substr(cursor)), font);
  }
  return std::max(0.0, filled);
}

void paint_lyrics(App& app, Graphics& g, int w, int h) {
  View& v = app.view;
  const Theme& th = theme(app.cfg.theme);
  if (app.disp.empty() || v.line_top.empty()) return;

  const double col_x = (w - v.col_w) * 0.5;
  const int n = static_cast<int>(app.disp.size());
  const int a = app.active;
  const int64_t pos = app.current_pos_ms();
  const double progress = (a >= 0) ? line_progress(app.disp, a, pos) : 0.0;

  int first = 0;
  if (a >= 0) first = std::max(0, a - 16);
  // Skip lines that are far above the viewport.
  while (first < n && v.line_top[first] + v.scroll + v.line_h[first] < -240) ++first;

  for (int i = first; i < n; ++i) {
    const double y = v.line_top[i] + v.scroll;
    if (y > h + 160) break;
    if (y + v.line_h[i] < -160) continue;
    const bool has_display_timing = app.lyrics.synced || app.timing_estimated;
    const bool is_active = has_display_timing && (i == a);
    const bool is_gap = app.disp[i].text.empty();
    if (is_gap) continue;

    const int dist = (a >= 0) ? std::abs(i - a) : 99;
    int alpha = is_active ? 255 : static_cast<int>(std::max(52.0, 208.0 - dist * 30.0));
    if (!has_display_timing) alpha = 225;

    const int px = static_cast<int>(std::lround(is_active ? v.act_px : v.base_px));
    Font* font = get_font(app, px, is_active ? 600 : 400);
    std::wstring text = wide(app.disp[i].text);

    RectF box(static_cast<REAL>(col_x), static_cast<REAL>(y),
              static_cast<REAL>(v.col_w), static_cast<REAL>(v.line_h[i]));

    if (is_active) {
      // Soft spotlight behind the line being sung.
      RectF band(box.X, box.Y + box.Height * 0.08f, box.Width, box.Height * 0.84f);
      GraphicsPath p;
      REAL r = band.Height * 0.5f;
      p.AddArc(band.X, band.Y, r, r, 90, 180);
      p.AddArc(band.X + band.Width - r, band.Y, r, r, 270, 180);
      p.CloseFigure();
      int ar = (th.accent >> 16) & 0xFF, ag = (th.accent >> 8) & 0xFF,
          ab = th.accent & 0xFF;
      PathGradientBrush halo(&p);
      halo.SetCenterColor(Color(26, ar, ag, ab));
      Color edge(0, ar, ag, ab);
      int one = 1;
      halo.SetSurroundColors(&edge, &one);
      g.FillPath(&halo, &p);
    }

    ARGB base_color = (th.text & 0x00FFFFFF) | (static_cast<ARGB>(alpha) << 24);

    // Only real source timestamps drive a wipe. Enhanced LRC segments use
    // their own markers; ordinary LRC uses a continuous line-level wipe.
    if (is_active && app.lyrics.synced && app.cfg.wipe) {
      const double full_w = measure_text(g, text, font);
      const double filled_w = app.disp[i].segments.empty()
                                  ? full_w * progress
                                  : timed_segment_fill_width(
                                        g, app.disp[i], app.disp[i].text, font, pos);
      if (full_w > 0.0 && filled_w < full_w - 0.5) {
        ARGB dim = (th.text & 0x00FFFFFF) | (0x66 << 24);
        draw_text(g, text, font, box, dim);
        const double left = col_x + (v.col_w - full_w) * 0.5;
        GraphicsState st = g.Save();
        g.SetClip(RectF(static_cast<REAL>(left), box.Y - 4,
                        static_cast<REAL>(std::max(0.0, filled_w)),
                        box.Height + 8));
        draw_text(g, text, font, box, base_color);
        g.Restore(st);
      } else {
        draw_text(g, text, font, box, base_color);
      }
    } else {
      draw_text(g, text, font, box, base_color);
    }
  }
}

void paint_plain(App& app, Graphics& g, int w, int h) {
  // Unsynced lyrics: a calm, centred sheet.
  View& v = app.view;
  const Theme& th = theme(app.cfg.theme);
  const double col_x = (w - v.col_w) * 0.5;
  for (size_t i = 0; i < app.disp.size() && i < v.line_top.size(); ++i) {
    double y = v.line_top[i] + v.scroll;
    if (y > h + 100) break;
    if (y + v.line_h[i] < -100) continue;
    if (app.disp[i].text.empty()) continue;
    Font* font = get_font(app, static_cast<int>(std::lround(v.base_px)), 400);
    ARGB c = (th.text & 0x00FFFFFF) | (0xE0 << 24);
    draw_text(g, wide(app.disp[i].text), font,
              RectF(static_cast<REAL>(col_x), static_cast<REAL>(y),
                    static_cast<REAL>(v.col_w), static_cast<REAL>(v.line_h[i])),
              c);
  }
}

void paint_centred_message(App& app, Graphics& g, int w, int h,
                           const std::string& big, const std::string& sub,
                           const std::string& line1, const std::string& line2,
                           const std::string& line3) {
  const Theme& th = theme(app.cfg.theme);
  double title_px = std::min(std::max(h * 0.052, 22.0), 64.0);
  double body_px = std::min(std::max(h * 0.020, 13.0), 24.0);

  double centre = h * 0.44;
  draw_text(g, wide(big), get_font(app, static_cast<int>(title_px), 600),
            RectF(0, static_cast<REAL>(centre - title_px * 1.6),
                  static_cast<REAL>(w), static_cast<REAL>(title_px * 2)),
            (th.text & 0x00FFFFFF) | (0xF0 << 24));

  if (!sub.empty()) {
    draw_text(g, wide(sub), get_font(app, static_cast<int>(body_px * 1.15), 400),
              RectF(0, static_cast<REAL>(centre + title_px * 0.5),
                    static_cast<REAL>(w), static_cast<REAL>(body_px * 2.2)),
              (th.text & 0x00FFFFFF) | (0xAA << 24));
  }

  double y = centre + title_px * 1.5;
  const std::string* hints[3] = {&line1, &line2, &line3};
  for (const std::string* hint : hints) {
    if (hint->empty()) continue;
    draw_text(g, wide(*hint), get_font(app, static_cast<int>(body_px), 400),
              RectF(0, static_cast<REAL>(y), static_cast<REAL>(w),
                    static_cast<REAL>(body_px * 2.4)),
              (th.text & 0x00FFFFFF) | (0x8C << 24));
    y += body_px * 2.1;
  }
}

void paint_chrome(App& app, Graphics& g, int w, int h) {
  const Theme& th = theme(app.cfg.theme);

  const TrackInfo track = app.current_track();
  const double duration_s = app.current_duration_s();

  // Header: the committed track, never the last unconfirmed SMTC candidate.
  if (!track.title.empty()) {
    double px = std::min(std::max(h * 0.0175, 12.0), 26.0);
    if (!track.artist.empty()) {
      draw_text(g, wide(track.artist),
                get_font(app, static_cast<int>(px * 0.86), 400),
                RectF(0, static_cast<REAL>(h * 0.028), static_cast<REAL>(w),
                      static_cast<REAL>(px * 2)),
                (th.text & 0x00FFFFFF) | (0x99 << 24));
    }
    draw_text(g, wide(track.title),
              get_font(app, static_cast<int>(px * 1.15), 600),
              RectF(0, static_cast<REAL>(h * 0.028 + px * 1.5),
                    static_cast<REAL>(w), static_cast<REAL>(px * 2)),
              (th.text & 0x00FFFFFF) | (0xE0 << 24));
  }

  // Progress bar.
  if (duration_s > 0) {
    int64_t dur = static_cast<int64_t>(duration_s * 1000.0);
    int64_t pos = app.current_pos_ms();
    double p = dur > 0 ? static_cast<double>(pos) / static_cast<double>(dur) : 0.0;
    p = std::min(1.0, std::max(0.0, p));

    RectF track(0, static_cast<REAL>(h - 4), static_cast<REAL>(w), 3.0f);
    SolidBrush bg(Color(0x2E, 0xFF, 0xFF, 0xFF));
    g.FillRectangle(&bg, track);
    if (p > 0) {
      int ar = (th.accent >> 16) & 0xFF, ag = (th.accent >> 8) & 0xFF,
          ab = th.accent & 0xFF;
      LinearGradientBrush fill(RectF(0, static_cast<REAL>(h - 4),
                                     static_cast<REAL>(w), 3.0f),
                               Color(0xFF, ar, ag, ab), Color(0xD0, ar, ag, ab),
                               LinearGradientModeHorizontal);
      g.FillRectangle(&fill, RectF(0, static_cast<REAL>(h - 4),
                                   static_cast<REAL>(w * p), 3.0f));
      // dot
      SolidBrush dot(Color(0xFF, 255, 255, 255));
      g.FillEllipse(&dot, RectF(static_cast<REAL>(w * p - 4),
                                static_cast<REAL>(h - 7), 7.0f, 7.0f));
    }

    // Times.
    double px = std::min(std::max(h * 0.015, 11.0), 20.0);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s / %s", format_time(pos).c_str(),
                  format_time(dur).c_str());
    Font* f = get_font(app, static_cast<int>(px), 400);
    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    right.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tb(Color(0x90, 255, 255, 255));
    g.DrawString(wide(buf).c_str(), -1, f,
                 RectF(0, static_cast<REAL>(h - 34), static_cast<REAL>(w - 18),
                       static_cast<REAL>(px * 2)),
                 &right, &tb);
  }

  // Source app + sync offset, bottom left.
  double px = std::min(std::max(h * 0.014, 10.0), 18.0);
  std::string status_line;
  if (!app.active_media.app.empty()) {
    std::string app_name = app.active_media.app;
    size_t bang = app_name.find('!');
    if (bang != std::string::npos) app_name = app_name.substr(bang + 1);
    status_line = "via " + app_name;
  }
  if (app.lstate == LyricsState::Ready && !app.lyrics.source.empty()) {
    if (!status_line.empty()) status_line += "  \xC2\xB7  ";
    status_line += "lyrics: " + app.lyrics.source;
    if (app.timing_estimated) {
      status_line += "  \xC2\xB7  estimated scroll";
    } else if (app.lyrics.synced) {
      const bool enhanced = std::any_of(
          app.lyrics.lines.begin(), app.lyrics.lines.end(),
          [](const LyricLine& line) { return !line.segments.empty(); });
      status_line += enhanced ? "  \xC2\xB7  Enhanced LRC"
                              : "  \xC2\xB7  line-synced";
    } else {
      status_line += "  \xC2\xB7  untimed";
    }
  }
  if (app.cfg.offset_ms != 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%ssync %+.2fs",
                  status_line.empty() ? "" : "  \xC2\xB7  ",
                  app.cfg.offset_ms / 1000.0);
    status_line += buf;
  }
  if (!status_line.empty()) {
    StringFormat left;
    left.SetAlignment(StringAlignmentNear);
    left.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tb(Color(0x66, 255, 255, 255));
    g.DrawString(wide(status_line).c_str(), -1,
                 get_font(app, static_cast<int>(px), 400),
                 RectF(18, static_cast<REAL>(h - 34),
                       static_cast<REAL>(w * 0.5), static_cast<REAL>(px * 2)),
                 &left, &tb);
  }
}

void paint_toast(App& app, Graphics& g, int w, int h) {
  View& v = app.view;
  if (v.toast.empty()) return;
  int64_t now = app.now_ms();
  if (now > v.toast_until) {
    v.toast.clear();
    return;
  }
  const Theme& th = theme(app.cfg.theme);
  double px = std::min(std::max(h * 0.019, 12.0), 22.0);
  Font* f = get_font(app, static_cast<int>(px), 400);
  std::wstring text = wide(v.toast);
  double tw = measure_text(g, text, f);
  double bw = tw + px * 2.4;
  double bh = px * 2.6;
  RectF box(static_cast<REAL>((w - bw) / 2), static_cast<REAL>(h - bh - 56),
            static_cast<REAL>(bw), static_cast<REAL>(bh));
  int ar = (th.accent >> 16) & 0xFF, ag = (th.accent >> 8) & 0xFF,
      ab = th.accent & 0xFF;
  draw_rounded_rect(g, box, bh * 0.5f, 0xC8141420, true,
                    (0x59 << 24) | (ar << 16) | (ag << 8) | ab);
  draw_text(g, text, f, box, 0xFFF0F0F5);
}


void paint_hint(App& app, Graphics& g, int w, int h) {
  View& v = app.view;
  if (v.hint_until == 0) return;
  int64_t now = app.now_ms();
  if (now > v.hint_until) return;
  int64_t left = v.hint_until - now;
  double fade = left < 1200 ? left / 1200.0 : 1.0;
  const Theme& th = theme(app.cfg.theme);
  const std::wstring text = app.cfg.wallpaper && !app.windowed
      ? L"Ctrl+Alt+W  wallpaper mode   \xC2\xB7   Ctrl+Alt+L  hide or show"
      : L"F11  full screen   \xC2\xB7   F1  shortcuts   \xC2\xB7   Ctrl+Alt+L  hide";
  double px = std::min(std::max(h * 0.017, 11.0), 20.0);
  ARGB c = (th.text & 0x00FFFFFF) | (static_cast<ARGB>(std::min(150.0, 150.0 * fade)) << 24);
  draw_text(g, text, get_font(app, static_cast<int>(px), 400),
            RectF(0, static_cast<REAL>(h - px * 4.5), static_cast<REAL>(w),
                  static_cast<REAL>(px * 2.2)),
            c);
}

void paint_help(App& app, Graphics& g, int w, int h) {
  View& v = app.view;
  if (!v.help) return;

  SolidBrush dim(Color(0xC4, 4, 4, 8));
  g.FillRectangle(&dim, RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)));

  struct Row {
    const wchar_t* key;
    const wchar_t* what;
  };
  const Row rows[] = {
      {L"F11", L"Full screen / windowed"},
      {L"Esc", L"Leave full screen, then hide to tray"},
      {L"A", L"Always on top"},
      {L"↑ / ↓", L"Bigger / smaller text"},
      {L"← / →", L"Nudge lyric timing by 0.25 s"},
      {L"R", L"Look the lyrics up again"},
      {L"C", L"Subtle cover-art tint on / off"},
      {L"B", L"Next cinematic wallpaper"},
      {L"K", L"Karaoke wipe on / off"},
      {L"T", L"Theme"},
      {L"Ctrl+Alt+W", L"Toggle desktop wallpaper mode"},
      {L"F1  /  H", L"Show / hide this help"},
      {L"Ctrl+Alt+L", L"Show / hide from any app"},
      {L"Q", L"Quit"},
  };
  const int row_count = static_cast<int>(sizeof(rows) / sizeof(rows[0]));

  double panel_w = std::min(w * 0.76, 760.0);
  double key_px = std::min(std::max(h * 0.019, 12.0), 22.0);
  double row_h = key_px * 2.35;
  double panel_h = row_h * (row_count + 2.6);
  RectF panel(static_cast<REAL>((w - panel_w) / 2),
              static_cast<REAL>((h - panel_h) / 2), static_cast<REAL>(panel_w),
              static_cast<REAL>(panel_h));
  draw_rounded_rect(g, panel, 18.0f, 0xEE0E0E18, true, 0x33FFFFFF);

  draw_text(g, L"Youtube-Lyrics", get_font(app, static_cast<int>(key_px * 1.6), 600),
            RectF(panel.X, panel.Y + static_cast<REAL>(row_h * 0.4),
                  panel.Width, static_cast<REAL>(row_h)),
            0xFFF2F2F8);

  StringFormat right;
  right.SetAlignment(StringAlignmentFar);
  right.SetLineAlignment(StringAlignmentCenter);
  StringFormat left;
  left.SetAlignment(StringAlignmentNear);
  left.SetLineAlignment(StringAlignmentCenter);

  double y = panel.Y + row_h * 1.7;
  for (int i = 0; i < row_count; ++i) {
    Font* kf = get_font(app, static_cast<int>(key_px), 600);
    Font* wf = get_font(app, static_cast<int>(key_px), 400);
    SolidBrush kb{Color(0xFFE8E8F2)};
    g.DrawString(rows[i].key, -1, kf,
                 RectF(panel.X + 30, static_cast<REAL>(y),
                       static_cast<REAL>(panel.Width * 0.32), static_cast<REAL>(row_h)),
                 &right, &kb);
    SolidBrush wb{Color(0x9EF0F0F8)};
    g.DrawString(rows[i].what, -1, wf,
                 RectF(panel.X + panel.Width * 0.36f, static_cast<REAL>(y),
                       static_cast<REAL>(panel.Width * 0.6), static_cast<REAL>(row_h)),
                 &left, &wb);
    y += row_h;
  }

  // Diagnostics line: what the media helper last reported.
  std::string raw;
  {
    std::lock_guard<std::mutex> lock(app.mu);
    raw = app.media.raw;
  }
  Font* mono = get_font(app, static_cast<int>(key_px * 0.72), 400);
  std::string diag = "media helper: ";
  diag += raw.empty() ? std::string("(waiting)") : raw;
  if (diag.size() > 220) diag = diag.substr(0, 220) + "...";
  SolidBrush db{Color(0x66C8C8D8)};
  g.DrawString(wide(diag).c_str(), -1, mono,
               RectF(panel.X + 30, static_cast<REAL>(panel.Y + panel.Height - row_h * 1.5),
                     panel.Width - 60, static_cast<REAL>(row_h * 1.4)),
               &left, &db);
}

}  // namespace


// The resting wallpaper. This is what is on screen most of the time when the app
// lives on the desktop, so it is designed rather than a placeholder: the mark
// from the icon, a slow amber pulse that means "listening", and one quiet line
// telling you what it is waiting for.
void paint_idle_wallpaper(App& app, Graphics& g, int w, int h, bool listening,
                          bool listener_silent) {
  const Theme& th = theme(app.cfg.theme);
  const int ar = (th.accent >> 16) & 0xFF, ag = (th.accent >> 8) & 0xFF,
            ab = th.accent & 0xFF;

  const double unit = std::min(std::max(h * 0.10, 56.0), 190.0);
  const double cx = w * 0.5;
  const double cy = h * 0.44;

  // Listening pulse: a ring breathes while the media helper is alive.
  if (!listener_silent) {
    const double t = static_cast<double>(app.now_ms() % 2000) / 2000.0;
    const double r = unit * (0.30 + t * 0.22);
    Pen ring(Color(static_cast<INT>((1.0 - t) * 46), ar, ag, ab),
             static_cast<REAL>(std::max(1.0, h * 0.0016)));
    g.DrawEllipse(&ring, RectF(static_cast<REAL>(cx - r), static_cast<REAL>(cy - r),
                               static_cast<REAL>(r * 2), static_cast<REAL>(r * 2)));
  }

  // The mark, drawn at display scale: paper tile, margin rule and lyric bars.
  {
    const double tile_size = unit * 0.76;
    const RectF tile(static_cast<REAL>(cx - tile_size * 0.5),
                     static_cast<REAL>(cy - tile_size * 0.5),
                     static_cast<REAL>(tile_size), static_cast<REAL>(tile_size));
    draw_rounded_rect(g, tile, static_cast<REAL>(tile_size * 0.235),
                      0xFFF3EFE6, true, 0x280B0A0F);
    const double mw = unit * 0.62;
    const double left = cx - mw * 0.5;
    const double top = cy - mw * 0.62;
    SolidBrush lit(Color(0xFF, ar, ag, ab));
    SolidBrush dim(Color(0x8C, 243, 239, 230));

    // margin rule
    SolidBrush rule(Color(0x46, 243, 239, 230));
    g.FillRectangle(&rule, RectF(static_cast<REAL>(left), static_cast<REAL>(top),
                                 static_cast<REAL>(mw * 0.012),
                                 static_cast<REAL>(mw * 0.74)));

    const double x0 = left + mw * 0.10;
    const double thin = mw * 0.052, thick = mw * 0.085;
    auto bar = [&](double y, double span, double height, Brush& b) {
      RectF r(static_cast<REAL>(x0), static_cast<REAL>(y),
              static_cast<REAL>(span), static_cast<REAL>(height));
      g.FillRectangle(&b, r);
    };
    bar(top + mw * 0.10, mw * 0.52, thin, dim);
    bar(top + mw * 0.33, mw * 0.82, thick, lit);
    bar(top + mw * 0.60, mw * 0.38, thin, dim);
  }

  const double tpx = std::min(std::max(h * 0.030, 18.0), 40.0);
  const double bpx = std::min(std::max(h * 0.0155, 12.0), 20.0);

  const std::string big = listening ? "Paused" : "Listening";
  const std::string sub = listening
                              ? track_label(app.current_track())
                              : "Play a song anywhere \u2014 YouTube Music, Spotify, any player.";
  draw_text(g, wide(big), get_font(app, static_cast<int>(tpx), 600),
            RectF(0, static_cast<REAL>(cy + unit * 0.62), static_cast<REAL>(w),
                  static_cast<REAL>(tpx * 2)),
            (0xD8u << 24) | 0x00F3EFE6);
  draw_text(g, wide(sub), get_font(app, static_cast<int>(bpx), 400),
            RectF(0, static_cast<REAL>(cy + unit * 0.62 + tpx * 1.5),
                  static_cast<REAL>(w), static_cast<REAL>(bpx * 3)),
            (0x84u << 24) | 0x00F3EFE6);

  if (listener_silent) {
    draw_text(g, L"No media session yet · check the tray menu or ytlyrics.log",
              get_font(app, static_cast<int>(bpx * 0.92), 400),
              RectF(0, static_cast<REAL>(cy + unit * 0.62 + tpx * 1.5 + bpx * 2.1),
                    static_cast<REAL>(w), static_cast<REAL>(bpx * 3)),
              (0x55u << 24) | 0x00F3EFE6);
  }
}

void render_frame(App& app, Graphics& g, int w, int h) {
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  g.SetPixelOffsetMode(PixelOffsetModeHalf);

  ensure_background(app, w, h);
  if (app.view.bg) {
    g.DrawImage(app.view.bg,
                RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)));
  } else {
    SolidBrush black(Color(0xFF101018));
    g.FillRectangle(&black, RectF(0, 0, static_cast<REAL>(w),
                                  static_cast<REAL>(h)));
  }

  const bool desktop_wallpaper = app.cfg.wallpaper && !app.windowed;
  const bool has_current_session = app.session_present &&
                                   !app.active_track.title.empty();
  if (desktop_wallpaper && !has_current_session) {
    // In wallpaper mode the still image is the entire idle experience. No
    // listening indicator, waiting text, stale lyric, or old track chrome.
  } else {
    switch (app.lstate) {
      case LyricsState::Ready:
        if (app.lyrics.synced || app.timing_estimated) {
          paint_lyrics(app, g, w, h);
        } else {
          paint_plain(app, g, w, h);
        }
        break;
      case LyricsState::Loading:
        paint_centred_message(app, g, w, h, "Looking for lyrics…",
                              track_label(app.current_track()), "", "", "");
        break;
      case LyricsState::NotFound:
        if (!desktop_wallpaper)
          paint_centred_message(app, g, w, h, "No lyrics found",
                                track_label(app.current_track()), app.lmessage,
                                "Press R to search again", "F1 for shortcuts");
        break;
      case LyricsState::Error:
        if (!desktop_wallpaper)
          paint_centred_message(app, g, w, h, "Lyrics lookup failed",
                                track_label(app.current_track()), app.lmessage,
                                "Press R to retry", "F1 for shortcuts");
        break;
      case LyricsState::Idle:
      default:
        if (!desktop_wallpaper) {
          const bool listening = !app.current_track().title.empty();
          const bool silent = !app.helper_heard && app.started_at > 0 &&
                              app.now_ms() - app.started_at > 12000;
          paint_idle_wallpaper(app, g, w, h, listening, silent);
        }
        break;
    }
  }

  if (!desktop_wallpaper || has_current_session) paint_chrome(app, g, w, h);
  if (!desktop_wallpaper) paint_hint(app, g, w, h);
  paint_toast(app, g, w, h);
  paint_help(app, g, w, h);
}

}  // namespace ytl
