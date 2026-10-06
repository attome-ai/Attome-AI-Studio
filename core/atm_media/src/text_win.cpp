#if defined(_WIN32) && !defined(ATM_TEXT_FREETYPE)
// Text rasterizing with DirectWrite: shaping, bidirectional text (Arabic, Hebrew) and font fallback come from the
// operating system. The plan's portable path (FreeType + HarfBuzz + SheenBidi) replaces this file on other systems.

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

namespace atm::media {
namespace {

using Microsoft::WRL::ComPtr;

struct Factories {
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<IWICImagingFactory> wic;
};

Factories *factories() {
  thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)com;
  static Factories *f = [] {
    auto *out = new Factories;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, out->d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown **>(out->dwrite.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&out->wic));
    return out;
  }();
  return f;
}

std::wstring widen(const std::string &s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

} // namespace

const std::vector<std::string> &list_fonts() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    Factories *f = factories();
    ComPtr<IDWriteFontCollection> collection;
    if (!f->dwrite || FAILED(f->dwrite->GetSystemFontCollection(&collection, FALSE)))
      return out;
    for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i) {
      ComPtr<IDWriteFontFamily> family;
      ComPtr<IDWriteLocalizedStrings> strings;
      if (FAILED(collection->GetFontFamily(i, &family)) || FAILED(family->GetFamilyNames(&strings)))
        continue;
      UINT32 at = 0;
      BOOL found = FALSE;
      strings->FindLocaleName(L"en-us", &at, &found);
      UINT32 length = 0;
      if (FAILED(strings->GetStringLength(found ? at : 0, &length)))
        continue;
      std::wstring w(size_t(length) + 1, L'\0');
      if (FAILED(strings->GetString(found ? at : 0, w.data(), length + 1)))
        continue;
      w.resize(length);
      const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
      std::string utf8(size_t(std::max(0, n)), '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), utf8.data(), n, nullptr, nullptr);
      if (!utf8.empty() && utf8[0] != '@')
        out.push_back(std::move(utf8));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }();
  return names;
}

Result<TextBitmap> render_text(const std::string &utf8, float size_px, const TextStyle &style, int max_width) {
  ATM_PROFILE_SCOPE("text.raster");
  Factories *f = factories();
  if (!f->d2d || !f->dwrite || !f->wic)
    return fail(ErrorCode::Unsupported, "M_TEXT", "Text rendering is not available on this system.");
  const std::wstring text = widen(utf8);
  if (text.empty() || size_px < 1.0f)
    return TextBitmap{};
  size_px = std::min(size_px, 2000.0f);

  // The family asked for, when this system has it; else Segoe UI.
  std::wstring family = L"Segoe UI";
  if (!style.font.empty()) {
    ComPtr<IDWriteFontCollection> collection;
    UINT32 at = 0;
    BOOL found = FALSE;
    const std::wstring asked = widen(style.font);
    if (SUCCEEDED(f->dwrite->GetSystemFontCollection(&collection, FALSE)) && SUCCEEDED(collection->FindFamilyName(asked.c_str(), &at, &found)) && found)
      family = asked;
  }
  const int align = std::clamp(style.align, -1, 1);
  ComPtr<IDWriteTextFormat> format;
  const auto make_format = [&] {
    format.Reset();
    return SUCCEEDED(f->dwrite->CreateTextFormat(family.c_str(), nullptr, style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                                 style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size_px,
                                                 L"en-us", &format));
  };
  if (!make_format())
    return fail(ErrorCode::Unsupported, "M_TEXT", "The text font could not be created.");
  format->SetTextAlignment(align < 0 ? DWRITE_TEXT_ALIGNMENT_LEADING : align > 0 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  const float wrap = float(std::max(16, max_width));
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(f->dwrite->CreateTextLayout(text.c_str(), UINT32(text.size()), format.Get(), wrap, 10000.0f, &layout)))
    return fail(ErrorCode::Unsupported, "M_TEXT", "The text could not be laid out.");
  if (std::fabs(style.line_spacing - 1.0f) > 0.01f) { // the font's own line height times the spacing; the glyphs stay in the middle of their line
    UINT32 count = 0;
    layout->GetLineMetrics(nullptr, 0, &count);
    std::vector<DWRITE_LINE_METRICS> lines(count);
    if (count > 0 && SUCCEEDED(layout->GetLineMetrics(lines.data(), count, &count))) {
      const float spacing = std::clamp(style.line_spacing, 0.5f, 3.0f), height = lines[0].height;
      format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, height * spacing, lines[0].baseline + (height * spacing - height) * 0.5f);
      layout.Reset();
      if (FAILED(f->dwrite->CreateTextLayout(text.c_str(), UINT32(text.size()), format.Get(), wrap, 10000.0f, &layout)))
        return fail(ErrorCode::Unsupported, "M_TEXT", "The text could not be laid out.");
    }
  }
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);

  const int pad = int(std::ceil(size_px * 0.15f)) + 2; // room for glyphs that reach past the layout box
  const int w = std::max(1, int(std::ceil(m.widthIncludingTrailingWhitespace)) + 2 * pad);
  const int h = std::max(1, int(std::ceil(m.height)) + 2 * pad);
  if (int64_t(w) * h > 64'000'000)
    return fail(ErrorCode::InvalidArgument, "M_TEXT_SIZE", "The text is too large to draw.", {}, "Use a smaller size.");
  // Centre the text in its box: the layout is `wrap` wide, so shift by the free space.
  // Left or right: the layout's own left edge of the text, so right-to-left text is placed by what is drawn, not by the alignment's name.
  const float shift = align == 0 ? (float(w - 2 * pad) - wrap) * 0.5f : -m.left;

  ComPtr<IWICBitmap> bitmap;
  if (FAILED(f->wic->CreateBitmap(UINT(w), UINT(h), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
    return fail(ErrorCode::Internal, "M_TEXT", "The text bitmap could not be created.");
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  if (FAILED(f->d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)))
    return fail(ErrorCode::Internal, "M_TEXT", "The text surface could not be created.");
  ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush);
  target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  target->BeginDraw();
  target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  target->DrawTextLayout(D2D1::Point2F(float(pad) + shift, float(pad)), layout.Get(), brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_NONE);
  if (FAILED(target->EndDraw()))
    return fail(ErrorCode::Internal, "M_TEXT", "Drawing the text failed.");

  TextBitmap out;
  out.width = w;
  out.height = h;
  out.alpha.resize(size_t(w) * size_t(h));
  WICRect all{0, 0, w, h};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, &lock)))
    return fail(ErrorCode::Internal, "M_TEXT", "The text bitmap could not be read.");
  UINT stride = 0, size = 0;
  BYTE *px = nullptr;
  lock->GetStride(&stride);
  lock->GetDataPointer(&size, &px);
  for (int y = 0; y < h; ++y) {
    const BYTE *row = px + size_t(y) * stride;
    for (int x = 0; x < w; ++x)
      out.alpha[size_t(y) * size_t(w) + size_t(x)] = row[x * 4 + 3]; // white, premultiplied: alpha is the coverage
  }
  return out;
}

} // namespace atm::media

#endif // _WIN32
