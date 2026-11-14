// Youtube-Lyrics - all GDI+ painting.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// How much larger the line being sung is drawn than the lines around it.
constexpr double kActiveScale = 1.14;

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

// Lyric text with a contact shadow, so it stays readable over a bright part of
// a photograph. `outline` is for the colour-keyed fallback layer, where the
// text sits directly on the user's own wallpaper with no dimming at all and a
// soft shadow cannot exist (the window has no per-pixel alpha): it gets a
// solid dark edge on all four diagonals instead.
void draw_text_backing(Graphics& g, const std::wstring& s, Font* font,
                       const RectF& box, int alpha, bool outline) {
  if (s.empty() || alpha <= 0) return;
  const REAL d = std::max(1.0f, font->GetSize() * 0.045f);
  if (outline) {
    const ARGB edge = 0xFF000000u;
    const REAL offsets[4][2] = {{-d, -d}, {d, -d}, {-d, d}, {d, d}};
    for (const auto& o : offsets) {
      RectF moved(box.X + o[0], box.Y + o[1], box.Width, box.Height);
      draw_text(g, s, font, moved, edge);
    }
    return;
  }
  const int shadow_alpha = std::min(190, alpha * 3 / 4);
  RectF moved(box.X, box.Y + d * 1.4f, box.Width, box.Height);
  draw_text(g, s, font, moved, static_cast<ARGB>(shadow_alpha) << 24);
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
      L"Rainfall District", L"Midnight Range", L"Stillwood", L"Drift"};
  if (index < 0 || index >= kWallpaperCount) index = 0;
  return names[index];
}

// Resizing a window asks for a new set of font sizes at every step. Dropped
// only between frames, never while a caller may still hold a pointer.
void trim_font_cache(App& app) {
  if (app.view.fonts.size() <= 80) return;
  for (auto& entry : app.view.fonts) delete entry.second;
  app.view.fonts.clear();
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

  // The last entry is procedural: no resource, nothing to decode, nothing to
  // free. ensure_background() paints it when no image is loaded.
  const int resource_ids[kWallpaperCount] = {
      IDR_WALLPAPER_CITY, IDR_WALLPAPER_MOUNTAIN, IDR_WALLPAPER_FOREST, 0};
  if (resource_ids[index] == 0) return;
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

// "Drift": a procedural scene, drawn once into the background cache and then
// reused like an embedded still. It is a slow aurora over a still horizon with
// a quiet star field — drawn with GDI+ gradients only, so it costs no bytes in
// the executable and scales to any resolution without resampling artefacts.
void paint_procedural_background(Graphics& g, int bw, int bh, const Theme& th) {
  const double w = bw, h = bh;
  RectF full(0, 0, static_cast<REAL>(w), static_cast<REAL>(h));
  LinearGradientBrush sky(full, Color(0xFF0A0E18), Color(0xFF141626),
                          LinearGradientModeVertical);
  g.FillRectangle(&sky, full);

  // Star field on a fixed lattice with hashed offsets: deterministic, so the
  // background looks identical every time it is rebuilt.
  {
    SolidBrush star(Color(0xFFFFFFFF));
    for (int i = 0; i < 190; ++i) {
      const uint32_t hash = static_cast<uint32_t>(i) * 2654435761u;
      const double x = static_cast<double>((hash >> 7) % 4096) / 4096.0;
      const double y = static_cast<double>((hash >> 19) % 4096) / 4096.0;
      const double px = x * w;
      const double py = y * h * 0.66;
      const double size = 0.6 + static_cast<double>((hash >> 3) % 7) * 0.18;
      const BYTE alpha = static_cast<BYTE>(26 + (hash >> 11) % 90);
      star.SetColor(Color(alpha, 226, 234, 255));
      g.FillEllipse(&star, RectF(static_cast<REAL>(px), static_cast<REAL>(py),
                                 static_cast<REAL>(size), static_cast<REAL>(size)));
    }
  }

  const int ar = (th.accent >> 16) & 0xFF, ag = (th.accent >> 8) & 0xFF,
            ab = th.accent & 0xFF;

  // Aurora ribbons: three wide, very low-opacity bands, each leaning differently.
  auto ribbon = [&](double cx, double cy, double rx, double ry, ARGB tint) {
    GraphicsPath path;
    path.AddEllipse(RectF(static_cast<REAL>(cx - rx), static_cast<REAL>(cy - ry),
                          static_cast<REAL>(rx * 2), static_cast<REAL>(ry * 2)));
    PathGradientBrush b(&path);
    b.SetCenterColor(Color(tint));
    Color surround(0, 0, 0, 0);
    int one = 1;
    b.SetSurroundColors(&surround, &one);
    g.FillPath(&b, &path);
  };
  ribbon(w * 0.30, h * 0.30, w * 0.42, h * 0.16, (0x2A << 24) | (ar << 16) | (ag << 8) | ab);
  ribbon(w * 0.62, h * 0.22, w * 0.34, h * 0.12,
         (0x22 << 24) | (0x6E << 16) | (0x9B << 8) | 0xC8);
  ribbon(w * 0.80, h * 0.40, w * 0.30, h * 0.14, (0x1C << 24) | (ar << 16) | (ag << 8) | ab);

  // Horizon: two soft hills and a thin water line, all faded into the haze.
  SolidBrush hill(Color(0xFF0D1220));
  GraphicsPath ridge;
  ridge.AddBezier(0.0f, static_cast<REAL>(h * 0.70f),
                  static_cast<REAL>(w * 0.26f), static_cast<REAL>(h * 0.58f),
                  static_cast<REAL>(w * 0.40f), static_cast<REAL>(h * 0.74f),
                  static_cast<REAL>(w * 0.58f), static_cast<REAL>(h * 0.70f));
  ridge.AddBezier(static_cast<REAL>(w * 0.58f), static_cast<REAL>(h * 0.70f),
                  static_cast<REAL>(w * 0.76f), static_cast<REAL>(h * 0.66f),
                  static_cast<REAL>(w * 0.86f), static_cast<REAL>(h * 0.76f),
                  static_cast<REAL>(w), static_cast<REAL>(h * 0.68f));
  ridge.AddLine(static_cast<REAL>(w), static_cast<REAL>(h),
                static_cast<REAL>(0), static_cast<REAL>(h));
  ridge.CloseFigure();
  g.FillPath(&hill, &ridge);

  // Mist rising off the water, then the water itself. Both are gradients: a
  // flat fill for the sea met the horizon in a perfectly straight hard line,
  // which is the one thing that makes a generated background look fake.
  LinearGradientBrush haze(RectF(0, static_cast<REAL>(h * 0.58f),
                                 static_cast<REAL>(w), static_cast<REAL>(h * 0.20f)),
                           Color(0, 150, 180, 200), Color(48, 150, 180, 200),
                           LinearGradientModeVertical);
  g.FillRectangle(&haze, RectF(0, static_cast<REAL>(h * 0.58f), static_cast<REAL>(w),
                               static_cast<REAL>(h * 0.20f)));
  LinearGradientBrush water(RectF(0, static_cast<REAL>(h * 0.74f),
                                  static_cast<REAL>(w), static_cast<REAL>(h * 0.26f)),
                            Color(0xFF1B2436), Color(0xFF080B12),
                            LinearGradientModeVertical);
  g.FillRectangle(&water, RectF(0, static_cast<REAL>(h * 0.74f), static_cast<REAL>(w),
                               static_cast<REAL>(h * 0.26f)));
  // The aurora's own light bleeding down into the water, so the two halves of
  // the scene belong together.
  glow(g, w * 0.36, h * 0.80, w * 0.26, h * 0.10,
       (0x1E << 24) | (ar << 16) | (ag << 8) | ab);
  // A few faint horizontal glints on the water, never bright enough to compete
  // with the lyrics above them.
  for (int i = 0; i < 26; ++i) {
    const uint32_t hash = static_cast<uint32_t>(i) * 2246822519u;
    const double y = h * (0.78 + static_cast<double>((hash >> 5) % 1000) / 1000.0 * 0.2);
    const double x = w * static_cast<double>((hash >> 15) % 1000) / 1000.0;
    const double len = w * (0.01 + static_cast<double>((hash >> 25) % 40) / 4000.0);
    SolidBrush glint(Color(static_cast<BYTE>(10 + (hash >> 9) % 22), ar, ag, ab));
    g.FillRectangle(&glint, RectF(static_cast<REAL>(x), static_cast<REAL>(y),
                                  static_cast<REAL>(len), 1.0f));
  }
}

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

  // The background is cached at the window's own size (up to 4K) so that each
  // frame copies it instead of rescaling a smaller bitmap, which was the most
  // expensive thing the renderer did on a high-resolution screen. Above 4K it
  // is capped and stretched.
  const double cache_scale = std::min(
      1.0, std::min(3840.0 / static_cast<double>(w),
                    2160.0 / static_cast<double>(h)));
  const int bw = std::max(1, static_cast<int>(std::lround(w * cache_scale)));
  const int bh = std::max(1, static_cast<int>(std::lround(h * cache_scale)));
  Bitmap* bg = new Bitmap(bw, bh, PixelFormat32bppPARGB);
  if (!bg || bg->GetLastStatus() != Ok) {
    delete bg;
    return;
  }
  delete v.bg_cached;
  v.bg_cached = nullptr;
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
  } else if (app.cfg.wallpaper_index == kWallpaperCount - 1) {
    paint_procedural_background(g, bw, bh, th);
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

  // A broad, soft darkening behind the lyric column. The stills are busy in
  // the middle; without this, pale text over a pale part of the picture was
  // lost. It is independent of the user's "background darkness" setting.
  glow(g, bw * 0.5, bh * 0.47, bw * 0.50, bh * 0.50, 0x66000000u);

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
  trim_font_cache(app);
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
  const double col_w = std::min(w * 0.86, 1560.0);
  // The active line is drawn larger than the rest; rows are wrapped to a width
  // that still fits when a row is enlarged.
  const double active_scale = kActiveScale;
  // (5 % more for the semibold weight the active line switches to.)
  const double max_text_w = col_w / (active_scale * 1.05);

  const std::vector<LyricLine>& src =
      app.lyrics.synced ? app.lyrics.lines : app.pending_plain_lines;
  if (src.empty()) return;

  // Text size comes from the window, never from the song. It used to be shrunk
  // until the single longest line of the sheet fitted on one row (down to
  // 13 px), and untimed sheets were shrunk until the whole song fitted on
  // screen, so one long line, or one long song, made every line unreadable.
  // Long lines are wrapped instead, and the size has a floor relative to the
  // window.
  double base = std::min(std::max(h * 0.056, 20.0), 112.0);
  base = std::min(base, std::max(18.0, col_w / 17.0));  // narrow windows
  base *= app.cfg.font_scale;
  const double floor_px = std::max(16.0, base * 0.76);

  auto measure_at = [&](const std::string& text, double px) {
    Font* f = get_font(app, static_cast<int>(std::lround(px)), 400);
    return measure_text(mg, wide(text), f);
  };

  // A modest shrink is allowed when it lets most lines stay on one row: fit
  // the 85th-percentile line, and let the few longer ones wrap.
  {
    std::vector<double> widths;
    widths.reserve(src.size());
    for (const auto& l : src) {
      if (!l.text.empty()) widths.push_back(measure_at(l.text, base));
    }
    if (!widths.empty()) {
      std::sort(widths.begin(), widths.end());
      const size_t index = std::min(widths.size() - 1,
                                    static_cast<size_t>(widths.size() * 0.85));
      const double typical = widths[index];
      if (typical > max_text_w && typical > 0) {
        base = std::max(floor_px, base * max_text_w / typical);
      }
    }
  }
  base = std::floor(base);

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
      double raw_p = static_cast<double>(position_ms - segment.t_ms) /
                     static_cast<double>(segment_end_ms - segment.t_ms);
      raw_p = std::max(0.0, std::min(1.0, raw_p));
      // Hermite smoothstep for natural syllable transition
      const double smooth_p = raw_p * raw_p * (3.0 - 2.0 * raw_p);
      filled += segment_width * smooth_p;
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

void draw_feathered_wipe(Graphics& g, const std::wstring& text, Font* font,
                         const RectF& box, double full_w, double filled_w,
                         double col_x, double col_w,
                         ARGB base_color, ARGB dim_color, bool outline) {
  if (text.empty() || full_w <= 0.0) return;

  // 1. Draw the calm, unsung dimmed text first
  draw_text_backing(g, text, font, box, static_cast<int>(base_color >> 24), outline);
  draw_text(g, text, font, box, dim_color);

  if (filled_w <= 0.5) return;

  const double left = col_x + (col_w - full_w) * 0.5;
  const double feather = std::max(14.0, std::min(28.0, font->GetSize() * 0.65));

  if (filled_w >= full_w) {
    // Fully sung: draw crisp 100% lit text
    draw_text(g, text, font, box, base_color);
    return;
  }

  // 2. Draw highlighted text using a LinearGradientBrush with smooth feathered alpha
  RectF grad_rect(static_cast<REAL>(left), box.Y, static_cast<REAL>(full_w), box.Height);
  LinearGradientBrush brush(grad_rect,
                            Color(base_color),
                            Color(0, (base_color >> 16) & 0xFF, (base_color >> 8) & 0xFF, base_color & 0xFF),
                            LinearGradientModeHorizontal);

  float p_solid = static_cast<float>((filled_w - feather * 0.5) / full_w);
  float p_fade  = static_cast<float>((filled_w + feather * 0.5) / full_w);
  p_solid = std::max(0.0f, std::min(1.0f, p_solid));
  p_fade  = std::max(p_solid + 0.002f, std::min(1.0f, p_fade));

  Color col_solid(base_color);
  Color col_clear(0, (base_color >> 16) & 0xFF, (base_color >> 8) & 0xFF, base_color & 0xFF);

  Color colors[4] = { col_solid, col_solid, col_clear, col_clear };
  REAL positions[4] = { 0.0f, p_solid, p_fade, 1.0f };
  brush.SetInterpolationColors(colors, positions, 4);

  StringFormat fmt;
  fmt.SetAlignment(StringAlignmentCenter);
  fmt.SetLineAlignment(StringAlignmentCenter);
  fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
  g.DrawString(text.c_str(), static_cast<INT>(text.size()), font, box, &fmt, &brush);
}

void paint_lyrics(App& app, Graphics& g, int w, int h) {
  View& v = app.view;
  const Theme& th = theme(app.cfg.theme);
  if (app.disp.empty() || v.line_top.empty()) return;

  const double col_x = (w - v.col_w) * 0.5;
  const int n = static_cast<int>(app.disp.size());
  const int a = app.active;
  const int64_t pos = app.current_pos_ms();
  // Line progress is already an eased position in time; easing it a second time
  // here made the wipe visibly lag behind the vocal on fast lines. The feathered
  // gradient in draw_feathered_wipe supplies the visual softening instead.
  const double progress = (a >= 0) ? line_progress(app.disp, a, pos) : 0.0;
  const bool keyed = app.cfg.mode() == DisplayMode::Desktop && !app.desktop_parented;

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

    double act = 0.0;
    auto it = v.line_activity.find(i);
    if (it != v.line_activity.end()) act = it->second;
    if (is_active && act < 0.05) act = 1.0;

    // Continuation rows belong to the same sung line, so they light with it
    // rather than staying at full distance while the first row turns amber.
    double row_act = act;
    if (!is_active && i > 0 && app.disp[i].cont &&
        app.disp[i - 1].start_ms == app.disp[i].start_ms) {
      auto prev = v.line_activity.find(i - 1);
      if (prev != v.line_activity.end()) row_act = std::max(row_act, prev->second);
    }

    const int dist = (a >= 0) ? std::abs(i - a) : 99;
    // 208 -> 52 across roughly five rows: close lines stay readable, distant
    // ones recede instead of disappearing. Readability beats deep fading when
    // the sheet is projected onto a cinematic still.
    const int base_alpha = static_cast<int>(std::max(88.0, 214.0 - dist * 24.0));
    int alpha = static_cast<int>(base_alpha + (255 - base_alpha) * row_act);
    if (!has_display_timing) alpha = 225;

    // The sung line grows as it lights and settles back as it passes.
    const double scale = 1.0 + (kActiveScale - 1.0) * row_act;
    Font* font = get_font(app, static_cast<int>(std::lround(v.base_px * scale)),
                          (row_act > 0.4) ? 600 : 400);
    std::wstring text = wide(app.disp[i].text);
    const double full_w = measure_text(g, text, font);

    RectF box(static_cast<REAL>(col_x), static_cast<REAL>(y),
              static_cast<REAL>(v.col_w), static_cast<REAL>(v.line_h[i]));

    if (act > 0.01) {
      // Soft contoured spotlight aura behind the singing line, fading smoothly with activity
      const double band_w = std::min(static_cast<double>(box.Width), full_w + box.Height * 1.5);
      const double band_x = col_x + (v.col_w - band_w) * 0.5;
      RectF band(static_cast<REAL>(band_x), box.Y + box.Height * 0.08f,
                 static_cast<REAL>(band_w), box.Height * 0.84f);
      GraphicsPath p;
      REAL r = band.Height * 0.5f;
      p.AddArc(band.X, band.Y, r, r, 90, 180);
      p.AddArc(band.X + band.Width - r, band.Y, r, r, 270, 180);
      p.CloseFigure();
      int ar = (th.accent >> 16) & 0xFF, ag = (th.accent >> 8) & 0xFF,
          ab = th.accent & 0xFF;
      int halo_alpha = static_cast<int>(std::min(30.0, 30.0 * act));
      PathGradientBrush halo(&p);
      halo.SetCenterColor(Color(halo_alpha, ar, ag, ab));
      Color edge(0, ar, ag, ab);
      int one = 1;
      halo.SetSurroundColors(&edge, &one);
      g.FillPath(&halo, &p);
    }

    ARGB base_color = (th.text & 0x00FFFFFF) | (static_cast<ARGB>(alpha) << 24);
    // The not-yet-sung part of the active line must never be fainter than the
    // lines around it, or the line being sung is the hardest one to read.
    ARGB dim_color = (th.text & 0x00FFFFFF) |
                     (static_cast<ARGB>(std::max(132.0, alpha * 0.60)) << 24);
    // Only rows near the focus get a backing; far rows are faint by design.
    const bool backing = dist <= 5 || !has_display_timing;

    if (is_active && app.lyrics.synced && app.cfg.wipe) {
      double filled_w = app.disp[i].segments.empty()
                            ? full_w * progress
                            : timed_segment_fill_width(
                                  g, app.disp[i], app.disp[i].text, font, pos);

      // Sub-frame smoothing only has to hide the 16 ms timer tick; it must be
      // measured against real elapsed time so a slower frame does not leave the
      // wipe behind the audio, and so seeking backwards snaps immediately.
      const int64_t now_frame = app.now_ms();
      double dt = v.last_wipe_at ? (now_frame - v.last_wipe_at) / 1000.0 : 0.016;
      dt = std::max(0.0, std::min(dt, 0.25));
      v.last_wipe_at = now_frame;
      if (v.smooth_fill_line != a || filled_w + 0.5 < v.smooth_fill) {
        v.smooth_fill_line = a;
        v.smooth_fill = filled_w;
      } else {
        v.smooth_fill = ease_towards(v.smooth_fill, filled_w, dt, 0.035);
        if (std::fabs(v.smooth_fill - filled_w) < 0.25) v.smooth_fill = filled_w;
        filled_w = v.smooth_fill;
      }

      draw_feathered_wipe(g, text, font, box, full_w, filled_w, col_x, v.col_w,
                          base_color, dim_color, keyed);
    } else {
      if (backing) draw_text_backing(g, text, font, box, alpha, keyed);
      draw_text(g, text, font, box, base_color);
    }
  }
}

void paint_plain(App& app, Graphics& g, int w, int h) {
  // Unsynced lyrics: a calm, centred sheet. There is no active line to anchor
  // to, so one optional focal row (the paced estimate) is emphasised and the
  // rest fade with distance exactly like the timed view.
  View& v = app.view;
  const Theme& th = theme(app.cfg.theme);
  const double col_x = (w - v.col_w) * 0.5;
  const int focus = app.timing_estimated ? app.active : -1;
  for (size_t i = 0; i < app.disp.size() && i < v.line_top.size(); ++i) {
    double y = v.line_top[i] + v.scroll;
    if (y > h + 100) break;
    if (y + v.line_h[i] < -100) continue;
    if (app.disp[i].text.empty()) continue;
    const bool lit = focus >= 0 && static_cast<int>(i) == focus;
    Font* font = get_font(app, static_cast<int>(std::lround(v.base_px)),
                          lit ? 600 : 400);
    int alpha = 0xE4;
    if (focus >= 0) {
      const int dist = std::abs(static_cast<int>(i) - focus);
      alpha = std::max(70, 0xE4 - dist * 24);
    }
    ARGB c = (th.text & 0x00FFFFFF) | (static_cast<ARGB>(alpha) << 24);
    const RectF row(static_cast<REAL>(col_x), static_cast<REAL>(y),
                    static_cast<REAL>(v.col_w), static_cast<REAL>(v.line_h[i]));
    const std::wstring row_text = wide(app.disp[i].text);
    draw_text_backing(g, row_text, font, row, alpha,
                      app.cfg.mode() == DisplayMode::Desktop && !app.desktop_parented);
    draw_text(g, row_text, font, row, c);
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
  if (app.total_offset_ms() != 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%ssync %+.2fs%s",
                  status_line.empty() ? "" : "  \xC2\xB7  ",
                  app.total_offset_ms() / 1000.0,
                  app.song_offset_ms != 0 ? " for this song" : "");
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
  const DisplayMode mode = app.cfg.mode();
  const std::wstring text =
      mode == DisplayMode::Desktop
          ? L"On the desktop   \u00b7   Ctrl+Alt+F  full screen   \u00b7   "
            L"Ctrl+Alt+S  settings   \u00b7   Ctrl+Alt+L  hide"
          : (mode == DisplayMode::Fullscreen
                 ? L"Esc  leave full screen   \u00b7   F1  shortcuts   \u00b7   "
                   L"Ctrl+Alt+W  move to the desktop"
                 : L"F11  full screen   \u00b7   F1  shortcuts   \u00b7   "
                   L"Ctrl+Alt+W  move to the desktop");
  double px = std::min(std::max(h * 0.017, 11.0), 20.0);
  ARGB c = (th.text & 0x00FFFFFF) | (static_cast<ARGB>(std::min(170.0, 170.0 * fade)) << 24);
  Font* font = get_font(app, static_cast<int>(px), 400);
  const RectF box(0, static_cast<REAL>(h - px * 4.5), static_cast<REAL>(w),
                  static_cast<REAL>(px * 2.2));
  draw_text_backing(g, text, font, box, static_cast<int>(170 * fade),
                    mode == DisplayMode::Desktop && !app.desktop_parented);
  draw_text(g, text, font, box, c);
}

void paint_help(App& app, Graphics& g, int w, int h) {
  View& v = app.view;
  if (!v.help) return;

  SolidBrush dim(Color(0xD0, 4, 4, 8));
  g.FillRectangle(&dim, RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)));

  struct Row {
    const wchar_t* key;
    const wchar_t* what;
  };
  struct Group {
    const wchar_t* title;
    const Row* rows;
    int count;
  };
  static const Row kWindow[] = {
      {L"F11", L"Full screen / window"},
      {L"Ctrl+Alt+F", L"Full screen from any app"},
      {L"Esc", L"Close this, or leave full screen"},
      {L"A", L"Always on top"},
      {L"Ctrl+Alt+W", L"Lyrics on the desktop, or back"},
      {L"Ctrl+Alt+L", L"Show / hide from any app"},
  };
  static const Row kLyrics[] = {
      {L"\u2191 / \u2193  + / \u2212", L"Bigger / smaller text"},
      {L"\u2190 / \u2192", L"This song earlier / later by 0.25 s"},
      {L"F", L"Reset text size"},
      {L"R", L"Look the lyrics up again"},
      {L"K", L"Karaoke wipe on / off"},
      {L"D", L"Save a diagnostic report"},
  };
  static const Row kLook[] = {
      {L"T", L"Theme"},
      {L"C", L"Album-art tint on / off"},
      {L"B", L"Next cinematic wallpaper"},
      {L"[", L"Background darkness (cycles)"},
      {L"Ctrl+Alt+S", L"Settings"},
      {L"F1 / H", L"Show / hide this help"},
      {L"Q", L"Quit"},
  };
  static const Group kGroups[] = {
      {L"WINDOW", kWindow, static_cast<int>(sizeof(kWindow) / sizeof(kWindow[0]))},
      {L"LYRICS", kLyrics, static_cast<int>(sizeof(kLyrics) / sizeof(kLyrics[0]))},
      {L"LOOK", kLook, static_cast<int>(sizeof(kLook) / sizeof(kLook[0]))},
  };
  constexpr int kGroupCount = 3;
  int row_total = 0;
  for (const Group& group : kGroups) row_total += group.count;

  // Three columns side by side: the panel is as tall as its longest column, and
  // the type is scaled down until it fits a small window in both directions.
  int longest = 0;
  for (const Group& group : kGroups) longest = std::max(longest, group.count);
  (void)row_total;
  double panel_w = std::min(w * 0.94, 1040.0);
  double key_px = std::min(std::max(h * 0.0165, 11.0), 19.0);
  key_px = std::min(key_px, panel_w / 58.0);
  const double rows_needed = longest + 6.2;
  if (key_px * 2.1 * rows_needed > h * 0.94) key_px = h * 0.94 / (2.1 * rows_needed);
  key_px = std::max(7.0, key_px);
  double row_h = key_px * 2.1;
  double panel_h = std::min(row_h * rows_needed, h * 0.96);
  RectF panel(static_cast<REAL>((w - panel_w) / 2),
              static_cast<REAL>((h - panel_h) / 2), static_cast<REAL>(panel_w),
              static_cast<REAL>(panel_h));
  draw_rounded_rect(g, panel, 18.0f, 0xF20E0E18, true, 0x33FFFFFF);

  const double pad = panel_w * 0.045;
  draw_text(g, L"Keyboard and mouse", get_font(app, static_cast<int>(key_px * 1.5), 600),
            RectF(panel.X + static_cast<REAL>(pad), panel.Y + static_cast<REAL>(row_h * 0.3),
                  panel.Width - static_cast<REAL>(pad * 2), static_cast<REAL>(row_h * 1.3)),
            0xFFF2F2F8);
  draw_text(g, L"Everything below is also in the tray menu. F1 or Esc closes this.",
            get_font(app, static_cast<int>(key_px * 0.85), 400),
            RectF(panel.X + static_cast<REAL>(pad), panel.Y + static_cast<REAL>(row_h * 1.35),
                  panel.Width - static_cast<REAL>(pad * 2), static_cast<REAL>(row_h)),
            0x88F0F0F8);

  StringFormat right;
  right.SetAlignment(StringAlignmentFar);
  right.SetLineAlignment(StringAlignmentCenter);
  StringFormat left;
  left.SetAlignment(StringAlignmentNear);
  left.SetLineAlignment(StringAlignmentCenter);

  const double column_w = (panel.Width - pad * 2) / kGroupCount;
  double y = panel.Y + row_h * 2.5;
  for (int gi = 0; gi < kGroupCount; ++gi) {
    const Group& group = kGroups[gi];
    const double x = panel.X + pad + column_w * gi;
    draw_text(g, group.title, get_font(app, static_cast<int>(key_px * 0.72), 600),
              RectF(static_cast<REAL>(x), static_cast<REAL>(y),
                    static_cast<REAL>(column_w - 12), static_cast<REAL>(row_h)),
              0xFFF5A524u);
    double ry = y + row_h * 1.15;
    for (int i = 0; i < group.count; ++i) {
      Font* kf = get_font(app, static_cast<int>(key_px * 0.92), 600);
      Font* wf = get_font(app, static_cast<int>(key_px * 0.92), 400);
      SolidBrush kb{Color(0xFFE8E8F2)};
      g.DrawString(group.rows[i].key, -1, kf,
                   RectF(static_cast<REAL>(x), static_cast<REAL>(ry),
                         static_cast<REAL>(key_px * 5.4), static_cast<REAL>(row_h)),
                   &left, &kb);
      SolidBrush wb{Color(0x9EF0F0F8)};
      g.DrawString(group.rows[i].what, -1, wf,
                   RectF(static_cast<REAL>(x + key_px * 5.6), static_cast<REAL>(ry),
                         static_cast<REAL>(column_w - key_px * 5.6),
                         static_cast<REAL>(row_h)),
                   &left, &wb);
      ry += row_h;
    }
  }

  // Diagnostics line: what the media helper last reported.
  std::string raw;
  {
    std::lock_guard<std::mutex> lock(app.mu);
    raw = app.media.raw;
  }
  Font* mono = get_font(app, static_cast<int>(key_px * 0.66), 400);
  std::string diag = "media helper: ";
  diag += raw.empty() ? std::string("(waiting)") : raw;
  if (diag.size() > 240) diag = diag.substr(0, 240) + "...";
  SolidBrush db{Color(0x66C8C8D8)};
  g.DrawString(wide(diag).c_str(), -1, mono,
               RectF(panel.X + static_cast<REAL>(pad),
                     static_cast<REAL>(panel.Y + panel.Height - row_h * 1.6),
                     panel.Width - static_cast<REAL>(pad * 2), static_cast<REAL>(row_h * 1.4)),
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

  // A first launch with no config at all gets one extra line explaining what
  // will happen, so "Listening" is not a mystery.
  const bool welcome = app.first_run && !listening && !listener_silent;
  const std::string big = listening ? "Paused" : "Listening";
  const std::string sub =
      listening ? track_label(app.current_track())
                : (welcome ? "Youtube-Lyrics is running in the tray and on your desktop."
                           : "Play a song anywhere \u2014 YouTube Music, Spotify, any player.");
  draw_text(g, wide(big), get_font(app, static_cast<int>(tpx), 600),
            RectF(0, static_cast<REAL>(cy + unit * 0.62), static_cast<REAL>(w),
                  static_cast<REAL>(tpx * 2)),
            (0xD8u << 24) | 0x00F3EFE6);
  draw_text(g, wide(sub), get_font(app, static_cast<int>(bpx), 400),
            RectF(0, static_cast<REAL>(cy + unit * 0.62 + tpx * 1.5),
                  static_cast<REAL>(w), static_cast<REAL>(bpx * 3)),
            (0x84u << 24) | 0x00F3EFE6);

  if (listener_silent) {
    draw_text(g, L"No media session yet \u00b7 check the tray menu or ytlyrics.log",
              get_font(app, static_cast<int>(bpx * 0.92), 400),
              RectF(0, static_cast<REAL>(cy + unit * 0.62 + tpx * 1.5 + bpx * 2.1),
                    static_cast<REAL>(w), static_cast<REAL>(bpx * 3)),
              (0x55u << 24) | 0x00F3EFE6);
  } else if (welcome) {
    draw_text(g,
              L"Ctrl+Alt+S  settings   \u00b7   F1  shortcuts   \u00b7   "
              L"Ctrl+Alt+W  desktop layer",
              get_font(app, static_cast<int>(bpx * 0.92), 400),
              RectF(0, static_cast<REAL>(cy + unit * 0.62 + tpx * 1.5 + bpx * 2.1),
                    static_cast<REAL>(w), static_cast<REAL>(bpx * 3)),
              (0x66u << 24) | 0x00F3EFE6);
  }
}

void render_frame(App& app, Graphics& g, int w, int h) {
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  g.SetPixelOffsetMode(PixelOffsetModeHalf);

  trim_font_cache(app);
  const bool desktop_wallpaper = app.cfg.mode() == DisplayMode::Desktop;
  const bool fallback_top = desktop_wallpaper && !app.desktop_parented;

  if (fallback_top) {
    // ClearType's coloured sub-pixels would be composited against the key
    // colour and show as fringes over the real wallpaper.
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    // In fallback mode where WorkerW could not be parented, clear with the
    // color-key color RGB(16, 16, 24) so that the user's desktop wallpaper
    // and desktop icons remain 100% visible and unobstructed.
    SolidBrush colorkey(Color(255, 16, 16, 24));
    g.FillRectangle(&colorkey, RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)));
  } else {
    ensure_background(app, w, h);
    if (app.view.bg) {
      View& v = app.view;
      bool drawn = false;
      if (static_cast<int>(v.bg->GetWidth()) == w &&
          static_cast<int>(v.bg->GetHeight()) == h) {
        // Same size as the window: blit a device-format copy.
        if (!v.bg_cached) v.bg_cached = new CachedBitmap(v.bg, &g);
        if (v.bg_cached && v.bg_cached->GetLastStatus() == Ok &&
            g.DrawCachedBitmap(v.bg_cached, 0, 0) == Ok) {
          drawn = true;
        } else {
          // The display format changed under us; rebuild it next frame.
          delete v.bg_cached;
          v.bg_cached = nullptr;
        }
      }
      if (!drawn) {
        g.DrawImage(v.bg, RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)));
      }
    } else {
      SolidBrush black(Color(0xFF101018));
      g.FillRectangle(&black, RectF(0, 0, static_cast<REAL>(w),
                                    static_cast<REAL>(h)));
    }
  }

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
        // In wallpaper mode a failed lookup leaves the still image alone rather
        // than printing an error across the desktop; the tray menu can reopen
        // this view. A windowed/full-screen session shows the detail.
        if (!desktop_wallpaper)
          paint_centred_message(app, g, w, h, "No lyrics found",
                                track_label(app.current_track()), app.lmessage,
                                "R  search again   \u00b7   add a .lrc file to the "
                                "local lyrics folder",
                                "Ctrl+Alt+S  settings   \u00b7   F1  shortcuts");
        break;
      case LyricsState::Error:
        if (!desktop_wallpaper)
          paint_centred_message(app, g, w, h, "Lyrics lookup failed",
                                track_label(app.current_track()), app.lmessage,
                                "R  retry   \u00b7   check your connection, then retry",
                                "Ctrl+Alt+S  settings   \u00b7   F1  shortcuts");
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
  paint_hint(app, g, w, h);
  paint_toast(app, g, w, h);
  paint_help(app, g, w, h);
}

}  // namespace ytl
