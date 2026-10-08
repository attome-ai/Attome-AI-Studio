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
  enum class Kind { blur, sharpen, table, vignette, grain, lut };
  Kind kind = Kind::blur;
  float sigma = 0.0f;          // blur, sharpen: the blur's sigma in pixels
  float amount = 0.0f;         // sharpen: the amount; grain: the amplitude (strength * 48); lut: the strength
  uint32_t cell = 1, seed = 0; // grain: the size of a grain in pixels, the frame's seed
  // table: 512 words, the new byte of each luma value (0..255) then of each chroma value;
  // vignette: floats as their bits: the mask (1025), x terms (width), y terms (height), chroma x (width / 2), chroma y (height / 2).
  std::vector<uint32_t> table;
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
  // (packed NV12) by end_frame. The pictures must stay held (not released) until end_frame has returned; a frame holds
  // one LUT at most.
  void begin_frame(int width, int height);
  // A decoded picture drawn over the frame.
  void draw_picture(const VideoDecoder &decoder, const Picture &picture, PictureDraw draw);
  // A clip with effects, as the renderer draws it: on its own over black (`draw` at full opacity), and over white for its
  // coverage unless it covers the whole frame (`everywhere`); its chain run as run_effects does with that coverage; then
  // put over the frame with `opacity` (0..255) through the coverage.
  void draw_clip(const VideoDecoder &decoder, const Picture &picture, PictureDraw draw, std::vector<Effect> chain, bool everywhere, int opacity);
  // An adjustment layer: its chain run on the frame so far, mixed with it by `amount` (0..256).
  void adjust(std::vector<Effect> chain, int amount);
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
