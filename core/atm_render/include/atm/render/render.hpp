#pragma once
// M12 render, first slice: a Sequence compiled into a flat list of layers, composited on the CPU.
// Tracks stack in track_order: the first track is the bottom layer. Every clip is fitted inside the canvas
// (aspect kept, centred) and blended with its opacity, in NV12: blending Y, U and V gives the same result as blending
// R, G and B, because the conversion between them is linear. The GPU compositor of the plan replaces the blit, not
// this interface.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

#include "atm/base/error.hpp"
#include "atm/eval/effects.hpp"
#include "atm/eval/keyframes.hpp"
#include "atm/media/media.hpp"

namespace atm::render {

struct BakedLut; // a .cube table resampled onto a grid of video-range YUV (render.cpp)

// An effect of a clip or an adjustment layer (ADR-004). `kind` is the id of an eval::EffectDef and v holds its
// parameters in that table's order, clamped to their ranges: gaussian_blur {radius}, color_grade {brightness,
// contrast, saturation}, vignette {strength, radius, softness}. A blur radius is a fraction of the canvas height.
struct Effect {
  std::string id;   // the effect's id in the document (the key of the clip's "effects")
  std::string kind;
  std::string file; // a lut's .cube path (the effect's params.file), else empty
  float v[eval::kMaxEffectParams] = {};
  // Keyframes of the parameters (the effect object's "keyframes": {<param>: {...}}), clip-local like the transform's.
  // A parameter with keys follows its curve and ignores its plain value in v; `def` gives the range it is held to.
  const eval::EffectDef *def = nullptr;
  eval::Curve curve[eval::kMaxEffectParams];
};

// The static transform of a clip (ADR-021 canvas fractions). Position is where the anchor sits on the canvas, top-left
// origin, so [0.5, 0.5] is the middle. The anchor is a point of the clip's own picture (fractions, [0.5, 0.5] its
// centre); scale and rotation turn around it. Scale 1 is "fitted inside the canvas"; 2 is twice that size. Rotation is
// in degrees, clockwise. Crop cuts fractions of the picture off each side; what is left stays where it was.
struct Transform {
  float pos_x = 0.5f, pos_y = 0.5f, scale_x = 1.0f, scale_y = 1.0f;
  float rotation = 0.0f;
  float anchor_x = 0.5f, anchor_y = 0.5f;
  float crop_left = 0.0f, crop_top = 0.0f, crop_right = 0.0f, crop_bottom = 0.0f;
  bool cropped() const { return crop_left > 0.0f || crop_top > 0.0f || crop_right > 0.0f || crop_bottom > 0.0f; }
};

struct Layer {
  std::string clip_id, path;
  int track = 0;
  bool video = true;          // false for clips on audio tracks
  int64_t start_frame = 0;    // on the sequence
  int64_t frames = 0;
  int64_t source_in_hns = 0;  // where in the file the clip starts, in the clip's own time: the file's time is this times `speed`
  // timing.speed: how fast the file plays (2 = twice as fast, 0.5 = slow motion). The clip's times (source_in, duration) are measured as the
  // clip plays, so the frame shown at clip time t is the file's frame at (source_in + t) * speed; the sound is played at the same speed.
  double speed = 1.0;
  // timing.reverse: the clip plays its part of the file backwards (the last frame first), its sound too.
  bool reverse = false;
  float opacity = 1.0f;
  float volume = 1.0f;
  // media_ref.stream: a linked pair shares one file, the picture clip with "video" (silent here) and the sound clip
  // with "audio" (never drawn). Without it a clip shows its picture and plays its sound.
  bool silent = false;
  // Sound (the clip's "audio" object and its track): gain is linear and folds in the clip's gain_db and the track's
  // volume_db (volume above stays a separate factor); pan is -1 (left) .. 1 (right), clip and track added. Fades are
  // measured from the clip's own ends, [origin_frame, clip_end_frame), not from a dissolve's extension.
  float gain = 1.0f, pan = 0.0f;
  int64_t fade_in_hns = 0, fade_out_hns = 0;
  bool fade_linear = false; // false: equal power (a quarter sine), the default
  int64_t clip_end_frame = 0;
  Transform xf;
  // Text clips (media_ref.type "text"): no file, the picture is the text. `text_size` is the font height as a
  // fraction of the canvas height; the colour is 0xRRGGBB.
  bool is_text = false, text_bold = false, text_italic = false;
  std::string text_font;      // content.font: a family name (empty: the default)
  int text_align = 0;         // content.align: left -1, centre 0, right 1; how the lines of a text sit against each other
  float line_spacing = 1.0f;  // content.line_spacing: a multiple of the font's own line height
  // Picture clips (media_ref.type "image"): a still file, the same on every frame, with its transparency.
  bool is_image = false;
  std::string text;
  float text_size = 0.08f;
  uint32_t text_color = 0xFFFFFF;
  // The look of a text (content.outline, content.shadow, content.background); every size is a fraction of the text's own size. Off
  // when the width, the opacity or the box is 0.
  float outline_width = 0.0f;
  uint32_t outline_color = 0x000000;
  float shadow_x = 0.0f, shadow_y = 0.0f, shadow_blur = 0.0f, shadow_opacity = 0.0f;
  uint32_t shadow_color = 0x000000;
  float box_opacity = 0.0f, box_padding = 0.3f, box_radius = 0.3f;
  uint32_t box_color = 0x000000;
  // Captions: a text with timed words (content.words) shows one word at a time, each from its start until the next one starts (the
  // last until the end of the clip), and pops in: it grows from (1 - 0.35 * word_pop) of its size over 5 frames. A word may have a colour of its own.
  struct Word {
    int64_t start = 0, end = 0; // frames after the clip's start
    std::string text;
    int64_t color = -1;         // 0xRRGGBB, or -1 for the text's colour
  };
  std::vector<Word> words;
  float word_pop = 0.0f;
  // Effects. On an adjustment layer (media_ref.type "adjustment", no picture of its own) they change everything below
  // it, and opacity mixes the changed picture with the unchanged one. On a clip they change only the clip: it is drawn
  // on its own, changed, and composited with its blurred coverage, so its edges soften into what is below.
  bool is_adjustment = false;
  std::vector<Effect> effects;
  // Dissolve (attome.dissolve) from this clip into the next one on its track. compile() lengthens both clips into the
  // media beyond the cut, so over [mix_start, mix_start + mix_frames) both are active; each is drawn over the layers
  // below and the two results are mixed by progress. Indices are into Composition::layers.
  int mix_with = -1; // on the outgoing clip: the incoming one
  int mixed_by = -1; // on the incoming clip: the outgoing one
  int64_t mix_start = 0, mix_frames = 0; // on the outgoing clip
  bool mixing_at(int64_t frame) const { return frame >= mix_start && frame < mix_start + mix_frames; }
  // On the outgoing clip: how the two clips mix. A wipe and a push have the incoming clip enter from `mix_dir` (an
  // eval::WipeDirection); a wipe's edge is `mix_softness` of the picture wide; a zoom changes the pictures by
  // `mix_amount`, and `mix_dir` is its eval::ZoomDirection.
  eval::TransitionKind mix_kind = eval::TransitionKind::dissolve;
  int mix_dir = 0;
  float mix_softness = 0.1f;
  float mix_amount = 0.5f;
  // Keyframes (transform.keyframes) replace the static values above while they exist. Their times are clip-local:
  // 0 is `origin_frame`, the clip's record_in (start_frame moves earlier when a dissolve leads into the clip).
  int64_t origin_frame = 0;
  eval::Curve opacity_keys, position_keys, scale_keys, rotation_keys, anchor_keys;
};

// The parameters of an effect of a layer at one frame, keyframes applied, in the order of the effect's table entry.
struct Composition;
std::array<float, eval::kMaxEffectParams> effect_values(const Layer &layer, const Effect &effect, const Composition &comp, int64_t frame);

// The transform of a layer at one frame, keyframes applied.
struct Pose {
  float opacity = 1.0f;
  Transform xf;
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
// `project_dir` is the project folder: the files of generated Takes are recorded relative to it, so a project can be
// moved. Without it such a path is used as it is.
Result<Composition> compile(const nlohmann::json &project, std::string_view sequence_id = {}, const std::string &project_dir = {});

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
  // Pixel size of the last drawn text of a clip at this renderer's output size, or {0, 0}.
  std::pair<int, int> text_extent(const std::string &clip_id) const;
  // Changes the transform of one clip in place; decoders stay open, so this is cheap enough to do on every mouse move.
  void set_transform(const std::string &clip_id, const Transform &xf);
  // The same for the other sliders of the Inspector: one parameter of an effect (its keys are dropped, the live value
  // holds), a clip's opacity (or an adjustment layer's amount), a text clip's size and colour.
  void set_effect_param(const std::string &effect_id, int param, float value);
  void set_opacity(const std::string &clip_id, float opacity);
  void set_text_style(const std::string &clip_id, float size, uint32_t color);

private:
  Composition comp_;
  int width_, height_;
  std::unordered_map<std::string, std::unique_ptr<media::VideoReader>> readers_; // by clip ID
  // A clip played backwards: a decoder reads forwards, so a run of frames is decoded in one go (one seek, then forwards) and kept, and
  // the frames are shown from the cache in the other order. By clip ID.
  struct BackCache {
    int64_t first = -1;                    // the clip-relative frame of frames[0]
    std::vector<std::vector<uint8_t>> frames; // packed NV12
    int width = 0, height = 0;
  };
  std::unordered_map<std::string, BackCache> back_;
  std::unordered_map<std::string, bool> failed_;
  struct TextEntry {
    std::string key;
    media::TextBitmap bitmap, outline, shadow, box; // the text, its grown copy, its soft copy, and the rounded box behind it
  };
  std::unordered_map<std::string, TextEntry> text_; // by clip ID
  std::unordered_map<std::string, media::Still> stills_; // picture clips, read once, by clip ID
  std::vector<uint8_t> mix_;                          // the incoming clip of a dissolve, drawn over the same background
  std::vector<uint8_t> adjust_, scratch_;             // an adjustment layer's copy of the picture below it
  std::vector<uint8_t> over_black_, over_white_, cover_; // a clip with effects, drawn on its own (see draw_isolated)
  std::string warning_;
  std::unordered_map<std::string, std::shared_ptr<const BakedLut>> luts_; // by path; null when the file would not load
  const BakedLut *lut_for(const Effect &e); // loads and bakes on first use; warns once when the file is missing
  // `raw` draws the layer at full opacity without its effects (the isolated pass of a clip with effects).
  void draw(const Layer &l, int64_t frame, uint8_t *out, bool &cleared, std::vector<const std::string *> &used,
            bool raw = false);
  void draw_isolated(const Layer &l, int64_t frame, uint8_t *out, bool &cleared, std::vector<const std::string *> &used,
                     float opacity);
};

// The whole sequence as 48 kHz stereo float.
Result<std::vector<float>> mix_audio(const Composition &composition);

} // namespace atm::render
