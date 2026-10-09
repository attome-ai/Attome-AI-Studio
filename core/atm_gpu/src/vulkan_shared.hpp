#pragma once
// What the Vulkan parts of atm_gpu share: a Context's device and queues, and how buffers are made. The effects
// (gpu_vulkan.cpp) and the video decoder (video_vulkan.cpp) work on the same device, so a decoded picture can be used where
// it is, without a copy.

#include <string>

#include <vulkan/vulkan.h>

#include "atm/base/error.hpp"
#include "atm/gpu/gpu.hpp"
#include "atm/gpu/video.hpp"

namespace atm::gpu {

tl::unexpected<Error> vk_fail(const char *what, VkResult r);

#define VK_TRY(what, call)                                                                                                                 \
  do {                                                                                                                                     \
    if (const VkResult vk_result_ = (call); vk_result_ != VK_SUCCESS)                                                                      \
      return vk_fail(what, vk_result_);                                                                                                    \
  } while (0)

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void *mapped = nullptr;
};

struct Vulkan {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE; // compute (and copies)
  uint32_t family = 0;
  // A queue that decodes H.264 (Vulkan Video), when the GPU and its driver have one; -1 when not.
  int video_family = -1;
  VkQueue video_queue = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_props{};
  // A timeline semaphore per queue counts the work sent to it: `submitted` (compute) and `video_submitted` are their
  // values once the last work sent has finished. Work on one queue waits for what it needs from the other, no more, so
  // decoding the next pictures goes on while a frame is drawn.
  VkSemaphore timeline = VK_NULL_HANDLE, video_timeline = VK_NULL_HANDLE;
  uint64_t submitted = 0, video_submitted = 0;
  // The compute work that last read decoded pictures: a decode into a slot waits for it (the slot may be one it read).
  uint64_t pictures_read = 0;

  // Sends `cmd` to `on` (the compute queue or the video queue), after the compute work up to `after_compute` and the
  // video work up to `after_video` (0: none), and moves that queue's timeline on.
  Result<void> submit(VkQueue on, VkCommandBuffer cmd, uint64_t after_compute = 0, uint64_t after_video = 0);
  // Until a timeline reaches `value`; fails when the GPU has not got there in 5 s (it is stuck).
  Result<void> wait(uint64_t value);
  Result<void> wait_video(uint64_t value);
  // The first memory type that `bits` allows with all of `want`; with `nice` too when there is one.
  int memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice = 0) const;
  // A buffer of at least `size` bytes; one that is already big enough is kept. `next` goes in its create info's pNext.
  Result<void> ensure(Buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice = 0,
                      const void *next = nullptr);
  void release(Buffer &buf);
};

// The device of a Context (for the parts of atm_gpu that are not the effects).
Vulkan &vulkan_of(Context::Impl &impl);

// A decoded picture where it lies: a layer of an image, in the layout it is kept in, and the part of it that is shown;
// with the video timeline's value once it is decoded. `shared`: the image holds the decoder's references too (its
// output and its references coincide), so reading it waits for all the decoding sent.
struct PictureImage {
  VkImage image = VK_NULL_HANDLE;
  uint32_t layer = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  int x = 0, y = 0, width = 0, height = 0;
  uint64_t decoded = 0;
  bool shared = false;
};
PictureImage picture_image(const VideoDecoder::Impl &decoder, int slot);

// Records copying the shown part of a picture into `dst` at `offset` as packed NV12 with rows `pitch` bytes apart (even),
// the chroma rows right after the luma's; the picture's layer goes back to its layout after.
void record_picture_copy(VkCommandBuffer cmd, const PictureImage &picture, VkBuffer dst, VkDeviceSize offset, uint32_t pitch);

} // namespace atm::gpu
