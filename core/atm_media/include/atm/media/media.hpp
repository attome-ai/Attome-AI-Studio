#pragma once
// M11 media I/O: probe, decode video frames, read audio, encode H.264 + AAC into MP4.
// The Windows backend uses Media Foundation (OS codecs, hardware encoders). Other systems have no backend yet;
// every call there returns Unsupported.
//
// Pictures are NV12 from the decoder to the encoder (BT.709, limited range): a full-size Y plane and a half-size
// plane of interleaved U and V. It is what decoders produce and encoders take, so nothing converts in between.
//
// Times are in 100 ns units ("hns"), the unit of the OS media stack. Exact Rational Time stays in the document;
// the conversion happens once, at this edge.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::media {

inline constexpr int kAudioRate = 48000; // every audio buffer here is 48 kHz stereo float, interleaved
inline constexpr int64_t kHnsPerSecond = 10'000'000;

struct MediaInfo {
  bool has_video = false, has_audio = false;
  int width = 0, height = 0;
  int64_t rate_num = 0, rate_den = 1;
  int64_t duration_hns = 0;
  int audio_rate = 0, audio_channels = 0;
  bool is_image = false; // a still picture (PNG, JPEG, ...): no duration, no frame rate
};

// What a file holds. Still pictures are recognised by their extension (is_still) and read with stb_image on every
// system; everything else goes to the media backend.
Result<MediaInfo> probe(const std::string &path);

// One decoded NV12 picture. Valid until the next call on the reader that returned it.
struct FrameView {
  const uint8_t *y = nullptr, *uv = nullptr;
  int y_pitch = 0, uv_pitch = 0;
  int width = 0, height = 0; // even
  const uint8_t *alpha = nullptr; // straight (not premultiplied) opacity per pixel, or null when fully opaque
  int alpha_pitch = 0;
};

// A packed NV12 picture: Y rows of `width` bytes, then height / 2 UV rows of `width` bytes.
inline size_t nv12_size(int width, int height) { return size_t(width) * size_t(height) * 3 / 2; }
void bgrx_to_nv12(const uint8_t *bgrx, int width, int height, uint8_t *nv12); // both packed
void nv12_to_bgrx(const uint8_t *nv12, int width, int height, uint8_t *bgrx);
void fill_black(uint8_t *nv12, int width, int height);

class VideoReader {
public:
  // Frames come out scaled to fit inside box_width x box_height, keeping the aspect ratio (0 = native size).
  static Result<std::unique_ptr<VideoReader>> open(const std::string &path, int box_width, int box_height);
  ~VideoReader();
  // The frame shown at `time`. Reads forward when the time is just ahead, seeks otherwise.
  Result<FrameView> frame_at(int64_t time_hns);

  struct Impl;

private:
  VideoReader() = default;
  std::unique_ptr<Impl> impl_;
};

// A still picture (PNG, JPEG, BMP, GIF's first frame, TGA) fitted inside box_width x box_height like VideoReader
// frames (0 = native size). Transparency is kept as an alpha plane; colours under fully transparent pixels are filled
// from their neighbours, so scaling the picture up leaves no dark fringe.
struct Still {
  int width = 0, height = 0;  // even
  std::vector<uint8_t> nv12;  // packed
  std::vector<uint8_t> alpha; // width * height, empty when the picture is opaque
  FrameView view() const;
};
bool is_still(const std::string &path); // by extension
Result<Still> read_still(const std::string &path, int box_width, int box_height);

// Audio of [start, start + duration) as 48 kHz stereo float. Empty when the file has no audio.
Result<std::vector<float>> read_audio(const std::string &path, int64_t start_hns, int64_t duration_hns);

struct EncodeSettings {
  std::string path; // .mp4
  int width = 1920, height = 1080;
  int64_t rate_num = 30, rate_den = 1;
  int bitrate = 12'000'000;
  bool audio = true;
  int quality_vs_speed = 0; // 0 = fastest … 100 = best quality per bit
};

class Encoder {
public:
  static Result<std::unique_ptr<Encoder>> create(const EncodeSettings &settings);
  ~Encoder();
  Result<void> video(const uint8_t *nv12, int64_t frame); // packed NV12, nv12_size(width, height) bytes
  Result<void> audio(const float *stereo, size_t frames);
  Result<void> finish();
  const std::string &name() const; // the video encoder Windows picked, e.g. "NVIDIA H.264 Encoder MFT"

  struct Impl;

private:
  Encoder() = default;
  std::unique_ptr<Impl> impl_;
};

// A line or paragraph of text as a coverage mask (0..255 per pixel), drawn in white. Text wraps at `max_width` pixels
// and is centred. Handles right-to-left and shaped scripts (Arabic). DirectWrite on Windows by default; FreeType,
// HarfBuzz and SheenBidi with the bundled Noto fonts everywhere else (ATM_TEXT_BACKEND).
struct TextBitmap {
  int width = 0, height = 0;
  std::vector<uint8_t> alpha; // width * height
};
Result<TextBitmap> render_text(const std::string &utf8, float size_px, bool bold, int max_width);

// The folder holding the bundled fonts (Noto, Lucide icons), as UTF-8, or empty when it is missing: ATTOME_FONTS,
// then fonts/ next to the program, then ../share/attome/fonts.
std::string font_dir();

// A packed BGRX picture as a JPEG file (quality 0..1). For agents and thumbnails, not for delivery.
Result<void> write_jpeg(const std::string &path, const uint8_t *bgrx, int width, int height, float quality = 0.85f);

} // namespace atm::media
