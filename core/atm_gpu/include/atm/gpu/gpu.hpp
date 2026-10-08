#pragma once
// atm_gpu (M10, ADR-009): the Vulkan compute path of the renderer. A Context owns the device, its queue, the pipelines
// and the buffers that are kept between frames (no allocation per frame). Every operation here has a CPU twin in
// atm_render that it is checked against, byte for byte where the arithmetic allows it.

#include <cstdint>
#include <memory>
#include <string>

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

class Context {
public:
  // A Vulkan 1.3 device (with synchronization2): the discrete GPU when there is one. ATTOME_GPU=off refuses (the CPU
  // path is used); ATTOME_GPU_DEVICE=<n> picks the n-th device Vulkan lists.
  // `device` is an index from list_devices(); -1 takes the discrete GPU first (ATTOME_GPU_DEVICE overrides that).
  static Result<std::unique_ptr<Context>> create(int device = -1);
  ~Context();
  const Info &info() const;

  // The renderer's blur of a packed NV12 picture, in place: three box passes each way on the luma (radius from `sigma`)
  // and on the chroma (radius from sigma / 2), integer arithmetic, edges clamped. The same bytes as render::blur_picture.
  Result<void> blur_nv12(uint8_t *nv12, int width, int height, float sigma, Timing *timing = nullptr);

  struct Impl;

private:
  Context() = default;
  std::unique_ptr<Impl> impl_;
};

// The box radius the blur uses for a Gaussian of `sigma` pixels (three passes), shared with the CPU path.
int box_radius(float sigma);

} // namespace atm::gpu
