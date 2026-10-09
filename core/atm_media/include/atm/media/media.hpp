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
#include <utility>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::media {

inline constexpr int kAudioRate = 48000; // every audio buffer here is 48 kHz stereo float, interleaved
inline constexpr int64_t kHnsPerSecond = 10'000'000;
// Frame times are whole 100 ns units, rounded either way (frame 2 at 30 fps is 666666 or 666667): a frame is taken as
// shown at a time up to this much before its own.
inline constexpr int64_t kFrameTimeSlack = kHnsPerSecond / 1000;

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

// The size VideoReader reads a w x h video at to fit inside box_width x box_height: the aspect kept, even sizes.
std::pair<int, int> fit_inside(int w, int h, int box_width, int box_height);

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

// The compressed video of a file, sample by sample, for a decoder of our own (Vulkan Video): the container is read, the
// video is not decoded. H.264 samples come as Annex B (start codes), with the parameter sets of the container in
// sequence_header(); a sample is one access unit (one picture), in decoding order.
struct Packet {
  std::vector<uint8_t> data;
  int64_t pts = 0;  // presentation time, 100 ns units
  bool key = false; // a point decoding can start from (an IDR picture)
};
class VideoStream {
public:
  static Result<std::unique_ptr<VideoStream>> open(const std::string &path);
  ~VideoStream();
  const std::string &codec() const;                   // "h264", "hevc", or the container's name for another
  const std::vector<uint8_t> &sequence_header() const; // Annex B: the SPS and PPS before the first picture
  // The next sample in decoding order; false at the end.
  Result<bool> next(Packet &packet);
  // Back to the last key sample at or before `time` (the next sample read is it).
  Result<void> seek(int64_t time_hns);

  struct Impl;

private:
  VideoStream() = default;
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

// Whether the H.264 encoders take a picture of w x h: each side 16 to 4096, and no more pixels than 4096 x 2304 (H.264
// level 5.1's largest frame). A vertical 4K picture (2160 x 3840) fits; the hardware and the Windows software encoders
// both take it.
inline bool h264_size_ok(int w, int h) {
  return w >= 16 && h >= 16 && w <= 4096 && h <= 4096 && int64_t(w) * int64_t(h) <= int64_t(4096) * 2304;
}

struct EncodeSettings {
  std::string path; // .mp4
  int width = 1920, height = 1080;
  int64_t rate_num = 30, rate_den = 1;
  int bitrate = 12'000'000;
  bool audio = true;
  int quality_vs_speed = 0; // 0 = fastest … 100 = best quality per bit
  // What PipeEncoder writes (the OS encoder always makes H.264): "prores" or "dnxhr", its profile ("" = hq) and the FFmpeg program to use ("" = look for one).
  std::string codec = "h264", profile, ffmpeg;
  int b_frames = 0; // H.264: B-frames between reference frames (Windows' own encoder makes them; the GPU encoders ignore the count)
};

// An FFmpeg program found on this machine, the user's own: Attome links none and ships none. `license` is that build's: lgpl, gpl, nonfree or unknown;
// `encoders` are the ones Attome uses that it has (prores_ks, dnxhd). Looked for in `configured`, then ATTOME_FFMPEG, then the PATH.
struct FfmpegInfo {
  std::string path, version, license;
  std::vector<std::string> encoders;
};
Result<FfmpegInfo> find_ffmpeg(const std::string &configured);

// ProRes and DNxHR (.mov) through that FFmpeg, run as a separate process the frames are piped into. The same four calls as Encoder; the sound is kept in a
// temporary file and joined to the video at finish().
class PipeEncoder {
public:
  static Result<std::unique_ptr<PipeEncoder>> create(const EncodeSettings &settings);
  ~PipeEncoder();
  Result<void> video(const uint8_t *nv12, int64_t frame);
  Result<void> audio(const float *stereo, size_t frames);
  Result<void> finish();
  const std::string &name() const;

  struct Impl;

private:
  PipeEncoder() = default;
  std::unique_ptr<Impl> impl_;
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
// How a text is drawn: the font (a family name from list_fonts(); empty or unknown = the default: Segoe UI, Noto Sans), bold, italic, where the
// lines sit against each other (-1 left, 0 centre, 1 right) and the distance between lines as a multiple of the font's own (1 = natural).
struct TextStyle {
  bool bold = false, italic = false;
  std::string font;
  int align = 0;
  float line_spacing = 1.0f;
};
Result<TextBitmap> render_text(const std::string &utf8, float size_px, const TextStyle &style, int max_width);
inline Result<TextBitmap> render_text(const std::string &utf8, float size_px, bool bold, int max_width) {
  TextStyle style;
  style.bold = bold;
  return render_text(utf8, size_px, style, max_width);
}
// The font families a text can use, sorted: the ones installed on Windows, the bundled Noto fonts elsewhere.
const std::vector<std::string> &list_fonts();

// The folder holding the bundled fonts (Noto, Lucide icons), as UTF-8, or empty when it is missing: ATTOME_FONTS,
// then fonts/ next to the program, then ../share/attome/fonts.
std::string font_dir();

// A packed BGRX picture as a JPEG file (quality 0..1). For agents and thumbnails, not for delivery.
Result<void> write_jpeg(const std::string &path, const uint8_t *bgrx, int width, int height, float quality = 0.85f);
// The same picture as a PNG: lossless, 8 bits a channel (the frames of an image sequence).
Result<void> write_png(const std::string &path, const uint8_t *bgrx, int width, int height);

} // namespace atm::media
