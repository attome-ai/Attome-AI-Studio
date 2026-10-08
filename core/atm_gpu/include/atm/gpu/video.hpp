#pragma once
// H.264 decoded on the GPU (Vulkan Video), on the device of a Context: the pictures are decoded where the effects run.
// The program reads the file and the headers (atm/media: VideoStream, h264.hpp) and keeps the reference pictures; the GPU
// decodes the slices. Decoding is exact (the standard fixes every output byte), so a picture is the same as Media
// Foundation's or FFmpeg's.

#include <cstdint>
#include <memory>
#include <span>

#include "atm/base/error.hpp"
#include "atm/gpu/gpu.hpp"

namespace atm::gpu {

class VideoDecoder {
public:
  // A decoder for the stream whose parameter sets are in `header` (Annex B: the container's sequence header). It fails
  // when the GPU cannot decode the stream (no video queue, a profile, size or kind it does not take): the CPU decodes it
  // then. The decoder uses `gpu` from the thread that uses `gpu`, and must go before it.
  static Result<std::unique_ptr<VideoDecoder>> create(Context &gpu, std::span<const uint8_t> header);
  ~VideoDecoder();

  int width() const;  // the picture shown (after the crop)
  int height() const;

  // One access unit (the NAL units of one picture, Annex B), in decoding order, with its presentation time. Parameter
  // sets in it are taken too. Read the pictures that are ready (next) before the next call.
  Result<void> decode(std::span<const uint8_t> access_unit, int64_t pts);
  // The next picture in the order they are shown, packed NV12 (width() x height()) into `nv12`; false when none is ready
  // yet. A picture is ready once no picture shown before it can still come (or after flush).
  Result<bool> next(uint8_t *nv12, int64_t *pts);
  // The end of the stream, or a seek: every picture held becomes ready, and decoding starts again at a key picture.
  void flush();

  struct Impl;

private:
  VideoDecoder() = default;
  std::unique_ptr<Impl> impl_;
};

} // namespace atm::gpu
