#pragma once
// M12 render, first slice: a Sequence compiled into a flat list of layers, composited on the CPU.
// Tracks stack in track_order: the first track is the bottom layer. Every clip is fitted inside the canvas
// (aspect kept, centred) and blended with its opacity, in NV12: blending Y, U and V gives the same result as blending
// R, G and B, because the conversion between them is linear. The GPU compositor of the plan replaces the blit, not
// this interface.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

#include "atm/base/error.hpp"
#include "atm/eval/keyframes.hpp"
#include "atm/media/media.hpp"

namespace atm::render {

struct Layer {
  std::string clip_id, path;
  int track = 0;
  bool video = true;          // false for clips on audio tracks
  int64_t start_frame = 0;    // on the sequence
  int64_t frames = 0;
  int64_t source_in_hns = 0;  // where in the file the clip starts
  float opacity = 1.0f;
  float volume = 1.0f;
  // Transform (ADR-021 canvas fractions): position is where the clip's centre sits on the canvas, top-left origin,
  // so [0.5, 0.5] is the middle. Scale 1 is "fitted inside the canvas"; 2 is twice that size.
  float pos_x = 0.5f, pos_y = 0.5f, scale_x = 1.0f, scale_y = 1.0f;
  // Text clips (media_ref.type "text"): no file, the picture is the text. `text_size` is the font height as a
  // fraction of the canvas height; the colour is 0xRRGGBB.
  bool is_text = false, text_bold = false;
  std::string text;
  float text_size = 0.08f;
  uint32_t text_color = 0xFFFFFF;
  // Dissolve (attome.dissolve) from this clip into the next one on its track. compile() lengthens both clips into the
  // media beyond the cut, so over [mix_start, mix_start + mix_frames) both are active; each is drawn over the layers
  // below and the two results are mixed by progress. Indices are into Composition::layers.
  int mix_with = -1; // on the outgoing clip: the incoming one
  int mixed_by = -1; // on the incoming clip: the outgoing one
  int64_t mix_start = 0, mix_frames = 0; // on the outgoing clip
  bool mixing_at(int64_t frame) const { return frame >= mix_start && frame < mix_start + mix_frames; }
  // Keyframes (transform.keyframes) replace the static values above while they exist. Their times are clip-local:
  // 0 is `origin_frame`, the clip's record_in (start_frame moves earlier when a dissolve leads into the clip).
  int64_t origin_frame = 0;
  eval::Curve opacity_keys, position_keys, scale_keys;
};

// The transform of a layer at one frame, keyframes applied.
struct Pose {
  float opacity = 1.0f, pos_x = 0.5f, pos_y = 0.5f, scale_x = 1.0f, scale_y = 1.0f;
};

struct Composition;
Pose pose_at(const Layer &layer, const Composition &comp, int64_t frame);

struct Composition {
  int width = 1920, height = 1080;
  int64_t rate_num = 30, rate_den = 1;
  int64_t frames = 0; // length of the sequence
  std::vector<Layer> layers; // bottom track first
  int64_t frame_hns(int64_t frame) const { return frame * rate_den * media::kHnsPerSecond / rate_num; }
};

// Reads a Sequence of a Project Document (the first one when `sequence_id` is empty).
Result<Composition> compile(const nlohmann::json &project, std::string_view sequence_id = {});

class Renderer {
public:
  Renderer(Composition composition, int width, int height); // output size; the canvas is scaled to it
  ~Renderer();
  const Composition &composition() const { return comp_; }
  int width() const { return width_; }
  int height() const { return height_; }
  // Writes a packed NV12 picture of media::nv12_size(width(), height()) bytes. A clip whose file cannot be read is
  // left out; the first such problem is returned by take_warning().
  Result<void> render(int64_t frame, uint8_t *nv12);
  std::string take_warning();
  // Changes the transform of one clip in place; decoders stay open, so this is cheap enough to do on every mouse move.
  // Pixel size of the last drawn text of a clip at this renderer's output size, or {0, 0}.
  std::pair<int, int> text_extent(const std::string &clip_id) const;
  void set_transform(const std::string &clip_id, float pos_x, float pos_y, float scale_x, float scale_y);

private:
  Composition comp_;
  int width_, height_;
  std::unordered_map<std::string, std::unique_ptr<media::VideoReader>> readers_; // by clip ID
  std::unordered_map<std::string, bool> failed_;
  struct TextEntry {
    std::string key;
    media::TextBitmap bitmap;
  };
  std::unordered_map<std::string, TextEntry> text_; // by clip ID
  std::vector<uint8_t> mix_;                          // the incoming clip of a dissolve, drawn over the same background
  std::string warning_;
  void draw(const Layer &l, int64_t frame, uint8_t *out, bool &cleared, std::vector<const std::string *> &used);
};

// The whole sequence as 48 kHz stereo float.
Result<std::vector<float>> mix_audio(const Composition &composition);

} // namespace atm::render
