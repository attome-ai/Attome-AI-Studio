#if defined(ATM_GPU_VULKAN)
// The Vulkan compute path (ADR-009). One device, one queue, one command buffer reused for every call, and buffers that
// grow to the largest picture seen and are then kept: a call allocates nothing.
//
// A picture goes CPU -> mapped upload buffer -> device buffer A, the passes ping-pong between A and B on the device,
// and A -> mapped download buffer (host-cached, so the CPU reads it fast) -> CPU.

#include "atm/gpu/gpu.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "atm/base/profiler.hpp"
#include "vulkan_shared.hpp"

namespace atm::gpu {
namespace {

const uint32_t kBoxSpirv[] = { // one pass along columns (or rows) with a running sum
#include "box.spv.inc"
};
const uint32_t kRowsSpirv[] = { // the three horizontal passes of a row at once, in shared memory
#include "box_rows.spv.inc"
};
const uint32_t kPixelsSpirv[] = { // the per-pixel effects (grade tables, vignette, grain, sharpen's mix)
#include "pixels.spv.inc"
};
constexpr uint32_t kMaxRowBytes = 8192; // box_rows.comp holds a row of up to this many bytes in shared memory

using Clock = std::chrono::steady_clock;
double us_since(Clock::time_point t0) { return std::chrono::duration<double, std::micro>(Clock::now() - t0).count(); }

// The push constants of box_rows.comp, in its order.
struct RowsPass {
  uint32_t offset, stride, bytes, ch;
  int32_t r;
  uint32_t shift, magic;
};

// ceil(2^32 / w): with it, (x * magic) >> 32 is x / w exactly for every x a box sum can be (x <= 256 w) while w < 4096.
uint32_t magic_for(int r) {
  const uint64_t w = uint64_t(2 * r + 1);
  return w < 4096 ? uint32_t(((uint64_t(1) << 32) + w - 1) / w) : 0u;
}

// The push constants of pixels.comp, in its order.
struct PixelsPass {
  uint32_t kind, width, height, stride;
  float f0;
  uint32_t u0, u1, chroma_x, table_at;
};

// The push constants of box.comp, in its order.
struct Pass {
  uint32_t offset, rows, stride, n, channels;
  int32_t r;
  uint32_t seg, vertical;
};

VkInstance make_instance() {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Attome";
  app.pEngineName = "Attome";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  return vkCreateInstance(&ici, nullptr, &instance) == VK_SUCCESS ? instance : VK_NULL_HANDLE;
}

std::vector<VkPhysicalDevice> physical_devices(VkInstance instance) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  return devices;
}

// What a physical device is and whether it can do the work: Vulkan 1.3 with synchronization2, a compute queue, subgroup
// sums in compute shaders (subgroups of 16 or more) and 36 KB of shared memory for a workgroup.
Device describe(VkPhysicalDevice physical, int index) {
  Device d;
  d.index = index;
  VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  driver.pNext = &subgroup;
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &driver;
  vkGetPhysicalDeviceProperties2(physical, &p2);
  const VkPhysicalDeviceProperties &props = p2.properties;
  d.name = props.deviceName;
  d.driver = std::string(driver.driverName) + " " + driver.driverInfo;
  d.vendor_id = props.vendorID;
  d.device_id = props.deviceID;
  d.api = props.apiVersion;
  d.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  d.integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  VkPhysicalDeviceMemoryProperties mem;
  vkGetPhysicalDeviceMemoryProperties(physical, &mem);
  for (uint32_t h = 0; h < mem.memoryHeapCount; ++h)
    if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      d.memory_mb += mem.memoryHeaps[h].size >> 20;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f13;
  vkGetPhysicalDeviceFeatures2(physical, &f2);
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
  std::vector<VkQueueFamilyProperties> fam(families);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, fam.data());
  const bool compute = std::any_of(fam.begin(), fam.end(), [](const VkQueueFamilyProperties &f) { return (f.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0; });
  if (props.apiVersion < VK_API_VERSION_1_3)
    d.why_not = "its driver offers Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." + std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + ", and 1.3 is needed";
  else if (!f13.synchronization2)
    d.why_not = "its driver lacks synchronization2";
  else if (!compute)
    d.why_not = "it has no compute queue";
  else if (!(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || !(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) || subgroup.subgroupSize < 16)
    d.why_not = "its compute shaders lack subgroup sums (or its subgroups are smaller than 16)";
  else if (props.limits.maxComputeSharedMemorySize < 16 * 1024)
    d.why_not = "it has too little shared memory for a compute workgroup";
  else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
    d.why_not = "it is a software device that runs on the CPU";
  d.usable = d.why_not.empty();
  return d;
}

// The queue family that decodes H.264, when the video extensions are there too (Vulkan Video); -1 when not.
int h264_decode_family(VkPhysicalDevice physical) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
  for (const char *needed : {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME})
    if (std::none_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, needed) == 0; }))
      return -1;
  vkGetPhysicalDeviceQueueFamilyProperties2(physical, &count, nullptr);
  std::vector<VkQueueFamilyVideoPropertiesKHR> video(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
  std::vector<VkQueueFamilyProperties2> families(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
  for (uint32_t i = 0; i < count; ++i)
    families[i].pNext = &video[i];
  vkGetPhysicalDeviceQueueFamilyProperties2(physical, &count, families.data());
  for (uint32_t i = 0; i < count; ++i)
    if ((families[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && (video[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR))
      return int(i);
  return -1;
}

} // namespace

tl::unexpected<Error> vk_fail(const char *what, VkResult r) {
  return fail(ErrorCode::EncoderUnavailable, "G_VULKAN", std::string("Vulkan: ") + what + " failed (" + std::to_string(int(r)) + ").", {},
              "The GPU path is not used; the CPU renderer does the work. Updating the graphics driver may help.");
}

int Vulkan::memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice) const {
  for (const VkMemoryPropertyFlags flags : {want | nice, want})
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & flags) == flags)
        return int(i);
  return -1;
}

Result<void> Vulkan::ensure(Buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice, const void *next) {
  if (buf.buffer && buf.size >= size)
    return {};
  release(buf);
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.pNext = next;
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_TRY("vkCreateBuffer", vkCreateBuffer(device, &bi, nullptr, &buf.buffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, buf.buffer, &req);
  const int type = memory_type(req.memoryTypeBits, want, nice);
  if (type < 0)
    return fail(ErrorCode::EncoderUnavailable, "G_MEMORY", "Vulkan: no memory of the kind a buffer needs.");
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = uint32_t(type);
  VK_TRY("vkAllocateMemory", vkAllocateMemory(device, &ai, nullptr, &buf.memory));
  VK_TRY("vkBindBufferMemory", vkBindBufferMemory(device, buf.buffer, buf.memory, 0));
  buf.size = size;
  if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    VK_TRY("vkMapMemory", vkMapMemory(device, buf.memory, 0, VK_WHOLE_SIZE, 0, &buf.mapped));
  return {};
}

void Vulkan::release(Buffer &buf) {
  if (buf.mapped)
    vkUnmapMemory(device, buf.memory);
  vkDestroyBuffer(device, buf.buffer, nullptr);
  vkFreeMemory(device, buf.memory, nullptr);
  buf = Buffer{};
}

std::vector<Device> list_devices() {
  ATM_PROFILE_SCOPE("gpu.list");
  std::vector<Device> out;
  VkInstance instance = make_instance();
  if (!instance)
    return out;
  int i = 0;
  for (VkPhysicalDevice p : physical_devices(instance))
    out.push_back(describe(p, i++));
  vkDestroyInstance(instance, nullptr);
  return out;
}

int box_radius(float sigma) { return int(std::lround((std::sqrt(1.0 + 4.0 * double(sigma) * double(sigma)) - 1.0) / 2.0)); }

struct Context::Impl : Vulkan {
  Info info;
  float timestamp_ns = 1.0f; // nanoseconds per timestamp tick
  bool timestamps = false;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool queries = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE, rows_shader = VK_NULL_HANDLE, pixels_shader = VK_NULL_HANDLE;
  VkPipeline box = VK_NULL_HANDLE, pixels = VK_NULL_HANDLE;
  std::map<uint64_t, VkPipeline> rows; // box_rows.comp by the row capacity and chunk it was made for
  uint32_t shared_limit = 0; // bytes of shared memory a workgroup can have
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  // The sets of the four bindings (source, destination, tables, aux): A and B are the picture and its spare, C a third
  // picture (sharpen's blurred copy), T the effects' numbers. Every set has T and C in bindings 2 and 3.
  VkDescriptorSet a_to_b = VK_NULL_HANDLE, b_to_a = VK_NULL_HANDLE, c_to_b = VK_NULL_HANDLE, b_to_c = VK_NULL_HANDLE;
  VkDescriptorSet d_to_b = VK_NULL_HANDLE, b_to_d = VK_NULL_HANDLE; // D: a clip's coverage (binding 5 of every set)
  Buffer a, b, c, d, upload, download, tables;
  Buffer lut, lut_stage; // a LUT kept in the GPU's memory (binding 4), and the host memory it is sent from
  uint64_t lut_loaded = 0; // the id of the LUT in `lut` (0: none)

  ~Impl() {
    if (device) {
      vkDeviceWaitIdle(device);
      for (Buffer *buf : {&a, &b, &c, &d, &upload, &download, &tables, &lut, &lut_stage})
        release(*buf);
      vkDestroyDescriptorPool(device, descriptors, nullptr);
      vkDestroyPipeline(device, box, nullptr);
      vkDestroyPipeline(device, pixels, nullptr);
      vkDestroyShaderModule(device, pixels_shader, nullptr);
      for (const auto &[cap, pipeline] : rows)
        vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyShaderModule(device, shader, nullptr);
      vkDestroyShaderModule(device, rows_shader, nullptr);
      vkDestroyPipelineLayout(device, layout, nullptr);
      vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
      vkDestroyQueryPool(device, queries, nullptr);
      vkDestroyFence(device, fence, nullptr);
      vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }

  Result<void> ensure_buffers(VkDeviceSize size, VkDeviceSize table_bytes, VkDeviceSize lut_bytes) {
    const bool grew = !a.buffer || a.size < size || !tables.buffer || tables.size < table_bytes || !lut.buffer || lut.size < lut_bytes;
    if (lut.buffer && lut.size < lut_bytes)
      lut_loaded = 0; // a new buffer holds nothing yet
    constexpr VkBufferUsageFlags kWork = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ATM_CHECK(ensure(a, size, kWork, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    ATM_CHECK(ensure(b, size, kWork, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    ATM_CHECK(ensure(c, size, kWork, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    ATM_CHECK(ensure(d, size, kWork, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    // The staging memory holds a picture and a coverage plane (at most another two thirds of a picture).
    // Cached, so a picture can be drawn straight into it (drawing reads what it wrote; uncached memory is slow to read).
    ATM_CHECK(ensure(upload, size * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
    // Read back by the CPU: cached memory is many times faster to read than write-combined.
    ATM_CHECK(ensure(download, size * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
    // The effects' numbers: small, written by the CPU before each submit, read by the shaders.
    ATM_CHECK(ensure(tables, std::max<VkDeviceSize>(table_bytes, 64 * 1024), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    ATM_CHECK(ensure(lut, std::max<VkDeviceSize>(lut_bytes, 256), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    ATM_CHECK(ensure(lut_stage, std::max<VkDeviceSize>(lut_bytes, 256), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    if (grew) { // the descriptor sets point at the new buffers
      const auto point = [&](VkDescriptorSet set, const Buffer &from, const Buffer &to) {
        VkDescriptorBufferInfo infos[6] = {{from.buffer, 0, VK_WHOLE_SIZE}, {to.buffer, 0, VK_WHOLE_SIZE}, {tables.buffer, 0, VK_WHOLE_SIZE},
                                           {c.buffer, 0, VK_WHOLE_SIZE}, {lut.buffer, 0, VK_WHOLE_SIZE}, {d.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[6]{};
        for (uint32_t i = 0; i < 6; ++i) {
          writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
      };
      point(a_to_b, a, b);
      point(b_to_a, b, a);
      point(c_to_b, c, b);
      point(b_to_c, b, c);
      point(d_to_b, d, b);
      point(b_to_d, b, d);
    }
    return {};
  }

  Result<void> init(int wanted) {
    instance = make_instance();
    if (!instance)
      return fail(ErrorCode::EncoderUnavailable, "G_NO_VULKAN", "Vulkan could not be started on this computer.", {},
                  "The CPU renderer does the work. Installing or updating the graphics driver adds Vulkan.");
    const std::vector<VkPhysicalDevice> devices = physical_devices(instance);
    // The device asked for; else the discrete GPU first, then an integrated one (ATTOME_GPU_DEVICE names one for a test run).
    if (wanted < 0)
      if (const char *pick = std::getenv("ATTOME_GPU_DEVICE"); pick && *pick)
        wanted = std::atoi(pick);
    int best = -1, best_score = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
      const Device found = describe(devices[i], int(i));
      if (wanted >= 0 && int(i) == wanted && !found.usable)
        return fail(ErrorCode::EncoderUnavailable, "G_DEVICE_UNUSABLE", found.name + " cannot render: " + found.why_not + ".");
      if (!found.usable)
        continue;
      const int score = wanted >= 0 ? (int(i) == wanted ? 10 : 0) : found.discrete ? 3 : found.integrated ? 2 : 1;
      if (score > best_score) {
        best = int(i);
        best_score = score;
      }
    }
    if (best < 0 || (wanted >= 0 && best != wanted))
      return fail(ErrorCode::EncoderUnavailable, "G_NO_DEVICE", wanted >= 0 ? "There is no GPU number " + std::to_string(wanted) + "." : "No GPU with Vulkan 1.3 was found.", {},
                  "The CPU renderer does the work. A recent graphics driver adds Vulkan 1.3 on most GPUs from 2016 on.");
    physical = devices[size_t(best)];
    info.index = best;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    info.device = props.deviceName;
    info.api = props.apiVersion;
    info.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical, &p2);
    info.driver = std::string(driver.driverName) + " " + driver.driverInfo;
    timestamp_ns = props.limits.timestampPeriod;
    shared_limit = props.limits.maxComputeSharedMemorySize;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, fam.data());
    bool found = false;
    for (uint32_t i = 0; i < families && !found; ++i)
      if (fam[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        family = i;
        found = true;
        timestamps = fam[i].timestampValidBits > 0;
      }
    if (!found)
      return fail(ErrorCode::EncoderUnavailable, "G_NO_QUEUE", "The GPU has no compute queue.");

    // A video decode queue too, when there is one (a family of its own on the GPUs seen so far).
    video_family = h264_decode_family(physical);
    if (video_family == int(family))
      video_family = -1; // one queue of the family is made, and decoding would need a second
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi[2]{{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    qi[0].queueFamilyIndex = family;
    qi[1].queueFamilyIndex = uint32_t(video_family);
    for (VkDeviceQueueCreateInfo &q : qi) {
      q.queueCount = 1;
      q.pQueuePriorities = &priority;
    }
    const char *video_extensions[] = {VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features on12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    on12.timelineSemaphore = VK_TRUE; // the decoder orders its work on two queues with one (every 1.3 device has it)
    VkPhysicalDeviceVulkan13Features on13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    on13.pNext = &on12;
    on13.synchronization2 = VK_TRUE; // the barriers and timestamps below are the synchronization2 kind
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &on13;
    di.queueCreateInfoCount = video_family >= 0 ? 2 : 1;
    di.pQueueCreateInfos = qi;
    di.enabledExtensionCount = video_family >= 0 ? 3 : 0;
    di.ppEnabledExtensionNames = video_extensions;
    VK_TRY("vkCreateDevice", vkCreateDevice(physical, &di, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    if (video_family >= 0)
      vkGetDeviceQueue(device, uint32_t(video_family), 0, &video_queue);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = family;
    VK_TRY("vkCreateCommandPool", vkCreateCommandPool(device, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_TRY("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device, &cai, &cmd));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_TRY("vkCreateFence", vkCreateFence(device, &fi, nullptr, &fence));
    if (timestamps) {
      VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qpi.queryCount = 5;
      VK_TRY("vkCreateQueryPool", vkCreateQueryPool(device, &qpi, nullptr, &queries));
    }

    VkDescriptorSetLayoutBinding bindings[6]{};
    for (uint32_t i = 0; i < 6; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = bindings;
    VK_TRY("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64}; // the largest block (pixels.comp) and room
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VK_TRY("vkCreatePipelineLayout", vkCreatePipelineLayout(device, &pli, nullptr, &layout));
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof kBoxSpirv;
    smi.pCode = kBoxSpirv;
    VK_TRY("vkCreateShaderModule", vkCreateShaderModule(device, &smi, nullptr, &shader));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = shader;
    cpi.stage.pName = "main";
    cpi.layout = layout;
    VK_TRY("vkCreateComputePipelines", vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &box));
    smi.codeSize = sizeof kRowsSpirv;
    smi.pCode = kRowsSpirv;
    VK_TRY("vkCreateShaderModule", vkCreateShaderModule(device, &smi, nullptr, &rows_shader));
    smi.codeSize = sizeof kPixelsSpirv;
    smi.pCode = kPixelsSpirv;
    VK_TRY("vkCreateShaderModule", vkCreateShaderModule(device, &smi, nullptr, &pixels_shader));
    cpi.stage.module = pixels_shader;
    VK_TRY("vkCreateComputePipelines", vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pixels));


    VkDescriptorPoolSize sizes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 36};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 6;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &sizes;
    VK_TRY("vkCreateDescriptorPool", vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
    VkDescriptorSetLayout six[6] = {set_layout, set_layout, set_layout, set_layout, set_layout, set_layout};
    VkDescriptorSet sets[6];
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descriptors;
    dai.descriptorSetCount = 6;
    dai.pSetLayouts = six;
    VK_TRY("vkAllocateDescriptorSets", vkAllocateDescriptorSets(device, &dai, sets));
    a_to_b = sets[0];
    b_to_a = sets[1];
    c_to_b = sets[2];
    b_to_c = sets[3];
    d_to_b = sets[4];
    b_to_d = sets[5];
    return {};
  }

  // box_rows.comp for rows of up to `cap` bytes (a multiple of 512) and `chunk` bytes per invocation, made the first time a
  // picture that wide comes.
  Result<VkPipeline> rows_pipeline(uint32_t cap, uint32_t chunk, uint32_t ch) {
    const uint64_t key = (uint64_t(cap) << 32) | (uint64_t(chunk) << 8) | ch;
    if (const auto it = rows.find(key); it != rows.end())
      return it->second;
    if ((cap + cap / 4u + 4u) * 4u + 1024u > shared_limit || cap > kMaxRowBytes)
      return fail(ErrorCode::EncoderUnavailable, "G_TOO_WIDE", "Rows of " + std::to_string(cap) + " bytes do not fit the GPU's shared memory (" + std::to_string(shared_limit) + " bytes).");
    const VkSpecializationMapEntry entries[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    const uint32_t values[3] = {cap, chunk, ch};
    VkSpecializationInfo spec{3, entries, sizeof values, values};
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = rows_shader;
    cpi.stage.pName = "main";
    cpi.stage.pSpecializationInfo = &spec;
    cpi.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_TRY("vkCreateComputePipelines", vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));
    rows[key] = pipeline;
    return pipeline;
  }

  void barrier(VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = src_stage;
    mb.srcAccessMask = src_access;
    mb.dstStageMask = dst_stage;
    mb.dstAccessMask = dst_access;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  void dispatch(const Pass &p) {
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    const uint32_t across = p.vertical ? p.n : p.rows, along = p.vertical ? p.rows : p.n; // invocations: x across the lines, y along them
    vkCmdDispatch(cmd, (across + 63) / 64, (along + p.seg - 1) / p.seg, 1);
  }

  void compute_to_compute() {
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
  }

  // The blur of the picture in buffer X, back into X: the three row passes X -> B in one dispatch, then the three column
  // passes B -> X -> B -> X. `xb` and `bx` are the sets X -> B and B -> X; a plane with radius 0 is left alone.
  void record_blur(VkDescriptorSet xb, VkDescriptorSet bx, uint32_t W, uint32_t H, uint32_t P, int ry, int rc, VkPipeline rows_luma, VkPipeline rows_chroma,
                   uint32_t shift, bool mark = false) {
    const uint32_t chroma_at = P * H;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &xb, 0, nullptr);
    const auto rows_pass = [&](VkPipeline pipeline, const RowsPass &p, uint32_t count) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
      vkCmdDispatch(cmd, count, 1, 1);
    };
    if (ry >= 1)
      rows_pass(rows_luma, {0, P, W, 1, ry, shift, magic_for(ry)}, H);
    if (rc >= 1)
      rows_pass(rows_chroma, {chroma_at, P, W, 2, rc, shift, magic_for(rc)}, H / 2);
    compute_to_compute();
    if (mark && timestamps)
      vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, queries, 4);
    // A stretch is the window's width (at least 16 rows): enough invocations to fill the GPU, and the window's first sum
    // costs no more than the stretch.
    const auto seg = [](int r) { return uint32_t(std::max(16, 2 * r + 1)); };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, box);
    for (int pass = 0; pass < 3; ++pass) {
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, pass == 1 ? &xb : &bx, 0, nullptr);
      if (ry >= 1)
        dispatch({0, H, P, P / 4, 1, ry, seg(ry), 1});
      if (rc >= 1)
        dispatch({chroma_at, H / 2, P, P / 4, 1, rc, seg(rc), 1});
      compute_to_compute();
    }
  }

  void record_pixels(const PixelsPass &p, uint32_t P, uint32_t H) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pixels);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &a_to_b, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    const uint32_t words = (P / 4) * (p.kind == 4 ? H / 2 : H + H / 2); // a LUT: one invocation per chroma word
    vkCmdDispatch(cmd, (words + 255) / 256, 1, 1);
    compute_to_compute();
  }

  // The copies between the caller's packed picture (rows `W` bytes apart) and the device's (rows `P` apart, P >= W, a
  // multiple of 4): one copy when they are the same, one per row when the rows are padded.
  std::vector<VkBufferCopy> regions;
  // `count` rows (a picture: H + H / 2; a coverage plane: H); `staged` is where they start in the staging memory.
  const std::vector<VkBufferCopy> &copies(int W, int count, uint32_t P, bool to_device, VkDeviceSize staged = 0) {
    regions.clear();
    if (P == uint32_t(W)) {
      const VkDeviceSize all = VkDeviceSize(W) * VkDeviceSize(count);
      regions.push_back({to_device ? staged : 0, to_device ? 0 : staged, all});
      return regions;
    }
    for (int y = 0; y < count; ++y) {
      const VkDeviceSize packed = staged + VkDeviceSize(y) * VkDeviceSize(W), padded = VkDeviceSize(y) * P;
      regions.push_back({to_device ? packed : padded, to_device ? padded : packed, VkDeviceSize(W)});
    }
    return regions;
  }
};

Result<std::unique_ptr<Context>> Context::create(int device) {
  ATM_PROFILE_SCOPE("gpu.create");
  if (const char *off = std::getenv("ATTOME_GPU"); off && std::string(off) == "off")
    return fail(ErrorCode::EncoderUnavailable, "G_OFF", "The GPU path is switched off (ATTOME_GPU=off).");
  auto self = std::unique_ptr<Context>(new Context());
  self->impl_ = std::make_unique<Impl>();
  ATM_CHECK(self->impl_->init(device));
  return self;
}

Context::~Context() = default;
const Info &Context::info() const { return impl_->info; }
Vulkan &vulkan_of(Context::Impl &impl) { return impl; }

Result<void> Context::blur_nv12(uint8_t *nv12, int W, int H, float sigma, Timing *timing) {
  Effect blur;
  blur.kind = Effect::Kind::blur;
  blur.sigma = sigma;
  return run_effects(nv12, W, H, {blur}, timing);
}

uint8_t *Context::staging(int W, int H) {
  Impl &m = *impl_;
  const uint32_t P = (uint32_t(W) + 3u) & ~3u, uh = uint32_t(H);
  if (!m.ensure_buffers(VkDeviceSize(P) * (uh + uh / 2), 0, 0))
    return nullptr;
  return static_cast<uint8_t *>(m.upload.mapped);
}

Result<void> Context::run_effects(uint8_t *nv12, int W, int H, const std::vector<Effect> &chain, Timing *timing, uint8_t *cover, uint8_t *result) {
  ATM_PROFILE_SCOPE("gpu.effects");
  Impl &m = *impl_;
  const auto t0 = Clock::now();
  const size_t luma = size_t(W) * size_t(H), size = luma + luma / 2;
  if (chain.empty())
    return {};
  if (uint32_t(W) > kMaxRowBytes)
    return fail(ErrorCode::EncoderUnavailable, "G_TOO_WIDE", "The GPU effects take pictures up to " + std::to_string(kMaxRowBytes) + " pixels wide.");
  const uint32_t P = (uint32_t(W) + 3u) & ~3u; // rows on the device: whole 4-byte words
  const uint32_t uw = uint32_t(W), uh = uint32_t(H);
  // The effects' numbers, one after another in the tables buffer.
  std::vector<uint32_t> table_at(chain.size(), 0);
  size_t table_words = 0;
  for (size_t i = 0; i < chain.size(); ++i) {
    table_at[i] = uint32_t(table_words);
    table_words += chain[i].table.size();
  }
  // The LUT of the chain (one at most), sent to the GPU's memory when it is not the one there already.
  const Effect *lut_effect = nullptr;
  for (const Effect &e : chain)
    if (e.kind == Effect::Kind::lut) {
      if (lut_effect && lut_effect->lut_id != e.lut_id)
        return fail(ErrorCode::EncoderUnavailable, "G_TWO_LUTS", "A chain of GPU effects holds one LUT at most.");
      lut_effect = &e;
    }
  ATM_CHECK(m.ensure_buffers(VkDeviceSize(P) * (uh + uh / 2), table_words * 4, lut_effect ? lut_effect->lut_floats * 4 : 0));
  const bool send_lut = lut_effect && lut_effect->lut_id != m.lut_loaded;
  if (send_lut)
    std::memcpy(m.lut_stage.mapped, lut_effect->lut, lut_effect->lut_floats * 4);
  for (size_t i = 0; i < chain.size(); ++i)
    if (!chain[i].table.empty())
      std::memcpy(static_cast<uint32_t *>(m.tables.mapped) + table_at[i], chain[i].table.data(), chain[i].table.size() * 4);
  auto t = Clock::now();
  if (nv12 != m.upload.mapped) { // drawn elsewhere: copied in
    ATM_PROFILE_SCOPE("gpu.to_staging");
    std::memcpy(m.upload.mapped, nv12, size);
  }
  if (cover)
    std::memcpy(static_cast<uint8_t *>(m.upload.mapped) + size, cover, luma);
  const double upload_us = us_since(t);

  uint32_t shift = 2; // bytes per invocation of the row passes: a power of two, at least a word
  while ((256u << shift) < uw)
    ++shift;
  // The pipelines before recording: a failure leaves nothing half-recorded.
  ATM_TRY(VkPipeline rows_luma, m.rows_pipeline((P + 511u) & ~511u, 1u << shift, 1));
  ATM_TRY(VkPipeline rows_chroma, m.rows_pipeline((P + 511u) & ~511u, 1u << shift, 2));
  VK_TRY("vkResetCommandBuffer", vkResetCommandBuffer(m.cmd, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_TRY("vkBeginCommandBuffer", vkBeginCommandBuffer(m.cmd, &bi));
  if (m.timestamps) {
    vkCmdResetQueryPool(m.cmd, m.queries, 0, 5);
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_NONE, m.queries, 0);
  }
  const auto &in = m.copies(W, H + H / 2, P, true);
  vkCmdCopyBuffer(m.cmd, m.upload.buffer, m.a.buffer, uint32_t(in.size()), in.data());
  if (cover) {
    const auto &in_cover = m.copies(W, H, P, true, size);
    vkCmdCopyBuffer(m.cmd, m.upload.buffer, m.d.buffer, uint32_t(in_cover.size()), in_cover.data());
  }
  if (send_lut) {
    const VkBufferCopy whole{0, 0, VkDeviceSize(lut_effect->lut_floats) * 4};
    vkCmdCopyBuffer(m.cmd, m.lut_stage.buffer, m.lut.buffer, 1, &whole);
  }
  m.compute_to_compute();
  if (m.timestamps)
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, m.queries, 1);
  bool marked = false; // the "rows" timestamp: after the first blur's row passes
  for (size_t i = 0; i < chain.size(); ++i) {
    const Effect &e = chain[i];
    switch (e.kind) {
    case Effect::Kind::blur:
      m.record_blur(m.a_to_b, m.b_to_a, uw, uh, P, box_radius(e.sigma), box_radius(e.sigma * 0.5f), rows_luma, rows_chroma, shift, !marked);
      marked = true;
      if (cover) // the coverage blurs as the luma does, so the clip's edge fades into what is below it
        m.record_blur(m.d_to_b, m.b_to_d, uw, uh, P, box_radius(e.sigma), 0, rows_luma, rows_chroma, shift);
      break;
    case Effect::Kind::sharpen: { // the luma of A, blurred in C, then A moves away from it
      const int r = box_radius(e.sigma);
      const VkBufferCopy luma_plane{0, 0, VkDeviceSize(P) * uh};
      vkCmdCopyBuffer(m.cmd, m.a.buffer, m.c.buffer, 1, &luma_plane);
      m.compute_to_compute();
      if (r >= 1) // with no blur the mix still clamps the luma to 16..235, as the CPU's does
        m.record_blur(m.c_to_b, m.b_to_c, uw, uh, P, r, 0, rows_luma, rows_chroma, shift);
      m.record_pixels({3, uw, uh, P, e.amount, 0, 0, 0, 0}, P, uh);
      break;
    }
    case Effect::Kind::table:
      m.record_pixels({0, uw, uh, P, 0.0f, 0, 0, 0, table_at[i]}, P, uh);
      break;
    case Effect::Kind::vignette:
      m.record_pixels({1, uw, uh, P, 0.0f, 0, 0, 1025u + uw + uh, table_at[i]}, P, uh);
      break;
    case Effect::Kind::grain:
      m.record_pixels({2, uw, uh, P, e.amount, std::max<uint32_t>(1, e.cell), e.seed, 0, 0}, P, uh);
      break;
    case Effect::Kind::lut:
      if (e.amount > 0.0f) // strength 0 leaves the picture alone, as on the CPU
        m.record_pixels({4, uw, uh, P, e.amount, 0, 0, 0, 0}, P, uh);
      break;
    }
    if (cover && e.kind != Effect::Kind::blur) // a colour effect: what it changed where the clip is not, taken back
      m.record_pixels({5, uw, uh, P, 0.0f, 0, 0, 0, 0}, P, uh);
  }
  if (m.timestamps) {
    if (!marked)
      vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m.queries, 4);
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m.queries, 2);
  }
  const auto &out = m.copies(W, H + H / 2, P, false);
  vkCmdCopyBuffer(m.cmd, m.a.buffer, m.download.buffer, uint32_t(out.size()), out.data());
  if (cover) {
    const auto &out_cover = m.copies(W, H, P, false, size);
    vkCmdCopyBuffer(m.cmd, m.d.buffer, m.download.buffer, uint32_t(out_cover.size()), out_cover.data());
  }
  m.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  if (m.timestamps)
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, m.queries, 3);
  VK_TRY("vkEndCommandBuffer", vkEndCommandBuffer(m.cmd));

  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = m.cmd;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  VK_TRY("vkResetFences", vkResetFences(m.device, 1, &m.fence));
  VK_TRY("vkQueueSubmit2", vkQueueSubmit2(m.queue, 1, &si, m.fence));
  {
    ATM_PROFILE_SCOPE("gpu.wait");
    VK_TRY("vkWaitForFences", vkWaitForFences(m.device, 1, &m.fence, VK_TRUE, UINT64_MAX));
  }
  if (send_lut)
    m.lut_loaded = lut_effect->lut_id;

  t = Clock::now();
  {
    ATM_PROFILE_SCOPE("gpu.from_staging");
    std::memcpy(result ? result : nv12, m.download.mapped, size);
  }
  if (cover)
    std::memcpy(cover, static_cast<const uint8_t *>(m.download.mapped) + size, luma);
  const double download_us = us_since(t);
  if (timing) {
    timing->upload_us = upload_us;
    timing->download_us = download_us;
    if (m.timestamps) {
      uint64_t ticks[5] = {};
      vkGetQueryPoolResults(m.device, m.queries, 0, 5, sizeof ticks, ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      const auto us = [&](uint64_t from, uint64_t to) { return double(to - from) * double(m.timestamp_ns) / 1000.0; };
      timing->compute_us = us(ticks[1], ticks[2]);
      timing->rows_us = us(ticks[1], ticks[4]);
      timing->columns_us = us(ticks[4], ticks[2]);
      timing->gpu_copy_us = us(ticks[0], ticks[1]) + us(ticks[2], ticks[3]);
    }
    timing->total_us = us_since(t0);
  }
  return {};
}

} // namespace atm::gpu

#endif
