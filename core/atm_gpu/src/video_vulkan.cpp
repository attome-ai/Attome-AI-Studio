#if defined(ATM_GPU_VULKAN)
// H.264 decoding with Vulkan Video (VK_KHR_video_decode_h264), on the device of a Context.
//
// The GPU decodes the slices; the program gives it the parameter sets (as session parameters), each picture's order
// counts and frame number, and which earlier pictures it may refer to. Those reference pictures are kept here, by the
// rules of the standard (8.2.5: the sliding window and the memory management operations), and the pictures are shown in
// the order of their counts (the "bumping" of C.4.5).
//
// The pictures live in an image of many layers, one layer per DPB slot: a slot is the picture decoded into it, kept while
// it is a reference or not shown yet. Where the decoder's references and its output pictures coincide (NVIDIA), that one
// image serves both; where they are apart (AMD), a second image of as many layers takes the output, and slot k is layer k
// of both. The decode queue and the compute queue (which reads pictures out) take turns on one timeline semaphore.

#include "atm/gpu/video.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

#include "atm/base/profiler.hpp"
#include "atm/media/h264.hpp"
#include "vulkan_shared.hpp"

namespace atm::gpu {
namespace h264 = media::h264;
namespace {

constexpr uint64_t kWaitNs = 2'000'000'000; // GPU work that has not finished in 2 s is stuck: fail rather than wait forever
constexpr uint32_t kMaxSlots = 17;          // 16 references and the picture being decoded

tl::unexpected<Error> cannot(const std::string &why) {
  return fail(ErrorCode::GpuUnsupported, "G_NO_VIDEO_DECODE", "The GPU cannot decode this video: " + why + ".", {}, "The CPU decodes it.");
}

tl::unexpected<Error> bad_stream(const std::string &why) {
  return fail(ErrorCode::MediaDecodeFailed, "G_H264", "The H.264 stream could not be decoded on the GPU: " + why + ".");
}

StdVideoH264LevelIdc level_of(int level_idc) {
  static constexpr int kLevels[] = {10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
  for (size_t i = 0; i < std::size(kLevels); ++i)
    if (kLevels[i] >= level_idc)
      return StdVideoH264LevelIdc(i);
  return STD_VIDEO_H264_LEVEL_IDC_6_2;
}

// How many frames a stream may hold back before showing them, when its VUI does not say: the DPB of its level (A.3.1).
int dpb_frames(const h264::Sps &s) {
  static constexpr std::pair<int, int> kMaxDpbMbs[] = {{10, 396}, {11, 900}, {12, 2376}, {13, 2376}, {20, 2376}, {21, 4752}, {22, 8100},
                                                       {30, 8100}, {31, 18000}, {32, 20480}, {40, 32768}, {41, 32768}, {42, 34816},
                                                       {50, 110400}, {51, 184320}, {52, 184320}};
  int mbs = 696320; // level 6 and above
  for (const auto &[level, max] : kMaxDpbMbs)
    if (level >= s.level_idc) {
      mbs = max;
      break;
    }
  return std::clamp(mbs / std::max(1, s.pic_width_in_mbs * s.pic_height_in_map_units), 1, 16);
}

StdVideoH264ScalingLists scaling_lists(uint16_t present, uint16_t use_default, const std::array<std::array<uint8_t, 16>, 6> &l4,
                                       const std::array<std::array<uint8_t, 64>, 6> &l8) {
  StdVideoH264ScalingLists out{};
  out.scaling_list_present_mask = present;
  out.use_default_scaling_matrix_mask = use_default;
  for (size_t i = 0; i < 6; ++i) {
    std::memcpy(out.ScalingList4x4[i], l4[i].data(), 16);
    std::memcpy(out.ScalingList8x8[i], l8[i].data(), 64);
  }
  return out;
}

// The functions of the video extensions (the loader exports only the core ones).
struct VideoFunctions {
  PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR capabilities = nullptr;
  PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR formats = nullptr;
  PFN_vkCreateVideoSessionKHR create_session = nullptr;
  PFN_vkDestroyVideoSessionKHR destroy_session = nullptr;
  PFN_vkGetVideoSessionMemoryRequirementsKHR session_memory = nullptr;
  PFN_vkBindVideoSessionMemoryKHR bind_session_memory = nullptr;
  PFN_vkCreateVideoSessionParametersKHR create_parameters = nullptr;
  PFN_vkDestroyVideoSessionParametersKHR destroy_parameters = nullptr;
  PFN_vkCmdBeginVideoCodingKHR begin = nullptr;
  PFN_vkCmdEndVideoCodingKHR end = nullptr;
  PFN_vkCmdControlVideoCodingKHR control = nullptr;
  PFN_vkCmdDecodeVideoKHR decode = nullptr;

  bool load(VkInstance instance, VkDevice device) {
    const auto inst = [&](auto &fn, const char *name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(vkGetInstanceProcAddr(instance, name)); };
    const auto dev = [&](auto &fn, const char *name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(vkGetDeviceProcAddr(device, name)); };
    inst(capabilities, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    inst(formats, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
    dev(create_session, "vkCreateVideoSessionKHR");
    dev(destroy_session, "vkDestroyVideoSessionKHR");
    dev(session_memory, "vkGetVideoSessionMemoryRequirementsKHR");
    dev(bind_session_memory, "vkBindVideoSessionMemoryKHR");
    dev(create_parameters, "vkCreateVideoSessionParametersKHR");
    dev(destroy_parameters, "vkDestroyVideoSessionParametersKHR");
    dev(begin, "vkCmdBeginVideoCodingKHR");
    dev(end, "vkCmdEndVideoCodingKHR");
    dev(control, "vkCmdControlVideoCodingKHR");
    dev(decode, "vkCmdDecodeVideoKHR");
    return capabilities && formats && create_session && destroy_session && session_memory && bind_session_memory && create_parameters &&
           destroy_parameters && begin && end && control && decode;
  }
};

// An image of one layer per slot, made for the decode queue and the compute queue both.
struct Layers {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

// A DPB slot: the picture decoded into a layer of the image, as a reference and as a picture to show.
struct Slot {
  bool reference = false, long_term = false;
  bool waiting = false; // decoded, not read out yet
  bool ready = false;   // may be read out: no picture shown before it can still come
  int frame_num = 0, long_term_idx = 0;
  h264::Poc poc;        // as a reference
  int64_t period = 0;   // pictures from one IDR (or operation 5) to the next show in the order of their counts
  int order = 0;
  int64_t pts = 0;
  bool used() const { return reference || waiting; }
};

} // namespace

struct VideoDecoder::Impl {
  Vulkan &vk;
  VideoFunctions fn;
  // The parameter sets by id: the bytes (to see a change) and what they say.
  std::map<int, std::vector<uint8_t>> sps_bytes, pps_bytes;
  std::vector<h264::Sps> sps;
  std::vector<h264::Pps> pps;
  bool parameters_changed = false;
  h264::PocCounter counter;

  VkVideoDecodeH264ProfileInfoKHR h264_profile{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
  VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
  VkVideoProfileListInfoKHR profiles{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
  VkVideoDecodeH264CapabilitiesKHR h264_caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
  VkVideoDecodeCapabilitiesKHR decode_caps{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
  VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};

  VkVideoSessionKHR session = VK_NULL_HANDLE;
  std::vector<VkDeviceMemory> session_memory;
  VkVideoSessionParametersKHR parameters = VK_NULL_HANDLE;
  Layers dpb, output; // output: only when the decoder writes its pictures apart from its references
  VkImageLayout output_layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR; // where a picture is read from, in the layout it is decoded in
  VkExtent2D coded{};
  int width = 0, height = 0, crop_x = 0, crop_y = 0; // the picture shown, and where it starts in the coded one
  int reorder = 16; // pictures that may wait to be shown
  Buffer bitstream, download;
  VkCommandPool video_pool = VK_NULL_HANDLE, copy_pool = VK_NULL_HANDLE;
  VkCommandBuffer video_cmd = VK_NULL_HANDLE, copy_cmd = VK_NULL_HANDLE;
  VkSemaphore timeline = VK_NULL_HANDLE;
  uint64_t submitted = 0; // the timeline's value once the last work sent has finished
  bool started = false;   // the session was reset and the image laid out
  std::vector<Slot> slots;
  int64_t period = 0;
  int max_long_term_idx = -1; // -1: no long-term indices

  explicit Impl(Vulkan &v) : vk(v) {}

  ~Impl() {
    if (timeline)
      (void)wait(submitted);
    if (parameters)
      fn.destroy_parameters(vk.device, parameters, nullptr);
    if (session)
      fn.destroy_session(vk.device, session, nullptr);
    for (VkDeviceMemory m : session_memory)
      vkFreeMemory(vk.device, m, nullptr);
    for (const Layers &l : {dpb, output}) {
      vkDestroyImageView(vk.device, l.view, nullptr);
      vkDestroyImage(vk.device, l.image, nullptr);
      vkFreeMemory(vk.device, l.memory, nullptr);
    }
    vk.release(bitstream);
    vk.release(download);
    vkDestroySemaphore(vk.device, timeline, nullptr);
    vkDestroyCommandPool(vk.device, video_pool, nullptr);
    vkDestroyCommandPool(vk.device, copy_pool, nullptr);
  }

  Result<void> wait(uint64_t value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &value;
    ATM_PROFILE_SCOPE("video.wait");
    VK_TRY("vkWaitSemaphores", vkWaitSemaphores(vk.device, &wi, kWaitNs));
    return {};
  }

  // Sends a recorded command buffer to `queue`, after all the work sent before it, and moves the timeline on.
  Result<void> submit(VkQueue queue, VkCommandBuffer cmd) {
    VkSemaphoreSubmitInfo after{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, done{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    after.semaphore = done.semaphore = timeline;
    after.value = submitted;
    done.value = submitted + 1;
    after.stageMask = done.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &after;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &done;
    VK_TRY("vkQueueSubmit2", vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
    ++submitted;
    return {};
  }

  // The SPS and PPS units of `data`, kept by id; a set that changed marks the session parameters for remaking.
  Result<void> take_parameter_sets(std::span<const uint8_t> data) {
    for (const h264::Nal &nal : h264::split_annex_b(data)) {
      if (nal.type != 7 && nal.type != 8)
        continue;
      std::vector<uint8_t> payload = h264::rbsp(nal.bytes);
      h264::Error error;
      if (nal.type == 7) {
        auto s = h264::parse_sps(payload, &error);
        if (!s)
          return bad_stream(error.message);
        if (auto &known = sps_bytes[s->id]; known != payload) {
          known = std::move(payload);
          std::erase_if(sps, [&](const h264::Sps &x) { return x.id == s->id; });
          sps.push_back(*s);
          parameters_changed = true;
        }
      } else {
        auto p = h264::parse_pps(payload, sps, &error);
        if (!p)
          return bad_stream(error.message);
        if (auto &known = pps_bytes[p->id]; known != payload) {
          known = std::move(payload);
          std::erase_if(pps, [&](const h264::Pps &x) { return x.id == p->id; });
          pps.push_back(*p);
          parameters_changed = true;
        }
      }
    }
    return {};
  }

  // The NV12 format the decoder takes for images of `usage`, with how such an image is made; null when there is none.
  std::optional<VkVideoFormatPropertiesKHR> nv12_format(VkImageUsageFlags usage) const {
    VkPhysicalDeviceVideoFormatInfoKHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
    info.pNext = &profiles;
    info.imageUsage = usage;
    uint32_t count = 0;
    fn.formats(vk.physical, &info, &count, nullptr);
    std::vector<VkVideoFormatPropertiesKHR> formats(count, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
    fn.formats(vk.physical, &info, &count, formats.data());
    for (const VkVideoFormatPropertiesKHR &f : formats)
      if (f.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
        return f;
    return std::nullopt;
  }

  // An image of a layer per slot, shared by the decode queue (writes) and the compute queue (reads), and a view of it
  // for the decoder.
  Result<void> make_layers(Layers &l, const VkVideoFormatPropertiesKHR &format, VkImageUsageFlags usage, VkImageUsageFlags view_usage) {
    const uint32_t families[2] = {uint32_t(vk.video_family), vk.family};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.pNext = &profiles;
    ii.flags = format.imageCreateFlags;
    ii.imageType = format.imageType;
    ii.format = format.format;
    ii.extent = {coded.width, coded.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = uint32_t(slots.size());
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = format.imageTiling;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ii.queueFamilyIndexCount = 2;
    ii.pQueueFamilyIndices = families;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_TRY("vkCreateImage", vkCreateImage(vk.device, &ii, nullptr, &l.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.device, l.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(vk.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    VK_TRY("vkAllocateMemory", vkAllocateMemory(vk.device, &ai, nullptr, &l.memory));
    VK_TRY("vkBindImageMemory", vkBindImageMemory(vk.device, l.image, l.memory, 0));
    VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    vu.usage = view_usage;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.pNext = &vu;
    vi.image = l.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = format.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, uint32_t(slots.size())};
    VK_TRY("vkCreateImageView", vkCreateImageView(vk.device, &vi, nullptr, &l.view));
    return {};
  }

  // The session for pictures like `s`: its profile, the images of the slots, the memory, the queues' tools.
  Result<void> init(const h264::Sps &s) {
    if (const std::string why = h264::gpu_unsupported(s); !why.empty())
      return cannot(why);
    if (!fn.load(vk.instance, vk.device))
      return cannot("the driver lacks the video functions");
    // Constrained Baseline is a part of Main; full Baseline (slice groups and the like) is its own profile.
    const bool constrained_baseline = s.profile_idc == 66 && (s.constraint_flags & 0x40);
    if (s.profile_idc != 66 && s.profile_idc != 77 && s.profile_idc != 100)
      return cannot("profile " + std::to_string(s.profile_idc) + " (Baseline, Main and High are decoded)");
    h264_profile.stdProfileIdc = constrained_baseline ? STD_VIDEO_H264_PROFILE_IDC_MAIN : StdVideoH264ProfileIdc(s.profile_idc);
    h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    profile.pNext = &h264_profile;
    profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile.lumaBitDepth = profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profiles.profileCount = 1;
    profiles.pProfiles = &profile;
    decode_caps.pNext = &h264_caps;
    caps.pNext = &decode_caps;
    if (const VkResult r = fn.capabilities(vk.physical, &profile, &caps); r != VK_SUCCESS)
      return cannot("its decoder does not take this profile (" + std::to_string(int(r)) + ")");
    if (level_of(s.level_idc) > h264_caps.maxLevelIdc)
      return cannot("level " + std::to_string(s.level_idc) + " is above the decoder's");
    coded = {uint32_t(s.coded_width()), uint32_t(s.coded_height())};
    if (coded.width < caps.minCodedExtent.width || coded.height < caps.minCodedExtent.height || coded.width > caps.maxCodedExtent.width ||
        coded.height > caps.maxCodedExtent.height)
      return cannot(std::to_string(coded.width) + "x" + std::to_string(coded.height) + " is outside the sizes its decoder takes");
    width = s.width();
    height = s.height();
    crop_x = 2 * s.crop_left;
    crop_y = 2 * s.crop_top;
    reorder = s.num_reorder_frames >= 0 ? s.num_reorder_frames : s.max_dec_frame_buffering >= 0 ? s.max_dec_frame_buffering : dpb_frames(s);

    // The images: two planes (NV12), decoded into and read out of; one image for both where the decoder allows it.
    constexpr VkImageUsageFlags kDpb = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR, kDst = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
    constexpr VkImageUsageFlags kRead = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const auto both = (decode_caps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) ? nv12_format(kDpb | kDst | kRead) : std::nullopt;
    const bool coincide = both.has_value();
    if (!coincide && !(decode_caps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR))
      return cannot("its decoder's pictures cannot be read out");
    const auto dpb_format = coincide ? both : nv12_format(kDpb);
    const auto output_format = coincide ? both : nv12_format(kDst | kRead);
    if (!dpb_format || !output_format)
      return cannot("its decoder does not give NV12 pictures that can be read out");
    slots.assign(std::min(caps.maxDpbSlots, kMaxSlots), Slot{});
    ATM_CHECK(make_layers(dpb, *dpb_format, coincide ? kDpb | kDst | kRead : kDpb, coincide ? kDpb | kDst : kDpb));
    if (!coincide) {
      ATM_CHECK(make_layers(output, *output_format, kDst | kRead, kDst));
      output_layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
    }

    VkVideoSessionCreateInfoKHR si{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    si.queueFamilyIndex = uint32_t(vk.video_family);
    si.pVideoProfile = &profile;
    si.pictureFormat = output_format->format;
    si.referencePictureFormat = dpb_format->format;
    si.maxCodedExtent = coded;
    si.maxDpbSlots = uint32_t(slots.size());
    si.maxActiveReferencePictures = std::min(caps.maxActiveReferencePictures, kMaxSlots - 1);
    si.pStdHeaderVersion = &caps.stdHeaderVersion;
    VK_TRY("vkCreateVideoSessionKHR", fn.create_session(vk.device, &si, nullptr, &session));
    uint32_t count = 0;
    fn.session_memory(vk.device, session, &count, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> needs(count, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
    fn.session_memory(vk.device, session, &count, needs.data());
    std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
    for (const VkVideoSessionMemoryRequirementsKHR &need : needs) {
      const int type = vk.memory_type(need.memoryRequirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = need.memoryRequirements.size;
      ai.memoryTypeIndex = uint32_t(type);
      VkDeviceMemory memory = VK_NULL_HANDLE;
      VK_TRY("vkAllocateMemory", vkAllocateMemory(vk.device, &ai, nullptr, &memory));
      session_memory.push_back(memory);
      VkBindVideoSessionMemoryInfoKHR bind{VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
      bind.memoryBindIndex = need.memoryBindIndex;
      bind.memory = memory;
      bind.memorySize = need.memoryRequirements.size;
      binds.push_back(bind);
    }
    VK_TRY("vkBindVideoSessionMemoryKHR", fn.bind_session_memory(vk.device, session, uint32_t(binds.size()), binds.data()));

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = uint32_t(vk.video_family);
    VK_TRY("vkCreateCommandPool", vkCreateCommandPool(vk.device, &pi, nullptr, &video_pool));
    pi.queueFamilyIndex = vk.family;
    VK_TRY("vkCreateCommandPool", vkCreateCommandPool(vk.device, &pi, nullptr, &copy_pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    cai.commandPool = video_pool;
    VK_TRY("vkAllocateCommandBuffers", vkAllocateCommandBuffers(vk.device, &cai, &video_cmd));
    cai.commandPool = copy_pool;
    VK_TRY("vkAllocateCommandBuffers", vkAllocateCommandBuffers(vk.device, &cai, &copy_cmd));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &type;
    VK_TRY("vkCreateSemaphore", vkCreateSemaphore(vk.device, &sci, nullptr, &timeline));
    return {};
  }

  // The session parameters, remade with every parameter set known (after the work that uses the old ones).
  Result<void> make_parameters() {
    ATM_CHECK(wait(submitted));
    for (const h264::Sps &s : sps)
      if (s.coded_width() > int(coded.width) || s.coded_height() > int(coded.height) || !h264::gpu_unsupported(s).empty())
        return cannot("the stream changes to pictures the session was not made for");
    std::vector<StdVideoH264ScalingLists> lists;
    lists.reserve(sps.size() + pps.size()); // pointers into it are kept
    std::vector<StdVideoH264SequenceParameterSet> std_sps;
    for (const h264::Sps &s : sps) {
      StdVideoH264SequenceParameterSet o{};
      o.flags.constraint_set0_flag = (s.constraint_flags >> 7) & 1;
      o.flags.constraint_set1_flag = (s.constraint_flags >> 6) & 1;
      o.flags.constraint_set2_flag = (s.constraint_flags >> 5) & 1;
      o.flags.constraint_set3_flag = (s.constraint_flags >> 4) & 1;
      o.flags.constraint_set4_flag = (s.constraint_flags >> 3) & 1;
      o.flags.constraint_set5_flag = (s.constraint_flags >> 2) & 1;
      o.flags.direct_8x8_inference_flag = s.direct_8x8_inference;
      o.flags.mb_adaptive_frame_field_flag = s.mb_adaptive_frame_field;
      o.flags.frame_mbs_only_flag = s.frame_mbs_only;
      o.flags.delta_pic_order_always_zero_flag = s.delta_pic_order_always_zero;
      o.flags.separate_colour_plane_flag = s.separate_colour_plane;
      o.flags.gaps_in_frame_num_value_allowed_flag = s.gaps_in_frame_num_allowed;
      o.flags.qpprime_y_zero_transform_bypass_flag = s.qpprime_y_zero_transform_bypass;
      o.flags.frame_cropping_flag = s.frame_cropping;
      o.flags.seq_scaling_matrix_present_flag = s.scaling_matrix_present;
      o.profile_idc = StdVideoH264ProfileIdc(s.profile_idc);
      o.level_idc = level_of(s.level_idc);
      o.chroma_format_idc = StdVideoH264ChromaFormatIdc(s.chroma_format_idc);
      o.seq_parameter_set_id = uint8_t(s.id);
      o.bit_depth_luma_minus8 = uint8_t(s.bit_depth_luma - 8);
      o.bit_depth_chroma_minus8 = uint8_t(s.bit_depth_chroma - 8);
      o.log2_max_frame_num_minus4 = uint8_t(s.log2_max_frame_num - 4);
      o.pic_order_cnt_type = StdVideoH264PocType(s.pic_order_cnt_type);
      o.offset_for_non_ref_pic = s.offset_for_non_ref_pic;
      o.offset_for_top_to_bottom_field = s.offset_for_top_to_bottom_field;
      o.log2_max_pic_order_cnt_lsb_minus4 = uint8_t(s.log2_max_pic_order_cnt_lsb - 4);
      o.num_ref_frames_in_pic_order_cnt_cycle = uint8_t(s.offset_for_ref_frame.size());
      o.max_num_ref_frames = uint8_t(s.max_num_ref_frames);
      o.pic_width_in_mbs_minus1 = uint32_t(s.pic_width_in_mbs - 1);
      o.pic_height_in_map_units_minus1 = uint32_t(s.pic_height_in_map_units - 1);
      o.frame_crop_left_offset = uint32_t(s.crop_left);
      o.frame_crop_right_offset = uint32_t(s.crop_right);
      o.frame_crop_top_offset = uint32_t(s.crop_top);
      o.frame_crop_bottom_offset = uint32_t(s.crop_bottom);
      o.pOffsetForRefFrame = s.offset_for_ref_frame.data();
      if (s.scaling_matrix_present) {
        lists.push_back(scaling_lists(s.scaling_list_present, s.use_default, s.scaling_4x4, s.scaling_8x8));
        o.pScalingLists = &lists.back();
      }
      std_sps.push_back(o);
    }
    std::vector<StdVideoH264PictureParameterSet> std_pps;
    for (const h264::Pps &p : pps) {
      StdVideoH264PictureParameterSet o{};
      o.flags.transform_8x8_mode_flag = p.transform_8x8_mode;
      o.flags.redundant_pic_cnt_present_flag = p.redundant_pic_cnt_present;
      o.flags.constrained_intra_pred_flag = p.constrained_intra_pred;
      o.flags.deblocking_filter_control_present_flag = p.deblocking_filter_control_present;
      o.flags.weighted_pred_flag = p.weighted_pred;
      o.flags.bottom_field_pic_order_in_frame_present_flag = p.bottom_field_pic_order_in_frame_present;
      o.flags.entropy_coding_mode_flag = p.entropy_coding_mode;
      o.flags.pic_scaling_matrix_present_flag = p.scaling_matrix_present;
      o.seq_parameter_set_id = uint8_t(p.sps_id);
      o.pic_parameter_set_id = uint8_t(p.id);
      o.num_ref_idx_l0_default_active_minus1 = uint8_t(p.num_ref_idx_l0_default_active - 1);
      o.num_ref_idx_l1_default_active_minus1 = uint8_t(p.num_ref_idx_l1_default_active - 1);
      o.weighted_bipred_idc = StdVideoH264WeightedBipredIdc(p.weighted_bipred_idc);
      o.pic_init_qp_minus26 = int8_t(p.pic_init_qp - 26);
      o.pic_init_qs_minus26 = int8_t(p.pic_init_qs - 26);
      o.chroma_qp_index_offset = int8_t(p.chroma_qp_index_offset);
      o.second_chroma_qp_index_offset = int8_t(p.second_chroma_qp_index_offset);
      if (p.scaling_matrix_present) {
        lists.push_back(scaling_lists(p.scaling_list_present, p.use_default, p.scaling_4x4, p.scaling_8x8));
        o.pScalingLists = &lists.back();
      }
      std_pps.push_back(o);
    }
    VkVideoDecodeH264SessionParametersAddInfoKHR add{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    add.stdSPSCount = uint32_t(std_sps.size());
    add.pStdSPSs = std_sps.data();
    add.stdPPSCount = uint32_t(std_pps.size());
    add.pStdPPSs = std_pps.data();
    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_info{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    h264_info.maxStdSPSCount = add.stdSPSCount;
    h264_info.maxStdPPSCount = add.stdPPSCount;
    h264_info.pParametersAddInfo = &add;
    VkVideoSessionParametersCreateInfoKHR pci{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    pci.pNext = &h264_info;
    pci.videoSession = session;
    if (parameters)
      fn.destroy_parameters(vk.device, parameters, nullptr);
    parameters = VK_NULL_HANDLE;
    VK_TRY("vkCreateVideoSessionParametersKHR", fn.create_parameters(vk.device, &pci, nullptr, &parameters));
    parameters_changed = false;
    return {};
  }

  VkVideoPictureResourceInfoKHR resource(const Layers &l, size_t slot) const {
    VkVideoPictureResourceInfoKHR r{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    r.codedExtent = coded;
    r.baseArrayLayer = uint32_t(slot);
    r.imageViewBinding = l.view;
    return r;
  }
  const Layers &shown() const { return output.image ? output : dpb; } // where pictures are read out of

  // The pictures waiting to be shown that come first: made ready while more than `reorder` wait, or while no slot is free
  // for the next picture.
  void bump() {
    const auto first_waiting = [&]() -> Slot * {
      Slot *best = nullptr;
      for (Slot &s : slots)
        if (s.waiting && !s.ready && (!best || std::tie(s.period, s.order) < std::tie(best->period, best->order)))
          best = &s;
      return best;
    };
    for (;;) {
      const auto held = std::count_if(slots.begin(), slots.end(), [](const Slot &s) { return s.waiting && !s.ready; });
      const bool full = std::none_of(slots.begin(), slots.end(), [](const Slot &s) { return !s.used(); });
      Slot *s = first_waiting();
      if (!s || (held <= reorder && !full))
        return;
      s->ready = true;
    }
  }

  void show_all() {
    for (Slot &s : slots)
      s.ready = s.ready || s.waiting;
    ++period;
  }

  // Which pictures stay references after the current one (8.2.5): the IDR rules, the memory management operations, or
  // the sliding window. Returns whether the current picture is kept as a long-term reference, and at which index.
  std::pair<bool, int> mark(const h264::Sps &s, const h264::SliceHeader &h) {
    if (h.idr()) {
      for (Slot &x : slots)
        x.reference = false;
      max_long_term_idx = h.long_term_reference ? 0 : -1;
      return {h.long_term_reference, 0};
    }
    const int max_frame_num = 1 << s.log2_max_frame_num;
    const auto wrap = [&](const Slot &x) { return x.frame_num > h.frame_num ? x.frame_num - max_frame_num : x.frame_num; };
    const auto short_term = [&](int pic_num) -> Slot * {
      for (Slot &x : slots)
        if (x.reference && !x.long_term && wrap(x) == pic_num)
          return &x;
      return nullptr;
    };
    const auto forget_long_term = [&](auto &&which) {
      for (Slot &x : slots)
        if (x.reference && x.long_term && which(x))
          x.reference = false;
    };
    if (!h.adaptive_ref_pic_marking) { // the sliding window: the oldest short-term reference goes when they are all used
      const auto refs = std::count_if(slots.begin(), slots.end(), [](const Slot &x) { return x.reference; });
      if (refs >= std::max(s.max_num_ref_frames, 1)) {
        Slot *oldest = nullptr;
        for (Slot &x : slots)
          if (x.reference && !x.long_term && (!oldest || wrap(x) < wrap(*oldest)))
            oldest = &x;
        if (oldest)
          oldest->reference = false;
      }
      return {false, 0};
    }
    std::pair<bool, int> current{false, 0};
    for (const h264::Mmco &m : h.mmco) {
      switch (m.op) {
      case 1: // a short-term picture is no longer a reference
        if (Slot *x = short_term(h.frame_num - m.difference_of_pic_nums))
          x->reference = false;
        break;
      case 2:
        forget_long_term([&](const Slot &x) { return x.long_term_idx == m.long_term_pic_num; });
        break;
      case 3: // a short-term picture becomes long-term
        if (Slot *x = short_term(h.frame_num - m.difference_of_pic_nums)) {
          forget_long_term([&](const Slot &y) { return y.long_term_idx == m.long_term_frame_idx; });
          x->long_term = true;
          x->long_term_idx = m.long_term_frame_idx;
        }
        break;
      case 4:
        max_long_term_idx = m.max_long_term_frame_idx_plus1 - 1;
        forget_long_term([&](const Slot &x) { return x.long_term_idx > max_long_term_idx; });
        break;
      case 5:
        for (Slot &x : slots)
          x.reference = false;
        max_long_term_idx = -1;
        break;
      case 6: // the current picture becomes long-term
        forget_long_term([&](const Slot &x) { return x.long_term_idx == m.long_term_frame_idx; });
        current = {true, m.long_term_frame_idx};
        break;
      }
    }
    return current;
  }

  Result<void> decode(std::span<const uint8_t> access_unit, int64_t pts) {
    ATM_PROFILE_SCOPE("video.decode");
    ATM_CHECK(take_parameter_sets(access_unit));
    std::vector<h264::Nal> slices;
    for (const h264::Nal &nal : h264::split_annex_b(access_unit))
      if (nal.type == 1 || nal.type == 5)
        slices.push_back(nal);
    if (slices.empty())
      return {};
    if (parameters_changed)
      ATM_CHECK(make_parameters());
    // The first slice's header says what the picture is. Only its start is read: a header is far shorter than this.
    h264::Nal head = slices.front();
    head.bytes = head.bytes.first(std::min<size_t>(head.bytes.size(), 4096));
    h264::Error error;
    const auto h = h264::parse_slice_header(head, sps, pps, &error);
    if (!h)
      return bad_stream(error.message);
    const h264::Pps &p = *std::find_if(pps.begin(), pps.end(), [&](const h264::Pps &x) { return x.id == h->pps_id; });
    const h264::Sps &s = *std::find_if(sps.begin(), sps.end(), [&](const h264::Sps &x) { return x.id == p.sps_id; });
    if (h->field_pic)
      return cannot("it has field pictures");
    const h264::Poc poc = counter.next(s, *h);
    const bool mmco5 = std::any_of(h->mmco.begin(), h->mmco.end(), [](const h264::Mmco &m) { return m.op == 5; });
    if (h->idr() || mmco5) // nothing before it shows after it
      show_all();
    const auto free_slot = std::find_if(slots.begin(), slots.end(), [](const Slot &x) { return !x.used(); });
    if (free_slot == slots.end())
      return bad_stream("no picture slot is free (read the pictures that are ready first)");
    const size_t target = size_t(free_slot - slots.begin());

    // The bitstream: every slice after a start code, the size rounded up as the decoder wants.
    size_t size = 0;
    for (const h264::Nal &nal : slices)
      size += 3 + nal.bytes.size();
    const VkDeviceSize align = std::max<VkDeviceSize>(1, caps.minBitstreamBufferSizeAlignment);
    const VkDeviceSize range = (VkDeviceSize(size) + align - 1) / align * align;
    ATM_CHECK(wait(submitted)); // the last decode has read the buffer, and its commands are done
    ATM_CHECK(vk.ensure(bitstream, range, VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                        &profiles));
    std::vector<uint32_t> offsets;
    auto *out = static_cast<uint8_t *>(bitstream.mapped);
    size_t at = 0;
    for (const h264::Nal &nal : slices) {
      offsets.push_back(uint32_t(at));
      out[at] = 0;
      out[at + 1] = 0;
      out[at + 2] = 1;
      std::memcpy(out + at + 3, nal.bytes.data(), nal.bytes.size());
      at += 3 + nal.bytes.size();
    }
    std::memset(out + at, 0, size_t(range) - at);

    // The references this picture may use: every slot that is one.
    std::vector<StdVideoDecodeH264ReferenceInfo> ref_info;
    std::vector<VkVideoDecodeH264DpbSlotInfoKHR> ref_dpb;
    std::vector<VkVideoPictureResourceInfoKHR> ref_res;
    std::vector<VkVideoReferenceSlotInfoKHR> refs;
    ref_info.reserve(slots.size() + 1);
    ref_dpb.reserve(slots.size() + 1);
    ref_res.reserve(slots.size() + 1);
    const auto add_slot = [&](size_t i, int frame_num, h264::Poc slot_poc, bool long_term) {
      StdVideoDecodeH264ReferenceInfo info{};
      info.flags.used_for_long_term_reference = long_term;
      info.FrameNum = uint16_t(frame_num);
      info.PicOrderCnt[0] = slot_poc.top;
      info.PicOrderCnt[1] = slot_poc.bottom;
      ref_info.push_back(info);
      VkVideoDecodeH264DpbSlotInfoKHR dpb_info{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
      dpb_info.pStdReferenceInfo = &ref_info.back();
      ref_dpb.push_back(dpb_info);
      ref_res.push_back(resource(dpb, i));
      VkVideoReferenceSlotInfoKHR slot{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
      slot.pNext = &ref_dpb.back();
      slot.slotIndex = int32_t(i);
      slot.pPictureResource = &ref_res.back();
      return slot;
    };
    for (size_t i = 0; i < slots.size(); ++i)
      if (slots[i].reference && !h->idr())
        refs.push_back(add_slot(i, slots[i].long_term ? slots[i].long_term_idx : slots[i].frame_num, slots[i].poc, slots[i].long_term));
    if (refs.size() > caps.maxActiveReferencePictures)
      return cannot("a picture uses more references than its decoder takes");
    const VkVideoReferenceSlotInfoKHR setup = add_slot(target, h->frame_num, poc, false);
    std::vector<VkVideoReferenceSlotInfoKHR> bound = refs; // what the session works with: the references and the new picture's slot
    bound.push_back(setup);
    bound.back().slotIndex = -1; // not a reference yet

    StdVideoDecodeH264PictureInfo std_picture{};
    std_picture.flags.is_intra = h->type() == 2 || h->type() == 4;
    std_picture.flags.IdrPicFlag = h->idr();
    std_picture.flags.is_reference = h->reference();
    std_picture.seq_parameter_set_id = uint8_t(s.id);
    std_picture.pic_parameter_set_id = uint8_t(p.id);
    std_picture.frame_num = uint16_t(h->frame_num);
    std_picture.idr_pic_id = uint16_t(h->idr_pic_id);
    std_picture.PicOrderCnt[0] = poc.top;
    std_picture.PicOrderCnt[1] = poc.bottom;
    VkVideoDecodeH264PictureInfoKHR picture{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
    picture.pStdPictureInfo = &std_picture;
    picture.sliceCount = uint32_t(offsets.size());
    picture.pSliceOffsets = offsets.data();

    VK_TRY("vkResetCommandBuffer", vkResetCommandBuffer(video_cmd, 0));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_TRY("vkBeginCommandBuffer", vkBeginCommandBuffer(video_cmd, &cbi));
    { // the first time every slot is laid out for decoding; after that, the references the last decode wrote are read
      VkImageMemoryBarrier2 ib[2]{};
      for (int i = 0; i < 2; ++i) {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        ib[i].srcStageMask = started ? VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR : VK_PIPELINE_STAGE_2_NONE;
        ib[i].srcAccessMask = started ? VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR : VK_ACCESS_2_NONE;
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
        ib[i].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
        ib[i].newLayout = i == 0 ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR : VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
        ib[i].oldLayout = started ? ib[i].newLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[i].image = i == 0 ? dpb.image : output.image;
        ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, uint32_t(slots.size())};
      }
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = output.image ? 2 : 1;
      dep.pImageMemoryBarriers = ib;
      vkCmdPipelineBarrier2(video_cmd, &dep);
    }
    VkVideoBeginCodingInfoKHR begin{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin.videoSession = session;
    begin.videoSessionParameters = parameters;
    begin.referenceSlotCount = uint32_t(bound.size());
    begin.pReferenceSlots = bound.data();
    fn.begin(video_cmd, &begin);
    if (!started) {
      VkVideoCodingControlInfoKHR control{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
      control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
      fn.control(video_cmd, &control);
    }
    VkVideoDecodeInfoKHR di{VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    di.pNext = &picture;
    di.srcBuffer = bitstream.buffer;
    di.srcBufferRange = range;
    di.dstPictureResource = output.image ? resource(output, target) : *setup.pPictureResource;
    di.pSetupReferenceSlot = &setup;
    di.referenceSlotCount = uint32_t(refs.size());
    di.pReferenceSlots = refs.data();
    fn.decode(video_cmd, &di);
    VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    fn.end(video_cmd, &end);
    VK_TRY("vkEndCommandBuffer", vkEndCommandBuffer(video_cmd));
    ATM_CHECK(submit(vk.video_queue, video_cmd));
    started = true;

    // The picture's place among the references and the pictures to show.
    Slot &slot = slots[target];
    const auto [long_term, long_term_idx] = h->reference() ? mark(s, *h) : std::pair<bool, int>{false, 0};
    slot = Slot{};
    slot.reference = h->reference();
    slot.long_term = long_term;
    slot.long_term_idx = long_term_idx;
    slot.frame_num = mmco5 ? 0 : h->frame_num;
    slot.poc = mmco5 ? h264::Poc{poc.top - poc.frame(), poc.bottom - poc.frame()} : poc;
    slot.waiting = true;
    slot.period = period;
    slot.order = slot.poc.frame();
    slot.pts = pts;
    bump();
    return {};
  }

  Result<bool> next(uint8_t *nv12, int64_t *pts) {
    Slot *best = nullptr;
    for (Slot &s : slots)
      if (s.ready && (!best || std::tie(s.period, s.order) < std::tie(best->period, best->order)))
        best = &s;
    if (!best)
      return false;
    ATM_PROFILE_SCOPE("video.read");
    const uint32_t layer = uint32_t(best - slots.data());
    const VkDeviceSize luma = VkDeviceSize(width) * VkDeviceSize(height);
    ATM_CHECK(vk.ensure(download, luma + luma / 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
    VK_TRY("vkResetCommandBuffer", vkResetCommandBuffer(copy_cmd, 0));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_TRY("vkBeginCommandBuffer", vkBeginCommandBuffer(copy_cmd, &cbi));
    const auto layout = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
      VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      ib.srcStageMask = src;
      ib.dstStageMask = dst;
      ib.dstAccessMask = dst_access;
      ib.oldLayout = from;
      ib.newLayout = to;
      ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ib.image = shown().image;
      ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &ib;
      vkCmdPipelineBarrier2(copy_cmd, &dep);
    };
    layout(output_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
           VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy planes[2]{};
    planes[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, layer, 1};
    planes[0].imageOffset = {crop_x, crop_y, 0};
    planes[0].imageExtent = {uint32_t(width), uint32_t(height), 1};
    planes[1].bufferOffset = luma;
    planes[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, layer, 1};
    planes[1].imageOffset = {crop_x / 2, crop_y / 2, 0};
    planes[1].imageExtent = {uint32_t(width / 2), uint32_t(height / 2), 1};
    vkCmdCopyImageToBuffer(copy_cmd, shown().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, download.buffer, 2, planes);
    layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output_layout, VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(copy_cmd, &dep);
    VK_TRY("vkEndCommandBuffer", vkEndCommandBuffer(copy_cmd));
    ATM_CHECK(submit(vk.queue, copy_cmd));
    ATM_CHECK(wait(submitted));
    std::memcpy(nv12, download.mapped, size_t(luma + luma / 2));
    *pts = best->pts;
    best->waiting = best->ready = false;
    return true;
  }

  void flush() {
    show_all();
    for (Slot &s : slots)
      s.reference = false;
    counter = h264::PocCounter{};
  }
};

VideoDecoder::~VideoDecoder() = default;

Result<std::unique_ptr<VideoDecoder>> VideoDecoder::create(Context &gpu, std::span<const uint8_t> header) {
  ATM_PROFILE_SCOPE("video.create");
  Vulkan &vk = vulkan_of(*gpu.impl_);
  if (vk.video_family < 0)
    return cannot(gpu.info().device + " has no H.264 decoder that Vulkan reaches");
  std::unique_ptr<VideoDecoder> self(new VideoDecoder);
  self->impl_ = std::make_unique<Impl>(vk);
  Impl &m = *self->impl_;
  ATM_CHECK(m.take_parameter_sets(header));
  if (m.sps.empty() || m.pps.empty())
    return bad_stream("its header has no parameter sets");
  ATM_CHECK(m.init(m.sps.front()));
  return self;
}

int VideoDecoder::width() const { return impl_->width; }
int VideoDecoder::height() const { return impl_->height; }
Result<void> VideoDecoder::decode(std::span<const uint8_t> access_unit, int64_t pts) { return impl_->decode(access_unit, pts); }
Result<bool> VideoDecoder::next(uint8_t *nv12, int64_t *pts) { return impl_->next(nv12, pts); }
void VideoDecoder::flush() { impl_->flush(); }

} // namespace atm::gpu

#endif
