#pragma once
// atm_gpu (M10, ADR-009): the Vulkan compute path of the renderer. A Context owns the device, its queue, the pipelines
// and the buffers that are kept between frames (no allocation per frame). Every operation here has a CPU twin in
// atm_render that it is checked against, byte for byte where the arithmetic allows it.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::gpu {

struct Info {
  std::string device;  // "NVIDIA GeForce RTX 5090"
  std::string driver;  // "NVIDIA 591.74"
  uint32_t api = 0;    // the device's Vulkan version (VK_MAKE_API_VERSION)
  bool discrete = false;
  int index = -1;      // in list_devices()
};

// A GPU Vulkan reports, whether Attome can render on it, and why not when it cannot.
struct Device {
  int index = -1;           // Vulkan's order; pass it to Context::create
  std::string name, driver; // "NVIDIA GeForce RTX 5090", "NVIDIA 591.74"
  uint32_t vendor_id = 0, device_id = 0;
  uint32_t api = 0;
  bool discrete = false, integrated = false;
  uint64_t memory_mb = 0;   // its own memory (device-local heaps)
  bool usable = false;
  std::string why_not;      // empty when usable
};
// Every GPU Vulkan reports (empty when there is no Vulkan, or this build has no GPU path). Cheap: no device is made.
std::vector<Device> list_devices();

// Where the time of the last operation went, in microseconds: copying into and out of the mapped staging memory on
// the CPU, the GPU's own time for the transfers and for the compute (from timestamp queries), and the whole call.
struct Timing {
  double upload_us = 0.0, gpu_copy_us = 0.0, compute_us = 0.0, download_us = 0.0, total_us = 0.0;
  double rows_us = 0.0, columns_us = 0.0; // the compute, split: the horizontal passes and the vertical ones
};

// One effect of a chain run on the GPU (Context::run_effects). The numbers that must match the CPU exactly come from the
// renderer, made by the same code its CPU path uses: the GPU only applies them.
struct Effect {
  enum class Kind { blur, sharpen, table, vignette, grain, lut, luma_key, chroma_key };
  Kind kind = Kind::blur;
  float sigma = 0.0f;          // blur, sharpen: the blur's sigma in pixels
  float amount = 0.0f;         // sharpen: the amount; grain: the amplitude (strength * 48); lut: the strength
  uint32_t cell = 1, seed = 0; // grain: the size of a grain in pixels, the frame's seed
  // table: 512 words, the new byte of each luma value (0..255) then of each chroma value; luma_key: 256 words, how much
  // of each luma value is kept (0..255; it needs the clip's coverage);
  // vignette: floats as their bits: the mask (1025), x terms (width), y terms (height), chroma x (width / 2), chroma y (height / 2);
  // chroma_key: the renderer's ChromaKeyTables words (it needs the clip's coverage).
  std::vector<uint32_t> table;
  uint32_t key_start = 0;  // chroma_key: how far from the screen a thin line must be to be kept (ChromaKeyTables::start)
  bool key_detail = false; // chroma_key: the fine detail pass runs
  // lut: the baked table, 33^3 nodes of video-range YUV (Y fastest), and an id that changes when the table does: it stays
  // in the GPU's memory between calls and is sent again only when the id changes. Not owned; it must live through the call.
  const float *lut = nullptr;
  size_t lut_floats = 0;
  uint64_t lut_id = 0;
};

class VideoDecoder;

// A picture a VideoDecoder decoded, kept in the GPU's memory until the decoder releases it.
struct Picture {
  int64_t pts = 0;
  int slot = -1;
};

// Where a picture lands in a frame drawn on the GPU (Context::draw_picture), worked out by the renderer as its CPU drawing
// does: for each plane the rectangle written (luma pixels; chroma U/V pairs and rows) and, for each of its columns and
// rows, a tap: the two picture samples and the weight of the second (i0 | weight << 16 | (i1 - i0) << 25, weight 0..256).
struct PictureDraw {
  int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  int cx0 = 0, cx1 = 0, cy0 = 0, cy1 = 0;
  std::vector<uint32_t> luma_x, luma_y, chroma_x, chroma_y;
  int alpha = 256; // opacity, 0..256
};

// A picture or a text mask placed anywhere (turned, cropped), worked out by the renderer as its CPU drawing does (its
// Placement): where each output point's picture point lies, the box of output pixels it can touch, its opacity, and a
// mask's colour. The GPU does the same arithmetic per pixel.
struct Placed {
  float px = 0.0f, py = 0.0f, cos_r = 1.0f, sin_r = 0.0f, inv_sx = 1.0f, inv_sy = 1.0f, ax = 0.0f, ay = 0.0f;
  float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f, sx = 1.0f, sy = 1.0f;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the box, in luma pixels
  int alpha = 256;                    // 0..256
  bool cropped = false;               // a mask: its cut edges fade
  uint8_t y = 16, u = 128, v = 128;   // a mask's colour
};

// Pixels of the CPU's that the GPU keeps between frames by `key` (the same key always means the same pixels): a still
// (NV12, with an alpha plane when it has one) or a text mask (`y` only: one byte of coverage per pixel).
struct Source {
  uint64_t key = 0;
  int width = 0, height = 0;
  const uint8_t *y = nullptr, *uv = nullptr, *alpha = nullptr;
  int y_pitch = 0, uv_pitch = 0, alpha_pitch = 0;
  bool mask = false;
};

// A transition at one frame, worked out by the renderer (its Mix): the kind, its one number (a dissolve's or a zoom's
// weight of the incoming picture, 0..256; how far a push or a slide has come in, in luma pixels) and its tables, as
// transition.comp reads them.
struct Transition {
  enum class Kind { dissolve, wipe, push, slide, iris, zoom };
  Kind kind = Kind::dissolve;
  int amount = 0;
  bool horizontal = false, from_start = false;
  std::vector<uint32_t> table;
};

class Context {
public:
  // A Vulkan 1.3 device (with synchronization2): the discrete GPU when there is one. ATTOME_GPU=off refuses (the CPU
  // path is used); ATTOME_GPU_DEVICE=<n> picks the n-th device Vulkan lists.
  // `device` is an index from list_devices(); -1 takes the discrete GPU first (ATTOME_GPU_DEVICE overrides that).
  // When the GPU decodes H.264 through Vulkan Video, the device gets that queue too (VideoDecoder, video.hpp).
  static Result<std::unique_ptr<Context>> create(int device = -1);
  ~Context();
  const Info &info() const;

  // The renderer's blur of a packed NV12 picture, in place: three box passes each way on the luma (radius from `sigma`)
  // and on the chroma (radius from sigma / 2), integer arithmetic, edges clamped. The same bytes as render::blur_picture.
  Result<void> blur_nv12(uint8_t *nv12, int width, int height, float sigma, Timing *timing = nullptr);

  // A chain of effects on a packed NV12 picture, in place: the picture goes to the GPU once, every effect runs there in
  // order, and it comes back once. With `cover` (width x height bytes) the picture is a clip drawn on its own over black and
  // this its coverage: a blur blurs the coverage too, and after each colour effect the change is scaled back by it, as the
  // CPU does; the coverage comes back as well.
  Result<void> run_effects(uint8_t *nv12, int width, int height, const std::vector<Effect> &chain, Timing *timing = nullptr,
                           uint8_t *cover = nullptr, uint8_t *result = nullptr);
  // The memory the next run_effects reads its picture from (packed NV12, width x height), mapped for the CPU and cached:
  // a picture drawn straight into it (and passed as run_effects' nv12) is not copied on its way in. `result` (when given)
  // is where the picture comes back to, instead of nv12. Null when the buffers cannot be made.
  uint8_t *staging(int width, int height);

  // A frame drawn on the GPU: begun black, then what is put on it in order, made in one go and read back into `nv12`
  // (packed NV12) by end_frame. The pictures must stay held (not released), and the sources' pixels unchanged, until
  // end_frame has returned; a frame holds one LUT at most.
  void begin_frame(int width, int height);
  // Drawings over the frame: a decoded picture upright or placed, a still or a text mask placed. A decoded picture can be
  // halved `halvings` times first (each pixel the mean of 2 x 2, as the renderer's half_nv12): `draw` and `placed` are
  // for the halved picture.
  void draw_picture(const VideoDecoder &decoder, const Picture &picture, PictureDraw draw, int halvings = 0);
  void draw_picture(const VideoDecoder &decoder, const Picture &picture, const Placed &placed, int halvings = 0);
  void draw_source(const Source &source, PictureDraw draw);
  void draw_source(const Source &source, const Placed &placed);
  // A clip with effects, as the renderer draws it: the drawings between begin_clip and end_clip make the clip on its own
  // (at full opacity), over black, and over white for its coverage unless it covers the whole frame (`everywhere`); its
  // chain runs as run_effects does with that coverage; then it goes over the frame with `opacity` (0..255).
  void begin_clip(std::vector<Effect> chain, bool everywhere, int opacity);
  void end_clip();
  // An adjustment layer: its chain run on the frame so far, mixed with it by `amount` (0..256).
  void adjust(std::vector<Effect> chain, int amount);
  // A transition between two clips, each drawn over the frame so far: after begin_transition the outgoing clip is drawn,
  // after begin_incoming the incoming one (over a copy of the frame as it was), and end_transition mixes the two.
  void begin_transition();
  void begin_incoming();
  void end_transition(Transition transition);
  Result<void> end_frame(uint8_t *nv12);

  struct Impl;

private:
  friend class VideoDecoder; // decodes on the same device
  Context() = default;
  std::unique_ptr<Impl> impl_;
};

// The box radius the blur uses for a Gaussian of `sigma` pixels (three passes), shared with the CPU path.
int box_radius(float sigma);

} // namespace atm::gpu
